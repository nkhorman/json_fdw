/*-------------------------------------------------------------------------
 *
 * json_fdw.c
 *
 * Function definitions for JSON foreign data wrapper.
 *
 * Copyright (c) 2013, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdbool.h>

#include "postgres.h"
#include "json_fdw.h"

#include <sys/stat.h>
#include <yajl/yajl_tree.h>
#include <zlib.h>

#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "port.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/int8.h"
#include "utils/timestamp.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#if PG_VERSION_NUM >= 90300
	#include "access/htup_details.h"
#endif

#include "curlapi.h"

/* Local functions forward declarations */
static StringInfo OptionNamesString(Oid currentContextId);
static void JsonGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
								  Oid foreignTableId);
static void JsonGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
								Oid foreignTableId);
static ForeignScan * JsonGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
				   						Oid foreignTableId, ForeignPath *bestPath,
				   						List *targetList, List *scanClauses);
static void JsonExplainForeignScan(ForeignScanState *scanState, 
								   ExplainState *explainState);
static void JsonBeginForeignScan(ForeignScanState *scanState, int executorFlags);
static TupleTableSlot * JsonIterateForeignScan(ForeignScanState *scanState);
static void JsonReScanForeignScan(ForeignScanState *scanState);
static void JsonEndForeignScan(ForeignScanState *scanState);
static JsonFdwOptions * JsonGetOptions(Oid foreignTableId);
static char * JsonGetOptionValue(Oid foreignTableId, const char *optionName);
static double TupleCount(RelOptInfo *baserel, const char *filename);
static BlockNumber PageCount(const char *filename);
static List * ColumnList(RelOptInfo *baserel);
static HTAB * ColumnMappingHash(Oid foreignTableId, List *columnList);
static bool GzipFilename(const char *filename);
static bool HdfsBlockName(const char *filename);
static StringInfo ReadLineFromFile(FILE *filePointer);
static StringInfo ReadLineFromGzipFile(gzFile gzFilePointer);
static void FillTupleSlot(const yajl_val jsonObject, const char *jsonObjectKey,
						  HTAB *columnMappingHash, Datum *columnValues,
						  bool *columnNulls);
static bool ColumnTypesCompatible(yajl_val jsonValue, Oid columnTypeId);
static bool ValidDateTimeFormat(const char *dateTimeString);
static Datum ColumnValueArray(yajl_val jsonArray, Oid valueTypeId, Oid valueTypeMod);
static Datum ColumnValue(yajl_val jsonValue, Oid columnTypeId, int32 columnTypeMod);
static bool JsonAnalyzeForeignTable(Relation relation,
									AcquireSampleRowsFunc *acquireSampleRowsFunc,
									BlockNumber *totalPageCount);
static int JsonAcquireSampleRows(Relation relation, int logLevel,
								 HeapTuple *sampleRows, int targetRowCount,
								 double *totalRowCount, double *totalDeadRowCount);


/* Declarations for dynamic loading */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(json_fdw_handler);
PG_FUNCTION_INFO_V1(json_fdw_validator);


/*
 * json_fdw_handler creates and returns a struct with pointers to foreign table
 * callback functions.
 */
Datum
json_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

	fdwRoutine->GetForeignRelSize = JsonGetForeignRelSize;
	fdwRoutine->GetForeignPaths = JsonGetForeignPaths;
	fdwRoutine->GetForeignPlan = JsonGetForeignPlan;
	fdwRoutine->ExplainForeignScan = JsonExplainForeignScan;
	fdwRoutine->BeginForeignScan = JsonBeginForeignScan;
	fdwRoutine->IterateForeignScan = JsonIterateForeignScan;
	fdwRoutine->ReScanForeignScan = JsonReScanForeignScan;
	fdwRoutine->EndForeignScan = JsonEndForeignScan;
	fdwRoutine->AnalyzeForeignTable = JsonAnalyzeForeignTable;

	PG_RETURN_POINTER(fdwRoutine);
}


/*
 * json_fdw_validator validates options given to one of the following commands:
 * foreign data wrapper, server, user mapping, or foreign table. This function
 * errors out if the given option name or its value is considered invalid. The
 * filename option is required by the foreign table, so we error out if it is
 * not provided.
 */
Datum
json_fdw_validator(PG_FUNCTION_ARGS)
{
	Datum optionArray = PG_GETARG_DATUM(0);
	Oid optionContextId = PG_GETARG_OID(1);
	List *optionList = untransformRelOptions(optionArray);
	ListCell *optionCell = NULL;
	bool filenameFound = false;

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionName = optionDef->defname;
		bool optionValid = false;

		int32 optionIndex = 0;
		for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
		{
			const JsonValidOption *validOption = &(ValidOptionArray[optionIndex]);

			if ((optionContextId == validOption->optionContextId) &&
				(strncmp(optionName, validOption->optionName, NAMEDATALEN) == 0))
			{
				optionValid = true;
				break;
			}
		}

		/* if invalid option, display an informative error message */
		if (!optionValid)
		{
			StringInfo optionNamesString = OptionNamesString(optionContextId);

			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("invalid option \"%s\"", optionName),
							errhint("Valid options in this context are: %s",
									optionNamesString->data)));
		}

		if (strncmp(optionName, OPTION_NAME_FILENAME, NAMEDATALEN) == 0)
		{
			filenameFound = true;
		}
	}

	if (optionContextId == ForeignTableRelationId)
	{
		if (!filenameFound)
		{
			ereport(ERROR, (errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
			 		errmsg("filename is required for json_fdw foreign tables")));
		}
	}

	PG_RETURN_VOID();
}


