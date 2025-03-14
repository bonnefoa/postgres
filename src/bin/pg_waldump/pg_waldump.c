/*-------------------------------------------------------------------------
 *
 * pg_waldump.c - decode and display WAL
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/fe_memutils.h"
#include "common/hashfn.h"
#include "common/logging.h"
#include "common/string.h"
#include "getopt_long.h"
#include "rmgrdesc.h"

static const char *progname;

static int	WalSegSz;

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

#define MAX_XLINFO_TYPES 16

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

#define RELSTATSHASH_INITIAL_SIZE	20

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

typedef struct _relMappingEntry
{
	Oid			relfilenode;
	Oid			oid;
	char	   *relname;
	char	   *parent_relname;
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

#define RELMAPPINGHASH_INITIAL_SIZE	20
static relMapping_hash * relmapping_hash = NULL;

#define XACTSTATE_INITIAL_SIZE	10000
static xactState_hash * xactstate_hash = NULL;

#define fatal_error(...) do { pg_log_fatal(__VA_ARGS__); exit(EXIT_FAILURE); } while(0)

static void
print_rmgr_list(void)
{
	int			i;

	for (i = 0; i <= RM_MAX_ID; i++)
	{
		printf("%s\n", RmgrDescTable[i].rm_name);
	}
}

/*
 * Check whether directory exists and whether we can open it. Keep errno set so
 * that the caller can report errors somewhat more accurately.
 */
static bool
verify_directory(const char *directory)
{
	DIR		   *dir = opendir(directory);

	if (dir == NULL)
		return false;
	closedir(dir);
	return true;
}

/*
 * Split a pathname as dirname(1) and basename(1) would.
 *
 * XXX this probably doesn't do very well on Windows.  We probably need to
 * apply canonicalize_path(), at the very least.
 */
static void
split_path(const char *path, char **dir, char **fname)
{
	char	   *sep;

	/* split filepath into directory & filename */
	sep = strrchr(path, '/');

	/* directory path */
	if (sep != NULL)
	{
		*dir = pnstrdup(path, sep - path);
		*fname = pg_strdup(sep + 1);
	}
	/* local directory */
	else
	{
		*dir = NULL;
		*fname = pg_strdup(path);
	}
}

/*
 * Open the file in the valid target directory.
 *
 * return a read only fd
 */
static int
open_file_in_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];

	Assert(directory != NULL);

	snprintf(fpath, MAXPGPATH, "%s/%s", directory, fname);
	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0 && errno != ENOENT)
		fatal_error("could not open file \"%s\": %m", fname);
	return fd;
}

/*
 * Try to find fname in the given directory. Returns true if it is found,
 * false otherwise. If fname is NULL, search the complete directory for any
 * file with a valid WAL file name. If file is successfully opened, set the
 * wal segment size.
 */
static bool
search_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	DIR		   *xldir;

	/* open file if valid filename is provided */
	if (fname != NULL)
		fd = open_file_in_directory(directory, fname);

	/*
	 * A valid file name is not passed, so search the complete directory.  If
	 * we find any file whose name is a valid WAL file name then try to open
	 * it.  If we cannot open it, bail out.
	 */
	else if ((xldir = opendir(directory)) != NULL)
	{
		struct dirent *xlde;

		while ((xlde = readdir(xldir)) != NULL)
		{
			if (IsXLogFileName(xlde->d_name))
			{
				fd = open_file_in_directory(directory, xlde->d_name);
				fname = pg_strdup(xlde->d_name);
				break;
			}
		}

		closedir(xldir);
	}

	/* set WalSegSz if file is successfully opened */
	if (fd >= 0)
	{
		PGAlignedXLogBlock buf;
		int			r;

		r = read(fd, buf.data, XLOG_BLCKSZ);
		if (r == XLOG_BLCKSZ)
		{
			XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

			WalSegSz = longhdr->xlp_seg_size;

			if (!IsValidWalSegSize(WalSegSz))
				fatal_error(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d byte",
									 "WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d bytes",
									 WalSegSz),
							fname, WalSegSz);
		}
		else if (r < 0)
			fatal_error("could not read file \"%s\": %m",
						fname);
		else
			fatal_error("could not read file \"%s\": read %d of %zu",
						fname, r, (Size) XLOG_BLCKSZ);
		close(fd);
		return true;
	}

	return false;
}

/*
 * Identify the target directory.
 *
 * Try to find the file in several places:
 * if directory != NULL:
 *	 directory /
 *	 directory / XLOGDIR /
 * else
 *	 .
 *	 XLOGDIR /
 *	 $PGDATA / XLOGDIR /
 *
 * The valid target directory is returned.
 */
static char *
identify_target_directory(char *directory, char *fname)
{
	char		fpath[MAXPGPATH];

	if (directory != NULL)
	{
		if (search_directory(directory, fname))
			return pg_strdup(directory);

		/* directory / XLOGDIR */
		snprintf(fpath, MAXPGPATH, "%s/%s", directory, XLOGDIR);
		if (search_directory(fpath, fname))
			return pg_strdup(fpath);
	}
	else
	{
		const char *datadir;

		/* current directory */
		if (search_directory(".", fname))
			return pg_strdup(".");
		/* XLOGDIR */
		if (search_directory(XLOGDIR, fname))
			return pg_strdup(XLOGDIR);

		datadir = getenv("PGDATA");
		/* $PGDATA / XLOGDIR */
		if (datadir != NULL)
		{
			snprintf(fpath, MAXPGPATH, "%s/%s", datadir, XLOGDIR);
			if (search_directory(fpath, fname))
				return pg_strdup(fpath);
		}
	}

	/* could not locate WAL file */
	if (fname)
		fatal_error("could not locate WAL file \"%s\"", fname);
	else
		fatal_error("could not find any WAL file");

	return NULL;				/* not reached */
}

