/*-------------------------------------------------------------------------
 *
 * yb_query_diagnostics.c
 *    Utilities for Query Diagnostics/Yugabyte (Postgres layer) integration
 *    that have to be defined on the PostgreSQL side.
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/yb_query_diagnostics.c
 *
 *-------------------------------------------------------------------------
 */

#include "yb_query_diagnostics.h"

#include "access/hash.h"
#include "common/file_perm.h"
#include "common/pg_yb_common.h"
#include "funcapi.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"

#define QUERY_DIAGNOSTICS_HASH_MAX_SIZE 100	/* Maximum number of entries in the hash table */
/* Constants used for yb_query_diagnostics_status view */
#define YB_QUERY_DIAGNOSTICS_STATUS_COLS 8
#define DIAGNOSTICS_SUCCESS 0
#define DIAGNOSTICS_IN_PROGRESS 1
#define DIAGNOSTICS_ERROR 2
#define DESCRIPTION_LEN 128

typedef struct BundleInfo
{
	YbQueryDiagnosticsMetadata metadata; /* stores bundle's metadata */
	int			status; /* 0 - Success; 1 - In Progress; 2 - ERROR */
	char		description[DESCRIPTION_LEN]; /* stores error description */
} BundleInfo;

typedef struct YbQueryDiagnosticsBundles
{
	int			index;			/* index to insert new buffer entry */
	int			max_entries;	/* maximum # of entries in the buffer */
	LWLock	 	lock;			/* protects circular buffer from search/modification */
	BundleInfo	bundles[FLEXIBLE_ARRAY_MEMBER]; /* circular buffer to store info about bundles */
} YbQueryDiagnosticsBundles;

/* GUC variables */
int yb_query_diagnostics_bg_worker_interval_ms;
int yb_query_diagnostics_circular_buffer_size;

/* Saved hook value in case of unload */
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Flags set by interrupt handlers for later service in the main loop. */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

static HTAB *bundles_in_progress = NULL;
static LWLock *bundles_in_progress_lock; /* protects bundles_in_progress hash table */
static YbQueryDiagnosticsBundles *bundles_completed = NULL;
static const char *status_msg[] = {"Success", "In Progress", "Error"};

static void YbQueryDiagnostics_ExecutorEnd(QueryDesc *queryDesc);

static void InsertNewBundleInfo(YbQueryDiagnosticsMetadata *metadata);
static void FetchParams(YbQueryDiagnosticsParams *params, FunctionCallInfo fcinfo);
static void ConstructDiagnosticsPath(YbQueryDiagnosticsMetadata *metadata);
static void FormatParams(StringInfo buf, const ParamListInfo params);
static int DumpToFile(const char *path, const char *file_name,
					   const char *data, char *description);
static void RemoveExpiredEntries();
static void AccumulateBindVariables(YbQueryDiagnosticsEntry *entry,
									const double totaltime_ms, const ParamListInfo params);
static void YbQueryDiagnosticsBgWorkerSighup(SIGNAL_ARGS);
static void YbQueryDiagnosticsBgWorkerSigterm(SIGNAL_ARGS);
static inline bool HasBundleExpired(const YbQueryDiagnosticsEntry *entry, TimestampTz current_time);
static int YbQueryDiagnosticsBundlesShmemSize(void);
static Datum CreateJsonb(const YbQueryDiagnosticsParams *params);
static void CreateJsonbInt(JsonbParseState *state, char *key, int64 value);
static void CreateJsonbBool(JsonbParseState *state, char *key, bool value);
static void InsertCompletedBundleInfo(YbQueryDiagnosticsMetadata *metadata, int status,
							 const char *description);
static void OutputBundle(const YbQueryDiagnosticsMetadata metadata, const char *description,
			 const char *status, Tuplestorestate *tupstore, TupleDesc tupdesc);
static void ProcessActiveBundles(Tuplestorestate *tupstore, TupleDesc tupdesc);
static void ProcessCompletedBundles(Tuplestorestate *tupstore, TupleDesc tupdesc);
static inline int CircularBufferMaxEntries(void);

void
YbQueryDiagnosticsInstallHook(void)
{
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = YbQueryDiagnostics_ExecutorEnd;
}

/*
 * YbQueryDiagnosticsBundlesShmemSize
 *		Compute space needed for yb_query_diagnostics_status view related shared memory
 */
static int
YbQueryDiagnosticsBundlesShmemSize(void)
{
	Size		size;

	size = offsetof(YbQueryDiagnosticsBundles, bundles);
	size = add_size(size, mul_size(CircularBufferMaxEntries(), sizeof(BundleInfo)));

	return size;
}

