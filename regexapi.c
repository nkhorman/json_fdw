/*--------------------------------------------------------------------*
 *
 *	This "source code" is part of Spamilter - http://www.spamilter.org
 *	Additionally, this "source code" is herby, also incorporated as part json_fdw
 *
 * Developed by;
 *	Neal Horman - http://www.wanlink.com
 *	Copyright (c) 2010-2015 Neal Horman. All Rights Reserved
 *
 *	Redistribution and use in source and binary forms, with or without
 *	modification, are permitted provided that the following conditions
 *	are met;
 *
 *		1. Redistributions of source code must retain the above copyright
 *		   notice, this list of conditions and the following disclaimer.
 *		2. Redistributions in binary form must reproduce the above copyright
 *		   notice, this list of conditions and the following disclaimer in the
 *		   documentation and/or other materials provided with the distribution.
 *		3. All advertising materials mentioning features or use of this software
 *		   must display the following acknowledgement:
 *		This product includes software developed by Neal Horman.
 *		4. Neither the name Neal Horman nor the names of any contributors
 *		   may be used to endorse or promote products derived from this software
 *		   without specific prior written permission.
 *		
 *		THIS SOFTWARE IS PROVIDED BY NEAL HORMAN AND ANY CONTRIBUTORS ``AS IS'' AND
 *		ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *		IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *		ARE DISCLAIMED.  IN NO EVENT SHALL NEAL HORMAN OR ANY CONTRIBUTORS BE LIABLE
 *		FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *		DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *		OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *		HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *		LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *		OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *		SUCH DAMAGE.
 *
 *		Alternately;
 *
 *		This "source code" is free software: you can redistribute it and/or modify
 *		it under the terms of the GNU General Public License as published by
 *		the Free Software Foundation, either version 3 of the License, or
 *		(at your option) any later version.
 *
 *		This "source code" is distributed in the hope that it will be useful,
 *		but WITHOUT ANY WARRANTY; without even the implied warranty of
 *		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *		GNU General Public License for more details.
 *
 *		You should have received a copy of the GNU General Public License
 *		along with this "source code".  If not, see <http://www.gnu.org/licenses/>.
 *
 *	RCSID:  $Id$
 *
 *--------------------------------------------------------------------*/

static char const rcsid[] = "@(#)$Id$";

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>

#define _IS_REGEXAPI_
#include "regexapi.h"

void regexapi_free(regexapi_t *prat)
{
	if(prat != NULL)
	{	unsigned int i,j;

		for(i=0; i<prat->matches; i++)
		{	regexapimatch_t *pmatch = prat->pmatches+i;
		
			if(pmatch->nsubs && pmatch->ppsubs != NULL)
			{
				for(j=0; j<pmatch->nsubs; j++)
					free(*(pmatch->ppsubs+j));
				free(pmatch->ppsubs);
			}
		}
		if(prat->matches && prat->pmatches != NULL)
			free(prat->pmatches);
		regfree(&prat->re);
		if(prat->preerr != NULL)
			free(prat->preerr);
		free(prat);
	}
}

const char *regexapi_sub(regexapi_t *prat, size_t match, size_t nsub)
{
	return (prat != NULL && match < prat->matches && nsub <= (prat->pmatches+match)->nsubs ? *((prat->pmatches+match)->ppsubs+nsub) : NULL);
}

int regexapi_nsubs(regexapi_t *prat, size_t match)
{
	return (prat != NULL && match < prat->matches ? (prat->pmatches+match)->nsubs : 0);
}

int regexapi_matches(regexapi_t *prat)
{
	return (prat != NULL ? prat->matches : 0);
}

int regexapi_err(regexapi_t *prat)
{
	return (prat != NULL ? prat->rerc : 0);
}

const char *regexapi_errStr(regexapi_t *prat)
{
	return (prat != NULL && prat->preerr != NULL ? prat->preerr : "");
}

static void regexapi_buildErrStr(regexapi_t *prat)
{
	if(prat != NULL)
	{	char errbuf[1024];

		memset(&errbuf,0,sizeof(errbuf));
		regerror(prat->rerc,&prat->re,errbuf,sizeof(errbuf));
		prat->preerr = strdup(errbuf);
	}
}

