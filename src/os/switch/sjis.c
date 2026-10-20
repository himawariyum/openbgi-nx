/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sjis.c - Shift-JIS (CP932) conversions for the Switch back end
 *
 * The two-byte table in sjis_table.h is generated from CP932.  Single-byte
 * characters are handled directly; the OsCommon_* helpers do the UTF-8
 * encoding and the text-encoding switch of OS_SetTextEncoding.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"

#include "sjis_table.h"

// one two-byte code to its Unicode scalar, U+FFFD when unmapped
uint32_t OsSwitch_SjisChar(uint16_t code)
{
	int lo = 0, hi = (int)SJIS_TABLE_COUNT - 1;
	while(lo <= hi)
	{
		int mid = (lo + hi) / 2;
		uint16_t k = kSjisToUnicode[mid][0];
		if(k == code)
			return kSjisToUnicode[mid][1];
		if(k < code)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0xfffd;
}

uint32_t OS_SjisToCodePoint(uint16_t sjis)
{
	if(sjis < 0x80)
		return sjis;
	if(sjis >= 0xa1 && sjis <= 0xdf)
		return 0xff61u + (sjis - 0xa1u);
	return OsSwitch_SjisChar(sjis);
}

// a Shift-JIS string to UTF-8 in `out` (n bytes), NUL-terminated
void OsSwitch_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	OsCommon_SjisToUtf8(sjis, out, n, OsSwitch_SjisChar);
}

size_t OS_SjisToUtf8(const char* sjis, char* out, size_t n)
{
	return OsCommon_SjisToUtf8(sjis, out, n, OsSwitch_SjisChar);
}

/* The code of one scalar: ASCII and the single-byte katakana directly, the
 * two Shift-JIS specials (yen, overline) by hand, everything else by a
 * linear search of the table (the reverse direction is rare). */
int OsSwitch_UnicodeToSjis(uint32_t cp, char out[2])
{
	uint32_t i;
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if(cp >= 0xff61 && cp <= 0xff9f)
	{
		out[0] = (char)(cp - 0xff61 + 0xa1);
		return 1;
	}
	if(cp == 0xa5)
	{
		out[0] = 0x5c;
		return 1;
	}
	if(cp == 0x203e)
	{
		out[0] = 0x7e;
		return 1;
	}
	for(i = 0; i < SJIS_TABLE_COUNT; i++)
	{
		if(kSjisToUnicode[i][1] == cp)
		{
			out[0] = (char)(kSjisToUnicode[i][0] >> 8);
			out[1] = (char)kSjisToUnicode[i][0];
			return 2;
		}
	}
	return 0;
}

// UTF-8 to Shift-JIS; a character without a code becomes '?'
size_t OS_Utf8ToSjis(const char* utf8, char* out, size_t n)
{
	const uint8_t* u = (const uint8_t*)utf8;
	size_t o = 0;
	while(*u && o + 2 < n)
	{
		int len, m;
		uint32_t cp = OsCommon_Utf8SeqLen(u) ? OsCommon_Utf8Decode(u, &len) : (len = 1, (uint32_t)*u);
		m = OsSwitch_UnicodeToSjis(cp, out + o);
		if(m == 0)
		{
			out[o] = '?';
			m = 1;
		}
		o += (size_t)m;
		u += len;
	}
	out[o] = 0;
	return o;
}
