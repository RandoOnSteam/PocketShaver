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

static inline uint32 OsxGuestElementMask(uint32 size)
{
	return (uint32)(((uint64)1 << (size * 8)) - 1);
}

static inline int32 OsxGuestSigned(uint32 value, uint32 size)
{
	uint32 bits;
	bits = 32 - size * 8;
	return (int32)(value << bits) >> bits;
}

static inline void OsxGuestUnpack(const uint32 *vector, uint32 size, uint32 *elements)
{
	uint32 index;
	if (size == 4) {
		elements[0] = vector[0];
		elements[1] = vector[1];
		elements[2] = vector[2];
		elements[3] = vector[3];
		return;
	}
	if (size == 2) {
		for (index = 0; index < 4; index++) {
			elements[index * 2] = vector[index] >> 16;
			elements[index * 2 + 1] = vector[index] & 0xffff;
		}
		return;
	}
	for (index = 0; index < 4; index++) {
		elements[index * 4] = vector[index] >> 24;
		elements[index * 4 + 1] = (vector[index] >> 16) & 0xff;
		elements[index * 4 + 2] = (vector[index] >> 8) & 0xff;
		elements[index * 4 + 3] = vector[index] & 0xff;
	}
}

static inline void OsxGuestPack(uint32 *vector, uint32 size, const uint32 *elements)
{
	uint32 index;
	if (size == 4) {
		vector[0] = elements[0];
		vector[1] = elements[1];
		vector[2] = elements[2];
		vector[3] = elements[3];
		return;
	}
	if (size == 2) {
		for (index = 0; index < 4; index++)
			vector[index] = (elements[index * 2] << 16) | (elements[index * 2 + 1] & 0xffff);
		return;
	}
	for (index = 0; index < 4; index++)
		vector[index] = (elements[index * 4] << 24) | ((elements[index * 4 + 1] & 0xff) << 16) | ((elements[index * 4 + 2] & 0xff) << 8) | (elements[index * 4 + 3] & 0xff);
}

static inline uint32 OsxGuestGetByte(const uint32 *vector, uint32 index)
{
	return (vector[index >> 2] >> (24 - ((index & 3) << 3))) & 0xff;
}

static inline void OsxGuestSetByte(uint32 *vector, uint32 index, uint32 value)
{
	uint32 shift;
	shift = 24 - ((index & 3) << 3);
	vector[index >> 2] = (vector[index >> 2] & ~(0xffu << shift)) | ((value & 0xff) << shift);
}

static inline uint32 OsxGuestGetHalf(const uint32 *vector, uint32 index)
{
	return (vector[index >> 1] >> (16 - ((index & 1) << 4))) & 0xffff;
}

static inline void OsxGuestSetHalf(uint32 *vector, uint32 index, uint32 value)
{
	uint32 shift;
	shift = 16 - ((index & 1) << 4);
	vector[index >> 1] = (vector[index >> 1] & ~(0xffffu << shift)) | ((value & 0xffff) << shift);
}

static inline void OsxGuestUnpackBytes(const uint32 *vector, uint8 *bytes)
{
	uint32 index;
	for (index = 0; index < 4; index++) {
		bytes[index * 4] = (uint8)(vector[index] >> 24);
		bytes[index * 4 + 1] = (uint8)(vector[index] >> 16);
		bytes[index * 4 + 2] = (uint8)(vector[index] >> 8);
		bytes[index * 4 + 3] = (uint8)vector[index];
	}
}

static inline void OsxGuestPackBytes(uint32 *vector, const uint8 *bytes)
{
	uint32 index;
	for (index = 0; index < 4; index++)
		vector[index] = ((uint32)bytes[index * 4] << 24) | ((uint32)bytes[index * 4 + 1] << 16) | ((uint32)bytes[index * 4 + 2] << 8) | bytes[index * 4 + 3];
}

static uint32 OsxGuestSaturateSigned(OsxCpu *cpu, int64 value, uint32 size)
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
	return (uint32)value & OsxGuestElementMask(size);
}

static uint32 OsxGuestSaturateUnsigned(OsxCpu *cpu, int64 value, uint32 size)
{
	int64 high;
	high = (int64)OsxGuestElementMask(size);
	if (value > high) {
		cpu->vscr |= VSCRSAT;
		value = high;
	} else if (value < 0) {
		cpu->vscr |= VSCRSAT;
		value = 0;
	}
	return (uint32)value;
}

static inline float OsxGuestGetFloat(const uint32 *vector, uint32 index)
{
	float value;
	memcpy(&value, &vector[index], 4);
	return value;
}

static inline void OsxGuestSetFloat(uint32 *vector, uint32 index, float value)
{
	memcpy(&vector[index], &value, 4);
}

static inline int OsxGuestIsNan(float value)
{
	return value != value;
}

