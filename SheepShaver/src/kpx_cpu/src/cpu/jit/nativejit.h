#ifndef NATIVEJIT_H
#define NATIVEJIT_H

#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__)
#define NATIVEJIT_HOST_X86_64 1
#elif defined(_M_IX86) || defined(__i386__) || defined(__i386)
#define NATIVEJIT_HOST_X86_32 1
#endif

#if defined(NATIVEJIT_HOST_X86_64) || defined(NATIVEJIT_HOST_X86_32)
#define NATIVEJIT_AVAILABLE 1
#else
#define NATIVEJIT_AVAILABLE 0
#endif

#define NATIVEJIT_T0 0
#define NATIVEJIT_T1 1
#define NATIVEJIT_T2 2

#define NATIVEJIT_ALU_ADD 0
#define NATIVEJIT_ALU_SUB 1
#define NATIVEJIT_ALU_AND 2
#define NATIVEJIT_ALU_OR 3
#define NATIVEJIT_ALU_XOR 4
#define NATIVEJIT_ALU_MUL 5
#define NATIVEJIT_ALU_MULHS 6
#define NATIVEJIT_ALU_MULHU 7
#define NATIVEJIT_ALU_SHL 8
#define NATIVEJIT_ALU_SHR 9
#define NATIVEJIT_ALU_SAR 10
#define NATIVEJIT_ALU_ROL 11

#define NATIVEJIT_CARRY_ZERO 0
#define NATIVEJIT_CARRY_ONE 1
#define NATIVEJIT_CARRY_XER 2

#define NATIVEJIT_F0 0
#define NATIVEJIT_F1 1

#define NATIVEJIT_FPU_ADD 0
#define NATIVEJIT_FPU_SUB 1
#define NATIVEJIT_FPU_MUL 2
#define NATIVEJIT_FPU_DIV 3

#define NATIVEJIT_LOOKUP_SIZE 4096
#define NATIVEJIT_LOOKUP_MASK (NATIVEJIT_LOOKUP_SIZE - 1)

#define NATIVEJIT_CODE_SIZE (8 * 1024 * 1024)
#define NATIVEJIT_BLOCK_RESERVE 4096

typedef void (*NATIVEJITBLOCK)(void* cpu, void* regs, void* membase);
typedef void (*NATIVEJITHELPER)(void* cpu, const void* argument);

typedef struct NATIVEJITSTATE
{
	uint32 mLookupPc[NATIVEJIT_LOOKUP_SIZE];
	void* mLookupEntry[NATIVEJIT_LOOKUP_SIZE];
	uint8* mCode;
	uint8* mCursor;
	uint8* mChainSite;
	uint32 mChainPc;
	uint32 mLow;
	uint32 mHigh;
	int mPrologue;
	int mFull;
	int mFloat;
} NATIVEJITSTATE;

#define NATIVEJIT_STATE_SPACE ((sizeof(NATIVEJITSTATE) + 4095) & ~(size_t)4095)

typedef struct NATIVEJITEMITTER
{
	uint8* mStart;
	uint8* mCode;
	uint8* mLimit;
	int mCarryOffset;
	int mSummaryOverflowOffset;
} NATIVEJITEMITTER;

uint8* NativeJitAllocate(size_t size);
void NativeJitFree(uint8* code, size_t size);
void NativeJitFlush(uint8* start, size_t length);

void NativeJitBegin(NATIVEJITEMITTER* pThis, uint8* start, uint8* limit,
	int carryoffset, int summaryoverflowoffset);
int NativeJitHasRoom(const NATIVEJITEMITTER* pThis);
void NativeJitPrologue(NATIVEJITEMITTER* pThis);
void NativeJitEpilogue(NATIVEJITEMITTER* pThis);

void NativeJitLoadImmediate(NATIVEJITEMITTER* pThis, int target, uint32 value);
void NativeJitMove(NATIVEJITEMITTER* pThis, int target, int source);
void NativeJitLoadRegister(NATIVEJITEMITTER* pThis, int target, int offset);
void NativeJitStoreRegister(NATIVEJITEMITTER* pThis, int offset, int source);
void NativeJitStoreRegisterImmediate(NATIVEJITEMITTER* pThis, int offset, uint32 value);
void NativeJitLoadRegisterByte(NATIVEJITEMITTER* pThis, int target, int offset);
void NativeJitStoreRegisterByte(NATIVEJITEMITTER* pThis, int offset, int source);

void NativeJitOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source);
void NativeJitOperateImmediate(NATIVEJITEMITTER* pThis, int operation, int target, uint32 value);
void NativeJitNot(NATIVEJITEMITTER* pThis, int target);
void NativeJitNegate(NATIVEJITEMITTER* pThis, int target);
void NativeJitSignExtend(NATIVEJITEMITTER* pThis, int target, int bits);
void NativeJitAddCarrying(NATIVEJITEMITTER* pThis, int target, int source, int carryin);
void NativeJitShiftRightAlgebraicCarrying(NATIVEJITEMITTER* pThis, int target, int amount);
void NativeJitShiftRightAlgebraicCarryingRegister(NATIVEJITEMITTER* pThis, int target, int amount);
void NativeJitCountLeadingZeros(NATIVEJITEMITTER* pThis, int target);
void NativeJitDivide(NATIVEJITEMITTER* pThis, int target, int source, int issigned);
void NativeJitCompare(NATIVEJITEMITTER* pThis, int target, int left, int right, int issigned);
void NativeJitCompareImmediate(NATIVEJITEMITTER* pThis, int target, int left, uint32 value, int issigned);
void NativeJitSelect(NATIVEJITEMITTER* pThis, int target, int condition, int whentrue, int whenfalse);

void NativeJitLoadMemory(NATIVEJITEMITTER* pThis, int target, int address, int size, int issigned);
void NativeJitStoreMemory(NATIVEJITEMITTER* pThis, int address, int source, int size);
void NativeJitLoadMemoryReversed(NATIVEJITEMITTER* pThis, int target, int address, int size);
void NativeJitStoreMemoryReversed(NATIVEJITEMITTER* pThis, int address, int source, int size);
void NativeJitLoadMemoryDouble(NATIVEJITEMITTER* pThis, int offset, int address);
void NativeJitStoreMemoryDouble(NATIVEJITEMITTER* pThis, int address, int offset);
void NativeJitSingleToDouble(NATIVEJITEMITTER* pThis, int offset, int source);
void NativeJitDoubleToSingle(NATIVEJITEMITTER* pThis, int target, int offset);

int NativeJitHasFloat(void);
void NativeJitFloatLoad(NATIVEJITEMITTER* pThis, int target, int offset);
void NativeJitFloatStore(NATIVEJITEMITTER* pThis, int offset, int source);
void NativeJitFloatOperate(NATIVEJITEMITTER* pThis, int operation, int target, int source);
void NativeJitFloatRoundSingle(NATIVEJITEMITTER* pThis, int target);
void NativeJitFloatNegate(NATIVEJITEMITTER* pThis, int target);
void NativeJitFloatClass(NATIVEJITEMITTER* pThis, int target, int offset, uint32 lowexponent);

uint8* NativeJitLabel(NATIVEJITEMITTER* pThis);
uint8* NativeJitBranchIfZero(NATIVEJITEMITTER* pThis, int condition);
uint8* NativeJitFloatBranchIfNaN(NATIVEJITEMITTER* pThis, int source);
uint8* NativeJitJump(NATIVEJITEMITTER* pThis);
void NativeJitBranchLand(NATIVEJITEMITTER* pThis, uint8* branch);
uint8* NativeJitChainExit(NATIVEJITEMITTER* pThis, int pcoffset, uint32 pcvalue, int flagsoffset,
	NATIVEJITSTATE* state);
uint8* NativeJitChainJump(NATIVEJITEMITTER* pThis, uint32 pcvalue, int flagsoffset, NATIVEJITSTATE* state);
void NativeJitIndirectExit(NATIVEJITEMITTER* pThis, int pcoffset, int flagsoffset, NATIVEJITSTATE* state);
void NativeJitChainLink(uint8* site, uint8* target);

void NativeJitCallHelper(NATIVEJITEMITTER* pThis, NATIVEJITHELPER helper, const void* argument);

#endif
