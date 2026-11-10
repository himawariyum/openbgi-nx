/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fs.c - Switch back end: files, directories, enumeration and drives
 *        (inc/bgi/os.h)
 *
 * Reads come from the game root (romfs:/ by default), writes from
 * sdmc:/switch/openbgi/.  The Windows attribute bits are approximated:
 * a directory, the owner's write bit as read-only, a leading '.' as hidden.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#define SWITCH_PATH_MAX 1024

struct OsFile
{
	FILE* f;
	char path[SWITCH_PATH_MAX];
};

// Create every missing directory of a writable path.
static void MakeDirs(const char* path)
{
	char tmp[SWITCH_PATH_MAX];
	char* p;
	size_t len;
	snprintf(tmp, sizeof tmp, "%s", path);
	len = strlen(tmp);
	if(len == 0)
		return;
	// stop before the file name
	for(p = tmp + len; p > tmp && *p != '/'; p--)
	{
	}
	if(p == tmp)
		return;
	*p = 0;
	// keep the "sdmc:" prefix intact
	for(p = tmp + 5; *p; p++)
	{
		if(*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

static uint32_t AttrsFromStat(const struct stat* st)
{
	uint32_t a = 0;
	if(S_ISDIR(st->st_mode))
		a |= OS_ATTR_DIRECTORY;
	else
		a |= OS_ATTR_ARCHIVE;
	if(!(st->st_mode & S_IWUSR))
		a |= OS_ATTR_READONLY;
	return a ? a : OS_ATTR_NORMAL;
}

static OsFile_t* FileOpenReadImpl(const char* path)
{
	char p[SWITCH_PATH_MAX];
	struct stat st;
	int missing = 0;
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	OsSwitch_PathEx(path, p, sizeof p, &missing);
	f->f = missing ? NULL : fopen(p, "rb"); // a name the directory list lacks is not tried
	if(!f->f)
	{
		OsSwitch_PathWrite(path, p, sizeof p);
		f->f = OsSwitch_NativeMissing(p) ? NULL : fopen(p, "rb");
	}
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	if(fstat(fileno(f->f), &st) == 0 && S_ISDIR(st.st_mode))
	{
		fclose(f->f);
		free(f);
		return NULL;
	}
	snprintf(f->path, sizeof f->path, "%s", p);
	return f;
}

OsFile_t* OS_FileCreate(const char* path)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char p[SWITCH_PATH_MAX];
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	OsSwitch_PathWrite(path, p, sizeof p);
	MakeDirs(p);
	f->f = fopen(p, "w+b");
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	snprintf(f->path, sizeof f->path, "%s", p);
	return f;
}

OsFile_t* OS_FileOpenWrite(const char* path)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char p[SWITCH_PATH_MAX];
	OsFile_t* f = (OsFile_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	OsSwitch_PathWrite(path, p, sizeof p);
	MakeDirs(p);
	f->f = fopen(p, "r+b");
	if(!f->f)
		f->f = fopen(p, "w+b");
	if(!f->f)
	{
		free(f);
		return NULL;
	}
	snprintf(f->path, sizeof f->path, "%s", p);
	return f;
}

void OS_FileClose(OsFile_t* f)
{
	if(!f)
		return;
	if(f->f)
		fclose(f->f);
	free(f);
}

static uint32_t FileReadImpl(OsFile_t* f, void* buf, uint32_t n)
{
	if(!f || !f->f)
		return 0;
	return (uint32_t)fread(buf, 1, n, f->f);
}

uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n)
{
	if(!f || !f->f)
		return 0;
	return (uint32_t)fwrite(buf, 1, n, f->f);
}

static int FileSeekImpl(OsFile_t* f, uint32_t pos)
{
	if(!f || !f->f)
		return 0;
	return fseek(f->f, (long)pos, SEEK_SET) == 0;
}

static uint32_t FileSizeImpl(OsFile_t* f)
{
	long pos, end;
	if(!f || !f->f)
		return 0;
	pos = ftell(f->f);
	fseek(f->f, 0, SEEK_END);
	end = ftell(f->f);
	fseek(f->f, pos, SEEK_SET);
	return end < 0 ? 0 : (uint32_t)end;
}

OsSwitchFsStats_t gSwitchFsStats;

OsFile_t* OS_FileOpenRead(const char* path)
{
	static int logged;
	uint64_t t = OsSwitch_TicksNs();
	OsFile_t* f = FileOpenReadImpl(path);
	gSwitchFsStats.openNs += OsSwitch_TicksNs() - t;
	gSwitchFsStats.opens++;
	if(logged < 16)
	{ // the first files the engine asks for: what boot loads, and when
		logged++;
		OsSwitch_Log("open %s '%s'", f ? "ok  " : "FAIL", path ? path : "");
	}
	return f;
}

uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n)
{
	uint64_t t = OsSwitch_TicksNs();
	uint32_t r = FileReadImpl(f, buf, n);
	gSwitchFsStats.readNs += OsSwitch_TicksNs() - t;
	gSwitchFsStats.reads++;
	gSwitchFsStats.bytes += r;
	if(n < 4096)
		gSwitchFsStats.smallReads++;
	return r;
}

