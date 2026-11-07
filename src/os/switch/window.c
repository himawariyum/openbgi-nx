/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - Switch back end: the main window, display modes, the software
 *            surface, child windows, the event pump and the cursor
 *
 * The Switch screen is always 1280x720.  The engine's picture (the display
 * mode or the size OS_WindowSetPicture chose) is scaled into that screen
 * with its aspect kept and centred, and the mouse coordinates are mapped
 * back, so the engine keeps working in picture pixels.  There is no window
 * manager: window decoration, moving and minimising are no-ops.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BGI_SWITCH_GPU_BLIT
#define BGI_SWITCH_GPU_BLIT 0
#endif

#define SWITCH_SCREEN_W 1280
#define SWITCH_SCREEN_H 720

// ---- state ------------------------------------------------------------------

static int gAlive, gShown = 1, gActive = 1, gMinimized;
static int gClientW = 800, gClientH = 600;
static int gWinX, gWinY;
static int gModeW = 800, gModeH = 600;
static int gFullscreen;
static int gMapOn;
static double gScale = 1.0;
static int gOffX, gOffY;
static int gCursorX = 400, gCursorY = 300;
static int gCursorShape = OS_CURSOR_ARROW;
static int gCursorShowCount;
static int gPadCursor;    // the pad moved the pointer: draw it (the touch screen needs no cursor)
static int gCursorDirty;  // the cursor changed since it was last presented
static int gKeys[256];
static int gCloseRequested, gQuitPosted, gCloseDelivered;
static int gDragAccept;
static int gEventCount;
static OsEventHandlers_t gHandlers;

struct OsSwitchDc
{
	int child;
};

static struct OsSwitchDc gMainDc = {-1};

// ---- coordinate mapping -----------------------------------------------------

static void RecomputeMapping(void)
{
	double sx, sy;
	gMapOn = 0;
	gScale = 1.0;
	gOffX = gOffY = 0;
	if(gModeW <= 0 || gModeH <= 0)
		return;
	if(gModeW == SWITCH_SCREEN_W && gModeH == SWITCH_SCREEN_H)
		return;
	gMapOn = 1;
	sx = (double)SWITCH_SCREEN_W / (double)gModeW;
	sy = (double)SWITCH_SCREEN_H / (double)gModeH;
	gScale = sx < sy ? sx : sy;
	gOffX = (int)((SWITCH_SCREEN_W - gModeW * gScale) / 2.0);
	gOffY = (int)((SWITCH_SCREEN_H - gModeH * gScale) / 2.0);
}

int OsSwitch_ModeToScreenX(int x)
{
	return gMapOn ? (int)(x * gScale + 0.5) + gOffX : x;
}

int OsSwitch_ModeToScreenY(int y)
{
	return gMapOn ? (int)(y * gScale + 0.5) + gOffY : y;
}

int OsSwitch_ScreenToModeX(int x)
{
	return gMapOn ? (int)((x - gOffX) / gScale) : x;
}

int OsSwitch_ScreenToModeY(int y)
{
	return gMapOn ? (int)((y - gOffY) / gScale) : y;
}

void OsSwitch_WindowSetPictureSize(int w, int h)
{
	OS_WindowSetPicture(w, h);
}

// ---- pixels -----------------------------------------------------------------

static uint32_t SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* p;
	if(!s || !s->pixels || x < 0 || y < 0 || x >= s->width || y >= s->height)
		return 0;
	p = (const uint8_t*)s->pixels + (size_t)y * (size_t)s->pitch;
	if(s->bpp == 16)
	{
		uint16_t v = *(const uint16_t*)(const void*)(p + (size_t)x * 2);
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		r = (r << 3) | (r >> 2);
		g = (g << 3) | (g >> 2);
		b = (b << 3) | (b >> 2);
		return (r << 16) | (g << 8) | b;
	}
	return (*(const uint32_t*)(const void*)(p + (size_t)x * 4)) & 0x00ffffffu;
}

// 0x00RRGGBB -> 0xAABBGGRR (the framebuffer's RGBA byte order)
static uint32_t PackPixel(uint32_t rgb)
{
	return 0xff000000u | ((rgb & 0xffu) << 16) | (rgb & 0x0000ff00u) | ((rgb >> 16) & 0xffu);
}

