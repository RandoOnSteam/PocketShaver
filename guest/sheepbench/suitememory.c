/*
 *  suitememory.c - SheepBench memory suite
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
#include "bench.h"

#define MEMORYSIZES 4

typedef struct MemoryMove {
	unsigned char *source;
	unsigned char *target;
	uint32_t bytes;
	Boolean data;
} MemoryMove;

static const uint32_t memorysizes[MEMORYSIZES] = {4096, 65536, 1048576, 16777216};

static void MemorySizeName(char *text, uint32_t bytes)
{
	if (bytes >= 1048576)
		sprintf(text, "%lu MB", (unsigned long)(bytes >> 20));
	else
		sprintf(text, "%lu KB", (unsigned long)(bytes >> 10));
}

static uint32_t MemoryMoveBody(void *context, uint32_t iterations)
{
	MemoryMove *move;
	uint32_t index;
	move = (MemoryMove *)context;
	for (index = 0; index < iterations; index++) {
		if (move->data)
			BlockMoveData(move->source, move->target, move->bytes);
		else
			BlockMove(move->source, move->target, move->bytes);
	}
	return move->target[0];
}

static void MemoryRunBlockMove(Bench *bench, uint32_t bytes, Boolean data)
{
	MemoryMove move;
	BenchMeasurement measurement;
	BenchMemory memory;
	BenchRow *row;
	char size[16];
	char name[80];
	char why[100];
	uint32_t iterations;
	uint32_t expected;
	uint32_t checksum;
	MemorySizeName(size, bytes);
	if (data)
		snprintf(name, sizeof(name), "BlockMoveData, %s", size);
	else
		snprintf(name, sizeof(name), "BlockMove, %s", size);
	row = BenchBegin(bench, "memory", name, BENCHUNITMBPERSEC);
	if (row == NULL || bench->cancelled)
		return;
	BenchExpected(SBKERNELCOPY, bytes, &iterations, &expected);
	if (BenchAllocate(&memory, (Size)SbScratchBytes(SBKERNELCOPY, bytes) + 16) == NULL) {
		snprintf(why, sizeof(why), "not enough memory (%lu KB)", (unsigned long)(SbScratchBytes(SBKERNELCOPY, bytes) / 1024));
		BenchSkip(bench, row, why);
		return;
	}
	SbPrepare(SBKERNELCOPY, bytes, memory.pointer);
	move.source = (unsigned char *)memory.pointer;
	move.target = move.source + bytes;
	move.bytes = bytes;
	move.data = data;
	MemoryMoveBody(&move, 1);
	checksum = SbRun(SBKERNELCOPY, 0, bytes, memory.pointer);
	if (checksum != expected) {
		snprintf(why, sizeof(why), "checksum %08lx, expected %08lx", (unsigned long)checksum, (unsigned long)expected);
		BenchFail(bench, row, why);
	} else {
		BenchMeasure(bench, MemoryMoveBody, &move, 1, &measurement);
		BenchRecord(bench, row, &measurement, bytes);
	}
	BenchRelease(&memory);
}

void SuiteMemory(Bench *bench)
{
	char size[16];
	char name[80];
	uint32_t bytes;
	int index;
	for (index = 0; index < MEMORYSIZES && !bench->cancelled; index++) {
		bytes = memorysizes[index];
		MemorySizeName(size, bytes);
		snprintf(name, sizeof(name), "copy (memcpy), %s", size);
		BenchKernelTest(bench, "memory", name, SBKERNELCOPY, bytes, BENCHUNITMBPERSEC, bytes);
		snprintf(name, sizeof(name), "copy (8-byte loop), %s", size);
		BenchKernelTest(bench, "memory", name, SBKERNELWORDCOPY, bytes, BENCHUNITMBPERSEC, bytes);
		MemoryRunBlockMove(bench, bytes, false);
		MemoryRunBlockMove(bench, bytes, true);
		snprintf(name, sizeof(name), "fill (memset), %s", size);
		BenchKernelTest(bench, "memory", name, SBKERNELFILL, bytes, BENCHUNITMBPERSEC, bytes);
		snprintf(name, sizeof(name), "dependent load latency, %s ring", size);
		BenchKernelTest(bench, "memory", name, SBKERNELCHASE, bytes / 4, BENCHUNITNS, 1);
	}
}
