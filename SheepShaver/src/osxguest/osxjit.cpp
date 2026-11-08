/*
 *  osxjit.cpp - OSX-specific JIT routines
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
#include <stddef.h>
#include <stdint.h>
#include "osxcpu.h"
/* OSXJITNOFUSED forces off AVX2 ops for testing. Don't release with it on! */
#if !defined(OSXJITNOFUSED)
#define OSXJITNOFUSED 0
#endif
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

							x64 OSX JIT

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#if OSXHOSTX64

#define OSXJITSETS 65536
#define OSXJITWAYS 4
#define OSXJITBLOCKS (OSXJITSETS * OSXJITWAYS)
#define OSXJITFRONT 16384
#define OSXJITRECORDS (256 * 1024)
#define OSXJITBYTES (64 * 1024 * 1024)
#define OSXJITLIMIT 32
#define OSXJITSLOWPATH 0
#define OSXJITRESERVE 65536
#define OSXJITMISSES 1024
#define OSXJITENTRY 25
#define OSXJITHOT 3
#define OSXJITCONTINUE 0
#define OSXJITINTERPRET 1
#define OSXJITLINK 2
#define OSXJITDYNAMIC 3
#define OSXJITOFFSET(member) ((uint32)offsetof(OsxCpu, member))
#define OSXJITGPR(reg) (OSXJITOFFSET(gpr) + (uint32)(reg) * 4)
#define OSXJITFPR(reg) (OSXJITOFFSET(fpr) + (uint32)(reg) * 8)

typedef int (OSXJITABI *OsxNativeFunction)(OsxCpu *cpu);

typedef struct OsxNativeRecord {
	uint32 address;
	uint32 version;
	uint8 *host;
	uint32 *versionaddress;
	uint8 *target;
} OsxNativeRecord;

typedef struct OsxNativeBlock {
	uint32 physical;
	uint32 mode;
	uint32 version;
	int count;
	OsxNativeFunction function;
} OsxNativeBlock;

typedef struct OsxNativeState {
	uint8 *writebase;
	uint8 *executebase;
	uint32 used;
	uint32 *versions;
	uint8 *compiledpages;
	uint32 pagecount;
	OsxNativeRecord *records;
	uint8 *checkthunk;
	uint8 *missthunk;
	uint8 *finishthunk;
	uint8 *linkthunk;
	uint8 *interpretthunk;
	uint8 *fillthunk;
	uint8 *timethunk;
	uint8 *linkcallthunk;
	uint8 *dynamicthunk;
	uint8 *translatethunks[2][6];
	uint32 thunkbytes;
	uint64 compiled;
	uint64 linked;
	uint64 interpreted;
	int shuffle;
	int fused;
	int rounding;
	uint32 vectorconstants[9][4];
#if OSXPROFILEJIT
	FILE *tmpmap;
	uint64 tmpstats[16];
	uint32 tmpinterp[65536];
#endif
	uint8 victims[OSXJITSETS];
	OsxNativeBlock front[OSXJITFRONT];
	OsxNativeBlock blocks[OSXJITBLOCKS];
} OsxNativeState;

typedef struct OsxNativeEmitter {
	uint8 *start;
	uint8 *cursor;
	uint8 *executestart;
	uint32 physical;
	uint8 *finishthunk;
	uint8 *interpretthunk;
	uint8 *timethunk;
	uint8 *linkcallthunk;
	uint8 *dynamicthunk;
	uint8 *(*translatethunks)[6];
	int deterministic;
	uint32 enabled;
	uint32 vectorvalue;
	int shuffle;
	int fused;
	int rounding;
	int bytemask;
	const uint32 (*vectorconstants)[4];
	uint8 *misses[OSXJITMISSES];
	int misscounts[OSXJITMISSES];
	int misscount;
#if OSXJITSLOWPATH
	int slowactive;
	uint8 *slowjumps[OSXJITLIMIT * 8];
	int slowowners[OSXJITLIMIT * 8];
	int slowjumpcount;
	uint32 slowinstructions[OSXJITLIMIT];
	int slowcounts[OSXJITLIMIT];
	uint8 *slowresumes[OSXJITLIMIT];
	int slowcount;
#endif
} OsxNativeEmitter;

static void Byte(OsxNativeEmitter *emitter, uint32 value)
{
	*emitter->cursor++ = (uint8)value;
}

static void Word(OsxNativeEmitter *emitter, uint32 value)
{
	memcpy(emitter->cursor, &value, 4);
	emitter->cursor += 4;
}

static void RegisterOp(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, int wide)
{
	uint32 prefix;
	prefix = 0x40;
	if (wide)
		prefix |= 8;
	if (source & 8)
		prefix |= 4;
	if (target & 8)
		prefix |= 1;
	if (prefix != 0x40)
		Byte(emitter, prefix);
	Byte(emitter, opcode);
	Byte(emitter, 0xc0 | ((source & 7) << 3) | (target & 7));
}

static void CpuMemory(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset, int wide)
{
	uint32 prefix;
	prefix = 0x40;
	if (wide)
		prefix |= 8;
	if (reg & 8)
		prefix |= 4;
	if (prefix != 0x40)
		Byte(emitter, prefix);
	Byte(emitter, opcode);
	if (offset < 128) {
		Byte(emitter, 0x43 | ((reg & 7) << 3));
		Byte(emitter, offset);
	} else {
		Byte(emitter, 0x83 | ((reg & 7) << 3));
		Word(emitter, offset);
	}
}

static void Immediate(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	if (reg & 8)
		Byte(emitter, 0x41);
	Byte(emitter, 0xb8 | (reg & 7));
	Word(emitter, value);
}

static void ImmediateOp(OsxNativeEmitter *emitter, int operation, int reg, uint32 value)
{
	if (reg & 8)
		Byte(emitter, 0x41);
	if (value <= 127 || value >= 0xffffff80u) {
		Byte(emitter, 0x83);
		Byte(emitter, 0xc0 | (operation << 3) | (reg & 7));
		Byte(emitter, value);
	} else {
		Byte(emitter, 0x81);
		Byte(emitter, 0xc0 | (operation << 3) | (reg & 7));
		Word(emitter, value);
	}
}

static void Shift(OsxNativeEmitter *emitter, int operation, int reg, uint32 amount)
{
	if (amount == 0)
		return;
	if (reg & 8)
		Byte(emitter, 0x41);
	Byte(emitter, 0xc1);
	Byte(emitter, 0xc0 | (operation << 3) | (reg & 7));
	Byte(emitter, amount);
}

static uint8 *Jump(OsxNativeEmitter *emitter, int condition)
{
	uint8 *patch;
	Byte(emitter, 0x0f);
	Byte(emitter, 0x80 | condition);
	patch = emitter->cursor;
	Word(emitter, 0);
	return patch;
}

static void Patch(uint8 *patch, uint8 *target)
{
	int32 distance;
	distance = (int32)(target - patch - 4);
	memcpy(patch, &distance, 4);
}

static void Miss(OsxNativeEmitter *emitter, int condition, int count)
{
#if OSXJITSLOWPATH
	if (emitter->slowactive) {
		emitter->slowjumps[emitter->slowjumpcount] = Jump(emitter, condition);
		emitter->slowowners[emitter->slowjumpcount++] = emitter->slowcount;
		return;
	}
#endif
	emitter->misses[emitter->misscount] = Jump(emitter, condition);
	emitter->misscounts[emitter->misscount++] = count;
}

static void Charge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	ImmediateOp(emitter, 5, 12, (uint32)count);
}

static void Account(OsxNativeEmitter *emitter, int count)
{
	Charge(emitter, count);
}

static void Settle(OsxNativeEmitter *emitter)
{
	CpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	RegisterOp(emitter, 0x89, 0, 13, 0);
	RegisterOp(emitter, 0x29, 0, 12, 0);
	CpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions), 1);
}

static void Release(OsxNativeEmitter *emitter, uint32 code)
{
	Immediate(emitter, 0, code);
	Byte(emitter, 0x41); Byte(emitter, 0x5d);
	Byte(emitter, 0x41); Byte(emitter, 0x5c);
	Byte(emitter, 0x5b);
	Byte(emitter, 0xc3);
}

static void Leave(OsxNativeEmitter *emitter, uint32 code)
{
	Settle(emitter);
	CpuMemory(emitter, 0x89, 12, OSXJITOFFSET(slice), 0);
	Release(emitter, code);
}

static void Quad(OsxNativeEmitter *emitter, uint64 value)
{
	memcpy(emitter->cursor, &value, 8);
	emitter->cursor += 8;
}

static void JumpTo(OsxNativeEmitter *emitter, uint8 *target)
{
	int32 distance;
	distance = (int32)(target - (emitter->executestart + (emitter->cursor - emitter->start) + 4));
	Word(emitter, (uint32)distance);
}

static void Return(OsxNativeEmitter *emitter, int count)
{
	Account(emitter, count);
	Byte(emitter, 0xe9);
	JumpTo(emitter, emitter->interpretthunk);
}

static void DynamicExit(OsxNativeEmitter *emitter, int count)
{
	Account(emitter, count);
	Byte(emitter, 0x0f); Byte(emitter, 0x8e);
	JumpTo(emitter, emitter->finishthunk);
	Byte(emitter, 0xe9);
	JumpTo(emitter, emitter->dynamicthunk);
}

static void TranslateThunk(OsxNativeEmitter *emitter, uint8 *fillthunk, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	uint8 *again;
	retry = emitter->cursor;
	if (limit == 0xfff) {
		RegisterOp(emitter, 0x89, 1, 0, 0);
	} else {
		Byte(emitter, 0x8d); Byte(emitter, 0x48); Byte(emitter, 0xfff - limit);
	}
	ImmediateOp(emitter, 4, 1, 0xfffff000);
	RegisterOp(emitter, 0x89, 2, 0, 0);
	Shift(emitter, 5, 2, 8);
	ImmediateOp(emitter, 4, 2, OSXTLBMASK << 4);
	if (store)
		CpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(writetlb), 1);
	else
		CpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(readtlb), 1);
	RegisterOp(emitter, 0x01, 8, 2, 1);
	Byte(emitter, 0x41); Byte(emitter, 0x39); Byte(emitter, 0x08);
	slow = Jump(emitter, 5);
	RegisterOp(emitter, 0x89, 2, 0, 0);
	Shift(emitter, 5, 2, 28);
	CpuMemory(emitter, 0x8b, 9, OSXJITOFFSET(datasr), 1);
	Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x14); Byte(emitter, 0x91);
	Byte(emitter, 0x41); Byte(emitter, 0x39); Byte(emitter, 0x50); Byte(emitter, 4);
	segment = Jump(emitter, 5);
	Byte(emitter, 0x4d); Byte(emitter, 0x8b); Byte(emitter, 0x40); Byte(emitter, 8);
	ImmediateOp(emitter, 4, 0, 0xfff);
	RegisterOp(emitter, 0x31, 1, 1, 0);
	Byte(emitter, 0xc3);
	Patch(slow, emitter->cursor);
	RegisterOp(emitter, 0x89, 1, 0, 0);
	ImmediateOp(emitter, 4, 1, 0xfffff000);
	Byte(emitter, 0x41); Byte(emitter, 0x39); Byte(emitter, 0x08);
	page = Jump(emitter, 5);
	ImmediateOp(emitter, 1, 1, 1);
	Byte(emitter, 0xc3);
	Patch(page, emitter->cursor);
	Patch(segment, emitter->cursor);
	if (store)
		Immediate(emitter, 9, OSXTLBWRITE);
	else
		Immediate(emitter, 9, OSXTLBREAD);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xec); Byte(emitter, 8);
	Byte(emitter, 0xe8);
	JumpTo(emitter, fillthunk);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xc4); Byte(emitter, 8);
	RegisterOp(emitter, 0x85, 1, 1, 0);
	again = Jump(emitter, 5);
	Patch(again, retry);
	ImmediateOp(emitter, 1, 1, 1);
	Byte(emitter, 0xc3);
}

static void EmitThunks(OsxNativeState *state)
{
	static const uint32 translatelimits[6] = { 0xfff, 0xffe, 0xffc, 0xff8, 0xff0, 0xfe0 };
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
#if OSXPROFILEJIT
	FILE *thunks;
#endif
	emitter.start = state->writebase;
	emitter.cursor = emitter.start;
	emitter.executestart = state->executebase;
	emitter.misscount = 0;
#if OSXJITSLOWPATH
	emitter.slowactive = 0;
	emitter.slowjumpcount = 0;
	emitter.slowcount = 0;
#endif
	state->checkthunk = state->executebase;
	Byte(&emitter, 0x45); Byte(&emitter, 0x3b); Byte(&emitter, 0x1a);
	misses[0] = Jump(&emitter, 5);
	CpuMemory(&emitter, 0x8b, 0, OSXJITOFFSET(fetchtlb), 1);
	RegisterOp(&emitter, 0x89, 2, 11, 0);
	Shift(&emitter, 5, 2, 8);
	ImmediateOp(&emitter, 4, 2, OSXTLBMASK << 4);
	RegisterOp(&emitter, 0x01, 0, 2, 1);
	RegisterOp(&emitter, 0x89, 1, 11, 0);
	ImmediateOp(&emitter, 4, 1, 0xfffff000);
	Byte(&emitter, 0x39); Byte(&emitter, 0x08);
	misses[1] = Jump(&emitter, 5);
	RegisterOp(&emitter, 0x89, 1, 11, 0);
	Shift(&emitter, 5, 1, 28);
	CpuMemory(&emitter, 0x8b, 9, OSXJITOFFSET(fetchsr), 1);
	Byte(&emitter, 0x41); Byte(&emitter, 0x8b); Byte(&emitter, 0x0c); Byte(&emitter, 0x89);
	Byte(&emitter, 0x39); Byte(&emitter, 0x48); Byte(&emitter, 4);
	misses[2] = Jump(&emitter, 5);
	Byte(&emitter, 0x48); Byte(&emitter, 0x8b); Byte(&emitter, 0x40); Byte(&emitter, 8);
	Byte(&emitter, 0x49); Byte(&emitter, 0x3b); Byte(&emitter, 0x42); Byte(&emitter, (uint32)offsetof(OsxNativeRecord, host));
	misses[3] = Jump(&emitter, 5);
	Byte(&emitter, 0x49); Byte(&emitter, 0x8b); Byte(&emitter, 0x4a); Byte(&emitter, (uint32)offsetof(OsxNativeRecord, versionaddress));
	Byte(&emitter, 0x8b); Byte(&emitter, 0x09);
	Byte(&emitter, 0x41); Byte(&emitter, 0x3b); Byte(&emitter, 0x4a); Byte(&emitter, (uint32)offsetof(OsxNativeRecord, version));
	misses[4] = Jump(&emitter, 5);
	CpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(fetchhost), 1);
	RegisterOp(&emitter, 0x89, 1, 11, 0);
	ImmediateOp(&emitter, 4, 1, 0xfffff000);
	ImmediateOp(&emitter, 1, 1, 1);
	CpuMemory(&emitter, 0x89, 1, OSXJITOFFSET(fetchtag), 0);
	RegisterOp(&emitter, 0x31, 0, 0, 0);
	for (index = 0; index < 5; index++)
		Patch(misses[index], emitter.cursor);
	Byte(&emitter, 0xc3);
	state->missthunk = state->executebase + (emitter.cursor - emitter.start);
	CpuMemory(&emitter, 0x89, 10, OSXJITOFFSET(jitlink), 1);
	Leave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = state->executebase + (emitter.cursor - emitter.start);
	Leave(&emitter, OSXJITCONTINUE);
	state->linkthunk = state->executebase + (emitter.cursor - emitter.start);
	Leave(&emitter, OSXJITLINK);
	state->interpretthunk = state->executebase + (emitter.cursor - emitter.start);
	Leave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = state->executebase + (emitter.cursor - emitter.start);
	Byte(&emitter, 0x58);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xe8); Byte(&emitter, 9);
	CpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(jitlink), 1);
	Leave(&emitter, OSXJITLINK);
	state->dynamicthunk = state->executebase + (emitter.cursor - emitter.start);
	RegisterOp(&emitter, 0x89, 0, 11, 0);
	Shift(&emitter, 5, 0, 2);
	ImmediateOp(&emitter, 4, 0, OSXJITRECORDS - 1);
	Byte(&emitter, 0x48); Byte(&emitter, 0xc1); Byte(&emitter, 0xe0); Byte(&emitter, 5);
	Byte(&emitter, 0x49); Byte(&emitter, 0xba);
	Quad(&emitter, (uint64)(uintptr_t)state->records);
	RegisterOp(&emitter, 0x01, 10, 0, 1);
	Byte(&emitter, 0xe8);
	JumpTo(&emitter, state->checkthunk);
	Byte(&emitter, 0x0f); Byte(&emitter, 0x85);
	JumpTo(&emitter, state->missthunk);
	Byte(&emitter, 0x41); Byte(&emitter, 0xff); Byte(&emitter, 0x62); Byte(&emitter, (uint32)offsetof(OsxNativeRecord, target));
	state->fillthunk = state->executebase + (emitter.cursor - emitter.start);
	Byte(&emitter, 0x50);
	Byte(&emitter, 0x41); Byte(&emitter, 0x52);
	Byte(&emitter, 0x41); Byte(&emitter, 0x53);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xec); Byte(&emitter, 0x30);
	Byte(&emitter, 0x0f); Byte(&emitter, 0x29); Byte(&emitter, 0x6c); Byte(&emitter, 0x24); Byte(&emitter, 0x20);
	RegisterOp(&emitter, 0x89, 1, 3, 1);
	RegisterOp(&emitter, 0x89, 2, 0, 0);
	RegisterOp(&emitter, 0x89, 8, 9, 0);
	Byte(&emitter, 0x48); Byte(&emitter, 0xb8);
	Quad(&emitter, (uint64)(uintptr_t)OsxCpuFillTlb);
	Byte(&emitter, 0xff); Byte(&emitter, 0xd0);
	Byte(&emitter, 0x0f); Byte(&emitter, 0x28); Byte(&emitter, 0x6c); Byte(&emitter, 0x24); Byte(&emitter, 0x20);
	RegisterOp(&emitter, 0x89, 1, 0, 0);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xc4); Byte(&emitter, 0x30);
	Byte(&emitter, 0x41); Byte(&emitter, 0x5b);
	Byte(&emitter, 0x41); Byte(&emitter, 0x5a);
	Byte(&emitter, 0x58);
	Byte(&emitter, 0xc3);
	state->timethunk = state->executebase + (emitter.cursor - emitter.start);
	Byte(&emitter, 0x41); Byte(&emitter, 0x53);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xec); Byte(&emitter, 0x20);
	RegisterOp(&emitter, 0x89, 1, 3, 1);
	Byte(&emitter, 0x48); Byte(&emitter, 0xb8);
	Quad(&emitter, (uint64)(uintptr_t)OsxCpuTimebase);
	Byte(&emitter, 0xff); Byte(&emitter, 0xd0);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xc4); Byte(&emitter, 0x20);
	Byte(&emitter, 0x41); Byte(&emitter, 0x5b);
	Byte(&emitter, 0xc3);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = state->executebase + (emitter.cursor - emitter.start);
		TranslateThunk(&emitter, state->fillthunk, index / 6, translatelimits[index % 6]);
	}
	state->thunkbytes = ((uint32)(emitter.cursor - emitter.start) + 63) & ~63u;
	state->used = state->thunkbytes;
#if OSXPROFILEJIT
	thunks = fopen("osxjitthunks.txt", "w");
	if (thunks) {
		fprintf(thunks, "check %x\nmiss %x\nfinish %x\nlink %x\ninterpret %x\nlinkcall %x\ndynamic %x\nfill %x\ntime %x\n", 0, (uint32)(state->missthunk - state->executebase), (uint32)(state->finishthunk - state->executebase), (uint32)(state->linkthunk - state->executebase), (uint32)(state->interpretthunk - state->executebase), (uint32)(state->linkcallthunk - state->executebase), (uint32)(state->dynamicthunk - state->executebase), (uint32)(state->fillthunk - state->executebase), (uint32)(state->timethunk - state->executebase));
		for (index = 0; index < 12; index++)
			fprintf(thunks, "translate%d_%d %x\n", index / 6, index % 6, (uint32)(state->translatethunks[index / 6][index % 6] - state->executebase));
		fprintf(thunks, "end %x\n", state->thunkbytes);
		fclose(thunks);
	}
#endif
}

static void Exit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		DynamicExit(emitter, count);
		return;
	}
	Account(emitter, count);
	Byte(emitter, 0x0f); Byte(emitter, 0x8e);
	JumpTo(emitter, emitter->finishthunk);
	Byte(emitter, 0xe9);
	Word(emitter, 0);
	Byte(emitter, 0xe8);
	JumpTo(emitter, emitter->linkcallthunk);
}

static void Advance(OsxNativeEmitter *emitter)
{
	ImmediateOp(emitter, 0, 11, 4);
}

static void Condition(OsxNativeEmitter *emitter, int field, int issigned)
{
	uint32 shift;
	Immediate(emitter, 0, 2);
	Immediate(emitter, 1, 4);
	Immediate(emitter, 2, 8);
	Byte(emitter, 0x0f);
	if (issigned)
		Byte(emitter, 0x4f);
	else
		Byte(emitter, 0x47);
	Byte(emitter, 0xc1);
	Byte(emitter, 0x0f);
	if (issigned)
		Byte(emitter, 0x4c);
	else
		Byte(emitter, 0x42);
	Byte(emitter, 0xc2);
	CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(xer), 0);
	Shift(emitter, 5, 2, 31);
	RegisterOp(emitter, 0x09, 0, 2, 0);
	shift = (uint32)(7 - field) * 4;
	Shift(emitter, 4, 0, shift);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	ImmediateOp(emitter, 4, 1, ~(15u << shift));
	RegisterOp(emitter, 0x09, 0, 1, 0);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
}

static void Record(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		RegisterOp(emitter, 0x85, 0, 0, 0);
		Condition(emitter, 0, 1);
	}
}

static void RequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRFP);
	Miss(emitter, 4, count);
}

static void RecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr), 0);
		Shift(emitter, 5, 0, 4);
		ImmediateOp(emitter, 4, 0, 0x0f000000);
		CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
		ImmediateOp(emitter, 4, 1, 0xf0ffffff);
		RegisterOp(emitter, 0x09, 0, 1, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
	}
}

static void FloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	RegisterOp(emitter, 0x89, 2, reg, 1);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 52);
	ImmediateOp(emitter, 4, 2, 0x7ff);
	ImmediateOp(emitter, 7, 2, 0x7ff);
	Miss(emitter, 4, count);
	if (normal) {
		RegisterOp(emitter, 0x85, 2, 2, 0);
		Miss(emitter, 4, count);
	}
}

static void Setcc(OsxNativeEmitter *emitter, uint32 condition, int reg)
{
	Byte(emitter, 0x0f);
	Byte(emitter, 0x90 | condition);
	Byte(emitter, 0xc0 | reg);
}

static void FloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	Byte(emitter, 0x66); Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0x7e); Byte(emitter, 0xc0);
	FloatExponent(emitter, 0, count, 0);
	RegisterOp(emitter, 0x85, 2, 2, 0);
	zero = Jump(emitter, 4);
	if (single) {
		ImmediateOp(emitter, 7, 2, 897);
		Miss(emitter, 2, count);
	}
	RegisterOp(emitter, 0x89, 2, 0, 1);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 63);
	ImmediateOp(emitter, 0, 2, 1);
	Shift(emitter, 4, 2, 14);
	Byte(emitter, 0xe9);
	merge = emitter->cursor;
	Word(emitter, 0);
	Patch(zero, emitter->cursor);
	RegisterOp(emitter, 0x89, 2, 0, 1);
	RegisterOp(emitter, 0x01, 2, 2, 1);
	Miss(emitter, 5, count);
	RegisterOp(emitter, 0x89, 2, 0, 1);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 63);
	Shift(emitter, 4, 2, 16);
	ImmediateOp(emitter, 1, 2, 0x2000);
	Patch(merge, emitter->cursor);
	CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr), 0);
	ImmediateOp(emitter, 4, 0, ~0x1f000u);
	RegisterOp(emitter, 0x09, 0, 2, 0);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(fpscr), 0);
}

static void FloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x10); Byte(emitter, 0x83 | (vector << 3));
	Word(emitter, OSXJITFPR(reg));
}

static void FloatRounding(OsxNativeEmitter *emitter, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(fpscr));
	Word(emitter, 3);
	Miss(emitter, 5, count);
}

static void FloatNarrow(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x5a); Byte(emitter, 0xc0);
	Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x5a); Byte(emitter, 0xc0);
}

static void FloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	uint32 shift;
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
	FloatExponent(emitter, 0, count, 0);
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
	FloatExponent(emitter, 0, count, 0);
	RegisterOp(emitter, 0x31, 0, 0, 0);
	RegisterOp(emitter, 0x31, 1, 1, 0);
	RegisterOp(emitter, 0x31, 2, 2, 0);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x2e); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(right));
	Setcc(emitter, 2, 0);
	Setcc(emitter, 7, 1);
	Setcc(emitter, 4, 2);
	Shift(emitter, 4, 0, 3);
	Shift(emitter, 4, 1, 2);
	RegisterOp(emitter, 0x01, 2, 2, 0);
	RegisterOp(emitter, 0x09, 0, 1, 0);
	RegisterOp(emitter, 0x09, 0, 2, 0);
	RegisterOp(emitter, 0x89, 1, 0, 0);
	Shift(emitter, 4, 1, 12);
	CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(fpscr), 0);
	ImmediateOp(emitter, 4, 2, ~0xf000u);
	RegisterOp(emitter, 0x09, 2, 1, 0);
	CpuMemory(emitter, 0x89, 2, OSXJITOFFSET(fpscr), 0);
	shift = (uint32)(7 - field) * 4;
	Shift(emitter, 4, 0, shift);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	ImmediateOp(emitter, 4, 1, ~(0xfu << shift));
	RegisterOp(emitter, 0x09, 1, 0, 0);
	CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(cr), 0);
}

static void FloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	FloatLoad(emitter, 0, right);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x2c); Byte(emitter, 0xc0);
	ImmediateOp(emitter, 7, 0, 0x80000000u);
	Miss(emitter, 4, count);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x2a); Byte(emitter, 0xc8);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x2e); Byte(emitter, 0xc8);
	exact = Jump(emitter, 4);
	Byte(emitter, 0x81); Byte(emitter, 0x8b);
	Word(emitter, OSXJITOFFSET(fpscr));
	Word(emitter, 0x82000000u);
	Patch(exact, emitter->cursor);
	CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 0);
	Byte(emitter, 0xc7); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(target) + 4);
	Word(emitter, 0xfff80000u);
}

static void FloatSingleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(reg));
	Word(emitter, 0x1fffffff);
	Miss(emitter, 5, count);
	Byte(emitter, 0x8b); Byte(emitter, 0x93);
	Word(emitter, OSXJITFPR(reg) + 4);
	Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 20);
	Byte(emitter, 0x81); Byte(emitter, 0xe2); Word(emitter, 0x7ff);
	Byte(emitter, 0x81); Byte(emitter, 0xea); Word(emitter, 897);
	Byte(emitter, 0x81); Byte(emitter, 0xfa); Word(emitter, 254);
	Miss(emitter, 3, count);
}

static void FloatSingleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	FloatSingleOperand(emitter, left, count);
	FloatSingleOperand(emitter, multiplier, count);
	Byte(emitter, 0x8b); Byte(emitter, 0x93);
	Word(emitter, OSXJITFPR(right) + 4);
	Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 20);
	Byte(emitter, 0x81); Byte(emitter, 0xe2); Word(emitter, 0x7ff);
	Byte(emitter, 0x81); Byte(emitter, 0xfa); Word(emitter, 0x7ff);
	Miss(emitter, 4, count);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x59); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(multiplier));
	FloatLoad(emitter, 1, right);
	if (extended == 28 || extended == 30) {
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x76); Byte(emitter, 0xd2);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xf2); Byte(emitter, 63);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x57); Byte(emitter, 0xca);
	}
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x58); Byte(emitter, 0xc1);
	if (extended >= 30) {
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x76); Byte(emitter, 0xc9);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xf1); Byte(emitter, 63);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x57); Byte(emitter, 0xc1);
	}
	FloatNarrow(emitter);
	FloatClass(emitter, target, count, 1);
}

static int Float(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 extended;
	int target;
	int left;
	int right;
	int multiplier;
	int single;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	single = (instruction >> 26) == 59;
	if (!single && !(instruction & 0x20)) {
		switch (extended) {
		case 40: case 72: case 136: case 264:
			RequireFloat(emitter, count);
			CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
			if (extended != 72) {
				Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0xba);
				if (extended == 40)
					Byte(emitter, 0xf8);
				else if (extended == 136)
					Byte(emitter, 0xe8);
				else
					Byte(emitter, 0xf0);
				Byte(emitter, 63);
			}
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
			break;
		case 0: case 32:
			RequireFloat(emitter, count);
			FloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			RequireFloat(emitter, count);
			FloatRounding(emitter, count);
			CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
			FloatExponent(emitter, 0, count, 0);
			FloatLoad(emitter, 0, right);
			FloatNarrow(emitter);
			FloatClass(emitter, target, count, 1);
			break;
		case 15:
			RequireFloat(emitter, count);
			FloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		RecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		if (!emitter->fused && single) {
			RequireFloat(emitter, count);
			FloatRounding(emitter, count);
			FloatSingleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			RecordFloat(emitter, instruction);
			return 1;
		}
		if (!emitter->fused)
			return 0;
		RequireFloat(emitter, count);
		FloatRounding(emitter, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
		FloatExponent(emitter, 0, count, 0);
		CpuMemory(emitter, 0x8b, 0, OSXJITFPR(multiplier), 1);
		FloatExponent(emitter, 0, count, 0);
		CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
		FloatExponent(emitter, 0, count, 0);
		FloatLoad(emitter, 0, left);
		FloatLoad(emitter, 1, multiplier);
		Byte(emitter, 0xc4); Byte(emitter, 0xe2); Byte(emitter, 0xf1);
		if (extended == 28 || extended == 30)
			Byte(emitter, 0xab);
		else
			Byte(emitter, 0xa9);
		Byte(emitter, 0x83);
		Word(emitter, OSXJITFPR(right));
		if (extended >= 30) {
			Byte(emitter, 0x66); Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0x7e); Byte(emitter, 0xc0);
			Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0xba); Byte(emitter, 0xf8); Byte(emitter, 63);
			Byte(emitter, 0x66); Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0x6e); Byte(emitter, 0xc0);
		}
		if (single)
			FloatNarrow(emitter);
		FloatClass(emitter, target, count, single);
		RecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	RequireFloat(emitter, count);
	FloatRounding(emitter, count);
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
	FloatExponent(emitter, 0, count, 0);
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
	FloatExponent(emitter, 0, count, extended == 18);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f);
	if (extended == 18)
		Byte(emitter, 0x5e);
	else if (extended == 20)
		Byte(emitter, 0x5c);
	else if (extended == 21)
		Byte(emitter, 0x58);
	else
		Byte(emitter, 0x59);
	Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(right));
	if (single)
		FloatNarrow(emitter);
	FloatClass(emitter, target, count, single);
	RecordFloat(emitter, instruction);
	return 1;
}

