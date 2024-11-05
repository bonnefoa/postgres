--
-- Tests using psql pipelining
--

CREATE TABLE psql_pipeline(a INTEGER PRIMARY KEY, s TEXT);

-- single query
\startpipeline
SELECT $1 \bind 'val1' \g
\endpipeline

-- multiple queries
\startpipeline
SELECT $1 \bind 'val1' \g
SELECT $1, $2 \bind 'val2' 'val3' \g
SELECT $1, $2 \bind 'val2' 'val3' \g
\endpipeline

-- Test \flush
\startpipeline
\flush
SELECT $1 \bind 'val1' \g
\flush
SELECT $1, $2 \bind 'val2' 'val3' \g
SELECT $1, $2 \bind 'val2' 'val3' \g
\endpipeline

-- send multiple syncs
\startpipeline
SELECT $1 \bind 'val1' \g
\syncpipeline
\syncpipeline
SELECT $1, $2 \bind 'val2' 'val3' \g
\syncpipeline
SELECT $1, $2 \bind 'val4' 'val5' \g
\endpipeline

-- startpipeline shouldn't have any effect if already in a pipeline
\startpipeline
\startpipeline
SELECT $1 \bind 'val1' \g
\endpipeline

-- Convert an implicit tx block to an explicit tx block
\startpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \g
BEGIN \bind \g
INSERT INTO psql_pipeline VALUES ($1) \bind 2 \g
ROLLBACK \bind \g
\endpipeline

-- Multiple explicit transactions
\startpipeline
BEGIN \bind \g
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \g
ROLLBACK \bind \g
BEGIN \bind \g
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \g
COMMIT \bind \g
\endpipeline

-- COPY FROM STDIN
\startpipeline
SELECT $1 \bind 'val1' \g
COPY psql_pipeline FROM STDIN \bind \g
\endpipeline
2	test2
\.

-- COPY FROM STDIN with \flushrequest + \getresults
\startpipeline
SELECT $1 \bind 'val1' \g
COPY psql_pipeline FROM STDIN \bind \g
\flushrequest
\getresults
3	test3
\.
\endpipeline

-- COPY FROM STDIN with \syncpipeline + \getresults
\startpipeline
SELECT $1 \bind 'val1' \g
COPY psql_pipeline FROM STDIN \bind \g
\syncpipeline
\getresults
4	test4
\.
\endpipeline

-- COPY TO STDOUT
\startpipeline
SELECT $1 \bind 'val1' \g
copy psql_pipeline TO STDOUT \bind \g
\endpipeline

-- COPY TO STDOUT with \flushrequest + \getresults
\startpipeline
SELECT $1 \bind 'val1' \g
copy psql_pipeline TO STDOUT \bind \g
\flushrequest
\getresults
\endpipeline

-- COPY TO STDOUT with \syncpipeline + \getresults
\startpipeline
SELECT $1 \bind 'val1' \g
copy psql_pipeline TO STDOUT \bind \g
\syncpipeline
\getresults
\endpipeline

-- Use \parse and \bind_named
\startpipeline
SELECT $1 \parse ''
SELECT $1, $2 \parse ''
SELECT $2 \parse pipeline_1
\bind_named '' 1 2 \g
\bind_named pipeline_1 2 \g
\endpipeline

-- \getresults displays all results preceding a \flushrequest
\startpipeline
SELECT $1 \bind 1 \g
SELECT $1 \bind 2 \g
\flushrequest
\getresults
\endpipeline

-- \getresults displays all results preceding a sync
\startpipeline
SELECT $1 \bind 1 \g
SELECT $1 \bind 2 \g
\syncpipeline
\getresults
\endpipeline

-- \getresults immediately returns if there's no result to fetch
\startpipeline
\getresults
SELECT $1 \bind 2 \g
\getresults
\flushrequest
\endpipeline
\getresults

-- \getresults only fetch results preceding a flushrequest
\startpipeline
SELECT $1 \bind 2 \g
\flushrequest
SELECT $1 \bind 2 \g
\getresults
\endpipeline

-- \getresults only fetch results preceding a sync message
\startpipeline
SELECT $1 \bind 2 \g
\syncpipeline
SELECT $1 \bind 2 \g
\getresults
\endpipeline

-- use pipeline with chunked results with both \getresults and \endpipeline
\startpipeline
\set FETCH_COUNT 10
SELECT $1 \bind 2 \g
\flushrequest
\getresults
SELECT $1 \bind 2 \g
\endpipeline
\unset FETCH_COUNT

-- \getresults with specific number of requested results
\startpipeline
SELECT $1 \bind 1 \g
SELECT $1 \bind 2 \g
SELECT $1 \bind 3 \g
\syncpipeline
\getresults 1
SELECT $1 \bind 4 \g
\getresults 3
\endpipeline

-- \syncpipeline count as one command to fetch for \getresults
\startpipeline
\syncpipeline
\syncpipeline
SELECT $1 \bind 1 \g
\flushrequest
\getresults 2
\getresults 1
\endpipeline

-- \getresults 0 should get all results
\startpipeline
SELECT $1 \bind 1 \g
SELECT $1 \bind 2 \g
SELECT $1 \bind 3 \g
\syncpipeline
\getresults 0
\endpipeline

-- pipelining errors

-- endpipeline outside of pipeline should fail
\endpipeline

-- Query using simple protocol should not be sent and should leave the pipeline usable
\startpipeline
SELECT 1;
SELECT $1 \bind 'val1' \g
\endpipeline

-- After an aborted pipeline, commands after a sync should be displayed
\startpipeline
SELECT $1 \bind \g
\syncpipeline
SELECT $1 \bind 1 \g
\endpipeline

