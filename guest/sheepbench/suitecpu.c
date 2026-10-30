/*
 *  suitecpu.c - SheepBench PowerPC CPU suite
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
#include <OSUtils.h>
#include "bench.h"

#define CPULOOPCOPY 0
#define CPULOOPBLOCKMOVE 1
#define CPULOOPCOMPARE 2
#define CPULOOPCALL 3
#define CPULOOPBCTRL 4
#define CPULOOPCONDITIONAL 5
#define CPULOOPCOUNT 7
#define CPUINTEGRITYRUNS 300
#define CPUBLR 0x4e800020UL
#define CPUNOP 0x60000000UL
#define CPUBCTRL 0x4e800421UL
#define CPUBEQLR 0x4d820020UL

typedef struct CpuLoop {
	const char *name;
	unsigned long instructions;
	unsigned long count;
	int far;
	int kind;
} CpuLoop;

typedef struct CpuLoopContext {
	const void *code;
	long first;
	long second;
	long count;
} CpuLoopContext;

typedef struct CpuVecContext {
	float *x;
	float *y;
	uint32_t count;
	const float *half;
} CpuVecContext;

static const CpuLoop cpuloops[CPULOOPCOUNT] = {
	{"loop: byte copy", 3, 3000, 0, CPULOOPCOPY},
	{"loop: BlockMove", 7, 200, 0, CPULOOPBLOCKMOVE},
	{"loop: compare (MacBench integer shape)", 14, 2047, 0, CPULOOPCOMPARE},
	{"loop: call, same page", 4, 1500, 0, CPULOOPCALL},
	{"loop: call, other page", 4, 1500, 1, CPULOOPCALL},
	{"loop: bctrl + blr", 6, 1500, 1, CPULOOPBCTRL},
	{"loop: conditional blr", 5, 1500, 1, CPULOOPCONDITIONAL}
};

static uint32_t CpuD(unsigned long op, unsigned long rt, unsigned long ra, long d)
{
	return (uint32_t)((op << 26) | (rt << 21) | (ra << 16) | ((unsigned long)d & 0xffffUL));
}

static uint32_t CpuX(unsigned long rt, unsigned long ra, unsigned long rb, unsigned long xo)
{
	return (uint32_t)((31UL << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1));
}

static uint32_t CpuBc(unsigned long bo, unsigned long bi, long displacement)
{
	return (uint32_t)((16UL << 26) | (bo << 21) | (bi << 16) | ((unsigned long)displacement & 0xfffcUL));
}

static uint32_t CpuBl(long displacement)
{
	return (uint32_t)((18UL << 26) | ((unsigned long)displacement & 0x3fffffcUL) | 1UL);
}

static uint32_t CpuMr(unsigned long ra, unsigned long rs)
{
	return CpuX(rs, ra, rs, 444);
}

static int CpuBuildLoop(const CpuLoop *loop, uint32_t *code, uint32_t *leaf)
{
	int count;
	count = 0;
	if (loop->kind == CPULOOPCOPY) {
		code[count++] = CpuX(5, 9, 0, 467);
		code[count++] = CpuD(35, 6, 3, 1);
		code[count++] = CpuD(39, 6, 4, 1);
		code[count++] = CpuBc(16, 0, -8);
		code[count++] = CPUBLR;
	} else if (loop->kind == CPULOOPBLOCKMOVE) {
		code[count++] = CpuX(5, 9, 0, 467);
		code[count++] = CpuD(50, 0, 3, 0);
		code[count++] = CpuD(50, 1, 3, 8);
		code[count++] = CpuD(14, 3, 3, 16);
		code[count++] = CpuD(54, 0, 4, 0);
		code[count++] = CpuD(54, 1, 4, 8);
		code[count++] = CpuD(14, 4, 4, 16);
		code[count++] = CpuBc(16, 0, -24);
		code[count++] = CPUBLR;
	} else if (loop->kind == CPULOOPCOMPARE) {
		code[count++] = CpuMr(8, 3);
		code[count++] = CpuMr(7, 4);
		code[count++] = CpuD(14, 6, 0, 1);
		code[count++] = CpuD(14, 9, 0, 0);
		code[count++] = CpuD(14, 10, 0, 0);
		code[count++] = CpuD(14, 4, 0, 0);
		code[count++] = CpuD(14, 10, 10, 1);
		code[count++] = CpuX(9, 4, 0, 922);
		code[count++] = CpuX(0, 4, 6, 0);
		code[count++] = CpuBc(4, 0, 0x2c);
		code[count++] = CpuD(42, 0, 8, 0);
		code[count++] = CpuX(10, 5, 0, 922);
		code[count++] = CpuX(0, 5, 0, 0);
		code[count++] = CpuBc(4, 0, 0x1c);
		code[count++] = CpuX(4, 7, 4, 87);
		code[count++] = CpuX(0, 7, 5, 87);
		code[count++] = CpuX(4, 4, 0, 954);
		code[count++] = CpuX(0, 0, 0, 954);
		code[count++] = CpuX(0, 4, 0, 0);
		code[count++] = CpuBc(12, 2, -0x34);
		code[count++] = CpuMr(3, 10);
		code[count++] = CPUBLR;
	} else if (loop->kind == CPULOOPCALL) {
		code[count++] = CpuX(12, 8, 0, 339);
		code[count++] = CpuX(5, 9, 0, 467);
		code[count++] = CpuD(14, 3, 0, 0);
		if (loop->far) {
			code[count] = CpuBl((long)((char *)leaf - (char *)&code[count]));
			count++;
			code[count++] = CpuBc(16, 0, -4);
			code[count++] = CpuX(12, 8, 0, 467);
			code[count++] = CPUBLR;
			leaf[0] = CpuD(14, 3, 3, 1);
			leaf[1] = CPUBLR;
		} else {
			code[count++] = CpuBl(20);
			code[count++] = CpuBc(16, 0, -4);
			code[count++] = CpuX(12, 8, 0, 467);
			code[count++] = CPUBLR;
			code[count++] = CPUNOP;
			code[count++] = CpuD(14, 3, 3, 1);
			code[count++] = CPUBLR;
		}
	} else if (loop->kind == CPULOOPBCTRL) {
		code[count++] = CpuX(12, 8, 0, 339);
		code[count++] = CpuMr(7, 5);
		code[count++] = CpuD(14, 3, 0, 0);
		code[count++] = CpuD(15, 6, 0, (long)((unsigned long)leaf >> 16));
		code[count++] = CpuD(24, 6, 6, (long)((unsigned long)leaf & 0xffffUL));
		code[count++] = CpuX(6, 9, 0, 467);
		code[count++] = CPUBCTRL;
		code[count++] = CpuD(13, 7, 7, -1);
		code[count++] = CpuBc(4, 2, -12);
		code[count++] = CpuX(12, 8, 0, 467);
		code[count++] = CPUBLR;
		leaf[0] = CpuD(14, 3, 3, 1);
		leaf[1] = CPUBLR;
	} else {
		code[count++] = CpuX(12, 8, 0, 339);
		code[count++] = CpuX(5, 9, 0, 467);
		code[count++] = CpuD(14, 3, 0, 0);
		code[count++] = CpuD(11, 0, 5, 0);
		code[count] = CpuBl((long)((char *)leaf - (char *)&code[count]));
		count++;
		code[count++] = CpuBc(16, 0, -4);
		code[count++] = CpuX(12, 8, 0, 467);
		code[count++] = CPUBLR;
		leaf[0] = CpuD(14, 3, 3, 1);
		leaf[1] = CPUBEQLR;
		leaf[2] = CPUBLR;
	}
	return count;
}

static uint32_t CpuLoopBody(void *context, uint32_t iterations)
{
	CpuLoopContext *loop;
	uint32_t index;
	uint32_t sum;
	loop = (CpuLoopContext *)context;
	sum = 0;
	for (index = 0; index < iterations; index++)
		sum += (uint32_t)BenchCall3(loop->code, loop->first, loop->second, loop->count);
	return sum;
}

static void CpuPrepareData(unsigned char *data)
{
	long index;
	memset(data, 0, 0x4000);
	for (index = 0; index < 4096; index++)
		data[index] = (unsigned char)(index * 7);
	data[0x2000] = 0x07;
	data[0x2001] = 0xff;
}

static void CpuRunLoop(Bench *bench, const CpuLoop *loop, unsigned char *pages, unsigned char *data)
{
	CpuLoopContext context;
	BenchMeasurement measurement;
	BenchRow *row;
	uint32_t *leaf;
	uint32_t *code;
	char why[100];
	long result;
	row = BenchBegin(bench, "cpu", loop->name, BENCHUNITNSINSN);
	if (row == NULL)
		return;
	leaf = (uint32_t *)pages;
	code = (uint32_t *)(pages + 4096);
	memset(pages, 0, 8192);
	CpuBuildLoop(loop, code, leaf);
	MakeDataExecutable(pages, 8192);
	CpuPrepareData(data);
	context.code = code;
	context.count = (long)loop->count;
	context.first = 0;
	context.second = 0;
	if (loop->kind == CPULOOPCOPY) {
		context.first = (long)data - 1;
		context.second = (long)data + 0x1000 - 1;
	} else if (loop->kind == CPULOOPBLOCKMOVE) {
		context.first = (long)data;
		context.second = (long)data + 0x1000;
	} else if (loop->kind == CPULOOPCOMPARE) {
		context.first = (long)data + 0x2000;
		context.second = (long)data + 0x3000;
	}
	result = BenchCall3(code, context.first, context.second, context.count);
	why[0] = 0;
	if (loop->kind == CPULOOPCOPY && memcmp(data, data + 0x1000, 3000) != 0)
		strcpy(why, "copy differs from source");
	else if (loop->kind == CPULOOPBLOCKMOVE && memcmp(data, data + 0x1000, 3200) != 0)
		strcpy(why, "copy differs from source");
	else if (loop->kind == CPULOOPCOMPARE && result != 2047)
		snprintf(why, sizeof(why), "loop ran %ld times, expected 2047", result);
	else if (loop->kind >= CPULOOPCALL && (unsigned long)result != loop->count)
		snprintf(why, sizeof(why), "%ld calls returned, expected %lu", result, loop->count);
	if (why[0]) {
		BenchFail(bench, row, why);
		return;
	}
	BenchMeasure(bench, CpuLoopBody, &context, 4, &measurement);
	BenchRecord(bench, row, &measurement, (double)loop->instructions * loop->count);
}

static void CpuRunLoops(Bench *bench)
{
	Ptr raw;
	unsigned char *pages;
	unsigned char *data;
	int index;
	raw = NewPtrClear(3 * 4096);
	data = (unsigned char *)NewPtr(0x4000);
	if (raw == NULL || data == NULL) {
		for (index = 0; index < CPULOOPCOUNT; index++)
			BenchSkip(bench, BenchBegin(bench, "cpu", cpuloops[index].name, BENCHUNITNSINSN), "no memory for code");
	} else {
		pages = (unsigned char *)(((unsigned long)raw + 4095) & ~4095UL);
		for (index = 0; index < CPULOOPCOUNT && !bench->cancelled; index++)
			CpuRunLoop(bench, &cpuloops[index], pages, data);
	}
	if (raw)
		DisposePtr(raw);
	if (data)
		DisposePtr((Ptr)data);
}

static uint32_t CpuAltivecBody(void *context, uint32_t iterations)
{
	CpuVecContext *vec;
	vec = (CpuVecContext *)context;
	BenchAltivec(vec->x, vec->y, vec->count, iterations, vec->half);
	return (uint32_t)vec->y[0];
}

static void CpuRunAltivec(Bench *bench)
{
	CpuVecContext context;
	BenchMeasurement measurement;
	BenchMemory memory;
	BenchRow *row;
	Ptr halfspace;
	float *half;
	char why[100];
	double nanoseconds;
	uint32_t iterations;
	uint32_t expected;
	uint32_t checksum;
	uint32_t native;
	row = BenchBegin(bench, "cpu", "AltiVec: vmaddfp over 1024 floats", BENCHUNITMPERSEC);
	if (row == NULL || bench->cancelled)
		return;
	if (!BenchHasAltivec()) {
		BenchSkip(bench, row, "no AltiVec (Gestalt)");
		return;
	}
	BenchExpected(SBKERNELVEC, 1024, &iterations, &expected);
	context.x = NULL;
	halfspace = NewPtr(32);
	if (halfspace == NULL || BenchAllocate(&memory, (Size)SbScratchBytes(SBKERNELVEC, 1024) + 16) == NULL) {
		BenchSkip(bench, row, "no memory");
		if (halfspace)
			DisposePtr(halfspace);
		return;
	}
	half = (float *)(((unsigned long)halfspace + 15) & ~15UL);
	half[0] = 0.5f;
	half[1] = 0.5f;
	half[2] = 0.5f;
	half[3] = 0.5f;
	SbPrepare(SBKERNELVEC, 1024, memory.pointer);
	SbVecArrays(memory.pointer, 1024, &context.x, &context.y);
	context.count = 1024;
	context.half = half;
	BenchAltivec(context.x, context.y, 1024, iterations, half);
	checksum = SbVecChecksum(memory.pointer, 1024);
	if (checksum != expected) {
		snprintf(why, sizeof(why), "checksum %08lx, expected %08lx", (unsigned long)checksum, (unsigned long)expected);
		BenchFail(bench, row, why);
	} else {
		if (bench->pass == BENCHPASSMAIN && BenchHostNative(bench, SBKERNELVEC, iterations, 1024, &nanoseconds, &native) &&
			native == expected)
			BenchRecordNative(bench, row, nanoseconds, 1024);
		BenchMeasure(bench, CpuAltivecBody, &context, 1, &measurement);
		BenchRecord(bench, row, &measurement, 1024);
	}
	BenchRelease(&memory);
	DisposePtr(halfspace);
}

static void CpuRunIntegrity(Bench *bench, uint32_t kernel)
{
	BenchRow *row;
	char test[80];
	char why[100];
	char line[200];
	uint32_t expected;
	uint32_t iterations;
	uint32_t result;
	uint32_t first;
	long bad;
	long index;
	snprintf(test, sizeof(test), "%s, %d runs give the same result", SbName(kernel), CPUINTEGRITYRUNS);
	row = BenchBegin(bench, "fpu", test, BENCHUNITPERSEC);
	if (row == NULL || !BenchExpected(kernel, 0, &iterations, &expected))
		return;
	bad = 0;
	first = expected;
	for (index = 0; index < CPUINTEGRITYRUNS; index++) {
		result = SbRun(kernel, iterations, 0, NULL);
		if (result != expected) {
			if (bad == 0)
				first = result;
			bad++;
		}
	}
	if (bad) {
		snprintf(why, sizeof(why), "%ld wrong results, first %08lx expected %08lx", bad, (unsigned long)first,
			(unsigned long)expected);
		BenchFail(bench, row, why);
		return;
	}
	if (row->status != BENCHROWFAILED)
		row->status = BENCHROWOK;
	snprintf(line, sizeof(line), "%-9s %-50s ok", row->suite, row->test);
	if (bench->pass == BENCHPASSJITOFF)
		strcat(line, " [JIT off]");
	BenchLine(bench, line);
}

void SuiteCpu(Bench *bench)
{
	CpuRunLoops(bench);
	BenchKernelTest(bench, "cpu", "integer mix", SBKERNELINT, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "cpu", "byte copy (C), 4 KB", SBKERNELBYTECOPY, 4096, BENCHUNITMBPERSEC, 4096);
	BenchKernelTest(bench, "cpu", "compare (C), 2048 entries", SBKERNELCOMPARE, 2048, BENCHUNITMPERSEC, 2048);
	BenchKernelTest(bench, "cpu", "call (C)", SBKERNELDIRECT, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "cpu", "call through a pointer (C)", SBKERNELCALL, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "cpu", "atomic increment (lwarx/stwcx.)", SBKERNELATOMIC, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "fpu", "double multiply-add, dependent", SBKERNELDOUBLE, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "fpu", "double multiply-add, 4 independent", SBKERNELDOUBLETHRU, 1024, BENCHUNITMPERSEC, 1024);
	BenchKernelTest(bench, "fpu", "single multiply-add, dependent", SBKERNELFLOAT, 0, BENCHUNITMPERSEC, 1);
	BenchKernelTest(bench, "fpu", "vector loop in C, 1024 floats", SBKERNELVEC, 1024, BENCHUNITMPERSEC, 1024);
	CpuRunAltivec(bench);
	CpuRunIntegrity(bench, SBKERNELDOUBLE);
	CpuRunIntegrity(bench, SBKERNELFLOAT);
}
