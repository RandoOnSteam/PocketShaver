/*
 *  sbkernels.c - SheepBench kernels shared by the guest app and the host
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

#include <string.h>
#include "sbkernels.h"

static uint32_t SbLeaf(uint32_t value)
{
	return value * 3u + 1u;
}

static uint32_t (*volatile sbleaf)(uint32_t) = SbLeaf;

static uint32_t SbSample(const unsigned char *data, uint32_t bytes)
{
	uint32_t sum;
	uint32_t index;
	sum = bytes;
	for (index = 0; index < bytes; index += 61)
		sum = sum * 31u + data[index];
	return sum;
}

const char *SbName(uint32_t kernel)
{
	static const char *names[SBKERNELCOUNT] = {"", "integer mix", "byte copy", "memcpy", "memset", "pointer chase",
		"double chain", "indirect call", "float chain"};
	if (kernel == 0 || kernel >= SBKERNELCOUNT)
		return "unknown";
	return names[kernel];
}

size_t SbScratchBytes(uint32_t kernel, uint32_t param)
{
	if (kernel == SBKERNELBYTECOPY || kernel == SBKERNELCOPY)
		return (size_t)param * 2;
	if (kernel == SBKERNELFILL)
		return param;
	if (kernel == SBKERNELCHASE)
		return (size_t)param * 4;
	return 0;
}

void SbPrepare(uint32_t kernel, uint32_t param, void *scratch)
{
	unsigned char *bytes;
	uint32_t *ring;
	uint32_t seed;
	uint32_t index;
	uint32_t other;
	uint32_t swap;
	bytes = (unsigned char *)scratch;
	if (kernel == SBKERNELBYTECOPY || kernel == SBKERNELCOPY) {
		for (index = 0; index < param * 2; index++)
			bytes[index] = (unsigned char)(index * 7u + (index >> 8));
	} else if (kernel == SBKERNELCHASE) {
		ring = (uint32_t *)scratch;
		for (index = 0; index < param; index++)
			ring[index] = index;
		seed = 12345;
		for (index = param - 1; index > 0; index--) {
			seed = seed * 1664525u + 1013904223u;
			other = seed % index;
			swap = ring[index];
			ring[index] = ring[other];
			ring[other] = swap;
		}
	}
}

uint32_t SbRun(uint32_t kernel, uint32_t iterations, uint32_t param, void *scratch)
{
	unsigned char *bytes;
	uint32_t *ring;
	uint32_t index;
	uint32_t inner;
	uint32_t state;
	uint32_t sum;
	double value;
	float single;
	bytes = (unsigned char *)scratch;
	sum = 0;
	if (kernel == SBKERNELINT) {
		state = 0x12345678u;
		for (index = 0; index < iterations; index++) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			sum = ((sum << 7) | (sum >> 25)) + state * 2654435761u;
		}
		return sum;
	}
	if (kernel == SBKERNELBYTECOPY) {
		for (index = 0; index < iterations; index++) {
			for (inner = 0; inner < param; inner++)
				bytes[param + inner] = (unsigned char)(bytes[inner] + index);
		}
		return SbSample(bytes + param, param);
	}
	if (kernel == SBKERNELCOPY) {
		for (index = 0; index < iterations; index++) {
			bytes[0] = (unsigned char)index;
			memcpy(bytes + param, bytes, param);
		}
		return SbSample(bytes + param, param);
	}
	if (kernel == SBKERNELFILL) {
		for (index = 0; index < iterations; index++)
			memset(bytes, (int)(index & 0xff), param);
		return SbSample(bytes, param);
	}
	if (kernel == SBKERNELCHASE) {
		ring = (uint32_t *)scratch;
		state = 0;
		for (index = 0; index < iterations; index++)
			state = ring[state];
		return state;
	}
	if (kernel == SBKERNELDOUBLE) {
		value = 1.0;
		for (index = 0; index < iterations; index++)
			value = value * 0.75 + 1.0;
		return (uint32_t)(value * 1048576.0);
	}
	if (kernel == SBKERNELCALL) {
		state = 1;
		for (index = 0; index < iterations; index++)
			state = sbleaf(state);
		return state;
	}
	if (kernel == SBKERNELFLOAT) {
		single = 1.0f;
		for (index = 0; index < iterations; index++)
			single = single * 0.75f + 1.0f;
		return (uint32_t)(single * 1024.0f);
	}
	return 0xffffffffu;
}
