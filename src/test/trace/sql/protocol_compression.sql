-- Test compression algorithm switch behavior (enable, disable, switch...)

SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd,lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 and zstd (not supported) ***'
   \quit
\endif

SET protocol_backend_compression='zstd';

SET protocol_backend_compression_threshold=100;

-- A flush request should force the uncompressed messages to be sent
-- immediately
\startpipeline
SELECT 1 \parse test
\flushrequest
\flush
\getresults
SELECT repeat('a', 100);
\endpipeline

-- With an ongoing frame, toggling compression to none should flush
-- current buffer
\startpipeline
SELECT 1 \parse one
SET protocol_backend_compression='none';
\endpipeline

-- Re-enable compression and compress everything
SET protocol_backend_compression_threshold=0;
SET protocol_backend_compression='zstd';
-- Check simple query
SELECT 1;
-- Check extended query
SELECT $1::text, '42', $1::numeric, interval '1 sec' \bind 1 \g
-- Check copy
\copy (SELECT *, repeat(' ', 80) FROM generate_series(1, 20)) to /dev/null;

-- Same tests with lz4
SET protocol_backend_compression='lz4';
-- Check simple query
SELECT 1;
-- Check extended query
SELECT $1::text, '42', $1::numeric, interval '1 sec' \bind 1 \g
-- Check copy
\copy (SELECT *, repeat(' ', 80) FROM generate_series(1, 20)) to /dev/null;

-- Test compression algorithm change with an opened frame
SET protocol_backend_compression='zstd';
SELECT 1;
SET protocol_backend_compression='lz4';
SELECT 1;
SET protocol_backend_compression='zstd';

-- Test zstd -> none -> zstd with an opened frame
SET protocol_backend_compression='none';
SELECT 1;

-- CompressedMessages should be sent if compress_number_messages threshold is
-- reached
SET protocol_backend_compression_number_messages=5;
SET protocol_backend_compression='zstd';
SELECT * FROM generate_series(1, 10);
SET protocol_backend_compression='lz4';
SELECT * FROM generate_series(1, 10);

RESET protocol_backend_compression_number_messages;
RESET protocol_backend_compression;
