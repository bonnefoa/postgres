/*--------------------------------------------------------------------------
 *
 * test_notify_rollback.c
 *		Code for testing mandatory access control (MAC) using object access hooks.
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_notify_rollback/test_notify_rollback.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/async.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

/* Saved hook values */
static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;

/* Notify rollback Hook */
static void REGRESS_utility_command(PlannedStmt *pstmt,
									const char *queryString, bool readOnlyTree,
									ProcessUtilityContext context,
									ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* ProcessUtility hook */
	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = REGRESS_utility_command;
}

static void
REGRESS_utility_command(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	/* Forward to next hook in the chain */
	if (next_ProcessUtility_hook)
		(*next_ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
	Async_Notify("test", NULL);
}
