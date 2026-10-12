#include "sysdeps.h"
#include <math.h>
#include <fenv.h>
#include "osxcpu.h"

#pragma fenv_access (on)

#define FPSCRFX 0x80000000u
#define FPSCRFEX 0x40000000u
#define FPSCRVX 0x20000000u
#define FPSCROX 0x10000000u
#define FPSCRUX 0x08000000u
#define FPSCRZX 0x04000000u
#define FPSCRXX 0x02000000u
#define FPSCRVXSNAN 0x01000000u
#define FPSCRVXISI 0x00800000u
#define FPSCRVXIDI 0x00400000u
#define FPSCRVXZDZ 0x00200000u
#define FPSCRVXIMZ 0x00100000u
#define FPSCRVXVC 0x00080000u
#define FPSCRFPRF 0x0001f000u
#define FPSCRVXSOFT 0x00000400u
#define FPSCRVXSQRT 0x00000200u
#define FPSCRVXCVI 0x00000100u
#define FPSCRVXALL 0x01f80700u
#define FPSCRSTICKY 0x9ff80700u
#define FPSCRENABLES 0x000000f8u

#define FLOATEXPONENT 0x7ff0000000000000ULL
#define FLOATFRACTION 0x000fffffffffffffULL
#define FLOATQUIET 0x0008000000000000ULL
#define FLOATSIGN 0x8000000000000000ULL
#define FLOATDEFAULTNAN 0x7ff8000000000000ULL

static inline int IsNan(uint64 bits)
{
	return (bits & FLOATEXPONENT) == FLOATEXPONENT && (bits & FLOATFRACTION) != 0;
}

static inline int IsSignalingNan(uint64 bits)
{
	return IsNan(bits) && !(bits & FLOATQUIET);
}

static inline int IsInfinity(uint64 bits)
{
	return (bits & ~FLOATSIGN) == FLOATEXPONENT;
}

static inline int IsZero(uint64 bits)
{
	return (bits & ~FLOATSIGN) == 0;
}

static inline double BitsToDouble(uint64 bits)
{
	OsxFloat value;
	value.bits = bits;
	return value.value;
}

static inline uint64 DoubleToBits(double number)
{
	OsxFloat value;
	value.value = number;
	return value.bits;
}

static uint32 ClassifyResult(uint64 bits, int single)
{
	uint32 exponent;
	int negative;
	negative = (bits & FLOATSIGN) != 0;
	exponent = (uint32)(bits >> 52) & 0x7ff;
	if (single && exponent != 0 && exponent < 897) {
		if (negative)
			return 0x18;
		return 0x14;
	}
	if (exponent == 0x7ff) {
		if (bits & FLOATFRACTION)
			return 0x11;
		if (negative)
			return 0x09;
		return 0x05;
	}
	if (exponent == 0) {
		if ((bits & FLOATFRACTION) == 0) {
			if (negative)
				return 0x12;
			return 0x02;
		}
		if (negative)
			return 0x18;
		return 0x14;
	}
	if (negative)
		return 0x08;
	return 0x04;
}

static void UpdateSummary(OsxCpu *cpu)
{
	uint32 fpscr;
	fpscr = cpu->fpscr;
	if (fpscr & FPSCRVXALL)
		fpscr |= FPSCRVX;
	else
		fpscr &= ~FPSCRVX;
	if ((((fpscr >> 22) & 0x7c) | ((fpscr >> 29) & 0x80)) & fpscr & FPSCRENABLES)
		fpscr |= FPSCRFEX;
	else
		fpscr &= ~FPSCRFEX;
	cpu->fpscr = fpscr;
}

static void RaiseFloatFlags(OsxCpu *cpu, uint32 flags)
{
	uint32 fresh;
	fresh = flags & ~cpu->fpscr;
	if (fresh)
		cpu->fpscr |= flags | FPSCRFX;
	UpdateSummary(cpu);
}