static uint32 RotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void Address(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
	else
		RegisterOp(emitter, 0x31, 0, 0, 0);
	if (index >= 0)
		CpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
	else if (immediate)
		ImmediateOp(emitter, 0, 0, immediate);
	if (update)
		RegisterOp(emitter, 0x89, 10, 0, 0);
}

static int LimitIndex(uint32 limit)
{
	switch (limit) {
	case 0xfff: return 0;
	case 0xffe: return 1;
	case 0xffc: return 2;
	case 0xff8: return 3;
	case 0xff0: return 4;
	}
	return 5;
}

static void Translate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	Byte(emitter, 0xe8);
	JumpTo(emitter, emitter->translatethunks[store != 0][LimitIndex(limit)]);
	Miss(emitter, 5, count);
}

static int Multiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
{
	int first;
	int base;
	uint32 registers;
	uint8 *loop;
	first = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	registers = (uint32)(32 - first);
	if (opcode == 46 && base >= first)
		return 0;
	Address(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	Translate(emitter, opcode == 47, 0xffc, count);
	ImmediateOp(emitter, 7, 0, 0x1000 - registers * 4);
	Miss(emitter, 7, count);
	RegisterOp(emitter, 0x01, 8, 0, 1);
	Byte(emitter, 0x4c); Byte(emitter, 0x8d); Byte(emitter, 0x8b);
	Word(emitter, OSXJITGPR(first));
	Immediate(emitter, 1, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x10);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x11);
	} else {
		Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x11);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x10);
	}
	Byte(emitter, 0x49); Byte(emitter, 0x83); Byte(emitter, 0xc0); Byte(emitter, 4);
	Byte(emitter, 0x49); Byte(emitter, 0x83); Byte(emitter, 0xc1); Byte(emitter, 4);
	Byte(emitter, 0xff); Byte(emitter, 0xc9);
	Byte(emitter, 0x75);
	Byte(emitter, (uint32)(loop - (emitter->cursor + 1)) & 0xff);
	return 1;
}

static void CallHelper(OsxNativeEmitter *emitter, uint64 function)
{
	emitter->bytemask = 0;
	CpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xec); Byte(emitter, 0x20);
	Byte(emitter, 0x48); Byte(emitter, 0xb8);
	Quad(emitter, function);
	Byte(emitter, 0xff); Byte(emitter, 0xd0);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xc4); Byte(emitter, 0x20);
	CpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
}

static void VectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	Byte(emitter, 0x83); Byte(emitter, 0xbb);
	Word(emitter, OSXJITOFFSET(altivec));
	Byte(emitter, 0);
	Miss(emitter, 4, count);
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRVEC);
	Miss(emitter, 4, count);
}

static void VectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	VectorEnabled(emitter, count);
	RegisterOp(emitter, 0x89, 1, 3, 1);
	Immediate(emitter, 2, instruction);
	CallHelper(emitter, (uint64)(uintptr_t)function);
}

static void VectorMemory(OsxNativeEmitter *emitter, int store, int reg, uint32 offset)
{
	Byte(emitter, 0xf3); Byte(emitter, 0x0f);
	if (store)
		Byte(emitter, 0x7f);
	else
		Byte(emitter, 0x6f);
	Byte(emitter, 0x83 | (reg << 3));
	Word(emitter, offset);
}

static void VectorOperation(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	Byte(emitter, 0x66); Byte(emitter, 0x0f);
	if (opcode > 255)
		Byte(emitter, opcode >> 8);
	Byte(emitter, opcode);
	Byte(emitter, 0xc0 | (target << 3) | source);
}

static void VectorConstant(OsxNativeEmitter *emitter, int reg, int index)
{
	Byte(emitter, 0x48); Byte(emitter, 0xb9);
	Quad(emitter, (uint64)(uintptr_t)emitter->vectorconstants[index]);
	Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x6f);
	Byte(emitter, 0x01 | (reg << 3));
}

static void VectorShift(OsxNativeEmitter *emitter, int opcode, int operation, int reg, uint32 amount)
{
	VectorOperation(emitter, (uint32)opcode, operation, reg);
	Byte(emitter, amount);
}

static void VectorSaturated(OsxNativeEmitter *emitter, int reg)
{
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0xd7);
	Byte(emitter, 0xc0 | reg);
	ImmediateOp(emitter, 7, 0, 0xffff);
	Byte(emitter, 0x0f); Byte(emitter, 0x95); Byte(emitter, 0xc0);
	Byte(emitter, 0x0f); Byte(emitter, 0xb6); Byte(emitter, 0xc0);
	CpuMemory(emitter, 0x09, 0, OSXJITOFFSET(vscr), 0);
}

static void VectorMultiply(OsxNativeEmitter *emitter, uint32 instruction)
{
	emitter->bytemask = 0;
	VectorOperation(emitter, 0x6f, 2, 0);
	VectorOperation(emitter, 0xd5, 2, 1);
	VectorOperation(emitter, 0xe5, 0, 1);
	VectorOperation(emitter, 0x6f, 1, 2);
	VectorOperation(emitter, 0x61, 2, 0);
	VectorOperation(emitter, 0x69, 1, 0);
	if ((instruction & 0x3f) == 33) {
		VectorConstant(emitter, 3, 4);
		VectorOperation(emitter, 0xfe, 2, 3);
		VectorOperation(emitter, 0xfe, 1, 3);
	}
	VectorShift(emitter, 0x72, 4, 2, 15);
	VectorShift(emitter, 0x72, 4, 1, 15);
	VectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
	VectorOperation(emitter, 0x6f, 3, 0);
	VectorOperation(emitter, 0x6f, 4, 0);
	VectorShift(emitter, 0x71, 4, 3, 15);
	VectorOperation(emitter, 0x61, 0, 3);
	VectorOperation(emitter, 0x69, 4, 3);
	VectorOperation(emitter, 0xfe, 2, 0);
	VectorOperation(emitter, 0xfe, 1, 4);
	VectorOperation(emitter, 0x6f, 5, 2);
	VectorOperation(emitter, 0x6b, 2, 1);
	VectorOperation(emitter, 0x6f, 0, 2);
	VectorOperation(emitter, 0x6f, 3, 2);
	VectorOperation(emitter, 0x6f, 4, 2);
	VectorShift(emitter, 0x71, 4, 3, 15);
	VectorOperation(emitter, 0x61, 0, 3);
	VectorOperation(emitter, 0x69, 4, 3);
	VectorOperation(emitter, 0x76, 0, 5);
	VectorOperation(emitter, 0x76, 4, 1);
	VectorOperation(emitter, 0xdb, 0, 4);
	VectorSaturated(emitter, 0);
	VectorOperation(emitter, 0x6f, 0, 2);
}

static int VectorNative(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 kind;
	uint32 form;
	uint32 target;
	uint32 left;
	uint32 right;
	uint32 field;
	uint32 opcode;
	kind = instruction & 0x7ff;
	form = instruction & 0x3f;
	opcode = 0;
	if (kind == 0 || kind == 64 || kind == 128)
		opcode = 0xfc + (kind >> 6);
	else if (kind == 1024 || kind == 1088 || kind == 1152)
		opcode = 0xf8 + ((kind - 1024) >> 6);
	else if (kind == 1028)
		opcode = 0xdb;
	else if (kind == 1156)
		opcode = 0xeb;
	else if (kind == 1220)
		opcode = 0xef;
	if (!opcode && kind != 1092 && kind != 1284 && kind != 524 && kind != 588 && kind != 652 && kind != 270 && form != 32 && form != 33 && form != 42) {
		if (!emitter->shuffle)
			return 0;
		if (form != 43 && form != 44 && kind != 12 && kind != 76 && kind != 140 && kind != 268 && kind != 332 && kind != 396)
			return 0;
	}
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	left = OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16;
	right = OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16;
	field = (instruction >> 16) & 31;
	VectorEnabled(emitter, count);
	if (kind == 524 || kind == 588 || kind == 652) {
		if (emitter->vectorvalue != right)
			VectorMemory(emitter, 0, 0, right);
		if (kind == 524)
			field = (field & 15) >> 2;
		else if (kind == 588)
			field = (field & 7) >> 1;
		else
			field &= 3;
		VectorOperation(emitter, 0x70, 0, 0);
		Byte(emitter, field * 0x55);
		if (kind != 652) {
			field = (instruction >> 16) & 31;
			if (kind == 524) {
				VectorShift(emitter, 0x72, 2, 0, (3 - (field & 3)) * 8);
				VectorOperation(emitter, 0x60, 0, 0);
			} else {
				VectorShift(emitter, 0x72, 2, 0, (1 - (field & 1)) * 16);
			}
			VectorOperation(emitter, 0x61, 0, 0);
			VectorOperation(emitter, 0x70, 0, 0);
			Byte(emitter, 0);
		}
	} else {
		if (emitter->vectorvalue == right)
			VectorOperation(emitter, 0x6f, 1, 0);
		else
			VectorMemory(emitter, 0, 1, right);
		if (emitter->vectorvalue != left)
			VectorMemory(emitter, 0, 0, left);
		if (form == 32 || form == 33) {
			VectorMultiply(emitter, instruction);
		} else if (kind == 270) {
			VectorOperation(emitter, 0x6f, 2, 0);
			VectorOperation(emitter, 0xeb, 2, 1);
			VectorShift(emitter, 0x71, 2, 2, 8);
			VectorOperation(emitter, 0xef, 3, 3);
			VectorOperation(emitter, 0x75, 2, 3);
			VectorSaturated(emitter, 2);
			VectorOperation(emitter, 0x67, 0, 1);
			Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x70); Byte(emitter, 0xc0); Byte(emitter, 0xb1);
			Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x70); Byte(emitter, 0xc0); Byte(emitter, 0xb1);
		} else if (opcode) {
			VectorOperation(emitter, opcode, 0, 1);
		} else if (kind == 1092) {
			VectorOperation(emitter, 0xdf, 1, 0);
			VectorOperation(emitter, 0x6f, 0, 1);
		} else if (kind == 1284) {
			VectorOperation(emitter, 0xeb, 0, 1);
			VectorOperation(emitter, 0x76, 1, 1);
			VectorOperation(emitter, 0xef, 0, 1);
		} else if (form == 42) {
			VectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
			VectorOperation(emitter, 0xdb, 1, 2);
			VectorOperation(emitter, 0xdf, 2, 0);
			VectorOperation(emitter, 0x6f, 0, 2);
			VectorOperation(emitter, 0xeb, 0, 1);
		} else if (form == 43) {
			VectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
			VectorConstant(emitter, 4, 1);
			VectorOperation(emitter, 0xdb, 2, 4);
			VectorConstant(emitter, 4, 2);
			VectorOperation(emitter, 0xef, 2, 4);
			VectorOperation(emitter, 0x6f, 3, 2);
			VectorConstant(emitter, 4, 3);
			VectorOperation(emitter, 0x64, 3, 4);
			VectorOperation(emitter, 0x6f, 4, 3);
			VectorOperation(emitter, 0xeb, 4, 2);
			VectorOperation(emitter, 0x3800, 0, 4);
			VectorOperation(emitter, 0x76, 4, 4);
			VectorOperation(emitter, 0xef, 3, 4);
			VectorOperation(emitter, 0xeb, 3, 2);
			VectorOperation(emitter, 0x3800, 1, 3);
			VectorOperation(emitter, 0xeb, 0, 1);
		} else {
			VectorConstant(emitter, 2, 0);
			VectorOperation(emitter, 0x3800, 0, 2);
			VectorOperation(emitter, 0x3800, 1, 2);
			if (form == 44) {
				VectorOperation(emitter, 0x3a0f, 1, 0);
				Byte(emitter, (instruction >> 6) & 15);
				VectorOperation(emitter, 0x6f, 0, 1);
			} else {
				opcode = 0x60 + ((kind >> 6) & 3);
				if (kind & 0x100)
					opcode += 8;
				VectorOperation(emitter, opcode, 0, 1);
			}
			VectorOperation(emitter, 0x3800, 0, 2);
		}
	}
	VectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void VectorLanes(OsxNativeEmitter *emitter, uint32 kind, uint32 target, uint32 left, uint32 right)
{
	uint32 lanes;
	uint32 width;
	uint32 lane;
	uint32 offset;
	uint32 operation;
	width = 4;
	if ((kind & 0xc0) == 0x40)
		width = 2;
	else if ((kind & 0xc0) == 0)
		width = 1;
	operation = 0xe0;
	if ((kind & 0x3ff) == 0x204 || (kind & 0x3ff) == 0x244 || (kind & 0x3ff) == 0x284)
		operation = 0xe8;
	else if ((kind & 0x3ff) == 0x304 || (kind & 0x3ff) == 0x344 || (kind & 0x3ff) == 0x384)
		operation = 0xf8;
	lanes = 16 / width;
	for (lane = 0; lane < lanes; lane++) {
		offset = lane * width;
		if (width == 4) {
			CpuMemory(emitter, 0x8b, 0, left + offset, 0);
			CpuMemory(emitter, 0x8b, 1, right + offset, 0);
		} else {
			Byte(emitter, 0x0f);
			if (width == 2 && operation == 0xf8)
				Byte(emitter, 0xbf);
			else if (width == 2)
				Byte(emitter, 0xb7);
			else if (operation == 0xf8)
				Byte(emitter, 0xbe);
			else
				Byte(emitter, 0xb6);
			Byte(emitter, 0x83);
			Word(emitter, left + offset);
			Byte(emitter, 0x0f);
			if (width == 2)
				Byte(emitter, 0xb7);
			else
				Byte(emitter, 0xb6);
			Byte(emitter, 0x8b);
			Word(emitter, right + offset);
		}
		ImmediateOp(emitter, 4, 1, width * 8 - 1);
		Byte(emitter, 0xd3); Byte(emitter, operation);
		if (width == 2)
			Byte(emitter, 0x66);
		if (width == 1)
			Byte(emitter, 0x88);
		else
			Byte(emitter, 0x89);
		Byte(emitter, 0x83);
		Word(emitter, target + offset);
	}
}

static void VectorSumAcross(OsxNativeEmitter *emitter, uint32 target, uint32 left, uint32 right)
{
	uint32 word;
	uint8 *inside;
	Byte(emitter, 0x48); Byte(emitter, 0x63); Byte(emitter, 0x83);
	Word(emitter, right + 12);
	for (word = 0; word < 4; word++) {
		Byte(emitter, 0x48); Byte(emitter, 0x63); Byte(emitter, 0x8b);
		Word(emitter, left + word * 4);
		RegisterOp(emitter, 0x01, 0, 1, 1);
	}
	Byte(emitter, 0x48); Byte(emitter, 0x63); Byte(emitter, 0xc8);
	RegisterOp(emitter, 0x39, 1, 0, 1);
	inside = Jump(emitter, 4);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xf8); Byte(emitter, 63);
	ImmediateOp(emitter, 6, 0, 0x7fffffffu);
	Byte(emitter, 0x83); Byte(emitter, 0x8b);
	Word(emitter, OSXJITOFFSET(vscr));
	Byte(emitter, 1);
	Patch(inside, emitter->cursor);
	CpuMemory(emitter, 0x89, 0, target + 12, 0);
	for (word = 0; word < 3; word++) {
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, target + word * 4);
		Word(emitter, 0);
	}
}

static void VectorAllOnes(OsxNativeEmitter *emitter, int reg)
{
	VectorOperation(emitter, 0x75, reg, reg);
}

static void VectorFloatOperation(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	Byte(emitter, 0x0f);
	Byte(emitter, opcode);
	Byte(emitter, 0xc0 | (target << 3) | source);
}

static int VectorFloat(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 kind;
	uint32 form;
	uint32 target;
	uint32 operation;
	int reg;
	kind = instruction & 0x7ff;
	form = instruction & 0x3f;
	if (form != 46 && form != 47 && kind != 10 && kind != 74 &&
		kind != 1034 && kind != 1098 && kind != 266 && kind != 842 && (kind & 0x3ff) != 198 &&
		(kind & 0x3ff) != 454 && (kind & 0x3ff) != 710)
		return 0;
	VectorEnabled(emitter, count);
	emitter->bytemask = 0;
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	VectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16);
	VectorMemory(emitter, 0, 1, OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16);
	if (form == 46 || form == 47)
		VectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
	for (reg = 0; form != 6 && kind != 842 && reg < 2 + (form == 46 || form == 47); reg++) {
		VectorOperation(emitter, 0x6f, 3, reg);
		VectorFloatOperation(emitter, 0xc2, 3, reg);
		Byte(emitter, 3);
		VectorFloatOperation(emitter, 0x50, 0, 3);
		RegisterOp(emitter, 0x85, 0, 0, 0);
		Miss(emitter, 5, count);
	}
	if (form == 46 || form == 47) {
		for (reg = 0; reg < 3; reg++) {
			VectorFloatOperation(emitter, 0x12, reg + 3, reg);
			VectorFloatOperation(emitter, 0x5a, reg, reg);
			VectorFloatOperation(emitter, 0x5a, reg + 3, reg + 3);
		}
		VectorOperation(emitter, 0x59, 0, 2);
		VectorOperation(emitter, 0x59, 3, 5);
		operation = 0x58;
		if (form == 47)
			operation = 0x5c;
		VectorOperation(emitter, operation, 0, 1);
		VectorOperation(emitter, operation, 3, 4);
		if (form == 47) {
			VectorAllOnes(emitter, 2);
			VectorShift(emitter, 0x73, 6, 2, 63);
			VectorOperation(emitter, 0xef, 0, 2);
			VectorOperation(emitter, 0xef, 3, 2);
		}
		VectorOperation(emitter, 0x5a, 0, 0);
		VectorOperation(emitter, 0x5a, 3, 3);
		VectorFloatOperation(emitter, 0x16, 0, 3);
	} else if (kind == 266 || kind == 842) {
		operation = 0x3f800000;
		if (kind == 842)
			operation -= ((instruction >> 16) & 31) << 23;
		Immediate(emitter, 0, operation);
		VectorOperation(emitter, 0x6e, 0, 0);
		VectorOperation(emitter, 0x70, 0, 0);
		Byte(emitter, 0);
		if (kind == 266)
			VectorFloatOperation(emitter, 0x5e, 0, 1);
		else {
			VectorFloatOperation(emitter, 0x5b, 1, 1);
			VectorFloatOperation(emitter, 0x59, 0, 1);
		}
	} else if (kind == 10 || kind == 74 || kind == 1034 || kind == 1098) {
		operation = 0x58;
		if (kind == 74)
			operation = 0x5c;
		else if (kind == 1034)
			operation = 0x5f;
		else if (kind == 1098)
			operation = 0x5d;
		VectorFloatOperation(emitter, operation, 0, 1);
	} else {
		operation = 0;
		if ((kind & 0x3ff) == 454)
			operation = 2;
		else if ((kind & 0x3ff) == 710)
			operation = 1;
		VectorFloatOperation(emitter, 0xc2, 1, 0);
		Byte(emitter, operation);
		VectorOperation(emitter, 0x6f, 0, 1);
		if (kind & 0x400) {
			VectorFloatOperation(emitter, 0x50, 0, 0);
			ImmediateOp(emitter, 7, 0, 15);
			Setcc(emitter, 4, 1);
			Byte(emitter, 0x0f); Byte(emitter, 0xb6); Byte(emitter, 0xc9);
			Shift(emitter, 4, 1, 7);
			RegisterOp(emitter, 0x85, 0, 0, 0);
			Setcc(emitter, 4, 0);
			Shift(emitter, 4, 0, 5);
			RegisterOp(emitter, 0x09, 0, 1, 0);
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
			ImmediateOp(emitter, 4, 1, ~0xf0u);
			RegisterOp(emitter, 0x09, 0, 1, 0);
			CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		}
	}
	VectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void VectorBroadcast(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	Immediate(emitter, 0, value);
	VectorOperation(emitter, 0x6e, reg, 0);
	VectorOperation(emitter, 0x70, reg, reg);
	Byte(emitter, 0);
}

static void VectorTruncate(OsxNativeEmitter *emitter, int target, int source)
{
	Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x5b);
	Byte(emitter, 0xc0 | (target << 3) | source);
}

static void VectorCompare(OsxNativeEmitter *emitter, int target, int source, int predicate)
{
	VectorFloatOperation(emitter, 0xc2, target, source);
	Byte(emitter, predicate);
}

static void VectorSaturationMask(OsxNativeEmitter *emitter, int mask, int other, int zero)
{
	VectorOperation(emitter, 0xeb, mask, other);
	VectorOperation(emitter, 0xef, zero, zero);
	VectorOperation(emitter, 0x76, mask, zero);
	VectorSaturated(emitter, mask);
}

static int VectorConvert(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 kind;
	uint32 target;
	uint32 scale;
	uint32 mode;
	kind = instruction & 0x7ff;
	switch (kind) {
	case 78: case 778: case 906: case 970:
		break;
	case 522: case 586: case 650: case 714:
		if (!emitter->rounding)
			return 0;
		break;
	default:
		return 0;
	}
	VectorEnabled(emitter, count);
	emitter->bytemask = 0;
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	scale = ((instruction >> 16) & 31) << 23;
	VectorMemory(emitter, 0, 1, OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16);
	if (kind == 78) {
		VectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16);
		VectorShift(emitter, 0x72, 6, 0, 16);
		VectorShift(emitter, 0x72, 4, 0, 16);
		VectorShift(emitter, 0x72, 6, 1, 16);
		VectorShift(emitter, 0x72, 4, 1, 16);
		VectorOperation(emitter, 0x6b, 0, 1);
		Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x70); Byte(emitter, 0xc0); Byte(emitter, 0xb1);
		Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x70); Byte(emitter, 0xc0); Byte(emitter, 0xb1);
	} else if (kind == 778) {
		VectorOperation(emitter, 0x6f, 0, 1);
		VectorShift(emitter, 0x72, 2, 0, 16);
		VectorShift(emitter, 0x72, 6, 1, 16);
		VectorShift(emitter, 0x72, 2, 1, 16);
		VectorFloatOperation(emitter, 0x5b, 0, 0);
		VectorFloatOperation(emitter, 0x5b, 1, 1);
		VectorBroadcast(emitter, 2, 0x47800000u);
		VectorFloatOperation(emitter, 0x59, 0, 2);
		VectorFloatOperation(emitter, 0x58, 0, 1);
		if (scale) {
			VectorBroadcast(emitter, 2, 0x3f800000u - scale);
			VectorFloatOperation(emitter, 0x59, 0, 2);
		}
	} else if (kind == 906 || kind == 970) {
		if (scale) {
			VectorBroadcast(emitter, 2, 0x3f800000u + scale);
			VectorFloatOperation(emitter, 0x59, 1, 2);
		}
		if (kind == 970) {
			VectorFloatOperation(emitter, 0x28, 3, 1);
			VectorCompare(emitter, 3, 1, 7);
			VectorBroadcast(emitter, 4, 0x4f000000u);
			VectorCompare(emitter, 4, 1, 2);
			VectorBroadcast(emitter, 2, 0xcf000000u);
			VectorFloatOperation(emitter, 0x28, 5, 1);
			VectorCompare(emitter, 5, 2, 1);
			VectorTruncate(emitter, 0, 1);
			VectorOperation(emitter, 0xef, 0, 4);
			VectorOperation(emitter, 0xdb, 0, 3);
		} else {
			VectorBroadcast(emitter, 2, 0xbf800000u);
			VectorFloatOperation(emitter, 0x28, 5, 1);
			VectorCompare(emitter, 5, 2, 2);
			VectorBroadcast(emitter, 4, 0x4f800000u);
			VectorCompare(emitter, 4, 1, 2);
			VectorOperation(emitter, 0xef, 3, 3);
			VectorFloatOperation(emitter, 0x5f, 1, 3);
			VectorBroadcast(emitter, 2, 0x4f000000u);
			VectorFloatOperation(emitter, 0x28, 3, 2);
			VectorCompare(emitter, 3, 1, 2);
			VectorFloatOperation(emitter, 0x54, 2, 3);
			VectorFloatOperation(emitter, 0x5c, 1, 2);
			VectorTruncate(emitter, 0, 1);
			VectorShift(emitter, 0x72, 6, 3, 31);
			VectorOperation(emitter, 0xef, 0, 3);
			VectorOperation(emitter, 0xeb, 0, 4);
		}
		VectorSaturationMask(emitter, 4, 5, 3);
	} else {
		mode = 1;
		if (kind == 522)
			mode = 0;
		else if (kind == 586)
			mode = 3;
		else if (kind == 650)
			mode = 2;
		VectorOperation(emitter, 0x3a08, 0, 1);
		Byte(emitter, mode | 8);
	}
	VectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static int VectorExtra(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 kind;
	uint32 form;
	uint32 target;
	uint32 left;
	uint32 right;
	uint32 value;
	uint32 word;
	uint32 saturate;
	uint32 modular;
	uint32 simple;
	kind = instruction & 0x7ff;
	form = instruction & 0x3f;
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	left = OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16;
	right = OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16;
	saturate = 0;
	modular = 0;
	simple = 0;
	if (form >= 32) {
		if (form != 34 && form != 40)
			return 0;
	} else {
		switch (kind) {
		case 512: saturate = 0xdc; modular = 0xfc; break;
		case 576: saturate = 0xdd; modular = 0xfd; break;
		case 768: saturate = 0xec; modular = 0xfc; break;
		case 832: saturate = 0xed; modular = 0xfd; break;
		case 1536: saturate = 0xd8; modular = 0xf8; break;
		case 1600: saturate = 0xd9; modular = 0xf9; break;
		case 1792: saturate = 0xe8; modular = 0xf8; break;
		case 1856: saturate = 0xe9; modular = 0xf9; break;
		case 2: simple = 0xde; break;
		case 514: simple = 0xda; break;
		case 322: simple = 0xee; break;
		case 834: simple = 0xea; break;
		case 1026: simple = 0xe0; break;
		case 1090: simple = 0xe3; break;
		case 386: case 898: case 1346: case 8: case 520: case 264: case 776:
			break;
		case 526: case 654: case 590: case 718:
			if (!emitter->shuffle)
				return 0;
			break;
		case 780: case 844: case 908:
		case 260: case 324: case 388: case 516: case 580: case 644: case 772: case 836: case 900:
		case 1928:
			VectorEnabled(emitter, count);
			emitter->vectorvalue = 0;
			if (kind == 1928) {
				VectorSumAcross(emitter, target, left, right);
				return 1;
			}
			if (kind == 780 || kind == 844 || kind == 908) {
				value = (uint32)((int32)(((instruction >> 16) & 31) << 27) >> 27);
				if (kind == 780)
					value = (value & 0xff) * 0x01010101u;
				else if (kind == 844)
					value = (value & 0xffff) * 0x00010001u;
				for (word = 0; word < 4; word++) {
					Byte(emitter, 0xc7); Byte(emitter, 0x83);
					Word(emitter, target + word * 4);
					Word(emitter, value);
				}
				return 1;
			}
			VectorLanes(emitter, kind, target, left, right);
			return 1;
		default:
			return 0;
		}
	}
	VectorEnabled(emitter, count);
	if (emitter->vectorvalue == right)
		VectorOperation(emitter, 0x6f, 1, 0);
	else
		VectorMemory(emitter, 0, 1, right);
	if (emitter->vectorvalue != left)
		VectorMemory(emitter, 0, 0, left);
	if (form == 40 || form == 34) {
		if (form == 40)
			VectorOperation(emitter, 0xf5, 0, 1);
		else
			VectorOperation(emitter, 0xd5, 0, 1);
		VectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
		if (form == 40)
			VectorOperation(emitter, 0xfe, 0, 2);
		else
			VectorOperation(emitter, 0xfd, 0, 2);
	} else if (saturate) {
		VectorOperation(emitter, 0x6f, 2, 0);
		VectorOperation(emitter, modular, 2, 1);
		VectorOperation(emitter, saturate, 0, 1);
		if (modular == 0xfc || modular == 0xf8)
			VectorOperation(emitter, 0x74, 2, 0);
		else
			VectorOperation(emitter, 0x75, 2, 0);
		VectorSaturated(emitter, 2);
	} else if (simple) {
		VectorOperation(emitter, simple, 0, 1);
	} else if (kind == 386) {
		VectorOperation(emitter, 0x6f, 2, 0);
		VectorOperation(emitter, 0x66, 2, 1);
		VectorOperation(emitter, 0xdb, 0, 2);
		VectorOperation(emitter, 0xdf, 2, 1);
		VectorOperation(emitter, 0xeb, 0, 2);
	} else if (kind == 898) {
		VectorOperation(emitter, 0x6f, 2, 1);
		VectorOperation(emitter, 0x66, 2, 0);
		VectorOperation(emitter, 0xdb, 0, 2);
		VectorOperation(emitter, 0xdf, 2, 1);
		VectorOperation(emitter, 0xeb, 0, 2);
	} else if (kind == 1346) {
		VectorAllOnes(emitter, 2);
		VectorShift(emitter, 0x71, 6, 2, 15);
		VectorOperation(emitter, 0xef, 0, 2);
		VectorOperation(emitter, 0xef, 1, 2);
		VectorOperation(emitter, 0xe3, 0, 1);
		VectorOperation(emitter, 0xef, 0, 2);
	} else if (kind == 520) {
		VectorShift(emitter, 0x71, 2, 0, 8);
		VectorShift(emitter, 0x71, 2, 1, 8);
		VectorOperation(emitter, 0xd5, 0, 1);
	} else if (kind == 8) {
		VectorAllOnes(emitter, 2);
		VectorShift(emitter, 0x71, 2, 2, 8);
		VectorOperation(emitter, 0xdb, 0, 2);
		VectorOperation(emitter, 0xdb, 1, 2);
		VectorOperation(emitter, 0xd5, 0, 1);
	} else if (kind == 776 || kind == 264) {
		if (kind == 264) {
			VectorShift(emitter, 0x71, 6, 0, 8);
			VectorShift(emitter, 0x71, 6, 1, 8);
		}
		VectorShift(emitter, 0x71, 4, 0, 8);
		VectorShift(emitter, 0x71, 4, 1, 8);
		VectorOperation(emitter, 0xd5, 0, 1);
	} else {
		if (kind == 526)
			VectorConstant(emitter, 2, 5);
		else if (kind == 654)
			VectorConstant(emitter, 2, 6);
		else if (kind == 590)
			VectorConstant(emitter, 2, 7);
		else
			VectorConstant(emitter, 2, 8);
		VectorOperation(emitter, 0x6f, 0, 1);
		VectorOperation(emitter, 0x3800, 0, 2);
		if (kind == 526 || kind == 654)
			VectorShift(emitter, 0x71, 4, 0, 8);
		else
			VectorShift(emitter, 0x72, 4, 0, 16);
	}
	VectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void VectorQuad(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 vector;
	uint32 word;
	int store;
	vector = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	store = (instruction & 0x100) != 0;
	VectorEnabled(emitter, count);
	Address(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	ImmediateOp(emitter, 4, 0, 0xfffffff0);
	Translate(emitter, store, 0xff0, count);
	if (emitter->shuffle) {
		if (!emitter->bytemask) {
			emitter->bytemask = 1;
			VectorConstant(emitter, 5, 0);
		}
		if (store) {
			VectorMemory(emitter, 0, 0, vector);
			VectorOperation(emitter, 0x3800, 0, 5);
			Byte(emitter, 0xf3); Byte(emitter, 0x41); Byte(emitter, 0x0f); Byte(emitter, 0x7f);
			Byte(emitter, 0x04); Byte(emitter, 0x00);
		} else {
			Byte(emitter, 0xf3); Byte(emitter, 0x41); Byte(emitter, 0x0f); Byte(emitter, 0x6f);
			Byte(emitter, 0x04); Byte(emitter, 0x00);
			VectorOperation(emitter, 0x3800, 0, 5);
			VectorMemory(emitter, 1, 0, vector);
		}
		return;
	}
	for (word = 0; word < 4; word++) {
		if (store) {
			CpuMemory(emitter, 0x8b, 2, vector + word * 4, 0);
			Byte(emitter, 0x0f); Byte(emitter, 0xca);
			Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x54); Byte(emitter, 0x00); Byte(emitter, word * 4);
		} else {
			Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x54); Byte(emitter, 0x00); Byte(emitter, word * 4);
			Byte(emitter, 0x0f); Byte(emitter, 0xca);
			CpuMemory(emitter, 0x89, 2, vector + word * 4, 0);
		}
	}
}

