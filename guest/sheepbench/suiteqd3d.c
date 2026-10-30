/*
 *  suiteqd3d.c - SheepBench QuickDraw 3D scene test (part of the 3D suite)
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
#include "bench.h"

#define QD3DWIDTH 640
#define QD3DHEIGHT 480
#define QD3DMAXTRIANGLES 10000
#define QD3DSUCCESS 1
#define QD3DRETRAVERSE 1
#define QD3DCLEARWITHCOLOR 1
#define QD3DPIXELARGB32 1
#define QD3DENDIANBIG 0
#define QD3DINTERACTIVE 0x6374776eUL
#define QD3DVENDORATI 1L
#define QD3DENGINERAGE128 0x00021000L
#define QD3DSYMBOLS 18

typedef void *Qd3dObject;

typedef struct Qd3dPoint2 {
	float x;
	float y;
} Qd3dPoint2;

typedef struct Qd3dPoint3 {
	float x;
	float y;
	float z;
} Qd3dPoint3;

typedef struct Qd3dColorArgb {
	float a;
	float r;
	float g;
	float b;
} Qd3dColorArgb;

typedef struct Qd3dColorRgb {
	float r;
	float g;
	float b;
} Qd3dColorRgb;

typedef struct Qd3dArea {
	Qd3dPoint2 min;
	Qd3dPoint2 max;
} Qd3dArea;

typedef struct Qd3dBitmap {
	unsigned char *image;
	unsigned long width;
	unsigned long height;
	unsigned long rowbytes;
	long bitorder;
} Qd3dBitmap;

typedef struct Qd3dDrawContextData {
	long clearmethod;
	Qd3dColorArgb clearcolor;
	Qd3dArea pane;
	long panestate;
	Qd3dBitmap mask;
	long maskstate;
	long doublebufferstate;
} Qd3dDrawContextData;

typedef struct Qd3dPixmap {
	void *image;
	unsigned long width;
	unsigned long height;
	unsigned long rowbytes;
	unsigned long pixelsize;
	long pixeltype;
	long bitorder;
	long byteorder;
} Qd3dPixmap;

typedef struct Qd3dPixmapContext {
	Qd3dDrawContextData data;
	Qd3dPixmap pixmap;
} Qd3dPixmapContext;

typedef struct Qd3dCamera {
	Qd3dPoint3 location;
	Qd3dPoint3 interest;
	Qd3dPoint3 up;
	float hither;
	float yon;
	Qd3dPoint2 origin;
	float width;
	float height;
	float fov;
	float aspect;
} Qd3dCamera;

typedef struct Qd3dLight {
	long on;
	float brightness;
	Qd3dColorRgb color;
} Qd3dLight;

typedef struct Qd3dVertex {
	Qd3dPoint3 point;
	Qd3dObject attributes;
} Qd3dVertex;

typedef struct Qd3dTriangle {
	Qd3dVertex vertices[3];
	Qd3dObject attributes;
} Qd3dTriangle;

typedef struct Qd3dLibrary {
	long (*initialize)(void);
	Qd3dObject (*viewnew)(void);
	Qd3dObject (*renderernew)(unsigned long);
	long (*viewsetrenderer)(Qd3dObject, Qd3dObject);
	Qd3dObject (*pixmapcontextnew)(const Qd3dPixmapContext *);
	long (*viewsetcontext)(Qd3dObject, Qd3dObject);
	Qd3dObject (*cameranew)(const Qd3dCamera *);
	long (*viewsetcamera)(Qd3dObject, Qd3dObject);
	Qd3dObject (*ambientnew)(const Qd3dLight *);
	Qd3dObject (*lightgroupnew)(void);
	Qd3dObject (*groupadd)(Qd3dObject, Qd3dObject);
	long (*viewsetlights)(Qd3dObject, Qd3dObject);
	long (*viewstart)(Qd3dObject);
	long (*viewend)(Qd3dObject);
	long (*trianglesubmit)(const Qd3dTriangle *, Qd3dObject);
	long (*dispose)(Qd3dObject);
	long (*setpreferences)(Qd3dObject, long, long);
	long (*viewsync)(Qd3dObject);
} Qd3dLibrary;

typedef struct Qd3dScene {
	Qd3dLibrary *library;
	Qd3dObject view;
	const Qd3dTriangle *triangles;
	int count;
} Qd3dScene;

typedef struct Qd3dObjects {
	Qd3dObject view;
	Qd3dObject renderer;
	Qd3dObject context;
	Qd3dObject camera;
	Qd3dObject ambient;
	Qd3dObject group;
} Qd3dObjects;

static Boolean Qd3dLoad(Qd3dLibrary *library)
{
	static const unsigned char *names[QD3DSYMBOLS] = {"\pQ3Initialize", "\pQ3View_New", "\pQ3Renderer_NewFromType",
		"\pQ3View_SetRenderer", "\pQ3PixmapDrawContext_New", "\pQ3View_SetDrawContext", "\pQ3ViewAngleAspectCamera_New",
		"\pQ3View_SetCamera", "\pQ3AmbientLight_New", "\pQ3LightGroup_New", "\pQ3Group_AddObject", "\pQ3View_SetLightGroup",
		"\pQ3View_StartRendering", "\pQ3View_EndRendering", "\pQ3Triangle_Submit", "\pQ3Object_Dispose",
		"\pQ3InteractiveRenderer_SetPreferences", "\pQ3View_Sync"};
	CFragConnectionID connection;
	Ptr main;
	Ptr symbol;
	CFragSymbolClass symbolclass;
	Str255 error;
	void **slots;
	int index;
	slots = (void **)library;
	if (GetSharedLibrary("\pQuickDraw\xaa 3D", kPowerPCCFragArch, kReferenceCFrag, &connection, &main, error) != noErr)
		return false;
	for (index = 0; index < QD3DSYMBOLS; index++) {
		if (FindSymbol(connection, names[index], &symbol, &symbolclass) != noErr)
			return false;
		slots[index] = symbol;
	}
	return library->initialize() == QD3DSUCCESS;
}

static void Qd3dDraw(Qd3dScene *scene)
{
	int index;
	scene->library->viewstart(scene->view);
	do {
		for (index = 0; index < scene->count; index++)
			scene->library->trianglesubmit(&scene->triangles[index], scene->view);
	} while (scene->library->viewend(scene->view) == QD3DRETRAVERSE);
	scene->library->viewsync(scene->view);
}

static uint32_t Qd3dBody(void *context, uint32_t iterations)
{
	uint32_t index;
	for (index = 0; index < iterations; index++)
		Qd3dDraw((Qd3dScene *)context);
	return iterations;
}

static void Qd3dGrid(Qd3dTriangle *triangles, int count)
{
	static const int corners[2][3] = {{0, 1, 2}, {1, 3, 2}};
	Qd3dPoint3 points[4];
	float x;
	float y;
	float size;
	int side;
	int index;
	int cell;
	int corner;
	side = 1;
	while (side * side * 2 < count)
		side++;
	size = 2.0f / side;
	for (index = 0; index < count; index++) {
		cell = index / 2;
		x = -1.0f + 2.0f * (cell % side) / side;
		y = -1.0f + 2.0f * (cell / side) / side;
		memset(points, 0, sizeof(points));
		points[0].x = x;
		points[0].y = y;
		points[1].x = x + size;
		points[1].y = y;
		points[2].x = x;
		points[2].y = y + size;
		points[3].x = x + size;
		points[3].y = y + size;
		for (corner = 0; corner < 3; corner++) {
			triangles[index].vertices[corner].point = points[corners[index & 1][corner]];
			triangles[index].vertices[corner].attributes = NULL;
		}
		triangles[index].attributes = NULL;
	}
}

static Boolean Qd3dSetup(Qd3dLibrary *library, Qd3dObjects *objects, uint32_t *pixels, Boolean prefer)
{
	Qd3dPixmapContext context;
	Qd3dCamera camera;
	Qd3dLight light;
	memset(objects, 0, sizeof(*objects));
	objects->view = library->viewnew();
	objects->renderer = library->renderernew(QD3DINTERACTIVE);
	if (objects->view == NULL || objects->renderer == NULL)
		return false;
	if (prefer)
		library->setpreferences(objects->renderer, QD3DVENDORATI, QD3DENGINERAGE128);
	library->viewsetrenderer(objects->view, objects->renderer);
	memset(&context, 0, sizeof(context));
	context.data.clearmethod = QD3DCLEARWITHCOLOR;
	context.data.clearcolor.a = 1.0f;
	context.pixmap.image = pixels;
	context.pixmap.width = QD3DWIDTH;
	context.pixmap.height = QD3DHEIGHT;
	context.pixmap.rowbytes = QD3DWIDTH * 4;
	context.pixmap.pixelsize = 32;
	context.pixmap.pixeltype = QD3DPIXELARGB32;
	context.pixmap.bitorder = QD3DENDIANBIG;
	context.pixmap.byteorder = QD3DENDIANBIG;
	objects->context = library->pixmapcontextnew(&context);
	memset(&camera, 0, sizeof(camera));
	camera.location.z = 3.0f;
	camera.up.y = 1.0f;
	camera.hither = 0.5f;
	camera.yon = 10.0f;
	camera.origin.x = -1.0f;
	camera.origin.y = 1.0f;
	camera.width = 2.0f;
	camera.height = 2.0f;
	camera.fov = 0.8f;
	camera.aspect = (float)QD3DWIDTH / QD3DHEIGHT;
	objects->camera = library->cameranew(&camera);
	light.on = 1;
	light.brightness = 1.0f;
	light.color.r = 1.0f;
	light.color.g = 1.0f;
	light.color.b = 1.0f;
	objects->ambient = library->ambientnew(&light);
	objects->group = library->lightgroupnew();
	if (objects->context == NULL || objects->camera == NULL || objects->ambient == NULL || objects->group == NULL)
		return false;
	library->groupadd(objects->group, objects->ambient);
	library->viewsetcontext(objects->view, objects->context);
	library->viewsetcamera(objects->view, objects->camera);
	library->viewsetlights(objects->view, objects->group);
	return true;
}

static void Qd3dDispose(Qd3dLibrary *library, Qd3dObjects *objects)
{
	if (objects->group)
		library->dispose(objects->group);
	if (objects->ambient)
		library->dispose(objects->ambient);
	if (objects->camera)
		library->dispose(objects->camera);
	if (objects->context)
		library->dispose(objects->context);
	if (objects->renderer)
		library->dispose(objects->renderer);
	if (objects->view)
		library->dispose(objects->view);
}

static void Qd3dRunPreference(Bench *bench, Qd3dLibrary *library, uint32_t *pixels, Qd3dTriangle *triangles, Boolean prefer)
{
	static const int counts[2] = {1000, 10000};
	Qd3dObjects objects;
	Qd3dScene scene;
	Qd3dTriangle one;
	BenchMeasurement measurement;
	BenchRow *row;
	const char *which;
	char name[80];
	char why[100];
	Boolean ok;
	int count;
	which = "default engine";
	if (prefer)
		which = "PocketShaver RAVE asked for";
	if (!Qd3dSetup(library, &objects, pixels, prefer)) {
		snprintf(name, sizeof(name), "QuickDraw 3D scene (%s)", which);
		BenchSkip(bench, BenchBegin(bench, "3d", name, BENCHUNITPERSEC), "QuickDraw 3D refused the view, pixmap, camera or light");
		Qd3dDispose(library, &objects);
		return;
	}
	memset(&one, 0, sizeof(one));
	one.vertices[0].point.x = -0.5f;
	one.vertices[0].point.y = -0.5f;
	one.vertices[1].point.x = 0.5f;
	one.vertices[1].point.y = -0.5f;
	one.vertices[2].point.y = 0.5f;
	scene.library = library;
	scene.view = objects.view;
	scene.triangles = &one;
	scene.count = 1;
	memset(pixels, 0x55, QD3DWIDTH * QD3DHEIGHT * 4L);
	Qd3dDraw(&scene);
	ok = (pixels[(QD3DHEIGHT / 2 + 20) * QD3DWIDTH + QD3DWIDTH / 2] & 0xffffffUL) != 0 &&
		(pixels[2 * QD3DWIDTH + 2] & 0xffffffUL) == 0;
	snprintf(why, sizeof(why), "middle %08lx, corner %08lx",
		(unsigned long)pixels[(QD3DHEIGHT / 2 + 20) * QD3DWIDTH + QD3DWIDTH / 2], (unsigned long)pixels[2 * QD3DWIDTH + 2]);
	for (count = 0; count < 2 && !bench->cancelled; count++) {
		snprintf(name, sizeof(name), "QuickDraw 3D scene (%s), %d triangles", which, counts[count]);
		row = BenchBegin(bench, "3d", name, BENCHUNITPERSEC);
		if (row == NULL)
			continue;
		if (!ok) {
			BenchFail(bench, row, why);
			continue;
		}
		Qd3dGrid(triangles, counts[count]);
		scene.triangles = triangles;
		scene.count = counts[count];
		BenchMeasure(bench, Qd3dBody, &scene, 1, &measurement);
		BenchRecord(bench, row, &measurement, 1);
	}
	Qd3dDispose(library, &objects);
}

void SuiteQd3d(Bench *bench)
{
	Qd3dLibrary library;
	BenchMemory memory;
	BenchMemory trianglememory;
	Qd3dTriangle *triangles;
	uint32_t *pixels;
	if (!Qd3dLoad(&library)) {
		BenchSkip(bench, BenchBegin(bench, "3d", "QuickDraw 3D scene", BENCHUNITPERSEC), "QuickDraw 3D is not installed");
		return;
	}
	triangles = (Qd3dTriangle *)BenchAllocate(&trianglememory, sizeof(Qd3dTriangle) * QD3DMAXTRIANGLES);
	if (BenchAllocate(&memory, QD3DWIDTH * QD3DHEIGHT * 4L + 16) == NULL || triangles == NULL)
		BenchSkip(bench, BenchBegin(bench, "3d", "QuickDraw 3D scene", BENCHUNITPERSEC), "not enough memory");
	else {
		pixels = (uint32_t *)(((unsigned long)memory.pointer + 15) & ~15UL);
		Qd3dRunPreference(bench, &library, pixels, triangles, false);
		if (!bench->cancelled)
			Qd3dRunPreference(bench, &library, pixels, triangles, true);
	}
	BenchRelease(&trianglememory);
	BenchRelease(&memory);
}