/*
 * YbQueryDiagnosticsShmemSize
 *		Compute space needed for QueryDiagnostics-related shared memory
 */
Size
YbQueryDiagnosticsShmemSize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(LWLock));
	size = add_size(size, hash_estimate_size(QUERY_DIAGNOSTICS_HASH_MAX_SIZE,
													sizeof(YbQueryDiagnosticsEntry)));
	size = add_size(size, YbQueryDiagnosticsBundlesShmemSize());

	return size;
}

/*
 * YbQueryDiagnosticsShmemInit
 *		Allocate and initialize QueryDiagnostics-related shared memory
 */
void
YbQueryDiagnosticsShmemInit(void)
{
	HASHCTL 	ctl;
	bool 		found;

	bundles_in_progress = NULL;
	/* Initialize the hash table control structure */
	MemSet(&ctl, 0, sizeof(ctl));

	/* Set the key size and entry size */
	ctl.keysize = sizeof(int64);
	ctl.entrysize = sizeof(YbQueryDiagnosticsEntry);

	/* Create the hash table in shared memory */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	bundles_in_progress_lock = (LWLock *)ShmemInitStruct("YbQueryDiagnostics Lock",
														  sizeof(LWLock), &found);

	if (!found)
	{
		/* First time through ... */
		LWLockRegisterTranche(LWTRANCHE_YB_QUERY_DIAGNOSTICS,
							  "yb_query_diagnostics bundles_in_progress hash lock");
		LWLockInitialize(bundles_in_progress_lock,
						 LWTRANCHE_YB_QUERY_DIAGNOSTICS);
	}

	bundles_in_progress = ShmemInitHash("YbQueryDiagnostics shared hash table",
											  QUERY_DIAGNOSTICS_HASH_MAX_SIZE,
											  QUERY_DIAGNOSTICS_HASH_MAX_SIZE,
											  &ctl,
											  HASH_ELEM | HASH_BLOBS);

	LWLockRelease(AddinShmemInitLock);

	bundles_completed =
		(YbQueryDiagnosticsBundles *) ShmemInitStruct("YbQueryDiagnostics Status",
													  YbQueryDiagnosticsBundlesShmemSize(),
													  &found);

	if (!found)
	{
		/* First time through ... */
		bundles_completed->index = 0;
		bundles_completed->max_entries = CircularBufferMaxEntries();

		MemSet(bundles_completed->bundles, 0, sizeof(BundleInfo) * bundles_completed->max_entries);

		LWLockRegisterTranche(LWTRANCHE_YB_QUERY_DIAGNOSTICS_CIRCULAR_BUFFER,
							  "query_diagnostics_circular_buffer_lock");
		LWLockInitialize(&bundles_completed->lock,
						 LWTRANCHE_YB_QUERY_DIAGNOSTICS_CIRCULAR_BUFFER);
	}
}

static inline int
CircularBufferMaxEntries(void)
{
	return yb_query_diagnostics_circular_buffer_size * 1024 / sizeof(BundleInfo);
}

/*
 * InsertBundleInfo
 * 		Add a query diagnostics entry to the circular buffer.
 */
static void
InsertCompletedBundleInfo(YbQueryDiagnosticsMetadata *metadata, int status,
						  const char *description)
{
	BundleInfo *sample;

	LWLockAcquire(&bundles_completed->lock, LW_EXCLUSIVE);

	sample = &bundles_completed->bundles[bundles_completed->index];
	sample->status = status;
	memcpy(&sample->metadata, metadata, sizeof(YbQueryDiagnosticsMetadata));
	memcpy(sample->description, description, strlen(description));

	/* Advance the index, wrapping around if necessary */
	if (++bundles_completed->index == bundles_completed->max_entries)
		bundles_completed->index = 0;

	LWLockRelease(&bundles_completed->lock);
}

static void
CreateJsonbInt(JsonbParseState *state, char *key, int64 value)
{
	JsonbValue	json_key;
	JsonbValue	json_value;

	json_key.type = jbvString;
	json_key.val.string.len = strlen(key);
	json_key.val.string.val = key;

	json_value.type = jbvNumeric;
	json_value.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, value));

	pushJsonbValue(&state, WJB_KEY, &json_key);
	pushJsonbValue(&state, WJB_VALUE, &json_value);
}

