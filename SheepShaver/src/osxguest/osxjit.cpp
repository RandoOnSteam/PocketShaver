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
/* OSXJITNOFUSED forces off FMA ops for testing. Don't release with it on! */
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

static void OsxGuestJitByte(OsxNativeEmitter *emitter, uint32 value)
{
	*emitter->cursor++ = (uint8)value;
}

static void OsxGuestWord(OsxNativeEmitter *emitter, uint32 value)
{
	memcpy(emitter->cursor, &value, 4);
	emitter->cursor += 4;
}

static void OsxGuestRegisterOp(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, int wide)
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
		OsxGuestJitByte(emitter, prefix);
	OsxGuestJitByte(emitter, opcode);
	OsxGuestJitByte(emitter, 0xc0 | ((source & 7) << 3) | (target & 7));
}

static void OsxGuestCpuMemory(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset, int wide)
{
	uint32 prefix;
	prefix = 0x40;
	if (wide)
		prefix |= 8;
	if (reg & 8)
		prefix |= 4;
	if (prefix != 0x40)
		OsxGuestJitByte(emitter, prefix);
	OsxGuestJitByte(emitter, opcode);
	if (offset < 128) {
		OsxGuestJitByte(emitter, 0x43 | ((reg & 7) << 3));
		OsxGuestJitByte(emitter, offset);
	} else {
		OsxGuestJitByte(emitter, 0x83 | ((reg & 7) << 3));
		OsxGuestWord(emitter, offset);
	}
}

static void OsxGuestImmediate(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	if (reg & 8)
		OsxGuestJitByte(emitter, 0x41);
	OsxGuestJitByte(emitter, 0xb8 | (reg & 7));
	OsxGuestWord(emitter, value);
}

static void OsxGuestImmediateOp(OsxNativeEmitter *emitter, int operation, int reg, uint32 value)
{
	if (reg & 8)
		OsxGuestJitByte(emitter, 0x41);
	if (value <= 127 || value >= 0xffffff80u) {
		OsxGuestJitByte(emitter, 0x83);
		OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | (reg & 7));
		OsxGuestJitByte(emitter, value);
	} else {
		OsxGuestJitByte(emitter, 0x81);
		OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | (reg & 7));
		OsxGuestWord(emitter, value);
	}
}

static void OsxGuestShift(OsxNativeEmitter *emitter, int operation, int reg, uint32 amount)
{
	if (amount == 0)
		return;
	if (reg & 8)
		OsxGuestJitByte(emitter, 0x41);
	OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | (reg & 7));
	OsxGuestJitByte(emitter, amount);
}

static uint8 *OsxGuestJump(OsxNativeEmitter *emitter, int condition)
{
	uint8 *patch;
	OsxGuestJitByte(emitter, 0x0f);
	OsxGuestJitByte(emitter, 0x80 | condition);
	patch = emitter->cursor;
	OsxGuestWord(emitter, 0);
	return patch;
}

static void OsxGuestPatch(uint8 *patch, uint8 *target)
{
	int32 distance;
	distance = (int32)(target - patch - 4);
	memcpy(patch, &distance, 4);
}

static void OsxGuestMiss(OsxNativeEmitter *emitter, int condition, int count)
{
#if OSXJITSLOWPATH
	if (emitter->slowactive) {
		emitter->slowjumps[emitter->slowjumpcount] = OsxGuestJump(emitter, condition);
		emitter->slowowners[emitter->slowjumpcount++] = emitter->slowcount;
		return;
	}
#endif
	emitter->misses[emitter->misscount] = OsxGuestJump(emitter, condition);
	emitter->misscounts[emitter->misscount++] = count;
}

static void OsxGuestCharge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	OsxGuestImmediateOp(emitter, 5, 12, (uint32)count);
}

static void OsxGuestAccount(OsxNativeEmitter *emitter, int count)
{
	OsxGuestCharge(emitter, count);
}

static void OsxGuestSettle(OsxNativeEmitter *emitter)
{
	OsxGuestCpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	OsxGuestRegisterOp(emitter, 0x89, 0, 13, 0);
	OsxGuestRegisterOp(emitter, 0x29, 0, 12, 0);
	OsxGuestCpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions), 1);
}

static void OsxGuestRelease(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestImmediate(emitter, 0, code);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x5d);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x5c);
	OsxGuestJitByte(emitter, 0x5b);
	OsxGuestJitByte(emitter, 0xc3);
}

static void OsxGuestLeave(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestSettle(emitter);
	OsxGuestCpuMemory(emitter, 0x89, 12, OSXJITOFFSET(slice), 0);
	OsxGuestRelease(emitter, code);
}

static void OsxGuestQuad(OsxNativeEmitter *emitter, uint64 value)
{
	memcpy(emitter->cursor, &value, 8);
	emitter->cursor += 8;
}

static void OsxGuestJumpTo(OsxNativeEmitter *emitter, uint8 *target)
{
	int32 distance;
	distance = (int32)(target - (emitter->executestart + (emitter->cursor - emitter->start) + 4));
	OsxGuestWord(emitter, (uint32)distance);
}

static void OsxGuestReturn(OsxNativeEmitter *emitter, int count)
{
	OsxGuestAccount(emitter, count);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJumpTo(emitter, emitter->interpretthunk);
}

static void OsxGuestDynamicExit(OsxNativeEmitter *emitter, int count)
{
	OsxGuestAccount(emitter, count);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x8e);
	OsxGuestJumpTo(emitter, emitter->finishthunk);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJumpTo(emitter, emitter->dynamicthunk);
}

static void OsxGuestTranslateThunk(OsxNativeEmitter *emitter, uint8 *fillthunk, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	uint8 *again;
	retry = emitter->cursor;
	if (limit == 0xfff) {
		OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
	} else {
		OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xfff - limit);
	}
	OsxGuestImmediateOp(emitter, 4, 1, 0xfffff000);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 0);
	OsxGuestShift(emitter, 5, 2, 8);
	OsxGuestImmediateOp(emitter, 4, 2, OSXTLBMASK << 4);
	if (store)
		OsxGuestCpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(writetlb), 1);
	else
		OsxGuestCpuMemory(emitter, 0x8b, 8, OSXJITOFFSET(readtlb), 1);
	OsxGuestRegisterOp(emitter, 0x01, 8, 2, 1);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x08);
	slow = OsxGuestJump(emitter, 5);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 0);
	OsxGuestShift(emitter, 5, 2, 28);
	OsxGuestCpuMemory(emitter, 0x8b, 9, OSXJITOFFSET(datasr), 1);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x91);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x50); OsxGuestJitByte(emitter, 4);
	segment = OsxGuestJump(emitter, 5);
	OsxGuestJitByte(emitter, 0x4d); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x40); OsxGuestJitByte(emitter, 8);
	OsxGuestImmediateOp(emitter, 4, 0, 0xfff);
	OsxGuestRegisterOp(emitter, 0x31, 1, 1, 0);
	OsxGuestJitByte(emitter, 0xc3);
	OsxGuestPatch(slow, emitter->cursor);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
	OsxGuestImmediateOp(emitter, 4, 1, 0xfffff000);
	OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x08);
	page = OsxGuestJump(emitter, 5);
	OsxGuestImmediateOp(emitter, 1, 1, 1);
	OsxGuestJitByte(emitter, 0xc3);
	OsxGuestPatch(page, emitter->cursor);
	OsxGuestPatch(segment, emitter->cursor);
	if (store)
		OsxGuestImmediate(emitter, 9, OSXTLBWRITE);
	else
		OsxGuestImmediate(emitter, 9, OSXTLBREAD);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xec); OsxGuestJitByte(emitter, 8);
	OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJumpTo(emitter, fillthunk);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 8);
	OsxGuestRegisterOp(emitter, 0x85, 1, 1, 0);
	again = OsxGuestJump(emitter, 5);
	OsxGuestPatch(again, retry);
	OsxGuestImmediateOp(emitter, 1, 1, 1);
	OsxGuestJitByte(emitter, 0xc3);
}

static void OsxGuestEmitThunks(OsxNativeState *state)
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
	OsxGuestJitByte(&emitter, 0x45); OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x1a);
	misses[0] = OsxGuestJump(&emitter, 5);
	OsxGuestCpuMemory(&emitter, 0x8b, 0, OSXJITOFFSET(fetchtlb), 1);
	OsxGuestRegisterOp(&emitter, 0x89, 2, 11, 0);
	OsxGuestShift(&emitter, 5, 2, 8);
	OsxGuestImmediateOp(&emitter, 4, 2, OSXTLBMASK << 4);
	OsxGuestRegisterOp(&emitter, 0x01, 0, 2, 1);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 11, 0);
	OsxGuestImmediateOp(&emitter, 4, 1, 0xfffff000);
	OsxGuestJitByte(&emitter, 0x39); OsxGuestJitByte(&emitter, 0x08);
	misses[1] = OsxGuestJump(&emitter, 5);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 11, 0);
	OsxGuestShift(&emitter, 5, 1, 28);
	OsxGuestCpuMemory(&emitter, 0x8b, 9, OSXJITOFFSET(fetchsr), 1);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x0c); OsxGuestJitByte(&emitter, 0x89);
	OsxGuestJitByte(&emitter, 0x39); OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 4);
	misses[2] = OsxGuestJump(&emitter, 5);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x40); OsxGuestJitByte(&emitter, 8);
	OsxGuestJitByte(&emitter, 0x49); OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x42); OsxGuestJitByte(&emitter, (uint32)offsetof(OsxNativeRecord, host));
	misses[3] = OsxGuestJump(&emitter, 5);
	OsxGuestJitByte(&emitter, 0x49); OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x4a); OsxGuestJitByte(&emitter, (uint32)offsetof(OsxNativeRecord, versionaddress));
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x09);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x4a); OsxGuestJitByte(&emitter, (uint32)offsetof(OsxNativeRecord, version));
	misses[4] = OsxGuestJump(&emitter, 5);
	OsxGuestCpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(fetchhost), 1);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 11, 0);
	OsxGuestImmediateOp(&emitter, 4, 1, 0xfffff000);
	OsxGuestImmediateOp(&emitter, 1, 1, 1);
	OsxGuestCpuMemory(&emitter, 0x89, 1, OSXJITOFFSET(fetchtag), 0);
	OsxGuestRegisterOp(&emitter, 0x31, 0, 0, 0);
	for (index = 0; index < 5; index++)
		OsxGuestPatch(misses[index], emitter.cursor);
	OsxGuestJitByte(&emitter, 0xc3);
	state->missthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestCpuMemory(&emitter, 0x89, 10, OSXJITOFFSET(jitlink), 1);
	OsxGuestLeave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestLeave(&emitter, OSXJITCONTINUE);
	state->linkthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestLeave(&emitter, OSXJITLINK);
	state->interpretthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestLeave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestJitByte(&emitter, 0x58);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xe8); OsxGuestJitByte(&emitter, 9);
	OsxGuestCpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(jitlink), 1);
	OsxGuestLeave(&emitter, OSXJITLINK);
	state->dynamicthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestRegisterOp(&emitter, 0x89, 0, 11, 0);
	OsxGuestShift(&emitter, 5, 0, 2);
	OsxGuestImmediateOp(&emitter, 4, 0, OSXJITRECORDS - 1);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0xc1); OsxGuestJitByte(&emitter, 0xe0); OsxGuestJitByte(&emitter, 5);
	OsxGuestJitByte(&emitter, 0x49); OsxGuestJitByte(&emitter, 0xba);
	OsxGuestQuad(&emitter, (uint64)(uintptr_t)state->records);
	OsxGuestRegisterOp(&emitter, 0x01, 10, 0, 1);
	OsxGuestJitByte(&emitter, 0xe8);
	OsxGuestJumpTo(&emitter, state->checkthunk);
	OsxGuestJitByte(&emitter, 0x0f); OsxGuestJitByte(&emitter, 0x85);
	OsxGuestJumpTo(&emitter, state->missthunk);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0xff); OsxGuestJitByte(&emitter, 0x62); OsxGuestJitByte(&emitter, (uint32)offsetof(OsxNativeRecord, target));
	state->fillthunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestJitByte(&emitter, 0x50);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x52);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x53);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xec); OsxGuestJitByte(&emitter, 0x30);
	OsxGuestJitByte(&emitter, 0x0f); OsxGuestJitByte(&emitter, 0x29); OsxGuestJitByte(&emitter, 0x6c); OsxGuestJitByte(&emitter, 0x24); OsxGuestJitByte(&emitter, 0x20);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 3, 1);
	OsxGuestRegisterOp(&emitter, 0x89, 2, 0, 0);
	OsxGuestRegisterOp(&emitter, 0x89, 8, 9, 0);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0xb8);
	OsxGuestQuad(&emitter, (uint64)(uintptr_t)OsxGuestCpuFillTlb);
	OsxGuestJitByte(&emitter, 0xff); OsxGuestJitByte(&emitter, 0xd0);
	OsxGuestJitByte(&emitter, 0x0f); OsxGuestJitByte(&emitter, 0x28); OsxGuestJitByte(&emitter, 0x6c); OsxGuestJitByte(&emitter, 0x24); OsxGuestJitByte(&emitter, 0x20);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 0, 0);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xc4); OsxGuestJitByte(&emitter, 0x30);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x5b);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x5a);
	OsxGuestJitByte(&emitter, 0x58);
	OsxGuestJitByte(&emitter, 0xc3);
	state->timethunk = state->executebase + (emitter.cursor - emitter.start);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x53);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xec); OsxGuestJitByte(&emitter, 0x20);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 3, 1);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0xb8);
	OsxGuestQuad(&emitter, (uint64)(uintptr_t)OsxGuestCpuTimebase);
	OsxGuestJitByte(&emitter, 0xff); OsxGuestJitByte(&emitter, 0xd0);
	OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xc4); OsxGuestJitByte(&emitter, 0x20);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x5b);
	OsxGuestJitByte(&emitter, 0xc3);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = state->executebase + (emitter.cursor - emitter.start);
		OsxGuestTranslateThunk(&emitter, state->fillthunk, index / 6, translatelimits[index % 6]);
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

static void OsxGuestExit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		OsxGuestDynamicExit(emitter, count);
		return;
	}
	OsxGuestAccount(emitter, count);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x8e);
	OsxGuestJumpTo(emitter, emitter->finishthunk);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestWord(emitter, 0);
	OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJumpTo(emitter, emitter->linkcallthunk);
}

static void OsxGuestAdvance(OsxNativeEmitter *emitter)
{
	OsxGuestImmediateOp(emitter, 0, 11, 4);
}

static void OsxGuestCondition(OsxNativeEmitter *emitter, int field, int issigned)
{
	uint32 shift;
	OsxGuestImmediate(emitter, 0, 2);
	OsxGuestImmediate(emitter, 1, 4);
	OsxGuestImmediate(emitter, 2, 8);
	OsxGuestJitByte(emitter, 0x0f);
	if (issigned)
		OsxGuestJitByte(emitter, 0x4f);
	else
		OsxGuestJitByte(emitter, 0x47);
	OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0x0f);
	if (issigned)
		OsxGuestJitByte(emitter, 0x4c);
	else
		OsxGuestJitByte(emitter, 0x42);
	OsxGuestJitByte(emitter, 0xc2);
	OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(xer), 0);
	OsxGuestShift(emitter, 5, 2, 31);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2, 0);
	shift = (uint32)(7 - field) * 4;
	OsxGuestShift(emitter, 4, 0, shift);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	OsxGuestImmediateOp(emitter, 4, 1, ~(15u << shift));
	OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
}

static void OsxGuestRecord(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
		OsxGuestCondition(emitter, 0, 1);
	}
}

static void OsxGuestRequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRFP);
	OsxGuestMiss(emitter, 4, count);
}

static void OsxGuestRecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr), 0);
		OsxGuestShift(emitter, 5, 0, 4);
		OsxGuestImmediateOp(emitter, 4, 0, 0x0f000000);
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
		OsxGuestImmediateOp(emitter, 4, 1, 0xf0ffffff);
		OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
	}
}

static void OsxGuestFloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	OsxGuestRegisterOp(emitter, 0x89, 2, reg, 1);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 52);
	OsxGuestImmediateOp(emitter, 4, 2, 0x7ff);
	OsxGuestImmediateOp(emitter, 7, 2, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	if (normal) {
		OsxGuestRegisterOp(emitter, 0x85, 2, 2, 0);
		OsxGuestMiss(emitter, 4, count);
	}
}

static void OsxGuestSetcc(OsxNativeEmitter *emitter, uint32 condition, int reg)
{
	OsxGuestJitByte(emitter, 0x0f);
	OsxGuestJitByte(emitter, 0x90 | condition);
	OsxGuestJitByte(emitter, 0xc0 | reg);
}

static void OsxGuestFloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestFloatExponent(emitter, 0, count, 0);
	OsxGuestRegisterOp(emitter, 0x85, 2, 2, 0);
	zero = OsxGuestJump(emitter, 4);
	if (single) {
		OsxGuestImmediateOp(emitter, 7, 2, 897);
		OsxGuestMiss(emitter, 2, count);
	}
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 1);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 63);
	OsxGuestImmediateOp(emitter, 0, 2, 1);
	OsxGuestShift(emitter, 4, 2, 14);
	OsxGuestJitByte(emitter, 0xe9);
	merge = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(zero, emitter->cursor);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 1);
	OsxGuestRegisterOp(emitter, 0x01, 2, 2, 1);
	OsxGuestMiss(emitter, 5, count);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 1);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 63);
	OsxGuestShift(emitter, 4, 2, 16);
	OsxGuestImmediateOp(emitter, 1, 2, 0x2000);
	OsxGuestPatch(merge, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr), 0);
	OsxGuestImmediateOp(emitter, 4, 0, ~0x1f000u);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2, 0);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(fpscr), 0);
}

static void OsxGuestFloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x10); OsxGuestJitByte(emitter, 0x83 | (vector << 3));
	OsxGuestWord(emitter, OSXJITFPR(reg));
}

static void OsxGuestFloatRounding(OsxNativeEmitter *emitter, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(fpscr));
	OsxGuestWord(emitter, 3);
	OsxGuestMiss(emitter, 5, count);
}

static void OsxGuestFloatNarrow(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5a); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5a); OsxGuestJitByte(emitter, 0xc0);
}

static void OsxGuestFloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	uint32 shift;
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
	OsxGuestFloatExponent(emitter, 0, count, 0);
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
	OsxGuestFloatExponent(emitter, 0, count, 0);
	OsxGuestRegisterOp(emitter, 0x31, 0, 0, 0);
	OsxGuestRegisterOp(emitter, 0x31, 1, 1, 0);
	OsxGuestRegisterOp(emitter, 0x31, 2, 2, 0);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(right));
	OsxGuestSetcc(emitter, 2, 0);
	OsxGuestSetcc(emitter, 7, 1);
	OsxGuestSetcc(emitter, 4, 2);
	OsxGuestShift(emitter, 4, 0, 3);
	OsxGuestShift(emitter, 4, 1, 2);
	OsxGuestRegisterOp(emitter, 0x01, 2, 2, 0);
	OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2, 0);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
	OsxGuestShift(emitter, 4, 1, 12);
	OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(fpscr), 0);
	OsxGuestImmediateOp(emitter, 4, 2, ~0xf000u);
	OsxGuestRegisterOp(emitter, 0x09, 2, 1, 0);
	OsxGuestCpuMemory(emitter, 0x89, 2, OSXJITOFFSET(fpscr), 0);
	shift = (uint32)(7 - field) * 4;
	OsxGuestShift(emitter, 4, 0, shift);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	OsxGuestImmediateOp(emitter, 4, 1, ~(0xfu << shift));
	OsxGuestRegisterOp(emitter, 0x09, 1, 0, 0);
	OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(cr), 0);
}

static void OsxGuestFloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	OsxGuestFloatLoad(emitter, 0, right);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2c); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestImmediateOp(emitter, 7, 0, 0x80000000u);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2a); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0xc8);
	exact = OsxGuestJump(emitter, 4);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0x8b);
	OsxGuestWord(emitter, OSXJITOFFSET(fpscr));
	OsxGuestWord(emitter, 0x82000000u);
	OsxGuestPatch(exact, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 0);
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(target) + 4);
	OsxGuestWord(emitter, 0xfff80000u);
}

static void OsxGuestFloatSingleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(reg));
	OsxGuestWord(emitter, 0x1fffffff);
	OsxGuestMiss(emitter, 5, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(reg) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xea); OsxGuestWord(emitter, 897);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 254);
	OsxGuestMiss(emitter, 3, count);
}

static void OsxGuestFloatSingleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	OsxGuestFloatSingleOperand(emitter, left, count);
	OsxGuestFloatSingleOperand(emitter, multiplier, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(right) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(multiplier));
	OsxGuestFloatLoad(emitter, 1, right);
	if (extended == 28 || extended == 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xd2);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xca);
	}
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc1);
	if (extended >= 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestFloatNarrow(emitter);
	OsxGuestFloatClass(emitter, target, count, 1);
}

static void OsxGuestFloatDoubleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(reg) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xea); OsxGuestWord(emitter, 565);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 917);
	OsxGuestMiss(emitter, 3, count);
}

static void OsxGuestFloatOddSum(OsxNativeEmitter *emitter)
{
	uint8 *exact;
	uint8 *odd;
	uint8 *differ;
	uint8 *done;
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xdb);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0xc3);
	exact = OsxGuestJump(emitter, 4);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 1);
	odd = OsxGuestJump(emitter, 5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xe0);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x50); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xed);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xd5); OsxGuestJitByte(emitter, 63);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 1);
	differ = OsxGuestJump(emitter, 5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xd4); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestJitByte(emitter, 0xe9);
	done = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(differ, emitter->cursor);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xfb); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestPatch(done, emitter->cursor);
	OsxGuestPatch(exact, emitter->cursor);
	OsxGuestPatch(odd, emitter->cursor);
}

static void OsxGuestFloatDoubleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	OsxGuestFloatDoubleOperand(emitter, left, count);
	OsxGuestFloatDoubleOperand(emitter, multiplier, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(right) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xb8); OsxGuestWord(emitter, 0x02000000);
	OsxGuestJitByte(emitter, 0xba); OsxGuestWord(emitter, 0x41a00000);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6e); OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6e); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x62); OsxGuestJitByte(emitter, 0xec);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xcd);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xca);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc1);
	OsxGuestFloatLoad(emitter, 2, multiplier);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xdd);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xdc);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd3);
	OsxGuestFloatLoad(emitter, 4, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xa3);
	OsxGuestWord(emitter, OSXJITFPR(multiplier));
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xec);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xca);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xd8);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xc2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xe8);
	OsxGuestFloatLoad(emitter, 0, right);
	if (extended == 28 || extended == 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xcc);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestFloatOddSum(emitter);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc2);
	if (extended >= 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestFloatClass(emitter, target, count, 0);
}

static int OsxGuestFloat(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestRequireFloat(emitter, count);
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
			if (extended != 72) {
				OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xba);
				if (extended == 40)
					OsxGuestJitByte(emitter, 0xf8);
				else if (extended == 136)
					OsxGuestJitByte(emitter, 0xe8);
				else
					OsxGuestJitByte(emitter, 0xf0);
				OsxGuestJitByte(emitter, 63);
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
			break;
		case 0: case 32:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
			OsxGuestFloatExponent(emitter, 0, count, 0);
			OsxGuestFloatLoad(emitter, 0, right);
			OsxGuestFloatNarrow(emitter);
			OsxGuestFloatClass(emitter, target, count, 1);
			break;
		case 15:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		if (!emitter->fused && single) {
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatSingleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			OsxGuestRecordFloat(emitter, instruction);
			return 1;
		}
		if (!emitter->fused) {
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatDoubleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			OsxGuestRecordFloat(emitter, instruction);
			return 1;
		}
		OsxGuestRequireFloat(emitter, count);
		OsxGuestFloatRounding(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
		OsxGuestFloatExponent(emitter, 0, count, 0);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(multiplier), 1);
		OsxGuestFloatExponent(emitter, 0, count, 0);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
		OsxGuestFloatExponent(emitter, 0, count, 0);
		OsxGuestFloatLoad(emitter, 0, left);
		OsxGuestFloatLoad(emitter, 1, multiplier);
		OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 0xe2); OsxGuestJitByte(emitter, 0xf1);
		if (extended == 28 || extended == 30)
			OsxGuestJitByte(emitter, 0xab);
		else
			OsxGuestJitByte(emitter, 0xa9);
		OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITFPR(right));
		if (extended >= 30) {
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xc0);
			OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xba); OsxGuestJitByte(emitter, 0xf8); OsxGuestJitByte(emitter, 63);
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6e); OsxGuestJitByte(emitter, 0xc0);
		}
		if (single)
			OsxGuestFloatNarrow(emitter);
		OsxGuestFloatClass(emitter, target, count, single);
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	OsxGuestRequireFloat(emitter, count);
	OsxGuestFloatRounding(emitter, count);
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(left), 1);
	OsxGuestFloatExponent(emitter, 0, count, 0);
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right), 1);
	OsxGuestFloatExponent(emitter, 0, count, extended == 18);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f);
	if (extended == 18)
		OsxGuestJitByte(emitter, 0x5e);
	else if (extended == 20)
		OsxGuestJitByte(emitter, 0x5c);
	else if (extended == 21)
		OsxGuestJitByte(emitter, 0x58);
	else
		OsxGuestJitByte(emitter, 0x59);
	OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(right));
	if (single)
		OsxGuestFloatNarrow(emitter);
	OsxGuestFloatClass(emitter, target, count, single);
	OsxGuestRecordFloat(emitter, instruction);
	return 1;
}

static uint32 OsxGuestRotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void OsxGuestAddress(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
	else
		OsxGuestRegisterOp(emitter, 0x31, 0, 0, 0);
	if (index >= 0)
		OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
	else if (immediate)
		OsxGuestImmediateOp(emitter, 0, 0, immediate);
	if (update)
		OsxGuestRegisterOp(emitter, 0x89, 10, 0, 0);
}

static int OsxGuestLimitIndex(uint32 limit)
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

static void OsxGuestTranslate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJumpTo(emitter, emitter->translatethunks[store != 0][OsxGuestLimitIndex(limit)]);
	OsxGuestMiss(emitter, 5, count);
}

static int OsxGuestMultiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
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
	OsxGuestAddress(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	OsxGuestTranslate(emitter, opcode == 47, 0xffc, count);
	OsxGuestImmediateOp(emitter, 7, 0, 0x1000 - registers * 4);
	OsxGuestMiss(emitter, 7, count);
	OsxGuestRegisterOp(emitter, 0x01, 8, 0, 1);
	OsxGuestJitByte(emitter, 0x4c); OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0x8b);
	OsxGuestWord(emitter, OSXJITGPR(first));
	OsxGuestImmediate(emitter, 1, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x10);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x11);
	} else {
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x11);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x10);
	}
	OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 4);
	OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 4);
	OsxGuestJitByte(emitter, 0xff); OsxGuestJitByte(emitter, 0xc9);
	OsxGuestJitByte(emitter, 0x75);
	OsxGuestJitByte(emitter, (uint32)(loop - (emitter->cursor + 1)) & 0xff);
	return 1;
}