/*
 * OptionNamesString finds all options that are valid for the current context,
 * and concatenates these option names in a comma separated string. The function
 * is unchanged from mongo_fdw.
 */
static StringInfo
OptionNamesString(Oid currentContextId)
{
	StringInfo optionNamesString = makeStringInfo();
	bool firstOptionAppended = false;

	int32 optionIndex = 0;
	for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
	{
		const JsonValidOption *validOption = &(ValidOptionArray[optionIndex]);

		/* if option belongs to current context, append option name */
		if (currentContextId == validOption->optionContextId)
		{
			if (firstOptionAppended)
			{
				appendStringInfoString(optionNamesString, ", ");
			}

			appendStringInfoString(optionNamesString, validOption->optionName);
			firstOptionAppended = true;
		}
	}

	return optionNamesString;
}


/*
 * JsonGetForeignRelSize obtains relation size estimates for a foreign table and
 * puts its estimate for row count into baserel->rows.
 */
static void
JsonGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId)
{
	JsonFdwOptions *options = JsonGetOptions(foreignTableId);

	double tupleCount = TupleCount(baserel, options->filename);
	double rowSelectivity = clauselist_selectivity(root, baserel->baserestrictinfo,
												   0, JOIN_INNER, NULL);

	double outputRowCount = clamp_row_est(tupleCount * rowSelectivity);
	baserel->rows = outputRowCount;
}


/*
 * JsonGetForeignPaths creates possible access paths for a scan on the foreign
 * table. Currently we only have one possible access path, which simply returns
 * all records in the order they appear in the underlying file.
 */
static void
JsonGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId)
{
	Path *foreignScanPath = NULL;
	JsonFdwOptions *options = JsonGetOptions(foreignTableId);

	BlockNumber pageCount = PageCount(options->filename);
	double tupleCount = TupleCount(baserel, options->filename);

	/*
	 * We estimate costs almost the same way as cost_seqscan(), thus assuming
	 * that I/O costs are equivalent to a regular table file of the same size.
	 * However, we take per-tuple CPU costs as 10x of a seqscan to account for
	 * the cost of parsing records.
	 */
	double tupleParseCost = cpu_tuple_cost * JSON_TUPLE_COST_MULTIPLIER;
	double tupleFilterCost = baserel->baserestrictcost.per_tuple;
	double cpuCostPerTuple = tupleParseCost + tupleFilterCost;
	double executionCost = (seq_page_cost * pageCount) + (cpuCostPerTuple * tupleCount);

	double startupCost = baserel->baserestrictcost.startup;
	double totalCost  = startupCost + executionCost;

	/* create a foreign path node and add it as the only possible path */
	foreignScanPath = (Path *) create_foreignscan_path(root, baserel, baserel->rows,
									 				   startupCost, totalCost,
									 				   NIL,  /* no known ordering */
									 				   NULL, /* not parameterized */
									 				   NIL); /* no fdw_private */

	add_path(baserel, foreignScanPath);
}


/*
 * JsonGetForeignPlan creates a ForeignScan plan node for scanning the foreign
 * table. We also add the query column list to scan nodes private list, because
 * we need it later for mapping columns.
 */
static ForeignScan *
JsonGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId,
				   ForeignPath *bestPath, List *targetList, List *scanClauses)
{
	ForeignScan *foreignScan = NULL;
	List *columnList = NULL;
	List *foreignPrivateList = NIL;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scanClauses into the plan node's qual list for the executor
	 * to check.
	 */
	scanClauses = extract_actual_clauses(scanClauses, false);

	/*
	 * As an optimization, we only add columns that are present in the query to
	 * the column mapping hash. To find these columns, we need baserel. We don't
	 * have access to baserel in executor's callback functions, so we get the
	 * column list here and put it into foreign scan node's private list.
	 */
	columnList = ColumnList(baserel);
	foreignPrivateList = list_make1(columnList);

	/* create the foreign scan node */
	foreignScan = make_foreignscan(targetList, scanClauses, baserel->relid, 
								   NIL, /* no expressions to evaluate */
								   foreignPrivateList);

	return foreignScan;
}


/* JsonExplainForeignScan produces extra output for the Explain command. */
static void
JsonExplainForeignScan(ForeignScanState *scanState, ExplainState *explainState)
{
	Oid foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	JsonFdwOptions *options = JsonGetOptions(foreignTableId);

	ExplainPropertyText("Json File", options->filename, explainState);
	ExplainPropertyText("HTTP Post Vars", options->pHttpPostVars, explainState);

	/* supress file size if we're not showing cost details */
	if (explainState->costs)
	{
		struct stat statBuffer;

		int statResult = stat(options->filename, &statBuffer);
		if (statResult == 0)
		{
			ExplainPropertyLong("Json File Size", (long) statBuffer.st_size, 
								explainState);
		}
	}
}

/*
 * JsonBeginForeignScan opens the underlying json file for reading. The function
 * also creates a hash table that maps referenced column names to column index
 * and type information.
 */