int OS_FileSeek(OsFile_t* f, uint32_t pos)
{
	uint64_t t = OsSwitch_TicksNs();
	int r = FileSeekImpl(f, pos);
	gSwitchFsStats.seekNs += OsSwitch_TicksNs() - t;
	gSwitchFsStats.seeks++;
	return r;
}

uint32_t OS_FileSize(OsFile_t* f)
{
	uint64_t t = OsSwitch_TicksNs();
	uint32_t r = FileSizeImpl(f);
	gSwitchFsStats.sizeNs += OsSwitch_TicksNs() - t;
	gSwitchFsStats.sizes++;
	return r;
}

// once a second while something changed, for the first seconds: where the boot time goes
void OsSwitch_FsStatsTick(void)
{
	static uint64_t last;
	static uint32_t prev;
	static int logged;
	const OsSwitchFsStats_t* s = &gSwitchFsStats;
	uint64_t now = OsSwitch_TicksNs();
	uint32_t activity = s->opens + s->reads + s->seeks + s->sizes + s->paths;
	if(logged >= 20 || now - last < 1000000000ull || activity == prev)
		return;
	last = now;
	prev = activity;
	logged++;
	OsSwitch_Log("fs: open %u (%llu ms) read %u (%u small, %llu KiB, %llu ms) seek %u (%llu ms) size %u (%llu ms) path %u (%llu ms, %u dir scans)",
		s->opens, (unsigned long long)(s->openNs / 1000000ull), s->reads, s->smallReads, (unsigned long long)(s->bytes >> 10),
		(unsigned long long)(s->readNs / 1000000ull), s->seeks, (unsigned long long)(s->seekNs / 1000000ull), s->sizes,
		(unsigned long long)(s->sizeNs / 1000000ull), s->paths, (unsigned long long)(s->pathNs / 1000000ull), s->caseScans);
}

int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi)
{
	struct stat st;
	uint64_t t;
	if(!f || !f->f || fstat(fileno(f->f), &st) != 0)
		return 0;
	t = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
	if(lo)
		*lo = (uint32_t)t;
	if(hi)
		*hi = (uint32_t)(t >> 32);
	return 1;
}

int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi)
{
	struct utimbuf ut;
	uint64_t t = ((uint64_t)hi << 32) | lo;
	if(!f || !f->f)
		return 0;
	t /= 10000000ull;
	if(t < 11644473600ull)
		t = 0;
	else
		t -= 11644473600ull;
	ut.actime = ut.modtime = (time_t)t;
	return utime(f->path, &ut) == 0;
}

uint32_t OS_FileAttrs(const char* path)
{
	char p[SWITCH_PATH_MAX];
	struct stat st;
	OsSwitch_Path(path, p, sizeof p);
	if(stat(p, &st) == 0)
		return AttrsFromStat(&st);
	OsSwitch_PathWrite(path, p, sizeof p);
	if(stat(p, &st) == 0)
		return AttrsFromStat(&st);
	return OS_INVALID_ATTRS;
}

int OS_FileSetAttrs(const char* path, uint32_t attrs)
{
	char p[SWITCH_PATH_MAX];
	struct stat st;
	OsSwitch_Path(path, p, sizeof p);
	if(stat(p, &st) != 0)
	{
		OsSwitch_PathWrite(path, p, sizeof p);
		if(stat(p, &st) != 0)
			return 0;
	}
	if(attrs & OS_ATTR_READONLY)
		st.st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else
		st.st_mode |= S_IWUSR;
	return chmod(p, st.st_mode) == 0;
}

int OS_FileDelete(const char* path)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char p[SWITCH_PATH_MAX];
	OsSwitch_PathWrite(path, p, sizeof p);
	return remove(p) == 0;
}

int OS_FileMove(const char* from, const char* to)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char a[SWITCH_PATH_MAX], b[SWITCH_PATH_MAX];
	OsSwitch_Path(from, a, sizeof a);
	OsSwitch_PathWrite(to, b, sizeof b);
	if(rename(a, b) == 0)
		return 1;
	OsSwitch_PathWrite(from, a, sizeof a);
	return rename(a, b) == 0;
}

