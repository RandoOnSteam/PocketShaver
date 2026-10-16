#include "sysdeps.h"
#include "cpu/ppc/ppc-cpu.hpp"

#if PPC_NATIVE_JIT

#if defined(__APPLE__) || defined(MEM_BULK)
#define NATIVEJIT_DIRECT_MEMORY 0
#else
#define NATIVEJIT_DIRECT_MEMORY 1
#endif

#define NATIVEJIT_OPCODE_RD(opcode) (((opcode) >> 21) & 31)
#define NATIVEJIT_OPCODE_RA(opcode) (((opcode) >> 16) & 31)
#define NATIVEJIT_OPCODE_RB(opcode) (((opcode) >> 11) & 31)
#define NATIVEJIT_OPCODE_SIMM(opcode) ((((uint32)(opcode) & 0xffffU) ^ 0x8000U) - 0x8000U)
#define NATIVEJIT_OPCODE_UIMM(opcode) ((opcode) & 0xffff)

#define NATIVEJIT_XER_SO 0
#define NATIVEJIT_XER_CA 1
#define NATIVEJIT_XER_OV 2
#define NATIVEJIT_XER_COUNT 3

typedef struct NATIVEJITLAYOUT
{
	int mGpr;
	int mFpr;
	int mFprHigh;
	int mFprLow;
	int mCr;
	int mLr;
	int mCtr;
	int mPc;
	int mFlags;
	int mSummaryOverflow;
	int mOverflow;
	int mCarry;
	int mCount;
	int mFpscr;
	int mFloat;
	NATIVEJITSTATE* mState;
} NATIVEJITLAYOUT;

static int NativeJitXerByteOffset(int field)
{
	powerpc_xer_register probe;
	const uint8* bytes;
	int index;
	probe.set(0);
	switch (field)
	{
	case NATIVEJIT_XER_SO:
		probe.set_so(1);
		break;
	case NATIVEJIT_XER_CA:
		probe.set_ca(1);
		break;
	case NATIVEJIT_XER_OV:
		probe.set_ov(1);
		probe.set_so(0);
		break;
	default:
		probe.set_count(1);
		break;
	}
	bytes = (const uint8*)&probe;
	for (index = 0; index < (int)sizeof(probe); index++)
	{
		if (bytes[index])
			return index;
	}
	return 0;
}

static int NativeJitFprWordOffset(int high)
{
	powerpc_fpr probe;
	const uint32* words;
	probe.j = 1;
	words = (const uint32*)&probe;
	if (words[0] == 1)
		return 4 * high;
	return 4 * !high;
}

static uint32 NativeJitRotateMask(uint32 maskbegin, uint32 maskend)
{
	uint32 high;
	uint32 low;
	high = 0xffffffffU >> maskbegin;
	low = 0xffffffffU << (31 - maskend);
	if (maskbegin <= maskend)
		return high & low;
	return high | low;
}

static void NativeJitLoadGpr(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, int target, uint32 gpr)
{
	NativeJitLoadRegister(emitter, target, layout->mGpr + 4 * gpr);
}

static void NativeJitStoreGpr(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 gpr, int source)
{
	NativeJitStoreRegister(emitter, layout->mGpr + 4 * gpr, source);
}

static void NativeJitSetCrField(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 field, int bits, int scratch)
{
	uint32 shift;
	shift = 28 - 4 * field;
	NativeJitLoadRegisterByte(emitter, scratch, layout->mSummaryOverflow);
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, bits, scratch);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHL, bits, shift);
	NativeJitLoadRegister(emitter, scratch, layout->mCr);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, scratch, ~(0xfU << shift));
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, scratch, bits);
	NativeJitStoreRegister(emitter, layout->mCr, scratch);
}

static void NativeJitRecord(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	if (opcode & 1)
	{
		NativeJitCompareImmediate(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 0, 1);
		NativeJitSetCrField(emitter, layout, 0, NATIVEJIT_T1, NATIVEJIT_T2);
	}
}

static void NativeJitEffectiveAddress(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int indexed)
{
	uint32 base;
	base = NATIVEJIT_OPCODE_RA(opcode);
	if (indexed)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
		if (base)
		{
			NativeJitLoadGpr(emitter, layout, NATIVEJIT_T2, base);
			NativeJitOperate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T1, NATIVEJIT_T2);
		}
	}
	else if (base)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, base);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T1, NATIVEJIT_OPCODE_SIMM(opcode));
	}
	else
		NativeJitLoadImmediate(emitter, NATIVEJIT_T1, NATIVEJIT_OPCODE_SIMM(opcode));
}

static int NativeJitTranslateMemory(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int indexed, int isload, int size, int issigned, int update)
{
	uint32 data;
	uint32 base;
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	data = NATIVEJIT_OPCODE_RD(opcode);
	base = NATIVEJIT_OPCODE_RA(opcode);
	if (update && (base == 0 || (isload && base == data)))
		return 0;
	NativeJitEffectiveAddress(emitter, layout, opcode, indexed);
	if (isload)
	{
		NativeJitLoadMemory(emitter, NATIVEJIT_T0, NATIVEJIT_T1, size, issigned);
		NativeJitStoreGpr(emitter, layout, data, NATIVEJIT_T0);
	}
	else
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, data);
		NativeJitStoreMemory(emitter, NATIVEJIT_T1, NATIVEJIT_T0, size);
	}
	if (update)
		NativeJitStoreGpr(emitter, layout, base, NATIVEJIT_T1);
	return 1;
}

static void NativeJitPrepareOverflow(NATIVEJITEMITTER* emitter)
{
	NativeJitMove(emitter, NATIVEJIT_T2, NATIVEJIT_T0);
	NativeJitOperate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T2, NATIVEJIT_T1);
	NativeJitNot(emitter, NATIVEJIT_T2);
}

static void NativeJitStoreOverflow(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout)
{
	NativeJitOperate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T1, NATIVEJIT_T0);
	NativeJitOperate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T2, NATIVEJIT_T1);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T2, 31);
	NativeJitStoreRegisterByte(emitter, layout->mOverflow, NATIVEJIT_T2);
	NativeJitLoadRegisterByte(emitter, NATIVEJIT_T1, layout->mSummaryOverflow);
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T1, NATIVEJIT_T2);
	NativeJitStoreRegisterByte(emitter, layout->mSummaryOverflow, NATIVEJIT_T1);
}

static int NativeJitTranslateCarrying(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int invert, int useregister, uint32 constant, int carryin, int overflow)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
	if (invert)
		NativeJitNot(emitter, NATIVEJIT_T0);
	if (useregister)
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	else
		NativeJitLoadImmediate(emitter, NATIVEJIT_T1, constant);
	if (overflow)
		NativeJitPrepareOverflow(emitter);
	NativeJitAddCarrying(emitter, NATIVEJIT_T0, NATIVEJIT_T1, carryin);
	if (overflow)
		NativeJitStoreOverflow(emitter, layout);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateOverflowArithmetic(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout,
	uint32 opcode, int subtract)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	if (subtract)
		NativeJitNot(emitter, NATIVEJIT_T0);
	NativeJitPrepareOverflow(emitter);
	NativeJitOperate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T0, NATIVEJIT_T1);
	if (subtract)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T0, 1);
	NativeJitStoreOverflow(emitter, layout);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateArithmetic(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int operation, int swap)
{
	if (swap)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RA(opcode));
	}
	else
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	}
	NativeJitOperate(emitter, operation, NATIVEJIT_T0, NATIVEJIT_T1);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateLogical(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int operation, int invertsource, int invertresult)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	if (invertsource)
		NativeJitNot(emitter, NATIVEJIT_T1);
	NativeJitOperate(emitter, operation, NATIVEJIT_T0, NATIVEJIT_T1);
	if (invertresult)
		NativeJitNot(emitter, NATIVEJIT_T0);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateLogicalImmediate(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int operation, uint32 value, int record)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
	NativeJitOperateImmediate(emitter, operation, NATIVEJIT_T0, value);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
	if (record)
	{
		NativeJitCompareImmediate(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 0, 1);
		NativeJitSetCrField(emitter, layout, 0, NATIVEJIT_T1, NATIVEJIT_T2);
	}
	return 1;
}

