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
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "sbkernels.h"

#if defined(_MSC_VER)
#define SBNOINLINE __declspec(noinline)
#elif defined(__clang__)
#define SBNOINLINE __attribute__((noinline))
#else
#define SBNOINLINE __attribute__((noinline, noclone))
#endif

static uint32_t SbLeaf(uint32_t value)
{
	return value * 3u + 1u;
}

static SBNOINLINE uint32_t SbDirectLeaf(uint32_t value)
{
	return value * 5u + 3u;
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

static unsigned char *SbAlign16(void *scratch)
{
	return (unsigned char *)(((size_t)scratch + 15) & ~(size_t)15);
}

static uint32_t SbBits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, 4);
	return bits;
}

static uint32_t SbFold(double value)
{
	uint32_t words[2];
	memcpy(words, &value, 8);
	return words[0] ^ words[1];
}

static uint32_t SbNext(uint32_t *seed)
{
	*seed = *seed * 1664525u + 1013904223u;
	return *seed;
}

static void SbAtomicIncrement(volatile uint32_t *counter)
{
#if defined(__powerpc__) || defined(__ppc__) || defined(__POWERPC__)
	uint32_t value;
	__asm__ volatile("1: lwarx %0,0,%1\n\taddi %0,%0,1\n\tstwcx. %0,0,%1\n\tbne- 1b" : "=&b"(value) : "r"(counter) : "cc", "memory");
#elif defined(_MSC_VER)
	_InterlockedIncrement((volatile long *)counter);
#else
	__sync_fetch_and_add(counter, 1u);
#endif
}

const char *SbName(uint32_t kernel)
{
	static const char *names[SBKERNELCOUNT] = {"", "integer mix", "byte copy", "memcpy", "memset", "pointer chase",
		"double chain", "indirect call", "float chain", "compare", "direct call", "atomic add", "double thru",
		"vector loop", "word copy"};
	if (kernel == 0 || kernel >= SBKERNELCOUNT)
		return "unknown";
	return names[kernel];
}

size_t SbScratchBytes(uint32_t kernel, uint32_t param)
{
	if (kernel == SBKERNELBYTECOPY || kernel == SBKERNELCOPY)
		return (size_t)param * 2;
	if (kernel == SBKERNELWORDCOPY)
		return (size_t)param * 2 + 16;
	if (kernel == SBKERNELFILL)
		return param;
	if (kernel == SBKERNELCHASE)
		return (size_t)param * 4;
	if (kernel == SBKERNELCOMPARE)
		return (size_t)param * 2 + 256;
	if (kernel == SBKERNELATOMIC)
		return 32;
	if (kernel == SBKERNELDOUBLETHRU)
		return (size_t)param * 16 + 16;
	if (kernel == SBKERNELVEC)
		return (size_t)param * 8 + 16;
	return 0;
}

void SbVecArrays(void *scratch, uint32_t param, float **x, float **y)
{
	*x = (float *)SbAlign16(scratch);
	*y = *x + param;
}

uint32_t SbVecChecksum(void *scratch, uint32_t param)
{
	float *x;
	float *y;
	uint32_t index;
	uint32_t sum;
	SbVecArrays(scratch, param, &x, &y);
	sum = 0;
	for (index = 0; index < param; index++)
		sum = ((sum << 1) | (sum >> 31)) ^ SbBits(y[index]);
	return sum;
}