static void VectorElement(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	uint32 vector;
	uint32 word;
	vector = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	VectorEnabled(emitter, count);
	Address(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	if (extended == 6 || extended == 38) {
		ImmediateOp(emitter, 4, 0, 15);
		if (extended == 38) {
			Byte(emitter, 0xf7); Byte(emitter, 0xd8);
			ImmediateOp(emitter, 0, 0, 16);
		}
		Byte(emitter, 0x69); Byte(emitter, 0xc0);
		Word(emitter, 0x01010101u);
		for (word = 0; word < 4; word++) {
			Byte(emitter, 0x8d); Byte(emitter, 0x90);
			Word(emitter, 0x00010203u + word * 0x04040404u);
			CpuMemory(emitter, 0x89, 2, vector + word * 4, 0);
		}
		return;
	}
	ImmediateOp(emitter, 4, 0, 0xfffffffcu);
	Translate(emitter, extended == 199, 0xffc, count);
	RegisterOp(emitter, 0x89, 1, 0, 0);
	ImmediateOp(emitter, 4, 1, 12);
	if (extended == 199) {
		Byte(emitter, 0x8b); Byte(emitter, 0x94); Byte(emitter, 0x0b);
		Word(emitter, vector);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x14); Byte(emitter, 0x00);
	} else {
		Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x14); Byte(emitter, 0x00);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x89); Byte(emitter, 0x94); Byte(emitter, 0x0b);
		Word(emitter, vector);
	}
}

static void Memory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
{
	int target;
	int base;
	int update;
	int store;
	int wide;
	int single;
	int reversed;
	uint32 limit;
	uint32 immediate;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	reversed = opcode >= 64;
	update = opcode & 1;
	opcode &= 62;
	store = opcode == 36 || opcode == 38 || opcode == 44 || opcode == 52 || opcode == 54;
	wide = opcode == 50 || opcode == 54;
	single = opcode == 48 || opcode == 52;
	if (wide || single)
		RequireFloat(emitter, count);
	if (single && store) {
		CpuMemory(emitter, 0x8b, 1, OSXJITFPR(target), 1);
		CallHelper(emitter, (uint64)(uintptr_t)OsxCpuDoubleToSingle);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(jitscratch), 0);
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	Address(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		RegisterOp(emitter, 0x89, 10, 0, 0);
	Translate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			CpuMemory(emitter, 0x8b, 2, OSXJITFPR(target), 1);
			Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0xca);
			Byte(emitter, 0x49); Byte(emitter, 0x89); Byte(emitter, 0x14); Byte(emitter, 0x00);
		} else if (single) {
			CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(jitscratch), 0);
		} else {
			CpuMemory(emitter, 0x8b, 2, OSXJITGPR(target), 0);
		}
		if (opcode == 36 || opcode == 52) {
			if (!reversed) {
				Byte(emitter, 0x0f); Byte(emitter, 0xca);
			}
			Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x14); Byte(emitter, 0x00);
		} else if (opcode == 44) {
			Byte(emitter, 0x66); Byte(emitter, 0xc1); Byte(emitter, 0xca); Byte(emitter, 8);
			Byte(emitter, 0x66); Byte(emitter, 0x41); Byte(emitter, 0x89); Byte(emitter, 0x14); Byte(emitter, 0x00);
		} else if (!wide) {
			Byte(emitter, 0x41); Byte(emitter, 0x88); Byte(emitter, 0x14); Byte(emitter, 0x00);
		}
	} else {
		if (wide) {
			Byte(emitter, 0x49); Byte(emitter, 0x8b); Byte(emitter, 0x04); Byte(emitter, 0x00);
			Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0xc8);
		} else if (opcode == 32 || single) {
			Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x04); Byte(emitter, 0x00);
			if (!reversed) {
				Byte(emitter, 0x0f); Byte(emitter, 0xc8);
			}
		} else {
			Byte(emitter, 0x41); Byte(emitter, 0x0f);
			if (opcode == 34)
				Byte(emitter, 0xb6);
			else
				Byte(emitter, 0xb7);
			Byte(emitter, 0x04); Byte(emitter, 0x00);
			if (opcode != 34) {
				Byte(emitter, 0x66); Byte(emitter, 0xc1); Byte(emitter, 0xc8); Byte(emitter, 8);
				if (opcode == 42) {
					Byte(emitter, 0x0f); Byte(emitter, 0xbf); Byte(emitter, 0xc0);
				}
			}
		}
		if (single) {
			if (update)
				CpuMemory(emitter, 0x89, 10, OSXJITGPR(base), 0);
			update = 0;
			RegisterOp(emitter, 0x89, 1, 0, 0);
			CallHelper(emitter, (uint64)(uintptr_t)OsxCpuSingleToDouble);
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
		} else if (wide) {
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
		} else {
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		}
	}
	if (update)
		CpuMemory(emitter, 0x89, 10, OSXJITGPR(base), 0);
}

static int Branch(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 options;
	uint32 displacement;
	uint32 extended;
	uint8 *counterpatch;
	uint8 *conditionpatch;
	uint32 offset;
	uint32 targetoffset;
	opcode = instruction >> 26;
	options = (instruction >> 21) & 31;
	counterpatch = NULL;
	conditionpatch = NULL;
	offset = (emitter->physical & 4095) + (uint32)count * 4;
	targetoffset = 4096;
	if (opcode == 19) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 528) {
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(ctr), 0);
			options |= 4;
		} else {
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(lr), 0);
		}
		ImmediateOp(emitter, 4, 1, ~3u);
	} else {
		if (opcode == 18) {
			displacement = instruction & 0x03fffffc;
			if (displacement & 0x02000000)
				displacement |= 0xfc000000;
			options = 20;
		} else {
			displacement = (uint32)(int32)(int16)(instruction & 0xfffc);
		}
		Immediate(emitter, 1, displacement);
		if (!(instruction & 2)) {
			RegisterOp(emitter, 0x01, 1, 11, 0);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		RegisterOp(emitter, 0x89, 2, 11, 0);
		ImmediateOp(emitter, 0, 2, 4);
		CpuMemory(emitter, 0x89, 2, OSXJITOFFSET(lr), 0);
	}
	if (!(options & 4)) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(ctr), 0);
		ImmediateOp(emitter, 5, 0, 1);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(ctr), 0);
		if (options & 2)
			counterpatch = Jump(emitter, 5);
		else
			counterpatch = Jump(emitter, 4);
	}
	if (!(options & 16)) {
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(cr), 0);
		Byte(emitter, 0xf7); Byte(emitter, 0xc2);
		Word(emitter, 1u << (31 - ((instruction >> 16) & 31)));
		if (options & 8)
			conditionpatch = Jump(emitter, 4);
		else
			conditionpatch = Jump(emitter, 5);
	}
	RegisterOp(emitter, 0x89, 11, 1, 0);
	Exit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			Patch(counterpatch, emitter->cursor);
		if (conditionpatch)
			Patch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void LoadCarry(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0x0f); Byte(emitter, 0xba); Byte(emitter, 0xa3);
	Word(emitter, OSXJITOFFSET(xer));
	Byte(emitter, 29);
}

static void StoreCarry(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0x0f); Byte(emitter, 0xb6); Byte(emitter, 0xd2);
	Shift(emitter, 4, 2, 29);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(xer), 0);
	ImmediateOp(emitter, 4, 1, 0xdfffffff);
	RegisterOp(emitter, 0x09, 1, 2, 0);
	CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(xer), 0);
}

static void RequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRPR);
	Miss(emitter, 5, count);
}

static int Carrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		CpuMemory(emitter, 0x03, 0, OSXJITGPR(right), 0);
		Setcc(emitter, 2, 2);
		break;
	case 8:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(right), 0);
		CpuMemory(emitter, 0x2b, 0, OSXJITGPR(left), 0);
		Setcc(emitter, 3, 2);
		break;
	case 138:
	case 136:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 136) {
			Byte(emitter, 0xf7); Byte(emitter, 0xd0);
		}
		LoadCarry(emitter);
		CpuMemory(emitter, 0x13, 0, OSXJITGPR(right), 0);
		Setcc(emitter, 2, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 200 || extended == 232) {
			Byte(emitter, 0xf7); Byte(emitter, 0xd0);
		}
		LoadCarry(emitter);
		if (extended == 202 || extended == 200)
			ImmediateOp(emitter, 2, 0, 0);
		else
			ImmediateOp(emitter, 2, 0, 0xffffffff);
		Setcc(emitter, 2, 2);
		break;
	default:
		return 0;
	}
	CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
	StoreCarry(emitter);
	Record(emitter, instruction);
	return 1;
}

static int Arithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	int left;
	int right;
	uint8 *ordinary;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 104:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		Byte(emitter, 0xf7); Byte(emitter, 0xd8);
		break;
	case 11:
	case 75:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		Byte(emitter, 0xf7);
		if (extended == 11)
			Byte(emitter, 0xa3);
		else
			Byte(emitter, 0xab);
		Word(emitter, OSXJITGPR(right));
		RegisterOp(emitter, 0x89, 0, 2, 0);
		break;
	case 459:
	case 491:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(right), 0);
		RegisterOp(emitter, 0x85, 1, 1, 0);
		Miss(emitter, 4, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 491) {
			ImmediateOp(emitter, 7, 1, 0xffffffff);
			ordinary = Jump(emitter, 5);
			ImmediateOp(emitter, 7, 0, 0x80000000);
			Miss(emitter, 4, count);
			Patch(ordinary, emitter->cursor);
			Byte(emitter, 0x99);
			Byte(emitter, 0xf7); Byte(emitter, 0xf9);
		} else {
			RegisterOp(emitter, 0x31, 2, 2, 0);
			Byte(emitter, 0xf7); Byte(emitter, 0xf1);
		}
		break;
	default:
		return 0;
	}
	CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
	Record(emitter, instruction);
	return 1;
}

static int Logical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int source;
	int target;
	int amount;
	source = (instruction >> 21) & 31;
	target = (instruction >> 16) & 31;
	amount = (instruction >> 11) & 31;
	switch (extended) {
	case 24:
	case 536:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount), 0);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		ImmediateOp(emitter, 4, 1, 63);
		Byte(emitter, 0x48); Byte(emitter, 0xd3);
		if (extended == 24)
			Byte(emitter, 0xe0);
		else
			Byte(emitter, 0xe8);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		break;
	case 26:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		Immediate(emitter, 2, 63);
		Byte(emitter, 0x0f); Byte(emitter, 0xbd); Byte(emitter, 0xc0);
		Byte(emitter, 0x0f); Byte(emitter, 0x44); Byte(emitter, 0xc2);
		ImmediateOp(emitter, 6, 0, 31);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		break;
	case 792:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount), 0);
		ImmediateOp(emitter, 4, 1, 63);
		Byte(emitter, 0x48); Byte(emitter, 0x63); Byte(emitter, 0x83);
		Word(emitter, OSXJITGPR(source));
		RegisterOp(emitter, 0x89, 2, 0, 1);
		Byte(emitter, 0x48); Byte(emitter, 0xd3); Byte(emitter, 0xf8);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		RegisterOp(emitter, 0x89, 9, 0, 1);
		Byte(emitter, 0x49); Byte(emitter, 0xd3); Byte(emitter, 0xe1);
		RegisterOp(emitter, 0x39, 9, 2, 1);
		Setcc(emitter, 5, 1);
		RegisterOp(emitter, 0x85, 2, 2, 1);
		Setcc(emitter, 8, 2);
		Byte(emitter, 0x20); Byte(emitter, 0xca);
		StoreCarry(emitter);
		break;
	case 824:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		if (amount == 0) {
			RegisterOp(emitter, 0x31, 2, 2, 0);
		} else {
			RegisterOp(emitter, 0x89, 2, 0, 0);
			RegisterOp(emitter, 0x89, 1, 0, 0);
			ImmediateOp(emitter, 4, 1, (1u << amount) - 1);
			Setcc(emitter, 5, 1);
			Shift(emitter, 5, 2, 31);
			Byte(emitter, 0x20); Byte(emitter, 0xca);
			Shift(emitter, 7, 0, (uint32)amount);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		StoreCarry(emitter);
		break;
	default:
		return 0;
	}
	Record(emitter, instruction);
	return 1;
}

static int SpecialOffset(uint32 number, uint32 *offset)
{
	switch (number) {
	case 18: *offset = OSXJITOFFSET(dsisr); return 1;
	case 19: *offset = OSXJITOFFSET(dar); return 1;
	case 26: *offset = OSXJITOFFSET(srr0); return 1;
	case 27: *offset = OSXJITOFFSET(srr1); return 1;
	case 272: case 273: case 274: case 275:
		*offset = OSXJITOFFSET(sprg) + (number - 272) * 4;
		return 1;
	}
	return 0;
}

static void Callout(OsxNativeEmitter *emitter, uint32 instruction, int count);

static int System(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	uint32 number;
	uint32 offset;
	uint32 mask;
	uint32 field;
	uint8 *quiet;
	uint8 *pending;
	uint8 *slowmask;
	uint8 *slowpower;
	uint8 *done;
	target = (instruction >> 21) & 31;
	switch (extended) {
	case 54: case 86: case 246: case 278: case 598: case 854:
		return 1;
	case 144:
		mask = 0;
		for (field = 0; field < 8; field++)
			if (instruction & (0x80000u >> field))
				mask |= 0xf0000000u >> (field * 4);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (mask != 0xffffffffu) {
			ImmediateOp(emitter, 4, 0, mask);
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
			ImmediateOp(emitter, 4, 1, ~mask);
			RegisterOp(emitter, 0x09, 0, 1, 0);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		return 1;
	case 83:
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(msr), 0);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 146:
		emitter->enabled = 0;
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(target), 0);
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(msr), 0);
		RegisterOp(emitter, 0x31, 2, 1, 0);
		Byte(emitter, 0xf7); Byte(emitter, 0xc2);
		Word(emitter, ~(uint32)OSXMSREE);
		slowmask = Jump(emitter, 5);
		Byte(emitter, 0xf7); Byte(emitter, 0xc1);
		Word(emitter, OSXMSRPOW);
		slowpower = Jump(emitter, 5);
		CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(msr), 0);
		RegisterOp(emitter, 0x21, 2, 1, 0);
		quiet = Jump(emitter, 4);
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(externalline), 0);
		CpuMemory(emitter, 0x0b, 2, OSXJITOFFSET(decpending), 0);
		pending = Jump(emitter, 4);
		Advance(emitter);
		Account(emitter, count + 1);
		Settle(emitter);
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(slice));
		Word(emitter, 0);
		Release(emitter, OSXJITCONTINUE);
		Patch(quiet, emitter->cursor);
		Patch(pending, emitter->cursor);
		Byte(emitter, 0xe9);
		done = emitter->cursor;
		Word(emitter, 0);
		Patch(slowmask, emitter->cursor);
		Patch(slowpower, emitter->cursor);
		Callout(emitter, instruction, count);
		Patch(done, emitter->cursor);
		return 1;
	case 595:
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		emitter->bytemask = 0;
		Byte(emitter, 0xe8);
		JumpTo(emitter, emitter->timethunk);
		if (number == 269) {
			Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xe8); Byte(emitter, 32);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 210:
	case 242:
#if OSXJITSLOWPATH
		emitter->slowactive = 1;
#endif
		RequireSupervisorMode(emitter, count);
		if (extended == 210)
			Immediate(emitter, 0, (instruction >> 16) & 15);
		else {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR((instruction >> 11) & 31), 0);
			Shift(emitter, 5, 0, 28);
		}
		RegisterOp(emitter, 0x89, 1, 11, 0);
		Shift(emitter, 5, 1, 28);
		RegisterOp(emitter, 0x39, 1, 0, 0);
		Miss(emitter, 4, count);
#if OSXJITSLOWPATH
		emitter->slowactive = 0;
#endif
		CpuMemory(emitter, 0x8b, 2, OSXJITGPR(target), 0);
		Byte(emitter, 0x89); Byte(emitter, 0x94); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(sr));
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(fetchtag));
		Word(emitter, 0);
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!SpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			Callout(emitter, instruction, count);
			return 1;
		}
		RequireSupervisorMode(emitter, count);
		if (extended == 339) {
			CpuMemory(emitter, 0x8b, 0, offset, 0);
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		} else {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			CpuMemory(emitter, 0x89, 0, offset, 0);
		}
		return 1;
	}
	return 0;
}

static void ConditionZero(OsxNativeEmitter *emitter)
{
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(xer), 0);
	Shift(emitter, 5, 0, 31);
}

static void StoreConditionZero(OsxNativeEmitter *emitter)
{
	Shift(emitter, 4, 0, 28);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	ImmediateOp(emitter, 4, 1, 0x0fffffff);
	RegisterOp(emitter, 0x09, 0, 1, 0);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
}

static int Reserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	Address(emitter, base, index, 0, 0);
	Byte(emitter, 0xa8); Byte(emitter, 3);
	Miss(emitter, 5, count);
	if (extended == 20) {
		RegisterOp(emitter, 0x89, 10, 0, 0);
		Memory(emitter, instruction, 32, index, count);
		CpuMemory(emitter, 0x89, 10, OSXJITOFFSET(reservation), 0);
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(reservationvalid));
		Word(emitter, 1);
		return 1;
	}
	Byte(emitter, 0x81); Byte(emitter, 0xbb);
	Word(emitter, OSXJITOFFSET(reservationvalid));
	Word(emitter, 0);
	invalid = Jump(emitter, 4);
	CpuMemory(emitter, 0x39, 0, OSXJITOFFSET(reservation), 0);
	elsewhere = Jump(emitter, 5);
	Memory(emitter, instruction, 36, index, count);
	ConditionZero(emitter);
	ImmediateOp(emitter, 1, 0, 2);
	Byte(emitter, 0xe9);
	done = emitter->cursor;
	Word(emitter, 0);
	Patch(invalid, emitter->cursor);
	Patch(elsewhere, emitter->cursor);
	ConditionZero(emitter);
	Patch(done, emitter->cursor);
	Byte(emitter, 0xc7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(reservationvalid));
	Word(emitter, 0);
	StoreConditionZero(emitter);
	return 1;
}

static int ZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	int offset;
	Address(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	ImmediateOp(emitter, 4, 0, ~31u);
	Translate(emitter, 1, 0xfe0, count);
	RegisterOp(emitter, 0x31, 2, 2, 0);
	for (offset = 0; offset < 32; offset += 8) {
		Byte(emitter, 0x49); Byte(emitter, 0x89); Byte(emitter, 0x54); Byte(emitter, 0x00);
		Byte(emitter, (uint32)offset);
	}
	return 1;
}

static int ConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
{
	uint32 extended;
	uint32 bitd;
	uint32 bita;
	uint32 bitb;
	extended = (instruction >> 1) & 0x3ff;
	bitd = (instruction >> 21) & 31;
	bita = (instruction >> 16) & 31;
	bitb = (instruction >> 11) & 31;
	if (extended == 0) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
		RegisterOp(emitter, 0x89, 1, 0, 0);
		Shift(emitter, 5, 1, (7 - (bita >> 2)) * 4);
		ImmediateOp(emitter, 4, 1, 0xf);
		Shift(emitter, 4, 1, (7 - (bitd >> 2)) * 4);
		ImmediateOp(emitter, 4, 0, ~(0xfu << ((7 - (bitd >> 2)) * 4)));
		RegisterOp(emitter, 0x09, 0, 1, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
	RegisterOp(emitter, 0x89, 1, 0, 0);
	Shift(emitter, 5, 1, 31 - bita);
	ImmediateOp(emitter, 4, 1, 1);
	RegisterOp(emitter, 0x89, 2, 0, 0);
	Shift(emitter, 5, 2, 31 - bitb);
	ImmediateOp(emitter, 4, 2, 1);
	if (extended == 129 || extended == 417)
		ImmediateOp(emitter, 6, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		RegisterOp(emitter, 0x09, 1, 2, 0);
	else if (extended == 129 || extended == 225 || extended == 257)
		RegisterOp(emitter, 0x21, 1, 2, 0);
	else
		RegisterOp(emitter, 0x31, 1, 2, 0);
	if (extended == 33 || extended == 225 || extended == 289)
		ImmediateOp(emitter, 6, 1, 1);
	Shift(emitter, 4, 1, 31 - bitd);
	ImmediateOp(emitter, 4, 0, ~(1u << (31 - bitd)));
	RegisterOp(emitter, 0x09, 0, 1, 0);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
	return 1;
}

static int Interpretable(uint32 instruction)
{
	uint32 opcode;
	uint32 extended;
	opcode = instruction >> 26;
	if (opcode == 4)
		return 1;
	if (opcode != 31)
		return 0;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 7: case 39: case 135: case 167:
	case 342: case 374: case 758: case 790: case 822: case 918: case 982:
		return 1;
	}
	return 0;
}

static void Callout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	emitter->bytemask = 0;
	CpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	RegisterOp(emitter, 0x89, 0, 13, 0);
	RegisterOp(emitter, 0x29, 0, 12, 0);
	if (count)
		ImmediateOp(emitter, 0, 0, (uint32)count);
	CpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions), 1);
	RegisterOp(emitter, 0x89, 1, 3, 1);
	Immediate(emitter, 2, instruction);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xec); Byte(emitter, 0x20);
	Byte(emitter, 0x48); Byte(emitter, 0xb8);
	Quad(emitter, (uint64)(uintptr_t)OsxCpuExecuteInline);
	Byte(emitter, 0xff); Byte(emitter, 0xd0);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xc4); Byte(emitter, 0x20);
	RegisterOp(emitter, 0x89, 2, 13, 0);
	RegisterOp(emitter, 0x29, 2, 12, 0);
	if (count)
		ImmediateOp(emitter, 0, 2, (uint32)count);
	CpuMemory(emitter, 0x29, 2, OSXJITOFFSET(instructions), 1);
	RegisterOp(emitter, 0x85, 0, 0, 0);
	resume = Jump(emitter, 4);
	Charge(emitter, count + 1);
	CpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
	Byte(emitter, 0x83); Byte(emitter, 0xbb);
	Word(emitter, OSXJITOFFSET(slice));
	Byte(emitter, 0);
	running = Jump(emitter, 15);
	Settle(emitter);
	Release(emitter, OSXJITCONTINUE);
	Patch(running, emitter->cursor);
	Leave(emitter, OSXJITCONTINUE);
	Patch(resume, emitter->cursor);
	CpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
}

static int Emit(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 extended;
	uint32 immediate;
	uint32 mask;
	uint32 number;
	uint32 offset;
	int target;
	int base;
	int index;
	int status;
	OsxVectorFunction handler;
	opcode = instruction >> 26;
	if (opcode != 4)
		emitter->vectorvalue = 0;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	immediate = instruction & 0xffff;
	if (opcode == 16 || opcode == 18 || (opcode == 19 && (((instruction >> 1) & 0x3ff) == 16 || ((instruction >> 1) & 0x3ff) == 528))) {
		return Branch(emitter, instruction, count);
	}
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return ConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!Multiple(emitter, instruction, opcode, count))
			Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 31 && ((instruction & 0x6fe) == 0xce || (instruction & 0x6fe) == 0x2ce)) {
		VectorQuad(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxCpuVectorHandler(instruction);
		if (handler) {
			if (VectorFloat(emitter, instruction, count) || VectorConvert(emitter, instruction, count) || VectorExtra(emitter, instruction, count) || VectorNative(emitter, instruction, count))
				return 1;
			emitter->vectorvalue = 0;
			VectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (opcode == 31) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 6 || extended == 38 || extended == 71 || extended == 199) {
			VectorElement(emitter, instruction, extended, count);
			return 1;
		}
	}
	if (Interpretable(instruction)) {
		Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55 && opcode != 46 && opcode != 47) {
		Memory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
#if OSXJITSLOWPATH
		emitter->slowactive = 1;
		status = Float(emitter, instruction, count);
		emitter->slowactive = 0;
#else
		status = Float(emitter, instruction, count);
#endif
		if (!status)
			Callout(emitter, instruction, count);
		return 1;
	case 8:
		Immediate(emitter, 0, (uint32)(int32)(int16)immediate);
		CpuMemory(emitter, 0x2b, 0, OSXJITGPR(base), 0);
		Setcc(emitter, 3, 2);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		StoreCarry(emitter);
		return 1;
	case 12: case 13:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		ImmediateOp(emitter, 0, 0, (uint32)(int32)(int16)immediate);
		Setcc(emitter, 2, 2);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		StoreCarry(emitter);
		if (opcode == 13) {
			RegisterOp(emitter, 0x85, 0, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 7:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		Byte(emitter, 0x69); Byte(emitter, 0xc0);
		Word(emitter, (uint32)(int32)(int16)immediate);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 10: case 11:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		ImmediateOp(emitter, 7, 0, immediate);
		Condition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		Address(emitter, base, -1, immediate, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 20: case 21: case 23:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (opcode == 23) {
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(index), 0);
			Byte(emitter, 0xd3); Byte(emitter, 0xc0);
		} else {
			Shift(emitter, 0, 0, (uint32)index);
		}
		mask = RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		ImmediateOp(emitter, 4, 0, mask);
		if (opcode == 20) {
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(base), 0);
			ImmediateOp(emitter, 4, 1, ~mask);
			RegisterOp(emitter, 0x09, 0, 1, 0);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
		Record(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26)
			ImmediateOp(emitter, 1, 0, immediate);
		else if (opcode < 28)
			ImmediateOp(emitter, 6, 0, immediate);
		else
			ImmediateOp(emitter, 4, 0, immediate);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
		if (opcode >= 28) {
			RegisterOp(emitter, 0x85, 0, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			Memory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			Memory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			Memory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
			CpuMemory(emitter, 0x3b, 0, OSXJITGPR(index), 0);
			Condition(emitter, target >> 2, extended == 0);
			return 1;
		}
		if (extended == 339 || extended == 467) {
			number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
			if (number == 1)
				offset = OSXJITOFFSET(xer);
			else if (number == 8)
				offset = OSXJITOFFSET(lr);
			else if (number == 9)
				offset = OSXJITOFFSET(ctr);
			else if (number == 256)
				offset = OSXJITOFFSET(vrsave);
			else
				return System(emitter, instruction, extended, count);
			if (extended == 339) {
				CpuMemory(emitter, 0x8b, 0, offset, 0);
				CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			} else {
				CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
				if (number == 1)
					ImmediateOp(emitter, 4, 0, 0xe000007f);
				CpuMemory(emitter, 0x89, 0, offset, 0);
			}
			return 1;
		}
		if (extended == 19) {
			CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
			if (extended == 266)
				CpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
			else if (extended == 40) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd8);
				CpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
			} else {
				Byte(emitter, 0x0f); Byte(emitter, 0xaf); Byte(emitter, 0x83);
				Word(emitter, OSXJITGPR(index));
			}
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(index), 0);
			if (extended == 60 || extended == 412) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd1);
			}
			if (extended == 28 || extended == 60 || extended == 476)
				RegisterOp(emitter, 0x21, 0, 1, 0);
			else if (extended == 284 || extended == 316)
				RegisterOp(emitter, 0x31, 0, 1, 0);
			else
				RegisterOp(emitter, 0x09, 0, 1, 0);
			if (extended == 124 || extended == 284 || extended == 476) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd0);
			}
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return Reserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return ZeroBlock(emitter, instruction, count);
		if (Carrying(emitter, instruction, extended) || Arithmetic(emitter, instruction, extended, count) || Logical(emitter, instruction, extended) || System(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			Byte(emitter, 0x0f);
			if (extended == 922)
				Byte(emitter, 0xbf);
			else
				Byte(emitter, 0xbe);
			Byte(emitter, 0xc0);
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
			Record(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

static uint32 VectorByte(uint32 element)
{
	return (element & ~3u) + 3 - (element & 3);
}

static void UnpackMask(uint8 *mask, uint32 first, int wide)
{
	uint32 element;
	memset(mask, 0x80, 16);
	for (element = 0; element < 8; element++) {
		if (!wide)
			mask[VectorByte(element * 2)] = (uint8)VectorByte(first + element);
		else if (element < 4) {
			mask[VectorByte(element * 4)] = (uint8)VectorByte((first + element) * 2);
			mask[VectorByte(element * 4 + 1)] = (uint8)VectorByte((first + element) * 2 + 1);
		}
	}
}

int OsxJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	int index;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->shuffle = OsxHostFeature(OSXFEATURESSSE3);
	state->fused = !OSXJITNOFUSED && OsxHostFeature(OSXFEATUREAVX2);
	state->rounding = OsxHostFeature(OSXFEATURESSE41);
	for (index = 0; index < 4; index++) {
		state->vectorconstants[0][index] = 0x00010203u + (uint32)index * 0x04040404u;
		state->vectorconstants[1][index] = 0x1f1f1f1fu;
		state->vectorconstants[2][index] = 0x03030303u;
		state->vectorconstants[3][index] = 0x0f0f0f0fu;
		state->vectorconstants[4][index] = 0x00004000u;
	}
	UnpackMask((uint8 *)state->vectorconstants[5], 0, 0);
	UnpackMask((uint8 *)state->vectorconstants[6], 8, 0);
	UnpackMask((uint8 *)state->vectorconstants[7], 0, 1);
	UnpackMask((uint8 *)state->vectorconstants[8], 4, 1);
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
	state->writebase = OsxJitAllocate(OSXJITBYTES);
	state->executebase = state->writebase;
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->writebase == NULL || state->executebase == NULL) {
		OsxJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	EmitThunks(state);
#if OSXPROFILEJIT
	state->tmpmap = fopen("osxjitmap.bin", "wb");
#endif
	return 1;
}

void OsxJitDestroy(OsxCpu *cpu)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL)
		return;
	if (cpu->logfile)
		fprintf(cpu->logfile, "jit compiled %llu linked %llu instructions %llu interpreted %llu bytes %u\n", (unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)cpu->instructions, (unsigned long long)state->interpreted, state->used);
#if OSXPROFILEJIT
	if (state->tmpmap)
		fclose(state->tmpmap);
#endif
	if (state->writebase)
		OsxJitRelease(state->writebase, OSXJITBYTES);
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
#if OSXG5
	if (cpu->core970) {
		Osx970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
		return 1;
	}
#endif
	OsxNativeState *state;
	uint32 first;
	uint32 last;
	int invalidated;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || length == 0 || address >= cpu->ramsize)
		return 0;
	if (length > cpu->ramsize - address)
		length = cpu->ramsize - address;
	first = address >> 12;
	last = (address + length - 1) >> 12;
	invalidated = 0;
	for (; first <= last; first++) {
		if (!state->compiledpages[first])
			continue;
		state->compiledpages[first] = 0;
		state->versions[first]++;
		invalidated = 1;
#if OSXPROFILEJIT
		state->tmpstats[14]++;
#endif
	}
	return invalidated;
}

