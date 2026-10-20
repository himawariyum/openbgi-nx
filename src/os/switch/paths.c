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

/* one entry of `dir` that matches `name` without regard to ASCII case;
 * `name` is replaced in place by the entry's spelling when a match exists */
static void MatchCase(const char* dir, char* name)
{
	DIR* d = opendir(dir[0] ? dir : ".");
	struct dirent* e;
	if(!d)
		return;
	while((e = readdir(d)) != NULL)
	{
		if(strcasecmp(e->d_name, name) == 0)
		{
			snprintf(name, strlen(name) + 1, "%s", e->d_name);
			break;
		}
	}
	closedir(d);
}

/* Shift-JIS Windows path -> UTF-8 Switch path.  `write` selects the SD
 * root; reads also get their components case-corrected. */
static void ConvertPath(const char* win, char* out, size_t n, int write)
{
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
		if(!write && strcmp(comp, ".") != 0 && strcmp(comp, "..") != 0)
		{
			char probe[SWITCH_PATH_MAX];
			struct stat st;
			snprintf(probe, sizeof probe, "%s%s", cur, comp);
			if(stat(probe, &st) != 0)
				MatchCase(cur, comp);
		}
		if(o && cur[o - 1] != '/')
			cur[o++] = '/';
		snprintf(cur + o, sizeof cur - o, "%s", comp);
		o = strlen(cur);
	}
	snprintf(out, n, "%s", cur);
}

void OsSwitch_Path(const char* win, char* out, size_t n)
{
	ConvertPath(win, out, n, 0);
}

void OsSwitch_PathWrite(const char* win, char* out, size_t n)
{
	ConvertPath(win, out, n, 1);
}

void OS_PathNative(const char* path, char* out, size_t n)
{
	OsSwitch_Path(path, out, n);
}
