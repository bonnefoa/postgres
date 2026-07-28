-- Test protocol compression with lz4

SET protocol_backend_compression_allowed_algorithms = 'lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 (not supported) ***'
   \quit
\endif

-------------------------
-- Enable/Disable compression
-------------------------

-- Force threshold to 0 to compress every message
SET protocol_backend_compression_threshold=0;

-- Disable compression without active compression
SET protocol_backend_compression = 'none';

-- Test lz4 -> none -> lz4 -> none
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';

-- Test lz4 -> none -> lz4 + using compressor
SET protocol_backend_compression = 'lz4';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'lz4';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;

-- Test unsupported algorithm
SET protocol_backend_compression = 'gzip';

-- Test none -> unsupported -> bogus
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'gzip';
SET protocol_backend_compression = 'bogus';

-- While we are below the compression threshold, message bytes are stored in
-- pqcompress' buffer. Setting compression to none should flush stored messages
-- immediately
SET protocol_backend_compression_threshold=100;
\startpipeline
SELECT 1 \parse one
SET protocol_backend_compression='none';
\endpipeline

-- Test enabling compression within a transaction
SET protocol_backend_compression_threshold=0;
BEGIN;
SET LOCAL protocol_backend_compression='lz4';
SELECT 1;
COMMIT;

-- Test enabling compression within an implicit transaction
\startpipeline
SET LOCAL protocol_backend_compression='lz4';
SELECT 1;
\endpipeline

-------------------------
-- Test Simple/Extended/Copy Queries
-------------------------

SET protocol_backend_compression='lz4';
SET protocol_backend_compression_threshold=0;

-- Check simple query
SELECT 1;

-- Check extended query
SELECT $1::text, '42', $1::numeric, interval '1 sec' \bind 1 \g

-- Check copy
\copy (SELECT *, repeat(' ', 20) FROM generate_series(1, 3)) to stdout;

-- Check Notify messages are compressed
LISTEN test_zstd;
NOTIFY test_zstd, 'test';
UNLISTEN test_zstd;

-- Removing allowed algorithm should leave the current compression
SET protocol_backend_compression_allowed_algorithms = '';
SELECT 1;
SET protocol_backend_compression_allowed_algorithms = 'lz4';

-- A flush request should force the uncompressed messages to be sent
-- immediately
\startpipeline
SELECT 1 \parse test
\flushrequest
\flush
\getresults
SELECT repeat('a', 20);
\endpipeline

-- Frame should be closed on ROLLBACK
BEGIN;
SELECT 1;
ROLLBACK;

-- We need to temporarily disable debug_parallel_query here. When the
-- error is triggered within a parallel worker, no data rows will be
-- sent which will lead to a different protocol trace.
SET debug_parallel_query=false;

-- Check triggering an error
-- Stay below the compression threshold
SET protocol_backend_compression_threshold=1000;
select 1 / i FROM generate_series(2, 0, -1) AS a(i);
-- Cross the compression threshold
SET protocol_backend_compression_threshold=5;
select 1 / i FROM generate_series(2, 0, -1) AS a(i);

RESET debug_parallel_query;

-- CompressedMessages should be sent if compress_number_messages threshold is
-- reached
SET protocol_backend_compression_threshold=0;
SET protocol_backend_compression_number_messages=5;
SELECT * FROM generate_series(1, 10);

-- Test mixing uncompressed and compressed messages
SET protocol_backend_compression_number_messages=2;
SET protocol_backend_compression_threshold=90;
\startpipeline
SELECT * FROM generate_series(1, 3);
SELECT repeat('a', 40) FROM generate_series(1, 3);
\endpipeline

RESET protocol_backend_compression_threshold;
RESET protocol_backend_compression_number_messages;
RESET protocol_backend_compression;