static void
JsonBeginForeignScan(ForeignScanState *scanState, int executorFlags)
{
	JsonFdwExecState *execState = NULL;
	ForeignScan *foreignScan = NULL;
	List *foreignPrivateList = NULL;
	Oid foreignTableId = InvalidOid;
	JsonFdwOptions *options = NULL;
	List *columnList = NULL;
	HTAB *columnMappingHash = NULL;
	bool gzipFile = false;
	bool hdfsBlock = false;
	FILE *filePointer = NULL;
	gzFile gzFilePointer = NULL;
	bool openError = false;
	const char *filename = NULL;
	ciu_t ciu;
	cfr_t *pCfr = NULL;

	/* if Explain with no Analyze, do nothing */
	if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		return;
	}

	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	options = JsonGetOptions(foreignTableId);

	foreignScan = (ForeignScan *) scanState->ss.ps.plan;
	foreignPrivateList = (List *) foreignScan->fdw_private;

	columnList = (List *) linitial(foreignPrivateList);
	columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

	filename = options->filename;

	// See if this is an off box url, and try to fetch it
	// and then pass it off to one of the native file handlers
	if(curlIsUrl(filename, &ciu))
	{
		pCfr = curlFetch(filename, options->pHttpPostVars, &ciu);
		openError = (pCfr == NULL || !pCfr->bFileFetched);
		if(!openError)
			// replace the url with the on box filename of the file that we just
			// downloaded so that the existing file handlers can just use a file
			filename = pCfr->pFileName;
	}

	if(!openError)
	{
		gzipFile = GzipFilename(filename);
		hdfsBlock = HdfsBlockName(filename);

		if (gzipFile || hdfsBlock)
		{
			gzFilePointer = gzopen(filename, PG_BINARY_R);
			openError = (gzFilePointer == NULL);
		}
		else
		{
			filePointer = AllocateFile(filename, PG_BINARY_R);
			openError = (filePointer == NULL);
		}
	}

	if (openError)
	{
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not open file \"%s\" for reading: %m",
							   options->filename)));
		curlFetchFree(pCfr);
		pCfr = NULL;
	}

	execState = (JsonFdwExecState *) palloc(sizeof(JsonFdwExecState));
	execState->filename = options->filename;
	execState->filePointer = filePointer;
	execState->gzFilePointer = gzFilePointer;
	execState->columnMappingHash = columnMappingHash;
	execState->maxErrorCount = options->maxErrorCount;
	execState->errorCount = 0;
	execState->currentLineNumber = 0;
	// we pass this off to EndForeignScan to manage
	execState->pCfr = pCfr;

	scanState->fdw_state = (void *) execState;
}


/*
 * JsonIterateForeignScan reads the next record from the data file, converts it 
 * to PostgreSQL tuple, and stores the converted tuple into the ScanTupleSlot as
 * a virtual tuple.
 */
static TupleTableSlot *
JsonIterateForeignScan(ForeignScanState *scanState)
{
	JsonFdwExecState *execState = (JsonFdwExecState *) scanState->fdw_state;
	TupleTableSlot *tupleSlot = scanState->ss.ss_ScanTupleSlot;
	HTAB *columnMappingHash = execState->columnMappingHash;
	char errorBuffer[ERROR_BUFFER_SIZE];
	yajl_val jsonValue = NULL;
	bool endOfFile = false;
	bool jsonObjectValid = false;
	bool errorCountExceeded = false;

	TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	Datum *columnValues = tupleSlot->tts_values;
	bool *columnNulls = tupleSlot->tts_isnull;
	int columnCount = tupleDescriptor->natts;

	/* initialize all values for this row to null */
	memset(columnValues, 0, columnCount * sizeof(Datum));
	memset(columnNulls, true, columnCount * sizeof(bool));

	ExecClearTuple(tupleSlot);

	/*
	 * Loop until we reach the end of file, or we read a line that parses to be
	 * a valid json object, or we exceed the maximum allowed error count.
	 */
	while (!(endOfFile || jsonObjectValid || errorCountExceeded))
	{
		StringInfo lineData = NULL;
		if (execState->gzFilePointer != NULL)
		{
			lineData = ReadLineFromGzipFile(execState->gzFilePointer);
		}
		else
		{
			lineData = ReadLineFromFile(execState->filePointer);
		}

		if (lineData->len == 0)
		{
			endOfFile = true;
		}
		else
		{
			execState->currentLineNumber++;

			jsonValue = yajl_tree_parse(lineData->data, errorBuffer, sizeof(errorBuffer));

			jsonObjectValid = YAJL_IS_OBJECT(jsonValue);
			if (!jsonObjectValid)
			{
				yajl_tree_free(jsonValue);

				execState->errorCount++;
			}

			if (execState->errorCount > execState->maxErrorCount)
			{
				errorCountExceeded = true;
			}
		}
	}

	if (jsonObjectValid)
	{
		FillTupleSlot(jsonValue, NULL, columnMappingHash, columnValues, columnNulls);
		ExecStoreVirtualTuple(tupleSlot);

		yajl_tree_free(jsonValue);
	}
	else if (errorCountExceeded)
	{
		ereport(ERROR, (errmsg("could not parse %u json objects", execState->errorCount),
						errhint("Last error message at line: %u: %s",
								execState->currentLineNumber, errorBuffer)));
	}

	return tupleSlot;
}


/* JsonReScanForeignScan rescans the foreign table. */
static void
JsonReScanForeignScan(ForeignScanState *scanState)
{
	JsonEndForeignScan(scanState);
	JsonBeginForeignScan(scanState, 0);
}


/*
 * JsonEndForeignScan finishes scanning the foreign table, and frees the acquired
 * resources.
 */
static void
JsonEndForeignScan(ForeignScanState *scanState)
{
	JsonFdwExecState *executionState = (JsonFdwExecState *) scanState->fdw_state;
	if (executionState == NULL)
	{
		return;
	}

	if (executionState->filePointer != NULL)
	{
		int closeStatus = FreeFile(executionState->filePointer);
		if (closeStatus != 0)
		{
			ereport(ERROR, (errcode_for_file_access(),
					 		errmsg("could not close file \"%s\": %m",
								   executionState->filename)));
		}
	}

	if (executionState->gzFilePointer != NULL)
	{
		int closeStatus = gzclose(executionState->gzFilePointer);
		if (closeStatus != Z_OK)
		{
			ereport(ERROR, (errcode_for_file_access(),
					 		errmsg("could not close file \"%s\": %m",
								   executionState->filename)));
		}
	}

	if (executionState->columnMappingHash != NULL)
	{
		hash_destroy(executionState->columnMappingHash);
	}

	curlFetchFree(executionState->pCfr);

	pfree(executionState);
}


