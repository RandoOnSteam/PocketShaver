#include "sysdeps.h"
#include "vm_alloc.h"
#include "cpu/jit/nativejit.h"

#if defined(NATIVEJIT_HOST_X86_32)

#define NATIVEJITX86_EAX 0
#define NATIVEJITX86_ECX 1
#define NATIVEJITX86_EDX 2
#define NATIVEJITX86_REGS 3
#define NATIVEJITX86_ESP 4
#define NATIVEJITX86_S1 5
#define NATIVEJITX86_MEMORY 6
#define NATIVEJITX86_S0 7

#define NATIVEJITX86_LOCALS 12
#define NATIVEJITX86_CPUSLOT 32
#define NATIVEJITX86_REGSSLOT 36
#define NATIVEJITX86_MEMORYSLOT 40
#define NATIVEJITX86_TEMPSLOT 8

static void NativeJitX86Byte(NATIVEJITEMITTER* pThis, uint32 value)
{
	*pThis->mCode++ = (uint8)value;
}

static void NativeJitX86Dword(NATIVEJITEMITTER* pThis, uint32 value)
{
	pThis->mCode[0] = (uint8)value;
	pThis->mCode[1] = (uint8)(value >> 8);
	pThis->mCode[2] = (uint8)(value >> 16);
	pThis->mCode[3] = (uint8)(value >> 24);
	pThis->mCode += 4;
}

static void NativeJitX86Opcode(NATIVEJITEMITTER* pThis, uint32 opcode)
{
	if (opcode > 0xff)
		NativeJitX86Byte(pThis, opcode >> 8);
	NativeJitX86Byte(pThis, opcode);
}

static void NativeJitX86RegReg(NATIVEJITEMITTER* pThis, uint32 opcode, int reg, int rm)
{
	NativeJitX86Opcode(pThis, opcode);
	NativeJitX86Byte(pThis, 0xc0 | (reg << 3) | rm);
}

static void NativeJitX86RegsOperand(NATIVEJITEMITTER* pThis, uint32 opcode, int reg, int offset)
{
	NativeJitX86Opcode(pThis, opcode);
	NativeJitX86Byte(pThis, 0x80 | (reg << 3) | NATIVEJITX86_REGS);
	NativeJitX86Dword(pThis, (uint32)offset);
}

static void NativeJitX86StackOperand(NATIVEJITEMITTER* pThis, uint32 opcode, int reg, int offset)
{
	NativeJitX86Opcode(pThis, opcode);
	NativeJitX86Byte(pThis, 0x44 | (reg << 3));
	NativeJitX86Byte(pThis, 0x24);
	NativeJitX86Byte(pThis, offset);
}

static void NativeJitX86MemoryOperand(NATIVEJITEMITTER* pThis, int prefix16, uint32 opcode, int reg)
{
	if (prefix16)
		NativeJitX86Byte(pThis, 0x66);
	NativeJitX86Opcode(pThis, opcode);
	NativeJitX86Byte(pThis, 0x04 | (reg << 3));
	NativeJitX86Byte(pThis, (NATIVEJITX86_S0 << 3) | NATIVEJITX86_MEMORY);
}

static void NativeJitX86MoveImmediate(NATIVEJITEMITTER* pThis, int reg, uint32 value)
{
	NativeJitX86Byte(pThis, 0xb8 | reg);
	NativeJitX86Dword(pThis, value);
}

static void NativeJitX86Move(NATIVEJITEMITTER* pThis, int target, int source)
{
	if (target != source)
		NativeJitX86RegReg(pThis, 0x89, source, target);
}

static void NativeJitX86ShiftImmediate(NATIVEJITEMITTER* pThis, int extension, int reg, uint32 amount)
{
	NativeJitX86Byte(pThis, 0xc1);
	NativeJitX86Byte(pThis, 0xc0 | (extension << 3) | reg);
	NativeJitX86Byte(pThis, amount);
}

static void NativeJitX86ShiftCl(NATIVEJITEMITTER* pThis, int extension, int reg)
{
	NativeJitX86Byte(pThis, 0xd3);
	NativeJitX86Byte(pThis, 0xc0 | (extension << 3) | reg);
}

static void NativeJitX86OperateImmediate(NATIVEJITEMITTER* pThis, int extension, int reg, uint32 value)
{
	NativeJitX86RegReg(pThis, 0x81, extension, reg);
	NativeJitX86Dword(pThis, value);
}

static uint8* NativeJitX86JumpShort(NATIVEJITEMITTER* pThis, uint32 opcode)
{
	NativeJitX86Byte(pThis, opcode);
	NativeJitX86Byte(pThis, 0);
	return pThis->mCode - 1;
}

