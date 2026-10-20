/*-------------------------------------------------------------------------
 *
 * pqcomm_compress.c
 *	  Common routines for backend protocol compression
 *
 * These routines add a compression layer before messages are forwarded to the
 * underlying communication layer.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress.c
 *
 *-------------------------------------------------------------------------
 */

/*------------------------
 * INTERFACE ROUTINES
 *
 * low-level I/O:
 *		pq_compress_flush - forward buffered messages to the transport layer and
 *		flush it
 *		pq_compress_flush_if_writable - forward buffered messages to the transport
 *		layer and flush it without blocking
 *
 * message-level I/O
 *		pq_compress_putmessage	- buffer a message for compression
 *		pq_compress_putmessage_noblock - buffer a message for compression without
 *		blocking
 *
 *------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "common/compression.h"
#include "libpq/libpq.h"
#include "nodes/pg_list.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/varlena.h"
#include "miscadmin.h"

/* Force flush after a specific number of messages */
int			protocol_backend_compression_number_messages;

/* Minimum byte threshold before compressing messages */
int			protocol_backend_compression_threshold;

/* Bitmask of pg_compress_algorithm values allowed */
int			protocol_backend_compression_allowed_algorithms;

/* Internal functions */
static void pq_compress_comm_reset(void);
static int	pq_compress_flush(void);
static int	pq_compress_flush_if_writable(void);
static bool pq_compress_is_send_pending(void);
static int	pq_compress_putmessage(char msgtype, const char *s, size_t len);
static void pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len);

static int	compress_putmessage(char msgtype, const char *s, size_t len, bool block);
static int	flush_buffered_messages(bool block, bool end_frame);
static void disable_compression(void);
static int	prev_put_message(char msgtype, const char *s, size_t len, bool block);

static bool PqCompressBusy;		/* busy handling message */

static const PQcommMethods *PrevPQcommMethod = NULL;
static const PQcompressMethods *PqCompressMethods = NULL;

static pqcomm_compress * cs = NULL;
static const PQcommMethods PqCommCompressMethods = {
	.comm_reset = pq_compress_comm_reset,
	.flush = pq_compress_flush,
	.flush_if_writable = pq_compress_flush_if_writable,
	.is_send_pending = pq_compress_is_send_pending,
	.putmessage = pq_compress_putmessage,
	.putmessage_noblock = pq_compress_putmessage_noblock
};

/*
 * init_compress_state -- initialize compression context
 */
static void
init_compress_state(void)
{
	MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

	Assert(cs == NULL);

	/* First time, create the context */
	cs = palloc0_object(pqcomm_compress);

	/* Initialize uncompressed message buffer */
	initStringInfo(&cs->inBuf);

	/*
	 * If we init outBuf and have the compressors enlarge it, there will be
	 * some wasted memory due to enlargeStringInfo doubling the buffer size.
	 * Thus we let the compressor itself do the init using initStringInfoExt.
	 */

	MemoryContextSwitchTo(oldctx);

	PqCompressMethods = NULL;
}

/*
 * pq_send_compressed_messages -- Build a new CompressedMessages with the
 * compressed content cs->outBuf and send it.
 *
 * returns 0 if OK, EOF if trouble
 */
int
pq_send_compressed_messages(bool block)
{
	StringInfoData buf;

	Assert(PqCompressBusy);

	if (cs->outBuf.len <= 0)
		/* No compressed payload to send */
		return 0;

	/* Build the CompressedMessages */
	pq_beginmessage(&buf, PqMsg_CompressedMessages);
	pq_sendbytes(&buf, cs->outBuf.data, cs->outBuf.len);

	/* And send it using the previous PqCommMethods */
	if (prev_put_message(buf.cursor, buf.data, buf.len, block))
	{
		pfree(buf.data);
		return EOF;
	}
	pfree(buf.data);

	/* With the compressed payload sent, we can reset the output buffer */
	resetStringInfo(&cs->outBuf);
	cs->num_compressed_messages = 0;

	return 0;
}

