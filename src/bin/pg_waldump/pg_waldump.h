#ifndef WALDUMP_H
#define WALDUMP_H

#define FRONTEND 1
#include "postgres.h"

#include "access/transam.h"
#include "common/logging.h"
#include "common/hashfn.h"
#include "common/fe_memutils.h"
#include "storage/relfilenode.h"
#include "rmgrdesc.h"

typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
} XLogDumpPrivate;

typedef struct XLogDumpConfig
{
	/* display options */
	bool		quiet;
	bool		bkp_details;
	int			stop_after_records;
	int			already_displayed_records;
	bool		follow;
	bool		stats;
	bool		stats_per_record;
	bool		stats_per_rel;
	int			limit_relations;
	bool		rel_details;

	/* filter options */
	int			filter_by_rmgr;
	TransactionId filter_by_xid;
	RelFileNode filter_by_relation;

	bool		filter_by_xid_enabled;
	bool		filter_only_aborted_xact;

	bool		filter_by_extended;
	bool		filter_by_relation_enabled;

} XLogDumpConfig;

typedef struct Stats
{
	uint64		count;
	uint64		rec_len;
	uint64		fpi_len;
} Stats;

typedef struct _relStatsEntry
{
	RelFileNode relFileNode;
	uint32		status;			/* hash status */
	Stats		stats;
}			RelStatsEntry;

#define SH_PREFIX		relStats
#define SH_ELEMENT_TYPE	RelStatsEntry
#define SH_KEY_TYPE		RelFileNode
#define	SH_KEY			relFileNode
#define SH_HASH_KEY(tb, key)	hash_bytes((const unsigned char *) &(key), sizeof(RelFileNode))
#define SH_EQUAL(tb, a, b)		RelFileNodeEquals(a, b)
#define	SH_SCOPE		static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

typedef struct _relMappingEntry
{
	Oid			relfilenode;
	char	   *relname;
	char	   *toast_parent;
	char	   *toast_index_parent;
	uint32		status;			/* hash status */
}			RelMappingEntry;

#define SH_PREFIX		relMapping
#define SH_ELEMENT_TYPE	RelMappingEntry
#define SH_KEY_TYPE		Oid
#define	SH_KEY			relfilenode
#define SH_HASH_KEY(tb, key)	hash_bytes((const unsigned char *) &(key), sizeof(Oid))
#define SH_EQUAL(tb, a, b)		(a == b)
#define	SH_SCOPE		static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

typedef struct _xactState
{
	TransactionId xid;
	bool		aborted;
	uint32		status;			/* hash status */
}			XactState;

#define SH_PREFIX		xactState
#define SH_ELEMENT_TYPE	XactState
#define SH_KEY_TYPE		TransactionId
#define	SH_KEY			xid
#define SH_HASH_KEY(tb, key)	hash_bytes((const unsigned char *) &(key), sizeof(TransactionId))
#define SH_EQUAL(tb, a, b)		(a == b)
#define	SH_SCOPE		static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

#define MAX_XLINFO_TYPES 16

typedef struct XLogDumpStats
{
	uint64		count;
	Stats		rmgr_stats[RM_NEXT_ID];
	Stats		record_stats[RM_NEXT_ID][MAX_XLINFO_TYPES];
	relStats_hash *rel_stats[RM_NEXT_ID][MAX_XLINFO_TYPES];
} XLogDumpStats;

typedef struct ResourceStats
{
	int			rmgr_id;
	int			rmgr_op;
	Stats		stats;
}			ResourceStats;

extern relMapping_hash * relmapping_hash;

extern void
XLogDumpDisplayStats(XLogDumpConfig *config, XLogDumpStats *stats);

#endif