static uint64 PropagateNan(OsxCpu *cpu, uint64 first, uint64 second, uint64 third, int count)
{
	uint32 flags;
	flags = 0;
	if (IsSignalingNan(first) || IsSignalingNan(second) || (count > 2 && IsSignalingNan(third)))
		flags |= FPSCRVXSNAN;
	if (flags)
		RaiseFloatFlags(cpu, flags);
	if (IsNan(first))
		return first | FLOATQUIET;
	if (IsNan(second))
		return second | FLOATQUIET;
	return third | FLOATQUIET;
}

static uint64 InvalidResult(OsxCpu *cpu, uint32 reason)
{
	RaiseFloatFlags(cpu, reason);
	return FLOATDEFAULTNAN;
}

static uint64 RoundSingle(uint64 bits)
{
	float narrow;
	if (IsNan(bits))
		return bits & 0xffffffffe0000000ULL;
	narrow = (float)BitsToDouble(bits);
	return DoubleToBits((double)narrow);
}

static void FinishResult(OsxCpu *cpu, int target, uint64 bits, int single, int inputsfinite)
{
	if (single)
		bits = RoundSingle(bits);
	if (inputsfinite && IsInfinity(bits))
		RaiseFloatFlags(cpu, FPSCROX | FPSCRXX);
	cpu->fpr[target].bits = bits;
	cpu->fpscr = (cpu->fpscr & ~FPSCRFPRF) | (ClassifyResult(bits, single) << 12);
}

void OsxCpuSetFloatCompare(OsxCpu *cpu, int field, double left, double right, int ordered)
{
	uint32 value;
	uint64 leftbits;
	uint64 rightbits;
	int shift;
	leftbits = DoubleToBits(left);
	rightbits = DoubleToBits(right);
	if (IsNan(leftbits) || IsNan(rightbits)) {
		value = 1;
		if (IsSignalingNan(leftbits) || IsSignalingNan(rightbits)) {
			if (ordered)
				RaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXVC);
			else
				RaiseFloatFlags(cpu, FPSCRVXSNAN);
		} else if (ordered) {
			RaiseFloatFlags(cpu, FPSCRVXVC);
		}
	} else if (left < right) {
		value = 8;
	} else if (left > right) {
		value = 4;
	} else {
		value = 2;
	}
	cpu->fpscr = (cpu->fpscr & ~0x0000f000u) | (value << 12);
	shift = (7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | (value << shift);
}