static int NeedsPc(uint32 instruction)
{
	uint32 opcode;
	opcode = instruction >> 26;
	if (opcode == 7 || opcode == 10 || opcode == 11 || opcode == 14 || opcode == 15 || opcode == 20 || opcode == 21 || opcode == 23 || (opcode >= 24 && opcode <= 29))
		return 0;
	if (opcode == 31) {
		switch ((instruction >> 1) & 0x3ff) {
		case 0: case 19: case 28: case 32: case 40: case 60: case 124: case 144:
		case 235: case 266: case 284: case 316: case 412: case 444: case 476: case 922: case 954:
			return 0;
		}
	}
	return 1;
}

static void Compile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
{
	OsxNativeEmitter emitter;
	uint32 instruction;
	uint32 word;
	int count;
	int limit;
	int status;
	int index;
	uint8 *sliceshort;
	uint8 *stubs[OSXJITMISSES];
#if OSXJITSLOWPATH
	uint8 *slowstub;
	uint8 *resume;
	int slowjumps;
	int jump;
#endif
	int shared;
	uint32 pendingpc;
#if OSXPROFILEJIT
	uint32 tmpoffsets[OSXJITLIMIT + 1];
	uint32 tmpwords[OSXJITLIMIT + 1];
	uint32 tmpheader[7];
	uint32 tmpbody;
	uint32 tmpexit;
#endif
	if (state->used + OSXJITRESERVE >= OSXJITBYTES) {
#if OSXPROFILEJIT
		state->tmpstats[0]++;
#endif
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
		memset(state->blocks, 0, sizeof(state->blocks));
		memset(state->front, 0, sizeof(state->front));
		state->used = state->thunkbytes;
		cpu->jitlink = NULL;
	}
	emitter.start = state->writebase + state->used;
	emitter.cursor = emitter.start;
	emitter.executestart = state->executebase + state->used;
	emitter.physical = physical;
	emitter.finishthunk = state->finishthunk;
	emitter.interpretthunk = state->interpretthunk;
	emitter.timethunk = state->timethunk;
	emitter.linkcallthunk = state->linkcallthunk;
	emitter.dynamicthunk = state->dynamicthunk;
	emitter.translatethunks = state->translatethunks;
	emitter.deterministic = cpu->deterministic != 0;
	emitter.enabled = 0;
	emitter.vectorvalue = 0;
	emitter.shuffle = state->shuffle;
	emitter.rounding = state->rounding;
	emitter.fused = state->fused;
	emitter.bytemask = 0;
	emitter.vectorconstants = state->vectorconstants;
	emitter.misscount = 0;
#if OSXJITSLOWPATH
	emitter.slowactive = 0;
	emitter.slowjumpcount = 0;
	emitter.slowcount = 0;
#endif
	Byte(&emitter, 0x53);
	Byte(&emitter, 0x41); Byte(&emitter, 0x54);
	Byte(&emitter, 0x41); Byte(&emitter, 0x55);
	RegisterOp(&emitter, 0x89, 3, 1, 1);
	CpuMemory(&emitter, 0x8b, 12, OSXJITOFFSET(slice), 0);
	RegisterOp(&emitter, 0x89, 13, 12, 0);
	CpuMemory(&emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
	ImmediateOp(&emitter, 7, 12, 0x7fffffff);
	sliceshort = Jump(&emitter, 12);
	limit = (int)((4096 - (physical & 4095)) >> 2);
	if (limit > OSXJITLIMIT)
		limit = OSXJITLIMIT;
	count = 0;
	status = 0;
	pendingpc = 0;
#if OSXJITSLOWPATH
	while (count < limit && emitter.misscount < OSXJITMISSES - 128 && emitter.slowjumpcount < OSXJITLIMIT * 8 - 16) {
#else
	while (count < limit && emitter.misscount < OSXJITMISSES - 128) {
#endif
		memcpy(&word, cpu->ram + physical + count * 4, 4);
		instruction = tswap32(word);
		if (pendingpc && NeedsPc(instruction)) {
			ImmediateOp(&emitter, 0, 11, pendingpc);
			pendingpc = 0;
		}
#if OSXPROFILEJIT
		tmpoffsets[count] = (uint32)(emitter.cursor - emitter.start);
		tmpwords[count] = instruction;
#endif
#if OSXJITSLOWPATH
		slowjumps = emitter.slowjumpcount;
		status = Emit(&emitter, instruction, count);
		if (emitter.slowjumpcount != slowjumps) {
			emitter.slowinstructions[emitter.slowcount] = instruction;
			emitter.slowcounts[emitter.slowcount] = count;
			emitter.slowresumes[emitter.slowcount] = emitter.cursor;
			emitter.slowcount++;
			emitter.bytemask = 0;
		}
#else
		status = Emit(&emitter, instruction, count);
#endif
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		ImmediateOp(&emitter, 0, 11, pendingpc);
#if OSXPROFILEJIT
	tmpbody = (uint32)(emitter.cursor - emitter.start);
#endif
	if (status == 0)
		Return(&emitter, count);
	else if (status == 1)
		Exit(&emitter, count, (physical & 4095) + (uint32)count * 4);
#if OSXPROFILEJIT
	tmpexit = (uint32)(emitter.cursor - emitter.start);
#endif
#if OSXJITSLOWPATH
	for (index = 0; index < emitter.slowcount; index++) {
		slowstub = emitter.cursor;
		for (jump = 0; jump < emitter.slowjumpcount; jump++)
			if (emitter.slowowners[jump] == index)
				Patch(emitter.slowjumps[jump], slowstub);
		Callout(&emitter, emitter.slowinstructions[index], emitter.slowcounts[index]);
		Byte(&emitter, 0xe9);
		resume = emitter.cursor;
		Word(&emitter, 0);
		Patch(resume, emitter.slowresumes[index]);
	}
#endif
	for (index = 0; index < emitter.misscount; index++) {
		shared = 0;
		while (shared < index && emitter.misscounts[shared] != emitter.misscounts[index])
			shared++;
		if (shared < index) {
			stubs[index] = stubs[shared];
		} else {
			stubs[index] = emitter.cursor;
			Return(&emitter, emitter.misscounts[index]);
		}
		Patch(emitter.misses[index], stubs[index]);
	}
	Patch(sliceshort, emitter.cursor);
	Return(&emitter, 0);
	memcpy(emitter.start + OSXJITENTRY + 3, &count, 4);
	state->compiledpages[physical >> 12] = 1;
	block->physical = physical;
	block->mode = mode;
	block->version = state->versions[physical >> 12] + 1;
	block->count = count;
	if (count == 0) {
		block->function = NULL;
		state->compiled++;
		return;
	}
	block->function = (OsxNativeFunction)(state->executebase + state->used);
#if OSXPROFILEJIT
	state->tmpstats[6] += (uint64)(emitter.cursor - emitter.start);
	state->tmpstats[7] += (uint64)count;
	if (state->tmpmap) {
		tmpheader[0] = (uint32)(uintptr_t)(state->executebase + state->used);
		tmpheader[1] = (uint32)((uint64)(uintptr_t)(state->executebase + state->used) >> 32);
		tmpheader[2] = (uint32)(emitter.cursor - emitter.start);
		tmpheader[3] = physical;
		tmpheader[4] = (uint32)count;
		tmpheader[5] = tmpbody;
		tmpheader[6] = tmpexit;
		fwrite(tmpheader, sizeof(tmpheader), 1, state->tmpmap);
		fwrite(tmpoffsets, 4, (size_t)count, state->tmpmap);
		fwrite(tmpwords, 4, (size_t)count, state->tmpmap);
	}
#endif
	state->used += ((uint32)(emitter.cursor - emitter.start) + 15) & ~15u;
	state->compiled++;
}

static void LinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + OSXJITENTRY;
	state->linked++;
}

static void Link(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	int32 distance;
	distance = (int32)((uint8 *)block->function + OSXJITENTRY - (jump + 4));
	memcpy(state->writebase + (jump - state->executebase), &distance, 4);
	state->linked++;
}

static void InterpretRun(OsxCpu *cpu, OsxNativeState *state)
{
	uint32 word;
	uint32 instruction;
	uint32 expected;
	int executed;
	executed = 0;
	do {
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		expected = cpu->pc + 4;
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxJitRun(OsxCpu *cpu, int32 count)
{
	OsxNativeState *state;
	OsxNativeBlock *block;
	uint32 physical;
	uint32 mode;
	uint32 instruction;
	uint32 word;
	uint32 set;
	uint32 hash;
	uint32 version;
	uint32 linkkind;
	uint32 linkmode;
	int way;
	int code;
	int hot;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || cpu->tracefile) {
		OsxCpuRun(cpu, count);
		return;
	}
	hot = OSXJITHOT;
	if (cpu->jithot)
		hot = cpu->jithot;
	if (cpu->halted)
		return;
	cpu->slice = count;
	cpu->jitlink = NULL;
	linkkind = 0;
	linkmode = 0;
	while (cpu->slice > 0) {
		if (cpu->tracefile) {
			OsxCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxCpuFetchPage(cpu)) {
#if OSXPROFILEJIT
			state->tmpstats[13]++;
#endif
			cpu->jitlink = NULL;
			cpu->pc = cpu->nextpc;
			cpu->slice--;
			continue;
		}
		if (!(cpu->msr & OSXMSRSE) && cpu->fetchhost >= cpu->ram && cpu->fetchhost < cpu->ram + cpu->ramsize && !(cpu->pc & 3)) {
			physical = (uint32)(cpu->fetchhost - cpu->ram) + (cpu->pc & 4095);
			mode = cpu->msr & (OSXMSRPR | OSXMSRIR);
			hash = ((physical >> 2) * 0x9e3779b1u) ^ (mode * 0x85ebca6bu);
			version = state->versions[physical >> 12] + 1;
			block = &state->front[hash >> 18];
#if OSXPROFILEJIT
			state->tmpstats[1]++;
#endif
			if (block->physical != physical || block->mode != mode || block->version != version) {
				set = hash >> 16;
				block = &state->blocks[set * OSXJITWAYS];
				for (way = 0; way < OSXJITWAYS; way++) {
					if (block[way].physical == physical && block[way].mode == mode && block[way].version == version)
						break;
				}
#if OSXPROFILEJIT
				state->tmpstats[1]--;
				if (way < OSXJITWAYS)
					state->tmpstats[2]++;
				else {
					int tmpway;
					int tmpstale;
					tmpstale = 0;
					for (tmpway = 0; tmpway < OSXJITWAYS; tmpway++)
						if (block[tmpway].physical == physical && block[tmpway].mode == mode && block[tmpway].function)
							tmpstale = 1;
					if (tmpstale)
						state->tmpstats[3]++;
					else
						state->tmpstats[5]++;
					tmpway = state->victims[set];
					if (block[tmpway].function && block[tmpway].version == state->versions[block[tmpway].physical >> 12] + 1)
						state->tmpstats[4]++;
				}
#endif
				if (way == OSXJITWAYS) {
					way = state->victims[set];
					state->victims[set] = (uint8)((way + 1) & (OSXJITWAYS - 1));
					block[way].physical = physical;
					block[way].mode = mode;
					block[way].version = version;
					block[way].function = NULL;
					block[way].count = -1;
				}
				block += way;
				if (block->count < 0) {
					if (block->count > -hot)
						block->count--;
					else
						Compile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						Link(state, cpu->jitlink, block);
					else
						LinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				cpu->jitlink = NULL;
				InterpretRun(cpu, state);
				continue;
			}
			if (block->count > 0) {
				code = block->function(cpu);
#if OSXPROFILEJIT
				state->tmpstats[8]++;
				state->tmpstats[9 + (code & 3)]++;
#endif
				cpu->nextpc = cpu->pc;
				if (code == OSXJITLINK || code == OSXJITDYNAMIC) {
					linkkind = (uint32)code;
					linkmode = mode;
					continue;
				}
				if (code == OSXJITCONTINUE || cpu->slice <= 0)
					continue;
			}
		}
		cpu->jitlink = NULL;
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag)
			continue;
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
#if OSXPROFILEJIT
		state->tmpstats[15]++;
		state->tmpinterp[(instruction >> 26) << 10]++;
		if ((instruction >> 26) == 31 || (instruction >> 26) == 19)
			state->tmpinterp[((instruction >> 26) << 10) | ((instruction >> 1) & 0x3ff)]++;
#endif
	}
}

#if OSXPROFILEJIT
void OsxJitTmpStats(OsxCpu *cpu, FILE *log)
{
	static const char *const names[16] = { "flushes", "fronthit", "sethit", "stale", "evictlive", "newcompile", "bytes", "instrs", "dispatch", "retcontinue", "retinterpret", "retlink", "retdynamic", "fetchfault", "pagesinvalidated", "interpreted" };
	OsxNativeState *state;
	int index;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL)
		return;
	fprintf(log, "TMPJIT used %u", state->used);
	for (index = 0; index < 16; index++) {
		fprintf(log, " %s %llu", names[index], (unsigned long long)state->tmpstats[index]);
		state->tmpstats[index] = 0;
	}
	fprintf(log, " interp");
	for (index = 0; index < 65536; index++) {
		if (state->tmpinterp[index] > 500)
			fprintf(log, " %d/%d:%u", index >> 10, index & 1023, state->tmpinterp[index]);
		state->tmpinterp[index] = 0;
	}
	fprintf(log, "\n");
}
#endif
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

							x86 OSX JIT

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#elif OSXHOSTX86

#define OSXJITSETS 65536
#define OSXJITWAYS 4
#define OSXJITBLOCKS (OSXJITSETS * OSXJITWAYS)
#define OSXJITFRONT 16384
#define OSXJITRECORDS (256 * 1024)
#define OSXJITBYTES (32 * 1024 * 1024)
#define OSXJITLIMIT 32
#define OSXJITRESERVE 65536
#define OSXJITMISSES 1024
#define OSXJITENTRY 27
#define OSXJITHOT 3
#define OSXJITCONTINUE 0
#define OSXJITINTERPRET 1
#define OSXJITLINK 2
#define OSXJITDYNAMIC 3
#define OSXJITUPDATE 5
#define OSXJITPC 6
#define OSXJITSLICE 7
#define OSXJITFRAMESLICE 16
#define OSXJITOFFSET(member) ((uint32)offsetof(OsxCpu, member))
#define OSXJITGPR(reg) (OSXJITOFFSET(gpr) + (uint32)(reg) * 4)
#define OSXJITFPR(reg) (OSXJITOFFSET(fpr) + (uint32)(reg) * 8)
#define OSXJITRECORD(member) ((uint32)offsetof(OsxNativeRecord, member))

typedef int (*OsxNativeFunction)(OsxCpu *cpu);

typedef struct OsxNativeRecord {
	uint32 address;
	uint32 version;
	uint8 *host;
	uint32 *versionaddress;
	uint8 *target;
} OsxNativeRecord;

typedef struct OsxNativeBlock {
	uint32 physical;
	uint32 mode;
	uint32 version;
	int count;
	OsxNativeFunction function;
} OsxNativeBlock;

typedef struct OsxNativeState {
	uint8 *code;
	uint32 used;
	uint32 *versions;
	uint8 *compiledpages;
	uint32 pagecount;
	OsxNativeRecord *records;
	uint8 *checkthunk;
	uint8 *missthunk;
	uint8 *finishthunk;
	uint8 *interpretthunk;
	uint8 *fillthunk;
	uint8 *timethunk;
	uint8 *linkcallthunk;
	uint8 *dynamicthunk;
	uint8 *translatethunks[2][6];
	uint32 thunkbytes;
	uint64 compiled;
	uint64 linked;
	uint64 interpreted;
	int fused;
	uint8 victims[OSXJITSETS];
	OsxNativeBlock front[OSXJITFRONT];
	OsxNativeBlock blocks[OSXJITBLOCKS];
} OsxNativeState;

typedef struct OsxNativeEmitter {
	OsxNativeState *state;
	uint8 *cursor;
	uint32 physical;
	int deterministic;
	uint32 enabled;
	uint8 *misses[OSXJITMISSES];
	int misscounts[OSXJITMISSES];
	int misscount;
} OsxNativeEmitter;

static void Byte(OsxNativeEmitter *emitter, uint32 value)
{
	*emitter->cursor++ = (uint8)value;
}

static void Word(OsxNativeEmitter *emitter, uint32 value)
{
	memcpy(emitter->cursor, &value, 4);
	emitter->cursor += 4;
}

static void RegisterOp(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	Byte(emitter, opcode);
	Byte(emitter, 0xc0 | (source << 3) | target);
}

static void CpuMemory(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	Byte(emitter, opcode);
	if (offset < 128) {
		Byte(emitter, 0x43 | (reg << 3));
		Byte(emitter, offset);
	} else {
		Byte(emitter, 0x83 | (reg << 3));
		Word(emitter, offset);
	}
}

static void Stack(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	Byte(emitter, opcode);
	Byte(emitter, 0x44 | (reg << 3));
	Byte(emitter, 0x24);
	Byte(emitter, offset);
}

static void Immediate(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	Byte(emitter, 0xb8 | reg);
	Word(emitter, value);
}

static void ImmediateOp(OsxNativeEmitter *emitter, int operation, int reg, uint32 value)
{
	if (value <= 127 || value >= 0xffffff80u) {
		Byte(emitter, 0x83);
		Byte(emitter, 0xc0 | (operation << 3) | reg);
		Byte(emitter, value);
	} else {
		Byte(emitter, 0x81);
		Byte(emitter, 0xc0 | (operation << 3) | reg);
		Word(emitter, value);
	}
}

static void Shift(OsxNativeEmitter *emitter, int operation, int reg, uint32 amount)
{
	if (amount == 0)
		return;
	Byte(emitter, 0xc1);
	Byte(emitter, 0xc0 | (operation << 3) | reg);
	Byte(emitter, amount);
}

static uint8 *Jump(OsxNativeEmitter *emitter, int condition)
{
	uint8 *patch;
	Byte(emitter, 0x0f);
	Byte(emitter, 0x80 | condition);
	patch = emitter->cursor;
	Word(emitter, 0);
	return patch;
}

static void Patch(uint8 *patch, uint8 *target)
{
	uint32 distance;
	distance = (uint32)(target - (patch + 4));
	memcpy(patch, &distance, 4);
}

static void JumpTo(OsxNativeEmitter *emitter, uint8 *target)
{
	Word(emitter, (uint32)(target - (emitter->cursor + 4)));
}

static void Call(OsxNativeEmitter *emitter, uint8 *target)
{
	Byte(emitter, 0xe8);
	JumpTo(emitter, target);
}

static void Miss(OsxNativeEmitter *emitter, int condition, int count)
{
	emitter->misses[emitter->misscount] = Jump(emitter, condition);
	emitter->misscounts[emitter->misscount++] = count;
}

static void Charge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	ImmediateOp(emitter, 5, OSXJITSLICE, (uint32)count);
}

static void Executed(OsxNativeEmitter *emitter, int reg, int count)
{
	Stack(emitter, 0x8b, reg, OSXJITFRAMESLICE);
	RegisterOp(emitter, 0x29, reg, OSXJITSLICE);
	if (count)
		ImmediateOp(emitter, 0, reg, (uint32)count);
}

static void Settle(OsxNativeEmitter *emitter)
{
	CpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	Executed(emitter, 0, 0);
	CpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions));
	CpuMemory(emitter, 0x83, 2, OSXJITOFFSET(instructions) + 4);
	Byte(emitter, 0);
}

static void Release(OsxNativeEmitter *emitter, uint32 code)
{
	Immediate(emitter, 0, code);
	Byte(emitter, 0x83); Byte(emitter, 0xc4); Byte(emitter, 28);
	Byte(emitter, 0x5d);
	Byte(emitter, 0x5f);
	Byte(emitter, 0x5e);
	Byte(emitter, 0x5b);
	Byte(emitter, 0xc3);
}

static void Leave(OsxNativeEmitter *emitter, uint32 code)
{
	Settle(emitter);
	CpuMemory(emitter, 0x89, OSXJITSLICE, OSXJITOFFSET(slice));
	Release(emitter, code);
}

static void Return(OsxNativeEmitter *emitter, int count)
{
	Charge(emitter, count);
	Byte(emitter, 0xe9);
	JumpTo(emitter, emitter->state->interpretthunk);
}

static void DynamicExit(OsxNativeEmitter *emitter, int count)
{
	Charge(emitter, count);
	Byte(emitter, 0x0f); Byte(emitter, 0x8e);
	JumpTo(emitter, emitter->state->finishthunk);
	Byte(emitter, 0xe9);
	JumpTo(emitter, emitter->state->dynamicthunk);
}

static void TranslateThunk(OsxNativeEmitter *emitter, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	uint8 *again;
	retry = emitter->cursor;
	if (limit == 0xfff) {
		RegisterOp(emitter, 0x89, 1, 0);
	} else {
		Byte(emitter, 0x8d); Byte(emitter, 0x48); Byte(emitter, 0xfff - limit);
	}
	ImmediateOp(emitter, 4, 1, 0xfffff000);
	RegisterOp(emitter, 0x89, 2, 0);
	Shift(emitter, 5, 2, 12);
	ImmediateOp(emitter, 4, 2, OSXTLBMASK);
	Byte(emitter, 0x8d); Byte(emitter, 0x14); Byte(emitter, 0x52);
	Shift(emitter, 4, 2, 2);
	if (store)
		CpuMemory(emitter, 0x03, 2, OSXJITOFFSET(writetlb));
	else
		CpuMemory(emitter, 0x03, 2, OSXJITOFFSET(readtlb));
	Byte(emitter, 0x39); Byte(emitter, 0x0a);
	slow = Jump(emitter, 5);
	RegisterOp(emitter, 0x89, 1, 0);
	Shift(emitter, 5, 1, 28);
	Shift(emitter, 4, 1, 2);
	CpuMemory(emitter, 0x03, 1, OSXJITOFFSET(datasr));
	Byte(emitter, 0x8b); Byte(emitter, 0x09);
	Byte(emitter, 0x39); Byte(emitter, 0x4a); Byte(emitter, 4);
	segment = Jump(emitter, 5);
	ImmediateOp(emitter, 4, 0, 0xfff);
	Byte(emitter, 0x03); Byte(emitter, 0x42); Byte(emitter, 8);
	RegisterOp(emitter, 0x31, 1, 1);
	Byte(emitter, 0xc3);
	Patch(slow, emitter->cursor);
	RegisterOp(emitter, 0x89, 1, 0);
	ImmediateOp(emitter, 4, 1, 0xfffff000);
	Byte(emitter, 0x39); Byte(emitter, 0x0a);
	page = Jump(emitter, 5);
	ImmediateOp(emitter, 1, 1, 1);
	Byte(emitter, 0xc3);
	Patch(page, emitter->cursor);
	Patch(segment, emitter->cursor);
	if (store)
		Immediate(emitter, 1, OSXTLBWRITE);
	else
		Immediate(emitter, 1, OSXTLBREAD);
	Call(emitter, emitter->state->fillthunk);
	RegisterOp(emitter, 0x85, 1, 1);
	again = Jump(emitter, 5);
	Patch(again, retry);
	ImmediateOp(emitter, 1, 1, 1);
	Byte(emitter, 0xc3);
}

static void EmitThunks(OsxNativeState *state)
{
	static const uint32 translatelimits[6] = { 0xfff, 0xffe, 0xffc, 0xff8, 0xff0, 0xfe0 };
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
	emitter.state = state;
	emitter.cursor = state->code;
	emitter.misscount = 0;
	state->checkthunk = emitter.cursor;
	Byte(&emitter, 0x3b); Byte(&emitter, 0x75); Byte(&emitter, OSXJITRECORD(address));
	misses[0] = Jump(&emitter, 5);
	CpuMemory(&emitter, 0x8b, 0, OSXJITOFFSET(fetchtlb));
	RegisterOp(&emitter, 0x89, 2, OSXJITPC);
	Shift(&emitter, 5, 2, 12);
	ImmediateOp(&emitter, 4, 2, OSXTLBMASK);
	Byte(&emitter, 0x8d); Byte(&emitter, 0x14); Byte(&emitter, 0x52);
	Byte(&emitter, 0x8d); Byte(&emitter, 0x04); Byte(&emitter, 0x90);
	RegisterOp(&emitter, 0x89, 1, OSXJITPC);
	ImmediateOp(&emitter, 4, 1, 0xfffff000);
	Byte(&emitter, 0x39); Byte(&emitter, 0x08);
	misses[1] = Jump(&emitter, 5);
	RegisterOp(&emitter, 0x89, 1, OSXJITPC);
	Shift(&emitter, 5, 1, 28);
	CpuMemory(&emitter, 0x8b, 2, OSXJITOFFSET(fetchsr));
	Byte(&emitter, 0x8b); Byte(&emitter, 0x0c); Byte(&emitter, 0x8a);
	Byte(&emitter, 0x39); Byte(&emitter, 0x48); Byte(&emitter, 4);
	misses[2] = Jump(&emitter, 5);
	Byte(&emitter, 0x8b); Byte(&emitter, 0x40); Byte(&emitter, 8);
	Byte(&emitter, 0x3b); Byte(&emitter, 0x45); Byte(&emitter, OSXJITRECORD(host));
	misses[3] = Jump(&emitter, 5);
	Byte(&emitter, 0x8b); Byte(&emitter, 0x4d); Byte(&emitter, OSXJITRECORD(versionaddress));
	Byte(&emitter, 0x8b); Byte(&emitter, 0x09);
	Byte(&emitter, 0x3b); Byte(&emitter, 0x4d); Byte(&emitter, OSXJITRECORD(version));
	misses[4] = Jump(&emitter, 5);
	CpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(fetchhost));
	RegisterOp(&emitter, 0x89, 1, OSXJITPC);
	ImmediateOp(&emitter, 4, 1, 0xfffff000);
	ImmediateOp(&emitter, 1, 1, 1);
	CpuMemory(&emitter, 0x89, 1, OSXJITOFFSET(fetchtag));
	RegisterOp(&emitter, 0x31, 0, 0);
	for (index = 0; index < 5; index++)
		Patch(misses[index], emitter.cursor);
	Byte(&emitter, 0xc3);
	state->missthunk = emitter.cursor;
	CpuMemory(&emitter, 0x89, OSXJITUPDATE, OSXJITOFFSET(jitlink));
	Leave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = emitter.cursor;
	Leave(&emitter, OSXJITCONTINUE);
	state->interpretthunk = emitter.cursor;
	Leave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = emitter.cursor;
	Byte(&emitter, 0x58);
	ImmediateOp(&emitter, 5, 0, 9);
	CpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(jitlink));
	Leave(&emitter, OSXJITLINK);
	state->dynamicthunk = emitter.cursor;
	RegisterOp(&emitter, 0x89, OSXJITUPDATE, OSXJITPC);
	Shift(&emitter, 5, OSXJITUPDATE, 2);
	ImmediateOp(&emitter, 4, OSXJITUPDATE, OSXJITRECORDS - 1);
	Byte(&emitter, 0x8d); Byte(&emitter, 0x6c); Byte(&emitter, 0xad); Byte(&emitter, 0);
	Byte(&emitter, 0x8d); Byte(&emitter, 0x2c); Byte(&emitter, 0xad);
	Word(&emitter, (uint32)(uintptr_t)state->records);
	Call(&emitter, state->checkthunk);
	Byte(&emitter, 0x0f); Byte(&emitter, 0x85);
	JumpTo(&emitter, state->missthunk);
	Byte(&emitter, 0xff); Byte(&emitter, 0x65); Byte(&emitter, OSXJITRECORD(target));
	state->fillthunk = emitter.cursor;
	Byte(&emitter, 0x50);
	ImmediateOp(&emitter, 5, 4, 20);
	Stack(&emitter, 0x89, 3, 0);
	Stack(&emitter, 0x89, 0, 4);
	Stack(&emitter, 0x89, 1, 8);
	Call(&emitter, (uint8 *)(uintptr_t)OsxCpuFillTlb);
	RegisterOp(&emitter, 0x89, 1, 0);
	ImmediateOp(&emitter, 0, 4, 20);
	Byte(&emitter, 0x58);
	Byte(&emitter, 0xc3);
	state->timethunk = emitter.cursor;
	ImmediateOp(&emitter, 5, 4, 12);
	Stack(&emitter, 0x89, 3, 0);
	Call(&emitter, (uint8 *)(uintptr_t)OsxCpuTimebase);
	ImmediateOp(&emitter, 0, 4, 12);
	Byte(&emitter, 0xc3);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = emitter.cursor;
		TranslateThunk(&emitter, index / 6, translatelimits[index % 6]);
	}
	state->thunkbytes = ((uint32)(emitter.cursor - state->code) + 63) & ~63u;
	state->used = state->thunkbytes;
}