/* pg_waldump's XLogReaderRoutine->segment_open callback */
static void
WALDumpOpenSegment(XLogReaderState *state, XLogSegNo nextSegNo,
				   TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		fname[MAXPGPATH];
	int			tries;

	XLogFileName(fname, tli, nextSegNo, state->segcxt.ws_segsize);

	/*
	 * In follow mode there is a short period of time after the server has
	 * written the end of the previous file before the new file is available.
	 * So we loop for 5 seconds looking for the file to appear before giving
	 * up.
	 */
	for (tries = 0; tries < 10; tries++)
	{
		state->seg.ws_file = open_file_in_directory(state->segcxt.ws_dir, fname);
		if (state->seg.ws_file >= 0)
			return;
		if (errno == ENOENT)
		{
			int			save_errno = errno;

			/* File not there yet, try again */
			pg_usleep(500 * 1000);

			errno = save_errno;
			continue;
		}
		/* Any other error, fall through and fail */
		break;
	}

	fatal_error("could not find file \"%s\": %m", fname);
}

/*
 * pg_waldump's XLogReaderRoutine->segment_close callback.  Same as
 * wal_segment_close
 */
static void
WALDumpCloseSegment(XLogReaderState *state)
{
	close(state->seg.ws_file);
	/* need to check errno? */
	state->seg.ws_file = -1;
}

/* pg_waldump's XLogReaderRoutine->page_read callback */
static int
WALDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				XLogRecPtr targetPtr, char *readBuff)
{
	XLogDumpPrivate *private = state->private_data;
	int			count = XLOG_BLCKSZ;
	WALReadError errinfo;

	if (private->endptr != InvalidXLogRecPtr)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private->endptr)
			count = XLOG_BLCKSZ;
		else if (targetPagePtr + reqLen <= private->endptr)
			count = private->endptr - targetPagePtr;
		else
		{
			private->endptr_reached = true;
			return -1;
		}
	}

	if (!WALRead(state, readBuff, targetPagePtr, count, private->timeline,
				 &errinfo))
	{
		WALOpenSegment *seg = &errinfo.wre_seg;
		char		fname[MAXPGPATH];

		XLogFileName(fname, seg->ws_tli, seg->ws_segno,
					 state->segcxt.ws_segsize);

		if (errinfo.wre_errno != 0)
		{
			errno = errinfo.wre_errno;
			fatal_error("could not read from file %s, offset %u: %m",
						fname, errinfo.wre_off);
		}
		else
			fatal_error("could not read from file %s, offset %u: read %d of %zu",
						fname, errinfo.wre_off, errinfo.wre_read,
						(Size) errinfo.wre_req);
	}

	return count;
}

/*
 * Boolean to return whether the given WAL record matches a specific relation
 * and optionally block.
 */
static bool
XLogRecordMatchesRelationBlock(XLogReaderState *record,
							   RelFileNode matchRlocator)
{
	int			block_id;

	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		RelFileNode rlocator;

		if (!XLogRecGetBlockTag(record, block_id, &rlocator, NULL, NULL))
			continue;

		if (RelFileNodeEquals(matchRlocator, rlocator))
			return true;
	}

	return false;
}

/*
 * Calculate the size of a record, split into !FPI and FPI parts.
 */
static void
XLogDumpRecordLen(XLogReaderState *record, uint32 *rec_len, uint32 *fpi_len)
{
	int			block_id;

	/*
	 * Calculate the amount of FPI data in the record.
	 *
	 * XXX: We peek into xlogreader's private decoded backup blocks for the
	 * bimg_len indicating the length of FPI data. It doesn't seem worth it to
	 * add an accessor macro for this.
	 */
	*fpi_len = 0;
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (XLogRecHasBlockImage(record, block_id))
			*fpi_len += record->blocks[block_id].bimg_len;
	}

	/*
	 * Calculate the length of the record as the total length - the length of
	 * all the block images.
	 */
	*rec_len = XLogRecGetTotalLen(record) - *fpi_len;
}

/*
 * Store per-rmgr and per-record statistics for a given record.
 */
