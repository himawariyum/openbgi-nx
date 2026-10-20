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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

AppletHookCookie gSwitchAppletHookCookie;
int gSwitchShouldClose = 0;

static char gBinaryDir[512] = "sdmc:/switch/openbgi/";

void OsSwitch_AppletHook(AppletHookType hook, void* param)
{
	BGI_UNUSED(param);
	if(hook == AppletHookType_OnExitRequest)
		gSwitchShouldClose = 1;
}

int OS_Init(void)
{
	Result rc = romfsInit();
	if(R_FAILED(rc))
		printf("romfsInit failed: 0x%x (continuing without romfs)\n", rc);

	appletHook(&gSwitchAppletHookCookie, OsSwitch_AppletHook, NULL);

	// Make sure the writable root exists.
	mkdir("sdmc:/switch", 0777);
	mkdir("sdmc:/switch/openbgi", 0777);

	OsSwitch_InputInit();

	if(!OsSwitch_GraphicsInit(1280, 720))
	{
		printf("Failed to initialize graphics\n");
		return 0;
	}

	return 1;
}

void OS_Shutdown(void)
{
	OsSwitch_FontShutdown();
	OsSwitch_GraphicsShutdown();
	OsSwitch_InputShutdown();
	appletUnhook(&gSwitchAppletHookCookie);
	romfsExit();
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
	BGI_UNUSED(argv0);
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