static void OsxGuestIntegerVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 count;
	uint32 mask;
	uint32 bits;
	uint32 amount;
	uint32 index;
	int64 sum;
	count = 16 / size;
	mask = OsxGuestElementMask(size);
	bits = size * 8;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	switch (operation) {
	case VECADDM:
		for (index = 0; index < count; index++)
			r[index] = (a[index] + b[index]) & mask;
		break;
	case VECSUBM:
		for (index = 0; index < count; index++)
			r[index] = (a[index] - b[index]) & mask;
		break;
	case VECADDUS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateUnsigned(cpu, (int64)a[index] + b[index], size);
		break;
	case VECADDSS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateSigned(cpu, (int64)OsxGuestSigned(a[index], size) + OsxGuestSigned(b[index], size), size);
		break;
	case VECSUBUS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateUnsigned(cpu, (int64)a[index] - b[index], size);
		break;
	case VECSUBSS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateSigned(cpu, (int64)OsxGuestSigned(a[index], size) - OsxGuestSigned(b[index], size), size);
		break;
	case VECMAXU:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (a[index] > b[index])
				r[index] = a[index];
		}
		break;
	case VECMINU:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (a[index] < b[index])
				r[index] = a[index];
		}
		break;
	case VECMAXS:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (OsxGuestSigned(a[index], size) > OsxGuestSigned(b[index], size))
				r[index] = a[index];
		}
		break;
	case VECMINS:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (OsxGuestSigned(a[index], size) < OsxGuestSigned(b[index], size))
				r[index] = a[index];
		}
		break;
	case VECAVGU:
		for (index = 0; index < count; index++)
			r[index] = (uint32)(((uint64)a[index] + b[index] + 1) >> 1);
		break;
	case VECAVGS:
		for (index = 0; index < count; index++) {
			sum = (int64)OsxGuestSigned(a[index], size) + OsxGuestSigned(b[index], size) + 1;
			r[index] = (uint32)((sum - (sum & 1)) / 2) & mask;
		}
		break;
	case VECRL:
		for (index = 0; index < count; index++) {
			amount = b[index] & (bits - 1);
			r[index] = a[index];
			if (amount)
				r[index] = ((a[index] << amount) | (a[index] >> (bits - amount))) & mask;
		}
		break;
	case VECSL:
		for (index = 0; index < count; index++)
			r[index] = (a[index] << (b[index] & (bits - 1))) & mask;
		break;
	case VECSR:
		for (index = 0; index < count; index++)
			r[index] = a[index] >> (b[index] & (bits - 1));
		break;
	case VECSRA:
		for (index = 0; index < count; index++)
			r[index] = (uint32)(OsxGuestSigned(a[index], size) >> (b[index] & (bits - 1))) & mask;
		break;
	}
	OsxGuestPack(result, size, r);
}

static void OsxGuestMultiplyVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 odd, int issigned)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[8];
	uint32 index;
	uint32 mask;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	mask = OsxGuestElementMask(size * 2);
	for (index = 0; index < 8 / size; index++) {
		if (issigned)
			r[index] = (uint32)((int64)OsxGuestSigned(a[index * 2 + odd], size) * OsxGuestSigned(b[index * 2 + odd], size)) & mask;
		else
			r[index] = (uint32)((uint64)a[index * 2 + odd] * b[index * 2 + odd]) & mask;
	}
	OsxGuestPack(result, size * 2, r);
}

static void OsxGuestMergeVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 low)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 index;
	uint32 base;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	base = low * (8 / size);
	for (index = 0; index < 8 / size; index++) {
		r[index * 2] = a[base + index];
		r[index * 2 + 1] = b[base + index];
	}
	OsxGuestPack(result, size, r);
}

static void OsxGuestPackVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int sourcesigned, int saturate, int targetsigned)
{
	uint32 source[32];
	uint32 r[16];
	uint32 index;
	uint32 count;
	uint32 mask;
	int64 wide;
	count = 16 / size;
	OsxGuestUnpack(left, size, source);
	OsxGuestUnpack(right, size, source + count);
	mask = OsxGuestElementMask(size / 2);
	for (index = 0; index < count * 2; index++) {
		if (!saturate) {
			r[index] = source[index] & mask;
			continue;
		}
		wide = (int64)source[index];
		if (sourcesigned)
			wide = OsxGuestSigned(source[index], size);
		if (targetsigned)
			r[index] = OsxGuestSaturateSigned(cpu, wide, size / 2);
		else
			r[index] = OsxGuestSaturateUnsigned(cpu, wide, size / 2);
	}
	OsxGuestPack(result, size / 2, r);
}

static void OsxGuestUnpackVector(uint32 *result, const uint32 *source, uint32 size, uint32 low)
{
	uint32 a[16];
	uint32 r[8];
	uint32 index;
	uint32 base;
	uint32 mask;
	OsxGuestUnpack(source, size, a);
	base = low * (8 / size);
	mask = OsxGuestElementMask(size * 2);
	for (index = 0; index < 8 / size; index++)
		r[index] = (uint32)OsxGuestSigned(a[base + index], size) & mask;
	OsxGuestPack(result, size * 2, r);
}