static void
XLogDumpCountRecord(XLogDumpConfig *config, XLogDumpStats *stats,
					XLogReaderState *record)
{
	RmgrId		rmid;
	uint8		recid;
	uint32		rec_len;
	uint32		fpi_len;
	int			block_id;
	RelFileNode rnode;
	relStats_hash *hash;

	stats->count++;

	rmid = XLogRecGetRmid(record);

	XLogDumpRecordLen(record, &rec_len, &fpi_len);

	/* Update per-rmgr statistics */

	stats->rmgr_stats[rmid].count++;
	stats->rmgr_stats[rmid].rec_len += rec_len;
	stats->rmgr_stats[rmid].fpi_len += fpi_len;

	/*
	 * Update per-record statistics, where the record is identified by a
	 * combination of the RmgrId and the four bits of the xl_info field that
	 * are the rmgr's domain (resulting in sixteen possible entries per
	 * RmgrId).
	 */

	recid = XLogRecGetInfo(record) >> 4;

	/*
	 * XACT records need to be handled differently. Those records use the
	 * first bit of those four bits for an optional flag variable and the
	 * following three bits for the opcode. We filter opcode out of xl_info
	 * and use it as the identifier of the record.
	 */
	if (rmid == RM_XACT_ID)
		recid &= 0x07;

	stats->record_stats[rmid][recid].count++;
	stats->record_stats[rmid][recid].rec_len += rec_len;
	stats->record_stats[rmid][recid].fpi_len += fpi_len;

	hash = stats->rel_stats[rmid][recid];
	if (hash == NULL)
	{
		hash = relStats_create(RELSTATSHASH_INITIAL_SIZE, NULL);
		stats->rel_stats[rmid][recid] = hash;
	}
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		RelStatsEntry *relStats_entry;
		bool		found;

		XLogRecGetBlockTag(record, block_id, &rnode, NULL, NULL);
		relStats_entry = relStats_insert(hash, rnode, &found);
		if (!found)
		{
			relStats_entry->stats.count = 0;
			relStats_entry->stats.rec_len = 0;
			relStats_entry->stats.fpi_len = 0;
		}
		relStats_entry->stats.count++;
		relStats_entry->stats.rec_len += rec_len;
		relStats_entry->stats.fpi_len += fpi_len;
	}
}

/*
 * Print a record to stdout
 */
static void
XLogDumpDisplayRecord(XLogDumpConfig *config, XLogReaderState *record)
{
	const char *id;
	const RmgrDescData *desc = &RmgrDescTable[XLogRecGetRmid(record)];
	uint32		rec_len;
	uint32		fpi_len;
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blk;
	int			block_id;
	uint8		info = XLogRecGetInfo(record);
	XLogRecPtr	xl_prev = XLogRecGetPrev(record);
	StringInfoData s;

	XLogDumpRecordLen(record, &rec_len, &fpi_len);

	printf("rmgr: %-11s len (rec/tot): %6u/%6u, tx: %10u, lsn: %X/%08X, prev %X/%08X, ",
		   desc->rm_name,
		   rec_len, XLogRecGetTotalLen(record),
		   XLogRecGetXid(record),
		   LSN_FORMAT_ARGS(record->ReadRecPtr),
		   LSN_FORMAT_ARGS(xl_prev));

	id = desc->rm_identify(info);
	if (id == NULL)
		printf("desc: UNKNOWN (%x) ", info & ~XLR_INFO_MASK);
	else
		printf("desc: %s ", id);

	initStringInfo(&s);
	desc->rm_desc(&s, record);
	printf("%s", s.data);
	pfree(s.data);

	if (!config->bkp_details)
	{
		/* print block references (short format) */
		for (block_id = 0; block_id <= record->max_block_id; block_id++)
		{
			if (!XLogRecHasBlockRef(record, block_id))
				continue;

			XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
			if (forknum != MAIN_FORKNUM)
				printf(", blkref #%u: rel %u/%u/%u fork %s blk %u",
					   block_id,
					   rnode.spcNode, rnode.dbNode, rnode.relNode,
					   forkNames[forknum],
					   blk);
			else
				printf(", blkref #%u: rel %u/%u/%u blk %u",
					   block_id,
					   rnode.spcNode, rnode.dbNode, rnode.relNode,
					   blk);
			if (XLogRecHasBlockImage(record, block_id))
			{
				if (XLogRecBlockImageApply(record, block_id))
					printf(" FPW");
				else
					printf(" FPW for WAL verification");
			}
		}
		putchar('\n');
	}
	else
	{
		/* print block references (detailed format) */
		putchar('\n');
		for (block_id = 0; block_id <= record->max_block_id; block_id++)
		{
			if (!XLogRecHasBlockRef(record, block_id))
				continue;

			XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
			printf("\tblkref #%u: rel %u/%u/%u fork %s blk %u",
				   block_id,
				   rnode.spcNode, rnode.dbNode, rnode.relNode,
				   forkNames[forknum],
				   blk);
			if (XLogRecHasBlockImage(record, block_id))
			{
				if (record->blocks[block_id].bimg_info &
					BKPIMAGE_IS_COMPRESSED)
				{
					printf(" (FPW%s); hole: offset: %u, length: %u, "
						   "compression saved: %u",
						   XLogRecBlockImageApply(record, block_id) ?
						   "" : " for WAL verification",
						   record->blocks[block_id].hole_offset,
						   record->blocks[block_id].hole_length,
						   BLCKSZ -
						   record->blocks[block_id].hole_length -
						   record->blocks[block_id].bimg_len);
				}
				else
				{
					printf(" (FPW%s); hole: offset: %u, length: %u",
						   XLogRecBlockImageApply(record, block_id) ?
						   "" : " for WAL verification",
						   record->blocks[block_id].hole_offset,
						   record->blocks[block_id].hole_length);
				}
			}
			putchar('\n');
		}
	}
}

/*
 * Display a single row of record counts and sizes for an rmgr or record.
 */
