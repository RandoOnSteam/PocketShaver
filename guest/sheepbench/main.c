/*
 *  main.c - SheepBench: emulator benchmark for Mac OS 9 under SheepShaver
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
#include <MacTypes.h>
#include <Memory.h>
#include <Files.h>
#include <Folders.h>
#include <Events.h>
#include <AppleEvents.h>
#include <Processes.h>
#include <Script.h>
#include "bench.h"

#define BENCHVERSION "2"
#define BENCHSUITECOUNT 7
#define BENCHREPORTLINE 400

typedef struct BenchSuite {
	const char *name;
	unsigned long flag;
	void (*run)(Bench *bench);
} BenchSuite;

typedef struct BenchOptions {
	unsigned long suites;
	Boolean jitoff;
	Boolean quick;
	Boolean shutdown;
	Boolean automatic;
} BenchOptions;

static const BenchSuite benchsuites[BENCHSUITECOUNT] = {
	{"cpu", BENCHSUITECPU, SuiteCpu},
	{"68k", BENCHSUITE68K, Suite68k},
	{"memory", BENCHSUITEMEMORY, SuiteMemory},
	{"mmu", BENCHSUITEMMU, SuiteMmu},
	{"quickdraw", BENCHSUITEQUICKDRAW, SuiteQuickDraw},
	{"3d", BENCHSUITE3D, Suite3d},
	{"disk", BENCHSUITEDISK, SuiteDisk}
};

static unsigned long BenchParseSuites(const char *text)
{
	unsigned long suites;
	int index;
	suites = 0;
	for (index = 0; index < BENCHSUITECOUNT; index++) {
		if (strstr(text, benchsuites[index].name))
			suites |= benchsuites[index].flag;
	}
	if (strstr(text, "all"))
		suites = BENCHSUITEALL;
	return suites;
}

static void BenchParseOption(BenchOptions *options, const char *line)
{
	if (strncmp(line, "suites ", 7) == 0)
		options->suites = BenchParseSuites(line + 7);
	else if (strncmp(line, "jit ", 4) == 0)
		options->jitoff = line[4] == '1';
	else if (strncmp(line, "quick ", 6) == 0)
		options->quick = line[6] == '1';
	else if (strncmp(line, "shutdown ", 9) == 0)
		options->shutdown = line[9] == '1';
}

static Boolean BenchOpenAuto(short *file)
{
	FSSpec spec;
	short volume;
	long folder;
	if (FSMakeFSSpec(0, 0, "\pSheepBench Auto", &spec) == noErr && FSpOpenDF(&spec, fsRdPerm, file) == noErr)
		return true;
	if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &volume, &folder) != noErr)
		return false;
	return FSMakeFSSpec(volume, folder, "\pSheepBench Auto", &spec) == noErr && FSpOpenDF(&spec, fsRdPerm, file) == noErr;
}

static Boolean BenchReadAuto(BenchOptions *options)
{
	char text[512];
	char *line;
	char *end;
	short file;
	long count;
	if (!BenchOpenAuto(&file))
		return false;
	count = sizeof(text) - 1;
	FSRead(file, &count, text);
	FSClose(file);
	text[count] = 0;
	options->suites = BENCHSUITEALL;
	options->automatic = true;
	line = text;
	while (*line) {
		end = strpbrk(line, "\r\n");
		if (end)
			*end = 0;
		BenchParseOption(options, line);
		if (end == NULL)
			break;
		line = end + 1;
	}
	return true;
}

static void BenchAsk(Bench *bench, BenchOptions *options)
{
	char answer[200];
	int index;
	printf("Suites:");
	for (index = 0; index < BENCHSUITECOUNT; index++)
		printf(" %s", benchsuites[index].name);
	printf("\nWhich suites (Return for all)? ");
	fflush(stdout);
	answer[0] = 0;
	fgets(answer, sizeof(answer), stdin);
	options->suites = BenchParseSuites(answer);
	if (options->suites == 0)
		options->suites = BENCHSUITEALL;
	printf("Quick run (y/N)? ");
	fflush(stdout);
	answer[0] = 0;
	fgets(answer, sizeof(answer), stdin);
	options->quick = answer[0] == 'y' || answer[0] == 'Y';
	if (BenchHostIs(bench, "benchswitch", "1") && BenchHostIs(bench, "jit", "on")) {
		printf("Also run with the JIT off (y/N)? ");
		fflush(stdout);
		answer[0] = 0;
		fgets(answer, sizeof(answer), stdin);
		options->jitoff = answer[0] == 'y' || answer[0] == 'Y';
	}
}

static void BenchHostLines(Bench *bench)
{
	char line[200];
	const char *text;
	const char *end;
	size_t length;
	if (!BenchHostRefresh(bench)) {
		BenchLine(bench, "host      no Sheep Shears mailbox, host comparisons skipped");
		return;
	}
	text = bench->hostinfo;
	while (*text) {
		end = strchr(text, '\n');
		length = strlen(text);
		if (end)
			length = (size_t)(end - text);
		if (length > sizeof(line) - 16)
			length = sizeof(line) - 16;
		strcpy(line, "host      ");
		memcpy(line + 10, text, length);
		line[10 + length] = 0;
		BenchLine(bench, line);
		if (end == NULL)
			break;
		text = end + 1;
	}
}

static void BenchRules(Bench *bench)
{
	bench->minrep = 0.020;
	bench->minreps = 5;
	bench->maxreps = 25;
	bench->budget = 3.0;
	if (bench->quick) {
		bench->minrep = 0.010;
		bench->maxreps = 9;
		bench->budget = 1.0;
	}
}

static void BenchRunPass(Bench *bench)
{
	int index;
	char line[80];
	for (index = 0; index < BENCHSUITECOUNT && !bench->cancelled; index++) {
		if (!(bench->suites & benchsuites[index].flag))
			continue;
		snprintf(line, sizeof(line), "starting %s suite", benchsuites[index].name);
		if (bench->pass == BENCHPASSJITOFF)
			strcat(line, " [JIT off]");
		BenchLine(bench, line);
		benchsuites[index].run(bench);
	}
}

static void BenchSummary(Bench *bench)
{
	BenchRow *row;
	char line[300];
	char on[24];
	char off[24];
	char speedup[24];
	double ratio;
	int index;
	BenchLine(bench, "JIT speedup (JIT on / JIT off):");
	for (index = 0; index < bench->rowcount; index++) {
		row = &bench->rows[index];
		if (row->value[BENCHPASSMAIN] <= 0 || row->value[BENCHPASSJITOFF] <= 0)
			continue;
		ratio = row->value[BENCHPASSMAIN] / row->value[BENCHPASSJITOFF];
		if (row->unit == BENCHUNITNSINSN || row->unit == BENCHUNITNS || row->unit == BENCHUNITUS)
			ratio = row->value[BENCHPASSJITOFF] / row->value[BENCHPASSMAIN];
		BenchNumber(on, row->value[BENCHPASSMAIN]);
		BenchNumber(off, row->value[BENCHPASSJITOFF]);
		BenchNumber(speedup, ratio);
		snprintf(line, sizeof(line), "%-9s %-50s %10s %10s %-7s %sx", row->suite, row->test, on, off,
			BenchUnitName(row->unit), speedup);
		BenchLine(bench, line);
	}
}

static const char *BenchStatusName(int status)
{
	static const char *names[4] = {"not run", "ok", "skipped", "FAILED"};
	if (status < 0 || status > 3)
		return "";
	return names[status];
}

static void BenchWriteRow(short file, BenchRow *row)
{
	char line[BENCHREPORTLINE];
	char values[4][24];
	char percent[24];
	double native;
	long count;
	BenchNumber(values[0], row->value[BENCHPASSMAIN]);
	BenchNumber(values[1], row->noise[BENCHPASSMAIN]);
	BenchNumber(values[2], BENCHNOVALUE);
	BenchNumber(percent, BENCHNOVALUE);
	if (row->native > 0 && row->value[BENCHPASSMAIN] > 0 && row->work > 0) {
		native = BenchUnitValue(row->unit, row->native * 1e-9, row->work);
		BenchNumber(values[2], native);
		if (row->unit == BENCHUNITNSINSN || row->unit == BENCHUNITNS || row->unit == BENCHUNITUS)
			BenchNumber(percent, native / row->value[BENCHPASSMAIN] * 100.0);
		else
			BenchNumber(percent, row->value[BENCHPASSMAIN] / native * 100.0);
	}
	BenchNumber(values[3], row->value[BENCHPASSJITOFF]);
	snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\r", row->suite, row->test,
		BenchUnitName(row->unit), values[0], values[1], row->reps[BENCHPASSMAIN], values[2], percent, values[3],
		BenchStatusName(row->status), row->why);
	count = (long)strlen(line);
	FSWrite(file, &count, line);
}

static void BenchWriteReport(Bench *bench)
{
	FSSpec spec;
	short file;
	long count;
	int index;
	const char *header;
	FSMakeFSSpec(0, 0, "\pSheepBench Report", &spec);
	FSpDelete(&spec);
	if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr || FSpOpenDF(&spec, fsRdWrPerm, &file) != noErr)
		return;
	header = "suite\ttest\tunit\tvalue\tnoise_pct\treps\tnative\tpct_native\tjit_off\tstatus\twhy\r";
	count = (long)strlen(header);
	FSWrite(file, &count, header);
	for (index = 0; index < bench->rowcount; index++)
		BenchWriteRow(file, &bench->rows[index]);
	FSClose(file);
}

static OSErr BenchShutDown(void)
{
	AEAddressDesc target;
	AppleEvent event;
	AppleEvent reply;
	OSType finder;
	OSErr err;
	finder = 'MACS';
	err = AECreateDesc(typeApplSignature, &finder, sizeof(finder), &target);
	if (err != noErr)
		return err;
	err = AECreateAppleEvent('FNDR', 'shut', &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
	AEDisposeDesc(&target);
	if (err != noErr)
		return err;
	reply.descriptorType = typeNull;
	reply.dataHandle = NULL;
	err = AESend(&event, &reply, kAENoReply, kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
	AEDisposeDesc(&event);
	return err;
}

int main(void)
{
	Bench *bench;
	BenchOptions options;
	char line[128];
	bench = (Bench *)NewPtrClear(sizeof(Bench));
	if (bench == NULL)
		return 1;
	bench->rows = (BenchRow *)NewPtrClear(sizeof(BenchRow) * BENCHMAXROWS);
	if (bench->rows == NULL)
		return 1;
	bench->mailbox = ShearsOpen();
	BenchHostRefresh(bench);
	BenchCalibrate(bench);
	memset(&options, 0, sizeof(options));
	if (!BenchReadAuto(&options))
		BenchAsk(bench, &options);
	bench->suites = options.suites;
	bench->quick = options.quick;
	BenchRules(bench);
	snprintf(line, sizeof(line), "SheepBench %s", BENCHVERSION);
	BenchLine(bench, line);
	snprintf(line, sizeof(line), "timebase %ld Hz", (long)bench->timebase);
	BenchLine(bench, line);
	BenchHostLines(bench);
	bench->pass = BENCHPASSMAIN;
	BenchRunPass(bench);
	if (options.jitoff && !bench->cancelled) {
		if (BenchHostIs(bench, "benchswitch", "1") && BenchHostSetJit(bench, false)) {
			bench->pass = BENCHPASSJITOFF;
			BenchRunPass(bench);
			BenchHostSetJit(bench, true);
			bench->pass = BENCHPASSMAIN;
			BenchSummary(bench);
		} else
			BenchLine(bench, "JIT-off pass skipped: the host did not allow switching (pref benchswitch)");
	}
	if (bench->cancelled)
		BenchLine(bench, "stopped with Command-period");
	BenchWriteReport(bench);
	snprintf(line, sizeof(line), "done: %d failures", bench->failures);
	BenchLine(bench, line);
	if (options.automatic) {
		if (options.shutdown)
			BenchShutDown();
		return 0;
	}
	printf("Press Return to quit.\n");
	getchar();
	return 0;
}