/* The workhorse behind OS_BlitToWindow / OS_StretchToWindow.  The source
 * rectangle is stretched to the destination rectangle; on the main window
 * the destination goes through the picture -> screen mapping, a child
 * window's surface is addressed directly.  `clip` is in destination
 * coordinates. */
static void BlitSurface(const struct OsSwitchDc* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s,
	int srcX, int srcY, int srcW, int srcH, const int* clip)
{
	int cl = 0, ct = 0, cr, cb;
	int wx0, wy0, wx1, wy1, w, h, x, y;
	uint32_t* fb;
	int pitch, fw, fh;
	double fx, fy;
	int main = !dc || dc->child < 0;

	if(!s || !s->pixels || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
		return;

	if(main)
	{
		fb = OsSwitch_Framebuffer(&pitch, &fw, &fh);
		if(!fb)
			return;
		cr = fw;
		cb = fh;
		wx0 = OsSwitch_ModeToScreenX(dstX);
		wy0 = OsSwitch_ModeToScreenY(dstY);
		wx1 = OsSwitch_ModeToScreenX(dstX + dstW);
		wy1 = OsSwitch_ModeToScreenY(dstY + dstH);
		if(wx1 <= wx0)
			wx1 = wx0 + 1;
		if(wy1 <= wy0)
			wy1 = wy0 + 1;
		if(clip)
		{
			int l = OsSwitch_ModeToScreenX(clip[0]);
			int t = OsSwitch_ModeToScreenY(clip[1]);
			int r = OsSwitch_ModeToScreenX(clip[2]);
			int b = OsSwitch_ModeToScreenY(clip[3]);
			if(l > cl)
				cl = l;
			if(t > ct)
				ct = t;
			if(r < cr)
				cr = r;
			if(b < cb)
				cb = b;
		}
	}
	else
	{
		// A child window draws into its own surface; handled by the caller.
		return;
	}

	if(cl < wx0)
		cl = wx0;
	if(ct < wy0)
		ct = wy0;
	if(cr > wx1)
		cr = wx1;
	if(cb > wy1)
		cb = wy1;
	w = cr - cl;
	h = cb - ct;
	if(w <= 0 || h <= 0)
		return;

#if BGI_SWITCH_GPU_BLIT
	{
		int sx0, sy0, sx1, sy1;
		fx = (double)srcW / (double)(wx1 - wx0);
		fy = (double)srcH / (double)(wy1 - wy0);
		sx0 = srcX + (int)((double)(cl - wx0) * fx);
		sy0 = srcY + (int)((double)(ct - wy0) * fy);
		sx1 = srcX + (int)((double)(cr - wx0) * fx + 0.999999);
		sy1 = srcY + (int)((double)(cb - wy0) * fy + 0.999999);
		if(sx0 < srcX) sx0 = srcX;
		if(sy0 < srcY) sy0 = srcY;
		if(sx1 > srcX + srcW) sx1 = srcX + srcW;
		if(sy1 > srcY + srcH) sy1 = srcY + srcH;
		if(sx1 > sx0 && sy1 > sy0 && OsSwitch_GraphicsBlit(s->pixels, s->pitch, s->bpp,
			s->width, s->height, sx0, sy0, sx1 - sx0, sy1 - sy0, cl, ct, w, h))
			return;
	}
#endif

	fx = (double)srcW / (double)(wx1 - wx0);
	fy = (double)srcH / (double)(wy1 - wy0);
	for(y = 0; y < h; y++)
	{
		int sy = srcY + (int)((ct - wy0 + y + 0.5) * fy);
		uint32_t* row = fb + (size_t)(ct + y) * (size_t)pitch;
		if(sy >= srcY + srcH)
			sy = srcY + srcH - 1;
		for(x = 0; x < w; x++)
		{
			int sx = srcX + (int)((cl - wx0 + x + 0.5) * fx);
			if(sx >= srcX + srcW)
				sx = srcX + srcW - 1;
			row[cl + x] = PackPixel(SurfacePixel(s, sx, sy));
		}
	}
}

static void ChildBlitDispatch(const struct OsSwitchDc* dc, int dstX, int dstY, int dstW, int dstH,
	const OsSurface_t* s, int srcX, int srcY, int srcW, int srcH);

// ---- window lifecycle -------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	if(h)
		memcpy(&gHandlers, h, sizeof gHandlers);
	else
		memset(&gHandlers, 0, sizeof gHandlers);
}