/*
 * JsonGetOptions returns the option values to be used when reading and parsing 
 * the json file. To resolve these values, the function checks options for the
 * foreign table, and if not present, falls back to default values.
 */
static JsonFdwOptions *
JsonGetOptions(Oid foreignTableId)
{
	JsonFdwOptions *jsonFdwOptions = NULL;
	char *filename = NULL;
	int32 maxErrorCount = 0;
	char *maxErrorCountString = NULL;

	filename = JsonGetOptionValue(foreignTableId, OPTION_NAME_FILENAME);

	maxErrorCountString = JsonGetOptionValue(foreignTableId, OPTION_NAME_MAX_ERROR_COUNT);
	if (maxErrorCountString == NULL)
	{
		maxErrorCount = DEFAULT_MAX_ERROR_COUNT;
	}
	else
	{
		maxErrorCount = pg_atoi(maxErrorCountString, sizeof(int32), 0);
	}

	jsonFdwOptions = (JsonFdwOptions *) palloc0(sizeof(JsonFdwOptions));
	jsonFdwOptions->filename = filename;
	jsonFdwOptions->maxErrorCount = maxErrorCount;

	return jsonFdwOptions;
}


/*
 * Json GetOptionValue walks over foreign table and foreign server options, and
 * looks for the option with the given name. If found, the function returns the
 * option's value. This function is unchanged from mongo_fdw.
 */
static char *
JsonGetOptionValue(Oid foreignTableId, const char *optionName)
{
	ForeignTable *foreignTable = NULL;
	ForeignServer *foreignServer = NULL;
	List *optionList = NIL;
	ListCell *optionCell = NULL;
	char *optionValue = NULL;

	foreignTable = GetForeignTable(foreignTableId);
	foreignServer = GetForeignServer(foreignTable->serverid);

	optionList = list_concat(optionList, foreignTable->options);
	optionList = list_concat(optionList, foreignServer->options);

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionDefName = optionDef->defname;

		if (strncmp(optionDefName, optionName, NAMEDATALEN) == 0)
		{
			optionValue = defGetString(optionDef);
			break;
		}
	}

	return optionValue;
}


/* TupleCount estimates the number of base relation tuples in the given file. */
static double
TupleCount(RelOptInfo *baserel, const char *filename)
{
	double tupleCount = 0.0;

	BlockNumber pageCountEstimate = baserel->pages;
	if (pageCountEstimate > 0)
	{
		/*
		 * We have number of pages and number of tuples from pg_class (from a
		 * previous Analyze), so compute a tuples-per-page estimate and scale
		 * that by the current file size.
		 */
		double density = baserel->tuples / (double) pageCountEstimate;
		BlockNumber pageCount = PageCount(filename);

		tupleCount = clamp_row_est(density * (double) pageCount);
	}
	else
	{
		/*
		 * Otherwise we have to fake it. We back into this estimate using the
		 * planner's idea of relation width, which may be inaccurate. For better
		 * estimates, users need to run Analyze.
		 */
		struct stat statBuffer;
		int tupleWidth = 0;

		int statResult = stat(filename, &statBuffer);
		if (statResult < 0)
		{
			/* file may not be there at plan time, so use a default estimate */
			statBuffer.st_size = 10 * BLCKSZ;
		}

		tupleWidth = MAXALIGN(baserel->width) + MAXALIGN(sizeof(HeapTupleHeaderData));
		tupleCount = clamp_row_est((double) statBuffer.st_size / (double) tupleWidth);
	}

	return tupleCount;
}


/* PageCount calculates and returns the number of pages in a file. */
static BlockNumber
PageCount(const char *filename)
{
	BlockNumber pageCount = 0;
	struct stat statBuffer;

	/* if file doesn't exist at plan time, use default estimate for its size */
	int statResult = stat(filename, &statBuffer);
	if (statResult < 0)
	{
		statBuffer.st_size = 10 * BLCKSZ;
	}

	pageCount = (statBuffer.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (pageCount < 1)
	{
		pageCount = 1;
	}

	return pageCount;
}


/*
 * ColumnList takes in the planner's information about this foreign table. The
 * function then finds all columns needed for query execution, including those
 * used in projections, joins, and filter clauses, de-duplicates these columns,
 * and returns them in a new list. This function is unchanged from mongo_fdw. 
 */
static List *
ColumnList(RelOptInfo *baserel)
{
	List *columnList = NIL;
	List *neededColumnList = NIL;
	AttrNumber columnIndex = 1;
	AttrNumber columnCount = baserel->max_attr;
	List *targetColumnList = baserel->reltargetlist;
	List *restrictInfoList = baserel->baserestrictinfo;
	ListCell *restrictInfoCell = NULL;

	/* first add the columns used in joins and projections */
	neededColumnList = list_copy(targetColumnList);

	/* then walk over all restriction clauses, and pull up any used columns */
	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Node *restrictClause = (Node *) restrictInfo->clause;
		List *clauseColumnList = NIL;

		/* recursively pull up any columns used in the restriction clause */
		clauseColumnList = pull_var_clause(restrictClause,
										   PVC_RECURSE_AGGREGATES,
										   PVC_RECURSE_PLACEHOLDERS);

		neededColumnList = list_union(neededColumnList, clauseColumnList);
	}

	/* walk over all column definitions, and de-duplicate column list */
	for (columnIndex = 1; columnIndex <= columnCount; columnIndex++)
	{
		ListCell *neededColumnCell = NULL;
		Var *column = NULL;

		/* look for this column in the needed column list */
		foreach(neededColumnCell, neededColumnList)
		{
			Var *neededColumn = (Var *) lfirst(neededColumnCell);
			if (neededColumn->varattno == columnIndex)
			{
				column = neededColumn;
				break;
			}
		}

		if (column != NULL)
		{
			columnList = lappend(columnList, column);
		}
	}

	return columnList;
}