regexapi_t *regexapi_exec(const char *pstr, const char *pregex, unsigned int cflags, unsigned int findCount)
{	regexapi_t *prat = calloc(sizeof(regexapi_t),1);

#ifdef _REGEX_DEBUG
	printf("%s:%d - pstr '%s' pregex '%s' cflags 0x%04X findCount %u\n" , __func__, __LINE__ , pstr, pregex, cflags, findCount);
#endif
	if(prat != NULL)
	{
		prat->rerc = regcomp(&prat->re,pregex,cflags);

#ifdef _REGEX_DEBUG
		if(prat->rerc == 0)
			printf("%s:%d - regcomp() = %d,  nsub = %d\n", __func__, __LINE__, prat->rerc, prat->re.re_nsub);
#endif
		if(prat->rerc == 0)
		{	size_t i;
			char *pdst = NULL;
			regmatch_t *presubs = (regmatch_t *)calloc(sizeof(regmatch_t),prat->re.re_nsub+1);
			regexapimatch_t *pmatch = NULL;
			size_t last = 0;

			// don't allow iteration for more subs than actually exist
			if(prat->re.re_nsub < findCount)
				findCount = prat->re.re_nsub;

			while((prat->rerc = regexec(&prat->re,pstr+last,prat->re.re_nsub+1,presubs,0)) == 0 && findCount != 0)
			{
				findCount --;
				prat->matches ++;
				prat->pmatches = realloc(prat->pmatches,sizeof(regexapimatch_t)*prat->matches);
				pmatch = prat->pmatches+(prat->matches-1);
#ifdef _REGEX_DEBUG
				printf("%s:%d - regexec() = %d\n", __func__, __LINE__, prat->rerc);
#endif
				pmatch->nsubs = 0;
				pmatch->ppsubs = (char **)calloc(1,sizeof(char *)*prat->re.re_nsub);

				if(pmatch->ppsubs != NULL)
				{
					for(i=1; i<prat->re.re_nsub+1; i++)
					{	size_t so = (presubs+i)->rm_so + last;
						size_t eo = (presubs+i)->rm_eo + last;
						size_t qo = (eo - so);

						pmatch->nsubs++;
						pdst = *(pmatch->ppsubs+(i-1)) = (char *)calloc(qo+1,1);
						strncpy(pdst,(pstr+so),qo);
						*(pdst+qo) = 0;

						if(i == prat->re.re_nsub)
							last = eo;
#ifdef _REGEX_DEBUG
						printf("%s:%d - sub %d: so %d eo %d qo %d - '%*.*s'\n", __func__, __LINE__, i, so, eo, qo, qo, qo, pdst);
#endif
					}
				}
			}

			if(presubs != NULL)
				free(presubs);

			if(prat->matches > 0 && prat->rerc == 1)
				prat->rerc = 0;
		}
	}

	if(prat != NULL && prat->rerc)
	{
		regexapi_buildErrStr(prat);
#ifdef _REGEX_DEBUG
		printf("regex error: %d/'%s'\n",prat->rerc,regexapi_errStr(prat));
#endif
	}

	return prat;
}

int regexapi(const char *pstr, const char *pregex, int cflags)
{	regexapi_t *prat = regexapi_exec(pstr,pregex,cflags,1);
	int rc = regexapi_matches(prat) != 0;

	if(prat != NULL)
		regexapi_free(prat);

	return rc;
}


#ifdef _REGEX_UNIT_TEST
int main(int argc, char **argv)
{
	if(argc == 3)
	{	int i = 1;
		regexapi_t *prat = regexapi_exec(argv[i],argv[i+1],REGEX_DEFAULT_CFLAGS,REGEX_FIND_ALL);

		printf("%s: '%s' %c= '%s'\n", argv[0], argv[i], (regexapi_matches(prat) ? '=' : '!'), argv[i+1]);
		if(regexapi_matches(prat))
		{	int q;

			for(i=0,q=regexapi_nsubs(prat,0); i<q; i++)
				printf("sub %d: '%s'\n",i+1,regexapi_sub(prat,0,i));
		}

		regexapi_free(prat);
	}
	else
		printf("%s [string to test] [regex]\n", argv[0]);

	return 0;
}
#endif
