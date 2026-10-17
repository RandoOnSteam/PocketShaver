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
#include "osxcpu.h"

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#include <windows.h>

#define OSXJITSETS 65536
#define OSXJITWAYS 4
#define OSXJITBLOCKS (OSXJITSETS * OSXJITWAYS)
#define OSXJITFRONT 16384
#define OSXJITRECORDS (256 * 1024)
#define OSXJITBYTES (64 * 1024 * 1024)
#define OSXJITLIMIT 32
#define OSXJITRESERVE 65536
#define OSXJITMISSES 1024
#define OSXJITENTRY 25
#define OSXJITCONTINUE 0
#define OSXJITINTERPRET 1
#define OSXJITLINK 2
#define OSXJITDYNAMIC 3
#define OSXJITOFFSET(member) ((uint32)offsetof(OsxCpu, member))
#define OSXJITGPR(reg) (OSXJITOFFSET(gpr) + (uint32)(reg) * 4)
#define OSXJITFPR(reg) (OSXJITOFFSET(fpr) + (uint32)(reg) * 8)

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
	HANDLE section;
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
	uint32 thunkbytes;
	uint64 compiled;
	uint64 linked;
	uint64 interpreted;
	int shuffle;
	uint32 vectorconstants[5][4];
#if OSXPROFILEJIT
	FILE *tmpmap;
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
	OsxNativeRecord *records;
	uint8 *checkthunk;
	uint8 *missthunk;
	uint8 *finishthunk;
	uint8 *linkthunk;
	uint8 *interpretthunk;
	uint8 *fillthunk;
	uint8 *timethunk;
	int deterministic;
	uint32 enabled;
	uint32 vectorvalue;
	int shuffle;
	const uint32 (*vectorconstants)[4];
	uint8 *fills[OSXJITLIMIT * 2][2];
	uint8 *fillretries[OSXJITLIMIT * 2];
	int fillcounts[OSXJITLIMIT * 2];
	int fillkinds[OSXJITLIMIT * 2];
	int fillcount;
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
	RegisterOp(emitter, 0x89, 0, 11, 0);
	Shift(emitter, 5, 0, 2);
	ImmediateOp(emitter, 4, 0, OSXJITRECORDS - 1);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xe0); Byte(emitter, 5);
	Byte(emitter, 0x49); Byte(emitter, 0xba);
	Quad(emitter, (uint64)(uintptr_t)emitter->records);
	RegisterOp(emitter, 0x01, 10, 0, 1);
	Byte(emitter, 0xe8);
	JumpTo(emitter, emitter->checkthunk);
	Byte(emitter, 0x0f); Byte(emitter, 0x85);
	JumpTo(emitter, emitter->missthunk);
	Byte(emitter, 0x41); Byte(emitter, 0xff); Byte(emitter, 0x62); Byte(emitter, (uint32)offsetof(OsxNativeRecord, target));
}

static void EmitThunks(OsxNativeState *state)
{
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
	emitter.start = state->writebase;
	emitter.cursor = emitter.start;
	emitter.executestart = state->executebase;
	emitter.misscount = 0;
	state->checkthunk = state->executebase;
	Byte(&emitter, 0x45); Byte(&emitter, 0x3b); Byte(&emitter, 0x1a);
	misses[0] = Jump(&emitter, 5);
	CpuMemory(&emitter, 0x8b, 0, OSXJITOFFSET(fetchtlb), 1);
	RegisterOp(&emitter, 0x89, 2, 11, 0);
	Shift(&emitter, 5, 2, 12);
	ImmediateOp(&emitter, 4, 2, OSXTLBMASK);
	Shift(&emitter, 4, 2, 4);
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
	state->fillthunk = state->executebase + (emitter.cursor - emitter.start);
	Byte(&emitter, 0x50);
	Byte(&emitter, 0x41); Byte(&emitter, 0x52);
	Byte(&emitter, 0x41); Byte(&emitter, 0x53);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xec); Byte(&emitter, 0x20);
	RegisterOp(&emitter, 0x89, 1, 3, 1);
	RegisterOp(&emitter, 0x89, 2, 0, 0);
	RegisterOp(&emitter, 0x89, 8, 9, 0);
	Byte(&emitter, 0x48); Byte(&emitter, 0xb8);
	Quad(&emitter, (uint64)(uintptr_t)OsxCpuFillTlb);
	Byte(&emitter, 0xff); Byte(&emitter, 0xd0);
	RegisterOp(&emitter, 0x89, 1, 0, 0);
	Byte(&emitter, 0x48); Byte(&emitter, 0x83); Byte(&emitter, 0xc4); Byte(&emitter, 0x20);
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
	state->thunkbytes = ((uint32)(emitter.cursor - emitter.start) + 63) & ~63u;
	state->used = state->thunkbytes;
	FlushInstructionCache(GetCurrentProcess(), state->executebase, state->thunkbytes);
}

