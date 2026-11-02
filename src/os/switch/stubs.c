/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * stubs.c - Switch back end: services the console cannot offer (CD audio,
 *           DirectShow movies, child processes, shell integration, native
 *           dialogs, the installer's resource dialogs) and the registry
 *           emulation on a file in the writable directory.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <string.h>

#define SWITCH_REG_FILE "sdmc:/switch/openbgi/registry.ini"

// ---- CD audio / sound files -------------------------------------------------

uint32_t OS_MciOpenCdAudio(const char* element)
{
	BGI_UNUSED(element);
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(item);
	if(out)
		*out = 0;
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(from);
	BGI_UNUSED(to);
	BGI_UNUSED(notify);
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_PlaySoundFile(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}

// ---- movies -----------------------------------------------------------------

int OS_MovieOpen(const char* path)
{
	OsSwitch_Log("OS_MovieOpen: no native Switch video backend; path='%s'", path ? path : "");
	return OS_MOVIE_FAILED;
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(volume);
	if(lengthMs)
		*lengthMs = 0;
	return 0;
}

void OS_MovieStop(void)
{
}

int OS_MovieIsRunning(void)
{
	return 0;
}

void OS_MovieSetVolume(int32_t vol)
{
	BGI_UNUSED(vol);
}

// ---- child processes / pipes ------------------------------------------------

OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	BGI_UNUSED(cmdLine);
	return NULL;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	BGI_UNUSED(p);
	BGI_UNUSED(ms);
	return 1;
}

void OS_ProcessClose(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

OsPipeProc_t* OS_PipeProcStart(const char* const* argv)
{
	OsSwitch_Log("OS_PipeProcStart: external process backend unavailable; program='%s'", argv && argv[0] ? argv[0] : "");
	return NULL;
}

int OS_PipeProcRead(OsPipeProc_t* p, void* buf, int n)
{
	BGI_UNUSED(p);
	BGI_UNUSED(buf);
	BGI_UNUSED(n);
	return -1;
}

void OS_PipeProcStop(OsPipeProc_t* p)
{
	BGI_UNUSED(p);
}

// ---- shell ------------------------------------------------------------------

int OS_CreateShortcut(const char* linkPath, const char* target)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	return 0;
}

int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0;
}

int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	BGI_UNUSED(file);
	BGI_UNUSED(params);
	BGI_UNUSED(dir);
	return 0;
}

int OS_ClipboardSetText(const char* text)
{
	BGI_UNUSED(text);
	return 0;
}

int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	BGI_UNUSED(path);
	BGI_UNUSED(stretch);
	BGI_UNUSED(tile);
	return 0;
}

void OS_ShellNotifyAssocChanged(void)
{
}

int OS_IpcSendToRunning(const char* windowClass, const char* data)
{
	BGI_UNUSED(windowClass);
	BGI_UNUSED(data);
	return 0;
}

// ---- registry ---------------------------------------------------------------

int OS_RegReadString(const char* key, const char* value, char* buf, size_t n)
{
	return OsCommon_RegFileRead(SWITCH_REG_FILE, key, value, buf, n);
}

int OS_RegCreateKey(const char* key, const char* cls)
{
	BGI_UNUSED(cls);
	return OsCommon_RegFileWrite(SWITCH_REG_FILE, key, NULL, "");
}

int OS_RegWriteString(const char* key, const char* value, const char* s)
{
	return OsCommon_RegFileWrite(SWITCH_REG_FILE, key, value, s ? s : "");
}

int OS_RegDeleteKey(const char* key)
{
	return OsCommon_RegFileDeleteKey(SWITCH_REG_FILE, key);
}

// ---- dialogs ----------------------------------------------------------------

int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	OsSwitch_Log("OS_MessageBox: caption='%s' text='%s' flags=0x%x", caption ? caption : "", text ? text : "", flags);
	BGI_UNUSED(text);
	BGI_UNUSED(caption);
	switch(flags & 0xf)
	{
	case OS_MB_YESNO:
		return OS_IDYES;
	case OS_MB_RETRYCANCEL:
		return OS_IDRETRY;
	case OS_MB_OKCANCEL:
	default:
		return OS_IDOK;
	}
}

