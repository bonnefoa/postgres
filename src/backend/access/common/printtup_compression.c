#include "postgres.h"

#include "access/printtup.h"
#include "access/printcompressed.h"
#include "common/logging.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "tcop/pquery.h"
#include "utils/lsyscache.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "varatt.h"

#ifdef USE_ZSTD
#include <zstd.h>
#endif

/* ----------------
 *		Private state for a printtup destination object
 *
 * NOTE: finfo is the lookup info for either typoutput or typsend, whichever
 * we are using for this column.
 * ----------------
 */
typedef struct
{								/* Per-attribute information */
	Oid			typoutput;		/* Oid for the type's text output fn */
	Oid			typsend;		/* Oid for the type's binary output fn */
	bool		typisvarlena;	/* is it varlena (ie possibly toastable)? */
	int16		format;			/* format code for this column */
	FmgrInfo	finfo;			/* Precomputed call info for output fn */
} PrinttupAttrInfo;

typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Portal		portal;			/* the Portal we are printing from */
	bool		sendDescrip;	/* send RowDescription at startup? */
	TupleDesc	attrinfo;		/* The attr info we are set up for */
	int			nattrs;
	PrinttupAttrInfo *myinfo;	/* Cached info about each attr */
	StringInfoData buf;			/* output buffer (*not* in tmpcontext) */
	MemoryContext tmpcontext;	/* Memory context for per-row workspace */

	StringInfoData data;
	ZSTD_CStream  *cctx;
	ZSTD_outBuffer zstd_outBuf;
} DR_printtup;


void
printcompressed_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_printtup *myState = (DR_printtup *) self;
	Portal		portal = myState->portal;
	size_t		ret;

	/*
	 * Create I/O buffer to be used for all messages.  This cannot be inside
	 * tmpcontext, since we want to re-use it across rows.
	 */
	initStringInfo(&myState->buf);

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.
	 */
	myState->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												"printtup",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * If we are supposed to emit row descriptions, then send the tuple
	 * descriptor of the tuples.
	 */
	if (myState->sendDescrip)
		SendRowDescriptionMessage(&myState->buf,
								  typeinfo,
								  FetchPortalTargetList(portal),
								  portal->formats);

	myState->cctx = ZSTD_createCStream();
	if (!myState->cctx)
		pg_fatal("could not create zstd compression context");

	ret = ZSTD_CCtx_setParameter(myState->cctx, ZSTD_c_compressionLevel,
								 ZSTD_CLEVEL_DEFAULT);
	if (ZSTD_isError(ret))
		pg_fatal("could not set zstd compression level to %d: %s",
				 ZSTD_CLEVEL_DEFAULT, ZSTD_getErrorName(ret));

    pq_beginmessage_reuse(&myState->buf, PqMsg_DataRowCompressed);
    pq_endmessage_reuse(&myState->buf);
}


/*
 * Get the lookup info that printtup() needs
 */
static void
printtup_prepare_info(DR_printtup *myState, TupleDesc typeinfo, int numAttrs)
{
	int16	   *formats = myState->portal->formats;
	int			i;

	/* get rid of any old data */
	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;

	myState->myinfo = (PrinttupAttrInfo *)
		palloc0(numAttrs * sizeof(PrinttupAttrInfo));

	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		int16		format = (formats ? formats[i] : 0);
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

		thisState->format = format;
		if (format == 0)
		{
			getTypeOutputInfo(attr->atttypid,
							  &thisState->typoutput,
							  &thisState->typisvarlena);
			fmgr_info(thisState->typoutput, &thisState->finfo);
		}
		else if (format == 1)
		{
			getTypeBinaryOutputInfo(attr->atttypid,
									&thisState->typsend,
									&thisState->typisvarlena);
			fmgr_info(thisState->typsend, &thisState->finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}
}

/* ----------------
 *		printtup --- send a tuple to the client
 *
 * Note: if you change this function, see also serializeAnalyzeReceive
 * in explain.c, which is meant to replicate the computations done here.
 * ----------------
 */
static bool
printtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	StringInfo	buf = &myState->data;
	int			natts = typeinfo->natts;
	int			i;

	/*
	 * Prepare a DataRow message (note buffer is in per-query context)
	 */
	pq_beginmessage_reuse(buf, PqMsg_DataRow);

	pq_sendint16(buf, natts);

	/*
	 * send the attributes of this tuple
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		attr = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			pq_sendint32(buf, -1);
			continue;
		}

		/*
		 * Here we catch undefined bytes in datums that are returned to the
		 * client without hitting disk; see comments at the related check in
		 * PageAddItem().  This test is most useful for uncompressed,
		 * non-external datums, but we're quite likely to see such here when
		 * testing new C functions.
		 */
		if (thisState->typisvarlena)
			VALGRIND_CHECK_MEM_IS_DEFINED(DatumGetPointer(attr),
										  VARSIZE_ANY(DatumGetPointer(attr)));

		if (thisState->format == 0)
		{
			/* Text output */
			char	   *outputstr;

			outputstr = OutputFunctionCall(&thisState->finfo, attr);
			pq_sendcountedtext(buf, outputstr, strlen(outputstr));
		}
		else
		{
			/* Binary output */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&thisState->finfo, attr);
			pq_sendint32(buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	return true;
}


/* ----------------
 *		printtup_compressed --- send a tuple to the client
 * ----------------
 */
bool printcompressed(TupleTableSlot *slot, DestReceiver *self) {
  TupleDesc typeinfo = slot->tts_tupleDescriptor;
  DR_printtup *myState = (DR_printtup *)self;
  MemoryContext oldcontext;
  StringInfo buf = &myState->buf;
  int natts = typeinfo->natts;

  /* Set or update my derived attribute info, if needed */
  if (myState->attrinfo != typeinfo || myState->nattrs != natts)
    printtup_prepare_info(myState, typeinfo, natts);

  /* Make sure the tuple is fully deconstructed */
  slot_getallattrs(slot);

  /* Switch into per-row context so we can recover memory below */
  oldcontext = MemoryContextSwitchTo(myState->tmpcontext);

  /* Fill buffer */
  printtup(slot, self);


  pq_endmessage_reuse(buf);

  /* Return to caller's context, and flush row's temporary memory */
  MemoryContextSwitchTo(oldcontext);
  MemoryContextReset(myState->tmpcontext);

  return true;
}


/* ----------------
 *		printcompressed_shutdown
 * ----------------
 */
void
printcompressed_shutdown(DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;

	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = NULL;

	if (myState->buf.data)
		pfree(myState->buf.data);
	myState->buf.data = NULL;

	if (myState->tmpcontext)
		MemoryContextDelete(myState->tmpcontext);
	myState->tmpcontext = NULL;
}

/* ----------------
 *		printcompressed_destroy
 * ----------------
 */
void
printcompressed_cleanup(DestReceiver *self)
{
	pfree(self);
}
