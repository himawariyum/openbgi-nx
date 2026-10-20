/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialog.c - the script's modal dialogs ("B0 84" / "B0 85" / "B0 87" /
 *            "B0 8F") and the desktop wallpaper ("B0 F0"); interface in
 *            bgi/dialog.h
 *
 * The original stores the arguments in globals, runs DialogBoxParam with
 * a dialog procedure per template and then clears the key states.  The
 * procedures only shuffle text between the arguments and the controls
 * (see the OS_Dialog* contract in os.h for what each does); the
 * reimplementation hands that to the OS layer.  The key states are
 * cleared after every dialog so that the Enter that closed it is not
 * seen by the script.  From 1.547 on the engine's clock stands still while
 * a dialog is up (Clock_BoxBegin / Clock_BoxEnd).
 */
#include "bgi/dialog.h"
#include "bgi/input.h"
#include "bgi/sys.h"
#include "bgi/os.h"
#include "bgi/version.h"

/* "B0 84": one text field; 1 ok (the entry in `out`), 0 cancelled.  From
 * 1.616 on a negative maxLen makes a digits-only field of -maxLen
 * characters. */
int Dialog_Input1(char* out, const char* title, const char* initial, int maxLen)
{
	int r, digits = 0;
	if(maxLen < 0 && gEngine->gen >= GEN_1_616)
	{
		digits = 1;
		maxLen = -maxLen;
	}
	Clock_BoxBegin();
	r = OS_DialogInput1(title, initial, maxLen, digits, out);
	Input_ClearStates();
	Clock_BoxEnd();
	return r;
}

/* "B0 85" / "B0 87": two labelled text fields, one above the other or
 * (`compact`) side by side, either digits-only; 1 ok (the entries in out1
 * / out2), 0 cancelled */
int Dialog_Input2(char* out1, char* out2, const char* title, const char* label1, const char* initial1, int maxLen1,
	int digits1, const char* label2, const char* initial2, int maxLen2, int digits2, int compact)
{
	int r;
	Clock_BoxBegin();
	r = OS_DialogInput2(title, label1, initial1, maxLen1, digits1, out1, label2, initial2, maxLen2, digits2, out2, compact);
	Input_ClearStates();
	Clock_BoxEnd();
	return r;
}

/* "B0 8F": the player profile - the four name buffers hold the initial
 * texts and receive the entries, month / day are 0-based indices in and
 * out.  1 ok (the dialog has no cancel). */
int Dialog_Profile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month,
	int32_t* day)
{
	int r;
	Clock_BoxBegin();
	r = OS_DialogProfile(surname, givenName, nickname, pronoun, month, day);
	Input_ClearStates();
	Clock_BoxEnd();
	return r;
}

// "B0 F0": set the desktop wallpaper to the image file at `path`; returns the OS layer's result
int SetWallpaper(const char* path, int stretch, int tile)
{
	return OS_SetWallpaper(path, stretch, tile);
}
