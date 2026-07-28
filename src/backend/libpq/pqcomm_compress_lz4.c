/*-------------------------------------------------------------------------
 *
 * pqcomm_compress_lz4.c
 *	  Compress backend messages with lz4
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress_lz4.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/libpq.h"
#include "utils/memutils.h"

#ifndef USE_LZ4

const		PQcompressMethods *
pq_init_compressor_lz4(pqcomm_compress * cs)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("lz4 compression is not supported by this build")));
}

#else
#include <lz4frame.h>

#define CHUNK_SIZE (64 * 1024)

typedef struct pqcomm_lz4
{
	LZ4F_cctx  *ctx;
	LZ4F_preferences_t prefs;
	bool		frame_started;
}			pqcomm_lz4;

static int	pq_compress_flush_lz4(pqcomm_compress * cs, bool block, bool end_frame);
static int	pq_compress_message_lz4(pqcomm_compress * cs, bool block, bool flush, bool end_frame);
static void pq_compress_free_lz4(pqcomm_compress * cs);

static const PQcompressMethods PqCompressMethodsLz4 = {
	.flush = pq_compress_flush_lz4,
	.compress_message = pq_compress_message_lz4,
	.free_compress_context = pq_compress_free_lz4,
};

/*
 * pq_init_compressor_lz4 -- Initialize a lz4 compressor
 *
 * On success, the compressor state will be stored in cs->private_data.
 *
 * returns PQcompressMethods of the lz4 compressor
 */
const		PQcompressMethods *
pq_init_compressor_lz4(pqcomm_compress * cs)
{
	pqcomm_lz4 *lz4_state;
	LZ4F_errorCode_t ctxError;
	size_t		outbuf_size;
	MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

	Assert(!cs->opened_frame);

	/* Initialize state */
	lz4_state = palloc0_object(pqcomm_lz4);

	/*
	 * From LZ4F_compressUpdate doc: "Important rule: dstCapacity MUST be
	 * large enough to ensure operation success even in worst case situations.
	 * This value is provided by LZ4F_compressBound()."
	 *
	 * As outBuf is emptied when CHUNK_SIZE is reached, we need to be able to
	 * handle the worst case, i.e. outBuf is filled with CHUNK_SIZE-1 bytes
	 * and LZ4F_compressUpdate needs LZ4F_compressBound() capacity in outBuf.
	 * Thus, we need to allocate twice LZ4F_compressBound(CHUNK_SIZE) to
	 * handle the worst case.
	 */
	outbuf_size = 2 * LZ4F_compressBound(CHUNK_SIZE, &lz4_state->prefs);

	/* Initialize outBuf to be able to store a full chunk */
	Assert(cs->outBuf.data == NULL);
	initStringInfoExt(&cs->outBuf, outbuf_size);

	MemoryContextSwitchTo(oldctx);

	/* and create the lz4 ctx */
	ctxError = LZ4F_createCompressionContext(&lz4_state->ctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
	{
		pfree(lz4_state);
		pfree(cs->outBuf.data);
		return NULL;
	}

	cs->private_data = lz4_state;
	return &PqCompressMethodsLz4;
}

/*
 * pq_compress_message_lz4 -- Compress a message using lz4 compressor
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_message_lz4(pqcomm_compress * cs, bool block, bool flush, bool end_frame)
{
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;
	size_t		remaining = cs->inBuf.len;
	size_t		written = 0;

	if (!lz4_state->frame_started && cs->inBuf.len == 0)
		return 0;

	/* Start new lz4 frame if necessary */
	if (!lz4_state->frame_started)
	{
		written = LZ4F_compressBegin(lz4_state->ctx,
									 cs->outBuf.data + cs->outBuf.len,
									 cs->outBuf.maxlen - cs->outBuf.len,
									 &lz4_state->prefs);
		if (LZ4F_isError(written))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not write lz4 header: %s", LZ4F_getErrorName(written))));

		cs->outBuf.len += written;
		lz4_state->frame_started = true;
	}

	Assert(cs->inBuf.cursor == 0);

	while (remaining > 0)
	{
		/*
		 * To keep memory usage constrained, we cap the amount of data to
		 * compress to CHUNK_SIZE.
		 */
		size_t		chunk = Min(remaining, CHUNK_SIZE);
		size_t		bound = LZ4F_compressBound(chunk, &lz4_state->prefs);

		if (cs->outBuf.maxlen - cs->outBuf.len < bound)
		{
			/* We need to free space in the outBuf, send what we have */
			if (pq_send_compressed_messages(block))
				return EOF;
		}
		Assert(cs->outBuf.maxlen - cs->outBuf.len >= bound);

		written = LZ4F_compressUpdate(lz4_state->ctx,
									  cs->outBuf.data + cs->outBuf.len,
									  cs->outBuf.maxlen - cs->outBuf.len,
									  cs->inBuf.data + cs->inBuf.cursor,
									  chunk,
									  NULL);
		if (LZ4F_isError(written))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not compress data: %s", LZ4F_getErrorName(written))));

		/* Update output buffer len */
		cs->outBuf.len += written;

		/* Update input buffer with consumed chunk */
		cs->inBuf.cursor += chunk;
		remaining = cs->inBuf.len - cs->inBuf.cursor;
	}

	resetStringInfo(&cs->inBuf);

	written = 0;
	if (end_frame)
	{
		written = LZ4F_compressEnd(lz4_state->ctx,
								   cs->outBuf.data + cs->outBuf.len,
								   cs->outBuf.maxlen - cs->outBuf.len,
								   NULL);
		lz4_state->frame_started = false;
	}
	else if (flush)
		written = LZ4F_flush(lz4_state->ctx,
							 cs->outBuf.data + cs->outBuf.len,
							 cs->outBuf.maxlen - cs->outBuf.len,
							 NULL);

	if (LZ4F_isError(written))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not flush lz4 compressed data: %s", LZ4F_getErrorName(written))));

	/* Update output buffer len */
	cs->outBuf.len += written;
	return 0;
}

/*
 * pq_compress_flush_lz4 -- flush any pending data in the compress buffer
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush_lz4(pqcomm_compress * cs, bool block, bool end_frame)
{
	return pq_compress_message_lz4(cs, block, true, end_frame);
}

/*
 * pq_compress_free_lz4 -- free the compressor context
 */
static void
pq_compress_free_lz4(pqcomm_compress * cs)
{
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;

	LZ4F_freeCompressionContext(lz4_state->ctx);
	pfree(lz4_state);
}

#endif