static void Exit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		DynamicExit(emitter, count);
		return;
	}
	Charge(emitter, count);
	Byte(emitter, 0x0f); Byte(emitter, 0x8e);
	JumpTo(emitter, emitter->state->finishthunk);
	Byte(emitter, 0xe9);
	Word(emitter, 0);
	Call(emitter, emitter->state->linkcallthunk);
}

static void Advance(OsxNativeEmitter *emitter)
{
	ImmediateOp(emitter, 0, OSXJITPC, 4);
}

static void Condition(OsxNativeEmitter *emitter, int field, int issigned)
{
	uint32 shift;
	Immediate(emitter, 0, 2);
	Immediate(emitter, 1, 4);
	Immediate(emitter, 2, 8);
	Byte(emitter, 0x0f);
	if (issigned)
		Byte(emitter, 0x4f);
	else
		Byte(emitter, 0x47);
	Byte(emitter, 0xc1);
	Byte(emitter, 0x0f);
	if (issigned)
		Byte(emitter, 0x4c);
	else
		Byte(emitter, 0x42);
	Byte(emitter, 0xc2);
	CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(xer));
	Shift(emitter, 5, 2, 31);
	RegisterOp(emitter, 0x09, 0, 2);
	shift = (uint32)(7 - field) * 4;
	Shift(emitter, 4, 0, shift);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	ImmediateOp(emitter, 4, 1, ~(15u << shift));
	RegisterOp(emitter, 0x09, 0, 1);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
}

static void Record(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		RegisterOp(emitter, 0x85, 0, 0);
		Condition(emitter, 0, 1);
	}
}

static void RequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRFP);
	Miss(emitter, 4, count);
}

static void RecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr));
		Shift(emitter, 5, 0, 4);
		ImmediateOp(emitter, 4, 0, 0x0f000000);
		CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
		ImmediateOp(emitter, 4, 1, 0xf0ffffff);
		RegisterOp(emitter, 0x09, 0, 1);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
	}
}

static void FloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	CpuMemory(emitter, 0x8b, 2, OSXJITFPR(reg) + 4);
	Shift(emitter, 5, 2, 20);
	ImmediateOp(emitter, 4, 2, 0x7ff);
	ImmediateOp(emitter, 7, 2, 0x7ff);
	Miss(emitter, 4, count);
	if (normal) {
		RegisterOp(emitter, 0x85, 2, 2);
		Miss(emitter, 4, count);
	}
}

static void Setcc(OsxNativeEmitter *emitter, uint32 condition, int reg)
{
	Byte(emitter, 0x0f);
	Byte(emitter, 0x90 | condition);
	Byte(emitter, 0xc0 | reg);
}

static void FloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x7e); Byte(emitter, 0xc0);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x6f); Byte(emitter, 0xc8);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xd1); Byte(emitter, 32);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x7e); Byte(emitter, 0xc9);
	RegisterOp(emitter, 0x89, 2, 1);
	Shift(emitter, 5, 2, 20);
	ImmediateOp(emitter, 4, 2, 0x7ff);
	ImmediateOp(emitter, 7, 2, 0x7ff);
	Miss(emitter, 4, count);
	RegisterOp(emitter, 0x85, 2, 2);
	zero = Jump(emitter, 4);
	if (single) {
		ImmediateOp(emitter, 7, 2, 897);
		Miss(emitter, 2, count);
	}
	RegisterOp(emitter, 0x89, 2, 1);
	Shift(emitter, 5, 2, 31);
	ImmediateOp(emitter, 0, 2, 1);
	Shift(emitter, 4, 2, 14);
	Byte(emitter, 0xe9);
	merge = emitter->cursor;
	Word(emitter, 0);
	Patch(zero, emitter->cursor);
	RegisterOp(emitter, 0x89, 2, 1);
	ImmediateOp(emitter, 4, 2, 0x7fffffff);
	RegisterOp(emitter, 0x09, 2, 0);
	Miss(emitter, 5, count);
	RegisterOp(emitter, 0x89, 2, 1);
	Shift(emitter, 5, 2, 31);
	Shift(emitter, 4, 2, 16);
	ImmediateOp(emitter, 1, 2, 0x2000);
	Patch(merge, emitter->cursor);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x11); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(target));
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr));
	ImmediateOp(emitter, 4, 0, ~0x1f000u);
	RegisterOp(emitter, 0x09, 0, 2);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(fpscr));
}

static void FloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x10); Byte(emitter, 0x83 | (vector << 3));
	Word(emitter, OSXJITFPR(reg));
}

static void FloatRounding(OsxNativeEmitter *emitter, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(fpscr));
	Word(emitter, 3);
	Miss(emitter, 5, count);
}

static void FloatNarrow(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x5a); Byte(emitter, 0xc0);
	Byte(emitter, 0xf3); Byte(emitter, 0x0f); Byte(emitter, 0x5a); Byte(emitter, 0xc0);
}

static void FloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	uint32 shift;
	FloatExponent(emitter, left, count, 0);
	FloatExponent(emitter, right, count, 0);
	RegisterOp(emitter, 0x31, 0, 0);
	RegisterOp(emitter, 0x31, 1, 1);
	RegisterOp(emitter, 0x31, 2, 2);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x2e); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(right));
	Setcc(emitter, 2, 0);
	Setcc(emitter, 7, 1);
	Setcc(emitter, 4, 2);
	Shift(emitter, 4, 0, 3);
	Shift(emitter, 4, 1, 2);
	RegisterOp(emitter, 0x01, 2, 2);
	RegisterOp(emitter, 0x09, 0, 1);
	RegisterOp(emitter, 0x09, 0, 2);
	RegisterOp(emitter, 0x89, 1, 0);
	Shift(emitter, 4, 1, 12);
	CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(fpscr));
	ImmediateOp(emitter, 4, 2, ~0xf000u);
	RegisterOp(emitter, 0x09, 2, 1);
	CpuMemory(emitter, 0x89, 2, OSXJITOFFSET(fpscr));
	shift = (uint32)(7 - field) * 4;
	Shift(emitter, 4, 0, shift);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	ImmediateOp(emitter, 4, 1, ~(0xfu << shift));
	RegisterOp(emitter, 0x09, 1, 0);
	CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(cr));
}

static void FloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	FloatLoad(emitter, 0, right);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x2c); Byte(emitter, 0xc0);
	ImmediateOp(emitter, 7, 0, 0x80000000u);
	Miss(emitter, 4, count);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x2a); Byte(emitter, 0xc8);
	Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x2e); Byte(emitter, 0xc8);
	exact = Jump(emitter, 4);
	Byte(emitter, 0x81); Byte(emitter, 0x8b);
	Word(emitter, OSXJITOFFSET(fpscr));
	Word(emitter, 0x82000000u);
	Patch(exact, emitter->cursor);
	CpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
	Byte(emitter, 0xc7); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(target) + 4);
	Word(emitter, 0xfff80000u);
}

static void FloatSingleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(reg));
	Word(emitter, 0x1fffffff);
	Miss(emitter, 5, count);
	Byte(emitter, 0x8b); Byte(emitter, 0x93);
	Word(emitter, OSXJITFPR(reg) + 4);
	Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 20);
	Byte(emitter, 0x81); Byte(emitter, 0xe2); Word(emitter, 0x7ff);
	Byte(emitter, 0x81); Byte(emitter, 0xea); Word(emitter, 897);
	Byte(emitter, 0x81); Byte(emitter, 0xfa); Word(emitter, 254);
	Miss(emitter, 3, count);
}

static void FloatSingleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	FloatSingleOperand(emitter, left, count);
	FloatSingleOperand(emitter, multiplier, count);
	Byte(emitter, 0x8b); Byte(emitter, 0x93);
	Word(emitter, OSXJITFPR(right) + 4);
	Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 20);
	Byte(emitter, 0x81); Byte(emitter, 0xe2); Word(emitter, 0x7ff);
	Byte(emitter, 0x81); Byte(emitter, 0xfa); Word(emitter, 0x7ff);
	Miss(emitter, 4, count);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x59); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(multiplier));
	FloatLoad(emitter, 1, right);
	if (extended == 28 || extended == 30) {
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x76); Byte(emitter, 0xd2);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xf2); Byte(emitter, 63);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x57); Byte(emitter, 0xca);
	}
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x58); Byte(emitter, 0xc1);
	if (extended >= 30) {
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x76); Byte(emitter, 0xc9);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xf1); Byte(emitter, 63);
		Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x57); Byte(emitter, 0xc1);
	}
	FloatNarrow(emitter);
	FloatClass(emitter, target, count, 1);
}

static int Float(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 extended;
	int target;
	int left;
	int right;
	int multiplier;
	int single;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	single = (instruction >> 26) == 59;
	if (!single && !(instruction & 0x20)) {
		switch (extended) {
		case 40: case 72: case 136: case 264:
			RequireFloat(emitter, count);
			CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right));
			CpuMemory(emitter, 0x8b, 1, OSXJITFPR(right) + 4);
			if (extended != 72) {
				Byte(emitter, 0x0f); Byte(emitter, 0xba);
				if (extended == 40)
					Byte(emitter, 0xf9);
				else if (extended == 136)
					Byte(emitter, 0xe9);
				else
					Byte(emitter, 0xf1);
				Byte(emitter, 31);
			}
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
			CpuMemory(emitter, 0x89, 1, OSXJITFPR(target) + 4);
			break;
		case 0: case 32:
			RequireFloat(emitter, count);
			FloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			RequireFloat(emitter, count);
			FloatRounding(emitter, count);
			FloatExponent(emitter, right, count, 0);
			FloatLoad(emitter, 0, right);
			FloatNarrow(emitter);
			FloatClass(emitter, target, count, 1);
			break;
		case 15:
			RequireFloat(emitter, count);
			FloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		RecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		if (!emitter->state->fused && single) {
			RequireFloat(emitter, count);
			FloatRounding(emitter, count);
			FloatSingleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			RecordFloat(emitter, instruction);
			return 1;
		}
		if (!emitter->state->fused)
			return 0;
		RequireFloat(emitter, count);
		FloatRounding(emitter, count);
		FloatExponent(emitter, left, count, 0);
		FloatExponent(emitter, multiplier, count, 0);
		FloatExponent(emitter, right, count, 0);
		FloatLoad(emitter, 0, left);
		FloatLoad(emitter, 1, multiplier);
		Byte(emitter, 0xc4); Byte(emitter, 0xe2); Byte(emitter, 0xf1);
		if (extended == 28 || extended == 30)
			Byte(emitter, 0xab);
		else
			Byte(emitter, 0xa9);
		Byte(emitter, 0x83);
		Word(emitter, OSXJITFPR(right));
		if (extended >= 30) {
			Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x76); Byte(emitter, 0xc9);
			Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x73); Byte(emitter, 0xf1); Byte(emitter, 63);
			Byte(emitter, 0x66); Byte(emitter, 0x0f); Byte(emitter, 0x57); Byte(emitter, 0xc1);
		}
		if (single)
			FloatNarrow(emitter);
		FloatClass(emitter, target, count, single);
		RecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	RequireFloat(emitter, count);
	FloatRounding(emitter, count);
	FloatExponent(emitter, left, count, 0);
	FloatExponent(emitter, right, count, extended == 18);
	FloatLoad(emitter, 0, left);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f);
	if (extended == 18)
		Byte(emitter, 0x5e);
	else if (extended == 20)
		Byte(emitter, 0x5c);
	else if (extended == 21)
		Byte(emitter, 0x58);
	else
		Byte(emitter, 0x59);
	Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(right));
	if (single)
		FloatNarrow(emitter);
	FloatClass(emitter, target, count, single);
	RecordFloat(emitter, instruction);
	return 1;
}

static uint32 RotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void Address(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
	else
		RegisterOp(emitter, 0x31, 0, 0);
	if (index >= 0)
		CpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
	else if (immediate)
		ImmediateOp(emitter, 0, 0, immediate);
	if (update)
		RegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
}

static int LimitIndex(uint32 limit)
{
	switch (limit) {
	case 0xfff: return 0;
	case 0xffe: return 1;
	case 0xffc: return 2;
	case 0xff8: return 3;
	case 0xff0: return 4;
	}
	return 5;
}

static void Translate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	Call(emitter, emitter->state->translatethunks[store != 0][LimitIndex(limit)]);
	Miss(emitter, 5, count);
}

static int Multiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
{
	int first;
	int base;
	uint32 registers;
	uint8 *loop;
	first = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	registers = (uint32)(32 - first);
	if (opcode == 46 && base >= first)
		return 0;
	Address(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	RegisterOp(emitter, 0x89, 1, 0);
	ImmediateOp(emitter, 4, 1, 0xfff);
	ImmediateOp(emitter, 7, 1, 0x1000 - registers * 4);
	Miss(emitter, 7, count);
	Translate(emitter, opcode == 47, 0xffc, count);
	Byte(emitter, 0x8d); Byte(emitter, 0xab);
	Word(emitter, OSXJITGPR(first));
	Immediate(emitter, 1, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		Byte(emitter, 0x8b); Byte(emitter, 0x10);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x89); Byte(emitter, 0x55); Byte(emitter, 0);
	} else {
		Byte(emitter, 0x8b); Byte(emitter, 0x55); Byte(emitter, 0);
		Byte(emitter, 0x0f); Byte(emitter, 0xca);
		Byte(emitter, 0x89); Byte(emitter, 0x10);
	}
	ImmediateOp(emitter, 0, 0, 4);
	ImmediateOp(emitter, 0, OSXJITUPDATE, 4);
	Byte(emitter, 0x49);
	Byte(emitter, 0x75);
	Byte(emitter, (uint32)(loop - (emitter->cursor + 1)) & 0xff);
	return 1;
}

static void CallHelper(OsxNativeEmitter *emitter, uint8 *function)
{
	CpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	Call(emitter, function);
	CpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
}

static void VectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	Byte(emitter, 0x83); Byte(emitter, 0xbb);
	Word(emitter, OSXJITOFFSET(altivec));
	Byte(emitter, 0);
	Miss(emitter, 4, count);
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRVEC);
	Miss(emitter, 4, count);
}

static void VectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	VectorEnabled(emitter, count);
	Stack(emitter, 0x89, 3, 0);
	Byte(emitter, 0xc7); Byte(emitter, 0x44); Byte(emitter, 0x24); Byte(emitter, 4);
	Word(emitter, instruction);
	CallHelper(emitter, (uint8 *)(uintptr_t)function);
}

static void Memory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
{
	int target;
	int base;
	int update;
	int store;
	int wide;
	int single;
	int reversed;
	uint32 limit;
	uint32 immediate;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	reversed = opcode >= 64;
	update = opcode & 1;
	opcode &= 62;
	store = opcode == 36 || opcode == 38 || opcode == 44 || opcode == 52 || opcode == 54;
	wide = opcode == 50 || opcode == 54;
	single = opcode == 48 || opcode == 52;
	if (wide || single)
		RequireFloat(emitter, count);
	if (single && store) {
		CpuMemory(emitter, 0x8b, 0, OSXJITFPR(target));
		Stack(emitter, 0x89, 0, 0);
		CpuMemory(emitter, 0x8b, 0, OSXJITFPR(target) + 4);
		Stack(emitter, 0x89, 0, 4);
		CallHelper(emitter, (uint8 *)(uintptr_t)OsxCpuDoubleToSingle);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(jitscratch));
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	Address(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		RegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
	Translate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			CpuMemory(emitter, 0x8b, 1, OSXJITFPR(target) + 4);
			CpuMemory(emitter, 0x8b, 2, OSXJITFPR(target));
			Byte(emitter, 0x0f); Byte(emitter, 0xc9);
			Byte(emitter, 0x0f); Byte(emitter, 0xca);
			Byte(emitter, 0x89); Byte(emitter, 0x08);
			Byte(emitter, 0x89); Byte(emitter, 0x50); Byte(emitter, 4);
		} else {
			if (single)
				CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(jitscratch));
			else
				CpuMemory(emitter, 0x8b, 2, OSXJITGPR(target));
			if (opcode == 36 || opcode == 52) {
				if (!reversed) {
					Byte(emitter, 0x0f); Byte(emitter, 0xca);
				}
				Byte(emitter, 0x89); Byte(emitter, 0x10);
			} else if (opcode == 44) {
				Byte(emitter, 0x66); Byte(emitter, 0xc1); Byte(emitter, 0xca); Byte(emitter, 8);
				Byte(emitter, 0x66); Byte(emitter, 0x89); Byte(emitter, 0x10);
			} else {
				Byte(emitter, 0x88); Byte(emitter, 0x10);
			}
		}
	} else {
		if (wide) {
			Byte(emitter, 0x8b); Byte(emitter, 0x08);
			Byte(emitter, 0x8b); Byte(emitter, 0x40); Byte(emitter, 4);
			Byte(emitter, 0x0f); Byte(emitter, 0xc9);
			Byte(emitter, 0x0f); Byte(emitter, 0xc8);
			CpuMemory(emitter, 0x89, 1, OSXJITFPR(target) + 4);
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
		} else if (opcode == 32 || single) {
			Byte(emitter, 0x8b); Byte(emitter, 0x00);
			if (!reversed) {
				Byte(emitter, 0x0f); Byte(emitter, 0xc8);
			}
		} else {
			Byte(emitter, 0x0f);
			if (opcode == 34)
				Byte(emitter, 0xb6);
			else
				Byte(emitter, 0xb7);
			Byte(emitter, 0x00);
			if (opcode != 34) {
				Byte(emitter, 0x66); Byte(emitter, 0xc1); Byte(emitter, 0xc0); Byte(emitter, 8);
				if (opcode == 42) {
					Byte(emitter, 0x0f); Byte(emitter, 0xbf); Byte(emitter, 0xc0);
				}
			}
		}
		if (single) {
			Stack(emitter, 0x89, 0, 0);
			CallHelper(emitter, (uint8 *)(uintptr_t)OsxCpuSingleToDouble);
			CpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
			CpuMemory(emitter, 0x89, 2, OSXJITFPR(target) + 4);
		} else if (!wide) {
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		}
	}
	if (update)
		CpuMemory(emitter, 0x89, OSXJITUPDATE, OSXJITGPR(base));
}

static int Branch(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 options;
	uint32 displacement;
	uint32 extended;
	uint8 *counterpatch;
	uint8 *conditionpatch;
	uint32 offset;
	uint32 targetoffset;
	opcode = instruction >> 26;
	options = (instruction >> 21) & 31;
	counterpatch = NULL;
	conditionpatch = NULL;
	offset = (emitter->physical & 4095) + (uint32)count * 4;
	targetoffset = 4096;
	if (opcode == 19) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 528) {
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(ctr));
			options |= 4;
		} else {
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(lr));
		}
		ImmediateOp(emitter, 4, 1, ~3u);
	} else {
		if (opcode == 18) {
			displacement = instruction & 0x03fffffc;
			if (displacement & 0x02000000)
				displacement |= 0xfc000000;
			options = 20;
		} else {
			displacement = (uint32)(int32)(int16)(instruction & 0xfffc);
		}
		Immediate(emitter, 1, displacement);
		if (!(instruction & 2)) {
			RegisterOp(emitter, 0x01, 1, OSXJITPC);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		RegisterOp(emitter, 0x89, 2, OSXJITPC);
		ImmediateOp(emitter, 0, 2, 4);
		CpuMemory(emitter, 0x89, 2, OSXJITOFFSET(lr));
	}
	if (!(options & 4)) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(ctr));
		ImmediateOp(emitter, 5, 0, 1);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(ctr));
		if (options & 2)
			counterpatch = Jump(emitter, 5);
		else
			counterpatch = Jump(emitter, 4);
	}
	if (!(options & 16)) {
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(cr));
		Byte(emitter, 0xf7); Byte(emitter, 0xc2);
		Word(emitter, 1u << (31 - ((instruction >> 16) & 31)));
		if (options & 8)
			conditionpatch = Jump(emitter, 4);
		else
			conditionpatch = Jump(emitter, 5);
	}
	RegisterOp(emitter, 0x89, OSXJITPC, 1);
	Exit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			Patch(counterpatch, emitter->cursor);
		if (conditionpatch)
			Patch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void LoadCarry(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0x0f); Byte(emitter, 0xba); Byte(emitter, 0xa3);
	Word(emitter, OSXJITOFFSET(xer));
	Byte(emitter, 29);
}

static void StoreCarry(OsxNativeEmitter *emitter)
{
	Byte(emitter, 0x0f); Byte(emitter, 0xb6); Byte(emitter, 0xd2);
	Shift(emitter, 4, 2, 29);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(xer));
	ImmediateOp(emitter, 4, 1, 0xdfffffff);
	RegisterOp(emitter, 0x09, 1, 2);
	CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(xer));
}

static void RequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(msr));
	Word(emitter, OSXMSRPR);
	Miss(emitter, 5, count);
}

static int Carrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		CpuMemory(emitter, 0x03, 0, OSXJITGPR(right));
		Setcc(emitter, 2, 2);
		break;
	case 8:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(right));
		CpuMemory(emitter, 0x2b, 0, OSXJITGPR(left));
		Setcc(emitter, 3, 2);
		break;
	case 138:
	case 136:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 136) {
			Byte(emitter, 0xf7); Byte(emitter, 0xd0);
		}
		LoadCarry(emitter);
		CpuMemory(emitter, 0x13, 0, OSXJITGPR(right));
		Setcc(emitter, 2, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 200 || extended == 232) {
			Byte(emitter, 0xf7); Byte(emitter, 0xd0);
		}
		LoadCarry(emitter);
		if (extended == 202 || extended == 200)
			ImmediateOp(emitter, 2, 0, 0);
		else
			ImmediateOp(emitter, 2, 0, 0xffffffff);
		Setcc(emitter, 2, 2);
		break;
	default:
		return 0;
	}
	CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
	StoreCarry(emitter);
	Record(emitter, instruction);
	return 1;
}

static int Arithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	int left;
	int right;
	uint8 *ordinary;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 104:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		Byte(emitter, 0xf7); Byte(emitter, 0xd8);
		break;
	case 11:
	case 75:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		Byte(emitter, 0xf7);
		if (extended == 11)
			Byte(emitter, 0xa3);
		else
			Byte(emitter, 0xab);
		Word(emitter, OSXJITGPR(right));
		RegisterOp(emitter, 0x89, 0, 2);
		break;
	case 459:
	case 491:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(right));
		RegisterOp(emitter, 0x85, 1, 1);
		Miss(emitter, 4, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 491) {
			ImmediateOp(emitter, 7, 1, 0xffffffff);
			ordinary = Jump(emitter, 5);
			ImmediateOp(emitter, 7, 0, 0x80000000);
			Miss(emitter, 4, count);
			Patch(ordinary, emitter->cursor);
			Byte(emitter, 0x99);
			Byte(emitter, 0xf7); Byte(emitter, 0xf9);
		} else {
			RegisterOp(emitter, 0x31, 2, 2);
			Byte(emitter, 0xf7); Byte(emitter, 0xf1);
		}
		break;
	default:
		return 0;
	}
	CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
	Record(emitter, instruction);
	return 1;
}

static int Logical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int source;
	int target;
	int amount;
	source = (instruction >> 21) & 31;
	target = (instruction >> 16) & 31;
	amount = (instruction >> 11) & 31;
	switch (extended) {
	case 24:
	case 536:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount));
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		Byte(emitter, 0xd3);
		if (extended == 24)
			Byte(emitter, 0xe0);
		else
			Byte(emitter, 0xe8);
		Byte(emitter, 0xf6); Byte(emitter, 0xc1); Byte(emitter, 32);
		Byte(emitter, 0x74); Byte(emitter, 2);
		RegisterOp(emitter, 0x31, 0, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		break;
	case 26:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		Immediate(emitter, 2, 63);
		Byte(emitter, 0x0f); Byte(emitter, 0xbd); Byte(emitter, 0xc0);
		Byte(emitter, 0x0f); Byte(emitter, 0x44); Byte(emitter, 0xc2);
		ImmediateOp(emitter, 6, 0, 31);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		break;
	case 792:
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount));
		ImmediateOp(emitter, 4, 1, 63);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		Immediate(emitter, 2, 0xffffffff);
		ImmediateOp(emitter, 7, 1, 32);
		Byte(emitter, 0x73); Byte(emitter, 8);
		Immediate(emitter, 2, 1);
		Byte(emitter, 0xd3); Byte(emitter, 0xe2);
		Byte(emitter, 0x4a);
		RegisterOp(emitter, 0x21, 2, 0);
		ImmediateOp(emitter, 7, 1, 31);
		Byte(emitter, 0x76); Byte(emitter, 5);
		Immediate(emitter, 1, 31);
		Byte(emitter, 0xd3); Byte(emitter, 0xf8);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		RegisterOp(emitter, 0x85, 2, 2);
		Setcc(emitter, 5, 2);
		RegisterOp(emitter, 0x85, 0, 0);
		Setcc(emitter, 8, 1);
		Byte(emitter, 0x20); Byte(emitter, 0xca);
		StoreCarry(emitter);
		break;
	case 824:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		if (amount == 0) {
			RegisterOp(emitter, 0x31, 2, 2);
		} else {
			RegisterOp(emitter, 0x89, 2, 0);
			RegisterOp(emitter, 0x89, 1, 0);
			ImmediateOp(emitter, 4, 1, (1u << amount) - 1);
			Setcc(emitter, 5, 1);
			Shift(emitter, 5, 2, 31);
			Byte(emitter, 0x20); Byte(emitter, 0xca);
			Shift(emitter, 7, 0, (uint32)amount);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		break;
	default:
		return 0;
	}
	Record(emitter, instruction);
	return 1;
}

static int SpecialOffset(uint32 number, uint32 *offset)
{
	switch (number) {
	case 18: *offset = OSXJITOFFSET(dsisr); return 1;
	case 19: *offset = OSXJITOFFSET(dar); return 1;
	case 26: *offset = OSXJITOFFSET(srr0); return 1;
	case 27: *offset = OSXJITOFFSET(srr1); return 1;
	case 272: case 273: case 274: case 275:
		*offset = OSXJITOFFSET(sprg) + (number - 272) * 4;
		return 1;
	}
	return 0;
}

static void Callout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	CpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	Executed(emitter, 0, count);
	CpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions));
	CpuMemory(emitter, 0x83, 2, OSXJITOFFSET(instructions) + 4);
	Byte(emitter, 0);
	Stack(emitter, 0x89, 3, 0);
	Byte(emitter, 0xc7); Byte(emitter, 0x44); Byte(emitter, 0x24); Byte(emitter, 4);
	Word(emitter, instruction);
	Call(emitter, (uint8 *)(uintptr_t)OsxCpuExecuteInline);
	Executed(emitter, 2, count);
	CpuMemory(emitter, 0x29, 2, OSXJITOFFSET(instructions));
	CpuMemory(emitter, 0x83, 3, OSXJITOFFSET(instructions) + 4);
	Byte(emitter, 0);
	RegisterOp(emitter, 0x85, 0, 0);
	resume = Jump(emitter, 4);
	Charge(emitter, count + 1);
	CpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
	CpuMemory(emitter, 0x83, 7, OSXJITOFFSET(slice));
	Byte(emitter, 0);
	running = Jump(emitter, 15);
	Settle(emitter);
	Release(emitter, OSXJITCONTINUE);
	Patch(running, emitter->cursor);
	Leave(emitter, OSXJITCONTINUE);
	Patch(resume, emitter->cursor);
	CpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
}

