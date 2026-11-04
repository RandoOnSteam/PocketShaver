/*
 *  sflroman.cpp - Mac Roman and UTF-8 conversion for the SFL platform layer
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "sflroman.h"
#include "gui.h"

static uint32 SflRomanCharacter(uint8 byte)
{
	static const uint16 characters[128] = {
		0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
		0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
		0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
		0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
		0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
		0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
		0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
		0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7
	};
	if (byte < 0x80)
		return byte;
	return characters[byte & 0x7f];
}

static uint8 SflRomanByte(uint32 character)
{
	uint32 byte;
	if (character < 0x80)
		return (uint8)character;
	for (byte = 0x80; byte < 0x100; byte++)
		if (SflRomanCharacter((uint8)byte) == character)
			return (uint8)byte;
	return SFLROMANUNKNOWN;
}

static size_t SflRomanPut(char *target, size_t used, size_t targetsize, uint32 character)
{
	if (character < 0x80) {
		if (used + 1 >= targetsize)
			return 0;
		target[used] = (char)character;
		return 1;
	}
	if (character < 0x800) {
		if (used + 2 >= targetsize)
			return 0;
		target[used] = (char)(0xc0 | (character >> 6));
		target[used + 1] = (char)(0x80 | (character & 0x3f));
		return 2;
	}
	if (used + 3 >= targetsize)
		return 0;
	target[used] = (char)(0xe0 | (character >> 12));
	target[used + 1] = (char)(0x80 | ((character >> 6) & 0x3f));
	target[used + 2] = (char)(0x80 | (character & 0x3f));
	return 3;
}

size_t SflRomanToUtf8(const uint8 *source, size_t length, char *target, size_t targetsize, bool crtolf)
{
	size_t index;
	size_t used;
	size_t written;
	uint32 character;
	WSASSERT(targetsize != 0);
	used = 0;
	for (index = 0; index < length; index++) {
		character = SflRomanCharacter(source[index]);
		if (crtolf && character == 13)
			character = 10;
		written = SflRomanPut(target, used, targetsize, character);
		if (written == 0)
			break;
		used += written;
	}
	target[used] = 0;
	return used;
}

static size_t SflUtf8Decode(const uint8 *source, size_t length, uint32 *character)
{
	if (source[0] < 0x80) {
		*character = source[0];
		return 1;
	}
	if ((source[0] & 0xe0) == 0xc0 && length >= 2) {
		*character = ((uint32)(source[0] & 0x1f) << 6) | (source[1] & 0x3f);
		return 2;
	}
	if ((source[0] & 0xf0) == 0xe0 && length >= 3) {
		*character = ((uint32)(source[0] & 0x0f) << 12) | ((uint32)(source[1] & 0x3f) << 6) | (source[2] & 0x3f);
		return 3;
	}
	if ((source[0] & 0xf8) == 0xf0 && length >= 4) {
		*character = ((uint32)(source[0] & 0x07) << 18) | ((uint32)(source[1] & 0x3f) << 12)
			| ((uint32)(source[2] & 0x3f) << 6) | (source[3] & 0x3f);
		return 4;
	}
	*character = SFLROMANUNKNOWN;
	return 1;
}

size_t SflUtf8ToRoman(const char *source, size_t length, uint8 *target, size_t targetsize, bool lftocr)
{
	const uint8 *bytes;
	size_t index;
	size_t used;
	uint32 character;
	bytes = (const uint8 *)source;
	used = 0;
	index = 0;
	while (index < length && used < targetsize) {
		if (bytes[index] == 0)
			break;
		if (lftocr && bytes[index] == 13 && index + 1 < length && bytes[index + 1] == 10) {
			index++;
			continue;
		}
		index += SflUtf8Decode(bytes + index, length - index, &character);
		if (lftocr && character == 10)
			character = 13;
		target[used] = SflRomanByte(character);
		used++;
	}
	return used;
}
