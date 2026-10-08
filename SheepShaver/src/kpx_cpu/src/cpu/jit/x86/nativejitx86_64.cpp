#include "sysdeps.h"
#include "vm_alloc.h"
#include "cpu/jit/nativejit.h"

#if defined(NATIVEJIT_HOST_X86_64)

#define NATIVEJITX64_RAX 0
#define NATIVEJITX64_RCX 1
#define NATIVEJITX64_RDX 2
#define NATIVEJITX64_RSI 6
#define NATIVEJITX64_RDI 7
#define NATIVEJITX64_R8 8
#define NATIVEJITX64_R10 10
#define NATIVEJITX64_R11 11
#define NATIVEJITX64_CPU 12
#define NATIVEJITX64_REGS 13
#define NATIVEJITX64_MEMORY 14

#if defined(_WIN32)
#define NATIVEJITX64_ARG0 NATIVEJITX64_RCX
#define NATIVEJITX64_ARG1 NATIVEJITX64_RDX
#define NATIVEJITX64_ARG2 NATIVEJITX64_R8
#else
#define NATIVEJITX64_ARG0 NATIVEJITX64_RDI
#define NATIVEJITX64_ARG1 NATIVEJITX64_RSI
#define NATIVEJITX64_ARG2 NATIVEJITX64_RDX
#endif

static void NativeJitX64Byte(NATIVEJITEMITTER* pThis, uint32 value)
{
	*pThis->mCode++ = (uint8)value;
}

static void NativeJitX64Dword(NATIVEJITEMITTER* pThis, uint32 value)
{
	pThis->mCode[0] = (uint8)value;
	pThis->mCode[1] = (uint8)(value >> 8);
	pThis->mCode[2] = (uint8)(value >> 16);
	pThis->mCode[3] = (uint8)(value >> 24);
	pThis->mCode += 4;
}

static void NativeJitX64Qword(NATIVEJITEMITTER* pThis, uint64 value)
{
	NativeJitX64Dword(pThis, (uint32)value);
	NativeJitX64Dword(pThis, (uint32)(value >> 32));
}

static void NativeJitX64Rex(NATIVEJITEMITTER* pThis, int wide, int reg, int index, int base, int force)
{
	uint32 rex;
	rex = 0x40;
	if (wide)
		rex |= 8;
	if (reg & 8)
		rex |= 4;
	if (index & 8)
		rex |= 2;
	if (base & 8)
		rex |= 1;
	if (rex != 0x40 || force)
		NativeJitX64Byte(pThis, rex);
}

static void NativeJitX64RegReg(NATIVEJITEMITTER* pThis, int wide, uint32 opcode, int reg, int rm)
{
	NativeJitX64Rex(pThis, wide, reg, 0, rm, 0);
	if (opcode > 0xff)
		NativeJitX64Byte(pThis, opcode >> 8);
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

static void NativeJitX64RegsOperand(NATIVEJITEMITTER* pThis, int wide, uint32 opcode, int reg, int offset, int forcerex)
{
	NativeJitX64Rex(pThis, wide, reg, 0, NATIVEJITX64_REGS, forcerex);
	if (opcode > 0xff)
		NativeJitX64Byte(pThis, opcode >> 8);
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0x80 | ((reg & 7) << 3) | (NATIVEJITX64_REGS & 7));
	NativeJitX64Dword(pThis, (uint32)offset);
}

static void NativeJitX64MemoryOperand(NATIVEJITEMITTER* pThis, int prefix16, uint32 opcode, int reg, int forcerex)
{
	if (prefix16)
		NativeJitX64Byte(pThis, 0x66);
	NativeJitX64Rex(pThis, 0, reg, NATIVEJITX64_R10, NATIVEJITX64_MEMORY, forcerex);
	if (opcode > 0xff)
		NativeJitX64Byte(pThis, opcode >> 8);
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0x04 | ((reg & 7) << 3));
	NativeJitX64Byte(pThis, ((NATIVEJITX64_R10 & 7) << 3) | (NATIVEJITX64_MEMORY & 7));
}

static void NativeJitX64MoveImmediate(NATIVEJITEMITTER* pThis, int reg, uint32 value)
{
	NativeJitX64Rex(pThis, 0, 0, 0, reg, 0);
	NativeJitX64Byte(pThis, 0xb8 | (reg & 7));
	NativeJitX64Dword(pThis, value);
}

static void NativeJitX64MoveImmediate64(NATIVEJITEMITTER* pThis, int reg, uint64 value)
{
	NativeJitX64Rex(pThis, 1, 0, 0, reg, 0);
	NativeJitX64Byte(pThis, 0xb8 | (reg & 7));
	NativeJitX64Qword(pThis, value);
}