static int NativeJitTranslateUnary(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int source, int destination, int signextendbits, int negate)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, source);
	if (signextendbits)
		NativeJitSignExtend(emitter, NATIVEJIT_T0, signextendbits);
	if (negate)
		NativeJitNegate(emitter, NATIVEJIT_T0);
	NativeJitStoreGpr(emitter, layout, destination, NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateShift(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int operation)
{
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	NativeJitOperate(emitter, operation, NATIVEJIT_T0, NATIVEJIT_T1);
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateCompare(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int immediate, int issigned)
{
	uint32 field;
	if (opcode & (1 << 21))
		return 0;
	field = (opcode >> 23) & 7;
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
	if (immediate)
	{
		if (issigned)
			NativeJitCompareImmediate(emitter, NATIVEJIT_T2, NATIVEJIT_T0, NATIVEJIT_OPCODE_SIMM(opcode), 1);
		else
			NativeJitCompareImmediate(emitter, NATIVEJIT_T2, NATIVEJIT_T0, NATIVEJIT_OPCODE_UIMM(opcode), 0);
	}
	else
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitCompare(emitter, NATIVEJIT_T2, NATIVEJIT_T0, NATIVEJIT_T1, issigned);
	}
	NativeJitSetCrField(emitter, layout, field, NATIVEJIT_T2, NATIVEJIT_T0);
	return 1;
}

static int NativeJitTranslateRotate(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int primary)
{
	uint32 mask;
	mask = NativeJitRotateMask((opcode >> 6) & 31, (opcode >> 1) & 31);
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
	if (primary == 23)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitOperate(emitter, NATIVEJIT_ALU_ROL, NATIVEJIT_T0, NATIVEJIT_T1);
	}
	else
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ROL, NATIVEJIT_T0, NATIVEJIT_OPCODE_RB(opcode));
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, mask);
	if (primary == 20)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RA(opcode));
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, ~mask);
		NativeJitOperate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T0, NATIVEJIT_T1);
	}
	NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
	NativeJitRecord(emitter, layout, opcode);
	return 1;
}

static int NativeJitTranslateXer(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int toxer);

static int NativeJitTranslateSpecial(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int tospecial)
{
	uint32 special;
	int offset;
	special = NATIVEJIT_OPCODE_RA(opcode) | (NATIVEJIT_OPCODE_RB(opcode) << 5);
	if (special == 1)
		return NativeJitTranslateXer(emitter, layout, opcode, tospecial);
	if (special == 8)
		offset = layout->mLr;
	else if (special == 9)
		offset = layout->mCtr;
	else
		return 0;
	if (tospecial)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitStoreRegister(emitter, offset, NATIVEJIT_T0);
	}
	else
	{
		NativeJitLoadRegister(emitter, NATIVEJIT_T0, offset);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	}
	return 1;
}

static void NativeJitCrBit(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, int target, uint32 bit)
{
	NativeJitLoadRegister(emitter, target, layout->mCr);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, target, 31 - bit);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, target, 1);
}

static void NativeJitInsertCr(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, int value, int scratch,
	uint32 shift, uint32 mask)
{
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHL, value, shift);
	NativeJitLoadRegister(emitter, scratch, layout->mCr);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, scratch, ~(mask << shift));
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, scratch, value);
	NativeJitStoreRegister(emitter, layout->mCr, scratch);
}

static int NativeJitTranslateConditionLogical(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int operation, int invertsource, int invertresult)
{
	NativeJitCrBit(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
	NativeJitCrBit(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
	if (invertsource)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T1, 1);
	NativeJitOperate(emitter, operation, NATIVEJIT_T0, NATIVEJIT_T1);
	if (invertresult)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T0, 1);
	NativeJitInsertCr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_T1, 31 - NATIVEJIT_OPCODE_RD(opcode), 1);
	return 1;
}

static int NativeJitTranslateConditionMove(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	uint32 target;
	uint32 source;
	target = (opcode >> 23) & 7;
	source = (opcode >> 18) & 7;
	NativeJitLoadRegister(emitter, NATIVEJIT_T0, layout->mCr);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T0, 28 - 4 * source);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, 0xf);
	NativeJitInsertCr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_T1, 28 - 4 * target, 0xf);
	return 1;
}

static int NativeJitTranslateMoveToCr(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	uint32 fields;
	uint32 mask;
	int index;
	fields = (opcode >> 12) & 0xff;
	mask = 0;
	for (index = 0; index < 8; index++)
	{
		if (fields & (0x80 >> index))
			mask |= 0xfU << (28 - 4 * index);
	}
	NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, mask);
	NativeJitLoadRegister(emitter, NATIVEJIT_T1, layout->mCr);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, ~mask);
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T0, NATIVEJIT_T1);
	NativeJitStoreRegister(emitter, layout->mCr, NATIVEJIT_T0);
	return 1;
}

static void NativeJitXerField(NATIVEJITEMITTER* emitter, int offset, uint32 shift)
{
	NativeJitLoadRegisterByte(emitter, NATIVEJIT_T1, offset);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHL, NATIVEJIT_T1, shift);
	NativeJitOperate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T0, NATIVEJIT_T1);
}

static void NativeJitSetXerField(NATIVEJITEMITTER* emitter, int offset, uint32 shift, uint32 mask)
{
	NativeJitMove(emitter, NATIVEJIT_T1, NATIVEJIT_T0);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T1, shift);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, mask);
	NativeJitStoreRegisterByte(emitter, offset, NATIVEJIT_T1);
}

static int NativeJitTranslateXer(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int toxer)
{
	if (toxer)
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitSetXerField(emitter, layout->mSummaryOverflow, 31, 1);
		NativeJitSetXerField(emitter, layout->mOverflow, 30, 1);
		NativeJitSetXerField(emitter, layout->mCarry, 29, 1);
		NativeJitSetXerField(emitter, layout->mCount, 0, 0x7f);
	}
	else
	{
		NativeJitLoadRegisterByte(emitter, NATIVEJIT_T0, layout->mCount);
		NativeJitXerField(emitter, layout->mSummaryOverflow, 31);
		NativeJitXerField(emitter, layout->mOverflow, 30);
		NativeJitXerField(emitter, layout->mCarry, 29);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	}
	return 1;
}

static int NativeJitTranslateMultiple(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, int isload)
{
	uint32 gpr;
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	NativeJitEffectiveAddress(emitter, layout, opcode, 0);
	for (gpr = NATIVEJIT_OPCODE_RD(opcode); gpr <= 31; gpr++)
	{
		if (isload)
		{
			NativeJitLoadMemory(emitter, NATIVEJIT_T0, NATIVEJIT_T1, 4, 0);
			NativeJitStoreGpr(emitter, layout, gpr, NATIVEJIT_T0);
		}
		else
		{
			NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, gpr);
			NativeJitStoreMemory(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 4);
		}
		if (gpr < 31)
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T1, 4);
	}
	return 1;
}

static int NativeJitTranslateReversed(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int isload, int size)
{
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	NativeJitEffectiveAddress(emitter, layout, opcode, 1);
	if (isload)
	{
		NativeJitLoadMemoryReversed(emitter, NATIVEJIT_T0, NATIVEJIT_T1, size);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
	}
	else
	{
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitStoreMemoryReversed(emitter, NATIVEJIT_T1, NATIVEJIT_T0, size);
	}
	return 1;
}

static int NativeJitTranslateZeroLine(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	int index;
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	NativeJitEffectiveAddress(emitter, layout, opcode, 1);
	NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, ~31U);
	NativeJitLoadImmediate(emitter, NATIVEJIT_T0, 0);
	for (index = 0; index < 8; index++)
	{
		NativeJitStoreMemoryReversed(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 4);
		if (index < 7)
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T1, 4);
	}
	return 1;
}

static int NativeJitTranslateFloatMemory(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int indexed, int isload, int isdouble, int update)
{
	int fpr;
	uint32 base;
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	base = NATIVEJIT_OPCODE_RA(opcode);
	if (update && base == 0)
		return 0;
	fpr = layout->mFpr + 8 * NATIVEJIT_OPCODE_RD(opcode);
	NativeJitEffectiveAddress(emitter, layout, opcode, indexed);
	if (isdouble && layout->mFloat)
	{
		if (isload)
			NativeJitLoadMemoryDouble(emitter, fpr, NATIVEJIT_T1);
		else
			NativeJitStoreMemoryDouble(emitter, NATIVEJIT_T1, fpr);
	}
	else if (isload && isdouble)
	{
		NativeJitLoadMemory(emitter, NATIVEJIT_T0, NATIVEJIT_T1, 4, 0);
		NativeJitStoreRegister(emitter, fpr + layout->mFprHigh, NATIVEJIT_T0);
		NativeJitMove(emitter, NATIVEJIT_T2, NATIVEJIT_T1);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T2, 4);
		NativeJitLoadMemory(emitter, NATIVEJIT_T0, NATIVEJIT_T2, 4, 0);
		NativeJitStoreRegister(emitter, fpr + layout->mFprLow, NATIVEJIT_T0);
	}
	else if (isload)
	{
		NativeJitLoadMemory(emitter, NATIVEJIT_T0, NATIVEJIT_T1, 4, 0);
		NativeJitSingleToDouble(emitter, fpr, NATIVEJIT_T0);
	}
	else if (isdouble)
	{
		NativeJitLoadRegister(emitter, NATIVEJIT_T0, fpr + layout->mFprHigh);
		NativeJitStoreMemory(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 4);
		NativeJitMove(emitter, NATIVEJIT_T2, NATIVEJIT_T1);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T2, 4);
		NativeJitLoadRegister(emitter, NATIVEJIT_T0, fpr + layout->mFprLow);
		NativeJitStoreMemory(emitter, NATIVEJIT_T2, NATIVEJIT_T0, 4);
	}
	else
	{
		NativeJitDoubleToSingle(emitter, NATIVEJIT_T0, fpr);
		NativeJitStoreMemory(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 4);
	}
	if (update)
		NativeJitStoreGpr(emitter, layout, base, NATIVEJIT_T1);
	return 1;
}