int OS_WindowCreate(const OsWindowDesc_t* d)
{
	if(gAlive)
		return 0;
	if(!OsSwitch_GraphicsInit(SWITCH_SCREEN_W, SWITCH_SCREEN_H))
		return 0;

	gClientW = d && d->width > 0 ? d->width : 800;
	gClientH = d && d->height > 0 ? d->height : 600;
	gWinX = d ? d->x : 0;
	gWinY = d ? d->y : 0;
	gModeW = gClientW;
	gModeH = gClientH;
	gFullscreen = 0;
	RecomputeMapping();
	gCursorX = gModeW / 2;
	gCursorY = gModeH / 2;
	memset(gKeys, 0, sizeof gKeys);
	gAlive = 1;
	gShown = 0;
	gActive = 1;
	gMinimized = 0;
	gCloseRequested = gQuitPosted = gCloseDelivered = 0;
	gCursorShowCount = 0;
	gCursorShape = OS_CURSOR_ARROW;
	OsSwitch_FramebufferClear(0xff000000u);

	if(gHandlers.created)
		gHandlers.created();
	/* Windows sends WM_ACTIVATE right after WM_CREATE; the engine's
	 * Window_IsActive() (gWindowActive) only becomes true through this
	 * handler, and menus / waits that require the focus ignore all input
	 * until then. */
	if(gHandlers.activate)
		gHandlers.activate(1);
	return 1;
}

void OS_WindowDestroy(void)
{
	if(!gAlive)
		return;
	gAlive = 0;
	if(gHandlers.destroyed)
		gHandlers.destroyed();
}

int OS_WindowAlive(void)
{
	return gAlive;
}

void OS_WindowShow(int show)
{
	gShown = show != 0;
}

void OS_WindowSetTitle(const char* t)
{
	BGI_UNUSED(t);
}

void OS_WindowMove(int x, int y)
{
	gWinX = x;
	gWinY = y;
}

void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	if(gFullscreen)
	{
		if(x)
			*x = 0;
		if(y)
			*y = 0;
		if(w)
			*w = gModeW;
		if(h)
			*h = gModeH;
		return;
	}
	if(x)
		*x = gWinX;
	if(y)
		*y = gWinY;
	if(w)
		*w = gClientW;
	if(h)
		*h = gClientH;
}

int OS_WindowMinimized(void)
{
	return gMinimized;
}

int OS_WindowActive(void)
{
	return gAlive && gActive;
}

void OS_WindowMinimize(void)
{
	gMinimized = 1;
	if(gHandlers.sized)
		gHandlers.sized(1);
}

void OS_WindowClose(void)
{
	gCloseRequested = 1;
}

void OS_ScreenSize(int* w, int* h)
{
	if(w)
		*w = SWITCH_SCREEN_W;
	if(h)
		*h = SWITCH_SCREEN_H;
}

void OS_FrameMetrics(int* fw, int* fh)
{
	if(fw)
		*fw = 0;
	if(fh)
		*fh = 0;
}

int OS_FrameTop(void)
{
	return 0;
}

void OS_ImeStatus(int open)
{
	BGI_UNUSED(open);
}

/* The engine's "screen" space is the picture (mode) space here, as for a
 * full-screen window on the other back ends: OS_CursorGetPos / SetPos, the
 * mouse events and the client area all use picture pixels, and the
 * letterbox is only applied when drawing and when reading the touch screen
 * (OsSwitch_ScreenToMode*).  So the client origin is (0, 0) and both
 * conversions are the identity.  (Mapping them through the letterbox made
 * GetMouseClientPos subtract the letterbox offset, which shifted every
 * hit test whenever the mode is not 1280x720.) */