static void NativeJitX64Move32(NATIVEJITEMITTER* pThis, int target, int source)
{
	NativeJitX64RegReg(pThis, 0, 0x89, source, target);
}

static void NativeJitX64Move64(NATIVEJITEMITTER* pThis, int target, int source)
{
	NativeJitX64RegReg(pThis, 1, 0x89, source, target);
}

static void NativeJitX64ShiftImmediate(NATIVEJITEMITTER* pThis, int wide, int extension, int reg, uint32 amount)
{
	NativeJitX64Rex(pThis, wide, 0, 0, reg, 0);
	NativeJitX64Byte(pThis, 0xc1);
	NativeJitX64Byte(pThis, 0xc0 | (extension << 3) | (reg & 7));
	NativeJitX64Byte(pThis, amount);
}

static void NativeJitX64ShiftCl(NATIVEJITEMITTER* pThis, int wide, int extension, int reg)
{
	NativeJitX64Rex(pThis, wide, 0, 0, reg, 0);
	NativeJitX64Byte(pThis, 0xd3);
	NativeJitX64Byte(pThis, 0xc0 | (extension << 3) | (reg & 7));
}

static uint8* NativeJitX64JumpShort(NATIVEJITEMITTER* pThis, uint32 opcode)
{
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0);
	return pThis->mCode - 1;
}

static void NativeJitX64Land(NATIVEJITEMITTER* pThis, uint8* displacement)
{
	*displacement = (uint8)(pThis->mCode - (displacement + 1));
}

static void NativeJitX64SseRegs(NATIVEJITEMITTER* pThis, uint32 prefix, uint32 opcode, int reg, int offset)
{
	NativeJitX64Byte(pThis, prefix);
	NativeJitX64RegsOperand(pThis, 0, 0x0f00 | opcode, reg, offset, 0);
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
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x54);
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x55);
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x56);
	NativeJitX64Byte(pThis, 0x48);
	NativeJitX64Byte(pThis, 0x83);
	NativeJitX64Byte(pThis, 0xec);
	NativeJitX64Byte(pThis, 0x20);
	NativeJitX64Move64(pThis, NATIVEJITX64_CPU, NATIVEJITX64_ARG0);
	NativeJitX64Move64(pThis, NATIVEJITX64_REGS, NATIVEJITX64_ARG1);
	NativeJitX64Move64(pThis, NATIVEJITX64_MEMORY, NATIVEJITX64_ARG2);
}

void NativeJitEpilogue(NATIVEJITEMITTER* pThis)
{
	NativeJitX64Byte(pThis, 0x48);
	NativeJitX64Byte(pThis, 0x83);
	NativeJitX64Byte(pThis, 0xc4);
	NativeJitX64Byte(pThis, 0x20);
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x5e);
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x5d);
	NativeJitX64Byte(pThis, 0x41);
	NativeJitX64Byte(pThis, 0x5c);
	NativeJitX64Byte(pThis, 0xc3);
}

void NativeJitLoadImmediate(NATIVEJITEMITTER* pThis, int target, uint32 value)
{
	NativeJitX64MoveImmediate(pThis, target, value);
}

void NativeJitMove(NATIVEJITEMITTER* pThis, int target, int source)
{
	if (target != source)
		NativeJitX64Move32(pThis, target, source);
}

void NativeJitLoadRegister(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX64RegsOperand(pThis, 0, 0x8b, target, offset, 0);
}

void NativeJitStoreRegister(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX64RegsOperand(pThis, 0, 0x89, source, offset, 0);
}

void NativeJitStoreRegisterImmediate(NATIVEJITEMITTER* pThis, int offset, uint32 value)
{
	NativeJitX64RegsOperand(pThis, 0, 0xc7, 0, offset, 0);
	NativeJitX64Dword(pThis, value);
}

void NativeJitLoadRegisterByte(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX64RegsOperand(pThis, 0, 0x0fb6, target, offset, 0);
}

void NativeJitStoreRegisterByte(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX64RegsOperand(pThis, 0, 0x88, source, offset, 0);
}

void NativeJitOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitX64RegReg(pThis, 0, 0x01, source, target);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitX64RegReg(pThis, 0, 0x29, source, target);
		break;
	case NATIVEJIT_ALU_AND:
		NativeJitX64RegReg(pThis, 0, 0x21, source, target);
		break;
	case NATIVEJIT_ALU_OR:
		NativeJitX64RegReg(pThis, 0, 0x09, source, target);
		break;
	case NATIVEJIT_ALU_XOR:
		NativeJitX64RegReg(pThis, 0, 0x31, source, target);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitX64RegReg(pThis, 0, 0x0faf, target, source);
		break;
	case NATIVEJIT_ALU_MULHS:
		NativeJitX64RegReg(pThis, 1, 0x63, NATIVEJITX64_R10, target);
		NativeJitX64RegReg(pThis, 1, 0x63, NATIVEJITX64_R11, source);
		NativeJitX64RegReg(pThis, 1, 0x0faf, NATIVEJITX64_R10, NATIVEJITX64_R11);
		NativeJitX64ShiftImmediate(pThis, 1, 5, NATIVEJITX64_R10, 32);
		NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
		break;
	case NATIVEJIT_ALU_MULHU:
		NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
		NativeJitX64Move32(pThis, NATIVEJITX64_R11, source);
		NativeJitX64RegReg(pThis, 1, 0x0faf, NATIVEJITX64_R10, NATIVEJITX64_R11);
		NativeJitX64ShiftImmediate(pThis, 1, 5, NATIVEJITX64_R10, 32);
		NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
		break;
	case NATIVEJIT_ALU_SHL:
		NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
		NativeJitX64ShiftCl(pThis, 1, 4, NATIVEJITX64_R10);
		NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
		break;
	case NATIVEJIT_ALU_SHR:
		NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
		NativeJitX64ShiftCl(pThis, 1, 5, NATIVEJITX64_R10);
		NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
		break;
	case NATIVEJIT_ALU_SAR:
		NativeJitX64RegReg(pThis, 1, 0x63, NATIVEJITX64_R10, target);
		NativeJitX64ShiftCl(pThis, 1, 7, NATIVEJITX64_R10);
		NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
		break;
	case NATIVEJIT_ALU_ROL:
		NativeJitX64ShiftCl(pThis, 0, 0, target);
		break;
	}
}

void NativeJitOperateImmediate(NATIVEJITEMITTER* pThis, int operation, int target, uint32 value)
{
	switch (operation)
	{
	case NATIVEJIT_ALU_ADD:
		NativeJitX64RegReg(pThis, 0, 0x81, 0, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_SUB:
		NativeJitX64RegReg(pThis, 0, 0x81, 5, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_AND:
		NativeJitX64RegReg(pThis, 0, 0x81, 4, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_OR:
		NativeJitX64RegReg(pThis, 0, 0x81, 1, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_XOR:
		NativeJitX64RegReg(pThis, 0, 0x81, 6, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_MUL:
		NativeJitX64RegReg(pThis, 0, 0x69, target, target);
		NativeJitX64Dword(pThis, value);
		break;
	case NATIVEJIT_ALU_SHL:
		if (value & 31)
			NativeJitX64ShiftImmediate(pThis, 0, 4, target, value & 31);
		break;
	case NATIVEJIT_ALU_SHR:
		if (value & 31)
			NativeJitX64ShiftImmediate(pThis, 0, 5, target, value & 31);
		break;
	case NATIVEJIT_ALU_SAR:
		if (value & 31)
			NativeJitX64ShiftImmediate(pThis, 0, 7, target, value & 31);
		break;
	case NATIVEJIT_ALU_ROL:
		if (value & 31)
			NativeJitX64ShiftImmediate(pThis, 0, 0, target, value & 31);
		break;
	}
}

void NativeJitNot(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX64RegReg(pThis, 0, 0xf7, 2, target);
}

void NativeJitNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX64RegReg(pThis, 0, 0xf7, 3, target);
}

void NativeJitSignExtend(NATIVEJITEMITTER* pThis, int target, int bits)
{
	if (bits == 8)
		NativeJitX64RegReg(pThis, 0, 0x0fbe, target, target);
	else
		NativeJitX64RegReg(pThis, 0, 0x0fbf, target, target);
}

void NativeJitAddCarrying(NATIVEJITEMITTER* pThis, int target, int source, int carryin)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, source);
	NativeJitX64RegReg(pThis, 1, 0x01, NATIVEJITX64_R11, NATIVEJITX64_R10);
	if (carryin == NATIVEJIT_CARRY_ONE)
	{
		NativeJitX64RegReg(pThis, 1, 0x83, 0, NATIVEJITX64_R10);
		NativeJitX64Byte(pThis, 1);
	}
	else if (carryin == NATIVEJIT_CARRY_XER)
	{
		NativeJitX64RegsOperand(pThis, 0, 0x0fb6, NATIVEJITX64_R11, pThis->mCarryOffset, 0);
		NativeJitX64RegReg(pThis, 1, 0x01, NATIVEJITX64_R11, NATIVEJITX64_R10);
	}
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 1, 5, NATIVEJITX64_R10, 32);
	NativeJitX64RegsOperand(pThis, 0, 0x88, NATIVEJITX64_R10, pThis->mCarryOffset, 1);
}

void NativeJitShiftRightAlgebraicCarrying(NATIVEJITEMITTER* pThis, int target, int amount)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, target);
	if (amount)
	{
		NativeJitX64ShiftImmediate(pThis, 0, 7, target, amount);
		NativeJitX64RegReg(pThis, 0, 0x81, 4, NATIVEJITX64_R11);
		NativeJitX64Dword(pThis, (1U << amount) - 1);
	}
	else
		NativeJitX64RegReg(pThis, 0, 0x31, NATIVEJITX64_R11, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 0, 0xf7, 3, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 0, 0x19, NATIVEJITX64_R11, NATIVEJITX64_R11);
	NativeJitX64ShiftImmediate(pThis, 0, 5, NATIVEJITX64_R10, 31);
	NativeJitX64RegReg(pThis, 0, 0x21, NATIVEJITX64_R11, NATIVEJITX64_R10);
	NativeJitX64RegsOperand(pThis, 0, 0x88, NATIVEJITX64_R10, pThis->mCarryOffset, 1);
}