static void OsxGuestCallHelper(OsxNativeEmitter *emitter, uint64 function)
{
	emitter->bytemask = 0;
	OsxGuestCpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xec); OsxGuestJitByte(emitter, 0x20);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xb8);
	OsxGuestQuad(emitter, function);
	OsxGuestJitByte(emitter, 0xff); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 0x20);
	OsxGuestCpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
}

static void OsxGuestVectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xbb);
	OsxGuestWord(emitter, OSXJITOFFSET(altivec));
	OsxGuestJitByte(emitter, 0);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRVEC);
	OsxGuestMiss(emitter, 4, count);
}

static void OsxGuestVectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	OsxGuestVectorEnabled(emitter, count);
	OsxGuestRegisterOp(emitter, 0x89, 1, 3, 1);
	OsxGuestImmediate(emitter, 2, instruction);
	OsxGuestCallHelper(emitter, (uint64)(uintptr_t)function);
}

static void OsxGuestVectorMemory(OsxNativeEmitter *emitter, int store, int reg, uint32 offset)
{
	OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f);
	if (store)
		OsxGuestJitByte(emitter, 0x7f);
	else
		OsxGuestJitByte(emitter, 0x6f);
	OsxGuestJitByte(emitter, 0x83 | (reg << 3));
	OsxGuestWord(emitter, offset);
}

static void OsxGuestVectorOperation(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f);
	if (opcode > 255)
		OsxGuestJitByte(emitter, opcode >> 8);
	OsxGuestJitByte(emitter, opcode);
	OsxGuestJitByte(emitter, 0xc0 | (target << 3) | source);
}

static void OsxGuestVectorConstant(OsxNativeEmitter *emitter, int reg, int index)
{
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xb9);
	OsxGuestQuad(emitter, (uint64)(uintptr_t)emitter->vectorconstants[index]);
	OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6f);
	OsxGuestJitByte(emitter, 0x01 | (reg << 3));
}

static void OsxGuestVectorShift(OsxNativeEmitter *emitter, int opcode, int operation, int reg, uint32 amount)
{
	OsxGuestVectorOperation(emitter, (uint32)opcode, operation, reg);
	OsxGuestJitByte(emitter, amount);
}

static void OsxGuestVectorSaturated(OsxNativeEmitter *emitter, int reg)
{
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xd7);
	OsxGuestJitByte(emitter, 0xc0 | reg);
	OsxGuestImmediateOp(emitter, 7, 0, 0xffff);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x95); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xb6); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestCpuMemory(emitter, 0x09, 0, OSXJITOFFSET(vscr), 0);
}

static void OsxGuestVectorMultiply(OsxNativeEmitter *emitter, uint32 instruction)
{
	emitter->bytemask = 0;
	OsxGuestVectorOperation(emitter, 0x6f, 2, 0);
	OsxGuestVectorOperation(emitter, 0xd5, 2, 1);
	OsxGuestVectorOperation(emitter, 0xe5, 0, 1);
	OsxGuestVectorOperation(emitter, 0x6f, 1, 2);
	OsxGuestVectorOperation(emitter, 0x61, 2, 0);
	OsxGuestVectorOperation(emitter, 0x69, 1, 0);
	if ((instruction & 0x3f) == 33) {
		OsxGuestVectorConstant(emitter, 3, 4);
		OsxGuestVectorOperation(emitter, 0xfe, 2, 3);
		OsxGuestVectorOperation(emitter, 0xfe, 1, 3);
	}
	OsxGuestVectorShift(emitter, 0x72, 4, 2, 15);
	OsxGuestVectorShift(emitter, 0x72, 4, 1, 15);
	OsxGuestVectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
	OsxGuestVectorOperation(emitter, 0x6f, 3, 0);
	OsxGuestVectorOperation(emitter, 0x6f, 4, 0);
	OsxGuestVectorShift(emitter, 0x71, 4, 3, 15);
	OsxGuestVectorOperation(emitter, 0x61, 0, 3);
	OsxGuestVectorOperation(emitter, 0x69, 4, 3);
	OsxGuestVectorOperation(emitter, 0xfe, 2, 0);
	OsxGuestVectorOperation(emitter, 0xfe, 1, 4);
	OsxGuestVectorOperation(emitter, 0x6f, 5, 2);
	OsxGuestVectorOperation(emitter, 0x6b, 2, 1);
	OsxGuestVectorOperation(emitter, 0x6f, 0, 2);
	OsxGuestVectorOperation(emitter, 0x6f, 3, 2);
	OsxGuestVectorOperation(emitter, 0x6f, 4, 2);
	OsxGuestVectorShift(emitter, 0x71, 4, 3, 15);
	OsxGuestVectorOperation(emitter, 0x61, 0, 3);
	OsxGuestVectorOperation(emitter, 0x69, 4, 3);
	OsxGuestVectorOperation(emitter, 0x76, 0, 5);
	OsxGuestVectorOperation(emitter, 0x76, 4, 1);
	OsxGuestVectorOperation(emitter, 0xdb, 0, 4);
	OsxGuestVectorSaturated(emitter, 0);
	OsxGuestVectorOperation(emitter, 0x6f, 0, 2);
}

static int OsxGuestVectorNative(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
	OsxGuestVectorEnabled(emitter, count);
	if (kind == 524 || kind == 588 || kind == 652) {
		if (emitter->vectorvalue != right)
			OsxGuestVectorMemory(emitter, 0, 0, right);
		if (kind == 524)
			field = (field & 15) >> 2;
		else if (kind == 588)
			field = (field & 7) >> 1;
		else
			field &= 3;
		OsxGuestVectorOperation(emitter, 0x70, 0, 0);
		OsxGuestJitByte(emitter, field * 0x55);
		if (kind != 652) {
			field = (instruction >> 16) & 31;
			if (kind == 524) {
				OsxGuestVectorShift(emitter, 0x72, 2, 0, (3 - (field & 3)) * 8);
				OsxGuestVectorOperation(emitter, 0x60, 0, 0);
			} else {
				OsxGuestVectorShift(emitter, 0x72, 2, 0, (1 - (field & 1)) * 16);
			}
			OsxGuestVectorOperation(emitter, 0x61, 0, 0);
			OsxGuestVectorOperation(emitter, 0x70, 0, 0);
			OsxGuestJitByte(emitter, 0);
		}
	} else {
		if (emitter->vectorvalue == right)
			OsxGuestVectorOperation(emitter, 0x6f, 1, 0);
		else
			OsxGuestVectorMemory(emitter, 0, 1, right);
		if (emitter->vectorvalue != left)
			OsxGuestVectorMemory(emitter, 0, 0, left);
		if (form == 32 || form == 33) {
			OsxGuestVectorMultiply(emitter, instruction);
		} else if (kind == 270) {
			OsxGuestVectorOperation(emitter, 0x6f, 2, 0);
			OsxGuestVectorOperation(emitter, 0xeb, 2, 1);
			OsxGuestVectorShift(emitter, 0x71, 2, 2, 8);
			OsxGuestVectorOperation(emitter, 0xef, 3, 3);
			OsxGuestVectorOperation(emitter, 0x75, 2, 3);
			OsxGuestVectorSaturated(emitter, 2);
			OsxGuestVectorOperation(emitter, 0x67, 0, 1);
			OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x70); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 0xb1);
			OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x70); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 0xb1);
		} else if (opcode) {
			OsxGuestVectorOperation(emitter, opcode, 0, 1);
		} else if (kind == 1092) {
			OsxGuestVectorOperation(emitter, 0xdf, 1, 0);
			OsxGuestVectorOperation(emitter, 0x6f, 0, 1);
		} else if (kind == 1284) {
			OsxGuestVectorOperation(emitter, 0xeb, 0, 1);
			OsxGuestVectorOperation(emitter, 0x76, 1, 1);
			OsxGuestVectorOperation(emitter, 0xef, 0, 1);
		} else if (form == 42) {
			OsxGuestVectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
			OsxGuestVectorOperation(emitter, 0xdb, 1, 2);
			OsxGuestVectorOperation(emitter, 0xdf, 2, 0);
			OsxGuestVectorOperation(emitter, 0x6f, 0, 2);
			OsxGuestVectorOperation(emitter, 0xeb, 0, 1);
		} else if (form == 43) {
			OsxGuestVectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
			OsxGuestVectorConstant(emitter, 4, 1);
			OsxGuestVectorOperation(emitter, 0xdb, 2, 4);
			OsxGuestVectorConstant(emitter, 4, 2);
			OsxGuestVectorOperation(emitter, 0xef, 2, 4);
			OsxGuestVectorOperation(emitter, 0x6f, 3, 2);
			OsxGuestVectorConstant(emitter, 4, 3);
			OsxGuestVectorOperation(emitter, 0x64, 3, 4);
			OsxGuestVectorOperation(emitter, 0x6f, 4, 3);
			OsxGuestVectorOperation(emitter, 0xeb, 4, 2);
			OsxGuestVectorOperation(emitter, 0x3800, 0, 4);
			OsxGuestVectorOperation(emitter, 0x76, 4, 4);
			OsxGuestVectorOperation(emitter, 0xef, 3, 4);
			OsxGuestVectorOperation(emitter, 0xeb, 3, 2);
			OsxGuestVectorOperation(emitter, 0x3800, 1, 3);
			OsxGuestVectorOperation(emitter, 0xeb, 0, 1);
		} else {
			OsxGuestVectorConstant(emitter, 2, 0);
			OsxGuestVectorOperation(emitter, 0x3800, 0, 2);
			OsxGuestVectorOperation(emitter, 0x3800, 1, 2);
			if (form == 44) {
				OsxGuestVectorOperation(emitter, 0x3a0f, 1, 0);
				OsxGuestJitByte(emitter, (instruction >> 6) & 15);
				OsxGuestVectorOperation(emitter, 0x6f, 0, 1);
			} else {
				opcode = 0x60 + ((kind >> 6) & 3);
				if (kind & 0x100)
					opcode += 8;
				OsxGuestVectorOperation(emitter, opcode, 0, 1);
			}
			OsxGuestVectorOperation(emitter, 0x3800, 0, 2);
		}
	}
	OsxGuestVectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void OsxGuestVectorLanes(OsxNativeEmitter *emitter, uint32 kind, uint32 target, uint32 left, uint32 right)
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
			OsxGuestCpuMemory(emitter, 0x8b, 0, left + offset, 0);
			OsxGuestCpuMemory(emitter, 0x8b, 1, right + offset, 0);
		} else {
			OsxGuestJitByte(emitter, 0x0f);
			if (width == 2 && operation == 0xf8)
				OsxGuestJitByte(emitter, 0xbf);
			else if (width == 2)
				OsxGuestJitByte(emitter, 0xb7);
			else if (operation == 0xf8)
				OsxGuestJitByte(emitter, 0xbe);
			else
				OsxGuestJitByte(emitter, 0xb6);
			OsxGuestJitByte(emitter, 0x83);
			OsxGuestWord(emitter, left + offset);
			OsxGuestJitByte(emitter, 0x0f);
			if (width == 2)
				OsxGuestJitByte(emitter, 0xb7);
			else
				OsxGuestJitByte(emitter, 0xb6);
			OsxGuestJitByte(emitter, 0x8b);
			OsxGuestWord(emitter, right + offset);
		}
		OsxGuestImmediateOp(emitter, 4, 1, width * 8 - 1);
		OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, operation);
		if (width == 2)
			OsxGuestJitByte(emitter, 0x66);
		if (width == 1)
			OsxGuestJitByte(emitter, 0x88);
		else
			OsxGuestJitByte(emitter, 0x89);
		OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, target + offset);
	}
}

static void OsxGuestVectorSumAcross(OsxNativeEmitter *emitter, uint32 target, uint32 left, uint32 right)
{
	uint32 word;
	uint8 *inside;
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x63); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, right + 12);
	for (word = 0; word < 4; word++) {
		OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x63); OsxGuestJitByte(emitter, 0x8b);
		OsxGuestWord(emitter, left + word * 4);
		OsxGuestRegisterOp(emitter, 0x01, 0, 1, 1);
	}
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x63); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestRegisterOp(emitter, 0x39, 1, 0, 1);
	inside = OsxGuestJump(emitter, 4);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xf8); OsxGuestJitByte(emitter, 63);
	OsxGuestImmediateOp(emitter, 6, 0, 0x7fffffffu);
	OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0x8b);
	OsxGuestWord(emitter, OSXJITOFFSET(vscr));
	OsxGuestJitByte(emitter, 1);
	OsxGuestPatch(inside, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x89, 0, target + 12, 0);
	for (word = 0; word < 3; word++) {
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, target + word * 4);
		OsxGuestWord(emitter, 0);
	}
}

static void OsxGuestVectorAllOnes(OsxNativeEmitter *emitter, int reg)
{
	OsxGuestVectorOperation(emitter, 0x75, reg, reg);
}

static void OsxGuestVectorFloatOperation(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	OsxGuestJitByte(emitter, 0x0f);
	OsxGuestJitByte(emitter, opcode);
	OsxGuestJitByte(emitter, 0xc0 | (target << 3) | source);
}

static int OsxGuestVectorFloat(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
	OsxGuestVectorEnabled(emitter, count);
	emitter->bytemask = 0;
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	OsxGuestVectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16);
	OsxGuestVectorMemory(emitter, 0, 1, OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16);
	if (form == 46 || form == 47)
		OsxGuestVectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
	for (reg = 0; form != 6 && kind != 842 && reg < 2 + (form == 46 || form == 47); reg++) {
		OsxGuestVectorOperation(emitter, 0x6f, 3, reg);
		OsxGuestVectorFloatOperation(emitter, 0xc2, 3, reg);
		OsxGuestJitByte(emitter, 3);
		OsxGuestVectorFloatOperation(emitter, 0x50, 0, 3);
		OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
		OsxGuestMiss(emitter, 5, count);
	}
	if (form == 46 || form == 47) {
		for (reg = 0; reg < 3; reg++) {
			OsxGuestVectorFloatOperation(emitter, 0x12, reg + 3, reg);
			OsxGuestVectorFloatOperation(emitter, 0x5a, reg, reg);
			OsxGuestVectorFloatOperation(emitter, 0x5a, reg + 3, reg + 3);
		}
		OsxGuestVectorOperation(emitter, 0x59, 0, 2);
		OsxGuestVectorOperation(emitter, 0x59, 3, 5);
		operation = 0x58;
		if (form == 47)
			operation = 0x5c;
		OsxGuestVectorOperation(emitter, operation, 0, 1);
		OsxGuestVectorOperation(emitter, operation, 3, 4);
		if (form == 47) {
			OsxGuestVectorAllOnes(emitter, 2);
			OsxGuestVectorShift(emitter, 0x73, 6, 2, 63);
			OsxGuestVectorOperation(emitter, 0xef, 0, 2);
			OsxGuestVectorOperation(emitter, 0xef, 3, 2);
		}
		OsxGuestVectorOperation(emitter, 0x5a, 0, 0);
		OsxGuestVectorOperation(emitter, 0x5a, 3, 3);
		OsxGuestVectorFloatOperation(emitter, 0x16, 0, 3);
	} else if (kind == 266 || kind == 842) {
		operation = 0x3f800000;
		if (kind == 842)
			operation -= ((instruction >> 16) & 31) << 23;
		OsxGuestImmediate(emitter, 0, operation);
		OsxGuestVectorOperation(emitter, 0x6e, 0, 0);
		OsxGuestVectorOperation(emitter, 0x70, 0, 0);
		OsxGuestJitByte(emitter, 0);
		if (kind == 266)
			OsxGuestVectorFloatOperation(emitter, 0x5e, 0, 1);
		else {
			OsxGuestVectorFloatOperation(emitter, 0x5b, 1, 1);
			OsxGuestVectorFloatOperation(emitter, 0x59, 0, 1);
		}
	} else if (kind == 10 || kind == 74 || kind == 1034 || kind == 1098) {
		operation = 0x58;
		if (kind == 74)
			operation = 0x5c;
		else if (kind == 1034)
			operation = 0x5f;
		else if (kind == 1098)
			operation = 0x5d;
		OsxGuestVectorFloatOperation(emitter, operation, 0, 1);
	} else {
		operation = 0;
		if ((kind & 0x3ff) == 454)
			operation = 2;
		else if ((kind & 0x3ff) == 710)
			operation = 1;
		OsxGuestVectorFloatOperation(emitter, 0xc2, 1, 0);
		OsxGuestJitByte(emitter, operation);
		OsxGuestVectorOperation(emitter, 0x6f, 0, 1);
		if (kind & 0x400) {
			OsxGuestVectorFloatOperation(emitter, 0x50, 0, 0);
			OsxGuestImmediateOp(emitter, 7, 0, 15);
			OsxGuestSetcc(emitter, 4, 1);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xb6); OsxGuestJitByte(emitter, 0xc9);
			OsxGuestShift(emitter, 4, 1, 7);
			OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
			OsxGuestSetcc(emitter, 4, 0);
			OsxGuestShift(emitter, 4, 0, 5);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
			OsxGuestImmediateOp(emitter, 4, 1, ~0xf0u);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		}
	}
	OsxGuestVectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void OsxGuestVectorBroadcast(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	OsxGuestImmediate(emitter, 0, value);
	OsxGuestVectorOperation(emitter, 0x6e, reg, 0);
	OsxGuestVectorOperation(emitter, 0x70, reg, reg);
	OsxGuestJitByte(emitter, 0);
}

static void OsxGuestVectorTruncate(OsxNativeEmitter *emitter, int target, int source)
{
	OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5b);
	OsxGuestJitByte(emitter, 0xc0 | (target << 3) | source);
}

static void OsxGuestVectorCompare(OsxNativeEmitter *emitter, int target, int source, int predicate)
{
	OsxGuestVectorFloatOperation(emitter, 0xc2, target, source);
	OsxGuestJitByte(emitter, predicate);
}

static void OsxGuestVectorSaturationMask(OsxNativeEmitter *emitter, int mask, int other, int zero)
{
	OsxGuestVectorOperation(emitter, 0xeb, mask, other);
	OsxGuestVectorOperation(emitter, 0xef, zero, zero);
	OsxGuestVectorOperation(emitter, 0x76, mask, zero);
	OsxGuestVectorSaturated(emitter, mask);
}

static int OsxGuestVectorConvert(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
	OsxGuestVectorEnabled(emitter, count);
	emitter->bytemask = 0;
	target = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	scale = ((instruction >> 16) & 31) << 23;
	OsxGuestVectorMemory(emitter, 0, 1, OSXJITOFFSET(vr) + ((instruction >> 11) & 31) * 16);
	if (kind == 78) {
		OsxGuestVectorMemory(emitter, 0, 0, OSXJITOFFSET(vr) + ((instruction >> 16) & 31) * 16);
		OsxGuestVectorShift(emitter, 0x72, 6, 0, 16);
		OsxGuestVectorShift(emitter, 0x72, 4, 0, 16);
		OsxGuestVectorShift(emitter, 0x72, 6, 1, 16);
		OsxGuestVectorShift(emitter, 0x72, 4, 1, 16);
		OsxGuestVectorOperation(emitter, 0x6b, 0, 1);
		OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x70); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 0xb1);
		OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x70); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 0xb1);
	} else if (kind == 778) {
		OsxGuestVectorOperation(emitter, 0x6f, 0, 1);
		OsxGuestVectorShift(emitter, 0x72, 2, 0, 16);
		OsxGuestVectorShift(emitter, 0x72, 6, 1, 16);
		OsxGuestVectorShift(emitter, 0x72, 2, 1, 16);
		OsxGuestVectorFloatOperation(emitter, 0x5b, 0, 0);
		OsxGuestVectorFloatOperation(emitter, 0x5b, 1, 1);
		OsxGuestVectorBroadcast(emitter, 2, 0x47800000u);
		OsxGuestVectorFloatOperation(emitter, 0x59, 0, 2);
		OsxGuestVectorFloatOperation(emitter, 0x58, 0, 1);
		if (scale) {
			OsxGuestVectorBroadcast(emitter, 2, 0x3f800000u - scale);
			OsxGuestVectorFloatOperation(emitter, 0x59, 0, 2);
		}
	} else if (kind == 906 || kind == 970) {
		if (scale) {
			OsxGuestVectorBroadcast(emitter, 2, 0x3f800000u + scale);
			OsxGuestVectorFloatOperation(emitter, 0x59, 1, 2);
		}
		if (kind == 970) {
			OsxGuestVectorFloatOperation(emitter, 0x28, 3, 1);
			OsxGuestVectorCompare(emitter, 3, 1, 7);
			OsxGuestVectorBroadcast(emitter, 4, 0x4f000000u);
			OsxGuestVectorCompare(emitter, 4, 1, 2);
			OsxGuestVectorBroadcast(emitter, 2, 0xcf000000u);
			OsxGuestVectorFloatOperation(emitter, 0x28, 5, 1);
			OsxGuestVectorCompare(emitter, 5, 2, 1);
			OsxGuestVectorTruncate(emitter, 0, 1);
			OsxGuestVectorOperation(emitter, 0xef, 0, 4);
			OsxGuestVectorOperation(emitter, 0xdb, 0, 3);
		} else {
			OsxGuestVectorBroadcast(emitter, 2, 0xbf800000u);
			OsxGuestVectorFloatOperation(emitter, 0x28, 5, 1);
			OsxGuestVectorCompare(emitter, 5, 2, 2);
			OsxGuestVectorBroadcast(emitter, 4, 0x4f800000u);
			OsxGuestVectorCompare(emitter, 4, 1, 2);
			OsxGuestVectorOperation(emitter, 0xef, 3, 3);
			OsxGuestVectorFloatOperation(emitter, 0x5f, 1, 3);
			OsxGuestVectorBroadcast(emitter, 2, 0x4f000000u);
			OsxGuestVectorFloatOperation(emitter, 0x28, 3, 2);
			OsxGuestVectorCompare(emitter, 3, 1, 2);
			OsxGuestVectorFloatOperation(emitter, 0x54, 2, 3);
			OsxGuestVectorFloatOperation(emitter, 0x5c, 1, 2);
			OsxGuestVectorTruncate(emitter, 0, 1);
			OsxGuestVectorShift(emitter, 0x72, 6, 3, 31);
			OsxGuestVectorOperation(emitter, 0xef, 0, 3);
			OsxGuestVectorOperation(emitter, 0xeb, 0, 4);
		}
		OsxGuestVectorSaturationMask(emitter, 4, 5, 3);
	} else {
		mode = 1;
		if (kind == 522)
			mode = 0;
		else if (kind == 586)
			mode = 3;
		else if (kind == 650)
			mode = 2;
		OsxGuestVectorOperation(emitter, 0x3a08, 0, 1);
		OsxGuestJitByte(emitter, mode | 8);
	}
	OsxGuestVectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static int OsxGuestVectorExtra(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestVectorEnabled(emitter, count);
			emitter->vectorvalue = 0;
			if (kind == 1928) {
				OsxGuestVectorSumAcross(emitter, target, left, right);
				return 1;
			}
			if (kind == 780 || kind == 844 || kind == 908) {
				value = (uint32)((int32)(((instruction >> 16) & 31) << 27) >> 27);
				if (kind == 780)
					value = (value & 0xff) * 0x01010101u;
				else if (kind == 844)
					value = (value & 0xffff) * 0x00010001u;
				for (word = 0; word < 4; word++) {
					OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
					OsxGuestWord(emitter, target + word * 4);
					OsxGuestWord(emitter, value);
				}
				return 1;
			}
			OsxGuestVectorLanes(emitter, kind, target, left, right);
			return 1;
		default:
			return 0;
		}
	}
	OsxGuestVectorEnabled(emitter, count);
	if (emitter->vectorvalue == right)
		OsxGuestVectorOperation(emitter, 0x6f, 1, 0);
	else
		OsxGuestVectorMemory(emitter, 0, 1, right);
	if (emitter->vectorvalue != left)
		OsxGuestVectorMemory(emitter, 0, 0, left);
	if (form == 40 || form == 34) {
		if (form == 40)
			OsxGuestVectorOperation(emitter, 0xf5, 0, 1);
		else
			OsxGuestVectorOperation(emitter, 0xd5, 0, 1);
		OsxGuestVectorMemory(emitter, 0, 2, OSXJITOFFSET(vr) + ((instruction >> 6) & 31) * 16);
		if (form == 40)
			OsxGuestVectorOperation(emitter, 0xfe, 0, 2);
		else
			OsxGuestVectorOperation(emitter, 0xfd, 0, 2);
	} else if (saturate) {
		OsxGuestVectorOperation(emitter, 0x6f, 2, 0);
		OsxGuestVectorOperation(emitter, modular, 2, 1);
		OsxGuestVectorOperation(emitter, saturate, 0, 1);
		if (modular == 0xfc || modular == 0xf8)
			OsxGuestVectorOperation(emitter, 0x74, 2, 0);
		else
			OsxGuestVectorOperation(emitter, 0x75, 2, 0);
		OsxGuestVectorSaturated(emitter, 2);
	} else if (simple) {
		OsxGuestVectorOperation(emitter, simple, 0, 1);
	} else if (kind == 386) {
		OsxGuestVectorOperation(emitter, 0x6f, 2, 0);
		OsxGuestVectorOperation(emitter, 0x66, 2, 1);
		OsxGuestVectorOperation(emitter, 0xdb, 0, 2);
		OsxGuestVectorOperation(emitter, 0xdf, 2, 1);
		OsxGuestVectorOperation(emitter, 0xeb, 0, 2);
	} else if (kind == 898) {
		OsxGuestVectorOperation(emitter, 0x6f, 2, 1);
		OsxGuestVectorOperation(emitter, 0x66, 2, 0);
		OsxGuestVectorOperation(emitter, 0xdb, 0, 2);
		OsxGuestVectorOperation(emitter, 0xdf, 2, 1);
		OsxGuestVectorOperation(emitter, 0xeb, 0, 2);
	} else if (kind == 1346) {
		OsxGuestVectorAllOnes(emitter, 2);
		OsxGuestVectorShift(emitter, 0x71, 6, 2, 15);
		OsxGuestVectorOperation(emitter, 0xef, 0, 2);
		OsxGuestVectorOperation(emitter, 0xef, 1, 2);
		OsxGuestVectorOperation(emitter, 0xe3, 0, 1);
		OsxGuestVectorOperation(emitter, 0xef, 0, 2);
	} else if (kind == 520) {
		OsxGuestVectorShift(emitter, 0x71, 2, 0, 8);
		OsxGuestVectorShift(emitter, 0x71, 2, 1, 8);
		OsxGuestVectorOperation(emitter, 0xd5, 0, 1);
	} else if (kind == 8) {
		OsxGuestVectorAllOnes(emitter, 2);
		OsxGuestVectorShift(emitter, 0x71, 2, 2, 8);
		OsxGuestVectorOperation(emitter, 0xdb, 0, 2);
		OsxGuestVectorOperation(emitter, 0xdb, 1, 2);
		OsxGuestVectorOperation(emitter, 0xd5, 0, 1);
	} else if (kind == 776 || kind == 264) {
		if (kind == 264) {
			OsxGuestVectorShift(emitter, 0x71, 6, 0, 8);
			OsxGuestVectorShift(emitter, 0x71, 6, 1, 8);
		}
		OsxGuestVectorShift(emitter, 0x71, 4, 0, 8);
		OsxGuestVectorShift(emitter, 0x71, 4, 1, 8);
		OsxGuestVectorOperation(emitter, 0xd5, 0, 1);
	} else {
		if (kind == 526)
			OsxGuestVectorConstant(emitter, 2, 5);
		else if (kind == 654)
			OsxGuestVectorConstant(emitter, 2, 6);
		else if (kind == 590)
			OsxGuestVectorConstant(emitter, 2, 7);
		else
			OsxGuestVectorConstant(emitter, 2, 8);
		OsxGuestVectorOperation(emitter, 0x6f, 0, 1);
		OsxGuestVectorOperation(emitter, 0x3800, 0, 2);
		if (kind == 526 || kind == 654)
			OsxGuestVectorShift(emitter, 0x71, 4, 0, 8);
		else
			OsxGuestVectorShift(emitter, 0x72, 4, 0, 16);
	}
	OsxGuestVectorMemory(emitter, 1, 0, target);
	emitter->vectorvalue = target;
	return 1;
}