static int System(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	uint32 number;
	uint32 offset;
	uint32 mask;
	uint32 field;
	uint8 *quiet;
	uint8 *pending;
	uint8 *slowmask;
	uint8 *slowpower;
	uint8 *done;
	target = (instruction >> 21) & 31;
	switch (extended) {
	case 54: case 86: case 246: case 278: case 598: case 854:
		return 1;
	case 144:
		mask = 0;
		for (field = 0; field < 8; field++)
			if (instruction & (0x80000u >> field))
				mask |= 0xf0000000u >> (field * 4);
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (mask != 0xffffffffu) {
			ImmediateOp(emitter, 4, 0, mask);
			CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
			ImmediateOp(emitter, 4, 1, ~mask);
			RegisterOp(emitter, 0x09, 0, 1);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
		return 1;
	case 83:
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(msr));
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 146:
		emitter->enabled = 0;
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 1, OSXJITGPR(target));
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(msr));
		RegisterOp(emitter, 0x31, 2, 1);
		Byte(emitter, 0xf7); Byte(emitter, 0xc2);
		Word(emitter, ~(uint32)OSXMSREE);
		slowmask = Jump(emitter, 5);
		Byte(emitter, 0xf7); Byte(emitter, 0xc1);
		Word(emitter, OSXMSRPOW);
		slowpower = Jump(emitter, 5);
		CpuMemory(emitter, 0x89, 1, OSXJITOFFSET(msr));
		RegisterOp(emitter, 0x21, 2, 1);
		quiet = Jump(emitter, 4);
		CpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(externalline));
		CpuMemory(emitter, 0x0b, 2, OSXJITOFFSET(decpending));
		pending = Jump(emitter, 4);
		Advance(emitter);
		Charge(emitter, count + 1);
		Settle(emitter);
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(slice));
		Word(emitter, 0);
		Release(emitter, OSXJITCONTINUE);
		Patch(quiet, emitter->cursor);
		Patch(pending, emitter->cursor);
		Byte(emitter, 0xe9);
		done = emitter->cursor;
		Word(emitter, 0);
		Patch(slowmask, emitter->cursor);
		Patch(slowpower, emitter->cursor);
		Callout(emitter, instruction, count);
		Patch(done, emitter->cursor);
		return 1;
	case 595:
		RequireSupervisorMode(emitter, count);
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		Call(emitter, emitter->state->timethunk);
		if (number == 269)
			RegisterOp(emitter, 0x89, 0, 2);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 210:
	case 242:
		RequireSupervisorMode(emitter, count);
		if (extended == 210)
			Immediate(emitter, 0, (instruction >> 16) & 15);
		else {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR((instruction >> 11) & 31));
			Shift(emitter, 5, 0, 28);
		}
		RegisterOp(emitter, 0x89, 1, OSXJITPC);
		Shift(emitter, 5, 1, 28);
		RegisterOp(emitter, 0x39, 1, 0);
		Miss(emitter, 4, count);
		CpuMemory(emitter, 0x8b, 2, OSXJITGPR(target));
		Byte(emitter, 0x89); Byte(emitter, 0x94); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(sr));
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(fetchtag));
		Word(emitter, 0);
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!SpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			Callout(emitter, instruction, count);
			return 1;
		}
		RequireSupervisorMode(emitter, count);
		if (extended == 339) {
			CpuMemory(emitter, 0x8b, 0, offset);
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		} else {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			CpuMemory(emitter, 0x89, 0, offset);
		}
		return 1;
	}
	return 0;
}

static void ConditionZero(OsxNativeEmitter *emitter)
{
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(xer));
	Shift(emitter, 5, 0, 31);
}

static void StoreConditionZero(OsxNativeEmitter *emitter)
{
	Shift(emitter, 4, 0, 28);
	CpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	ImmediateOp(emitter, 4, 1, 0x0fffffff);
	RegisterOp(emitter, 0x09, 0, 1);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
}

static int Reserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	Address(emitter, base, index, 0, 0);
	Byte(emitter, 0xa8); Byte(emitter, 3);
	Miss(emitter, 5, count);
	if (extended == 20) {
		RegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
		Memory(emitter, instruction, 32, index, count);
		CpuMemory(emitter, 0x89, OSXJITUPDATE, OSXJITOFFSET(reservation));
		Byte(emitter, 0xc7); Byte(emitter, 0x83);
		Word(emitter, OSXJITOFFSET(reservationvalid));
		Word(emitter, 1);
		return 1;
	}
	Byte(emitter, 0x81); Byte(emitter, 0xbb);
	Word(emitter, OSXJITOFFSET(reservationvalid));
	Word(emitter, 0);
	invalid = Jump(emitter, 4);
	CpuMemory(emitter, 0x39, 0, OSXJITOFFSET(reservation));
	elsewhere = Jump(emitter, 5);
	Memory(emitter, instruction, 36, index, count);
	ConditionZero(emitter);
	ImmediateOp(emitter, 1, 0, 2);
	Byte(emitter, 0xe9);
	done = emitter->cursor;
	Word(emitter, 0);
	Patch(invalid, emitter->cursor);
	Patch(elsewhere, emitter->cursor);
	ConditionZero(emitter);
	Patch(done, emitter->cursor);
	Byte(emitter, 0xc7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(reservationvalid));
	Word(emitter, 0);
	StoreConditionZero(emitter);
	return 1;
}

static int ZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	int offset;
	Address(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	ImmediateOp(emitter, 4, 0, ~31u);
	Translate(emitter, 1, 0xfe0, count);
	RegisterOp(emitter, 0x31, 2, 2);
	for (offset = 0; offset < 32; offset += 4) {
		Byte(emitter, 0x89); Byte(emitter, 0x50);
		Byte(emitter, (uint32)offset);
	}
	return 1;
}

static int ConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
{
	uint32 extended;
	uint32 bitd;
	uint32 bita;
	uint32 bitb;
	extended = (instruction >> 1) & 0x3ff;
	bitd = (instruction >> 21) & 31;
	bita = (instruction >> 16) & 31;
	bitb = (instruction >> 11) & 31;
	if (extended == 0) {
		CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
		RegisterOp(emitter, 0x89, 1, 0);
		Shift(emitter, 5, 1, (7 - (bita >> 2)) * 4);
		ImmediateOp(emitter, 4, 1, 0xf);
		Shift(emitter, 4, 1, (7 - (bitd >> 2)) * 4);
		ImmediateOp(emitter, 4, 0, ~(0xfu << ((7 - (bitd >> 2)) * 4)));
		RegisterOp(emitter, 0x09, 0, 1);
		CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
	RegisterOp(emitter, 0x89, 1, 0);
	Shift(emitter, 5, 1, 31 - bita);
	ImmediateOp(emitter, 4, 1, 1);
	RegisterOp(emitter, 0x89, 2, 0);
	Shift(emitter, 5, 2, 31 - bitb);
	ImmediateOp(emitter, 4, 2, 1);
	if (extended == 129 || extended == 417)
		ImmediateOp(emitter, 6, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		RegisterOp(emitter, 0x09, 1, 2);
	else if (extended == 129 || extended == 225 || extended == 257)
		RegisterOp(emitter, 0x21, 1, 2);
	else
		RegisterOp(emitter, 0x31, 1, 2);
	if (extended == 33 || extended == 225 || extended == 289)
		ImmediateOp(emitter, 6, 1, 1);
	Shift(emitter, 4, 1, 31 - bitd);
	ImmediateOp(emitter, 4, 0, ~(1u << (31 - bitd)));
	RegisterOp(emitter, 0x09, 0, 1);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
	return 1;
}

static int Interpretable(uint32 instruction)
{
	uint32 opcode;
	uint32 extended;
	opcode = instruction >> 26;
	if (opcode == 4)
		return 1;
	if (opcode != 31)
		return 0;
	if ((instruction & 0x6fe) == 0xce || (instruction & 0x6fe) == 0x2ce)
		return 1;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 6: case 7: case 38: case 39: case 71: case 135: case 167: case 199:
	case 342: case 374: case 758: case 790: case 822: case 918: case 982:
		return 1;
	}
	return 0;
}

static int Emit(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 extended;
	uint32 immediate;
	uint32 mask;
	uint32 number;
	uint32 offset;
	int target;
	int base;
	int index;
	OsxVectorFunction handler;
	opcode = instruction >> 26;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	immediate = instruction & 0xffff;
	if (opcode == 16 || opcode == 18 || (opcode == 19 && (((instruction >> 1) & 0x3ff) == 16 || ((instruction >> 1) & 0x3ff) == 528)))
		return Branch(emitter, instruction, count);
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return ConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!Multiple(emitter, instruction, opcode, count))
			Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxCpuVectorHandler(instruction);
		if (handler) {
			VectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (Interpretable(instruction)) {
		Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55) {
		Memory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
		if (!Float(emitter, instruction, count))
			Callout(emitter, instruction, count);
		return 1;
	case 8:
		Immediate(emitter, 0, (uint32)(int32)(int16)immediate);
		CpuMemory(emitter, 0x2b, 0, OSXJITGPR(base));
		Setcc(emitter, 3, 2);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		return 1;
	case 12: case 13:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		ImmediateOp(emitter, 0, 0, (uint32)(int32)(int16)immediate);
		Setcc(emitter, 2, 2);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		if (opcode == 13) {
			RegisterOp(emitter, 0x85, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 7:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		Byte(emitter, 0x69); Byte(emitter, 0xc0);
		Word(emitter, (uint32)(int32)(int16)immediate);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 10: case 11:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		ImmediateOp(emitter, 7, 0, immediate);
		Condition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		Address(emitter, base, -1, immediate, 0);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 20: case 21: case 23:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (opcode == 23) {
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(index));
			Byte(emitter, 0xd3); Byte(emitter, 0xc0);
		} else {
			Shift(emitter, 0, 0, (uint32)index);
		}
		mask = RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		ImmediateOp(emitter, 4, 0, mask);
		if (opcode == 20) {
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(base));
			ImmediateOp(emitter, 4, 1, ~mask);
			RegisterOp(emitter, 0x09, 0, 1);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
		Record(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26)
			ImmediateOp(emitter, 1, 0, immediate);
		else if (opcode < 28)
			ImmediateOp(emitter, 6, 0, immediate);
		else
			ImmediateOp(emitter, 4, 0, immediate);
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
		if (opcode >= 28) {
			RegisterOp(emitter, 0x85, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			Memory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			Memory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			Memory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
			CpuMemory(emitter, 0x3b, 0, OSXJITGPR(index));
			Condition(emitter, target >> 2, extended == 0);
			return 1;
		}
		if (extended == 339 || extended == 467) {
			number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
			if (number == 1)
				offset = OSXJITOFFSET(xer);
			else if (number == 8)
				offset = OSXJITOFFSET(lr);
			else if (number == 9)
				offset = OSXJITOFFSET(ctr);
			else if (number == 256)
				offset = OSXJITOFFSET(vrsave);
			else
				return System(emitter, instruction, extended, count);
			if (extended == 339) {
				CpuMemory(emitter, 0x8b, 0, offset);
				CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			} else {
				CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
				if (number == 1)
					ImmediateOp(emitter, 4, 0, 0xe000007f);
				CpuMemory(emitter, 0x89, 0, offset);
			}
			return 1;
		}
		if (extended == 19) {
			CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
			if (extended == 266)
				CpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
			else if (extended == 40) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd8);
				CpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
			} else {
				Byte(emitter, 0x0f); Byte(emitter, 0xaf); Byte(emitter, 0x83);
				Word(emitter, OSXJITGPR(index));
			}
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			CpuMemory(emitter, 0x8b, 1, OSXJITGPR(index));
			if (extended == 60 || extended == 412) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd1);
			}
			if (extended == 28 || extended == 60 || extended == 476)
				RegisterOp(emitter, 0x21, 0, 1);
			else if (extended == 284 || extended == 316)
				RegisterOp(emitter, 0x31, 0, 1);
			else
				RegisterOp(emitter, 0x09, 0, 1);
			if (extended == 124 || extended == 284 || extended == 476) {
				Byte(emitter, 0xf7); Byte(emitter, 0xd0);
			}
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return Reserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return ZeroBlock(emitter, instruction, count);
		if (Carrying(emitter, instruction, extended) || Arithmetic(emitter, instruction, extended, count) || Logical(emitter, instruction, extended) || System(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			CpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			Byte(emitter, 0x0f);
			if (extended == 922)
				Byte(emitter, 0xbf);
			else
				Byte(emitter, 0xbe);
			Byte(emitter, 0xc0);
			CpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
			Record(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

int OsxJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	if (!OsxHostFeature(OSXFEATURESSE2))
		return 0;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->fused = !OSXJITNOFUSED && OsxHostFeature(OSXFEATUREAVX2);
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
	state->code = OsxJitAllocate(OSXJITBYTES);
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->code == NULL) {
		OsxJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	EmitThunks(state);
	return 1;
}

void OsxJitDestroy(OsxCpu *cpu)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL)
		return;
	if (cpu->logfile)
		fprintf(cpu->logfile, "jit compiled %llu linked %llu instructions %llu interpreted %llu bytes %u\n", (unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)cpu->instructions, (unsigned long long)state->interpreted, state->used);
	if (state->code)
		OsxJitRelease(state->code, OSXJITBYTES);
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
	OsxNativeState *state;
	uint32 first;
	uint32 last;
	int invalidated;
#if OSXG5
	if (cpu->core970) {
		Osx970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
		return 1;
	}
#endif
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || length == 0 || address >= cpu->ramsize)
		return 0;
	if (length > cpu->ramsize - address)
		length = cpu->ramsize - address;
	first = address >> 12;
	last = (address + length - 1) >> 12;
	invalidated = 0;
	for (; first <= last; first++) {
		if (!state->compiledpages[first])
			continue;
		state->compiledpages[first] = 0;
		state->versions[first]++;
		invalidated = 1;
	}
	return invalidated;
}

static int NeedsPc(uint32 instruction)
{
	uint32 opcode;
	opcode = instruction >> 26;
	if (opcode == 7 || opcode == 10 || opcode == 11 || opcode == 14 || opcode == 15 || opcode == 20 || opcode == 21 || opcode == 23 || (opcode >= 24 && opcode <= 29))
		return 0;
	if (opcode == 31) {
		switch ((instruction >> 1) & 0x3ff) {
		case 0: case 19: case 28: case 32: case 40: case 60: case 124: case 144:
		case 235: case 266: case 284: case 316: case 412: case 444: case 476: case 922: case 954:
			return 0;
		}
	}
	return 1;
}

static void Compile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
{
	OsxNativeEmitter emitter;
	uint32 instruction;
	uint32 word;
	uint32 pendingpc;
	uint8 *start;
	uint8 *sliceshort;
	uint8 *stubs[OSXJITMISSES];
	int count;
	int limit;
	int status;
	int index;
	int shared;
	if (state->used + OSXJITRESERVE >= OSXJITBYTES) {
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
		memset(state->blocks, 0, sizeof(state->blocks));
		memset(state->front, 0, sizeof(state->front));
		state->used = state->thunkbytes;
		cpu->jitlink = NULL;
	}
	start = state->code + state->used;
	emitter.state = state;
	emitter.cursor = start;
	emitter.physical = physical;
	emitter.deterministic = cpu->deterministic != 0;
	emitter.enabled = 0;
	emitter.misscount = 0;
	Byte(&emitter, 0x53);
	Byte(&emitter, 0x56);
	Byte(&emitter, 0x57);
	Byte(&emitter, 0x55);
	Byte(&emitter, 0x83); Byte(&emitter, 0xec); Byte(&emitter, 28);
	Stack(&emitter, 0x8b, 3, 48);
	Byte(&emitter, 0x8b); Byte(&emitter, 0xbb);
	Word(&emitter, OSXJITOFFSET(slice));
	Stack(&emitter, 0x89, OSXJITSLICE, OSXJITFRAMESLICE);
	Byte(&emitter, 0x8b); Byte(&emitter, 0xb3);
	Word(&emitter, OSXJITOFFSET(pc));
	Byte(&emitter, 0x81); Byte(&emitter, 0xff);
	Word(&emitter, 0x7fffffff);
	sliceshort = Jump(&emitter, 12);
	limit = (int)((4096 - (physical & 4095)) >> 2);
	if (limit > OSXJITLIMIT)
		limit = OSXJITLIMIT;
	count = 0;
	status = 0;
	pendingpc = 0;
	while (count < limit && emitter.misscount < OSXJITMISSES - 128) {
		memcpy(&word, cpu->ram + physical + count * 4, 4);
		instruction = tswap32(word);
		if (pendingpc && NeedsPc(instruction)) {
			ImmediateOp(&emitter, 0, OSXJITPC, pendingpc);
			pendingpc = 0;
		}
		status = Emit(&emitter, instruction, count);
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		ImmediateOp(&emitter, 0, OSXJITPC, pendingpc);
	if (status == 0)
		Return(&emitter, count);
	else if (status == 1)
		Exit(&emitter, count, (physical & 4095) + (uint32)count * 4);
	for (index = 0; index < emitter.misscount; index++) {
		shared = 0;
		while (shared < index && emitter.misscounts[shared] != emitter.misscounts[index])
			shared++;
		if (shared < index) {
			stubs[index] = stubs[shared];
		} else {
			stubs[index] = emitter.cursor;
			Return(&emitter, emitter.misscounts[index]);
		}
		Patch(emitter.misses[index], stubs[index]);
	}
	Patch(sliceshort, emitter.cursor);
	Return(&emitter, 0);
	memcpy(start + OSXJITENTRY + 2, &count, 4);
	state->compiledpages[physical >> 12] = 1;
	block->physical = physical;
	block->mode = mode;
	block->version = state->versions[physical >> 12] + 1;
	block->count = count;
	state->compiled++;
	if (count == 0) {
		block->function = NULL;
		return;
	}
	block->function = (OsxNativeFunction)start;
	state->used += ((uint32)(emitter.cursor - start) + 15) & ~15u;
}

static void LinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + OSXJITENTRY;
	state->linked++;
}

static void Link(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	Patch(jump, (uint8 *)block->function + OSXJITENTRY);
	state->linked++;
}

static void InterpretRun(OsxCpu *cpu, OsxNativeState *state)
{
	uint32 word;
	uint32 instruction;
	uint32 expected;
	int executed;
	executed = 0;
	do {
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		expected = cpu->pc + 4;
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxJitRun(OsxCpu *cpu, int32 count)
{
	OsxNativeState *state;
	OsxNativeBlock *block;
	uint32 physical;
	uint32 mode;
	uint32 instruction;
	uint32 word;
	uint32 set;
	uint32 hash;
	uint32 version;
	uint32 linkkind;
	uint32 linkmode;
	int way;
	int code;
	int hot;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || cpu->tracefile) {
		OsxCpuRun(cpu, count);
		return;
	}
	hot = OSXJITHOT;
	if (cpu->jithot)
		hot = cpu->jithot;
	if (cpu->halted)
		return;
	cpu->slice = count;
	cpu->jitlink = NULL;
	linkkind = 0;
	linkmode = 0;
	while (cpu->slice > 0) {
		if (cpu->tracefile) {
			OsxCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxCpuFetchPage(cpu)) {
			cpu->jitlink = NULL;
			cpu->pc = cpu->nextpc;
			cpu->slice--;
			continue;
		}
		if (!(cpu->msr & OSXMSRSE) && cpu->fetchhost >= cpu->ram && cpu->fetchhost < cpu->ram + cpu->ramsize && !(cpu->pc & 3)) {
			physical = (uint32)(cpu->fetchhost - cpu->ram) + (cpu->pc & 4095);
			mode = cpu->msr & (OSXMSRPR | OSXMSRIR);
			hash = ((physical >> 2) * 0x9e3779b1u) ^ (mode * 0x85ebca6bu);
			version = state->versions[physical >> 12] + 1;
			block = &state->front[hash >> 18];
			if (block->physical != physical || block->mode != mode || block->version != version) {
				set = hash >> 16;
				block = &state->blocks[set * OSXJITWAYS];
				for (way = 0; way < OSXJITWAYS; way++) {
					if (block[way].physical == physical && block[way].mode == mode && block[way].version == version)
						break;
				}
				if (way == OSXJITWAYS) {
					way = state->victims[set];
					state->victims[set] = (uint8)((way + 1) & (OSXJITWAYS - 1));
					block[way].physical = physical;
					block[way].mode = mode;
					block[way].version = version;
					block[way].function = NULL;
					block[way].count = -1;
				}
				block += way;
				if (block->count < 0) {
					if (block->count > -hot)
						block->count--;
					else
						Compile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						Link(state, cpu->jitlink, block);
					else
						LinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				InterpretRun(cpu, state);
				continue;
			}
			if (block->count > 0) {
				code = block->function(cpu);
				cpu->nextpc = cpu->pc;
				if (code == OSXJITLINK || code == OSXJITDYNAMIC) {
					linkkind = (uint32)code;
					linkmode = mode;
					continue;
				}
				if (code == OSXJITCONTINUE || cpu->slice <= 0)
					continue;
			}
		}
		cpu->jitlink = NULL;
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag)
			continue;
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
	}
}

#if OSXPROFILEJIT
void OsxJitTmpStats(OsxCpu *cpu, FILE *log)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state)
		fprintf(log, "TMPJIT used %u compiled %llu linked %llu interpreted %llu\n", state->used,
			(unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)state->interpreted);
}
#endif
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

							ARM64 OSX JIT

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#elif OSXHOSTARM64

#if defined(__APPLE__)
#include "../kpx_cpu/src/cpu/jit/jit-wx.hpp"
#endif

#define OSXJITSETS 65536
#define OSXJITWAYS 4
#define OSXJITBLOCKS (OSXJITSETS * OSXJITWAYS)
#define OSXJITFRONT 16384
#define OSXJITRECORDBITS 18
#define OSXJITRECORDS (1 << OSXJITRECORDBITS)
#define OSXJITBYTES (64 * 1024 * 1024)
#define OSXJITLIMIT 32
#define OSXJITRESERVE 65536
#define OSXJITMISSES 1024
#define OSXJITHOT 3
#define OSXJITCONTINUE 0
#define OSXJITINTERPRET 1
#define OSXJITLINK 2
#define OSXJITDYNAMIC 3
#define OSXJITCPU 19
#define OSXJITPC 20
#define OSXJITSLICE 21
#define OSXJITENTRYSLICE 22
#define OSXJITUPDATE 23
#define OSXJITSCRATCH 16
#define OSXJITZERO 31
#define OSXCONDEQ 0
#define OSXCONDNE 1
#define OSXCONDHS 2
#define OSXCONDLO 3
#define OSXCONDMI 4
#define OSXCONDVS 6
#define OSXCONDHI 8
#define OSXCONDLT 11
#define OSXCONDGT 12
#define OSXA64ADD 0x11000000u
#define OSXA64ADDS 0x31000000u
#define OSXA64SUB 0x51000000u
#define OSXA64SUBS 0x71000000u
#define OSXA64AND 0x12000000u
#define OSXA64ORR 0x32000000u
#define OSXA64EOR 0x52000000u
#define OSXA64ANDS 0x72000000u
#define OSXA64LDRW 0xb9400000u
#define OSXA64STRW 0xb9000000u
#define OSXA64LDRX 0xf9400000u
#define OSXA64STRX 0xf9000000u
#define OSXA64LDRD 0xfd400000u
#define OSXA64STRD 0xfd000000u
#define OSXA64B 0x14000000u
#define OSXA64BL 0x94000000u
#define OSXA64BCOND 0x54000000u
#define OSXA64CBZ 0x34000000u
#define OSXA64CBNZ 0x35000000u
#define OSXA64TBZ 0x36000000u
#define OSXA64TBNZ 0x37000000u
#define OSXA64RET 0xd65f03c0u
#define OSXJITOFFSET(member) ((uint32)offsetof(OsxCpu, member))
#define OSXJITGPR(reg) (OSXJITOFFSET(gpr) + (uint32)(reg) * 4)
#define OSXJITFPR(reg) (OSXJITOFFSET(fpr) + (uint32)(reg) * 8)
#define OSXJITRECORD(member) ((uint32)offsetof(OsxNativeRecord, member))

typedef int (*OsxNativeFunction)(OsxCpu *cpu);

typedef struct OsxNativeRecord {
	uint32 address;
	uint32 version;
	uint8 *host;
	uint32 *versionaddress;
	uint8 *target;
} OsxNativeRecord;

typedef struct OsxNativeBlock {
	uint32 physical;
	uint32 mode;
	uint32 version;
	int count;
	OsxNativeFunction function;
} OsxNativeBlock;

typedef struct OsxNativeState {
	uint8 *writebase;
	uint8 *executebase;
	uint32 used;
	uint32 entry;
	uint32 *versions;
	uint8 *compiledpages;
	uint32 pagecount;
	OsxNativeRecord *records;
	uint8 *checkthunk;
	uint8 *missthunk;
	uint8 *finishthunk;
	uint8 *interpretthunk;
	uint8 *timethunk;
	uint8 *linkcallthunk;
	uint8 *dynamicthunk;
	uint8 *translatethunks[2][6];
	uint32 thunkbytes;
	uint64 compiled;
	uint64 linked;
	uint64 interpreted;
	uint8 victims[OSXJITSETS];
	OsxNativeBlock front[OSXJITFRONT];
	OsxNativeBlock blocks[OSXJITBLOCKS];
} OsxNativeState;

typedef struct OsxNativeEmitter {
	OsxNativeState *state;
	uint8 *cursor;
	uint32 physical;
	int deterministic;
	uint32 enabled;
	uint8 *misses[OSXJITMISSES];
	int misscounts[OSXJITMISSES];
	int misscount;
} OsxNativeEmitter;

static void Op(OsxNativeEmitter *emitter, uint32 word)
{
	memcpy(emitter->cursor, &word, 4);
	emitter->cursor += 4;
}

static uint8 *Here(OsxNativeEmitter *emitter)
{
	return emitter->cursor - (emitter->state->writebase - emitter->state->executebase);
}

static void Publish(OsxNativeState *state, uint8 *execute, uint32 length)
{
#if defined(__APPLE__)
	jit_wx_publish(execute, execute + (state->writebase - state->executebase), length);
#else
	OsxJitFlush(execute, length);
#endif
}

static void BranchTo(OsxNativeEmitter *emitter, uint32 opcode, uint8 *target)
{
	Op(emitter, opcode | ((uint32)((target - Here(emitter)) / 4) & 0x03ffffff));
}

static uint8 *Forward(OsxNativeEmitter *emitter, uint32 word)
{
	uint8 *site;
	site = emitter->cursor;
	Op(emitter, word);
	return site;
}

static void Patch(uint8 *site, uint8 *target)
{
	uint32 word;
	uint32 distance;
	memcpy(&word, site, 4);
	distance = (uint32)((target - site) / 4);
	if ((word & 0x7c000000u) == OSXA64B)
		word = (word & 0xfc000000u) | (distance & 0x03ffffff);
	else if ((word & 0x7e000000u) == OSXA64TBZ)
		word = (word & 0xfff8001fu) | ((distance & 0x3fff) << 5);
	else
		word = (word & 0xff00001fu) | ((distance & 0x7ffff) << 5);
	memcpy(site, &word, 4);
}

static void Miss(OsxNativeEmitter *emitter, uint32 word, int count)
{
	emitter->misses[emitter->misscount] = Forward(emitter, word);
	emitter->misscounts[emitter->misscount++] = count;
}

static uint32 Conditional(int condition)
{
	return OSXA64BCOND | (uint32)condition;
}

static uint32 BitTest(uint32 opcode, int reg, uint32 bit)
{
	return opcode | ((bit & 31) << 19) | ((bit >> 5) << 31) | (uint32)reg;
}

static void Reg3(OsxNativeEmitter *emitter, uint32 opcode, int target, int left, int right)
{
	Op(emitter, opcode | ((uint32)right << 16) | ((uint32)left << 5) | (uint32)target);
}

static void Move(OsxNativeEmitter *emitter, int target, int source)
{
	Reg3(emitter, 0x2a000000u, target, OSXJITZERO, source);
}

static void Constant(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	if ((value & 0xffff0000u) == 0)
		Op(emitter, 0x52800000u | (value << 5) | (uint32)reg);
	else if ((value & 0xffff) == 0)
		Op(emitter, 0x52a00000u | ((value >> 16) << 5) | (uint32)reg);
	else if ((~value & 0xffff0000u) == 0)
		Op(emitter, 0x12800000u | ((~value & 0xffff) << 5) | (uint32)reg);
	else if ((~value & 0xffff) == 0)
		Op(emitter, 0x12a00000u | ((~value >> 16) << 5) | (uint32)reg);
	else {
		Op(emitter, 0x52800000u | ((value & 0xffff) << 5) | (uint32)reg);
		Op(emitter, 0x72a00000u | ((value >> 16) << 5) | (uint32)reg);
	}
}

static void Constant64(OsxNativeEmitter *emitter, int reg, uint64 value)
{
	uint32 part;
	int half;
	Op(emitter, 0xd2800000u | ((uint32)(value & 0xffff) << 5) | (uint32)reg);
	for (half = 1; half < 4; half++) {
		part = (uint32)(value >> (half * 16)) & 0xffff;
		if (part)
			Op(emitter, 0xf2800000u | ((uint32)half << 21) | (part << 5) | (uint32)reg);
	}
}

static void AddImmediate(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 value)
{
	if (value < 4096)
		Op(emitter, opcode | (value << 10) | ((uint32)source << 5) | (uint32)target);
	else if (0u - value < 4096)
		Op(emitter, (opcode ^ 0x40000000u) | ((0u - value) << 10) | ((uint32)source << 5) | (uint32)target);
	else if ((value & 0xfff) == 0 && value < 0x1000000)
		Op(emitter, opcode | 0x400000u | ((value >> 12) << 10) | ((uint32)source << 5) | (uint32)target);
	else {
		Constant(emitter, OSXJITSCRATCH, value);
		Reg3(emitter, opcode - 0x06000000u, target, source, OSXJITSCRATCH);
	}
}

static int Bitmask(uint32 value, uint32 *encoded)
{
	uint32 size;
	uint32 mask;
	uint32 element;
	uint32 ones;
	uint32 pattern;
	uint32 rotation;
	uint32 rotated;
	if (value == 0 || value == 0xffffffffu)
		return 0;
	size = 32;
	while (size > 2) {
		mask = (1u << (size / 2)) - 1;
		if ((value & mask) != ((value >> (size / 2)) & mask))
			break;
		size /= 2;
	}
	mask = 0xffffffffu;
	if (size < 32)
		mask = (1u << size) - 1;
	element = value & mask;
	ones = 0;
	for (rotation = 0; rotation < size; rotation++)
		ones += (element >> rotation) & 1;
	pattern = (1u << ones) - 1;
	for (rotation = 0; rotation < size; rotation++) {
		rotated = pattern;
		if (rotation)
			rotated = ((pattern >> rotation) | (pattern << (size - rotation))) & mask;
		if (rotated == element) {
			*encoded = (rotation << 6) | (((~(size - 1) << 1) | (ones - 1)) & 0x3f);
			return 1;
		}
	}
	return 0;
}

