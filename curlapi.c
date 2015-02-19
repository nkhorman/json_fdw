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

#include <sys/types.h> // for struct dirent
#include <sys/dir.h> // for struct dirent
#include <openssl/md5.h> // for MD5_xxx foo
#include <pthread.h> // for pthread_self()

#include "curl/curl.h"
#include "curlapi.h"
#include "regexapi.h"

// Where files are downloaded to
#define CURL_BASE_DIR "/tmp"
// Maximum length of on disk tempoarary file names
#define MAXFILENAME 1024

// URL validation support
typedef struct _regexapilist_t
{
	const char *pattern;
	int flags;
	int findCount;
}regexapilist_t;

#define URLSPEC "[a-z0-9][a-z0-9._-]*[.][a-z]{2,}"
#define URISPEC "/.*"

// List of valid URL regexes that CURL supports
static regexapilist_t const regexUrls[] =
{
	{ "http[s]?://(" URLSPEC ")(" URISPEC ")", ( REG_EXTENDED | REG_ICASE ), 2 },
	{ NULL, 0, 0 },
};

// Supported URL regex validation iterator
static regexapi_t *regexapi_exec_list(const char *subject, regexapilist_t const *pRegexList)
{	regexapi_t *pRat = NULL;

	while(pRat == NULL && pRegexList->pattern != NULL)
		pRat = regexapi_exec(subject, pRegexList->pattern, pRegexList->flags, pRegexList->findCount);

	return pRat;
}

// This closes the the file that was just made
static void ciuClose(ciu_t *pCiu)
{
	if(pCiu->diskFile != NULL)
	{
		fflush(pCiu->diskFile);
		fclose(pCiu->diskFile);
		pCiu->diskFile = NULL;
	}
}

// Callback from CURL to write contents to disk
static size_t curlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{	ciu_t *pCiu = (ciu_t *)userp;

	return fwrite(contents, size, nmemb, pCiu->diskFile);
}

// Build a unique string
static char *MD5_Ascii(unsigned char *md, char *dst)
{	static const char hex[]="0123456789abcdef";
	int i;

	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
	{
		dst[i+i] = hex[md[i] >> 4];
		dst[i+i+1] = hex[md[i] & 0x0f];
	}
	dst[i+i] = '\0';

	return dst;
}

// Test if filename is a CURL supported URL and setup the ciu_t if it is
int curlIsUrl(const char *pFname, ciu_t *pCiu)
{	int isCurlUrl = 0;
	regexapi_t *pRat = regexapi_exec_list(pFname, &regexUrls[0]);

	// Assume that we should init the ciu
	memset(pCiu, 0, sizeof(ciu_t));

	// If we found a regex match, then we assume that CURL supports the url
	if(pRat != NULL)
	{
		char tmpfnamebuf[MAXFILENAME];

		memset(tmpfnamebuf, 0, sizeof(tmpfnamebuf));
		sprintf(tmpfnamebuf, "%s/tmpXXXXXXXXXX", CURL_BASE_DIR);

		if(mkstemp(tmpfnamebuf) != -1)
		{
			pCiu->diskFileName = strdup(tmpfnamebuf);
			pCiu->bNeedUnlink = true;
		}
		// Open the file for writing
		pCiu->diskFile = (pCiu->diskFileName != NULL ? fopen(pCiu->diskFileName, "w") : NULL);
		// Unlink it, only if it was marked as tempoary and we did open the file
		pCiu->bNeedUnlink &= (pCiu->diskFile != NULL);

		// If we succeeded in opening a file, the this is a CURL URL
		isCurlUrl = (pCiu->diskFile != NULL);

		// Cleanup the regex
		regexapi_free(pRat);
	}

	return isCurlUrl;
}

// Encode some html form Post data
static char *curlEncodePostData(const char *src)
{	char *dst = (src != NULL ? calloc(1,strlen(src)*3) : NULL);
	char *str = dst;

	if(src != NULL)
	{	const char *hexDigit = "0123456789ABCDEF";
		int eq = 0; // we assume that the first `=' is the kvp separator (ie val=data), so don't encode it.

		while(*src)
		{
			// http://en.wikipedia.org/wiki/Percent-encoding#Character_data plus a few more
			if((eq > 0 && *src == '=') || strchr("\"%-.<>\\^_`{|}~[],:#@?;\r\n", *src))
			{	char c = *src;

				eq += (*src == '=');

				*(dst++) = '%';
				*(dst++) = hexDigit [(c&0xf0)>>4];
				*(dst++) = hexDigit [(c&0x0f)];
			}
			else if(*src == ' ')
				*(dst++) = '+';
			else
				*(dst++) = *src;
			src++;
		}
	}

	return str;
}

