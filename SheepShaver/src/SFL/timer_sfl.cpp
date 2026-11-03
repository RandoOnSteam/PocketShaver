/*
 *  timer_sfl.cpp - Time Manager support for the SFL platform layer
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "macos_util.h"
#include "timer.h"
#include "gui.h"
#include "thread.h"

#define SFLTIMERIDLEWAITMS 10

typedef struct SFLTIMER
{
	WSTIMESPEC start;
	WSSIGNAL idle;
	bool created;
} SFLTIMER;

static SFLTIMER SflTimer;

void timer_init(void)
{
	if (SflTimer.created)
		return;
	WSClockGetSpec(&SflTimer.start);
	WSSignal_Create(&SflTimer.idle);
	SflTimer.created = true;
}

uint64 GetTicks_usec(void)
{
	WSTIMESPEC now;
	WSClockGetSpec(&now);
	WSTimespecSubtract(&now, &SflTimer.start, &now);
	return WSClockTimespecToMicros(&now);
}

void Delay_usec(uint32 usec)
{
	if (usec < 1000) {
		WSThreadYield();
		return;
	}
	WSThreadSleep(usec / 1000);
}

void Microseconds(uint32 &hi, uint32 &lo)
{
	uint64 now;
	now = GetTicks_usec();
	hi = (uint32)(now >> 32);
	lo = (uint32)now;
}

uint32 TimerDateTime(void)
{
	return TimeToMacTime(time(NULL));
}

void timer_current_time(tm_time_t &t)
{
	t = (tm_time_t)GetTicks_usec();
}

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a + b;
}

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a - b;
}

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0)
		res = (tm_time_t)mactime * 1000;
	else
		res = -(tm_time_t)mactime;
}

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime < 0)
		return 0;
	if (hosttime > 0x7fffffff)
		return (int32)(hosttime / 1000);
	return -(int32)hosttime;
}

void idle_wait(void)
{
	WSSignal_Wait(&SflTimer.idle, SFLTIMERIDLEWAITMS);
}

void idle_resume(void)
{
	WSSignal_Raise(&SflTimer.idle);
}
