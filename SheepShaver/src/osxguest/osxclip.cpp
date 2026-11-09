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
#include "osxhost.h"
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
#define CLIPTEXT 0x54455854u

static uint32 OsxGuestMacRomanCharacter(uint32 byte)
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

static uint8 OsxGuestMacRomanByte(uint32 character)
{
	uint32 byte;
	if (character < 0x80)
		return (uint8)character;
	for (byte = 0x80; byte < 0x100; byte++)
		if (OsxGuestMacRomanCharacter(byte) == character)
			return (uint8)byte;
	return '?';
}

static void OsxGuestStoreUnit(uint8 *output, uint32 *used, uint32 unit)
{
	output[*used] = (uint8)(unit >> 8);
	output[*used + 1] = (uint8)unit;
	*used += 2;
}

static uint8 *OsxGuestFromUtf8(const char *text, uint32 *length)
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
			OsxGuestStoreUnit(output, &used, 0xd800 + ((character - 0x10000) >> 10));
			OsxGuestStoreUnit(output, &used, 0xdc00 + ((character - 0x10000) & 0x3ff));
		} else if (character < 0x10000)
			OsxGuestStoreUnit(output, &used, character);
	}
	*length = used;
	return output;
}

static char *OsxGuestUtf8FromGuest(const uint8 *data, uint32 length)
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

static uint8 *OsxGuestReadHostClipboard(uint32 *length)
{
	char *text = OsxGuestHostClipboardGet();
	uint8 *data;
	if (!text)
		return NULL;
	data = OsxGuestFromUtf8(text, length);
	free(text);
	return data;
}

static void OsxGuestWriteHostClipboard(const uint8 *data, uint32 length)
{
	char *text = OsxGuestUtf8FromGuest(data, length);
	if (text) {
		OsxGuestHostClipboardSet(text);
		free(text);
	}
}

static uint8 *OsxGuestHost(OsxCpu *cpu, uint32 address, int kind)
{
	uint32 physical;
	if (!OsxGuestCpuTranslate(cpu, address, kind, 0, &physical))
		return NULL;
	if (physical < cpu->ramsize)
		return cpu->ram + physical;
#if OSXG5
	if (physical - cpu->ramsize < cpu->highram)
		return cpu->ram + physical;
#endif
	return NULL;
}

static int OsxGuestCopyGuest(OsxCpu *cpu, uint32 address, uint8 *data, uint32 length, int toguest)
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
		host = OsxGuestHost(cpu, address + done, kind);
		if (host == NULL)
			return 0;
		if (toguest) {
			memcpy(host, data + done, chunk);
			OsxGuestJitInvalidate(cpu, (uint32)(host - cpu->ram), chunk);
		} else
			memcpy(data + done, host, chunk);
	}
	return 1;
}

static uint32 OsxGuestReadWord(OsxCpu *cpu, uint32 address)
{
	uint32 value;
	if (!OsxGuestCpuReadVirtual(cpu, address, &value))
		return 0;
	return value;
}

static int OsxGuestEventLoopThread(OsxClip *clip, OsxCpu *cpu)
{
	uint32 frame;
	uint32 caller;
	int depth;
	frame = cpu->gpr[1];
	for (depth = 0; depth < CLIPDEPTH; depth++) {
		if (!OsxGuestCpuReadVirtual(cpu, frame, &frame) || frame == 0 || (frame & 3) || !OsxGuestCpuReadVirtual(cpu, frame + 8, &caller))
			return 0;
		if (caller >= clip->textlow && caller < clip->texthigh)
			return 1;
	}
	return 0;
}

static void OsxGuestCall(OsxClip *clip, OsxCpu *cpu, int function, uint32 first, uint32 second, uint32 third, uint32 fourth, uint32 fifth)
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

static int OsxGuestBegin(OsxMachine *machine, OsxCpu *cpu, int task)
{
	OsxClip *clip;
	uint8 *host;
	uint32 stack;
	clip = &machine->clip;
	stack = (cpu->gpr[1] - CLIPFRAME) & ~15u;
	host = OsxGuestHost(cpu, stack, OSXTLBWRITE);
	if (host == NULL)
		return 0;
	memcpy(clip->saved, cpu->gpr, sizeof(clip->saved));
	clip->savedlr = cpu->lr;
	clip->savedctr = cpu->ctr;
	clip->pc = cpu->pc;
	clip->stack = stack;
	clip->stackphysical = (uint32)(host - cpu->ram);
	OsxGuestCpuWriteVirtual(cpu, stack, cpu->gpr[1]);
	clip->busy = 1;
	clip->task = task;
	clip->step = 0;
	clip->block = 0;
	clip->data = 0;
	clip->started = OsxGuestHostNanoseconds();
	if (task == CLIPTASKPUT)
		OsxGuestCall(clip, cpu, OSXCLIPMALLOC, clip->pendinglength + clip->pendinglength / 2 + CLIPHEADER, 0, 0, 0, 0);
	else
		OsxGuestCall(clip, cpu, OSXCLIPMALLOC, CLIPHEADER, 0, 0, 0, 0);
	return 1;
}