static uint64 ConvertToInteger(OsxCpu *cpu, uint64 bits, int truncate)
{
	double value;
	double rounded;
	uint32 mode;
	int32 integer;
	if (IsNan(bits)) {
		if (IsSignalingNan(bits))
			RaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXCVI);
		else
			RaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	value = BitsToDouble(bits);
	if (truncate)
		mode = 1;
	else
		mode = cpu->fpscr & 3;
	if (mode == 0)
		rounded = rint(value);
	else if (mode == 1)
		rounded = trunc(value);
	else if (mode == 2)
		rounded = ceil(value);
	else
		rounded = floor(value);
	if (rounded > 2147483647.0) {
		RaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff800007fffffffULL;
	}
	if (rounded < -2147483648.0) {
		RaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	if (rounded != value)
		cpu->fpscr |= FPSCRXX | FPSCRFX;
	integer = (int32)rounded;
	return 0xfff8000000000000ULL | (uint32)integer;
}

static void MoveToFpscrField(OsxCpu *cpu, int field, uint32 value)
{
	int shift;
	uint32 mask;
	shift = (7 - field) * 4;
	mask = 0xfu << shift;
	if (field == 0)
		mask &= ~(FPSCRFEX | FPSCRVX);
	cpu->fpscr = (cpu->fpscr & ~mask) | ((value << shift) & mask);
}

static void ExecuteArithmetic(OsxCpu *cpu, uint32 instruction, int single)
{
	uint32 extended;
	int target;
	uint64 a;
	uint64 b;
	uint64 c;
	uint64 result;
	double left;
	double right;
	double multiplier;
	int finite;
	target = (instruction >> 21) & 31;
	a = cpu->fpr[(instruction >> 16) & 31].bits;
	b = cpu->fpr[(instruction >> 11) & 31].bits;
	c = cpu->fpr[(instruction >> 6) & 31].bits;
	extended = (instruction >> 1) & 31;
	left = BitsToDouble(a);
	right = BitsToDouble(b);
	multiplier = BitsToDouble(c);
	switch (extended) {
	case 18:
		finite = !IsInfinity(a) && !IsInfinity(b) && !IsZero(b);
		if (IsNan(a) || IsNan(b))
			result = PropagateNan(cpu, a, b, 0, 2);
		else if (IsZero(a) && IsZero(b))
			result = InvalidResult(cpu, FPSCRVXZDZ);
		else if (IsInfinity(a) && IsInfinity(b))
			result = InvalidResult(cpu, FPSCRVXIDI);
		else {
			if (IsZero(b))
				RaiseFloatFlags(cpu, FPSCRZX);
			result = DoubleToBits(left / right);
		}
		break;
	case 20:
		finite = !IsInfinity(a) && !IsInfinity(b);
		if (IsNan(a) || IsNan(b))
			result = PropagateNan(cpu, a, b, 0, 2);
		else if (IsInfinity(a) && IsInfinity(b) && ((a ^ b) & FLOATSIGN) == 0)
			result = InvalidResult(cpu, FPSCRVXISI);
		else
			result = DoubleToBits(left - right);
		break;
	case 21:
		finite = !IsInfinity(a) && !IsInfinity(b);
		if (IsNan(a) || IsNan(b))
			result = PropagateNan(cpu, a, b, 0, 2);
		else if (IsInfinity(a) && IsInfinity(b) && ((a ^ b) & FLOATSIGN) != 0)
			result = InvalidResult(cpu, FPSCRVXISI);
		else
			result = DoubleToBits(left + right);
		break;
	case 22:
		finite = !IsInfinity(b);
		if (IsNan(b))
			result = PropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !IsZero(b))
			result = InvalidResult(cpu, FPSCRVXSQRT);
		else
			result = DoubleToBits(sqrt(right));
		break;
	case 23:
		finite = 0;
		if (!IsNan(a) && left >= 0.0)
			result = c;
		else
			result = b;
		cpu->fpr[target].bits = result;
		if (instruction & 1)
			cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
		return;
	case 24:
		finite = 0;
		if (IsNan(b))
			result = PropagateNan(cpu, b, 0, 0, 1);
		else {
			if (IsZero(b))
				RaiseFloatFlags(cpu, FPSCRZX);
			result = DoubleToBits(1.0 / right);
		}
		single = 1;
		break;
	case 25:
		finite = !IsInfinity(a) && !IsInfinity(c);
		if (IsNan(a) || IsNan(c))
			result = PropagateNan(cpu, a, c, 0, 2);
		else if ((IsInfinity(a) && IsZero(c)) || (IsZero(a) && IsInfinity(c)))
			result = InvalidResult(cpu, FPSCRVXIMZ);
		else
			result = DoubleToBits(left * multiplier);
		break;
	case 26:
		finite = 0;
		if (IsNan(b))
			result = PropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !IsZero(b))
			result = InvalidResult(cpu, FPSCRVXSQRT);
		else {
			if (IsZero(b))
				RaiseFloatFlags(cpu, FPSCRZX);
			result = DoubleToBits(1.0 / sqrt(right));
		}
		break;
	case 28:
	case 29:
	case 30:
	case 31:
		finite = !IsInfinity(a) && !IsInfinity(b) && !IsInfinity(c);
		if (IsNan(a) || IsNan(b) || IsNan(c)) {
			result = PropagateNan(cpu, a, b, c, 3);
		} else if ((IsInfinity(a) && IsZero(c)) || (IsZero(a) && IsInfinity(c))) {
			result = InvalidResult(cpu, FPSCRVXIMZ);
		} else {
			if (extended == 28 || extended == 30)
				right = -right;
			result = DoubleToBits(fma(left, multiplier, right));
			if (IsNan(result))
				result = InvalidResult(cpu, FPSCRVXISI);
			else if (extended >= 30)
				result ^= FLOATSIGN;
		}
		break;
	default:
		OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	FinishResult(cpu, target, result, single, finite);
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void ExecuteControl(OsxCpu *cpu, uint32 instruction)
{
	uint32 extended;
	int target;
	int sourcefield;
	int field;
	uint64 b;
	uint32 mask;
	uint32 value;
	uint32 bit;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	b = cpu->fpr[(instruction >> 11) & 31].bits;
	switch (extended) {
	case 0:
		OsxCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, BitsToDouble(b), 0);
		return;
	case 32:
		OsxCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, BitsToDouble(b), 1);
		return;
	case 12:
		FinishResult(cpu, target, b, 1, !IsInfinity(b));
		break;
	case 14:
		cpu->fpr[target].bits = ConvertToInteger(cpu, b, 0);
		break;
	case 15:
		cpu->fpr[target].bits = ConvertToInteger(cpu, b, 1);
		break;
	case 40:
		cpu->fpr[target].bits = b ^ FLOATSIGN;
		break;
	case 72:
		cpu->fpr[target].bits = b;
		break;
	case 136:
		cpu->fpr[target].bits = b | FLOATSIGN;
		break;
	case 264:
		cpu->fpr[target].bits = b & ~FLOATSIGN;
		break;
	case 583:
		cpu->fpr[target].bits = 0xfff8000000000000ULL | cpu->fpscr;
		break;
	case 64:
		sourcefield = ((instruction >> 18) & 7);
		value = (cpu->fpscr >> ((7 - sourcefield) * 4)) & 0xf;
		cpu->cr = (cpu->cr & ~(0xfu << ((7 - (target >> 2)) * 4))) | (value << ((7 - (target >> 2)) * 4));
		cpu->fpscr &= ~((0xfu << ((7 - sourcefield) * 4)) & FPSCRSTICKY);
		UpdateSummary(cpu);
		return;
	case 38:
	case 70:
		bit = 0x80000000u >> target;
		if (bit != FPSCRFEX && bit != FPSCRVX) {
			if (extended == 38)
				cpu->fpscr |= bit;
			else
				cpu->fpscr &= ~bit;
		}
		UpdateSummary(cpu);
		break;
	case 134:
		MoveToFpscrField(cpu, target >> 2, (instruction >> 12) & 0xf);
		UpdateSummary(cpu);
		break;
	case 711:
		mask = 0;
		value = (instruction >> 17) & 0xff;
		for (field = 0; field < 8; field++)
			if (value & (0x80u >> field))
				mask |= 0xf0000000u >> (field * 4);
		mask &= ~(FPSCRFEX | FPSCRVX);
		cpu->fpscr = (cpu->fpscr & ~mask) | ((uint32)b & mask);
		UpdateSummary(cpu);
		break;
	default:
		OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void ExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	if ((instruction >> 26) == 59) {
		ExecuteArithmetic(cpu, instruction, 1);
		return;
	}
	if (instruction & 0x20)
		ExecuteArithmetic(cpu, instruction, 0);
	else
		ExecuteControl(cpu, instruction);
}

void OsxCpuExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	static const int roundingmodes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
	if (!(cpu->fpscr & 3)) {
		ExecuteFloat(cpu, instruction);
		return;
	}
	fesetround(roundingmodes[cpu->fpscr & 3]);
	ExecuteFloat(cpu, instruction);
	fesetround(FE_TONEAREST);
}
