/*-------------------------------------------------------------------------
 *
 * fe-compress.c
 *	  Decompression support for frontend/backend protocol
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-compress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <string.h>

#include "common/compression.h"
#include "libpq-fe.h"
#include "libpq-int.h"

#define LZ4_FRAME_MAGIC 0x4224D18
#define ZSTD_FRAME_MAGIC 0x28B52FFD

static int	pqGetMsgLen(PGconn *conn, msg_buffer * msgBuf);
static bool pqHasFullMessage(PGconn *conn, msg_buffer * msgBuf);

/*
 * pqReadCompressedMessage: Read the content of a CompressedMessages message.
 *
 * returns EOF if not enough data, -2 if trouble, 0 if CompressedMessages
 * content has been fully processed, 1 CompressedMessages payload is ready to
 * be decompressed with pqDecompressPayload
 */
int
pqReadCompressedMessage(PGconn *conn, int msgLength)
{
	int			endCompressMessage = conn->inBuffer.start + msgLength + 5;

	if (conn->compress_cursor > 0
		&& conn->compress_cursor == msgLength + 5)
	{
		/*
		 * The CompressedMessages was fully processed. Advance inBuffer so the
		 * message can be tagged as consumed.
		 */
		conn->inBuffer.cursor = endCompressMessage;
		conn->compress_cursor = 0;
		return 0;
	}

	if (conn->compress_algorithm == PG_COMPRESSION_NONE)
	{
		/* First frame, detect the algorithm used using frame magic number */
		int			magic;

		if (pqGetInt(&magic, 4, conn, &conn->inBuffer))
			return EOF;
		conn->inBuffer.cursor -= 4;

		if (magic == LZ4_FRAME_MAGIC)
		{
			if (pqInitDecompressorLz4(conn))
				return -2;
			conn->compress_algorithm = PG_COMPRESSION_LZ4;
		}
		else if (magic == ZSTD_FRAME_MAGIC)
		{
			if (pqInitDecompressorZstd(conn))
				return -2;
			conn->compress_algorithm = PG_COMPRESSION_ZSTD;
		}
		else
		{
			libpq_append_conn_error(conn, "Unknown compression frame magic number 0x%x", magic);
			return -2;
		}
	}

	/*
	 * We have a payload to decompress, set compress_cursor and compress_end
	 * to target target the compressed payload.
	 */
	conn->compress_cursor = conn->inBuffer.cursor - conn->inBuffer.start;
	conn->compress_end = endCompressMessage - conn->inBuffer.start;
	return 1;
}

/*
 * pqDecompressPayload: Decompress into the decompress buffer until:
 * - A full message is available for processing.
 * Or
 * - All input bytes are processed and the decompress buffer isn't full.
 *
 * Due to keeping the compression frame opened in a transaction, it's possible
 * to have cursor==end while the compressor still has bytes to output, but
 * couldn't due to decompressBuffer being full. We rely on decompressBuffer
 * being full as a condition to run one more pq_decompress_payload to make sure
 * there's nothing stuck in the decompressor's buffers.
 *
 * returns EOF if not enough data, -2 if trouble, 0 if there's at least one
 * full message available
 */
int
pqDecompressPayload(PGconn *conn)
{
	bool		outBufferFull;

	if (pqHasFullMessage(conn, &conn->decompressBuffer))
	{
		/* There's already a full message in decompress buffer */
		return 0;
	}

	outBufferFull = conn->decompressBuffer.end == conn->decompressBuffer.bufSize;
	while (conn->compress_cursor < conn->compress_end || outBufferFull)
	{
		if (outBufferFull)
		{
			int			msgLen = pqGetMsgLen(conn, &conn->decompressBuffer);
			size_t		bytes_needed;

			if (msgLen == -1)
			{
				/*
				 * Not enough bytes for the msg length. Either the last
				 * message is in the last 5 bytes of decompressBuffer, or
				 * everything was processed. Just ask for 4 bytes, and
				 * pqCheckMsgBufferSpace will clean the already processed
				 * messages.
				 */
				msgLen = 4;
			}
			/* bytes_needed needs to include the starting 1 byte id. */
			bytes_needed = Max(msgLen + 1, conn->decompress_chunk_size);

			/*
			 * decompressBuffer is full, and we don't have a full message,
			 * call pqCheckMsgBufferSpace to either left justify the content
			 * or enlarge the buffer. Once space is freed, decompress needs to
			 * be called again to give the decompressor a chance to flush its
			 * internal buffers.
			 */
			if (pqCheckMsgBufferSpace(bytes_needed + conn->decompressBuffer.start, &conn->decompressBuffer, conn))
			{
				return -2;
			}
		}

		/*
		 * We should always have available bytes before calling
		 * decompress_payload
		 */
		Assert(conn->decompressBuffer.bufSize > conn->decompressBuffer.end);

		/* Do the decompression */
		if (conn->decompressor.decompress_payload(conn))

			/*
			 * there was a fatal error while decompressing the payload, bail
			 * out
			 */
			return -2;

		if (pqHasFullMessage(conn, &conn->decompressBuffer))

			/*
			 * We have at least one full message, exit to let the outer loop
			 * consume it
			 */
			return 0;

		outBufferFull = conn->decompressBuffer.end == conn->decompressBuffer.bufSize;
	}
	return EOF;
}

/*
 * Returns the length of the next message in the msgBuf
 *
 * Returns EOF if there's not enough bytes to read the length
 */
static int
pqGetMsgLen(PGconn *conn, msg_buffer * msgBuf)
{
	char		id;
	int			msgLength;

	msgBuf->cursor = msgBuf->start;
	if (pqGetc(&id, conn, msgBuf))
		return EOF;
	if (pqGetInt(&msgLength, 4, conn, msgBuf))
		return EOF;

	return msgLength;
}

/*
 * Returns true if there's a full message in the msgBuf
 */
static bool
pqHasFullMessage(PGconn *conn, msg_buffer * msgBuf)
{
	int			len = pqGetMsgLen(conn, msgBuf);
	int			available = msgBuf->end - msgBuf->start;

	if (len < 0)
		return false;

	/* Available bytes needs to include the 1 byte id */
	return available >= 1 + len;
}
