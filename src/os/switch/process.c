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
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

AppletHookCookie gSwitchAppletHookCookie;
int gSwitchShouldClose = 0;
int gSwitchFocusEvent = 0; // the applet focus state changed (OS_PumpMessages raises the activation)

static char gBinaryDir[512] = "sdmc:/switch/openbgi/";
static char gSwitchArgv0[1024];
static FILE* gSwitchLog;

void OsSwitch_Log(const char* format, ...)
{
	va_list args;
	static char startupBuf[128][256];
	static uint64_t t0;
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
		if(startupCount < 128)
		{ // every line carries the time since the first one (the start of main)
			uint64_t now = OsSwitch_TicksNs();
			if(!t0)
				t0 = now;
			snprintf(startupBuf[startupCount], sizeof startupBuf[0], "[%6llu ms] %s", (unsigned long long)((now - t0) / 1000000ull), lineBuf);
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

/* Launchers hand the NRO path over in different shapes: in quotes, with
 * backslashes, or without the "sdmc:" device.  Normalise it in place
 * (`cap` leaves room for the "sdmc:" that may be added). */
static void SwitchCleanPath(char* p, size_t cap)
{
	size_t len, start = 0;
	char* c;
	while(p[start] == ' ' || p[start] == '\t')
		start++;
	if(start)
		memmove(p, p + start, strlen(p + start) + 1);
	len = strlen(p);
	while(len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
		p[--len] = 0;
	if(len && p[len - 1] == '"')
		p[--len] = 0;
	if(p[0] == '"')
		memmove(p, p + 1, len--);
	for(c = p; *c; c++)
		if(*c == '\\')
			*c = '/';
	if(p[0] == '/' && len + 6 <= cap)
	{
		memmove(p + 5, p, len + 1);
		memcpy(p, "sdmc:", 5);
	}
}

static int SwitchTokenEndsNro(const char* t)
{
	size_t n = strlen(t);
	while(n && (t[n - 1] == '"' || t[n - 1] == ' '))
		n--;
	return n >= 4 && strncasecmp(t + n - 4, ".nro", 4) == 0;
}

#define SWITCH_FIX_MAX 32
static char* gSwitchFixArgv[SWITCH_FIX_MAX + 1];
static char gSwitchFixBuf[4096];

// the tokens a..b of `in` joined by single spaces, copied into the static buffer; NULL when it is full
static char* SwitchFixJoin(char** in, int a, int b, size_t* used)
{
	char* start = gSwitchFixBuf + *used;
	size_t pos = *used;
	int i;
	for(i = a; i <= b; i++)
	{
		size_t n = strlen(in[i]);
		if(pos + n + 8 >= sizeof gSwitchFixBuf)
			return NULL;
		if(i > a)
			gSwitchFixBuf[pos++] = ' ';
		memcpy(gSwitchFixBuf + pos, in[i], n);
		pos += n;
	}
	gSwitchFixBuf[pos++] = 0;
	*used = pos + 8; // slack for the "sdmc:" that SwitchCleanPath may prepend
	return start;
}

/* Put the launch arguments back together.  A forwarder builds the argument
 * string of the NRO it starts by hand and usually leaves the path unquoted;
 * libnx then splits it at the spaces, so
 *     sdmc:/switch/Wonderful Everyday/bgi.nro
 * arrives as "sdmc:/switch/Wonderful" and "Everyday/bgi.nro" (and the
 * engine would take the second for a game path).  The path ends at the
 * first token that ends in ".nro".  What follows is regrouped the same way:
 * consecutive plain tokens are one argument (a game directory with
 * spaces), a token starting with '-' is an option of its own. */
void OsSwitch_FixArgs(int* argc, char*** argv)
{
	char** in;
	size_t used = 0;
	int n, first, i, out = 0;
	if(!argc || !argv || *argc <= 0 || !*argv)
		return;
	in = *argv;
	n = *argc;
	for(i = 0; i < n; i++)
		OsSwitch_Log("launch argv[%d]='%s'", i, in[i] ? in[i] : "(null)");
	for(i = 0; i < n; i++)
		if(!in[i])
			return;
	for(first = 0; first < n && !SwitchTokenEndsNro(in[first]); first++)
		;
	if(first >= n)
		first = 0; // no token names an NRO: keep the first as it is
	gSwitchFixArgv[0] = SwitchFixJoin(in, 0, first, &used);
	if(!gSwitchFixArgv[0])
		return;
	SwitchCleanPath(gSwitchFixArgv[0], strlen(gSwitchFixArgv[0]) + 8);
	for(i = first + 1; i < n && out < SWITCH_FIX_MAX - 1;)
	{
		int j = i;
		if(in[i][0] != '-' && !strpbrk(in[i], " \t"))
			while(j + 1 < n && in[j + 1][0] != '-' && !strpbrk(in[j + 1], " \t"))
				j++;
		gSwitchFixArgv[++out] = SwitchFixJoin(in, i, j, &used);
		if(!gSwitchFixArgv[out])
			return;
		i = j + 1;
	}
	gSwitchFixArgv[out + 1] = NULL;
	*argc = out + 1;
	*argv = gSwitchFixArgv;
	for(i = 0; i <= out; i++)
		OsSwitch_Log("fixed argv[%d]='%s'", i, gSwitchFixArgv[i]);
}

/* When the NRO that was launched sits beside ipl._bp or system.arc, its own
 * directory is the game root (instead of the empty default romfs:/ root).
 * Nothing else is searched: several games may be installed side by side, and
 * only the folder of this NRO, or a folder given as an argument (which the
 * engine applies later, Boot_ParseCmdLine), says which one to run. */
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
		OsSwitch_Log("no ipl._bp or system.arc in '%s': give the game folder as an argument, e.g. \"sdmc:/switch/My Game\"", dir);
}

void OsSwitch_AppletHook(AppletHookType hook, void* param)
{
	BGI_UNUSED(param);
	if(hook == AppletHookType_OnExitRequest)
		gSwitchShouldClose = 1;
	else if(hook == AppletHookType_OnFocusState)
		gSwitchFocusEvent = 1;
}

/* romfsInit() could not find the NRO's romfs.  Read the NRO's asset header
 * (it follows the NRO image: "ASET", version, then the icon, nacp and romfs
 * sections as offset/size pairs relative to the header) and mount the romfs
 * from the cleaned-up NRO path. */
static Result SwitchMountRomfsFromNro(void)
{
	u8 hdr[0x38];
	u32 nroSize = 0, magic = 0;
	u64 romfsOff = 0;
	FILE* f;
	if(!gSwitchArgv0[0])
		return MAKERESULT(Module_Libnx, LibnxError_NotFound);
	f = fopen(gSwitchArgv0, "rb");
	if(!f)
	{
		OsSwitch_Log("romfs fallback: cannot open '%s'", gSwitchArgv0);
		return MAKERESULT(Module_Libnx, LibnxError_NotFound);
	}
	if(fseek(f, 0x18, SEEK_SET) != 0 || fread(&nroSize, 4, 1, f) != 1 || fseek(f, (long)nroSize, SEEK_SET) != 0 ||
	   fread(hdr, sizeof hdr, 1, f) != 1)
	{
		fclose(f);
		return MAKERESULT(Module_Libnx, LibnxError_NotFound);
	}
	fclose(f);
	memcpy(&magic, hdr, 4);
	if(magic != 0x54455341u) // "ASET"
	{
		OsSwitch_Log("romfs fallback: '%s' has no asset header", gSwitchArgv0);
		return MAKERESULT(Module_Libnx, LibnxError_NotFound);
	}
	memcpy(&romfsOff, hdr + 0x28, 8);
	return romfsMountFromFsdev(gSwitchArgv0, (u64)nroSize + romfsOff, "romfs");
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
	{ // how the process was started: a forwarder differs from the Homebrew Menu in these
		u64 total = 0, used = 0;
		svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
		OsSwitch_Log("applet type=%d heap override=%llu MiB memory %llu/%llu MiB used", (int)appletGetAppletType(),
			(unsigned long long)(envGetHeapOverrideSize() >> 20), (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
	}
	OsSwitch_CoresInit();
	if(R_FAILED(rc))
	{ // libnx opens argv[0] for the NRO's own romfs; a launcher that mangled it leaves that to us
		rc = SwitchMountRomfsFromNro();
		OsSwitch_Log("romfs mounted from the NRO file by path: result 0x%x", (unsigned)rc);
	}
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
	snprintf(gSwitchArgv0, sizeof gSwitchArgv0 - 8, "%s", argv0 ? argv0 : "");
	SwitchCleanPath(gSwitchArgv0, sizeof gSwitchArgv0);
	// logged before the file exists: OsSwitch_Log keeps the lines until it does
	OsSwitch_Log("argv0=%s envHasArgv=%d", argv0 ? "given" : "NULL (no arguments from the launcher)", (int)envHasArgv());
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

/* "sdmc:/..." and "romfs:/..." are absolute (without this a game folder given as an
 * argument became "<cwd>/sdmc:/..." and was never found) */
static int SwitchHasDevice(const char* p)
{
	const char* colon = strchr(p, ':');
	const char* slash = strchr(p, '/');
	return colon && colon > p + 1 && (!slash || colon < slash);
}

void OS_NativeAbsolute(const char* path, char* out, size_t n)
{
	char cwd[1024];
	if(path[0] == '/' || SwitchHasDevice(path) || !getcwd(cwd, sizeof cwd))
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