void OS_ClientToScreen(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

void OS_ScreenToClient(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

void OsSwitch_ModeSize(int* w, int* h)
{
	if(w)
		*w = gModeW;
	if(h)
		*h = gModeH;
}

// ---- display modes ----------------------------------------------------------

int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	BGI_UNUSED(bpp);
	if(w <= 0 || h <= 0)
		return 0;
	gModeW = w;
	gModeH = h;
	gFullscreen = fullscreen != 0;
	RecomputeMapping();
	return 1;
}

void OS_DisplayRestore(void)
{
	gFullscreen = 0;
	RecomputeMapping();
}

void OS_WindowSetFullscreen(int modeW, int modeH)
{
	if(modeW > 0 && modeH > 0)
	{
		gModeW = modeW;
		gModeH = modeH;
	}
	gFullscreen = 1;
	RecomputeMapping();
}

void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	gFullscreen = 0;
	gWinX = x;
	gWinY = y;
	if(clientW > 0 && clientH > 0)
	{
		gClientW = clientW;
		gClientH = clientH;
	}
	RecomputeMapping();
}

void OS_WindowSetPicture(int w, int h)
{
	if(w <= 0 || h <= 0)
		return;
	if(!gFullscreen)
	{
		gModeW = w;
		gModeH = h;
		RecomputeMapping();
	}
}

// ---- device context and presentation ---------------------------------------

OsDc_t* OS_WindowGetDc(void)
{
#if BGI_SWITCH_GPU_BLIT
	if(gAlive)
		OsSwitch_GraphicsBeginFrame();
#endif
	return gAlive ? (OsDc_t*)&gMainDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	struct OsSwitchDc* d = (struct OsSwitchDc*)dc;
	if(!d || d->child < 0)
		OsSwitch_GraphicsPresent();
}

void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	const struct OsSwitchDc* d = (const struct OsSwitchDc*)dc;
	if(d && d->child >= 0)
		ChildBlitDispatch(d, dstX, dstY, w, h, src, srcX, srcY, w, h);
	else
		BlitSurface(d, dstX, dstY, w, h, src, srcX, srcY, w, h, NULL);
}

void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	const struct OsSwitchDc* d = (const struct OsSwitchDc*)dc;
	if(!src)
		return;
	if(d && d->child >= 0)
		ChildBlitDispatch(d, dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height);
	else
		BlitSurface(d, dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height, clip);
}

void OS_WindowFillBlack(void)
{
#if BGI_SWITCH_GPU_BLIT
	if(OsSwitch_GraphicsFillBlack())
		return;
#endif
	OsSwitch_FramebufferClear(0xff000000u);
}

// ---- events -----------------------------------------------------------------

void OsSwitch_RaiseKey(int vk, int down)
{
	if(vk < 0 || vk > 255)
		return;
	if(down)
	{
		int repeat = gKeys[vk] & 0x80;
		gKeys[vk] = 0x80;
		if(gHandlers.key_down)
			gHandlers.key_down(vk, repeat);
	}
	else
	{
		gKeys[vk] = 0;
		if(gHandlers.key_up)
			gHandlers.key_up(vk);
	}
	gEventCount++;
}

void OsSwitch_CursorPad(int pad)
{
	pad = pad != 0;
	if(pad != gPadCursor)
		gCursorDirty = 1;
	gPadCursor = pad;
}

void OsSwitch_RaiseMouseMove(int x, int y)
{
	if(x == gCursorX && y == gCursorY)
		return;
	gCursorX = x;
	gCursorY = y;
	if(gPadCursor)
		gCursorDirty = 1;
	if(gHandlers.mouse_move)
		gHandlers.mouse_move(x, y);
	gEventCount++;
}

void OsSwitch_RaiseMouseButton(int button, int down, int x, int y)
{
	// the buttons are virtual keys 1 (L), 2 (R), 4 (M), 5, 6 for OS_KeyState (GetAsyncKeyState)
	static const int vks[5] = {0x01, 0x02, 0x04, 0x05, 0x06};
	if(button >= 0 && button < 5)
		gKeys[vks[button]] = down ? 0x80 : 0;
	gCursorX = x;
	gCursorY = y;
	if(gHandlers.mouse_button)
		gHandlers.mouse_button(button, down, x, y);
	gEventCount++;
}