static void OsxGuestUnpackPixels(uint32 *result, const uint32 *source, uint32 low)
{
	uint32 index;
	uint32 pixel;
	uint32 alpha;
	for (index = 0; index < 4; index++) {
		pixel = OsxGuestGetHalf(source, low * 4 + index);
		alpha = 0;
		if (pixel & 0x8000)
			alpha = 0xff;
		result[index] = (alpha << 24) | (((pixel >> 10) & 0x1f) << 16) | (((pixel >> 5) & 0x1f) << 8) | (pixel & 0x1f);
	}
}

static void OsxGuestPackPixels(uint32 *result, const uint32 *left, const uint32 *right)
{
	uint32 r[8];
	uint32 index;
	uint32 word;
	for (index = 0; index < 8; index++) {
		word = left[index & 3];
		if (index >= 4)
			word = right[index & 3];
		r[index] = (((word >> 24) & 1) << 15) | (((word >> 19) & 0x1f) << 10) | (((word >> 11) & 0x1f) << 5) | ((word >> 3) & 0x1f);
	}
	OsxGuestPack(result, 2, r);
}

static void OsxGuestShiftBits(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 index;
	uint32 next;
	if (amount == 0) {
		for (index = 0; index < 4; index++)
			result[index] = source[index];
		return;
	}
	for (index = 0; index < 4; index++) {
		next = 0;
		if (left) {
			if (index < 3)
				next = source[index + 1];
			result[index] = (source[index] << amount) | (next >> (32 - amount));
		} else {
			if (index > 0)
				next = source[index - 1];
			result[index] = (source[index] >> amount) | (next << (32 - amount));
		}
	}
}

static void OsxGuestShiftOctets(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 padded[8];
	uint32 index;
	uint32 word;
	uint32 shift;
	word = amount >> 2;
	shift = (amount & 3) * 8;
	if (left) {
		for (index = 0; index < 4; index++)
			padded[index] = source[index];
		padded[4] = 0;
		padded[5] = 0;
		padded[6] = 0;
		padded[7] = 0;
		if (shift == 0) {
			for (index = 0; index < 4; index++)
				result[index] = padded[index + word];
		} else {
			for (index = 0; index < 4; index++)
				result[index] = (padded[index + word] << shift) | (padded[index + word + 1] >> (32 - shift));
		}
		return;
	}
	for (index = 0; index < 4; index++)
		padded[index + 4] = source[index];
	padded[0] = 0;
	padded[1] = 0;
	padded[2] = 0;
	padded[3] = 0;
	if (shift == 0) {
		for (index = 0; index < 4; index++)
			result[index] = padded[index - word + 4];
	} else {
		for (index = 0; index < 4; index++)
			result[index] = (padded[index - word + 4] >> shift) | (padded[index - word + 3] << (32 - shift));
	}
}

static void OsxGuestCompareVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation, int record)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 index;
	uint32 count;
	uint32 mask;
	uint32 alltrue;
	uint32 allfalse;
	int match;
	count = 16 / size;
	mask = OsxGuestElementMask(size);
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	alltrue = 1;
	allfalse = 1;
	for (index = 0; index < count; index++) {
		if (operation == VECCMPGTU)
			match = a[index] > b[index];
		else if (operation == VECCMPGTS)
			match = OsxGuestSigned(a[index], size) > OsxGuestSigned(b[index], size);
		else
			match = a[index] == b[index];
		r[index] = 0;
		if (match) {
			r[index] = mask;
			allfalse = 0;
		} else {
			alltrue = 0;
		}
	}
	OsxGuestPack(result, size, r);
	if (record)
		cpu->cr = (cpu->cr & ~0xf0u) | (alltrue << 7) | (allfalse << 5);
}

static void OsxGuestCompareFloat(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, int record)
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
		first = OsxGuestGetFloat(left, index);
		second = OsxGuestGetFloat(right, index);
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