static void NativeJitX86Land(NATIVEJITEMITTER* pThis, uint8* displacement)
{
	*displacement = (uint8)(pThis->mCode - (displacement + 1));
}

static void NativeJitX86TestCount32(NATIVEJITEMITTER* pThis)
{
	NativeJitX86Byte(pThis, 0xf6);
	NativeJitX86Byte(pThis, 0xc1);
	NativeJitX86Byte(pThis, 32);
}

static void NativeJitX86StoreCarry(NATIVEJITEMITTER* pThis, int source)
{
	NativeJitX86RegsOperand(pThis, 0x88, source, pThis->mCarryOffset);
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
	(void)start;
	(void)length;
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
	NativeJitX86Byte(pThis, 0x55);
	NativeJitX86Byte(pThis, 0x53);
	NativeJitX86Byte(pThis, 0x56);
	NativeJitX86Byte(pThis, 0x57);
	NativeJitX86Byte(pThis, 0x83);
	NativeJitX86Byte(pThis, 0xec);
	NativeJitX86Byte(pThis, NATIVEJITX86_LOCALS);
	NativeJitX86StackOperand(pThis, 0x8b, NATIVEJITX86_REGS, NATIVEJITX86_REGSSLOT);
	NativeJitX86StackOperand(pThis, 0x8b, NATIVEJITX86_MEMORY, NATIVEJITX86_MEMORYSLOT);
}

void NativeJitEpilogue(NATIVEJITEMITTER* pThis)
{
	NativeJitX86Byte(pThis, 0x83);
	NativeJitX86Byte(pThis, 0xc4);
	NativeJitX86Byte(pThis, NATIVEJITX86_LOCALS);
	NativeJitX86Byte(pThis, 0x5f);
	NativeJitX86Byte(pThis, 0x5e);
	NativeJitX86Byte(pThis, 0x5b);
	NativeJitX86Byte(pThis, 0x5d);
	NativeJitX86Byte(pThis, 0xc3);
}

void NativeJitLoadImmediate(NATIVEJITEMITTER* pThis, int target, uint32 value)
{
	NativeJitX86MoveImmediate(pThis, target, value);
}

void NativeJitMove(NATIVEJITEMITTER* pThis, int target, int source)
{
	NativeJitX86Move(pThis, target, source);
}

void NativeJitLoadRegister(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX86RegsOperand(pThis, 0x8b, target, offset);
}

void NativeJitStoreRegister(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX86RegsOperand(pThis, 0x89, source, offset);
}

void NativeJitStoreRegisterImmediate(NATIVEJITEMITTER* pThis, int offset, uint32 value)
{
	NativeJitX86RegsOperand(pThis, 0xc7, 0, offset);
	NativeJitX86Dword(pThis, value);
}

void NativeJitLoadRegisterByte(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX86RegsOperand(pThis, 0x0fb6, target, offset);
}

void NativeJitStoreRegisterByte(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX86RegsOperand(pThis, 0x88, source, offset);
}

void NativeJitOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	uint8* small;
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitX86RegReg(pThis, 0x01, source, target);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitX86RegReg(pThis, 0x29, source, target);
		break;
	case NATIVEJIT_ALU_AND:
		NativeJitX86RegReg(pThis, 0x21, source, target);
		break;
	case NATIVEJIT_ALU_OR:
		NativeJitX86RegReg(pThis, 0x09, source, target);
		break;
	case NATIVEJIT_ALU_XOR:
		NativeJitX86RegReg(pThis, 0x31, source, target);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitX86RegReg(pThis, 0x0faf, target, source);
		break;
	case NATIVEJIT_ALU_MULHS:
	case NATIVEJIT_ALU_MULHU:
		NativeJitX86Move(pThis, NATIVEJITX86_S1, source);
		NativeJitX86Move(pThis, NATIVEJITX86_EAX, target);
		if (operation == NATIVEJIT_ALU_MULHS)
			NativeJitX86RegReg(pThis, 0xf7, 5, NATIVEJITX86_S1);
		else
			NativeJitX86RegReg(pThis, 0xf7, 4, NATIVEJITX86_S1);
		NativeJitX86Move(pThis, target, NATIVEJITX86_EDX);
		break;
	case NATIVEJIT_ALU_SHL:
	case NATIVEJIT_ALU_SHR:
		NativeJitX86Move(pThis, NATIVEJITX86_S0, target);
		if (operation == NATIVEJIT_ALU_SHL)
			NativeJitX86ShiftCl(pThis, 4, NATIVEJITX86_S0);
		else
			NativeJitX86ShiftCl(pThis, 5, NATIVEJITX86_S0);
		NativeJitX86TestCount32(pThis);
		small = NativeJitX86JumpShort(pThis, 0x74);
		NativeJitX86RegReg(pThis, 0x31, NATIVEJITX86_S0, NATIVEJITX86_S0);
		NativeJitX86Land(pThis, small);
		NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
		break;
	case NATIVEJIT_ALU_SAR:
		NativeJitX86Move(pThis, NATIVEJITX86_S0, target);
		NativeJitX86ShiftCl(pThis, 7, NATIVEJITX86_S0);
		NativeJitX86TestCount32(pThis);
		small = NativeJitX86JumpShort(pThis, 0x74);
		NativeJitX86ShiftImmediate(pThis, 7, NATIVEJITX86_S0, 31);
		NativeJitX86Land(pThis, small);
		NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
		break;
	case NATIVEJIT_ALU_ROL:
		NativeJitX86ShiftCl(pThis, 0, target);
		break;
	}
}