static void OsxGuestVectorQuad(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint32 vector;
	uint32 word;
	int store;
	vector = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	store = (instruction & 0x100) != 0;
	OsxGuestVectorEnabled(emitter, count);
	OsxGuestAddress(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	OsxGuestImmediateOp(emitter, 4, 0, 0xfffffff0);
	OsxGuestTranslate(emitter, store, 0xff0, count);
	if (emitter->shuffle) {
		if (!emitter->bytemask) {
			emitter->bytemask = 1;
			OsxGuestVectorConstant(emitter, 5, 0);
		}
		if (store) {
			OsxGuestVectorMemory(emitter, 0, 0, vector);
			OsxGuestVectorOperation(emitter, 0x3800, 0, 5);
			OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7f);
			OsxGuestJitByte(emitter, 0x04); OsxGuestJitByte(emitter, 0x00);
		} else {
			OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6f);
			OsxGuestJitByte(emitter, 0x04); OsxGuestJitByte(emitter, 0x00);
			OsxGuestVectorOperation(emitter, 0x3800, 0, 5);
			OsxGuestVectorMemory(emitter, 1, 0, vector);
		}
		return;
	}
	for (word = 0; word < 4; word++) {
		if (store) {
			OsxGuestCpuMemory(emitter, 0x8b, 2, vector + word * 4, 0);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x54); OsxGuestJitByte(emitter, 0x00); OsxGuestJitByte(emitter, word * 4);
		} else {
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x54); OsxGuestJitByte(emitter, 0x00); OsxGuestJitByte(emitter, word * 4);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
			OsxGuestCpuMemory(emitter, 0x89, 2, vector + word * 4, 0);
		}
	}
}

static void OsxGuestVectorElement(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	uint32 vector;
	uint32 word;
	vector = OSXJITOFFSET(vr) + ((instruction >> 21) & 31) * 16;
	OsxGuestVectorEnabled(emitter, count);
	OsxGuestAddress(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	if (extended == 6 || extended == 38) {
		OsxGuestImmediateOp(emitter, 4, 0, 15);
		if (extended == 38) {
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd8);
			OsxGuestImmediateOp(emitter, 0, 0, 16);
		}
		OsxGuestJitByte(emitter, 0x69); OsxGuestJitByte(emitter, 0xc0);
		OsxGuestWord(emitter, 0x01010101u);
		for (word = 0; word < 4; word++) {
			OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0x90);
			OsxGuestWord(emitter, 0x00010203u + word * 0x04040404u);
			OsxGuestCpuMemory(emitter, 0x89, 2, vector + word * 4, 0);
		}
		return;
	}
	OsxGuestImmediateOp(emitter, 4, 0, 0xfffffffcu);
	OsxGuestTranslate(emitter, extended == 199, 0xffc, count);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
	OsxGuestImmediateOp(emitter, 4, 1, 12);
	if (extended == 199) {
		OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x94); OsxGuestJitByte(emitter, 0x0b);
		OsxGuestWord(emitter, vector);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
	} else {
		OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x94); OsxGuestJitByte(emitter, 0x0b);
		OsxGuestWord(emitter, vector);
	}
}

static void OsxGuestMemory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
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
		OsxGuestRequireFloat(emitter, count);
	if (single && store) {
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITFPR(target), 1);
		OsxGuestCallHelper(emitter, (uint64)(uintptr_t)OsxGuestCpuDoubleToSingle);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(jitscratch), 0);
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	OsxGuestAddress(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		OsxGuestRegisterOp(emitter, 0x89, 10, 0, 0);
	OsxGuestTranslate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITFPR(target), 1);
			OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
			OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
		} else if (single) {
			OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(jitscratch), 0);
		} else {
			OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITGPR(target), 0);
		}
		if (opcode == 36 || opcode == 52) {
			if (!reversed) {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
			}
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
		} else if (opcode == 44) {
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xca); OsxGuestJitByte(emitter, 8);
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
		} else if (!wide) {
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x88); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x00);
		}
	} else {
		if (wide) {
			OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x04); OsxGuestJitByte(emitter, 0x00);
			OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc8);
		} else if (opcode == 32 || single) {
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x04); OsxGuestJitByte(emitter, 0x00);
			if (!reversed) {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc8);
			}
		} else {
			OsxGuestJitByte(emitter, 0x41); OsxGuestJitByte(emitter, 0x0f);
			if (opcode == 34)
				OsxGuestJitByte(emitter, 0xb6);
			else
				OsxGuestJitByte(emitter, 0xb7);
			OsxGuestJitByte(emitter, 0x04); OsxGuestJitByte(emitter, 0x00);
			if (opcode != 34) {
				OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xc8); OsxGuestJitByte(emitter, 8);
				if (opcode == 42) {
					OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xbf); OsxGuestJitByte(emitter, 0xc0);
				}
			}
		}
		if (single) {
			if (update)
				OsxGuestCpuMemory(emitter, 0x89, 10, OSXJITGPR(base), 0);
			update = 0;
			OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
			OsxGuestCallHelper(emitter, (uint64)(uintptr_t)OsxGuestCpuSingleToDouble);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
		} else if (wide) {
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target), 1);
		} else {
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		}
	}
	if (update)
		OsxGuestCpuMemory(emitter, 0x89, 10, OSXJITGPR(base), 0);
}

static int OsxGuestBranch(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(ctr), 0);
			options |= 4;
		} else {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(lr), 0);
		}
		OsxGuestImmediateOp(emitter, 4, 1, ~3u);
	} else {
		if (opcode == 18) {
			displacement = instruction & 0x03fffffc;
			if (displacement & 0x02000000)
				displacement |= 0xfc000000;
			options = 20;
		} else {
			displacement = (uint32)(int32)(int16)(instruction & 0xfffc);
		}
		OsxGuestImmediate(emitter, 1, displacement);
		if (!(instruction & 2)) {
			OsxGuestRegisterOp(emitter, 0x01, 1, 11, 0);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		OsxGuestRegisterOp(emitter, 0x89, 2, 11, 0);
		OsxGuestImmediateOp(emitter, 0, 2, 4);
		OsxGuestCpuMemory(emitter, 0x89, 2, OSXJITOFFSET(lr), 0);
	}
	if (!(options & 4)) {
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(ctr), 0);
		OsxGuestImmediateOp(emitter, 5, 0, 1);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(ctr), 0);
		if (options & 2)
			counterpatch = OsxGuestJump(emitter, 5);
		else
			counterpatch = OsxGuestJump(emitter, 4);
	}
	if (!(options & 16)) {
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(cr), 0);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestWord(emitter, 1u << (31 - ((instruction >> 16) & 31)));
		if (options & 8)
			conditionpatch = OsxGuestJump(emitter, 4);
		else
			conditionpatch = OsxGuestJump(emitter, 5);
	}
	OsxGuestRegisterOp(emitter, 0x89, 11, 1, 0);
	OsxGuestExit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			OsxGuestPatch(counterpatch, emitter->cursor);
		if (conditionpatch)
			OsxGuestPatch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void OsxGuestLoadCarry(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xba); OsxGuestJitByte(emitter, 0xa3);
	OsxGuestWord(emitter, OSXJITOFFSET(xer));
	OsxGuestJitByte(emitter, 29);
}

static void OsxGuestStoreCarry(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xb6); OsxGuestJitByte(emitter, 0xd2);
	OsxGuestShift(emitter, 4, 2, 29);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(xer), 0);
	OsxGuestImmediateOp(emitter, 4, 1, 0xdfffffff);
	OsxGuestRegisterOp(emitter, 0x09, 1, 2, 0);
	OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(xer), 0);
}

static void OsxGuestRequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRPR);
	OsxGuestMiss(emitter, 5, count);
}

static int OsxGuestCarrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(right), 0);
		OsxGuestSetcc(emitter, 2, 2);
		break;
	case 8:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(right), 0);
		OsxGuestCpuMemory(emitter, 0x2b, 0, OSXJITGPR(left), 0);
		OsxGuestSetcc(emitter, 3, 2);
		break;
	case 138:
	case 136:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 136) {
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
		}
		OsxGuestLoadCarry(emitter);
		OsxGuestCpuMemory(emitter, 0x13, 0, OSXJITGPR(right), 0);
		OsxGuestSetcc(emitter, 2, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 200 || extended == 232) {
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
		}
		OsxGuestLoadCarry(emitter);
		if (extended == 202 || extended == 200)
			OsxGuestImmediateOp(emitter, 2, 0, 0);
		else
			OsxGuestImmediateOp(emitter, 2, 0, 0xffffffff);
		OsxGuestSetcc(emitter, 2, 2);
		break;
	default:
		return 0;
	}
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
	OsxGuestStoreCarry(emitter);
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestArithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd8);
		break;
	case 11:
	case 75:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		OsxGuestJitByte(emitter, 0xf7);
		if (extended == 11)
			OsxGuestJitByte(emitter, 0xa3);
		else
			OsxGuestJitByte(emitter, 0xab);
		OsxGuestWord(emitter, OSXJITGPR(right));
		OsxGuestRegisterOp(emitter, 0x89, 0, 2, 0);
		break;
	case 459:
	case 491:
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(right), 0);
		OsxGuestRegisterOp(emitter, 0x85, 1, 1, 0);
		OsxGuestMiss(emitter, 4, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left), 0);
		if (extended == 491) {
			OsxGuestImmediateOp(emitter, 7, 1, 0xffffffff);
			ordinary = OsxGuestJump(emitter, 5);
			OsxGuestImmediateOp(emitter, 7, 0, 0x80000000);
			OsxGuestMiss(emitter, 4, count);
			OsxGuestPatch(ordinary, emitter->cursor);
			OsxGuestJitByte(emitter, 0x99);
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xf9);
		} else {
			OsxGuestRegisterOp(emitter, 0x31, 2, 2, 0);
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xf1);
		}
		break;
	default:
		return 0;
	}
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestLogical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
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
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount), 0);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		OsxGuestImmediateOp(emitter, 4, 1, 63);
		OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xd3);
		if (extended == 24)
			OsxGuestJitByte(emitter, 0xe0);
		else
			OsxGuestJitByte(emitter, 0xe8);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		break;
	case 26:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		OsxGuestImmediate(emitter, 2, 63);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xbd); OsxGuestJitByte(emitter, 0xc0);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x44); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestImmediateOp(emitter, 6, 0, 31);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		break;
	case 792:
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount), 0);
		OsxGuestImmediateOp(emitter, 4, 1, 63);
		OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x63); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITGPR(source));
		OsxGuestRegisterOp(emitter, 0x89, 2, 0, 1);
		OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xf8);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		OsxGuestRegisterOp(emitter, 0x89, 9, 0, 1);
		OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xe1);
		OsxGuestRegisterOp(emitter, 0x39, 9, 2, 1);
		OsxGuestSetcc(emitter, 5, 1);
		OsxGuestRegisterOp(emitter, 0x85, 2, 2, 1);
		OsxGuestSetcc(emitter, 8, 2);
		OsxGuestJitByte(emitter, 0x20); OsxGuestJitByte(emitter, 0xca);
		OsxGuestStoreCarry(emitter);
		break;
	case 824:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source), 0);
		if (amount == 0) {
			OsxGuestRegisterOp(emitter, 0x31, 2, 2, 0);
		} else {
			OsxGuestRegisterOp(emitter, 0x89, 2, 0, 0);
			OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
			OsxGuestImmediateOp(emitter, 4, 1, (1u << amount) - 1);
			OsxGuestSetcc(emitter, 5, 1);
			OsxGuestShift(emitter, 5, 2, 31);
			OsxGuestJitByte(emitter, 0x20); OsxGuestJitByte(emitter, 0xca);
			OsxGuestShift(emitter, 7, 0, (uint32)amount);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		OsxGuestStoreCarry(emitter);
		break;
	default:
		return 0;
	}
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestSpecialOffset(uint32 number, uint32 *offset)
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

static void OsxGuestCallout(OsxNativeEmitter *emitter, uint32 instruction, int count);

static int OsxGuestSystem(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (mask != 0xffffffffu) {
			OsxGuestImmediateOp(emitter, 4, 0, mask);
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
			OsxGuestImmediateOp(emitter, 4, 1, ~mask);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		return 1;
	case 83:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(msr), 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 146:
		emitter->enabled = 0;
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(target), 0);
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(msr), 0);
		OsxGuestRegisterOp(emitter, 0x31, 2, 1, 0);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestWord(emitter, ~(uint32)OSXMSREE);
		slowmask = OsxGuestJump(emitter, 5);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc1);
		OsxGuestWord(emitter, OSXMSRPOW);
		slowpower = OsxGuestJump(emitter, 5);
		OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(msr), 0);
		OsxGuestRegisterOp(emitter, 0x21, 2, 1, 0);
		quiet = OsxGuestJump(emitter, 4);
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(externalline), 0);
		OsxGuestCpuMemory(emitter, 0x0b, 2, OSXJITOFFSET(decpending), 0);
		pending = OsxGuestJump(emitter, 4);
		OsxGuestAdvance(emitter);
		OsxGuestAccount(emitter, count + 1);
		OsxGuestSettle(emitter);
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(slice));
		OsxGuestWord(emitter, 0);
		OsxGuestRelease(emitter, OSXJITCONTINUE);
		OsxGuestPatch(quiet, emitter->cursor);
		OsxGuestPatch(pending, emitter->cursor);
		OsxGuestJitByte(emitter, 0xe9);
		done = emitter->cursor;
		OsxGuestWord(emitter, 0);
		OsxGuestPatch(slowmask, emitter->cursor);
		OsxGuestPatch(slowpower, emitter->cursor);
		OsxGuestCallout(emitter, instruction, count);
		OsxGuestPatch(done, emitter->cursor);
		return 1;
	case 595:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		emitter->bytemask = 0;
		OsxGuestJitByte(emitter, 0xe8);
		OsxGuestJumpTo(emitter, emitter->timethunk);
		if (number == 269) {
			OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xe8); OsxGuestJitByte(emitter, 32);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 210:
	case 242:
#if OSXJITSLOWPATH
		emitter->slowactive = 1;
#endif
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 210)
			OsxGuestImmediate(emitter, 0, (instruction >> 16) & 15);
		else {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR((instruction >> 11) & 31), 0);
			OsxGuestShift(emitter, 5, 0, 28);
		}
		OsxGuestRegisterOp(emitter, 0x89, 1, 11, 0);
		OsxGuestShift(emitter, 5, 1, 28);
		OsxGuestRegisterOp(emitter, 0x39, 1, 0, 0);
		OsxGuestMiss(emitter, 4, count);
#if OSXJITSLOWPATH
		emitter->slowactive = 0;
#endif
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITGPR(target), 0);
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x94); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(sr));
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(fetchtag));
		OsxGuestWord(emitter, 0);
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!OsxGuestSpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			OsxGuestCallout(emitter, instruction, count);
			return 1;
		}
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 339) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, offset, 0);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		} else {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			OsxGuestCpuMemory(emitter, 0x89, 0, offset, 0);
		}
		return 1;
	}
	return 0;
}

static void OsxGuestConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(xer), 0);
	OsxGuestShift(emitter, 5, 0, 31);
}

static void OsxGuestStoreConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestShift(emitter, 4, 0, 28);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr), 0);
	OsxGuestImmediateOp(emitter, 4, 1, 0x0fffffff);
	OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
}

static int OsxGuestReserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	OsxGuestAddress(emitter, base, index, 0, 0);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 3);
	OsxGuestMiss(emitter, 5, count);
	if (extended == 20) {
		OsxGuestRegisterOp(emitter, 0x89, 10, 0, 0);
		OsxGuestMemory(emitter, instruction, 32, index, count);
		OsxGuestCpuMemory(emitter, 0x89, 10, OSXJITOFFSET(reservation), 0);
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
		OsxGuestWord(emitter, 1);
		return 1;
	}
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xbb);
	OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
	OsxGuestWord(emitter, 0);
	invalid = OsxGuestJump(emitter, 4);
	OsxGuestCpuMemory(emitter, 0x39, 0, OSXJITOFFSET(reservation), 0);
	elsewhere = OsxGuestJump(emitter, 5);
	OsxGuestMemory(emitter, instruction, 36, index, count);
	OsxGuestConditionZero(emitter);
	OsxGuestImmediateOp(emitter, 1, 0, 2);
	OsxGuestJitByte(emitter, 0xe9);
	done = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(invalid, emitter->cursor);
	OsxGuestPatch(elsewhere, emitter->cursor);
	OsxGuestConditionZero(emitter);
	OsxGuestPatch(done, emitter->cursor);
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
	OsxGuestWord(emitter, 0);
	OsxGuestStoreConditionZero(emitter);
	return 1;
}

static int OsxGuestZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	int offset;
	OsxGuestAddress(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	OsxGuestImmediateOp(emitter, 4, 0, ~31u);
	OsxGuestTranslate(emitter, 1, 0xfe0, count);
	OsxGuestRegisterOp(emitter, 0x31, 2, 2, 0);
	for (offset = 0; offset < 32; offset += 8) {
		OsxGuestJitByte(emitter, 0x49); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x54); OsxGuestJitByte(emitter, 0x00);
		OsxGuestJitByte(emitter, (uint32)offset);
	}
	return 1;
}

static int OsxGuestConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
		OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
		OsxGuestShift(emitter, 5, 1, (7 - (bita >> 2)) * 4);
		OsxGuestImmediateOp(emitter, 4, 1, 0xf);
		OsxGuestShift(emitter, 4, 1, (7 - (bitd >> 2)) * 4);
		OsxGuestImmediateOp(emitter, 4, 0, ~(0xfu << ((7 - (bitd >> 2)) * 4)));
		OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0, 0);
	OsxGuestShift(emitter, 5, 1, 31 - bita);
	OsxGuestImmediateOp(emitter, 4, 1, 1);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0, 0);
	OsxGuestShift(emitter, 5, 2, 31 - bitb);
	OsxGuestImmediateOp(emitter, 4, 2, 1);
	if (extended == 129 || extended == 417)
		OsxGuestImmediateOp(emitter, 6, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		OsxGuestRegisterOp(emitter, 0x09, 1, 2, 0);
	else if (extended == 129 || extended == 225 || extended == 257)
		OsxGuestRegisterOp(emitter, 0x21, 1, 2, 0);
	else
		OsxGuestRegisterOp(emitter, 0x31, 1, 2, 0);
	if (extended == 33 || extended == 225 || extended == 289)
		OsxGuestImmediateOp(emitter, 6, 1, 1);
	OsxGuestShift(emitter, 4, 1, 31 - bitd);
	OsxGuestImmediateOp(emitter, 4, 0, ~(1u << (31 - bitd)));
	OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr), 0);
	return 1;
}

static int OsxGuestInterpretable(uint32 instruction)
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

static void OsxGuestCallout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	emitter->bytemask = 0;
	OsxGuestCpuMemory(emitter, 0x89, 11, OSXJITOFFSET(pc), 0);
	OsxGuestRegisterOp(emitter, 0x89, 0, 13, 0);
	OsxGuestRegisterOp(emitter, 0x29, 0, 12, 0);
	if (count)
		OsxGuestImmediateOp(emitter, 0, 0, (uint32)count);
	OsxGuestCpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions), 1);
	OsxGuestRegisterOp(emitter, 0x89, 1, 3, 1);
	OsxGuestImmediate(emitter, 2, instruction);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xec); OsxGuestJitByte(emitter, 0x20);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xb8);
	OsxGuestQuad(emitter, (uint64)(uintptr_t)OsxGuestCpuExecuteInline);
	OsxGuestJitByte(emitter, 0xff); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 0x20);
	OsxGuestRegisterOp(emitter, 0x89, 2, 13, 0);
	OsxGuestRegisterOp(emitter, 0x29, 2, 12, 0);
	if (count)
		OsxGuestImmediateOp(emitter, 0, 2, (uint32)count);
	OsxGuestCpuMemory(emitter, 0x29, 2, OSXJITOFFSET(instructions), 1);
	OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
	resume = OsxGuestJump(emitter, 4);
	OsxGuestCharge(emitter, count + 1);
	OsxGuestCpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
	OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xbb);
	OsxGuestWord(emitter, OSXJITOFFSET(slice));
	OsxGuestJitByte(emitter, 0);
	running = OsxGuestJump(emitter, 15);
	OsxGuestSettle(emitter);
	OsxGuestRelease(emitter, OSXJITCONTINUE);
	OsxGuestPatch(running, emitter->cursor);
	OsxGuestLeave(emitter, OSXJITCONTINUE);
	OsxGuestPatch(resume, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
}

static int OsxGuestEmit(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
		return OsxGuestBranch(emitter, instruction, count);
	}
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return OsxGuestConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!OsxGuestMultiple(emitter, instruction, opcode, count))
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 31 && ((instruction & 0x6fe) == 0xce || (instruction & 0x6fe) == 0x2ce)) {
		OsxGuestVectorQuad(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxGuestCpuVectorHandler(instruction);
		if (handler) {
			if (OsxGuestVectorFloat(emitter, instruction, count) || OsxGuestVectorConvert(emitter, instruction, count) || OsxGuestVectorExtra(emitter, instruction, count) || OsxGuestVectorNative(emitter, instruction, count))
				return 1;
			emitter->vectorvalue = 0;
			OsxGuestVectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (opcode == 31) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 6 || extended == 38 || extended == 71 || extended == 199) {
			OsxGuestVectorElement(emitter, instruction, extended, count);
			return 1;
		}
	}
	if (OsxGuestInterpretable(instruction)) {
		OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55 && opcode != 46 && opcode != 47) {
		OsxGuestMemory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
#if OSXJITSLOWPATH
		emitter->slowactive = 1;
		status = OsxGuestFloat(emitter, instruction, count);
		emitter->slowactive = 0;
#else
		status = OsxGuestFloat(emitter, instruction, count);
#endif
		if (!status)
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	case 8:
		OsxGuestImmediate(emitter, 0, (uint32)(int32)(int16)immediate);
		OsxGuestCpuMemory(emitter, 0x2b, 0, OSXJITGPR(base), 0);
		OsxGuestSetcc(emitter, 3, 2);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		OsxGuestStoreCarry(emitter);
		return 1;
	case 12: case 13:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		OsxGuestImmediateOp(emitter, 0, 0, (uint32)(int32)(int16)immediate);
		OsxGuestSetcc(emitter, 2, 2);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		OsxGuestStoreCarry(emitter);
		if (opcode == 13) {
			OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 7:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		OsxGuestJitByte(emitter, 0x69); OsxGuestJitByte(emitter, 0xc0);
		OsxGuestWord(emitter, (uint32)(int32)(int16)immediate);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 10: case 11:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestImmediateOp(emitter, 7, 0, immediate);
		OsxGuestCondition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestAddress(emitter, base, -1, immediate, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
		return 1;
	case 20: case 21: case 23:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (opcode == 23) {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(index), 0);
			OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xc0);
		} else {
			OsxGuestShift(emitter, 0, 0, (uint32)index);
		}
		mask = OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		OsxGuestImmediateOp(emitter, 4, 0, mask);
		if (opcode == 20) {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(base), 0);
			OsxGuestImmediateOp(emitter, 4, 1, ~mask);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
		OsxGuestRecord(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26)
			OsxGuestImmediateOp(emitter, 1, 0, immediate);
		else if (opcode < 28)
			OsxGuestImmediateOp(emitter, 6, 0, immediate);
		else
			OsxGuestImmediateOp(emitter, 4, 0, immediate);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
		if (opcode >= 28) {
			OsxGuestRegisterOp(emitter, 0x85, 0, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			OsxGuestMemory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			OsxGuestMemory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			OsxGuestMemory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
			OsxGuestCpuMemory(emitter, 0x3b, 0, OSXJITGPR(index), 0);
			OsxGuestCondition(emitter, target >> 2, extended == 0);
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
				return OsxGuestSystem(emitter, instruction, extended, count);
			if (extended == 339) {
				OsxGuestCpuMemory(emitter, 0x8b, 0, offset, 0);
				OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			} else {
				OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
				if (number == 1)
					OsxGuestImmediateOp(emitter, 4, 0, 0xe000007f);
				OsxGuestCpuMemory(emitter, 0x89, 0, offset, 0);
			}
			return 1;
		}
		if (extended == 19) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr), 0);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base), 0);
			if (extended == 266)
				OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
			else if (extended == 40) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd8);
				OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index), 0);
			} else {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xaf); OsxGuestJitByte(emitter, 0x83);
				OsxGuestWord(emitter, OSXJITGPR(index));
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target), 0);
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(index), 0);
			if (extended == 60 || extended == 412) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd1);
			}
			if (extended == 28 || extended == 60 || extended == 476)
				OsxGuestRegisterOp(emitter, 0x21, 0, 1, 0);
			else if (extended == 284 || extended == 316)
				OsxGuestRegisterOp(emitter, 0x31, 0, 1, 0);
			else
				OsxGuestRegisterOp(emitter, 0x09, 0, 1, 0);
			if (extended == 124 || extended == 284 || extended == 476) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return OsxGuestReserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return OsxGuestZeroBlock(emitter, instruction, count);
		if (OsxGuestCarrying(emitter, instruction, extended) || OsxGuestArithmetic(emitter, instruction, extended, count) || OsxGuestLogical(emitter, instruction, extended) || OsxGuestSystem(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target), 0);
			OsxGuestJitByte(emitter, 0x0f);
			if (extended == 922)
				OsxGuestJitByte(emitter, 0xbf);
			else
				OsxGuestJitByte(emitter, 0xbe);
			OsxGuestJitByte(emitter, 0xc0);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base), 0);
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

static uint32 OsxGuestVectorByte(uint32 element)
{
	return (element & ~3u) + 3 - (element & 3);
}

static void OsxGuestUnpackMask(uint8 *mask, uint32 first, int wide)
{
	uint32 element;
	memset(mask, 0x80, 16);
	for (element = 0; element < 8; element++) {
		if (!wide)
			mask[OsxGuestVectorByte(element * 2)] = (uint8)OsxGuestVectorByte(first + element);
		else if (element < 4) {
			mask[OsxGuestVectorByte(element * 4)] = (uint8)OsxGuestVectorByte((first + element) * 2);
			mask[OsxGuestVectorByte(element * 4 + 1)] = (uint8)OsxGuestVectorByte((first + element) * 2 + 1);
		}
	}
}

int OsxGuestJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	int index;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	OsxGuestHostHugePages(state, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->shuffle = OsxGuestHostFeature(OSXFEATURESSSE3);
	state->fused = !OSXJITNOFUSED && OsxGuestHostFeature(OSXFEATUREFMA);
	state->rounding = OsxGuestHostFeature(OSXFEATURESSE41);
	for (index = 0; index < 4; index++) {
		state->vectorconstants[0][index] = 0x00010203u + (uint32)index * 0x04040404u;
		state->vectorconstants[1][index] = 0x1f1f1f1fu;
		state->vectorconstants[2][index] = 0x03030303u;
		state->vectorconstants[3][index] = 0x0f0f0f0fu;
		state->vectorconstants[4][index] = 0x00004000u;
	}
	OsxGuestUnpackMask((uint8 *)state->vectorconstants[5], 0, 0);
	OsxGuestUnpackMask((uint8 *)state->vectorconstants[6], 8, 0);
	OsxGuestUnpackMask((uint8 *)state->vectorconstants[7], 0, 1);
	OsxGuestUnpackMask((uint8 *)state->vectorconstants[8], 4, 1);
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	OsxGuestHostHugePages(state->records, OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
	state->writebase = OsxGuestJitAllocate(OSXJITBYTES);
	OsxGuestHostHugePages(state->writebase, OSXJITBYTES);
	state->executebase = state->writebase;
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->writebase == NULL || state->executebase == NULL) {
		OsxGuestJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	OsxGuestEmitThunks(state);
#if OSXPROFILEJIT
	state->tmpmap = fopen("osxjitmap.bin", "wb");
#endif
	return 1;
}

void OsxGuestJitDestroy(OsxCpu *cpu)
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
		OsxGuestJitRelease(state->writebase, OSXJITBYTES);
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxGuestJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
#if OSXG5
	if (cpu->core970) {
		OsxGuest970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
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

static int OsxGuestNeedsPc(uint32 instruction)
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

static void OsxGuestCompile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
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
	OsxGuestJitByte(&emitter, 0x53);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x54);
	OsxGuestJitByte(&emitter, 0x41); OsxGuestJitByte(&emitter, 0x55);
	OsxGuestRegisterOp(&emitter, 0x89, 3, 1, 1);
	OsxGuestCpuMemory(&emitter, 0x8b, 12, OSXJITOFFSET(slice), 0);
	OsxGuestRegisterOp(&emitter, 0x89, 13, 12, 0);
	OsxGuestCpuMemory(&emitter, 0x8b, 11, OSXJITOFFSET(pc), 0);
	OsxGuestImmediateOp(&emitter, 7, 12, 0x7fffffff);
	sliceshort = OsxGuestJump(&emitter, 12);
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
		if (pendingpc && OsxGuestNeedsPc(instruction)) {
			OsxGuestImmediateOp(&emitter, 0, 11, pendingpc);
			pendingpc = 0;
		}
#if OSXPROFILEJIT
		tmpoffsets[count] = (uint32)(emitter.cursor - emitter.start);
		tmpwords[count] = instruction;
#endif
#if OSXJITSLOWPATH
		slowjumps = emitter.slowjumpcount;
		status = OsxGuestEmit(&emitter, instruction, count);
		if (emitter.slowjumpcount != slowjumps) {
			emitter.slowinstructions[emitter.slowcount] = instruction;
			emitter.slowcounts[emitter.slowcount] = count;
			emitter.slowresumes[emitter.slowcount] = emitter.cursor;
			emitter.slowcount++;
			emitter.bytemask = 0;
		}
#else
		status = OsxGuestEmit(&emitter, instruction, count);
#endif
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		OsxGuestImmediateOp(&emitter, 0, 11, pendingpc);
#if OSXPROFILEJIT
	tmpbody = (uint32)(emitter.cursor - emitter.start);
#endif
	if (status == 0)
		OsxGuestReturn(&emitter, count);
	else if (status == 1)
		OsxGuestExit(&emitter, count, (physical & 4095) + (uint32)count * 4);
#if OSXPROFILEJIT
	tmpexit = (uint32)(emitter.cursor - emitter.start);
#endif
#if OSXJITSLOWPATH
	for (index = 0; index < emitter.slowcount; index++) {
		slowstub = emitter.cursor;
		for (jump = 0; jump < emitter.slowjumpcount; jump++)
			if (emitter.slowowners[jump] == index)
				OsxGuestPatch(emitter.slowjumps[jump], slowstub);
		OsxGuestCallout(&emitter, emitter.slowinstructions[index], emitter.slowcounts[index]);
		OsxGuestJitByte(&emitter, 0xe9);
		resume = emitter.cursor;
		OsxGuestWord(&emitter, 0);
		OsxGuestPatch(resume, emitter.slowresumes[index]);
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
			OsxGuestReturn(&emitter, emitter.misscounts[index]);
		}
		OsxGuestPatch(emitter.misses[index], stubs[index]);
	}
	OsxGuestPatch(sliceshort, emitter.cursor);
	OsxGuestReturn(&emitter, 0);
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

static void OsxGuestLinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + OSXJITENTRY;
	state->linked++;
}

static void OsxGuestLink(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	int32 distance;
	distance = (int32)((uint8 *)block->function + OSXJITENTRY - (jump + 4));
	memcpy(state->writebase + (jump - state->executebase), &distance, 4);
	state->linked++;
}

static void OsxGuestInterpretRun(OsxCpu *cpu, OsxNativeState *state)
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxGuestJitRun(OsxCpu *cpu, int32 count)
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
		OsxGuestCpuRun(cpu, count);
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
			OsxGuestCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxGuestCpuFetchPage(cpu)) {
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
						OsxGuestCompile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						OsxGuestLink(state, cpu->jitlink, block);
					else
						OsxGuestLinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				cpu->jitlink = NULL;
				OsxGuestInterpretRun(cpu, state);
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
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
void OsxGuestJitTmpStats(OsxCpu *cpu, FILE *log)
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
	uint8 *stored;
	uint32 storedoffset;
	int storedreg;
} OsxNativeEmitter;

static void OsxGuestJitByte(OsxNativeEmitter *emitter, uint32 value)
{
	*emitter->cursor++ = (uint8)value;
}

static void OsxGuestWord(OsxNativeEmitter *emitter, uint32 value)
{
	memcpy(emitter->cursor, &value, 4);
	emitter->cursor += 4;
}

static void OsxGuestRegisterOp(OsxNativeEmitter *emitter, uint32 opcode, int target, int source)
{
	OsxGuestJitByte(emitter, opcode);
	OsxGuestJitByte(emitter, 0xc0 | (source << 3) | target);
}

static int OsxGuestReadsOperand(uint32 opcode)
{
	switch (opcode) {
	case 0x03: case 0x0b: case 0x13: case 0x1b: case 0x23: case 0x2b:
	case 0x33: case 0x39: case 0x3b: case 0x85: case 0x8b:
		return 1;
	}
	return 0;
}

static void OsxGuestCpuMemory(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	if (emitter->cursor == emitter->stored && offset == emitter->storedoffset && OsxGuestReadsOperand(opcode)) {
		if (opcode != 0x8b || reg != emitter->storedreg)
			OsxGuestRegisterOp(emitter, opcode, emitter->storedreg, reg);
		return;
	}
	OsxGuestJitByte(emitter, opcode);
	if (offset < 128) {
		OsxGuestJitByte(emitter, 0x43 | (reg << 3));
		OsxGuestJitByte(emitter, offset);
	} else {
		OsxGuestJitByte(emitter, 0x83 | (reg << 3));
		OsxGuestWord(emitter, offset);
	}
	if (opcode == 0x89 && offset >= OSXJITGPR(0) && offset < OSXJITGPR(32)) {
		emitter->stored = emitter->cursor;
		emitter->storedoffset = offset;
		emitter->storedreg = reg;
	}
}

static void OsxGuestStack(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	OsxGuestJitByte(emitter, opcode);
	OsxGuestJitByte(emitter, 0x44 | (reg << 3));
	OsxGuestJitByte(emitter, 0x24);
	OsxGuestJitByte(emitter, offset);
}

static void OsxGuestImmediate(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	OsxGuestJitByte(emitter, 0xb8 | reg);
	OsxGuestWord(emitter, value);
}

static void OsxGuestImmediateOp(OsxNativeEmitter *emitter, int operation, int reg, uint32 value)
{
	if (value <= 127 || value >= 0xffffff80u) {
		OsxGuestJitByte(emitter, 0x83);
		OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | reg);
		OsxGuestJitByte(emitter, value);
	} else {
		OsxGuestJitByte(emitter, 0x81);
		OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | reg);
		OsxGuestWord(emitter, value);
	}
}

static void OsxGuestShift(OsxNativeEmitter *emitter, int operation, int reg, uint32 amount)
{
	if (amount == 0)
		return;
	OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0xc0 | (operation << 3) | reg);
	OsxGuestJitByte(emitter, amount);
}

static uint8 *OsxGuestJump(OsxNativeEmitter *emitter, int condition)
{
	uint8 *patch;
	OsxGuestJitByte(emitter, 0x0f);
	OsxGuestJitByte(emitter, 0x80 | condition);
	patch = emitter->cursor;
	OsxGuestWord(emitter, 0);
	return patch;
}

static void OsxGuestPatch(uint8 *patch, uint8 *target)
{
	uint32 distance;
	distance = (uint32)(target - (patch + 4));
	memcpy(patch, &distance, 4);
}

static void OsxGuestJumpTo(OsxNativeEmitter *emitter, uint8 *target)
{
	OsxGuestWord(emitter, (uint32)(target - (emitter->cursor + 4)));
}

static void OsxGuestCall(OsxNativeEmitter *emitter, uint8 *target)
{
	OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJumpTo(emitter, target);
}

static void OsxGuestMiss(OsxNativeEmitter *emitter, int condition, int count)
{
	emitter->misses[emitter->misscount] = OsxGuestJump(emitter, condition);
	emitter->misscounts[emitter->misscount++] = count;
}

static void OsxGuestCharge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	OsxGuestImmediateOp(emitter, 5, OSXJITSLICE, (uint32)count);
}

static void OsxGuestExecuted(OsxNativeEmitter *emitter, int reg, int count)
{
	OsxGuestStack(emitter, 0x8b, reg, OSXJITFRAMESLICE);
	OsxGuestRegisterOp(emitter, 0x29, reg, OSXJITSLICE);
	if (count)
		OsxGuestImmediateOp(emitter, 0, reg, (uint32)count);
}

static void OsxGuestSettle(OsxNativeEmitter *emitter)
{
	OsxGuestCpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestExecuted(emitter, 0, 0);
	OsxGuestCpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions));
	OsxGuestCpuMemory(emitter, 0x83, 2, OSXJITOFFSET(instructions) + 4);
	OsxGuestJitByte(emitter, 0);
}

static void OsxGuestRelease(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestImmediate(emitter, 0, code);
	OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 28);
	OsxGuestJitByte(emitter, 0x5d);
	OsxGuestJitByte(emitter, 0x5f);
	OsxGuestJitByte(emitter, 0x5e);
	OsxGuestJitByte(emitter, 0x5b);
	OsxGuestJitByte(emitter, 0xc3);
}

static void OsxGuestLeave(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestSettle(emitter);
	OsxGuestCpuMemory(emitter, 0x89, OSXJITSLICE, OSXJITOFFSET(slice));
	OsxGuestRelease(emitter, code);
}

static void OsxGuestReturn(OsxNativeEmitter *emitter, int count)
{
	OsxGuestCharge(emitter, count);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJumpTo(emitter, emitter->state->interpretthunk);
}

static void OsxGuestDynamicExit(OsxNativeEmitter *emitter, int count)
{
	OsxGuestCharge(emitter, count);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x8e);
	OsxGuestJumpTo(emitter, emitter->state->finishthunk);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJumpTo(emitter, emitter->state->dynamicthunk);
}

static void OsxGuestTranslateThunk(OsxNativeEmitter *emitter, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	uint8 *again;
	retry = emitter->cursor;
	if (limit == 0xfff) {
		OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	} else {
		OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0x48); OsxGuestJitByte(emitter, 0xfff - limit);
	}
	OsxGuestImmediateOp(emitter, 4, 1, 0xfffff000);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0);
	OsxGuestShift(emitter, 5, 2, 12);
	OsxGuestImmediateOp(emitter, 4, 2, OSXTLBMASK);
	OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0x14); OsxGuestJitByte(emitter, 0x52);
	OsxGuestShift(emitter, 4, 2, 2);
	if (store)
		OsxGuestCpuMemory(emitter, 0x03, 2, OSXJITOFFSET(writetlb));
	else
		OsxGuestCpuMemory(emitter, 0x03, 2, OSXJITOFFSET(readtlb));
	OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x0a);
	slow = OsxGuestJump(emitter, 5);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	OsxGuestShift(emitter, 5, 1, 28);
	OsxGuestShift(emitter, 4, 1, 2);
	OsxGuestCpuMemory(emitter, 0x03, 1, OSXJITOFFSET(datasr));
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x09);
	OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x4a); OsxGuestJitByte(emitter, 4);
	segment = OsxGuestJump(emitter, 5);
	OsxGuestImmediateOp(emitter, 4, 0, 0xfff);
	OsxGuestJitByte(emitter, 0x03); OsxGuestJitByte(emitter, 0x42); OsxGuestJitByte(emitter, 8);
	OsxGuestRegisterOp(emitter, 0x31, 1, 1);
	OsxGuestJitByte(emitter, 0xc3);
	OsxGuestPatch(slow, emitter->cursor);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	OsxGuestImmediateOp(emitter, 4, 1, 0xfffff000);
	OsxGuestJitByte(emitter, 0x39); OsxGuestJitByte(emitter, 0x0a);
	page = OsxGuestJump(emitter, 5);
	OsxGuestImmediateOp(emitter, 1, 1, 1);
	OsxGuestJitByte(emitter, 0xc3);
	OsxGuestPatch(page, emitter->cursor);
	OsxGuestPatch(segment, emitter->cursor);
	if (store)
		OsxGuestImmediate(emitter, 1, OSXTLBWRITE);
	else
		OsxGuestImmediate(emitter, 1, OSXTLBREAD);
	OsxGuestCall(emitter, emitter->state->fillthunk);
	OsxGuestRegisterOp(emitter, 0x85, 1, 1);
	again = OsxGuestJump(emitter, 5);
	OsxGuestPatch(again, retry);
	OsxGuestImmediateOp(emitter, 1, 1, 1);
	OsxGuestJitByte(emitter, 0xc3);
}

static void OsxGuestEmitThunks(OsxNativeState *state)
{
	static const uint32 translatelimits[6] = { 0xfff, 0xffe, 0xffc, 0xff8, 0xff0, 0xfe0 };
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
	emitter.state = state;
	emitter.cursor = state->code;
	emitter.misscount = 0;
	emitter.stored = NULL;
	state->checkthunk = emitter.cursor;
	OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x75); OsxGuestJitByte(&emitter, OSXJITRECORD(address));
	misses[0] = OsxGuestJump(&emitter, 5);
	OsxGuestCpuMemory(&emitter, 0x8b, 0, OSXJITOFFSET(fetchtlb));
	OsxGuestRegisterOp(&emitter, 0x89, 2, OSXJITPC);
	OsxGuestShift(&emitter, 5, 2, 12);
	OsxGuestImmediateOp(&emitter, 4, 2, OSXTLBMASK);
	OsxGuestJitByte(&emitter, 0x8d); OsxGuestJitByte(&emitter, 0x14); OsxGuestJitByte(&emitter, 0x52);
	OsxGuestJitByte(&emitter, 0x8d); OsxGuestJitByte(&emitter, 0x04); OsxGuestJitByte(&emitter, 0x90);
	OsxGuestRegisterOp(&emitter, 0x89, 1, OSXJITPC);
	OsxGuestImmediateOp(&emitter, 4, 1, 0xfffff000);
	OsxGuestJitByte(&emitter, 0x39); OsxGuestJitByte(&emitter, 0x08);
	misses[1] = OsxGuestJump(&emitter, 5);
	OsxGuestRegisterOp(&emitter, 0x89, 1, OSXJITPC);
	OsxGuestShift(&emitter, 5, 1, 28);
	OsxGuestCpuMemory(&emitter, 0x8b, 2, OSXJITOFFSET(fetchsr));
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x0c); OsxGuestJitByte(&emitter, 0x8a);
	OsxGuestJitByte(&emitter, 0x39); OsxGuestJitByte(&emitter, 0x48); OsxGuestJitByte(&emitter, 4);
	misses[2] = OsxGuestJump(&emitter, 5);
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x40); OsxGuestJitByte(&emitter, 8);
	OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x45); OsxGuestJitByte(&emitter, OSXJITRECORD(host));
	misses[3] = OsxGuestJump(&emitter, 5);
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x4d); OsxGuestJitByte(&emitter, OSXJITRECORD(versionaddress));
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0x09);
	OsxGuestJitByte(&emitter, 0x3b); OsxGuestJitByte(&emitter, 0x4d); OsxGuestJitByte(&emitter, OSXJITRECORD(version));
	misses[4] = OsxGuestJump(&emitter, 5);
	OsxGuestCpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(fetchhost));
	OsxGuestRegisterOp(&emitter, 0x89, 1, OSXJITPC);
	OsxGuestImmediateOp(&emitter, 4, 1, 0xfffff000);
	OsxGuestImmediateOp(&emitter, 1, 1, 1);
	OsxGuestCpuMemory(&emitter, 0x89, 1, OSXJITOFFSET(fetchtag));
	OsxGuestRegisterOp(&emitter, 0x31, 0, 0);
	for (index = 0; index < 5; index++)
		OsxGuestPatch(misses[index], emitter.cursor);
	OsxGuestJitByte(&emitter, 0xc3);
	state->missthunk = emitter.cursor;
	OsxGuestCpuMemory(&emitter, 0x89, OSXJITUPDATE, OSXJITOFFSET(jitlink));
	OsxGuestLeave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = emitter.cursor;
	OsxGuestLeave(&emitter, OSXJITCONTINUE);
	state->interpretthunk = emitter.cursor;
	OsxGuestLeave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = emitter.cursor;
	OsxGuestJitByte(&emitter, 0x58);
	OsxGuestImmediateOp(&emitter, 5, 0, 9);
	OsxGuestCpuMemory(&emitter, 0x89, 0, OSXJITOFFSET(jitlink));
	OsxGuestLeave(&emitter, OSXJITLINK);
	state->dynamicthunk = emitter.cursor;
	OsxGuestRegisterOp(&emitter, 0x89, OSXJITUPDATE, OSXJITPC);
	OsxGuestShift(&emitter, 5, OSXJITUPDATE, 2);
	OsxGuestImmediateOp(&emitter, 4, OSXJITUPDATE, OSXJITRECORDS - 1);
	OsxGuestJitByte(&emitter, 0x8d); OsxGuestJitByte(&emitter, 0x6c); OsxGuestJitByte(&emitter, 0xad); OsxGuestJitByte(&emitter, 0);
	OsxGuestJitByte(&emitter, 0x8d); OsxGuestJitByte(&emitter, 0x2c); OsxGuestJitByte(&emitter, 0xad);
	OsxGuestWord(&emitter, (uint32)(uintptr_t)state->records);
	OsxGuestCall(&emitter, state->checkthunk);
	OsxGuestJitByte(&emitter, 0x0f); OsxGuestJitByte(&emitter, 0x85);
	OsxGuestJumpTo(&emitter, state->missthunk);
	OsxGuestJitByte(&emitter, 0xff); OsxGuestJitByte(&emitter, 0x65); OsxGuestJitByte(&emitter, OSXJITRECORD(target));
	state->fillthunk = emitter.cursor;
	OsxGuestJitByte(&emitter, 0x50);
	OsxGuestImmediateOp(&emitter, 5, 4, 20);
	OsxGuestStack(&emitter, 0x89, 3, 0);
	OsxGuestStack(&emitter, 0x89, 0, 4);
	OsxGuestStack(&emitter, 0x89, 1, 8);
	OsxGuestCall(&emitter, (uint8 *)(uintptr_t)OsxGuestCpuFillTlb);
	OsxGuestRegisterOp(&emitter, 0x89, 1, 0);
	OsxGuestImmediateOp(&emitter, 0, 4, 20);
	OsxGuestJitByte(&emitter, 0x58);
	OsxGuestJitByte(&emitter, 0xc3);
	state->timethunk = emitter.cursor;
	OsxGuestImmediateOp(&emitter, 5, 4, 12);
	OsxGuestStack(&emitter, 0x89, 3, 0);
	OsxGuestCall(&emitter, (uint8 *)(uintptr_t)OsxGuestCpuTimebase);
	OsxGuestImmediateOp(&emitter, 0, 4, 12);
	OsxGuestJitByte(&emitter, 0xc3);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = emitter.cursor;
		OsxGuestTranslateThunk(&emitter, index / 6, translatelimits[index % 6]);
	}
	state->thunkbytes = ((uint32)(emitter.cursor - state->code) + 63) & ~63u;
	state->used = state->thunkbytes;
}

static void OsxGuestExit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		OsxGuestDynamicExit(emitter, count);
		return;
	}
	OsxGuestCharge(emitter, count);
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x8e);
	OsxGuestJumpTo(emitter, emitter->state->finishthunk);
	OsxGuestJitByte(emitter, 0xe9);
	OsxGuestWord(emitter, 0);
	OsxGuestCall(emitter, emitter->state->linkcallthunk);
}

static void OsxGuestAdvance(OsxNativeEmitter *emitter)
{
	OsxGuestImmediateOp(emitter, 0, OSXJITPC, 4);
}

static void OsxGuestCondition(OsxNativeEmitter *emitter, int field, int issigned)
{
	uint32 shift;
	OsxGuestImmediate(emitter, 0, 2);
	OsxGuestImmediate(emitter, 1, 4);
	OsxGuestImmediate(emitter, 2, 8);
	OsxGuestJitByte(emitter, 0x0f);
	if (issigned)
		OsxGuestJitByte(emitter, 0x4f);
	else
		OsxGuestJitByte(emitter, 0x47);
	OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0x0f);
	if (issigned)
		OsxGuestJitByte(emitter, 0x4c);
	else
		OsxGuestJitByte(emitter, 0x42);
	OsxGuestJitByte(emitter, 0xc2);
	OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(xer));
	OsxGuestShift(emitter, 5, 2, 31);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2);
	shift = (uint32)(7 - field) * 4;
	OsxGuestShift(emitter, 4, 0, shift);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	OsxGuestImmediateOp(emitter, 4, 1, ~(15u << shift));
	OsxGuestRegisterOp(emitter, 0x09, 0, 1);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
}

static void OsxGuestRecord(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestRegisterOp(emitter, 0x85, 0, 0);
		OsxGuestCondition(emitter, 0, 1);
	}
}

static void OsxGuestRequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRFP);
	OsxGuestMiss(emitter, 4, count);
}

static void OsxGuestRecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr));
		OsxGuestShift(emitter, 5, 0, 4);
		OsxGuestImmediateOp(emitter, 4, 0, 0x0f000000);
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
		OsxGuestImmediateOp(emitter, 4, 1, 0xf0ffffff);
		OsxGuestRegisterOp(emitter, 0x09, 0, 1);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
	}
}

static void OsxGuestFloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITFPR(reg) + 4);
	OsxGuestShift(emitter, 5, 2, 20);
	OsxGuestImmediateOp(emitter, 4, 2, 0x7ff);
	OsxGuestImmediateOp(emitter, 7, 2, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	if (normal) {
		OsxGuestRegisterOp(emitter, 0x85, 2, 2);
		OsxGuestMiss(emitter, 4, count);
	}
}

static void OsxGuestSetcc(OsxNativeEmitter *emitter, uint32 condition, int reg)
{
	OsxGuestJitByte(emitter, 0x0f);
	OsxGuestJitByte(emitter, 0x90 | condition);
	OsxGuestJitByte(emitter, 0xc0 | reg);
}

static void OsxGuestFloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6f); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xd1); OsxGuestJitByte(emitter, 32);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xc9);
	OsxGuestRegisterOp(emitter, 0x89, 2, 1);
	OsxGuestShift(emitter, 5, 2, 20);
	OsxGuestImmediateOp(emitter, 4, 2, 0x7ff);
	OsxGuestImmediateOp(emitter, 7, 2, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestRegisterOp(emitter, 0x85, 2, 2);
	zero = OsxGuestJump(emitter, 4);
	if (single) {
		OsxGuestImmediateOp(emitter, 7, 2, 897);
		OsxGuestMiss(emitter, 2, count);
	}
	OsxGuestRegisterOp(emitter, 0x89, 2, 1);
	OsxGuestShift(emitter, 5, 2, 31);
	OsxGuestImmediateOp(emitter, 0, 2, 1);
	OsxGuestShift(emitter, 4, 2, 14);
	OsxGuestJitByte(emitter, 0xe9);
	merge = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(zero, emitter->cursor);
	OsxGuestRegisterOp(emitter, 0x89, 2, 1);
	OsxGuestImmediateOp(emitter, 4, 2, 0x7fffffff);
	OsxGuestRegisterOp(emitter, 0x09, 2, 0);
	OsxGuestMiss(emitter, 5, count);
	OsxGuestRegisterOp(emitter, 0x89, 2, 1);
	OsxGuestShift(emitter, 5, 2, 31);
	OsxGuestShift(emitter, 4, 2, 16);
	OsxGuestImmediateOp(emitter, 1, 2, 0x2000);
	OsxGuestPatch(merge, emitter->cursor);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x11); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(target));
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(fpscr));
	OsxGuestImmediateOp(emitter, 4, 0, ~0x1f000u);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(fpscr));
}

static void OsxGuestFloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x10); OsxGuestJitByte(emitter, 0x83 | (vector << 3));
	OsxGuestWord(emitter, OSXJITFPR(reg));
}

static void OsxGuestFloatRounding(OsxNativeEmitter *emitter, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(fpscr));
	OsxGuestWord(emitter, 3);
	OsxGuestMiss(emitter, 5, count);
}

static void OsxGuestFloatNarrow(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5a); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestJitByte(emitter, 0xf3); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5a); OsxGuestJitByte(emitter, 0xc0);
}

static void OsxGuestFloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	uint32 shift;
	OsxGuestFloatExponent(emitter, left, count, 0);
	OsxGuestFloatExponent(emitter, right, count, 0);
	OsxGuestRegisterOp(emitter, 0x31, 0, 0);
	OsxGuestRegisterOp(emitter, 0x31, 1, 1);
	OsxGuestRegisterOp(emitter, 0x31, 2, 2);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(right));
	OsxGuestSetcc(emitter, 2, 0);
	OsxGuestSetcc(emitter, 7, 1);
	OsxGuestSetcc(emitter, 4, 2);
	OsxGuestShift(emitter, 4, 0, 3);
	OsxGuestShift(emitter, 4, 1, 2);
	OsxGuestRegisterOp(emitter, 0x01, 2, 2);
	OsxGuestRegisterOp(emitter, 0x09, 0, 1);
	OsxGuestRegisterOp(emitter, 0x09, 0, 2);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	OsxGuestShift(emitter, 4, 1, 12);
	OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(fpscr));
	OsxGuestImmediateOp(emitter, 4, 2, ~0xf000u);
	OsxGuestRegisterOp(emitter, 0x09, 2, 1);
	OsxGuestCpuMemory(emitter, 0x89, 2, OSXJITOFFSET(fpscr));
	shift = (uint32)(7 - field) * 4;
	OsxGuestShift(emitter, 4, 0, shift);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	OsxGuestImmediateOp(emitter, 4, 1, ~(0xfu << shift));
	OsxGuestRegisterOp(emitter, 0x09, 1, 0);
	OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(cr));
}

