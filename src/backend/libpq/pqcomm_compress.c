/*-------------------------------------------------------------------------
 *
 * pqcomm_compress.c
 *	  Common routines for backend protocol compression
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "common/compression.h"
#include "libpq/libpq.h"
#include "nodes/pg_list.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/varlena.h"
#include "miscadmin.h"

/* Force flush after a specific number of messages */
int			protocol_backend_compression_number_messages;

/* Minimum byte threshold before compressing messages */
int			protocol_backend_compression_threshold;

/* Force an independent compression frame for each transaction */
bool		protocol_backend_compression_transaction_frame;

/* Bitmask of pg_compress_algorithm values allowed */
int			protocol_backend_compression_allowed_algorithms;

/* Internal functions */
static void pq_compress_comm_reset(void);
static int	pq_compress_flush(void);
static int	pq_compress_flush_if_writable(void);
static int	_pq_compress_flush(void);
static bool pq_compress_is_send_pending(void);
static int	pq_compress_putmessage(char msgtype, const char *s, size_t len);
static void pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len);
static int	_pq_compress_putmessage(char msgtype, const char *s, size_t len, bool block);
static void pq_compress_close(int code, Datum arg);
static int	pq_send_messages(bool block, bool partial);

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

	/* Initialise compressed message types */
	initStringInfo(&cs->msgTypes);

	/* Initialise message buffer */
	initStringInfo(&cs->inBuf);

	/*
	 * If we init outBuf and have the compressors enlarge it, there will be
	 * some wasted memory due to enlargeStringInfo doubling the buffer size.
	 * Thus we let the compressor do the init itself with using
	 * initStringInfoExt.
	 */

	MemoryContextSwitchTo(oldctx);

	cs->algorithm = PG_COMPRESSION_NONE;
	cs->ongoing_compress_message = false;
	cs->ongoing_frame = false;
	PqCompressMethods = NULL;
}

/*
 * pq_send_uncompressed_messages -- Send all uncompressed messages stored in
 * compress state's inBuf.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_send_uncompressed_messages(bool block)
{
	int			cursor = 0;

	Assert(PqCompressBusy);

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

		if (block)
		{
			if (PrevPQcommMethod->putmessage(msgtype, cs->inBuf.data + cursor, length))
				return EOF;
		}
		else
			PrevPQcommMethod->putmessage_noblock(msgtype, cs->inBuf.data + cursor, length);

		cursor += length;

		/* Sanity check */
		Assert(cursor <= cs->inBuf.len);
	};

	resetStringInfo(&cs->inBuf);
	resetStringInfo(&cs->msgTypes);
	return 0;
}

/*
 * pq_send_messages -- Send messages, both compressed and uncompressed messages
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_send_messages(bool block, bool partial)
{
	Assert(PqCompressBusy);

	/* Send compressed payload first */
	if (pq_send_compressed_message(block, partial))
		return EOF;

	/* then send any uncompressed payload */
	return pq_send_uncompressed_messages(block);
}

/*
 * pq_send_compressed_message -- Create a new CompressedMessages with the
 * compress state's outBuf and send it.
 *
 * returns 0 if OK, EOF if trouble
 */
int
pq_send_compressed_message(bool block, bool partial)
{
	StringInfoData buf;

	Assert(PqCompressBusy);

	if (cs->outBuf.len <= 0)
		/* No compressed payload to send */
		return 0;

	pq_beginmessage(&buf, PqMsg_CompressedMessages);

	/* Send compression algorithm used. */
	pq_sendbyte(&buf, cs->algorithm);

	/*
	 * Send the message types, may be 0 len if first message is partial or if
	 * we just close the frame.
	 */
	cs->msgTypes.data[cs->msgTypes.len] = '\0';
	pq_send_ascii_string(&buf, cs->msgTypes.data);
	resetStringInfo(&cs->msgTypes);

	/* Send the compressed payload itself. */
	pq_sendbytes(&buf, cs->outBuf.data, cs->outBuf.len);

	/*
	 * We can't rely on pq_endmessage since it would call
	 * pq_compress_putmessage.
	 */
	if (block)
	{
		if (PrevPQcommMethod->putmessage(buf.cursor, buf.data, buf.len))
			return EOF;
	}
	else
		PrevPQcommMethod->putmessage_noblock(buf.cursor, buf.data, buf.len);
	pfree(buf.data);

	/* With the compressed payload sent, we can reset the output buffer */
	resetStringInfo(&cs->outBuf);

	/*
	 * If the message was partial, we must keep ongoing_compress_message set
	 * as the rest of the message will need one or more
	 * PqMsg_CompressedMessages.
	 */
	if (!partial)
		cs->ongoing_compress_message = false;
	return 0;
}