int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir,
	const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(filter);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(descs);
	BGI_UNUSED(pats);
	BGI_UNUSED(count);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	return 0;
}

int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	BGI_UNUSED(maxLen);
	BGI_UNUSED(digits);
	BGI_UNUSED(out);
	return 0;
}

int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	BGI_UNUSED(title);
	BGI_UNUSED(label1);
	BGI_UNUSED(initial1);
	BGI_UNUSED(maxLen1);
	BGI_UNUSED(digits1);
	BGI_UNUSED(out1);
	BGI_UNUSED(label2);
	BGI_UNUSED(initial2);
	BGI_UNUSED(maxLen2);
	BGI_UNUSED(digits2);
	BGI_UNUSED(out2);
	BGI_UNUSED(compact);
	return 0;
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	BGI_UNUSED(surname);
	BGI_UNUSED(givenName);
	BGI_UNUSED(nickname);
	BGI_UNUSED(pronoun);
	BGI_UNUSED(month);
	BGI_UNUSED(day);
	return 0;
}

int OS_LauncherDialog(OsLauncher_t* l)
{
	BGI_UNUSED(l);
	OsSwitch_Log("OS_LauncherDialog called: Switch launcher UI is not implemented; returning -1");
	return -1;
}

int OS_InstallerDialog(int id, void* ctx)
{
	BGI_UNUSED(id);
	BGI_UNUSED(ctx);
	return 0;
}

// ---- machine identity -------------------------------------------------------

void OS_MemoryStatus(OsMemStatus_t* m)
{
	if(!m)
		return;
	memset(m, 0, sizeof *m);
	m->loadPercent = 50;
	m->totalPhys = 0xf0000000u; // 4 GB, capped
	m->availPhys = 0x70000000u;
	m->totalPageFile = m->totalPhys;
	m->availPageFile = m->availPhys;
	m->totalVirtual = 0xf0000000u;
	m->availVirtual = m->availPhys;
}

void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	if(totalPhys)
		*totalPhys = 0xf0000000ull;
	if(availPhys)
		*availPhys = 0x70000000ull;
}

int OS_ComputerName(char* buf, size_t n)
{
	if(buf && n)
		snprintf(buf, n, "Switch");
	return 1;
}

int OS_UserName(char* buf, size_t n)
{
	if(buf && n)
		snprintf(buf, n, "Player");
	return 1;
}

int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
	BGI_UNUSED(leaf);
	if(out)
		memset(out, 0, sizeof(uint32_t) * 4);
	return 0;
}

uint64_t OS_ReadTsc(void)
{
	return 0;
}

uint32_t OS_CpuSignature(void)
{
	return 0;
}

int OS_Version(int* platform, int* major, int* minor)
{
	if(platform)
		*platform = 2; // NT paths
	if(major)
		*major = 10;
	if(minor)
		*minor = 0;
	return 1;
}

void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	if(out)
	{
		out[0] = 10;
		out[1] = 0;
		out[2] = 0;
		out[3] = 2;
	}
	if(csd && n)
		csd[0] = 0;
}

int OS_UiLanguage(void)
{
	return 0x0411; // Japanese (Japan)
}

int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	if(desc && n)
		snprintf(desc, n, "NVIDIA Tegra X1");
	if(version)
		memset(version, 0, sizeof(uint32_t) * 4);
	return 1;
}

int OS_SpecialFolder(int csidl, char* buf, size_t n)
{
	BGI_UNUSED(csidl);
	BGI_UNUSED(buf);
	BGI_UNUSED(n);
	return 0;
}

int OS_WindowsDir(char* buf, size_t n)
{
	if(buf && n)
		snprintf(buf, n, "C:\\");
	return 1;
}
