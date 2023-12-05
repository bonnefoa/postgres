LOAD 'test_notify_rollback';

-- Start a transaction block
BEGIN;
-- Abort the transaction with a syntax error
    g;
-- Rollback the transaction, test_notify_rollback's hook will
-- queue a notification while we're in a failed transaction state
ROLLBACK;

-- Try to use notify after the rollback, making sure we don't
-- segfault trying to use memory that was reset
NOTIFY test;
