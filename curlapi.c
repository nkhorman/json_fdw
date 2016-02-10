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
#include <fcntl.h>
#include <stdarg.h>

#include <sys/types.h> // for struct dirent
#include <sys/dir.h> // for struct dirent
#include <sys/stat.h> // for mkdir
#include <openssl/md5.h> // for MD5_xxx foo
#include <pthread.h> // for pthread_self()

#include "curl/curl.h"
#include "curlapi.h"
#include "regexapi.h"
#include "gettickcount.h"

// Where files are downloaded to
#define CURL_BASE_DIR "/tmp/json_fdw_cache"
// Maximum length of on disk tempoarary file names
#define MAXFILENAME 1024

#define FREEPTR(a) do { if((a) != NULL) { free((a)); (a) = NULL; }; } while(0)

#ifdef DEBUG_WLOGIT
void (*gcurlLogFn)(const char *) = NULL;

void curlLogItSet(void (*pfn)(const char *))
{
	gcurlLogFn = pfn;
}

static void curlLogIt(const char *pFmt, ...)
{
	if(gcurlLogFn != NULL)
	{	va_list vl;
		char *pStr = NULL;

		va_start(vl, pFmt);
		asprintf(&pStr, pFmt, vl);
		va_end(vl);

		if(pStr != NULL)
		{
			gcurlLogFn(pStr);
			free(pStr);
		}
	}
}
#endif

static const char *hexDigits = "0123456789ABCDEF";

// An MD5 object
typedef struct _cmd5_t
{
	MD5_CTX ctx;
	unsigned char digest[MD5_DIGEST_LENGTH];
	char ascii[(MD5_DIGEST_LENGTH*2)+1];
}cmd5_t; // Curl MD5 Type

// Alloc and Init MD5 object
static cmd5_t *curlMd5Init(void)
{	cmd5_t *pMd5 = calloc(1, sizeof(cmd5_t));

	if(pMd5 != NULL)
		MD5_Init(&pMd5->ctx);

	return pMd5;
}

// Free an MD5 object
static void curlMd5Free(cmd5_t *pMd5)
{
	if(pMd5 != NULL)
		free(pMd5);
}

static void curlMd5Hash(cmd5_t *pMd5, const char *pStr)
{
	MD5_Update(&pMd5->ctx, (const unsigned char *)pStr, strlen(pStr));
}

// Finalize the MD5 object, and build an ASCII string
// of the digest, then strdup it
static char *curlMd5Final(cmd5_t *pMd5)
{	int i;

	MD5_Final(pMd5->digest, &pMd5->ctx);

	// Convert MD5 digest into ASCII string
	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
	{
		pMd5->ascii[i+i] = hexDigits[pMd5->digest[i] >> 4];
		pMd5->ascii[i+i+1] = hexDigits[pMd5->digest[i] & 0x0f];
	}

	return strdup(pMd5->ascii);
}

/*
// Create an MD5 hash vprintf style
// The caller must free the resultant string pointer
static char *curlAsciiMd5HashV(const char *pFmt, va_list vl)
{	char *pHashStr = NULL;
	char *pSrc = NULL;

	vasprintf(&pSrc, pFmt, vl);

	// hash it
	if(pSrc != NULL)
	{	cmd5_t *pMd5 = curlMd5Init();

		if(pMd5 != NULL)
		{
			curlMd5Hash(pMd5, pSrc);
			pHashStr = curlMd5Final(pMd5);
			curlMd5Free(pMd5);
		}
		free(pSrc);
	}

	return pHashStr;
}

// Create an MD5 hash printf style
// The caller must free the resultant string pointer
static char *curlAsciiMd5Hash(const char *pFmt, ...)
{	va_list vl;
	char *pStr = NULL;

	va_start(vl, pFmt);
	pStr = curlAsciiMd5HashV(pFmt, vl);
	va_end(vl);

	return pStr;
}
*/

// URL validation support
typedef struct _regexapilist_t
{
	const char *pattern;
	int flags;
	int findCount;
}regexapilist_t;

