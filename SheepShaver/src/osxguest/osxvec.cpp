/*
 *  osxvec.cpp - Mac OS X guest AltiVec unit
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
#include "osxcpu.h"

#define VSCRSAT 0x00000001
#define VECADDM 0
#define VECSUBM 1
#define VECADDUS 2
#define VECADDSS 3
#define VECSUBUS 4
#define VECSUBSS 5
#define VECMAXU 6
#define VECMAXS 7
#define VECMINU 8
#define VECMINS 9
#define VECAVGU 10
#define VECAVGS 11
#define VECRL 12
#define VECSL 13
#define VECSR 14
#define VECSRA 15
#define VECCMPEQ 0
#define VECCMPGTU 1
#define VECCMPGTS 2

static uint32 ElementMask(uint32 size)
{
	return (uint32)(((uint64)1 << (size * 8)) - 1);
}

static uint32 GetElement(const uint32 *vector, uint32 size, uint32 index)
{
	uint32 perword;
	uint32 shift;
	if (size == 4)
		return vector[index];
	perword = 4 / size;
	shift = 32 - size * 8 * (index % perword + 1);
	return (vector[index / perword] >> shift) & ElementMask(size);
}

static void SetElement(uint32 *vector, uint32 size, uint32 index, uint32 value)
{
	uint32 perword;
	uint32 shift;
	uint32 mask;
	if (size == 4) {
		vector[index] = value;
		return;
	}
	perword = 4 / size;
	shift = 32 - size * 8 * (index % perword + 1);
	mask = ElementMask(size) << shift;
	vector[index / perword] = (vector[index / perword] & ~mask) | ((value << shift) & mask);
}

static int64 SignExtend(uint32 value, uint32 size)
{
	uint32 bits;
	bits = 32 - size * 8;
	return (int64)((int32)(value << bits) >> bits);
}

static uint32 SaturateSigned(OsxCpu *cpu, int64 value, uint32 size)
{
	int64 high;
	int64 low;
	high = ((int64)1 << (size * 8 - 1)) - 1;
	low = -high - 1;
	if (value > high) {
		cpu->vscr |= VSCRSAT;
		value = high;
	} else if (value < low) {
		cpu->vscr |= VSCRSAT;
		value = low;
	}
	return (uint32)value & ElementMask(size);
}

static uint32 SaturateUnsigned(OsxCpu *cpu, int64 value, uint32 size)
{
	int64 high;
	high = (int64)ElementMask(size);
	if (value > high) {
		cpu->vscr |= VSCRSAT;
		value = high;
	} else if (value < 0) {
		cpu->vscr |= VSCRSAT;
		value = 0;
	}
	return (uint32)value;
}

static float GetFloat(const uint32 *vector, uint32 index)
{
	float value;
	memcpy(&value, &vector[index], 4);
	return value;
}

static void SetFloat(uint32 *vector, uint32 index, float value)
{
	memcpy(&vector[index], &value, 4);
}

static int IsNan(float value)
{
	return value != value;
}

static uint32 RotateElement(uint32 value, uint32 amount, uint32 size)
{
	uint32 bits;
	bits = size * 8;
	if (amount == 0)
		return value;
	return ((value << amount) | (value >> (bits - amount))) & ElementMask(size);
}

static uint32 IntegerElement(OsxCpu *cpu, uint32 left, uint32 right, uint32 size, int operation)
{
	int64 signedleft;
	int64 signedright;
	int64 sum;
	uint32 mask;
	uint32 amount;
	mask = ElementMask(size);
	signedleft = SignExtend(left, size);
	signedright = SignExtend(right, size);
	amount = right & (size * 8 - 1);
	switch (operation) {
	case VECADDM: return (left + right) & mask;
	case VECSUBM: return (left - right) & mask;
	case VECADDUS: return SaturateUnsigned(cpu, (int64)left + right, size);
	case VECADDSS: return SaturateSigned(cpu, signedleft + signedright, size);
	case VECSUBUS: return SaturateUnsigned(cpu, (int64)left - right, size);
	case VECSUBSS: return SaturateSigned(cpu, signedleft - signedright, size);
	case VECMAXU:
		if (left > right)
			return left;
		return right;
	case VECMINU:
		if (left < right)
			return left;
		return right;
	case VECMAXS:
		if (signedleft > signedright)
			return left;
		return right;
	case VECMINS:
		if (signedleft < signedright)
			return left;
		return right;
	case VECAVGU: return (uint32)(((uint64)left + right + 1) >> 1);
	case VECAVGS:
		sum = signedleft + signedright + 1;
		return (uint32)((sum - (sum & 1)) / 2) & mask;
	case VECRL: return RotateElement(left, amount, size);
	case VECSL: return (left << amount) & mask;
	case VECSR: return left >> amount;
	case VECSRA: return (uint32)(signedleft >> amount) & mask;
	}
	return 0;
}

static void IntegerVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation)
{
	uint32 index;
	for (index = 0; index < 16 / size; index++)
		SetElement(result, size, index, IntegerElement(cpu, GetElement(left, size, index), GetElement(right, size, index), size, operation));
}

static void MultiplyVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 odd, int issigned)
{
	uint32 index;
	uint32 first;
	uint32 second;
	int64 product;
	for (index = 0; index < 8 / size; index++) {
		first = GetElement(left, size, index * 2 + odd);
		second = GetElement(right, size, index * 2 + odd);
		product = (int64)first * second;
		if (issigned)
			product = SignExtend(first, size) * SignExtend(second, size);
		SetElement(result, size * 2, index, (uint32)product & ElementMask(size * 2));
	}
}

static void MergeVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 low)
{
	uint32 index;
	uint32 base;
	base = low * (8 / size);
	for (index = 0; index < 8 / size; index++) {
		SetElement(result, size, index * 2, GetElement(left, size, base + index));
		SetElement(result, size, index * 2 + 1, GetElement(right, size, base + index));
	}
}

static void PackVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int sourcesigned, int saturate, int targetsigned)
{
	uint32 index;
	uint32 count;
	uint32 value;
	const uint32 *source;
	int64 wide;
	count = 16 / size;
	for (index = 0; index < count * 2; index++) {
		source = left;
		if (index >= count)
			source = right;
		value = GetElement(source, size, index % count);
		wide = (int64)value;
		if (sourcesigned)
			wide = SignExtend(value, size);
		if (!saturate)
			value &= ElementMask(size / 2);
		else if (targetsigned)
			value = SaturateSigned(cpu, wide, size / 2);
		else
			value = SaturateUnsigned(cpu, wide, size / 2);
		SetElement(result, size / 2, index, value);
	}
}

static void UnpackVector(uint32 *result, const uint32 *source, uint32 size, uint32 low)
{
	uint32 index;
	for (index = 0; index < 8 / size; index++)
		SetElement(result, size * 2, index, (uint32)SignExtend(GetElement(source, size, low * (8 / size) + index), size) & ElementMask(size * 2));
}

static void UnpackPixels(uint32 *result, const uint32 *source, uint32 low)
{
	uint32 index;
	uint32 pixel;
	uint32 alpha;
	for (index = 0; index < 4; index++) {
		pixel = GetElement(source, 2, low * 4 + index);
		alpha = 0;
		if (pixel & 0x8000)
			alpha = 0xff;
		result[index] = (alpha << 24) | (((pixel >> 10) & 0x1f) << 16) | (((pixel >> 5) & 0x1f) << 8) | (pixel & 0x1f);
	}
}

static void PackPixels(uint32 *result, const uint32 *left, const uint32 *right)
{
	uint32 index;
	uint32 word;
	for (index = 0; index < 8; index++) {
		word = left[index & 3];
		if (index >= 4)
			word = right[index & 3];
		SetElement(result, 2, index, (((word >> 24) & 1) << 15) | (((word >> 19) & 0x1f) << 10) | (((word >> 11) & 0x1f) << 5) | ((word >> 3) & 0x1f));
	}
}

static void ShiftBits(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 index;
	uint32 next;
	for (index = 0; index < 4; index++) {
		if (amount == 0) {
			result[index] = source[index];
			continue;
		}
		if (left) {
			next = 0;
			if (index < 3)
				next = source[index + 1];
			result[index] = (source[index] << amount) | (next >> (32 - amount));
		} else {
			next = 0;
			if (index > 0)
				next = source[index - 1];
			result[index] = (source[index] >> amount) | (next << (32 - amount));
		}
	}
}

static void ShiftOctets(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 index;
	uint32 value;
	for (index = 0; index < 16; index++) {
		value = 0;
		if (left && index + amount < 16)
			value = GetElement(source, 1, index + amount);
		if (!left && index >= amount)
			value = GetElement(source, 1, index - amount);
		SetElement(result, 1, index, value);
	}
}

static void CompareVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation, int record)
{
	uint32 index;
	uint32 count;
	uint32 first;
	uint32 second;
	uint32 alltrue;
	uint32 allfalse;
	int match;
	count = 16 / size;
	alltrue = 1;
	allfalse = 1;
	for (index = 0; index < count; index++) {
		first = GetElement(left, size, index);
		second = GetElement(right, size, index);
		match = first == second;
		if (operation == VECCMPGTU)
			match = first > second;
		else if (operation == VECCMPGTS)
			match = SignExtend(first, size) > SignExtend(second, size);
		SetElement(result, size, index, 0);
		if (match) {
			SetElement(result, size, index, ElementMask(size));
			allfalse = 0;
		} else {
			alltrue = 0;
		}
	}
	if (record)
		cpu->cr = (cpu->cr & ~0xf0u) | (alltrue << 7) | (allfalse << 5);
}

static void CompareFloat(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, int record)
{
	uint32 index;
	uint32 alltrue;
	uint32 allfalse;
	float first;
	float second;
	int match;
	alltrue = 1;
	allfalse = 1;
	for (index = 0; index < 4; index++) {
		first = GetFloat(left, index);
		second = GetFloat(right, index);
		if (kind == 966) {
			result[index] = 0;
			if (!(first <= second))
				result[index] |= 0x80000000u;
			if (!(first >= -second))
				result[index] |= 0x40000000u;
			if (result[index])
				allfalse = 0;
			continue;
		}
		match = first == second;
		if (kind == 454)
			match = first >= second;
		else if (kind == 710)
			match = first > second;
		result[index] = 0;
		if (match) {
			result[index] = 0xffffffffu;
			allfalse = 0;
		} else {
			alltrue = 0;
		}
	}
	if (!record)
		return;
	if (kind == 966)
		cpu->cr = (cpu->cr & ~0xf0u) | (allfalse << 5);
	else
		cpu->cr = (cpu->cr & ~0xf0u) | (alltrue << 7) | (allfalse << 5);
}

static float RoundFloat(float value, uint32 kind)
{
	double source;
	double rounded;
	source = value;
	if (IsNan(value))
		return value;
	if (kind == 522) {
		rounded = floor(source + 0.5);
		if (rounded - source == 0.5 && fmod(rounded, 2.0) != 0.0)
			rounded -= 1.0;
		return (float)rounded;
	}
	if (kind == 586) {
		if (source < 0)
			return (float)ceil(source);
		return (float)floor(source);
	}
	if (kind == 650)
		return (float)ceil(source);
	return (float)floor(source);
}

static uint32 ConvertToInteger(OsxCpu *cpu, float value, uint32 scale, int issigned)
{
	double scaled;
	if (IsNan(value))
		return 0;
	scaled = ldexp((double)value, (int)scale);
	if (scaled > 8589934592.0)
		scaled = 8589934592.0;
	if (scaled < -8589934592.0)
		scaled = -8589934592.0;
	if (issigned)
		return SaturateSigned(cpu, (int64)scaled, 4);
	return SaturateUnsigned(cpu, (int64)scaled, 4);
}

static void FloatVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, uint32 immediate)
{
	uint32 index;
	float first;
	float second;
	for (index = 0; index < 4; index++) {
		first = GetFloat(left, index);
		second = GetFloat(right, index);
		switch (kind) {
		case 10: SetFloat(result, index, first + second); break;
		case 74: SetFloat(result, index, first - second); break;
		case 1034:
			if (first > second || IsNan(first))
				SetFloat(result, index, first);
			else
				SetFloat(result, index, second);
			break;
		case 1098:
			if (first < second || IsNan(first))
				SetFloat(result, index, first);
			else
				SetFloat(result, index, second);
			break;
		case 266: SetFloat(result, index, 1.0f / second); break;
		case 330: SetFloat(result, index, (float)(1.0 / sqrt((double)second))); break;
		case 394: SetFloat(result, index, (float)pow(2.0, (double)second)); break;
		case 458: SetFloat(result, index, (float)(log((double)second) / log(2.0))); break;
		case 522: case 586: case 650: case 714: SetFloat(result, index, RoundFloat(second, kind)); break;
		case 778: SetFloat(result, index, (float)ldexp((double)right[index], -(int)immediate)); break;
		case 842: SetFloat(result, index, (float)ldexp((double)(int32)right[index], -(int)immediate)); break;
		case 906: result[index] = ConvertToInteger(cpu, second, immediate, 0); break;
		case 970: result[index] = ConvertToInteger(cpu, second, immediate, 1); break;
		}
	}
}

static void SumVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind)
{
	uint32 index;
	uint32 element;
	int64 sum;
	for (index = 0; index < 4; index++) {
		sum = 0;
		if (kind == 1544) {
			sum = right[index];
			for (element = 0; element < 4; element++)
				sum += GetElement(left, 1, index * 4 + element);
			result[index] = SaturateUnsigned(cpu, sum, 4);
			continue;
		}
		sum = (int32)right[index];
		if (kind == 1800) {
			for (element = 0; element < 4; element++)
				sum += SignExtend(GetElement(left, 1, index * 4 + element), 1);
			result[index] = SaturateSigned(cpu, sum, 4);
		} else if (kind == 1608) {
			for (element = 0; element < 2; element++)
				sum += SignExtend(GetElement(left, 2, index * 2 + element), 2);
			result[index] = SaturateSigned(cpu, sum, 4);
		}
	}
	if (kind == 1672) {
		for (index = 0; index < 4; index += 2) {
			sum = (int64)(int32)left[index] + (int32)left[index + 1] + (int32)right[index + 1];
			result[index] = 0;
			result[index + 1] = SaturateSigned(cpu, sum, 4);
		}
	} else if (kind == 1928) {
		sum = (int32)right[3];
		for (index = 0; index < 4; index++)
			sum += (int32)left[index];
		result[0] = 0;
		result[1] = 0;
		result[2] = 0;
		result[3] = SaturateSigned(cpu, sum, 4);
	}
}

static void MultiplySum(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 kind)
{
	uint32 index;
	uint32 element;
	int64 sum;
	for (index = 0; index < 4; index++) {
		sum = (int64)c[index];
		if (kind == 40 || kind == 41)
			sum = (int32)c[index];
		if (kind == 36 || kind == 37) {
			for (element = 0; element < 4; element++) {
				if (kind == 36)
					sum += (int64)GetElement(a, 1, index * 4 + element) * GetElement(b, 1, index * 4 + element);
				else
					sum += SignExtend(GetElement(a, 1, index * 4 + element), 1) * GetElement(b, 1, index * 4 + element);
			}
			result[index] = (uint32)sum;
			continue;
		}
		for (element = 0; element < 2; element++) {
			if (kind == 38 || kind == 39)
				sum += (int64)GetElement(a, 2, index * 2 + element) * GetElement(b, 2, index * 2 + element);
			else
				sum += SignExtend(GetElement(a, 2, index * 2 + element), 2) * SignExtend(GetElement(b, 2, index * 2 + element), 2);
		}
		if (kind == 39)
			result[index] = SaturateUnsigned(cpu, sum, 4);
		else if (kind == 41)
			result[index] = SaturateSigned(cpu, sum, 4);
		else
			result[index] = (uint32)sum;
	}
}

static void ExecuteVectorA(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 instruction)
{
	uint32 kind;
	uint32 index;
	uint32 select;
	uint32 shift;
	int64 product;
	kind = instruction & 0x3f;
	switch (kind) {
	case 32: case 33: case 34:
		for (index = 0; index < 8; index++) {
			product = SignExtend(GetElement(a, 2, index), 2) * SignExtend(GetElement(b, 2, index), 2);
			if (kind == 34) {
				SetElement(result, 2, index, (uint32)((uint64)GetElement(a, 2, index) * GetElement(b, 2, index) + GetElement(c, 2, index)) & 0xffff);
				continue;
			}
			if (kind == 33)
				product += 0x4000;
			SetElement(result, 2, index, SaturateSigned(cpu, (product >> 15) + SignExtend(GetElement(c, 2, index), 2), 2));
		}
		return;
	case 36: case 37: case 38: case 39: case 40: case 41:
		MultiplySum(cpu, result, a, b, c, kind);
		return;
	case 42:
		for (index = 0; index < 4; index++)
			result[index] = (a[index] & ~c[index]) | (b[index] & c[index]);
		return;
	case 43:
		for (index = 0; index < 16; index++) {
			select = GetElement(c, 1, index) & 0x1f;
			if (select < 16)
				SetElement(result, 1, index, GetElement(a, 1, select));
			else
				SetElement(result, 1, index, GetElement(b, 1, select - 16));
		}
		return;
	case 44:
		shift = (instruction >> 6) & 15;
		for (index = 0; index < 16; index++) {
			if (index + shift < 16)
				SetElement(result, 1, index, GetElement(a, 1, index + shift));
			else
				SetElement(result, 1, index, GetElement(b, 1, index + shift - 16));
		}
		return;
	case 46: case 47:
		for (index = 0; index < 4; index++) {
			if (kind == 46)
				SetFloat(result, index, (float)((double)GetFloat(a, index) * GetFloat(c, index) + GetFloat(b, index)));
			else
				SetFloat(result, index, (float)-((double)GetFloat(a, index) * GetFloat(c, index) - GetFloat(b, index)));
		}
		return;
	}
	OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
}

static int ExecuteVectorX(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, uint32 instruction)
{
	uint32 kind;
	uint32 size;
	uint32 field;
	uint32 index;
	int64 immediate;
	kind = instruction & 0x7ff;
	size = 1u << ((kind >> 6) & 3);
	field = (instruction >> 16) & 31;
	switch (kind) {
	case 0: case 64: case 128: IntegerVector(cpu, result, a, b, size, VECADDM); return 1;
	case 1024: case 1088: case 1152: IntegerVector(cpu, result, a, b, 1u << (((kind - 1024) >> 6) & 3), VECSUBM); return 1;
	case 512: case 576: case 640: IntegerVector(cpu, result, a, b, 1u << (((kind - 512) >> 6) & 3), VECADDUS); return 1;
	case 768: case 832: case 896: IntegerVector(cpu, result, a, b, 1u << (((kind - 768) >> 6) & 3), VECADDSS); return 1;
	case 1536: case 1600: case 1664: IntegerVector(cpu, result, a, b, 1u << (((kind - 1536) >> 6) & 3), VECSUBUS); return 1;
	case 1792: case 1856: case 1920: IntegerVector(cpu, result, a, b, 1u << (((kind - 1792) >> 6) & 3), VECSUBSS); return 1;
	case 2: case 66: case 130: IntegerVector(cpu, result, a, b, 1u << (((kind - 2) >> 6) & 3), VECMAXU); return 1;
	case 258: case 322: case 386: IntegerVector(cpu, result, a, b, 1u << (((kind - 258) >> 6) & 3), VECMAXS); return 1;
	case 514: case 578: case 642: IntegerVector(cpu, result, a, b, 1u << (((kind - 514) >> 6) & 3), VECMINU); return 1;
	case 770: case 834: case 898: IntegerVector(cpu, result, a, b, 1u << (((kind - 770) >> 6) & 3), VECMINS); return 1;
	case 1026: case 1090: case 1154: IntegerVector(cpu, result, a, b, 1u << (((kind - 1026) >> 6) & 3), VECAVGU); return 1;
	case 1282: case 1346: case 1410: IntegerVector(cpu, result, a, b, 1u << (((kind - 1282) >> 6) & 3), VECAVGS); return 1;
	case 4: case 68: case 132: IntegerVector(cpu, result, a, b, 1u << (((kind - 4) >> 6) & 3), VECRL); return 1;
	case 260: case 324: case 388: IntegerVector(cpu, result, a, b, 1u << (((kind - 260) >> 6) & 3), VECSL); return 1;
	case 516: case 580: case 644: IntegerVector(cpu, result, a, b, 1u << (((kind - 516) >> 6) & 3), VECSR); return 1;
	case 772: case 836: case 900: IntegerVector(cpu, result, a, b, 1u << (((kind - 772) >> 6) & 3), VECSRA); return 1;
	case 384:
		for (index = 0; index < 4; index++)
			result[index] = (uint32)(((uint64)a[index] + b[index]) >> 32);
		return 1;
	case 1408:
		for (index = 0; index < 4; index++)
			result[index] = a[index] >= b[index];
		return 1;
	case 8: MultiplyVector(result, a, b, 1, 1, 0); return 1;
	case 72: MultiplyVector(result, a, b, 2, 1, 0); return 1;
	case 264: MultiplyVector(result, a, b, 1, 1, 1); return 1;
	case 328: MultiplyVector(result, a, b, 2, 1, 1); return 1;
	case 520: MultiplyVector(result, a, b, 1, 0, 0); return 1;
	case 584: MultiplyVector(result, a, b, 2, 0, 0); return 1;
	case 776: MultiplyVector(result, a, b, 1, 0, 1); return 1;
	case 840: MultiplyVector(result, a, b, 2, 0, 1); return 1;
	case 12: case 76: case 140: MergeVector(result, a, b, size, 0); return 1;
	case 268: case 332: case 396: MergeVector(result, a, b, 1u << (((kind - 268) >> 6) & 3), 1); return 1;
	case 524: case 588: case 652:
		size = 1u << (((kind - 524) >> 6) & 3);
		immediate = GetElement(b, size, field & (16 / size - 1));
		for (index = 0; index < 16 / size; index++)
			SetElement(result, size, index, (uint32)immediate);
		return 1;
	case 780: case 844: case 908:
		size = 1u << (((kind - 780) >> 6) & 3);
		immediate = SignExtend(field << 27, 4) >> 27;
		for (index = 0; index < 16 / size; index++)
			SetElement(result, size, index, (uint32)immediate & ElementMask(size));
		return 1;
	case 14: PackVector(cpu, result, a, b, 2, 0, 0, 0); return 1;
	case 78: PackVector(cpu, result, a, b, 4, 0, 0, 0); return 1;
	case 142: PackVector(cpu, result, a, b, 2, 0, 1, 0); return 1;
	case 206: PackVector(cpu, result, a, b, 4, 0, 1, 0); return 1;
	case 270: PackVector(cpu, result, a, b, 2, 1, 1, 0); return 1;
	case 334: PackVector(cpu, result, a, b, 4, 1, 1, 0); return 1;
	case 398: PackVector(cpu, result, a, b, 2, 1, 1, 1); return 1;
	case 462: PackVector(cpu, result, a, b, 4, 1, 1, 1); return 1;
	case 782: PackPixels(result, a, b); return 1;
	case 526: UnpackVector(result, b, 1, 0); return 1;
	case 590: UnpackVector(result, b, 2, 0); return 1;
	case 654: UnpackVector(result, b, 1, 1); return 1;
	case 718: UnpackVector(result, b, 2, 1); return 1;
	case 846: UnpackPixels(result, b, 0); return 1;
	case 974: UnpackPixels(result, b, 1); return 1;
	case 1028: case 1092: case 1156: case 1220: case 1284:
		for (index = 0; index < 4; index++) {
			if (kind == 1028)
				result[index] = a[index] & b[index];
			else if (kind == 1092)
				result[index] = a[index] & ~b[index];
			else if (kind == 1156)
				result[index] = a[index] | b[index];
			else if (kind == 1220)
				result[index] = a[index] ^ b[index];
			else
				result[index] = ~(a[index] | b[index]);
		}
		return 1;
	case 452: ShiftBits(result, a, GetElement(b, 1, 15) & 7, 1); return 1;
	case 708: ShiftBits(result, a, GetElement(b, 1, 15) & 7, 0); return 1;
	case 1036: ShiftOctets(result, a, (GetElement(b, 1, 15) >> 3) & 15, 1); return 1;
	case 1100: ShiftOctets(result, a, (GetElement(b, 1, 15) >> 3) & 15, 0); return 1;
	case 1544: case 1800: case 1608: case 1672: case 1928:
		SumVector(cpu, result, a, b, kind);
		return 1;
	case 10: case 74: case 1034: case 1098: case 266: case 330: case 394: case 458:
	case 522: case 586: case 650: case 714: case 778: case 842: case 906: case 970:
		FloatVector(cpu, result, a, b, kind, field);
		return 1;
	case 1540:
		result[0] = 0;
		result[1] = 0;
		result[2] = 0;
		result[3] = cpu->vscr;
		return 1;
	case 1604:
		cpu->vscr = b[3] & 0x00010001u;
		return 0;
	}
	OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
	return 0;
}

static int VectorEnabled(OsxCpu *cpu)
{
	if (!cpu->altivec) {
		OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
		return 0;
	}
	if (!(cpu->msr & OSXMSRVEC)) {
		OsxCpuRaiseException(cpu, OSXVECTORALTIVEC, 0, cpu->pc);
		return 0;
	}
	return 1;
}

void OsxCpuExecuteVector(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	uint32 kind;
	int vd;
	int va;
	int vb;
	if (!VectorEnabled(cpu))
		return;
	vd = (instruction >> 21) & 31;
	va = (instruction >> 16) & 31;
	vb = (instruction >> 11) & 31;
	kind = instruction & 0x3f;
	memcpy(result, cpu->vr[vd], sizeof(result));
	if (kind >= 32) {
		ExecuteVectorA(cpu, result, cpu->vr[va], cpu->vr[vb], cpu->vr[(instruction >> 6) & 31], instruction);
	} else if (kind == 6) {
		kind = instruction & 0x3ff;
		if (kind == 198 || kind == 454 || kind == 710 || kind == 966)
			CompareFloat(cpu, result, cpu->vr[va], cpu->vr[vb], kind, (instruction >> 10) & 1);
		else if (kind == 6 || kind == 70 || kind == 134)
			CompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << (kind >> 6), VECCMPEQ, (instruction >> 10) & 1);
		else if (kind == 518 || kind == 582 || kind == 646)
			CompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 518) >> 6), VECCMPGTU, (instruction >> 10) & 1);
		else if (kind == 774 || kind == 838 || kind == 902)
			CompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 774) >> 6), VECCMPGTS, (instruction >> 10) & 1);
		else {
			OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
			return;
		}
	} else if (!ExecuteVectorX(cpu, result, cpu->vr[va], cpu->vr[vb], instruction)) {
		return;
	}
	memcpy(cpu->vr[vd], result, sizeof(result));
}

int OsxCpuExecuteVectorMemory(OsxCpu *cpu, uint32 instruction, uint32 extended, uint32 address)
{
	uint32 value;
	uint32 index;
	uint32 element;
	int vd;
	vd = (instruction >> 21) & 31;
	switch (extended) {
	case 342: case 374: case 822:
		return cpu->altivec;
	case 6: case 38: case 7: case 39: case 71: case 103: case 359: case 135: case 167: case 199: case 231: case 487:
		break;
	default:
		return 0;
	}
	if (!VectorEnabled(cpu))
		return 1;
	switch (extended) {
	case 6: case 38:
		element = address & 15;
		if (extended == 38)
			element = 16 - element;
		for (index = 0; index < 16; index++)
			SetElement(cpu->vr[vd], 1, index, element + index);
		return 1;
	case 7:
		if (OsxCpuReadData(cpu, address, 1, &value))
			SetElement(cpu->vr[vd], 1, address & 15, value);
		return 1;
	case 39:
		address &= ~1u;
		if (OsxCpuReadData(cpu, address, 2, &value))
			SetElement(cpu->vr[vd], 2, (address & 15) >> 1, value);
		return 1;
	case 71:
		address &= ~3u;
		if (OsxCpuReadData(cpu, address, 4, &value))
			cpu->vr[vd][(address & 15) >> 2] = value;
		return 1;
	case 103: case 359:
		address &= ~15u;
		for (index = 0; index < 4; index++) {
			if (!OsxCpuReadData(cpu, address + index * 4, 4, &value))
				return 1;
			cpu->vr[vd][index] = value;
		}
		return 1;
	case 135:
		OsxCpuWriteData(cpu, address, 1, GetElement(cpu->vr[vd], 1, address & 15));
		return 1;
	case 167:
		address &= ~1u;
		OsxCpuWriteData(cpu, address, 2, GetElement(cpu->vr[vd], 2, (address & 15) >> 1));
		return 1;
	case 199:
		address &= ~3u;
		OsxCpuWriteData(cpu, address, 4, cpu->vr[vd][(address & 15) >> 2]);
		return 1;
	case 231: case 487:
		address &= ~15u;
		for (index = 0; index < 4; index++)
			if (!OsxCpuWriteData(cpu, address + index * 4, 4, cpu->vr[vd][index]))
				return 1;
		return 1;
	}
	return 1;
}
