/*
 *  osxglbridge.cpp - host OpenGL bridge for Mac OS X guest Flurry and Quartz OpenGL contexts
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
#include "sysdeps.h"
#include <stddef.h>
#include "osxcpu.h"
#include "osxmachine.h"
#include "osxglbridge.h"
#include "osxhost.h"
#include "osxglengine.h"
#if OSXGLBRIDGE
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"

#define GLBRIDGEDEPTH32 0x85
#define GLBRIDGEVIEWPORT 0x0ba2u
#define GLBRIDGEVENDOR 0x1f00u
#define GLBRIDGERENDERER 0x1f01u
#define GLBRIDGEVERSION 0x1f02u
#define GLBRIDGEEXTENSIONS 0x1f03u
#define GLBRIDGETEXTURE2D 0x0de1u

struct OsxGlBridge {
	void *gl;
	uint32 width;
	uint32 height;
	uint8 *vram;
	uint32 rowbytes;
	uint8 *pixels;
};

static int OsxGuestGlStartHost(uint32 width, uint32 height, uint8 *vram, uint32 rowbytes, OsxHostVideo *video, uint8 **present)
{
	DMCModeDesc mode;
	if (MetalCompositorIsInitialized())
		return 1;
	*present = (uint8 *)malloc(rowbytes * height);
	if (!*present)
		return 0;
	memcpy(*present, vram, rowbytes * height);
	memset(&mode, 0, sizeof(mode));
	mode.width = width;
	mode.height = height;
	mode.depth = 32;
	mode.row_bytes = rowbytes;
	mode.pitch = rowbytes;
	mode.screen_base_host = *present;
	if (dmc_create(&mode) != kDMCNoErr)
		return 0;
	if (MetalCompositorInit(width, height, GLBRIDGEDEPTH32, rowbytes, rowbytes, *present, rowbytes * height) != 0) {
		MetalCompositorShutdown();
		dmc_shutdown();
		return 0;
	}
	gfxaccel_resources_init();
	OsxGuestHostSuspendFrame(video);
	return 1;
}

void OsxGuestGlShutdownHost(void)
{
	if (MetalCompositorIsInitialized()) {
		gfxaccel_resources_shutdown();
		MetalCompositorShutdown();
		dmc_shutdown();
	}
}

static void OsxGuestGlPublishFrame(void *gl, OsxCpu *cpu, uint8 *pixels, uint32 width, uint32 height, uint8 *vram, uint32 rowbytes)
{
	uint32 row;
	OsxGl::OsxGuestEngineReadPixels(gl, cpu, width, height, pixels);
	for (row = 0; row < height; row++)
		memcpy(vram + row * rowbytes, pixels + (height - row - 1) * width * 4, width * 4);
}

OsxGlBridge *OsxGuestCreateGlBridge(OsxCpu *cpu, uint32 width, uint32 height, uint8 *vram, uint32 rowbytes, OsxHostVideo *video, uint8 **present)
{
	OsxGlBridge *bridge;
	if (!OsxGuestGlStartHost(width, height, vram, rowbytes, video, present))
		return NULL;
	bridge = (OsxGlBridge *)calloc(1, sizeof(*bridge));
	if (!bridge)
		return NULL;
	bridge->pixels = (uint8 *)malloc(width * height * 4);
	if (!bridge->pixels) {
		free(bridge);
		return NULL;
	}
	bridge->gl = OsxGl::OsxGuestEngineCreate(NULL);
	bridge->width = width;
	bridge->height = height;
	bridge->vram = vram;
	bridge->rowbytes = rowbytes;
	OsxGl::OsxGuestEngineResize(bridge->gl, cpu, width, height);
	return bridge;
}

void OsxGuestDestroyGlBridge(OsxGlBridge *bridge)
{
	OsxGl::OsxGuestEngineDestroy(bridge->gl);
	free(bridge->pixels);
	free(bridge);
}

int OsxGuestGlBridgeCall(OsxGlBridge *bridge, OsxCpu *cpu, uint32 call)
{
	if (call == OSXFLURRYFLUSH) {
		OsxGl::OsxGuestEngineFlush(bridge->gl, cpu);
		OsxGuestGlPublishFrame(bridge->gl, cpu, bridge->pixels, bridge->width, bridge->height, bridge->vram, bridge->rowbytes);
		return 1;
	}
	return OsxGl::OsxGuestEngineFlurryCall(bridge->gl, cpu, call);
}

typedef struct OsxQuartzPbuffer {
	struct OsxQuartzPbuffer *next;
	uint32 address;
	uint32 segment;
	uint32 width;
	uint32 height;
	unsigned texture;
} OsxQuartzPbuffer;

#define QUARTZWINDOWLISTMAX 64

typedef struct OsxQuartzContext {
	struct OsxQuartzContext *next;
	void *gl;
	uint32 strings;
	int extensions;
	uint32 address;
	uint32 engine;
	uint32 segment;
	uint32 original[OSXQUARTZSLOTS];
	uint32 viewport[4];
	int appviewport;
	int drawableflush;
	OsxQuartzPbuffer *pbuffer;
	uint32 offscreenaddress;
	uint32 offscreenwidth;
	uint32 offscreenheight;
	uint32 offscreenrowbytes;
	int offscreenready;
	int flushstep;
	uint32 flushentry;
	uint32 flushstack;
	uint32 flushsaved;
	uint32 flushlr;
	uint32 flushfirst;
	uint32 flushwidth;
	uint32 flushheight;
	uint32 connection;
	uint32 windowid;
	uint32 surfaceid;
	int surfaceknown;
	int geometryvalid;
	int windowvisible;
	int windowshown;
	uint64 geometrytime;
	int32 windowrect[4];
	int32 surfacerect[4];
	uint32 windowlist[QUARTZWINDOWLISTMAX];
	uint32 windowlistcount;
	uint32 windowlistindex;
	uint32 occludercount;
	int32 occluders[QUARTZWINDOWLISTMAX][4];
	uint8 occluderopaque[QUARTZWINDOWLISTMAX];
	int32 pendingrects[QUARTZWINDOWLISTMAX][4];
	uint8 pendingopaque[QUARTZWINDOWLISTMAX];
	uint32 pendingcount;
	uint32 presentwidth;
	uint32 presentheight;
	uint32 shownwidth;
	uint32 shownheight;
	int windowwanted;
	uint32 hoststrings;
	uint32 hoststringname;
	uint32 targetwidth;
	uint32 targetheight;
	uint8 *pixels;
	uint32 pixelssize;
} OsxQuartzContext;

struct OsxQuartzBridge {
	OsxQuartzContext *contexts;
	OsxQuartzPbuffer *pbuffers;
	uint32 floatsegment;
	int allocatedextensions;
	uint32 width;
	uint32 height;
	uint32 library;
	uint32 strings;
	uint32 allocate;
	uint32 clear;
	uint8 **present;
	const uint32 *windowcalls;
	uint8 *vram;
	uint32 rowbytes;
	OsxHostVideo *video;
	int extensions;
};

#define QUARTZFLUSHIDLE 0
#define QUARTZFLUSHQUERY 1
#define QUARTZFLUSHWINDOWRECT 2
#define QUARTZFLUSHSURFACERECT 3
#define QUARTZFLUSHWINDOWLIST 4
#define QUARTZFLUSHOCCLUDER 5
#define QUARTZFLUSHOPACITY 6
#define QUARTZFLUSHOFFSCREEN 9
#define QUARTZFLUSHHOSTALLOCATE 10
#define QUARTZFLUSHHOSTZERO 11
#define QUARTZFLUSHFRAME 0x200
#define QUARTZFLUSHSCRATCH 0x40
#define QUARTZHOSTSTRINGSIZE 128
#define QUARTZSLOTVIEWPORT 336
#define QUARTZGEOMETRYPERIOD 100000000ull
#define QUARTZRECTSURFACE 16
#define QUARTZLISTCOUNT 32
#define QUARTZLISTIDS 48
#define QUARTZSLOTGETINTEGER 105
#define QUARTZATTRIBUTES 64
#define QUARTZGETSTRING 118

static int OsxGuestGlGuestStore(OsxCpu *cpu, uint32 address, const uint8 *input, uint32 length)
{
	uint32 physical;
	uint32 count;
	while (length) {
		if (!OsxGuestCpuTranslate(cpu, address, OSXTLBWRITE, 0, &physical) || physical >= cpu->ramsize)
			return 0;
		count = 4096 - (address & 4095);
		if (count > length)
			count = length;
		if (count > cpu->ramsize - physical)
			return 0;
		memcpy(cpu->ram + physical, input, count);
		input += count;
		address += count;
		length -= count;
	}
	return 1;
}

static int OsxGuestQuartzEngineSub(uint32 call)
{
	if (call >= 1 && call <= 336)
		return (int)call - 1;
	if (call >= 344 && call <= 386)
		return (int)call + 62;
	switch (call) {
	case 338: return 400;
	case 339: return 401;
	case 342: return 404;
	case 343: return 405;
	case 406: return 468;
	case 439: return 501;
	case 440: return 502;
	case 441: return 503;
	}
	return -1;
}

static int OsxGuestQuartzStartHost(OsxQuartzBridge *bridge)
{
	return OsxGuestGlStartHost(bridge->width, bridge->height, bridge->vram, bridge->rowbytes, bridge->video, bridge->present);
}

static void OsxGuestQuartzEnsureTarget(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 width, uint32 height)
{
	if (width > bridge->width)
		width = bridge->width;
	if (height > bridge->height)
		height = bridge->height;
	if (width <= context->targetwidth && height <= context->targetheight)
		return;
	if (width < context->targetwidth)
		width = context->targetwidth;
	if (height < context->targetheight)
		height = context->targetheight;
	context->targetwidth = width;
	context->targetheight = height;
	OsxGl::OsxGuestEngineResize(context->gl, cpu, width, height);
}

static int OsxGuestQuartzCallDraws(uint32 call)
{
	switch (call) {
	case 1: case 4: case 5: case 7: case 9: case 10: case 11:
	case 53: case 54: case 55: case 56: case 57:
	case 66: case 68: case 69:
	case 85: case 86: case 87: case 88:
	case 238: case 239: case 240: case 241: case 242: case 243: case 244: case 245: case 246:
	case 406:
		return 1;
	}
	return 0;
}

static int OsxGuestQuartzAttributeHasValue(uint32 attribute)
{
	switch (attribute) {
	case 7: case 8: case 11: case 12: case 13: case 14: case 55: case 56: case 70: case 84: case 128:
		return 1;
	}
	return 0;
}

static void OsxGuestQuartzRelaxPixelFormat(OsxQuartzBridge *bridge, OsxCpu *cpu, uint32 address)
{
	uint32 attribute;
	uint32 value;
	int source;
	int target;
	target = 0;
	for (source = 0; source < QUARTZATTRIBUTES; source++) {
		if (!OsxGuestCpuReadVirtual(cpu, address + source * 4, &attribute))
			return;
		if (attribute == 73 || attribute == 90 || attribute == 91)
			continue;
		if (attribute == 58)
			bridge->floatsegment = cpu->sr[0];
		OsxGuestCpuWriteVirtual(cpu, address + target * 4, attribute);
		target++;
		if (!attribute)
			return;
		if (OsxGuestQuartzAttributeHasValue(attribute)) {
			source++;
			if (!OsxGuestCpuReadVirtual(cpu, address + source * 4, &value))
				return;
			if (attribute == 55 || attribute == 56)
				value = 0;
			if (attribute == 8 && value > 32)
				bridge->floatsegment = cpu->sr[0];
			OsxGuestCpuWriteVirtual(cpu, address + target * 4, value);
			target++;
		}
	}
}

OsxQuartzBridge *OsxGuestCreateQuartzBridge(uint32 width, uint32 height, uint32 library, uint32 strings, int extensions, int allocatedextensions,
	uint32 allocate, uint32 clear, uint8 *vram, uint32 rowbytes, OsxHostVideo *video, uint8 **present, const uint32 *windowcalls)
{
	OsxQuartzBridge *bridge;
	bridge = (OsxQuartzBridge *)calloc(1, sizeof(*bridge));
	if (bridge) {
		bridge->width = width;
		bridge->height = height;
		bridge->library = library;
		bridge->strings = strings;
		bridge->extensions = extensions;
		bridge->allocatedextensions = allocatedextensions;
		bridge->allocate = allocate;
		bridge->clear = clear;
		bridge->vram = vram;
		bridge->rowbytes = rowbytes;
		bridge->video = video;
		bridge->present = present;
		bridge->windowcalls = windowcalls;
	}
	return bridge;
}

static void OsxGuestQuartzFreeContext(OsxQuartzContext *context)
{
	OsxGl::OsxGuestEngineDestroy(context->gl);
	free(context->pixels);
	free(context);
}

void OsxGuestDestroyQuartzBridge(OsxQuartzBridge *bridge)
{
	OsxQuartzContext *context;
	OsxQuartzContext *next;
	OsxQuartzPbuffer *pbuffer;
	OsxQuartzPbuffer *following;
	for (context = bridge->contexts; context; context = next) {
		next = context->next;
		OsxGuestQuartzFreeContext(context);
	}
	for (pbuffer = bridge->pbuffers; pbuffer; pbuffer = following) {
		following = pbuffer->next;
		free(pbuffer);
	}
	free(bridge);
}

static OsxQuartzPbuffer *OsxGuestQuartzFindPbuffer(OsxQuartzBridge *bridge, uint32 address, uint32 segment)
{
	OsxQuartzPbuffer *pbuffer;
	for (pbuffer = bridge->pbuffers; pbuffer; pbuffer = pbuffer->next)
		if (pbuffer->address == address && pbuffer->segment == segment)
			return pbuffer;
	return NULL;
}

static int OsxGuestQuartzAttachPbuffer(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 address,
	uint32 width, uint32 height)
{
	OsxQuartzPbuffer *pbuffer;
	pbuffer = OsxGuestQuartzFindPbuffer(bridge, address, context->segment);
	if (!pbuffer) {
		pbuffer = (OsxQuartzPbuffer *)calloc(1, sizeof(*pbuffer));
		if (!pbuffer)
			return 0;
		pbuffer->address = address;
		pbuffer->segment = context->segment;
		pbuffer->next = bridge->pbuffers;
		bridge->pbuffers = pbuffer;
	}
	if (pbuffer->width != width || pbuffer->height != height) {
		if (pbuffer->texture)
			OsxGl::OsxGuestEngineTextureDelete(context->gl, cpu, pbuffer->texture);
		pbuffer->texture = OsxGl::OsxGuestEngineTextureCreate(context->gl, cpu, width, height, bridge->floatsegment == context->segment);
		pbuffer->width = width;
		pbuffer->height = height;
	}
	OsxGl::OsxGuestEngineAttachTexture(context->gl, cpu, pbuffer->texture, width, height);
	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = width;
	context->viewport[3] = height;
	context->pbuffer = pbuffer;
	return 1;
}

static int OsxGuestQuartzPbufferTexture(OsxQuartzBridge *bridge, OsxCpu *cpu)
{
	OsxQuartzContext *context;
	OsxQuartzContext *destination;
	OsxQuartzPbuffer *pbuffer;
	uint32 target;
	uint32 internalformat;
	destination = NULL;
	if (!OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 12, &target) || !OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 16, &internalformat))
		return 0;
	if (target != GLBRIDGETEXTURE2D && target != 0x84f5)
		return 0;
	for (context = bridge->contexts; context; context = context->next)
		if (context->segment == cpu->sr[0] && context->address == cpu->gpr[3])
			destination = context;
	pbuffer = OsxGuestQuartzFindPbuffer(bridge, cpu->gpr[4], cpu->sr[0]);
	if (!destination || !pbuffer)
		return 0;
	OsxGl::OsxGuestEngineCopyTexture(destination->gl, cpu, pbuffer->texture, pbuffer->width, pbuffer->height, internalformat, target);
	cpu->gpr[3] = 0;
	cpu->nextpc = cpu->lr & ~3u;
	return 1;
}

static uint8 *OsxGuestQuartzPixels(OsxQuartzContext *context, uint32 size);

static void OsxGuestQuartzPresent(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint8 *pixels;
	if (context->pbuffer || context->viewport[2] != bridge->width || context->viewport[3] != bridge->height)
		return;
	OsxGuestQuartzEnsureTarget(bridge, cpu, context, bridge->width, bridge->height);
	pixels = OsxGuestQuartzPixels(context, bridge->width * bridge->height * 4);
	if (pixels)
		OsxGuestGlPublishFrame(context->gl, cpu, pixels, bridge->width, bridge->height, bridge->vram, bridge->rowbytes);
}

static void OsxGuestQuartzGuestCall(OsxCpu *cpu, OsxQuartzContext *context, int step, uint32 target)
{
	context->flushstep = step;
	cpu->gpr[1] = context->flushstack;
	cpu->lr = cpu->pc;
	cpu->ctr = target;
	cpu->nextpc = target;
}

static int OsxGuestQuartzChainStart(OsxCpu *cpu, OsxQuartzContext *context, uint32 entry)
{
	uint32 stack;
	if (context->flushstep != QUARTZFLUSHIDLE)
		return 0;
	stack = (cpu->gpr[1] - QUARTZFLUSHFRAME) & ~15u;
	if (!OsxGuestCpuWriteVirtual(cpu, stack, cpu->gpr[1]))
		return 0;
	context->flushstack = stack;
	context->flushsaved = cpu->gpr[1];
	context->flushlr = cpu->lr;
	context->flushfirst = cpu->gpr[3];
	context->flushentry = entry;
	return 1;
}

static void OsxGuestQuartzChainFinish(OsxCpu *cpu, OsxQuartzContext *context)
{
	context->flushstep = QUARTZFLUSHIDLE;
	cpu->gpr[1] = context->flushsaved;
	cpu->lr = context->flushlr;
	cpu->gpr[3] = context->flushfirst;
	if (context->flushentry >= 1000)
		cpu->gpr[0] = cpu->lr;
	else
		cpu->nextpc = cpu->lr & ~3u;
}

static int OsxGuestQuartzWindowContext(OsxQuartzBridge *bridge, OsxQuartzContext *context)
{
	return !context->pbuffer && !context->offscreenaddress &&
		(context->viewport[2] != bridge->width || context->viewport[3] != bridge->height);
}


static uint8 *OsxGuestQuartzPixels(OsxQuartzContext *context, uint32 size)
{
	uint8 *pixels;
	if (size > context->pixelssize) {
		pixels = (uint8 *)realloc(context->pixels, size);
		if (!pixels)
			return NULL;
		context->pixels = pixels;
		context->pixelssize = size;
	}
	return context->pixels;
}

static void OsxGuestQuartzWindowExtent(OsxQuartzBridge *bridge, OsxQuartzContext *context)
{
	context->flushwidth = context->viewport[0] + context->viewport[2];
	context->flushheight = context->viewport[1] + context->viewport[3];
	if (context->flushwidth > bridge->width)
		context->flushwidth = bridge->width;
	if (context->flushheight > bridge->height)
		context->flushheight = bridge->height;
}

static void OsxGuestQuartzWindowCapture(OsxCpu *cpu, OsxQuartzContext *context)
{
	uint8 *pixels;
	if (!context->windowwanted)
		return;
	pixels = OsxGuestQuartzPixels(context, context->flushwidth * context->flushheight * 4);
	if (!pixels)
		return;
	OsxGl::OsxGuestEngineReadPixels(context->gl, cpu, context->flushwidth, context->flushheight, pixels);
	context->presentwidth = context->flushwidth;
	context->presentheight = context->flushheight;
	context->windowwanted = 0;
}

static void OsxGuestQuartzWindowSkipFlush(OsxCpu *cpu, OsxQuartzContext *context)
{
	OsxGuestQuartzWindowCapture(cpu, context);
	cpu->gpr[3] = 0;
	cpu->nextpc = cpu->lr & ~3u;
}

static int OsxGuestQuartzWindowSettled(OsxQuartzContext *context)
{
	return context->geometryvalid && context->windowshown && context->shownwidth == context->flushwidth &&
		context->shownheight == context->flushheight && OsxGuestHostNanoseconds() - context->geometrytime < QUARTZGEOMETRYPERIOD;
}

static void OsxGuestQuartzGeometryBegin(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowid;
	cpu->gpr[5] = context->flushstack + QUARTZFLUSHSCRATCH;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHWINDOWRECT, bridge->windowcalls[OSXQUARTZSCREENRECT]);
}

static void OsxGuestQuartzGeometryFinish(OsxCpu *cpu, OsxQuartzContext *context)
{
	int show;
	context->geometrytime = OsxGuestHostNanoseconds();
	show = !context->windowshown || context->shownwidth != context->flushwidth || context->shownheight != context->flushheight;
	context->windowshown = 1;
	context->shownwidth = context->flushwidth;
	context->shownheight = context->flushheight;
	context->windowwanted = 1;
	OsxGuestQuartzWindowCapture(cpu, context);
	OsxGuestQuartzChainFinish(cpu, context);
	if (!show)
		OsxGuestQuartzWindowSkipFlush(cpu, context);
}

static int OsxGuestQuartzReadRect(OsxCpu *cpu, uint32 address, int32 *rect)
{
	uint32 bits;
	float value;
	int index;
	for (index = 0; index < 4; index++) {
		if (!OsxGuestCpuReadVirtual(cpu, address + index * 4, &bits))
			return 0;
		memcpy(&value, &bits, sizeof(value));
		rect[index] = (int32)(value + 0.5f);
	}
	return 1;
}

static void OsxGuestQuartzNextOccluder(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	if (context->windowlistindex >= context->windowlistcount ||
		context->windowlist[context->windowlistindex] == context->windowid) {
		context->windowvisible = context->windowlistindex < context->windowlistcount;
		context->geometryvalid = 1;
		memcpy(context->occluders, context->pendingrects, sizeof(context->occluders));
		memcpy(context->occluderopaque, context->pendingopaque, sizeof(context->occluderopaque));
		context->occludercount = context->pendingcount;
		OsxGuestQuartzGeometryFinish(cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowlist[context->windowlistindex];
	cpu->gpr[5] = context->flushstack + QUARTZFLUSHSCRATCH;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHOCCLUDER, bridge->windowcalls[OSXQUARTZSCREENRECT]);
}

static int OsxGuestQuartzWindowBegin(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 entry)
{
	if (!context->surfaceknown || !bridge->windowcalls[OSXQUARTZSCREENRECT] || !bridge->windowcalls[OSXQUARTZSURFACEBOUNDS] ||
		!bridge->windowcalls[OSXQUARTZWINDOWLIST] || !bridge->windowcalls[OSXQUARTZOPACITY])
		return 0;
	if (!context->appviewport && !context->original[QUARTZSLOTGETINTEGER])
		return 0;
	if (context->appviewport) {
		OsxGuestQuartzWindowExtent(bridge, context);
		if (!context->flushwidth || !context->flushheight)
			return 0;
		if (OsxGuestQuartzWindowSettled(context)) {
			OsxGuestQuartzWindowSkipFlush(cpu, context);
			return 1;
		}
	}
	if (!OsxGuestQuartzChainStart(cpu, context, entry))
		return 0;
	if (context->appviewport) {
		OsxGuestQuartzGeometryBegin(bridge, cpu, context);
		return 1;
	}
	cpu->gpr[3] = context->engine;
	cpu->gpr[4] = GLBRIDGEVIEWPORT;
	cpu->gpr[5] = context->flushstack + QUARTZFLUSHSCRATCH;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHQUERY, context->original[QUARTZSLOTGETINTEGER]);
	return 1;
}

static void OsxGuestQuartzWindowQueried(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 values[4];
	int index;
	for (index = 0; index < 4; index++) {
		if (!OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + index * 4, &values[index])) {
			OsxGuestQuartzChainFinish(cpu, context);
			return;
		}
	}
	memcpy(context->viewport, values, sizeof(values));
	OsxGuestQuartzWindowExtent(bridge, context);
	if (!context->flushwidth || !context->flushheight) {
		OsxGuestQuartzChainFinish(cpu, context);
		return;
	}
	OsxGuestQuartzEnsureTarget(bridge, cpu, context, context->flushwidth, context->flushheight);
	if (OsxGuestQuartzWindowSettled(context)) {
		OsxGuestQuartzChainFinish(cpu, context);
		OsxGuestQuartzWindowSkipFlush(cpu, context);
		return;
	}
	OsxGuestQuartzGeometryBegin(bridge, cpu, context);
}

static void OsxGuestQuartzWindowRectDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	if (cpu->gpr[3] || !OsxGuestQuartzReadRect(cpu, context->flushstack + QUARTZFLUSHSCRATCH, context->windowrect)) {
		context->geometryvalid = 0;
		OsxGuestQuartzGeometryFinish(cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowid;
	cpu->gpr[5] = context->surfaceid;
	cpu->gpr[6] = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZRECTSURFACE;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHSURFACERECT, bridge->windowcalls[OSXQUARTZSURFACEBOUNDS]);
}

static void OsxGuestQuartzSurfaceRectDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	if (cpu->gpr[3] || !OsxGuestQuartzReadRect(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZRECTSURFACE, context->surfacerect)) {
		context->geometryvalid = 0;
		OsxGuestQuartzGeometryFinish(cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = 0;
	cpu->gpr[5] = QUARTZWINDOWLISTMAX;
	cpu->gpr[6] = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTIDS;
	cpu->gpr[7] = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHWINDOWLIST, bridge->windowcalls[OSXQUARTZWINDOWLIST]);
}

static void OsxGuestQuartzWindowListDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 count;
	uint32 index;
	if (cpu->gpr[3] || !OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT, &count)) {
		context->geometryvalid = 0;
		OsxGuestQuartzGeometryFinish(cpu, context);
		return;
	}
	if (count > QUARTZWINDOWLISTMAX)
		count = QUARTZWINDOWLISTMAX;
	for (index = 0; index < count; index++) {
		if (!OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTIDS + index * 4, &context->windowlist[index])) {
			context->geometryvalid = 0;
			OsxGuestQuartzGeometryFinish(cpu, context);
			return;
		}
	}
	context->windowlistcount = count;
	context->windowlistindex = 0;
	context->pendingcount = 0;
	OsxGuestQuartzNextOccluder(bridge, cpu, context);
}

static void OsxGuestQuartzOccluderDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 address;
	address = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT;
	if (cpu->gpr[3] || !OsxGuestQuartzReadRect(cpu, context->flushstack + QUARTZFLUSHSCRATCH, context->pendingrects[context->pendingcount]) ||
		!OsxGuestCpuWriteVirtual(cpu, address, 0)) {
		context->windowlistindex++;
		OsxGuestQuartzNextOccluder(bridge, cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowlist[context->windowlistindex];
	cpu->gpr[5] = address;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHOPACITY, bridge->windowcalls[OSXQUARTZOPACITY]);
}

static void OsxGuestQuartzOpacityDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 opaque;
	if (cpu->gpr[3] || !OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT, &opaque))
		opaque = 0;
	context->pendingopaque[context->pendingcount] = opaque != 0;
	context->pendingcount++;
	context->windowlistindex++;
	OsxGuestQuartzNextOccluder(bridge, cpu, context);
}

static uint32 OsxGuestQuartzPixelValue(const uint8 *pixel)
{
	return ((uint32)pixel[1] << 16) | ((uint32)pixel[2] << 8) | pixel[3];
}

static uint32 OsxGuestQuartzBackingPixel(OsxQuartzBridge *bridge, int32 left, int32 top, int32 right, int32 bottom)
{
	uint32 candidate;
	uint32 value;
	uint32 votes;
	int32 x;
	int32 y;
	candidate = 0;
	votes = 0;
	for (y = top; y < bottom; y++) {
		for (x = left; x < right; x++) {
			value = OsxGuestQuartzPixelValue(bridge->vram + y * bridge->rowbytes + x * 4);
			if (!votes) {
				candidate = value;
				votes = 1;
			} else if (value == candidate)
				votes++;
			else
				votes--;
		}
	}
	return candidate;
}

static void OsxGuestQuartzComposeWindow(OsxQuartzBridge *bridge, OsxQuartzContext *context, uint8 *present)
{
	int32 surfaceleft;
	int32 surfacetop;
	int32 width;
	int32 height;
	int32 x;
	int32 y;
	int32 first;
	int32 last;
	uint32 index;
	uint32 overlaps;
	uint32 occluder;
	uint8 *pixels;
	int covered;
	int translucent;
	uint32 backing;
	int32 active[QUARTZWINDOWLISTMAX];
	if (!context->geometryvalid || !context->windowvisible || !context->presentwidth || !context->presentheight)
		return;
	width = context->presentwidth;
	height = context->presentheight;
	pixels = OsxGuestQuartzPixels(context, (uint32)width * (uint32)height * 4);
	if (!pixels)
		return;
	context->windowwanted = 1;
	surfaceleft = context->windowrect[0] + context->surfacerect[0];
	if (context->surfacerect[1] + context->surfacerect[3] == context->windowrect[3])
		surfacetop = context->windowrect[1] + context->surfacerect[1];
	else
		surfacetop = context->windowrect[1] + context->windowrect[3] - context->surfacerect[1] - context->surfacerect[3];
	overlaps = 0;
	translucent = 0;
	backing = 0;
	for (index = 0; index < context->occludercount; index++) {
		if (context->occluders[index][0] < surfaceleft + width && context->occluders[index][0] + context->occluders[index][2] > surfaceleft &&
			context->occluders[index][1] < surfacetop + height && context->occluders[index][1] + context->occluders[index][3] > surfacetop) {
			active[overlaps++] = (int32)index;
			if (!context->occluderopaque[index])
				translucent = 1;
		}
	}
	if (translucent && surfaceleft >= 0 && surfacetop >= 0 && surfaceleft + width <= (int32)bridge->width && surfacetop + height <= (int32)bridge->height)
		backing = OsxGuestQuartzBackingPixel(bridge, surfaceleft, surfacetop, surfaceleft + width, surfacetop + height);
	for (y = 0; y < height; y++) {
		if (surfacetop + y < 0 || surfacetop + y >= (int32)bridge->height)
			continue;
		first = 0;
		if (surfaceleft < 0)
			first = -surfaceleft;
		last = width;
		if (surfaceleft + last > (int32)bridge->width)
			last = (int32)bridge->width - surfaceleft;
		if (first >= last)
			continue;
		if (!overlaps) {
			memcpy(present + (surfacetop + y) * bridge->rowbytes + (surfaceleft + first) * 4,
				pixels + ((height - 1 - y) * width + first) * 4, (size_t)(last - first) * 4);
			continue;
		}
		for (x = first; x < last; x++) {
			covered = 0;
			for (occluder = 0; occluder < overlaps && !covered; occluder++) {
				const int32 *rect = context->occluders[active[occluder]];
				if (surfaceleft + x >= rect[0] && surfaceleft + x < rect[0] + rect[2] &&
					surfacetop + y >= rect[1] && surfacetop + y < rect[1] + rect[3]) {
					if (context->occluderopaque[active[occluder]] ||
						OsxGuestQuartzPixelValue(bridge->vram + (surfacetop + y) * bridge->rowbytes + (surfaceleft + x) * 4) != backing)
						covered = 1;
				}
			}
			if (!covered)
				memcpy(present + (surfacetop + y) * bridge->rowbytes + (surfaceleft + x) * 4,
					pixels + ((height - 1 - y) * width + x) * 4, 4);
		}
	}
}

void OsxGuestQuartzComposeWindows(OsxQuartzBridge *bridge, const uint8 *vram, uint8 *present, uint32 size)
{
	OsxQuartzContext *context;
	memcpy(present, vram, size);
	if (!bridge)
		return;
	for (context = bridge->contexts; context; context = context->next)
		OsxGuestQuartzComposeWindow(bridge, context, present);
}

static void OsxGuestQuartzOffscreenStore(OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 row;
	uint32 length;
	uint8 *pixels;
	length = context->offscreenwidth * 4;
	pixels = OsxGuestQuartzPixels(context, length * context->offscreenheight);
	if (!pixels)
		return;
	OsxGl::OsxGuestEngineReadPixels(context->gl, cpu, context->offscreenwidth, context->offscreenheight, pixels);
	for (row = 0; row < context->offscreenheight; row++) {
		if (!OsxGuestGlGuestStore(cpu, context->offscreenaddress + row * context->offscreenrowbytes,
			pixels + (context->offscreenheight - row - 1) * length, length))
			return;
	}
}

static void OsxGuestQuartzOffscreenFlush(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 entry)
{
	if (context->offscreenready || !bridge->clear) {
		OsxGuestQuartzOffscreenStore(cpu, context);
		return;
	}
	if (!OsxGuestQuartzChainStart(cpu, context, entry))
		return;
	cpu->gpr[3] = context->offscreenaddress;
	cpu->gpr[4] = context->offscreenrowbytes * context->offscreenheight;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHOFFSCREEN, bridge->clear);
}

static uint32 OsxGuestQuartzHostStringAddress(OsxQuartzContext *context)
{
	if (context->hoststringname == GLBRIDGEVENDOR)
		return context->hoststrings + QUARTZHOSTSTRINGSIZE;
	if (context->hoststringname == GLBRIDGEEXTENSIONS)
		return context->hoststrings + QUARTZHOSTSTRINGSIZE * 2;
	return context->hoststrings;
}

static int OsxGuestQuartzHostString(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	if ((cpu->gpr[4] != GLBRIDGERENDERER && cpu->gpr[4] != GLBRIDGEVENDOR && !(cpu->gpr[4] == GLBRIDGEEXTENSIONS && !context->extensions && bridge->allocatedextensions)) || !context->strings || !bridge->allocate || !bridge->clear)
		return 0;
	context->hoststringname = cpu->gpr[4];
	if (context->hoststrings) {
		cpu->gpr[3] = OsxGuestQuartzHostStringAddress(context);
		cpu->nextpc = cpu->lr & ~3u;
		return 1;
	}
	if (!OsxGuestQuartzChainStart(cpu, context, QUARTZGETSTRING))
		return 0;
	cpu->gpr[3] = QUARTZHOSTSTRINGSIZE * 2 + sizeof(OSXQUARTZEXTENSIONS);
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHHOSTALLOCATE, bridge->allocate);
	return 1;
}

static void OsxGuestQuartzHostStringsStore(OsxCpu *cpu, OsxQuartzContext *context)
{
	static const uint32 names[2] = { GLBRIDGERENDERER, GLBRIDGEVENDOR };
	char text[QUARTZHOSTSTRINGSIZE];
	int index;
	for (index = 0; index < 2; index++) {
		OsxGl::OsxGuestEngineHostString(context->gl, cpu, names[index], text, sizeof(text));
		OsxGuestGlGuestStore(cpu, context->hoststrings + index * QUARTZHOSTSTRINGSIZE, (const uint8 *)text, sizeof(text));
	}
	OsxGuestGlGuestStore(cpu, context->hoststrings + QUARTZHOSTSTRINGSIZE * 2, (const uint8 *)OSXQUARTZEXTENSIONS, sizeof(OSXQUARTZEXTENSIONS));
}

static void OsxGuestQuartzDestroyContext(OsxQuartzBridge *bridge, OsxQuartzContext *context)
{
	OsxQuartzContext **link;
	for (link = &bridge->contexts; *link; link = &(*link)->next) {
		if (*link == context) {
			*link = context->next;
			break;
		}
	}
	OsxGuestQuartzFreeContext(context);
}

static void OsxGuestQuartzChainAdvance(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	switch (context->flushstep) {
	case QUARTZFLUSHQUERY:
		OsxGuestQuartzWindowQueried(bridge, cpu, context);
		break;
	case QUARTZFLUSHWINDOWRECT:
		OsxGuestQuartzWindowRectDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHSURFACERECT:
		OsxGuestQuartzSurfaceRectDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHWINDOWLIST:
		OsxGuestQuartzWindowListDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHOCCLUDER:
		OsxGuestQuartzOccluderDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHOPACITY:
		OsxGuestQuartzOpacityDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHOFFSCREEN:
		context->offscreenready = 1;
		OsxGuestQuartzOffscreenStore(cpu, context);
		OsxGuestQuartzChainFinish(cpu, context);
		break;
	case QUARTZFLUSHHOSTALLOCATE:
		context->hoststrings = cpu->gpr[3];
		if (!cpu->gpr[3]) {
			context->flushfirst = 0;
			OsxGuestQuartzChainFinish(cpu, context);
			break;
		}
		cpu->gpr[4] = QUARTZHOSTSTRINGSIZE * 2 + sizeof(OSXQUARTZEXTENSIONS);
		OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHHOSTZERO, bridge->clear);
		break;
	case QUARTZFLUSHHOSTZERO:
		OsxGuestQuartzHostStringsStore(cpu, context);
		context->flushfirst = OsxGuestQuartzHostStringAddress(context);
		OsxGuestQuartzChainFinish(cpu, context);
		break;
	}
}

static int OsxGuestQuartzGlCall(OsxQuartzBridge *bridge, OsxQuartzContext *context, OsxCpu *cpu, uint32 call)
{
	int sub;
	if (call == QUARTZGETSTRING) {
		if (!context->strings)
			return 0;
		if ((cpu->gpr[4] == GLBRIDGEEXTENSIONS || cpu->gpr[4] == 0x8874) && !context->extensions)
			return 0;
		if (cpu->gpr[4] == GLBRIDGEEXTENSIONS)
			cpu->gpr[3] = context->strings;
		else if (cpu->gpr[4] == GLBRIDGEVERSION)
			cpu->gpr[3] = context->strings + OSXQUARTZVERSIONSTRING - OSXQUARTZSTRINGS;
		else if (cpu->gpr[4] == 0x8874)
			cpu->gpr[3] = context->strings + sizeof(OSXQUARTZEXTENSIONS) - 1;
		else
			return 0;
		return 1;
	}
	if (call == 589 || call == 590) {
		OsxGl::OsxGuestEngineFlush(context->gl, cpu);
		return 1;
	}
	if (call == 591) {
		if (OsxGuestQuartzWindowContext(bridge, context) && context->original[call])
			return 0;
		return 1;
	}
	if (OsxGl::OsxGuestEngineProgramCall(context->gl, cpu, call))
		return 1;
	if (call == 340 || call == 341 || call == 457 || call == 460)
		return 1;
	if (call == 470) {
		cpu->gpr[3] = 1;
		return 1;
	}
	if (call == 471) {
		OsxGl::OsxGuestEngineFlush(context->gl, cpu);
		return 1;
	}
	if ((call == 73 || call == 64) && (cpu->gpr[4] == 0x8620 || cpu->gpr[4] == 0x8804)) {
		OsxGl::OsxGuestEngineProgramEnable(context->gl, cpu, cpu->gpr[4], call == 73);
		return 1;
	}
	if (call == 337) {
		OsxGl::OsxGuestEngineBlendSeparate(context->gl, cpu, 1);
		return 1;
	}
	if (call == 8)
		OsxGl::OsxGuestEngineBlendSeparate(context->gl, cpu, 0);
	sub = OsxGuestQuartzEngineSub(call);
	if (sub < 0)
		return 0;
	if (call == QUARTZSLOTVIEWPORT) {
		memcpy(context->viewport, &cpu->gpr[4], sizeof(context->viewport));
		context->appviewport = 1;
		if (!context->pbuffer)
			OsxGuestQuartzEnsureTarget(bridge, cpu, context, cpu->gpr[4] + cpu->gpr[6], cpu->gpr[5] + cpu->gpr[7]);
	}
	OsxGl::OsxGuestEngineCall(context->gl, cpu, (uint32)sub);
	return 1;
}





int OsxGuestQuartzBridgeCall(OsxQuartzBridge *bridge, OsxCpu *cpu, const uint32 *tails, uint32 call, FILE *log)
{
	void *share;
	OsxQuartzContext *context;
	OsxQuartzContext *candidate;
	uint32 base;
	uint32 engine;
	uint32 address;
	uint32 value;
	uint32 slot;
	uint32 next;
	int result;
	context = NULL;
	for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
		if (candidate->flushstep != QUARTZFLUSHIDLE && candidate->segment == cpu->sr[0] && candidate->flushstack == cpu->gpr[1]) {
			OsxGuestQuartzChainAdvance(bridge, cpu, candidate);
			return 1;
		}
	}
	if (call == 1013) {
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, log);
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				candidate->connection = cpu->gpr[4];
				candidate->windowid = cpu->gpr[5];
				candidate->surfaceid = cpu->gpr[6];
				candidate->surfaceknown = 1;
				candidate->drawableflush = 0;
				candidate->geometryvalid = 0;
				break;
			}
		}
		return 1;
	}
	if (call == 1016) {
		cpu->gpr[0] = cpu->lr;
		OsxGuestQuartzRelaxPixelFormat(bridge, cpu, cpu->gpr[3]);
		return 1;
	}
	if (call == 1015) {
		cpu->gpr[0] = cpu->lr;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				if (candidate->flushstep == QUARTZFLUSHIDLE)
					OsxGuestQuartzDestroyContext(bridge, candidate);
				break;
			}
		}
		return 1;
	}
	if (call == 1014) {
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, log);
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				candidate->offscreenwidth = cpu->gpr[4];
				candidate->offscreenheight = cpu->gpr[5];
				candidate->offscreenrowbytes = cpu->gpr[6];
				candidate->offscreenaddress = cpu->gpr[7];
				candidate->offscreenready = 0;
				OsxGuestQuartzEnsureTarget(bridge, cpu, candidate, candidate->offscreenwidth, candidate->offscreenheight);
				break;
			}
		}
		return 1;
	}
	if (call == 1017) {
		cpu->gpr[0] = cpu->lr;
		value = 0;
		if (cpu->gpr[5] == 73)
			value = 1;
		else if (cpu->gpr[5] == 120 || cpu->gpr[5] == 121)
			value = 0x8000000;
		if (value && OsxGuestCpuWriteVirtual(cpu, cpu->gpr[6], value)) {
			cpu->gpr[3] = 0;
			cpu->nextpc = cpu->lr & ~3u;
		}
		return 1;
	}
	if (call == 1018) {
		cpu->gpr[0] = cpu->lr;
		if (!OsxGuestCpuReadVirtual(cpu, cpu->gpr[3], &engine))
			return 1;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->engine == engine) {
				candidate->drawableflush = 1;
				if (OsxGuestQuartzWindowContext(bridge, candidate) && OsxGuestQuartzWindowBegin(bridge, cpu, candidate, call))
					return 1;
				OsxGuestQuartzPresent(bridge, cpu, candidate);
				cpu->gpr[3] = 0;
				cpu->nextpc = cpu->lr & ~3u;
				break;
			}
		}
		return 1;
	}
	if (call == 1019) {
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, log);
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				if (!OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 24, &address) ||
					!OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 28, &value) ||
					!address || !value || address > 4096 || value > 4096)
					break;
				if (OsxGuestQuartzAttachPbuffer(bridge, cpu, candidate, cpu->gpr[4], address, value)) {
					cpu->gpr[3] = 0;
					cpu->nextpc = cpu->lr & ~3u;
				}
				break;
			}
		}
		return 1;
	}
	if (call == 1020) {
		if (!OsxGuestQuartzPbufferTexture(bridge, cpu))
			cpu->gpr[0] = cpu->lr;
		return 1;
	}
	if (call == 1022) {
		value = cpu->gpr[0];
		engine = cpu->gpr[3];
		cpu->gpr[3] -= 4;
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, log);
		cpu->gpr[0] = value;
		cpu->gpr[3] = engine;
		cpu->nextpc = cpu->lr & ~3u;
		return 1;
	}
	if (!call || call == 1023) {
		cpu->gpr[0] = cpu->lr;
		address = cpu->gpr[3];
		base = bridge->library;
		if (!call)
			base = cpu->pc - tails[0];
		if (!address || !OsxGuestCpuReadVirtual(cpu, address, &engine) || !engine ||
			!OsxGuestCpuReadVirtual(cpu, base + tails[0], &value) || value != OSXQUARTZTRAP)
			return 1;
		share = NULL;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment != cpu->sr[0])
				continue;
			share = candidate->gl;
			if (candidate->engine == engine)
				context = candidate;
		}
		if (!context) {
			if (!OsxGuestQuartzStartHost(bridge))
				return 1;
			context = (OsxQuartzContext *)calloc(1, sizeof(*context));
			if (!context)
				return 1;
			context->gl = OsxGl::OsxGuestEngineCreate(share);
			context->address = address;
			context->engine = engine;
			if (bridge->strings)
				context->strings = base + bridge->strings;
			context->extensions = bridge->extensions;
			context->segment = cpu->sr[0];
			context->next = bridge->contexts;
			bridge->contexts = context;
			if (log)
				fprintf(log, "quartz context %08x engine %08x segment %08x base %08x\n", address, engine, context->segment, base);
		}
		context->address = address;
		for (slot = 1; slot < OSXQUARTZSLOTS; slot++) {
			if (!tails[slot] || !OsxGuestCpuReadVirtual(cpu, address + slot * 4, &value) || !value)
				continue;
			if (value != base + tails[slot]) {
				context->original[slot] = value;
				OsxGuestCpuWriteVirtual(cpu, address + slot * 4, base + tails[slot]);
			}
		}
		return 1;
	}
	if (call >= OSXQUARTZSLOTS || !tails[call])
		return 0;
	for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
		if (candidate->segment == cpu->sr[0] && candidate->engine == cpu->gpr[3]) {
			context = candidate;
			break;
		}
	}
	if (!context) {
		if ((cpu->ctr & ~3u) == cpu->pc)
			return 0;
		cpu->nextpc = cpu->ctr & ~3u;
		return 1;
	}
	OsxGl::OsxGuestEngineBind(context->gl, cpu);
	if (call == QUARTZGETSTRING && OsxGuestQuartzHostString(bridge, cpu, context))
		return 1;
	if (!context->appviewport && !context->offscreenaddress && !context->pbuffer && OsxGuestQuartzCallDraws(call))
		OsxGuestQuartzEnsureTarget(bridge, cpu, context, bridge->width, bridge->height);
	next = cpu->nextpc;
	result = OsxGuestQuartzGlCall(bridge, context, cpu, call);
	if (cpu->nextpc != next)
		return 1;
	if (result) {
		cpu->nextpc = cpu->lr & ~3u;
		if (call == 591 || (call == 91 && !context->drawableflush && !OsxGuestQuartzWindowContext(bridge, context)))
			OsxGuestQuartzPresent(bridge, cpu, context);
		if (context->offscreenaddress && (call == 90 || call == 91 || call == 589 || call == 590))
			OsxGuestQuartzOffscreenFlush(bridge, cpu, context, call);
		else if (call == 91 && !context->drawableflush && OsxGuestQuartzWindowContext(bridge, context))
			OsxGuestQuartzWindowBegin(bridge, cpu, context, call);
	} else {
		if (context->original[call])
			cpu->nextpc = context->original[call];
		else
			cpu->nextpc = cpu->ctr & ~3u;
	}
	return 1;
}

#endif