#define URLHOSTNAME "([a-z0-9][a-z0-9._-]*[.][a-z]{2,})"
#define URLHOSTIPV4 "([0-9]{1,3}[.][0-9]{1,3}[.][0.9]{1,3}[.][0-9]{1,3})"
#define URLHOSTLOCAL "(localhost)"
#define URLHOST "(" URLHOSTNAME "|" URLHOSTLOCAL "|" URLHOSTIPV4 ")"
#define URLPORT "(:[0-9]+)*"
#define URLSPEC URLHOST URLPORT
#define URISPEC "/.*"

// http[s]?://([a-z0-9][a-z0-9._-]*[.][a-z]{2,}(:[0-9]+)*)(.*)
// http[s]?://((([a-z0-9][a-z0-9._-]*[.][a-z]{2,})|(localhost)|([0-9]{1,3}[.][0-9]{1,3}[.][0.9]{1,3}[.][0-9]{1,3}))(:[0-9]+)*)(/.{0,})

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

// Callback from CURL to write contents to disk
static size_t curlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{	ccf_t *pCcf = (ccf_t *)userp;

	return fwrite(contents, size, nmemb, pCcf->pFile);
}

// If pHdr matches the first of pSrc, then duplicate the balance of the header
// The caller must free the result
static char *curlHeaderCallbackMatch(const char *pSrc, size_t srcLen, const char *pHdr)
{	char *pDst = NULL;
	size_t hdrLen = strlen(pHdr);

	// capture the etag header value
	if(srcLen > hdrLen && strncasecmp(pSrc, pHdr, hdrLen) == 0)
	{	const char *pl = pSrc + hdrLen;
		const char *pr = pSrc + srcLen - 1;

		// left trim
		while(*pl == ' ' || *pl == '\t')
			pl++;
		// right trim
		while(*pl == ' ' || *pl == '\t' || *pr == '\n' || *pr == '\r')
			pr--;

		/*
		// remove lead / trailing quote pair
		if(*pl == '"' && *pr == '"')
			{ pl++; pr--; }
		if(*pl == '\'' && *pr == '\'')
			{ pl++; pr--; }
		*/

		if(pr>pl)
		{	int l = pr-pl+1;

			asprintf(&pDst, "%*.*s", l, l, pl);
		}
	}

	return pDst;
}

// Callback from CURL for header examination
// Collect header values that we are interested in
static size_t curlHeaderCallback(void *contents, size_t size, size_t nmemb, void *userp)
{	cfr_t *pCfr = (cfr_t *)userp;
	size_t len = size * nmemb;

	if(pCfr != NULL)
	{	char *pHdrVal = NULL;
		ccf_t *pCcf = &pCfr->ccf;
		int i;
		struct hdra_t
		{
			const char *str;
			size_t idx;
		} pHdrs[] =
		{
			// Order in this array doesn't matter,
			// but the number of elements in this
			// array must be no more than HDR_COUNT
			{HDR_STR_ETAG, HDR_IDX_ETAG},
			{HDR_STR_LASTMODIFIED, HDR_IDX_LASTMODIFIED},
			{HDR_STR_CACHECONTROL, HDR_IDX_CACHECONTROL}
		};

		// Search the array of header keys, find the one that matches what
		// was just passed into us in contents, and, if not already set to
		// non-null, store the duplicated header value
		for(i=0; pHdrVal == NULL && i < sizeof(pHdrs)/sizeof(pHdrs[0]); i++)
		{
			// pHdrVal is already strdup'd for us
			pHdrVal = curlHeaderCallbackMatch((const char *)contents, len, pHdrs[i].str);

			if(pHdrVal != NULL)
			{
				FREEPTR(pCcf->pHdrs[pHdrs[i].idx]);
				pCcf->pHdrs[pHdrs[i].idx] = pHdrVal;
			}
		}
	}

	return len;
}