void OsSwitch_RaiseMouseWheel(int delta)
{
	if(gHandlers.mouse_wheel)
		gHandlers.mouse_wheel(delta);
	gEventCount++;
}

int OS_PumpMessages(void)
{
	int had = 0;

	// Let the applet layer process its own messages (home button, etc.).
	if(!appletMainLoop())
		gSwitchShouldClose = 1;

	OsSwitch_InputUpdate();

	{ // the applet focus (HOME menu, a system dialog) is the window's activation
		static int sFocus = 1;
		int focus = appletGetFocusState() == AppletFocusState_InFocus;
		if(focus != sFocus)
		{
			sFocus = focus;
			gActive = focus;
			if(gHandlers.activate)
				gHandlers.activate(focus);
		}
	}

#if !BGI_SWITCH_GPU_BLIT
	if(gCursorDirty)
	{ // the engine only presents when the picture changes; show the moved cursor
		gCursorDirty = 0;
		OsSwitch_GraphicsPresent();
	}
#endif

	if(gSwitchShouldClose && !gCloseDelivered)
	{
		gCloseDelivered = 1;
		gCloseRequested = 1;
	}
	if(gCloseRequested)
	{
		gCloseRequested = 0;
		if(gHandlers.close_request)
			gHandlers.close_request();
		return 1;
	}
	if(gQuitPosted)
	{
		gQuitPosted = 0;
		if(gHandlers.quit)
			gHandlers.quit();
		return 1;
	}
	if(gEventCount)
	{
		gEventCount = 0;
		had = 1;
	}
	return had;
}

void OS_PostQuit(void)
{
	gQuitPosted = 1;
}

void OS_DragAccept(int accept)
{
	gDragAccept = accept != 0;
}

// ---- cursor -----------------------------------------------------------------

/* The Switch has no system cursor, so the one the engine shows (OS_CursorShow)
 * is drawn here: an arrow over the software framebuffer while it is uploaded
 * (graphics.c), the pixels under it saved and put back right after, so the
 * engine's partial redraws never see it. */
#define CUR_W 12
#define CUR_H 18
static const char* const kArrow[CUR_H] = {
	"#           ",
	"##          ",
	"#.#         ",
	"#..#        ",
	"#...#       ",
	"#....#      ",
	"#.....#     ",
	"#......#    ",
	"#.......#   ",
	"#........#  ",
	"#.........# ",
	"#......#####",
	"#...#..#    ",
	"#..# #..#   ",
	"#.#  #..#   ",
	"##    #..#  ",
	"#     #..#  ",
	"       ##   "};
static uint32_t gCurSave[CUR_W * CUR_H];
static int gCurSx, gCurSy, gCurSaved;

void OsSwitch_CursorDraw(void)
{
	int pitch, fw, fh, x, y;
	uint32_t* fb;
	gCurSaved = 0;
	if(!gAlive || !gPadCursor || gCursorShowCount < 0)
		return;
	fb = OsSwitch_Framebuffer(&pitch, &fw, &fh);
	if(!fb)
		return;
	gCurSx = OsSwitch_ModeToScreenX(gCursorX);
	gCurSy = OsSwitch_ModeToScreenY(gCursorY);
	for(y = 0; y < CUR_H; y++)
	{
		for(x = 0; x < CUR_W; x++)
		{
			int px = gCurSx + x, py = gCurSy + y;
			char c = kArrow[y][x];
			if(px < 0 || py < 0 || px >= fw || py >= fh)
				continue;
			gCurSave[y * CUR_W + x] = fb[(size_t)py * (size_t)pitch + px];
			if(c == '#')
				fb[(size_t)py * (size_t)pitch + px] = 0xff000000u;
			else if(c == '.')
				fb[(size_t)py * (size_t)pitch + px] = 0xffffffffu;
		}
	}
	gCurSaved = 1;
}

