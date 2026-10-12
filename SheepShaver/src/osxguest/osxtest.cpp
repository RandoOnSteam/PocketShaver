/*
 *  osxtest.cpp - Routines for OSX emulation testing
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
#include "osxcpu.h"
#include "osxguest.h"

#define TESTKINDALU 0
#define TESTKINDMEMORY 1
#define TESTKINDFLOAT 2
#define TESTKINDFLOATMEMORY 3
#define TESTKINDMULTIPLE 4
#define TESTKINDSTRING 5

#define TESTFLAGUPDATE 1
#define TESTFLAGINDEXED 2
#define TESTFLAGSTORE 4

#define TESTCODEOFFSET 0x20000
#define TESTDATAOFFSET 0x1f000
#define TESTDATASIZE 0x1000
#define TESTMAXREPORTS 400
#define TESTEXECRETURN 0x18000001

typedef struct OsxTestTemplate {
	const char *name;
	uint32 base;
	uint32 randommask;
	int kind;
	int flags;
} OsxTestTemplate;

typedef struct OsxTestRandom {
	uint64 state;
} OsxTestRandom;

static uint32 NextRandom(OsxTestRandom *random)
{
	uint64 value;
	value = random->state;
	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	random->state = value;
	return (uint32)(value >> 16);
}

static uint32 InterestingInteger(OsxTestRandom *random)
{
	static const uint32 values[] = {
		0, 1, 2, 3, 0xffffffffu, 0xfffffffeu, 0x80000000u, 0x7fffffffu, 0x80000001u,
		0xffff, 0x10000, 0x8000, 0xffff8000u, 31, 32, 33, 63, 64, 0xff, 0x100,
		0x55555555u, 0xaaaaaaaau, 0x0000ffffu, 0xffff0000u
	};
	uint32 choice;
	choice = NextRandom(random) % 3;
	if (choice == 0)
		return values[NextRandom(random) % (sizeof(values) / sizeof(values[0]))];
	if (choice == 1)
		return NextRandom(random) % 70;
	return NextRandom(random) ^ (NextRandom(random) << 16);
}

static uint64 InterestingFloat(OsxTestRandom *random)
{
	static const uint64 values[] = {
		0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
		0x3fe0000000000000ULL, 0x4008000000000000ULL, 0x7e37e43c8800759cULL, 0x81d3f6e2a8f5c1b4ULL,
		0x0000000000000001ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL,
		0x7ff4000000000000ULL, 0x41e0000000000000ULL, 0xc1e0000000000000ULL, 0x41dfffffffe00000ULL,
		0x3ff8000000000000ULL, 0x4004000000000000ULL, 0x47efffffe0000000ULL, 0x3810000000000000ULL,
		0x36a0000000000000ULL, 0x4330000000000000ULL, 0x3fd5555555555555ULL, 0x400921fb54442d18ULL,
		0xc00921fb54442d18ULL, 0x7fefffffffffffffULL, 0x0010000000000000ULL, 0x41efffffffe00000ULL
	};
	uint32 choice;
	OsxFloat value;
	choice = NextRandom(random) % 4;
	if (choice == 0)
		return values[NextRandom(random) % (sizeof(values) / sizeof(values[0]))];
	if (choice == 1) {
		value.value = (double)(int32)NextRandom(random) / (double)(1 + (NextRandom(random) & 0xffff));
		return value.bits;
	}
	if (choice == 2) {
		value.value = (double)(float)((double)(int32)NextRandom(random) * 1.0e-3);
		return value.bits;
	}
	return ((uint64)NextRandom(random) << 32) ^ ((uint64)NextRandom(random) << 16) ^ NextRandom(random);
}

static const OsxTestTemplate *Templates(int *count)
{
	static const OsxTestTemplate templates[] = {
		{ "addi", 14u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "addis", 15u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "addic", 12u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "addic.", 13u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "subfic", 8u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "mulli", 7u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "cmpi", 11u << 26, 0x039fffff, TESTKINDALU, 0 },
		{ "cmpli", 10u << 26, 0x039fffff, TESTKINDALU, 0 },
		{ "ori", 24u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "oris", 25u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "xori", 26u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "xoris", 27u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "andi.", 28u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "andis.", 29u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "rlwimi", 20u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "rlwinm", 21u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "rlwnm", 23u << 26, 0x03ffffff, TESTKINDALU, 0 },
		{ "add", (31u << 26) | (266u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "addc", (31u << 26) | (10u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "adde", (31u << 26) | (138u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "addme", (31u << 26) | (234u << 1), 0x03ff0401, TESTKINDALU, 0 },
		{ "addze", (31u << 26) | (202u << 1), 0x03ff0401, TESTKINDALU, 0 },
		{ "subf", (31u << 26) | (40u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "subfc", (31u << 26) | (8u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "subfe", (31u << 26) | (136u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "subfme", (31u << 26) | (232u << 1), 0x03ff0401, TESTKINDALU, 0 },
		{ "subfze", (31u << 26) | (200u << 1), 0x03ff0401, TESTKINDALU, 0 },
		{ "neg", (31u << 26) | (104u << 1), 0x03ff0401, TESTKINDALU, 0 },
		{ "mullw", (31u << 26) | (235u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "divw", (31u << 26) | (491u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "divwu", (31u << 26) | (459u << 1), 0x03fff401, TESTKINDALU, 0 },
		{ "mulhw", (31u << 26) | (75u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "mulhwu", (31u << 26) | (11u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "and", (31u << 26) | (28u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "andc", (31u << 26) | (60u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "or", (31u << 26) | (444u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "orc", (31u << 26) | (412u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "xor", (31u << 26) | (316u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "nor", (31u << 26) | (124u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "nand", (31u << 26) | (476u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "eqv", (31u << 26) | (284u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "slw", (31u << 26) | (24u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "srw", (31u << 26) | (536u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "sraw", (31u << 26) | (792u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "srawi", (31u << 26) | (824u << 1), 0x03fff801, TESTKINDALU, 0 },
		{ "cntlzw", (31u << 26) | (26u << 1), 0x03ff0001, TESTKINDALU, 0 },
		{ "extsb", (31u << 26) | (954u << 1), 0x03ff0001, TESTKINDALU, 0 },
		{ "extsh", (31u << 26) | (922u << 1), 0x03ff0001, TESTKINDALU, 0 },
		{ "cmp", (31u << 26) | (0u << 1), 0x039ff800, TESTKINDALU, 0 },
		{ "cmpl", (31u << 26) | (32u << 1), 0x039ff800, TESTKINDALU, 0 },
		{ "mfcr", (31u << 26) | (19u << 1), 0x03e00000, TESTKINDALU, 0 },
		{ "mtcrf", (31u << 26) | (144u << 1), 0x03eff000, TESTKINDALU, 0 },
		{ "mcrxr", (31u << 26) | (512u << 1), 0x03800000, TESTKINDALU, 0 },
		{ "mcrf", (19u << 26) | (0u << 1), 0x039c0000, TESTKINDALU, 0 },
		{ "crand", (19u << 26) | (257u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "crandc", (19u << 26) | (129u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "creqv", (19u << 26) | (289u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "crnand", (19u << 26) | (225u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "crnor", (19u << 26) | (33u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "cror", (19u << 26) | (449u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "crorc", (19u << 26) | (417u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "crxor", (19u << 26) | (193u << 1), 0x03fff800, TESTKINDALU, 0 },
		{ "lwz", 32u << 26, 0, TESTKINDMEMORY, 0 },
		{ "lwzu", 33u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE },
		{ "lbz", 34u << 26, 0, TESTKINDMEMORY, 0 },
		{ "lbzu", 35u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE },
		{ "stw", 36u << 26, 0, TESTKINDMEMORY, TESTFLAGSTORE },
		{ "stwu", 37u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "stb", 38u << 26, 0, TESTKINDMEMORY, TESTFLAGSTORE },
		{ "stbu", 39u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "lhz", 40u << 26, 0, TESTKINDMEMORY, 0 },
		{ "lhzu", 41u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE },
		{ "lha", 42u << 26, 0, TESTKINDMEMORY, 0 },
		{ "lhau", 43u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE },
		{ "sth", 44u << 26, 0, TESTKINDMEMORY, TESTFLAGSTORE },
		{ "sthu", 45u << 26, 0, TESTKINDMEMORY, TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "lmw", 46u << 26, 0, TESTKINDMULTIPLE, 0 },
		{ "stmw", 47u << 26, 0, TESTKINDMULTIPLE, TESTFLAGSTORE },
		{ "lfs", 48u << 26, 0, TESTKINDFLOATMEMORY, 0 },
		{ "lfsu", 49u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGUPDATE },
		{ "lfd", 50u << 26, 0, TESTKINDFLOATMEMORY, 0 },
		{ "lfdu", 51u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGUPDATE },
		{ "stfs", 52u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGSTORE },
		{ "stfsu", 53u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "stfd", 54u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGSTORE },
		{ "stfdu", 55u << 26, 0, TESTKINDFLOATMEMORY, TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "lwzx", (31u << 26) | (23u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "lwzux", (31u << 26) | (55u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "lbzx", (31u << 26) | (87u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "lbzux", (31u << 26) | (119u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "lhzx", (31u << 26) | (279u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "lhzux", (31u << 26) | (311u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "lhax", (31u << 26) | (343u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "lhaux", (31u << 26) | (375u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "stwx", (31u << 26) | (151u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "stwux", (31u << 26) | (183u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "stbx", (31u << 26) | (215u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "stbux", (31u << 26) | (247u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "sthx", (31u << 26) | (407u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "sthux", (31u << 26) | (439u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "lwbrx", (31u << 26) | (534u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "lhbrx", (31u << 26) | (790u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED },
		{ "stwbrx", (31u << 26) | (662u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "sthbrx", (31u << 26) | (918u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "dcbz", (31u << 26) | (1014u << 1), 0, TESTKINDMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "lfsx", (31u << 26) | (535u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED },
		{ "lfsux", (31u << 26) | (567u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "lfdx", (31u << 26) | (599u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED },
		{ "lfdux", (31u << 26) | (631u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE },
		{ "stfsx", (31u << 26) | (663u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "stfsux", (31u << 26) | (695u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "stfdx", (31u << 26) | (727u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "stfdux", (31u << 26) | (759u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGUPDATE | TESTFLAGSTORE },
		{ "stfiwx", (31u << 26) | (983u << 1), 0, TESTKINDFLOATMEMORY, TESTFLAGINDEXED | TESTFLAGSTORE },
		{ "lswi", (31u << 26) | (597u << 1), 0, TESTKINDSTRING, 0 },
		{ "stswi", (31u << 26) | (725u << 1), 0, TESTKINDSTRING, TESTFLAGSTORE },
		{ "fdiv", (63u << 26) | (18u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fsub", (63u << 26) | (20u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fadd", (63u << 26) | (21u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fsel", (63u << 26) | (23u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fmul", (63u << 26) | (25u << 1), 0x03ff07c1, TESTKINDFLOAT, 0 },
		{ "frsqrte", (63u << 26) | (26u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fmsub", (63u << 26) | (28u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fmadd", (63u << 26) | (29u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fnmsub", (63u << 26) | (30u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fnmadd", (63u << 26) | (31u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fdivs", (59u << 26) | (18u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fsubs", (59u << 26) | (20u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fadds", (59u << 26) | (21u << 1), 0x03fff001, TESTKINDFLOAT, 0 },
		{ "fres", (59u << 26) | (24u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fmuls", (59u << 26) | (25u << 1), 0x03ff07c1, TESTKINDFLOAT, 0 },
		{ "fmsubs", (59u << 26) | (28u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fmadds", (59u << 26) | (29u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fnmsubs", (59u << 26) | (30u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fnmadds", (59u << 26) | (31u << 1), 0x03ffffc1, TESTKINDFLOAT, 0 },
		{ "fcmpu", (63u << 26) | (0u << 1), 0x039ff800, TESTKINDFLOAT, 0 },
		{ "fcmpo", (63u << 26) | (32u << 1), 0x039ff800, TESTKINDFLOAT, 0 },
		{ "frsp", (63u << 26) | (12u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fctiw", (63u << 26) | (14u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fctiwz", (63u << 26) | (15u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fneg", (63u << 26) | (40u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fmr", (63u << 26) | (72u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fnabs", (63u << 26) | (136u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "fabs", (63u << 26) | (264u << 1), 0x03e0f801, TESTKINDFLOAT, 0 },
		{ "mffs", (63u << 26) | (583u << 1), 0x03e00001, TESTKINDFLOAT, 0 },
		{ "mtfsf", (63u << 26) | (711u << 1), 0x01fef801, TESTKINDFLOAT, 0 },
		{ "mtfsfi", (63u << 26) | (134u << 1), 0x0380f001, TESTKINDFLOAT, 0 },
		{ "mtfsb0", (63u << 26) | (70u << 1), 0x03e00001, TESTKINDFLOAT, 0 },
		{ "mtfsb1", (63u << 26) | (38u << 1), 0x03e00001, TESTKINDFLOAT, 0 },
		{ "mcrfs", (63u << 26) | (64u << 1), 0x039c0000, TESTKINDFLOAT, 0 }
	};
	*count = (int)(sizeof(templates) / sizeof(templates[0]));
	return templates;
}

static uint32 BuildMemoryInstruction(OsxTestRandom *random, const OsxTestTemplate *test, OsxReferenceState *state, uint32 database)
{
	uint32 instruction;
	uint32 target;
	uint32 base;
	uint32 index;
	int32 displacement;
	instruction = test->base;
	base = 1 + NextRandom(random) % 31;
	target = NextRandom(random) % 32;
	if ((test->flags & TESTFLAGUPDATE) && test->kind != TESTKINDFLOATMEMORY && !(test->flags & TESTFLAGSTORE)) {
		while (target == base)
			target = NextRandom(random) % 32;
	}
	if (test->kind == TESTKINDMULTIPLE) {
		target = 16 + NextRandom(random) % 16;
		base = 1 + NextRandom(random) % 15;
	}
	state->gpr[base] = database + 0x800 + (NextRandom(random) % 0x100);
	if (test->kind == TESTKINDMULTIPLE)
		state->gpr[base] &= ~3u;
	if (test->flags & TESTFLAGINDEXED) {
		index = NextRandom(random) % 32;
		while (index == base || index == target)
			index = NextRandom(random) % 32;
		state->gpr[index] = (uint32)((int32)(NextRandom(random) % 0x600) - 0x300);
		instruction |= (target << 21) | (base << 16) | (index << 11);
		return instruction;
	}
	displacement = (int32)(NextRandom(random) % 0x600) - 0x300;
	instruction |= (target << 21) | (base << 16) | ((uint32)displacement & 0xffff);
	return instruction;
}

static uint32 BuildStringInstruction(OsxTestRandom *random, const OsxTestTemplate *test, OsxReferenceState *state, uint32 database)
{
	uint32 target;
	uint32 count;
	target = 2 + NextRandom(random) % 20;
	count = 1 + NextRandom(random) % ((32 - target) * 4 - 1);
	if (count > 31)
		count = 31;
	state->gpr[1] = database + 0x800 + (NextRandom(random) % 0x100);
	return test->base | (target << 21) | (1u << 16) | (count << 11);
}

static void FillState(OsxTestRandom *random, OsxReferenceState *state)
{
	int index;
	for (index = 0; index < 32; index++) {
		state->gpr[index] = InterestingInteger(random);
		state->fpr[index] = InterestingFloat(random);
	}
	state->cr = NextRandom(random) ^ (NextRandom(random) << 16);
	state->xer = (NextRandom(random) & 0xe0000000u) | (NextRandom(random) & 0x7f);
	state->lr = InterestingInteger(random);
	state->ctr = InterestingInteger(random);
	state->fpscr = 0;
	if (NextRandom(random) % 4 == 0)
		state->fpscr = NextRandom(random) & 3;
	state->pc = 0;
}

static void LoadMine(OsxCpu *cpu, const OsxReferenceState *state, uint32 entry)
{
	int index;
	for (index = 0; index < 32; index++) {
		cpu->gpr[index] = state->gpr[index];
		cpu->fpr[index].bits = state->fpr[index];
	}
	cpu->cr = state->cr;
	cpu->xer = state->xer;
	cpu->lr = state->lr;
	cpu->ctr = state->ctr;
	cpu->fpscr = state->fpscr;
	cpu->pc = entry;
	cpu->msr = OSXMSRFP;
	OsxCpuUpdateModes(cpu);
	cpu->fetchtag = 0;
}

static void SaveMine(OsxCpu *cpu, OsxReferenceState *state)
{
	int index;
	for (index = 0; index < 32; index++) {
		state->gpr[index] = cpu->gpr[index];
		state->fpr[index] = cpu->fpr[index].bits;
	}
	state->cr = cpu->cr;
	state->xer = cpu->xer;
	state->lr = cpu->lr;
	state->ctr = cpu->ctr;
	state->fpscr = cpu->fpscr;
	state->pc = cpu->pc;
}

static int CompareStates(const OsxReferenceState *expected, const OsxReferenceState *actual, char *report, size_t reportsize, int *fpscronly)
{
	int index;
	int differences;
	size_t used;
	differences = 0;
	used = 0;
	report[0] = 0;
	*fpscronly = 0;
	for (index = 0; index < 32; index++) {
		if (expected->gpr[index] != actual->gpr[index] && used < reportsize - 64) {
			used += sprintf(report + used, " r%d ref=%08x new=%08x", index, expected->gpr[index], actual->gpr[index]);
			differences++;
		}
		if (expected->fpr[index] != actual->fpr[index] && used < reportsize - 80) {
			used += sprintf(report + used, " f%d ref=%016llx new=%016llx", index, (unsigned long long)expected->fpr[index], (unsigned long long)actual->fpr[index]);
			differences++;
		}
	}
	if (expected->cr != actual->cr && used < reportsize - 64) {
		used += sprintf(report + used, " cr ref=%08x new=%08x", expected->cr, actual->cr);
		differences++;
	}
	if (expected->xer != actual->xer && used < reportsize - 64) {
		used += sprintf(report + used, " xer ref=%08x new=%08x", expected->xer, actual->xer);
		differences++;
	}
	if (expected->lr != actual->lr && used < reportsize - 64) {
		used += sprintf(report + used, " lr ref=%08x new=%08x", expected->lr, actual->lr);
		differences++;
	}
	if (expected->ctr != actual->ctr && used < reportsize - 64) {
		used += sprintf(report + used, " ctr ref=%08x new=%08x", expected->ctr, actual->ctr);
		differences++;
	}
	if (expected->fpscr != actual->fpscr && used < reportsize - 64) {
		used += sprintf(report + used, " fpscr ref=%08x new=%08x", expected->fpscr, actual->fpscr);
		if (differences == 0)
			*fpscronly = 1;
		differences++;
	}
	return differences;
}

void OsxLockstepTest(uint8 *ramhost, uint32 ramsize, int iterations, const char *logpath)
{
	OsxTestRandom random;
	OsxReferenceState initial;
	OsxReferenceState expected;
	OsxReferenceState actual;
	const OsxTestTemplate *templates;
	const OsxTestTemplate *test;
	OsxCpu *cpu;
	FILE *log;
	uint8 *saved;
	uint8 *referencememory;
	uint8 *data;
	uint32 entry;
	uint32 database;
	uint32 instruction;
	char report[2048];
	int templatecount;
	int iteration;
	int failures[256];
	int fpscrfailures[256];
	int runs[256];
	int reports;
	int fpscronly;
	int memorydiffers;
	int index;
	int totalfailures;
	log = fopen(logpath, "w");
	if (log == NULL)
		return;
	cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	saved = (uint8 *)malloc(TESTDATASIZE);
	referencememory = (uint8 *)malloc(TESTDATASIZE);
	OsxCpuReset(cpu, 0x00080202, 16666666);
	cpu->ram = ramhost;
	cpu->ramsize = ramsize;
	cpu->vramsize = 0;
	cpu->logfile = log;
	entry = ramsize - TESTCODEOFFSET;
	database = ramsize - TESTDATAOFFSET;
	data = ramhost + database;
	templates = Templates(&templatecount);
	memset(failures, 0, sizeof(failures));
	memset(fpscrfailures, 0, sizeof(fpscrfailures));
	memset(runs, 0, sizeof(runs));
	random.state = 0x9e3779b97f4a7c15ULL;
	reports = 0;
	for (iteration = 0; iteration < iterations; iteration++) {
		index = (int)(NextRandom(&random) % (uint32)templatecount);
		test = &templates[index];
		FillState(&random, &initial);
		if (test->kind == TESTKINDMEMORY || test->kind == TESTKINDFLOATMEMORY || test->kind == TESTKINDMULTIPLE)
			instruction = BuildMemoryInstruction(&random, test, &initial, database);
		else if (test->kind == TESTKINDSTRING)
			instruction = BuildStringInstruction(&random, test, &initial, database);
		else
			instruction = test->base | ((NextRandom(&random) ^ (NextRandom(&random) << 16)) & test->randommask);
		for (index = 0; index < TESTDATASIZE; index++)
			saved[index] = (uint8)NextRandom(&random);
		ramhost[entry] = (uint8)(instruction >> 24);
		ramhost[entry + 1] = (uint8)(instruction >> 16);
		ramhost[entry + 2] = (uint8)(instruction >> 8);
		ramhost[entry + 3] = (uint8)instruction;
		ramhost[entry + 4] = (uint8)(TESTEXECRETURN >> 24);
		ramhost[entry + 5] = (uint8)(TESTEXECRETURN >> 16);
		ramhost[entry + 6] = (uint8)(TESTEXECRETURN >> 8);
		ramhost[entry + 7] = (uint8)TESTEXECRETURN;
		memcpy(data, saved, TESTDATASIZE);
		expected = initial;
		OsxReferenceExecute(entry, &expected);
		memcpy(referencememory, data, TESTDATASIZE);
		memcpy(data, saved, TESTDATASIZE);
		LoadMine(cpu, &initial, entry);
		OsxCpuRun(cpu, 1);
		SaveMine(cpu, &actual);
		memorydiffers = memcmp(referencememory, data, TESTDATASIZE) != 0;
		runs[test - templates]++;
		if (CompareStates(&expected, &actual, report, sizeof(report), &fpscronly) || memorydiffers || actual.pc != entry + 4) {
			if (fpscronly && !memorydiffers && actual.pc == entry + 4)
				fpscrfailures[test - templates]++;
			else
				failures[test - templates]++;
			if (reports < TESTMAXREPORTS) {
				if (memorydiffers)
					strcat(report, " memory");
				fprintf(log, "%-8s %08x%s pc=%08x\n", test->name, instruction, report, actual.pc);
				reports++;
			}
		}
	}
	totalfailures = 0;
	for (index = 0; index < templatecount; index++) {
		if (failures[index] || fpscrfailures[index])
			fprintf(log, "summary %-8s runs %d failures %d fpscr-only %d\n", templates[index].name, runs[index], failures[index], fpscrfailures[index]);
		totalfailures += failures[index];
	}
	fprintf(log, "total iterations %d failures %d\n", iterations, totalfailures);
	fclose(log);
	free(saved);
	free(referencememory);
	free(cpu);
}