// Create a temporary file possibly to write into,
// if we receive content from the fetch operation
// Also, figure out what filename we should use for
// content caching purposes.
static void curlCacheFileOpen(ccf_t *pCcf)
{	int fd = -1;
	char tmpfnamebuf[MAXFILENAME];

	// make sure we can store our files
	mkdir(CURL_BASE_DIR, 0755);

	// create a temporary file, for a possible use later
	memset(tmpfnamebuf, 0, sizeof(tmpfnamebuf));
	sprintf(tmpfnamebuf, "%s/tmpXXXXXXXXXX", CURL_BASE_DIR);

	if((fd = mkstemp(tmpfnamebuf)) != -1)
	{
		pCcf->pFileNameTmp = strdup(tmpfnamebuf);
		pCcf->bNeedUnlink = true;

		// Get a FILE pointer
		pCcf->pFile = (fd != -1 ? fdopen(fd, "w") : NULL);
	}

	// Figure out what the on disk filename should be after the retrieval
	if(pCcf->pUrlBaseName == NULL || !*pCcf->pUrlBaseName)
	{
		FREEPTR(pCcf->pUrlBaseName);
		FREEPTR(pCcf->pFileName);

		// The URL didn't specify a file, use the urlhash as the filename
		asprintf(&pCcf->pFileName, "%s/%s", CURL_BASE_DIR, pCcf->pUrlHash);
	}
	else	// Use the specified basename of the filename from the URL
		// so that file handling semantics based on filenames work
		asprintf(&pCcf->pFileName, "%s/%s", CURL_BASE_DIR, pCcf->pUrlBaseName);
}

// Test if pUrl is a CURL supported URL
// If so, grab the basename, for use later
static bool curlIsUrl(const char *pUrl, ccf_t *pCcf)
{	bool bIsUrl = false;
	regexapi_t *pRat = regexapi_exec_list(pUrl, &regexUrls[0]);

	// If we found a regex match, then we assume that CURL supports the url
	if(pRat != NULL)
	{	int regexNSubs = regexapi_nsubs(pRat, 0);
		// Assume that the last subcomponent of the regex is the filename portion
		const char *pRegexSub = (regexNSubs > 1 ? regexapi_sub(pRat, 0, regexNSubs - 1) : NULL);
		// and get the basename of that
		char *pBaseName = (pRegexSub != NULL ? strrchr(pRegexSub, '/') : NULL);

		bIsUrl = (pBaseName != NULL && *pBaseName);
		if(bIsUrl)
		{	char *pTerm = strchr(pBaseName, '?');

			// The string returned to us is not const, so we'll terminate it
			// at the URI point, so as to not have silly basenames
			if(pTerm != NULL)
				*pTerm = 0;

			// no basename, just a plain url ?
			if(*pBaseName == '/')
				pBaseName++;

			if(*pBaseName)
				pCcf->pUrlBaseName = strdup(pBaseName);
		}

		// Cleanup the regex
		regexapi_free(pRat);
	}

	return bIsUrl;
}

