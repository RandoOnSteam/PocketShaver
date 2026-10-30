/*
 *  suitequickdraw.c - SheepBench QuickDraw suite, checked against QuickDraw's own offscreen drawing
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
#include <Quickdraw.h>
#include <QDOffscreen.h>
#include <Windows.h>
#include <Fonts.h>
#include <Icons.h>
#include "bench.h"

#define QDPAINT 0
#define QDINVERT 1
#define QDPATTERN 2
#define QDFRAME 3
#define QDLINES 4
#define QDCOPY32 5
#define QDCOPY16 6
#define QDCOPY8 7
#define QDXOR 8
#define QDOR 9
#define QDSCROLL 10
#define QDTEXT 11
#define QDICON 12
#define QDCOUNT 13

typedef struct QdOperation {
	const char *name;
	int count;
} QdOperation;

typedef struct QdContext {
	WindowPtr window;
	Rect area;
	GWorldPtr source32;
	GWorldPtr source16;
	GWorldPtr source8;
	GWorldPtr sync;
	int operation;
} QdContext;

static const QdOperation qdoperations[QDCOUNT] = {
	{"PaintRect, solid", 1},
	{"InvertRect", 1},
	{"FillRect, gray pattern", 1},
	{"FrameRect, 16 nested", 16},
	{"LineTo, 32 diagonals", 32},
	{"CopyBits srcCopy, 32-bit source", 1},
	{"CopyBits srcCopy, 16-bit source", 1},
	{"CopyBits srcCopy, 8-bit source", 1},
	{"CopyBits srcXor, 32-bit source", 1},
	{"CopyBits srcOr, 32-bit source", 1},
	{"ScrollRect by 8 (overlapping blit)", 1},
	{"DrawString, 12 lines of Geneva 12", 12},
	{"PlotIconID, 32 x 32 icons", 16}
};

static void QdColor(int index)
{
	static const RGBColor colors[4] = {{0xffff, 0x0000, 0x0000}, {0x0000, 0x8000, 0xffff}, {0x2000, 0xc000, 0x2000},
		{0x0000, 0x0000, 0x0000}};
	RGBForeColor(&colors[index & 3]);
}

static BitMap *QdBits(GWorldPtr world)
{
	return (BitMap *)*GetGWorldPixMap(world);
}

static BitMap *QdWindowBits(WindowPtr window)
{
	return &((GrafPtr)window)->portBits;
}

static GWorldPtr QdSource(QdContext *context, int operation)
{
	if (operation == QDCOPY16)
		return context->source16;
	if (operation == QDCOPY8)
		return context->source8;
	return context->source32;
}

static short QdMode(int operation)
{
	if (operation == QDXOR)
		return srcXor;
	if (operation == QDOR)
		return srcOr;
	return srcCopy;
}

static void QdDraw(QdContext *context, int operation, int index)
{
	Rect rect;
	Rect source;
	RgnHandle update;
	GWorldPtr world;
	int count;
	int width;
	int height;
	rect = context->area;
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (operation == QDPAINT) {
		QdColor(index);
		PaintRect(&rect);
	} else if (operation == QDINVERT)
		InvertRect(&rect);
	else if (operation == QDPATTERN) {
		QdColor(index);
		FillRect(&rect, &qd.gray);
	} else if (operation == QDFRAME) {
		QdColor(index);
		for (count = 0; count < 16 && rect.right - rect.left > 2 && rect.bottom - rect.top > 2; count++) {
			FrameRect(&rect);
			InsetRect(&rect, width / 40 + 1, height / 40 + 1);
		}
	} else if (operation == QDLINES) {
		QdColor(index);
		for (count = 0; count < 32; count++) {
			MoveTo(0, (short)(count * height / 32));
			LineTo((short)(width - 1), (short)(height - 1 - count * height / 32));
		}
	} else if (operation >= QDCOPY32 && operation <= QDOR) {
		world = QdSource(context, operation);
		source = (*GetGWorldPixMap(world))->bounds;
		ForeColor(blackColor);
		BackColor(whiteColor);
		CopyBits(QdBits(world), &qd.thePort->portBits, &source, &rect, QdMode(operation), NULL);
	} else if (operation == QDSCROLL) {
		update = NewRgn();
		ScrollRect(&rect, 0, 8, update);
		DisposeRgn(update);
	} else if (operation == QDTEXT) {
		QdColor(index);
		TextFont(kFontIDGeneva);
		TextSize(12);
		for (count = 0; count < 12; count++) {
			MoveTo(4, (short)(14 + count * 15));
			DrawString("\pThe quick brown fox jumps over the lazy dog 0123456789");
		}
	} else {
		for (count = 0; count < 16; count++) {
			SetRect(&source, (short)((count % 4) * 36), (short)((count / 4) * 36), (short)((count % 4) * 36 + 32),
				(short)((count / 4) * 36 + 32));
			PlotIconID(&source, 0, 0, (short)(-3999 - (count & 1)));
		}
	}
}

static void QdSync(QdContext *context)
{
	Rect one;
	CGrafPtr port;
	GDHandle device;
	SetRect(&one, 0, 0, 1, 1);
	GetGWorld(&port, &device);
	SetGWorld(context->sync, NULL);
	CopyBits(QdWindowBits(context->window), QdBits(context->sync), &one, &one, srcCopy, NULL);
	SetGWorld(port, device);
}

static uint32_t QdBody(void *pointer, uint32_t iterations)
{
	QdContext *context;
	uint32_t index;
	context = (QdContext *)pointer;
	SetPort((GrafPtr)context->window);
	for (index = 0; index < iterations; index++)
		QdDraw(context, context->operation, (int)index);
	QdSync(context);
	return iterations;
}

static GWorldPtr QdNewWorld(short depth, short width, short height)
{
	GWorldPtr world;
	Rect bounds;
	world = NULL;
	SetRect(&bounds, 0, 0, width, height);
	if (NewGWorld(&world, depth, &bounds, NULL, NULL, 0) != noErr || world == NULL) {
		world = NULL;
		if (NewGWorld(&world, depth, &bounds, NULL, NULL, useTempMem) != noErr || world == NULL)
			return NULL;
	}
	LockPixels(GetGWorldPixMap(world));
	return world;
}

static void QdFillSource(GWorldPtr world)
{
	CGrafPtr port;
	GDHandle device;
	Rect bounds;
	Rect band;
	int index;
	bounds = (*GetGWorldPixMap(world))->bounds;
	GetGWorld(&port, &device);
	SetGWorld(world, NULL);
	EraseRect(&bounds);
	for (index = 0; index < 8; index++) {
		band = bounds;
		band.top = (short)(bounds.top + index * (bounds.bottom - bounds.top) / 8);
		band.bottom = (short)(bounds.top + (index + 1) * (bounds.bottom - bounds.top) / 8);
		QdColor(index);
		PaintRect(&band);
	}
	ForeColor(blackColor);
	for (index = 0; index < 64; index++) {
		SetRect(&band, (short)((index % 8) * 16), (short)((index / 8) * 16), (short)((index % 8) * 16 + 8),
			(short)((index / 8) * 16 + 8));
		if ((index + index / 8) & 1)
			PaintRect(&band);
	}
	SetGWorld(port, device);
}

static void QdReset(void)
{
	ForeColor(blackColor);
	BackColor(whiteColor);
	PenNormal();
	TextFont(0);
	TextSize(0);
}

static void QdDrawSet(QdContext *context)
{
	int index;
	QdReset();
	EraseRect(&context->area);
	if (context->operation >= QDCOPY32 && context->operation <= QDOR)
		QdDraw(context, QDPAINT, 2);
	for (index = 0; index < 3; index++)
		QdDraw(context, context->operation, index);
	QdReset();
}

static Boolean QdCompare(QdContext *context, GWorldPtr reference, GWorldPtr back, GWorldPtr mask, char *why)
{
	PixMapPtr referencemap;
	PixMapPtr backmap;
	PixMapPtr maskmap;
	unsigned char *referencebits;
	unsigned char *backbits;
	unsigned char *maskbits;
	unsigned char *want;
	unsigned char *got;
	unsigned long wantvalue;
	unsigned long gotvalue;
	long seen;
	int bytes;
	int skip;
	int referencerow;
	int backrow;
	int maskrow;
	int x;
	int y;
	int index;
	referencemap = *GetGWorldPixMap(reference);
	backmap = *GetGWorldPixMap(back);
	maskmap = *GetGWorldPixMap(mask);
	bytes = referencemap->pixelSize / 8;
	if (bytes < 1 || backmap->pixelSize != referencemap->pixelSize) {
		snprintf(why, 100, "can only compare 8, 16 or 32-bit screens (%d)", referencemap->pixelSize);
		return false;
	}
	skip = 0;
	if (bytes == 4)
		skip = 1;
	referencerow = referencemap->rowBytes & 0x3fff;
	backrow = backmap->rowBytes & 0x3fff;
	maskrow = maskmap->rowBytes & 0x3fff;
	referencebits = (unsigned char *)GetPixBaseAddr(GetGWorldPixMap(reference));
	backbits = (unsigned char *)GetPixBaseAddr(GetGWorldPixMap(back));
	maskbits = (unsigned char *)GetPixBaseAddr(GetGWorldPixMap(mask));
	seen = 0;
	for (y = 0; y < context->area.bottom; y++) {
		for (x = 0; x < context->area.right; x++) {
			if (!(maskbits[y * maskrow + x / 8] & (0x80 >> (x & 7))))
				continue;
			seen++;
			want = referencebits + y * referencerow + x * bytes;
			got = backbits + y * backrow + x * bytes;
			if (memcmp(want + skip, got + skip, bytes - skip) != 0) {
				wantvalue = 0;
				gotvalue = 0;
				for (index = 0; index < bytes; index++) {
					wantvalue = (wantvalue << 8) | want[index];
					gotvalue = (gotvalue << 8) | got[index];
				}
				snprintf(why, 100, "screen %lx, QuickDraw's own drawing %lx at %d,%d", gotvalue, wantvalue, x, y);
				return false;
			}
		}
	}
	if (seen == 0) {
		strcpy(why, "the test window is not visible");
		return false;
	}
	return true;
}

static Boolean QdVerify(QdContext *context, char *why)
{
	GWorldPtr reference;
	GWorldPtr back;
	GWorldPtr mask;
	CGrafPtr port;
	GDHandle device;
	Boolean ok;
	short width;
	short height;
	width = context->area.right;
	height = context->area.bottom;
	reference = QdNewWorld(0, width, height);
	back = QdNewWorld(0, width, height);
	mask = QdNewWorld(1, width, height);
	why[0] = 0;
	ok = false;
	if (reference == NULL || back == NULL || mask == NULL)
		strcpy(why, "no memory for the check");
	else {
		GetGWorld(&port, &device);
		SetPort((GrafPtr)context->window);
		QdDrawSet(context);
		SetGWorld(reference, NULL);
		QdDrawSet(context);
		SetGWorld(back, NULL);
		CopyBits(QdWindowBits(context->window), QdBits(back), &context->area, &context->area, srcCopy, NULL);
		SetGWorld(mask, NULL);
		EraseRect(&context->area);
		ForeColor(blackColor);
		PaintRgn(((GrafPtr)context->window)->visRgn);
		SetGWorld(port, device);
		ok = QdCompare(context, reference, back, mask, why);
	}
	if (reference)
		DisposeGWorld(reference);
	if (back)
		DisposeGWorld(back);
	if (mask)
		DisposeGWorld(mask);
	return ok;
}

static void QdRunSize(Bench *bench, const char *sizename, short width, short height, short top, short left)
{
	QdContext context;
	BenchMeasurement measurement;
	BenchRow *row;
	Rect bounds;
	char name[80];
	char why[100];
	int operation;
	memset(&context, 0, sizeof(context));
	SetRect(&bounds, left, top, left + width, top + height);
	context.window = NewCWindow(NULL, &bounds, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
	if (context.window == NULL)
		return;
	SetRect(&context.area, 0, 0, width, height);
	context.source32 = QdNewWorld(32, 128, 128);
	context.source16 = QdNewWorld(16, 128, 128);
	context.source8 = QdNewWorld(8, 128, 128);
	context.sync = QdNewWorld(0, 1, 1);
	if (context.source32)
		QdFillSource(context.source32);
	if (context.source16)
		QdFillSource(context.source16);
	if (context.source8)
		QdFillSource(context.source8);
	for (operation = 0; operation < QDCOUNT && !bench->cancelled; operation++) {
		snprintf(name, sizeof(name), "%s, %s", qdoperations[operation].name, sizename);
		row = BenchBegin(bench, "quickdraw", name, BENCHUNITPERSEC);
		if (row == NULL)
			continue;
		if (context.sync == NULL || (operation >= QDCOPY32 && operation <= QDOR && QdSource(&context, operation) == NULL)) {
			BenchSkip(bench, row, "no memory for the offscreen pictures");
			continue;
		}
		context.operation = operation;
		if (!QdVerify(&context, why)) {
			BenchFail(bench, row, why);
			continue;
		}
		SetPort((GrafPtr)context.window);
		QdReset();
		BenchMeasure(bench, QdBody, &context, 1, &measurement);
		QdReset();
		BenchRecord(bench, row, &measurement, (double)qdoperations[operation].count);
	}
	if (context.source32)
		DisposeGWorld(context.source32);
	if (context.source16)
		DisposeGWorld(context.source16);
	if (context.source8)
		DisposeGWorld(context.source8);
	if (context.sync)
		DisposeGWorld(context.sync);
	DisposeWindow(context.window);
}

void SuiteQuickDraw(Bench *bench)
{
	GDHandle device;
	Rect screen;
	short width;
	short height;
	device = GetMainDevice();
	screen = (**device).gdRect;
	width = screen.right - screen.left;
	height = screen.bottom - screen.top;
	HideCursor();
	QdRunSize(bench, "64 x 64", 64, 64, 48, 48);
	if (width >= 560 && height >= 560)
		QdRunSize(bench, "512 x 512", 512, 512, 48, 48);
	else
		BenchSkip(bench, BenchBegin(bench, "quickdraw", "512 x 512 tests", BENCHUNITPERSEC),
			"the screen is smaller than 560 x 560");
	QdRunSize(bench, "full screen", width, (short)(height - 24), 24, 0);
	ShowCursor();
}