void NativeJitShiftRightAlgebraicCarryingRegister(NATIVEJITEMITTER* pThis, int target, int amount)
{
	(void)amount;
	NativeJitX64RegReg(pThis, 1, 0x63, NATIVEJITX64_R10, target);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, target);
	NativeJitX64MoveImmediate(pThis, target, 1);
	NativeJitX64ShiftCl(pThis, 1, 4, target);
	NativeJitX64RegReg(pThis, 1, 0xff, 1, target);
	NativeJitX64RegReg(pThis, 1, 0x21, target, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 1, 0xf7, 3, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 0, 0x19, NATIVEJITX64_R11, NATIVEJITX64_R11);
	NativeJitX64ShiftCl(pThis, 1, 7, NATIVEJITX64_R10);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 1, 5, NATIVEJITX64_R10, 63);
	NativeJitX64RegReg(pThis, 0, 0x21, NATIVEJITX64_R11, NATIVEJITX64_R10);
	NativeJitX64RegsOperand(pThis, 0, 0x88, NATIVEJITX64_R10, pThis->mCarryOffset, 1);
}

void NativeJitCountLeadingZeros(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R10, 63);
	NativeJitX64RegReg(pThis, 0, 0x0fbd, NATIVEJITX64_R11, target);
	NativeJitX64RegReg(pThis, 0, 0x0f44, NATIVEJITX64_R11, NATIVEJITX64_R10);
	NativeJitX64RegReg(pThis, 0, 0x83, 6, NATIVEJITX64_R11);
	NativeJitX64Byte(pThis, 31);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R11);
}

void NativeJitDivide(NATIVEJITEMITTER* pThis, int target, int source, int issigned)
{
	uint8* zero;
	uint8* minimum;
	uint8* overflow;
	uint8* done;
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, target);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, source);
	NativeJitX64RegReg(pThis, 0, 0x85, NATIVEJITX64_R11, NATIVEJITX64_R11);
	zero = NativeJitX64JumpShort(pThis, 0x74);
	minimum = NULL;
	overflow = NULL;
	if (issigned)
	{
		NativeJitX64RegReg(pThis, 0, 0x81, 7, NATIVEJITX64_R10);
		NativeJitX64Dword(pThis, 0x80000000U);
		minimum = NativeJitX64JumpShort(pThis, 0x75);
		NativeJitX64RegReg(pThis, 0, 0x83, 7, NATIVEJITX64_R11);
		NativeJitX64Byte(pThis, 0xff);
		overflow = NativeJitX64JumpShort(pThis, 0x74);
		NativeJitX64Land(pThis, minimum);
		NativeJitX64Move32(pThis, NATIVEJITX64_RAX, NATIVEJITX64_R10);
		NativeJitX64Byte(pThis, 0x99);
		NativeJitX64RegReg(pThis, 0, 0xf7, 7, NATIVEJITX64_R11);
	}
	else
	{
		NativeJitX64Move32(pThis, NATIVEJITX64_RAX, NATIVEJITX64_R10);
		NativeJitX64RegReg(pThis, 0, 0x31, NATIVEJITX64_RDX, NATIVEJITX64_RDX);
		NativeJitX64RegReg(pThis, 0, 0xf7, 6, NATIVEJITX64_R11);
	}
	NativeJitX64Move32(pThis, target, NATIVEJITX64_RAX);
	done = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, zero);
	if (overflow)
		NativeJitX64Land(pThis, overflow);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
	if (issigned)
		NativeJitX64ShiftImmediate(pThis, 0, 7, target, 31);
	else
		NativeJitX64MoveImmediate(pThis, target, 0);
	NativeJitX64Land(pThis, done);
}

