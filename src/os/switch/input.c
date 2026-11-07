/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * input.c - Switch back end: gamepad and touch input (inc/bgi/os.h)
 *
 * The gamepad stands in for the keyboard (A = Enter, B = Escape, the D-pad
 * and the left stick = the arrow keys, ...); the touch screen is the mouse.
 * The right stick moves the mouse as well, and pressing it clicks.
 *
 * The cursor lives in picture (display mode) pixels, see window.c; the touch
 * screen reports 1280x720 screen pixels and is mapped into it.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <string.h>

PadState gSwitchPad;

#define STICK_DEADZONE 12000
#define STICK_MOUSE    10 // pixels per update at full deflection
#define TOUCH_SAMPLES  16 // touch samples read per update, so short taps are not lost

static int gTouchDown;
static int gTouchLogged;
static u64 gTouchLastSample; // sampling_number of the last processed touch sample
static int gTouchX, gTouchY; // last touch position, in picture pixels
static int gStickMouseDown;
static int gStickKeys[4]; // the arrow keys the left stick is holding

static void SetStickKey(int slot, int vk, int down)
{
	if(down == gStickKeys[slot])
		return;
	gStickKeys[slot] = down;
	OsSwitch_RaiseKey(vk, down);
}

static int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void OsSwitch_InputInit(void)
{
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&gSwitchPad);
	hidInitializeTouchScreen();
	gTouchDown = 0;
	gTouchLastSample = 0;
	gTouchX = gTouchY = 0;
	gStickMouseDown = 0;
	memset(gStickKeys, 0, sizeof gStickKeys);
}

void OsSwitch_InputShutdown(void)
{
}

// Every touch sample since the last update, oldest first, so a tap shorter than a frame still gives move, press and release.
static void UpdateTouch(void)
{
	HidTouchScreenState states[TOUCH_SAMPLES];
	s32 n = hidGetTouchScreenStates(states, TOUCH_SAMPLES); // states[0] is the newest
	int mw = 0, mh = 0;
	s32 i;

	OsSwitch_ModeSize(&mw, &mh);
	for(i = n - 1; i >= 0; i--)
	{
		const HidTouchScreenState* t = &states[i];
		if(t->sampling_number <= gTouchLastSample)
			continue;
		gTouchLastSample = t->sampling_number;

		if(t->count > 0)
		{
			int mx = Clamp(OsSwitch_ScreenToModeX((int)t->touches[0].x), 0, mw - 1);
			int my = Clamp(OsSwitch_ScreenToModeY((int)t->touches[0].y), 0, mh - 1);
			gTouchX = mx;
			gTouchY = my;
			OsSwitch_CursorPad(0);
			if(!gTouchDown && gTouchLogged < 40)
			{
				gTouchLogged++;
				OsSwitch_Log("touch raw=%d,%d picture=%d,%d mode=%dx%d", (int)t->touches[0].x, (int)t->touches[0].y, mx, my, mw, mh);
			}
			OsSwitch_RaiseMouseMove(mx, my); // the cursor is on the target before the button goes down
			if(!gTouchDown)
			{
				gTouchDown = 1;
				OsSwitch_RaiseMouseButton(0, 1, mx, my);
			}
		}
		else if(gTouchDown)
		{
			gTouchDown = 0;
			OsSwitch_RaiseMouseButton(0, 0, gTouchX, gTouchY);
		}
	}
}

void OsSwitch_InputUpdate(void)
{
	static const struct
	{
		u64 mask;
		int vk;
	} buttons[] = {
		{HidNpadButton_A, 0x0d}, // Enter
		{HidNpadButton_B, 0x1b}, // Escape
		{HidNpadButton_X, 0x20}, // Space
		{HidNpadButton_Y, 0x09}, // Tab
		{HidNpadButton_Up, 0x26},
		{HidNpadButton_Down, 0x28},
		{HidNpadButton_Left, 0x25},
		{HidNpadButton_Right, 0x27},
		{HidNpadButton_L, 0x21}, // Page Up
		{HidNpadButton_R, 0x22}, // Page Down
		{HidNpadButton_ZL, 'Q'},
		{HidNpadButton_ZR, 'E'},
		{HidNpadButton_Plus, 0x79},  // F10
		{HidNpadButton_Minus, 0x78}, // F9
	};
	HidAnalogStickState left, right;
	size_t i;
	int cx, cy, mw = 0, mh = 0;

	padUpdate(&gSwitchPad);

	for(i = 0; i < BGI_COUNTOF(buttons); i++)
	{
		if(padGetButtonsDown(&gSwitchPad) & buttons[i].mask)
			OsSwitch_RaiseKey(buttons[i].vk, 1);
		if(padGetButtonsUp(&gSwitchPad) & buttons[i].mask)
			OsSwitch_RaiseKey(buttons[i].vk, 0);
	}

	// The left stick acts as the arrow keys.
	left = padGetStickPos(&gSwitchPad, 0);
	SetStickKey(0, 0x25, left.x < -STICK_DEADZONE);
	SetStickKey(1, 0x27, left.x > STICK_DEADZONE);
	SetStickKey(2, 0x26, left.y > STICK_DEADZONE);
	SetStickKey(3, 0x28, left.y < -STICK_DEADZONE);

	// The touch screen is an absolute pointer with the left button.
	UpdateTouch();

	// The right stick moves the pointer (picture pixels).
	right = padGetStickPos(&gSwitchPad, 1);
	cx = 0;
	cy = 0;
	OS_CursorGetPos(&cx, &cy);
	if(right.x < -STICK_DEADZONE || right.x > STICK_DEADZONE || right.y < -STICK_DEADZONE || right.y > STICK_DEADZONE)
	{
		OsSwitch_ModeSize(&mw, &mh);
		if(right.x < -STICK_DEADZONE || right.x > STICK_DEADZONE)
			cx += right.x * STICK_MOUSE / 32767;
		if(right.y < -STICK_DEADZONE || right.y > STICK_DEADZONE)
			cy -= right.y * STICK_MOUSE / 32767;
		cx = Clamp(cx, 0, mw - 1);
		cy = Clamp(cy, 0, mh - 1);
		OsSwitch_CursorPad(1);
		OsSwitch_RaiseMouseMove(cx, cy);
	}

	// Pressing the right stick clicks.
	if((padGetButtonsDown(&gSwitchPad) & HidNpadButton_StickR) && !gStickMouseDown)
	{
		gStickMouseDown = 1;
		OsSwitch_RaiseMouseButton(0, 1, cx, cy);
	}
	else if((padGetButtonsUp(&gSwitchPad) & HidNpadButton_StickR) && gStickMouseDown)
	{
		gStickMouseDown = 0;
		OsSwitch_RaiseMouseButton(0, 0, cx, cy);
	}
}