static void
XLogDumpStatsRow(const char *name,
				 uint64 n, uint64 total_count,
				 uint64 rec_len, uint64 total_rec_len,
				 uint64 fpi_len, uint64 total_fpi_len,
				 uint64 tot_len, uint64 total_len)
{
	double		n_pct,
				rec_len_pct,
				fpi_len_pct,
				tot_len_pct;

	n_pct = 0;
	if (total_count != 0)
		n_pct = 100 * (double) n / total_count;

	rec_len_pct = 0;
	if (total_rec_len != 0)
		rec_len_pct = 100 * (double) rec_len / total_rec_len;

	fpi_len_pct = 0;
	if (total_fpi_len != 0)
		fpi_len_pct = 100 * (double) fpi_len / total_fpi_len;

	tot_len_pct = 0;
	if (total_len != 0)
		tot_len_pct = 100 * (double) tot_len / total_len;

	printf("%-75s "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f)\n",
		   name, n, n_pct, rec_len, rec_len_pct, fpi_len, fpi_len_pct,
		   tot_len, tot_len_pct);
}

static char *
relnodeToRelname(XLogDumpConfig *config, RelFileNode relnode)
{
	RelMappingEntry *relmapping_entry;

	if (relmapping_hash == NULL)
		return psprintf("|%u/%u/%u", relnode.spcNode, relnode.dbNode,
						relnode.relNode);

	relmapping_entry = relMapping_lookup(relmapping_hash, relnode.relNode);
	if (relmapping_entry == NULL)
		return psprintf("|%u/%u/%u", relnode.spcNode, relnode.dbNode,
						relnode.relNode);

	if (relmapping_entry->parent_relname != NULL)
		return psprintf("|%s (toast) (%u/%u/%u)", relmapping_entry->parent_relname,
						relnode.spcNode, relnode.dbNode, relnode.relNode);
	else
		return psprintf("|%s (%u/%u/%u)", relmapping_entry->relname, relnode.spcNode,
						relnode.dbNode, relnode.relNode);
}

/*
 * qsort comparator for ExtensionMemberIds
 */
static int
RelStatsEntryCompare(const void *p1, const void *p2)
{
	const		RelStatsEntry *obj1 = *(const RelStatsEntry * *) p1;
	const		RelStatsEntry *obj2 = *(const RelStatsEntry * *) p2;

	if (obj1->stats.rec_len > obj2->stats.rec_len)
		return -1;
	if (obj1->stats.rec_len < obj2->stats.rec_len)
		return 1;
	return 0;
}

/*
 * qsort comparator for ResourceStats
 */
static int
ResourceStatsCompare(const void *p1, const void *p2)
{
	const		ResourceStats *obj1 = (const ResourceStats *) p1;
	const		ResourceStats *obj2 = (const ResourceStats *) p2;

	if (obj1->stats.rec_len > obj2->stats.rec_len)
		return -1;
	if (obj1->stats.rec_len < obj2->stats.rec_len)
		return 1;
	return 0;
}

static void
XLogDumpDisplayPerRelStats(XLogDumpConfig *config, relStats_hash * relStatsHash,
						   uint64 total_count, uint64 total_rec_len,
						   uint64 total_fpi_len, uint64 total_len)
{
	RelStatsEntry *relstats_entry;
	relStats_iterator iter;
	int			j = 0;
	int			n = 0;

	RelStatsEntry **relstatsArray = (RelStatsEntry * *) calloc(relStatsHash->members, sizeof(RelStatsEntry *));

	relStats_start_iterate(relStatsHash, &iter);
	while ((relstats_entry = relStats_iterate(relStatsHash, &iter)) != NULL)
	{
		relstatsArray[j++] = relstats_entry;
	}
	qsort((void *) relstatsArray, relStatsHash->members, sizeof(RelStatsEntry *),
		  RelStatsEntryCompare);

	for (int i = 0; i < relStatsHash->members; i++)
	{
		char	   *relname;
		RelStatsEntry *relstatsEntry = relstatsArray[i];
		Stats	   *relStats = &relstatsEntry->stats;

		if (config->limit_relations > 0 && n >= config->limit_relations)
			break;
		relname = relnodeToRelname(config, relstatsEntry->relFileNode);

		XLogDumpStatsRow(relname,
						 relStats->count, total_count, relStats->rec_len, total_rec_len,
						 relStats->fpi_len, total_fpi_len,
						 relStats->rec_len + relStats->fpi_len, total_len);
		n++;
	}
	free((void *) relstatsArray);
}

/*
 * Display summary statistics about the records seen so far.
 */
