/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * paths.c - Windows paths on the Switch file systems
 *
 * The engine builds Shift-JIS Windows paths ("C:\game\...").  Reads are
 * looked up under the game root (romfs:/ unless OS_SetGameDir chose an SD
 * directory); writes go to sdmc:/switch/openbgi/.  Components that do not
 * exist as spelled are matched case-insensitively, because the game data
 * was written for a case-insensitive file system.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#define SWITCH_PATH_MAX 1024

static char gGameRoot[SWITCH_PATH_MAX] = "romfs:/";
static const char gWriteRoot[] = "sdmc:/switch/openbgi/";

static void WithSlash(char* out, size_t n, const char* root)
{
	size_t len;
	snprintf(out, n, "%s", root);
	len = strlen(out);
	if(len == 0 || out[len - 1] != '/')
		snprintf(out + len, n - len, "/");
}

void OsSwitch_GameRoot(char* out, size_t n)
{
	WithSlash(out, n, gGameRoot);
}

void OsSwitch_WriteRoot(char* out, size_t n)
{
	snprintf(out, n, "%s", gWriteRoot);
}

void OsSwitch_SetGameRoot(const char* root)
{
	char tmp[SWITCH_PATH_MAX];
	WithSlash(tmp, sizeof tmp, root);
	snprintf(gGameRoot, sizeof gGameRoot, "%s", tmp);
}

/* Directory listings, read once.  The engine probes for many files that do
 * not exist (every search path for every resource, ~2500 opens while
 * booting), and each probe used to stat() the path and then read the whole
 * directory to look for a differently-cased spelling: seconds of SD card
 * time.  A directory is now listed once into a hash table keyed by the
 * ASCII-lowercased name; looking a name up costs no file system access, and
 * a name that is not in the listing is known to be missing.  Anything that
 * creates, deletes, moves or renames a file drops the lists
 * (OsSwitch_DirCacheFlush). */
typedef struct DirCache
{
	char path[SWITCH_PATH_MAX];
	char* blob;          // the names, NUL separated
	char** names;        // `count` pointers into blob
	int* table;          // hash slots: name index + 1, 0 = empty
	int count;           // -1: the directory cannot be listed
	uint32_t tableSize;  // a power of two
	struct DirCache* next;
} DirCache_t;

static DirCache_t* gDirCache;
static Mutex gDirLock;

static uint32_t FoldHash(const char* s)
{
	uint32_t h = 2166136261u;
	for(; *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if(c >= 'A' && c <= 'Z')
			c = (unsigned char)(c + 32);
		h = (h ^ c) * 16777619u;
	}
	return h;
}

static void DirCacheFree(DirCache_t* c)
{
	free(c->blob);
	free(c->names);
	free(c->table);
	free(c);
}

void OsSwitch_DirCacheFlush(void)
{
	mutexLock(&gDirLock);
	while(gDirCache)
	{
		DirCache_t* n = gDirCache->next;
		DirCacheFree(gDirCache);
		gDirCache = n;
	}
	mutexUnlock(&gDirLock);
}