static float OsxGuestRoundFloat(float value, uint32 kind)
{
	double source;
	uint32 bits;
	double rounded;
	source = value;
	if (OsxGuestIsNan(value))
		return value;
	if (kind == 522) {
		rounded = floor(source + 0.5);
		if (rounded - source == 0.5 && fmod(rounded, 2.0) != 0.0)
			rounded -= 1.0;
		memcpy(&bits, &value, sizeof(bits));
		if (rounded == 0.0 && (bits & 0x80000000u))
			rounded = -0.0;
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

static uint32 OsxGuestConvertToInteger(OsxCpu *cpu, float value, uint32 scale, int issigned)
{
	double scaled;
	if (OsxGuestIsNan(value))
		return 0;
	scaled = ldexp((double)value, (int)scale);
	if (scaled > 8589934592.0)
		scaled = 8589934592.0;
	if (scaled < -8589934592.0)
		scaled = -8589934592.0;
	if (issigned)
		return OsxGuestSaturateSigned(cpu, (int64)scaled, 4);
	return OsxGuestSaturateUnsigned(cpu, (int64)scaled, 4);
}

static void OsxGuestFloatVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, uint32 immediate)
{
	uint32 index;
	float first;
	float second;
	switch (kind) {
	case 10:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (OsxGuestIsNan(first))
				result[index] = left[index] | 0x00400000u;
			else if (OsxGuestIsNan(second))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, first + second);
		}
		return;
	case 74:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (OsxGuestIsNan(first))
				result[index] = left[index] | 0x00400000u;
			else if (OsxGuestIsNan(second))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, first - second);
		}
		return;
	case 1034:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (first > second || OsxGuestIsNan(first))
				OsxGuestSetFloat(result, index, first);
			else
				OsxGuestSetFloat(result, index, second);
		}
		return;
	case 1098:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (first < second || OsxGuestIsNan(first))
				OsxGuestSetFloat(result, index, first);
			else
				OsxGuestSetFloat(result, index, second);
		}
		return;
	case 266:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, 1.0f / OsxGuestGetFloat(right, index));
		return;
	case 330:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)(1.0 / sqrt((double)OsxGuestGetFloat(right, index))));
		return;
	case 394:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)pow(2.0, (double)OsxGuestGetFloat(right, index)));
		return;
	case 458:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)(log((double)OsxGuestGetFloat(right, index)) / log(2.0)));
		return;
	case 522: case 586: case 650: case 714:
		for (index = 0; index < 4; index++) {
			if (OsxGuestIsNan(OsxGuestGetFloat(right, index)))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, OsxGuestRoundFloat(OsxGuestGetFloat(right, index), kind));
		}
		return;
	case 778:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)ldexp((double)right[index], -(int)immediate));
		return;
	case 842:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)ldexp((double)(int32)right[index], -(int)immediate));
		return;
	case 906:
		for (index = 0; index < 4; index++)
			result[index] = OsxGuestConvertToInteger(cpu, OsxGuestGetFloat(right, index), immediate, 0);
		return;
	case 970:
		for (index = 0; index < 4; index++)
			result[index] = OsxGuestConvertToInteger(cpu, OsxGuestGetFloat(right, index), immediate, 1);
		return;
	}
}

static void OsxGuestSumVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind)
{
	uint32 a[16];
	uint32 index;
	uint32 element;
	int64 sum;
	switch (kind) {
	case 1544:
		OsxGuestUnpack(left, 1, a);
		for (index = 0; index < 4; index++) {
			sum = right[index];
			for (element = 0; element < 4; element++)
				sum += a[index * 4 + element];
			result[index] = OsxGuestSaturateUnsigned(cpu, sum, 4);
		}
		return;
	case 1800:
		OsxGuestUnpack(left, 1, a);
		for (index = 0; index < 4; index++) {
			sum = (int32)right[index];
			for (element = 0; element < 4; element++)
				sum += OsxGuestSigned(a[index * 4 + element], 1);
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1608:
		OsxGuestUnpack(left, 2, a);
		for (index = 0; index < 4; index++) {
			sum = (int32)right[index];
			for (element = 0; element < 2; element++)
				sum += OsxGuestSigned(a[index * 2 + element], 2);
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1672:
		for (index = 0; index < 4; index += 2) {
			sum = (int64)(int32)left[index] + (int32)left[index + 1] + (int32)right[index + 1];
			result[index] = 0;
			result[index + 1] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1928:
		sum = (int32)right[3];
		for (index = 0; index < 4; index++)
			sum += (int32)left[index];
		result[0] = 0;
		result[1] = 0;
		result[2] = 0;
		result[3] = OsxGuestSaturateSigned(cpu, sum, 4);
		return;
	}
}

static void OsxGuestMultiplySum(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 kind)
{
	uint32 first[16];
	uint32 second[16];
	uint32 index;
	uint32 element;
	int64 sum;
	if (kind == 36 || kind == 37) {
		OsxGuestUnpack(a, 1, first);
		OsxGuestUnpack(b, 1, second);
		for (index = 0; index < 4; index++) {
			sum = (int64)c[index];
			for (element = 0; element < 4; element++) {
				if (kind == 36)
					sum += (int64)first[index * 4 + element] * second[index * 4 + element];
				else
					sum += (int64)OsxGuestSigned(first[index * 4 + element], 1) * second[index * 4 + element];
			}
			result[index] = (uint32)sum;
		}
		return;
	}
	OsxGuestUnpack(a, 2, first);
	OsxGuestUnpack(b, 2, second);
	for (index = 0; index < 4; index++) {
		sum = (int64)c[index];
		if (kind == 40 || kind == 41)
			sum = (int32)c[index];
		for (element = 0; element < 2; element++) {
			if (kind == 38 || kind == 39)
				sum += (int64)first[index * 2 + element] * second[index * 2 + element];
			else
				sum += (int64)OsxGuestSigned(first[index * 2 + element], 2) * OsxGuestSigned(second[index * 2 + element], 2);
		}
		if (kind == 39)
			result[index] = OsxGuestSaturateUnsigned(cpu, sum, 4);
		else if (kind == 41)
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		else
			result[index] = (uint32)sum;
	}
}

static void OsxGuestMultiplyHigh(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 kind)
{
	uint32 first[8];
	uint32 second[8];
	uint32 third[8];
	uint32 r[8];
	uint32 index;
	int64 product;
	OsxGuestUnpack(a, 2, first);
	OsxGuestUnpack(b, 2, second);
	OsxGuestUnpack(c, 2, third);
	if (kind == 34) {
		for (index = 0; index < 8; index++)
			r[index] = (uint32)((uint64)first[index] * second[index] + third[index]) & 0xffff;
	} else if (kind == 33) {
		for (index = 0; index < 8; index++) {
			product = (int64)OsxGuestSigned(first[index], 2) * OsxGuestSigned(second[index], 2) + 0x4000;
			r[index] = OsxGuestSaturateSigned(cpu, (product >> 15) + OsxGuestSigned(third[index], 2), 2);
		}
	} else {
		for (index = 0; index < 8; index++) {
			product = (int64)OsxGuestSigned(first[index], 2) * OsxGuestSigned(second[index], 2);
			r[index] = OsxGuestSaturateSigned(cpu, (product >> 15) + OsxGuestSigned(third[index], 2), 2);
		}
	}
	OsxGuestPack(result, 2, r);
}

static void OsxGuestPermute(uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c)
{
	uint8 source[32];
	uint8 select[16];
	uint32 index;
	OsxGuestUnpackBytes(a, source);
	OsxGuestUnpackBytes(b, source + 16);
	OsxGuestUnpackBytes(c, select);
	for (index = 0; index < 4; index++) {
		result[index] = ((uint32)source[select[index * 4] & 31] << 24)
			| ((uint32)source[select[index * 4 + 1] & 31] << 16)
			| ((uint32)source[select[index * 4 + 2] & 31] << 8)
			| source[select[index * 4 + 3] & 31];
	}
}

static void OsxGuestShiftDouble(uint32 *result, const uint32 *a, const uint32 *b, uint32 shift)
{
	uint8 source[32];
	OsxGuestUnpackBytes(a, source);
	OsxGuestUnpackBytes(b, source + 16);
	OsxGuestPackBytes(result, source + shift);
}

static void OsxGuestExecuteVectorA(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 instruction)
{
	uint32 kind;
	uint32 index;
	kind = instruction & 0x3f;
	switch (kind) {
	case 32: case 33: case 34:
		OsxGuestMultiplyHigh(cpu, result, a, b, c, kind);
		return;
	case 36: case 37: case 38: case 39: case 40: case 41:
		OsxGuestMultiplySum(cpu, result, a, b, c, kind);
		return;
	case 42:
		for (index = 0; index < 4; index++)
			result[index] = (a[index] & ~c[index]) | (b[index] & c[index]);
		return;
	case 43:
		OsxGuestPermute(result, a, b, c);
		return;
	case 44:
		OsxGuestShiftDouble(result, a, b, (instruction >> 6) & 15);
		return;
	case 46:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)((double)OsxGuestGetFloat(a, index) * OsxGuestGetFloat(c, index) + OsxGuestGetFloat(b, index)));
		return;
	case 47:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)-((double)OsxGuestGetFloat(a, index) * OsxGuestGetFloat(c, index) - OsxGuestGetFloat(b, index)));
		return;
	}
	OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
}

