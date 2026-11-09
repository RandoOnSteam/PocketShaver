/*
 *  osxfpu.cpp - OSX FPU emulation
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

static inline int OsxGuestIsNan(uint64 bits)
{
	return (bits & FLOATEXPONENT) == FLOATEXPONENT && (bits & FLOATFRACTION) != 0;
}

static inline int OsxGuestIsSignalingNan(uint64 bits)
{
	return OsxGuestIsNan(bits) && !(bits & FLOATQUIET);
}

static inline int OsxGuestIsInfinity(uint64 bits)
{
	return (bits & ~FLOATSIGN) == FLOATEXPONENT;
}

static inline int OsxGuestIsZero(uint64 bits)
{
	return (bits & ~FLOATSIGN) == 0;
}

static inline double OsxGuestBitsToDouble(uint64 bits)
{
	OsxFloat value;
	value.bits = bits;
	return value.value;
}

static inline uint64 OsxGuestDoubleToBits(double number)
{
	OsxFloat value;
	value.value = number;
	return value.bits;
}

static uint32 OsxGuestClassifyResult(uint64 bits, int single)
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

static void OsxGuestUpdateSummary(OsxCpu *cpu)
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

static void OsxGuestRaiseFloatFlags(OsxCpu *cpu, uint32 flags)
{
	uint32 fresh;
	fresh = flags & ~cpu->fpscr;
	if (fresh)
		cpu->fpscr |= flags | FPSCRFX;
	OsxGuestUpdateSummary(cpu);
}

static uint64 OsxGuestPropagateNan(OsxCpu *cpu, uint64 first, uint64 second, uint64 third, int count)
{
	uint32 flags;
	flags = 0;
	if (OsxGuestIsSignalingNan(first) || OsxGuestIsSignalingNan(second) || (count > 2 && OsxGuestIsSignalingNan(third)))
		flags |= FPSCRVXSNAN;
	if (flags)
		OsxGuestRaiseFloatFlags(cpu, flags);
	if (OsxGuestIsNan(first))
		return first | FLOATQUIET;
	if (OsxGuestIsNan(second))
		return second | FLOATQUIET;
	return third | FLOATQUIET;
}

static uint64 OsxGuestInvalidResult(OsxCpu *cpu, uint32 reason)
{
	OsxGuestRaiseFloatFlags(cpu, reason);
	return FLOATDEFAULTNAN;
}

static uint64 OsxGuestRoundSingle(uint64 bits)
{
	float narrow;
	if (OsxGuestIsNan(bits))
		return bits & 0xffffffffe0000000ULL;
	narrow = (float)OsxGuestBitsToDouble(bits);
	return OsxGuestDoubleToBits((double)narrow);
}

static void OsxGuestFinishResult(OsxCpu *cpu, int target, uint64 bits, int single, int inputsfinite)
{
	if (single)
		bits = OsxGuestRoundSingle(bits);
	if (inputsfinite && OsxGuestIsInfinity(bits))
		OsxGuestRaiseFloatFlags(cpu, FPSCROX | FPSCRXX);
	cpu->fpr[target].bits = bits;
	cpu->fpscr = (cpu->fpscr & ~FPSCRFPRF) | (OsxGuestClassifyResult(bits, single) << 12);
}

void OsxGuestCpuSetFloatCompare(OsxCpu *cpu, int field, double left, double right, int ordered)
{
	uint32 value;
	uint64 leftbits;
	uint64 rightbits;
	int shift;
	leftbits = OsxGuestDoubleToBits(left);
	rightbits = OsxGuestDoubleToBits(right);
	if (OsxGuestIsNan(leftbits) || OsxGuestIsNan(rightbits)) {
		value = 1;
		if (OsxGuestIsSignalingNan(leftbits) || OsxGuestIsSignalingNan(rightbits)) {
			if (ordered)
				OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXVC);
			else
				OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN);
		} else if (ordered) {
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXVC);
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

static uint64 OsxGuestConvertToInteger(OsxCpu *cpu, uint64 bits, int truncate)
{
	double value;
	double rounded;
	uint32 mode;
	int32 integer;
	if (OsxGuestIsNan(bits)) {
		if (OsxGuestIsSignalingNan(bits))
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXCVI);
		else
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	value = OsxGuestBitsToDouble(bits);
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
		OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff800007fffffffULL;
	}
	if (rounded < -2147483648.0) {
		OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	if (rounded != value)
		cpu->fpscr |= FPSCRXX | FPSCRFX;
	integer = (int32)rounded;
	return 0xfff8000000000000ULL | (uint32)integer;
}

static void OsxGuestMoveToFpscrField(OsxCpu *cpu, int field, uint32 value)
{
	int shift;
	uint32 mask;
	shift = (7 - field) * 4;
	mask = 0xfu << shift;
	if (field == 0)
		mask &= ~(FPSCRFEX | FPSCRVX);
	cpu->fpscr = (cpu->fpscr & ~mask) | ((value << shift) & mask);
}

static void OsxGuestExecuteArithmetic(OsxCpu *cpu, uint32 instruction, int single)
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
	left = OsxGuestBitsToDouble(a);
	right = OsxGuestBitsToDouble(b);
	multiplier = OsxGuestBitsToDouble(c);
	switch (extended) {
	case 18:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b) && !OsxGuestIsZero(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsZero(a) && OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXZDZ);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXIDI);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(left / right);
		}
		break;
	case 20:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b) && ((a ^ b) & FLOATSIGN) == 0)
			result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
		else
			result = OsxGuestDoubleToBits(left - right);
		break;
	case 21:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b) && ((a ^ b) & FLOATSIGN) != 0)
			result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
		else
			result = OsxGuestDoubleToBits(left + right);
		break;
	case 22:
		finite = !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXSQRT);
		else
			result = OsxGuestDoubleToBits(sqrt(right));
		break;
	case 23:
		finite = 0;
		if (!OsxGuestIsNan(a) && left >= 0.0)
			result = c;
		else
			result = b;
		cpu->fpr[target].bits = result;
		if (instruction & 1)
			cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
		return;
	case 24:
		finite = 0;
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(1.0 / right);
		}
		single = 1;
		break;
	case 25:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(c);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(c))
			result = OsxGuestPropagateNan(cpu, a, c, 0, 2);
		else if ((OsxGuestIsInfinity(a) && OsxGuestIsZero(c)) || (OsxGuestIsZero(a) && OsxGuestIsInfinity(c)))
			result = OsxGuestInvalidResult(cpu, FPSCRVXIMZ);
		else
			result = OsxGuestDoubleToBits(left * multiplier);
		break;
	case 26:
		finite = 0;
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXSQRT);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(1.0 / sqrt(right));
		}
		break;
	case 28:
	case 29:
	case 30:
	case 31:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b) && !OsxGuestIsInfinity(c);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b) || OsxGuestIsNan(c)) {
			result = OsxGuestPropagateNan(cpu, a, b, c, 3);
		} else if ((OsxGuestIsInfinity(a) && OsxGuestIsZero(c)) || (OsxGuestIsZero(a) && OsxGuestIsInfinity(c))) {
			result = OsxGuestInvalidResult(cpu, FPSCRVXIMZ);
		} else {
			if (extended == 28 || extended == 30)
				right = -right;
			result = OsxGuestDoubleToBits(fma(left, multiplier, right));
			if (OsxGuestIsNan(result))
				result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
			else if (extended >= 30)
				result ^= FLOATSIGN;
		}
		break;
	default:
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	OsxGuestFinishResult(cpu, target, result, single, finite);
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void OsxGuestExecuteControl(OsxCpu *cpu, uint32 instruction)
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
		OsxGuestCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, OsxGuestBitsToDouble(b), 0);
		return;
	case 32:
		OsxGuestCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, OsxGuestBitsToDouble(b), 1);
		return;
	case 12:
		OsxGuestFinishResult(cpu, target, b, 1, !OsxGuestIsInfinity(b));
		break;
	case 14:
		cpu->fpr[target].bits = OsxGuestConvertToInteger(cpu, b, 0);
		break;
	case 15:
		cpu->fpr[target].bits = OsxGuestConvertToInteger(cpu, b, 1);
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
		OsxGuestUpdateSummary(cpu);
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
		OsxGuestUpdateSummary(cpu);
		break;
	case 134:
		OsxGuestMoveToFpscrField(cpu, target >> 2, (instruction >> 12) & 0xf);
		OsxGuestUpdateSummary(cpu);
		break;
	case 711:
		mask = 0;
		value = (instruction >> 17) & 0xff;
		for (field = 0; field < 8; field++)
			if (value & (0x80u >> field))
				mask |= 0xf0000000u >> (field * 4);
		mask &= ~(FPSCRFEX | FPSCRVX);
		cpu->fpscr = (cpu->fpscr & ~mask) | ((uint32)b & mask);
		OsxGuestUpdateSummary(cpu);
		break;
	default:
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void OsxGuestExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	if ((instruction >> 26) == 59) {
		OsxGuestExecuteArithmetic(cpu, instruction, 1);
		return;
	}
	if (instruction & 0x20)
		OsxGuestExecuteArithmetic(cpu, instruction, 0);
	else
		OsxGuestExecuteControl(cpu, instruction);
}

void OsxGuestCpuExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	static const int roundingmodes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
	if (!(cpu->fpscr & 3)) {
		OsxGuestExecuteFloat(cpu, instruction);
		return;
	}
	fesetround(roundingmodes[cpu->fpscr & 3]);
	OsxGuestExecuteFloat(cpu, instruction);
	fesetround(FE_TONEAREST);
}