void NativeJitOperateImmediate(NATIVEJITEMITTER* pThis, int operation, int target, uint32 value)
{
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitX86OperateImmediate(pThis, 0, target, value);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitX86OperateImmediate(pThis, 5, target, value);
		break;
	case NATIVEJIT_ALU_AND:
		NativeJitX86OperateImmediate(pThis, 4, target, value);
		break;
	case NATIVEJIT_ALU_OR:
		NativeJitX86OperateImmediate(pThis, 1, target, value);
		break;
	case NATIVEJIT_ALU_XOR:
		NativeJitX86OperateImmediate(pThis, 6, target, value);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitX86RegReg(pThis, 0x69, target, target);
		NativeJitX86Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_SHL:
		if (value & 31)
			NativeJitX86ShiftImmediate(pThis, 4, target, value & 31);
		break;
	case NATIVEJIT_ALU_SHR:
		if (value & 31)
			NativeJitX86ShiftImmediate(pThis, 5, target, value & 31);
		break;
	case NATIVEJIT_ALU_SAR:
		if (value & 31)
			NativeJitX86ShiftImmediate(pThis, 7, target, value & 31);
		break;
	case NATIVEJIT_ALU_ROL:
		if (value & 31)
			NativeJitX86ShiftImmediate(pThis, 0, target, value & 31);
		break;
	}
}

void NativeJitNot(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX86RegReg(pThis, 0xf7, 2, target);
}

void NativeJitNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX86RegReg(pThis, 0xf7, 3, target);
}

void NativeJitSignExtend(NATIVEJITEMITTER* pThis, int target, int bits)
{
	if (bits == 8)
		NativeJitX86RegReg(pThis, 0x0fbe, target, target);
	else
		NativeJitX86RegReg(pThis, 0x0fbf, target, target);
}

void NativeJitAddCarrying(NATIVEJITEMITTER* pThis, int target, int source, int carryin)
{
	if (carryin == NATIVEJIT_CARRY_XER)
	{
		NativeJitX86RegsOperand(pThis, 0x0fb6, NATIVEJITX86_S1, pThis->mCarryOffset);
		NativeJitX86OperateImmediate(pThis, 0, NATIVEJITX86_S1, 0xffffffffU);
	}
	else if (carryin == NATIVEJIT_CARRY_ONE)
		NativeJitX86Byte(pThis, 0xf9);
	else
		NativeJitX86Byte(pThis, 0xf8);
	NativeJitX86RegReg(pThis, 0x11, source, target);
	NativeJitX86RegsOperand(pThis, 0x0f92, 0, pThis->mCarryOffset);
}

void NativeJitShiftRightAlgebraicCarrying(NATIVEJITEMITTER* pThis, int target, int amount)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, target);
	NativeJitX86Move(pThis, NATIVEJITX86_S1, target);
	if (amount)
	{
		NativeJitX86ShiftImmediate(pThis, 7, target, amount);
		NativeJitX86OperateImmediate(pThis, 4, NATIVEJITX86_S1, (1U << amount) - 1);
	}
	else
		NativeJitX86RegReg(pThis, 0x31, NATIVEJITX86_S1, NATIVEJITX86_S1);
	NativeJitX86RegReg(pThis, 0xf7, 3, NATIVEJITX86_S1);
	NativeJitX86RegReg(pThis, 0x19, NATIVEJITX86_S1, NATIVEJITX86_S1);
	NativeJitX86ShiftImmediate(pThis, 5, NATIVEJITX86_S0, 31);
	NativeJitX86RegReg(pThis, 0x21, NATIVEJITX86_S1, NATIVEJITX86_S0);
	NativeJitX86Move(pThis, NATIVEJITX86_EDX, NATIVEJITX86_S0);
	NativeJitX86StoreCarry(pThis, NATIVEJITX86_EDX);
}

