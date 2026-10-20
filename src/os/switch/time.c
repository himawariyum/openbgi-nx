/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * time.c - Switch back end: time services (inc/bgi/os.h)
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <time.h>

static u64 gStartTick;

uint64_t OsSwitch_TicksNs(void)
{
	u64 now = armGetSystemTick();
	if(!gStartTick)
		gStartTick = now;
	return armTicksToNs(now - gStartTick);
}

uint32_t OS_TicksMs(void)
{
	return (uint32_t)(OsSwitch_TicksNs() / 1000000ull);
}

void OS_SleepMs(uint32_t ms)
{
	svcSleepThread((u64)ms * 1000000ull);
}

void OS_TimerBeginPeriod(void)
{
}

void OS_TimerEndPeriod(void)
{
}

void OS_LocalTime(OsLocalTime_t* t)
{
	time_t now = time(NULL);
	struct tm tmv;
	if(!t)
		return;
	localtime_r(&now, &tmv);
	t->year = (uint16_t)(tmv.tm_year + 1900);
	t->month = (uint16_t)(tmv.tm_mon + 1);
	t->dayOfWeek = (uint16_t)tmv.tm_wday;
	t->day = (uint16_t)tmv.tm_mday;
	t->hour = (uint16_t)tmv.tm_hour;
	t->minute = (uint16_t)tmv.tm_min;
	t->second = (uint16_t)tmv.tm_sec;
	t->millis = (uint16_t)(OsSwitch_TicksNs() / 1000000ull % 1000);
}

void OS_IdleInit(void)
{
}

void OS_IdleShutdown(void)
{
}

void OS_IdleSleep(void)
{
	svcSleepThread(500000ull); // half a millisecond
}

void OS_IdleSleepMs(int ms)
{
	if(ms <= 1)
		svcSleepThread(500000ull);
	else
		svcSleepThread((u64)(ms > 10 ? 10 : ms) * 1000000ull);
}

int OS_PerfCounterNs(int64_t* out)
{
	if(!out)
		return 0;
	*out = (int64_t)OsSwitch_TicksNs();
	return 1;
}