int OS_FileCopy(const char* from, const char* to, int failIfExists)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char a[SWITCH_PATH_MAX], b[SWITCH_PATH_MAX];
	FILE* in;
	FILE* out;
	char buf[8192];
	size_t n;
	OsSwitch_Path(from, a, sizeof a);
	if(access(a, F_OK) != 0)
		OsSwitch_PathWrite(from, a, sizeof a);
	OsSwitch_PathWrite(to, b, sizeof b);
	if(failIfExists && access(b, F_OK) == 0)
		return 0;
	in = fopen(a, "rb");
	if(!in)
		return 0;
	MakeDirs(b);
	out = fopen(b, "wb");
	if(!out)
	{
		fclose(in);
		return 0;
	}
	while((n = fread(buf, 1, sizeof buf, in)) > 0)
	{
		if(fwrite(buf, 1, n, out) != n)
		{
			fclose(in);
			fclose(out);
			return 0;
		}
	}
	fclose(in);
	fclose(out);
	return 1;
}

int OS_DirCreate(const char* path)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char p[SWITCH_PATH_MAX];
	OsSwitch_PathWrite(path, p, sizeof p);
	MakeDirs(p);
	if(mkdir(p, 0777) == 0)
		return 1;
	return 0; // CreateDirectory fails when it exists already
}

int OS_DirRemove(const char* path)
{
	OsSwitch_DirCacheFlush(); // the directory lists are out of date once this returns
	char p[SWITCH_PATH_MAX];
	OsSwitch_PathWrite(path, p, sizeof p);
	return rmdir(p) == 0;
}

struct OsFind
{
	DIR* d;
	char pattern[260];
	char dir[SWITCH_PATH_MAX];
};

static int FindFill(OsFind_t* h, OsFindData_t* out)
{
	struct dirent* e;
	while((e = readdir(h->d)) != NULL)
	{
		char p[SWITCH_PATH_MAX];
		struct stat st;
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if(!OsCommon_WildcardMatch(h->pattern, e->d_name))
			continue;
		snprintf(p, sizeof p, "%s%s", h->dir, e->d_name);
		if(stat(p, &st) != 0)
			continue;
		if(out)
		{
			out->attrs = AttrsFromStat(&st);
			snprintf(out->name, sizeof out->name, "%s", e->d_name);
		}
		return 1;
	}
	return 0;
}

OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out)
{
	char p[SWITCH_PATH_MAX];
	const char* slash;
	OsFind_t* h;
	size_t dlen;

	OsSwitch_Path(pattern, p, sizeof p);
	slash = strrchr(p, '/');
	if(!slash)
		return NULL;
	dlen = (size_t)(slash - p) + 1;
	h = (OsFind_t*)calloc(1, sizeof *h);
	if(!h)
		return NULL;
	snprintf(h->dir, sizeof h->dir, "%.*s", (int)dlen, p);
	snprintf(h->pattern, sizeof h->pattern, "%s", slash + 1);
	h->d = opendir(h->dir);
	if(!h->d)
	{
		// writable fallback
		OsSwitch_PathWrite(pattern, p, sizeof p);
		slash = strrchr(p, '/');
		if(!slash)
		{
			free(h);
			return NULL;
		}
		dlen = (size_t)(slash - p) + 1;
		snprintf(h->dir, sizeof h->dir, "%.*s", (int)dlen, p);
		snprintf(h->pattern, sizeof h->pattern, "%s", slash + 1);
		h->d = opendir(h->dir);
	}
	if(!h->d)
	{
		free(h);
		return NULL;
	}
	if(!FindFill(h, out))
	{
		closedir(h->d);
		free(h);
		return NULL;
	}
	return h;
}

int OS_FindNext(OsFind_t* h, OsFindData_t* out)
{
	if(!h || !h->d)
		return 0;
	return FindFill(h, out);
}

void OS_FindClose(OsFind_t* h)
{
	if(!h)
		return;
	if(h->d)
		closedir(h->d);
	free(h);
}

uint32_t OS_DriveType(const char* root)
{
	BGI_UNUSED(root);
	return OS_DRIVE_FIXED;
}

uint32_t OS_LogicalDrives(char* buf, uint32_t n)
{
	static const char drives[] = "C:\\";
	if(n < sizeof drives)
		return 0;
	memcpy(buf, drives, sizeof drives);
	return sizeof drives - 1;
}

int OS_VolumeReady(const char* root)
{
	BGI_UNUSED(root);
	return 1;
}

int OS_MediaPresent(char driveLetter)
{
	BGI_UNUSED(driveLetter);
	return 0;
}