static int NativeJitTranslateStoreFloatWord(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	if (!NATIVEJIT_DIRECT_MEMORY)
		return 0;
	NativeJitEffectiveAddress(emitter, layout, opcode, 1);
	NativeJitLoadRegister(emitter, NATIVEJIT_T0, layout->mFpr + 8 * NATIVEJIT_OPCODE_RD(opcode) + layout->mFprLow);
	NativeJitStoreMemory(emitter, NATIVEJIT_T1, NATIVEJIT_T0, 4);
	return 1;
}

static int NativeJitTranslateFloatMove(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	uint32 andmask, uint32 ormask, uint32 xormask)
{
	int target;
	int source;
	if (opcode & 1)
		return 0;
	target = layout->mFpr + 8 * NATIVEJIT_OPCODE_RD(opcode);
	source = layout->mFpr + 8 * NATIVEJIT_OPCODE_RB(opcode);
	if (layout->mFloat && andmask == 0xffffffffU && ormask == 0)
	{
		NativeJitFloatLoad(emitter, NATIVEJIT_F0, source);
		if (xormask)
			NativeJitFloatNegate(emitter, NATIVEJIT_F0);
		NativeJitFloatStore(emitter, target, NATIVEJIT_F0);
		return 1;
	}
	NativeJitLoadRegister(emitter, NATIVEJIT_T0, source + layout->mFprLow);
	NativeJitStoreRegister(emitter, target + layout->mFprLow, NATIVEJIT_T0);
	NativeJitLoadRegister(emitter, NATIVEJIT_T0, source + layout->mFprHigh);
	if (andmask != 0xffffffffU)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, andmask);
	if (ormask)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T0, ormask);
	if (xormask)
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T0, xormask);
	NativeJitStoreRegister(emitter, target + layout->mFprHigh, NATIVEJIT_T0);
	return 1;
}

static int NativeJitIsFloatArithmetic(uint32 opcode)
{
	uint32 primary;
	uint32 extended;
	primary = opcode >> 26;
	extended = (opcode >> 1) & 31;
	if ((primary != 59 && primary != 63) || (opcode & 1))
		return 0;
	if (extended == 18 || extended == 20 || extended == 21 || extended == 25)
		return 1;
	return extended >= 28;
}

static int NativeJitIsFprfNeutral(uint32 opcode)
{
	uint32 primary;
	uint32 extended;
	primary = opcode >> 26;
	if (NativeJitIsFloatArithmetic(opcode))
		return 1;
	if (primary == 7 || primary == 8 || primary == 10 || primary == 11 || primary == 12 || primary == 13)
		return 1;
	if (primary == 14 || primary == 15 || primary == 20 || primary == 21 || primary == 23)
		return 1;
	if (primary >= 24 && primary <= 29)
		return 1;
	if (primary >= 32 && primary <= 55)
		return 1;
	extended = (opcode >> 1) & 0x3ff;
	if (primary == 63 && !(opcode & 1))
		return extended == 72 || extended == 40 || extended == 264 || extended == 136;
	return 0;
}

static int NativeJitFprfLive(const powerpc_block_info* bi, int index)
{
	int next;
	for (next = index + 1; next < bi->size; next++)
	{
		if (NativeJitIsFloatArithmetic(bi->di[next].opcode))
			return 0;
		if (!NativeJitIsFprfNeutral(bi->di[next].opcode))
			return 1;
	}
	return 1;
}

static int NativeJitTranslateFloatArithmetic(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	int fprflive, uint32 address, const void* decodeinfo)
{
	uint32 extended;
	uint32 lowexponent;
	uint8* nan;
	uint8* done;
	int target;
	int left;
	int right;
	int multiplier;
	int single;
	if (!layout->mFloat || !NativeJitIsFloatArithmetic(opcode))
		return 0;
	extended = (opcode >> 1) & 31;
	single = (opcode >> 26) == 59;
	target = layout->mFpr + 8 * NATIVEJIT_OPCODE_RD(opcode);
	left = layout->mFpr + 8 * NATIVEJIT_OPCODE_RA(opcode);
	right = layout->mFpr + 8 * NATIVEJIT_OPCODE_RB(opcode);
	multiplier = layout->mFpr + 8 * ((opcode >> 6) & 31);
	NativeJitFloatLoad(emitter, NATIVEJIT_F0, left);
	switch (extended)
	{
	case 21:
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, right);
		NativeJitFloatOperate(emitter, NATIVEJIT_FPU_ADD, NATIVEJIT_F0, NATIVEJIT_F1);
		break;
	case 20:
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, right);
		NativeJitFloatOperate(emitter, NATIVEJIT_FPU_SUB, NATIVEJIT_F0, NATIVEJIT_F1);
		break;
	case 18:
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, right);
		NativeJitFloatOperate(emitter, NATIVEJIT_FPU_DIV, NATIVEJIT_F0, NATIVEJIT_F1);
		break;
	case 25:
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, multiplier);
		NativeJitFloatOperate(emitter, NATIVEJIT_FPU_MUL, NATIVEJIT_F0, NATIVEJIT_F1);
		break;
	default:
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, multiplier);
		NativeJitFloatOperate(emitter, NATIVEJIT_FPU_MUL, NATIVEJIT_F0, NATIVEJIT_F1);
		NativeJitFloatLoad(emitter, NATIVEJIT_F1, right);
		if (extended & 1)
			NativeJitFloatOperate(emitter, NATIVEJIT_FPU_ADD, NATIVEJIT_F0, NATIVEJIT_F1);
		else
			NativeJitFloatOperate(emitter, NATIVEJIT_FPU_SUB, NATIVEJIT_F0, NATIVEJIT_F1);
		break;
	}
	nan = NativeJitFloatBranchIfNaN(emitter, NATIVEJIT_F0);
	if (single)
		NativeJitFloatRoundSingle(emitter, NATIVEJIT_F0);
	if (extended == 30 || extended == 31)
		NativeJitFloatNegate(emitter, NATIVEJIT_F0);
	NativeJitFloatStore(emitter, target, NATIVEJIT_F0);
	if (fprflive)
	{
		lowexponent = 1;
		if (single && extended != 30 && extended != 31)
			lowexponent = 897;
		NativeJitFloatClass(emitter, NATIVEJIT_T0, target, lowexponent);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHL, NATIVEJIT_T0, 12);
		NativeJitLoadRegister(emitter, NATIVEJIT_T1, layout->mFpscr);
		NativeJitMove(emitter, NATIVEJIT_T2, NATIVEJIT_T1);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T2, ~0x1f000U);
		NativeJitOperate(emitter, NATIVEJIT_ALU_OR, NATIVEJIT_T2, NATIVEJIT_T0);
		NativeJitMove(emitter, NATIVEJIT_T0, NATIVEJIT_T1);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, 0x80);
		NativeJitSelect(emitter, NATIVEJIT_T1, NATIVEJIT_T0, NATIVEJIT_T1, NATIVEJIT_T2);
		NativeJitStoreRegister(emitter, layout->mFpscr, NATIVEJIT_T1);
	}
	done = NativeJitJump(emitter);
	NativeJitBranchLand(emitter, nan);
	NativeJitStoreRegisterImmediate(emitter, layout->mPc, address);
	NativeJitCallHelper(emitter, powerpc_cpu::NativeJitInterpret, decodeinfo);
	NativeJitBranchLand(emitter, done);
	return 4;
}