// Encode some html form Post data
static char *curlEncodePostData(const char *src)
{	char *dst = (src != NULL ? calloc(1,strlen(src)*3) : NULL);
	char *str = dst;

	if(src != NULL)
	{	int eq = 0; // we assume that the first `=' is the kvp separator (ie val=data), so don't encode it.

		while(*src)
		{
			// http://en.wikipedia.org/wiki/Percent-encoding#Character_data plus a few more
			if((eq > 0 && *src == '=') || strchr("\"%-.<>\\^_`{|}~[],:#@?;\r\n", *src))
			{	char c = *src;

				eq += (*src == '=');

				*(dst++) = '%';
				*(dst++) = hexDigits[c >> 4];
				*(dst++) = hexDigits[c & 0x0f];
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

static void curlCfrClose(cfr_t *pCfr)
{
	if(pCfr != NULL)
	{
		if(pCfr->ccf.pFile != NULL)
		{
			fflush(pCfr->ccf.pFile);
			fclose(pCfr->ccf.pFile);
			pCfr->ccf.pFile = NULL;
		}
	}
}

// Free the structure and sub-components
void curlCfrFree(cfr_t *pCfr)
{
	if(pCfr != NULL)
	{	int i;

		curlCfrClose(pCfr);

		if(pCfr->ccf.pFileNameTmp != NULL)
		{
			if(pCfr->ccf.bNeedUnlink)
				unlink(pCfr->ccf.pFileNameTmp);
		}

		FREEPTR(pCfr->ccf.pUrlBaseName);
		FREEPTR(pCfr->ccf.pFileName);
		FREEPTR(pCfr->ccf.pUrlHash);
		FREEPTR(pCfr->ccf.pFileNameTmp);

		for(i=0; i<HDR_COUNT; i++)
			FREEPTR(pCfr->ccf.pHdrs[i]);

		FREEPTR(pCfr->pContentType);

		free(pCfr);
	}
}

// Build an md5 hash for the URL being requested
// The caller must free the result
static char *curlUrlHash(const char *pUrl, const char *pHttpPostVars)
{	cmd5_t * pMd5 = curlMd5Init();
	char *pUrlHash = NULL;

	curlMd5Hash(pMd5, pUrl);
	if(pHttpPostVars != NULL)
		curlMd5Hash(pMd5, pHttpPostVars);
	pUrlHash = curlMd5Final(pMd5);
	curlMd5Free(pMd5);

	return pUrlHash;
}

/*
static ccf_t *curlCacheMetaSet(const char *pFileName
	, const char *pEtag
	, const char *pLastModified
	, const char *pCacheControl
	)
{	ccf_t *pCcf = calloc(1, sizeof(ccf_t));

	if(pCcf != NULL)
	{
		pCcf->pFileName = strdup(pFileName);
		pCcf->pHdrs[HDR_IDX_ETAG] = strdup(pEtag);
		pCcf->pHdrs[HDR_IDX_LASTMODIFIED] = strdup(pLastModified);
		pCcf->pHdrs[HDR_IDX_CACHECONTROL] = strdup(pCacheControl);
	}

	return pCcf;
}
*/

static char *stradvtok(char **ppSrc, char delim)
{
	char *dst = *ppSrc;
	char *src = *ppSrc;

	while(src != NULL && *src && *src != delim)
	{
		if(dst == src && *src != delim && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n'))
			dst++;
		src++;
	}

	if(*src == delim)
	{
		*src = '\0';
		src++;
		while(*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')
			src++;
	}

	*ppSrc = src;

	return dst;
}


static void curlCacheMetaGet(ccf_t *pCcf)
{	char *pFname = NULL;

	asprintf(&pFname, "%s/%s.meta", CURL_BASE_DIR, pCcf->pUrlHash);
	if(pFname != NULL)
	{	FILE *fin = fopen(pFname, "r");

		if(fin != NULL)
		{	char buf[4096];
			char *p1;
			char *p2;
			char *p3;
			char *p4;
			char *pbuf;

			memset(buf, 0, sizeof(buf));
			pbuf = fgets(buf, sizeof(buf)-1, fin);

			if(pbuf != NULL)
			{
				p1 = stradvtok(&pbuf, '|');
				p2 = stradvtok(&pbuf, '|');
				p3 = stradvtok(&pbuf, '|');
				p4 = stradvtok(&pbuf, '|');

				pCcf->pFileName = strdup(p1);
				pCcf->pHdrs[HDR_IDX_ETAG] = strdup(p2);
				pCcf->pHdrs[HDR_IDX_LASTMODIFIED] = strdup(p3);
				pCcf->pHdrs[HDR_IDX_CACHECONTROL] = strdup(p4);
			}

			fclose(fin);
		}

		free(pFname);
	}
}

#define NOTNULLPTR(a) ((a) != NULL ? (a) : "")

static void curlCacheMetaPut(ccf_t *pCcf)
{	char *pFname = NULL;

	asprintf(&pFname, "%s/%s.meta", CURL_BASE_DIR, pCcf->pUrlHash);

	// TODO - lock operation to prevent contention races
	if(pFname != NULL)
	{	FILE *fout = fopen(pFname, "w");

		if(fout != NULL)
		{
			fprintf(fout,"%s|%s|%s|%s|"
				, NOTNULLPTR(pCcf->pFileName)
				, NOTNULLPTR(pCcf->pHdrs[HDR_IDX_ETAG])
				, NOTNULLPTR(pCcf->pHdrs[HDR_IDX_LASTMODIFIED])
				, NOTNULLPTR(pCcf->pHdrs[HDR_IDX_CACHECONTROL])
				);
			fclose(fout);
		}
		free(pFname);
	}
}

// Move the temp file to the cached file ?
static void curlCacheFileFinalize(cfr_t *pCfr)
{
	if(pCfr != NULL)
	{
		// TODO;
		// 	1. set unlink flag based on cache-control
		switch(pCfr->httpResponseCode)
		{
			case 200: // new content, remove old, use new
				// TODO - lock operation to prevent contention races
				unlink(pCfr->ccf.pFileName);
				rename(pCfr->ccf.pFileNameTmp, pCfr->ccf.pFileName);
				pCfr->ccf.bNeedUnlink = false;
				break;
			case 304:	// no new content, remove temp file
			default:
				unlink(pCfr->ccf.pFileNameTmp);
				break;
		}
	}
}

// Fetch the file from the url
cfr_t *curlFetch(const char *pUrl, const char *pHttpPostVars)
{	cfr_t *pCfr = calloc(1,sizeof(cfr_t));

	if(!curlIsUrl(pUrl, &pCfr->ccf))
		FREEPTR(pCfr);

	if(pCfr != NULL)
	{	struct curl_slist *chunk = NULL;
		CURLcode res;
		CURL *curl_handle = NULL;
		char *pPostStr = curlEncodePostData(pHttpPostVars);
		unsigned long queryStart = 0;

		pCfr->ccf.pUrlHash = curlUrlHash(pUrl, pHttpPostVars);
		curlCacheMetaGet(&pCfr->ccf);
		curlCacheFileOpen(&pCfr->ccf);

		curl_global_init(CURL_GLOBAL_ALL);
		curl_handle = curl_easy_init();
		curl_easy_setopt(curl_handle, CURLOPT_URL, pUrl);

		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curlWriteCallback);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&pCfr->ccf);
		curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // TODO - table option ?
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30); // TODO - table option ?

		curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, ""); // turn on builtin supported default content dencoding
		curl_easy_setopt(curl_handle, CURLOPT_TRANSFER_ENCODING, 1L); // turn on transfer decoding

		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // turn on redirection following
		curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5); // for a maximum of 5
		curl_easy_setopt(curl_handle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL); // maintain a post as a post on redirects
		curl_easy_setopt(curl_handle, CURLOPT_AUTOREFERER, 1L); // turn one Refer when redirecting

		curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&pCfr->ccf);
		curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, curlHeaderCallback);

		// TODO - auth foo - possibly some or all of these
		//	CURLOPT_USERPWD or (CURLOPT_USERNAME and CURLOPT_PASSWORD)
		//	CURLOPT_LOGIN_OPTIONS
		//	CURLOPT_PROXYUSERNAME and CURLOPT_PROXYPASSWORD
		//	CURLOPT_HTTPAUTH
		//	CURLOPT_TLSAUTH_USERNAME and CURLOPT_TLSAUTH_PASSWORD
		//	CURLOPT_PROXYAUTH
		//	CURLOPT_SASL_IR
		//	CURLOPT_XOAUTH2_BEARER
		//

		if(pPostStr != NULL && *pPostStr)
		{
			curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, pPostStr);
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(pPostStr));
		}

		// inject etag header request
		// TODO;
		//	1. don't if the actual file is missing, so that we get a new one
		// 	2. if stale acording to cache-control
		if(pCfr->ccf.pHdrs[HDR_IDX_ETAG] != NULL)
		{	char *pHdr = NULL;

			asprintf(&pHdr, "If-None-Match: %s", pCfr->ccf.pHdrs[HDR_IDX_ETAG]);
			chunk = curl_slist_append(chunk, pHdr);
			curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, chunk);
		}

		// the file should already be open, get it
		queryStart = GetTickCount();
		res = curl_easy_perform(curl_handle);
		pCfr->queryDuration = GetTickCount() - queryStart; // how long did the fetch take ?

		// clean up post data
		if(pPostStr != NULL)
			free(pPostStr);

		// close the open file
		curlCfrClose(pCfr);

		// this means that we communicated with the server
		if(res == CURLE_OK)
		{	char *pContentType = NULL;

			curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &pCfr->httpResponseCode);
			curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &pContentType);

			if(pContentType != NULL)
				pCfr->pContentType = strdup(pContentType);

			curlCacheMetaPut(&pCfr->ccf);

			switch(pCfr->httpResponseCode)
			{
				case 200:
					pCfr->bFileFetched = true;
#ifndef JSON_CONTENT_TYPE_NONE
					// make sure it's the correct content type
					pCfr->bFileFetched &= (
#ifdef JSON_CONTENT_TYPE_NULL
						// Highly non-conforming server/application
						pContentType == NULL ||
#endif
#ifdef JSON_CONTENT_TYPE_LIBERAL
						// If your using a badly configured/coded/non-conforming server
						// application, you might get one or more of these mime types
						(pContentType != NULL && strcasecmp("application/x-javascript", pContentType) == 0) ||
						(pContentType != NULL && strcasecmp("text/javascript", pContentType) == 0) ||
						(pContentType != NULL && strcasecmp("text/x-javascript", pContentType) == 0) ||
						(pContentType != NULL && strcasecmp("text/x-json", pContentType) == 0) ||
						(pContentType != NULL && strcasecmp("text/html", pContentType) == 0) ||
#endif
						// The content might be a straight up gzip compressed file
						(pContentType != NULL && strcasecmp("application/x-gzip", pContentType) == 0) ||
						// If it is uncompressed, it should look like this
						(pContentType != NULL && strcasecmp("application/json", pContentType) == 0)
						);
#endif
					break;
				case 304:
					// we lie here, because we already have the file
					pCfr->bFileFetched = true;
					break;
				default:
					break;
			}

			curlCacheFileFinalize(pCfr);
		}

		// all done, cleanup
		curl_easy_cleanup(curl_handle);
		curl_global_cleanup();
		curl_slist_free_all(chunk);
	}

	return pCfr;
}

