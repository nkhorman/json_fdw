/*-------------------------------------------------------------------------
 *
 * json_fdw.h
 *
 * Type and function declarations for JSON foreign data wrapper.
 *
 * Copyright (c) 2013, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_FDW_H
#define JSON_FDW_H

#include "fmgr.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "utils/hsearch.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

#include "curlapi.h"


/* Defines for valid option names and default values */
#define OPTION_NAME_FILENAME "filename"
#define OPTION_NAME_MAX_ERROR_COUNT "max_error_count"
#define DEFAULT_MAX_ERROR_COUNT 0

#define OPTION_NAME_HTTP_POST_VARS "http_post_vars"
#define OPTION_NAME_ROM_URL "rom_url"
#define OPTION_NAME_ROM_PATH "rom_path"

#define JSON_TUPLE_COST_MULTIPLIER 10
#define ERROR_BUFFER_SIZE 1024
#define READ_BUFFER_SIZE 4096
#define GZIP_FILE_EXTENSION ".gz"
#define HDFS_BLOCK_PREFIX "blk_"
#define HDFS_BLOCK_PREFIX_LENGTH 4


/*
 * JsonValidOption keeps an option name and a context. When an option is passed
 * into json_fdw objects (server and foreign table), we compare this option's
 * name and context against those of valid options.
 */
typedef struct JsonValidOption
{
	const char *optionName;
	Oid optionContextId;

} JsonValidOption;


/*
 * JsonFdwOptions holds the option values to be used when reading and parsing
 * the json file. To resolve these values, we first check foreign table's 
 * options, and if not present, we then fall back to the default values 
 * specified above.
 */
typedef struct JsonFdwOptions
{
	char const *filename;
	int32 maxErrorCount;
	char const *pHttpPostVars;
	char const *pRomUrl;
	char const *pRomPath;
} JsonFdwOptions;


/*
 * JsonFdwExecState keeps foreign data wrapper specific execution state that we
 * create and hold onto when executing the query.
 */
typedef struct JsonFdwExecState
{
	char const *filename;		// on disk file name of json content
	FILE *filePointer;		// file pointer to on disk content
	void *gzFilePointer;		// gz file pointe to on disk content

	uint32 maxErrorCount;
	uint32 errorCount;
	uint32 currentLineNumber;
	HTAB *columnMappingHash;

	cfr_t *pCfr;			// curl fetch result
} JsonFdwExecState;

typedef struct _jfmes_t
{
	Relation rel;			// relcache entry for the foriegn table
	int p_nums;			// number of parameters to transmit
	FmgrInfo *p_flinfo;		// output conversion functions for them

	List *retrieved_attrs;		// list of target attribute members
	List *retrieved_names;		// list of target attribute names
	List *table_options;
	char const *pUrl;		// put url

	MemoryContext temp_cxt;		// context for per-tuple temp data

} jfmes_t; // Json Fdw Modify Exec State Type


/*
 * ColumnMapping reprents a hash table entry that maps a column name to column
 * related information. We construct these hash table entries to speed up the
 * conversion from JSON documents to PostgreSQL tuples; and each hash entry maps
 * the column name to the column's tuple index and its type-related information.
 */
typedef struct ColumnMapping
{
	char columnName[NAMEDATALEN];
	uint32 columnIndex;
	Oid columnTypeId;
	int32 columnTypeMod;
	Oid columnArrayTypeId;

} ColumnMapping;


/* Function declarations for foreign data wrapper */
extern Datum json_fdw_handler(PG_FUNCTION_ARGS);
extern Datum json_fdw_validator(PG_FUNCTION_ARGS);


#endif   /* JSON_FDW_H */