static int NativeJitTranslateBranch(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode,
	uint32 address, int targetoffset)
{
	uint32 options;
	uint32 conditionbit;
	uint32 target;
	options = NATIVEJIT_OPCODE_RD(opcode);
	conditionbit = NATIVEJIT_OPCODE_RA(opcode);
	if (targetoffset == layout->mCtr && !(options & 4))
		return 0;
	NativeJitLoadImmediate(emitter, NATIVEJIT_T0, 1);
	if (!(options & 4))
	{
		NativeJitLoadRegister(emitter, NATIVEJIT_T1, layout->mCtr);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SUB, NATIVEJIT_T1, 1);
		NativeJitStoreRegister(emitter, layout->mCtr, NATIVEJIT_T1);
		NativeJitCompareImmediate(emitter, NATIVEJIT_T0, NATIVEJIT_T1, 0, 0);
		if (options & 2)
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T0, 1);
		else
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T0, 2);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, 1);
	}
	if (!(options & 16))
	{
		NativeJitLoadRegister(emitter, NATIVEJIT_T1, layout->mCr);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_SHR, NATIVEJIT_T1, 31 - conditionbit);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, 1);
		if (!(options & 8))
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_XOR, NATIVEJIT_T1, 1);
		NativeJitOperate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T0, NATIVEJIT_T1);
	}
	if (targetoffset == 0 && (options & 20) != 20)
	{
		if (opcode & 1)
			NativeJitStoreRegisterImmediate(emitter, layout->mLr, address + 4);
		return 3;
	}
	if (targetoffset)
	{
		NativeJitLoadRegister(emitter, NATIVEJIT_T1, targetoffset);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_AND, NATIVEJIT_T1, ~3U);
	}
	else
	{
		target = NATIVEJIT_OPCODE_SIMM(opcode) & ~3U;
		if (!(opcode & 2))
			target += address;
		NativeJitLoadImmediate(emitter, NATIVEJIT_T1, target);
	}
	NativeJitLoadImmediate(emitter, NATIVEJIT_T2, address + 4);
	NativeJitSelect(emitter, NATIVEJIT_T1, NATIVEJIT_T0, NATIVEJIT_T1, NATIVEJIT_T2);
	NativeJitStoreRegister(emitter, layout->mPc, NATIVEJIT_T1);
	if (opcode & 1)
		NativeJitStoreRegisterImmediate(emitter, layout->mLr, address + 4);
	return 2;
}

static int NativeJitTranslateExtended(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode)
{
	uint32 extended;
	extended = (opcode >> 1) & 0x3ff;
	switch (extended)
	{
	case 0:
		return NativeJitTranslateCompare(emitter, layout, opcode, 0, 1);
	case 32:
		return NativeJitTranslateCompare(emitter, layout, opcode, 0, 0);
	case 266:
		return NativeJitTranslateArithmetic(emitter, layout, opcode, NATIVEJIT_ALU_ADD, 0);
	case 40:
		return NativeJitTranslateArithmetic(emitter, layout, opcode, NATIVEJIT_ALU_SUB, 1);
	case 235:
		return NativeJitTranslateArithmetic(emitter, layout, opcode, NATIVEJIT_ALU_MUL, 0);
	case 75:
		return NativeJitTranslateArithmetic(emitter, layout, opcode, NATIVEJIT_ALU_MULHS, 0);
	case 11:
		return NativeJitTranslateArithmetic(emitter, layout, opcode, NATIVEJIT_ALU_MULHU, 0);
	case 10:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 1, 0, NATIVEJIT_CARRY_ZERO, 0);
	case 138:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 1, 0, NATIVEJIT_CARRY_XER, 0);
	case 202:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 0, 0, NATIVEJIT_CARRY_XER, 0);
	case 234:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 0, 0xffffffffU, NATIVEJIT_CARRY_XER, 0);
	case 8:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 1, 0, NATIVEJIT_CARRY_ONE, 0);
	case 136:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 1, 0, NATIVEJIT_CARRY_XER, 0);
	case 200:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 0, 0, NATIVEJIT_CARRY_XER, 0);
	case 232:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 0, 0xffffffffU, NATIVEJIT_CARRY_XER, 0);
	case 522:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 1, 0, NATIVEJIT_CARRY_ZERO, 1);
	case 650:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 1, 0, NATIVEJIT_CARRY_XER, 1);
	case 714:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 0, 0, NATIVEJIT_CARRY_XER, 1);
	case 746:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 0, 0, 0xffffffffU, NATIVEJIT_CARRY_XER, 1);
	case 520:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 1, 0, NATIVEJIT_CARRY_ONE, 1);
	case 648:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 1, 0, NATIVEJIT_CARRY_XER, 1);
	case 712:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 0, 0, NATIVEJIT_CARRY_XER, 1);
	case 744:
		return NativeJitTranslateCarrying(emitter, layout, opcode, 1, 0, 0xffffffffU, NATIVEJIT_CARRY_XER, 1);
	case 778:
		return NativeJitTranslateOverflowArithmetic(emitter, layout, opcode, 0);
	case 552:
		return NativeJitTranslateOverflowArithmetic(emitter, layout, opcode, 1);
	case 104:
		return NativeJitTranslateUnary(emitter, layout, opcode, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_OPCODE_RD(opcode), 0, 1);
	case 954:
		return NativeJitTranslateUnary(emitter, layout, opcode, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_OPCODE_RA(opcode), 8, 0);
	case 922:
		return NativeJitTranslateUnary(emitter, layout, opcode, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_OPCODE_RA(opcode), 16, 0);
	case 28:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 0, 0);
	case 60:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 1, 0);
	case 444:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 0, 0);
	case 412:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 1, 0);
	case 124:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 0, 1);
	case 316:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_XOR, 0, 0);
	case 284:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_XOR, 0, 1);
	case 476:
		return NativeJitTranslateLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 0, 1);
	case 24:
		return NativeJitTranslateShift(emitter, layout, opcode, NATIVEJIT_ALU_SHL);
	case 536:
		return NativeJitTranslateShift(emitter, layout, opcode, NATIVEJIT_ALU_SHR);
	case 824:
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitShiftRightAlgebraicCarrying(emitter, NATIVEJIT_T0, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
		NativeJitRecord(emitter, layout, opcode);
		return 1;
	case 26:
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitCountLeadingZeros(emitter, NATIVEJIT_T0);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
		NativeJitRecord(emitter, layout, opcode);
		return 1;
	case 792:
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RD(opcode));
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitShiftRightAlgebraicCarryingRegister(emitter, NATIVEJIT_T0, NATIVEJIT_T1);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RA(opcode), NATIVEJIT_T0);
		NativeJitRecord(emitter, layout, opcode);
		return 1;
	case 491:
	case 459:
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, NATIVEJIT_OPCODE_RA(opcode));
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T1, NATIVEJIT_OPCODE_RB(opcode));
		NativeJitDivide(emitter, NATIVEJIT_T0, NATIVEJIT_T1, extended == 491);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
		NativeJitRecord(emitter, layout, opcode);
		return 1;
	case 19:
		NativeJitLoadRegister(emitter, NATIVEJIT_T0, layout->mCr);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
		return 1;
	case 144:
		return NativeJitTranslateMoveToCr(emitter, layout, opcode);
	case 534:
		return NativeJitTranslateReversed(emitter, layout, opcode, 1, 4);
	case 662:
		return NativeJitTranslateReversed(emitter, layout, opcode, 0, 4);
	case 790:
		return NativeJitTranslateReversed(emitter, layout, opcode, 1, 2);
	case 918:
		return NativeJitTranslateReversed(emitter, layout, opcode, 0, 2);
	case 1014:
		return NativeJitTranslateZeroLine(emitter, layout, opcode);
	case 598:
	case 854:
	case 278:
	case 246:
	case 86:
	case 54:
	case 470:
		return 1;
	case 983:
		return NativeJitTranslateStoreFloatWord(emitter, layout, opcode);
	case 535:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 1, 0, 0);
	case 567:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 1, 0, 1);
	case 599:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 1, 1, 0);
	case 631:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 1, 1, 1);
	case 663:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 0, 0, 0);
	case 695:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 0, 0, 1);
	case 727:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 0, 1, 0);
	case 759:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 1, 0, 1, 1);
	case 339:
		return NativeJitTranslateSpecial(emitter, layout, opcode, 0);
	case 467:
		return NativeJitTranslateSpecial(emitter, layout, opcode, 1);
	case 23:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 4, 0, 0);
	case 55:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 4, 0, 1);
	case 87:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 1, 0, 0);
	case 119:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 1, 0, 1);
	case 279:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 2, 0, 0);
	case 311:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 2, 0, 1);
	case 343:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 2, 1, 0);
	case 375:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 1, 2, 1, 1);
	case 151:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 4, 0, 0);
	case 183:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 4, 0, 1);
	case 215:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 1, 0, 0);
	case 247:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 1, 0, 1);
	case 407:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 2, 0, 0);
	case 439:
		return NativeJitTranslateMemory(emitter, layout, opcode, 1, 0, 2, 0, 1);
	}
	return 0;
}

