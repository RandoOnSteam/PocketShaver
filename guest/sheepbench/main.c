/*
 *  main.c - SheepBench, a Mac OS 9 benchmark for SheepShaver
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
#include <Events.h>
#include <Quickdraw.h>
#include <QDOffscreen.h>
#include <Windows.h>
#include <OSUtils.h>
#include "../shearsclient.h"
#include "sbkernels.h"

#define BENCHTIMESECONDS 0.25
#define BENCHPATCHCALLS 20000
#define BENCHINTEGRITYRUNS 300
#define BENCHDISKBYTES (4L * 1024 * 1024)
#define BENCHDISKCHUNK (64L * 1024)
#define BENCHREPORTMAX 8192

typedef struct Bench {
	ShearsMailbox *mailbox;
	double timebase;
	int failures;
	char report[BENCHREPORTMAX];
	long reportlength;
} Bench;

typedef struct BenchKernel {
	const char *suite;
	uint32_t kernel;
	uint32_t iterations;
	uint32_t param;
	const char *unit;
	double work;
} BenchKernel;

static long CallCode(unsigned long *code)
{
	register long result __asm__("r3");
	__asm__ volatile("mtctr %1\n\tbctrl" : "=r"(result) : "r"(code) : "ctr", "lr", "r0", "r4", "r5", "r6", "r7", "r8", "r9",
		"r10", "r11", "r12", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return result;
}

typedef struct BenchMemory {
	void *pointer;
	Handle temporary;
} BenchMemory;

static void *BenchAllocate(BenchMemory *memory, Size bytes)
{
	OSErr err;
	memory->temporary = NULL;
	memory->pointer = NewPtr(bytes);
	if (memory->pointer)
		return memory->pointer;
	memory->temporary = TempNewHandle(bytes, &err);
	if (memory->temporary == NULL)
		return NULL;
	HLock(memory->temporary);
	memory->pointer = *memory->temporary;
	return memory->pointer;
}

static void BenchRelease(BenchMemory *memory)
{
	if (memory->temporary)
		DisposeHandle(memory->temporary);
	else if (memory->pointer)
		DisposePtr((Ptr)memory->pointer);
	memory->pointer = NULL;
	memory->temporary = NULL;
}

static unsigned long long TimebaseNow(void)
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

static double Seconds(Bench *bench, unsigned long long start)
{
	return (double)(TimebaseNow() - start) / bench->timebase;
}

static void TerminatePayload(ShearsMailbox *mailbox)
{
	unsigned long length;
	length = mailbox->length;
	if (length > SHEARSPAYLOAD - 1)
		length = SHEARSPAYLOAD - 1;
	mailbox->payload[length] = 0;
}

static void BenchLine(Bench *bench, const char *text)
{
	char line[256];
	long length;
	printf("%s\n", text);
	length = (long)strlen(text);
	if (bench->reportlength + length + 1 < BENCHREPORTMAX) {
		memcpy(bench->report + bench->reportlength, text, (size_t)length);
		bench->reportlength += length;
		bench->report[bench->reportlength++] = '\r';
	}
	if (bench->mailbox) {
		sprintf(line, "SheepBench: %s\n", text);
		ShearsCall(bench->mailbox, SHEARSCMDREPORT, line, strlen(line));
	}
}

static void Fixed1(char *text, double value)
{
	long scaled;
	scaled = (long)(value * 10.0 + 0.5);
	sprintf(text, "%ld.%ld", scaled / 10, scaled % 10);
}

static void CalibrateTimebase(Bench *bench)
{
	const char *info;
	const char *found;
	unsigned long start;
	unsigned long long first;
	bench->timebase = 0;
	if (bench->mailbox && ShearsCall(bench->mailbox, SHEARSCMDINFO, NULL, 0) == 0) {
		TerminatePayload(bench->mailbox);
		info = (const char *)bench->mailbox->payload;
		found = strstr(info, "timebase=");
		if (found)
			bench->timebase = atof(found + 9);
	}
	if (bench->timebase > 1000.0)
		return;
	start = TickCount();
	while (TickCount() == start)
		continue;
	first = TimebaseNow();
	start = TickCount();
	while (TickCount() < start + 30)
		continue;
	bench->timebase = (double)(TimebaseNow() - first) / (30.0 / 60.15);
}

static void HostInfo(Bench *bench)
{
	char line[256];
	char info[512];
	char *text;
	char *next;
	if (bench->mailbox == NULL || ShearsCall(bench->mailbox, SHEARSCMDINFO, NULL, 0) != 0) {
		BenchLine(bench, "host: no Sheep Shears mailbox, host comparisons skipped");
		return;
	}
	TerminatePayload(bench->mailbox);
	strncpy(info, (const char *)bench->mailbox->payload, sizeof(info) - 1);
	info[sizeof(info) - 1] = 0;
	text = info;
	while (*text) {
		next = strchr(text, '\n');
		if (next)
			*next = 0;
		snprintf(line, sizeof(line), "host %s", text);
		BenchLine(bench, line);
		if (next == NULL)
			break;
		text = next + 1;
	}
}

static int HostNative(Bench *bench, const BenchKernel *test, uint32_t *checksum, double *nanoseconds)
{
	unsigned long request[3];
	unsigned long long picoseconds;
	if (bench->mailbox == NULL)
		return 0;
	request[0] = test->kernel;
	request[1] = test->iterations;
	request[2] = test->param;
	if (ShearsCall(bench->mailbox, SHEARSCMDNATIVE, request, sizeof(request)) != 0)
		return 0;
	*checksum = ShearsWord(bench->mailbox, 0);
	picoseconds = ((unsigned long long)ShearsWord(bench->mailbox, 1) << 32) | ShearsWord(bench->mailbox, 2);
	*nanoseconds = (double)picoseconds / 1000.0;
	return 1;
}

static void RunKernel(Bench *bench, const BenchKernel *test)
{
	char line[256];
	char guesttext[32];
	char nativetext[32];
	char ratetext[32];
	char percenttext[32];
	unsigned long long start;
	double elapsed;
	double guestns;
	double nativens;
	uint32_t hostsum;
	uint32_t guestsum;
	uint32_t repeats;
	uint32_t index;
	int hasnative;
	void *scratch;
	BenchMemory memory;
	scratch = BenchAllocate(&memory, (Size)SbScratchBytes(test->kernel, test->param) + 16);
	if (scratch == NULL) {
		sprintf(line, "%-8s %-14s param %-8lu skipped: no memory", test->suite, SbName(test->kernel), (unsigned long)test->param);
		BenchLine(bench, line);
		return;
	}
	SbPrepare(test->kernel, test->param, scratch);
	guestsum = SbRun(test->kernel, test->iterations, test->param, scratch);
	repeats = 1;
	for (;;) {
		start = TimebaseNow();
		for (index = 0; index < repeats; index++)
			SbRun(test->kernel, test->iterations, test->param, scratch);
		elapsed = Seconds(bench, start);
		if (elapsed >= BENCHTIMESECONDS || repeats >= 0x1000000UL)
			break;
		repeats *= 2;
	}
	BenchRelease(&memory);
	guestns = elapsed * 1e9 / ((double)repeats * test->iterations);
	Fixed1(guesttext, guestns);
	Fixed1(ratetext, test->work * 1e3 / guestns);
	hasnative = HostNative(bench, test, &hostsum, &nativens);
	if (hasnative && hostsum != guestsum) {
		bench->failures++;
		sprintf(line, "%-8s %-14s param %-8lu FAIL checksum %08lx host %08lx", test->suite, SbName(test->kernel),
			(unsigned long)test->param, (unsigned long)guestsum, (unsigned long)hostsum);
		BenchLine(bench, line);
		return;
	}
	if (hasnative) {
		Fixed1(nativetext, nativens);
		Fixed1(percenttext, nativens * 100.0 / guestns);
		sprintf(line, "%-8s %-14s param %-8lu %10s ns  %10s %s  native %8s ns  %6s%% of native", test->suite,
			SbName(test->kernel), (unsigned long)test->param, guesttext, ratetext, test->unit, nativetext, percenttext);
	} else
		sprintf(line, "%-8s %-14s param %-8lu %10s ns  %10s %s", test->suite, SbName(test->kernel),
			(unsigned long)test->param, guesttext, ratetext, test->unit);
	BenchLine(bench, line);
}

static void RunKernels(Bench *bench)
{
	static const BenchKernel tests[] = {
		{"cpu", SBKERNELINT, 100000, 0, "M/s", 1.0},
		{"cpu", SBKERNELCALL, 100000, 0, "Mcalls/s", 1.0},
		{"fpu", SBKERNELDOUBLE, 100000, 0, "Mflop/s", 2.0},
		{"fpu", SBKERNELFLOAT, 100000, 0, "Mflop/s", 2.0},
		{"memory", SBKERNELBYTECOPY, 4, 4096, "MB/s", 4096.0},
		{"memory", SBKERNELCOPY, 4, 65536, "MB/s", 65536.0},
		{"memory", SBKERNELCOPY, 1, 4194304, "MB/s", 4194304.0},
		{"memory", SBKERNELFILL, 4, 65536, "MB/s", 65536.0},
		{"memory", SBKERNELFILL, 1, 4194304, "MB/s", 4194304.0},
		{"memory", SBKERNELCHASE, 100000, 4096, "Mloads/s", 1.0},
		{"memory", SBKERNELCHASE, 100000, 1048576, "Mloads/s", 1.0}
	};
	unsigned int index;
	for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
		RunKernel(bench, &tests[index]);
}

static void RunIntegrity(Bench *bench, uint32_t kernel)
{
	BenchKernel test;
	char line[256];
	double nanoseconds;
	uint32_t expected;
	uint32_t result;
	uint32_t first;
	long bad;
	long index;
	test.suite = "fpu";
	test.kernel = kernel;
	test.iterations = 100000;
	test.param = 0;
	test.unit = "";
	test.work = 1.0;
	if (!HostNative(bench, &test, &expected, &nanoseconds))
		return;
	bad = 0;
	first = expected;
	for (index = 0; index < BENCHINTEGRITYRUNS; index++) {
		result = SbRun(kernel, test.iterations, 0, NULL);
		if (result != expected) {
			if (bad == 0)
				first = result;
			bad++;
		}
	}
	if (bad)
		bench->failures++;
	sprintf(line, "fpu      %-14s %ld runs  wrong results %ld  first wrong %08lx expected %08lx", SbName(kernel),
		(long)BENCHINTEGRITYRUNS, bad, (unsigned long)first, (unsigned long)expected);
	BenchLine(bench, line);
}

static void RunCodePatch(Bench *bench)
{
	char line[256];
	char ratetext[32];
	unsigned long *code;
	unsigned long long start;
	long value;
	long stale;
	long index;
	double elapsed;
	code = (unsigned long *)NewPtr(64);
	if (code == NULL)
		return;
	stale = 0;
	start = TimebaseNow();
	for (index = 0; index < BENCHPATCHCALLS; index++) {
		value = index & 0x7fff;
		code[0] = 0x38600000UL | (unsigned long)value;
		code[1] = 0x4e800020UL;
		MakeDataExecutable(code, 8);
		if (CallCode(code) != value)
			stale++;
	}
	elapsed = Seconds(bench, start);
	DisposePtr((Ptr)code);
	Fixed1(ratetext, (double)BENCHPATCHCALLS / elapsed / 1000.0);
	if (stale)
		bench->failures++;
	sprintf(line, "jit      code patch     %ld calls  %s K/s  stale results %ld", (long)BENCHPATCHCALLS, ratetext, stale);
	BenchLine(bench, line);
}

static void QuickDrawRate(Bench *bench, const char *name, int operation, WindowPtr window, GWorldPtr world)
{
	char line[256];
	char ratetext[32];
	Rect rect;
	Rect source;
	RGBColor color;
	unsigned long long start;
	double elapsed;
	long count;
	SetPort((GrafPtr)window);
	count = 0;
	start = TimebaseNow();
	do {
		SetRect(&rect, (short)((count * 37) % 448), (short)((count * 53) % 320), 0, 0);
		rect.right = rect.left + 64;
		rect.bottom = rect.top + 64;
		color.red = (unsigned short)(count * 4099);
		color.green = (unsigned short)(count * 7919);
		color.blue = (unsigned short)(count * 104729);
		RGBForeColor(&color);
		if (operation == 0)
			PaintRect(&rect);
		else if (operation == 1)
			FillRect(&rect, &qd.ltGray);
		else if (operation == 2) {
			SetRect(&source, 0, 0, 256, 256);
			SetRect(&rect, (short)((count * 37) % 256), (short)((count * 53) % 128), 0, 0);
			rect.right = rect.left + 256;
			rect.bottom = rect.top + 256;
			ForeColor(blackColor);
			CopyBits((BitMap *)*GetGWorldPixMap(world), &((GrafPtr)window)->portBits, &source, &rect, srcCopy, NULL);
		} else if (operation == 3) {
			MoveTo(rect.left, (short)(rect.top + 20));
			DrawString("\pThe quick brown fox");
		} else {
			MoveTo(rect.left, rect.top);
			LineTo(rect.right, rect.bottom);
		}
		count++;
		elapsed = Seconds(bench, start);
	} while (elapsed < BENCHTIMESECONDS);
	ForeColor(blackColor);
	Fixed1(ratetext, (double)count / elapsed / 1000.0);
	sprintf(line, "qd       %-14s %10s Kops/s", name, ratetext);
	BenchLine(bench, line);
}

static void RunQuickDraw(Bench *bench)
{
	WindowPtr window;
	GWorldPtr world;
	Rect bounds;
	SetRect(&bounds, 60, 80, 572, 464);
	window = NewCWindow(NULL, &bounds, "\pSheepBench", true, documentProc, (WindowPtr)-1, false, 0);
	if (window == NULL)
		return;
	SetRect(&bounds, 0, 0, 256, 256);
	world = NULL;
	if (NewGWorld(&world, 32, &bounds, NULL, NULL, 0) != noErr)
		world = NULL;
	QuickDrawRate(bench, "paint rect", 0, window, world);
	QuickDrawRate(bench, "pattern fill", 1, window, world);
	if (world) {
		LockPixels(GetGWorldPixMap(world));
		QuickDrawRate(bench, "copybits", 2, window, world);
		UnlockPixels(GetGWorldPixMap(world));
		DisposeGWorld(world);
	}
	QuickDrawRate(bench, "draw string", 3, window, world);
	QuickDrawRate(bench, "line", 4, window, world);
	DisposeWindow(window);
}

static void RunDisk(Bench *bench)
{
	char line[256];
	char writetext[32];
	char readtext[32];
	FSSpec spec;
	unsigned char *buffer;
	unsigned long long start;
	double writeseconds;
	double readseconds;
	short file;
	long offset;
	long count;
	long index;
	long bad;
	OSErr err;
	buffer = (unsigned char *)NewPtr(BENCHDISKCHUNK);
	if (buffer == NULL)
		return;
	FSMakeFSSpec(0, 0, "\pSheepBench Scratch", &spec);
	FSpDelete(&spec);
	err = FSpCreate(&spec, 'ShBn', 'BINA', 0);
	if (err == noErr)
		err = FSpOpenDF(&spec, fsRdWrPerm, &file);
	if (err != noErr) {
		sprintf(line, "disk     skipped: error %d", (int)err);
		BenchLine(bench, line);
		DisposePtr((Ptr)buffer);
		return;
	}
	start = TimebaseNow();
	for (offset = 0; offset < BENCHDISKBYTES && err == noErr; offset += BENCHDISKCHUNK) {
		for (index = 0; index < BENCHDISKCHUNK; index += 4)
			*(long *)(buffer + index) = offset + index;
		count = BENCHDISKCHUNK;
		err = FSWrite(file, &count, buffer);
	}
	FlushVol(NULL, spec.vRefNum);
	writeseconds = Seconds(bench, start);
	SetFPos(file, fsFromStart, 0);
	bad = 0;
	start = TimebaseNow();
	for (offset = 0; offset < BENCHDISKBYTES && err == noErr; offset += BENCHDISKCHUNK) {
		count = BENCHDISKCHUNK;
		err = FSRead(file, &count, buffer);
		for (index = 0; index < count; index += 4)
			if (*(long *)(buffer + index) != offset + index)
				bad++;
	}
	readseconds = Seconds(bench, start);
	FSClose(file);
	FSpDelete(&spec);
	DisposePtr((Ptr)buffer);
	if (err != noErr || bad) {
		bench->failures++;
		sprintf(line, "disk     FAIL error %d, %ld bad words", (int)err, bad);
		BenchLine(bench, line);
		return;
	}
	Fixed1(writetext, (double)BENCHDISKBYTES / writeseconds / 1048576.0);
	Fixed1(readtext, (double)BENCHDISKBYTES / readseconds / 1048576.0);
	sprintf(line, "disk     4 MB file      write %s MB/s  read %s MB/s", writetext, readtext);
	BenchLine(bench, line);
}

static void SaveReport(Bench *bench)
{
	FSSpec spec;
	short file;
	long count;
	FSMakeFSSpec(0, 0, "\pSheepBench Results", &spec);
	FSpDelete(&spec);
	if (FSpCreate(&spec, 'ttxt', 'TEXT', 0) != noErr || FSpOpenDF(&spec, fsRdWrPerm, &file) != noErr)
		return;
	count = bench->reportlength;
	FSWrite(file, &count, bench->report);
	FSClose(file);
}

int main(void)
{
	Bench *bench;
	char line[128];
	bench = (Bench *)NewPtrClear(sizeof(Bench));
	if (bench == NULL)
		return 1;
	bench->mailbox = ShearsOpen();
	CalibrateTimebase(bench);
	BenchLine(bench, "SheepBench 1");
	sprintf(line, "timebase %ld Hz", (long)bench->timebase);
	BenchLine(bench, line);
	HostInfo(bench);
	RunKernels(bench);
	RunIntegrity(bench, SBKERNELDOUBLE);
	RunIntegrity(bench, SBKERNELFLOAT);
	BenchLine(bench, "starting code patch test");
	RunCodePatch(bench);
	BenchLine(bench, "starting quickdraw test");
	RunQuickDraw(bench);
	BenchLine(bench, "starting disk test");
	RunDisk(bench);
	sprintf(line, "done: %d failures", bench->failures);
	BenchLine(bench, line);
	SaveReport(bench);
	printf("Press Return to quit.\n");
	getchar();
	return 0;
}
