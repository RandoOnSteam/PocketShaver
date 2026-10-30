/*
 *  suitemmu.c - SheepBench MMU suite: page-stride loads, first touch, self-modifying code
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
#include <Gestalt.h>
#include <OSUtils.h>
#include "bench.h"

#define MMUPAGES 4096
#define MMUTOUCHBYTES (4L * 1024L * 1024L)

typedef struct MmuStride {
	volatile uint32_t *base;
	uint32_t pages;
	uint32_t stride;
} MmuStride;

typedef struct MmuTouch {
	long bytes;
	long bad;
} MmuTouch;

typedef struct MmuPatch {
	uint32_t *code;
	uint32_t value;
	long bad;
	long calls;
	long firstbad;
	uint32_t firstgot;
	uint32_t firstwant;
} MmuPatch;

static uint32_t MmuStrideBody(void *context, uint32_t iterations)
{
	MmuStride *stride;
	uint32_t iteration;
	uint32_t index;
	uint32_t sum;
	stride = (MmuStride *)context;
	sum = 0;
	for (iteration = 0; iteration < iterations; iteration++) {
		for (index = 0; index < stride->pages; index++)
			sum += stride->base[index * stride->stride];
	}
	return sum;
}

static void MmuRunStride(Bench *bench)
{
	static const uint32_t counts[3] = {16, 256, 4096};
	MmuStride stride;
	BenchMeasurement measurement;
	BenchMemory memory;
	BenchRow *row;
	uint32_t *base;
	char name[80];
	char why[100];
	uint32_t expected;
	uint32_t got;
	int index;
	int same;
	if (BenchAllocate(&memory, MMUPAGES * 4096L + 4096L) == NULL) {
		BenchSkip(bench, BenchBegin(bench, "mmu", "loads one per page", BENCHUNITNS), "not enough memory (16 MB)");
		return;
	}
	base = (uint32_t *)(((unsigned long)memory.pointer + 4095) & ~4095UL);
	for (index = 0; index < MMUPAGES; index++)
		base[index * 1024] = (uint32_t)index;
	for (index = 0; index < 3 && !bench->cancelled; index++) {
		expected = counts[index] * (counts[index] - 1) / 2;
		for (same = 0; same < 2 && !bench->cancelled; same++) {
			if (same)
				snprintf(name, sizeof(name), "%lu loads in one page (reference)", (unsigned long)counts[index]);
			else
				snprintf(name, sizeof(name), "loads one per page, %lu pages", (unsigned long)counts[index]);
			row = BenchBegin(bench, "mmu", name, BENCHUNITNS);
			if (row == NULL)
				continue;
			stride.base = base;
			stride.pages = counts[index];
			stride.stride = 1024;
			if (same)
				stride.stride = 0;
			got = MmuStrideBody(&stride, 1);
			if (!same && got != expected) {
				snprintf(why, sizeof(why), "sum %lu, expected %lu", (unsigned long)got, (unsigned long)expected);
				BenchFail(bench, row, why);
				continue;
			}
			BenchMeasure(bench, MmuStrideBody, &stride, 1, &measurement);
			BenchRecord(bench, row, &measurement, (double)counts[index]);
		}
	}
	BenchRelease(&memory);
}

static uint32_t MmuTouchBody(void *context, uint32_t iterations)
{
	MmuTouch *touch;
	BenchMemory memory;
	unsigned char *bytes;
	uint32_t iteration;
	long index;
	touch = (MmuTouch *)context;
	for (iteration = 0; iteration < iterations; iteration++) {
		bytes = (unsigned char *)BenchAllocate(&memory, touch->bytes);
		if (bytes == NULL) {
			touch->bad++;
			continue;
		}
		for (index = 0; index < touch->bytes; index += 4096)
			bytes[index] = (unsigned char)index;
		BenchRelease(&memory);
	}
	return iterations;
}

static uint32_t MmuPatchBody(void *context, uint32_t iterations)
{
	MmuPatch *patch;
	uint32_t index;
	uint32_t got;
	patch = (MmuPatch *)context;
	for (index = 0; index < iterations; index++) {
		patch->value = (patch->value + 1) & 0x7fff;
		patch->code[0] = 0x38600000UL | patch->value;
		patch->code[1] = 0x4e800020UL;
		MakeDataExecutable(patch->code, 8);
		patch->calls++;
		got = (uint32_t)BenchCall3(patch->code, 0, 0, 0);
		if (got != patch->value) {
			if (patch->bad == 0) {
				patch->firstbad = patch->calls;
				patch->firstgot = got;
				patch->firstwant = patch->value;
			}
			patch->bad++;
		}
	}
	return iterations;
}

static uint32_t MmuFlushBody(void *context, uint32_t iterations)
{
	MmuPatch *patch;
	uint32_t index;
	patch = (MmuPatch *)context;
	for (index = 0; index < iterations; index++)
		MakeDataExecutable(patch->code, 4096);
	return iterations;
}

static void MmuRunTouch(Bench *bench)
{
	MmuTouch touch;
	BenchMeasurement measurement;
	BenchRow *row;
	row = BenchBegin(bench, "mmu", "first write to each page of a new 4 MB block", BENCHUNITNS);
	if (row == NULL || bench->cancelled)
		return;
	touch.bytes = MMUTOUCHBYTES;
	touch.bad = 0;
	BenchMeasure(bench, MmuTouchBody, &touch, 1, &measurement);
	if (touch.bad)
		BenchSkip(bench, row, "could not allocate 4 MB");
	else
		BenchRecord(bench, row, &measurement, (double)(MMUTOUCHBYTES / 4096));
}

static void MmuRunPatch(Bench *bench)
{
	MmuPatch patch;
	BenchMeasurement measurement;
	BenchRow *row;
	Ptr raw;
	char why[100];
	raw = NewPtrClear(2 * 4096);
	row = BenchBegin(bench, "mmu", "rewrite, flush and call a routine (self-modifying code)", BENCHUNITUS);
	if (row == NULL || bench->cancelled) {
		if (raw)
			DisposePtr(raw);
		return;
	}
	if (raw == NULL) {
		BenchSkip(bench, row, "not enough memory");
		return;
	}
	memset(&patch, 0, sizeof(patch));
	patch.code = (uint32_t *)(((unsigned long)raw + 4095) & ~4095UL);
	BenchMeasure(bench, MmuPatchBody, &patch, 1, &measurement);
	if (patch.bad) {
		snprintf(why, sizeof(why), "%ld of %ld calls ran old code (first: call %ld returned %lu, not %lu)", patch.bad,
			patch.calls, patch.firstbad, (unsigned long)patch.firstgot, (unsigned long)patch.firstwant);
		BenchFail(bench, row, why);
	} else
		BenchRecord(bench, row, &measurement, 1);
	row = BenchBegin(bench, "mmu", "MakeDataExecutable, 4 KB", BENCHUNITUS);
	if (row && !bench->cancelled) {
		BenchMeasure(bench, MmuFlushBody, &patch, 1, &measurement);
		BenchRecord(bench, row, &measurement, 1);
	}
	DisposePtr(raw);
}

void SuiteMmu(Bench *bench)
{
	char line[120];
	long value;
	if (bench->pass == BENCHPASSMAIN) {
		if (Gestalt('pgsz', &value) == noErr) {
			snprintf(line, sizeof(line), "info      mmu page size %ld", value);
			BenchLine(bench, line);
		}
		if (Gestalt('vm  ', &value) == noErr) {
			if (value & 1)
				BenchLine(bench, "info      mmu virtual memory on");
			else
				BenchLine(bench, "info      mmu virtual memory off");
		}
	}
	MmuRunStride(bench);
	MmuRunTouch(bench);
	MmuRunPatch(bench);
}
