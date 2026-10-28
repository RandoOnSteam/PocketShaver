/*
 *  osxclip.cpp - OSX host<->guest Script Manager clipboard bridge
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
#include "my_sdl.h"
#include "osxmachine.h"

#define CLIPMACHMSG 0xffffffe1u
#define CLIPMACHMSGOVERWRITE 0xffffffe0u
#define CLIPMACHSEND 1u
#define CLIPMACHRECEIVE 2u
#define CLIPMAXBYTES 0x400000u
#define CLIPFRAME 0x400u
#define CLIPDEPTH 32
#define CLIPHEADER 16u
#define CLIPPUTINTERVAL 250000000ULL
#define CLIPGETINTERVAL 500000000ULL
#define CLIPTIMEOUT 30000000000ULL
#define CLIPTASKPUT 1
#define CLIPTASKGET 2
#define CLIPUTXT 0x75747874u

static void StoreUnit(uint8 *output, uint32 *used, uint32 unit)
{
	output[*used] = (uint8)(unit >> 8);
	output[*used + 1] = (uint8)unit;
	*used += 2;
}

static uint8 *GuestFromUtf8(const char *text, uint32 *length)
{
	const uint8 *cursor;
	uint8 *output;
	uint32 used;
	uint32 character;
	int extra;
	output = (uint8 *)malloc(strlen(text) * 2 + 2);
	if (output == NULL)
		return NULL;
	used = 0;
	cursor = (const uint8 *)text;
	while (*cursor) {
		character = *cursor++;
		extra = 0;
		if ((character & 0xe0) == 0xc0) {
			character &= 0x1f;
			extra = 1;
		} else if ((character & 0xf0) == 0xe0) {
			character &= 0x0f;
			extra = 2;
		} else if ((character & 0xf8) == 0xf0) {
			character &= 0x07;
			extra = 3;
		} else if (character >= 0x80)
			character = 0xfffd;
		while (extra && (*cursor & 0xc0) == 0x80) {
			character = (character << 6) | (*cursor++ & 0x3f);
			extra--;
		}
		if (extra)
			character = 0xfffd;
		if (character == '\r' && *cursor == '\n')
			cursor++;
		else if (character == '\n')
			character = '\r';
		if (character >= 0x10000 && character <= 0x10ffff) {
			StoreUnit(output, &used, 0xd800 + ((character - 0x10000) >> 10));
			StoreUnit(output, &used, 0xdc00 + ((character - 0x10000) & 0x3ff));
		} else if (character < 0x10000)
			StoreUnit(output, &used, character);
	}
	*length = used;
	return output;
}

static char *Utf8FromGuest(const uint8 *data, uint32 length)
{
	char *output;
	uint32 used;
	uint32 index;
	uint32 character;
	uint32 low;
	output = (char *)malloc(length * 2 + 1);
	if (output == NULL)
		return NULL;
	used = 0;
	for (index = 0; index + 1 < length; index += 2) {
		character = ((uint32)data[index] << 8) | data[index + 1];
		if (character >= 0xd800 && character < 0xdc00 && index + 3 < length) {
			low = ((uint32)data[index + 2] << 8) | data[index + 3];
			if (low >= 0xdc00 && low < 0xe000) {
				character = 0x10000 + ((character - 0xd800) << 10) + (low - 0xdc00);
				index += 2;
			}
		}
		if (character == '\r')
			character = '\n';
		if (character == 0 || character == 0xfeff)
			continue;
		if (character < 0x80)
			output[used++] = (char)character;
		else if (character < 0x800) {
			output[used++] = (char)(0xc0 | (character >> 6));
			output[used++] = (char)(0x80 | (character & 0x3f));
		} else if (character < 0x10000) {
			output[used++] = (char)(0xe0 | (character >> 12));
			output[used++] = (char)(0x80 | ((character >> 6) & 0x3f));
			output[used++] = (char)(0x80 | (character & 0x3f));
		} else {
			output[used++] = (char)(0xf0 | (character >> 18));
			output[used++] = (char)(0x80 | ((character >> 12) & 0x3f));
			output[used++] = (char)(0x80 | ((character >> 6) & 0x3f));
			output[used++] = (char)(0x80 | (character & 0x3f));
		}
	}
	output[used] = 0;
	return output;
}

static uint8 *ReadHostClipboard(uint32 *length)
{
	uint8 *data;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	char *text;
	if (!SDL_HasClipboardText())
		return NULL;
	text = SDL_GetClipboardText();
	if (text == NULL)
		return NULL;
	data = GuestFromUtf8(text, length);
	SDL_free(text);
#elif defined(_WIN32)
	HANDLE handle;
	const wchar_t *wide;
	char *text;
	int size;
	data = NULL;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
		return NULL;
	handle = GetClipboardData(CF_UNICODETEXT);
	wide = NULL;
	if (handle)
		wide = (const wchar_t *)GlobalLock(handle);
	if (wide) {
		size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
		text = (char *)malloc(size + 1);
		if (text && size > 0 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, size, NULL, NULL) > 0)
			data = GuestFromUtf8(text, length);
		free(text);
		GlobalUnlock(handle);
	}
	CloseClipboard();
#else
	data = NULL;
	*length = 0;
#endif
	return data;
}

static void WriteHostClipboard(const uint8 *data, uint32 length)
{
	char *text;
#if !SDL_VERSION_ATLEAST(2, 0, 0) && defined(_WIN32)
	HGLOBAL handle;
	wchar_t *wide;
	int size;
	int index;
	int used;
#endif
	text = Utf8FromGuest(data, length);
	if (text == NULL)
		return;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetClipboardText(text);
#elif defined(_WIN32)
	size = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	handle = GlobalAlloc(GMEM_MOVEABLE, (size_t)size * 2 * sizeof(wchar_t));
	if (handle && OpenClipboard(NULL)) {
		wide = (wchar_t *)GlobalLock(handle);
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide + size, size);
		used = 0;
		for (index = 0; index < size; index++) {
			if (wide[size + index] == L'\n')
				wide[used++] = L'\r';
			wide[used++] = wide[size + index];
		}
		GlobalUnlock(handle);
		EmptyClipboard();
		if (!SetClipboardData(CF_UNICODETEXT, handle))
			GlobalFree(handle);
		CloseClipboard();
	} else if (handle)
		GlobalFree(handle);
#endif
	free(text);
}

static uint8 *GuestHost(OsxCpu *cpu, uint32 address, int kind)
{
	uint32 physical;
	if (!OsxCpuTranslate(cpu, address, kind, 0, &physical))
		return NULL;
	if (physical < cpu->ramsize)
		return cpu->ram + physical;
#if OSXG5
	if (physical - cpu->ramsize < cpu->highram)
		return cpu->ram + physical;
#endif
	return NULL;
}

static int CopyGuest(OsxCpu *cpu, uint32 address, uint8 *data, uint32 length, int toguest)
{
	uint8 *host;
	uint32 chunk;
	uint32 done;
	int kind;
	kind = OSXTLBREAD;
	if (toguest)
		kind = OSXTLBWRITE;
	for (done = 0; done < length; done += chunk) {
		chunk = 0x1000 - ((address + done) & 0xfff);
		if (chunk > length - done)
			chunk = length - done;
		host = GuestHost(cpu, address + done, kind);
		if (host == NULL)
			return 0;
		if (toguest) {
			memcpy(host, data + done, chunk);
			OsxJitInvalidate(cpu, (uint32)(host - cpu->ram), chunk);
		} else
			memcpy(data + done, host, chunk);
	}
	return 1;
}

static uint32 ReadWord(OsxCpu *cpu, uint32 address)
{
	uint32 value;
	if (!OsxCpuReadVirtual(cpu, address, &value))
		return 0;
	return value;
}

static int EventLoopThread(OsxClip *clip, OsxCpu *cpu)
{
	uint32 frame;
	uint32 caller;
	int depth;
	frame = cpu->gpr[1];
	for (depth = 0; depth < CLIPDEPTH; depth++) {
		if (!OsxCpuReadVirtual(cpu, frame, &frame) || frame == 0 || (frame & 3) || !OsxCpuReadVirtual(cpu, frame + 8, &caller))
			return 0;
		if (caller >= clip->textlow && caller < clip->texthigh)
			return 1;
	}
	return 0;
}

static void Call(OsxClip *clip, OsxCpu *cpu, int function, uint32 first, uint32 second, uint32 third, uint32 fourth, uint32 fifth)
{
	cpu->gpr[1] = clip->stack;
	cpu->gpr[3] = first;
	cpu->gpr[4] = second;
	cpu->gpr[5] = third;
	cpu->gpr[6] = fourth;
	cpu->gpr[7] = fifth;
	cpu->gpr[12] = clip->functions[function];
	cpu->ctr = clip->functions[function];
	cpu->lr = clip->pc;
	cpu->nextpc = clip->functions[function];
}

static int Begin(OsxMachine *machine, OsxCpu *cpu, int task)
{
	OsxClip *clip;
	uint8 *host;
	uint32 stack;
	clip = &machine->clip;
	stack = (cpu->gpr[1] - CLIPFRAME) & ~15u;
	host = GuestHost(cpu, stack, OSXTLBWRITE);
	if (host == NULL)
		return 0;
	memcpy(clip->saved, cpu->gpr, sizeof(clip->saved));
	clip->savedlr = cpu->lr;
	clip->savedctr = cpu->ctr;
	clip->pc = cpu->pc;
	clip->stack = stack;
	clip->stackphysical = (uint32)(host - cpu->ram);
	OsxCpuWriteVirtual(cpu, stack, cpu->gpr[1]);
	clip->busy = 1;
	clip->task = task;
	clip->step = 0;
	clip->block = 0;
	clip->data = 0;
	clip->started = OsxHostNanoseconds();
	if (task == CLIPTASKPUT)
		Call(clip, cpu, OSXCLIPMALLOC, clip->pendinglength + CLIPHEADER, 0, 0, 0, 0);
	else
		Call(clip, cpu, OSXCLIPMALLOC, CLIPHEADER, 0, 0, 0, 0);
	return 1;
}

static int Finish(OsxClip *clip, OsxCpu *cpu)
{
	memcpy(cpu->gpr, clip->saved, sizeof(clip->saved));
	cpu->lr = clip->savedlr;
	cpu->ctr = clip->savedctr;
	clip->busy = 0;
	free(clip->pending);
	clip->pending = NULL;
	return 0;
}

static int Release(OsxClip *clip, OsxCpu *cpu, int step, uint32 block)
{
	clip->step = step;
	Call(clip, cpu, OSXCLIPFREE, block, 0, 0, 0, 0);
	return 1;
}

static int AdvancePut(OsxMachine *machine, OsxClip *clip, OsxCpu *cpu, uint32 result)
{
	switch (clip->step) {
	case 0:
		if (result == 0)
			return Finish(clip, cpu);
		clip->block = result;
		clip->step = 1;
		Call(clip, cpu, OSXCLIPBZERO, clip->block, clip->pendinglength + CLIPHEADER, 0, 0, 0);
		return 1;
	case 1:
		if (!CopyGuest(cpu, clip->block + CLIPHEADER, clip->pending, clip->pendinglength, 1))
			return Release(clip, cpu, 6, clip->block);
		clip->step = 2;
		Call(clip, cpu, OSXCLIPCLEARCURRENT, 0, 0, 0, 0, 0);
		return 1;
	case 2:
		clip->step = 3;
		Call(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 3:
		if (result != 0)
			return Release(clip, cpu, 6, clip->block);
		clip->step = 4;
		Call(clip, cpu, OSXCLIPPUTFLAVOR, ReadWord(cpu, clip->block), CLIPUTXT, 0, clip->pendinglength, clip->block + CLIPHEADER);
		return 1;
	case 4:
		clip->step = 5;
		Call(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 5:
		if (result == 0)
			clip->lastscrap = ReadWord(cpu, clip->block);
		clip->primed = 1;
		if (machine->log)
			fprintf(machine->log, "clipboard to guest %u bytes\n", clip->pendinglength);
		return Release(clip, cpu, 6, clip->block);
	}
	return Finish(clip, cpu);
}

static int AdvanceGet(OsxMachine *machine, OsxClip *clip, OsxCpu *cpu, uint32 result)
{
	uint8 *data;
	uint32 length;
	uint32 hash;
	switch (clip->step) {
	case 0:
		if (result == 0)
			return Finish(clip, cpu);
		clip->block = result;
		clip->step = 1;
		Call(clip, cpu, OSXCLIPBZERO, clip->block, CLIPHEADER, 0, 0, 0);
		return 1;
	case 1:
		clip->step = 2;
		Call(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 2:
		if (result != 0)
			return Release(clip, cpu, 8, clip->block);
		clip->scrap = ReadWord(cpu, clip->block);
		if (!clip->primed || clip->scrap == clip->lastscrap) {
			clip->primed = 1;
			clip->lastscrap = clip->scrap;
			return Release(clip, cpu, 8, clip->block);
		}
		clip->step = 3;
		Call(clip, cpu, OSXCLIPFLAVORSIZE, clip->scrap, CLIPUTXT, clip->block + 4, 0, 0);
		return 1;
	case 3:
		clip->size = ReadWord(cpu, clip->block + 4);
		if (result != 0 || clip->size == 0 || clip->size > CLIPMAXBYTES) {
			clip->lastscrap = clip->scrap;
			return Release(clip, cpu, 8, clip->block);
		}
		clip->step = 4;
		Call(clip, cpu, OSXCLIPMALLOC, clip->size, 0, 0, 0, 0);
		return 1;
	case 4:
		if (result == 0)
			return Release(clip, cpu, 8, clip->block);
		clip->data = result;
		clip->step = 5;
		Call(clip, cpu, OSXCLIPBZERO, clip->data, clip->size, 0, 0, 0);
		return 1;
	case 5:
		OsxCpuWriteVirtual(cpu, clip->block + 4, clip->size);
		clip->step = 6;
		Call(clip, cpu, OSXCLIPFLAVORDATA, clip->scrap, CLIPUTXT, clip->block + 4, clip->data, 0);
		return 1;
	case 6:
		length = ReadWord(cpu, clip->block + 4);
		if (result == 0 && length <= clip->size) {
			data = (uint8 *)malloc(length + 1);
			if (data && CopyGuest(cpu, clip->data, data, length, 0)) {
				hash = OsxAdler32(data, length);
				if (hash != clip->lasthash)
					WriteHostClipboard(data, length);
				if (machine->log)
					fprintf(machine->log, "clipboard to host %u bytes\n", length);
				clip->lasthash = hash;
			}
			free(data);
		}
		clip->lastscrap = clip->scrap;
		return Release(clip, cpu, 7, clip->data);
	case 7:
		return Release(clip, cpu, 8, clip->block);
	}
	return Finish(clip, cpu);
}

int OsxClipSyscall(OsxMachine *machine, OsxCpu *cpu)
{
	OsxClip *clip;
	uint8 *data;
	uint8 *host;
	uint32 length;
	uint32 hash;
	uint64 now;
	clip = &machine->clip;
	if (!clip->ready || !(cpu->msr & OSXMSRPR))
		return 0;
	if (clip->busy) {
		if (cpu->pc == clip->pc && cpu->gpr[1] == clip->stack) {
			host = GuestHost(cpu, clip->stack, OSXTLBREAD);
			if (host && (uint32)(host - cpu->ram) == clip->stackphysical) {
				if (clip->task == CLIPTASKPUT)
					return AdvancePut(machine, clip, cpu, cpu->gpr[3]);
				return AdvanceGet(machine, clip, cpu, cpu->gpr[3]);
			}
		}
		if (OsxHostNanoseconds() - clip->started > CLIPTIMEOUT) {
			clip->busy = 0;
			free(clip->pending);
			clip->pending = NULL;
		}
		return 0;
	}
	if ((cpu->gpr[0] != CLIPMACHMSG && cpu->gpr[0] != CLIPMACHMSGOVERWRITE) || (cpu->gpr[4] & (CLIPMACHSEND | CLIPMACHRECEIVE)) != CLIPMACHRECEIVE)
		return 0;
	if (clip->functions[OSXCLIPDOCK]) {
		if (ReadWord(cpu, clip->functions[OSXCLIPDOCK] + 20) != clip->functions[OSXCLIPDOCKCOMMANDS])
			return 0;
		clip->functions[OSXCLIPDOCK] = 0;
	}
	now = OsxHostNanoseconds();
	if (now - clip->lastput < CLIPPUTINTERVAL || !EventLoopThread(clip, cpu))
		return 0;
	clip->lastput = now;
	data = ReadHostClipboard(&length);
	if (data && length > 0 && length <= CLIPMAXBYTES) {
		hash = OsxAdler32(data, length);
		if (hash != clip->lasthash) {
			clip->lasthash = hash;
			clip->pending = data;
			clip->pendinglength = length;
			if (Begin(machine, cpu, CLIPTASKPUT))
				return 1;
			clip->pending = NULL;
			clip->lasthash = 0;
		}
	}
	free(data);
	if (now - clip->lastget < CLIPGETINTERVAL)
		return 0;
	clip->lastget = now;
	return Begin(machine, cpu, CLIPTASKGET);
}