static void
CreateJsonbBool(JsonbParseState *state, char *key, bool value)
{
	JsonbValue	json_key;
	JsonbValue	json_value;

	json_key.type = jbvString;
	json_key.val.string.len = strlen(key);
	json_key.val.string.val = key;

	json_value.type = jbvBool;
	json_value.val.boolean = value;

	pushJsonbValue(&state, WJB_KEY, &json_key);
	pushJsonbValue(&state, WJB_VALUE, &json_value);
}

/*
 * CreateJsonb
 * 		Create a JSONB representation of the explain parameters given as input
 * 		while starting query diagnostics.
 */
static Datum
CreateJsonb(const YbQueryDiagnosticsParams *params)
{
	JsonbParseState *state = NULL;
	JsonbValue *result;

	Assert(params != NULL);

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	CreateJsonbInt(state, "explain_sample_rate", params->explain_sample_rate);
	CreateJsonbBool(state, "explain_analyze", params->explain_analyze);
	CreateJsonbBool(state, "explain_dist", params->explain_dist);
	CreateJsonbBool(state, "explain_debug", params->explain_debug);

	result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result));
}

/*
 * yb_get_query_diagnostics_status
 *		This function returns a set of rows containing information about active, successful and
 *		errored out query diagnostic bundles.
 *		It's designed to be displayed as yb_query_diagnostics_status view.
 */
Datum
yb_get_query_diagnostics_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* Ensure that query diagnostics is enabled */
	if (!YBIsQueryDiagnosticsEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("TEST_yb_enable_query_diagnostics gflag must be true")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errmsg_internal("return type must be a row type")));

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	ProcessActiveBundles(tupstore, tupdesc);
	ProcessCompletedBundles(tupstore, tupdesc);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static void
OutputBundle(const YbQueryDiagnosticsMetadata metadata, const char *description,
			 const char *status, Tuplestorestate *tupstore, TupleDesc tupdesc)
{
	/* Arrays to hold the values and null flags for each column in a row */
	Datum		values[YB_QUERY_DIAGNOSTICS_STATUS_COLS];
	bool		nulls[YB_QUERY_DIAGNOSTICS_STATUS_COLS];
	int			j = 0;

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	values[j++] = CStringGetTextDatum(status);
	values[j++] = CStringGetTextDatum(description);
	values[j++] = Int64GetDatum(metadata.params.query_id);
	values[j++] = TimestampTzGetDatum(metadata.start_time);
	values[j++] = Int64GetDatum(metadata.params.diagnostics_interval_sec);
	values[j++] = Int64GetDatum(metadata.params.bind_var_query_min_duration_ms);
	values[j++] = CreateJsonb(&metadata.params);
	values[j++] = CStringGetTextDatum(metadata.path);

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

/*
 * ProcessActiveBundles
 *		Process and store information about active query diagnostics bundles
 *
 * This function iterates through the shared hash table, retrieves the information for each entry,
 * and stores it in the tuplestore in proper format.
 */
static void
ProcessActiveBundles(Tuplestorestate *tupstore, TupleDesc tupdesc)
{
	HASH_SEQ_STATUS	status;
	YbQueryDiagnosticsEntry *entry;

	LWLockAcquire(bundles_in_progress_lock, LW_SHARED);

	hash_seq_init(&status, bundles_in_progress);

	while ((entry = hash_seq_search(&status)) != NULL)
		OutputBundle(entry->metadata, "",
					 status_msg[DIAGNOSTICS_IN_PROGRESS], tupstore, tupdesc);

	LWLockRelease(bundles_in_progress_lock);
}

/*
 * ProcessCompletedBundles
 *		Process and store information about successful and errored out query diagnostic bundles
 *
 * This function iterates through the circular buffer of query diagnostic bundles,
 * formats the information for each valid entry, and stores it in the tuplestore.
 */
static void
ProcessCompletedBundles(Tuplestorestate *tupstore, TupleDesc tupdesc)
{
	LWLockAcquire(&bundles_completed->lock, LW_SHARED);

	for (int i = 0; i < bundles_completed->max_entries; ++i)
	{
		BundleInfo *sample= &bundles_completed->bundles[i];

		if (sample->metadata.params.query_id != 0)
			OutputBundle(sample->metadata, sample->description,
						 status_msg[sample->status], tupstore, tupdesc);
	}

	LWLockRelease(&bundles_completed->lock);
}

static void
YbQueryDiagnosticsBgWorkerSighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

static void
YbQueryDiagnosticsBgWorkerSigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * YbQueryDiagnosticsBgWorkerRegister
 *		Register the background worker for yb_query_diagnostics
 *
 * Background worker is required to periodically check for expired entries
 * within the shared hash table and stop the query diagnostics for them.
 */
void
YbQueryDiagnosticsBgWorkerRegister(void)
{
	BackgroundWorker worker;
	MemSet(&worker, 0, sizeof(worker));
	sprintf(worker.bgw_name, "yb_query_diagnostics bgworker");
	sprintf(worker.bgw_type, "yb_query_diagnostics bgworker");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	/* Value of 1 allows the background worker for yb_query_diagnostics to restart */
	worker.bgw_restart_time = 1;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "YbQueryDiagnosticsMain");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
}