static void
XLogDumpDisplayStats(XLogDumpConfig *config, XLogDumpStats *stats)
{
	int			ri,
				rj,
				n = 0;
	uint64		total_count = 0;
	uint64		total_rec_len = 0;
	uint64		total_fpi_len = 0;
	uint64		total_len = 0;
	double		rec_len_pct,
				fpi_len_pct;
	ResourceStats *resourceStats;
	uint64		count,
				rec_len,
				fpi_len,
				tot_len;


	/*
	 * Each row shows its percentages of the total, so make a first pass to
	 * calculate column totals.
	 */

	for (ri = 0; ri < RM_NEXT_ID; ri++)
	{
		total_count += stats->rmgr_stats[ri].count;
		total_rec_len += stats->rmgr_stats[ri].rec_len;
		total_fpi_len += stats->rmgr_stats[ri].fpi_len;
	}
	total_len = total_rec_len + total_fpi_len;

	/*
	 * 27 is strlen("Transaction/COMMIT_PREPARED"), 20 is strlen(2^64), 8 is
	 * strlen("(100.00%)")
	 */

	printf("%-75s %20s %8s %20s %8s %20s %8s %20s %8s\n"
		   "%-75s %20s %8s %20s %8s %20s %8s %20s %8s\n",
		   "Type", "N", "(%)", "Record size", "(%)", "FPI size", "(%)", "Combined size", "(%)",
		   "----", "-", "---", "-----------", "---", "--------", "---", "-------------", "---");

	if (config->stats_per_record)
	{
		resourceStats = calloc(MAX_XLINFO_TYPES * RM_NEXT_ID, sizeof(ResourceStats));
		for (ri = 0; ri < RM_NEXT_ID; ri++)
		{
			for (rj = 0; rj < MAX_XLINFO_TYPES; rj++)
			{
				ResourceStats *rstat = &resourceStats[n++];

				rstat->rmgr_id = ri;
				rstat->rmgr_op = rj;
				rstat->stats = stats->record_stats[ri][rj];
			}
		}
		qsort((void *) resourceStats, MAX_XLINFO_TYPES * RM_NEXT_ID, sizeof(ResourceStats),
			  ResourceStatsCompare);

		for (int i = 0; i < MAX_XLINFO_TYPES * RM_NEXT_ID; i++)
		{
			ResourceStats *rstats = &resourceStats[i];
			const RmgrDescData *desc = &RmgrDescTable[rstats->rmgr_id];
			const char *id;
			relStats_hash *relStatsHash;

			ri = rstats->rmgr_id;
			rj = rstats->rmgr_op;

			count = stats->record_stats[ri][rj].count;
			rec_len = stats->record_stats[ri][rj].rec_len;
			fpi_len = stats->record_stats[ri][rj].fpi_len;
			tot_len = rec_len + fpi_len;

			/* Skip undefined combinations and ones that didn't occur */
			if (count == 0)
				continue;

			/* the upper four bits in xl_info are the rmgr's */
			id = desc->rm_identify(rj << 4);
			if (id == NULL)
				id = psprintf("UNKNOWN (%x)", rj << 4);

			XLogDumpStatsRow(psprintf("%s/%s", desc->rm_name, id),
							 count, total_count, rec_len, total_rec_len,
							 fpi_len, total_fpi_len, tot_len, total_len);

			relStatsHash = stats->rel_stats[ri][rj];
			if (relStatsHash != NULL)
				XLogDumpDisplayPerRelStats(config, relStatsHash, total_count,
										   total_rec_len, total_fpi_len, total_len);

			printf("\n");
		}
	}
	else
	{
		resourceStats = calloc(RM_NEXT_ID, sizeof(ResourceStats));
		for (ri = 0; ri < RM_NEXT_ID; ri++)
		{
			ResourceStats *rstat = &resourceStats[n++];

			rstat->rmgr_id = ri;
			rstat->stats = stats->rmgr_stats[ri];
		}
		qsort((void *) resourceStats, RM_NEXT_ID, sizeof(ResourceStats),
			  ResourceStatsCompare);

		for (int i = 0; i < RM_NEXT_ID; i++)
		{
			ResourceStats *rstats = &resourceStats[i];
			const RmgrDescData *desc = &RmgrDescTable[rstats->rmgr_id];

			count = stats->rmgr_stats[rstats->rmgr_id].count;
			rec_len = stats->rmgr_stats[rstats->rmgr_id].rec_len;
			fpi_len = stats->rmgr_stats[rstats->rmgr_id].fpi_len;
			tot_len = rec_len + fpi_len;

			if (count == 0)
				continue;

			XLogDumpStatsRow(desc->rm_name,
							 count, total_count, rec_len, total_rec_len,
							 fpi_len, total_fpi_len, tot_len, total_len);
		}
	}

	free(resourceStats);

	printf("%-75s %20s %8s %20s %8s %20s %8s %20s\n",
		   "", "--------", "", "--------", "", "--------", "", "--------");

	/*
	 * The percentages in earlier rows were calculated against the column
	 * total, but the ones that follow are against the row total. Note that
	 * these are displayed with a % symbol to differentiate them from the
	 * earlier ones, and are thus up to 9 characters long.
	 */

	rec_len_pct = 0;
	if (total_len != 0)
		rec_len_pct = 100 * (double) total_rec_len / total_len;

	fpi_len_pct = 0;
	if (total_len != 0)
		fpi_len_pct = 100 * (double) total_fpi_len / total_len;

	printf("%-75s "
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-6s\n",
		   "Total", stats->count, "",
		   total_rec_len, psprintf("[%.02f%%]", rec_len_pct),
		   total_fpi_len, psprintf("[%.02f%%]", fpi_len_pct),
		   total_len, "[100%]");
}

static bool
isXactAborted(TransactionId xid)
{
	XactState  *xact_state = xactState_lookup(xactstate_hash, xid);

	if (xact_state == NULL)
		return false;
	return xact_state->aborted;
}

