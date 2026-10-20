/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysutil.c - allocation helpers, the engine lock, the tick source, the
 *             idle sleep and the CRT random generator (inc/bgi/common.h
 *             for the allocation and random functions, inc/bgi/sys.h for
 *             the rest)
 */
#include "bgi/sys.h"
#include "bgi/os.h"
#include "bgi/display.h"

// ---- allocation ----------------------------------------------------------

// malloc that never returns NULL: the process is aborted when memory runs out (n = 0 gives a 1-byte block)
void* BGI_Alloc(size_t n)
{
	void* p = malloc(n ? n : 1);
	if(!p)
	{
		fprintf(stderr, "bgi: out of memory (%lu bytes)\n", (unsigned long)n);
		abort();
	}
	return p;
}

void* BGI_Calloc(size_t n)
{
	void* p = BGI_Alloc(n);
	memset(p, 0, n);
	return p;
}

void BGI_Free(void* p)
{
	free(p);
}

char* BGI_Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)BGI_Alloc(n);
	memcpy(d, s, n);
	return d;
}

//  --- engine lock ------------------------

/* The critical section held around the hash generator the DSC, SDC and
 * save-file codecs share, which the original's loader thread uses too.
 * Enter / Leave are no-ops before Init and after Delete. */
static OsLock_t* gEngineLock;

void EngineLock_Init(void)
{
	gEngineLock = OS_LockCreate();
}

// destroy the lock after waiting for whoever holds it
void EngineLock_Delete(void)
{
	OS_LockEnter(gEngineLock);
	OS_LockLeave(gEngineLock);
	OS_LockDestroy(gEngineLock);
	gEngineLock = NULL;
}

void EngineLock_Enter(void)
{
	if(gEngineLock)
		OS_LockEnter(gEngineLock);
}

void EngineLock_Leave(void)
{
	if(gEngineLock)
		OS_LockLeave(gEngineLock);
}

// ---- ticks ----------------------------------------------------------------

/* The original queries timeGetDevCaps and, when the minimum period is
 * below 10 ms, calls timeBeginPeriod(min) and switches GetTicks to
 * timeGetTime.  The OS layer always provides a millisecond clock, so the
 * switch is implicit here. */
static int gTimerPeriodOn; // between Timer_Begin and Timer_End

// raise the timer resolution (start-up)
void Timer_Begin(void)
{
	OS_TimerBeginPeriod();
	gTimerPeriodOn = 1;
}

// restore it (shutdown)
void Timer_End(void)
{
	if(gTimerPeriodOn)
	{
		OS_TimerEndPeriod();
		gTimerPeriodOn = 0;
	}
}

/* The engine's clock: milliseconds, wrapping at 2^32.  Up to 1.529 it is
 * the system clock.  From 1.535 on it can be stopped (Clock_Stop /
 * Clock_Resume): while stopped it reads the value it had when it stopped,
 * and the stopped time is taken out of it afterwards.  From 1.573 on it
 * does not see jumps either: a reading below the previous one, or more
 * than the step limit ("81 04", 500 ms by default) above it, moves the
 * base by the difference, so the clock starts at 0 on its first reading
 * and stands still across a stall such as a long load. */
static uint32_t gClockBase;          // subtracted from the system clock
static uint32_t gClockFrozen;        // the value while stopped; 0 while running (a clock stopped at 0 runs on)
static uint32_t gClockLast;          // the system clock at the previous reading (1.573 on)
static uint32_t gClockMaxStep = 500; // the longest step that counts as time passing
static int gClockStopInactive;       // "81 68": stop while the window is inactive
static int gClockStopped;            // 1.547 on: a stop is in effect (the next one is not counted)

uint32_t GetTicks(void)
{
	uint32_t now = OS_TicksMs();
	if(gEngine->gen < GEN_1_535)
		return now;
	if(gEngine->gen >= GEN_1_573)
	{
		if(now < gClockLast || gClockLast + gClockMaxStep < now)
			gClockBase += now - gClockLast;
		gClockLast = now;
	}
	return gClockFrozen ? gClockFrozen : now - gClockBase;
}

/* Stop the clock (the window lost the focus, or a message box or a
 * dialog is up).  `force` (1.547 on, the boxes and dialogs) stops it
 * whatever "81 68" says; the deactivation stops it only when "81 68" asked
 * for that.  From 1.547 on a second stop before the resume changes
 * nothing; from 1.588 on nothing happens before the window exists.  1 when
 * the clock was stopped here. */
int Clock_Stop(int force)
{
	int stopped = 0;
	if(gEngine->gen < GEN_1_535)
		return 0;
	if(gEngine->gen < GEN_1_547)
	{
		if(gClockStopInactive && !gClockFrozen)
		{
			gClockFrozen = GetTicks();
			return 1;
		}
		return 0;
	}
	if((gEngine->gen >= GEN_1_588 && !gWindowAlive) || gClockStopped)
		return 0;
	if((gClockStopInactive || force) && !gClockFrozen)
	{
		gClockFrozen = GetTicks();
		stopped = 1;
	}
	gClockStopped = 1;
	return stopped;
}

/* Let the clock run on, the stopped time taken out of it (1.535: only
 * while "81 68" is set).  1 when it had been stopped. */
int Clock_Resume(void)
{
	uint32_t at = gClockFrozen;
	if(gEngine->gen < GEN_1_535)
		return 0;
	if(gEngine->gen < GEN_1_547)
	{
		if(!gClockStopInactive || !at)
			return 0;
	}
	else
	{
		if((gEngine->gen >= GEN_1_588 && !gWindowAlive) || !gClockStopped)
			return 0;
		gClockStopped = 0;
		if(!at)
			return 0;
	}
	gClockFrozen = 0;
	gClockBase += GetTicks() - at;
	return 1;
}

// 1.547 on: a message box or a dialog is up - the clock stops whatever "81 68" says, and runs on after it
void Clock_BoxBegin(void)
{
	if(gEngine->gen >= GEN_1_547)
		Clock_Stop(1);
}

void Clock_BoxEnd(void)
{
	if(gEngine->gen >= GEN_1_547)
		Clock_Resume();
}

// "81 68" (1.535 on): stop the clock (and from 1.547 on the sound) while the window is inactive; the previous setting
int Clock_SetStopWhenInactive(int on)
{
	int prev = gClockStopInactive;
	gClockStopInactive = on;
	return prev;
}

int Clock_StopsWhenInactive(void)
{
	return gClockStopInactive;
}

// "81 04" (1.616 on): the step limit, 50 .. 60000 ms; 1 when accepted
int Clock_SetMaxStep(uint32_t ms)
{
	if(ms - 50 > 60000 - 50)
		return 0;
	gClockMaxStep = ms;
	return 1;
}

// ---- idle sleep ------------------------------------------------------------

void Idle_Init(void)
{
	OS_IdleInit();
}

void Idle_Shutdown(void)
{
	OS_IdleShutdown();
}

// the ~0.5 ms sleep at the end of a pass that presented nothing (Display_FrameIdle)
void Sys_Idle(void)
{
	OS_IdleSleep();
}

// ---- the CRT random generator -----------------------------------------------------------
static uint32_t gRandSeed = 1; // the CRT's holdrand, initially 1

// "80 00": srand
void BGI_Srand(uint32_t seed)
{
	gRandSeed = seed;
}

// "80 01" / "80 02": rand, the MSVC sequence (seed * 214013 + 2531011, bits 16 .. 30)
int BGI_Rand(void)
{
	gRandSeed = gRandSeed * 214013u + 2531011u;
	return (int)((gRandSeed >> 16) & 0x7fffu);
}