static void LogicImmediate(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 value)
{
	uint32 encoded;
	if (Bitmask(value, &encoded)) {
		Op(emitter, opcode | (encoded << 10) | ((uint32)source << 5) | (uint32)target);
		return;
	}
	Constant(emitter, OSXJITSCRATCH, value);
	Reg3(emitter, opcode - 0x08000000u, target, source, OSXJITSCRATCH);
}

static void Bitfield(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 rotate, uint32 last)
{
	Op(emitter, opcode | (rotate << 16) | (last << 10) | ((uint32)source << 5) | (uint32)target);
}

static void ShiftLeft(OsxNativeEmitter *emitter, int target, int source, uint32 amount)
{
	Bitfield(emitter, 0x53000000u, target, source, (32 - amount) & 31, 31 - amount);
}

static void ShiftRight(OsxNativeEmitter *emitter, int target, int source, uint32 amount)
{
	Bitfield(emitter, 0x53000000u, target, source, amount, 31);
}

static void Extract(OsxNativeEmitter *emitter, int target, int source, uint32 first, uint32 width)
{
	Bitfield(emitter, 0x53000000u, target, source, first, first + width - 1);
}

static void Insert(OsxNativeEmitter *emitter, int target, int source, uint32 first, uint32 width)
{
	Bitfield(emitter, 0x33000000u, target, source, (32 - first) & 31, width - 1);
}

static void Set(OsxNativeEmitter *emitter, int reg, int condition)
{
	Op(emitter, 0x1a9f07e0u | ((uint32)(condition ^ 1) << 12) | (uint32)reg);
}

static void Field(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	uint32 scale;
	scale = 4;
	if (opcode & 0x40000000u)
		scale = 8;
	if (offset % scale == 0 && offset / scale < 4096) {
		Op(emitter, opcode | ((offset / scale) << 10) | (OSXJITCPU << 5) | (uint32)reg);
		return;
	}
	Constant(emitter, OSXJITSCRATCH, offset);
	Reg3(emitter, 0x8b000000u, OSXJITSCRATCH, OSXJITCPU, OSXJITSCRATCH);
	Op(emitter, opcode | (OSXJITSCRATCH << 5) | (uint32)reg);
}

static void Load(OsxNativeEmitter *emitter, int reg, uint32 offset)
{
	Field(emitter, OSXA64LDRW, reg, offset);
}

static void Store(OsxNativeEmitter *emitter, int reg, uint32 offset)
{
	Field(emitter, OSXA64STRW, reg, offset);
}

static void CallFar(OsxNativeEmitter *emitter, void *function)
{
	Constant64(emitter, OSXJITSCRATCH, (uint64)(uintptr_t)function);
	Op(emitter, 0xd63f0000u | (OSXJITSCRATCH << 5));
}

static void Charge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	AddImmediate(emitter, OSXA64SUBS, OSXJITSLICE, OSXJITSLICE, (uint32)count);
}

static void Settle(OsxNativeEmitter *emitter)
{
	Store(emitter, OSXJITPC, OSXJITOFFSET(pc));
	Reg3(emitter, 0x4b000000u, 0, OSXJITENTRYSLICE, OSXJITSLICE);
	Field(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	Reg3(emitter, 0x8b204000u, 1, 1, 0);
	Field(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
}

static void Release(OsxNativeEmitter *emitter, uint32 code)
{
	Constant(emitter, 0, code);
	Op(emitter, 0xa94363f7u);
	Op(emitter, 0xa9425bf5u);
	Op(emitter, 0xa94153f3u);
	Op(emitter, 0xa8c47bfdu);
	Op(emitter, OSXA64RET);
}

static void Leave(OsxNativeEmitter *emitter, uint32 code)
{
	Settle(emitter);
	Store(emitter, OSXJITSLICE, OSXJITOFFSET(slice));
	Release(emitter, code);
}

static void Return(OsxNativeEmitter *emitter, int count)
{
	Charge(emitter, count);
	BranchTo(emitter, OSXA64B, emitter->state->interpretthunk);
}

static void DynamicExit(OsxNativeEmitter *emitter, int count)
{
	Charge(emitter, count);
	Op(emitter, Conditional(OSXCONDGT) | (2 << 5));
	BranchTo(emitter, OSXA64B, emitter->state->finishthunk);
	BranchTo(emitter, OSXA64B, emitter->state->dynamicthunk);
}

static void TranslateThunk(OsxNativeEmitter *emitter, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	retry = emitter->cursor;
	AddImmediate(emitter, OSXA64ADD, 1, 0, 0xfff - limit);
	LogicImmediate(emitter, OSXA64AND, 1, 1, 0xfffff000u);
	Extract(emitter, 2, 0, 12, OSXTLBBITS);
	if (store)
		Field(emitter, OSXA64LDRX, 3, OSXJITOFFSET(writetlb));
	else
		Field(emitter, OSXA64LDRX, 3, OSXJITOFFSET(readtlb));
	Reg3(emitter, 0x8b001000u, 3, 3, 2);
	Op(emitter, 0xb9400062u);
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	slow = Forward(emitter, Conditional(OSXCONDNE));
	ShiftRight(emitter, 1, 0, 28);
	Field(emitter, OSXA64LDRX, 2, OSXJITOFFSET(datasr));
	Op(emitter, 0xb8617841u);
	Op(emitter, 0xb9400462u);
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	segment = Forward(emitter, Conditional(OSXCONDNE));
	Op(emitter, 0xf9400461u);
	LogicImmediate(emitter, OSXA64AND, 2, 0, 0xfff);
	Reg3(emitter, 0x8b000000u, 1, 1, 2);
	Op(emitter, OSXA64RET);
	Patch(slow, emitter->cursor);
	LogicImmediate(emitter, OSXA64AND, 1, 0, 0xfffff000u);
	Op(emitter, 0xb9400062u);
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	page = Forward(emitter, Conditional(OSXCONDNE));
	Op(emitter, 0xd2800001u);
	Op(emitter, OSXA64RET);
	Patch(page, emitter->cursor);
	Patch(segment, emitter->cursor);
	Op(emitter, 0xa9bf03feu);
	Move(emitter, 1, 0);
	Reg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	if (store)
		Constant(emitter, 2, OSXTLBWRITE);
	else
		Constant(emitter, 2, OSXTLBREAD);
	CallFar(emitter, (void *)OsxCpuFillTlb);
	Move(emitter, 3, 0);
	Op(emitter, 0xa8c103feu);
	Op(emitter, OSXA64CBNZ | ((uint32)((retry - emitter->cursor) / 4 & 0x7ffff) << 5) | 3);
	Op(emitter, 0xd2800001u);
	Op(emitter, OSXA64RET);
}

static void EmitThunks(OsxNativeState *state)
{
	static const uint32 translatelimits[6] = { 0xfff, 0xffe, 0xffc, 0xff8, 0xff0, 0xfe0 };
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
	emitter.state = state;
	emitter.cursor = state->writebase;
	emitter.misscount = 0;
	state->checkthunk = Here(&emitter);
	Op(&emitter, 0xb9400000u | (OSXJITRECORD(address) / 4) << 10 | (OSXJITUPDATE << 5));
	Reg3(&emitter, 0x6b000000u, OSXJITZERO, 0, OSXJITPC);
	misses[0] = Forward(&emitter, Conditional(OSXCONDNE));
	Field(&emitter, OSXA64LDRX, 0, OSXJITOFFSET(fetchtlb));
	Extract(&emitter, 1, OSXJITPC, 12, OSXTLBBITS);
	Reg3(&emitter, 0x8b001000u, 0, 0, 1);
	Op(&emitter, 0xb9400002u);
	LogicImmediate(&emitter, OSXA64AND, 1, OSXJITPC, 0xfffff000u);
	Reg3(&emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	misses[1] = Forward(&emitter, Conditional(OSXCONDNE));
	ShiftRight(&emitter, 1, OSXJITPC, 28);
	Field(&emitter, OSXA64LDRX, 2, OSXJITOFFSET(fetchsr));
	Op(&emitter, 0xb8617841u);
	Op(&emitter, 0xb9400402u);
	Reg3(&emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	misses[2] = Forward(&emitter, Conditional(OSXCONDNE));
	Op(&emitter, 0xf9400400u);
	Op(&emitter, 0xf9400001u | (OSXJITRECORD(host) / 8) << 10 | (OSXJITUPDATE << 5));
	Reg3(&emitter, 0xeb000000u, OSXJITZERO, 0, 1);
	misses[3] = Forward(&emitter, Conditional(OSXCONDNE));
	Op(&emitter, 0xf9400001u | (OSXJITRECORD(versionaddress) / 8) << 10 | (OSXJITUPDATE << 5));
	Op(&emitter, 0xb9400021u);
	Op(&emitter, 0xb9400002u | (OSXJITRECORD(version) / 4) << 10 | (OSXJITUPDATE << 5));
	Reg3(&emitter, 0x6b000000u, OSXJITZERO, 1, 2);
	misses[4] = Forward(&emitter, Conditional(OSXCONDNE));
	Field(&emitter, OSXA64STRX, 0, OSXJITOFFSET(fetchhost));
	LogicImmediate(&emitter, OSXA64AND, 1, OSXJITPC, 0xfffff000u);
	LogicImmediate(&emitter, OSXA64ORR, 1, 1, 1);
	Store(&emitter, 1, OSXJITOFFSET(fetchtag));
	Constant(&emitter, 0, 0);
	Op(&emitter, OSXA64RET);
	for (index = 0; index < 5; index++)
		Patch(misses[index], emitter.cursor);
	Constant(&emitter, 0, 1);
	Op(&emitter, OSXA64RET);
	state->missthunk = Here(&emitter);
	Field(&emitter, OSXA64STRX, OSXJITUPDATE, OSXJITOFFSET(jitlink));
	Leave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = Here(&emitter);
	Leave(&emitter, OSXJITCONTINUE);
	state->interpretthunk = Here(&emitter);
	Leave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = Here(&emitter);
	Op(&emitter, 0xd10023c0u);
	Field(&emitter, OSXA64STRX, 0, OSXJITOFFSET(jitlink));
	Leave(&emitter, OSXJITLINK);
	state->dynamicthunk = Here(&emitter);
	Extract(&emitter, OSXJITUPDATE, OSXJITPC, 2, OSXJITRECORDBITS);
	Constant64(&emitter, OSXJITSCRATCH, (uint64)(uintptr_t)state->records);
	Op(&emitter, 0x8b001400u | (OSXJITUPDATE << 16) | (OSXJITSCRATCH << 5) | OSXJITUPDATE);
	BranchTo(&emitter, OSXA64BL, state->checkthunk);
	Op(&emitter, OSXA64CBNZ | ((uint32)((state->missthunk - Here(&emitter)) / 4 & 0x7ffff) << 5));
	Op(&emitter, 0xf9400000u | (OSXJITRECORD(target) / 8) << 10 | (OSXJITUPDATE << 5) | OSXJITSCRATCH);
	Op(&emitter, 0xd61f0000u | (OSXJITSCRATCH << 5));
	state->timethunk = Here(&emitter);
	Op(&emitter, 0xa9bf7bfdu);
	Reg3(&emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	CallFar(&emitter, (void *)OsxCpuTimebase);
	Op(&emitter, 0xa8c17bfdu);
	Op(&emitter, OSXA64RET);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = Here(&emitter);
		TranslateThunk(&emitter, index / 6, translatelimits[index % 6]);
	}
	state->thunkbytes = ((uint32)(emitter.cursor - state->writebase) + 63) & ~63u;
	state->used = state->thunkbytes;
	Publish(state, state->executebase, state->thunkbytes);
}

static void Exit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		DynamicExit(emitter, count);
		return;
	}
	Charge(emitter, count);
	Op(emitter, Conditional(OSXCONDGT) | (2 << 5));
	BranchTo(emitter, OSXA64B, emitter->state->finishthunk);
	Op(emitter, OSXA64B | 1);
	BranchTo(emitter, OSXA64BL, emitter->state->linkcallthunk);
}

static void Combine(OsxNativeEmitter *emitter)
{
	Op(emitter, 0x2a000420u);
	Op(emitter, 0x2a000440u);
	ShiftLeft(emitter, 0, 0, 1);
}

static void Condition(OsxNativeEmitter *emitter, int field, int issigned)
{
	if (issigned) {
		Set(emitter, 0, OSXCONDLT);
		Set(emitter, 1, OSXCONDGT);
	} else {
		Set(emitter, 0, OSXCONDLO);
		Set(emitter, 1, OSXCONDHI);
	}
	Set(emitter, 2, OSXCONDEQ);
	Combine(emitter);
	Load(emitter, 1, OSXJITOFFSET(xer));
	Op(emitter, 0x2a417c00u);
	Load(emitter, 1, OSXJITOFFSET(cr));
	Insert(emitter, 1, 0, (uint32)(7 - field) * 4, 4);
	Store(emitter, 1, OSXJITOFFSET(cr));
}

static void Record(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
		Condition(emitter, 0, 1);
	}
}

static void RequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	Load(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	Miss(emitter, BitTest(OSXA64TBZ, OSXJITSCRATCH, 13), count);
}

static void RecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		Load(emitter, 0, OSXJITOFFSET(fpscr));
		Load(emitter, 1, OSXJITOFFSET(cr));
		Extract(emitter, 0, 0, 28, 4);
		Insert(emitter, 1, 0, 24, 4);
		Store(emitter, 1, OSXJITOFFSET(cr));
	}
}

static void FloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	Field(emitter, OSXA64LDRX, 0, OSXJITFPR(reg));
	Op(emitter, 0xd374f802u);
	AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 0x7ff);
	Miss(emitter, Conditional(OSXCONDEQ), count);
	if (normal)
		Miss(emitter, OSXA64CBZ | 2, count);
}

static void FloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	Field(emitter, OSXA64LDRD, vector, OSXJITFPR(reg));
}

static void FloatRounding(OsxNativeEmitter *emitter, int count)
{
	Load(emitter, OSXJITSCRATCH, OSXJITOFFSET(fpscr));
	LogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, OSXJITSCRATCH, 3);
	Miss(emitter, Conditional(OSXCONDNE), count);
}

static void FloatNarrow(OsxNativeEmitter *emitter)
{
	Op(emitter, 0x1e624000u);
	Op(emitter, 0x1e22c000u);
}

static void FloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	Op(emitter, 0x9e660000u);
	Op(emitter, 0xd374f802u);
	AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 0x7ff);
	Miss(emitter, Conditional(OSXCONDEQ), count);
	zero = Forward(emitter, OSXA64CBZ | 2);
	if (single) {
		AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 897);
		Miss(emitter, Conditional(OSXCONDLO), count);
	}
	Op(emitter, 0xd37ffc02u);
	AddImmediate(emitter, OSXA64ADD, 2, 2, 1);
	ShiftLeft(emitter, 2, 2, 14);
	merge = Forward(emitter, OSXA64B);
	Patch(zero, emitter->cursor);
	Op(emitter, 0xd37ff803u);
	Miss(emitter, 0xb5000003u, count);
	Op(emitter, 0xd37ffc02u);
	ShiftLeft(emitter, 2, 2, 16);
	LogicImmediate(emitter, OSXA64ORR, 2, 2, 0x2000);
	Patch(merge, emitter->cursor);
	Field(emitter, OSXA64STRD, 0, OSXJITFPR(target));
	Load(emitter, 0, OSXJITOFFSET(fpscr));
	LogicImmediate(emitter, OSXA64AND, 0, 0, ~0x1f000u);
	Reg3(emitter, 0x2a000000u, 0, 0, 2);
	Store(emitter, 0, OSXJITOFFSET(fpscr));
}

static void FloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	FloatExponent(emitter, left, count, 0);
	FloatExponent(emitter, right, count, 0);
	FloatLoad(emitter, 0, left);
	FloatLoad(emitter, 1, right);
	Op(emitter, 0x1e612000u);
	Set(emitter, 0, OSXCONDMI);
	Set(emitter, 1, OSXCONDGT);
	Set(emitter, 2, OSXCONDEQ);
	Combine(emitter);
	Load(emitter, 1, OSXJITOFFSET(fpscr));
	Insert(emitter, 1, 0, 12, 4);
	Store(emitter, 1, OSXJITOFFSET(fpscr));
	Load(emitter, 1, OSXJITOFFSET(cr));
	Insert(emitter, 1, 0, (uint32)(7 - field) * 4, 4);
	Store(emitter, 1, OSXJITOFFSET(cr));
}

static void FloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	FloatLoad(emitter, 0, right);
	Op(emitter, 0x1e602000u);
	Miss(emitter, Conditional(OSXCONDVS), count);
	Op(emitter, 0x1e780000u);
	Constant(emitter, 1, 0x7fffffffu);
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
	Miss(emitter, Conditional(OSXCONDEQ), count);
	Constant(emitter, 1, 0x80000000u);
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
	Miss(emitter, Conditional(OSXCONDEQ), count);
	Op(emitter, 0x1e620001u);
	Op(emitter, 0x1e602020u);
	exact = Forward(emitter, Conditional(OSXCONDEQ));
	Load(emitter, 1, OSXJITOFFSET(fpscr));
	LogicImmediate(emitter, OSXA64ORR, 1, 1, 0x82000000u);
	Store(emitter, 1, OSXJITOFFSET(fpscr));
	Patch(exact, emitter->cursor);
	Store(emitter, 0, OSXJITFPR(target));
	Constant(emitter, 1, 0xfff80000u);
	Store(emitter, 1, OSXJITFPR(target) + 4);
}

static int Float(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 extended;
	int target;
	int left;
	int right;
	int multiplier;
	int single;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	single = (instruction >> 26) == 59;
	if (!single && !(instruction & 0x20)) {
		switch (extended) {
		case 40: case 72: case 136: case 264:
			RequireFloat(emitter, count);
			FloatLoad(emitter, 0, right);
			if (extended == 136 || extended == 264)
				Op(emitter, 0x1e60c000u);
			if (extended == 40 || extended == 136)
				Op(emitter, 0x1e614000u);
			Field(emitter, OSXA64STRD, 0, OSXJITFPR(target));
			break;
		case 0: case 32:
			RequireFloat(emitter, count);
			FloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			RequireFloat(emitter, count);
			FloatRounding(emitter, count);
			FloatExponent(emitter, right, count, 0);
			FloatLoad(emitter, 0, right);
			FloatNarrow(emitter);
			FloatClass(emitter, target, count, 1);
			break;
		case 15:
			RequireFloat(emitter, count);
			FloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		RecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		RequireFloat(emitter, count);
		FloatRounding(emitter, count);
		FloatExponent(emitter, left, count, 0);
		FloatExponent(emitter, multiplier, count, 0);
		FloatExponent(emitter, right, count, 0);
		FloatLoad(emitter, 0, left);
		FloatLoad(emitter, 1, multiplier);
		FloatLoad(emitter, 2, right);
		if (extended == 28 || extended == 30)
			Op(emitter, 0x1f618800u);
		else
			Op(emitter, 0x1f410800u);
		if (extended >= 30)
			Op(emitter, 0x1e614000u);
		if (single)
			FloatNarrow(emitter);
		FloatClass(emitter, target, count, single);
		RecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	RequireFloat(emitter, count);
	FloatRounding(emitter, count);
	FloatExponent(emitter, left, count, 0);
	FloatExponent(emitter, right, count, extended == 18);
	FloatLoad(emitter, 0, left);
	FloatLoad(emitter, 1, right);
	if (extended == 18)
		Op(emitter, 0x1e611800u);
	else if (extended == 20)
		Op(emitter, 0x1e613800u);
	else if (extended == 21)
		Op(emitter, 0x1e612800u);
	else
		Op(emitter, 0x1e610800u);
	if (single)
		FloatNarrow(emitter);
	FloatClass(emitter, target, count, single);
	RecordFloat(emitter, instruction);
	return 1;
}

static uint32 RotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void Address(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		Load(emitter, 0, OSXJITGPR(base));
	else
		Constant(emitter, 0, 0);
	if (index >= 0) {
		Load(emitter, 1, OSXJITGPR(index));
		Reg3(emitter, 0x0b000000u, 0, 0, 1);
	} else if (immediate) {
		AddImmediate(emitter, OSXA64ADD, 0, 0, immediate);
	}
	if (update)
		Move(emitter, OSXJITUPDATE, 0);
}

static int LimitIndex(uint32 limit)
{
	switch (limit) {
	case 0xfff: return 0;
	case 0xffe: return 1;
	case 0xffc: return 2;
	case 0xff8: return 3;
	case 0xff0: return 4;
	}
	return 5;
}

static void Translate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	BranchTo(emitter, OSXA64BL, emitter->state->translatethunks[store != 0][LimitIndex(limit)]);
	Miss(emitter, 0xb4000001u, count);
}

static int Multiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
{
	int first;
	int base;
	uint32 registers;
	uint8 *loop;
	first = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	registers = (uint32)(32 - first);
	if (opcode == 46 && base >= first)
		return 0;
	Address(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	LogicImmediate(emitter, OSXA64AND, 1, 0, 0xfff);
	AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 1, 0x1000 - registers * 4);
	Miss(emitter, Conditional(OSXCONDHI), count);
	Translate(emitter, opcode == 47, 0xffc, count);
	Constant(emitter, OSXJITSCRATCH, OSXJITGPR(first));
	Reg3(emitter, 0x8b000000u, 2, OSXJITCPU, OSXJITSCRATCH);
	Constant(emitter, 3, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		Op(emitter, 0xb8404424u);
		Op(emitter, 0x5ac00884u);
		Op(emitter, 0xb8004444u);
	} else {
		Op(emitter, 0xb8404444u);
		Op(emitter, 0x5ac00884u);
		Op(emitter, 0xb8004424u);
	}
	AddImmediate(emitter, OSXA64SUBS, 3, 3, 1);
	Op(emitter, Conditional(OSXCONDNE) | (((uint32)((loop - emitter->cursor) / 4) & 0x7ffff) << 5));
	return 1;
}

static void CallHelper(OsxNativeEmitter *emitter, void *function)
{
	Store(emitter, OSXJITPC, OSXJITOFFSET(pc));
	CallFar(emitter, function);
	Load(emitter, OSXJITPC, OSXJITOFFSET(pc));
}

static void VectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	Load(emitter, OSXJITSCRATCH, OSXJITOFFSET(altivec));
	Miss(emitter, OSXA64CBZ | OSXJITSCRATCH, count);
	Load(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	Miss(emitter, BitTest(OSXA64TBZ, OSXJITSCRATCH, 25), count);
}

static void VectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	VectorEnabled(emitter, count);
	Reg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	Constant(emitter, 1, instruction);
	CallHelper(emitter, (void *)function);
}

static void Memory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
{
	int target;
	int base;
	int update;
	int store;
	int wide;
	int single;
	int reversed;
	uint32 limit;
	uint32 immediate;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	reversed = opcode >= 64;
	update = opcode & 1;
	opcode &= 62;
	store = opcode == 36 || opcode == 38 || opcode == 44 || opcode == 52 || opcode == 54;
	wide = opcode == 50 || opcode == 54;
	single = opcode == 48 || opcode == 52;
	if (wide || single)
		RequireFloat(emitter, count);
	if (single && store) {
		Field(emitter, OSXA64LDRX, 0, OSXJITFPR(target));
		CallHelper(emitter, (void *)OsxCpuDoubleToSingle);
		Store(emitter, 0, OSXJITOFFSET(jitscratch));
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	Address(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		Move(emitter, OSXJITUPDATE, 0);
	Translate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			Field(emitter, OSXA64LDRX, 2, OSXJITFPR(target));
			Op(emitter, 0xdac00c42u);
			Op(emitter, 0xf9000022u);
		} else {
			if (single)
				Load(emitter, 2, OSXJITOFFSET(jitscratch));
			else
				Load(emitter, 2, OSXJITGPR(target));
			if (opcode == 36 || opcode == 52) {
				if (!reversed)
					Op(emitter, 0x5ac00842u);
				Op(emitter, 0xb9000022u);
			} else if (opcode == 44) {
				Op(emitter, 0x5ac00442u);
				Op(emitter, 0x79000022u);
			} else {
				Op(emitter, 0x39000022u);
			}
		}
	} else {
		if (wide) {
			Op(emitter, 0xf9400020u);
			Op(emitter, 0xdac00c00u);
			Field(emitter, OSXA64STRX, 0, OSXJITFPR(target));
		} else if (opcode == 32 || single) {
			Op(emitter, 0xb9400020u);
			if (!reversed)
				Op(emitter, 0x5ac00800u);
		} else if (opcode == 34) {
			Op(emitter, 0x39400020u);
		} else {
			Op(emitter, 0x79400020u);
			Op(emitter, 0x5ac00400u);
			if (opcode == 42)
				Bitfield(emitter, 0x13000000u, 0, 0, 0, 15);
		}
		if (single) {
			CallHelper(emitter, (void *)OsxCpuSingleToDouble);
			Field(emitter, OSXA64STRX, 0, OSXJITFPR(target));
		} else if (!wide) {
			Store(emitter, 0, OSXJITGPR(target));
		}
	}
	if (update)
		Store(emitter, OSXJITUPDATE, OSXJITGPR(base));
}

static int Branch(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 options;
	uint32 displacement;
	uint32 extended;
	uint8 *counterpatch;
	uint8 *conditionpatch;
	uint32 offset;
	uint32 targetoffset;
	opcode = instruction >> 26;
	options = (instruction >> 21) & 31;
	counterpatch = NULL;
	conditionpatch = NULL;
	offset = (emitter->physical & 4095) + (uint32)count * 4;
	targetoffset = 4096;
	if (opcode == 19) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 528) {
			Load(emitter, 1, OSXJITOFFSET(ctr));
			options |= 4;
		} else {
			Load(emitter, 1, OSXJITOFFSET(lr));
		}
		LogicImmediate(emitter, OSXA64AND, 1, 1, ~3u);
	} else {
		if (opcode == 18) {
			displacement = instruction & 0x03fffffc;
			if (displacement & 0x02000000)
				displacement |= 0xfc000000;
			options = 20;
		} else {
			displacement = (uint32)(int32)(int16)(instruction & 0xfffc);
		}
		if (instruction & 2) {
			Constant(emitter, 1, displacement);
		} else {
			AddImmediate(emitter, OSXA64ADD, 1, OSXJITPC, displacement);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		AddImmediate(emitter, OSXA64ADD, 2, OSXJITPC, 4);
		Store(emitter, 2, OSXJITOFFSET(lr));
	}
	if (!(options & 4)) {
		Load(emitter, 0, OSXJITOFFSET(ctr));
		AddImmediate(emitter, OSXA64SUBS, 0, 0, 1);
		Store(emitter, 0, OSXJITOFFSET(ctr));
		if (options & 2)
			counterpatch = Forward(emitter, Conditional(OSXCONDNE));
		else
			counterpatch = Forward(emitter, Conditional(OSXCONDEQ));
	}
	if (!(options & 16)) {
		Load(emitter, 2, OSXJITOFFSET(cr));
		if (options & 8)
			conditionpatch = Forward(emitter, BitTest(OSXA64TBZ, 2, 31 - ((instruction >> 16) & 31)));
		else
			conditionpatch = Forward(emitter, BitTest(OSXA64TBNZ, 2, 31 - ((instruction >> 16) & 31)));
	}
	Move(emitter, OSXJITPC, 1);
	Exit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			Patch(counterpatch, emitter->cursor);
		if (conditionpatch)
			Patch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void LoadCarry(OsxNativeEmitter *emitter)
{
	Load(emitter, 1, OSXJITOFFSET(xer));
	Extract(emitter, 1, 1, 29, 1);
	AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 1, 1);
}

static void StoreCarry(OsxNativeEmitter *emitter)
{
	Load(emitter, 1, OSXJITOFFSET(xer));
	Insert(emitter, 1, 2, 29, 1);
	Store(emitter, 1, OSXJITOFFSET(xer));
}

static void RequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	Load(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	Miss(emitter, BitTest(OSXA64TBNZ, OSXJITSCRATCH, 14), count);
}

static int Carrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		Load(emitter, 0, OSXJITGPR(left));
		Load(emitter, 1, OSXJITGPR(right));
		Reg3(emitter, 0x2b000000u, 0, 0, 1);
		break;
	case 8:
		Load(emitter, 0, OSXJITGPR(right));
		Load(emitter, 1, OSXJITGPR(left));
		Reg3(emitter, 0x6b000000u, 0, 0, 1);
		break;
	case 138:
	case 136:
		Load(emitter, 0, OSXJITGPR(left));
		if (extended == 136)
			Reg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
		LoadCarry(emitter);
		Load(emitter, 2, OSXJITGPR(right));
		Reg3(emitter, 0x3a000000u, 0, 0, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		Load(emitter, 0, OSXJITGPR(left));
		if (extended == 200 || extended == 232)
			Reg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
		LoadCarry(emitter);
		if (extended == 202 || extended == 200) {
			Reg3(emitter, 0x3a000000u, 0, 0, OSXJITZERO);
		} else {
			Constant(emitter, 2, 0xffffffffu);
			Reg3(emitter, 0x3a000000u, 0, 0, 2);
		}
		break;
	default:
		return 0;
	}
	Set(emitter, 2, OSXCONDHS);
	Store(emitter, 0, OSXJITGPR(target));
	StoreCarry(emitter);
	Record(emitter, instruction);
	return 1;
}