// list `path` into a new cache entry (count -1 when it cannot be opened)
static DirCache_t* DirCacheBuild(const char* path)
{
	DirCache_t* c = (DirCache_t*)calloc(1, sizeof *c);
	DIR* d;
	struct dirent* e;
	size_t blobCap = 4096, blobLen = 0, *offs, offCap = 128;
	int i, count = 0;
	if(!c)
		return NULL;
	snprintf(c->path, sizeof c->path, "%s", path);
	d = opendir(path);
	if(!d)
	{
		c->count = -1;
		return c;
	}
	c->blob = (char*)malloc(blobCap);
	offs = (size_t*)malloc(offCap * sizeof *offs);
	if(!c->blob || !offs)
		goto fail;
	while((e = readdir(d)) != NULL)
	{
		size_t n = strlen(e->d_name) + 1;
		if(e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
			continue;
		if(blobLen + n > blobCap)
		{
			char* nb;
			while(blobLen + n > blobCap)
				blobCap *= 2;
			nb = (char*)realloc(c->blob, blobCap);
			if(!nb)
				goto fail;
			c->blob = nb;
		}
		if((size_t)count == offCap)
		{
			size_t* no;
			offCap *= 2;
			no = (size_t*)realloc(offs, offCap * sizeof *offs);
			if(!no)
				goto fail;
			offs = no;
		}
		memcpy(c->blob + blobLen, e->d_name, n);
		offs[count++] = blobLen;
		blobLen += n;
	}
	closedir(d);
	d = NULL;
	c->names = (char**)malloc((size_t)(count ? count : 1) * sizeof *c->names);
	c->tableSize = 16;
	while(c->tableSize < (uint32_t)count * 2u)
		c->tableSize *= 2u;
	c->table = (int*)calloc(c->tableSize, sizeof *c->table);
	if(!c->names || !c->table)
		goto fail;
	for(i = 0; i < count; i++)
	{
		uint32_t slot;
		c->names[i] = c->blob + offs[i];
		slot = FoldHash(c->names[i]) & (c->tableSize - 1u);
		while(c->table[slot])
			slot = (slot + 1u) & (c->tableSize - 1u);
		c->table[slot] = i + 1;
	}
	c->count = count;
	free(offs);
	return c;
fail:
	if(d)
		closedir(d);
	free(offs);
	c->count = -1;
	free(c->blob);
	free(c->names);
	free(c->table);
	c->blob = NULL;
	c->names = NULL;
	c->table = NULL;
	return c;
}

/* Look `name` up in directory `dir`, without regard to ASCII case (the SD
 * card's FAT is case-insensitive, romfs is not).  1: found, and `name` now
 * has the listed spelling (an exact match wins over another case); 0: not in
 * the directory; -1: the directory cannot be listed. */
static int DirCacheLookup(const char* dir, char* name, size_t nameCap)
{
	char key[SWITCH_PATH_MAX];
	DirCache_t* c;
	size_t len;
	int result = 0;
	uint32_t slot;
	const char* ci = NULL;

	snprintf(key, sizeof key, "%s", dir);
	len = strlen(key);
	while(len > 1 && key[len - 1] == '/' && key[len - 2] != ':')
		key[--len] = 0;

	mutexLock(&gDirLock);
	for(c = gDirCache; c; c = c->next)
		if(strcmp(c->path, key) == 0)
			break;
	if(!c)
	{
		gSwitchFsStats.caseScans++; // a directory listed (was: one scan per probe)
		c = DirCacheBuild(key);
		if(!c)
		{
			mutexUnlock(&gDirLock);
			return -1;
		}
		c->next = gDirCache;
		gDirCache = c;
	}
	if(c->count < 0)
		result = -1;
	else
	{
		slot = FoldHash(name) & (c->tableSize - 1u);
		while(c->table[slot])
		{
			const char* cand = c->names[c->table[slot] - 1];
			if(strcmp(cand, name) == 0)
			{
				ci = cand;
				break;
			}
			if(!ci && strcasecmp(cand, name) == 0)
				ci = cand;
			slot = (slot + 1u) & (c->tableSize - 1u);
		}
		if(ci)
		{
			snprintf(name, nameCap, "%s", ci);
			result = 1;
		}
	}
	mutexUnlock(&gDirLock);
	return result;
}

/* 1 when the native path is known not to exist: its directory is listed and
 * has no such entry (nothing is tried on the file system for it) */
int OsSwitch_NativeMissing(const char* path)
{
	char dir[SWITCH_PATH_MAX], name[SWITCH_PATH_MAX];
	const char* slash = strrchr(path, '/');
	size_t dl;
	if(!slash || !slash[1] || slash == path)
		return 0;
	dl = (size_t)(slash - path) + 1;
	if(dl >= sizeof dir)
		return 0;
	memcpy(dir, path, dl);
	dir[dl] = 0;
	snprintf(name, sizeof name, "%s", slash + 1);
	return DirCacheLookup(dir, name, sizeof name) == 0;
}

/* Shift-JIS Windows path -> UTF-8 Switch path.  `write` selects the SD
 * root; reads also get their components case-corrected. */
static void ConvertPath(const char* win, char* out, size_t n, int write, int* missing)
{
	int gone = 0; // a component was not found: nothing below it can exist
	char utf[SWITCH_PATH_MAX * 2];
	char cur[SWITCH_PATH_MAX];
	const char* p;
	size_t o = 0;

	if(!win)
		win = "";
	OsSwitch_SjisToUtf8(win, utf, sizeof utf);
	p = utf;

	// an already native path is taken as it is
	if(strncmp(p, "romfs:/", 7) == 0 || strncmp(p, "sdmc:/", 6) == 0)
	{
		snprintf(out, n, "%s", p);
		return;
	}

	if(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
	{
		p += 2;
		if(*p == '\\' || *p == '/')
			p++;
	}
	while(*p == '\\' || *p == '/')
		p++;

	if(write)
		snprintf(cur, sizeof cur, "%s", gWriteRoot);
	else
		WithSlash(cur, sizeof cur, gGameRoot);
	o = strlen(cur);

	while(*p)
	{
		char comp[SWITCH_PATH_MAX];
		size_t c = 0;
		while(*p && *p != '\\' && *p != '/' && c + 1 < sizeof comp)
			comp[c++] = *p++;
		comp[c] = 0;
		while(*p == '\\' || *p == '/')
			p++;
		if(c == 0)
			continue;
		if(!write && !gone && strcmp(comp, ".") != 0 && strcmp(comp, "..") != 0)
		{
			if(DirCacheLookup(cur, comp, sizeof comp) == 0)
			{
				gone = 1;
				if(missing)
					*missing = 1;
			}
		}
		if(o && cur[o - 1] != '/')
			cur[o++] = '/';
		snprintf(cur + o, sizeof cur - o, "%s", comp);
		o = strlen(cur);
	}
	snprintf(out, n, "%s", cur);
}

// *missing is set when a read path is known not to exist
void OsSwitch_PathEx(const char* win, char* out, size_t n, int* missing)
{
	uint64_t t = OsSwitch_TicksNs();
	if(missing)
		*missing = 0;
	ConvertPath(win, out, n, 0, missing);
	gSwitchFsStats.pathNs += OsSwitch_TicksNs() - t;
	gSwitchFsStats.paths++;
}

void OsSwitch_Path(const char* win, char* out, size_t n)
{
	OsSwitch_PathEx(win, out, n, NULL);
}

void OsSwitch_PathWrite(const char* win, char* out, size_t n)
{
	ConvertPath(win, out, n, 1, NULL);
}

void OS_PathNative(const char* path, char* out, size_t n)
{
	OsSwitch_Path(path, out, n);
}