static void
readRelMappingFile(char *relmapping_file)
{
	int			linenr = 0;
	char	   *line;
	char		buf[1024];

	FILE	   *f = fopen(relmapping_file, "r");

	if (f == NULL)
	{
		fatal_error("could not relmapping file \"%s\": %m", relmapping_file);
		return;
	}

	while ((line = fgets(buf, sizeof(buf), f)) != NULL)
	{
		RelMappingEntry *entry;
		Oid			relfilenode;
		Oid			oid;
		char	   *relname;
		char	   *parent_relname;
		int			len;
		bool		found;

		linenr++;

		/* skip header */
		if (linenr == 0)
			continue;

		if (strlen(line) >= sizeof(buf) - 1)
		{
			fatal_error("line %d too long in file \"%s\"", linenr, relmapping_file);
			goto exit;
		}

		/* ignore whitespace at end of line, especially the newline */
		len = strlen(line);
		while (len > 0 && isspace((unsigned char) line[len - 1]))
			line[--len] = '\0';

		/* ignore leading whitespace too */
		while (*line && isspace((unsigned char) line[0]))
			line++;

		/* relfilenode,relname,oid,parent relname */
		relfilenode = atoi(strtok(line, ","));
		relname = strtok(NULL, ",");
		oid = atoi(strtok(NULL, ","));
		parent_relname = strtok(NULL, ",");

		entry = relMapping_insert(relmapping_hash, relfilenode, &found);
		Assert(found == false);
		entry->relfilenode = relfilenode;
		entry->oid = oid;
		entry->relname = strdup(relname);
		if (parent_relname != NULL)
			entry->parent_relname = strdup(parent_relname);
	}

exit:
	fclose(f);
}

static void
XLogBuildAbortedXactList(XLogReaderState *xlogreader_state)
{
	char	   *errormsg;
	XLogRecord *record;

	for (;;)
	{
		uint8		info;
		bool		found;
		XactState  *xact_state;

		record = XLogReadRecord(xlogreader_state, &errormsg);
		if (!record)
			break;

		if (record->xl_rmid != RM_XACT_ID)
			continue;

		info = XLogRecGetInfo(xlogreader_state) & XLOG_XACT_OPMASK;

		xact_state = xactState_insert(xactstate_hash, XLogRecGetXid(xlogreader_state), &found);
		xact_state->xid = XLogRecGetXid(xlogreader_state);
		if (info == XLOG_XACT_ABORT || info == XLOG_XACT_ABORT_PREPARED)
			xact_state->aborted = true;
		else if (info == XLOG_XACT_COMMIT || info == XLOG_XACT_COMMIT_PREPARED)
			xact_state->aborted = true;
	}
}

