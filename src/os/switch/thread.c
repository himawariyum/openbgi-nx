/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - Switch back end: threads, recursive locks and atomics
 *            (inc/bgi/os.h), on libnx threads and rmutexes
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <stdlib.h>

struct OsLock
{
	RMutex m;
};

OsLock_t* OS_LockCreate(void)
{
	OsLock_t* l = (OsLock_t*)calloc(1, sizeof *l);
	if(!l)
		return NULL;
	rmutexInit(&l->m);
	return l;
}

void OS_LockDestroy(OsLock_t* l)
{
	free(l);
}

void OS_LockEnter(OsLock_t* l)
{
	if(l)
		rmutexLock(&l->m);
}

void OS_LockLeave(OsLock_t* l)
{
	if(l)
		rmutexUnlock(&l->m);
}

/* Where threads run.  threadCreate(..., -2) means "the process's default
 * core", which comes from the NPDM of the title the NRO was started as, so it
 * differs between launch methods (a forwarder, the Homebrew Menu's title
 * override): the main thread, the audio thread and the engine's workers could
 * all end up on one core, starving the audio mixer.  The threads are
 * therefore put on the process's other cores explicitly. */
static u64 gCoreMask = 1;
static int gMainCore;

void OsSwitch_CoresInit(void)
{
	u64 mask = 0, t0, ms;
	volatile u32 x = 1;
	u32 i;
	if(R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask)
		gCoreMask = mask;
	gMainCore = (int)svcGetCurrentProcessorNumber();
	// a small fixed workload, to compare the CPU speed between launch methods in the log
	t0 = OsSwitch_TicksNs();
	for(i = 0; i < 10000000u; i++)
		x = x * 1664525u + 1013904223u;
	ms = (OsSwitch_TicksNs() - t0) / 1000000ull;
	OsSwitch_Log("cores: mask=0x%llx main thread on core %d; cpu bench %llu ms (10M mul-add)", (unsigned long long)gCoreMask, gMainCore,
		(unsigned long long)ms);
}

// A core other than the main thread's for thread number `slot`, or -2 (the process default) when there is none.
int OsSwitch_PickCore(int slot)
{
	int found[4], n = 0, core;
	for(core = 0; core < 4; core++)
		if(((gCoreMask >> core) & 1) && core != gMainCore)
			found[n++] = core;
	return n ? found[slot % n] : -2;
}

struct OsThread
{
	Thread t;
	OsThreadFn_t fn;
	void* arg;
};

static void ThreadEntry(void* p)
{
	OsThread_t* t = (OsThread_t*)p;
	t->fn(t->arg);
}

OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority)
{
	OsThread_t* t = (OsThread_t*)calloc(1, sizeof *t);
	int prio = 0x2c; // the libnx default
	if(!t)
		return NULL;
	t->fn = fn;
	t->arg = arg;
	if(priority == OS_PRIO_HIGHEST)
		prio = 0x20;
	else if(priority > OS_PRIO_NORMAL)
		prio = 0x28;
	if(R_FAILED(threadCreate(&t->t, ThreadEntry, t, NULL, 0x10000, prio, OsSwitch_PickCore(1))) &&
	   R_FAILED(threadCreate(&t->t, ThreadEntry, t, NULL, 0x10000, prio, -2)))
	{
		OsSwitch_Log("thread: threadCreate failed (priority 0x%x)", prio);
		free(t);
		return NULL;
	}
	if(R_FAILED(threadStart(&t->t)))
	{
		threadClose(&t->t);
		free(t);
		return NULL;
	}
	return t;
}

void OS_ThreadJoin(OsThread_t* t)
{
	if(!t)
		return;
	threadWaitForExit(&t->t);
	threadClose(&t->t);
	free(t);
}

int OS_CpuCount(void)
{
	return 3;
}

int32_t OS_AtomicInc(volatile int32_t* p)
{
	return __sync_add_and_fetch(p, 1);
}

int32_t OS_AtomicDec(volatile int32_t* p)
{
	return __sync_sub_and_fetch(p, 1);
}