void NativeJitShiftRightAlgebraicCarryingRegister(NATIVEJITEMITTER* pThis, int target, int amount)
{
	uint8* large;
	uint8* done;
	(void)amount;
	NativeJitX86Move(pThis, NATIVEJITX86_S0, target);
	NativeJitX86Move(pThis, NATIVEJITX86_S1, target);
	NativeJitX86TestCount32(pThis);
	large = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86ShiftCl(pThis, 7, NATIVEJITX86_S0);
	NativeJitX86MoveImmediate(pThis, target, 1);
	NativeJitX86ShiftCl(pThis, 4, target);
	NativeJitX86Byte(pThis, 0x48 | target);
	NativeJitX86RegReg(pThis, 0x21, target, NATIVEJITX86_S1);
	NativeJitX86RegReg(pThis, 0xf7, 3, NATIVEJITX86_S1);
	NativeJitX86RegReg(pThis, 0x19, NATIVEJITX86_S1, NATIVEJITX86_S1);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
	NativeJitX86ShiftImmediate(pThis, 7, target, 31);
	NativeJitX86RegReg(pThis, 0x21, NATIVEJITX86_S1, target);
	done = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, large);
	NativeJitX86ShiftImmediate(pThis, 7, NATIVEJITX86_S0, 31);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
	NativeJitX86Land(pThis, done);
	NativeJitX86OperateImmediate(pThis, 4, target, 1);
	NativeJitX86StoreCarry(pThis, target);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
}

void NativeJitCountLeadingZeros(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S0, 63);
	NativeJitX86RegReg(pThis, 0x0fbd, NATIVEJITX86_S1, target);
	NativeJitX86RegReg(pThis, 0x0f44, NATIVEJITX86_S1, NATIVEJITX86_S0);
	NativeJitX86RegReg(pThis, 0x83, 6, NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 31);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S1);
}

void NativeJitDivide(NATIVEJITEMITTER* pThis, int target, int source, int issigned)
{
	uint8* zero;
	uint8* minimum;
	uint8* overflow;
	uint8* done;
	NativeJitX86Move(pThis, NATIVEJITX86_S0, target);
	NativeJitX86Move(pThis, NATIVEJITX86_S1, source);
	NativeJitX86RegReg(pThis, 0x85, NATIVEJITX86_S1, NATIVEJITX86_S1);
	zero = NativeJitX86JumpShort(pThis, 0x74);
	overflow = NULL;
	if (issigned)
	{
		NativeJitX86OperateImmediate(pThis, 7, NATIVEJITX86_S0, 0x80000000U);
		minimum = NativeJitX86JumpShort(pThis, 0x75);
		NativeJitX86RegReg(pThis, 0x83, 7, NATIVEJITX86_S1);
		NativeJitX86Byte(pThis, 0xff);
		overflow = NativeJitX86JumpShort(pThis, 0x74);
		NativeJitX86Land(pThis, minimum);
		NativeJitX86Move(pThis, NATIVEJITX86_EAX, NATIVEJITX86_S0);
		NativeJitX86Byte(pThis, 0x99);
		NativeJitX86RegReg(pThis, 0xf7, 7, NATIVEJITX86_S1);
	}
	else
	{
		NativeJitX86Move(pThis, NATIVEJITX86_EAX, NATIVEJITX86_S0);
		NativeJitX86RegReg(pThis, 0x31, NATIVEJITX86_EDX, NATIVEJITX86_EDX);
		NativeJitX86RegReg(pThis, 0xf7, 6, NATIVEJITX86_S1);
	}
	NativeJitX86Move(pThis, target, NATIVEJITX86_EAX);
	done = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, zero);
	if (overflow)
		NativeJitX86Land(pThis, overflow);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
	if (issigned)
		NativeJitX86ShiftImmediate(pThis, 7, target, 31);
	else
		NativeJitX86MoveImmediate(pThis, target, 0);
	NativeJitX86Land(pThis, done);
}

static void NativeJitX86CompareResult(NATIVEJITEMITTER* pThis, int target, int issigned)
{
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S1, 8);
	if (issigned)
		NativeJitX86RegReg(pThis, 0x0f4c, NATIVEJITX86_S0, NATIVEJITX86_S1);
	else
		NativeJitX86RegReg(pThis, 0x0f42, NATIVEJITX86_S0, NATIVEJITX86_S1);
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S1, 4);
	if (issigned)
		NativeJitX86RegReg(pThis, 0x0f4f, NATIVEJITX86_S0, NATIVEJITX86_S1);
	else
		NativeJitX86RegReg(pThis, 0x0f47, NATIVEJITX86_S0, NATIVEJITX86_S1);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
}