/*
 * ColumnMappingHash creates a hash table that maps column names to column index
 * and types. This table helps us quickly translate JSON document key/values to
 * corresponding PostgreSQL columns. This function is unchanged from mongo_fdw.
 */
static HTAB *
ColumnMappingHash(Oid foreignTableId, List *columnList)
{
	HTAB *columnMappingHash = NULL;
	ListCell *columnCell = NULL;
	const long hashTableSize = 2048;

	/* create hash table */
	HASHCTL hashInfo;
	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = NAMEDATALEN;
	hashInfo.entrysize = sizeof(ColumnMapping);
	hashInfo.hash = string_hash;
	hashInfo.hcxt = CurrentMemoryContext;

	columnMappingHash = hash_create("Column Mapping Hash", hashTableSize, &hashInfo,
									(HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT));
	Assert(columnMappingHash != NULL);

	foreach(columnCell, columnList)
	{
		Var *column = (Var *) lfirst(columnCell);
		AttrNumber columnId = column->varattno;

		ColumnMapping *columnMapping = NULL;
		char *columnName = NULL;
		bool handleFound = false;
		void *hashKey = NULL;

		columnName = get_relid_attribute_name(foreignTableId, columnId);
		hashKey = (void *) columnName;

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
													  HASH_ENTER, &handleFound);
		Assert(columnMapping != NULL);

		columnMapping->columnIndex = columnId - 1;
		columnMapping->columnTypeId = column->vartype;
		columnMapping->columnTypeMod = column->vartypmod;
		columnMapping->columnArrayTypeId = get_element_type(column->vartype);
	}

	return columnMappingHash;
}


/* GzipFilename returns true if the filename ends with a gzip file extension. */
static bool
GzipFilename(const char *filename)
{
	bool gzipFile = false;
	const char *extension = NULL;

	extension = strrchr(filename, '.');
	if (extension != NULL)
	{
		if (strncmp(extension, GZIP_FILE_EXTENSION, MAXPGPATH) == 0)
		{
			gzipFile = true;
		}
	}

	return gzipFile;
}


/* HdfsBlockName returns true if filename belongs to a hdfs block. */
static bool
HdfsBlockName(const char *filename)
{
	bool hdfsBlock = false;
	const char *basename = NULL;

	const char *lastDirSeparator = last_dir_separator(filename);
	if (lastDirSeparator == NULL)
	{
		basename = filename;
	}
	else
	{
		basename = lastDirSeparator + 1;
	}

	if (strncmp(basename, HDFS_BLOCK_PREFIX, HDFS_BLOCK_PREFIX_LENGTH) == 0)
	{
		hdfsBlock = true;
	}

	return hdfsBlock;
}


/*
 * ReadLineFromFile reads and returns the next line in the file. If the function
 * reaches the end of file without reading input, it returns an empty string.
 */
static StringInfo
ReadLineFromFile(FILE *filePointer)
{
	StringInfo lineData = makeStringInfo();
	bool endOfFile = false;
	bool endOfLine = false;
	char buffer[READ_BUFFER_SIZE];

	/* read from file until either we reach end of file or end of line */
	while (!endOfFile && !endOfLine)
	{
		char *fgetsResult = fgets(buffer, sizeof(buffer), filePointer);
		if (fgetsResult == NULL)
		{
			int errorResult = ferror(filePointer);
			if (errorResult != 0)
			{
				ereport(ERROR, (errcode_for_file_access(),
								errmsg("could not read from json file: %m")));
			}

			endOfFile = true;
		}
		else
		{
			uint32 bufferLength = strlen(buffer);

			/* check if we read a new line */
			char lastCharacter = buffer[bufferLength - 1];
			if (lastCharacter == '\n')
			{
				endOfLine = true;
			}

			appendStringInfoString(lineData, buffer);
		}
	}

	return lineData;
}


/*
 * ReadLineFromFile reads and returns the next line in the file. If the function
 * reaches the end of file without reading input, it returns an empty string.
 */
static StringInfo
ReadLineFromGzipFile(gzFile gzFilePointer)
{
	StringInfo lineData = makeStringInfo();
	bool endOfFile = false;
	bool endOfLine = false;
	char buffer[READ_BUFFER_SIZE];

	/* read from file until either we reach end of file or end of line */
	while (!endOfFile && !endOfLine)
	{
		char *getsResult = gzgets(gzFilePointer, buffer, sizeof(buffer));
		if (getsResult == NULL)
		{
			int errorResult = 0;
			const char *message = gzerror(gzFilePointer, &errorResult);
			if (errorResult != Z_OK && errorResult != Z_STREAM_END)
			{
				ereport(ERROR, (errmsg("could not read from json file"), 
								errhint("%s", message)));
			}

			endOfFile = true;
		}
		else
		{
			uint32 bufferLength = strlen(buffer);

			/* check if we read a new line */
			char lastCharacter = buffer[bufferLength - 1];
			if (lastCharacter == '\n')
			{
				endOfLine = true;
			}

			appendStringInfoString(lineData, buffer);
		}
	}

	return lineData;
}