static void Exit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	uint64 link;
	if (targetoffset > 4095) {
		DynamicExit(emitter, count);
		return;
	}
	Account(emitter, count);
	Byte(emitter, 0x0f); Byte(emitter, 0x8e);
	JumpTo(emitter, emitter->finishthunk);
	Byte(emitter, 0xe9);
	Word(emitter, 0);
	link = (uint64)(uintptr_t)(emitter->executestart + (emitter->cursor - emitter->start) - 4);
	Byte(emitter, 0x48); Byte(emitter, 0xb8);
	Word(emitter, (uint32)link);
	Word(emitter, (uint32)(link >> 32));
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(jitlink), 1);
	Byte(emitter, 0xe9);
	JumpTo(emitter, emitter->linkthunk);
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

static int Float(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 extended;
	int target;
	int left;
	int right;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	if (extended == 40 || extended == 72 || extended == 136 || extended == 264) {
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
		RecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	RequireFloat(emitter, count);
	Byte(emitter, 0xf7); Byte(emitter, 0x83);
	Word(emitter, OSXJITOFFSET(fpscr));
	Word(emitter, 3);
	Miss(emitter, 5, count);
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
	FloatExponent(emitter, 0, count, 0);
	CpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
	FloatExponent(emitter, 0, count, 0);
	Byte(emitter, 0xf2); Byte(emitter, 0x0f); Byte(emitter, 0x10); Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(left));
	Byte(emitter, 0xf2); Byte(emitter, 0x0f);
	if (extended == 20)
		Byte(emitter, 0x5c);
	else if (extended == 21)
		Byte(emitter, 0x58);
	else
		Byte(emitter, 0x59);
	Byte(emitter, 0x83);
	Word(emitter, OSXJITFPR(right));
	Byte(emitter, 0x66); Byte(emitter, 0x48); Byte(emitter, 0x0f); Byte(emitter, 0x7e); Byte(emitter, 0xc0);
	FloatExponent(emitter, 0, count, 1);
	CpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
	RegisterOp(emitter, 0x89, 2, 0, 1);
	Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xea); Byte(emitter, 63);
	ImmediateOp(emitter, 0, 2, 1);
	Shift(emitter, 4, 2, 14);
	CpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr), 0);
	ImmediateOp(emitter, 4, 0, ~0x1f000u);
	RegisterOp(emitter, 0x09, 0, 2, 0);
	CpuMemory(emitter, 0x89, 0, OSXJITOFFSET(fpscr), 0);
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

static void Translate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	uint8 *retry;
	int fill;
	retry = emitter->cursor;
	fill = emitter->fillcount++;
	emitter->fillretries[fill] = retry;
	emitter->fillcounts[fill] = count;
	emitter->fillkinds[fill] = OSXTLBREAD;
	if (store)
		emitter->fillkinds[fill] = OSXTLBWRITE;
	RegisterOp(emitter, 0x89, 1, 0, 0);
	ImmediateOp(emitter, 4, 1, 0xfffff000);
	RegisterOp(emitter, 0x89, 2, 0, 0);
	Shift(emitter, 5, 2, 12);
	ImmediateOp(emitter, 4, 2, OSXTLBMASK);
	Shift(emitter, 4, 2, 4);
	if (store)
		CpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(writetlb), 1);
	else
		CpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(readtlb), 1);
	RegisterOp(emitter, 0x01, 8, 2, 1);
	Byte(emitter, 0x41); Byte(emitter, 0x39); Byte(emitter, 0x08);
	emitter->fills[fill][0] = Jump(emitter, 5);
	RegisterOp(emitter, 0x89, 2, 0, 0);
	Shift(emitter, 5, 2, 28);
	CpuMemory(emitter, 0x8b, 9, OSXJITOFFSET(datasr), 1);
	Byte(emitter, 0x41); Byte(emitter, 0x8b); Byte(emitter, 0x14); Byte(emitter, 0x91);
	Byte(emitter, 0x41); Byte(emitter, 0x39); Byte(emitter, 0x50); Byte(emitter, 4);
	emitter->fills[fill][1] = Jump(emitter, 5);
	ImmediateOp(emitter, 4, 0, 0xfff);
	ImmediateOp(emitter, 7, 0, limit);
	Miss(emitter, 7, count);
	Byte(emitter, 0x4d); Byte(emitter, 0x8b); Byte(emitter, 0x40); Byte(emitter, 8);
}