static void NativeJitX64CompareResult(NATIVEJITEMITTER* pThis, int target, int issigned)
{
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R11, 8);
	if (issigned)
		NativeJitX64RegReg(pThis, 0, 0x0f4c, NATIVEJITX64_R10, NATIVEJITX64_R11);
	else
		NativeJitX64RegReg(pThis, 0, 0x0f42, NATIVEJITX64_R10, NATIVEJITX64_R11);
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R11, 4);
	if (issigned)
		NativeJitX64RegReg(pThis, 0, 0x0f4f, NATIVEJITX64_R10, NATIVEJITX64_R11);
	else
		NativeJitX64RegReg(pThis, 0, 0x0f47, NATIVEJITX64_R10, NATIVEJITX64_R11);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
}

void NativeJitCompare(NATIVEJITEMITTER* pThis, int target, int left, int right, int issigned)
{
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R10, 2);
	NativeJitX64RegReg(pThis, 0, 0x39, right, left);
	NativeJitX64CompareResult(pThis, target, issigned);
}

void NativeJitCompareImmediate(NATIVEJITEMITTER* pThis, int target, int left, uint32 value, int issigned)
{
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R10, 2);
	NativeJitX64RegReg(pThis, 0, 0x81, 7, left);
	NativeJitX64Dword(pThis, value);
	NativeJitX64CompareResult(pThis, target, issigned);
}

void NativeJitSelect(NATIVEJITEMITTER* pThis, int target, int condition, int whentrue, int whenfalse)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, whenfalse);
	NativeJitX64RegReg(pThis, 0, 0x85, condition, condition);
	NativeJitX64RegReg(pThis, 0, 0x0f45, NATIVEJITX64_R10, whentrue);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
}

void NativeJitLoadMemory(NATIVEJITEMITTER* pThis, int target, int address, int size, int issigned)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	if (size == 4)
	{
		NativeJitX64MemoryOperand(pThis, 0, 0x8b, target, 0);
		NativeJitX64Rex(pThis, 0, 0, 0, target, 0);
		NativeJitX64Byte(pThis, 0x0f);
		NativeJitX64Byte(pThis, 0xc8 | (target & 7));
	}
	else if (size == 2)
	{
		NativeJitX64MemoryOperand(pThis, 0, 0x0fb7, target, 0);
		NativeJitX64Byte(pThis, 0x66);
		NativeJitX64ShiftImmediate(pThis, 0, 0, target, 8);
		if (issigned)
			NativeJitX64RegReg(pThis, 0, 0x0fbf, target, target);
	}
	else if (issigned)
		NativeJitX64MemoryOperand(pThis, 0, 0x0fbe, target, 0);
	else
		NativeJitX64MemoryOperand(pThis, 0, 0x0fb6, target, 0);
}

void NativeJitStoreMemory(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, source);
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	if (size == 4)
	{
		NativeJitX64Rex(pThis, 0, 0, 0, NATIVEJITX64_R11, 0);
		NativeJitX64Byte(pThis, 0x0f);
		NativeJitX64Byte(pThis, 0xc8 | (NATIVEJITX64_R11 & 7));
		NativeJitX64MemoryOperand(pThis, 0, 0x89, NATIVEJITX64_R11, 0);
	}
	else if (size == 2)
	{
		NativeJitX64Byte(pThis, 0x66);
		NativeJitX64ShiftImmediate(pThis, 0, 0, NATIVEJITX64_R11, 8);
		NativeJitX64MemoryOperand(pThis, 1, 0x89, NATIVEJITX64_R11, 0);
	}
	else
		NativeJitX64MemoryOperand(pThis, 0, 0x88, NATIVEJITX64_R11, 1);
}

void NativeJitLoadMemoryReversed(NATIVEJITEMITTER* pThis, int target, int address, int size)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	if (size == 4)
		NativeJitX64MemoryOperand(pThis, 0, 0x8b, target, 0);
	else
		NativeJitX64MemoryOperand(pThis, 0, 0x0fb7, target, 0);
}

void NativeJitStoreMemoryReversed(NATIVEJITEMITTER* pThis, int address, int source, int size)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, source);
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	if (size == 4)
		NativeJitX64MemoryOperand(pThis, 0, 0x89, NATIVEJITX64_R11, 0);
	else
		NativeJitX64MemoryOperand(pThis, 1, 0x89, NATIVEJITX64_R11, 0);
}

void NativeJitLoadMemoryDouble(NATIVEJITEMITTER* pThis, int offset, int address)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	NativeJitX64Byte(pThis, 0x4f);
	NativeJitX64Byte(pThis, 0x8b);
	NativeJitX64Byte(pThis, 0x1c);
	NativeJitX64Byte(pThis, 0x16);
	NativeJitX64Byte(pThis, 0x49);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0xcb);
	NativeJitX64RegsOperand(pThis, 1, 0x89, NATIVEJITX64_R11, offset, 0);
}

