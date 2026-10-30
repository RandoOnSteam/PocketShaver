/*
 *  suite3d.c - SheepBench 3D suite: every RAVE engine that takes an offscreen ARGB32 context
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
#include <CodeFragments.h>
#include <Events.h>
#include "bench.h"

#define RAVEWIDTH 640
#define RAVEHEIGHT 480
#define RAVEMAXTRIANGLES 10000
#define RAVEMAXENGINES 6
#define RAVEDEVICEMEMORY 0
#define RAVEPIXELARGB32 4
#define RAVEGESTALTNAME 6
#define RAVETAGBLEND 9
#define RAVETAGTEXTUREFILTER 11
#define RAVETAGTEXTUREOP 12
#define RAVETAGTEXTURE 13
#define RAVETAGFOGMODE 17
#define RAVETAGFOGCOLORA 18
#define RAVETAGFOGCOLORR 19
#define RAVETAGFOGCOLORG 20
#define RAVETAGFOGCOLORB 21
#define RAVETAGFOGSTART 22
#define RAVETAGFOGEND 23
#define RAVEBLENDINTERPOLATE 1
#define RAVEFOGLINEAR 2
#define RAVEMETHODSETFLOAT 0
#define RAVEMETHODSETINT 1
#define RAVEMETHODSETPTR 2
#define RAVEMETHODTRIGOURAUD 8
#define RAVEMETHODTRITEXTURE 9
#define RAVEMETHODRENDERSTART 13
#define RAVEMETHODRENDEREND 14
#define RAVEMETHODSYNC 17
#define RAVEKINDFLAT 0
#define RAVEKINDGOURAUD 1
#define RAVEKINDTEXTURE 2
#define RAVEKINDBLEND 3
#define RAVEKINDFOG 4
#define RAVEKINDS 5

typedef struct RaveDevice {
	long devicetype;
	long rowbytes;
	long pixeltype;
	long width;
	long height;
	void *baseaddr;
} RaveDevice;

typedef struct RaveRect {
	long left;
	long right;
	long top;
	long bottom;
} RaveRect;

typedef struct RaveGouraud {
	float x;
	float y;
	float z;
	float invw;
	float r;
	float g;
	float b;
	float a;
} RaveGouraud;

typedef struct RaveTexture {
	float x;
	float y;
	float z;
	float invw;
	float r;
	float g;
	float b;
	float a;
	float uoverw;
	float voverw;
	float kdr;
	float kdg;
	float kdb;
	float ksr;
	float ksg;
	float ksb;
} RaveTexture;

typedef struct RaveImage {
	long width;
	long height;
	long rowbytes;
	void *pixmap;
} RaveImage;

typedef void *(*RaveFirstProc)(const RaveDevice *);
typedef void *(*RaveNextProc)(const RaveDevice *, void *);
typedef long (*RaveGestaltProc)(void *, long, void *);
typedef long (*RaveContextNewProc)(const RaveDevice *, const RaveRect *, void *, void *, unsigned long, void **);
typedef long (*RaveContextDeleteProc)(void *);
typedef long (*RaveTextureNewProc)(void *, unsigned long, long, const RaveImage *, void **);
typedef long (*RaveTextureDeleteProc)(void *, void *);
typedef void (*RaveSetFloatProc)(void *, long, float);
typedef void (*RaveSetIntProc)(void *, long, unsigned long);
typedef void (*RaveSetPtrProc)(void *, long, const void *);
typedef void (*RaveTriGouraudProc)(void *, const RaveGouraud *, const RaveGouraud *, const RaveGouraud *, unsigned long);
typedef void (*RaveTriTextureProc)(void *, const RaveTexture *, const RaveTexture *, const RaveTexture *, unsigned long);
typedef void (*RaveRenderStartProc)(void *, const RaveRect *, void *);
typedef long (*RaveRenderEndProc)(void *, const RaveRect *);
typedef long (*RaveSyncProc)(void *);

typedef struct RaveLibrary {
	RaveFirstProc first;
	RaveNextProc next;
	RaveGestaltProc gestalt;
	RaveContextNewProc contextnew;
	RaveContextDeleteProc contextdelete;
	RaveTextureNewProc texturenew;
	RaveTextureDeleteProc texturedelete;
} RaveLibrary;

typedef struct RaveFrame {
	void *context;
	void **methods;
	int kind;
	int triangles;
	RaveGouraud *gouraud;
	RaveTexture *texture;
} RaveFrame;

typedef struct RaveScene {
	RaveDevice device;
	uint32_t *pixels;
	RaveGouraud *flat;
	RaveGouraud *shaded;
	RaveTexture *textured;
	uint32_t *texels;
} RaveScene;

static Boolean RaveLoad(RaveLibrary *library)
{
	static const unsigned char *names[7] = {"\pQADeviceGetFirstEngine", "\pQADeviceGetNextEngine", "\pQAEngineGestalt",
		"\pQADrawContextNew", "\pQADrawContextDelete", "\pQATextureNew", "\pQATextureDelete"};
	CFragConnectionID connection;
	Ptr main;
	Ptr symbol;
	CFragSymbolClass symbolclass;
	Str255 error;
	void **slots[7];
	EventRecord record;
	unsigned long until;
	int index;
	slots[0] = (void **)&library->first;
	slots[1] = (void **)&library->next;
	slots[2] = (void **)&library->gestalt;
	slots[3] = (void **)&library->contextnew;
	slots[4] = (void **)&library->contextdelete;
	slots[5] = (void **)&library->texturenew;
	slots[6] = (void **)&library->texturedelete;
	if (GetSharedLibrary("\pQuickDraw\xaa 3D Accelerator", kPowerPCCFragArch, kReferenceCFrag, &connection, &main, error) !=
		noErr)
		return false;
	until = TickCount() + 120;
	while (TickCount() < until)
		WaitNextEvent(0, &record, 10, NULL);
	for (index = 0; index < 7; index++) {
		if (FindSymbol(connection, names[index], &symbol, &symbolclass) != noErr)
			return false;
		*slots[index] = symbol;
	}
	return true;
}

static void RaveSetFloat(RaveFrame *frame, long tag, float value)
{
	((RaveSetFloatProc)frame->methods[2 + RAVEMETHODSETFLOAT])(frame->context, tag, value);
}

static void RaveSetInt(RaveFrame *frame, long tag, unsigned long value)
{
	((RaveSetIntProc)frame->methods[2 + RAVEMETHODSETINT])(frame->context, tag, value);
}

static void RaveDrawFrame(RaveFrame *frame)
{
	RaveTriTextureProc texture;
	RaveTriGouraudProc gouraud;
	int index;
	((RaveRenderStartProc)frame->methods[2 + RAVEMETHODRENDERSTART])(frame->context, NULL, NULL);
	if (frame->kind == RAVEKINDTEXTURE) {
		texture = (RaveTriTextureProc)frame->methods[2 + RAVEMETHODTRITEXTURE];
		for (index = 0; index < frame->triangles; index++)
			texture(frame->context, &frame->texture[index * 3], &frame->texture[index * 3 + 1], &frame->texture[index * 3 + 2], 0);
	} else {
		gouraud = (RaveTriGouraudProc)frame->methods[2 + RAVEMETHODTRIGOURAUD];
		for (index = 0; index < frame->triangles; index++)
			gouraud(frame->context, &frame->gouraud[index * 3], &frame->gouraud[index * 3 + 1], &frame->gouraud[index * 3 + 2],
				0);
	}
	((RaveRenderEndProc)frame->methods[2 + RAVEMETHODRENDEREND])(frame->context, NULL);
	((RaveSyncProc)frame->methods[2 + RAVEMETHODSYNC])(frame->context);
}

static uint32_t RaveBody(void *context, uint32_t iterations)
{
	uint32_t index;
	for (index = 0; index < iterations; index++)
		RaveDrawFrame((RaveFrame *)context);
	return iterations;
}

static void RaveMakeTriangles(RaveGouraud *gouraud, RaveTexture *texture, int count, Boolean flat)
{
	RaveGouraud *vertex;
	RaveTexture *textured;
	uint32_t seed;
	float centerx;
	float centery;
	float depth;
	int index;
	int corner;
	seed = 4321;
	for (index = 0; index < count; index++) {
		seed = seed * 1664525UL + 1013904223UL;
		centerx = (float)((seed >> 8) & 0x3ff) * (RAVEWIDTH - 40) / 1024.0f + 20;
		seed = seed * 1664525UL + 1013904223UL;
		centery = (float)((seed >> 8) & 0x3ff) * (RAVEHEIGHT - 40) / 1024.0f + 20;
		depth = (float)(index % 97) / 100.0f + 0.01f;
		for (corner = 0; corner < 3; corner++) {
			vertex = &gouraud[index * 3 + corner];
			textured = &texture[index * 3 + corner];
			vertex->x = centerx - 20.0f;
			if (corner == 1)
				vertex->x = centerx + 20.0f;
			if (corner == 2)
				vertex->x = centerx;
			vertex->y = centery - 15.0f;
			if (corner == 2)
				vertex->y = centery + 20.0f;
			vertex->z = depth;
			vertex->invw = 1.0f;
			vertex->r = 0.1f;
			vertex->g = 0.1f;
			vertex->b = 0.1f;
			if (corner == 0)
				vertex->r = 1.0f;
			if (corner == 1)
				vertex->g = 1.0f;
			if (corner == 2)
				vertex->b = 1.0f;
			if (flat) {
				vertex->r = 0.9f;
				vertex->g = 0.4f;
				vertex->b = 0.1f;
			}
			vertex->a = 0.5f;
			memset(textured, 0, sizeof(*textured));
			textured->x = vertex->x;
			textured->y = vertex->y;
			textured->z = vertex->z;
			textured->invw = 1.0f;
			textured->r = 1.0f;
			textured->g = 1.0f;
			textured->b = 1.0f;
			textured->a = 1.0f;
			if (corner == 1)
				textured->uoverw = 1.0f;
			if (corner == 2)
				textured->voverw = 1.0f;
			textured->kdr = 1.0f;
			textured->kdg = 1.0f;
			textured->kdb = 1.0f;
		}
	}
}

static Boolean RaveCheckEngine(RaveFrame *frame, uint32_t *pixels, char *why)
{
	RaveGouraud vertices[3];
	uint32_t inside;
	uint32_t outside;
	int corner;
	for (corner = 0; corner < 3; corner++) {
		vertices[corner].x = 100.0f;
		vertices[corner].y = 100.0f;
		if (corner == 1)
			vertices[corner].x = 300.0f;
		if (corner == 2)
			vertices[corner].y = 300.0f;
		vertices[corner].z = 0.5f;
		vertices[corner].invw = 1.0f;
		vertices[corner].r = 1.0f;
		vertices[corner].g = 0.0f;
		vertices[corner].b = 0.0f;
		vertices[corner].a = 1.0f;
	}
	RaveSetFloat(frame, 1, 1.0f);
	RaveSetFloat(frame, 2, 0.0f);
	RaveSetFloat(frame, 3, 0.0f);
	RaveSetFloat(frame, 4, 1.0f);
	((RaveRenderStartProc)frame->methods[2 + RAVEMETHODRENDERSTART])(frame->context, NULL, NULL);
	((RaveTriGouraudProc)frame->methods[2 + RAVEMETHODTRIGOURAUD])(frame->context, &vertices[0], &vertices[1], &vertices[2], 0);
	((RaveRenderEndProc)frame->methods[2 + RAVEMETHODRENDEREND])(frame->context, NULL);
	((RaveSyncProc)frame->methods[2 + RAVEMETHODSYNC])(frame->context);
	inside = pixels[150 * RAVEWIDTH + 150] & 0xffffffUL;
	outside = pixels[440 * RAVEWIDTH + 600] & 0xffffffUL;
	if (inside != 0xff0000UL || outside != 0x0000ffUL) {
		snprintf(why, 100, "drew %06lx inside a red triangle and %06lx on blue", (unsigned long)inside, (unsigned long)outside);
		return false;
	}
	return true;
}

static void RaveConfigure(RaveFrame *frame, void *texture)
{
	if (frame->kind == RAVEKINDBLEND)
		RaveSetInt(frame, RAVETAGBLEND, RAVEBLENDINTERPOLATE);
	else
		RaveSetInt(frame, RAVETAGBLEND, 0);
	if (frame->kind == RAVEKINDFOG) {
		RaveSetInt(frame, RAVETAGFOGMODE, RAVEFOGLINEAR);
		RaveSetFloat(frame, RAVETAGFOGCOLORA, 1.0f);
		RaveSetFloat(frame, RAVETAGFOGCOLORR, 0.5f);
		RaveSetFloat(frame, RAVETAGFOGCOLORG, 0.5f);
		RaveSetFloat(frame, RAVETAGFOGCOLORB, 0.5f);
		RaveSetFloat(frame, RAVETAGFOGSTART, 0.1f);
		RaveSetFloat(frame, RAVETAGFOGEND, 0.9f);
	} else
		RaveSetInt(frame, RAVETAGFOGMODE, 0);
	if (frame->kind == RAVEKINDTEXTURE) {
		((RaveSetPtrProc)frame->methods[2 + RAVEMETHODSETPTR])(frame->context, RAVETAGTEXTURE, texture);
		RaveSetInt(frame, RAVETAGTEXTUREOP, 0);
		RaveSetInt(frame, RAVETAGTEXTUREFILTER, 1);
	}
}

static void RaveRunEngine(Bench *bench, RaveLibrary *library, RaveScene *scene, void *engine, const char *enginename,
	void *texture)
{
	static const char *kinds[RAVEKINDS] = {"flat", "Gouraud", "textured", "Z buffer + alpha blend", "Z buffer + fog"};
	static const int counts[3] = {100, 1000, 10000};
	RaveFrame frame;
	RaveRect rect;
	BenchMeasurement measurement;
	BenchRow *row;
	char why[100];
	char name[80];
	Boolean ok;
	int kind;
	int count;
	memset(&frame, 0, sizeof(frame));
	rect.left = 0;
	rect.right = RAVEWIDTH;
	rect.top = 0;
	rect.bottom = RAVEHEIGHT;
	if (library->contextnew(&scene->device, &rect, NULL, engine, 0, &frame.context) != 0 || frame.context == NULL) {
		snprintf(name, sizeof(name), "%s: draw context", enginename);
		BenchSkip(bench, BenchBegin(bench, "3d", name, BENCHUNITPERSEC),
			"the engine refused a 640 x 480 ARGB32 offscreen context");
		return;
	}
	frame.methods = (void **)frame.context;
	ok = RaveCheckEngine(&frame, scene->pixels, why);
	for (kind = 0; kind < RAVEKINDS && !bench->cancelled; kind++) {
		for (count = 0; count < 3 && !bench->cancelled; count++) {
			snprintf(name, sizeof(name), "%s: %s, %d triangles", enginename, kinds[kind], counts[count]);
			row = BenchBegin(bench, "3d", name, BENCHUNITPERSEC);
			if (row == NULL)
				continue;
			if (!ok) {
				BenchFail(bench, row, why);
				continue;
			}
			if (kind == RAVEKINDTEXTURE && texture == NULL) {
				BenchSkip(bench, row, "the engine made no texture");
				continue;
			}
			frame.kind = kind;
			frame.triangles = counts[count];
			frame.gouraud = scene->shaded;
			if (kind == RAVEKINDFLAT)
				frame.gouraud = scene->flat;
			frame.texture = scene->textured;
			RaveConfigure(&frame, texture);
			BenchMeasure(bench, RaveBody, &frame, 1, &measurement);
			BenchRecord(bench, row, &measurement, 1);
		}
	}
	library->contextdelete(frame.context);
}

static void RaveRunEngines(Bench *bench, RaveLibrary *library, RaveScene *scene)
{
	RaveImage image;
	char name[64];
	char line[120];
	void *engine;
	void *texture;
	int count;
	int index;
	count = 0;
	for (index = 0; index < 64 * 64; index++) {
		scene->texels[index] = 0xff2060c0UL;
		if (((index / 64 + index % 64) & 8) != 0)
			scene->texels[index] = 0xffffffffUL;
	}
	engine = library->first(&scene->device);
	while (engine && count < RAVEMAXENGINES && !bench->cancelled) {
		memset(name, 0, sizeof(name));
		if (library->gestalt(engine, RAVEGESTALTNAME, name) != 0 || name[0] == 0)
			snprintf(name, sizeof(name), "engine %d", count + 1);
		name[40] = 0;
		snprintf(line, sizeof(line), "info      3d engine %d is %s", count + 1, name);
		if (bench->pass == BENCHPASSMAIN)
			BenchLine(bench, line);
		image.width = 64;
		image.height = 64;
		image.rowbytes = 64 * 4;
		image.pixmap = scene->texels;
		texture = NULL;
		if (library->texturenew(engine, 0, RAVEPIXELARGB32, &image, &texture) != 0)
			texture = NULL;
		RaveRunEngine(bench, library, scene, engine, name, texture);
		if (texture)
			library->texturedelete(engine, texture);
		engine = library->next(&scene->device, engine);
		count++;
	}
	if (count == 0)
		BenchSkip(bench, BenchBegin(bench, "3d", "RAVE engines", BENCHUNITPERSEC),
			"no engine takes an offscreen ARGB32 context");
}

static void RaveRun(Bench *bench)
{
	RaveLibrary library;
	RaveScene scene;
	BenchMemory memory;
	BenchMemory flatmemory;
	BenchMemory shadedmemory;
	BenchMemory texturememory;
	if (!RaveLoad(&library)) {
		BenchSkip(bench, BenchBegin(bench, "3d", "RAVE engines", BENCHUNITPERSEC),
			"RAVE (QuickDraw 3D Accelerator) is not installed");
		return;
	}
	memset(&scene, 0, sizeof(scene));
	scene.flat = (RaveGouraud *)BenchAllocate(&flatmemory, sizeof(RaveGouraud) * 3 * RAVEMAXTRIANGLES);
	scene.shaded = (RaveGouraud *)BenchAllocate(&shadedmemory, sizeof(RaveGouraud) * 3 * RAVEMAXTRIANGLES);
	scene.textured = (RaveTexture *)BenchAllocate(&texturememory, sizeof(RaveTexture) * 3 * RAVEMAXTRIANGLES);
	scene.texels = (uint32_t *)NewPtr(64 * 64 * 4);
	if (BenchAllocate(&memory, RAVEWIDTH * RAVEHEIGHT * 4L + 16) == NULL || scene.flat == NULL || scene.shaded == NULL ||
		scene.textured == NULL || scene.texels == NULL)
		BenchSkip(bench, BenchBegin(bench, "3d", "RAVE engines", BENCHUNITPERSEC), "not enough memory");
	else {
		scene.pixels = (uint32_t *)(((unsigned long)memory.pointer + 15) & ~15UL);
		RaveMakeTriangles(scene.flat, scene.textured, RAVEMAXTRIANGLES, true);
		RaveMakeTriangles(scene.shaded, scene.textured, RAVEMAXTRIANGLES, false);
		scene.device.devicetype = RAVEDEVICEMEMORY;
		scene.device.rowbytes = RAVEWIDTH * 4;
		scene.device.pixeltype = RAVEPIXELARGB32;
		scene.device.width = RAVEWIDTH;
		scene.device.height = RAVEHEIGHT;
		scene.device.baseaddr = scene.pixels;
		RaveRunEngines(bench, &library, &scene);
	}
	BenchRelease(&flatmemory);
	BenchRelease(&shadedmemory);
	BenchRelease(&texturememory);
	if (scene.texels)
		DisposePtr((Ptr)scene.texels);
	BenchRelease(&memory);
}

void Suite3d(Bench *bench)
{
	RaveRun(bench);
	SuiteQd3d(bench);
}