/*
 * _pq_compress_putmessage -- Put message in compress state's inBuf. If compression
 * threshold is exceeded, the message is compressed and staged in compress state's
 * outBuf.
 *
 * During a flush, the compressed output buffer will be sent in a single
 * CompressedMessages message. If the compression threshold wasn't reached, the
 * uncompressed buffered messages are sent without compression.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
_pq_compress_putmessage(char msgtype, const char *s, size_t len, bool block)
{
	/* No-op if reentrant call */
	if (PqCompressBusy)
		return 0;
	PqCompressBusy = true;

	pq_sendbyte(&cs->inBuf, msgtype);
	pq_sendint32(&cs->inBuf, len + 4);
	pq_sendbytes(&cs->inBuf, s, len);

	/*
	 * Transactional proxies need to inspect the content of some packets. To
	 * avoid having the proxy decompress the whole session for that purpose,
	 * we force those messages to always be sent uncompressed. For
	 * BackendKeyData, since the content is random bytes, there's no benefit
	 * to compress it.
	 */
	if (msgtype == PqMsg_ReadyForQuery
		|| msgtype == PqMsg_ErrorResponse
		|| msgtype == PqMsg_ParameterStatus
		|| msgtype == PqMsg_CommandComplete
		|| msgtype == PqMsg_BackendKeyData)
	{
		if (cs->ongoing_frame)
		{
			/*
			 * If transaction frame is enabled, we need to close the current
			 * frame if we just finished a transaction.
			 */
			bool		end_frame = msgtype == PqMsg_ReadyForQuery &&
				protocol_backend_compression_transaction_frame && !IsTransactionBlock();

			if (end_frame || cs->ongoing_compress_message)
			{
				/* Flush any compressed payload first */
				if (PqCompressMethods->flush(cs, block, end_frame))
					goto fail;
				if (pq_send_compressed_message(block, false))
					goto fail;
			}
			if (end_frame)
				cs->ongoing_frame = false;
		}

		/* And send the uncompressed message */
		if (pq_send_uncompressed_messages(block))
			goto fail;

		PqCompressBusy = false;
		return 0;
	}

	if (cs->ongoing_compress_message || cs->inBuf.len > protocol_backend_compression_threshold)
	{
		/*
		 * We either crossed the compress threshold, or some messages were
		 * already compressed, compress this message
		 */
		cs->ongoing_compress_message = true;
		cs->ongoing_frame = true;
		if (PqCompressMethods->compress_message(cs, block))
			goto fail;
		resetStringInfo(&cs->inBuf);
	}

	/* Keep track of the message type */
	appendStringInfoChar(&cs->msgTypes, msgtype);

	/* Send messages if we've crossed the number of messages threshold */
	if (cs->ongoing_compress_message
		&& protocol_backend_compression_number_messages > 0
		&& cs->msgTypes.len > protocol_backend_compression_number_messages)
		if (_pq_compress_flush())
			goto fail;

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
 * _pq_compress_flush -- flush pending messages
 *
 * returns 0 if OK, EOF if trouble
 */
static int
_pq_compress_flush(void)
{
	if (cs->ongoing_compress_message
		&& (PqCompressMethods->flush(cs, true, false)))
		return EOF;

	/*
	 * We may have uncompressed messages currently buffered, so call
	 * pq_send_messages to send uncompressed and compressed payload.
	 */
	if (pq_send_messages(true, false))
		return EOF;

	return PrevPQcommMethod->flush();
}

/*
 * pq_compress_flush -- flush pending messages
 *
 * If we have an ongoing compressed message, force the compressor to flush any
 * buffered data and send it.
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
	if (_pq_compress_flush())
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
	if (cs->ongoing_compress_message
		&& PqCompressMethods->flush(cs, false, false))
		goto fail;

	if (pq_send_messages(false, false))
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
 * pq_compress_is_send_pending -- is there any pending data in the output buffer?
 */
static bool
pq_compress_is_send_pending(void)
{
	return cs->ongoing_compress_message
		|| cs->inBuf.len > 0
		|| PrevPQcommMethod->is_send_pending();
}

