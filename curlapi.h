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

typedef struct _ciu_t
{
	char *diskFileName;
	FILE* diskFile;
	bool bNeedUnlink;
} ciu_t; // "CurlIsUrl_Type"

typedef struct _cf_t
{
	char *pFileName;
	bool bNeedUnlink;
	bool bFileFetched;
} cfr_t; // "CurlFetchResult_Type"

int curlIsUrl(const char *pFname, ciu_t *pCcs);
cfr_t *curlFetch(const char *pUrl, const char *pHttpPostVars, ciu_t *pCiu);
void curlFetchFree(cfr_t *pCfs);

#endif
