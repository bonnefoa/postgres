SET protocol_backend_compression_allowed_algorithms = 'zstd';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 and zstd (not supported) ***'
   \quit
\endif
SHOW protocol_backend_compression_allowed_algorithms;

-- Directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR

-----------
-- Test large object with compression
-----------
SET protocol_backend_compression_allowed_algorithms = 'zstd';
SET protocol_backend_compression_threshold=0;
SET protocol_backend_compression='zstd';

\set filename :abs_srcdir '/data/tenk.data'

\lo_import :filename

\set newloid :LASTOID

-- just make sure \lo_export does not barf
\set filename :abs_builddir '/results/lotest_compression.txt'
\lo_export :newloid :filename
\lo_unlink :newloid