static void OsxGuestFloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	OsxGuestFloatLoad(emitter, 0, right);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2c); OsxGuestJitByte(emitter, 0xc0);
	OsxGuestImmediateOp(emitter, 7, 0, 0x80000000u);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2a); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0xc8);
	exact = OsxGuestJump(emitter, 4);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0x8b);
	OsxGuestWord(emitter, OSXJITOFFSET(fpscr));
	OsxGuestWord(emitter, 0x82000000u);
	OsxGuestPatch(exact, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(target) + 4);
	OsxGuestWord(emitter, 0xfff80000u);
}

static void OsxGuestFloatSingleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(reg));
	OsxGuestWord(emitter, 0x1fffffff);
	OsxGuestMiss(emitter, 5, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(reg) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xea); OsxGuestWord(emitter, 897);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 254);
	OsxGuestMiss(emitter, 3, count);
}

static void OsxGuestFloatSingleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	OsxGuestFloatSingleOperand(emitter, left, count);
	OsxGuestFloatSingleOperand(emitter, multiplier, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(right) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(multiplier));
	OsxGuestFloatLoad(emitter, 1, right);
	if (extended == 28 || extended == 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xd2);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xca);
	}
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc1);
	if (extended >= 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestFloatNarrow(emitter);
	OsxGuestFloatClass(emitter, target, count, 1);
}

static void OsxGuestFloatDoubleOperand(OsxNativeEmitter *emitter, int reg, int count)
{
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(reg) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xea); OsxGuestWord(emitter, 565);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 917);
	OsxGuestMiss(emitter, 3, count);
}

static void OsxGuestFloatOddSum(OsxNativeEmitter *emitter)
{
	uint8 *exact;
	uint8 *odd;
	uint8 *differ;
	uint8 *done;
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xdb);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x2e); OsxGuestJitByte(emitter, 0xc3);
	exact = OsxGuestJump(emitter, 4);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x7e); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 1);
	odd = OsxGuestJump(emitter, 5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xe0);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x50); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xed);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xd5); OsxGuestJitByte(emitter, 63);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 1);
	differ = OsxGuestJump(emitter, 5);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xd4); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestJitByte(emitter, 0xe9);
	done = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(differ, emitter->cursor);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xfb); OsxGuestJitByte(emitter, 0xd5);
	OsxGuestPatch(done, emitter->cursor);
	OsxGuestPatch(exact, emitter->cursor);
	OsxGuestPatch(odd, emitter->cursor);
}

static void OsxGuestFloatDoubleMultiplyAdd(OsxNativeEmitter *emitter, int extended, int target, int left, int multiplier, int right, int count)
{
	OsxGuestFloatDoubleOperand(emitter, left, count);
	OsxGuestFloatDoubleOperand(emitter, multiplier, count);
	OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x93);
	OsxGuestWord(emitter, OSXJITFPR(right) + 4);
	OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xea); OsxGuestJitByte(emitter, 20);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xe2); OsxGuestWord(emitter, 0x7ff);
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xfa); OsxGuestWord(emitter, 0x7ff);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xb8); OsxGuestWord(emitter, 0x02000000);
	OsxGuestJitByte(emitter, 0xba); OsxGuestWord(emitter, 0x41a00000);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6e); OsxGuestJitByte(emitter, 0xe8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x6e); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x62); OsxGuestJitByte(emitter, 0xec);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xcd);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xca);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc1);
	OsxGuestFloatLoad(emitter, 2, multiplier);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xdd);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xdc);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd3);
	OsxGuestFloatLoad(emitter, 4, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xa3);
	OsxGuestWord(emitter, OSXJITFPR(multiplier));
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xec);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xca);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xe9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xd8);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xeb);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x59); OsxGuestJitByte(emitter, 0xc2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xe8);
	OsxGuestFloatLoad(emitter, 0, right);
	if (extended == 28 || extended == 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xc8);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xcc);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xd0);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xd9);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xda);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xc3);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x5c); OsxGuestJitByte(emitter, 0xe2);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc4);
	OsxGuestFloatOddSum(emitter);
	OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x28); OsxGuestJitByte(emitter, 0xc1);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x58); OsxGuestJitByte(emitter, 0xc2);
	if (extended >= 30) {
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
		OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
	}
	OsxGuestFloatClass(emitter, target, count, 0);
}

static int OsxGuestFloat(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestRequireFloat(emitter, count);
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(right));
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITFPR(right) + 4);
			if (extended != 72) {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xba);
				if (extended == 40)
					OsxGuestJitByte(emitter, 0xf9);
				else if (extended == 136)
					OsxGuestJitByte(emitter, 0xe9);
				else
					OsxGuestJitByte(emitter, 0xf1);
				OsxGuestJitByte(emitter, 31);
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
			OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITFPR(target) + 4);
			break;
		case 0: case 32:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatExponent(emitter, right, count, 0);
			OsxGuestFloatLoad(emitter, 0, right);
			OsxGuestFloatNarrow(emitter);
			OsxGuestFloatClass(emitter, target, count, 1);
			break;
		case 15:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		if (!emitter->state->fused && single) {
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatSingleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			OsxGuestRecordFloat(emitter, instruction);
			return 1;
		}
		if (!emitter->state->fused) {
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatDoubleMultiplyAdd(emitter, extended, target, left, multiplier, right, count);
			OsxGuestRecordFloat(emitter, instruction);
			return 1;
		}
		OsxGuestRequireFloat(emitter, count);
		OsxGuestFloatRounding(emitter, count);
		OsxGuestFloatExponent(emitter, left, count, 0);
		OsxGuestFloatExponent(emitter, multiplier, count, 0);
		OsxGuestFloatExponent(emitter, right, count, 0);
		OsxGuestFloatLoad(emitter, 0, left);
		OsxGuestFloatLoad(emitter, 1, multiplier);
		OsxGuestJitByte(emitter, 0xc4); OsxGuestJitByte(emitter, 0xe2); OsxGuestJitByte(emitter, 0xf1);
		if (extended == 28 || extended == 30)
			OsxGuestJitByte(emitter, 0xab);
		else
			OsxGuestJitByte(emitter, 0xa9);
		OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITFPR(right));
		if (extended >= 30) {
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 0xc9);
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 0xf1); OsxGuestJitByte(emitter, 63);
			OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x57); OsxGuestJitByte(emitter, 0xc1);
		}
		if (single)
			OsxGuestFloatNarrow(emitter);
		OsxGuestFloatClass(emitter, target, count, single);
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	OsxGuestRequireFloat(emitter, count);
	OsxGuestFloatRounding(emitter, count);
	OsxGuestFloatExponent(emitter, left, count, 0);
	OsxGuestFloatExponent(emitter, right, count, extended == 18);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestJitByte(emitter, 0xf2); OsxGuestJitByte(emitter, 0x0f);
	if (extended == 18)
		OsxGuestJitByte(emitter, 0x5e);
	else if (extended == 20)
		OsxGuestJitByte(emitter, 0x5c);
	else if (extended == 21)
		OsxGuestJitByte(emitter, 0x58);
	else
		OsxGuestJitByte(emitter, 0x59);
	OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITFPR(right));
	if (single)
		OsxGuestFloatNarrow(emitter);
	OsxGuestFloatClass(emitter, target, count, single);
	OsxGuestRecordFloat(emitter, instruction);
	return 1;
}

static uint32 OsxGuestRotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void OsxGuestAddress(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
	else
		OsxGuestRegisterOp(emitter, 0x31, 0, 0);
	if (index >= 0)
		OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
	else if (immediate)
		OsxGuestImmediateOp(emitter, 0, 0, immediate);
	if (update)
		OsxGuestRegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
}

static int OsxGuestLimitIndex(uint32 limit)
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

static void OsxGuestTranslate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	OsxGuestCall(emitter, emitter->state->translatethunks[store != 0][OsxGuestLimitIndex(limit)]);
	OsxGuestMiss(emitter, 5, count);
}

static int OsxGuestMultiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
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
	OsxGuestAddress(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	OsxGuestImmediateOp(emitter, 4, 1, 0xfff);
	OsxGuestImmediateOp(emitter, 7, 1, 0x1000 - registers * 4);
	OsxGuestMiss(emitter, 7, count);
	OsxGuestTranslate(emitter, opcode == 47, 0xffc, count);
	OsxGuestJitByte(emitter, 0x8d); OsxGuestJitByte(emitter, 0xab);
	OsxGuestWord(emitter, OSXJITGPR(first));
	OsxGuestImmediate(emitter, 1, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x10);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x55); OsxGuestJitByte(emitter, 0);
	} else {
		OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x55); OsxGuestJitByte(emitter, 0);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x10);
	}
	OsxGuestImmediateOp(emitter, 0, 0, 4);
	OsxGuestImmediateOp(emitter, 0, OSXJITUPDATE, 4);
	OsxGuestJitByte(emitter, 0x49);
	OsxGuestJitByte(emitter, 0x75);
	OsxGuestJitByte(emitter, (uint32)(loop - (emitter->cursor + 1)) & 0xff);
	return 1;
}

static void OsxGuestCallHelper(OsxNativeEmitter *emitter, uint8 *function)
{
	OsxGuestCpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestCall(emitter, function);
	OsxGuestCpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
}

static void OsxGuestVectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	OsxGuestJitByte(emitter, 0x83); OsxGuestJitByte(emitter, 0xbb);
	OsxGuestWord(emitter, OSXJITOFFSET(altivec));
	OsxGuestJitByte(emitter, 0);
	OsxGuestMiss(emitter, 4, count);
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRVEC);
	OsxGuestMiss(emitter, 4, count);
}

static void OsxGuestVectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	OsxGuestVectorEnabled(emitter, count);
	OsxGuestStack(emitter, 0x89, 3, 0);
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x44); OsxGuestJitByte(emitter, 0x24); OsxGuestJitByte(emitter, 4);
	OsxGuestWord(emitter, instruction);
	OsxGuestCallHelper(emitter, (uint8 *)(uintptr_t)function);
}

static void OsxGuestMemory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
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
		OsxGuestRequireFloat(emitter, count);
	if (single && store) {
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(target));
		OsxGuestStack(emitter, 0x89, 0, 0);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITFPR(target) + 4);
		OsxGuestStack(emitter, 0x89, 0, 4);
		OsxGuestCallHelper(emitter, (uint8 *)(uintptr_t)OsxGuestCpuDoubleToSingle);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(jitscratch));
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	OsxGuestAddress(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		OsxGuestRegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
	OsxGuestTranslate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITFPR(target) + 4);
			OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITFPR(target));
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc9);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
			OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x08);
			OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x50); OsxGuestJitByte(emitter, 4);
		} else {
			if (single)
				OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(jitscratch));
			else
				OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITGPR(target));
			if (opcode == 36 || opcode == 52) {
				if (!reversed) {
					OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xca);
				}
				OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x10);
			} else if (opcode == 44) {
				OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xca); OsxGuestJitByte(emitter, 8);
				OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x10);
			} else {
				OsxGuestJitByte(emitter, 0x88); OsxGuestJitByte(emitter, 0x10);
			}
		}
	} else {
		if (wide) {
			OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x08);
			OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x40); OsxGuestJitByte(emitter, 4);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc9);
			OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc8);
			OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITFPR(target) + 4);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
		} else if (opcode == 32 || single) {
			OsxGuestJitByte(emitter, 0x8b); OsxGuestJitByte(emitter, 0x00);
			if (!reversed) {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xc8);
			}
		} else {
			OsxGuestJitByte(emitter, 0x0f);
			if (opcode == 34)
				OsxGuestJitByte(emitter, 0xb6);
			else
				OsxGuestJitByte(emitter, 0xb7);
			OsxGuestJitByte(emitter, 0x00);
			if (opcode != 34) {
				OsxGuestJitByte(emitter, 0x66); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 0xc0); OsxGuestJitByte(emitter, 8);
				if (opcode == 42) {
					OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xbf); OsxGuestJitByte(emitter, 0xc0);
				}
			}
		}
		if (single) {
			OsxGuestStack(emitter, 0x89, 0, 0);
			OsxGuestCallHelper(emitter, (uint8 *)(uintptr_t)OsxGuestCpuSingleToDouble);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITFPR(target));
			OsxGuestCpuMemory(emitter, 0x89, 2, OSXJITFPR(target) + 4);
		} else if (!wide) {
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		}
	}
	if (update)
		OsxGuestCpuMemory(emitter, 0x89, OSXJITUPDATE, OSXJITGPR(base));
}

static int OsxGuestBranch(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(ctr));
			options |= 4;
		} else {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(lr));
		}
		OsxGuestImmediateOp(emitter, 4, 1, ~3u);
	} else {
		if (opcode == 18) {
			displacement = instruction & 0x03fffffc;
			if (displacement & 0x02000000)
				displacement |= 0xfc000000;
			options = 20;
		} else {
			displacement = (uint32)(int32)(int16)(instruction & 0xfffc);
		}
		OsxGuestImmediate(emitter, 1, displacement);
		if (!(instruction & 2)) {
			OsxGuestRegisterOp(emitter, 0x01, 1, OSXJITPC);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		OsxGuestRegisterOp(emitter, 0x89, 2, OSXJITPC);
		OsxGuestImmediateOp(emitter, 0, 2, 4);
		OsxGuestCpuMemory(emitter, 0x89, 2, OSXJITOFFSET(lr));
	}
	if (!(options & 4)) {
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(ctr));
		OsxGuestImmediateOp(emitter, 5, 0, 1);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(ctr));
		if (options & 2)
			counterpatch = OsxGuestJump(emitter, 5);
		else
			counterpatch = OsxGuestJump(emitter, 4);
	}
	if (!(options & 16)) {
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(cr));
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestWord(emitter, 1u << (31 - ((instruction >> 16) & 31)));
		if (options & 8)
			conditionpatch = OsxGuestJump(emitter, 4);
		else
			conditionpatch = OsxGuestJump(emitter, 5);
	}
	OsxGuestRegisterOp(emitter, 0x89, OSXJITPC, 1);
	OsxGuestExit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			OsxGuestPatch(counterpatch, emitter->cursor);
		if (conditionpatch)
			OsxGuestPatch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void OsxGuestLoadCarry(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xba); OsxGuestJitByte(emitter, 0xa3);
	OsxGuestWord(emitter, OSXJITOFFSET(xer));
	OsxGuestJitByte(emitter, 29);
}

static void OsxGuestStoreCarry(OsxNativeEmitter *emitter)
{
	OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xb6); OsxGuestJitByte(emitter, 0xd2);
	OsxGuestShift(emitter, 4, 2, 29);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(xer));
	OsxGuestImmediateOp(emitter, 4, 1, 0xdfffffff);
	OsxGuestRegisterOp(emitter, 0x09, 1, 2);
	OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(xer));
}

static void OsxGuestRequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(msr));
	OsxGuestWord(emitter, OSXMSRPR);
	OsxGuestMiss(emitter, 5, count);
}

static int OsxGuestCarrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(right));
		OsxGuestSetcc(emitter, 2, 2);
		break;
	case 8:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(right));
		OsxGuestCpuMemory(emitter, 0x2b, 0, OSXJITGPR(left));
		OsxGuestSetcc(emitter, 3, 2);
		break;
	case 138:
	case 136:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 136) {
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
		}
		OsxGuestLoadCarry(emitter);
		OsxGuestCpuMemory(emitter, 0x13, 0, OSXJITGPR(right));
		OsxGuestSetcc(emitter, 2, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 200 || extended == 232) {
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
		}
		OsxGuestLoadCarry(emitter);
		if (extended == 202 || extended == 200)
			OsxGuestImmediateOp(emitter, 2, 0, 0);
		else
			OsxGuestImmediateOp(emitter, 2, 0, 0xffffffff);
		OsxGuestSetcc(emitter, 2, 2);
		break;
	default:
		return 0;
	}
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
	OsxGuestStoreCarry(emitter);
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestArithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd8);
		break;
	case 11:
	case 75:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		OsxGuestJitByte(emitter, 0xf7);
		if (extended == 11)
			OsxGuestJitByte(emitter, 0xa3);
		else
			OsxGuestJitByte(emitter, 0xab);
		OsxGuestWord(emitter, OSXJITGPR(right));
		OsxGuestRegisterOp(emitter, 0x89, 0, 2);
		break;
	case 459:
	case 491:
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(right));
		OsxGuestRegisterOp(emitter, 0x85, 1, 1);
		OsxGuestMiss(emitter, 4, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(left));
		if (extended == 491) {
			OsxGuestImmediateOp(emitter, 7, 1, 0xffffffff);
			ordinary = OsxGuestJump(emitter, 5);
			OsxGuestImmediateOp(emitter, 7, 0, 0x80000000);
			OsxGuestMiss(emitter, 4, count);
			OsxGuestPatch(ordinary, emitter->cursor);
			OsxGuestJitByte(emitter, 0x99);
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xf9);
		} else {
			OsxGuestRegisterOp(emitter, 0x31, 2, 2);
			OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xf1);
		}
		break;
	default:
		return 0;
	}
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestLogical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
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
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount));
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		OsxGuestJitByte(emitter, 0xd3);
		if (extended == 24)
			OsxGuestJitByte(emitter, 0xe0);
		else
			OsxGuestJitByte(emitter, 0xe8);
		OsxGuestJitByte(emitter, 0xf6); OsxGuestJitByte(emitter, 0xc1); OsxGuestJitByte(emitter, 32);
		OsxGuestJitByte(emitter, 0x74); OsxGuestJitByte(emitter, 2);
		OsxGuestRegisterOp(emitter, 0x31, 0, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		break;
	case 26:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		OsxGuestImmediate(emitter, 2, 63);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xbd); OsxGuestJitByte(emitter, 0xc0);
		OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0x44); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestImmediateOp(emitter, 6, 0, 31);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		break;
	case 792:
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(amount));
		OsxGuestImmediateOp(emitter, 4, 1, 63);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		OsxGuestImmediate(emitter, 2, 0xffffffff);
		OsxGuestImmediateOp(emitter, 7, 1, 32);
		OsxGuestJitByte(emitter, 0x73); OsxGuestJitByte(emitter, 8);
		OsxGuestImmediate(emitter, 2, 1);
		OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xe2);
		OsxGuestJitByte(emitter, 0x4a);
		OsxGuestRegisterOp(emitter, 0x21, 2, 0);
		OsxGuestImmediateOp(emitter, 7, 1, 31);
		OsxGuestJitByte(emitter, 0x76); OsxGuestJitByte(emitter, 5);
		OsxGuestImmediate(emitter, 1, 31);
		OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xf8);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		OsxGuestRegisterOp(emitter, 0x85, 2, 2);
		OsxGuestSetcc(emitter, 5, 2);
		OsxGuestRegisterOp(emitter, 0x85, 0, 0);
		OsxGuestSetcc(emitter, 8, 1);
		OsxGuestJitByte(emitter, 0x20); OsxGuestJitByte(emitter, 0xca);
		OsxGuestStoreCarry(emitter);
		break;
	case 824:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(source));
		if (amount == 0) {
			OsxGuestRegisterOp(emitter, 0x31, 2, 2);
		} else {
			OsxGuestRegisterOp(emitter, 0x89, 2, 0);
			OsxGuestRegisterOp(emitter, 0x89, 1, 0);
			OsxGuestImmediateOp(emitter, 4, 1, (1u << amount) - 1);
			OsxGuestSetcc(emitter, 5, 1);
			OsxGuestShift(emitter, 5, 2, 31);
			OsxGuestJitByte(emitter, 0x20); OsxGuestJitByte(emitter, 0xca);
			OsxGuestShift(emitter, 7, 0, (uint32)amount);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		break;
	default:
		return 0;
	}
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestSpecialOffset(uint32 number, uint32 *offset)
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

static void OsxGuestCallout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	OsxGuestCpuMemory(emitter, 0x89, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestExecuted(emitter, 0, count);
	OsxGuestCpuMemory(emitter, 0x01, 0, OSXJITOFFSET(instructions));
	OsxGuestCpuMemory(emitter, 0x83, 2, OSXJITOFFSET(instructions) + 4);
	OsxGuestJitByte(emitter, 0);
	OsxGuestStack(emitter, 0x89, 3, 0);
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x44); OsxGuestJitByte(emitter, 0x24); OsxGuestJitByte(emitter, 4);
	OsxGuestWord(emitter, instruction);
	OsxGuestCall(emitter, (uint8 *)(uintptr_t)OsxGuestCpuExecuteInline);
	OsxGuestExecuted(emitter, 2, count);
	OsxGuestCpuMemory(emitter, 0x29, 2, OSXJITOFFSET(instructions));
	OsxGuestCpuMemory(emitter, 0x83, 3, OSXJITOFFSET(instructions) + 4);
	OsxGuestJitByte(emitter, 0);
	OsxGuestRegisterOp(emitter, 0x85, 0, 0);
	resume = OsxGuestJump(emitter, 4);
	OsxGuestCharge(emitter, count + 1);
	OsxGuestCpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestCpuMemory(emitter, 0x83, 7, OSXJITOFFSET(slice));
	OsxGuestJitByte(emitter, 0);
	running = OsxGuestJump(emitter, 15);
	OsxGuestSettle(emitter);
	OsxGuestRelease(emitter, OSXJITCONTINUE);
	OsxGuestPatch(running, emitter->cursor);
	OsxGuestLeave(emitter, OSXJITCONTINUE);
	OsxGuestPatch(resume, emitter->cursor);
	OsxGuestCpuMemory(emitter, 0x8b, OSXJITPC, OSXJITOFFSET(pc));
}

static int OsxGuestSystem(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (mask != 0xffffffffu) {
			OsxGuestImmediateOp(emitter, 4, 0, mask);
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
			OsxGuestImmediateOp(emitter, 4, 1, ~mask);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
		return 1;
	case 83:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(msr));
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 146:
		emitter->enabled = 0;
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(target));
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(msr));
		OsxGuestRegisterOp(emitter, 0x31, 2, 1);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc2);
		OsxGuestWord(emitter, ~(uint32)OSXMSREE);
		slowmask = OsxGuestJump(emitter, 5);
		OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xc1);
		OsxGuestWord(emitter, OSXMSRPOW);
		slowpower = OsxGuestJump(emitter, 5);
		OsxGuestCpuMemory(emitter, 0x89, 1, OSXJITOFFSET(msr));
		OsxGuestRegisterOp(emitter, 0x21, 2, 1);
		quiet = OsxGuestJump(emitter, 4);
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITOFFSET(externalline));
		OsxGuestCpuMemory(emitter, 0x0b, 2, OSXJITOFFSET(decpending));
		pending = OsxGuestJump(emitter, 4);
		OsxGuestAdvance(emitter);
		OsxGuestCharge(emitter, count + 1);
		OsxGuestSettle(emitter);
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(slice));
		OsxGuestWord(emitter, 0);
		OsxGuestRelease(emitter, OSXJITCONTINUE);
		OsxGuestPatch(quiet, emitter->cursor);
		OsxGuestPatch(pending, emitter->cursor);
		OsxGuestJitByte(emitter, 0xe9);
		done = emitter->cursor;
		OsxGuestWord(emitter, 0);
		OsxGuestPatch(slowmask, emitter->cursor);
		OsxGuestPatch(slowpower, emitter->cursor);
		OsxGuestCallout(emitter, instruction, count);
		OsxGuestPatch(done, emitter->cursor);
		return 1;
	case 595:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		OsxGuestCall(emitter, emitter->state->timethunk);
		if (number == 269)
			OsxGuestRegisterOp(emitter, 0x89, 0, 2);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 210:
	case 242:
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 210)
			OsxGuestImmediate(emitter, 0, (instruction >> 16) & 15);
		else {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR((instruction >> 11) & 31));
			OsxGuestShift(emitter, 5, 0, 28);
		}
		OsxGuestRegisterOp(emitter, 0x89, 1, OSXJITPC);
		OsxGuestShift(emitter, 5, 1, 28);
		OsxGuestRegisterOp(emitter, 0x39, 1, 0);
		OsxGuestMiss(emitter, 4, count);
		OsxGuestCpuMemory(emitter, 0x8b, 2, OSXJITGPR(target));
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x94); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(sr));
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(fetchtag));
		OsxGuestWord(emitter, 0);
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!OsxGuestSpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			OsxGuestCallout(emitter, instruction, count);
			return 1;
		}
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 339) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, offset);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		} else {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			OsxGuestCpuMemory(emitter, 0x89, 0, offset);
		}
		return 1;
	}
	return 0;
}

static void OsxGuestConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(xer));
	OsxGuestShift(emitter, 5, 0, 31);
}

static void OsxGuestStoreConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestShift(emitter, 4, 0, 28);
	OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITOFFSET(cr));
	OsxGuestImmediateOp(emitter, 4, 1, 0x0fffffff);
	OsxGuestRegisterOp(emitter, 0x09, 0, 1);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
}

static int OsxGuestReserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	OsxGuestAddress(emitter, base, index, 0, 0);
	OsxGuestJitByte(emitter, 0xa8); OsxGuestJitByte(emitter, 3);
	OsxGuestMiss(emitter, 5, count);
	if (extended == 20) {
		OsxGuestRegisterOp(emitter, 0x89, OSXJITUPDATE, 0);
		OsxGuestMemory(emitter, instruction, 32, index, count);
		OsxGuestCpuMemory(emitter, 0x89, OSXJITUPDATE, OSXJITOFFSET(reservation));
		OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
		OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
		OsxGuestWord(emitter, 1);
		return 1;
	}
	OsxGuestJitByte(emitter, 0x81); OsxGuestJitByte(emitter, 0xbb);
	OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
	OsxGuestWord(emitter, 0);
	invalid = OsxGuestJump(emitter, 4);
	OsxGuestCpuMemory(emitter, 0x39, 0, OSXJITOFFSET(reservation));
	elsewhere = OsxGuestJump(emitter, 5);
	OsxGuestMemory(emitter, instruction, 36, index, count);
	OsxGuestConditionZero(emitter);
	OsxGuestImmediateOp(emitter, 1, 0, 2);
	OsxGuestJitByte(emitter, 0xe9);
	done = emitter->cursor;
	OsxGuestWord(emitter, 0);
	OsxGuestPatch(invalid, emitter->cursor);
	OsxGuestPatch(elsewhere, emitter->cursor);
	OsxGuestConditionZero(emitter);
	OsxGuestPatch(done, emitter->cursor);
	OsxGuestJitByte(emitter, 0xc7); OsxGuestJitByte(emitter, 0x83);
	OsxGuestWord(emitter, OSXJITOFFSET(reservationvalid));
	OsxGuestWord(emitter, 0);
	OsxGuestStoreConditionZero(emitter);
	return 1;
}

