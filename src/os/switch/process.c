/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * process.c - Switch back end: start-up, shutdown, the game and binary
 *             directories, and the native file services of inc/bgi/os.h
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

AppletHookCookie gSwitchAppletHookCookie;
int gSwitchShouldClose = 0;

static char gBinaryDir[512] = "sdmc:/switch/openbgi/";
static char gSwitchArgv0[1024];
static FILE* gSwitchLog;

void OsSwitch_Log(const char* format, ...)
{
	va_list args;
	static char startupBuf[32][256];
	static int startupCount = 0;
	static char hidRing[30][256];
	static int hidHead = 0;
	static int hidCount = 0;
	char lineBuf[256];
	int isHid = 0;
	int i, idx;

	if(!format)
		return;
	va_start(args, format);
	vsnprintf(lineBuf, sizeof lineBuf, format, args);
	va_end(args);

	if(strncmp(lineBuf, "INPUT:", 6) == 0 || strncmp(lineBuf, "OsSwitch_Raise", 14) == 0)
	{
		isHid = 1;
		strcpy(hidRing[hidHead], lineBuf);
		hidHead = (hidHead + 1) % 30;
		if(hidCount < 30)
			hidCount++;
	}
	else
	{
		if(startupCount < 32)
		{
			strcpy(startupBuf[startupCount], lineBuf);
			startupCount++;
		}
	}

	if(gSwitchLog)
	{
		rewind(gSwitchLog);
		ftruncate(fileno(gSwitchLog), 0);
		for(i = 0; i < startupCount; i++)
		{
			fprintf(gSwitchLog, "%s\n", startupBuf[i]);
		}
		for(i = 0; i < hidCount; i++)
		{
			idx = (hidHead - hidCount + i + 30) % 30;
			fprintf(gSwitchLog, "%s\n", hidRing[idx]);
		}
		fflush(gSwitchLog);
	}
}

static int SwitchDirHasBootFile(const char* dir, const char* name)
{
	char path[1200];
	struct stat st;
	int n = snprintf(path, sizeof path, "%s/%s", dir, name);
	return n > 0 && (size_t)n < sizeof path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Homebrew Menu and forwarders commonly launch an NRO without a game path
 * argument.  When the NRO sits beside ipl._bp or system.arc, use that
 * directory as the game root instead of the empty default romfs:/ root. */
static void SwitchAutoSelectGameRoot(void)
{
	char dir[1024];
	char* slash;
	dir[0] = 0;
	if(gSwitchArgv0[0])
	{
		snprintf(dir, sizeof dir, "%s", gSwitchArgv0);
		slash = strrchr(dir, '/');
		if(!slash)
			slash = strrchr(dir, '\\');
		if(slash)
		{
			if((slash == dir + 5 && strncmp(dir, "sdmc:", 5) == 0) ||
			   (slash == dir + 6 && strncmp(dir, "romfs:", 6) == 0))
				slash[1] = 0;
			else
				*slash = 0;
		}
		else
			dir[0] = 0;
	}
	if(!dir[0] && !getcwd(dir, sizeof dir))
	{
		OsSwitch_Log("auto game root: argv0 has no directory and getcwd failed");
		return;
	}
	OsSwitch_Log("argv0='%s'; candidate game dir='%s'", gSwitchArgv0, dir);
	if(SwitchDirHasBootFile(dir, "ipl._bp") || SwitchDirHasBootFile(dir, "system.arc"))
	{
		OsSwitch_SetGameRoot(dir);
		OsSwitch_Log("auto game root selected: %s", dir);
	}
	else
		OsSwitch_Log("no ipl._bp or system.arc beside NRO/current directory; game root remains romfs:/");
}

void OsSwitch_AppletHook(AppletHookType hook, void* param)
{
	BGI_UNUSED(param);
	if(hook == AppletHookType_OnExitRequest)
		gSwitchShouldClose = 1;
}

int OS_Init(void)
{
	Result rc = romfsInit();
	char logPath[1024];
	char* slash;

	appletHook(&gSwitchAppletHookCookie, OsSwitch_AppletHook, NULL);

	// Put startup.log in the same directory as the NRO/game if possible
	snprintf(logPath, sizeof logPath, "%s", gSwitchArgv0);
	slash = strrchr(logPath, '/');
	if(!slash)
		slash = strrchr(logPath, '\\');
	if(slash)
	{
		strcpy(slash + 1, "startup.log");
		gSwitchLog = fopen(logPath, "wb");
	}
	if(!gSwitchLog)
	{
		mkdir("sdmc:/switch", 0777);
		mkdir("sdmc:/switch/openbgi", 0777);
		gSwitchLog = fopen("sdmc:/switch/openbgi/startup.log", "wb");
	}

	OsSwitch_Log("OpenBGI Switch startup log");
	OsSwitch_Log("romfsInit result: 0x%x", (unsigned)rc);
	if(R_FAILED(rc))
		printf("romfsInit failed: 0x%x (continuing without romfs)\n", rc);
	SwitchAutoSelectGameRoot();

	OsSwitch_InputInit();
	OsSwitch_Log("input initialized");

	if(!OsSwitch_GraphicsInit(1280, 720))
	{
		OsSwitch_Log("OS_Init: OsSwitch_GraphicsInit FAILED");
		printf("Failed to initialize graphics\n");
		if(gSwitchLog)
		{
			fclose(gSwitchLog);
			gSwitchLog = NULL;
		}
		return 0;
	}
	OsSwitch_Log("OS_Init: graphics initialized; returning success");

	return 1;
}

void OS_Shutdown(void)
{
	OsSwitch_Log("OS_Shutdown: begin");
	OsSwitch_FontShutdown();
	OsSwitch_GraphicsShutdown();
	OsSwitch_InputShutdown();
	appletUnhook(&gSwitchAppletHookCookie);
	romfsExit();
	OsSwitch_Log("OS_Shutdown: complete");
	if(gSwitchLog)
	{
		fclose(gSwitchLog);
		gSwitchLog = NULL;
	}
}

// The engine's base directory is the pseudo drive "C:\".
void OS_ExeDir(char* buf, size_t n)
{
	snprintf(buf, n, "C:\\");
}

void OS_BinaryDir(char* buf, size_t n)
{
	snprintf(buf, n, "%s", gBinaryDir);
}

void OS_SetArgv0(const char* argv0)
{
	snprintf(gSwitchArgv0, sizeof gSwitchArgv0, "%s", argv0 ? argv0 : "");
}

void OS_GameDir(char* buf, size_t n)
{
	OsSwitch_GameRoot(buf, n);
}

int OS_SetGameDir(const char* dir)
{
	char native[512];
	struct stat st;
	if(!dir || !*dir)
		return 0;
	if(strncmp(dir, "romfs:", 6) == 0 || strncmp(dir, "sdmc:", 5) == 0)
		snprintf(native, sizeof native, "%s", dir);
	else if(dir[0] == '/' || (dir[0] && dir[1] == ':'))
		snprintf(native, sizeof native, "%s", dir);
	else
		snprintf(native, sizeof native, "sdmc:/switch/openbgi/%s", dir);
	if(stat(native, &st) != 0 || !S_ISDIR(st.st_mode))
		return 0;
	OsSwitch_SetGameRoot(native);
	return 1;
}

void OS_NativeToUtf8(const char* native, char* out, size_t n)
{
	snprintf(out, n, "%s", native);
}

void OS_Utf8ToNative(const char* utf8, char* out, size_t n)
{
	snprintf(out, n, "%s", utf8);
}

FILE* OS_NativeOpen(const char* path, const char* mode)
{
	return fopen(path, mode);
}

uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size)
{
	FILE* f = fopen(path, "rb");
	long len;
	uint8_t* data;
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len < 0)
	{
		fclose(f);
		return NULL;
	}
	data = (uint8_t*)malloc((size_t)len + 1);
	if(!data)
	{
		fclose(f);
		return NULL;
	}
	if(len && fread(data, 1, (size_t)len, f) != (size_t)len)
	{
		free(data);
		fclose(f);
		return NULL;
	}
	data[len] = 0;
	fclose(f);
	if(size)
		*size = (uint32_t)len;
	return data;
}

