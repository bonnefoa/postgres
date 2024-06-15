--
-- Information related to commit
--

-- These tests require track_commit to be enabled.
SET pg_stat_statements.track_commit = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- commit counting
--
CREATE TABLE pgss_commit_test (a int, b text);
INSERT INTO pgss_commit_test (a, b) SELECT generate_series(1, 1000), 'something';
SELECT commits, calls, rows, query FROM pg_stat_statements;

-- Cleanup
DROP TABLE pgss_commit_test;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