static int NativeJitStaticTargets(uint32 opcode, uint32 address, uint32* targets)
{
	uint32 primary;
	uint32 extended;
	uint32 target;
	int count;
	primary = opcode >> 26;
	count = 0;
	if (primary == 18)
	{
		target = ((opcode & 0x03fffffcU) ^ 0x02000000U) - 0x02000000U;
		if (!(opcode & 2))
			target += address;
		targets[count++] = target;
		return count;
	}
	if (primary == 16)
	{
		target = NATIVEJIT_OPCODE_SIMM(opcode) & ~3U;
		if (!(opcode & 2))
			target += address;
		targets[count++] = target;
		if ((NATIVEJIT_OPCODE_RD(opcode) & 20) != 20)
			targets[count++] = address + 4;
		return count;
	}
	extended = (opcode >> 1) & 0x3ff;
	if (primary == 19 && (extended == 16 || extended == 528) && (NATIVEJIT_OPCODE_RD(opcode) & 20) == 20)
		return 0;
	targets[count++] = address + 4;
	return count;
}

static int NativeJitTranslate(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 opcode, uint32 address,
	int fprflive, const void* decodeinfo)
{
	uint32 primary;
	uint32 source;
	uint32 target;
	primary = opcode >> 26;
	source = NATIVEJIT_OPCODE_RA(opcode);
	switch (primary)
	{
	case 31:
		return NativeJitTranslateExtended(emitter, layout, opcode);
	case 18:
		target = ((opcode & 0x03fffffcU) ^ 0x02000000U) - 0x02000000U;
		if (!(opcode & 2))
			target += address;
		if (opcode & 1)
			NativeJitStoreRegisterImmediate(emitter, layout->mLr, address + 4);
		NativeJitStoreRegisterImmediate(emitter, layout->mPc, target);
		return 2;
	case 16:
		return NativeJitTranslateBranch(emitter, layout, opcode, address, 0);
	case 19:
		switch ((opcode >> 1) & 0x3ff)
		{
		case 16:
			return NativeJitTranslateBranch(emitter, layout, opcode, address, layout->mLr);
		case 528:
			return NativeJitTranslateBranch(emitter, layout, opcode, address, layout->mCtr);
		case 0:
			return NativeJitTranslateConditionMove(emitter, layout, opcode);
		case 257:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 0, 0);
		case 129:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 1, 0);
		case 289:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_XOR, 0, 1);
		case 225:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_AND, 0, 1);
		case 33:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 0, 1);
		case 449:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 0, 0);
		case 417:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_OR, 1, 0);
		case 193:
			return NativeJitTranslateConditionLogical(emitter, layout, opcode, NATIVEJIT_ALU_XOR, 0, 0);
		}
		return 0;
	case 59:
		return NativeJitTranslateFloatArithmetic(emitter, layout, opcode, fprflive, address, decodeinfo);
	case 63:
		if (((opcode >> 1) & 31) >= 18)
			return NativeJitTranslateFloatArithmetic(emitter, layout, opcode, fprflive, address, decodeinfo);
		switch ((opcode >> 1) & 0x3ff)
		{
		case 72:
			return NativeJitTranslateFloatMove(emitter, layout, opcode, 0xffffffffU, 0, 0);
		case 40:
			return NativeJitTranslateFloatMove(emitter, layout, opcode, 0xffffffffU, 0, 0x80000000U);
		case 264:
			return NativeJitTranslateFloatMove(emitter, layout, opcode, 0x7fffffffU, 0, 0);
		case 136:
			return NativeJitTranslateFloatMove(emitter, layout, opcode, 0xffffffffU, 0x80000000U, 0);
		}
		return 0;
	case 46:
		return NativeJitTranslateMultiple(emitter, layout, opcode, 1);
	case 47:
		return NativeJitTranslateMultiple(emitter, layout, opcode, 0);
	case 48:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 1, 0, 0);
	case 49:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 1, 0, 1);
	case 50:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 1, 1, 0);
	case 51:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 1, 1, 1);
	case 52:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 0, 0, 0);
	case 53:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 0, 0, 1);
	case 54:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 0, 1, 0);
	case 55:
		return NativeJitTranslateFloatMemory(emitter, layout, opcode, 0, 0, 1, 1);
	case 14:
		if (source)
		{
			NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, source);
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T0, NATIVEJIT_OPCODE_SIMM(opcode));
		}
		else
			NativeJitLoadImmediate(emitter, NATIVEJIT_T0, NATIVEJIT_OPCODE_SIMM(opcode));
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
		return 1;
	case 15:
		if (source)
		{
			NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, source);
			NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_ADD, NATIVEJIT_T0, NATIVEJIT_OPCODE_UIMM(opcode) << 16);
		}
		else
			NativeJitLoadImmediate(emitter, NATIVEJIT_T0, NATIVEJIT_OPCODE_UIMM(opcode) << 16);
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
		return 1;
	case 12:
		return NativeJitTranslateCarrying(emitter, layout, opcode & ~1U, 0, 0, NATIVEJIT_OPCODE_SIMM(opcode), NATIVEJIT_CARRY_ZERO, 0);
	case 13:
		return NativeJitTranslateCarrying(emitter, layout, opcode | 1, 0, 0, NATIVEJIT_OPCODE_SIMM(opcode), NATIVEJIT_CARRY_ZERO, 0);
	case 8:
		return NativeJitTranslateCarrying(emitter, layout, opcode & ~1U, 1, 0, NATIVEJIT_OPCODE_SIMM(opcode), NATIVEJIT_CARRY_ONE, 0);
	case 7:
		NativeJitLoadGpr(emitter, layout, NATIVEJIT_T0, source);
		NativeJitOperateImmediate(emitter, NATIVEJIT_ALU_MUL, NATIVEJIT_T0, NATIVEJIT_OPCODE_SIMM(opcode));
		NativeJitStoreGpr(emitter, layout, NATIVEJIT_OPCODE_RD(opcode), NATIVEJIT_T0);
		return 1;
	case 24:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_OR, NATIVEJIT_OPCODE_UIMM(opcode), 0);
	case 25:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_OR, NATIVEJIT_OPCODE_UIMM(opcode) << 16, 0);
	case 26:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_XOR, NATIVEJIT_OPCODE_UIMM(opcode), 0);
	case 27:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_XOR, NATIVEJIT_OPCODE_UIMM(opcode) << 16, 0);
	case 28:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_AND, NATIVEJIT_OPCODE_UIMM(opcode), 1);
	case 29:
		return NativeJitTranslateLogicalImmediate(emitter, layout, opcode, NATIVEJIT_ALU_AND, NATIVEJIT_OPCODE_UIMM(opcode) << 16, 1);
	case 10:
		return NativeJitTranslateCompare(emitter, layout, opcode, 1, 0);
	case 11:
		return NativeJitTranslateCompare(emitter, layout, opcode, 1, 1);
	case 20:
	case 21:
	case 23:
		return NativeJitTranslateRotate(emitter, layout, opcode, primary);
	case 32:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 4, 0, 0);
	case 33:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 4, 0, 1);
	case 34:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 1, 0, 0);
	case 35:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 1, 0, 1);
	case 40:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 2, 0, 0);
	case 41:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 2, 0, 1);
	case 42:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 2, 1, 0);
	case 43:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 1, 2, 1, 1);
	case 36:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 4, 0, 0);
	case 37:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 4, 0, 1);
	case 38:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 1, 0, 0);
	case 39:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 1, 0, 1);
	case 44:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 2, 0, 0);
	case 45:
		return NativeJitTranslateMemory(emitter, layout, opcode, 0, 0, 2, 0, 1);
	}
	return 0;
}

void powerpc_cpu::NativeJitInterpret(void* cpu, const void* decodeinfo)
{
	const block_info::decode_info* info;
	info = (const block_info::decode_info*)decodeinfo;
	info->execute((powerpc_cpu*)cpu, info->opcode);
}

void powerpc_cpu::EnableNativeJit()
{
	uint8* code;
	code = NativeJitAllocate(NATIVEJIT_CODE_SIZE);
	if (code == NULL)
		return;
	nativejit = (NATIVEJITSTATE*)code;
	nativejit->mCode = code + NATIVEJIT_STATE_SPACE;
	nativejit->mFloat = NativeJitHasFloat() != 0;
#if PPC_NATIVEJIT_VERIFY
	nativeverifyblocks = 0;
	nativeverifyskipped = 0;
	nativeverifymismatches = 0;
	nativeverifyuntranslatedcount = 0;
#endif
	NativeJitReset();
}

#if PPC_NATIVEJIT_VERIFY
static int NativeJitVerifyPure(uint32 opcode)
{
	uint32 extended;
	uint32 special;
	extended = (opcode >> 1) & 0x3ff;
	special = NATIVEJIT_OPCODE_RA(opcode) | (NATIVEJIT_OPCODE_RB(opcode) << 5);
	if ((opcode >> 26) == 31)
	{
		switch (extended)
		{
		case 747: case 1003: case 971: case 83: case 20: case 150:
		case 533: case 597: case 661: case 725:
			return 1;
		case 339:
			return special != 22 && special != 268 && special != 269;
		}
		return 0;
	}
	if ((opcode >> 26) == 63)
	{
		switch (extended)
		{
		case 0: case 32: case 14: case 15: case 583: case 711: case 70: case 38: case 64:
			return 1;
		}
	}
	return 0;
}
#endif