static int OsxGuestZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	int offset;
	OsxGuestAddress(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	OsxGuestImmediateOp(emitter, 4, 0, ~31u);
	OsxGuestTranslate(emitter, 1, 0xfe0, count);
	OsxGuestRegisterOp(emitter, 0x31, 2, 2);
	for (offset = 0; offset < 32; offset += 4) {
		OsxGuestJitByte(emitter, 0x89); OsxGuestJitByte(emitter, 0x50);
		OsxGuestJitByte(emitter, (uint32)offset);
	}
	return 1;
}

static int OsxGuestConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
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
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
		OsxGuestRegisterOp(emitter, 0x89, 1, 0);
		OsxGuestShift(emitter, 5, 1, (7 - (bita >> 2)) * 4);
		OsxGuestImmediateOp(emitter, 4, 1, 0xf);
		OsxGuestShift(emitter, 4, 1, (7 - (bitd >> 2)) * 4);
		OsxGuestImmediateOp(emitter, 4, 0, ~(0xfu << ((7 - (bitd >> 2)) * 4)));
		OsxGuestRegisterOp(emitter, 0x09, 0, 1);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
	OsxGuestRegisterOp(emitter, 0x89, 1, 0);
	OsxGuestShift(emitter, 5, 1, 31 - bita);
	OsxGuestImmediateOp(emitter, 4, 1, 1);
	OsxGuestRegisterOp(emitter, 0x89, 2, 0);
	OsxGuestShift(emitter, 5, 2, 31 - bitb);
	OsxGuestImmediateOp(emitter, 4, 2, 1);
	if (extended == 129 || extended == 417)
		OsxGuestImmediateOp(emitter, 6, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		OsxGuestRegisterOp(emitter, 0x09, 1, 2);
	else if (extended == 129 || extended == 225 || extended == 257)
		OsxGuestRegisterOp(emitter, 0x21, 1, 2);
	else
		OsxGuestRegisterOp(emitter, 0x31, 1, 2);
	if (extended == 33 || extended == 225 || extended == 289)
		OsxGuestImmediateOp(emitter, 6, 1, 1);
	OsxGuestShift(emitter, 4, 1, 31 - bitd);
	OsxGuestImmediateOp(emitter, 4, 0, ~(1u << (31 - bitd)));
	OsxGuestRegisterOp(emitter, 0x09, 0, 1);
	OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITOFFSET(cr));
	return 1;
}

static int OsxGuestInterpretable(uint32 instruction)
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

static int OsxGuestEmit(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
		return OsxGuestBranch(emitter, instruction, count);
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return OsxGuestConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!OsxGuestMultiple(emitter, instruction, opcode, count))
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxGuestCpuVectorHandler(instruction);
		if (handler) {
			OsxGuestVectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (OsxGuestInterpretable(instruction)) {
		OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55) {
		OsxGuestMemory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
		if (!OsxGuestFloat(emitter, instruction, count))
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	case 8:
		OsxGuestImmediate(emitter, 0, (uint32)(int32)(int16)immediate);
		OsxGuestCpuMemory(emitter, 0x2b, 0, OSXJITGPR(base));
		OsxGuestSetcc(emitter, 3, 2);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		return 1;
	case 12: case 13:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		OsxGuestImmediateOp(emitter, 0, 0, (uint32)(int32)(int16)immediate);
		OsxGuestSetcc(emitter, 2, 2);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		if (opcode == 13) {
			OsxGuestRegisterOp(emitter, 0x85, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 7:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		OsxGuestJitByte(emitter, 0x69); OsxGuestJitByte(emitter, 0xc0);
		OsxGuestWord(emitter, (uint32)(int32)(int16)immediate);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 10: case 11:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestImmediateOp(emitter, 7, 0, immediate);
		OsxGuestCondition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestAddress(emitter, base, -1, immediate, 0);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
		return 1;
	case 20: case 21: case 23:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (opcode == 23) {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(index));
			OsxGuestJitByte(emitter, 0xd3); OsxGuestJitByte(emitter, 0xc0);
		} else {
			OsxGuestShift(emitter, 0, 0, (uint32)index);
		}
		mask = OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		OsxGuestImmediateOp(emitter, 4, 0, mask);
		if (opcode == 20) {
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(base));
			OsxGuestImmediateOp(emitter, 4, 1, ~mask);
			OsxGuestRegisterOp(emitter, 0x09, 0, 1);
		}
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
		OsxGuestRecord(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26)
			OsxGuestImmediateOp(emitter, 1, 0, immediate);
		else if (opcode < 28)
			OsxGuestImmediateOp(emitter, 6, 0, immediate);
		else
			OsxGuestImmediateOp(emitter, 4, 0, immediate);
		OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
		if (opcode >= 28) {
			OsxGuestRegisterOp(emitter, 0x85, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			OsxGuestMemory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			OsxGuestMemory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			OsxGuestMemory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
			OsxGuestCpuMemory(emitter, 0x3b, 0, OSXJITGPR(index));
			OsxGuestCondition(emitter, target >> 2, extended == 0);
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
				return OsxGuestSystem(emitter, instruction, extended, count);
			if (extended == 339) {
				OsxGuestCpuMemory(emitter, 0x8b, 0, offset);
				OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			} else {
				OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
				if (number == 1)
					OsxGuestImmediateOp(emitter, 4, 0, 0xe000007f);
				OsxGuestCpuMemory(emitter, 0x89, 0, offset);
			}
			return 1;
		}
		if (extended == 19) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITOFFSET(cr));
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(base));
			if (extended == 266)
				OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
			else if (extended == 40) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd8);
				OsxGuestCpuMemory(emitter, 0x03, 0, OSXJITGPR(index));
			} else {
				OsxGuestJitByte(emitter, 0x0f); OsxGuestJitByte(emitter, 0xaf); OsxGuestJitByte(emitter, 0x83);
				OsxGuestWord(emitter, OSXJITGPR(index));
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(target));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			OsxGuestCpuMemory(emitter, 0x8b, 1, OSXJITGPR(index));
			if (extended == 60 || extended == 412) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd1);
			}
			if (extended == 28 || extended == 60 || extended == 476)
				OsxGuestRegisterOp(emitter, 0x21, 0, 1);
			else if (extended == 284 || extended == 316)
				OsxGuestRegisterOp(emitter, 0x31, 0, 1);
			else
				OsxGuestRegisterOp(emitter, 0x09, 0, 1);
			if (extended == 124 || extended == 284 || extended == 476) {
				OsxGuestJitByte(emitter, 0xf7); OsxGuestJitByte(emitter, 0xd0);
			}
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return OsxGuestReserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return OsxGuestZeroBlock(emitter, instruction, count);
		if (OsxGuestCarrying(emitter, instruction, extended) || OsxGuestArithmetic(emitter, instruction, extended, count) || OsxGuestLogical(emitter, instruction, extended) || OsxGuestSystem(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			OsxGuestCpuMemory(emitter, 0x8b, 0, OSXJITGPR(target));
			OsxGuestJitByte(emitter, 0x0f);
			if (extended == 922)
				OsxGuestJitByte(emitter, 0xbf);
			else
				OsxGuestJitByte(emitter, 0xbe);
			OsxGuestJitByte(emitter, 0xc0);
			OsxGuestCpuMemory(emitter, 0x89, 0, OSXJITGPR(base));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

int OsxGuestJitCreate(OsxCpu *cpu)
{
	OsxNativeState *state;
	if (!OsxGuestHostFeature(OSXFEATURESSE2))
		return 0;
	state = (OsxNativeState *)calloc(1, sizeof(OsxNativeState));
	if (state == NULL)
		return 0;
	state->pagecount = (cpu->ramsize + 4095) >> 12;
	state->fused = !OSXJITNOFUSED && OsxGuestHostFeature(OSXFEATUREFMA);
	state->versions = (uint32 *)calloc(state->pagecount, sizeof(uint32));
	state->compiledpages = (uint8 *)calloc(state->pagecount, 1);
	state->records = (OsxNativeRecord *)malloc(OSXJITRECORDS * sizeof(OsxNativeRecord));
	if (state->records)
		memset(state->records, 0xff, OSXJITRECORDS * sizeof(OsxNativeRecord));
	state->code = OsxGuestJitAllocate(OSXJITBYTES);
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->code == NULL) {
		OsxGuestJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	OsxGuestEmitThunks(state);
	return 1;
}

void OsxGuestJitDestroy(OsxCpu *cpu)
{
	OsxNativeState *state;
	state = (OsxNativeState *)cpu->nativejit;
	if (state == NULL)
		return;
	if (cpu->logfile)
		fprintf(cpu->logfile, "jit compiled %llu linked %llu instructions %llu interpreted %llu bytes %u\n", (unsigned long long)state->compiled, (unsigned long long)state->linked, (unsigned long long)cpu->instructions, (unsigned long long)state->interpreted, state->used);
	if (state->code)
		OsxGuestJitRelease(state->code, OSXJITBYTES);
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxGuestJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
	OsxNativeState *state;
	uint32 first;
	uint32 last;
	int invalidated;
#if OSXG5
	if (cpu->core970) {
		OsxGuest970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
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

static int OsxGuestNeedsPc(uint32 instruction)
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

static void OsxGuestCompile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
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
	emitter.stored = NULL;
	OsxGuestJitByte(&emitter, 0x53);
	OsxGuestJitByte(&emitter, 0x56);
	OsxGuestJitByte(&emitter, 0x57);
	OsxGuestJitByte(&emitter, 0x55);
	OsxGuestJitByte(&emitter, 0x83); OsxGuestJitByte(&emitter, 0xec); OsxGuestJitByte(&emitter, 28);
	OsxGuestStack(&emitter, 0x8b, 3, 48);
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0xbb);
	OsxGuestWord(&emitter, OSXJITOFFSET(slice));
	OsxGuestStack(&emitter, 0x89, OSXJITSLICE, OSXJITFRAMESLICE);
	OsxGuestJitByte(&emitter, 0x8b); OsxGuestJitByte(&emitter, 0xb3);
	OsxGuestWord(&emitter, OSXJITOFFSET(pc));
	OsxGuestJitByte(&emitter, 0x81); OsxGuestJitByte(&emitter, 0xff);
	OsxGuestWord(&emitter, 0x7fffffff);
	sliceshort = OsxGuestJump(&emitter, 12);
	limit = (int)((4096 - (physical & 4095)) >> 2);
	if (limit > OSXJITLIMIT)
		limit = OSXJITLIMIT;
	count = 0;
	status = 0;
	pendingpc = 0;
	while (count < limit && emitter.misscount < OSXJITMISSES - 128) {
		memcpy(&word, cpu->ram + physical + count * 4, 4);
		instruction = tswap32(word);
		if (pendingpc && OsxGuestNeedsPc(instruction)) {
			OsxGuestImmediateOp(&emitter, 0, OSXJITPC, pendingpc);
			pendingpc = 0;
		}
		status = OsxGuestEmit(&emitter, instruction, count);
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		OsxGuestImmediateOp(&emitter, 0, OSXJITPC, pendingpc);
	if (status == 0)
		OsxGuestReturn(&emitter, count);
	else if (status == 1)
		OsxGuestExit(&emitter, count, (physical & 4095) + (uint32)count * 4);
	for (index = 0; index < emitter.misscount; index++) {
		shared = 0;
		while (shared < index && emitter.misscounts[shared] != emitter.misscounts[index])
			shared++;
		if (shared < index) {
			stubs[index] = stubs[shared];
		} else {
			stubs[index] = emitter.cursor;
			OsxGuestReturn(&emitter, emitter.misscounts[index]);
		}
		OsxGuestPatch(emitter.misses[index], stubs[index]);
	}
	OsxGuestPatch(sliceshort, emitter.cursor);
	OsxGuestReturn(&emitter, 0);
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

static void OsxGuestLinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + OSXJITENTRY;
	state->linked++;
}

static void OsxGuestLink(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	OsxGuestPatch(jump, (uint8 *)block->function + OSXJITENTRY);
	state->linked++;
}

static void OsxGuestInterpretRun(OsxCpu *cpu, OsxNativeState *state)
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxGuestJitRun(OsxCpu *cpu, int32 count)
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
		OsxGuestCpuRun(cpu, count);
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
			OsxGuestCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxGuestCpuFetchPage(cpu)) {
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
						OsxGuestCompile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						OsxGuestLink(state, cpu->jitlink, block);
					else
						OsxGuestLinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				OsxGuestInterpretRun(cpu, state);
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
	}
}

#if OSXPROFILEJIT
void OsxGuestJitTmpStats(OsxCpu *cpu, FILE *log)
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

static void OsxGuestOp(OsxNativeEmitter *emitter, uint32 word)
{
	memcpy(emitter->cursor, &word, 4);
	emitter->cursor += 4;
}

static uint8 *OsxGuestHere(OsxNativeEmitter *emitter)
{
	return emitter->cursor - (emitter->state->writebase - emitter->state->executebase);
}

static void OsxGuestPublish(OsxNativeState *state, uint8 *execute, uint32 length)
{
#if defined(__APPLE__)
	jit_wx_publish(execute, execute + (state->writebase - state->executebase), length);
#else
	OsxGuestJitFlush(execute, length);
#endif
}

static void OsxGuestBranchTo(OsxNativeEmitter *emitter, uint32 opcode, uint8 *target)
{
	OsxGuestOp(emitter, opcode | ((uint32)((target - OsxGuestHere(emitter)) / 4) & 0x03ffffff));
}

static uint8 *OsxGuestForward(OsxNativeEmitter *emitter, uint32 word)
{
	uint8 *site;
	site = emitter->cursor;
	OsxGuestOp(emitter, word);
	return site;
}

static void OsxGuestPatch(uint8 *site, uint8 *target)
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

static void OsxGuestMiss(OsxNativeEmitter *emitter, uint32 word, int count)
{
	emitter->misses[emitter->misscount] = OsxGuestForward(emitter, word);
	emitter->misscounts[emitter->misscount++] = count;
}

static uint32 OsxGuestConditional(int condition)
{
	return OSXA64BCOND | (uint32)condition;
}

static uint32 OsxGuestBitTest(uint32 opcode, int reg, uint32 bit)
{
	return opcode | ((bit & 31) << 19) | ((bit >> 5) << 31) | (uint32)reg;
}

static void OsxGuestReg3(OsxNativeEmitter *emitter, uint32 opcode, int target, int left, int right)
{
	OsxGuestOp(emitter, opcode | ((uint32)right << 16) | ((uint32)left << 5) | (uint32)target);
}

static void OsxGuestMove(OsxNativeEmitter *emitter, int target, int source)
{
	OsxGuestReg3(emitter, 0x2a000000u, target, OSXJITZERO, source);
}

static void OsxGuestConstant(OsxNativeEmitter *emitter, int reg, uint32 value)
{
	if ((value & 0xffff0000u) == 0)
		OsxGuestOp(emitter, 0x52800000u | (value << 5) | (uint32)reg);
	else if ((value & 0xffff) == 0)
		OsxGuestOp(emitter, 0x52a00000u | ((value >> 16) << 5) | (uint32)reg);
	else if ((~value & 0xffff0000u) == 0)
		OsxGuestOp(emitter, 0x12800000u | ((~value & 0xffff) << 5) | (uint32)reg);
	else if ((~value & 0xffff) == 0)
		OsxGuestOp(emitter, 0x12a00000u | ((~value >> 16) << 5) | (uint32)reg);
	else {
		OsxGuestOp(emitter, 0x52800000u | ((value & 0xffff) << 5) | (uint32)reg);
		OsxGuestOp(emitter, 0x72a00000u | ((value >> 16) << 5) | (uint32)reg);
	}
}

static void OsxGuestConstant64(OsxNativeEmitter *emitter, int reg, uint64 value)
{
	uint32 part;
	int half;
	OsxGuestOp(emitter, 0xd2800000u | ((uint32)(value & 0xffff) << 5) | (uint32)reg);
	for (half = 1; half < 4; half++) {
		part = (uint32)(value >> (half * 16)) & 0xffff;
		if (part)
			OsxGuestOp(emitter, 0xf2800000u | ((uint32)half << 21) | (part << 5) | (uint32)reg);
	}
}

static void OsxGuestAddImmediate(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 value)
{
	if (value < 4096)
		OsxGuestOp(emitter, opcode | (value << 10) | ((uint32)source << 5) | (uint32)target);
	else if (0u - value < 4096)
		OsxGuestOp(emitter, (opcode ^ 0x40000000u) | ((0u - value) << 10) | ((uint32)source << 5) | (uint32)target);
	else if ((value & 0xfff) == 0 && value < 0x1000000)
		OsxGuestOp(emitter, opcode | 0x400000u | ((value >> 12) << 10) | ((uint32)source << 5) | (uint32)target);
	else {
		OsxGuestConstant(emitter, OSXJITSCRATCH, value);
		OsxGuestReg3(emitter, opcode - 0x06000000u, target, source, OSXJITSCRATCH);
	}
}

static int OsxGuestBitmask(uint32 value, uint32 *encoded)
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

static void OsxGuestLogicImmediate(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 value)
{
	uint32 encoded;
	if (OsxGuestBitmask(value, &encoded)) {
		OsxGuestOp(emitter, opcode | (encoded << 10) | ((uint32)source << 5) | (uint32)target);
		return;
	}
	OsxGuestConstant(emitter, OSXJITSCRATCH, value);
	OsxGuestReg3(emitter, opcode - 0x08000000u, target, source, OSXJITSCRATCH);
}

static void OsxGuestBitfield(OsxNativeEmitter *emitter, uint32 opcode, int target, int source, uint32 rotate, uint32 last)
{
	OsxGuestOp(emitter, opcode | (rotate << 16) | (last << 10) | ((uint32)source << 5) | (uint32)target);
}

static void OsxGuestShiftLeft(OsxNativeEmitter *emitter, int target, int source, uint32 amount)
{
	OsxGuestBitfield(emitter, 0x53000000u, target, source, (32 - amount) & 31, 31 - amount);
}

static void OsxGuestShiftRight(OsxNativeEmitter *emitter, int target, int source, uint32 amount)
{
	OsxGuestBitfield(emitter, 0x53000000u, target, source, amount, 31);
}

static void OsxGuestExtract(OsxNativeEmitter *emitter, int target, int source, uint32 first, uint32 width)
{
	OsxGuestBitfield(emitter, 0x53000000u, target, source, first, first + width - 1);
}

static void OsxGuestInsert(OsxNativeEmitter *emitter, int target, int source, uint32 first, uint32 width)
{
	OsxGuestBitfield(emitter, 0x33000000u, target, source, (32 - first) & 31, width - 1);
}

static void OsxGuestJitSet(OsxNativeEmitter *emitter, int reg, int condition)
{
	OsxGuestOp(emitter, 0x1a9f07e0u | ((uint32)(condition ^ 1) << 12) | (uint32)reg);
}

static void OsxGuestField(OsxNativeEmitter *emitter, uint32 opcode, int reg, uint32 offset)
{
	uint32 scale;
	scale = 4;
	if (opcode & 0x40000000u)
		scale = 8;
	if (offset % scale == 0 && offset / scale < 4096) {
		OsxGuestOp(emitter, opcode | ((offset / scale) << 10) | (OSXJITCPU << 5) | (uint32)reg);
		return;
	}
	OsxGuestConstant(emitter, OSXJITSCRATCH, offset);
	OsxGuestReg3(emitter, 0x8b000000u, OSXJITSCRATCH, OSXJITCPU, OSXJITSCRATCH);
	OsxGuestOp(emitter, opcode | (OSXJITSCRATCH << 5) | (uint32)reg);
}

static void OsxGuestLoad(OsxNativeEmitter *emitter, int reg, uint32 offset)
{
	OsxGuestField(emitter, OSXA64LDRW, reg, offset);
}

static void OsxGuestStore(OsxNativeEmitter *emitter, int reg, uint32 offset)
{
	OsxGuestField(emitter, OSXA64STRW, reg, offset);
}

static void OsxGuestCallFar(OsxNativeEmitter *emitter, void *function)
{
	OsxGuestConstant64(emitter, OSXJITSCRATCH, (uint64)(uintptr_t)function);
	OsxGuestOp(emitter, 0xd63f0000u | (OSXJITSCRATCH << 5));
}

static void OsxGuestCharge(OsxNativeEmitter *emitter, int count)
{
	if (count == 0)
		return;
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITSLICE, OSXJITSLICE, (uint32)count);
}

static void OsxGuestSettle(OsxNativeEmitter *emitter)
{
	OsxGuestStore(emitter, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestReg3(emitter, 0x4b000000u, 0, OSXJITENTRYSLICE, OSXJITSLICE);
	OsxGuestField(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	OsxGuestReg3(emitter, 0x8b204000u, 1, 1, 0);
	OsxGuestField(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
}

static void OsxGuestRelease(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestConstant(emitter, 0, code);
	OsxGuestOp(emitter, 0xa94363f7u);
	OsxGuestOp(emitter, 0xa9425bf5u);
	OsxGuestOp(emitter, 0xa94153f3u);
	OsxGuestOp(emitter, 0xa8c47bfdu);
	OsxGuestOp(emitter, OSXA64RET);
}

static void OsxGuestLeave(OsxNativeEmitter *emitter, uint32 code)
{
	OsxGuestSettle(emitter);
	OsxGuestStore(emitter, OSXJITSLICE, OSXJITOFFSET(slice));
	OsxGuestRelease(emitter, code);
}

static void OsxGuestReturn(OsxNativeEmitter *emitter, int count)
{
	OsxGuestCharge(emitter, count);
	OsxGuestBranchTo(emitter, OSXA64B, emitter->state->interpretthunk);
}

static void OsxGuestDynamicExit(OsxNativeEmitter *emitter, int count)
{
	OsxGuestCharge(emitter, count);
	OsxGuestOp(emitter, OsxGuestConditional(OSXCONDGT) | (2 << 5));
	OsxGuestBranchTo(emitter, OSXA64B, emitter->state->finishthunk);
	OsxGuestBranchTo(emitter, OSXA64B, emitter->state->dynamicthunk);
}

static void OsxGuestTranslateThunk(OsxNativeEmitter *emitter, int store, uint32 limit)
{
	uint8 *retry;
	uint8 *slow;
	uint8 *segment;
	uint8 *page;
	retry = emitter->cursor;
	OsxGuestAddImmediate(emitter, OSXA64ADD, 1, 0, 0xfff - limit);
	OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, 0xfffff000u);
	OsxGuestExtract(emitter, 2, 0, 12, OSXTLBBITS);
	if (store)
		OsxGuestField(emitter, OSXA64LDRX, 3, OSXJITOFFSET(writetlb));
	else
		OsxGuestField(emitter, OSXA64LDRX, 3, OSXJITOFFSET(readtlb));
	OsxGuestReg3(emitter, 0x8b001000u, 3, 3, 2);
	OsxGuestOp(emitter, 0xb9400062u);
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	slow = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestShiftRight(emitter, 1, 0, 28);
	OsxGuestField(emitter, OSXA64LDRX, 2, OSXJITOFFSET(datasr));
	OsxGuestOp(emitter, 0xb8617841u);
	OsxGuestOp(emitter, 0xb9400462u);
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	segment = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestOp(emitter, 0xf9400461u);
	OsxGuestLogicImmediate(emitter, OSXA64AND, 2, 0, 0xfff);
	OsxGuestReg3(emitter, 0x8b000000u, 1, 1, 2);
	OsxGuestOp(emitter, OSXA64RET);
	OsxGuestPatch(slow, emitter->cursor);
	OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 0, 0xfffff000u);
	OsxGuestOp(emitter, 0xb9400062u);
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	page = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestOp(emitter, 0xd2800001u);
	OsxGuestOp(emitter, OSXA64RET);
	OsxGuestPatch(page, emitter->cursor);
	OsxGuestPatch(segment, emitter->cursor);
	OsxGuestOp(emitter, 0xa9bf03feu);
	OsxGuestMove(emitter, 1, 0);
	OsxGuestReg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	if (store)
		OsxGuestConstant(emitter, 2, OSXTLBWRITE);
	else
		OsxGuestConstant(emitter, 2, OSXTLBREAD);
	OsxGuestCallFar(emitter, (void *)OsxGuestCpuFillTlb);
	OsxGuestMove(emitter, 3, 0);
	OsxGuestOp(emitter, 0xa8c103feu);
	OsxGuestOp(emitter, OSXA64CBNZ | ((uint32)((retry - emitter->cursor) / 4 & 0x7ffff) << 5) | 3);
	OsxGuestOp(emitter, 0xd2800001u);
	OsxGuestOp(emitter, OSXA64RET);
}

static void OsxGuestEmitThunks(OsxNativeState *state)
{
	static const uint32 translatelimits[6] = { 0xfff, 0xffe, 0xffc, 0xff8, 0xff0, 0xfe0 };
	OsxNativeEmitter emitter;
	uint8 *misses[5];
	int index;
	emitter.state = state;
	emitter.cursor = state->writebase;
	emitter.misscount = 0;
	state->checkthunk = OsxGuestHere(&emitter);
	OsxGuestOp(&emitter, 0xb9400000u | (OSXJITRECORD(address) / 4) << 10 | (OSXJITUPDATE << 5));
	OsxGuestReg3(&emitter, 0x6b000000u, OSXJITZERO, 0, OSXJITPC);
	misses[0] = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestField(&emitter, OSXA64LDRX, 0, OSXJITOFFSET(fetchtlb));
	OsxGuestExtract(&emitter, 1, OSXJITPC, 12, OSXTLBBITS);
	OsxGuestReg3(&emitter, 0x8b001000u, 0, 0, 1);
	OsxGuestOp(&emitter, 0xb9400002u);
	OsxGuestLogicImmediate(&emitter, OSXA64AND, 1, OSXJITPC, 0xfffff000u);
	OsxGuestReg3(&emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	misses[1] = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestShiftRight(&emitter, 1, OSXJITPC, 28);
	OsxGuestField(&emitter, OSXA64LDRX, 2, OSXJITOFFSET(fetchsr));
	OsxGuestOp(&emitter, 0xb8617841u);
	OsxGuestOp(&emitter, 0xb9400402u);
	OsxGuestReg3(&emitter, 0x6b000000u, OSXJITZERO, 2, 1);
	misses[2] = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestOp(&emitter, 0xf9400400u);
	OsxGuestOp(&emitter, 0xf9400001u | (OSXJITRECORD(host) / 8) << 10 | (OSXJITUPDATE << 5));
	OsxGuestReg3(&emitter, 0xeb000000u, OSXJITZERO, 0, 1);
	misses[3] = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestOp(&emitter, 0xf9400001u | (OSXJITRECORD(versionaddress) / 8) << 10 | (OSXJITUPDATE << 5));
	OsxGuestOp(&emitter, 0xb9400021u);
	OsxGuestOp(&emitter, 0xb9400002u | (OSXJITRECORD(version) / 4) << 10 | (OSXJITUPDATE << 5));
	OsxGuestReg3(&emitter, 0x6b000000u, OSXJITZERO, 1, 2);
	misses[4] = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestField(&emitter, OSXA64STRX, 0, OSXJITOFFSET(fetchhost));
	OsxGuestLogicImmediate(&emitter, OSXA64AND, 1, OSXJITPC, 0xfffff000u);
	OsxGuestLogicImmediate(&emitter, OSXA64ORR, 1, 1, 1);
	OsxGuestStore(&emitter, 1, OSXJITOFFSET(fetchtag));
	OsxGuestConstant(&emitter, 0, 0);
	OsxGuestOp(&emitter, OSXA64RET);
	for (index = 0; index < 5; index++)
		OsxGuestPatch(misses[index], emitter.cursor);
	OsxGuestConstant(&emitter, 0, 1);
	OsxGuestOp(&emitter, OSXA64RET);
	state->missthunk = OsxGuestHere(&emitter);
	OsxGuestField(&emitter, OSXA64STRX, OSXJITUPDATE, OSXJITOFFSET(jitlink));
	OsxGuestLeave(&emitter, OSXJITDYNAMIC);
	state->finishthunk = OsxGuestHere(&emitter);
	OsxGuestLeave(&emitter, OSXJITCONTINUE);
	state->interpretthunk = OsxGuestHere(&emitter);
	OsxGuestLeave(&emitter, OSXJITINTERPRET);
	state->linkcallthunk = OsxGuestHere(&emitter);
	OsxGuestOp(&emitter, 0xd10023c0u);
	OsxGuestField(&emitter, OSXA64STRX, 0, OSXJITOFFSET(jitlink));
	OsxGuestLeave(&emitter, OSXJITLINK);
	state->dynamicthunk = OsxGuestHere(&emitter);
	OsxGuestExtract(&emitter, OSXJITUPDATE, OSXJITPC, 2, OSXJITRECORDBITS);
	OsxGuestConstant64(&emitter, OSXJITSCRATCH, (uint64)(uintptr_t)state->records);
	OsxGuestOp(&emitter, 0x8b001400u | (OSXJITUPDATE << 16) | (OSXJITSCRATCH << 5) | OSXJITUPDATE);
	OsxGuestBranchTo(&emitter, OSXA64BL, state->checkthunk);
	OsxGuestOp(&emitter, OSXA64CBNZ | ((uint32)((state->missthunk - OsxGuestHere(&emitter)) / 4 & 0x7ffff) << 5));
	OsxGuestOp(&emitter, 0xf9400000u | (OSXJITRECORD(target) / 8) << 10 | (OSXJITUPDATE << 5) | OSXJITSCRATCH);
	OsxGuestOp(&emitter, 0xd61f0000u | (OSXJITSCRATCH << 5));
	state->timethunk = OsxGuestHere(&emitter);
	OsxGuestOp(&emitter, 0xa9bf7bfdu);
	OsxGuestReg3(&emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	OsxGuestCallFar(&emitter, (void *)OsxGuestCpuTimebase);
	OsxGuestOp(&emitter, 0xa8c17bfdu);
	OsxGuestOp(&emitter, OSXA64RET);
	for (index = 0; index < 12; index++) {
		state->translatethunks[index / 6][index % 6] = OsxGuestHere(&emitter);
		OsxGuestTranslateThunk(&emitter, index / 6, translatelimits[index % 6]);
	}
	state->thunkbytes = ((uint32)(emitter.cursor - state->writebase) + 63) & ~63u;
	state->used = state->thunkbytes;
	OsxGuestPublish(state, state->executebase, state->thunkbytes);
}

static void OsxGuestExit(OsxNativeEmitter *emitter, int count, uint32 targetoffset)
{
	if (targetoffset > 4095) {
		OsxGuestDynamicExit(emitter, count);
		return;
	}
	OsxGuestCharge(emitter, count);
	OsxGuestOp(emitter, OsxGuestConditional(OSXCONDGT) | (2 << 5));
	OsxGuestBranchTo(emitter, OSXA64B, emitter->state->finishthunk);
	OsxGuestOp(emitter, OSXA64B | 1);
	OsxGuestBranchTo(emitter, OSXA64BL, emitter->state->linkcallthunk);
}

static void OsxGuestCombine(OsxNativeEmitter *emitter)
{
	OsxGuestOp(emitter, 0x2a000420u);
	OsxGuestOp(emitter, 0x2a000440u);
	OsxGuestShiftLeft(emitter, 0, 0, 1);
}

static void OsxGuestCondition(OsxNativeEmitter *emitter, int field, int issigned)
{
	if (issigned) {
		OsxGuestJitSet(emitter, 0, OSXCONDLT);
		OsxGuestJitSet(emitter, 1, OSXCONDGT);
	} else {
		OsxGuestJitSet(emitter, 0, OSXCONDLO);
		OsxGuestJitSet(emitter, 1, OSXCONDHI);
	}
	OsxGuestJitSet(emitter, 2, OSXCONDEQ);
	OsxGuestCombine(emitter);
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(xer));
	OsxGuestOp(emitter, 0x2a417c00u);
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(cr));
	OsxGuestInsert(emitter, 1, 0, (uint32)(7 - field) * 4, 4);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(cr));
}