static void
YbQueryDiagnostics_ExecutorEnd(QueryDesc *queryDesc)
{
	uint64		query_id = queryDesc->plannedstmt->queryId;
	double 		totaltime_ms;
	YbQueryDiagnosticsEntry *entry;

	LWLockAcquire(bundles_in_progress_lock, LW_SHARED);

	/*
	 * This can slow down the query execution, even if the query is not being bundled.
	 */
	entry = (YbQueryDiagnosticsEntry *) hash_search(bundles_in_progress,
													&query_id, HASH_FIND,
													NULL);

	if (entry)
	{
		totaltime_ms = INSTR_TIME_GET_MILLISEC(queryDesc->totaltime->counter);

		if (queryDesc->params &&
			entry->metadata.params.bind_var_query_min_duration_ms <= totaltime_ms)
			AccumulateBindVariables(entry, totaltime_ms, queryDesc->params);
	}

	LWLockRelease(bundles_in_progress_lock);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
AccumulateBindVariables(YbQueryDiagnosticsEntry *entry, const double totaltime_ms,
						const ParamListInfo params)
{
	/* TODO(GH#22153): Handle the case when entry->bind_vars overflows */

	/* Check if the bind_vars is already full */
	SpinLockAcquire(&entry->mutex);
	bool is_full = strlen(entry->bind_vars) == YB_QD_MAX_BIND_VARS_LEN - 1;
	SpinLockRelease(&entry->mutex);

	if (is_full)
		return;

	StringInfoData buf;
	initStringInfo(&buf);
	FormatParams(&buf, params);
	appendStringInfo(&buf, "%lf\n", totaltime_ms);

	SpinLockAcquire(&entry->mutex);
	if (strlen(entry->bind_vars) + buf.len < YB_QD_MAX_BIND_VARS_LEN)
		memcpy(entry->bind_vars + strlen(entry->bind_vars), buf.data, buf.len);
	SpinLockRelease(&entry->mutex);

	pfree(buf.data);
}

/*
 * FormatParams
 *		Iterates over all of the params and prints them in CSV fromat.
 */
static void
FormatParams(StringInfo buf, const ParamListInfo params)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
													 "FormatParams temporary context",
													 ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(cxt);
	for (int i = 0; i < params->numParams; ++i)
	{
		if (params->params[i].isnull)
			appendStringInfo(buf, "NULL");
		else
		{
			Oid			typoutput;
			bool		typIsVarlena;
			char	   *val;

			getTypeOutputInfo(params->params[i].ptype,
							  &typoutput, &typIsVarlena);
			val = OidOutputFunctionCall(typoutput, params->params[i].value);

			appendStringInfo(buf, "%s,", val);
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * InsertNewBundleInfo
 *		Adds the entry into bundles_in_progress hash table.
 *		Entry is inserted only if it is not already present,
 *		otherwise an error is raised.
 */
static void
InsertNewBundleInfo(YbQueryDiagnosticsMetadata *metadata)
{
	int64		key = metadata->params.query_id;
	bool		found;
	YbQueryDiagnosticsEntry *entry;

	LWLockAcquire(bundles_in_progress_lock, LW_EXCLUSIVE);
	entry = (YbQueryDiagnosticsEntry *) hash_search(bundles_in_progress, &key,
													HASH_ENTER, &found);

	if (!found)
	{
		entry->metadata = *metadata;
		MemSet(entry->bind_vars, 0, YB_QD_MAX_BIND_VARS_LEN);
		SpinLockInit(&entry->mutex);
	}

	LWLockRelease(bundles_in_progress_lock);

	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("Query diagnostics for query_id[ %ld ] is already being generated",
						metadata->params.query_id)));
}

/*
 * HasBundleExpired
 * 		Checks if the diagnostics bundle has expired.
 * 		note: since TimestampTz is equivalent to microsecond,
 * 		diagnostics_interval is converted to microseconds before adding to start_time.
 */
static inline bool
HasBundleExpired(const YbQueryDiagnosticsEntry *entry, TimestampTz current_time)
{
	return current_time >= entry->metadata.start_time +
						   (entry->metadata.params.diagnostics_interval_sec * USECS_PER_SEC);
}

static void
RemoveExpiredEntries()
{
	/* TODO(GH#22447): Do this in O(1) */
	TimestampTz current_time = GetCurrentTimestamp();
	HASH_SEQ_STATUS status;
	YbQueryDiagnosticsEntry *entry;

	LWLockAcquire(bundles_in_progress_lock, LW_SHARED);
	/* Initialize the hash table scan */
	hash_seq_init(&status, bundles_in_progress);

	/* Scan the hash table */
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (HasBundleExpired(entry, current_time))
		{
			/*
			 * To avoid holding the lock while flushing to disk, we create a copy of the data
			 * that is to be dumped, this protects us from potential overwriting of the entry
			 * during the flushing process.
			 */
			SpinLockAcquire(&entry->mutex);

			char		bind_var_copy[YB_QD_MAX_BIND_VARS_LEN];
			char 		description[DESCRIPTION_LEN];
			int			status;
			YbQueryDiagnosticsMetadata metadata_copy = entry->metadata;

			memcpy(bind_var_copy, entry->bind_vars, YB_QD_MAX_BIND_VARS_LEN);
			memcpy(&metadata_copy, &entry->metadata, sizeof(YbQueryDiagnosticsMetadata));
			description[0] = '\0';

			SpinLockRelease(&entry->mutex);

			/* release the shared lock before flushing to disk */
			LWLockRelease(bundles_in_progress_lock);

			/* creates the directory structure recursively for this bundle */
			if (pg_mkdir_p((char *)metadata_copy.path, pg_dir_create_mode) == -1 && errno != EEXIST)
			{
				snprintf(description, DESCRIPTION_LEN,
						 "Failed to create query diagnostics directory");
				status = DIAGNOSTICS_ERROR;
			}
			else
				status = DumpToFile(metadata_copy.path, "bind_variables.csv",
									bind_var_copy, description);

			InsertCompletedBundleInfo(&metadata_copy, status, description);

			LWLockAcquire(bundles_in_progress_lock, LW_EXCLUSIVE);

			hash_search(bundles_in_progress, &metadata_copy.params.query_id,
						HASH_REMOVE, NULL);

			LWLockRelease(bundles_in_progress_lock);
			LWLockAcquire(bundles_in_progress_lock, LW_SHARED);
		}
	}
	LWLockRelease(bundles_in_progress_lock);
}