void NativeJitStoreMemoryDouble(NATIVEJITEMITTER* pThis, int address, int offset)
{
	NativeJitX64Move32(pThis, NATIVEJITX64_R10, address);
	NativeJitX64RegsOperand(pThis, 1, 0x8b, NATIVEJITX64_R11, offset, 0);
	NativeJitX64Byte(pThis, 0x49);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0xcb);
	NativeJitX64Byte(pThis, 0x4f);
	NativeJitX64Byte(pThis, 0x89);
	NativeJitX64Byte(pThis, 0x1c);
	NativeJitX64Byte(pThis, 0x16);
}

void NativeJitSingleToDouble(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX64Byte(pThis, 0x66);
	NativeJitX64RegReg(pThis, 0, 0x0f6e, 0, source);
	NativeJitX64Byte(pThis, 0xf3);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0x5a);
	NativeJitX64Byte(pThis, 0xc0);
	NativeJitX64SseRegs(pThis, 0xf2, 0x11, 0, offset);
}

void NativeJitDoubleToSingle(NATIVEJITEMITTER* pThis, int target, int offset)
{
	uint8* convert;
	uint8* done;
	NativeJitX64RegsOperand(pThis, 0, 0x8b, NATIVEJITX64_R10, offset + 4, 0);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 0, 5, NATIVEJITX64_R11, 20);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, NATIVEJITX64_R11);
	NativeJitX64Dword(pThis, 0x7ff);
	NativeJitX64RegReg(pThis, 0, 0x81, 5, NATIVEJITX64_R11);
	NativeJitX64Dword(pThis, 874);
	NativeJitX64RegReg(pThis, 0, 0x83, 7, NATIVEJITX64_R11);
	NativeJitX64Byte(pThis, 22);
	convert = NativeJitX64JumpShort(pThis, 0x76);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 0, 4, target, 3);
	NativeJitX64RegsOperand(pThis, 0, 0x8b, NATIVEJITX64_R11, offset, 0);
	NativeJitX64ShiftImmediate(pThis, 0, 5, NATIVEJITX64_R11, 29);
	NativeJitX64RegReg(pThis, 0, 0x09, NATIVEJITX64_R11, target);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, target);
	NativeJitX64Dword(pThis, 0x3fffffff);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, NATIVEJITX64_R10);
	NativeJitX64Dword(pThis, 0xc0000000U);
	NativeJitX64RegReg(pThis, 0, 0x09, NATIVEJITX64_R10, target);
	done = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, convert);
	NativeJitX64SseRegs(pThis, 0xf2, 0x5a, 0, offset);
	NativeJitX64Byte(pThis, 0x66);
	NativeJitX64RegReg(pThis, 0, 0x0f7e, 0, target);
	NativeJitX64Land(pThis, done);
}

static void NativeJitX64SseRegReg(NATIVEJITEMITTER* pThis, uint32 prefix, uint32 opcode, int reg, int rm)
{
	NativeJitX64Byte(pThis, prefix);
	NativeJitX64Rex(pThis, 0, reg, 0, rm, 0);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

int NativeJitHasFloat(void)
{
	return 1;
}

void NativeJitFloatLoad(NATIVEJITEMITTER* pThis, int target, int offset)
{
	NativeJitX64SseRegs(pThis, 0xf2, 0x10, target, offset);
}

void NativeJitFloatStore(NATIVEJITEMITTER* pThis, int offset, int source)
{
	NativeJitX64SseRegs(pThis, 0xf2, 0x11, source, offset);
}

void NativeJitFloatOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source)
{
	switch (operation)
	{
	case NATIVEJIT_FPU_ADD:
		NativeJitX64SseRegReg(pThis, 0xf2, 0x58, target, source);
		break;
	case NATIVEJIT_FPU_SUB:
		NativeJitX64SseRegReg(pThis, 0xf2, 0x5c, target, source);
		break;
	case NATIVEJIT_FPU_MUL:
		NativeJitX64SseRegReg(pThis, 0xf2, 0x59, target, source);
		break;
	case NATIVEJIT_FPU_DIV:
		NativeJitX64SseRegReg(pThis, 0xf2, 0x5e, target, source);
		break;
	}
}

void NativeJitFloatRoundSingle(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX64SseRegReg(pThis, 0xf2, 0x5a, target, target);
	NativeJitX64SseRegReg(pThis, 0xf3, 0x5a, target, target);
}