static void OsxGuestFill(uint32 *result, uint32 value, uint32 size)
{
	if (size == 1)
		value |= value << 8;
	if (size <= 2)
		value |= value << 16;
	result[0] = value;
	result[1] = value;
	result[2] = value;
	result[3] = value;
}

static void OsxGuestSplat(uint32 *result, const uint32 *source, uint32 size, uint32 field)
{
	uint32 value;
	if (size == 4)
		value = source[field & 3];
	else if (size == 2)
		value = OsxGuestGetHalf(source, field & 7);
	else
		value = OsxGuestGetByte(source, field & 15);
	OsxGuestFill(result, value, size);
}

static int OsxGuestExecuteVectorX(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, uint32 instruction)
{
	uint32 kind;
	uint32 size;
	uint32 field;
	uint32 index;
	kind = instruction & 0x7ff;
	size = 1u << ((kind >> 6) & 3);
	field = (instruction >> 16) & 31;
	switch (kind) {
	case 0: case 64: case 128: OsxGuestIntegerVector(cpu, result, a, b, size, VECADDM); return 1;
	case 1024: case 1088: case 1152: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1024) >> 6) & 3), VECSUBM); return 1;
	case 512: case 576: case 640: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 512) >> 6) & 3), VECADDUS); return 1;
	case 768: case 832: case 896: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 768) >> 6) & 3), VECADDSS); return 1;
	case 1536: case 1600: case 1664: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1536) >> 6) & 3), VECSUBUS); return 1;
	case 1792: case 1856: case 1920: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1792) >> 6) & 3), VECSUBSS); return 1;
	case 2: case 66: case 130: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 2) >> 6) & 3), VECMAXU); return 1;
	case 258: case 322: case 386: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 258) >> 6) & 3), VECMAXS); return 1;
	case 514: case 578: case 642: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 514) >> 6) & 3), VECMINU); return 1;
	case 770: case 834: case 898: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 770) >> 6) & 3), VECMINS); return 1;
	case 1026: case 1090: case 1154: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1026) >> 6) & 3), VECAVGU); return 1;
	case 1282: case 1346: case 1410: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1282) >> 6) & 3), VECAVGS); return 1;
	case 4: case 68: case 132: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 4) >> 6) & 3), VECRL); return 1;
	case 260: case 324: case 388: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 260) >> 6) & 3), VECSL); return 1;
	case 516: case 580: case 644: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 516) >> 6) & 3), VECSR); return 1;
	case 772: case 836: case 900: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 772) >> 6) & 3), VECSRA); return 1;
	case 384:
		for (index = 0; index < 4; index++)
			result[index] = (uint32)(((uint64)a[index] + b[index]) >> 32);
		return 1;
	case 1408:
		for (index = 0; index < 4; index++)
			result[index] = a[index] >= b[index];
		return 1;
	case 8: OsxGuestMultiplyVector(result, a, b, 1, 1, 0); return 1;
	case 72: OsxGuestMultiplyVector(result, a, b, 2, 1, 0); return 1;
	case 264: OsxGuestMultiplyVector(result, a, b, 1, 1, 1); return 1;
	case 328: OsxGuestMultiplyVector(result, a, b, 2, 1, 1); return 1;
	case 520: OsxGuestMultiplyVector(result, a, b, 1, 0, 0); return 1;
	case 584: OsxGuestMultiplyVector(result, a, b, 2, 0, 0); return 1;
	case 776: OsxGuestMultiplyVector(result, a, b, 1, 0, 1); return 1;
	case 840: OsxGuestMultiplyVector(result, a, b, 2, 0, 1); return 1;
	case 12: case 76: case 140: OsxGuestMergeVector(result, a, b, size, 0); return 1;
	case 268: case 332: case 396: OsxGuestMergeVector(result, a, b, 1u << (((kind - 268) >> 6) & 3), 1); return 1;
	case 524: case 588: case 652:
		OsxGuestSplat(result, b, 1u << (((kind - 524) >> 6) & 3), field);
		return 1;
	case 780: case 844: case 908:
		size = 1u << (((kind - 780) >> 6) & 3);
		OsxGuestFill(result, (uint32)((int32)(field << 27) >> 27) & OsxGuestElementMask(size), size);
		return 1;
	case 14: OsxGuestPackVector(cpu, result, a, b, 2, 0, 0, 0); return 1;
	case 78: OsxGuestPackVector(cpu, result, a, b, 4, 0, 0, 0); return 1;
	case 142: OsxGuestPackVector(cpu, result, a, b, 2, 0, 1, 0); return 1;
	case 206: OsxGuestPackVector(cpu, result, a, b, 4, 0, 1, 0); return 1;
	case 270: OsxGuestPackVector(cpu, result, a, b, 2, 1, 1, 0); return 1;
	case 334: OsxGuestPackVector(cpu, result, a, b, 4, 1, 1, 0); return 1;
	case 398: OsxGuestPackVector(cpu, result, a, b, 2, 1, 1, 1); return 1;
	case 462: OsxGuestPackVector(cpu, result, a, b, 4, 1, 1, 1); return 1;
	case 782: OsxGuestPackPixels(result, a, b); return 1;
	case 526: OsxGuestUnpackVector(result, b, 1, 0); return 1;
	case 590: OsxGuestUnpackVector(result, b, 2, 0); return 1;
	case 654: OsxGuestUnpackVector(result, b, 1, 1); return 1;
	case 718: OsxGuestUnpackVector(result, b, 2, 1); return 1;
	case 846: OsxGuestUnpackPixels(result, b, 0); return 1;
	case 974: OsxGuestUnpackPixels(result, b, 1); return 1;
	case 1028:
		for (index = 0; index < 4; index++)
			result[index] = a[index] & b[index];
		return 1;
	case 1092:
		for (index = 0; index < 4; index++)
			result[index] = a[index] & ~b[index];
		return 1;
	case 1156:
		for (index = 0; index < 4; index++)
			result[index] = a[index] | b[index];
		return 1;
	case 1220:
		for (index = 0; index < 4; index++)
			result[index] = a[index] ^ b[index];
		return 1;
	case 1284:
		for (index = 0; index < 4; index++)
			result[index] = ~(a[index] | b[index]);
		return 1;
	case 452: OsxGuestShiftBits(result, a, b[3] & 7, 1); return 1;
	case 708: OsxGuestShiftBits(result, a, b[3] & 7, 0); return 1;
	case 1036: OsxGuestShiftOctets(result, a, (b[3] >> 3) & 15, 1); return 1;
	case 1100: OsxGuestShiftOctets(result, a, (b[3] >> 3) & 15, 0); return 1;
	case 1544: case 1800: case 1608: case 1672: case 1928:
		OsxGuestSumVector(cpu, result, a, b, kind);
		return 1;
	case 10: case 74: case 1034: case 1098: case 266: case 330: case 394: case 458:
	case 522: case 586: case 650: case 714: case 778: case 842: case 906: case 970:
		OsxGuestFloatVector(cpu, result, a, b, kind, field);
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
	OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
	return 0;
}