/*
 * pq_send_uncompressed_messages -- Send all uncompressed messages stored in
 * cs->inBuf.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_send_uncompressed_messages(bool block)
{
	int			cursor = 0;

	Assert(PqCompressBusy);

	/*
	 * We should only send uncompressed messages if the compress output buffer
	 * is empty
	 */
	Assert(cs->outBuf.len == 0);

	if (cs->inBuf.len <= 0)
		/* Nothing to send, exit */
		return 0;

	while (cursor < cs->inBuf.len)
	{
		int			length;
		char		msgtype = cs->inBuf.data[cursor++];

		/* Read back the length from the stored message */
		memcpy(&length, cs->inBuf.data + cursor, 4);
		/* Length includes the 4-byte length, subtract it */
		length = (int) pg_ntoh32(length) - 4;
		cursor += 4;

		if (prev_put_message(msgtype, cs->inBuf.data + cursor, length, block))
			return EOF;

		cursor += length;

		/* Sanity check */
		Assert(cursor <= cs->inBuf.len);
	};

	/* Everything was sent, we can reset the uncompressed buffer */
	resetStringInfo(&cs->inBuf);
	cs->num_compressed_messages = 0;
	return 0;
}

/*
 * flush_buffered_messages -- flush compressor and forward buffered messages to
 * the transport layer
 *
 * returns 0 if OK, EOF if trouble
 */
static int
flush_buffered_messages(bool block, bool end_frame)
{
	Assert(PqCompressBusy);

	if (cs->opened_frame)
	{
		/*
		 * We have an opened compression frame, force the compressor to flush
		 * everything in outBuf
		 */
		if (PqCompressMethods->flush(cs, block, end_frame))
			return EOF;
		if (end_frame)
			cs->opened_frame = false;

		/* Then build and send the CompressedMessages */
		return pq_send_compressed_messages(block);
	}

	/* No compression, just send any buffered uncompressed messages we have */
	return pq_send_uncompressed_messages(block);
}

/*
 * compress_putmessage -- Buffer a message for compression. If the compression
 * threshold is reached, the message is compressed and stored in cs->outBuf.
 *
 * During a flush, the compressed output buffer will be sent in a single
 * CompressedMessages message. If the compression threshold wasn't reached, the
 * buffered uncompressed messages are sent without compression.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
compress_putmessage(char msgtype, const char *s, size_t len, bool block)
{
	/* No-op if reentrant call */
	if (PqCompressBusy)
		return 0;
	PqCompressBusy = true;

	if (msgtype != PqMsg_DataRow && msgtype != PqMsg_CopyData)
	{
		/*
		 * Only DataRow and CopyData are compressed, other messages are sent
		 * uncompressed.
		 */

		/*
		 * First, we need to send any messages buffered. If it's a transaction
		 * end, close the compression frame
		 */
		bool		end_frame = msgtype == PqMsg_ReadyForQuery
			&& cs->opened_frame
			&& !IsTransactionBlock();

		if (flush_buffered_messages(block, end_frame))
			goto fail;

		/* Then send the uncompressed message using PrevPQcommMethod */
		if (prev_put_message(msgtype, s, len, block))
			goto fail;

		PqCompressBusy = false;
		return 0;
	}

	/* We have a message that could be compressed, put it in inBuf */
	pq_sendbyte(&cs->inBuf, msgtype);
	pq_sendint32(&cs->inBuf, len + 4);
	/* message may be empty */
	if (len > 0)
		pq_sendbytes(&cs->inBuf, s, len);

	if (cs->inBuf.len > protocol_backend_compression_threshold)
	{
		/*
		 * We either crossed the compress threshold, or the threshold was
		 * already crossed with previous messages. Enable compression.
		 */
		cs->opened_frame = true;
	}

	cs->num_compressed_messages++;

	if (cs->opened_frame)
	{
		if (PqCompressMethods->compress_message(cs, block, false, false))
			goto fail;

		/* Flush if we've crossed the number of messages threshold */
		if (protocol_backend_compression_number_messages > 0
			&& cs->num_compressed_messages >= protocol_backend_compression_number_messages)
			if (flush_buffered_messages(block, false))
				goto fail;
	}

	PqCompressBusy = false;
	return 0;

fail:
	PqCompressBusy = false;
	return EOF;
}

/*
 * pq_compress_comm_reset -- reset libpq during error recovery
 */
static void
pq_compress_comm_reset(void)
{
	/* Do not throw away pending data, but do reset the busy flag */
	PqCompressBusy = false;
	PrevPQcommMethod->comm_reset();
}

/*
 * pq_compress_flush -- flush pending messages
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush(void)
{
	/* No-op if reentrant call */
	if (PqCompressBusy)
		return 0;

	PqCompressBusy = true;
	if (flush_buffered_messages(true, false))
		goto fail;
	if (PrevPQcommMethod->flush())
		goto fail;
	PqCompressBusy = false;
	return 0;