static void NativeJitDirectExit(NATIVEJITEMITTER* emitter, const NATIVEJITLAYOUT* layout, uint32 target,
	uint32 blockpc, uint8* loopstart)
{
	uint8* site;
	NativeJitStoreRegisterImmediate(emitter, layout->mPc, target);
	site = NativeJitChainJump(emitter, target, layout->mFlags, layout->mState);
	if (target == blockpc && !PPC_NATIVEJIT_VERIFY)
		NativeJitChainLink(site, loopstart);
}

static powerpc_block_info::decode_info* NativeJitDecodeAllocate(NATIVEJITSTATE* state, NATIVEJITEMITTER* emitter,
	const powerpc_block_info::decode_info* source, int count)
{
	powerpc_block_info::decode_info* entries;
	uint8* top;
	int index;
	top = (uint8*)((uintptr)(state->mDecodeTop - count * sizeof(powerpc_block_info::decode_info)) & ~(uintptr)15);
	if (top < emitter->mCode + NATIVEJIT_BLOCK_RESERVE)
		return NULL;
	entries = (powerpc_block_info::decode_info*)top;
	for (index = 0; index < count; index++)
		entries[index] = source[index];
	state->mDecodeTop = top;
	emitter->mLimit = top;
	return entries;
}

static powerpc_block_info::decode_info* NativeJitDecodeEntry(NATIVEJITSTATE* state, NATIVEJITEMITTER* emitter,
	powerpc_block_info::decode_info* decode, const powerpc_block_info::decode_info* olddecode, int index)
{
	if (decode != olddecode)
		return &decode[index];
	return NativeJitDecodeAllocate(state, emitter, &olddecode[index], 1);
}

void* powerpc_cpu::NativeJitCompileBlock(block_info* bi)
{
	NATIVEJITEMITTER emitter;
	NATIVEJITLAYOUT layout;
	uint8* base;
	uint32 address;
	uint32 syncedpc;
	int index;
	int translated;
	uint8* loopstart;
	uint8* site;
	uint8* branch;
	uint32 targets[2];
	uint32 lastopcode;
	int exitcount;
	int nativecount;
	block_info::decode_info* olddecode;
	block_info::decode_info* decode;
	block_info::decode_info* decodeinfo;
	uint8* codestart;
#if PPC_NATIVEJIT_VERIFY
	int verifiable;
	verifiable = 1;
#endif
	base = (uint8*)regs_ptr();
	nativecount = 0;
	layout.mGpr = (int)((uint8*)&regs().gpr[0] - base);
	layout.mCr = (int)((uint8*)&regs().cr - base);
	layout.mLr = (int)((uint8*)&regs().lr - base);
	layout.mCtr = (int)((uint8*)&regs().ctr - base);
	layout.mPc = (int)((uint8*)&regs().pc - base);
	layout.mFlags = (int)((uint8*)&regs().spcflags - base);
	layout.mFpr = (int)((uint8*)&regs().fpr[0] - base);
	layout.mFprHigh = NativeJitFprWordOffset(1);
	layout.mFprLow = NativeJitFprWordOffset(0);
	layout.mSummaryOverflow = (int)((uint8*)&regs().xer - base) + NativeJitXerByteOffset(NATIVEJIT_XER_SO);
	layout.mOverflow = (int)((uint8*)&regs().xer - base) + NativeJitXerByteOffset(NATIVEJIT_XER_OV);
	layout.mCarry = (int)((uint8*)&regs().xer - base) + NativeJitXerByteOffset(NATIVEJIT_XER_CA);
	layout.mCount = (int)((uint8*)&regs().xer - base) + NativeJitXerByteOffset(NATIVEJIT_XER_COUNT);
	layout.mFpscr = (int)((uint8*)&regs().fpscr - base);
	layout.mFloat = nativejit->mFloat;
	layout.mState = nativejit;
	codestart = nativejit->mCursor;
	NativeJitBegin(&emitter, codestart, nativejit->mDecodeTop, layout.mCarry, layout.mSummaryOverflow);
	if (!NativeJitHasRoom(&emitter))
	{
		nativejit->mFull = 1;
		return NULL;
	}
	olddecode = bi->di;
	decode = olddecode;
#if PPC_NATIVEJIT_VERIFY
	decode = NativeJitDecodeAllocate(nativejit, &emitter, olddecode, bi->size);
	if (decode == NULL)
	{
		nativejit->mFull = 1;
		return NULL;
	}
#endif
	loopstart = NativeJitLabel(&emitter);
	translated = 0;
	syncedpc = bi->pc;
	address = bi->pc;
	for (index = 0; index < bi->size; index++)
	{
		if (!NativeJitHasRoom(&emitter))
		{
			nativejit->mFull = 1;
			return NULL;
		}
		decodeinfo = NULL;
		if (NativeJitIsFloatArithmetic(olddecode[index].opcode))
		{
			decodeinfo = NativeJitDecodeEntry(nativejit, &emitter, decode, olddecode, index);
			if (decodeinfo == NULL)
			{
				nativejit->mFull = 1;
				return NULL;
			}
		}
		translated = NativeJitTranslate(&emitter, &layout, olddecode[index].opcode, address, NativeJitFprfLive(bi, index),
			decodeinfo);
		if (translated)
			nativecount++;
		if (translated == 0)
		{
#if PPC_NATIVEJIT_VERIFY
			NativeJitVerifyNoteUntranslated(bi->di[index].opcode);
			if (!NativeJitVerifyPure(bi->di[index].opcode))
				verifiable = 0;
#endif
			if (decodeinfo == NULL)
				decodeinfo = NativeJitDecodeEntry(nativejit, &emitter, decode, olddecode, index);
			if (decodeinfo == NULL)
			{
				nativejit->mFull = 1;
				return NULL;
			}
			if (syncedpc != address)
				NativeJitStoreRegisterImmediate(&emitter, layout.mPc, address);
			NativeJitCallHelper(&emitter, NativeJitInterpret, decodeinfo);
			syncedpc = address + 4;
		}
		else if (translated == 4)
			syncedpc = 0xffffffffU;
		else if (translated >= 2)
			syncedpc = address + 4;
		address += 4;
	}
	if (nativecount == 0)
		return NULL;
	lastopcode = olddecode[bi->size - 1].opcode;
	exitcount = NativeJitStaticTargets(lastopcode, address - 4, targets);
	if (translated == 3)
	{
		branch = NativeJitBranchIfZero(&emitter, NATIVEJIT_T0);
		NativeJitDirectExit(&emitter, &layout, targets[0], bi->pc, loopstart);
		NativeJitBranchLand(&emitter, branch);
		NativeJitDirectExit(&emitter, &layout, targets[1], bi->pc, loopstart);
	}
	else if (translated && exitcount == 1 && (lastopcode >> 26) != 19)
	{
		if (syncedpc != address)
			NativeJitStoreRegisterImmediate(&emitter, layout.mPc, address);
		NativeJitDirectExit(&emitter, &layout, targets[0], bi->pc, loopstart);
	}
	else
	{
		if (syncedpc != address)
			NativeJitStoreRegisterImmediate(&emitter, layout.mPc, address);
		for (index = 0; index < exitcount; index++)
		{
			site = NativeJitChainExit(&emitter, layout.mPc, targets[index], layout.mFlags, nativejit);
			if (targets[index] == bi->pc && !PPC_NATIVEJIT_VERIFY)
				NativeJitChainLink(site, loopstart);
		}
		NativeJitJumpToCode(&emitter, nativejit->mRetired);
	}
	NativeJitFlush(codestart, emitter.mCode - codestart);
	nativejit->mPrologue = (int)(loopstart - codestart);
#if PPC_NATIVEJIT_VERIFY
	bi->nativecomplete = verifiable;
#else
	index = NATIVEJIT_LOOKUP_INDEX(bi->pc);
	nativejit->mLookupPc[index] = bi->pc;
	nativejit->mLookupEntry[index] = loopstart;
#endif
	bi->di = NULL;
	if (decode != olddecode)
		bi->di = decode;
	nativejit->mCursor = emitter.mCode;
	return codestart;
}

