/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the process entry point
 *
 * The original is WinMain; Engine_Main in engine.c (bgi/engine.h) is that
 * function without the Windows glue.  This file only turns the host's
 * entry point into a call of Engine_Main with the command line as one
 * string, which the engine parses itself (Engine_Main strips the
 * reimplementation's "--" options, Boot_ParseCmdLine understands the
 * original's switches, e.g. "Execute as a launcher.").
 */
#include "bgi/engine.h"
#include "bgi/os.h"

#if defined(BGI_WIN32)
#include "bgi/os_win32.h" // windows.h, and the back end's command line

/* the Windows entry point.  WinMain's own command line is in the system's
 * code page, which may not be able to spell a path; the back end converts
 * the process's wide one instead (OsWin32_CommandLine: without the
 * program name, in the engine's encoding). */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
	char cmd[0x1000];
	int r;
	BGI_UNUSED(hInst);
	BGI_UNUSED(hPrev);
	BGI_UNUSED(cmdLine);
	BGI_UNUSED(show);
	if(!OS_Init())
		return -1;
	OsWin32_CommandLine(cmd, sizeof cmd);
	r = Engine_Main(cmd);
	OS_Shutdown();
	return r;
}

#else

/* the POSIX entry point: the arguments are rejoined into one command line,
 * as GetCommandLine would give it without the program name - joined by
 * single spaces, an argument with a space or a tab in it (a game directory
 * "VN/[August] Fortune Arterial", Do not use mutex.) in double quotes
 * unless it already is in them.  Arguments that do not fit the buffer are
 * dropped. */
#ifdef BGI_SWITCH
void OsSwitch_FixArgs(int* argc, char*** argv); // os/switch/process.c
#endif

int main(int argc, char** argv)
{
	char cmdLine[0x1000];
	size_t len = 0;
	int i, r;
	cmdLine[0] = 0;
#ifdef BGI_SWITCH
	// a forwarder leaves the NRO path unquoted and libnx splits it at the spaces
	OsSwitch_FixArgs(&argc, &argv);
#endif
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		size_t n = strlen(a);
		int quote = strpbrk(a, " \t") != NULL && !(n >= 2 && a[0] == '"' && a[n - 1] == '"');
		if(len + n + 4 >= sizeof cmdLine)
			break;
		if(len)
			cmdLine[len++] = ' ';
		if(quote)
			cmdLine[len++] = '"';
		memcpy(cmdLine + len, a, n);
		len += n;
		if(quote)
			cmdLine[len++] = '"';
		cmdLine[len] = 0;
	}
	// a launcher may pass no arguments at all (libnx then gives argc 0 and argv NULL)
	OS_SetArgv0(argc > 0 && argv ? argv[0] : NULL);
	if(!OS_Init())
		return -1;
	r = Engine_Main(cmdLine);
	OS_Shutdown();
	return r;
}

#endif