void OsSwitch_CursorErase(void)
{
	int pitch, fw, fh, x, y;
	uint32_t* fb;
	if(!gCurSaved)
		return;
	gCurSaved = 0;
	fb = OsSwitch_Framebuffer(&pitch, &fw, &fh);
	if(!fb)
		return;
	for(y = 0; y < CUR_H; y++)
	{
		for(x = 0; x < CUR_W; x++)
		{
			int px = gCurSx + x, py = gCurSy + y;
			if(px < 0 || py < 0 || px >= fw || py >= fh || kArrow[y][x] == ' ')
				continue;
			fb[(size_t)py * (size_t)pitch + px] = gCurSave[y * CUR_W + x];
		}
	}
}

void OS_CursorSetShape(int shape)
{
	gCursorShape = shape;
}

void OS_CursorShow(int show)
{
	gCursorShowCount += show ? 1 : -1;
	gCursorDirty = 1;
	{ // TEMPORARY diagnostic
		static int logged;
		if(logged < 20)
		{
			logged++;
			OsSwitch_Log("OS_CursorShow(%d) -> count=%d padCursor=%d", show, gCursorShowCount, gPadCursor);
		}
	}
}

void OS_CursorGetPos(int* x, int* y)
{
	if(x)
		*x = gCursorX;
	if(y)
		*y = gCursorY;
}

void OS_CursorSetPos(int x, int y)
{
	gCursorX = x;
	gCursorY = y;
}

int OS_MouseButtonsSwapped(void)
{
	return 0;
}

int OS_KeyState(int vk)
{
	if(vk < 0 || vk > 255)
		return 0;
	return (gKeys[vk] & 0x80) != 0;
}

void OS_KeyboardState(uint8_t out[256])
{
	int i;
	for(i = 0; i < 256; i++)
		out[i] = gKeys[i] ? 0x80 : 0;
}

// ---- child windows ----------------------------------------------------------

#define SWITCH_CHILD_MAX 8

struct OsSwitchChild
{
	int used;
	int shown;
	int index;
	int x, y, w, h;
	uint32_t* pixels;
	struct OsSwitchDc dc;
	char title[0x100];
};

static struct OsSwitchChild gChildren[SWITCH_CHILD_MAX];

// draw a child's surface into the main framebuffer
static void CompositeChild(struct OsSwitchChild* c);

static struct OsSwitchChild* ChildSlot(OsChild_t* c)
{
	return (struct OsSwitchChild*)c;
}

OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	struct OsSwitchChild* c;
	if(index < 0 || index >= SWITCH_CHILD_MAX || gChildren[index].used)
		return NULL;
	if(outerW <= 0 || outerH <= 0)
		return NULL;
	c = &gChildren[index];
	memset(c, 0, sizeof *c);
	c->pixels = (uint32_t*)calloc((size_t)outerW * (size_t)outerH, sizeof(uint32_t));
	if(!c->pixels)
		return NULL;
	{
		size_t i, n = (size_t)outerW * (size_t)outerH;
		for(i = 0; i < n; i++)
			c->pixels[i] = 0xff000000u;
	}
	c->used = 1;
	c->shown = 0;
	c->index = index;
	c->x = x;
	c->y = y;
	c->w = outerW;
	c->h = outerH;
	c->dc.child = index;
	if(title)
		OsSwitch_SjisToUtf8(title, c->title, sizeof c->title);
	return (OsChild_t*)c;
}

void OS_ChildClose(OsChild_t* h)
{
	struct OsSwitchChild* c = ChildSlot(h);
	int index;
	if(!c || !c->used)
		return;
	index = c->index;
	free(c->pixels);
	memset(c, 0, sizeof *c);
	if(gHandlers.child_destroyed)
		gHandlers.child_destroyed(index);
}

void OS_ChildShow(OsChild_t* h, int show)
{
	struct OsSwitchChild* c = ChildSlot(h);
	if(!c || !c->used)
		return;
	c->shown = show != 0;
	if(c->shown)
		CompositeChild(c);
}

void OS_ChildSetTitle(OsChild_t* h, const char* title)
{
	struct OsSwitchChild* c = ChildSlot(h);
	if(c && c->used && title)
		OsSwitch_SjisToUtf8(title, c->title, sizeof c->title);
}