/*
 * DumpToFile
 *		Creates the file (/path/file_name) and writes the data to it.
 */
static int
DumpToFile(const char *path, const char *file_name, const char *data, char *description)
{
	File 		file = 0;
	const int	file_path_len = MAXPGPATH + strlen(file_name) + 1;
	char		file_path[file_path_len];

	/* No data to write */
	if (data[0] == '\0')
	{
		snprintf(description, DESCRIPTION_LEN, "No data captured");
		return DIAGNOSTICS_SUCCESS;
	}

#ifdef WIN32
	snprintf(file_path, file_path_len, "%s\\%s", path, file_name);
#else
	snprintf(file_path, file_path_len, "%s/%s", path, file_name);
#endif

	/*
	 * We use PG_TRY to handle any function returning an error. This ensures that the entry
	 * can be safely removed from the hash table even if the file writing fails.
	 */
	PG_TRY();
	{
		if ((file = PathNameOpenFile(file_path,
										O_RDWR | O_CREAT | O_TRUNC)) < 0)
			snprintf(description, DESCRIPTION_LEN,
					 "out of file descriptors: %m; release and retry");

		else if(FileWrite(file, (char *)data, strlen(data),
							WAIT_EVENT_DATA_FILE_WRITE) < 0)
			snprintf(description, DESCRIPTION_LEN, "Error writing to file; %m");
	}
	PG_CATCH();
	{
		ErrorData *edata;

		/* Capture the error data */
		edata = CopyErrorData();
		FlushErrorState();

		snprintf(description, DESCRIPTION_LEN, "%s", edata->message);

		FreeErrorData(edata);
	}
	PG_END_TRY();

	if (file > 0)
		FileClose(file);

	return description[0] == '\0' ? DIAGNOSTICS_SUCCESS : DIAGNOSTICS_ERROR;
}