static int OsxGuestFinish(OsxClip *clip, OsxCpu *cpu)
{
	memcpy(cpu->gpr, clip->saved, sizeof(clip->saved));
	cpu->lr = clip->savedlr;
	cpu->ctr = clip->savedctr;
	clip->busy = 0;
	free(clip->pending);
	clip->pending = NULL;
	return 0;
}

static int OsxGuestRelease(OsxClip *clip, OsxCpu *cpu, int step, uint32 block)
{
	clip->step = step;
	OsxGuestCall(clip, cpu, OSXCLIPFREE, block, 0, 0, 0, 0);
	return 1;
}

static int OsxGuestAdvancePut(OsxMachine *machine, OsxClip *clip, OsxCpu *cpu, uint32 result)
{
	switch (clip->step) {
	case 0:
		if (result == 0) {
			clip->lasthash = 0;
			return OsxGuestFinish(clip, cpu);
		}
		clip->block = result;
		clip->step = 1;
		OsxGuestCall(clip, cpu, OSXCLIPBZERO, clip->block, clip->pendinglength + clip->pendinglength / 2 + CLIPHEADER, 0, 0, 0);
		return 1;
	case 1:
		if (!OsxGuestCopyGuest(cpu, clip->block + CLIPHEADER, clip->pending, clip->pendinglength + clip->pendinglength / 2, 1)) {
			clip->lasthash = 0;
			return OsxGuestRelease(clip, cpu, 6, clip->block);
		}
		clip->step = 2;
		OsxGuestCall(clip, cpu, OSXCLIPCLEARCURRENT, 0, 0, 0, 0, 0);
		return 1;
	case 2:
		clip->step = 3;
		OsxGuestCall(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 3:
		if (result != 0) {
			clip->lasthash = 0;
			return OsxGuestRelease(clip, cpu, 6, clip->block);
		}
		clip->step = 7;
		OsxGuestCall(clip, cpu, OSXCLIPPUTFLAVOR, OsxGuestReadWord(cpu, clip->block), CLIPUTXT, 0, clip->pendinglength, clip->block + CLIPHEADER);
		return 1;
	case 7:
		clip->step = 4;
		OsxGuestCall(clip, cpu, OSXCLIPPUTFLAVOR, OsxGuestReadWord(cpu, clip->block), CLIPTEXT, 0, clip->pendinglength / 2, clip->block + CLIPHEADER + clip->pendinglength);
		return 1;
	case 4:
		clip->step = 5;
		OsxGuestCall(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 5:
		if (result == 0)
			clip->lastscrap = OsxGuestReadWord(cpu, clip->block);
		clip->primed = 1;
		if (machine->log) {
			fprintf(machine->log, "clipboard to guest %u bytes\n", clip->pendinglength);
			fflush(machine->log);
		}
		return OsxGuestRelease(clip, cpu, 6, clip->block);
	}
	return OsxGuestFinish(clip, cpu);
}

static int OsxGuestAdvanceGet(OsxMachine *machine, OsxClip *clip, OsxCpu *cpu, uint32 result)
{
	uint8 *data;
	uint32 length;
	uint32 hash;
	uint32 index;
	switch (clip->step) {
	case 0:
		if (result == 0)
			return OsxGuestFinish(clip, cpu);
		clip->block = result;
		clip->step = 1;
		OsxGuestCall(clip, cpu, OSXCLIPBZERO, clip->block, CLIPHEADER, 0, 0, 0);
		return 1;
	case 1:
		clip->step = 2;
		OsxGuestCall(clip, cpu, OSXCLIPGETCURRENT, clip->block, 0, 0, 0, 0);
		return 1;
	case 2:
		if (result != 0)
			return OsxGuestRelease(clip, cpu, 8, clip->block);
		clip->scrap = OsxGuestReadWord(cpu, clip->block);
		if (!clip->primed) {
			clip->primed = 1;
			clip->lastscrap = clip->scrap;
			return OsxGuestRelease(clip, cpu, 8, clip->block);
		}
		clip->step = 3;
		clip->flavor = CLIPUTXT;
		OsxGuestCall(clip, cpu, OSXCLIPFLAVORSIZE, clip->scrap, CLIPUTXT, clip->block + 4, 0, 0);
		return 1;
	case 3:
		length = OsxGuestReadWord(cpu, clip->block + 4);
		if (result == 0 && clip->scrap == clip->lastscrap && length == clip->size)
			return OsxGuestRelease(clip, cpu, 8, clip->block);
		clip->size = length;
		if ((result != 0 || clip->size == 0) && clip->flavor == CLIPUTXT) {
			clip->flavor = CLIPTEXT;
			OsxGuestCall(clip, cpu, OSXCLIPFLAVORSIZE, clip->scrap, CLIPTEXT, clip->block + 4, 0, 0);
			return 1;
		}
		if (result != 0 || clip->size == 0 || clip->size > CLIPMAXBYTES) {
			clip->lastscrap = clip->scrap;
			return OsxGuestRelease(clip, cpu, 8, clip->block);
		}
		clip->step = 4;
		OsxGuestCall(clip, cpu, OSXCLIPMALLOC, clip->size, 0, 0, 0, 0);
		return 1;
	case 4:
		if (result == 0)
			return OsxGuestRelease(clip, cpu, 8, clip->block);
		clip->data = result;
		clip->step = 5;
		OsxGuestCall(clip, cpu, OSXCLIPBZERO, clip->data, clip->size, 0, 0, 0);
		return 1;
	case 5:
		OsxGuestCpuWriteVirtual(cpu, clip->block + 4, clip->size);
		clip->step = 6;
		OsxGuestCall(clip, cpu, OSXCLIPFLAVORDATA, clip->scrap, clip->flavor, clip->block + 4, clip->data, 0);
		return 1;
	case 6:
		length = OsxGuestReadWord(cpu, clip->block + 4);
		if (result == 0 && length <= clip->size) {
			data = (uint8 *)malloc(length * 2 + 1);
			if (data && OsxGuestCopyGuest(cpu, clip->data, data + length, length, 0)) {
				if (clip->flavor == CLIPTEXT) {
					for (index = 0; index < length; index++) {
						data[index * 2] = (uint8)(OsxGuestMacRomanCharacter(data[length + index]) >> 8);
						data[index * 2 + 1] = (uint8)OsxGuestMacRomanCharacter(data[length + index]);
					}
					length *= 2;
				} else
					memmove(data, data + length, length);
				if (length >= 2 && data[0] == 0xfe && data[1] == 0xff) {
					length -= 2;
					memmove(data, data + 2, length);
				}
				hash = OsxGuestAdler32(data, length);
				if (hash != clip->lasthash) {
					OsxGuestWriteHostClipboard(data, length);
					if (machine->log) {
						fprintf(machine->log, "clipboard to host %u bytes\n", length);
						fflush(machine->log);
					}
				}
				clip->lasthash = hash;
			}
			free(data);
		}
		clip->lastscrap = clip->scrap;
		return OsxGuestRelease(clip, cpu, 7, clip->data);
	case 7:
		return OsxGuestRelease(clip, cpu, 8, clip->block);
	}
	return OsxGuestFinish(clip, cpu);
}

int OsxGuestClipSyscall(OsxMachine *machine, OsxCpu *cpu)
{
	OsxClip *clip;
	uint8 *data;
	uint8 *host;
	uint32 length;
	uint32 hash;
	uint32 index;
	uint64 now;
	clip = &machine->clip;
	if (!clip->ready || !(cpu->msr & OSXMSRPR))
		return 0;
	if (clip->busy) {
		if (cpu->pc == clip->pc && cpu->gpr[1] == clip->stack) {
			host = OsxGuestHost(cpu, clip->stack, OSXTLBREAD);
			if (host && (uint32)(host - cpu->ram) == clip->stackphysical) {
				if (clip->task == CLIPTASKPUT)
					return OsxGuestAdvancePut(machine, clip, cpu, cpu->gpr[3]);
				return OsxGuestAdvanceGet(machine, clip, cpu, cpu->gpr[3]);
			}
		}
		if (OsxGuestHostNanoseconds() - clip->started > CLIPTIMEOUT) {
			clip->busy = 0;
			free(clip->pending);
			clip->pending = NULL;
		}
		return 0;
	}
	if ((cpu->gpr[0] != CLIPMACHMSG && cpu->gpr[0] != CLIPMACHMSGOVERWRITE) || (cpu->gpr[4] & (CLIPMACHSEND | CLIPMACHRECEIVE)) != CLIPMACHRECEIVE)
		return 0;
	if (clip->functions[OSXCLIPDOCK]) {
		if (OsxGuestReadWord(cpu, clip->functions[OSXCLIPDOCK] + 20) != clip->functions[OSXCLIPDOCKCOMMANDS])
			return 0;
		clip->functions[OSXCLIPDOCK] = 0;
	}
	now = OsxGuestHostNanoseconds();
	if (now - clip->lastput < CLIPPUTINTERVAL || !OsxGuestEventLoopThread(clip, cpu))
		return 0;
	clip->lastput = now;
	data = OsxGuestReadHostClipboard(&length);
	if (data && length > 0 && length <= CLIPMAXBYTES) {
		host = (uint8 *)realloc(data, length + length / 2 + 1);
		if (host == NULL) {
			free(data);
			return 0;
		}
		data = host;
		for (index = 0; index < length / 2; index++)
			data[length + index] = OsxGuestMacRomanByte(((uint32)data[index * 2] << 8) | data[index * 2 + 1]);
		hash = OsxGuestAdler32(data, length);
		if (hash != clip->lasthash) {
			clip->lasthash = hash;
			clip->pending = data;
			clip->pendinglength = length;
			if (OsxGuestBegin(machine, cpu, CLIPTASKPUT))
				return 1;
			clip->pending = NULL;
			clip->lasthash = 0;
		}
	}
	free(data);
	if (now - clip->lastget < CLIPGETINTERVAL)
		return 0;
	clip->lastget = now;
	return OsxGuestBegin(machine, cpu, CLIPTASKGET);
}