static void OsxGuestRecord(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
		OsxGuestCondition(emitter, 0, 1);
	}
}

static void OsxGuestRequireFloat(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 2)
		return;
	emitter->enabled |= 2;
	OsxGuestLoad(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	OsxGuestMiss(emitter, OsxGuestBitTest(OSXA64TBZ, OSXJITSCRATCH, 13), count);
}

static void OsxGuestRecordFloat(OsxNativeEmitter *emitter, uint32 instruction)
{
	if (instruction & 1) {
		OsxGuestLoad(emitter, 0, OSXJITOFFSET(fpscr));
		OsxGuestLoad(emitter, 1, OSXJITOFFSET(cr));
		OsxGuestExtract(emitter, 0, 0, 28, 4);
		OsxGuestInsert(emitter, 1, 0, 24, 4);
		OsxGuestStore(emitter, 1, OSXJITOFFSET(cr));
	}
}

static void OsxGuestFloatExponent(OsxNativeEmitter *emitter, int reg, int count, int normal)
{
	OsxGuestField(emitter, OSXA64LDRX, 0, OSXJITFPR(reg));
	OsxGuestOp(emitter, 0xd374f802u);
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 0x7ff);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
	if (normal)
		OsxGuestMiss(emitter, OSXA64CBZ | 2, count);
}

static void OsxGuestFloatLoad(OsxNativeEmitter *emitter, int vector, int reg)
{
	OsxGuestField(emitter, OSXA64LDRD, vector, OSXJITFPR(reg));
}

static void OsxGuestFloatRounding(OsxNativeEmitter *emitter, int count)
{
	OsxGuestLoad(emitter, OSXJITSCRATCH, OSXJITOFFSET(fpscr));
	OsxGuestLogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, OSXJITSCRATCH, 3);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDNE), count);
}

static void OsxGuestFloatNarrow(OsxNativeEmitter *emitter)
{
	OsxGuestOp(emitter, 0x1e624000u);
	OsxGuestOp(emitter, 0x1e22c000u);
}

static void OsxGuestFloatClass(OsxNativeEmitter *emitter, int target, int count, int single)
{
	uint8 *zero;
	uint8 *merge;
	OsxGuestOp(emitter, 0x9e660000u);
	OsxGuestOp(emitter, 0xd374f802u);
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 0x7ff);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
	zero = OsxGuestForward(emitter, OSXA64CBZ | 2);
	if (single) {
		OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 2, 897);
		OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDLO), count);
	}
	OsxGuestOp(emitter, 0xd37ffc02u);
	OsxGuestAddImmediate(emitter, OSXA64ADD, 2, 2, 1);
	OsxGuestShiftLeft(emitter, 2, 2, 14);
	merge = OsxGuestForward(emitter, OSXA64B);
	OsxGuestPatch(zero, emitter->cursor);
	OsxGuestOp(emitter, 0xd37ff803u);
	OsxGuestMiss(emitter, 0xb5000003u, count);
	OsxGuestOp(emitter, 0xd37ffc02u);
	OsxGuestShiftLeft(emitter, 2, 2, 16);
	OsxGuestLogicImmediate(emitter, OSXA64ORR, 2, 2, 0x2000);
	OsxGuestPatch(merge, emitter->cursor);
	OsxGuestField(emitter, OSXA64STRD, 0, OSXJITFPR(target));
	OsxGuestLoad(emitter, 0, OSXJITOFFSET(fpscr));
	OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, ~0x1f000u);
	OsxGuestReg3(emitter, 0x2a000000u, 0, 0, 2);
	OsxGuestStore(emitter, 0, OSXJITOFFSET(fpscr));
}

static void OsxGuestFloatCompare(OsxNativeEmitter *emitter, int field, int left, int right, int count)
{
	OsxGuestFloatExponent(emitter, left, count, 0);
	OsxGuestFloatExponent(emitter, right, count, 0);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestFloatLoad(emitter, 1, right);
	OsxGuestOp(emitter, 0x1e612000u);
	OsxGuestJitSet(emitter, 0, OSXCONDMI);
	OsxGuestJitSet(emitter, 1, OSXCONDGT);
	OsxGuestJitSet(emitter, 2, OSXCONDEQ);
	OsxGuestCombine(emitter);
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(fpscr));
	OsxGuestInsert(emitter, 1, 0, 12, 4);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(fpscr));
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(cr));
	OsxGuestInsert(emitter, 1, 0, (uint32)(7 - field) * 4, 4);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(cr));
}

static void OsxGuestFloatTruncate(OsxNativeEmitter *emitter, int target, int right, int count)
{
	uint8 *exact;
	OsxGuestFloatLoad(emitter, 0, right);
	OsxGuestOp(emitter, 0x1e602000u);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDVS), count);
	OsxGuestOp(emitter, 0x1e780000u);
	OsxGuestConstant(emitter, 1, 0x7fffffffu);
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
	OsxGuestConstant(emitter, 1, 0x80000000u);
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
	OsxGuestOp(emitter, 0x1e620001u);
	OsxGuestOp(emitter, 0x1e602020u);
	exact = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDEQ));
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(fpscr));
	OsxGuestLogicImmediate(emitter, OSXA64ORR, 1, 1, 0x82000000u);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(fpscr));
	OsxGuestPatch(exact, emitter->cursor);
	OsxGuestStore(emitter, 0, OSXJITFPR(target));
	OsxGuestConstant(emitter, 1, 0xfff80000u);
	OsxGuestStore(emitter, 1, OSXJITFPR(target) + 4);
}

static int OsxGuestFloat(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatLoad(emitter, 0, right);
			if (extended == 136 || extended == 264)
				OsxGuestOp(emitter, 0x1e60c000u);
			if (extended == 40 || extended == 136)
				OsxGuestOp(emitter, 0x1e614000u);
			OsxGuestField(emitter, OSXA64STRD, 0, OSXJITFPR(target));
			break;
		case 0: case 32:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatCompare(emitter, target >> 2, left, right, count);
			return 1;
		case 12:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatRounding(emitter, count);
			OsxGuestFloatExponent(emitter, right, count, 0);
			OsxGuestFloatLoad(emitter, 0, right);
			OsxGuestFloatNarrow(emitter);
			OsxGuestFloatClass(emitter, target, count, 1);
			break;
		case 15:
			OsxGuestRequireFloat(emitter, count);
			OsxGuestFloatTruncate(emitter, target, right, count);
			break;
		default:
			return 0;
		}
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	extended &= 31;
	if (extended >= 28) {
		multiplier = (instruction >> 6) & 31;
		OsxGuestRequireFloat(emitter, count);
		OsxGuestFloatRounding(emitter, count);
		OsxGuestFloatExponent(emitter, left, count, 0);
		OsxGuestFloatExponent(emitter, multiplier, count, 0);
		OsxGuestFloatExponent(emitter, right, count, 0);
		OsxGuestFloatLoad(emitter, 0, left);
		OsxGuestFloatLoad(emitter, 1, multiplier);
		OsxGuestFloatLoad(emitter, 2, right);
		if (extended == 28 || extended == 30)
			OsxGuestOp(emitter, 0x1f618800u);
		else
			OsxGuestOp(emitter, 0x1f410800u);
		if (extended >= 30)
			OsxGuestOp(emitter, 0x1e614000u);
		if (single)
			OsxGuestFloatNarrow(emitter);
		OsxGuestFloatClass(emitter, target, count, single);
		OsxGuestRecordFloat(emitter, instruction);
		return 1;
	}
	if (extended != 18 && extended != 20 && extended != 21 && extended != 25)
		return 0;
	if (extended == 25)
		right = (instruction >> 6) & 31;
	OsxGuestRequireFloat(emitter, count);
	OsxGuestFloatRounding(emitter, count);
	OsxGuestFloatExponent(emitter, left, count, 0);
	OsxGuestFloatExponent(emitter, right, count, extended == 18);
	OsxGuestFloatLoad(emitter, 0, left);
	OsxGuestFloatLoad(emitter, 1, right);
	if (extended == 18)
		OsxGuestOp(emitter, 0x1e611800u);
	else if (extended == 20)
		OsxGuestOp(emitter, 0x1e613800u);
	else if (extended == 21)
		OsxGuestOp(emitter, 0x1e612800u);
	else
		OsxGuestOp(emitter, 0x1e610800u);
	if (single)
		OsxGuestFloatNarrow(emitter);
	OsxGuestFloatClass(emitter, target, count, single);
	OsxGuestRecordFloat(emitter, instruction);
	return 1;
}

static uint32 OsxGuestRotateMask(uint32 begin, uint32 end)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffu >> begin;
	low = 0xffffffffu << (31 - end);
	if (begin <= end)
		return high & low;
	return high | low;
}

static void OsxGuestAddress(OsxNativeEmitter *emitter, int base, int index, uint32 immediate, int update)
{
	if (base || update)
		OsxGuestLoad(emitter, 0, OSXJITGPR(base));
	else
		OsxGuestConstant(emitter, 0, 0);
	if (index >= 0) {
		OsxGuestLoad(emitter, 1, OSXJITGPR(index));
		OsxGuestReg3(emitter, 0x0b000000u, 0, 0, 1);
	} else if (immediate) {
		OsxGuestAddImmediate(emitter, OSXA64ADD, 0, 0, immediate);
	}
	if (update)
		OsxGuestMove(emitter, OSXJITUPDATE, 0);
}

static int OsxGuestLimitIndex(uint32 limit)
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

static void OsxGuestTranslate(OsxNativeEmitter *emitter, int store, uint32 limit, int count)
{
	OsxGuestBranchTo(emitter, OSXA64BL, emitter->state->translatethunks[store != 0][OsxGuestLimitIndex(limit)]);
	OsxGuestMiss(emitter, 0xb4000001u, count);
}

static int OsxGuestMultiple(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int count)
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
	OsxGuestAddress(emitter, base, -1, (uint32)(int32)(int16)(instruction & 0xffff), 0);
	OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 0, 0xfff);
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 1, 0x1000 - registers * 4);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDHI), count);
	OsxGuestTranslate(emitter, opcode == 47, 0xffc, count);
	OsxGuestConstant(emitter, OSXJITSCRATCH, OSXJITGPR(first));
	OsxGuestReg3(emitter, 0x8b000000u, 2, OSXJITCPU, OSXJITSCRATCH);
	OsxGuestConstant(emitter, 3, registers);
	loop = emitter->cursor;
	if (opcode == 46) {
		OsxGuestOp(emitter, 0xb8404424u);
		OsxGuestOp(emitter, 0x5ac00884u);
		OsxGuestOp(emitter, 0xb8004444u);
	} else {
		OsxGuestOp(emitter, 0xb8404444u);
		OsxGuestOp(emitter, 0x5ac00884u);
		OsxGuestOp(emitter, 0xb8004424u);
	}
	OsxGuestAddImmediate(emitter, OSXA64SUBS, 3, 3, 1);
	OsxGuestOp(emitter, OsxGuestConditional(OSXCONDNE) | (((uint32)((loop - emitter->cursor) / 4) & 0x7ffff) << 5));
	return 1;
}

static void OsxGuestCallHelper(OsxNativeEmitter *emitter, void *function)
{
	OsxGuestStore(emitter, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestCallFar(emitter, function);
	OsxGuestLoad(emitter, OSXJITPC, OSXJITOFFSET(pc));
}

static void OsxGuestVectorEnabled(OsxNativeEmitter *emitter, int count)
{
	if (emitter->enabled & 1)
		return;
	emitter->enabled |= 1;
	OsxGuestLoad(emitter, OSXJITSCRATCH, OSXJITOFFSET(altivec));
	OsxGuestMiss(emitter, OSXA64CBZ | OSXJITSCRATCH, count);
	OsxGuestLoad(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	OsxGuestMiss(emitter, OsxGuestBitTest(OSXA64TBZ, OSXJITSCRATCH, 25), count);
}

static void OsxGuestVectorCall(OsxNativeEmitter *emitter, uint32 instruction, OsxVectorFunction function, int count)
{
	OsxGuestVectorEnabled(emitter, count);
	OsxGuestReg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	OsxGuestConstant(emitter, 1, instruction);
	OsxGuestCallHelper(emitter, (void *)function);
}

static void OsxGuestMemory(OsxNativeEmitter *emitter, uint32 instruction, uint32 opcode, int index, int count)
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
		OsxGuestRequireFloat(emitter, count);
	if (single && store) {
		OsxGuestField(emitter, OSXA64LDRX, 0, OSXJITFPR(target));
		OsxGuestCallHelper(emitter, (void *)OsxGuestCpuDoubleToSingle);
		OsxGuestStore(emitter, 0, OSXJITOFFSET(jitscratch));
	}
	limit = 0xffc;
	if (wide)
		limit = 0xff8;
	else if (opcode == 34 || opcode == 38)
		limit = 0xfff;
	else if (opcode == 40 || opcode == 42 || opcode == 44)
		limit = 0xffe;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	OsxGuestAddress(emitter, base, index, immediate, update && index < 0);
	if (update && index >= 0)
		OsxGuestMove(emitter, OSXJITUPDATE, 0);
	OsxGuestTranslate(emitter, store, limit, count);
	if (store) {
		if (wide) {
			OsxGuestField(emitter, OSXA64LDRX, 2, OSXJITFPR(target));
			OsxGuestOp(emitter, 0xdac00c42u);
			OsxGuestOp(emitter, 0xf9000022u);
		} else {
			if (single)
				OsxGuestLoad(emitter, 2, OSXJITOFFSET(jitscratch));
			else
				OsxGuestLoad(emitter, 2, OSXJITGPR(target));
			if (opcode == 36 || opcode == 52) {
				if (!reversed)
					OsxGuestOp(emitter, 0x5ac00842u);
				OsxGuestOp(emitter, 0xb9000022u);
			} else if (opcode == 44) {
				OsxGuestOp(emitter, 0x5ac00442u);
				OsxGuestOp(emitter, 0x79000022u);
			} else {
				OsxGuestOp(emitter, 0x39000022u);
			}
		}
	} else {
		if (wide) {
			OsxGuestOp(emitter, 0xf9400020u);
			OsxGuestOp(emitter, 0xdac00c00u);
			OsxGuestField(emitter, OSXA64STRX, 0, OSXJITFPR(target));
		} else if (opcode == 32 || single) {
			OsxGuestOp(emitter, 0xb9400020u);
			if (!reversed)
				OsxGuestOp(emitter, 0x5ac00800u);
		} else if (opcode == 34) {
			OsxGuestOp(emitter, 0x39400020u);
		} else {
			OsxGuestOp(emitter, 0x79400020u);
			OsxGuestOp(emitter, 0x5ac00400u);
			if (opcode == 42)
				OsxGuestBitfield(emitter, 0x13000000u, 0, 0, 0, 15);
		}
		if (single) {
			OsxGuestCallHelper(emitter, (void *)OsxGuestCpuSingleToDouble);
			OsxGuestField(emitter, OSXA64STRX, 0, OSXJITFPR(target));
		} else if (!wide) {
			OsxGuestStore(emitter, 0, OSXJITGPR(target));
		}
	}
	if (update)
		OsxGuestStore(emitter, OSXJITUPDATE, OSXJITGPR(base));
}

static int OsxGuestBranch(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
			OsxGuestLoad(emitter, 1, OSXJITOFFSET(ctr));
			options |= 4;
		} else {
			OsxGuestLoad(emitter, 1, OSXJITOFFSET(lr));
		}
		OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, ~3u);
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
			OsxGuestConstant(emitter, 1, displacement);
		} else {
			OsxGuestAddImmediate(emitter, OSXA64ADD, 1, OSXJITPC, displacement);
			targetoffset = offset + displacement;
		}
	}
	if (instruction & 1) {
		OsxGuestAddImmediate(emitter, OSXA64ADD, 2, OSXJITPC, 4);
		OsxGuestStore(emitter, 2, OSXJITOFFSET(lr));
	}
	if (!(options & 4)) {
		OsxGuestLoad(emitter, 0, OSXJITOFFSET(ctr));
		OsxGuestAddImmediate(emitter, OSXA64SUBS, 0, 0, 1);
		OsxGuestStore(emitter, 0, OSXJITOFFSET(ctr));
		if (options & 2)
			counterpatch = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
		else
			counterpatch = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDEQ));
	}
	if (!(options & 16)) {
		OsxGuestLoad(emitter, 2, OSXJITOFFSET(cr));
		if (options & 8)
			conditionpatch = OsxGuestForward(emitter, OsxGuestBitTest(OSXA64TBZ, 2, 31 - ((instruction >> 16) & 31)));
		else
			conditionpatch = OsxGuestForward(emitter, OsxGuestBitTest(OSXA64TBNZ, 2, 31 - ((instruction >> 16) & 31)));
	}
	OsxGuestMove(emitter, OSXJITPC, 1);
	OsxGuestExit(emitter, count + 1, targetoffset);
	if (counterpatch || conditionpatch) {
		if (counterpatch)
			OsxGuestPatch(counterpatch, emitter->cursor);
		if (conditionpatch)
			OsxGuestPatch(conditionpatch, emitter->cursor);
		return 1;
	}
	return 2;
}

static void OsxGuestLoadCarry(OsxNativeEmitter *emitter)
{
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(xer));
	OsxGuestExtract(emitter, 1, 1, 29, 1);
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 1, 1);
}

static void OsxGuestStoreCarry(OsxNativeEmitter *emitter)
{
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(xer));
	OsxGuestInsert(emitter, 1, 2, 29, 1);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(xer));
}

static void OsxGuestRequireSupervisorMode(OsxNativeEmitter *emitter, int count)
{
	OsxGuestLoad(emitter, OSXJITSCRATCH, OSXJITOFFSET(msr));
	OsxGuestMiss(emitter, OsxGuestBitTest(OSXA64TBNZ, OSXJITSCRATCH, 14), count);
}

static int OsxGuestCarrying(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
{
	int target;
	int left;
	int right;
	target = (instruction >> 21) & 31;
	left = (instruction >> 16) & 31;
	right = (instruction >> 11) & 31;
	switch (extended) {
	case 10:
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		OsxGuestLoad(emitter, 1, OSXJITGPR(right));
		OsxGuestReg3(emitter, 0x2b000000u, 0, 0, 1);
		break;
	case 8:
		OsxGuestLoad(emitter, 0, OSXJITGPR(right));
		OsxGuestLoad(emitter, 1, OSXJITGPR(left));
		OsxGuestReg3(emitter, 0x6b000000u, 0, 0, 1);
		break;
	case 138:
	case 136:
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		if (extended == 136)
			OsxGuestReg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
		OsxGuestLoadCarry(emitter);
		OsxGuestLoad(emitter, 2, OSXJITGPR(right));
		OsxGuestReg3(emitter, 0x3a000000u, 0, 0, 2);
		break;
	case 202:
	case 200:
	case 234:
	case 232:
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		if (extended == 200 || extended == 232)
			OsxGuestReg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
		OsxGuestLoadCarry(emitter);
		if (extended == 202 || extended == 200) {
			OsxGuestReg3(emitter, 0x3a000000u, 0, 0, OSXJITZERO);
		} else {
			OsxGuestConstant(emitter, 2, 0xffffffffu);
			OsxGuestReg3(emitter, 0x3a000000u, 0, 0, 2);
		}
		break;
	default:
		return 0;
	}
	OsxGuestJitSet(emitter, 2, OSXCONDHS);
	OsxGuestStore(emitter, 0, OSXJITGPR(target));
	OsxGuestStoreCarry(emitter);
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestArithmetic(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		OsxGuestReg3(emitter, 0x4b000000u, 0, OSXJITZERO, 0);
		break;
	case 11:
	case 75:
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		OsxGuestLoad(emitter, 1, OSXJITGPR(right));
		if (extended == 11) {
			OsxGuestOp(emitter, 0x9ba17c00u);
			OsxGuestOp(emitter, 0xd360fc00u);
		} else {
			OsxGuestOp(emitter, 0x9b217c00u);
			OsxGuestOp(emitter, 0x9360fc00u);
		}
		break;
	case 459:
	case 491:
		OsxGuestLoad(emitter, 1, OSXJITGPR(right));
		OsxGuestMiss(emitter, OSXA64CBZ | 1, count);
		OsxGuestLoad(emitter, 0, OSXJITGPR(left));
		if (extended == 491) {
			OsxGuestAddImmediate(emitter, OSXA64ADDS, OSXJITZERO, 1, 1);
			ordinary = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
			OsxGuestConstant(emitter, 2, 0x80000000u);
			OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 0, 2);
			OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
			OsxGuestPatch(ordinary, emitter->cursor);
			OsxGuestReg3(emitter, 0x1ac00c00u, 0, 0, 1);
		} else {
			OsxGuestReg3(emitter, 0x1ac00800u, 0, 0, 1);
		}
		break;
	default:
		return 0;
	}
	OsxGuestStore(emitter, 0, OSXJITGPR(target));
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestLogical(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended)
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
		OsxGuestLoad(emitter, 1, OSXJITGPR(amount));
		OsxGuestLoad(emitter, 0, OSXJITGPR(source));
		OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, 63);
		if (extended == 24)
			OsxGuestReg3(emitter, 0x9ac02000u, 0, 0, 1);
		else
			OsxGuestReg3(emitter, 0x9ac02400u, 0, 0, 1);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		break;
	case 26:
		OsxGuestLoad(emitter, 0, OSXJITGPR(source));
		OsxGuestOp(emitter, 0x5ac01000u);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		break;
	case 792:
		OsxGuestLoad(emitter, 1, OSXJITGPR(amount));
		OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, 63);
		OsxGuestLoad(emitter, 0, OSXJITGPR(source));
		OsxGuestOp(emitter, 0x93407c02u);
		OsxGuestReg3(emitter, 0x9ac02800u, 0, 2, 1);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		OsxGuestReg3(emitter, 0x9ac02000u, 3, 0, 1);
		OsxGuestReg3(emitter, 0xeb000000u, OSXJITZERO, 3, 2);
		OsxGuestJitSet(emitter, 3, OSXCONDNE);
		OsxGuestOp(emitter, 0xf100005fu);
		OsxGuestJitSet(emitter, 2, OSXCONDLT);
		OsxGuestReg3(emitter, 0x0a000000u, 2, 2, 3);
		OsxGuestStoreCarry(emitter);
		break;
	case 824:
		OsxGuestLoad(emitter, 0, OSXJITGPR(source));
		if (amount == 0) {
			OsxGuestConstant(emitter, 2, 0);
		} else {
			OsxGuestLogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 0, (1u << amount) - 1);
			OsxGuestJitSet(emitter, 1, OSXCONDNE);
			OsxGuestShiftRight(emitter, 2, 0, 31);
			OsxGuestReg3(emitter, 0x0a000000u, 2, 2, 1);
			OsxGuestBitfield(emitter, 0x13000000u, 0, 0, (uint32)amount, 31);
		}
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		break;
	default:
		return 0;
	}
	OsxGuestRecord(emitter, instruction);
	return 1;
}