fail:
	PqCompressBusy = false;
	return EOF;
}

/*
 * pq_compress_flush_if_writable -- flush pending messages without blocking
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush_if_writable(void)
{
	/* No-op if reentrant call */
	if (PqCompressBusy)
		return 0;

	PqCompressBusy = true;
	if (flush_buffered_messages(false, false))
		goto fail;
	if (PrevPQcommMethod->flush_if_writable())
		goto fail;
	PqCompressBusy = false;
	return 0;

fail:
	PqCompressBusy = false;
	return EOF;
}

/*
 * pq_compress_is_send_pending -- is there any pending data?
 */
static bool
pq_compress_is_send_pending(void)
{
	return cs->inBuf.len > 0
		|| cs->outBuf.len > 0
		|| PrevPQcommMethod->is_send_pending();
}

/*
 * pq_compress_putmessage -- buffer a message for compression
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_putmessage(char msgtype, const char *s, size_t len)
{
	return compress_putmessage(msgtype, s, len, true);
}

/*
 * pq_compress_putmessage_noblock -- buffer a message for compression without
 * blocking
 */
static void
pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	compress_putmessage(msgtype, s, len, false);
}

/*
 * disable_compression -- Disable compression by restoring previous
 * PqCommMethods
 */
static void
disable_compression(void)
{
	if (!cs)
		/* Compression was never enabled, nothing to do */
		return;

	/*
	 * We also need to send all uncompressed and compressed bytes before
	 * disabling the compressor.
	 */
	PqCompressBusy = true;
	if (flush_buffered_messages(false, cs->opened_frame))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not flush compressed data")));
	}
	PqCompressBusy = false;

	/*
	 * Compression is disabled, restore PqCommMethods to the previous value
	 */
	PqCommMethods = PrevPQcommMethod;
}

/*
 * prev_put_message -- put message using previous PqCommMethods
 *
 * returns 0 if OK, EOF if trouble
 */
static int
prev_put_message(char msgtype, const char *s, size_t len, bool block)
{
	if (block)
		return PrevPQcommMethod->putmessage(msgtype, s, len);
	PrevPQcommMethod->putmessage_noblock(msgtype, s, len);
	return 0;
}

bool
check_protocol_backend_compression(char **newval, void **extra, GucSource source)
{
	char	   *algorithm_name = NULL;
	char	   *detail = NULL;
	pg_compress_algorithm algorithm;
	const		PQcompressMethods *new_compress_methods = NULL;
  char	   *str = *newval;

	if (MyProcPort == NULL)
		/* If there's no client connection, just ignore compression setting */
		return true;

	/* Extract algorithm */
	parse_compress_options(str, &algorithm_name, &detail);
	if (!parse_compress_algorithm(algorithm_name, &algorithm)) {
    GUC_check_errdetail( "invalid value for parameter \"protocol_backend_compression\": \"%s\"",
						str);
    return false;
  }

	if (algorithm == PG_COMPRESSION_NONE)
		return true;

	/* Is the compression algorithm allowed? */
	if ((protocol_backend_compression_allowed_algorithms & (1 << algorithm)) == 0) {
    GUC_check_errdetail("%s compression is not permitted by \"protocol_backend_compression_allowed_algorithms\"",
						get_compress_algorithm_name(algorithm));
    return false;
  }

	if (cs && cs->algorithm != algorithm) {
    GUC_check_errdetail("Session was already initialized with %s compression, changing compression is not supported",
							get_compress_algorithm_name(cs->algorithm));
    return false;
  }

	switch (algorithm)
	{
		case PG_COMPRESSION_ZSTD:
			if (!MyProcPort->supported_compress_zstd)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("zstd compression is not supported by client")));
			}
			break;
		case PG_COMPRESSION_LZ4:
			if (!MyProcPort->supported_compress_lz4)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lz4 compression is not supported by client")));
			}
			break;
		case PG_COMPRESSION_GZIP:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gzip compression is not supported")));
			break;
		case PG_COMPRESSION_NONE:
			pg_unreachable();
	};

  return true;
}

/*
 * GUC assign hook for protocol_backend_compression: switch PqCommMethods to the
 * implementation matching the newly selected compression method.
 */