void OS_ChildMove(OsChild_t* h, int x, int y, int outerW, int outerH)
{
	struct OsSwitchChild* c = ChildSlot(h);
	if(!c || !c->used)
		return;
	if(outerW > 0 && outerH > 0 && (outerW != c->w || outerH != c->h))
	{
		uint32_t* p = (uint32_t*)realloc(c->pixels, (size_t)outerW * (size_t)outerH * sizeof(uint32_t));
		if(p)
		{
			c->pixels = p;
			c->w = outerW;
			c->h = outerH;
		}
	}
	c->x = x;
	c->y = y;
	if(gHandlers.child_paint)
		gHandlers.child_paint(c->index, (OsDc_t*)&c->dc);
}

void OS_ChildGetPos(OsChild_t* h, int* x, int* y)
{
	struct OsSwitchChild* c = ChildSlot(h);
	if(x)
		*x = c && c->used ? c->x : 0;
	if(y)
		*y = c && c->used ? c->y : 0;
}

OsDc_t* OS_ChildGetDc(OsChild_t* h)
{
	struct OsSwitchChild* c = ChildSlot(h);
	return c && c->used ? (OsDc_t*)&c->dc : NULL;
}

void OS_ChildReleaseDc(OsChild_t* h, OsDc_t* dc)
{
	struct OsSwitchChild* c = ChildSlot(h);
	BGI_UNUSED(dc);
	if(c && c->used && c->shown)
		CompositeChild(c);
}

