/*--------------------------------------------------------------------*
 *
 * Developed by;
 *	Neal Horman - http://www.wanlink.com
 *	Copyright (c) 2015 Neal Horman. All Rights Reserved
 *
 *	This "source code" is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This "source code" is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this "source code".  If not, see <http://www.gnu.org/licenses/>.
 *
 *	RCSID:  $Id$
 *
 *--------------------------------------------------------------------*/

#ifndef _CURLAPI_H_
#define _CURLAPI_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

// HDR_IDX_xx values must be zero relative, and consecutive
#define HDR_STR_ETAG "ETag: "
#define HDR_STR_LASTMODIFIED "Last-Modified: "
#define HDR_STR_CACHECONTROL "Cache-Control: "

enum
{
HDR_IDX_ETAG,
HDR_IDX_LASTMODIFIED,
HDR_IDX_CACHECONTROL,

HDR_COUNT // must always be last
};

typedef struct _ccf_t
{
	char *pUrlBaseName;
	char *pFileName;
	char *pUrlHash;
	char *pFileNameTmp;
	FILE* pFile;
	bool bNeedUnlink;
	char *pHdrs[HDR_COUNT];
}ccf_t; // CurlCacheFile_Type

typedef struct _cfr_t
{
	ccf_t ccf;
	bool bFileFetched;
	unsigned long httpResponseCode;
	char *pContentType;
	unsigned long queryDuration;
} cfr_t; // "CurlFetchResult_Type"

cfr_t *curlFetchFile(const char *pUrl, const char *pHttpPostVars);
void curlPost(const char *pUrl, const char *pHttpPostVars);
void curlCfrFree(cfr_t *pCfr);

int curlPut(const char *pUrl, const char *pBuffer, size_t bufferSize, const char *pContentType);

#ifdef DEBUG_WLOGIT
void curlLogItSet(void (*pfn)(const char *));
static void curlLogIt(const char *pFmt, ...);
#endif

#endif