/*
 * FillTupleSlot walks over all key/value pairs in the given document. For each
 * pair, the function checks if the key appears in the column mapping hash, and
 * if the value type is compatible with the one specified for the column. If so
 * the function converts the value and fills the corresponding tuple position.
 * The jsonObjectKey parameter is used for recursion, and should always be
 * passed as NULL. This function is based on the function with the same name in
 * mongo_fdw.
 */
static void
FillTupleSlot(const yajl_val jsonObject, const char *jsonObjectKey,
			  HTAB *columnMappingHash, Datum *columnValues, bool *columnNulls)
{
	uint32 jsonKeyCount = jsonObject->u.object.len;
	const char **jsonKeyArray = jsonObject->u.object.keys;
	yajl_val *jsonValueArray = jsonObject->u.object.values;
	uint32 jsonKeyIndex = 0;

	/* loop over key/value pairs of the json object */
	for (jsonKeyIndex = 0; jsonKeyIndex < jsonKeyCount; jsonKeyIndex++)
	{
		const char *jsonKey = jsonKeyArray[jsonKeyIndex];
		yajl_val jsonValue = jsonValueArray[jsonKeyIndex];

		ColumnMapping *columnMapping = NULL;
		Oid columnTypeId = InvalidOid;
		Oid columnArrayTypeId = InvalidOid;
		Oid columnTypeMod = InvalidOid;
		bool compatibleTypes = false;
		bool handleFound = false;
		const char *jsonFullKey = NULL;
		void *hashKey = NULL;

		if (jsonObjectKey != NULL)
		{
			/*
			 * For fields in nested json objects, we use fully qualified field
			 * name to check the column mapping.
			 */
			StringInfo jsonFullKeyString = makeStringInfo();
			appendStringInfo(jsonFullKeyString, "%s.%s", jsonObjectKey, jsonKey);
			jsonFullKey = jsonFullKeyString->data;
		}
		else
		{
			jsonFullKey = jsonKey;
		}

		/* recurse into nested objects */
		if (YAJL_IS_OBJECT(jsonValue))
		{
			FillTupleSlot(jsonValue, jsonFullKey, columnMappingHash,
						  columnValues, columnNulls);
			continue;
		}

		/* look up the corresponding column for this json key */
		hashKey = (void *) jsonFullKey;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
													  HASH_FIND, &handleFound);

		/* if no corresponding column or null json value, continue */
		if (columnMapping == NULL || YAJL_IS_NULL(jsonValue))
		{
			continue;
		}

		/* check if columns have compatible types */
		columnTypeId = columnMapping->columnTypeId;
		columnArrayTypeId = columnMapping->columnArrayTypeId;
		columnTypeMod = columnMapping->columnTypeMod;

		if (OidIsValid(columnArrayTypeId))
		{
			compatibleTypes = YAJL_IS_ARRAY(jsonValue);
		}
		else
		{
			compatibleTypes = ColumnTypesCompatible(jsonValue, columnTypeId);
		}

		/* if types are incompatible, leave this column null */
		if (!compatibleTypes)
		{
			continue;
		}

		/* fill in corresponding column value and null flag */
		if (OidIsValid(columnArrayTypeId))
		{
			uint32 columnIndex = columnMapping->columnIndex;
			columnValues[columnIndex] = ColumnValueArray(jsonValue, columnArrayTypeId,
														 columnTypeMod);
			columnNulls[columnIndex] = false;
		}
		else
		{
			uint32 columnIndex = columnMapping->columnIndex;
			columnValues[columnIndex] = ColumnValue(jsonValue, columnTypeId,
													columnTypeMod);
			columnNulls[columnIndex] = false;
		}
	}
}


/*
 * ColumnTypesCompatible checks if the given json value can be converted to the
 * given PostgreSQL type.
 */