#ifdef _CURL_UNIT_TEST
int main(int argc, char **argv)
{
	cfr_t *pCfr = NULL;
	const char *pUrl = NULL;
	const char *pHttpPostVars = NULL;
	const char *pFileName = NULL;
	int debug = 0;
	int i = 2;

	if(argc == 1)
	{
		printf("%s: [-d] [url] [optional post vars]\n", argv[0]);
		exit(0);
	}

	if(argc >= i && *argv[i-1] == '-')
	{
		if(argv[i-1][1] == 'd')
			debug = 1;
		i++;
	}

	pUrl = (argc >= i ? argv[i-1] : NULL);
	i++;
	pHttpPostVars = (argc >= i ? argv[i-1] : NULL);

	pCfr = curlFetch(pUrl, pHttpPostVars);
	if(pCfr != NULL)
		pFileName = pCfr->ccf.pFileName;

	printf("'%s' --> '%s' == %s\n", pUrl, pFileName, (pCfr && pCfr->bFileFetched ? "OK" : "FAIL"));
	if(pCfr && pCfr->bFileFetched)
	{	char *pCmd = NULL;

		printf("HTTP response code %lu\n", pCfr->httpResponseCode);
		printf("%s duration %lums\n", (pCfr->httpResponseCode == 200 ? "Fetch" : "Query"), pCfr->queryDuration);
		if(pCfr->pContentType != NULL && strcasecmp("application/json", pCfr->pContentType) == 0)
		{
			if(debug)
				asprintf(&pCmd, "ls -la %s/; cat %s", CURL_BASE_DIR, pFileName);
			else
				asprintf(&pCmd, "cat %s", pFileName);
			system(pCmd);
			free(pCmd);
		}

		asprintf(&pCmd, "ls -la %s/", CURL_BASE_DIR);
		system(pCmd);
		free(pCmd);
	}

	curlCfrFree(pCfr);

	return 0;
}
#endif
