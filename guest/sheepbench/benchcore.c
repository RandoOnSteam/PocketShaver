/*
 *  benchcore.c - SheepBench measurement rule, result rows and host link
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
#include <stdlib.h>
#include <string.h>
#include <Events.h>
#include "bench.h"
#include "sbcases.h"
#include "sbexpected.h"

#define BENCHMAXSAMPLES 64
#define BENCHNOISELIMIT 0.02
#define BENCHKEYCOMMAND 0x37
#define BENCHKEYPERIOD 0x2f

typedef struct BenchKernelContext {
	uint32_t kernel;
	uint32_t param;
	void *scratch;
} BenchKernelContext;

unsigned long long BenchNow(void)
{
	unsigned long high;
	unsigned long low;
	unsigned long again;
	do {
		__asm__ volatile("mftbu %0" : "=r"(high));
		__asm__ volatile("mftb %0" : "=r"(low));
		__asm__ volatile("mftbu %0" : "=r"(again));
	} while (high != again);
	return ((unsigned long long)high << 32) | low;
}

double BenchSeconds(Bench *bench, unsigned long long ticks)
{
	return (double)ticks / bench->timebase;
}

void BenchCalibrate(Bench *bench)
{
	char value[32];
	unsigned long start;
	unsigned long long first;
	bench->timebase = 0;
	if (BenchHostValue(bench, "timebase", value, sizeof(value)))
		bench->timebase = atof(value);
	if (bench->timebase > 1000.0)
		return;
	start = TickCount();
	while (TickCount() == start)
		continue;
	first = BenchNow();
	start = TickCount();
	while (TickCount() < start + 30)
		continue;
	bench->timebase = (double)(BenchNow() - first) / (30.0 / 60.15);
}

void BenchLine(Bench *bench, const char *text)
{
	char line[340];
	BenchUiLine(bench, text);
	if (bench->mailbox) {
		snprintf(line, sizeof(line), "SheepBench: %s\n", text);
		ShearsCall(bench->mailbox, SHEARSCMDREPORT, line, strlen(line));
	}
}

void BenchLog(Bench *bench, const char *text)
{
	if (bench->mailbox)
		ShearsLog(bench->mailbox, text);
}

void BenchNumber(char *text, double value)
{
	long scaled;
	if (value < 0) {
		strcpy(text, "-");
		return;
	}
	if (value >= 100.0) {
		scaled = (long)(value * 10.0 + 0.5);
		sprintf(text, "%ld.%01ld", scaled / 10, scaled % 10);
	} else if (value >= 1.0) {
		scaled = (long)(value * 100.0 + 0.5);
		sprintf(text, "%ld.%02ld", scaled / 100, scaled % 100);
	} else {
		scaled = (long)(value * 1000.0 + 0.5);
		sprintf(text, "%ld.%03ld", scaled / 1000, scaled % 1000);
	}
}

Boolean BenchCancelled(Bench *bench)
{
	KeyMap keys;
	unsigned char *bytes;
	if (bench->cancelled)
		return true;
	BenchUiPoll(bench);
	if (bench->cancelled)
		return true;
	GetKeys(keys);
	bytes = (unsigned char *)keys;
	if ((bytes[BENCHKEYCOMMAND >> 3] >> (BENCHKEYCOMMAND & 7) & 1) && (bytes[BENCHKEYPERIOD >> 3] >> (BENCHKEYPERIOD & 7) & 1))
		bench->cancelled = true;
	return bench->cancelled;
}

static int BenchCompare(const void *first, const void *second)
{
	double a;
	double b;
	a = *(const double *)first;
	b = *(const double *)second;
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

static double BenchMedian(const double *values, int count)
{
	double sorted[BENCHMAXSAMPLES];
	memcpy(sorted, values, sizeof(double) * count);
	qsort(sorted, count, sizeof(double), BenchCompare);
	if (count & 1)
		return sorted[count / 2];
	return (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
}

static double BenchTimeOnce(Bench *bench, BenchBody body, void *context, uint32_t iterations, volatile uint32_t *sink)
{
	unsigned long long start;
	start = BenchNow();
	*sink = body(context, iterations);
	return BenchSeconds(bench, BenchNow() - start);
}

void BenchMeasure(Bench *bench, BenchBody body, void *context, uint32_t start, BenchMeasurement *result)
{
	double times[BENCHMAXSAMPLES];
	double deviations[BENCHMAXSAMPLES];
	double elapsed;
	double median;
	double deviation;
	double spent;
	double factor;
	double budget;
	volatile uint32_t sink;
	uint32_t iterations;
	int count;
	int index;
	int maxreps;
	iterations = start;
	if (iterations == 0)
		iterations = 1;
	budget = bench->budget;
	if (bench->pass == BENCHPASSJITOFF)
		budget *= 0.5;
	maxreps = bench->maxreps;
	if (maxreps > BENCHMAXSAMPLES)
		maxreps = BENCHMAXSAMPLES;
	result->cancelled = false;
	spent = 0;
	for (;;) {
		elapsed = BenchTimeOnce(bench, body, context, iterations, &sink);
		spent += elapsed;
		if (elapsed >= bench->minrep || iterations >= 0x40000000UL)
			break;
		if (BenchCancelled(bench)) {
			result->cancelled = true;
			break;
		}
		factor = 64;
		if (elapsed > 0)
			factor = bench->minrep * 1.25 / elapsed;
		if (factor < 2)
			factor = 2;
		if (factor > 64)
			factor = 64;
		if ((double)iterations * factor >= 1073741824.0)
			iterations = 0x40000000UL;
		else
			iterations = (uint32_t)((double)iterations * factor);
	}
	count = 0;
	times[count++] = elapsed;
	median = elapsed;
	deviation = 0;
	while (!result->cancelled) {
		if (count >= bench->minreps) {
			median = BenchMedian(times, count);
			for (index = 0; index < count; index++) {
				deviations[index] = times[index] - median;
				if (deviations[index] < 0)
					deviations[index] = -deviations[index];
			}
			deviation = BenchMedian(deviations, count);
			if (median > 0 && deviation / median < BENCHNOISELIMIT)
				break;
			if (count >= maxreps || spent >= budget)
				break;
		}
		if (BenchCancelled(bench)) {
			result->cancelled = true;
			break;
		}
		elapsed = BenchTimeOnce(bench, body, context, iterations, &sink);
		spent += elapsed;
		times[count++] = elapsed;
	}
	if (count < bench->minreps) {
		median = BenchMedian(times, count);
		deviation = 0;
	}
	result->seconds = median / iterations;
	result->noise = 0;
	if (median > 0)
		result->noise = deviation / median;
	result->reps = count;
	result->settled = median > 0 && result->noise < BENCHNOISELIMIT;
	result->iterations = iterations;
}

double BenchUnitValue(int unit, double seconds, double work)
{
	if (seconds <= 0)
		return BENCHNOVALUE;
	if (unit == BENCHUNITNSINSN || unit == BENCHUNITNS)
		return seconds * 1e9 / work;
	if (unit == BENCHUNITMPERSEC || unit == BENCHUNITMBPERSEC)
		return work / seconds / 1e6;
	if (unit == BENCHUNITPERSEC)
		return work / seconds;
	return seconds * 1e6 / work;
}

const char *BenchUnitName(int unit)
{
	static const char *names[] = {"ns/insn", "M/s", "MB/s", "ns", "/s", "us"};
	if (unit < 0 || unit > BENCHUNITUS)
		return "";
	return names[unit];
}

static Boolean BenchHigherIsBetter(int unit)
{
	return unit == BENCHUNITMPERSEC || unit == BENCHUNITMBPERSEC || unit == BENCHUNITPERSEC;
}

static const char *BenchPassName(Bench *bench)
{
	if (bench->pass == BENCHPASSJITOFF)
		return " [JIT off]";
	return "";
}

BenchRow *BenchBegin(Bench *bench, const char *suite, const char *test, int unit)
{
	BenchRow *row;
	char status[120];
	int index;
	snprintf(status, sizeof(status), "%s: %s%s", suite, test, BenchPassName(bench));
	BenchUiStatus(bench, status);
	for (index = 0; index < bench->rowcount; index++) {
		row = &bench->rows[index];
		if (strcmp(row->suite, suite) == 0 && strcmp(row->test, test) == 0)
			return row;
	}
	if (bench->rowcount >= BENCHMAXROWS)
		return NULL;
	row = &bench->rows[bench->rowcount++];
	memset(row, 0, sizeof(*row));
	strncpy(row->suite, suite, sizeof(row->suite) - 1);
	strncpy(row->test, test, sizeof(row->test) - 1);
	row->unit = unit;
	row->status = BENCHROWNOTRUN;
	row->value[0] = BENCHNOVALUE;
	row->value[1] = BENCHNOVALUE;
	row->native = BENCHNOVALUE;
	return row;
}

void BenchRecord(Bench *bench, BenchRow *row, const BenchMeasurement *measurement, double work)
{
	char line[300];
	char value[24];
	char noise[24];
	char native[24];
	char percent[24];
	char *end;
	if (row == NULL)
		return;
	row->work = work;
	row->value[bench->pass] = BenchUnitValue(row->unit, measurement->seconds, work);
	row->noise[bench->pass] = measurement->noise * 100.0;
	row->reps[bench->pass] = measurement->reps;
	if (row->status == BENCHROWNOTRUN || row->status == BENCHROWSKIPPED)
		row->status = BENCHROWOK;
	BenchNumber(value, row->value[bench->pass]);
	BenchNumber(noise, row->noise[bench->pass]);
	snprintf(line, sizeof(line), "%-9s %-50s %10s %-7s noise %s%% reps %d", row->suite, row->test, value,
		BenchUnitName(row->unit), noise, measurement->reps);
	end = line + strlen(line);
	if (bench->pass == BENCHPASSMAIN && row->native > 0 && measurement->seconds > 0) {
		BenchNumber(native, BenchUnitValue(row->unit, row->native * 1e-9, work));
		BenchNumber(percent, row->native / (measurement->seconds * 1e9) * 100.0);
		snprintf(end, sizeof(line) - (end - line), "  native %s (%s%%)", native, percent);
		end = line + strlen(line);
	}
	if (!measurement->settled)
		snprintf(end, sizeof(line) - (end - line), " noisy");
	end = line + strlen(line);
	snprintf(end, sizeof(line) - (end - line), "%s", BenchPassName(bench));
	BenchLine(bench, line);
}

void BenchRecordNative(Bench *bench, BenchRow *row, double nanoseconds, double work)
{
	if (row == NULL || bench->pass != BENCHPASSMAIN)
		return;
	row->native = nanoseconds;
	row->work = work;
}

void BenchSkip(Bench *bench, BenchRow *row, const char *why)
{
	char line[300];
	if (row == NULL || bench->pass != BENCHPASSMAIN)
		return;
	row->status = BENCHROWSKIPPED;
	strncpy(row->why, why, sizeof(row->why) - 1);
	snprintf(line, sizeof(line), "%-9s %-50s skipped: %s", row->suite, row->test, why);
	BenchLine(bench, line);
}

void BenchFail(Bench *bench, BenchRow *row, const char *why)
{
	char line[300];
	bench->failures++;
	if (row == NULL)
		return;
	row->status = BENCHROWFAILED;
	strncpy(row->why, why, sizeof(row->why) - 1);
	snprintf(line, sizeof(line), "%-9s %-50s FAILED: %s%s", row->suite, row->test, why, BenchPassName(bench));
	BenchLine(bench, line);
}

Boolean BenchHostRefresh(Bench *bench)
{
	unsigned long length;
	bench->hostinfo[0] = 0;
	if (bench->mailbox == NULL || ShearsCall(bench->mailbox, SHEARSCMDINFO, NULL, 0) != 0)
		return false;
	length = bench->mailbox->length;
	if (length > BENCHHOSTINFO)
		length = BENCHHOSTINFO;
	memcpy(bench->hostinfo, bench->mailbox->payload, length);
	bench->hostinfo[length] = 0;
	return true;
}

const char *BenchHostValue(Bench *bench, const char *key, char *value, int capacity)
{
	const char *text;
	const char *end;
	size_t keylength;
	int length;
	keylength = strlen(key);
	text = bench->hostinfo;
	while (*text) {
		end = strchr(text, '\n');
		if (end == NULL)
			end = text + strlen(text);
		if ((size_t)(end - text) > keylength && strncmp(text, key, keylength) == 0 && text[keylength] == '=') {
			length = (int)(end - text - keylength - 1);
			if (length >= capacity)
				length = capacity - 1;
			memcpy(value, text + keylength + 1, length);
			value[length] = 0;
			return value;
		}
		if (*end == 0)
			break;
		text = end + 1;
	}
	return NULL;
}

Boolean BenchHostIs(Bench *bench, const char *key, const char *expected)
{
	char value[64];
	return BenchHostValue(bench, key, value, sizeof(value)) != NULL && strcmp(value, expected) == 0;
}

Boolean BenchHostNative(Bench *bench, uint32_t kernel, uint32_t iterations, uint32_t param, double *nanoseconds,
	uint32_t *checksum)
{
	unsigned long request[3];
	unsigned long long picoseconds;
	if (bench->mailbox == NULL)
		return false;
	request[0] = kernel;
	request[1] = iterations;
	request[2] = param;
	if (ShearsCall(bench->mailbox, SHEARSCMDNATIVE, request, sizeof(request)) != 0)
		return false;
	*checksum = ShearsWord(bench->mailbox, 0);
	picoseconds = ((unsigned long long)ShearsWord(bench->mailbox, 1) << 32) | ShearsWord(bench->mailbox, 2);
	*nanoseconds = (double)picoseconds / 1000.0;
	return true;
}

Boolean BenchHostSetJit(Bench *bench, Boolean on)
{
	unsigned long word;
	unsigned long until;
	if (bench->mailbox == NULL)
		return false;
	word = 0;
	if (on)
		word = 1;
	if (ShearsCall(bench->mailbox, SHEARSCMDSETJIT, &word, sizeof(word)) != 0)
		return false;
	until = TickCount() + 120;
	while (TickCount() < until) {
		if (BenchHostRefresh(bench) && BenchHostIs(bench, "switch_pending", "0"))
			return true;
	}
	return false;
}

void *BenchAllocate(BenchMemory *memory, Size bytes)
{
	OSErr err;
	memory->temporary = NULL;
	memory->pointer = NULL;
	if (bytes < 1024L * 1024L)
		memory->pointer = NewPtr(bytes);
	if (memory->pointer)
		return memory->pointer;
	memory->temporary = TempNewHandle(bytes, &err);
	if (memory->temporary == NULL) {
		memory->pointer = NewPtr(bytes);
		return memory->pointer;
	}
	HLock(memory->temporary);
	memory->pointer = *memory->temporary;
	return memory->pointer;
}

void BenchRelease(BenchMemory *memory)
{
	if (memory->temporary)
		DisposeHandle(memory->temporary);
	else if (memory->pointer)
		DisposePtr((Ptr)memory->pointer);
	memory->pointer = NULL;
	memory->temporary = NULL;
}

Boolean BenchExpected(uint32_t kernel, uint32_t param, uint32_t *iterations, uint32_t *checksum)
{
	int index;
	for (index = 0; index < SBCASECOUNT; index++) {
		if (sbcases[index].kernel == kernel && sbcases[index].param == param) {
			*iterations = sbcases[index].iterations;
			*checksum = sbexpected[index];
			return true;
		}
	}
	return false;
}

static uint32_t BenchKernelBody(void *context, uint32_t iterations)
{
	BenchKernelContext *kernel;
	kernel = (BenchKernelContext *)context;
	return SbRun(kernel->kernel, iterations, kernel->param, kernel->scratch);
}

Boolean BenchKernelTest(Bench *bench, const char *suite, const char *test, uint32_t kernel, uint32_t param, int unit,
	double work)
{
	BenchKernelContext context;
	BenchMeasurement measurement;
	BenchMemory memory;
	BenchRow *row;
	char why[100];
	double nanoseconds;
	uint32_t iterations;
	uint32_t expected;
	uint32_t checksum;
	uint32_t native;
	row = BenchBegin(bench, suite, test, unit);
	if (row == NULL || bench->cancelled)
		return false;
	if (!BenchExpected(kernel, param, &iterations, &expected)) {
		BenchFail(bench, row, "no build-time checksum for this case");
		return false;
	}
	context.scratch = BenchAllocate(&memory, (Size)SbScratchBytes(kernel, param) + 16);
	if (context.scratch == NULL) {
		snprintf(why, sizeof(why), "not enough memory (%lu KB)", (unsigned long)(SbScratchBytes(kernel, param) / 1024));
		BenchSkip(bench, row, why);
		return false;
	}
	context.kernel = kernel;
	context.param = param;
	SbPrepare(kernel, param, context.scratch);
	checksum = SbRun(kernel, iterations, param, context.scratch);
	if (checksum != expected) {
		snprintf(why, sizeof(why), "checksum %08lx, expected %08lx", (unsigned long)checksum, (unsigned long)expected);
		BenchFail(bench, row, why);
		BenchRelease(&memory);
		return false;
	}
	if (bench->pass == BENCHPASSMAIN && BenchHostNative(bench, kernel, iterations, param, &nanoseconds, &native)) {
		if (native != expected) {
			snprintf(why, sizeof(why), "host native checksum %08lx, expected %08lx", (unsigned long)native,
				(unsigned long)expected);
			BenchFail(bench, row, why);
			BenchRelease(&memory);
			return false;
		}
		BenchRecordNative(bench, row, nanoseconds, work);
	}
	BenchMeasure(bench, BenchKernelBody, &context, iterations, &measurement);
	BenchRecord(bench, row, &measurement, work);
	BenchRelease(&memory);
	return true;
}

long BenchCall3(const void *code, long first, long second, long third)
{
	register long r3 __asm__("r3") = first;
	register long r4 __asm__("r4") = second;
	register long r5 __asm__("r5") = third;
	__asm__ volatile("mtctr %3\n\tbctrl" : "+r"(r3), "+r"(r4), "+r"(r5) : "r"(code) : "ctr", "lr", "r0", "r6", "r7", "r8",
		"r9", "r10", "r11", "r12", "fr0", "fr1", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return r3;
}