static int Arithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	int left;
	int right;
	uint8 *ordinary;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 104:
		Load(emitter, 0, OSXJITGPR(left));
		Reg3(emitter, 0x4b000000u, 0, OSXJITZERO, 0);
		break;
	case 11:
	case 75:
		Load(emitter, 0, OSXJITGPR(left));
		Load(emitter, 1, OSXJITGPR(right));
		if (extended == 11) {
			Op(emitter, 0x9ba17c00u);
			Op(emitter, 0xd360fc00u);
		} else {
			Op(emitter, 0x9b217c00u);
			Op(emitter, 0x9360fc00u);
		}
		break;
	case 459:
	case 491:
		Load(emitter, 1, OSXJITGPR(right));
		Miss(emitter, OSXA64CBZ | 1, count);
		Load(emitter, 0, OSXJITGPR(left));
		if (extended == 491) {
			AddImmediate(emitter, OSXA64ADDS, OSXJITZERO, 1, 1);
			ordinary = Forward(emitter, Conditional(OSXCONDNE));
			Constant(emitter, 2, 0x80000000u);
			Reg3(emitter, 0x6b000000u, OSXJITZERO, 0, 2);
			Miss(emitter, Conditional(OSXCONDEQ), count);
			Patch(ordinary, emitter->cursor);
			Reg3(emitter, 0x1ac00c00u, 0, 0, 1);
		} else {
			Reg3(emitter, 0x1ac00800u, 0, 0, 1);
		}
		break;
	default:
		return 0;
	}
	Store(emitter, 0, OSXJITGPR(target));
	Record(emitter, instruction);
	return 1;
}

static int Logical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int source;
	int target;
	int amount;
	source = (instruction >> 21) & 31;
	target = (instruction >> 16) & 31;
	amount = (instruction >> 11) & 31;
	switch (extended) {
	case 24:
	case 536:
		Load(emitter, 1, OSXJITGPR(amount));
		Load(emitter, 0, OSXJITGPR(source));
		LogicImmediate(emitter, OSXA64AND, 1, 1, 63);
		if (extended == 24)
			Reg3(emitter, 0x9ac02000u, 0, 0, 1);
		else
			Reg3(emitter, 0x9ac02400u, 0, 0, 1);
		Store(emitter, 0, OSXJITGPR(target));
		break;
	case 26:
		Load(emitter, 0, OSXJITGPR(source));
		Op(emitter, 0x5ac01000u);
		Store(emitter, 0, OSXJITGPR(target));
		break;
	case 792:
		Load(emitter, 1, OSXJITGPR(amount));
		LogicImmediate(emitter, OSXA64AND, 1, 1, 63);
		Load(emitter, 0, OSXJITGPR(source));
		Op(emitter, 0x93407c02u);
		Reg3(emitter, 0x9ac02800u, 0, 2, 1);
		Store(emitter, 0, OSXJITGPR(target));
		Reg3(emitter, 0x9ac02000u, 3, 0, 1);
		Reg3(emitter, 0xeb000000u, OSXJITZERO, 3, 2);
		Set(emitter, 3, OSXCONDNE);
		Op(emitter, 0xf100005fu);
		Set(emitter, 2, OSXCONDLT);
		Reg3(emitter, 0x0a000000u, 2, 2, 3);
		StoreCarry(emitter);
		break;
	case 824:
		Load(emitter, 0, OSXJITGPR(source));
		if (amount == 0) {
			Constant(emitter, 2, 0);
		} else {
			LogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 0, (1u << amount) - 1);
			Set(emitter, 1, OSXCONDNE);
			ShiftRight(emitter, 2, 0, 31);
			Reg3(emitter, 0x0a000000u, 2, 2, 1);
			Bitfield(emitter, 0x13000000u, 0, 0, (uint32)amount, 31);
		}
		Store(emitter, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		break;
	default:
		return 0;
	}
	Record(emitter, instruction);
	return 1;
}

static int SpecialOffset(uint32 number, uint32 *offset)
{
	switch (number) {
	case 18: *offset = OSXJITOFFSET(dsisr); return 1;
	case 19: *offset = OSXJITOFFSET(dar); return 1;
	case 26: *offset = OSXJITOFFSET(srr0); return 1;
	case 27: *offset = OSXJITOFFSET(srr1); return 1;
	case 272: case 273: case 274: case 275:
		*offset = OSXJITOFFSET(sprg) + (number - 272) * 4;
		return 1;
	}
	return 0;
}

static void Callout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	Store(emitter, OSXJITPC, OSXJITOFFSET(pc));
	Reg3(emitter, 0x4b000000u, 0, OSXJITENTRYSLICE, OSXJITSLICE);
	if (count)
		AddImmediate(emitter, OSXA64ADD, 0, 0, (uint32)count);
	Field(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	Reg3(emitter, 0x8b204000u, 1, 1, 0);
	Field(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
	Reg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	Constant(emitter, 1, instruction);
	CallFar(emitter, (void *)OsxCpuExecuteInline);
	Reg3(emitter, 0x4b000000u, 2, OSXJITENTRYSLICE, OSXJITSLICE);
	if (count)
		AddImmediate(emitter, OSXA64ADD, 2, 2, (uint32)count);
	Field(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	Reg3(emitter, 0xcb204000u, 1, 1, 2);
	Field(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
	resume = Forward(emitter, OSXA64CBZ);
	Charge(emitter, count + 1);
	Load(emitter, OSXJITPC, OSXJITOFFSET(pc));
	Load(emitter, 0, OSXJITOFFSET(slice));
	AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
	running = Forward(emitter, Conditional(OSXCONDGT));
	Settle(emitter);
	Release(emitter, OSXJITCONTINUE);
	Patch(running, emitter->cursor);
	Leave(emitter, OSXJITCONTINUE);
	Patch(resume, emitter->cursor);
	Load(emitter, OSXJITPC, OSXJITOFFSET(pc));
}

static int System(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int target;
	uint32 number;
	uint32 offset;
	uint32 mask;
	uint32 field;
	uint8 *quiet;
	uint8 *pending;
	uint8 *slowmask;
	uint8 *slowpower;
	uint8 *done;
	target = (instruction >> 21) & 31;
	switch (extended) {
	case 54: case 86: case 246: case 278: case 598: case 854:
		return 1;
	case 144:
		mask = 0;
		for (field = 0; field < 8; field++)
			if (instruction & (0x80000u >> field))
				mask |= 0xf0000000u >> (field * 4);
		Load(emitter, 0, OSXJITGPR(target));
		if (mask != 0xffffffffu) {
			LogicImmediate(emitter, OSXA64AND, 0, 0, mask);
			Load(emitter, 1, OSXJITOFFSET(cr));
			LogicImmediate(emitter, OSXA64AND, 1, 1, ~mask);
			Reg3(emitter, 0x2a000000u, 0, 0, 1);
		}
		Store(emitter, 0, OSXJITOFFSET(cr));
		return 1;
	case 83:
		RequireSupervisorMode(emitter, count);
		Load(emitter, 0, OSXJITOFFSET(msr));
		Store(emitter, 0, OSXJITGPR(target));
		return 1;
	case 146:
		emitter->enabled = 0;
		RequireSupervisorMode(emitter, count);
		Load(emitter, 1, OSXJITGPR(target));
		Load(emitter, 2, OSXJITOFFSET(msr));
		Reg3(emitter, 0x4a000000u, 2, 2, 1);
		LogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 2, ~(uint32)OSXMSREE);
		slowmask = Forward(emitter, Conditional(OSXCONDNE));
		LogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 1, OSXMSRPOW);
		slowpower = Forward(emitter, Conditional(OSXCONDNE));
		Store(emitter, 1, OSXJITOFFSET(msr));
		Reg3(emitter, 0x6a000000u, 2, 2, 1);
		quiet = Forward(emitter, Conditional(OSXCONDEQ));
		Load(emitter, 2, OSXJITOFFSET(externalline));
		Load(emitter, 3, OSXJITOFFSET(decpending));
		Reg3(emitter, 0x2a000000u, 2, 2, 3);
		pending = Forward(emitter, OSXA64CBZ | 2);
		AddImmediate(emitter, OSXA64ADD, OSXJITPC, OSXJITPC, 4);
		Charge(emitter, count + 1);
		Settle(emitter);
		Store(emitter, OSXJITZERO, OSXJITOFFSET(slice));
		Release(emitter, OSXJITCONTINUE);
		Patch(quiet, emitter->cursor);
		Patch(pending, emitter->cursor);
		done = Forward(emitter, OSXA64B);
		Patch(slowmask, emitter->cursor);
		Patch(slowpower, emitter->cursor);
		Callout(emitter, instruction, count);
		Patch(done, emitter->cursor);
		return 1;
	case 595:
		RequireSupervisorMode(emitter, count);
		Load(emitter, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4);
		Store(emitter, 0, OSXJITGPR(target));
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		BranchTo(emitter, OSXA64BL, emitter->state->timethunk);
		if (number == 269)
			Op(emitter, 0xd360fc00u);
		Store(emitter, 0, OSXJITGPR(target));
		return 1;
	case 210:
	case 242:
		RequireSupervisorMode(emitter, count);
		if (extended == 210) {
			Constant(emitter, 0, (instruction >> 16) & 15);
		} else {
			Load(emitter, 0, OSXJITGPR((instruction >> 11) & 31));
			ShiftRight(emitter, 0, 0, 28);
		}
		ShiftRight(emitter, 1, OSXJITPC, 28);
		Reg3(emitter, 0x6b000000u, OSXJITZERO, 1, 0);
		Miss(emitter, Conditional(OSXCONDEQ), count);
		Load(emitter, 2, OSXJITGPR(target));
		Constant(emitter, OSXJITSCRATCH, OSXJITOFFSET(sr));
		Reg3(emitter, 0x8b000000u, OSXJITSCRATCH, OSXJITCPU, OSXJITSCRATCH);
		Op(emitter, 0xb8207800u | (OSXJITSCRATCH << 5) | 2);
		Store(emitter, OSXJITZERO, OSXJITOFFSET(fetchtag));
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!SpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			Callout(emitter, instruction, count);
			return 1;
		}
		RequireSupervisorMode(emitter, count);
		if (extended == 339) {
			Load(emitter, 0, offset);
			Store(emitter, 0, OSXJITGPR(target));
		} else {
			Load(emitter, 0, OSXJITGPR(target));
			Store(emitter, 0, offset);
		}
		return 1;
	}
	return 0;
}

static void ConditionZero(OsxNativeEmitter *emitter)
{
	Load(emitter, 0, OSXJITOFFSET(xer));
	ShiftRight(emitter, 0, 0, 31);
}

static void StoreConditionZero(OsxNativeEmitter *emitter)
{
	Load(emitter, 1, OSXJITOFFSET(cr));
	Insert(emitter, 1, 0, 28, 4);
	Store(emitter, 1, OSXJITOFFSET(cr));
}

static int Reserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	Address(emitter, base, index, 0, 0);
	LogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 0, 3);
	Miss(emitter, Conditional(OSXCONDNE), count);
	if (extended == 20) {
		Move(emitter, OSXJITUPDATE, 0);
		Memory(emitter, instruction, 32, index, count);
		Store(emitter, OSXJITUPDATE, OSXJITOFFSET(reservation));
		Constant(emitter, 0, 1);
		Store(emitter, 0, OSXJITOFFSET(reservationvalid));
		return 1;
	}
	Load(emitter, 1, OSXJITOFFSET(reservationvalid));
	invalid = Forward(emitter, OSXA64CBZ | 1);
	Load(emitter, 1, OSXJITOFFSET(reservation));
	Reg3(emitter, 0x6b000000u, OSXJITZERO, 1, 0);
	elsewhere = Forward(emitter, Conditional(OSXCONDNE));
	Memory(emitter, instruction, 36, index, count);
	ConditionZero(emitter);
	LogicImmediate(emitter, OSXA64ORR, 0, 0, 2);
	done = Forward(emitter, OSXA64B);
	Patch(invalid, emitter->cursor);
	Patch(elsewhere, emitter->cursor);
	ConditionZero(emitter);
	Patch(done, emitter->cursor);
	Store(emitter, OSXJITZERO, OSXJITOFFSET(reservationvalid));
	StoreConditionZero(emitter);
	return 1;
}

static int ZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	Address(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	LogicImmediate(emitter, OSXA64AND, 0, 0, ~31u);
	Translate(emitter, 1, 0xfe0, count);
	Op(emitter, 0xa9007c3fu);
	Op(emitter, 0xa9017c3fu);
	return 1;
}

static int ConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
{
	uint32 extended;
	uint32 bitd;
	uint32 bita;
	uint32 bitb;
	extended = (instruction >> 1) & 0x3ff;
	bitd = (instruction >> 21) & 31;
	bita = (instruction >> 16) & 31;
	bitb = (instruction >> 11) & 31;
	if (extended == 0) {
		Load(emitter, 0, OSXJITOFFSET(cr));
		Extract(emitter, 1, 0, (7 - (bita >> 2)) * 4, 4);
		Insert(emitter, 0, 1, (7 - (bitd >> 2)) * 4, 4);
		Store(emitter, 0, OSXJITOFFSET(cr));
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	Load(emitter, 0, OSXJITOFFSET(cr));
	Extract(emitter, 1, 0, 31 - bita, 1);
	Extract(emitter, 2, 0, 31 - bitb, 1);
	if (extended == 129 || extended == 417)
		LogicImmediate(emitter, OSXA64EOR, 2, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		Reg3(emitter, 0x2a000000u, 1, 1, 2);
	else if (extended == 129 || extended == 225 || extended == 257)
		Reg3(emitter, 0x0a000000u, 1, 1, 2);
	else
		Reg3(emitter, 0x4a000000u, 1, 1, 2);
	if (extended == 33 || extended == 225 || extended == 289)
		LogicImmediate(emitter, OSXA64EOR, 1, 1, 1);
	Insert(emitter, 0, 1, 31 - bitd, 1);
	Store(emitter, 0, OSXJITOFFSET(cr));
	return 1;
}

static int Interpretable(uint32 instruction)
{
	uint32 opcode;
	uint32 extended;
	opcode = instruction >> 26;
	if (opcode == 4)
		return 1;
	if (opcode != 31)
		return 0;
	if ((instruction & 0x6fe) == 0xce || (instruction & 0x6fe) == 0x2ce)
		return 1;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 6: case 7: case 38: case 39: case 71: case 135: case 167: case 199:
	case 342: case 374: case 758: case 790: case 822: case 918: case 982:
		return 1;
	}
	return 0;
}

static int Emit(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 opcode;
	uint32 extended;
	uint32 immediate;
	uint32 mask;
	uint32 number;
	uint32 offset;
	int target;
	int base;
	int index;
	OsxVectorFunction handler;
	opcode = instruction >> 26;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	immediate = instruction & 0xffff;
	if (opcode == 16 || opcode == 18 || (opcode == 19 && (((instruction >> 1) & 0x3ff) == 16 || ((instruction >> 1) & 0x3ff) == 528)))
		return Branch(emitter, instruction, count);
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return ConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!Multiple(emitter, instruction, opcode, count))
			Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxCpuVectorHandler(instruction);
		if (handler) {
			VectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (Interpretable(instruction)) {
		Callout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55) {
		Memory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
		if (!Float(emitter, instruction, count))
			Callout(emitter, instruction, count);
		return 1;
	case 8:
		Constant(emitter, 0, (uint32)(int32)(int16)immediate);
		Load(emitter, 1, OSXJITGPR(base));
		Reg3(emitter, 0x6b000000u, 0, 0, 1);
		Set(emitter, 2, OSXCONDHS);
		Store(emitter, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		return 1;
	case 12: case 13:
		Load(emitter, 0, OSXJITGPR(base));
		AddImmediate(emitter, OSXA64ADDS, 0, 0, (uint32)(int32)(int16)immediate);
		Set(emitter, 2, OSXCONDHS);
		Store(emitter, 0, OSXJITGPR(target));
		StoreCarry(emitter);
		if (opcode == 13) {
			AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 7:
		Load(emitter, 0, OSXJITGPR(base));
		Constant(emitter, 1, (uint32)(int32)(int16)immediate);
		Op(emitter, 0x1b017c00u);
		Store(emitter, 0, OSXJITGPR(target));
		return 1;
	case 10: case 11:
		Load(emitter, 0, OSXJITGPR(base));
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, immediate);
		Condition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		Address(emitter, base, -1, immediate, 0);
		Store(emitter, 0, OSXJITGPR(target));
		return 1;
	case 20: case 21: case 23:
		Load(emitter, 0, OSXJITGPR(target));
		if (opcode == 23) {
			Load(emitter, 1, OSXJITGPR(index));
			Reg3(emitter, 0x4b000000u, 1, OSXJITZERO, 1);
			Reg3(emitter, 0x1ac02c00u, 0, 0, 1);
		} else if (index) {
			Op(emitter, 0x13800000u | ((32 - (uint32)index) << 10));
		}
		mask = RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		if (mask != 0xffffffffu)
			LogicImmediate(emitter, OSXA64AND, 0, 0, mask);
		if (opcode == 20) {
			Load(emitter, 1, OSXJITGPR(base));
			LogicImmediate(emitter, OSXA64AND, 1, 1, ~mask);
			Reg3(emitter, 0x2a000000u, 0, 0, 1);
		}
		Store(emitter, 0, OSXJITGPR(base));
		Record(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		Load(emitter, 0, OSXJITGPR(target));
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26) {
			if (immediate)
				LogicImmediate(emitter, OSXA64ORR, 0, 0, immediate);
		} else if (opcode < 28) {
			if (immediate)
				LogicImmediate(emitter, OSXA64EOR, 0, 0, immediate);
		} else {
			LogicImmediate(emitter, OSXA64AND, 0, 0, immediate);
		}
		Store(emitter, 0, OSXJITGPR(base));
		if (opcode >= 28) {
			AddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
			Condition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			Memory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			Memory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			Memory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			Load(emitter, 0, OSXJITGPR(base));
			Load(emitter, 1, OSXJITGPR(index));
			Reg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
			Condition(emitter, target >> 2, extended == 0);
			return 1;
		}
		if (extended == 339 || extended == 467) {
			number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
			if (number == 1)
				offset = OSXJITOFFSET(xer);
			else if (number == 8)
				offset = OSXJITOFFSET(lr);
			else if (number == 9)
				offset = OSXJITOFFSET(ctr);
			else if (number == 256)
				offset = OSXJITOFFSET(vrsave);
			else
				return System(emitter, instruction, extended, count);
			if (extended == 339) {
				Load(emitter, 0, offset);
				Store(emitter, 0, OSXJITGPR(target));
			} else {
				Load(emitter, 0, OSXJITGPR(target));
				if (number == 1)
					LogicImmediate(emitter, OSXA64AND, 0, 0, 0xe000007fu);
				Store(emitter, 0, offset);
			}
			return 1;
		}
		if (extended == 19) {
			Load(emitter, 0, OSXJITOFFSET(cr));
			Store(emitter, 0, OSXJITGPR(target));
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			Load(emitter, 0, OSXJITGPR(base));
			Load(emitter, 1, OSXJITGPR(index));
			if (extended == 266)
				Reg3(emitter, 0x0b000000u, 0, 0, 1);
			else if (extended == 40)
				Reg3(emitter, 0x4b000000u, 0, 1, 0);
			else
				Op(emitter, 0x1b017c00u);
			Store(emitter, 0, OSXJITGPR(target));
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			Load(emitter, 0, OSXJITGPR(target));
			Load(emitter, 1, OSXJITGPR(index));
			if (extended == 28 || extended == 476)
				Reg3(emitter, 0x0a000000u, 0, 0, 1);
			else if (extended == 60)
				Reg3(emitter, 0x0a200000u, 0, 0, 1);
			else if (extended == 284)
				Reg3(emitter, 0x4a200000u, 0, 0, 1);
			else if (extended == 316)
				Reg3(emitter, 0x4a000000u, 0, 0, 1);
			else if (extended == 412)
				Reg3(emitter, 0x2a200000u, 0, 0, 1);
			else
				Reg3(emitter, 0x2a000000u, 0, 0, 1);
			if (extended == 124 || extended == 476)
				Reg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
			Store(emitter, 0, OSXJITGPR(base));
			Record(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return Reserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return ZeroBlock(emitter, instruction, count);
		if (Carrying(emitter, instruction, extended) || Arithmetic(emitter, instruction, extended, count) || Logical(emitter, instruction, extended) || System(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			Load(emitter, 0, OSXJITGPR(target));
			if (extended == 922)
				Bitfield(emitter, 0x13000000u, 0, 0, 0, 15);
			else
				Bitfield(emitter, 0x13000000u, 0, 0, 0, 7);
			Store(emitter, 0, OSXJITGPR(base));
			Record(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

int OsxJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
#if defined(__APPLE__)
	state->executebase = (uint8 *)jit_wx_map(OSXJITBYTES);
	state->writebase = (uint8 *)malloc(OSXJITBYTES);
#else
	state->writebase = OsxJitAllocate(OSXJITBYTES);
	state->executebase = state->writebase;
#endif
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->writebase == NULL || state->executebase == NULL) {
		OsxJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	EmitThunks(state);
	return 1;
}

void OsxJitDestroy(OsxCpu *cpu)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL)
		return;
	if (cpu->logfile)
		fprintf(cpu->logfile, "jit compiled %llu linked %llu instructions %llu interpreted %llu bytes %u\n", (unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)cpu->instructions, (unsigned long long)state->interpreted, state->used);
#if defined(__APPLE__)
	if (state->executebase)
		jit_wx_unmap(state->executebase, OSXJITBYTES);
	free(state->writebase);
#else
	if (state->writebase)
		OsxJitRelease(state->writebase, OSXJITBYTES);
#endif
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
	OsxNativeState *state;
	uint32 first;
	uint32 last;
	int invalidated;
#if OSXG5
	if (cpu->core970) {
		Osx970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
		return 1;
	}
#endif
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || length == 0 || address >= cpu->ramsize)
		return 0;
	if (length > cpu->ramsize - address)
		length = cpu->ramsize - address;
	first = address >> 12;
	last = (address + length - 1) >> 12;
	invalidated = 0;
	for (; first <= last; first++) {
		if (!state->compiledpages[first])
			continue;
		state->compiledpages[first] = 0;
		state->versions[first]++;
		invalidated = 1;
	}
	return invalidated;
}

static int NeedsPc(uint32 instruction)
{
	uint32 opcode;
	opcode = instruction >> 26;
	if (opcode == 7 || opcode == 10 || opcode == 11 || opcode == 14 || opcode == 15 || opcode == 20 || opcode == 21 || opcode == 23 || (opcode >= 24 && opcode <= 29))
		return 0;
	if (opcode == 31) {
		switch ((instruction >> 1) & 0x3ff) {
		case 0: case 19: case 28: case 32: case 40: case 60: case 124: case 144:
		case 235: case 266: case 284: case 316: case 412: case 444: case 476: case 922: case 954:
			return 0;
		}
	}
	return 1;
}

static void Compile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
{
	OsxNativeEmitter emitter;
	uint32 instruction;
	uint32 word;
	uint32 pendingpc;
	uint8 *start;
	uint8 *sliceshort;
	uint8 *stubs[OSXJITMISSES];
	int count;
	int limit;
	int status;
	int index;
	int shared;
	if (state->used + OSXJITRESERVE >= OSXJITBYTES) {
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
		memset(state->blocks, 0, sizeof(state->blocks));
		memset(state->front, 0, sizeof(state->front));
		state->used = state->thunkbytes;
		cpu->jitlink = NULL;
	}
	start = state->writebase + state->used;
	emitter.state = state;
	emitter.cursor = start;
	emitter.physical = physical;
	emitter.deterministic = cpu->deterministic != 0;
	emitter.enabled = 0;
	emitter.misscount = 0;
	Op(&emitter, 0xa9bc7bfdu);
	Op(&emitter, 0x910003fdu);
	Op(&emitter, 0xa90153f3u);
	Op(&emitter, 0xa9025bf5u);
	Op(&emitter, 0xa90363f7u);
	Reg3(&emitter, 0xaa000000u, OSXJITCPU, OSXJITZERO, 0);
	Load(&emitter, OSXJITSLICE, OSXJITOFFSET(slice));
	Move(&emitter, OSXJITENTRYSLICE, OSXJITSLICE);
	Load(&emitter, OSXJITPC, OSXJITOFFSET(pc));
	state->entry = (uint32)(emitter.cursor - start);
	Op(&emitter, OSXA64SUBS | (OSXJITSLICE << 5) | OSXJITZERO);
	sliceshort = Forward(&emitter, Conditional(OSXCONDLT));
	limit = (int)((4096 - (physical & 4095)) >> 2);
	if (limit > OSXJITLIMIT)
		limit = OSXJITLIMIT;
	count = 0;
	status = 0;
	pendingpc = 0;
	while (count < limit && emitter.misscount < OSXJITMISSES - 128) {
		memcpy(&word, cpu->ram + physical + count * 4, 4);
		instruction = tswap32(word);
		if (pendingpc && NeedsPc(instruction)) {
			AddImmediate(&emitter, OSXA64ADD, OSXJITPC, OSXJITPC, pendingpc);
			pendingpc = 0;
		}
		status = Emit(&emitter, instruction, count);
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		AddImmediate(&emitter, OSXA64ADD, OSXJITPC, OSXJITPC, pendingpc);
	if (status == 0)
		Return(&emitter, count);
	else if (status == 1)
		Exit(&emitter, count, (physical & 4095) + (uint32)count * 4);
	for (index = 0; index < emitter.misscount; index++) {
		shared = 0;
		while (shared < index && emitter.misscounts[shared] != emitter.misscounts[index])
			shared++;
		if (shared < index) {
			stubs[index] = stubs[shared];
		} else {
			stubs[index] = emitter.cursor;
			Return(&emitter, emitter.misscounts[index]);
		}
		Patch(emitter.misses[index], stubs[index]);
	}
	Patch(sliceshort, emitter.cursor);
	Return(&emitter, 0);
	memcpy(&word, start + state->entry, 4);
	word |= (uint32)count << 10;
	memcpy(start + state->entry, &word, 4);
	state->compiledpages[physical >> 12] = 1;
	block->physical = physical;
	block->mode = mode;
	block->version = state->versions[physical >> 12] + 1;
	block->count = count;
	state->compiled++;
	if (count == 0) {
		block->function = NULL;
		return;
	}
	block->function = (OsxNativeFunction)(state->executebase + state->used);
	Publish(state, state->executebase + state->used, (uint32)(emitter.cursor - start));
	state->used += ((uint32)(emitter.cursor - start) + 15) & ~15u;
}

static void LinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + state->entry;
	state->linked++;
}

static void Link(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	uint32 word;
	word = OSXA64B | ((uint32)(((uint8 *)block->function + state->entry - jump) / 4) & 0x03ffffff);
	memcpy(jump + (state->writebase - state->executebase), &word, 4);
	Publish(state, jump, 4);
	state->linked++;
}

static void InterpretRun(OsxCpu *cpu, OsxNativeState *state)
{
	uint32 word;
	uint32 instruction;
	uint32 expected;
	int executed;
	executed = 0;
	do {
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		expected = cpu->pc + 4;
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxJitRun(OsxCpu *cpu, int32 count)
{
	OsxNativeState *state;
	OsxNativeBlock *block;
	uint32 physical;
	uint32 mode;
	uint32 instruction;
	uint32 word;
	uint32 set;
	uint32 hash;
	uint32 version;
	uint32 linkkind;
	uint32 linkmode;
	int way;
	int code;
	int hot;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || cpu->tracefile) {
		OsxCpuRun(cpu, count);
		return;
	}
	hot = OSXJITHOT;
	if (cpu->jithot)
		hot = cpu->jithot;
	if (cpu->halted)
		return;
	cpu->slice = count;
	cpu->jitlink = NULL;
	linkkind = 0;
	linkmode = 0;
	while (cpu->slice > 0) {
		if (cpu->tracefile) {
			OsxCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxCpuFetchPage(cpu)) {
			cpu->jitlink = NULL;
			cpu->pc = cpu->nextpc;
			cpu->slice--;
			continue;
		}
		if (!(cpu->msr & OSXMSRSE) && cpu->fetchhost >= cpu->ram && cpu->fetchhost < cpu->ram + cpu->ramsize && !(cpu->pc & 3)) {
			physical = (uint32)(cpu->fetchhost - cpu->ram) + (cpu->pc & 4095);
			mode = cpu->msr & (OSXMSRPR | OSXMSRIR);
			hash = ((physical >> 2) * 0x9e3779b1u) ^ (mode * 0x85ebca6bu);
			version = state->versions[physical >> 12] + 1;
			block = &state->front[hash >> 18];
			if (block->physical != physical || block->mode != mode || block->version != version) {
				set = hash >> 16;
				block = &state->blocks[set * OSXJITWAYS];
				for (way = 0; way < OSXJITWAYS; way++) {
					if (block[way].physical == physical && block[way].mode == mode && block[way].version == version)
						break;
				}
				if (way == OSXJITWAYS) {
					way = state->victims[set];
					state->victims[set] = (uint8)((way + 1) & (OSXJITWAYS - 1));
					block[way].physical = physical;
					block[way].mode = mode;
					block[way].version = version;
					block[way].function = NULL;
					block[way].count = -1;
				}
				block += way;
				if (block->count < 0) {
					if (block->count > -hot)
						block->count--;
					else
						Compile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						Link(state, cpu->jitlink, block);
					else
						LinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				InterpretRun(cpu, state);
				continue;
			}
			if (block->count > 0) {
				code = block->function(cpu);
				cpu->nextpc = cpu->pc;
				if (code == OSXJITLINK || code == OSXJITDYNAMIC) {
					linkkind = (uint32)code;
					linkmode = mode;
					continue;
				}
				if (code == OSXJITCONTINUE || cpu->slice <= 0)
					continue;
			}
		}
		cpu->jitlink = NULL;
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag)
			continue;
		memcpy(&word, cpu->fetchhost + (cpu->pc & 4095), 4);
		instruction = tswap32(word);
		cpu->slice--;
		OsxCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
	}
}

#if OSXPROFILEJIT
void OsxJitTmpStats(OsxCpu *cpu, FILE *log)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state)
		fprintf(log, "TMPJIT used %u compiled %llu linked %llu interpreted %llu\n", state->used,
			(unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)state->interpreted);
}
#endif
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

							Stubbed OSX JIT

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#else

int OsxJitCreate(OsxCpu *cpu)
{
	return 0;
}

void OsxJitDestroy(OsxCpu *cpu)
{
}

int OsxJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
#if OSXG5
	if (cpu->core970) {
		Osx970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
		return 1;
	}
#endif
	return 0;
}

void OsxJitRun(OsxCpu *cpu, int32 count)
{
	OsxCpuRun(cpu, count);
}

#if OSXPROFILEJIT
void OsxJitTmpStats(OsxCpu *cpu, FILE *log)
{
}
#endif

#endif
