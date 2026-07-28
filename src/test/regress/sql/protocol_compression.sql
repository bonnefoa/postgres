-- Tests for the protocol_backend_compression GUC.

-- Allow all available compression algorithms
SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd,lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 and zstd (not supported) ***'
   \quit
\endif
SHOW protocol_backend_compression_allowed_algorithms;

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR

-- Force threshold to 0 to compress every message
SET protocol_backend_compression_threshold=0;

-- Disable compression without active compression
SET protocol_backend_compression = 'none';

-- Setting a supported algorithm, with and without level
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'zstd:5';
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'lz4:5';
SHOW protocol_backend_compression;

-- disabling compression
SET protocol_backend_compression = 'none';

-- unsupported compression
SET protocol_backend_compression = 'gzip';

-- unrecognized value
SET protocol_backend_compression = 'bogus';

-- test compressor context teardown/setup
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'lz4';

-- test reactivating compressor context
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';

SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'zstd';

-- switching away from "none" straight into an unsupported/invalid value
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'gzip';
SET protocol_backend_compression = 'bogus';

-- Tests for the protocol_backend_compression_allowed_algorithms GUC.

-- With only lz4 allowed, switching to zstd should fail
SET protocol_backend_compression_allowed_algorithms = 'lz4';
SHOW protocol_backend_compression_allowed_algorithms;
SET protocol_backend_compression = 'zstd';

-- Unsupported and unrecognized algorithms are rejected, without changing
-- the effective setting.
SET protocol_backend_compression_allowed_algorithms = 'gzip';
SET protocol_backend_compression_allowed_algorithms = 'bogus';
SHOW protocol_backend_compression_allowed_algorithms;

-- Disabling compression is always allowed
SET protocol_backend_compression_allowed_algorithms = 'none';
SET protocol_backend_compression_allowed_algorithms = '';
SHOW protocol_backend_compression_allowed_algorithms;
SET protocol_backend_compression = 'none';

-- With every algorithm forbidden, enabling compression should fail
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'lz4';

-- Test lo with compression
SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SET protocol_backend_compression_threshold=0;
SET protocol_backend_compression='zstd';

\set filename :abs_srcdir '/data/tenk.data'

\lo_import :filename

\set newloid :LASTOID

-- just make sure \lo_export does not barf
\set filename :abs_builddir '/results/lotest_compression.txt'
\lo_export :newloid :filename
\lo_unlink :newloid