int OS_WriteNativeFile(const char* path, const void* data, uint32_t size)
{
	FILE* f = fopen(path, "wb");
	if(!f)
		return 0;
	if(size && fwrite(data, 1, size, f) != size)
	{
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

int OS_NativeStat(const char* path, OsNativeStat_t* out)
{
	struct stat st;
	if(!path || stat(path, &st) != 0)
		return OS_NATIVE_NONE;
	if(out)
	{
		out->size = (uint64_t)st.st_size;
		out->mtime = (int64_t)st.st_mtime * 1000;
	}
	return S_ISDIR(st.st_mode) ? OS_NATIVE_DIR : OS_NATIVE_FILE;
}

int OS_NativeMkdir(const char* path)
{
	if(mkdir(path, 0777) == 0)
		return 1;
	return OS_NativeStat(path, NULL) == OS_NATIVE_DIR;
}

int OS_NativeRemove(const char* path)
{
	return remove(path) == 0 || OS_NativeStat(path, NULL) == OS_NATIVE_NONE;
}

int OS_NativeList(const char* dir, OsNativeListFn fn, void* ctx)
{
	DIR* d = opendir(dir[0] ? dir : ".");
	struct dirent* e;
	int count = 0;
	if(!d)
		return -1;
	while((e = readdir(d)) != NULL)
	{
		char path[1024];
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof path, "%s/%s", dir[0] ? dir : ".", e->d_name);
		fn(ctx, e->d_name, OS_NativeStat(path, NULL) == OS_NATIVE_DIR);
		count++;
	}
	closedir(d);
	return count;
}

int OS_NativeSeek(FILE* f, uint64_t pos)
{
	return fseek(f, (long)pos, SEEK_SET) == 0;
}

void OS_NativeAbsolute(const char* path, char* out, size_t n)
{
	char cwd[1024];
	if(path[0] == '/' || !getcwd(cwd, sizeof cwd))
		snprintf(out, n, "%s", path);
	else if(strcmp(path, ".") == 0 || !path[0])
		snprintf(out, n, "%s", cwd);
	else
		snprintf(out, n, "%s/%s", cwd, path);
}

int OS_WorkerLimit(void)
{
	return 1; // the engine's workers fork(); Switch runs them one after the other
}

int OS_RunWorkers(int n, int (*fn)(int index, void* ctx), void* ctx)
{
	int i, failed = 0;
	for(i = 0; i < n; i++)
		failed += fn(i, ctx) != 0;
	return failed;
}

void OS_NativeArgs(int* argc, char*** argv)
{
	BGI_UNUSED(argc);
	BGI_UNUSED(argv);
}

int OS_SetCurrentDir(const char* dir)
{
	return chdir(dir) == 0;
}

struct OsMutex
{
	int dummy;
};

OsMutex_t* OS_InstanceMutexCreate(const char* name, int* alreadyExists)
{
	OsMutex_t* m = (OsMutex_t*)calloc(1, sizeof *m);
	BGI_UNUSED(name);
	if(alreadyExists)
		*alreadyExists = 0;
	return m;
}

void OS_InstanceMutexRelease(OsMutex_t* m)
{
	free(m);
}

int OS_NamedMutexExists(const char* name)
{
	BGI_UNUSED(name);
	return 0;
}