void NativeJitCompare(NATIVEJITEMITTER* pThis, int target, int left, int right, int issigned)
{
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S0, 2);
	NativeJitX86RegReg(pThis, 0x39, right, left);
	NativeJitX86CompareResult(pThis, target, issigned);
}

void NativeJitCompareImmediate(NATIVEJITEMITTER* pThis, int target, int left, uint32 value, int issigned)
{
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S0, 2);
	NativeJitX86OperateImmediate(pThis, 7, left, value);
	NativeJitX86CompareResult(pThis, target, issigned);
}

void NativeJitSelect(NATIVEJITEMITTER* pThis, int target, int condition, int whentrue, int whenfalse)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, whenfalse);
	NativeJitX86RegReg(pThis, 0x85, condition, condition);
	NativeJitX86RegReg(pThis, 0x0f45, NATIVEJITX86_S0, whentrue);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
}

void NativeJitLoadMemory(NATIVEJITEMITTER* pThis, int target, int address, int size, int issigned)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	if (size == 4)
	{
		NativeJitX86MemoryOperand(pThis, 0, 0x8b, target);
		NativeJitX86Byte(pThis, 0x0f);
		NativeJitX86Byte(pThis, 0xc8 | target);
	}
	else if (size == 2)
	{
		NativeJitX86MemoryOperand(pThis, 0, 0x0fb7, target);
		NativeJitX86Byte(pThis, 0x66);
		NativeJitX86ShiftImmediate(pThis, 0, target, 8);
		if (issigned)
			NativeJitX86RegReg(pThis, 0x0fbf, target, target);
	}
	else if (issigned)
		NativeJitX86MemoryOperand(pThis, 0, 0x0fbe, target);
	else
		NativeJitX86MemoryOperand(pThis, 0, 0x0fb6, target);
}

void NativeJitStoreMemory(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	if (size == 1)
	{
		NativeJitX86MemoryOperand(pThis, 0, 0x88, source);
		return;
	}
	NativeJitX86Move(pThis, NATIVEJITX86_S1, source);
	if (size == 4)
	{
		NativeJitX86Byte(pThis, 0x0f);
		NativeJitX86Byte(pThis, 0xc8 | NATIVEJITX86_S1);
		NativeJitX86MemoryOperand(pThis, 0, 0x89, NATIVEJITX86_S1);
	}
	else
	{
		NativeJitX86Byte(pThis, 0x66);
		NativeJitX86ShiftImmediate(pThis, 0, NATIVEJITX86_S1, 8);
		NativeJitX86MemoryOperand(pThis, 1, 0x89, NATIVEJITX86_S1);
	}
}

void NativeJitLoadMemoryReversed(NATIVEJITEMITTER* pThis, int target, int address, int size)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	if (size == 4)
		NativeJitX86MemoryOperand(pThis, 0, 0x8b, target);
	else
		NativeJitX86MemoryOperand(pThis, 0, 0x0fb7, target);
}

void NativeJitStoreMemoryReversed(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	if (size == 4)
		NativeJitX86MemoryOperand(pThis, 0, 0x89, source);
	else
		NativeJitX86MemoryOperand(pThis, 1, 0x89, source);
}

void NativeJitLoadMemoryDouble(NATIVEJITEMITTER* pThis, int offset, int address)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	NativeJitX86MemoryOperand(pThis, 0, 0x8b, NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0xc8 | NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 0x66);
	NativeJitX86RegReg(pThis, 0x0f6e, 1, NATIVEJITX86_S1);
	NativeJitX86OperateImmediate(pThis, 0, NATIVEJITX86_S0, 4);
	NativeJitX86MemoryOperand(pThis, 0, 0x8b, NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0xc8 | NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 0x66);
	NativeJitX86RegReg(pThis, 0x0f6e, 0, NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 0x66);
	NativeJitX86RegReg(pThis, 0x0f62, 0, 1);
	NativeJitFloatStore(pThis, offset, 0);
}

void NativeJitStoreMemoryDouble(NATIVEJITEMITTER* pThis, int address, int offset)
{
	NativeJitX86Move(pThis, NATIVEJITX86_S0, address);
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S1, offset + 4);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0xc8 | NATIVEJITX86_S1);
	NativeJitX86MemoryOperand(pThis, 0, 0x89, NATIVEJITX86_S1);
	NativeJitX86OperateImmediate(pThis, 0, NATIVEJITX86_S0, 4);
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S1, offset);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0xc8 | NATIVEJITX86_S1);
	NativeJitX86MemoryOperand(pThis, 0, 0x89, NATIVEJITX86_S1);
}

