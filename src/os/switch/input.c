/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * input.c - Switch back end: gamepad and touch input (inc/bgi/os.h)
 *
 * The gamepad stands in for the keyboard (A = Enter, B = Escape, the D-pad
 * and the left stick = the arrow keys, ...); the touch screen is the mouse.
 * The right stick moves the mouse as well, and pressing it clicks.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <string.h>

PadState gSwitchPad;

#define STICK_DEADZONE 12000
#define STICK_MOUSE    10 // pixels per update at full deflection

static int gTouchDown;
static int gStickMouseDown;
static int gStickKeys[4]; // the arrow keys the left stick is holding

static void SetStickKey(int slot, int vk, int down)
{
	if(down == gStickKeys[slot])
		return;
	gStickKeys[slot] = down;
	OsSwitch_RaiseKey(vk, down);
}

void OsSwitch_InputInit(void)
{
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&gSwitchPad);
	hidInitializeTouchScreen();
	gTouchDown = 0;
	gStickMouseDown = 0;
	memset(gStickKeys, 0, sizeof gStickKeys);
}

void OsSwitch_InputShutdown(void)
{
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
	HidTouchScreenState touch;
	size_t i;
	int cx, cy;

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

	// The right stick moves the pointer.
	right = padGetStickPos(&gSwitchPad, 1);
	cx = 0;
	cy = 0;
	OS_CursorGetPos(&cx, &cy);
	if(right.x < -STICK_DEADZONE || right.x > STICK_DEADZONE)
		cx += right.x * STICK_MOUSE / 32767;
	if(right.y < -STICK_DEADZONE || right.y > STICK_DEADZONE)
		cy -= right.y * STICK_MOUSE / 32767;
	if(cx < 0)
		cx = 0;
	if(cy < 0)
		cy = 0;
	OS_CursorSetPos(cx, cy);
	if(right.x < -STICK_DEADZONE || right.x > STICK_DEADZONE || right.y < -STICK_DEADZONE || right.y > STICK_DEADZONE)
		OsSwitch_RaiseMouseMove(cx, cy);

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

	// The touch screen is an absolute pointer with the left button.
	if(hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0)
	{
		int mx = OsSwitch_ScreenToModeX((int)touch.touches[0].x);
		int my = OsSwitch_ScreenToModeY((int)touch.touches[0].y);
		OsSwitch_RaiseMouseMove(mx, my);
		if(!gTouchDown)
		{
			gTouchDown = 1;
			OsSwitch_RaiseMouseButton(0, 1, mx, my);
		}
	}
	else if(gTouchDown)
	{
		int mx = 0, my = 0;
		OS_CursorGetPos(&mx, &my);
		gTouchDown = 0;
		OsSwitch_RaiseMouseButton(0, 0, mx, my);
	}
}
