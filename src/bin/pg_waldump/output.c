#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pg_waldump.h"

static char *
relnodeToRelname(XLogDumpConfig *config, RelFileNode relnode)
{
	RelMappingEntry *relmapping_entry;
	char	   *res;

	if (relmapping_hash == NULL)
		return psprintf("|%u/%u/%u", relnode.spcNode, relnode.dbNode,
						relnode.relNode);

	relmapping_entry = relMapping_lookup(relmapping_hash, relnode.relNode);
	if (relmapping_entry == NULL)
		return psprintf("|%u/%u/%u", relnode.spcNode, relnode.dbNode,
						relnode.relNode);

	if (relmapping_entry->toast_parent != NULL)
		res = psprintf("|%s (toast)", relmapping_entry->toast_parent);
	else if (relmapping_entry->toast_index_parent != NULL)
		res = psprintf("|%s (toast index)", relmapping_entry->toast_index_parent);
	else
		res = psprintf("|%s", relmapping_entry->relname);

	if (config->rel_details)
		res = psprintf("%s (%u/%u/%u)", res, relnode.spcNode,
					   relnode.dbNode, relnode.relNode);

	return res;
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
void
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