void SbPrepare(uint32_t kernel, uint32_t param, void *scratch)
{
	unsigned char *bytes;
	uint32_t *ring;
	int16_t *keys;
	signed char *table;
	double *doubles;
	float *x;
	float *y;
	uint32_t seed;
	uint32_t index;
	uint32_t other;
	uint32_t swap;
	bytes = (unsigned char *)scratch;
	seed = 12345;
	if (kernel == SBKERNELBYTECOPY || kernel == SBKERNELCOPY) {
		for (index = 0; index < param * 2; index++)
			bytes[index] = (unsigned char)(index * 7u + (index >> 8));
	} else if (kernel == SBKERNELWORDCOPY) {
		bytes = SbAlign16(scratch);
		for (index = 0; index < param; index++)
			bytes[index] = (unsigned char)(SbNext(&seed) >> 24);
		memset(bytes + param, 0, param);
	} else if (kernel == SBKERNELCHASE) {
		ring = (uint32_t *)scratch;
		for (index = 0; index < param; index++)
			ring[index] = index;
		for (index = param - 1; index > 0; index--) {
			seed = seed * 1664525u + 1013904223u;
			other = seed % index;
			swap = ring[index];
			ring[index] = ring[other];
			ring[other] = swap;
		}
	} else if (kernel == SBKERNELCOMPARE) {
		keys = (int16_t *)scratch;
		table = (signed char *)scratch + (size_t)param * 2;
		for (index = 0; index < param; index++)
			keys[index] = (int16_t)(SbNext(&seed) >> 24);
		for (index = 0; index < 256; index++)
			table[index] = (signed char)(SbNext(&seed) >> 29);
	} else if (kernel == SBKERNELDOUBLETHRU) {
		doubles = (double *)SbAlign16(scratch);
		for (index = 0; index < param * 2; index++)
			doubles[index] = (double)(SbNext(&seed) >> 16) / 65536.0;
	} else if (kernel == SBKERNELVEC) {
		SbVecArrays(scratch, param, &x, &y);
		for (index = 0; index < param; index++) {
			x[index] = (float)(SbNext(&seed) >> 28);
			y[index] = 0.0f;
		}
	}
}

uint32_t SbRun(uint32_t kernel, uint32_t iterations, uint32_t param, void *scratch)
{
	unsigned char *bytes;
	uint32_t *ring;
	const int16_t *keys;
	const signed char *table;
	const double *source;
	volatile double *target;
	double *doubles;
	float *x;
	float *y;
	uint32_t index;
	uint32_t inner;
	uint32_t state;
	uint32_t sum;
	double value;
	double first;
	double second;
	double accumulators[4];
	float single;
	int16_t key;
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
	if (kernel == SBKERNELCOMPARE) {
		keys = (const int16_t *)scratch;
		table = (const signed char *)scratch + (size_t)param * 2;
		for (index = 0; index < iterations; index++) {
			for (inner = 0; inner < param; inner++) {
				key = keys[inner];
				if (table[key & 255] == table[(inner * 7) & 255])
					sum++;
				else
					sum ^= (uint32_t)key;
			}
		}
		return sum;
	}
	if (kernel == SBKERNELDIRECT) {
		state = 1;
		for (index = 0; index < iterations; index++)
			state = SbDirectLeaf(state);
		return state;
	}
	if (kernel == SBKERNELATOMIC) {
		ring = (uint32_t *)SbAlign16(scratch);
		*ring = 0;
		for (index = 0; index < iterations; index++)
			SbAtomicIncrement((volatile uint32_t *)ring);
		return *ring;
	}
	if (kernel == SBKERNELDOUBLETHRU) {
		doubles = (double *)SbAlign16(scratch);
		accumulators[0] = 0.0;
		accumulators[1] = 0.0;
		accumulators[2] = 0.0;
		accumulators[3] = 0.0;
		for (index = 0; index < iterations; index++) {
			for (inner = 0; inner + 4 <= param; inner += 4) {
				accumulators[0] = doubles[inner] * doubles[param + inner] + accumulators[0];
				accumulators[1] = doubles[inner + 1] * doubles[param + inner + 1] + accumulators[1];
				accumulators[2] = doubles[inner + 2] * doubles[param + inner + 2] + accumulators[2];
				accumulators[3] = doubles[inner + 3] * doubles[param + inner + 3] + accumulators[3];
			}
		}
		return SbFold(accumulators[0]) ^ SbFold(accumulators[1]) * 3u ^ SbFold(accumulators[2]) * 5u ^
			SbFold(accumulators[3]) * 7u;
	}
	if (kernel == SBKERNELVEC) {
		SbVecArrays(scratch, param, &x, &y);
		for (index = 0; index < iterations; index++) {
			for (inner = 0; inner < param; inner++)
				y[inner] = y[inner] * 0.5f + x[inner];
		}
		return SbVecChecksum(scratch, param);
	}
	if (kernel == SBKERNELWORDCOPY) {
		bytes = SbAlign16(scratch);
		source = (const double *)bytes;
		target = (volatile double *)(bytes + param);
		for (index = 0; index < iterations; index++) {
			for (inner = 0; inner + 2 <= param / 8; inner += 2) {
				first = source[inner];
				second = source[inner + 1];
				target[inner] = first;
				target[inner + 1] = second;
			}
		}
		return SbSample(bytes + param, param);
	}
	return 0xffffffffu;
}
