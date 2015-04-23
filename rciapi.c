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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <yajl/yajl_tree.h>
#include <yajl/yajl_tree_path.h>

// same as in json_fdw.h
#define ERROR_BUFFER_SIZE 1024
#define READ_BUFFER_SIZE 4096

#include "curlapi.h"
#include "regexapi.h"
#include "regexapi_helper.h"
#include "rciapi.h"

static yajl_val romRootFetch(char const *pRomUrl, char const *pRomPath)
{	yajl_val root = NULL;

	if(pRomUrl != NULL && pRomPath != NULL && *pRomUrl && *pRomPath)
	{	cfr_t *pCfr = curlFetchFile(pRomUrl, NULL);

		if(pCfr != NULL && pCfr->bFileFetched)
		{	FILE *fin = fopen(pCfr->ccf.pFileName, "r");

			if(fin != NULL)
			{	char errorBuffer[ERROR_BUFFER_SIZE];

				root = yajl_tree_parse_file(fin, READ_BUFFER_SIZE, NULL, errorBuffer, sizeof(errorBuffer));
				fclose(fin);
			}

			// must be an object with schema 2,
			// else, not the rom we are looking for
			if(
				!YAJL_IS_OBJECT(root)
				 || atoi( ytp_get(root, "romschema", NULL)) != 2
				)
			{
				// free and null, if failure
				yajl_tree_free(root);
				root = NULL;
			}
		}
		curlCfrFree(pCfr);
	}

	return root;
}

void rciFree(rci_t *pRci)
{
	if(pRci != NULL)
	{
		if(pRci->pUrl != NULL)
			free(pRci->pUrl);
		if(pRci->pQuery != NULL)
			free(pRci->pQuery);
		if(pRci->romRoot != NULL)
			yajl_tree_free(pRci->romRoot);
		free(pRci);
	}
}

// strcat but dst is realloc'd to add src
static char *strcatr(char *dst, char const *src)
{
	if(src != NULL && *src)
		dst = strcat(realloc(dst, (dst != NULL ? strlen(dst) : 0) + strlen(src) + 1), src);

	return  dst;
}

// special case strcatr, don't concat "/blah/" and "/" into "/blah//"
static char *strcatrurl(char *dst, char const *src)
{
	// won't be "/blah//" ?
	if( !(dst != NULL && dst[strlen(dst)-1] == '/' && src != NULL && *src == '/' && src[1] == 0))
		dst = strcatr(dst, src); // so concat the two

	return dst;
}

rci_t *rciFetch(char const *pRomUrl, char const *pRomPath, int action)
{	rci_t *pRci = calloc(1, sizeof(rci_t));

	if(pRci != NULL)
	{
		pRci->romRoot = romRootFetch(pRomUrl, pRomPath);
		// the schema has already been validated
		if(pRci->romRoot != NULL && action != RCI_ACTION_NONE)
		{
			char const *pAction = (action == RCI_ACTION_INSERT ? "insert"
				: action == RCI_ACTION_UPDATE ? "update"
				: action == RCI_ACTION_DELETE ? "delete"
				: "select"
				);
			yajl_val rootTable = ytp_get(pRci->romRoot, pRomPath, NULL);
			yajl_val rootQuery;

			pRci->romRootAction = ytp_get(rootTable, pAction, NULL);
			pRci->pMethod = ytp_get(pRci->romRootAction, "method", NULL);
			pRci->pUrl = strcatr(NULL, ytp_GetPath(pRci->romRoot, "$.host"));

			rootQuery = ytp_get(pRci->romRootAction, "query");

			// If no host specified in ROM, use the
			// host specification of the ROM url
			if(pRci->pUrl == NULL || !*pRci->pUrl)
			{	// split the ROM url into pieces
				regexapi_t *pRat = regexapi_url(pRomUrl);

				// use the pieces ?
				if(pRat != NULL)
				{	int regexNSubs = regexapi_nsubs(pRat, 0);

					if(regexNSubs >= 2)
					{
						if(pRci->pUrl != NULL)
							free(pRci->pUrl);
						asprintf(&pRci->pUrl, "%s://%s"
							, regexapi_sub(pRat, 0, 0) // protocol specification
							, regexapi_sub(pRat, 0, 1) // host specification
							);
					}
					regexapi_free(pRat);
				}
			}

			// concat / build the url based on the path selected
			pRci->pUrl = strcatrurl(pRci->pUrl, ytp_GetPath(pRci->romRoot, "$.url"));
			pRci->pUrl = strcatrurl(pRci->pUrl, ytp_get(rootTable, "url", NULL));
			pRci->pUrl = strcatrurl(pRci->pUrl, ytp_GetPath(pRci->romRootAction, "$.url"));

			// use the query array objects to build a set
			// of url named parameters with values ?
			if(YAJL_IS_ARRAY(rootQuery))
			{	int i,q,first=1;
				char const *pStrName;
				char const *pStrValue;

				// each query object
				for(i=0,q=rootQuery->u.array.len; i<q; i++)
				{
					pStrName = ytp_get(rootQuery, i+1, "name", NULL);
					pStrValue = ytp_get(rootQuery, i+1, "value", NULL);
					printf("%s:%d i %u name '%s' value '%s'\n", __func__, __LINE__, i, pStrName, pStrValue);
					if(
						pStrName != NULL && *pStrName
						&& pStrValue != NULL && *pStrValue
						)
					{

						if(first)
						{
							// This supposes that the url as built above this
							// code section, doesn't already have paramenters.
							// TODO - figure out if this has already been done.
							pRci->pUrl = strcatr(pRci->pUrl, "?");
							first = 0;
						}
						else
							pRci->pUrl = strcatr(pRci->pUrl, "&");

						pRci->pUrl = strcatr(pRci->pUrl, pStrName);
						pRci->pUrl = strcatr(pRci->pUrl, "=");
						pRci->pUrl = strcatr(pRci->pUrl, pStrValue);
					}
				}
			}
		}
		else if(pRci->romRoot == NULL)
		{
			rciFree(pRci);
			pRci = NULL;
		}
	}

	return pRci;
}


#ifdef _UNIT_TEST_RCI

void test1(int argc, char **argv)
{
	char const	*pRomUrl = "http://127.0.0.1:9734/files/rom.json";
	char const	*pRomPath = "devicestate";
	rci_t		*pRci = NULL;

	pRci = rciFetch(pRomUrl, pRomPath, RCI_ACTION_SELECT);

	if(pRci != NULL)
		printf("url '%s' method '%s'\n", pRci->pUrl, pRci->pMethod);
	else
		printf("rciFetch failed\n");

	rciFree(pRci);
}

int main(int argc, char **argv)
{
	test1(argc, argv);

	return 0;
}
#endif