void powerpc_cpu::NativeJitLinkTo(block_info* bi)
{
	int index;
	if (nativejit == NULL)
		return;
	if (bi->nativeentry)
	{
		index = NATIVEJIT_LOOKUP_INDEX(bi->pc);
		nativejit->mLookupPc[index] = bi->pc;
		nativejit->mLookupEntry[index] = (uint8*)bi->nativeentry + nativejit->mPrologue;
	}
	if (nativejit->mChainSite == NULL)
		return;
	if (bi->nativeentry && bi->pc == nativejit->mChainPc)
	{
		NativeJitChainLink(nativejit->mChainSite, (uint8*)bi->nativeentry + nativejit->mPrologue);
		NativeJitFlush(nativejit->mChainSite, 5);
	}
	nativejit->mChainSite = NULL;
}

void powerpc_cpu::NativeJitReset()
{
	NATIVEJITEMITTER emitter;
	uint8* base;
	int index;
	if (nativejit == NULL)
		return;
	base = (uint8*)regs_ptr();
	NativeJitBegin(&emitter, nativejit->mCode, (uint8*)nativejit + NATIVEJIT_CODE_SIZE, 0, 0);
	NativeJitIndirectExit(&emitter, (int)((uint8*)&regs().pc - base), (int)((uint8*)&regs().spcflags - base), nativejit);
	NativeJitEpilogue(&emitter);
	nativejit->mReturnStub = emitter.mCode;
	NativeJitReturnStub(&emitter, (int)((uint8*)&regs().pc - base), nativejit);
	nativejit->mEntry = emitter.mCode;
	NativeJitEntryStub(&emitter);
	NativeJitFlush(nativejit->mCode, emitter.mCode - nativejit->mCode);
	nativejit->mRetired = nativejit->mCode;
	nativejit->mDecodeTop = (uint8*)nativejit + NATIVEJIT_CODE_SIZE;
	nativejit->mCursor = emitter.mCode;
	nativejit->mFull = 0;
	nativejit->mChainSite = NULL;
	for (index = 0; index < NATIVEJIT_LOOKUP_SIZE; index++)
	{
		nativejit->mLookupPc[index] = 1;
		nativejit->mLookupEntry[index] = NULL;
	}
}

void powerpc_cpu::NativeJitRetire(block_info* bi)
{
	uint8* loopstart;
	int index;
	if (nativejit == NULL || bi->nativeentry == NULL)
		return;
	index = NATIVEJIT_LOOKUP_INDEX(bi->pc);
	if (nativejit->mLookupPc[index] == bi->pc)
	{
		nativejit->mLookupPc[index] = 1;
		nativejit->mLookupEntry[index] = NULL;
	}
	nativejit->mChainSite = NULL;
	loopstart = (uint8*)bi->nativeentry + nativejit->mPrologue;
	NativeJitJumpTo(loopstart, nativejit->mRetired);
	NativeJitFlush(loopstart, 5);
	bi->nativeentry = NULL;
}

void powerpc_cpu::NativeJitRetireBlock(void* cpu, block_info* bi)
{
	((powerpc_cpu*)cpu)->NativeJitRetire(bi);
}

#if PPC_NATIVEJIT_VERIFY
#ifdef SHEEPSHAVER
extern bool PPCGuestAddressValid(uint32 addr, uint32 len);
extern bool MMIOIsWindow(void* host_addr);
#endif

#define NATIVEJITVERIFY_STORES 64
#define NATIVEJITVERIFY_BYTES 4096
#define NATIVEJITVERIFY_REPORTS 64
#define NATIVEJITVERIFY_LOAD 1
#define NATIVEJITVERIFY_STORE 2
#define NATIVEJITVERIFY_FPRF 0x0001f000U

typedef struct NATIVEJITVERIFYSTATE
{
	uint32 mGpr[32];
	uint64 mFpr[32];
	uint32 mCr;
	uint32 mXer;
	uint32 mFpscr;
	uint32 mLr;
	uint32 mCtr;
	uint32 mPc;
	uint32 mReserveValid;
	uint32 mReserveAddress;
} NATIVEJITVERIFYSTATE;

typedef struct NATIVEJITVERIFYSTORE
{
	uint32 mAddress;
	uint32 mSize;
	uint32 mOffset;
} NATIVEJITVERIFYSTORE;

static void NativeJitVerifyCapture(NATIVEJITVERIFYSTATE* state, powerpc_registers* registers)
{
	int index;
	for (index = 0; index < 32; index++)
	{
		state->mGpr[index] = registers->gpr[index];
		state->mFpr[index] = registers->fpr[index].j;
	}
	state->mCr = registers->cr.get();
	state->mXer = registers->xer.get();
	state->mFpscr = registers->fpscr;
	state->mLr = registers->lr;
	state->mCtr = registers->ctr;
	state->mPc = registers->pc;
	state->mReserveValid = registers->reserve_valid;
	state->mReserveAddress = registers->reserve_addr;
}

static void NativeJitVerifyRestore(const NATIVEJITVERIFYSTATE* state, powerpc_registers* registers)
{
	int index;
	for (index = 0; index < 32; index++)
	{
		registers->gpr[index] = state->mGpr[index];
		registers->fpr[index].j = state->mFpr[index];
	}
	registers->cr.set(state->mCr);
	registers->xer.set(state->mXer);
	registers->fpscr = state->mFpscr;
	registers->lr = state->mLr;
	registers->ctr = state->mCtr;
	registers->pc = state->mPc;
	registers->reserve_valid = state->mReserveValid;
	registers->reserve_addr = state->mReserveAddress;
}

static int NativeJitVerifyExtendedAccess(uint32 extended, uint32* size)
{
	switch (extended)
	{
	case 23: case 55: case 534: case 535: case 567: case 20:
		*size = 4;
		return NATIVEJITVERIFY_LOAD;
	case 150:
		*size = 4;
		return NATIVEJITVERIFY_STORE;
	case 87: case 119:
		*size = 1;
		return NATIVEJITVERIFY_LOAD;
	case 279: case 311: case 343: case 375: case 790:
		*size = 2;
		return NATIVEJITVERIFY_LOAD;
	case 599: case 631:
		*size = 8;
		return NATIVEJITVERIFY_LOAD;
	case 151: case 183: case 662: case 663: case 695: case 983:
		*size = 4;
		return NATIVEJITVERIFY_STORE;
	case 215: case 247:
		*size = 1;
		return NATIVEJITVERIFY_STORE;
	case 407: case 439: case 918:
		*size = 2;
		return NATIVEJITVERIFY_STORE;
	case 727: case 759:
		*size = 8;
		return NATIVEJITVERIFY_STORE;
	case 1014:
		*size = 32;
		return NATIVEJITVERIFY_STORE;
	}
	return 0;
}

static int NativeJitVerifyPrimaryAccess(uint32 opcode, uint32* size)
{
	switch (opcode >> 26)
	{
	case 32: case 33: case 48: case 49:
		*size = 4;
		return NATIVEJITVERIFY_LOAD;
	case 34: case 35:
		*size = 1;
		return NATIVEJITVERIFY_LOAD;
	case 40: case 41: case 42: case 43:
		*size = 2;
		return NATIVEJITVERIFY_LOAD;
	case 50: case 51:
		*size = 8;
		return NATIVEJITVERIFY_LOAD;
	case 46:
		*size = 4 * (32 - NATIVEJIT_OPCODE_RD(opcode));
		return NATIVEJITVERIFY_LOAD;
	case 36: case 37: case 52: case 53:
		*size = 4;
		return NATIVEJITVERIFY_STORE;
	case 38: case 39:
		*size = 1;
		return NATIVEJITVERIFY_STORE;
	case 44: case 45:
		*size = 2;
		return NATIVEJITVERIFY_STORE;
	case 54: case 55:
		*size = 8;
		return NATIVEJITVERIFY_STORE;
	case 47:
		*size = 4 * (32 - NATIVEJIT_OPCODE_RD(opcode));
		return NATIVEJITVERIFY_STORE;
	}
	return 0;
}

static int NativeJitVerifyAccess(const powerpc_registers* registers, uint32 opcode, uint32* address, uint32* size)
{
	uint32 base;
	int kind;
	base = 0;
	if (NATIVEJIT_OPCODE_RA(opcode))
		base = registers->gpr[NATIVEJIT_OPCODE_RA(opcode)];
	if ((opcode >> 26) == 31)
	{
		switch ((opcode >> 1) & 0x3ff)
		{
		case 597: case 725:
			*address = base;
			*size = NATIVEJIT_OPCODE_RB(opcode);
			if (*size == 0)
				*size = 32;
			if (((opcode >> 1) & 0x3ff) == 597)
				return NATIVEJITVERIFY_LOAD;
			return NATIVEJITVERIFY_STORE;
		case 533: case 661:
			*address = base + registers->gpr[NATIVEJIT_OPCODE_RB(opcode)];
			*size = registers->xer.get() & 0x7f;
			if (*size == 0)
				return 0;
			if (((opcode >> 1) & 0x3ff) == 533)
				return NATIVEJITVERIFY_LOAD;
			return NATIVEJITVERIFY_STORE;
		}
		kind = NativeJitVerifyExtendedAccess((opcode >> 1) & 0x3ff, size);
		*address = base + registers->gpr[NATIVEJIT_OPCODE_RB(opcode)];
		if (((opcode >> 1) & 0x3ff) == 1014)
			*address &= ~31U;
		return kind;
	}
	kind = NativeJitVerifyPrimaryAccess(opcode, size);
	*address = base + NATIVEJIT_OPCODE_SIMM(opcode);
	return kind;
}

