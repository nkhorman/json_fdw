
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

#ifndef _WIN32

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define NsInAMs 1000000
#define MsInASec 1000

#ifdef __APPLE__

// see https://developer.apple.com/library/mac/qa/qa1398/_index.html
// and http://stackoverflow.com/questions/3269321/osx-programmatically-get-uptime

#include <assert.h>
#include <CoreServices/CoreServices.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include "gettickcount.h"

unsigned long GetTickCount(void)
{
	static mach_timebase_info_data_t    sTimebaseInfo;

	// If this is the first time we've run, get the timebase.
	// We can use denom == 0 to indicate that sTimebaseInfo is 
	// uninitialised because it makes no sense to have a zero 
	// denominator is a fraction.

	if ( sTimebaseInfo.denom == 0 )
		(void) mach_timebase_info(&sTimebaseInfo);

	return (mach_absolute_time() * (sTimebaseInfo.numer / sTimebaseInfo.denom)) / NsInAMs;
}

#else

unsigned long GetTickCount(void)
{	struct timespec ts_uptime;
	unsigned long uptimeInMs = 0;

	if(clock_gettime(CLOCK_UPTIME, &ts_uptime) == 0)
	{
		uptimeInMs = (ts_uptime.tv_sec * MsInASec);

		if(ts_uptime.tv_nsec > 0)
			uptimeInMs += (ts_uptime.tv_nsec / NsInAMs);
	}

	return uptimeInMs;
}
#endif // __APPLE__
#endif // _WINDOWS

#ifdef UNIT_TEST
// to compile - gcc -DUNIT_TEST -o uptime gettickcount.c && while [ 1 ]; do clear; ./uptime; sleep 1; done
#include <stdio.h>

int main(int argc, char **argv)
{	unsigned long nTicks = GetTickCount();
	unsigned long secs = (nTicks > 0 ? nTicks / 1000 : 0);
	unsigned long Secs=0, Mins=0, Hrs=0, Days=0;

	if(secs > 0)
	{
		Secs = secs % 60;
		Mins = (secs / 60) % 60;
		Hrs = (secs / (60 * 60)) % 24;
		Days = (secs / (60 * 60 * 24));
	}
	printf("uptime in ms %lu = %lu days and %02lu:%02lu:%02lu",nTicks,Days,Hrs,Mins,Secs);

	return 0;
}
#endif