static bool
ColumnTypesCompatible(yajl_val jsonValue, Oid columnTypeId)
{
	bool compatibleTypes = false;

	/* we consider the PostgreSQL column type as authoritative */
	switch(columnTypeId)
	{
		case INT2OID: case INT4OID:
		case INT8OID: case FLOAT4OID:
		case FLOAT8OID: case NUMERICOID:
		{
			if (YAJL_IS_NUMBER(jsonValue))
			{
				compatibleTypes = true;
			}
			break;
		}
		case BOOLOID:
		{
			if (YAJL_IS_TRUE(jsonValue) || YAJL_IS_FALSE(jsonValue))
			{
				compatibleTypes = true;
			}
			break;
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			if (YAJL_IS_STRING(jsonValue))
			{
				compatibleTypes = true;
			}
			break;
		}
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			if (YAJL_IS_STRING(jsonValue))
			{
				const char *stringValue = (char *) YAJL_GET_STRING(jsonValue);

				bool validDateTimeFormat = ValidDateTimeFormat(stringValue);
				if (validDateTimeFormat)
				{
					compatibleTypes = true;
				}
			}
			break;
		}
		default:
		{
			/*
			 * We currently error out on other data types. Some types such as
			 * byte arrays are easy to add, but they need testing. Other types
			 * such as money or inet, do not have equivalents in JSON.
			 */
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert json type to column type"),
							errhint("column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return compatibleTypes;
}


/*
 * ValidDateTimeFormat checks if the given dateTimeString can be parsed and decoded
 * as a date/timestamp. The algorithm used here is based on date_in, timestamp_in,
 * and timestamptz_in functions.
 */
static bool
ValidDateTimeFormat(const char *dateTimeString)
{
	bool validDateTimeFormat = false;
	char workBuffer[MAXDATELEN + 1];
	char *fieldArray[MAXDATEFIELDS];
	int fieldTypeArray[MAXDATEFIELDS];
	int fieldCount = 0;

	int parseError = ParseDateTime(dateTimeString, workBuffer, sizeof(workBuffer),
								   fieldArray, fieldTypeArray, MAXDATEFIELDS, 
								   &fieldCount);
	if (parseError == 0)
	{
		int dateType = 0;
		struct pg_tm dateTime;
		fsec_t fractionalSecond = 0;
		int timezone = 0;

		int decodeError = DecodeDateTime(fieldArray, fieldTypeArray, fieldCount,
										 &dateType, &dateTime, &fractionalSecond,
										 &timezone);
		if (decodeError == 0)
		{
			/* 
			 * We only accept DTK_DATE, DTK_EPOCH, DTK_LATE, and DTK_EARLY date
			 * types. For other date types, input functions raise an error.
			 */
			if (dateType == DTK_DATE || dateType == DTK_EPOCH ||
				dateType == DTK_LATE || dateType == DTK_EARLY)
			{
				validDateTimeFormat = true;
			}
		}
	}

	return validDateTimeFormat;
}


/*
 * ColumnValueArray uses array element type id to read the current array pointed
 * to by the jsonArray, and converts each array element with matching type to 
 * the corresponding PostgreSQL datum. Then, the function constructs an array
 * datum from element datums, and returns the array datum. This function ignores
 * values that aren't type compatible with valueTypeId.
 */
static Datum
ColumnValueArray(yajl_val jsonArray, Oid valueTypeId, Oid valueTypeMod)
{
	Datum columnValueDatum = 0;
	ArrayType *columnValueObject = NULL;
	bool typeByValue = false;
	char typeAlignment = 0;
	int16 typeLength = 0;

	uint32 jsonValueCount = jsonArray->u.array.len;
	yajl_val *jsonValueArray = jsonArray->u.array.values;

	/* allocate enough room for datum array's maximum possible size */
	Datum *datumArray = palloc0(jsonValueCount * sizeof(Datum));
	uint32 datumArraySize = 0;

	uint32 jsonValueIndex = 0;
	for (jsonValueIndex = 0; jsonValueIndex < jsonValueCount; jsonValueIndex++)
	{
		yajl_val jsonValue = jsonValueArray[jsonValueIndex];

		bool compatibleTypes = ColumnTypesCompatible(jsonValue, valueTypeId);
		if (compatibleTypes)
		{
			datumArray[datumArraySize] = ColumnValue(jsonValue, valueTypeId,
													 valueTypeMod);
			datumArraySize++;
		}
	}

	get_typlenbyvalalign(valueTypeId, &typeLength, &typeByValue, &typeAlignment);
	columnValueObject = construct_array(datumArray, datumArraySize, valueTypeId,
										typeLength, typeByValue, typeAlignment);

	columnValueDatum = PointerGetDatum(columnValueObject);
	return columnValueDatum;
}


/*
 * ColumnValue uses column type information to read the current value pointed to
 * by jsonValue, and converts this value to the corresponding PostgreSQL datum.
 * The function then returns this datum.
 */
static Datum
ColumnValue(yajl_val jsonValue, Oid columnTypeId, int32 columnTypeMod)
{
	Datum columnValue = 0;

	switch(columnTypeId)
	{
		case INT2OID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall1(int2in, CStringGetDatum(value));
			break;
		}
		case INT4OID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall1(int4in, CStringGetDatum(value));
			break;
		}
		case INT8OID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall1(int8in, CStringGetDatum(value));
			break;
		}
		case FLOAT4OID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall1(float4in, CStringGetDatum(value));
			break;
		}
		case FLOAT8OID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall1(float8in, CStringGetDatum(value));
			break;
		}
		case NUMERICOID:
		{
			const char *value = YAJL_GET_NUMBER(jsonValue);
			columnValue = DirectFunctionCall3(numeric_in, CStringGetDatum(value),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case BOOLOID:
		{
			bool value = YAJL_IS_TRUE(jsonValue);
			columnValue = BoolGetDatum(value);
			break;
		}
		case BPCHAROID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = DirectFunctionCall3(bpcharin, CStringGetDatum(value),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case VARCHAROID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = DirectFunctionCall3(varcharin, CStringGetDatum(value),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case TEXTOID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = CStringGetTextDatum(value);
			break;
		}
		case DATEOID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = DirectFunctionCall1(date_in, CStringGetDatum(value));
			break;
		}
		case TIMESTAMPOID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = DirectFunctionCall3(timestamp_in, CStringGetDatum(value),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case TIMESTAMPTZOID:
		{
			const char *value = YAJL_GET_STRING(jsonValue);
			columnValue = DirectFunctionCall3(timestamptz_in, CStringGetDatum(value),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert json type to column type"),
							errhint("column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return columnValue;
}


/*
 * JsonAnalyzeForeignTable sets the total page count and the function pointer
 * used to acquire a random sample of rows from the foreign file.
 */
static bool
JsonAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *acquireSampleRowsFunc,
						BlockNumber *totalPageCount)
{
	Oid foreignTableId = RelationGetRelid(relation);
	JsonFdwOptions *options = JsonGetOptions(foreignTableId);
	BlockNumber pageCount = 0;
	struct stat statBuffer;

	int statResult = stat(options->filename, &statBuffer);
	if (statResult < 0)
	{
		ereport(ERROR, (errcode_for_file_access(),
				 		errmsg("could not stat file \"%s\": %m",
							   options->filename)));
	}

	/*
	 * Our estimate should return at least 1 so that we can tell later on that
	 * pg_class.relpages is not default.
	 */
	pageCount = (statBuffer.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (pageCount < 1)
	{
		pageCount = 1;
	}

	(*totalPageCount) = pageCount;
	(*acquireSampleRowsFunc) = JsonAcquireSampleRows;

	return true;
}


/*
 * JsonAcquireSampleRows acquires a random sample of rows from the foreign
 * table. Selected rows are returned in the caller allocated sampleRows array,
 * which must have at least target row count entries. The actual number of rows
 * selected is returned as the function result. We also count the number of rows
 * in the collection and return it in total row count. We also always set dead
 * row count to zero.
 *
 * Note that the returned list of rows does not always follow their actual order
 * in the JSON file. Therefore, correlation estimates derived later could be
 * inaccurate, but that's OK. We currently don't use correlation estimates (the
 * planner only pays attention to correlation for index scans).
 */
static int
JsonAcquireSampleRows(Relation relation, int logLevel,
					  HeapTuple *sampleRows, int targetRowCount,
					  double *totalRowCount, double *totalDeadRowCount)
{
	int sampleRowCount = 0;
	double rowCount = 0.0;
	double rowCountToSkip = -1;	/* -1 means not set yet */
	double selectionState = 0;
	MemoryContext oldContext = CurrentMemoryContext;
	MemoryContext tupleContext = NULL;
	Datum *columnValues = NULL;
	bool *columnNulls = NULL;
	TupleTableSlot *scanTupleSlot = NULL;
	List *columnList = NIL;
	List *foreignPrivateList = NULL;
	ForeignScanState *scanState = NULL;
	ForeignScan *foreignScan = NULL;
	char *relationName = NULL;
	int executorFlags = 0;

	TupleDesc tupleDescriptor = RelationGetDescr(relation);
	int columnCount = tupleDescriptor->natts;
	Form_pg_attribute *attributes = tupleDescriptor->attrs;

	/* create list of columns of the relation */
	int columnIndex = 0;
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		Var *column = (Var *) palloc0(sizeof(Var));

		/* only assign required fields for column mapping hash */
		column->varattno = columnIndex + 1;
		column->vartype = attributes[columnIndex]->atttypid;
		column->vartypmod = attributes[columnIndex]->atttypmod;

		columnList = lappend(columnList, column);
	}

	/* setup foreign scan plan node */
	foreignPrivateList = list_make1(columnList);
	foreignScan = makeNode(ForeignScan);
	foreignScan->fdw_private = foreignPrivateList;

	/* set up tuple slot */
	columnValues = (Datum *) palloc0(columnCount * sizeof(Datum));
	columnNulls = (bool *) palloc0(columnCount * sizeof(bool));	
	scanTupleSlot = MakeTupleTableSlot();
	scanTupleSlot->tts_tupleDescriptor = tupleDescriptor;
	scanTupleSlot->tts_values = columnValues;
	scanTupleSlot->tts_isnull = columnNulls;

	/* setup scan state */
	scanState = makeNode(ForeignScanState);
	scanState->ss.ss_currentRelation = relation;
	scanState->ss.ps.plan = (Plan *) foreignScan;
	scanState->ss.ss_ScanTupleSlot = scanTupleSlot;

	JsonBeginForeignScan(scanState, executorFlags);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read and
	 * parse rows from the file using ReadLineFromFile and FillTupleSlot.
	 */
	tupleContext = AllocSetContextCreate(CurrentMemoryContext,
										 "json_fdw temporary context",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	/* prepare for sampling rows */
	selectionState = anl_init_selection_state(targetRowCount);

	for (;;)
	{
		/* check for user-requested abort or sleep */
		vacuum_delay_point();

		memset(columnValues, 0, columnCount * sizeof(Datum));
		memset(columnNulls, true, columnCount * sizeof(bool));

		MemoryContextReset(tupleContext);
		MemoryContextSwitchTo(tupleContext);

		/* read the next record */
		JsonIterateForeignScan(scanState);

		MemoryContextSwitchTo(oldContext);

		/* if there are no more records to read, break */
		if (scanTupleSlot->tts_isempty)
		{
			break;
		}

		/*
		 * The first targetRowCount sample rows are simply copied into the
		 * reservoir. Then we start replacing tuples in the sample until we
		 * reach the end of the relation. This algorithm is from Jeff Vitter's
		 * paper (see more info in commands/analyze.c).
		 */
		if (sampleRowCount < targetRowCount)
		{
			sampleRows[sampleRowCount++] = heap_form_tuple(tupleDescriptor, 
														   columnValues, 
														   columnNulls);
		}
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the "not yet
			 * incremented" value of rowCount as t.
			 */
			if (rowCountToSkip < 0)
			{
				rowCountToSkip = anl_get_next_S(rowCount, targetRowCount, 
												&selectionState);
			}

			if (rowCountToSkip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random.
				 */
				int rowIndex = (int) (targetRowCount * anl_random_fract());
				Assert(rowIndex >= 0);
				Assert(rowIndex < targetRowCount);

				heap_freetuple(sampleRows[rowIndex]);
				sampleRows[rowIndex] = heap_form_tuple(tupleDescriptor,
													   columnValues, columnNulls);
			}

			rowCountToSkip -= 1;
		}

		rowCount += 1;
	}

	/* clean up */
	MemoryContextDelete(tupleContext);
	pfree(columnValues);
	pfree(columnNulls);

	JsonEndForeignScan(scanState);

	/* emit some interesting relation info */
	relationName = RelationGetRelationName(relation);
	ereport(logLevel, (errmsg("\"%s\": file contains %.0f rows; %d rows in sample",
							  relationName, rowCount, sampleRowCount)));

	(*totalRowCount) = rowCount;
	(*totalDeadRowCount) = 0;

	return sampleRowCount;
}
