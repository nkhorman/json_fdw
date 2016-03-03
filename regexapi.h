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

#ifndef _REGEXAPI_H_
#define _REGEXAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

	#include <regex.h>

	#define REGEX_DEFAULT_CFLAGS ( REG_EXTENDED | REG_ICASE )
	#define REGEX_FIND_ALL ~0 

#ifdef _IS_REGEXAPI_
	typedef struct _regexapimatch_t
	{
		size_t nsubs;
		char **ppsubs;
	}regexapimatch_t;

	typedef struct _regexapi_t
	{
		regex_t re;
		int rerc;
		char *preerr;

		unsigned int matches;
		regexapimatch_t *pmatches;
	}regexapi_t;
#else
	typedef struct _regexapi_t regexapi_t;
#endif

	void regexapi_free(regexapi_t *prat);
	const char *regexapi_sub(regexapi_t *prat, size_t match, size_t nsub);
	int regexapi_nsubs(regexapi_t *prat, size_t match);
	int regexapi_matches(regexapi_t *prat);
	int regexapi_err(regexapi_t *prat);
	const char *regexapi_errStr(regexapi_t *prat);
	regexapi_t *regexapi_exec(const char *pstr, const char *pregex, unsigned int cflags, unsigned int findCount);

	// for simplicitly
	int regexapi(const char *pstr, const char *pregex, int cflags);

#ifdef __cplusplus
}
#endif

#endif