/*
 * YbQueryDiagnosticsMain
 *		Background worker for yb_query_diagnostics
 *
 * Scans and removes expired entries within the shared hash table.
 * The worker sleeps for yb_query_diagnostics_bg_worker_interval_ms seconds
 * before scanning the hash table again.
 */
void
YbQueryDiagnosticsMain(Datum main_arg)
{
	/*
	 * TODO(GH#22612): Add support to switch off and on the bgworker as per the need,
	 *			       thereby saving resources
	 */
	ereport(LOG,
			(errmsg("starting bgworker for yb_query_diagnostics with time interval of %dms",
					 yb_query_diagnostics_bg_worker_interval_ms)));

	/* Register functions for SIGTERM/SIGHUP management */
	pqsignal(SIGHUP, YbQueryDiagnosticsBgWorkerSighup);
	pqsignal(SIGTERM, YbQueryDiagnosticsBgWorkerSigterm);

	/* Initialize the worker process */
	BackgroundWorkerUnblockSignals();

	pgstat_report_appname("yb_query_diagnostics bgworker");

	while (!got_sigterm)
	{
		int			rc;
		/* Wait necessary amount of time */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   yb_query_diagnostics_bg_worker_interval_ms,
					   YB_WAIT_EVENT_QUERY_DIAGNOSTICS_MAIN);
		ResetLatch(MyLatch);

		/* Bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Process signals */
		if (got_sighup)
		{
			/* Process config file */
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
			ereport(LOG,
					(errmsg("bgworker yb_query_diagnostics signal: processed SIGHUP")));
		}

		/* Check for expired entries within the shared hash table */
		RemoveExpiredEntries();
	}
	proc_exit(0);
}

/*
 * ConstructDiagnosticsPath
 *		Creates the directory structure for storing the diagnostics data.
 *		Directory structure: pg_data/query-diagnostics/queryid/random_number/
 *
 * Errors out in case the path is too long.
 */
static void
ConstructDiagnosticsPath(YbQueryDiagnosticsMetadata *metadata)
{
	int rand_num = DatumGetUInt32(hash_any((unsigned char*)&metadata->start_time,
										   sizeof(metadata->start_time)));
#ifdef WIN32
	const char *format = "%s\\%s\\%ld\\%d\\";
#else
	const char *format = "%s/%s/%ld/%d/";
#endif
	if (snprintf(metadata->path, MAXPGPATH, format,
				 DataDir, "query-diagnostics", metadata->params.query_id, rand_num) >= MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("Path to pg_data is too long"),
				 errhint("Move the data directory to a shorter path")));
}

/*
 * FetchParams
 *		Fetches the parameters from the yb_query_diagnostics function call and validates them.
 */
static void
FetchParams(YbQueryDiagnosticsParams *params, FunctionCallInfo fcinfo)
{
	params->query_id = PG_GETARG_INT64(0);
	params->diagnostics_interval_sec = PG_GETARG_INT64(1);
	params->explain_sample_rate = PG_GETARG_INT64(2);
	params->explain_analyze = PG_GETARG_BOOL(3);
	params->explain_dist = PG_GETARG_BOOL(4);
	params->explain_debug = PG_GETARG_BOOL(5);
	params->bind_var_query_min_duration_ms = PG_GETARG_INT64(6);

	if (params->query_id == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("there cannot be a query with query_id 0")));

	if (params->diagnostics_interval_sec <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("diagnostics_interval_sec should be greater than 0")));

	if (params->explain_sample_rate < 0 || params->explain_sample_rate > 100)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("explain_sample_rate should be between 0 and 100")));

	if (params->bind_var_query_min_duration_ms < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bind_var_query_min_duration_ms cannot be less than 0")));
}

/*
 * yb_query_diagnostics
 *		Enable query diagnostics for the given query ID.
 *	returns:
 * 		path to the diagnostics bundle is returned if the diagnostics started successfully,
 *		otherwise raises an ereport(ERROR).
 */
Datum
yb_query_diagnostics(PG_FUNCTION_ARGS)
{
	if (!YBIsQueryDiagnosticsEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("query diagnostics is not enabled"),
				 errhint("set TEST_yb_enable_query_diagnostics gflag to true")));

	YbQueryDiagnosticsMetadata metadata;
	metadata.start_time = GetCurrentTimestamp();

	FetchParams(&metadata.params, fcinfo);

	ConstructDiagnosticsPath(&metadata);

	InsertNewBundleInfo(&metadata);

	PG_RETURN_TEXT_P(cstring_to_text(metadata.path));
}
