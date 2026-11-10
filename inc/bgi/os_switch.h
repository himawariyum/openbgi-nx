/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_switch.h - declarations shared by the files of the Nintendo Switch
 *               back end (src/os/switch/); nothing above the OS layer
 *               includes this.
 */
#ifndef BGI_OS_SWITCH_H_
#define BGI_OS_SWITCH_H_

#include "bgi/common.h"

#include <switch.h>
#include <deko3d.h>

// ---- Graphics (deko3d) ------------------------------------------------------
extern DkDevice gSwitchDevice;
extern DkQueue gSwitchQueue;
extern DkSwapchain gSwitchSwapchain;
extern DkMemBlock gSwitchImageMemBlock;
extern DkImage gSwitchFramebuffers[2];
extern uint32_t gSwitchFramebufferWidth;
extern uint32_t gSwitchFramebufferHeight;

int OsSwitch_GraphicsInit(int width, int height);
void OsSwitch_GraphicsShutdown(void);

// Optional direct-to-swapchain 2D blitter. Source pixels are converted to
// RGBA8 in a fenced upload slot; Deko3D's 2D engine performs the scaling.
int OsSwitch_GraphicsBeginFrame(void);
int OsSwitch_GraphicsFillBlack(void);
int OsSwitch_GraphicsBlit(const void* pixels, int pitch, int bpp, int surfaceW, int surfaceH,
	int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH);
void OsSwitch_WindowCompositeChildrenGpu(void);

// Upload the software framebuffer (0xAABBGGRR pixels, RGBA in memory) and
// present it. When the optional GPU blitter has begun a frame, this presents
// the queued 2D blits instead. Safe to call when graphics are not initialised.
void OsSwitch_GraphicsPresent(void);

// The software framebuffer the window layer draws into.
uint32_t* OsSwitch_Framebuffer(int* pitchPixels, int* width, int* height);
void OsSwitch_FramebufferClear(uint32_t rgba);

// ---- Window coordinate mapping ---------------------------------------------
// The engine works in the picture's (display mode's) coordinates; the back
// end scales that picture into the 1280x720 screen with its aspect kept.
int OsSwitch_ModeToScreenX(int x);
int OsSwitch_ModeToScreenY(int y);
int OsSwitch_ScreenToModeX(int x);
int OsSwitch_ScreenToModeY(int y);
void OsSwitch_WindowSetPictureSize(int w, int h);
void OsSwitch_ModeSize(int* w, int* h); // the picture size the cursor lives in
void OsSwitch_CursorPad(int pad);       // 1: the pad drives the pointer, draw the cursor; 0: touch, hide it
int OsSwitch_CursorVisible(int* screenX, int* screenY);           // the cursor to draw, in screen pixels
const char* const* OsSwitch_CursorBitmap(int* w, int* h);         // its rows: '#' black, '.' white, ' ' none
void OsSwitch_GraphicsPresentCursor(void);                        // present a frame for a moved cursor
void OsSwitch_CursorDraw(void);         // draw the cursor over the framebuffer (just before the upload)
void OsSwitch_CursorErase(void);        // and put the pixels back (just after it)

// ---- Input ------------------------------------------------------------------
extern PadState gSwitchPad;

void OsSwitch_InputInit(void);
void OsSwitch_InputShutdown(void);
void OsSwitch_InputUpdate(void);

// Called by input.c for every virtual-key transition; raises the engine's
// key_down / key_up callbacks (window.c).
void OsSwitch_RaiseKey(int vk, int down);
void OsSwitch_RaiseMouseMove(int x, int y);
void OsSwitch_RaiseMouseButton(int button, int down, int x, int y);
void OsSwitch_RaiseMouseWheel(int delta);

// ---- Path conversions -------------------------------------------------------
// Shift-JIS Windows path -> UTF-8 Switch path under the game root (romfs:/
// or the OS_SetGameDir override).  `write` selects the writable SD root
// (sdmc:/switch/openbgi/) instead.
void OsSwitch_Path(const char* win, char* out, size_t n);
void OsSwitch_PathWrite(const char* win, char* out, size_t n);

// Shift-JIS text -> UTF-8 (window titles, dialog labels)
void OsSwitch_SjisToUtf8(const char* sjis, char* out, size_t n);
uint32_t OsSwitch_SjisChar(uint16_t code);
int OsSwitch_UnicodeToSjis(uint32_t cp, char out[2]);

// The roots used by the path conversion (both with a trailing separator).
void OsSwitch_GameRoot(char* out, size_t n);
void OsSwitch_WriteRoot(char* out, size_t n);
void OsSwitch_SetGameRoot(const char* root);

// ---- Font rendering ---------------------------------------------------------
void OsSwitch_FontShutdown(void);

// ---- Application lifecycle --------------------------------------------------
extern AppletHookCookie gSwitchAppletHookCookie;
extern int gSwitchShouldClose;
extern int gSwitchFocusEvent;
/* File I/O counters for the boot profile (fs.c, paths.c); logged once a second by OsSwitch_FsStatsTick */
typedef struct OsSwitchFsStats
{
	uint64_t openNs, readNs, seekNs, sizeNs, pathNs, bytes;
	uint32_t opens, reads, smallReads, seeks, sizes, paths, caseScans;
} OsSwitchFsStats_t;
extern OsSwitchFsStats_t gSwitchFsStats;
void OsSwitch_FsStatsTick(void);
void OsSwitch_PathEx(const char* win, char* out, size_t n, int* missing); // OsSwitch_Path + "known not to exist"
int OsSwitch_NativeMissing(const char* path);                              // a native path whose directory has no such entry
void OsSwitch_DirCacheFlush(void);                                         // forget the directory lists (after creating / deleting / moving)

void OsSwitch_CoresInit(void);                  // log the cores and a CPU speed sample (thread.c)
int OsSwitch_PickCore(int slot);                // a core other than the main thread's, or -2
void OsSwitch_FixArgs(int* argc, char*** argv); // regroup the launch arguments (main.c)

void OsSwitch_AppletHook(AppletHookType hook, void* param);

// Temporary persistent startup log: sdmc:/switch/openbgi/startup.log
void OsSwitch_Log(const char* format, ...);

// ---- Time -------------------------------------------------------------------
// Nanoseconds since the back end started (monotonic).
uint64_t OsSwitch_TicksNs(void);

#endif // BGI_OS_SWITCH_H_