void NativeJitFloatNegate(NATIVEJITEMITTER* pThis, int target)
{
	NativeJitX64MoveImmediate(pThis, NATIVEJITX64_R10, 0x80000000U);
	NativeJitX64SseRegReg(pThis, 0x66, 0x6e, 3, NATIVEJITX64_R10);
	NativeJitX64Byte(pThis, 0x66);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0x73);
	NativeJitX64Byte(pThis, 0xf3);
	NativeJitX64Byte(pThis, 32);
	NativeJitX64SseRegReg(pThis, 0x66, 0x57, target, 3);
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
	NativeJitX64RegsOperand(pThis, 0, 0x8b, NATIVEJITX64_R11, offset + 4, 0);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R11);
	NativeJitX64ShiftImmediate(pThis, 0, 4, target, 1);
	NativeJitX64RegsOperand(pThis, 0, 0x0b, target, offset, 0);
	notzero = NativeJitX64JumpShort(pThis, 0x75);
	NativeJitX64MoveImmediate(pThis, target, 2);
	NativeJitX64RegReg(pThis, 0, 0x85, NATIVEJITX64_R11, NATIVEJITX64_R11);
	positivezero = NativeJitX64JumpShort(pThis, 0x79);
	NativeJitX64MoveImmediate(pThis, target, 0x12);
	NativeJitX64Land(pThis, positivezero);
	zerodone = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, notzero);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R11);
	NativeJitX64ShiftImmediate(pThis, 0, 5, target, 20);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, target);
	NativeJitX64Dword(pThis, 0x7ff);
	NativeJitX64RegReg(pThis, 0, 0x81, 7, target);
	NativeJitX64Dword(pThis, 0x7ff);
	finite = NativeJitX64JumpShort(pThis, 0x75);
	NativeJitX64Move32(pThis, target, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, target);
	NativeJitX64Dword(pThis, 0xfffff);
	NativeJitX64RegsOperand(pThis, 0, 0x0b, target, offset, 0);
	infinite = NativeJitX64JumpShort(pThis, 0x74);
	NativeJitX64MoveImmediate(pThis, target, 0x11);
	nandone = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, infinite);
	NativeJitX64MoveImmediate(pThis, target, 5);
	infinitesigned = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, finite);
	NativeJitX64RegReg(pThis, 0, 0x81, 7, target);
	NativeJitX64Dword(pThis, lowexponent);
	normal = NativeJitX64JumpShort(pThis, 0x73);
	NativeJitX64MoveImmediate(pThis, target, 0x14);
	subnormalsigned = NativeJitX64JumpShort(pThis, 0xeb);
	NativeJitX64Land(pThis, normal);
	NativeJitX64MoveImmediate(pThis, target, 4);
	NativeJitX64Land(pThis, infinitesigned);
	NativeJitX64Land(pThis, subnormalsigned);
	NativeJitX64RegReg(pThis, 0, 0x85, NATIVEJITX64_R11, NATIVEJITX64_R11);
	positive = NativeJitX64JumpShort(pThis, 0x79);
	NativeJitX64RegReg(pThis, 0, 0x83, 6, target);
	NativeJitX64Byte(pThis, 0x0c);
	NativeJitX64Land(pThis, positive);
	NativeJitX64Land(pThis, zerodone);
	NativeJitX64Land(pThis, nandone);
}

uint8* NativeJitLabel(NATIVEJITEMITTER* pThis)
{
	return pThis->mCode;
}

uint8* NativeJitBranchIfZero(NATIVEJITEMITTER* pThis, int condition)
{
	NativeJitX64RegReg(pThis, 0, 0x85, condition, condition);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0x84);
	NativeJitX64Dword(pThis, 0);
	return pThis->mCode - 4;
}

uint8* NativeJitFloatBranchIfNaN(NATIVEJITEMITTER* pThis, int source)
{
	NativeJitX64SseRegReg(pThis, 0x66, 0x2e, source, source);
	NativeJitX64Byte(pThis, 0x0f);
	NativeJitX64Byte(pThis, 0x8a);
	NativeJitX64Dword(pThis, 0);
	return pThis->mCode - 4;
}

