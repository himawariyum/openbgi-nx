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
	if(R_FAILED(threadCreate(&t->t, ThreadEntry, t, NULL, 0x10000, prio, -2)))
	{
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