static int OsxGuestVectorEnabled(OsxCpu *cpu)
{
	if (!cpu->altivec) {
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
		return 0;
	}
	if (!(cpu->msr & OSXMSRVEC)) {
		OsxGuestCpuRaiseException(cpu, OSXVECTORALTIVEC, 0, cpu->pc);
		return 0;
	}
	return 1;
}

static OSXJITABI void OsxGuestVectorGeneric(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	uint32 kind;
	int vd;
	int va;
	int vb;
#if OSXPROFILE
	cpu->tmpform[3][OsxGuestCpuTmpFormKey(instruction)]++;
#endif
	vd = (instruction >> 21) & 31;
	va = (instruction >> 16) & 31;
	vb = (instruction >> 11) & 31;
	kind = instruction & 0x3f;
	if (kind >= 32) {
		OsxGuestExecuteVectorA(cpu, result, cpu->vr[va], cpu->vr[vb], cpu->vr[(instruction >> 6) & 31], instruction);
		if (kind == 35 || kind == 45 || kind > 47)
			return;
	} else if (kind == 6) {
		kind = instruction & 0x3ff;
		if (kind == 198 || kind == 454 || kind == 710 || kind == 966)
			OsxGuestCompareFloat(cpu, result, cpu->vr[va], cpu->vr[vb], kind, (instruction >> 10) & 1);
		else if (kind == 6 || kind == 70 || kind == 134)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << (kind >> 6), VECCMPEQ, (instruction >> 10) & 1);
		else if (kind == 518 || kind == 582 || kind == 646)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 518) >> 6), VECCMPGTU, (instruction >> 10) & 1);
		else if (kind == 774 || kind == 838 || kind == 902)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 774) >> 6), VECCMPGTS, (instruction >> 10) & 1);
		else {
			OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
			return;
		}
	} else if (!OsxGuestExecuteVectorX(cpu, result, cpu->vr[va], cpu->vr[vb], instruction)) {
		return;
	}
	cpu->vr[vd][0] = result[0];
	cpu->vr[vd][1] = result[1];
	cpu->vr[vd][2] = result[2];
	cpu->vr[vd][3] = result[3];
}

