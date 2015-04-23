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

#include "regexapi.h"
#include "regexapi_helper.h"

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
	{ "(http[s]?)://(" URLSPEC ")(" URISPEC ")", ( REG_EXTENDED | REG_ICASE ), 2 },
	{ NULL, 0, 0 },
};

// Supported URL regex validation iterator
static regexapi_t *regexapi_exec_list(const char *subject, regexapilist_t const *pRegexList)
{	regexapi_t *pRat = NULL;

	while(pRat == NULL && pRegexList->pattern != NULL)
		pRat = regexapi_exec(subject, pRegexList->pattern, pRegexList->flags, pRegexList->findCount);

	return pRat;
}

regexapi_t *regexapi_url(char const *subject)
{
	return regexapi_exec_list(subject, regexUrls);
}