// Free the Fetch result structure
void curlFetchFree(cfr_t *pCfr)
{
	if(pCfr != NULL)
	{
		if(pCfr->pFileName != NULL)
		{
			if(pCfr->bNeedUnlink)
				unlink(pCfr->pFileName);
			free(pCfr->pFileName);
		}

		free(pCfr);
	}
}

// Fetch the file from the url
cfr_t *curlFetch(const char *pUrl, const char *pHttpPostVars, ciu_t *pCiu)
{	cfr_t *pCfr = calloc(1,sizeof(cfr_t));

	if(pCfr != NULL)
	{
		CURLcode res;
		CURL *curl_handle = NULL;
		char *pPostStr = curlEncodePostData(pHttpPostVars);

		curl_global_init(CURL_GLOBAL_ALL);
		curl_handle = curl_easy_init();
		curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);

		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curlWriteCallback);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)pCiu);
		curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // TODO - table option ?
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30); // TODO - table option ?

		if(pPostStr != NULL && *pPostStr)
		{
			curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, pPostStr);
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(pPostStr));
		}

		// the file should already be open, get it
		res = curl_easy_perform(curl_handle);

		// all done, cleanup
		curl_global_cleanup();

		// clean up post data
		if(pPostStr != NULL)
			free(pPostStr);

		// give the filename allocated in Ciu to Cfr, it will free it later
		pCfr->pFileName = pCiu->diskFileName;
		// take note if we need to clean up the file later
		pCfr->bNeedUnlink = pCiu->bNeedUnlink;

		// clean up
		ciuClose(pCiu);

		// this means that we communicated with the server
		if(res == CURLE_OK)
		{	unsigned long httpResponseCode = 0;
			char *pContentType = NULL;

			curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpResponseCode);
			curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &pContentType);

			// validattion
			pCfr->bFileFetched = (
				// retrived the file
				httpResponseCode == 200
				// make sure it's the correct content type
#ifndef JSON_CONTENT_TYPE_NONE
				&&
					(
#ifdef JSON_CONTENT_TYPE_NULL
					pContentType == NULL ||
#endif
#ifdef JSON_CONTENT_TYPE_LIBERAL
					(pContentType != NULL && strcasecmp("application/x-javascript", pContentType) == 0) ||
					(pContentType != NULL && strcasecmp("text/javascript", pContentType) == 0) ||
					(pContentType != NULL && strcasecmp("text/x-javascript", pContentType) == 0) ||
					(pContentType != NULL && strcasecmp("text/x-json", pContentType) == 0) ||
					(pContentType != NULL && strcasecmp("text/html", pContentType) == 0) ||
#endif
					(pContentType != NULL && strcasecmp("application/json", pContentType) == 0)
					)
#endif
				);
		}

		curl_easy_cleanup(curl_handle);
	}

	return pCfr;
}

#ifdef _CURL_UNIT_TEST
int main(int argc, char **argv)
{
	ciu_t ciu;
	cfr_t *pCfr = NULL;
	const char *pUrl = (argc >= 2 ? argv[1] : NULL);
	const char *pHttpPostVars = (argc >= 3 ? argv[2] : NULL);
	const char *pFileName = NULL;

	if(argc == 1)
	{
		printf("%s: [url] [optional post vars]\n", argv[0]);
		exit(0);
	}

	if(curlIsUrl(pUrl, &ciu))
	{
		pCfr = curlFetch(pUrl, pHttpPostVars, &ciu);
		if(pCfr != NULL)
			pFileName = pCfr->pFileName;
	}

	printf("'%s' --> '%s' == %s\n", pUrl, pFileName, (pCfr && pCfr->bFileFetched ? "OK" : "FAIL"));
	if(pCfr && pCfr->bFileFetched)
	{	char *pCmd = NULL;

		//asprintf(&pCmd, "ls -la /tmp/; cat %s", pFileName);
		asprintf(&pCmd, "cat %s", pFileName);
		system(pCmd);
		free(pCmd);
	}

	curlFetchFree(pCfr);

	//if(pCfr && pCfr->bFileFetched)
	//	system("ls -la /tmp/");

	return 0;
}
#endif
