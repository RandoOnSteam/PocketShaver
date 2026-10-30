/*
 *  suite68k.c - SheepBench 68k suite, run by the ROM's 68k emulator through Mixed Mode
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

#include <stdio.h>
#include <string.h>
#include <MixedMode.h>
#include "bench.h"
#include "k68code.h"

#define K68INT 0
#define K68COPY 1
#define K68BYTECOPY 2
#define K68CALL 3
#define K68MULDIV 4
#define K68COUNT 5
#define K68BUFFER 16384
#define K68RESULT(size) ((ProcInfoType)(size) << 4)
#define K68PARAM(number, size) ((ProcInfoType)(size) << (6 + ((number) - 1) * 2))

typedef struct K68Kernel {
	const char *name;
	int arguments;
	unsigned long instructions;
	unsigned long inner;
} K68Kernel;

typedef struct K68Context {
	UniversalProcPtr procs[K68COUNT];
	ProcInfoType infos[K68COUNT];
	Ptr code;
	unsigned char *source;
	unsigned char *target;
	int kernel;
} K68Context;

static const K68Kernel k68kernels[K68COUNT] = {
	{"68k: add/eor/rol loop", 1, 5, 20000},
	{"68k: move.l (a0)+,(a1)+ copy, 16 KB", 3, 3, 4096},
	{"68k: move.b (a0)+,(a1)+ copy, 4 KB", 3, 3, 4096},
	{"68k: bsr/rts", 1, 4, 10000},
	{"68k: mulu.w/divu.w", 1, 9, 10000}
};

static long K68Run(K68Context *context, int kernel)
{
	if (k68kernels[kernel].arguments == 1)
		return CallUniversalProc(context->procs[kernel], context->infos[kernel], (long)k68kernels[kernel].inner);
	return CallUniversalProc(context->procs[kernel], context->infos[kernel], (long)context->source, (long)context->target,
		(long)k68kernels[kernel].inner);
}

static uint32_t K68Body(void *pointer, uint32_t iterations)
{
	K68Context *context;
	uint32_t index;
	uint32_t sum;
	context = (K68Context *)pointer;
	sum = 0;
	for (index = 0; index < iterations; index++)
		sum += (uint32_t)K68Run(context, context->kernel);
	return sum;
}

static uint32_t K68Expected(int kernel)
{
	uint32_t count;
	uint32_t result;
	uint32_t value;
	result = 0;
	if (kernel == K68INT) {
		for (count = k68kernels[kernel].inner; count; count--) {
			result += count;
			result ^= 0x12345678UL;
			result = (result << 3) | (result >> 29);
		}
	} else if (kernel == K68CALL)
		result = k68kernels[kernel].inner;
	else if (kernel == K68MULDIV) {
		for (count = k68kernels[kernel].inner; count; count--) {
			value = (count & 0xffff) * 5;
			result = (result & 0xffff0000UL) | ((result + value / 7) & 0xffff);
			result = (result & 0xffff0000UL) | ((result + value % 7) & 0xffff);
		}
	}
	return result;
}

static void K68RunAll(Bench *bench, K68Context *context)
{
	BenchMeasurement measurement;
	BenchRow *row;
	char why[100];
	uint32_t result;
	int kernel;
	for (kernel = 0; kernel < K68COUNT && !bench->cancelled; kernel++) {
		row = BenchBegin(bench, "68k", k68kernels[kernel].name, BENCHUNITNSINSN);
		if (row == NULL)
			continue;
		if (context->procs[kernel] == NULL) {
			BenchSkip(bench, row, "no routine descriptor");
			continue;
		}
		memset(context->target, 0, K68BUFFER);
		result = (uint32_t)K68Run(context, kernel);
		why[0] = 0;
		if (kernel == K68COPY && memcmp(context->source, context->target, k68kernels[kernel].inner * 4) != 0)
			strcpy(why, "copy differs from the source");
		else if (kernel == K68BYTECOPY && memcmp(context->source, context->target, k68kernels[kernel].inner) != 0)
			strcpy(why, "copy differs from the source");
		else if (kernel != K68COPY && kernel != K68BYTECOPY && result != K68Expected(kernel))
			snprintf(why, sizeof(why), "returned %08lx, expected %08lx", (unsigned long)result,
				(unsigned long)K68Expected(kernel));
		if (why[0]) {
			BenchFail(bench, row, why);
			continue;
		}
		context->kernel = kernel;
		BenchMeasure(bench, K68Body, context, 1, &measurement);
		BenchRecord(bench, row, &measurement, (double)k68kernels[kernel].instructions * k68kernels[kernel].inner);
	}
}

void Suite68k(Bench *bench)
{
	K68Context context;
	const uint32_t *offsets;
	int index;
	memset(&context, 0, sizeof(context));
	context.code = NewPtr(sizeof(k68code));
	context.source = (unsigned char *)NewPtr(K68BUFFER);
	context.target = (unsigned char *)NewPtr(K68BUFFER);
	if (context.code == NULL || context.source == NULL || context.target == NULL)
		BenchSkip(bench, BenchBegin(bench, "68k", "68k loops", BENCHUNITNSINSN), "not enough memory");
	else {
		BlockMove(k68code, context.code, sizeof(k68code));
		offsets = (const uint32_t *)k68code;
		for (index = 0; index < K68BUFFER; index++)
			context.source[index] = (unsigned char)(index * 13 + 7);
		for (index = 0; index < K68COUNT; index++) {
			context.infos[index] = kCStackBased | K68RESULT(kFourByteCode) | K68PARAM(1, kFourByteCode);
			if (k68kernels[index].arguments == 3)
				context.infos[index] |= K68PARAM(2, kFourByteCode) | K68PARAM(3, kFourByteCode);
			context.procs[index] = NewRoutineDescriptor((ProcPtr)(context.code + offsets[index]), context.infos[index],
				kM68kISA);
		}
		K68RunAll(bench, &context);
		for (index = 0; index < K68COUNT; index++) {
			if (context.procs[index])
				DisposeRoutineDescriptor(context.procs[index]);
		}
	}
	if (context.code)
		DisposePtr(context.code);
	if (context.source)
		DisposePtr((Ptr)context.source);
	if (context.target)
		DisposePtr((Ptr)context.target);
}