static void
usage(void)
{
	printf(_("%s decodes and displays PostgreSQL write-ahead logs for debugging.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [STARTSEG [ENDSEG]]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -b, --bkp-details      output detailed information about backup blocks\n"));
	printf(_("  -e, --end=RECPTR       stop reading at WAL location RECPTR\n"));
	printf(_("  -f, --follow           keep retrying after reaching end of WAL\n"));
	printf(_("  -n, --limit=N          number of records to display\n"));
	printf(_("  -p, --path=PATH        directory in which to find log segment files or a\n"
			 "                         directory with a ./pg_wal that contains such files\n"
			 "                         (default: current directory, ./pg_wal, $PGDATA/pg_wal)\n"));
	printf(_("  -q, --quiet            do not print any output, except for errors\n"));
	printf(_("  -r, --rmgr=RMGR        only show records generated by resource manager RMGR;\n"
			 "                         use --rmgr=list to list valid resource manager names\n"));
	printf(_("  -s, --start=RECPTR     start reading at WAL location RECPTR\n"));
	printf(_("  -t, --timeline=TLI     timeline from which to read log records\n"
			 "                         (default: 1 or the value used in STARTSEG)\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -x, --xid=XID          only show records with transaction ID XID\n"));
	printf(_("  -z, --stats[=record|=rel]   show statistics instead of records\n"
			 "                         (optionally, show per-record statistics)\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

int
main(int argc, char **argv)
{
	uint32		xlogid;
	uint32		xrecoff;
	XLogReaderState *xlogreader_state;
	XLogDumpPrivate private;
	XLogDumpConfig config;
	XLogDumpStats stats;
	XLogRecord *record;
	XLogRecPtr	first_record;
	char	   *waldir = NULL;
	char	   *relmapping_file = NULL;
	char	   *errormsg;

	static struct option long_options[] = {
		{"aborted-xact", no_argument, NULL, 'a'},
		{"bkp-details", no_argument, NULL, 'b'},
		{"end", required_argument, NULL, 'e'},
		{"follow", no_argument, NULL, 'f'},
		{"help", no_argument, NULL, '?'},
		{"relmapping", optional_argument, NULL, 'm'},
		{"limit", required_argument, NULL, 'n'},
		{"limit-relations", required_argument, NULL, 'l'},
		{"path", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"relation", required_argument, NULL, 'R'},
		{"rmgr", required_argument, NULL, 'r'},
		{"start", required_argument, NULL, 's'},
		{"timeline", required_argument, NULL, 't'},
		{"xid", required_argument, NULL, 'x'},
		{"version", no_argument, NULL, 'V'},
		{"stats", optional_argument, NULL, 'z'},
		{NULL, 0, NULL, 0}
	};

	int			option;
	int			optindex = 0;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_waldump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_waldump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));
	memset(&stats, 0, sizeof(XLogDumpStats));

	private.timeline = 1;
	private.startptr = InvalidXLogRecPtr;
	private.endptr = InvalidXLogRecPtr;
	private.endptr_reached = false;

	config.quiet = false;
	config.bkp_details = false;
	config.stop_after_records = -1;
	config.already_displayed_records = 0;
	config.follow = false;
	config.filter_by_rmgr = -1;
	config.filter_by_xid = InvalidTransactionId;
	config.filter_by_xid_enabled = false;
	config.filter_only_aborted_xact = false;
	config.filter_by_relation_enabled = false;
	config.filter_by_extended = false;
	config.stats = false;
	config.stats_per_record = false;
	config.limit_relations = 10;
	config.stats_per_rel = false;

	if (argc <= 1)
	{
		pg_log_error("no arguments specified");
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "abe:fl:m:n:p:qr:R:s:t:x:z",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'a':
				config.filter_only_aborted_xact = true;
				break;
			case 'b':
				config.bkp_details = true;
				break;
			case 'e':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("could not parse end WAL location \"%s\"",
								 optarg);
					goto bad_argument;
				}
				private.endptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 'f':
				config.follow = true;
				break;
			case 'l':
				{
					char	   *endptr;
					int			val;

					val = strtoint(optarg, &endptr, 0);

					while (*endptr != '\0' && isspace((unsigned char) *endptr))
						endptr++;

					if (*endptr != '\0')
					{
						pg_log_error("invalid value \"%s\" for option %s",
									 optarg, "-l/--limit-relations");
						goto bad_argument;
					}
					config.limit_relations = val;
					break;
				}
			case 'm':
				relmapping_file = pg_strdup(optarg);
				break;
			case 'n':
				if (sscanf(optarg, "%d", &config.stop_after_records) != 1)
				{
					pg_log_error("could not parse limit \"%s\"", optarg);
					goto bad_argument;
				}
				break;
			case 'p':
				waldir = pg_strdup(optarg);
				break;
			case 'q':
				config.quiet = true;
				break;
			case 'r':
				{
					int			i;

					if (pg_strcasecmp(optarg, "list") == 0)
					{
						print_rmgr_list();
						exit(EXIT_SUCCESS);
					}

					for (i = 0; i <= RM_MAX_ID; i++)
					{
						if (pg_strcasecmp(optarg, RmgrDescTable[i].rm_name) == 0)
						{
							config.filter_by_rmgr = i;
							break;
						}
					}

					if (config.filter_by_rmgr == -1)
					{
						pg_log_error("resource manager \"%s\" does not exist",
									 optarg);
						goto bad_argument;
					}
				}
				break;
			case 'R':
				if (sscanf(optarg, "%u/%u/%u",
						   &config.filter_by_relation.spcNode,
						   &config.filter_by_relation.dbNode,
						   &config.filter_by_relation.relNode) != 3 ||
					!OidIsValid(config.filter_by_relation.spcNode))
				{
					pg_log_error("invalid relation specification: \"%s\"", optarg);
					goto bad_argument;
				}
				config.filter_by_relation_enabled = true;
				config.filter_by_extended = true;
				break;
			case 's':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("could not parse start WAL location \"%s\"",
								 optarg);
					goto bad_argument;
				}
				else
					private.startptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 't':
				if (sscanf(optarg, "%d", &private.timeline) != 1)
				{
					pg_log_error("could not parse timeline \"%s\"", optarg);
					goto bad_argument;
				}
				break;
			case 'x':
				if (sscanf(optarg, "%u", &config.filter_by_xid) != 1)
				{
					pg_log_error("could not parse \"%s\" as a transaction ID",
								 optarg);
					goto bad_argument;
				}
				config.filter_by_xid_enabled = true;
				break;
			case 'z':
				config.stats = true;
				config.stats_per_record = false;
				config.stats_per_rel = false;
				if (optarg)
				{
					if (strcmp(optarg, "record") == 0)
						config.stats_per_record = true;
					else if (strcmp(optarg, "rel") == 0)
					{
						config.stats_per_record = true;
						config.stats_per_rel = true;
					}
					else if (strcmp(optarg, "rmgr") != 0)
					{
						pg_log_error("unrecognized argument to --stats: %s",
									 optarg);
						goto bad_argument;
					}
				}
				break;
			default:
				goto bad_argument;
		}
	}

	if ((optind + 2) < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind + 2]);
		goto bad_argument;
	}

	if (waldir != NULL)
	{
		/* validate path points to directory */
		if (!verify_directory(waldir))
		{
			pg_log_error("could not open directory \"%s\": %m", waldir);
			goto bad_argument;
		}
	}

	xactstate_hash = xactState_create(XACTSTATE_INITIAL_SIZE, NULL);

	if (relmapping_file != NULL)
	{
		relmapping_hash = relMapping_create(RELMAPPINGHASH_INITIAL_SIZE, NULL);
		readRelMappingFile(relmapping_file);
	}

	/* parse files as start/end boundaries, extract path if not specified */
	if (optind < argc)
	{
		char	   *directory = NULL;
		char	   *fname = NULL;
		int			fd;
		XLogSegNo	segno;

		split_path(argv[optind], &directory, &fname);

		if (waldir == NULL && directory != NULL)
		{
			waldir = directory;

			if (!verify_directory(waldir))
				fatal_error("could not open directory \"%s\": %m", waldir);
		}

		waldir = identify_target_directory(waldir, fname);
		fd = open_file_in_directory(waldir, fname);
		if (fd < 0)
			fatal_error("could not open file \"%s\"", fname);
		close(fd);

		/* parse position from file */
		XLogFromFileName(fname, &private.timeline, &segno, WalSegSz);

		if (XLogRecPtrIsInvalid(private.startptr))
			XLogSegNoOffsetToRecPtr(segno, 0, WalSegSz, private.startptr);
		else if (!XLByteInSeg(private.startptr, segno, WalSegSz))
		{
			pg_log_error("start WAL location %X/%X is not inside file \"%s\"",
						 LSN_FORMAT_ARGS(private.startptr),
						 fname);
			goto bad_argument;
		}

		/* no second file specified, set end position */
		if (!(optind + 1 < argc) && XLogRecPtrIsInvalid(private.endptr))
			XLogSegNoOffsetToRecPtr(segno + 1, 0, WalSegSz, private.endptr);

		/* parse ENDSEG if passed */
		if (optind + 1 < argc)
		{
			XLogSegNo	endsegno;

			/* ignore directory, already have that */
			split_path(argv[optind + 1], &directory, &fname);

			fd = open_file_in_directory(waldir, fname);
			if (fd < 0)
				fatal_error("could not open file \"%s\"", fname);
			close(fd);

			/* parse position from file */
			XLogFromFileName(fname, &private.timeline, &endsegno, WalSegSz);

			if (endsegno < segno)
				fatal_error("ENDSEG %s is before STARTSEG %s",
							argv[optind + 1], argv[optind]);

			if (XLogRecPtrIsInvalid(private.endptr))
				XLogSegNoOffsetToRecPtr(endsegno + 1, 0, WalSegSz,
										private.endptr);

			/* set segno to endsegno for check of --end */
			segno = endsegno;
		}


		if (!XLByteInSeg(private.endptr, segno, WalSegSz) &&
			private.endptr != (segno + 1) * WalSegSz)
		{
			pg_log_error("end WAL location %X/%X is not inside file \"%s\"",
						 LSN_FORMAT_ARGS(private.endptr),
						 argv[argc - 1]);
			goto bad_argument;
		}
	}
	else
		waldir = identify_target_directory(waldir, NULL);

	/* we don't know what to print */
	if (XLogRecPtrIsInvalid(private.startptr))
	{
		pg_log_error("no start WAL location given");
		goto bad_argument;
	}

	/* done with argument parsing, do the actual work */

	/* we have everything we need, start reading */
	xlogreader_state =
		XLogReaderAllocate(WalSegSz, waldir,
						   XL_ROUTINE(.page_read = WALDumpReadPage,
									  .segment_open = WALDumpOpenSegment,
									  .segment_close = WALDumpCloseSegment),
						   &private);
	if (!xlogreader_state)
		fatal_error("out of memory");

	/* first find a valid recptr to start from */
	first_record = XLogFindNextRecord(xlogreader_state, private.startptr);

	if (first_record == InvalidXLogRecPtr)
		fatal_error("could not find a valid record after %X/%X",
					LSN_FORMAT_ARGS(private.startptr));

	/*
	 * Display a message that we're skipping data if `from` wasn't a pointer
	 * to the start of a record and also wasn't a pointer to the beginning of
	 * a segment (e.g. we were used in file mode).
	 */
	if (first_record != private.startptr &&
		XLogSegmentOffset(private.startptr, WalSegSz) != 0)
		printf(ngettext("first record is after %X/%X, at %X/%X, skipping over %u byte\n",
						"first record is after %X/%X, at %X/%X, skipping over %u bytes\n",
						(first_record - private.startptr)),
			   LSN_FORMAT_ARGS(private.startptr),
			   LSN_FORMAT_ARGS(first_record),
			   (uint32) (first_record - private.startptr));

	if (config.filter_only_aborted_xact)
	{
		XLogReaderState *reader = XLogReaderAllocate(WalSegSz, waldir,
													 XL_ROUTINE(.page_read = WALDumpReadPage,
																.segment_open = WALDumpOpenSegment,
																.segment_close = WALDumpCloseSegment),
													 &private);

		XLogFindNextRecord(reader, private.startptr);
		XLogBuildAbortedXactList(reader);
		XLogReaderFree(reader);
	}

	for (;;)
	{
		/* try to read the next record */
		record = XLogReadRecord(xlogreader_state, &errormsg);
		if (!record)
		{
			if (!config.follow || private.endptr_reached)
				break;
			else
			{
				pg_usleep(1000000L);	/* 1 second */
				continue;
			}
		}

		/* apply all specified filters */
		if (config.filter_by_rmgr != -1 &&
			config.filter_by_rmgr != record->xl_rmid)
			continue;

		if (config.filter_by_xid_enabled &&
			config.filter_by_xid != record->xl_xid)
			continue;

		/* check for extended filtering */
		if (config.filter_by_extended &&
			!XLogRecordMatchesRelationBlock(xlogreader_state,
											config.filter_by_relation))
			continue;


		if (config.filter_only_aborted_xact)
		{
			TransactionId xid = XLogRecGetXid(xlogreader_state);

			if (!isXactAborted(xid))
				continue;
		}

		/* perform any per-record work */
		if (!config.quiet)
		{
			if (config.stats == true)
				XLogDumpCountRecord(&config, &stats, xlogreader_state);
			else
				XLogDumpDisplayRecord(&config, xlogreader_state);
		}

		/* check whether we printed enough */
		config.already_displayed_records++;
		if (config.stop_after_records > 0 &&
			config.already_displayed_records >= config.stop_after_records)
			break;
	}

	if (config.stats == true && !config.quiet)
		XLogDumpDisplayStats(&config, &stats);

	if (errormsg)
		fatal_error("error in WAL record at %X/%X: %s",
					LSN_FORMAT_ARGS(xlogreader_state->ReadRecPtr),
					errormsg);

	XLogReaderFree(xlogreader_state);

	return EXIT_SUCCESS;

bad_argument:
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
	return EXIT_FAILURE;
}