static void CallHelper(OsxNativeEmitter *emitter, uint64 function)
{
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
		VectorConstant(emitter, 1, 0);
		if (store) {
			VectorMemory(emitter, 0, 0, vector);
			VectorOperation(emitter, 0x3800, 0, 1);
			Byte(emitter, 0xf3); Byte(emitter, 0x41); Byte(emitter, 0x0f); Byte(emitter, 0x7f);
			Byte(emitter, 0x04); Byte(emitter, 0x00);
		} else {
			Byte(emitter, 0xf3); Byte(emitter, 0x41); Byte(emitter, 0x0f); Byte(emitter, 0x6f);
			Byte(emitter, 0x04); Byte(emitter, 0x00);
			VectorOperation(emitter, 0x3800, 0, 1);
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

static void Memory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
{
	int target;
	int base;
	int update;
	int store;
	int wide;
	int single;
	uint32 limit;
	uint32 immediate;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	update = opcode & 1;
	opcode &= ~1u;
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
			Byte(emitter, 0x0f); Byte(emitter, 0xca);
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
			Byte(emitter, 0x0f); Byte(emitter, 0xc8);
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
		Advance(emitter);
		Exit(emitter, count + 1, offset + 4);
	}
	return 2;
}

static void Setcc(OsxNativeEmitter *emitter, uint32 condition, int reg)
{
	Byte(emitter, 0x0f);
	Byte(emitter, 0x90 | condition);
	Byte(emitter, 0xc0 | reg);
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
		Byte(emitter, 0xe8);
		JumpTo(emitter, emitter->timethunk);
		if (number == 269) {
			Byte(emitter, 0x48); Byte(emitter, 0xc1); Byte(emitter, 0xe8); Byte(emitter, 32);
		}
		CpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
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

static int Multiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
{
	int first;
	int base;
	int reg;
	int32 displacement;
	uint32 single;
	first = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	displacement = (int32)(int16)(instruction & 0xffff);
	if (displacement + 4 * (31 - first) > 32767)
		return 0;
	if (opcode == 46 && base >= first)
		return 0;
	single = 32;
	if (opcode == 47)
		single = 36;
	for (reg = first; reg < 32; reg++)
		Memory(emitter, (single << 26) | ((uint32)reg << 21) | ((uint32)base << 16) | ((uint32)(displacement + 4 * (reg - first)) & 0xffff), single, -1, count);
	return 1;
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
	if (opcode == 4 || opcode == 59)
		return 1;
	if (opcode != 31)
		return 0;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 6: case 7: case 38: case 39: case 71: case 135: case 167: case 199:
	case 342: case 374: case 534: case 662: case 758: case 790: case 792: case 822: case 918: case 982: case 210: case 242:
		return 1;
	}
	return 0;
}

static void Callout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	CpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	RegisterOp(emitter, 0x89, 1, 3, 1);
	Immediate(emitter, 2, instruction);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xec); Byte(emitter, 0x20);
	Byte(emitter, 0x48); Byte(emitter, 0xb8);
	Quad(emitter, (uint64)(uintptr_t)OsxCpuExecuteInline);
	Byte(emitter, 0xff); Byte(emitter, 0xd0);
	Byte(emitter, 0x48); Byte(emitter, 0x83); Byte(emitter, 0xc4); Byte(emitter, 0x20);
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
	if (opcode == 46 || opcode == 47)
		return Multiple(emitter, instruction, opcode, count);
	if (opcode == 31 && ((instruction & 0x6fe) == 0xce || (instruction & 0x6fe) == 0x2ce)) {
		VectorQuad(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4 && OsxCpuVectorHandler(instruction)) {
		if (VectorNative(emitter, instruction, count))
			return 1;
		emitter->vectorvalue = 0;
		VectorCall(emitter, instruction, OsxCpuVectorHandler(instruction), count);
		return 1;
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
	case 63:
		if (!Float(emitter, instruction, count))
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

int OsxJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	int index;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->shuffle = IsProcessorFeaturePresent(36) != 0;
	for (index = 0; index < 4; index++) {
		state->vectorconstants[0][index] = 0x00010203u + (uint32)index * 0x04040404u;
		state->vectorconstants[1][index] = 0x1f1f1f1fu;
		state->vectorconstants[2][index] = 0x03030303u;
		state->vectorconstants[3][index] = 0x0f0f0f0fu;
		state->vectorconstants[4][index] = 0x00004000u;
	}
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
	state->section = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, OSXJITBYTES, NULL);
	if (state->section) {
		state->writebase = (uint8 *)MapViewOfFile(state->section, FILE_MAP_WRITE | FILE_MAP_EXECUTE, 0, 0, OSXJITBYTES);
		state->executebase = state->writebase;
	}
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
		UnmapViewOfFile(state->writebase);
	if (state->section)
		CloseHandle(state->section);
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
	int count;
	int limit;
	int status;
	int index;
	uint8 *sliceshort;
	uint8 *stubs[OSXJITMISSES];
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
	emitter.records = state->records;
	emitter.checkthunk = state->checkthunk;
	emitter.missthunk = state->missthunk;
	emitter.finishthunk = state->finishthunk;
	emitter.linkthunk = state->linkthunk;
	emitter.interpretthunk = state->interpretthunk;
	emitter.fillthunk = state->fillthunk;
	emitter.timethunk = state->timethunk;
	emitter.deterministic = cpu->deterministic != 0;
	emitter.enabled = 0;
	emitter.vectorvalue = 0;
	emitter.shuffle = state->shuffle;
	emitter.vectorconstants = state->vectorconstants;
	emitter.fillcount = 0;
	emitter.misscount = 0;
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
	while (count < limit && emitter.misscount < OSXJITMISSES - 128 && emitter.fillcount < OSXJITLIMIT * 2 - 4) {
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
		status = Emit(&emitter, instruction, count);
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
	for (index = 0; index < emitter.fillcount; index++) {
		Patch(emitter.fills[index][0], emitter.cursor);
		Patch(emitter.fills[index][1], emitter.cursor);
		Immediate(&emitter, 9, (uint32)emitter.fillkinds[index]);
		Byte(&emitter, 0xe8);
		JumpTo(&emitter, emitter.fillthunk);
		RegisterOp(&emitter, 0x85, 1, 1, 0);
		Miss(&emitter, 4, emitter.fillcounts[index]);
		Byte(&emitter, 0xe9);
		Word(&emitter, 0);
		Patch(emitter.cursor - 4, emitter.fillretries[index]);
	}
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
	FlushInstructionCache(GetCurrentProcess(), state->executebase + state->used, (SIZE_T)(emitter.cursor - emitter.start));
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
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL || cpu->tracefile) {
		OsxCpuRun(cpu, count);
		return;
	}
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
		if (cpu->fetchhost >= cpu->ram && cpu->fetchhost < cpu->ram + cpu->ramsize && !(cpu->pc & 3)) {
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
					Compile(cpu, state, block + way, physical, mode);
				}
				state->front[hash >> 18] = block[way];
				block = &state->front[hash >> 18];
			}
			if (cpu->jitlink) {
				if (block->count && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						Link(state, cpu->jitlink, block);
					else
						LinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count) {
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
	return 0;
}

void OsxJitRun(OsxCpu *cpu, int32 count)
{
	OsxCpuRun(cpu, count);
}

#endif
