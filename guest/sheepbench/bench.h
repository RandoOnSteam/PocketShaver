/*
 *  bench.h - SheepBench shared declarations
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

#ifndef BENCH_H
#define BENCH_H

#include <MacTypes.h>
#include <Memory.h>
#include <Files.h>
#include <stdint.h>
#include "../shearsclient.h"
#include "sbkernels.h"

#define BENCHSUITECPU 1UL
#define BENCHSUITE68K 2UL
#define BENCHSUITEMEMORY 4UL
#define BENCHSUITEMMU 8UL
#define BENCHSUITEQUICKDRAW 16UL
#define BENCHSUITE3D 32UL
#define BENCHSUITEDISK 64UL
#define BENCHSUITEALL 127UL

#define BENCHPASSMAIN 0
#define BENCHPASSJITOFF 1
#define BENCHPASSES 2

#define BENCHUNITNSINSN 0
#define BENCHUNITMPERSEC 1
#define BENCHUNITMBPERSEC 2
#define BENCHUNITNS 3
#define BENCHUNITPERSEC 4
#define BENCHUNITUS 5

#define BENCHROWNOTRUN 0
#define BENCHROWOK 1
#define BENCHROWSKIPPED 2
#define BENCHROWFAILED 3

#define BENCHMAXROWS 360
#define BENCHHOSTINFO 4064
#define BENCHNOVALUE (-1.0)

typedef struct BenchRow {
	char suite[12];
	char test[80];
	int unit;
	int status;
	double value[BENCHPASSES];
	double noise[BENCHPASSES];
	int reps[BENCHPASSES];
	double native;
	double work;
	char why[100];
} BenchRow;

typedef struct BenchMeasurement {
	double seconds;
	double noise;
	int reps;
	Boolean settled;
	Boolean cancelled;
	uint32_t iterations;
} BenchMeasurement;

typedef struct BenchMemory {
	void *pointer;
	Handle temporary;
} BenchMemory;

typedef struct BenchOptions {
	unsigned long suites;
	Boolean jitoff;
	Boolean quick;
	Boolean shutdown;
	Boolean automatic;
} BenchOptions;

typedef struct Bench {
	ShearsMailbox *mailbox;
	void *ui;
	int suiteindex;
	int suitecount;
	const char *suitename;
	double timebase;
	int failures;
	int pass;
	Boolean cancelled;
	Boolean quick;
	Boolean jitoff;
	unsigned long suites;
	double minrep;
	int minreps;
	int maxreps;
	double budget;
	BenchRow *rows;
	int rowcount;
	char hostinfo[BENCHHOSTINFO + 1];
} Bench;

typedef uint32_t (*BenchBody)(void *context, uint32_t iterations);

unsigned long long BenchNow(void);
double BenchSeconds(Bench *bench, unsigned long long ticks);
void BenchCalibrate(Bench *bench);
void BenchLine(Bench *bench, const char *text);
void BenchLog(Bench *bench, const char *text);
void BenchNumber(char *text, double value);
Boolean BenchCancelled(Bench *bench);
void BenchMeasure(Bench *bench, BenchBody body, void *context, uint32_t start, BenchMeasurement *result);
double BenchUnitValue(int unit, double seconds, double work);
const char *BenchUnitName(int unit);
BenchRow *BenchBegin(Bench *bench, const char *suite, const char *test, int unit);
void BenchRecord(Bench *bench, BenchRow *row, const BenchMeasurement *measurement, double work);
void BenchRecordNative(Bench *bench, BenchRow *row, double nanoseconds, double work);
void BenchSkip(Bench *bench, BenchRow *row, const char *why);
void BenchFail(Bench *bench, BenchRow *row, const char *why);
Boolean BenchHostRefresh(Bench *bench);
const char *BenchHostValue(Bench *bench, const char *key, char *value, int capacity);
Boolean BenchHostIs(Bench *bench, const char *key, const char *expected);
Boolean BenchHostNative(Bench *bench, uint32_t kernel, uint32_t iterations, uint32_t param, double *nanoseconds,
	uint32_t *checksum);
Boolean BenchHostSetJit(Bench *bench, Boolean on);
void *BenchAllocate(BenchMemory *memory, Size bytes);
void BenchRelease(BenchMemory *memory);
Boolean BenchExpected(uint32_t kernel, uint32_t param, uint32_t *iterations, uint32_t *checksum);
Boolean BenchKernelTest(Bench *bench, const char *suite, const char *test, uint32_t kernel, uint32_t param, int unit,
	double work);
long BenchCall3(const void *code, long first, long second, long third);

int BenchSuiteCount(void);
const char *BenchSuiteName(int index);
const char *BenchSuiteDescription(int index);
unsigned long BenchSuiteFlag(int index);
void BenchRun(Bench *bench, const BenchOptions *options);
OSErr BenchWriteReport(Bench *bench, const FSSpec *spec);
Boolean BenchUiOpen(Bench *bench);
void BenchUiClose(Bench *bench);
void BenchUiLine(Bench *bench, const char *text);
void BenchUiStatus(Bench *bench, const char *text);
void BenchUiPoll(Bench *bench);
Boolean BenchUiMenu(Bench *bench, BenchOptions *options);
void BenchUiDone(Bench *bench);

void SuiteCpu(Bench *bench);
void Suite68k(Bench *bench);
void SuiteMemory(Bench *bench);
void SuiteMmu(Bench *bench);
void SuiteQuickDraw(Bench *bench);
void Suite3d(Bench *bench);
void SuiteQd3d(Bench *bench);
void SuiteDisk(Bench *bench);
Boolean BenchHasAltivec(void);
void BenchAltivec(float *x, float *y, uint32_t count, uint32_t iterations, const float *half);

#endif