-- Incorrect number of parameters, the pipeline will be aborted and following queries won't be executed
\startpipeline
SELECT \bind 'val1' \g
SELECT $1 \bind 'val1' \g
\endpipeline

-- An explicit transaction with an error needs to be rollbacked after the pipeline
\startpipeline
BEGIN \bind \g
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \g
ROLLBACK \bind \g
\endpipeline
ROLLBACK;

-- \watch sends a simple query which won't be allowed within a pipeline
\startpipeline
SELECT \bind \g
\watch 1
\endpipeline

-- \gdesc should fail as synchronous commands are not allowed in pipeline, pipeline should still be usable
\startpipeline
SELECT $1 \bind 1 \gdesc
SELECT $1 \bind 1 \g
\endpipeline

-- \gset is not allowed, pipeline should still be usable
\startpipeline
SELECT $1 as i, $2 as j \parse ''
SELECT $1 as k, $2 as l \parse 'second'
\bind_named '' 1 2 \gset
\bind_named second 1 2 \gset pref02_ \echo :pref02_i :pref02_j
\bind_named '' 1 2 \g
\endpipeline

-- \gx is not allowed, pipeline should still be usable
\startpipeline
SELECT $1 \bind 1 \gx
\reset
SELECT $1 \bind 1 \g
\endpipeline

-- \gx warning should be emitted in an aborted pipeline
\startpipeline
SELECT $1 \bind \g
\flushrequest
\getresults
SELECT $1 \bind 1 \gx
\endpipeline

-- \gexec is not allowed, pipeline should still be usable
\startpipeline
SELECT 'INSERT INTO psql_pipeline(a) SELECT generate_series(1, 10)' \parse 'insert_stmt'
\bind_named insert_stmt \gexec
\bind_named insert_stmt \g
SELECT COUNT(*) FROM psql_pipeline \bind \g
\endpipeline

-- After an error, pipeline is aborted and requires a sync to be reusable
\startpipeline
SELECT $1 \bind \g
SELECT $1 \bind 1 \g
SELECT $1 \parse a
\bind_named a 1 \g
\close a
\flushrequest
\getresults
-- Pipeline is aborted
SELECT $1 \bind 1 \g
SELECT $1 \parse a
\bind_named a 1 \g
\close a
-- Sync allows pipeline to recover
\syncpipeline
\getresults
SELECT $1 \bind 1 \g
SELECT $1 \parse a
\bind_named a 1 \g
\close a
\flushrequest
\getresults
\endpipeline

-- In an aborted pipeline, \getresults 1 aborted commands one at a time
\startpipeline
SELECT $1 \bind \g
SELECT $1 \bind 1 \g
SELECT $1 \parse a
\bind_named a 1 \g
\syncpipeline
\getresults 1
\getresults 1
\getresults 1
\getresults 1
\getresults 1
\endpipeline

-- Test chunked results with an aborted pipeline
\startpipeline
\set FETCH_COUNT 10
SELECT $1 \bind \g
\flushrequest
\getresults
SELECT $1 \bind \g
\endpipeline
\unset FETCH_COUNT

-- \getresults returns an error when an incorrect number is provided
\startpipeline
\getresults -1
\endpipeline

-- \getresults when there's no result shouldn't impact the following query
\getresults 1
select 1;

-- pipelining and transaction block behaviour

-- set local will issue a warning when modifying a GUC outside of a transaction block
-- The change will still be valid as a pipeline runs within an implicit transaction block
-- Sending a sync will commit the implicit transaction block. The first command after a sync
-- won't be seen as belonging to a pipeline.
\startpipeline
SET LOCAL statement_timeout='1h' \bind \g
SHOW statement_timeout \bind \g
\syncpipeline
SHOW statement_timeout \bind \g
SET LOCAL statement_timeout='2h' \bind \g
SHOW statement_timeout \bind \g
\endpipeline

-- Reindex concurrently is forbidden in the middle of a pipeline
\startpipeline
SELECT $1 \bind 1 \g
REINDEX TABLE CONCURRENTLY psql_pipeline \bind \g
SELECT $1 \bind 2 \g
\endpipeline

-- Reindex concurrently will work if it's the first command of a pipeline
\startpipeline
REINDEX TABLE CONCURRENTLY psql_pipeline \bind \g
SELECT $1 \bind 2 \g
\endpipeline

-- subtransactions are not allowed in pipeline mode
\startpipeline
SAVEPOINT a \bind \g
SELECT $1 \bind 1 \g
ROLLBACK TO SAVEPOINT a \bind \g
SELECT $1 \bind 2 \g
\endpipeline

-- Lock command will fail as first pipeline command is not seen as a transaction block
\startpipeline
LOCK psql_pipeline \bind \g
SELECT $1 \bind 2 \g
\endpipeline

-- Lock command will succeed after the first command as pipeline will be seen as an implicit transaction block
\startpipeline
SELECT $1 \bind 1 \g
LOCK psql_pipeline \bind \g
SELECT $1 \bind 2 \g
\endpipeline

-- Vacuum command will work as the first command
\startpipeline
VACUUM psql_pipeline \bind \g
\endpipeline

-- Vacuum command will fail within pipeline implicit transaction
\startpipeline
SELECT 1 \bind \g
VACUUM psql_pipeline \bind \g
\endpipeline

-- Vacuum command will work after a sync
\startpipeline
SELECT 1 \bind \g
\syncpipeline
VACUUM psql_pipeline \bind \g
\endpipeline

-- Error messages are currently accumulating and will be repeated
\startpipeline
SELECT 1 \bind \g
SELECT 1;
SELECT 1;
\endpipeline