static int NativeJitVerifyUsable(uint32 address, uint32 size)
{
	if (address + size < address)
		return 0;
#ifdef SHEEPSHAVER
	if (!PPCGuestAddressValid(address, size))
		return 0;
	if (MMIOIsWindow(vm_do_get_real_address(address)) || MMIOIsWindow(vm_do_get_real_address(address + size - 1)))
		return 0;
#endif
	return 1;
}

static void NativeJitVerifyReportValue(const char* name, int index, uint64 before, uint64 interpreted, uint64 native)
{
	if (interpreted == native)
		return;
	fprintf(stderr, "  %s%d before %016llx interpreter %016llx native %016llx\n", name, index,
		(unsigned long long)before, (unsigned long long)interpreted, (unsigned long long)native);
}

static int NativeJitVerifyCompare(const NATIVEJITVERIFYSTATE* interpreted, const NATIVEJITVERIFYSTATE* native)
{
	int index;
	for (index = 0; index < 32; index++)
	{
		if (interpreted->mGpr[index] != native->mGpr[index] || interpreted->mFpr[index] != native->mFpr[index])
			return 1;
	}
	if (interpreted->mCr != native->mCr || interpreted->mXer != native->mXer)
		return 1;
	if ((interpreted->mFpscr & ~NATIVEJITVERIFY_FPRF) != (native->mFpscr & ~NATIVEJITVERIFY_FPRF))
		return 1;
	return interpreted->mLr != native->mLr || interpreted->mCtr != native->mCtr || interpreted->mPc != native->mPc;
}

static void NativeJitVerifyReport(powerpc_block_info* bi, const NATIVEJITVERIFYSTATE* before,
	const NATIVEJITVERIFYSTATE* interpreted, const NATIVEJITVERIFYSTATE* native)
{
	int index;
	fprintf(stderr, "NATIVEJITVERIFY mismatch block %08x size %d\n", (unsigned)bi->pc, (int)bi->size);
	for (index = 0; index < bi->size; index++)
		fprintf(stderr, "  %08x: %08x\n", (unsigned)(bi->pc + 4 * index), (unsigned)bi->di[index].opcode);
	for (index = 0; index < 32; index++)
	{
		NativeJitVerifyReportValue("r", index, before->mGpr[index], interpreted->mGpr[index], native->mGpr[index]);
		NativeJitVerifyReportValue("f", index, before->mFpr[index], interpreted->mFpr[index], native->mFpr[index]);
	}
	NativeJitVerifyReportValue("cr", 0, before->mCr, interpreted->mCr, native->mCr);
	NativeJitVerifyReportValue("xer", 0, before->mXer, interpreted->mXer, native->mXer);
	NativeJitVerifyReportValue("fpscr", 0, before->mFpscr, interpreted->mFpscr, native->mFpscr);
	NativeJitVerifyReportValue("lr", 0, before->mLr, interpreted->mLr, native->mLr);
	NativeJitVerifyReportValue("ctr", 0, before->mCtr, interpreted->mCtr, native->mCtr);
	NativeJitVerifyReportValue("pc", 0, before->mPc, interpreted->mPc, native->mPc);
	fprintf(stderr, "  inputs");
	for (index = 0; index < 32; index++)
		fprintf(stderr, " %08x", (unsigned)before->mGpr[index]);
	fprintf(stderr, " cr %08x xer %08x\n", (unsigned)before->mCr, (unsigned)before->mXer);
}

void powerpc_cpu::NativeJitVerifyNoteUntranslated(uint32 opcode)
{
	uint32 key;
	int index;
	key = opcode & 0xfc000000U;
	if ((opcode >> 26) == 31 || (opcode >> 26) == 19 || (opcode >> 26) == 63 || (opcode >> 26) == 59)
		key |= opcode & 0x7feU;
	for (index = 0; index < nativeverifyuntranslatedcount; index++)
	{
		if (nativeverifyuntranslated[index] == key)
			return;
	}
	if (nativeverifyuntranslatedcount == 256)
		return;
	nativeverifyuntranslated[nativeverifyuntranslatedcount++] = key;
	fprintf(stderr, "NATIVEJITVERIFY untranslated primary %u extended %u first %08x\n",
		(unsigned)(opcode >> 26), (unsigned)((key >> 1) & 0x3ff), (unsigned)opcode);
	fflush(stderr);
}

void powerpc_cpu::NativeJitVerify(block_info* bi)
{
	NATIVEJITVERIFYSTATE before;
	NATIVEJITVERIFYSTATE interpreted;
	NATIVEJITVERIFYSTATE native;
	NATIVEJITVERIFYSTORE stores[NATIVEJITVERIFY_STORES];
	uint8 original[NATIVEJITVERIFY_BYTES];
	uint8 expected[NATIVEJITVERIFY_BYTES];
	powerpc_registers* registers;
	uint32 address;
	uint32 size;
	uint32 used;
	int storecount;
	int usable;
	int kind;
	int index;
	int mismatch;
	registers = &regs();
	if (!bi->nativecomplete)
	{
		((NATIVEJITENTRY)nativejit->mEntry)(this, regs_ptr(), (void*)VMBaseDiff, bi->nativeentry);
		return;
	}
	NativeJitVerifyCapture(&before, registers);
	storecount = 0;
	used = 0;
	usable = 1;
	for (index = 0; index < bi->size && usable; index++)
	{
		kind = NativeJitVerifyAccess(registers, bi->di[index].opcode, &address, &size);
		if (kind && !NativeJitVerifyUsable(address, size))
			usable = 0;
		else if (kind == NATIVEJITVERIFY_STORE)
		{
			if (storecount == NATIVEJITVERIFY_STORES || used + size > NATIVEJITVERIFY_BYTES)
				usable = 0;
			else
			{
				stores[storecount].mAddress = address;
				stores[storecount].mSize = size;
				stores[storecount].mOffset = used;
				memcpy(original + used, vm_do_get_real_address(address), size);
				used += size;
				storecount++;
			}
		}
		if (usable)
			bi->di[index].execute(this, bi->di[index].opcode);
	}
	if (usable)
	{
		NativeJitVerifyCapture(&interpreted, registers);
		for (index = 0; index < storecount; index++)
			memcpy(expected + stores[index].mOffset, vm_do_get_real_address(stores[index].mAddress), stores[index].mSize);
	}
	for (index = storecount - 1; index >= 0; index--)
	{
		if (memcmp(vm_do_get_real_address(stores[index].mAddress), original + stores[index].mOffset, stores[index].mSize))
			memcpy(vm_do_get_real_address(stores[index].mAddress), original + stores[index].mOffset, stores[index].mSize);
	}
	NativeJitVerifyRestore(&before, registers);
	((NATIVEJITENTRY)nativejit->mEntry)(this, regs_ptr(), (void*)VMBaseDiff, bi->nativeentry);
	if (!usable)
	{
		nativeverifyskipped++;
		return;
	}
	nativeverifyblocks++;
	NativeJitVerifyCapture(&native, registers);
	mismatch = NativeJitVerifyCompare(&interpreted, &native);
	for (index = 0; index < storecount; index++)
	{
		if (memcmp(vm_do_get_real_address(stores[index].mAddress), expected + stores[index].mOffset, stores[index].mSize))
			mismatch = 1;
	}
	if (mismatch)
	{
		nativeverifymismatches++;
		if (nativeverifymismatches <= NATIVEJITVERIFY_REPORTS)
		{
			NativeJitVerifyReport(bi, &before, &interpreted, &native);
			for (index = 0; index < storecount; index++)
			{
				if (memcmp(vm_do_get_real_address(stores[index].mAddress), expected + stores[index].mOffset, stores[index].mSize))
					fprintf(stderr, "  store %08x size %u differs\n", (unsigned)stores[index].mAddress, (unsigned)stores[index].mSize);
			}
			fflush(stderr);
		}
	}
	if ((nativeverifyblocks & 0x3fffff) == 0)
	{
		fprintf(stderr, "NATIVEJITVERIFY verified %llu skipped %llu mismatches %llu\n",
			(unsigned long long)nativeverifyblocks, (unsigned long long)nativeverifyskipped,
			(unsigned long long)nativeverifymismatches);
		fflush(stderr);
	}
}
#endif

#endif
