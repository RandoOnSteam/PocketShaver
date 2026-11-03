/*
 *  clip_sfl.cpp - Text clipboard of the SFL platform layer
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
#include "macos_util.h"
#include "clip.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "main.h"
#include "emul_op.h"
#include "thunks.h"
#include "sflroman.h"
#include "gui.h"
#include "sflui.h"

#define SFLCLIPTEXT FOURCC('T', 'E', 'X', 'T')
#define SFLCLIPUTF8PERBYTE 3

typedef struct SFLCLIP
{
	char *known;
	size_t knownlength;
	char *posted;
	size_t postedlength;
	uint32 proc;
	bool weputthis;
} SFLCLIP;

static SFLCLIP SflClip;

static void SflClipRemember(char **slot, size_t *slotlength, const char *text, size_t length)
{
	if (*slot != NULL)
		WSFREE(*slot);
	*slot = WSALLOC(char, length + 1);
	*slotlength = 0;
	if (*slot == NULL)
		return;
	WSMemoryCopy(*slot, text, length);
	(*slot)[length] = 0;
	*slotlength = length;
}

static bool SflClipMatches(const char *slot, size_t slotlength, const char *text, size_t length)
{
	if (slot == NULL || slotlength != length)
		return false;
	return memcmp(slot, text, length) == 0;
}

void ClipInit(void)
{
	static const uint8 putscrap[] = {
		0x59, 0x8f,
		0xa9, 0xfc,
		0x2f, 0x3c, 0, 0, 0, 0,
		0x2f, 0x3c, 0, 0, 0, 0,
		0x2f, 0x3c, 0, 0, 0, 0,
		0xa9, 0xfe,
		0x58, 0x8f,
		uint8(M68K_RTS >> 8), uint8(M68K_RTS)
	};
	SflClip.proc = SheepProc(putscrap, sizeof(putscrap));
	SflClip.weputthis = false;
}

void ClipExit(void)
{
	if (SflClip.known != NULL)
		WSFREE(SflClip.known);
	if (SflClip.posted != NULL)
		WSFREE(SflClip.posted);
	SflClip.known = NULL;
	SflClip.posted = NULL;
}

void PutScrap(uint32 type, void *scrap, int32 length)
{
	char *text;
	size_t textlength;
	if (SflClip.weputthis) {
		SflClip.weputthis = false;
		return;
	}
	if (type != SFLCLIPTEXT || length <= 0)
		return;
	text = WSALLOC(char, (size_t)length * SFLCLIPUTF8PERBYTE + 1);
	if (text == NULL)
		return;
	textlength = SflRomanToUtf8((const uint8 *)scrap, (size_t)length, text, (size_t)length * SFLCLIPUTF8PERBYTE + 1, true);
	SflUiClipboardSet(text, textlength);
	SflClipRemember(&SflClip.known, &SflClip.knownlength, text, textlength);
	WSFREE(text);
}

void ZeroScrap()
{
}

void ClipInterrupt(void)
{
	char *text;
	size_t length;
	M68kRegisters r;
	if (!SflUiClipboardGet(&text, &length))
		return;
	if (length != 0 && !SflClipMatches(SflClip.known, SflClip.knownlength, text, length)
		&& !SflClipMatches(SflClip.posted, SflClip.postedlength, text, length)) {
		SflClipRemember(&SflClip.posted, &SflClip.postedlength, text, length);
		r.d[0] = 0x01000003;
		r.a[0] = 15;
		Execute68kTrap(0xa02f, &r);
	}
	WSFREE(text);
}

void GetScrap(void **handle, uint32 type, int32 offset)
{
	char *text;
	size_t length;
	size_t converted;
	uint32 scrap;
	M68kRegisters r;
	if (type != SFLCLIPTEXT || SflClip.proc == 0)
		return;
	if (!SflUiClipboardGet(&text, &length))
		return;
	if (length != 0 && !SflClipMatches(SflClip.known, SflClip.knownlength, text, length)) {
		SflClipRemember(&SflClip.known, &SflClip.knownlength, text, length);
		r.d[0] = (uint32)length;
		Execute68kTrap(0xa71e, &r);
		scrap = r.a[0];
		if (scrap != 0) {
			converted = SflUtf8ToRoman(text, length, Mac2HostAddr(scrap), length, true);
			WriteMacInt32(SflClip.proc + 6, (uint32)converted);
			WriteMacInt32(SflClip.proc + 12, type);
			WriteMacInt32(SflClip.proc + 18, scrap);
			SflClip.weputthis = true;
			Execute68k(SflClip.proc, &r);
			r.a[0] = scrap;
			Execute68kTrap(0xa01f, &r);
		}
	}
	WSFREE(text);
}