void NativeJitSingleToDouble(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX86StackOperand(pThis, 0x89, source, NATIVEJITX86_TEMPSLOT);
	NativeJitX86StackOperand(pThis, 0xd9, 0, NATIVEJITX86_TEMPSLOT);
	NativeJitX86RegsOperand(pThis, 0xdd, 3, offset);
}

void NativeJitDoubleToSingle(NATIVEJITEMITTER* pThis, int target, int offset)
{
	uint8* convert;
	uint8* done;
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S0, offset + 4);
	NativeJitX86Move(pThis, NATIVEJITX86_S1, NATIVEJITX86_S0);
	NativeJitX86ShiftImmediate(pThis, 5, NATIVEJITX86_S1, 20);
	NativeJitX86OperateImmediate(pThis, 4, NATIVEJITX86_S1, 0x7ff);
	NativeJitX86OperateImmediate(pThis, 5, NATIVEJITX86_S1, 874);
	NativeJitX86RegReg(pThis, 0x83, 7, NATIVEJITX86_S1);
	NativeJitX86Byte(pThis, 22);
	convert = NativeJitX86JumpShort(pThis, 0x76);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S0);
	NativeJitX86ShiftImmediate(pThis, 4, target, 3);
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S1, offset);
	NativeJitX86ShiftImmediate(pThis, 5, NATIVEJITX86_S1, 29);
	NativeJitX86RegReg(pThis, 0x09, NATIVEJITX86_S1, target);
	NativeJitX86OperateImmediate(pThis, 4, target, 0x3fffffff);
	NativeJitX86OperateImmediate(pThis, 4, NATIVEJITX86_S0, 0xc0000000U);
	NativeJitX86RegReg(pThis, 0x09, NATIVEJITX86_S0, target);
	done = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, convert);
	NativeJitX86RegsOperand(pThis, 0xdd, 0, offset);
	NativeJitX86StackOperand(pThis, 0xd9, 3, NATIVEJITX86_TEMPSLOT);
	NativeJitX86StackOperand(pThis, 0x8b, target, NATIVEJITX86_TEMPSLOT);
	NativeJitX86Land(pThis, done);
}

typedef uint32 (*NATIVEJITX86FEATURES)(void);

static void NativeJitX86SseRegs(NATIVEJITEMITTER* pThis, uint32 prefix, uint32 opcode, int reg, int offset)
{
	NativeJitX86Byte(pThis, prefix);
	NativeJitX86RegsOperand(pThis, 0x0f00 | opcode, reg, offset);
}

static void NativeJitX86SseRegReg(NATIVEJITEMITTER* pThis, uint32 prefix, uint32 opcode, int reg, int rm)
{
	NativeJitX86Byte(pThis, prefix);
	NativeJitX86RegReg(pThis, 0x0f00 | opcode, reg, rm);
}

int NativeJitHasFloat(void)
{
	uint8* code;
	uint32 features;
	code = NativeJitAllocate(4096);
	if (code == NULL)
		return 0;
	code[0] = 0x53;
	code[1] = 0xb8;
	code[2] = 1;
	code[3] = 0;
	code[4] = 0;
	code[5] = 0;
	code[6] = 0x0f;
	code[7] = 0xa2;
	code[8] = 0x89;
	code[9] = 0xd0;
	code[10] = 0x5b;
	code[11] = 0xc3;
	features = ((NATIVEJITX86FEATURES)code)();
	NativeJitFree(code, 4096);
	return (features >> 26) & 1;
}

void NativeJitFloatLoad(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX86SseRegs(pThis, 0xf2, 0x10, target, offset);
}

void NativeJitFloatStore(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX86SseRegs(pThis, 0xf2, 0x11, source, offset);
}

void NativeJitFloatOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	switch (operation)
	{
	case NATIVEJIT_FPU_ADD:
		NativeJitX86SseRegReg(pThis, 0xf2, 0x58, target, source);
		break;
	case NATIVEJIT_FPU_SUB:
		NativeJitX86SseRegReg(pThis, 0xf2, 0x5c, target, source);
		break;
	case NATIVEJIT_FPU_MUL:
		NativeJitX86SseRegReg(pThis, 0xf2, 0x59, target, source);
		break;
	case NATIVEJIT_FPU_DIV:
		NativeJitX86SseRegReg(pThis, 0xf2, 0x5e, target, source);
		break;
	}
}

void NativeJitFloatRoundSingle(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX86SseRegReg(pThis, 0xf2, 0x5a, target, target);
	NativeJitX86SseRegReg(pThis, 0xf3, 0x5a, target, target);
}

void NativeJitFloatNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_S0, 0x80000000U);
	NativeJitX86SseRegReg(pThis, 0x66, 0x6e, 3, NATIVEJITX86_S0);
	NativeJitX86Byte(pThis, 0x66);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0x73);
	NativeJitX86Byte(pThis, 0xf3);
	NativeJitX86Byte(pThis, 32);
	NativeJitX86SseRegReg(pThis, 0x66, 0x57, target, 3);
}

void NativeJitFloatClass(NATIVEJITEMITTER* pThis, int target, int offset, uint32 lowexponent)
{
	uint8* notzero;
	uint8* positivezero;
	uint8* zerodone;
	uint8* finite;
	uint8* infinite;
	uint8* nandone;
	uint8* infinitesigned;
	uint8* normal;
	uint8* subnormalsigned;
	uint8* positive;
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S1, offset + 4);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S1);
	NativeJitX86ShiftImmediate(pThis, 4, target, 1);
	NativeJitX86RegsOperand(pThis, 0x0b, target, offset);
	notzero = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86MoveImmediate(pThis, target, 2);
	NativeJitX86RegReg(pThis, 0x85, NATIVEJITX86_S1, NATIVEJITX86_S1);
	positivezero = NativeJitX86JumpShort(pThis, 0x79);
	NativeJitX86MoveImmediate(pThis, target, 0x12);
	NativeJitX86Land(pThis, positivezero);
	zerodone = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, notzero);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S1);
	NativeJitX86ShiftImmediate(pThis, 5, target, 20);
	NativeJitX86OperateImmediate(pThis, 4, target, 0x7ff);
	NativeJitX86OperateImmediate(pThis, 7, target, 0x7ff);
	finite = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86Move(pThis, target, NATIVEJITX86_S1);
	NativeJitX86OperateImmediate(pThis, 4, target, 0xfffff);
	NativeJitX86RegsOperand(pThis, 0x0b, target, offset);
	infinite = NativeJitX86JumpShort(pThis, 0x74);
	NativeJitX86MoveImmediate(pThis, target, 0x11);
	nandone = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, infinite);
	NativeJitX86MoveImmediate(pThis, target, 5);
	infinitesigned = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, finite);
	NativeJitX86OperateImmediate(pThis, 7, target, lowexponent);
	normal = NativeJitX86JumpShort(pThis, 0x73);
	NativeJitX86MoveImmediate(pThis, target, 0x14);
	subnormalsigned = NativeJitX86JumpShort(pThis, 0xeb);
	NativeJitX86Land(pThis, normal);
	NativeJitX86MoveImmediate(pThis, target, 4);
	NativeJitX86Land(pThis, infinitesigned);
	NativeJitX86Land(pThis, subnormalsigned);
	NativeJitX86RegReg(pThis, 0x85, NATIVEJITX86_S1, NATIVEJITX86_S1);
	positive = NativeJitX86JumpShort(pThis, 0x79);
	NativeJitX86RegReg(pThis, 0x83, 6, target);
	NativeJitX86Byte(pThis, 0x0c);
	NativeJitX86Land(pThis, positive);
	NativeJitX86Land(pThis, zerodone);
	NativeJitX86Land(pThis, nandone);
}

uint8* NativeJitLabel(NATIVEJITEMITTER* pThis)
{
	return pThis->mCode;
}

uint8* NativeJitBranchIfZero(NATIVEJITEMITTER* pThis, int condition)
{
	NativeJitX86RegReg(pThis, 0x85, condition, condition);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0x84);
	NativeJitX86Dword(pThis, 0);
	return pThis->mCode - 4;
}

uint8* NativeJitFloatBranchIfNaN(NATIVEJITEMITTER* pThis, int source)
{
	NativeJitX86SseRegReg(pThis, 0x66, 0x2e, source, source);
	NativeJitX86Byte(pThis, 0x0f);
	NativeJitX86Byte(pThis, 0x8a);
	NativeJitX86Dword(pThis, 0);
	return pThis->mCode - 4;
}

uint8* NativeJitJump(NATIVEJITEMITTER* pThis)
{
	NativeJitX86Byte(pThis, 0xe9);
	NativeJitX86Dword(pThis, 0);
	return pThis->mCode - 4;
}

void NativeJitBranchLand(NATIVEJITEMITTER* pThis, uint8* branch)
{
	uint32 distance;
	distance = (uint32)(pThis->mCode - (branch + 4));
	branch[0] = (uint8)distance;
	branch[1] = (uint8)(distance >> 8);
	branch[2] = (uint8)(distance >> 16);
	branch[3] = (uint8)(distance >> 24);
}