static int OsxGuestSpecialOffset(uint32 number, uint32 *offset)
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

static void OsxGuestCallout(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	uint8 *resume;
	uint8 *running;
	emitter->enabled = 0;
	OsxGuestStore(emitter, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestReg3(emitter, 0x4b000000u, 0, OSXJITENTRYSLICE, OSXJITSLICE);
	if (count)
		OsxGuestAddImmediate(emitter, OSXA64ADD, 0, 0, (uint32)count);
	OsxGuestField(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	OsxGuestReg3(emitter, 0x8b204000u, 1, 1, 0);
	OsxGuestField(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
	OsxGuestReg3(emitter, 0xaa000000u, 0, OSXJITZERO, OSXJITCPU);
	OsxGuestConstant(emitter, 1, instruction);
	OsxGuestCallFar(emitter, (void *)OsxGuestCpuExecuteInline);
	OsxGuestReg3(emitter, 0x4b000000u, 2, OSXJITENTRYSLICE, OSXJITSLICE);
	if (count)
		OsxGuestAddImmediate(emitter, OSXA64ADD, 2, 2, (uint32)count);
	OsxGuestField(emitter, OSXA64LDRX, 1, OSXJITOFFSET(instructions));
	OsxGuestReg3(emitter, 0xcb204000u, 1, 1, 2);
	OsxGuestField(emitter, OSXA64STRX, 1, OSXJITOFFSET(instructions));
	resume = OsxGuestForward(emitter, OSXA64CBZ);
	OsxGuestCharge(emitter, count + 1);
	OsxGuestLoad(emitter, OSXJITPC, OSXJITOFFSET(pc));
	OsxGuestLoad(emitter, 0, OSXJITOFFSET(slice));
	OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
	running = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDGT));
	OsxGuestSettle(emitter);
	OsxGuestRelease(emitter, OSXJITCONTINUE);
	OsxGuestPatch(running, emitter->cursor);
	OsxGuestLeave(emitter, OSXJITCONTINUE);
	OsxGuestPatch(resume, emitter->cursor);
	OsxGuestLoad(emitter, OSXJITPC, OSXJITOFFSET(pc));
}

static int OsxGuestSystem(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
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
		OsxGuestLoad(emitter, 0, OSXJITGPR(target));
		if (mask != 0xffffffffu) {
			OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, mask);
			OsxGuestLoad(emitter, 1, OSXJITOFFSET(cr));
			OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, ~mask);
			OsxGuestReg3(emitter, 0x2a000000u, 0, 0, 1);
		}
		OsxGuestStore(emitter, 0, OSXJITOFFSET(cr));
		return 1;
	case 83:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestLoad(emitter, 0, OSXJITOFFSET(msr));
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		return 1;
	case 146:
		emitter->enabled = 0;
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestLoad(emitter, 1, OSXJITGPR(target));
		OsxGuestLoad(emitter, 2, OSXJITOFFSET(msr));
		OsxGuestReg3(emitter, 0x4a000000u, 2, 2, 1);
		OsxGuestLogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 2, ~(uint32)OSXMSREE);
		slowmask = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
		OsxGuestLogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 1, OSXMSRPOW);
		slowpower = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
		OsxGuestStore(emitter, 1, OSXJITOFFSET(msr));
		OsxGuestReg3(emitter, 0x6a000000u, 2, 2, 1);
		quiet = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDEQ));
		OsxGuestLoad(emitter, 2, OSXJITOFFSET(externalline));
		OsxGuestLoad(emitter, 3, OSXJITOFFSET(decpending));
		OsxGuestReg3(emitter, 0x2a000000u, 2, 2, 3);
		pending = OsxGuestForward(emitter, OSXA64CBZ | 2);
		OsxGuestAddImmediate(emitter, OSXA64ADD, OSXJITPC, OSXJITPC, 4);
		OsxGuestCharge(emitter, count + 1);
		OsxGuestSettle(emitter);
		OsxGuestStore(emitter, OSXJITZERO, OSXJITOFFSET(slice));
		OsxGuestRelease(emitter, OSXJITCONTINUE);
		OsxGuestPatch(quiet, emitter->cursor);
		OsxGuestPatch(pending, emitter->cursor);
		done = OsxGuestForward(emitter, OSXA64B);
		OsxGuestPatch(slowmask, emitter->cursor);
		OsxGuestPatch(slowpower, emitter->cursor);
		OsxGuestCallout(emitter, instruction, count);
		OsxGuestPatch(done, emitter->cursor);
		return 1;
	case 595:
		OsxGuestRequireSupervisorMode(emitter, count);
		OsxGuestLoad(emitter, 0, OSXJITOFFSET(sr) + ((instruction >> 16) & 15) * 4);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		return 1;
	case 371:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if ((number != 268 && number != 269) || emitter->deterministic)
			return 0;
		OsxGuestBranchTo(emitter, OSXA64BL, emitter->state->timethunk);
		if (number == 269)
			OsxGuestOp(emitter, 0xd360fc00u);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		return 1;
	case 210:
	case 242:
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 210) {
			OsxGuestConstant(emitter, 0, (instruction >> 16) & 15);
		} else {
			OsxGuestLoad(emitter, 0, OSXJITGPR((instruction >> 11) & 31));
			OsxGuestShiftRight(emitter, 0, 0, 28);
		}
		OsxGuestShiftRight(emitter, 1, OSXJITPC, 28);
		OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 1, 0);
		OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDEQ), count);
		OsxGuestLoad(emitter, 2, OSXJITGPR(target));
		OsxGuestConstant(emitter, OSXJITSCRATCH, OSXJITOFFSET(sr));
		OsxGuestReg3(emitter, 0x8b000000u, OSXJITSCRATCH, OSXJITCPU, OSXJITSCRATCH);
		OsxGuestOp(emitter, 0xb8207800u | (OSXJITSCRATCH << 5) | 2);
		OsxGuestStore(emitter, OSXJITZERO, OSXJITOFFSET(fetchtag));
		return 1;
	case 339:
	case 467:
		number = ((instruction >> 16) & 31) | ((instruction >> 6) & 0x3e0);
		if (!OsxGuestSpecialOffset(number, &offset)) {
			if (extended != 339)
				return 0;
			OsxGuestCallout(emitter, instruction, count);
			return 1;
		}
		OsxGuestRequireSupervisorMode(emitter, count);
		if (extended == 339) {
			OsxGuestLoad(emitter, 0, offset);
			OsxGuestStore(emitter, 0, OSXJITGPR(target));
		} else {
			OsxGuestLoad(emitter, 0, OSXJITGPR(target));
			OsxGuestStore(emitter, 0, offset);
		}
		return 1;
	}
	return 0;
}

static void OsxGuestConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestLoad(emitter, 0, OSXJITOFFSET(xer));
	OsxGuestShiftRight(emitter, 0, 0, 31);
}

static void OsxGuestStoreConditionZero(OsxNativeEmitter *emitter)
{
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(cr));
	OsxGuestInsert(emitter, 1, 0, 28, 4);
	OsxGuestStore(emitter, 1, OSXJITOFFSET(cr));
}

static int OsxGuestReserved(OsxNativeEmitter *emitter, uint32 instruction, uint32 extended, int count)
{
	int base;
	int index;
	uint8 *invalid;
	uint8 *elsewhere;
	uint8 *done;
	base = (instruction >> 16) & 31;
	index = (instruction >> 11) & 31;
	OsxGuestAddress(emitter, base, index, 0, 0);
	OsxGuestLogicImmediate(emitter, OSXA64ANDS, OSXJITZERO, 0, 3);
	OsxGuestMiss(emitter, OsxGuestConditional(OSXCONDNE), count);
	if (extended == 20) {
		OsxGuestMove(emitter, OSXJITUPDATE, 0);
		OsxGuestMemory(emitter, instruction, 32, index, count);
		OsxGuestStore(emitter, OSXJITUPDATE, OSXJITOFFSET(reservation));
		OsxGuestConstant(emitter, 0, 1);
		OsxGuestStore(emitter, 0, OSXJITOFFSET(reservationvalid));
		return 1;
	}
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(reservationvalid));
	invalid = OsxGuestForward(emitter, OSXA64CBZ | 1);
	OsxGuestLoad(emitter, 1, OSXJITOFFSET(reservation));
	OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 1, 0);
	elsewhere = OsxGuestForward(emitter, OsxGuestConditional(OSXCONDNE));
	OsxGuestMemory(emitter, instruction, 36, index, count);
	OsxGuestConditionZero(emitter);
	OsxGuestLogicImmediate(emitter, OSXA64ORR, 0, 0, 2);
	done = OsxGuestForward(emitter, OSXA64B);
	OsxGuestPatch(invalid, emitter->cursor);
	OsxGuestPatch(elsewhere, emitter->cursor);
	OsxGuestConditionZero(emitter);
	OsxGuestPatch(done, emitter->cursor);
	OsxGuestStore(emitter, OSXJITZERO, OSXJITOFFSET(reservationvalid));
	OsxGuestStoreConditionZero(emitter);
	return 1;
}

static int OsxGuestZeroBlock(OsxNativeEmitter *emitter, uint32 instruction, int count)
{
	OsxGuestAddress(emitter, (instruction >> 16) & 31, (instruction >> 11) & 31, 0, 0);
	OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, ~31u);
	OsxGuestTranslate(emitter, 1, 0xfe0, count);
	OsxGuestOp(emitter, 0xa9007c3fu);
	OsxGuestOp(emitter, 0xa9017c3fu);
	return 1;
}

static int OsxGuestConditionLogical(OsxNativeEmitter *emitter, uint32 instruction)
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
		OsxGuestLoad(emitter, 0, OSXJITOFFSET(cr));
		OsxGuestExtract(emitter, 1, 0, (7 - (bita >> 2)) * 4, 4);
		OsxGuestInsert(emitter, 0, 1, (7 - (bitd >> 2)) * 4, 4);
		OsxGuestStore(emitter, 0, OSXJITOFFSET(cr));
		return 1;
	}
	if (extended != 33 && extended != 129 && extended != 193 && extended != 225 && extended != 257 && extended != 289 && extended != 417 && extended != 449)
		return 0;
	OsxGuestLoad(emitter, 0, OSXJITOFFSET(cr));
	OsxGuestExtract(emitter, 1, 0, 31 - bita, 1);
	OsxGuestExtract(emitter, 2, 0, 31 - bitb, 1);
	if (extended == 129 || extended == 417)
		OsxGuestLogicImmediate(emitter, OSXA64EOR, 2, 2, 1);
	if (extended == 33 || extended == 417 || extended == 449)
		OsxGuestReg3(emitter, 0x2a000000u, 1, 1, 2);
	else if (extended == 129 || extended == 225 || extended == 257)
		OsxGuestReg3(emitter, 0x0a000000u, 1, 1, 2);
	else
		OsxGuestReg3(emitter, 0x4a000000u, 1, 1, 2);
	if (extended == 33 || extended == 225 || extended == 289)
		OsxGuestLogicImmediate(emitter, OSXA64EOR, 1, 1, 1);
	OsxGuestInsert(emitter, 0, 1, 31 - bitd, 1);
	OsxGuestStore(emitter, 0, OSXJITOFFSET(cr));
	return 1;
}

static int OsxGuestInterpretable(uint32 instruction)
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

static int OsxGuestEmit(OsxNativeEmitter *emitter, uint32 instruction, int count)
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
		return OsxGuestBranch(emitter, instruction, count);
	if (opcode == 19 && ((instruction >> 1) & 0x3ff) == 150)
		return 1;
	if (opcode == 19)
		return OsxGuestConditionLogical(emitter, instruction);
	if (opcode == 46 || opcode == 47) {
		if (!OsxGuestMultiple(emitter, instruction, opcode, count))
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode == 4) {
		handler = OsxGuestCpuVectorHandler(instruction);
		if (handler) {
			OsxGuestVectorCall(emitter, instruction, handler, count);
			return 1;
		}
	}
	if (OsxGuestInterpretable(instruction)) {
		OsxGuestCallout(emitter, instruction, count);
		return 1;
	}
	if (opcode >= 32 && opcode <= 55) {
		OsxGuestMemory(emitter, instruction, opcode, -1, count);
		return 1;
	}
	switch (opcode) {
	case 59:
	case 63:
		if (!OsxGuestFloat(emitter, instruction, count))
			OsxGuestCallout(emitter, instruction, count);
		return 1;
	case 8:
		OsxGuestConstant(emitter, 0, (uint32)(int32)(int16)immediate);
		OsxGuestLoad(emitter, 1, OSXJITGPR(base));
		OsxGuestReg3(emitter, 0x6b000000u, 0, 0, 1);
		OsxGuestJitSet(emitter, 2, OSXCONDHS);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		return 1;
	case 12: case 13:
		OsxGuestLoad(emitter, 0, OSXJITGPR(base));
		OsxGuestAddImmediate(emitter, OSXA64ADDS, 0, 0, (uint32)(int32)(int16)immediate);
		OsxGuestJitSet(emitter, 2, OSXCONDHS);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		OsxGuestStoreCarry(emitter);
		if (opcode == 13) {
			OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 7:
		OsxGuestLoad(emitter, 0, OSXJITGPR(base));
		OsxGuestConstant(emitter, 1, (uint32)(int32)(int16)immediate);
		OsxGuestOp(emitter, 0x1b017c00u);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		return 1;
	case 10: case 11:
		OsxGuestLoad(emitter, 0, OSXJITGPR(base));
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, immediate);
		OsxGuestCondition(emitter, target >> 2, opcode == 11);
		return 1;
	case 14: case 15:
		if (opcode == 15)
			immediate <<= 16;
		else
			immediate = (uint32)(int32)(int16)immediate;
		OsxGuestAddress(emitter, base, -1, immediate, 0);
		OsxGuestStore(emitter, 0, OSXJITGPR(target));
		return 1;
	case 20: case 21: case 23:
		OsxGuestLoad(emitter, 0, OSXJITGPR(target));
		if (opcode == 23) {
			OsxGuestLoad(emitter, 1, OSXJITGPR(index));
			OsxGuestReg3(emitter, 0x4b000000u, 1, OSXJITZERO, 1);
			OsxGuestReg3(emitter, 0x1ac02c00u, 0, 0, 1);
		} else if (index) {
			OsxGuestOp(emitter, 0x13800000u | ((32 - (uint32)index) << 10));
		}
		mask = OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		if (mask != 0xffffffffu)
			OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, mask);
		if (opcode == 20) {
			OsxGuestLoad(emitter, 1, OSXJITGPR(base));
			OsxGuestLogicImmediate(emitter, OSXA64AND, 1, 1, ~mask);
			OsxGuestReg3(emitter, 0x2a000000u, 0, 0, 1);
		}
		OsxGuestStore(emitter, 0, OSXJITGPR(base));
		OsxGuestRecord(emitter, instruction);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		OsxGuestLoad(emitter, 0, OSXJITGPR(target));
		if (opcode & 1)
			immediate <<= 16;
		if (opcode < 26) {
			if (immediate)
				OsxGuestLogicImmediate(emitter, OSXA64ORR, 0, 0, immediate);
		} else if (opcode < 28) {
			if (immediate)
				OsxGuestLogicImmediate(emitter, OSXA64EOR, 0, 0, immediate);
		} else {
			OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, immediate);
		}
		OsxGuestStore(emitter, 0, OSXJITGPR(base));
		if (opcode >= 28) {
			OsxGuestAddImmediate(emitter, OSXA64SUBS, OSXJITZERO, 0, 0);
			OsxGuestCondition(emitter, 0, 1);
		}
		return 1;
	case 31:
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 23 || extended == 55 || extended == 87 || extended == 119 || extended == 151 || extended == 183 || extended == 215 || extended == 247 || extended == 279 || extended == 311 || extended == 343 || extended == 375 || extended == 407 || extended == 439) {
			opcode = 32 + ((extended >> 5) & 14) + ((extended >> 5) & 1);
			OsxGuestMemory(emitter, instruction, opcode, index, count);
			return 1;
		}
		if (extended == 534 || extended == 662) {
			OsxGuestMemory(emitter, instruction, 96 + ((extended >> 5) & 4), index, count);
			return 1;
		}
		if (extended >= 535 && extended <= 759 && (extended & 31) == 23) {
			OsxGuestMemory(emitter, instruction, 48 + ((extended - 535) >> 5), index, count);
			return 1;
		}
		if (extended == 0 || extended == 32) {
			OsxGuestLoad(emitter, 0, OSXJITGPR(base));
			OsxGuestLoad(emitter, 1, OSXJITGPR(index));
			OsxGuestReg3(emitter, 0x6b000000u, OSXJITZERO, 0, 1);
			OsxGuestCondition(emitter, target >> 2, extended == 0);
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
				return OsxGuestSystem(emitter, instruction, extended, count);
			if (extended == 339) {
				OsxGuestLoad(emitter, 0, offset);
				OsxGuestStore(emitter, 0, OSXJITGPR(target));
			} else {
				OsxGuestLoad(emitter, 0, OSXJITGPR(target));
				if (number == 1)
					OsxGuestLogicImmediate(emitter, OSXA64AND, 0, 0, 0xe000007fu);
				OsxGuestStore(emitter, 0, offset);
			}
			return 1;
		}
		if (extended == 19) {
			OsxGuestLoad(emitter, 0, OSXJITOFFSET(cr));
			OsxGuestStore(emitter, 0, OSXJITGPR(target));
			return 1;
		}
		if (extended == 266 || extended == 40 || extended == 235) {
			OsxGuestLoad(emitter, 0, OSXJITGPR(base));
			OsxGuestLoad(emitter, 1, OSXJITGPR(index));
			if (extended == 266)
				OsxGuestReg3(emitter, 0x0b000000u, 0, 0, 1);
			else if (extended == 40)
				OsxGuestReg3(emitter, 0x4b000000u, 0, 1, 0);
			else
				OsxGuestOp(emitter, 0x1b017c00u);
			OsxGuestStore(emitter, 0, OSXJITGPR(target));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 28 || extended == 60 || extended == 124 || extended == 284 || extended == 316 || extended == 412 || extended == 444 || extended == 476) {
			OsxGuestLoad(emitter, 0, OSXJITGPR(target));
			OsxGuestLoad(emitter, 1, OSXJITGPR(index));
			if (extended == 28 || extended == 476)
				OsxGuestReg3(emitter, 0x0a000000u, 0, 0, 1);
			else if (extended == 60)
				OsxGuestReg3(emitter, 0x0a200000u, 0, 0, 1);
			else if (extended == 284)
				OsxGuestReg3(emitter, 0x4a200000u, 0, 0, 1);
			else if (extended == 316)
				OsxGuestReg3(emitter, 0x4a000000u, 0, 0, 1);
			else if (extended == 412)
				OsxGuestReg3(emitter, 0x2a200000u, 0, 0, 1);
			else
				OsxGuestReg3(emitter, 0x2a000000u, 0, 0, 1);
			if (extended == 124 || extended == 476)
				OsxGuestReg3(emitter, 0x2a200000u, 0, OSXJITZERO, 0);
			OsxGuestStore(emitter, 0, OSXJITGPR(base));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		if (extended == 20 || extended == 150)
			return OsxGuestReserved(emitter, instruction, extended, count);
		if (extended == 1014)
			return OsxGuestZeroBlock(emitter, instruction, count);
		if (OsxGuestCarrying(emitter, instruction, extended) || OsxGuestArithmetic(emitter, instruction, extended, count) || OsxGuestLogical(emitter, instruction, extended) || OsxGuestSystem(emitter, instruction, extended, count))
			return 1;
		if (extended == 922 || extended == 954) {
			OsxGuestLoad(emitter, 0, OSXJITGPR(target));
			if (extended == 922)
				OsxGuestBitfield(emitter, 0x13000000u, 0, 0, 0, 15);
			else
				OsxGuestBitfield(emitter, 0x13000000u, 0, 0, 0, 7);
			OsxGuestStore(emitter, 0, OSXJITGPR(base));
			OsxGuestRecord(emitter, instruction);
			return 1;
		}
		return 0;
	}
	return 0;
}

int OsxGuestJitCreate(OsxCpu *cpu)
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
	state->writebase = OsxGuestJitAllocate(OSXJITBYTES);
	state->executebase = state->writebase;
#endif
	cpu->nativejit = state;
	if (state->versions == NULL || state->compiledpages == NULL || state->records == NULL || state->writebase == NULL || state->executebase == NULL) {
		OsxGuestJitDestroy(cpu);
		cpu->nativejit = NULL;
		return 0;
	}
	OsxGuestEmitThunks(state);
	return 1;
}

void OsxGuestJitDestroy(OsxCpu *cpu)
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
		OsxGuestJitRelease(state->writebase, OSXJITBYTES);
#endif
	free(state->versions);
	free(state->compiledpages);
	free(state->records);
	free(state);
}

int OsxGuestJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
	OsxNativeState *state;
	uint32 first;
	uint32 last;
	int invalidated;
#if OSXG5
	if (cpu->core970) {
		OsxGuest970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
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

static int OsxGuestNeedsPc(uint32 instruction)
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

static void OsxGuestCompile(OsxCpu *cpu, OsxNativeState *state, OsxNativeBlock *block, uint32 physical, uint32 mode)
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
	OsxGuestOp(&emitter, 0xa9bc7bfdu);
	OsxGuestOp(&emitter, 0x910003fdu);
	OsxGuestOp(&emitter, 0xa90153f3u);
	OsxGuestOp(&emitter, 0xa9025bf5u);
	OsxGuestOp(&emitter, 0xa90363f7u);
	OsxGuestReg3(&emitter, 0xaa000000u, OSXJITCPU, OSXJITZERO, 0);
	OsxGuestLoad(&emitter, OSXJITSLICE, OSXJITOFFSET(slice));
	OsxGuestMove(&emitter, OSXJITENTRYSLICE, OSXJITSLICE);
	OsxGuestLoad(&emitter, OSXJITPC, OSXJITOFFSET(pc));
	state->entry = (uint32)(emitter.cursor - start);
	OsxGuestOp(&emitter, OSXA64SUBS | (OSXJITSLICE << 5) | OSXJITZERO);
	sliceshort = OsxGuestForward(&emitter, OsxGuestConditional(OSXCONDLT));
	limit = (int)((4096 - (physical & 4095)) >> 2);
	if (limit > OSXJITLIMIT)
		limit = OSXJITLIMIT;
	count = 0;
	status = 0;
	pendingpc = 0;
	while (count < limit && emitter.misscount < OSXJITMISSES - 128) {
		memcpy(&word, cpu->ram + physical + count * 4, 4);
		instruction = tswap32(word);
		if (pendingpc && OsxGuestNeedsPc(instruction)) {
			OsxGuestAddImmediate(&emitter, OSXA64ADD, OSXJITPC, OSXJITPC, pendingpc);
			pendingpc = 0;
		}
		status = OsxGuestEmit(&emitter, instruction, count);
		if (status == 0)
			break;
		count++;
		if (status == 2)
			break;
		pendingpc += 4;
	}
	if (pendingpc)
		OsxGuestAddImmediate(&emitter, OSXA64ADD, OSXJITPC, OSXJITPC, pendingpc);
	if (status == 0)
		OsxGuestReturn(&emitter, count);
	else if (status == 1)
		OsxGuestExit(&emitter, count, (physical & 4095) + (uint32)count * 4);
	for (index = 0; index < emitter.misscount; index++) {
		shared = 0;
		while (shared < index && emitter.misscounts[shared] != emitter.misscounts[index])
			shared++;
		if (shared < index) {
			stubs[index] = stubs[shared];
		} else {
			stubs[index] = emitter.cursor;
			OsxGuestReturn(&emitter, emitter.misscounts[index]);
		}
		OsxGuestPatch(emitter.misses[index], stubs[index]);
	}
	OsxGuestPatch(sliceshort, emitter.cursor);
	OsxGuestReturn(&emitter, 0);
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
	OsxGuestPublish(state, state->executebase + state->used, (uint32)(emitter.cursor - start));
	state->used += ((uint32)(emitter.cursor - start) + 15) & ~15u;
}

static void OsxGuestLinkDynamic(OsxCpu *cpu, OsxNativeState *state, OsxNativeRecord *record, OsxNativeBlock *block, uint32 physical)
{
	record->address = cpu->pc;
	record->host = cpu->fetchhost;
	record->versionaddress = &state->versions[physical >> 12];
	record->version = state->versions[physical >> 12];
	record->target = (uint8 *)block->function + state->entry;
	state->linked++;
}

static void OsxGuestLink(OsxNativeState *state, uint8 *jump, OsxNativeBlock *block)
{
	uint32 word;
	word = OSXA64B | ((uint32)(((uint8 *)block->function + state->entry - jump) / 4) & 0x03ffffff);
	memcpy(jump + (state->writebase - state->executebase), &word, 4);
	OsxGuestPublish(state, jump, 4);
	state->linked++;
}

static void OsxGuestInterpretRun(OsxCpu *cpu, OsxNativeState *state)
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
		executed++;
	} while (cpu->pc == expected && executed < OSXJITLIMIT && cpu->slice > 0 && (cpu->pc & 4095) != 0 && !cpu->halted);
}

void OsxGuestJitRun(OsxCpu *cpu, int32 count)
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
		OsxGuestCpuRun(cpu, count);
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
			OsxGuestCpuRun(cpu, cpu->slice);
			return;
		}
		if (((cpu->pc & 0xfffff000) | 1) != cpu->fetchtag && !OsxGuestCpuFetchPage(cpu)) {
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
						OsxGuestCompile(cpu, state, block, physical, mode);
				}
				if (block->count > 0) {
					state->front[hash >> 18] = *block;
					block = &state->front[hash >> 18];
				}
			}
			if (cpu->jitlink) {
				if (block->count > 0 && mode == linkmode) {
					if (linkkind == OSXJITLINK)
						OsxGuestLink(state, cpu->jitlink, block);
					else
						OsxGuestLinkDynamic(cpu, state, (OsxNativeRecord *)cpu->jitlink, block, physical);
				}
				cpu->jitlink = NULL;
			}
			if (block->count < 0) {
				OsxGuestInterpretRun(cpu, state);
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
		OsxGuestCpuExecuteInstruction(cpu, instruction);
		state->interpreted++;
	}
}

#if OSXPROFILEJIT
void OsxGuestJitTmpStats(OsxCpu *cpu, FILE *log)
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

int OsxGuestJitCreate(OsxCpu *cpu)
{
	return 0;
}

void OsxGuestJitDestroy(OsxCpu *cpu)
{
}

int OsxGuestJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length)
{
#if OSXG5
	if (cpu->core970) {
		OsxGuest970InvalidatePhysical((Osx970 *)cpu->core970, address, length);
		return 1;
	}
#endif
	return 0;
}

void OsxGuestJitRun(OsxCpu *cpu, int32 count)
{
	OsxGuestCpuRun(cpu, count);
}

#if OSXPROFILEJIT
void OsxGuestJitTmpStats(OsxCpu *cpu, FILE *log)
{
}
#endif

#endif
