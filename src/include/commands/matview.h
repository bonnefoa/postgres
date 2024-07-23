/*-------------------------------------------------------------------------
 *
 * matview.h
 *	  prototypes for matview.c.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/matview.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MATVIEW_H
#define MATVIEW_H

#include "catalog/objectaddress.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"
#include "utils/relcache.h"


extern void SetMatViewPopulatedState(Relation relation, bool newstate);

extern ObjectAddress ExecRefreshMatView(RefreshMatViewStmt *stmt, ParseState *pstate,
										QueryCompletion *qc);
extern ObjectAddress RefreshMatViewByOid(Oid matviewOid, bool is_create, bool skipData,
										 bool concurrent, ParseState *pstate,
										 QueryCompletion *qc);

extern DestReceiver *CreateTransientRelDestReceiver(Oid transientoid);

extern bool MatViewIncrementalMaintenanceIsEnabled(void);

#endif							/* MATVIEW_H */
