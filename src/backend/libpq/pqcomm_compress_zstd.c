/*-------------------------------------------------------------------------
 *
 * pqcomm_compress_zstd.c
 *	  Compress backend messages with zstd
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress_zstd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/libpq.h"
#include "utils/memutils.h"

#ifndef USE_ZSTD

const		PQcompressMethods *
pq_init_compressor_zstd(pqcomm_compress * cs)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("zstd compression is not supported by this build")));
}

#else
#include <zstd.h>

typedef struct pqcomm_zstd
{
	ZSTD_CCtx  *cctx;
}			pqcomm_zstd;

static int	pq_compress_flush_zstd(pqcomm_compress * cs, bool block, bool end_frame);
static int	pq_compress_message_zstd(pqcomm_compress * cs, bool block, bool flush, bool end_frame);
static void pq_compress_free_zstd(pqcomm_compress * cs);

static const PQcompressMethods PqCompressMethodsZstd = {
	.flush = pq_compress_flush_zstd,
	.compress_message = pq_compress_message_zstd,
	.free_compress_context = pq_compress_free_zstd,
};

/*
 * pq_init_compressor_zstd -- Initialize a zstd compressor
 *
 * On success, the compressor state will be stored in cs->private_data
 *
 * returns PQcompressMethods of the zstd compressor, NULL on failure
 */
const		PQcompressMethods *
pq_init_compressor_zstd(pqcomm_compress * cs)
{
	pqcomm_zstd *zstate;

	/* We have an algorithm change, create a brand-new context */
	MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

	Assert(!cs->opened_frame);
	Assert(cs->algorithm == PG_COMPRESSION_NONE);
	Assert(cs->outBuf.data == NULL);

	/* Initialize compress buffer */
	initStringInfoExt(&cs->outBuf, ZSTD_CStreamOutSize());

	zstate = palloc0_object(pqcomm_zstd);
	MemoryContextSwitchTo(oldctx);

	/* Create ctx */
	zstate->cctx = ZSTD_createCCtx();
	if (!zstate->cctx)
	{
		pfree(zstate);
		pfree(cs->outBuf.data);
		return NULL;
	}

	cs->private_data = zstate;
	return &PqCompressMethodsZstd;
}


/*
 * pq_compress_message_zstd -- Compress a message using zstd compressor
 *
 * If the compress buffer is full, it will be sent immediately, allowing to
 * reset the buffer and compress the rest of the message.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_message_zstd(pqcomm_compress * cs, bool block, bool flush, bool end_frame)
{
	ZSTD_inBuffer inBuf;
	size_t		yet_to_flush = 0;
	ZSTD_outBuffer outBuf;
	pqcomm_zstd *zstate = (pqcomm_zstd *) cs->private_data;
	ZSTD_EndDirective end_op = ZSTD_e_continue;

	inBuf.src = cs->inBuf.data;
	inBuf.size = cs->inBuf.len;
	inBuf.pos = 0;

	outBuf.dst = cs->outBuf.data;
	outBuf.size = cs->outBuf.maxlen;
	outBuf.pos = cs->outBuf.len;

	if (end_frame)
		end_op = ZSTD_e_end;
	else if (flush)
		end_op = ZSTD_e_flush;

	do
	{
		yet_to_flush =
			ZSTD_compressStream2(zstate->cctx,
								 &outBuf,
								 &inBuf, end_op);
		if (ZSTD_isError(yet_to_flush))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not compress data: %s",
							ZSTD_getErrorName(yet_to_flush))));
		/* Keep compress buffer in sync */
		cs->outBuf.len = outBuf.pos;

		/*
		 * If the output buffer is left with not enough space, send the
		 * compressed bytes to the underlying pqcomm, which will empty the
		 * buffer.
		 */
		if (yet_to_flush > 0)
		{
			if (pq_send_compressed_messages(block) == EOF)
				return EOF;
			/* sync position after pq_send_compressed_messages call */
			outBuf.pos = cs->outBuf.len;
		}
	} while (yet_to_flush > 0);

	resetStringInfo(&cs->inBuf);
	return 0;
}

/*
 * pq_compress_flush_zstd -- flush any pending data in the compress buffer
 *
 * If the compress buffer is full, it will be sent immediately, allowing to
 * reset the buffer and compress the rest of the data.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush_zstd(pqcomm_compress * cs, bool block, bool end_frame)
{
	return pq_compress_message_zstd(cs, block, true, end_frame);
}

/*
 * pq_compress_free_zstd -- free the compressor context
 */
static void
pq_compress_free_zstd(pqcomm_compress * cs)
{
	pqcomm_zstd *zstate = (pqcomm_zstd *) cs->private_data;

	ZSTD_freeCCtx(zstate->cctx);
	pfree(zstate);
}

#endif