void OsxGuestCpuExecuteVector(OsxCpu *cpu, uint32 instruction)
{
	if (OsxGuestVectorEnabled(cpu))
		OsxGuestVectorGeneric(cpu, instruction);
}

static inline uint32 *OsxGuestVectorD(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 21) & 31];
}

static inline const uint32 *OsxGuestVectorA(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 16) & 31];
}

static inline const uint32 *OsxGuestVectorB(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 11) & 31];
}

static inline const uint32 *OsxGuestVectorC(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 6) & 31];
}

static inline void OsxGuestStore(uint32 *target, const uint32 *result)
{
	target[0] = result[0];
	target[1] = result[1];
	target[2] = result[2];
	target[3] = result[3];
}

static OSXJITABI void OsxGuestVectorPermute(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestPermute(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), OsxGuestVectorC(cpu, instruction));
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorShiftDouble(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestShiftDouble(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), (instruction >> 6) & 15);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorMultiplyHigh(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestMultiplyHigh(cpu, result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), OsxGuestVectorC(cpu, instruction), instruction & 0x3f);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorSelect(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	const uint32 *c;
	uint32 result[4];
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	c = OsxGuestVectorC(cpu, instruction);
	for (index = 0; index < 4; index++)
		result[index] = (a[index] & ~c[index]) | (b[index] & c[index]);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorLogical(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	switch (instruction & 0x7ff) {
	case 1028:
		for (index = 0; index < 4; index++)
			d[index] = a[index] & b[index];
		return;
	case 1092:
		for (index = 0; index < 4; index++)
			d[index] = a[index] & ~b[index];
		return;
	case 1156:
		for (index = 0; index < 4; index++)
			d[index] = a[index] | b[index];
		return;
	case 1220:
		for (index = 0; index < 4; index++)
			d[index] = a[index] ^ b[index];
		return;
	}
	for (index = 0; index < 4; index++)
		d[index] = ~(a[index] | b[index]);
}

static OSXJITABI void OsxGuestVectorModular(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	uint32 high;
	uint32 low;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	high = 0x80808080u;
	if (instruction & 0x40)
		high = 0x80008000u;
	low = ~high;
	if (instruction & 0x400) {
		for (index = 0; index < 4; index++)
			d[index] = ((a[index] | high) - (b[index] & low)) ^ ((a[index] ^ ~b[index]) & high);
	} else {
		for (index = 0; index < 4; index++)
			d[index] = ((a[index] & low) + (b[index] & low)) ^ ((a[index] ^ b[index]) & high);
	}
}

static OSXJITABI void OsxGuestVectorModularWord(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	if (instruction & 0x400) {
		for (index = 0; index < 4; index++)
			d[index] = a[index] - b[index];
	} else {
		for (index = 0; index < 4; index++)
			d[index] = a[index] + b[index];
	}
}

static OSXJITABI void OsxGuestVectorMerge(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	uint32 kind;
	kind = instruction & 0x7ff;
	OsxGuestMergeVector(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), 1u << ((kind >> 6) & 3), (kind >> 8) & 1);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorSplat(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestSplat(result, OsxGuestVectorB(cpu, instruction), 1u << ((((instruction & 0x7ff) - 524) >> 6) & 3), (instruction >> 16) & 31);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static int OsxGuestVectorValid(uint32 instruction)
{
	static const uint16 kinds[] = {
		0, 64, 128, 1024, 1088, 1152, 512, 576, 640, 768, 832, 896, 1536, 1600, 1664, 1792, 1856, 1920,
		2, 66, 130, 258, 322, 386, 514, 578, 642, 770, 834, 898, 1026, 1090, 1154, 1282, 1346, 1410,
		4, 68, 132, 260, 324, 388, 516, 580, 644, 772, 836, 900, 384, 1408, 8, 72, 264, 328, 520, 584, 776, 840,
		12, 76, 140, 268, 332, 396, 524, 588, 652, 780, 844, 908, 14, 78, 142, 206, 270, 334, 398, 462, 782,
		526, 590, 654, 718, 846, 974, 1028, 1092, 1156, 1220, 1284, 452, 708, 1036, 1100, 1544, 1800, 1608, 1672, 1928,
		10, 74, 1034, 1098, 266, 330, 394, 458, 522, 586, 650, 714, 778, 842, 906, 970, 1540, 1604
	};
	static const uint16 compares[] = { 198, 454, 710, 966, 6, 70, 134, 518, 582, 646, 774, 838, 902 };
	uint32 kind;
	uint32 index;
	kind = instruction & 0x3f;
	if (kind >= 32)
		return kind != 35 && kind != 45 && kind <= 47;
	if (kind == 6) {
		for (index = 0; index < sizeof(compares) / sizeof(compares[0]); index++)
			if ((instruction & 0x3ff) == compares[index])
				return 1;
		return 0;
	}
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
		if ((instruction & 0x7ff) == kinds[index])
			return 1;
	return 0;
}

OsxVectorFunction OsxGuestCpuVectorHandler(uint32 instruction)
{
	if ((instruction >> 26) != 4 || !OsxGuestVectorValid(instruction))
		return NULL;
	switch (instruction & 0x3f) {
	case 33:
		return OsxGuestVectorMultiplyHigh;
	case 42:
		return OsxGuestVectorSelect;
	case 43:
		return OsxGuestVectorPermute;
	case 44:
		return OsxGuestVectorShiftDouble;
	}
	switch (instruction & 0x7ff) {
	case 0: case 64: case 1024: case 1088:
		return OsxGuestVectorModular;
	case 128: case 1152:
		return OsxGuestVectorModularWord;
	case 1028: case 1092: case 1156: case 1220: case 1284:
		return OsxGuestVectorLogical;
	case 12: case 76: case 140: case 268: case 332: case 396:
		return OsxGuestVectorMerge;
	case 524: case 588: case 652:
		return OsxGuestVectorSplat;
	}
	return OsxGuestVectorGeneric;
}

int OsxGuestCpuExecuteVectorMemory(OsxCpu *cpu, uint32 instruction, uint32 extended, uint32 address)
{
	uint32 words[4];
	uint32 value;
	uint32 element;
	uint32 index;
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
	if (!OsxGuestVectorEnabled(cpu))
		return 1;
	switch (extended) {
	case 103: case 359:
		if (OsxGuestCpuReadQuad(cpu, address & ~15u, words)) {
			cpu->vr[vd][0] = words[0];
			cpu->vr[vd][1] = words[1];
			cpu->vr[vd][2] = words[2];
			cpu->vr[vd][3] = words[3];
		}
		return 1;
	case 231: case 487:
		OsxGuestCpuWriteQuad(cpu, address & ~15u, cpu->vr[vd]);
		return 1;
	case 6: case 38:
		element = address & 15;
		if (extended == 38)
			element = 16 - element;
		for (index = 0; index < 4; index++)
			cpu->vr[vd][index] = (((element + index * 4) & 0xff) << 24) | (((element + index * 4 + 1) & 0xff) << 16) | (((element + index * 4 + 2) & 0xff) << 8) | ((element + index * 4 + 3) & 0xff);
		return 1;
	case 7:
		if (OsxGuestCpuReadData(cpu, address, 1, &value))
			OsxGuestSetByte(cpu->vr[vd], address & 15, value);
		return 1;
	case 39:
		address &= ~1u;
		if (OsxGuestCpuReadData(cpu, address, 2, &value))
			OsxGuestSetHalf(cpu->vr[vd], (address & 15) >> 1, value);
		return 1;
	case 71:
		address &= ~3u;
		if (OsxGuestCpuReadData(cpu, address, 4, &value))
			cpu->vr[vd][(address & 15) >> 2] = value;
		return 1;
	case 135:
		OsxGuestCpuWriteData(cpu, address, 1, OsxGuestGetByte(cpu->vr[vd], address & 15));
		return 1;
	case 167:
		address &= ~1u;
		OsxGuestCpuWriteData(cpu, address, 2, OsxGuestGetHalf(cpu->vr[vd], (address & 15) >> 1));
		return 1;
	case 199:
		address &= ~3u;
		OsxGuestCpuWriteData(cpu, address, 4, cpu->vr[vd][(address & 15) >> 2]);
		return 1;
	}
	return 1;
}
