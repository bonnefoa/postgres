--
-- Statement level tracking
--

SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DO block - top-level tracking.
CREATE TABLE stats_track_tab (x int);
SET pg_stat_statements.track = 'top';
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END;
$$ LANGUAGE plpgsql;
SELECT toplevel, calls, query FROM pg_stat_statements
  WHERE query LIKE '%DELETE%' ORDER BY query COLLATE "C", toplevel;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DO block - all-level tracking.
SET pg_stat_statements.track = 'all';
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Explain - all-level tracking.
SET pg_stat_statements.track = 'all';
explain (costs off) SELECT 1;
explain (costs off) UPDATE stats_track_tab SET x=1 WHERE x=1;
explain (costs off) DELETE FROM stats_track_tab;
explain (costs off) INSERT INTO stats_track_tab VALUES ((1));
explain (costs off) MERGE INTO stats_track_tab USING (SELECT id FROM generate_series(1, 10) id) ON x = id
    WHEN MATCHED THEN UPDATE SET x = id
    WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
explain (costs off) SELECT 1 UNION SELECT 2;

-- Check we correctly capture substring with CTE
explain (costs off) WITH a AS (select 4) SELECT 1;
explain (costs off) WITH a AS (select 4) UPDATE stats_track_tab SET x=1 WHERE x=1;
explain (costs off) WITH a AS (select 4) DELETE FROM stats_track_tab;
explain (costs off) WITH a AS (select 4) INSERT INTO stats_track_tab VALUES ((1));
explain (costs off) WITH a AS (select 4) MERGE INTO stats_track_tab USING (SELECT id FROM generate_series(1, 10) id) ON x = id
    WHEN MATCHED THEN UPDATE SET x = id
    WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
explain (costs off) WITH a AS (select 4) SELECT 1 UNION SELECT 2;

SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Procedure with multiple utility statements.
CREATE OR REPLACE PROCEDURE proc_with_utility_stmt()
LANGUAGE SQL
AS $$
  SHOW pg_stat_statements.track;
  show pg_stat_statements.track;
  SHOW pg_stat_statements.track_utility;
$$;
SET pg_stat_statements.track_utility = TRUE;
-- all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL proc_with_utility_stmt();
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;
-- top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL proc_with_utility_stmt();
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Create Table As, all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TEMPORARY TABLE pgss_test AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Create Table As using prepared stmt, all-level tracking.
PREPARE test_prepare_pgss AS select generate_series(1, 10);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TEMPORARY TABLE pgss_test2 AS EXECUTE test_prepare_pgss;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Declare cursor, all-level tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
BEGIN;
DECLARE FOOCUR CURSOR FOR SELECT * from stats_track_tab;
FETCH FORWARD 1 FROM foocur;
CLOSE foocur;
COMMIT;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain analyze, all-level tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF) SELECT 100;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain analyze with declare cursor, all-level tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF) DECLARE foocur CURSOR FOR SELECT * FROM stats_track_tab;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain with ctas, all-level tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF, SUMMARY OFF, TIMING OFF) CREATE TABLE pgss_test_3 AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Create Table As, top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TABLE pgss_test_4 AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Create Table As using prepared stmt, top tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TEMPORARY TABLE pgss_test5 AS EXECUTE test_prepare_pgss;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Declare cursor, top tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
BEGIN;
DECLARE FOOCUR CURSOR FOR SELECT * from stats_track_tab;
FETCH FORWARD 1 FROM foocur;
CLOSE foocur;
COMMIT;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain analyze, top tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF) SELECT 100;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain analyze with declare cursor, top tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF) DECLARE foocur CURSOR FOR SELECT * FROM stats_track_tab;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Explain with ctas, top-level tracking.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF, SUMMARY OFF, TIMING OFF) CREATE TABLE pgss_test_3 AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- DO block - top-level tracking without utility.
SET pg_stat_statements.track = 'top';
SET pg_stat_statements.track_utility = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- DO block - all-level tracking without utility.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- PL/pgSQL function - top-level tracking.
SET pg_stat_statements.track = 'top';
SET pg_stat_statements.track_utility = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(3);
SELECT PLUS_TWO(7);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(8);
SELECT PLUS_ONE(10);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- immutable SQL function --- can be executed at plan time
CREATE FUNCTION PLUS_THREE(i INTEGER) RETURNS INTEGER AS
$$ SELECT i + 3 LIMIT 1 $$ IMMUTABLE LANGUAGE SQL;

SELECT PLUS_THREE(8);
SELECT PLUS_THREE(10);

SELECT toplevel, calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- PL/pgSQL function - all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- we drop and recreate the functions to avoid any caching funnies
DROP FUNCTION PLUS_ONE(INTEGER);
DROP FUNCTION PLUS_TWO(INTEGER);
DROP FUNCTION PLUS_THREE(INTEGER);

-- PL/pgSQL function
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(-1);
SELECT PLUS_TWO(2);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(3);
SELECT PLUS_ONE(1);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- immutable SQL function --- can be executed at plan time
CREATE FUNCTION PLUS_THREE(i INTEGER) RETURNS INTEGER AS
$$ SELECT i + 3 LIMIT 1 $$ IMMUTABLE LANGUAGE SQL;

SELECT PLUS_THREE(8);
SELECT PLUS_THREE(10);

SELECT toplevel, calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- pg_stat_statements.track = none
--
SET pg_stat_statements.track = 'none';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT 1 AS "one";
SELECT 1 + 1 AS "two";

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