uint8* NativeJitJump(NATIVEJITEMITTER* pThis)
{
	NativeJitX64Byte(pThis, 0xe9);
	NativeJitX64Dword(pThis, 0);
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

static void NativeJitX64RipOperand(NATIVEJITEMITTER* pThis, int wide, uint32 opcode, int reg, const void* address,
	int immediatesize)
{
	NativeJitX64Rex(pThis, wide, reg, 0, 0, 0);
	if (opcode > 0xff)
		NativeJitX64Byte(pThis, opcode >> 8);
	NativeJitX64Byte(pThis, opcode);
	NativeJitX64Byte(pThis, 0x05 | ((reg & 7) << 3));
	NativeJitX64Dword(pThis, (uint32)((const uint8*)address - (pThis->mCode + 4 + immediatesize)));
}

static void NativeJitX64RecordSite(NATIVEJITEMITTER* pThis, uint8* site, uint32 pcvalue, NATIVEJITSTATE* state)
{
	NativeJitX64RipOperand(pThis, 1, 0x8d, NATIVEJITX64_R10, site, 0);
	NativeJitX64RipOperand(pThis, 1, 0x89, NATIVEJITX64_R10, &state->mChainSite, 0);
	NativeJitX64RipOperand(pThis, 0, 0xc7, 0, &state->mChainPc, 4);
	NativeJitX64Dword(pThis, pcvalue);
}

uint8* NativeJitChainJump(NATIVEJITEMITTER* pThis, uint32 pcvalue, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* site;
	NativeJitX64RegsOperand(pThis, 0, 0x83, 7, flagsoffset, 0);
	NativeJitX64Byte(pThis, 0);
	flagged = NativeJitX64JumpShort(pThis, 0x75);
	site = pThis->mCode;
	NativeJitX64Byte(pThis, 0xe9);
	NativeJitX64Dword(pThis, 0);
	NativeJitX64Land(pThis, flagged);
	NativeJitX64RecordSite(pThis, site, pcvalue, state);
	return site;
}

void NativeJitIndirectExit(NATIVEJITEMITTER* pThis, int pcoffset, int flagsoffset, NATIVEJITSTATE* state)
{
	uint8* flagged;
	uint8* missed;
	NativeJitX64RegsOperand(pThis, 0, 0x83, 7, flagsoffset, 0);
	NativeJitX64Byte(pThis, 0);
	flagged = NativeJitX64JumpShort(pThis, 0x75);
	NativeJitX64RegsOperand(pThis, 0, 0x8b, NATIVEJITX64_R10, pcoffset, 0);
	NativeJitX64Move32(pThis, NATIVEJITX64_R11, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 0, 5, NATIVEJITX64_R11, 2);
	NativeJitX64Move32(pThis, NATIVEJITX64_RAX, NATIVEJITX64_R10);
	NativeJitX64ShiftImmediate(pThis, 0, 5, NATIVEJITX64_RAX, NATIVEJIT_LOOKUP_SHIFT);
	NativeJitX64RegReg(pThis, 0, 0x31, NATIVEJITX64_RAX, NATIVEJITX64_R11);
	NativeJitX64RegReg(pThis, 0, 0x81, 4, NATIVEJITX64_R11);
	NativeJitX64Dword(pThis, NATIVEJIT_LOOKUP_MASK);
	NativeJitX64RipOperand(pThis, 1, 0x8d, NATIVEJITX64_RAX, state->mLookupPc, 0);
	NativeJitX64Byte(pThis, 0x46);
	NativeJitX64Byte(pThis, 0x3b);
	NativeJitX64Byte(pThis, 0x14);
	NativeJitX64Byte(pThis, 0x98);
	missed = NativeJitX64JumpShort(pThis, 0x75);
	NativeJitX64Byte(pThis, 0x42);
	NativeJitX64Byte(pThis, 0xff);
	NativeJitX64Byte(pThis, 0xa4);
	NativeJitX64Byte(pThis, 0xd8);
	NativeJitX64Dword(pThis, (uint32)((uint8*)state->mLookupEntry - (uint8*)state->mLookupPc));
	NativeJitX64Land(pThis, flagged);
	NativeJitX64Land(pThis, missed);
}

uint8* NativeJitChainExit(NATIVEJITEMITTER* pThis, int pcoffset, uint32 pcvalue, int flagsoffset,
	NATIVEJITSTATE* state)
{
	uint8* differentpc;
	uint8* flagged;
	uint8* site;
	NativeJitX64RegsOperand(pThis, 0, 0x81, 7, pcoffset, 0);
	NativeJitX64Dword(pThis, pcvalue);
	differentpc = NativeJitX64JumpShort(pThis, 0x75);
	NativeJitX64RegsOperand(pThis, 0, 0x83, 7, flagsoffset, 0);
	NativeJitX64Byte(pThis, 0);
	flagged = NativeJitX64JumpShort(pThis, 0x75);
	site = pThis->mCode;
	NativeJitX64Byte(pThis, 0xe9);
	NativeJitX64Dword(pThis, 0);
	NativeJitX64RecordSite(pThis, site, pcvalue, state);
	NativeJitX64Land(pThis, differentpc);
	NativeJitX64Land(pThis, flagged);
	return site;
}

void NativeJitJumpTo(uint8* site, uint8* target)
{
	site[0] = 0xe9;
	NativeJitChainLink(site, target);
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
	NativeJitX64Move64(pThis, NATIVEJITX64_ARG0, NATIVEJITX64_CPU);
	NativeJitX64MoveImmediate64(pThis, NATIVEJITX64_ARG1, (uint64)(uintptr)argument);
	NativeJitX64MoveImmediate64(pThis, NATIVEJITX64_RAX, (uint64)(uintptr)helper);
	NativeJitX64Byte(pThis, 0xff);
	NativeJitX64Byte(pThis, 0xd0);
}

#endif
