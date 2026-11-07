/*
 *  nativejitarm64.cpp - Shared OSX + Native arm64 JIT
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
#include "vm_alloc.h"
#include "cpu/jit/nativejit.h"

#if defined(NATIVEJIT_HOST_ARM64)

#if defined(_WIN32)
#include <windows.h>
#endif

#define NATIVEJITA64_CPU 19
#define NATIVEJITA64_REGS 20
#define NATIVEJITA64_MEMORY 21
#define NATIVEJITA64_SITE 9
#define NATIVEJITA64_TEMP 10
#define NATIVEJITA64_TEMP2 11
#define NATIVEJITA64_SCRATCH 16
#define NATIVEJITA64_SCRATCH2 17
#define NATIVEJITA64_ZERO 31
#define NATIVEJITA64_FSCRATCH 16

#define NATIVEJITA64_EQ 0
#define NATIVEJITA64_NE 1
#define NATIVEJITA64_HI 8
#define NATIVEJITA64_LS 9
#define NATIVEJITA64_VS 6
#define NATIVEJITA64_LO 3
#define NATIVEJITA64_LT 11
#define NATIVEJITA64_GT 12
#define NATIVEJITA64_HS 2
#define NATIVEJITA64_MI 4

#define NATIVEJITA64_ADD 0x11000000U
#define NATIVEJITA64_SUB 0x51000000U
#define NATIVEJITA64_SUBS 0x71000000U
#define NATIVEJITA64_AND 0x12000000U
#define NATIVEJITA64_ORR 0x32000000U
#define NATIVEJITA64_EOR 0x52000000U
#define NATIVEJITA64_ANDS 0x72000000U
#define NATIVEJITA64_B 0x14000000U
#define NATIVEJITA64_BCOND 0x54000000U
#define NATIVEJITA64_CBZ 0x34000000U
#define NATIVEJITA64_CBNZ 0x35000000U
#define NATIVEJITA64_TBZSIGN 0x36f80000U

static void NativeJitA64Op(NATIVEJITEMITTER* pThis, uint32 word)
{
	pThis->mCode[0] = (uint8)word;
	pThis->mCode[1] = (uint8)(word >> 8);
	pThis->mCode[2] = (uint8)(word >> 16);
	pThis->mCode[3] = (uint8)(word >> 24);
	pThis->mCode += 4;
}

static void NativeJitA64Reg3(NATIVEJITEMITTER* pThis, uint32 opcode, int target, int left, int right)
{
	NativeJitA64Op(pThis, opcode | ((uint32)right << 16) | ((uint32)left << 5) | (uint32)target);
}

static void NativeJitA64Move(NATIVEJITEMITTER* pThis, int target, int source)
{
	NativeJitA64Reg3(pThis, 0x2a000000U, target, NATIVEJITA64_ZERO, source);
}

static void NativeJitA64Move64(NATIVEJITEMITTER* pThis, int target, int source)
{
	NativeJitA64Reg3(pThis, 0xaa000000U, target, NATIVEJITA64_ZERO, source);
}

static void NativeJitA64Constant(NATIVEJITEMITTER* pThis, int reg, uint32 value)
{
	if ((value & 0xffff0000U) == 0)
		NativeJitA64Op(pThis, 0x52800000U | (value << 5) | (uint32)reg);
	else if ((value & 0xffff) == 0)
		NativeJitA64Op(pThis, 0x52a00000U | ((value >> 16) << 5) | (uint32)reg);
	else if ((~value & 0xffff0000U) == 0)
		NativeJitA64Op(pThis, 0x12800000U | ((~value & 0xffff) << 5) | (uint32)reg);
	else if ((~value & 0xffff) == 0)
		NativeJitA64Op(pThis, 0x12a00000U | ((~value >> 16) << 5) | (uint32)reg);
	else
	{
		NativeJitA64Op(pThis, 0x52800000U | ((value & 0xffff) << 5) | (uint32)reg);
		NativeJitA64Op(pThis, 0x72a00000U | ((value >> 16) << 5) | (uint32)reg);
	}
}

static void NativeJitA64Constant64(NATIVEJITEMITTER* pThis, int reg, uint64 value)
{
	uint32 part;
	int half;
	NativeJitA64Op(pThis, 0xd2800000U | ((uint32)(value & 0xffff) << 5) | (uint32)reg);
	for (half = 1; half < 4; half++)
	{
		part = (uint32)(value >> (half * 16)) & 0xffff;
		if (part)
			NativeJitA64Op(pThis, 0xf2800000U | ((uint32)half << 21) | (part << 5) | (uint32)reg);
	}
}

static void NativeJitA64AddImmediate(NATIVEJITEMITTER* pThis, uint32 opcode, int target, int source, uint32 value)
{
	if (value < 4096)
		NativeJitA64Op(pThis, opcode | (value << 10) | ((uint32)source << 5) | (uint32)target);
	else if (0U - value < 4096)
		NativeJitA64Op(pThis, (opcode ^ 0x40000000U) | ((0U - value) << 10) | ((uint32)source << 5) | (uint32)target);
	else
	{
		NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, value);
		NativeJitA64Reg3(pThis, opcode - 0x06000000U, target, source, NATIVEJITA64_SCRATCH);
	}
}

static int NativeJitA64Bitmask(uint32 value, uint32* encoded)
{
	uint32 size;
	uint32 mask;
	uint32 element;
	uint32 ones;
	uint32 pattern;
	uint32 rotation;
	uint32 rotated;
	if (value == 0 || value == 0xffffffffU)
		return 0;
	size = 32;
	while (size > 2)
	{
		mask = (1U << (size / 2)) - 1;
		if ((value & mask) != ((value >> (size / 2)) & mask))
			break;
		size /= 2;
	}
	mask = 0xffffffffU;
	if (size < 32)
		mask = (1U << size) - 1;
	element = value & mask;
	ones = 0;
	for (rotation = 0; rotation < size; rotation++)
		ones += (element >> rotation) & 1;
	pattern = (1U << ones) - 1;
	for (rotation = 0; rotation < size; rotation++)
	{
		rotated = pattern;
		if (rotation)
			rotated = ((pattern >> rotation) | (pattern << (size - rotation))) & mask;
		if (rotated == element)
		{
			*encoded = (rotation << 6) | (((~(size - 1) << 1) | (ones - 1)) & 0x3f);
			return 1;
		}
	}
	return 0;
}

static void NativeJitA64Logic(NATIVEJITEMITTER* pThis, uint32 opcode, int target, int source, uint32 value)
{
	uint32 encoded;
	if (NativeJitA64Bitmask(value, &encoded))
	{
		NativeJitA64Op(pThis, opcode | (encoded << 10) | ((uint32)source << 5) | (uint32)target);
		return;
	}
	NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, value);
	NativeJitA64Reg3(pThis, opcode - 0x08000000U, target, source, NATIVEJITA64_SCRATCH);
}

static void NativeJitA64Bitfield(NATIVEJITEMITTER* pThis, uint32 opcode, int target, int source, uint32 rotate, uint32 last)
{
	NativeJitA64Op(pThis, opcode | (rotate << 16) | (last << 10) | ((uint32)source << 5) | (uint32)target);
}

static void NativeJitA64Set(NATIVEJITEMITTER* pThis, int reg, int condition)
{
	NativeJitA64Op(pThis, 0x1a9f07e0U | ((uint32)(condition ^ 1) << 12) | (uint32)reg);
}

static void NativeJitA64Select(NATIVEJITEMITTER* pThis, int target, int whentrue, int whenfalse, int condition)
{
	NativeJitA64Op(pThis, 0x1a800000U | ((uint32)whenfalse << 16) | ((uint32)condition << 12) | ((uint32)whentrue << 5) | (uint32)target);
}

static void NativeJitA64Field(NATIVEJITEMITTER* pThis, uint32 opcode, uint32 scale, int reg, int offset)
{
	if (offset >= 0 && (uint32)offset % scale == 0 && (uint32)offset / scale < 4096)
	{
		NativeJitA64Op(pThis, opcode | (((uint32)offset / scale) << 10) | (NATIVEJITA64_REGS << 5) | (uint32)reg);
		return;
	}
	NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH2, (uint32)offset);
	NativeJitA64Op(pThis, 0x8b200000U | (NATIVEJITA64_SCRATCH2 << 16) | (6U << 13) | (NATIVEJITA64_REGS << 5) | NATIVEJITA64_SCRATCH2);
	NativeJitA64Op(pThis, opcode | (NATIVEJITA64_SCRATCH2 << 5) | (uint32)reg);
}

static void NativeJitA64Guest(NATIVEJITEMITTER* pThis, uint32 opcode, int reg, int address)
{
	NativeJitA64Op(pThis, opcode | ((uint32)address << 16) | (NATIVEJITA64_MEMORY << 5) | (uint32)reg);
}

static void NativeJitA64Far(NATIVEJITEMITTER* pThis, int reg, const void* address)
{
	NativeJitA64Constant64(pThis, reg, (uint64)(uintptr)address);
}

static void NativeJitA64Patch(uint8* site, uint8* target)
{
	uint32 word;
	uint32 distance;
	word = (uint32)site[0] | ((uint32)site[1] << 8) | ((uint32)site[2] << 16) | ((uint32)site[3] << 24);
	distance = (uint32)((target - site) / 4);
	if ((word & 0x7c000000U) == NATIVEJITA64_B)
		word = (word & 0xfc000000U) | (distance & 0x03ffffff);
	else if ((word & 0x7e000000U) == 0x36000000U)
		word = (word & 0xfff8001fU) | ((distance & 0x3fff) << 5);
	else
		word = (word & 0xff00001fU) | ((distance & 0x7ffff) << 5);
	site[0] = (uint8)word;
	site[1] = (uint8)(word >> 8);
	site[2] = (uint8)(word >> 16);
	site[3] = (uint8)(word >> 24);
}

static uint8* NativeJitA64Forward(NATIVEJITEMITTER* pThis, uint32 word)
{
	NativeJitA64Op(pThis, word);
	return pThis->mCode - 4;
}

static void NativeJitA64Land(NATIVEJITEMITTER* pThis, uint8* site)
{
	NativeJitA64Patch(site, pThis->mCode);
}

uint8* NativeJitAllocate(size_t size)
{
	void* code;
	code = vm_acquire(size, VM_MAP_DEFAULT);
	if (code == VM_MAP_FAILED)
		return NULL;
	if (vm_protect(code, size, VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE) < 0)
	{
		vm_release(code, size);
		return NULL;
	}
	return (uint8*)code;
}

void NativeJitFree(uint8* code, size_t size)
{
	vm_release(code, size);
}

void NativeJitFlush(uint8* start, size_t length)
{
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), start, length);
#else
	__builtin___clear_cache((char*)start, (char*)start + length);
#endif
}

void NativeJitBegin(NATIVEJITEMITTER* pThis, uint8* start, uint8* limit,
	int carryoffset, int summaryoverflowoffset)
{
	pThis->mStart = start;
	pThis->mCode = start;
	pThis->mLimit = limit;
	pThis->mCarryOffset = carryoffset;
	pThis->mSummaryOverflowOffset = summaryoverflowoffset;
}

int NativeJitHasRoom(const NATIVEJITEMITTER* pThis)
{
	return pThis->mCode + NATIVEJIT_BLOCK_RESERVE < pThis->mLimit;
}

void NativeJitPrologue(NATIVEJITEMITTER* pThis)
{
	NativeJitA64Op(pThis, 0xa9bd7bfdU);
	NativeJitA64Op(pThis, 0x910003fdU);
	NativeJitA64Op(pThis, 0xa90153f3U);
	NativeJitA64Op(pThis, 0xf90013f5U);
	NativeJitA64Move64(pThis, NATIVEJITA64_CPU, 0);
	NativeJitA64Move64(pThis, NATIVEJITA64_REGS, 1);
	NativeJitA64Move64(pThis, NATIVEJITA64_MEMORY, 2);
}

void NativeJitEntryStub(NATIVEJITEMITTER* pThis)
{
	NativeJitPrologue(pThis);
	NativeJitA64Op(pThis, 0xd61f0060U);
}

void NativeJitEpilogue(NATIVEJITEMITTER* pThis)
{
	NativeJitA64Op(pThis, 0xf94013f5U);
	NativeJitA64Op(pThis, 0xa94153f3U);
	NativeJitA64Op(pThis, 0xa8c37bfdU);
	NativeJitA64Op(pThis, 0xd65f03c0U);
}

void NativeJitLoadImmediate(NATIVEJITEMITTER* pThis, int target, uint32 value)
{
	NativeJitA64Constant(pThis, target, value);
}

void NativeJitMove(NATIVEJITEMITTER* pThis, int target, int source)
{
	if (target != source)
		NativeJitA64Move(pThis, target, source);
}

void NativeJitLoadRegister(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitA64Field(pThis, 0xb9400000U, 4, target, offset);
}

void NativeJitStoreRegister(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitA64Field(pThis, 0xb9000000U, 4, source, offset);
}

void NativeJitStoreRegisterImmediate(NATIVEJITEMITTER* pThis, int offset, uint32 value)
{
	NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, value);
	NativeJitA64Field(pThis, 0xb9000000U, 4, NATIVEJITA64_SCRATCH, offset);
}

void NativeJitLoadRegisterByte(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitA64Field(pThis, 0x39400000U, 1, target, offset);
}

void NativeJitStoreRegisterByte(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitA64Field(pThis, 0x39000000U, 1, source, offset);
}

void NativeJitOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitA64Reg3(pThis, 0x0b000000U, target, target, source);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitA64Reg3(pThis, 0x4b000000U, target, target, source);
		break;
	case NATIVEJIT_ALU_AND:
		NativeJitA64Reg3(pThis, 0x0a000000U, target, target, source);
		break;
	case NATIVEJIT_ALU_OR:
		NativeJitA64Reg3(pThis, 0x2a000000U, target, target, source);
		break;
	case NATIVEJIT_ALU_XOR:
		NativeJitA64Reg3(pThis, 0x4a000000U, target, target, source);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitA64Op(pThis, 0x1b007c00U | ((uint32)source << 16) | ((uint32)target << 5) | (uint32)target);
		break;
	case NATIVEJIT_ALU_MULHS:
		NativeJitA64Op(pThis, 0x9b207c00U | ((uint32)source << 16) | ((uint32)target << 5) | NATIVEJITA64_SCRATCH);
		NativeJitA64Bitfield(pThis, 0xd3400000U, target, NATIVEJITA64_SCRATCH, 32, 63);
		break;
	case NATIVEJIT_ALU_MULHU:
		NativeJitA64Op(pThis, 0x9ba07c00U | ((uint32)source << 16) | ((uint32)target << 5) | NATIVEJITA64_SCRATCH);
		NativeJitA64Bitfield(pThis, 0xd3400000U, target, NATIVEJITA64_SCRATCH, 32, 63);
		break;
	case NATIVEJIT_ALU_SHL:
		NativeJitA64Reg3(pThis, 0x9ac02000U, NATIVEJITA64_SCRATCH, target, source);
		NativeJitA64Move(pThis, target, NATIVEJITA64_SCRATCH);
		break;
	case NATIVEJIT_ALU_SHR:
		NativeJitA64Reg3(pThis, 0x9ac02400U, target, target, source);
		break;
	case NATIVEJIT_ALU_SAR:
		NativeJitA64Bitfield(pThis, 0x93400000U, NATIVEJITA64_SCRATCH, target, 0, 31);
		NativeJitA64Reg3(pThis, 0x9ac02800U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, source);
		NativeJitA64Move(pThis, target, NATIVEJITA64_SCRATCH);
		break;
	case NATIVEJIT_ALU_ROL:
		NativeJitA64Reg3(pThis, 0x4b000000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_ZERO, source);
		NativeJitA64Reg3(pThis, 0x1ac02c00U, target, target, NATIVEJITA64_SCRATCH);
		break;
	}
}

void NativeJitOperateImmediate(NATIVEJITEMITTER* pThis, int operation, int target, uint32 value)
{
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitA64AddImmediate(pThis, NATIVEJITA64_ADD, target, target, value);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitA64AddImmediate(pThis, NATIVEJITA64_SUB, target, target, value);
		break;
	case NATIVEJIT_ALU_AND:
		if (value == 0)
			NativeJitA64Constant(pThis, target, 0);
		else if (value != 0xffffffffU)
			NativeJitA64Logic(pThis, NATIVEJITA64_AND, target, target, value);
		break;
	case NATIVEJIT_ALU_OR:
		if (value == 0xffffffffU)
			NativeJitA64Constant(pThis, target, value);
		else if (value)
			NativeJitA64Logic(pThis, NATIVEJITA64_ORR, target, target, value);
		break;
	case NATIVEJIT_ALU_XOR:
		if (value == 0xffffffffU)
			NativeJitA64Reg3(pThis, 0x2a200000U, target, NATIVEJITA64_ZERO, target);
		else if (value)
			NativeJitA64Logic(pThis, NATIVEJITA64_EOR, target, target, value);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, value);
		NativeJitA64Op(pThis, 0x1b007c00U | (NATIVEJITA64_SCRATCH << 16) | ((uint32)target << 5) | (uint32)target);
		break;
	case NATIVEJIT_ALU_SHL:
		if (value & 31)
			NativeJitA64Bitfield(pThis, 0x53000000U, target, target, (32 - (value & 31)) & 31, 31 - (value & 31));
		break;
	case NATIVEJIT_ALU_SHR:
		if (value & 31)
			NativeJitA64Bitfield(pThis, 0x53000000U, target, target, value & 31, 31);
		break;
	case NATIVEJIT_ALU_SAR:
		if (value & 31)
			NativeJitA64Bitfield(pThis, 0x13000000U, target, target, value & 31, 31);
		break;
	case NATIVEJIT_ALU_ROL:
		if (value & 31)
			NativeJitA64Op(pThis, 0x13800000U | ((uint32)target << 16) | (((32 - (value & 31)) & 31) << 10) | ((uint32)target << 5) | (uint32)target);
		break;
	}
}

void NativeJitNot(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitA64Reg3(pThis, 0x2a200000U, target, NATIVEJITA64_ZERO, target);
}

void NativeJitNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitA64Reg3(pThis, 0x4b000000U, target, NATIVEJITA64_ZERO, target);
}

void NativeJitSignExtend(NATIVEJITEMITTER* pThis, int target, int bits)
{
	NativeJitA64Bitfield(pThis, 0x13000000U, target, target, 0, (uint32)bits - 1);
}

void NativeJitAddCarrying(NATIVEJITEMITTER* pThis, int target, int source, int carryin)
{
	NativeJitA64Reg3(pThis, 0x8b000000U, NATIVEJITA64_SCRATCH, target, source);
	if (carryin == NATIVEJIT_CARRY_ONE)
		NativeJitA64Op(pThis, 0x91000400U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SCRATCH);
	else if (carryin == NATIVEJIT_CARRY_XER)
	{
		NativeJitA64Field(pThis, 0x39400000U, 1, NATIVEJITA64_TEMP, pThis->mCarryOffset);
		NativeJitA64Reg3(pThis, 0x8b000000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, NATIVEJITA64_TEMP);
	}
	NativeJitA64Move(pThis, target, NATIVEJITA64_SCRATCH);
	NativeJitA64Bitfield(pThis, 0xd3400000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, 32, 63);
	NativeJitA64Field(pThis, 0x39000000U, 1, NATIVEJITA64_SCRATCH, pThis->mCarryOffset);
}

void NativeJitShiftRightAlgebraicCarrying(NATIVEJITEMITTER* pThis, int target, int amount)
{
	NativeJitA64Move(pThis, NATIVEJITA64_TEMP, target);
	if (amount)
	{
		NativeJitA64Logic(pThis, NATIVEJITA64_ANDS, NATIVEJITA64_ZERO, target, (1U << amount) - 1);
		NativeJitA64Set(pThis, NATIVEJITA64_TEMP2, NATIVEJITA64_NE);
		NativeJitA64Bitfield(pThis, 0x13000000U, target, target, (uint32)amount, 31);
	}
	else
		NativeJitA64Constant(pThis, NATIVEJITA64_TEMP2, 0);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, 31, 31);
	NativeJitA64Reg3(pThis, 0x0a000000U, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP2);
	NativeJitA64Field(pThis, 0x39000000U, 1, NATIVEJITA64_TEMP, pThis->mCarryOffset);
}

void NativeJitShiftRightAlgebraicCarryingRegister(NATIVEJITEMITTER* pThis, int target, int amount)
{
	NativeJitA64Bitfield(pThis, 0x93400000U, NATIVEJITA64_SCRATCH, target, 0, 31);
	NativeJitA64Op(pThis, 0xd2800020U | NATIVEJITA64_TEMP);
	NativeJitA64Reg3(pThis, 0x9ac02000U, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, amount);
	NativeJitA64Op(pThis, 0xd1000400U | (NATIVEJITA64_TEMP << 5) | NATIVEJITA64_TEMP);
	NativeJitA64Reg3(pThis, 0x8a000000U, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, target);
	NativeJitA64Op(pThis, 0xf100001fU | (NATIVEJITA64_TEMP << 5));
	NativeJitA64Set(pThis, NATIVEJITA64_TEMP, NATIVEJITA64_NE);
	NativeJitA64Reg3(pThis, 0x9ac02800U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, amount);
	NativeJitA64Move(pThis, target, NATIVEJITA64_SCRATCH);
	NativeJitA64Bitfield(pThis, 0xd3400000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, 63, 63);
	NativeJitA64Reg3(pThis, 0x0a000000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, NATIVEJITA64_TEMP);
	NativeJitA64Field(pThis, 0x39000000U, 1, NATIVEJITA64_SCRATCH, pThis->mCarryOffset);
}

void NativeJitCountLeadingZeros(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitA64Op(pThis, 0x5ac01000U | ((uint32)target << 5) | (uint32)target);
}

void NativeJitDivide(NATIVEJITEMITTER* pThis, int target, int source, int issigned)
{
	uint8* zero;
	uint8* minimum;
	uint8* overflow;
	uint8* done;
	zero = NativeJitA64Forward(pThis, NATIVEJITA64_CBZ | (uint32)source);
	overflow = NULL;
	if (issigned)
	{
		NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, 0x80000000U);
		NativeJitA64Reg3(pThis, 0x6b000000U, NATIVEJITA64_ZERO, target, NATIVEJITA64_SCRATCH);
		minimum = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_NE);
		NativeJitA64Op(pThis, 0x3100041fU | ((uint32)source << 5));
		overflow = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_EQ);
		NativeJitA64Land(pThis, minimum);
		NativeJitA64Reg3(pThis, 0x1ac00c00U, target, target, source);
	}
	else
		NativeJitA64Reg3(pThis, 0x1ac00800U, target, target, source);
	done = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, zero);
	if (overflow)
		NativeJitA64Land(pThis, overflow);
	if (issigned)
		NativeJitA64Bitfield(pThis, 0x13000000U, target, target, 31, 31);
	else
		NativeJitA64Constant(pThis, target, 0);
	NativeJitA64Land(pThis, done);
}

static void NativeJitA64CompareResult(NATIVEJITEMITTER* pThis, int target, int issigned)
{
	NativeJitA64Constant(pThis, NATIVEJITA64_TEMP, 2);
	NativeJitA64Constant(pThis, NATIVEJITA64_TEMP2, 8);
	if (issigned)
		NativeJitA64Select(pThis, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP, NATIVEJITA64_LT);
	else
		NativeJitA64Select(pThis, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP, NATIVEJITA64_LO);
	NativeJitA64Constant(pThis, NATIVEJITA64_TEMP2, 4);
	if (issigned)
		NativeJitA64Select(pThis, target, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP, NATIVEJITA64_GT);
	else
		NativeJitA64Select(pThis, target, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP, NATIVEJITA64_HI);
}

void NativeJitCompare(NATIVEJITEMITTER* pThis, int target, int left, int right, int issigned)
{
	NativeJitA64Reg3(pThis, 0x6b000000U, NATIVEJITA64_ZERO, left, right);
	NativeJitA64CompareResult(pThis, target, issigned);
}

void NativeJitCompareImmediate(NATIVEJITEMITTER* pThis, int target, int left, uint32 value, int issigned)
{
	NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, value);
	NativeJitA64Reg3(pThis, 0x6b000000U, NATIVEJITA64_ZERO, left, NATIVEJITA64_SCRATCH);
	NativeJitA64CompareResult(pThis, target, issigned);
}

void NativeJitSelect(NATIVEJITEMITTER* pThis, int target, int condition, int whentrue, int whenfalse)
{
	NativeJitA64Op(pThis, 0x7100001fU | ((uint32)condition << 5));
	NativeJitA64Select(pThis, target, whentrue, whenfalse, NATIVEJITA64_NE);
}

void NativeJitLoadMemory(NATIVEJITEMITTER* pThis, int target, int address, int size, int issigned)
{
	if (size == 4)
	{
		NativeJitA64Guest(pThis, 0xb8604800U, target, address);
		NativeJitA64Op(pThis, 0x5ac00800U | ((uint32)target << 5) | (uint32)target);
	}
	else if (size == 2)
	{
		NativeJitA64Guest(pThis, 0x78604800U, target, address);
		NativeJitA64Op(pThis, 0x5ac00400U | ((uint32)target << 5) | (uint32)target);
		if (issigned)
			NativeJitA64Bitfield(pThis, 0x13000000U, target, target, 0, 15);
	}
	else if (issigned)
		NativeJitA64Guest(pThis, 0x38e04800U, target, address);
	else
		NativeJitA64Guest(pThis, 0x38604800U, target, address);
}

void NativeJitStoreMemory(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	if (size == 4)
	{
		NativeJitA64Op(pThis, 0x5ac00800U | ((uint32)source << 5) | NATIVEJITA64_SCRATCH);
		NativeJitA64Guest(pThis, 0xb8204800U, NATIVEJITA64_SCRATCH, address);
	}
	else if (size == 2)
	{
		NativeJitA64Op(pThis, 0x5ac00400U | ((uint32)source << 5) | NATIVEJITA64_SCRATCH);
		NativeJitA64Guest(pThis, 0x78204800U, NATIVEJITA64_SCRATCH, address);
	}
	else
		NativeJitA64Guest(pThis, 0x38204800U, source, address);
}

void NativeJitLoadMemoryReversed(NATIVEJITEMITTER* pThis, int target, int address, int size)
{
	if (size == 4)
		NativeJitA64Guest(pThis, 0xb8604800U, target, address);
	else
		NativeJitA64Guest(pThis, 0x78604800U, target, address);
}

void NativeJitStoreMemoryReversed(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	if (size == 4)
		NativeJitA64Guest(pThis, 0xb8204800U, source, address);
	else
		NativeJitA64Guest(pThis, 0x78204800U, source, address);
}

void NativeJitLoadMemoryDouble(NATIVEJITEMITTER* pThis, int offset, int address)
{
	NativeJitA64Guest(pThis, 0xf8604800U, NATIVEJITA64_SCRATCH, address);
	NativeJitA64Op(pThis, 0xdac00c00U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SCRATCH);
	NativeJitA64Field(pThis, 0xf9000000U, 8, NATIVEJITA64_SCRATCH, offset);
}

void NativeJitStoreMemoryDouble(NATIVEJITEMITTER* pThis, int address, int offset)
{
	NativeJitA64Field(pThis, 0xf9400000U, 8, NATIVEJITA64_SCRATCH, offset);
	NativeJitA64Op(pThis, 0xdac00c00U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SCRATCH);
	NativeJitA64Guest(pThis, 0xf8204800U, NATIVEJITA64_SCRATCH, address);
}

void NativeJitSingleToDouble(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitA64Op(pThis, 0x1e270000U | ((uint32)source << 5) | NATIVEJITA64_FSCRATCH);
	NativeJitA64Op(pThis, 0x1e22c000U | (NATIVEJITA64_FSCRATCH << 5) | NATIVEJITA64_FSCRATCH);
	NativeJitA64Field(pThis, 0xfd000000U, 8, NATIVEJITA64_FSCRATCH, offset);
}

void NativeJitDoubleToSingle(NATIVEJITEMITTER* pThis, int target, int offset)
{
	uint8* convert;
	uint8* done;
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, offset + 4);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP, 20, 30);
	NativeJitA64AddImmediate(pThis, NATIVEJITA64_SUB, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP2, 874);
	NativeJitA64Op(pThis, 0x7100581fU | (NATIVEJITA64_TEMP2 << 5));
	convert = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_LS);
	NativeJitA64Bitfield(pThis, 0x53000000U, target, NATIVEJITA64_TEMP, 29, 28);
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP2, offset);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_TEMP2, NATIVEJITA64_TEMP2, 29, 31);
	NativeJitA64Reg3(pThis, 0x2a000000U, target, target, NATIVEJITA64_TEMP2);
	NativeJitA64Logic(pThis, NATIVEJITA64_AND, target, target, 0x3fffffffU);
	NativeJitA64Logic(pThis, NATIVEJITA64_AND, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, 0xc0000000U);
	NativeJitA64Reg3(pThis, 0x2a000000U, target, target, NATIVEJITA64_TEMP);
	done = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, convert);
	NativeJitA64Field(pThis, 0xfd400000U, 8, NATIVEJITA64_FSCRATCH, offset);
	NativeJitA64Op(pThis, 0x1e624000U | (NATIVEJITA64_FSCRATCH << 5) | NATIVEJITA64_FSCRATCH);
	NativeJitA64Op(pThis, 0x1e260000U | (NATIVEJITA64_FSCRATCH << 5) | (uint32)target);
	NativeJitA64Land(pThis, done);
}

int NativeJitHasFloat(void)
{
	return 1;
}

void NativeJitFloatLoad(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitA64Field(pThis, 0xfd400000U, 8, target, offset);
}

void NativeJitFloatStore(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitA64Field(pThis, 0xfd000000U, 8, source, offset);
}

void NativeJitFloatOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	switch (operation)
	{
	case NATIVEJIT_FPU_ADD:
		NativeJitA64Reg3(pThis, 0x1e602800U, target, target, source);
		break;
	case NATIVEJIT_FPU_SUB:
		NativeJitA64Reg3(pThis, 0x1e603800U, target, target, source);
		break;
	case NATIVEJIT_FPU_MUL:
		NativeJitA64Reg3(pThis, 0x1e600800U, target, target, source);
		break;
	case NATIVEJIT_FPU_DIV:
		NativeJitA64Reg3(pThis, 0x1e601800U, target, target, source);
		break;
	}
}

void NativeJitFloatRoundSingle(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitA64Op(pThis, 0x1e624000U | ((uint32)target << 5) | (uint32)target);
	NativeJitA64Op(pThis, 0x1e22c000U | ((uint32)target << 5) | (uint32)target);
}

void NativeJitFloatNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitA64Op(pThis, 0x1e614000U | ((uint32)target << 5) | (uint32)target);
}

void NativeJitFloatClass(NATIVEJITEMITTER* pThis, int target, int offset, uint32 lowexponent)
{
	uint8* notzero;
	uint8* zerodone;
	uint8* finite;
	uint8* infinite;
	uint8* nandone;
	uint8* infinitesigned;
	uint8* normal;
	uint8* subnormalsigned;
	uint8* positive;
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, offset + 4);
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP2, offset);
	NativeJitA64Op(pThis, 0x2a000400U | (NATIVEJITA64_TEMP << 16) | (NATIVEJITA64_TEMP2 << 5) | NATIVEJITA64_SCRATCH);
	notzero = NativeJitA64Forward(pThis, NATIVEJITA64_CBNZ | NATIVEJITA64_SCRATCH);
	NativeJitA64Constant(pThis, target, 2);
	NativeJitA64Constant(pThis, NATIVEJITA64_SCRATCH, 0x12);
	NativeJitA64Op(pThis, 0x7100001fU | (NATIVEJITA64_TEMP << 5));
	NativeJitA64Select(pThis, target, NATIVEJITA64_SCRATCH, target, NATIVEJITA64_LT);
	zerodone = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, notzero);
	NativeJitA64Bitfield(pThis, 0x53000000U, target, NATIVEJITA64_TEMP, 20, 30);
	NativeJitA64AddImmediate(pThis, NATIVEJITA64_SUBS, NATIVEJITA64_ZERO, target, 0x7ff);
	finite = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_NE);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_TEMP, 0, 19);
	NativeJitA64Reg3(pThis, 0x2a000000U, NATIVEJITA64_SCRATCH, NATIVEJITA64_SCRATCH, NATIVEJITA64_TEMP2);
	infinite = NativeJitA64Forward(pThis, NATIVEJITA64_CBZ | NATIVEJITA64_SCRATCH);
	NativeJitA64Constant(pThis, target, 0x11);
	nandone = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, infinite);
	NativeJitA64Constant(pThis, target, 5);
	infinitesigned = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, finite);
	NativeJitA64AddImmediate(pThis, NATIVEJITA64_SUBS, NATIVEJITA64_ZERO, target, lowexponent);
	normal = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_HS);
	NativeJitA64Constant(pThis, target, 0x14);
	subnormalsigned = NativeJitA64Forward(pThis, NATIVEJITA64_B);
	NativeJitA64Land(pThis, normal);
	NativeJitA64Constant(pThis, target, 4);
	NativeJitA64Land(pThis, infinitesigned);
	NativeJitA64Land(pThis, subnormalsigned);
	positive = NativeJitA64Forward(pThis, NATIVEJITA64_TBZSIGN | NATIVEJITA64_TEMP);
	NativeJitA64Logic(pThis, NATIVEJITA64_EOR, target, target, 0x0c);
	NativeJitA64Land(pThis, positive);
	NativeJitA64Land(pThis, zerodone);
	NativeJitA64Land(pThis, nandone);
}

uint8* NativeJitLabel(NATIVEJITEMITTER* pThis)
{
	return pThis->mCode;
}

uint8* NativeJitBranchIfZero(NATIVEJITEMITTER* pThis, int condition)
{
	return NativeJitA64Forward(pThis, NATIVEJITA64_CBZ | (uint32)condition);
}

uint8* NativeJitFloatBranchIfNaN(NATIVEJITEMITTER* pThis, int source)
{
	NativeJitA64Op(pThis, 0x1e602000U | ((uint32)source << 16) | ((uint32)source << 5));
	return NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_VS);
}

uint8* NativeJitJump(NATIVEJITEMITTER* pThis)
{
	return NativeJitA64Forward(pThis, NATIVEJITA64_B);
}

void NativeJitBranchLand(NATIVEJITEMITTER* pThis, uint8* branch)
{
	NativeJitA64Land(pThis, branch);
}

static void NativeJitA64Address(NATIVEJITEMITTER* pThis, int reg, uint8* target)
{
	uint32 distance;
	distance = (uint32)(target - pThis->mCode);
	NativeJitA64Op(pThis, 0x10000000U | ((distance & 3) << 29) | (((distance >> 2) & 0x7ffff) << 5) | (uint32)reg);
}

static void NativeJitA64RecordSite(NATIVEJITEMITTER* pThis, uint8* site, uint32 pcvalue, NATIVEJITSTATE* state)
{
	NativeJitA64Address(pThis, NATIVEJITA64_SITE, site);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, &state->mChainSite);
	NativeJitA64Op(pThis, 0xf9000000U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SITE);
	NativeJitA64Constant(pThis, NATIVEJITA64_TEMP, pcvalue);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, &state->mChainPc);
	NativeJitA64Op(pThis, 0xb9000000U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_TEMP);
}

uint8* NativeJitChainJump(NATIVEJITEMITTER* pThis, uint32 pcvalue, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* site;
	(void)pcvalue;
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, flagsoffset);
	flagged = NativeJitA64Forward(pThis, NATIVEJITA64_CBNZ | NATIVEJITA64_TEMP);
	site = pThis->mCode;
	NativeJitA64Op(pThis, NATIVEJITA64_B | 1);
	NativeJitA64Land(pThis, flagged);
	NativeJitA64Address(pThis, NATIVEJITA64_SITE, site);
	NativeJitJumpToCode(pThis, state->mReturnStub);
	return site;
}

void NativeJitReturnStub(NATIVEJITEMITTER* pThis, int pcoffset, NATIVEJITSTATE* state)
{
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, &state->mChainSite);
	NativeJitA64Op(pThis, 0xf9000000U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SITE);
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, pcoffset);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, &state->mChainPc);
	NativeJitA64Op(pThis, 0xb9000000U | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_TEMP);
	NativeJitEpilogue(pThis);
}

void NativeJitJumpToCode(NATIVEJITEMITTER* pThis, uint8* target)
{
	NativeJitA64Op(pThis, NATIVEJITA64_B | ((uint32)((target - pThis->mCode) / 4) & 0x03ffffff));
}

void NativeJitIndirectExit(NATIVEJITEMITTER* pThis, int pcoffset, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* missed;
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, flagsoffset);
	flagged = NativeJitA64Forward(pThis, NATIVEJITA64_CBNZ | NATIVEJITA64_TEMP);
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_SITE, pcoffset);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_TEMP, NATIVEJITA64_SITE, 2, 31);
	NativeJitA64Bitfield(pThis, 0x53000000U, NATIVEJITA64_TEMP2, NATIVEJITA64_SITE, NATIVEJIT_LOOKUP_SHIFT, 31);
	NativeJitA64Reg3(pThis, 0x4a000000U, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP2);
	NativeJitA64Logic(pThis, NATIVEJITA64_AND, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP, NATIVEJIT_LOOKUP_MASK);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, state->mLookupPc);
	NativeJitA64Op(pThis, 0xb8607800U | (NATIVEJITA64_TEMP << 16) | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_TEMP2);
	NativeJitA64Reg3(pThis, 0x6b000000U, NATIVEJITA64_ZERO, NATIVEJITA64_TEMP2, NATIVEJITA64_SITE);
	missed = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_NE);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, state->mLookupEntry);
	NativeJitA64Op(pThis, 0xf8607800U | (NATIVEJITA64_TEMP << 16) | (NATIVEJITA64_SCRATCH << 5) | NATIVEJITA64_SCRATCH);
	NativeJitA64Op(pThis, 0xd61f0000U | (NATIVEJITA64_SCRATCH << 5));
	NativeJitA64Land(pThis, flagged);
	NativeJitA64Land(pThis, missed);
}

uint8* NativeJitChainExit(NATIVEJITEMITTER* pThis, int pcoffset, uint32 pcvalue, int flagsoffset,
	NATIVEJITSTATE* state)
{
	uint8* differentpc;
	uint8* flagged;
	uint8* site;
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, pcoffset);
	NativeJitA64Constant(pThis, NATIVEJITA64_TEMP2, pcvalue);
	NativeJitA64Reg3(pThis, 0x6b000000U, NATIVEJITA64_ZERO, NATIVEJITA64_TEMP, NATIVEJITA64_TEMP2);
	differentpc = NativeJitA64Forward(pThis, NATIVEJITA64_BCOND | NATIVEJITA64_NE);
	NativeJitA64Field(pThis, 0xb9400000U, 4, NATIVEJITA64_TEMP, flagsoffset);
	flagged = NativeJitA64Forward(pThis, NATIVEJITA64_CBNZ | NATIVEJITA64_TEMP);
	site = pThis->mCode;
	NativeJitA64Op(pThis, NATIVEJITA64_B | 1);
	NativeJitA64RecordSite(pThis, site, pcvalue, state);
	NativeJitA64Land(pThis, differentpc);
	NativeJitA64Land(pThis, flagged);
	return site;
}

void NativeJitJumpTo(uint8* site, uint8* target)
{
	NativeJitChainLink(site, target);
}

void NativeJitChainLink(uint8* site, uint8* target)
{
	uint32 word;
	word = NATIVEJITA64_B | ((uint32)((target - site) / 4) & 0x03ffffff);
	site[0] = (uint8)word;
	site[1] = (uint8)(word >> 8);
	site[2] = (uint8)(word >> 16);
	site[3] = (uint8)(word >> 24);
}

void NativeJitCallHelper(NATIVEJITEMITTER* pThis, NATIVEJITHELPER helper, const void* argument)
{
	NativeJitA64Move64(pThis, 0, NATIVEJITA64_CPU);
	NativeJitA64Far(pThis, 1, argument);
	NativeJitA64Far(pThis, NATIVEJITA64_SCRATCH, (const void*)helper);
	NativeJitA64Op(pThis, 0xd63f0000U | (NATIVEJITA64_SCRATCH << 5));
}

#endif