// A child blit: the surface is the child's own pixels, addressed 1:1.
static void BlitChild(struct OsSwitchChild* c, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s,
	int srcX, int srcY, int srcW, int srcH)
{
	int x, y;
	double fx, fy;
	if(!c || !c->used || !s || !s->pixels || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
		return;
	fx = (double)srcW / (double)dstW;
	fy = (double)srcH / (double)dstH;
	for(y = 0; y < dstH; y++)
	{
		int dy = dstY + y;
		int sy = srcY + (int)((y + 0.5) * fy);
		if(dy < 0 || dy >= c->h)
			continue;
		if(sy >= srcY + srcH)
			sy = srcY + srcH - 1;
		for(x = 0; x < dstW; x++)
		{
			int dx = dstX + x;
			int sx = srcX + (int)((x + 0.5) * fx);
			if(dx < 0 || dx >= c->w)
				continue;
			if(sx >= srcX + srcW)
				sx = srcX + srcW - 1;
			c->pixels[(size_t)dy * (size_t)c->w + dx] = SurfacePixel(s, sx, sy);
		}
	}
}

static void CompositeChild(struct OsSwitchChild* c)
{
	int pitch, fw, fh, x, y;
	uint32_t* fb;
	if(!c || !c->used || !c->shown)
		return;
	fb = OsSwitch_Framebuffer(&pitch, &fw, &fh);
	if(!fb)
		return;
	for(y = 0; y < c->h; y++)
	{
		int sy = OsSwitch_ModeToScreenY(c->y + y);
		if(sy < 0 || sy >= fh)
			continue;
		for(x = 0; x < c->w; x++)
		{
			int sx = OsSwitch_ModeToScreenX(c->x + x);
			if(sx < 0 || sx >= fw)
				continue;
			fb[(size_t)sy * (size_t)pitch + sx] = PackPixel(c->pixels[(size_t)y * (size_t)c->w + x]);
		}
	}
}

// Child blits go through this entry point from OS_BlitToWindow etc. when
// the DC names a child.
static void ChildBlitDispatch(const struct OsSwitchDc* dc, int dstX, int dstY, int dstW, int dstH,
	const OsSurface_t* s, int srcX, int srcY, int srcW, int srcH)
{
	if(dc && dc->child >= 0 && dc->child < SWITCH_CHILD_MAX)
		BlitChild(&gChildren[dc->child], dstX, dstY, dstW, dstH, s, srcX, srcY, srcW, srcH);
}

// ---- text entry -------------------------------------------------------------

#define SWITCH_EDIT_MAX 0x400

static char gEditText[SWITCH_EDIT_MAX] = "";
static int gEditLimit = SWITCH_EDIT_MAX - 1;
static int gEditVisible;
static int gEditX, gEditY, gEditW = 1, gEditH = 1;
static uint32_t gEditColour = 0x000000;
static int gEditRejectAscii;

void OS_EditShow(int show)
{
	gEditVisible = show != 0;
}

void OS_EditMove(int x, int y, int w, int h)
{
	gEditX = x;
	gEditY = y;
	gEditW = w > 0 ? w : 1;
	gEditH = h > 0 ? h : 1;
}

void OS_EditSetFont(const char* face, int height, int width)
{
	BGI_UNUSED(face);
	BGI_UNUSED(height);
	BGI_UNUSED(width);
}

void OS_EditLimitText(int maxChars)
{
	if(maxChars > 0 && maxChars < SWITCH_EDIT_MAX)
		gEditLimit = maxChars;
}

void OS_EditSetText(const char* t)
{
	if(!t)
		t = "";
	snprintf(gEditText, sizeof gEditText, "%s", t);
}

void OS_EditSelectAll(void)
{
}

void OS_EditFocus(int toEdit)
{
	BGI_UNUSED(toEdit);
}

int OS_EditGetText(char* buf, size_t n)
{
	size_t len;
	if(!buf || n == 0)
		return 0;
	len = strlen(gEditText);
	if(len >= n)
		len = n - 1;
	memcpy(buf, gEditText, len);
	buf[len] = 0;
	return (int)len;
}

void OS_EditSetColour(uint32_t rgb)
{
	gEditColour = rgb & 0x00ffffffu;
}

void OS_EditRefresh(void)
{
	if(gHandlers.edit_paint)
		gHandlers.edit_paint();
}

void OS_EditDestroy(void)
{
	gEditVisible = 0;
	gEditText[0] = 0;
}

void OS_EditRejectAscii(int reject)
{
	gEditRejectAscii = reject != 0;
}

int OS_EditImeOpen(void)
{
	return 0;
}

// ---- debugger window --------------------------------------------------------

int OS_DebugWindowOpen(int w, int h, const char* title)
{
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(title);
	return 0;
}

void OS_DebugWindowClose(void)
{
}

int OS_DebugWindowAlive(void)
{
	return 0;
}

void OS_DebugWindowPresent(const OsSurface_t* s)
{
	BGI_UNUSED(s);
}

void OsSwitch_WindowCompositeChildrenGpu(void)
{
#if BGI_SWITCH_GPU_BLIT
	int i;
	for(i = 0; i < SWITCH_CHILD_MAX; i++)
	{
		struct OsSwitchChild* c = &gChildren[i];
		int x0, y0, x1, y1, cl, ct, cr, cb;
		double fx, fy;
		int sx0, sy0, sx1, sy1;
		if(!c->used || !c->shown)
			continue;
		x0 = OsSwitch_ModeToScreenX(c->x);
		y0 = OsSwitch_ModeToScreenY(c->y);
		x1 = OsSwitch_ModeToScreenX(c->x + c->w);
		y1 = OsSwitch_ModeToScreenY(c->y + c->h);
		if(x1 <= x0 || y1 <= y0)
			continue;
		cl = x0 < 0 ? 0 : x0;
		ct = y0 < 0 ? 0 : y0;
		cr = x1 > (int)gSwitchFramebufferWidth ? (int)gSwitchFramebufferWidth : x1;
		cb = y1 > (int)gSwitchFramebufferHeight ? (int)gSwitchFramebufferHeight : y1;
		if(cr <= cl || cb <= ct)
			continue;
		fx = (double)c->w / (double)(x1 - x0);
		fy = (double)c->h / (double)(y1 - y0);
		sx0 = (int)((cl - x0) * fx);
		sy0 = (int)((ct - y0) * fy);
		sx1 = (int)((cr - x0) * fx + 0.999999);
		sy1 = (int)((cb - y0) * fy + 0.999999);
		if(sx0 < 0) sx0 = 0;
		if(sy0 < 0) sy0 = 0;
		if(sx1 > c->w) sx1 = c->w;
		if(sy1 > c->h) sy1 = c->h;
		if(sx1 > sx0 && sy1 > sy0)
			OsSwitch_GraphicsBlit(c->pixels, c->w * 4, 32, c->w, c->h,
				sx0, sy0, sx1 - sx0, sy1 - sy0, cl, ct, cr - cl, cb - ct);
	}
#endif
}