/*
 * pq_compress_putmessage -- add message to the compress buffer
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_putmessage(char msgtype, const char *s, size_t len)
{
	return _pq_compress_putmessage(msgtype, s, len, true);
}

/*
 * pq_compress_putmessage_noblock -- add message to the compress buffer without blocking
 */
static void
pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	_pq_compress_putmessage(msgtype, s, len, false);
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
	void	   *new_private_data = NULL;
	bool		no_compression_enabled;
	bool		no_compression_change;
	pg_compress_algorithm algorithm;
	pg_compress_specification specification;
	const		PQcompressMethods *new_compress_methods = NULL;

	if (MyProcPort == NULL)
		/* If there's no client connection, just ignore compression settings */
		return;

	/* Extract algorithm and specification */
	parse_compress_options(newval, &algorithm_name, &detail);
	if (!parse_compress_algorithm(algorithm_name, &algorithm))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid value for parameter \"protocol_backend_compression\": \"%s\"",
						newval)));
	parse_compress_specification(algorithm, detail, &specification);

	no_compression_enabled = !cs && algorithm == PG_COMPRESSION_NONE;
	no_compression_change = cs && cs->algorithm == algorithm
		&& cs->specification.level == specification.level
		&& cs->specification.long_distance == specification.long_distance;
	if (no_compression_enabled || no_compression_change)
		/* No change or compression isn't enabled, nothing to do */
		return;

	if (algorithm != PG_COMPRESSION_NONE &&
		(protocol_backend_compression_allowed_algorithms & (1 << algorithm)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s compression is not permitted by \"protocol_backend_compression_allowed_algorithms\"",
						get_compress_algorithm_name(algorithm))));

	if (!cs)
	{
		/*
		 * First time we have a non null compression algo, initialize
		 * compression state.
		 */
		init_compress_state();
		on_proc_exit(pq_compress_close, 0);
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
			new_compress_methods = pq_init_compressor_zstd(cs, &specification, &new_private_data);
			break;
		case PG_COMPRESSION_LZ4:
			if (!MyProcPort->supported_compress_lz4)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lz4 compression is not supported by client")));
			}
			new_compress_methods = pq_init_compressor_lz4(cs, &specification, &new_private_data);
			break;
		case PG_COMPRESSION_GZIP:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gzip compression is not supported")));
			break;
		case PG_COMPRESSION_NONE:
			break;
	};

	/*
	 * We managed to create the new compressor context successfully. If we
	 * have an opened frame, we need to close allowing the specification
	 * change to be used with a new frame.
	 */
	if (cs->ongoing_frame)
	{
		Assert(cs->algorithm != PG_COMPRESSION_NONE);
		if (PqCompressMethods->flush(cs, false, true))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not flush compressed data")));
		cs->ongoing_frame = false;

		PqCompressBusy = true;
		if (pq_send_messages(false, false))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not send compressed data")));
		PqCompressBusy = false;
	}

	if (cs->algorithm != algorithm && PqCompressMethods != NULL)
	{
		/*
		 * That's an algorithm change, we need to drop the previous
		 * compression context.
		 */
		PqCompressMethods->free_compress_context(cs);
	}

	if (PrevPQcommMethod == NULL && new_compress_methods != NULL)
	{
		/* Compression is enabled, replace the global PqCommMethods */
		PrevPQcommMethod = PqCommMethods;
		PqCommMethods = &PqCommCompressMethods;
	}
	else if (new_compress_methods == NULL && PrevPQcommMethod != NULL)
	{
		/*
		 * Compression is disabled, restore PqCommMethods to the previous
		 * value
		 */
		PqCommMethods = PrevPQcommMethod;
		PrevPQcommMethod = NULL;
	}

	/* And update the global compress methods */
	PqCompressMethods = new_compress_methods;

	/* And update compress state */
	cs->private_data = new_private_data;
	cs->algorithm = algorithm;
	cs->specification = specification;
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

/*
 * pq_compress_close -- free compressor memory at backend exit
 */
static void
pq_compress_close(int code, Datum arg)
{
	Assert(cs);
	if (PqCompressMethods != NULL)
		PqCompressMethods->free_compress_context(cs);
	if (cs->outBuf.data != NULL)
		pfree(cs->outBuf.data);
	pfree(cs->msgTypes.data);
	pfree(cs->inBuf.data);
	pfree(cs);
}