void
assign_protocol_backend_compression(const char *newval, void *extra)
{
	char	   *algorithm_name = NULL;
	char	   *detail = NULL;
	pg_compress_algorithm algorithm;
	const		PQcompressMethods *new_compress_methods = NULL;

	if (MyProcPort == NULL)
		/* If there's no client connection, just ignore compression setting */
		return;

	/* Extract algorithm */
	parse_compress_options(newval, &algorithm_name, &detail);
	if (!parse_compress_algorithm(algorithm_name, &algorithm))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid value for parameter \"protocol_backend_compression\": \"%s\"",
						newval)));

	if (algorithm == PG_COMPRESSION_NONE)
	{
		disable_compression();
		return;
	}

	/* Is the compression algorithm allowed? */
	if ((protocol_backend_compression_allowed_algorithms & (1 << algorithm)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s compression is not permitted by \"protocol_backend_compression_allowed_algorithms\"",
						get_compress_algorithm_name(algorithm))));

	if (cs)
	{
		/*
		 * We have an existing compression state, check that we try to
		 * re-enable the same compression algorithm
		 */
		if (cs->algorithm != algorithm)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Session was already initialized with %s compression, changing compression is not supported",
							get_compress_algorithm_name(cs->algorithm))));

		/*
		 * Re-enable compression by hooking back PqCommMethods to the compress
		 * methods
		 */
		PqCommMethods = &PqCommCompressMethods;
		return;
	}

	/*
	 * First time we have a non-null compression algorithm, initialize
	 * compression state.
	 */
	init_compress_state();

	switch (algorithm)
	{
		case PG_COMPRESSION_ZSTD:
			if (!MyProcPort->supported_compress_zstd)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("zstd compression is not supported by client")));
			}
			new_compress_methods = pq_init_compressor_zstd(cs);
			break;
		case PG_COMPRESSION_LZ4:
			if (!MyProcPort->supported_compress_lz4)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lz4 compression is not supported by client")));
			}
			new_compress_methods = pq_init_compressor_lz4(cs);
			break;
		case PG_COMPRESSION_GZIP:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gzip compression is not supported")));
			break;
		case PG_COMPRESSION_NONE:
			pg_unreachable();
	};

	if (new_compress_methods == NULL)
	{
		/*
		 * Compressor initialization failed, clean up any initialized state
		 */
		pfree(cs->inBuf.data);
		pfree(cs);
		cs = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not create %s compression context", get_compress_algorithm_name(algorithm))));
	}

	Assert(PrevPQcommMethod == NULL);
	Assert(PqCompressMethods == NULL);

	/* Set the Pq*Methods hooks */
	PrevPQcommMethod = PqCommMethods;
	PqCompressMethods = new_compress_methods;
	PqCommMethods = &PqCommCompressMethods;
	cs->algorithm = algorithm;
}

/*
 * GUC check hook for protocol_backend_compression_allowed_algorithms: parse the
 * comma-separated list of algorithm names and store it as a bitmask of
 * pg_compress_algorithm values.
 *
 * Returns an error if the compression algorithm is not supported.
 */
bool
check_protocol_backend_allowed_algorithms(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			flags = 0;
	bool		result = true;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".",
							"protocol_backend_compression_allowed_algorithms");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *item = (char *) lfirst(l);
		pg_compress_algorithm algorithm;

		if (!parse_compress_algorithm(item, &algorithm))
		{
			GUC_check_errdetail("Invalid compression algorithm \"%s\".", item);
			result = false;
			break;
		}
		if (algorithm == PG_COMPRESSION_GZIP)
		{
			GUC_check_errdetail("gzip compression is not supported");
			result = false;
			break;
		}
#ifndef USE_ZSTD
		if (algorithm == PG_COMPRESSION_ZSTD)
		{
			GUC_check_errdetail("zstd compression is not supported by this build");
			result = false;
			break;
		}
#endif
#ifndef USE_LZ4
		if (algorithm == PG_COMPRESSION_LZ4)
		{
			GUC_check_errdetail("lz4 compression is not supported by this build");
			result = false;
			break;
		}
#endif
		flags |= (1 << algorithm);
	}

	pfree(rawstring);
	list_free(elemlist);

	if (!result)
		return result;

	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return result;
}

/*
 * GUC assign hook for protocol_backend_compression_allowed_algorithms.
 */
void
assign_protocol_backend_allowed_algorithms(const char *newval, void *extra)
{
	protocol_backend_compression_allowed_algorithms = *((int *) extra);
}