static void NativeJitX86StoreAbsolute(NATIVEJITEMITTER* pThis, const void* address, uint32 value)
{
	NativeJitX86Byte(pThis, 0xc7);
	NativeJitX86Byte(pThis, 0x05);
	NativeJitX86Dword(pThis, (uint32)(uintptr)address);
	NativeJitX86Dword(pThis, value);
}

uint8* NativeJitChainJump(NATIVEJITEMITTER* pThis, uint32 pcvalue, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* site;
	NativeJitX86RegsOperand(pThis, 0x83, 7, flagsoffset);
	NativeJitX86Byte(pThis, 0);
	flagged = NativeJitX86JumpShort(pThis, 0x75);
	site = pThis->mCode;
	NativeJitX86Byte(pThis, 0xe9);
	NativeJitX86Dword(pThis, 0);
	NativeJitX86Land(pThis, flagged);
	NativeJitX86StoreAbsolute(pThis, &state->mChainSite, (uint32)(uintptr)site);
	NativeJitX86StoreAbsolute(pThis, &state->mChainPc, pcvalue);
	return site;
}

void NativeJitIndirectExit(NATIVEJITEMITTER* pThis, int pcoffset, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* missed;
	NativeJitX86RegsOperand(pThis, 0x83, 7, flagsoffset);
	NativeJitX86Byte(pThis, 0);
	flagged = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86RegsOperand(pThis, 0x8b, NATIVEJITX86_S0, pcoffset);
	NativeJitX86Move(pThis, NATIVEJITX86_S1, NATIVEJITX86_S0);
	NativeJitX86ShiftImmediate(pThis, 5, NATIVEJITX86_S1, 2);
	NativeJitX86OperateImmediate(pThis, 4, NATIVEJITX86_S1, NATIVEJIT_LOOKUP_MASK);
	NativeJitX86Byte(pThis, 0x3b);
	NativeJitX86Byte(pThis, 0x3c);
	NativeJitX86Byte(pThis, 0xad);
	NativeJitX86Dword(pThis, (uint32)(uintptr)state->mLookupPc);
	missed = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86Byte(pThis, 0xff);
	NativeJitX86Byte(pThis, 0x24);
	NativeJitX86Byte(pThis, 0xad);
	NativeJitX86Dword(pThis, (uint32)(uintptr)state->mLookupEntry);
	NativeJitX86Land(pThis, flagged);
	NativeJitX86Land(pThis, missed);
}

uint8* NativeJitChainExit(NATIVEJITEMITTER* pThis, int pcoffset, uint32 pcvalue, int flagsoffset,
	NATIVEJITSTATE* state)
{
	uint8* differentpc;
	uint8* flagged;
	uint8* site;
	NativeJitX86RegsOperand(pThis, 0x81, 7, pcoffset);
	NativeJitX86Dword(pThis, pcvalue);
	differentpc = NativeJitX86JumpShort(pThis, 0x75);
	NativeJitX86RegsOperand(pThis, 0x83, 7, flagsoffset);
	NativeJitX86Byte(pThis, 0);
	flagged = NativeJitX86JumpShort(pThis, 0x75);
	site = pThis->mCode;
	NativeJitX86Byte(pThis, 0xe9);
	NativeJitX86Dword(pThis, 0);
	NativeJitX86StoreAbsolute(pThis, &state->mChainSite, (uint32)(uintptr)site);
	NativeJitX86StoreAbsolute(pThis, &state->mChainPc, pcvalue);
	NativeJitX86Land(pThis, differentpc);
	NativeJitX86Land(pThis, flagged);
	return site;
}

void NativeJitChainLink(uint8* site, uint8* target)
{
	uint32 distance;
	distance = (uint32)(target - (site + 5));
	site[1] = (uint8)distance;
	site[2] = (uint8)(distance >> 8);
	site[3] = (uint8)(distance >> 16);
	site[4] = (uint8)(distance >> 24);
}

void NativeJitCallHelper(NATIVEJITEMITTER* pThis, NATIVEJITHELPER helper, const void* argument)
{
	NativeJitX86StackOperand(pThis, 0x8b, NATIVEJITX86_EAX, NATIVEJITX86_CPUSLOT);
	NativeJitX86StackOperand(pThis, 0x89, NATIVEJITX86_EAX, 0);
	NativeJitX86Byte(pThis, 0xc7);
	NativeJitX86Byte(pThis, 0x44);
	NativeJitX86Byte(pThis, 0x24);
	NativeJitX86Byte(pThis, 4);
	NativeJitX86Dword(pThis, (uint32)(uintptr)argument);
	NativeJitX86MoveImmediate(pThis, NATIVEJITX86_EAX, (uint32)(uintptr)helper);
	NativeJitX86Byte(pThis, 0xff);
	NativeJitX86Byte(pThis, 0xd0);
}

#endif
