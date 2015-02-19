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

// Help build a unique string
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

// Build a unique string
static char *curlSessionUnique(void)
{	char *pStr = NULL;
	MD5_CTX md5ctx;
	unsigned char md5digest[MD5_DIGEST_LENGTH];
	char md5ascii[(MD5_DIGEST_LENGTH*2)+10];

	memset(&md5ctx, 0, sizeof(md5ctx));
	memset(&md5digest, 0, sizeof(md5digest));
	memset(&md5ascii, 0, sizeof(md5ascii));

	// use our thread id and time in epoch form as our uniqueness
	asprintf(&pStr,"%04X0x%lX", (unsigned int)pthread_self(), (unsigned long)time(NULL));

	// hash it
	if(pStr != NULL)
	{
		MD5_Init(&md5ctx);
		MD5_Update(&md5ctx, (const unsigned char *)pStr, strlen(pStr));
		MD5_Final(md5digest, &md5ctx);
		MD5_Ascii(md5digest, md5ascii);
		free(pStr);
	}

	if(strlen(md5ascii))
		pStr = strdup(md5ascii);

	return pStr;
}

// Test if filename is a CURL supported URL and setup the ciu_t if it is
// If the URL specifies a filename, try to use that on disk, so that the
// native file handlers can guess the file type, and act accordingly,
// otherwise, just create a tempoary filename.
int curlIsUrl(const char *pFname, ciu_t *pCiu)
{	int isCurlUrl = 0;
	regexapi_t *pRat = regexapi_exec_list(pFname, &regexUrls[0]);

	// Assume that we should init the ciu
	memset(pCiu, 0, sizeof(ciu_t));

	// If we found a regex match, then we assume that CURL supports the url
	if(pRat != NULL)
	{	int regexNSubs = regexapi_nsubs(pRat, 0);
		// Assume that the last subcomponent of the regex is the filename portion
		const char *pRegexSub = (regexNSubs > 1 ? regexapi_sub(pRat, 0, regexNSubs - 1) : NULL);
		// and get the basename of that to use as the on disk filename
		char *pBaseName = (pRegexSub != NULL ? strrchr(pRegexSub, '/') : NULL);

		if(pBaseName != NULL && *pBaseName)
		{
			// The string returned to us is not const, so we'll terminate it
			// at the URI point, so as to not have silly on disk filenames
			char *pTerm = strchr(pBaseName, '?');
			int fd = -1;

			if(pTerm != NULL)
				*pTerm = 0;

			if(*pBaseName == '/')
				pBaseName++;

			if(!*pBaseName) // the URL didn't specify a file, build a temp filename
			{
				char tmpfnamebuf[MAXFILENAME];

				memset(tmpfnamebuf, 0, sizeof(tmpfnamebuf));
				sprintf(tmpfnamebuf, "%s/tmpXXXXXXXXXX", CURL_BASE_DIR);

				if((fd = mkstemp(tmpfnamebuf)) != -1)
				{
					pCiu->diskFileName = strdup(tmpfnamebuf);
					pCiu->bNeedUnlink = true;
				}
			}
			else	// Use the specified filename from the URL
			{	char *pUnique = curlSessionUnique();

				// but add a source of uniqueness, so we don't
				// possibly trample the filename of another session
				if(pUnique != NULL)
				{
					asprintf(&pCiu->diskFileName, "%s/%s.%s", CURL_BASE_DIR, pUnique, pBaseName);
					free(pUnique);
				}
				else // fall back
					asprintf(&pCiu->diskFileName, "%s/%s", CURL_BASE_DIR, pBaseName);

				// create the file for exclusive access
				if((fd = open(pCiu->diskFileName, O_WRONLY|O_CREAT|O_EXCL, 0600)) != -1)
					pCiu->bNeedUnlink = true;
			}

			// Get a FILE pointer
			pCiu->diskFile = (fd != -1 ? fdopen(fd, "w") : NULL);
			// Flag to Unlink, if we did open the file
			pCiu->bNeedUnlink &= (pCiu->diskFile != NULL);

			// If we succeeded in opening a file, then this is a CURL URL
			isCurlUrl = (pCiu->diskFile != NULL);
		}
		else
			isCurlUrl = 0;	// Because we are here, suffix validation failed

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

		curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, ""); // turn on builtin supported default content dencoding
		curl_easy_setopt(curl_handle, CURLOPT_TRANSFER_ENCODING, 1L); // turn on transfer decoding

		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // turn on redirection following
		curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5); // for a maximum of 5
		curl_easy_setopt(curl_handle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL); // maintain a post as a post on redirects
		curl_easy_setopt(curl_handle, CURLOPT_AUTOREFERER, 1L); // turn one Refer when redirecting

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

		// the file should already be open, get it
		res = curl_easy_perform(curl_handle);

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
					)
#endif
				);
		}

		// all done, cleanup
		curl_easy_cleanup(curl_handle);
		curl_global_cleanup();
	}

	return pCfr;
}

#ifdef _CURL_UNIT_TEST
int main(int argc, char **argv)
{
	ciu_t ciu;
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

	if(curlIsUrl(pUrl, &ciu))
	{
		pCfr = curlFetch(pUrl, pHttpPostVars, &ciu);
		if(pCfr != NULL)
			pFileName = pCfr->pFileName;
	}

	printf("'%s' --> '%s' == %s\n", pUrl, pFileName, (pCfr && pCfr->bFileFetched ? "OK" : "FAIL"));
	if(pCfr && pCfr->bFileFetched)
	{	char *pCmd = NULL;

		if(debug)
			asprintf(&pCmd, "ls -la /tmp/; cat %s", pFileName);
		else
			asprintf(&pCmd, "cat %s", pFileName);
		system(pCmd);
		free(pCmd);
	}

	curlFetchFree(pCfr);

	if(debug && pCfr && pCfr->bFileFetched)
		system("ls -la /tmp/");

	return 0;
}
#endif
