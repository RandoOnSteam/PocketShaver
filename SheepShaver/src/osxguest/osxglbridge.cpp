/*
 *  osxglbridge.cpp - host OpenGL bridge for Mac OS X guest Quartz OpenGL contexts
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

static void OsxGuestGlModeDescription(DMCModeDesc *mode, uint32 width, uint32 height, uint32 rowbytes, uint8 *present)
{
	memset(mode, 0, sizeof(*mode));
	mode->width = width;
	mode->height = height;
	mode->depth = 32;
	mode->row_bytes = rowbytes;
	mode->pitch = rowbytes;
	mode->screen_base_host = present;
}

static int OsxGuestGlStartHost(uint32 width, uint32 height, uint8 *vram, uint32 rowbytes, OsxHostVideo *video, uint8 **present)
{
	DMCModeDesc mode;
	if (MetalCompositorIsInitialized())
		return 1;
	*present = (uint8 *)malloc(rowbytes * height);
	if (!*present)
		return 0;
	memcpy(*present, vram, rowbytes * height);
	OsxGuestGlModeDescription(&mode, width, height, rowbytes, *present);
	if (dmc_create(&mode) != kDMCNoErr)
		return 0;
	if (MetalCompositorInit(width, height, GLBRIDGEDEPTH32, rowbytes, rowbytes, *present, rowbytes * height) != 0) {
		MetalCompositorShutdown();
		dmc_shutdown();
		return 0;
	}
	gfxaccel_resources_init();
	OsxGuestHostSuspendFrame(video, *present);
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

static void OsxGuestGlPublishFrame(void *gl, OsxCpu *cpu, uint8 *pixels, uint32 width, uint32 height, uint8 *vram, uint32 rowbytes, uint32 depth)
{
	uint32 row;
	OsxGl::OsxGuestEngineReadPixels(gl, cpu, width, height, pixels);
	for (row = 0; row < height; row++) {
		if (depth == OSXDEPTHTHOUSANDS)
			OsxGuestDisplayPackPixels(vram + row * rowbytes, pixels + (height - row - 1) * width * 4, width);
		else
			memcpy(vram + row * rowbytes, pixels + (height - row - 1) * width * 4, width * 4);
	}
}

typedef struct OsxQuartzPbuffer {
	struct OsxQuartzPbuffer *next;
	uint32 address;
	uint32 segment;
	uint32 process;
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
	uint32 process;
	int fullscreen;
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
	int32 pendingrects[QUARTZWINDOWLISTMAX][4];
	uint32 pendingcount;
	uint32 pendingregion;
	int32 ownlevel;
	int idle;
	uint32 idlegpr[32];
	uint32 idlelr;
	uint32 idlectr;
	uint32 idlepc;
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
	uint32 floatprocess;
	OsxQuartzProcessFunction process;
	void *processowner;
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
#define QUARTZFLUSHOWNLEVEL 6
#define QUARTZFLUSHLEVEL 7
#define QUARTZFLUSHOPAQUESHAPE 8
#define QUARTZFLUSHREGIONBOUNDS 9
#define QUARTZFLUSHRELEASEREGION 10
#define QUARTZFLUSHOFFSCREEN 11
#define QUARTZFLUSHHOSTALLOCATE 12
#define QUARTZFLUSHHOSTZERO 13
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
#define QUARTZCGLAUXBUFFERS 7
#define QUARTZCGLCOLORSIZE 8
#define QUARTZCGLALPHASIZE 11
#define QUARTZCGLDEPTHSIZE 12
#define QUARTZCGLSTENCILSIZE 13
#define QUARTZCGLACCUMSIZE 14
#define QUARTZCGLDISPLAYMASK 84
#define QUARTZCGLVIRTUALSCREENCOUNT 128
#define QUARTZCGLFULLSCREEN 54
#define QUARTZCGLSAMPLEBUFFERS 55
#define QUARTZCGLSAMPLES 56
#define QUARTZCGLCOLORFLOAT 58
#define QUARTZCGLRENDERERID 70
#define QUARTZCGLACCELERATED 73
#define QUARTZCGLPBUFFER 90
#define QUARTZCGLREMOTEPBUFFER 91
#define QUARTZCGLVIDEOMEMORY 120
#define QUARTZCGLTEXTUREMEMORY 121
#define QUARTZCGLCOLORSIZEINTEGER 32
#define QUARTZRENDERERMEMORY 0x8000000u
#define QUARTZRENDERERHARDWARE 0x21900u
#define QUARTZSTOREMULTIPLE 47
#define QUARTZSTACKREGISTER 1
#define QUARTZPROLOGUESCAN 3

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
	if (call >= 344 && call <= 403)
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

static void OsxGuestQuartzReturn(OsxCpu *cpu)
{
	uint32 word;
	uint32 address;
	uint32 value;
	uint32 saved;
	int index;
	for (index = 1; index <= QUARTZPROLOGUESCAN; index++) {
		if (!OsxGuestCpuReadVirtual(cpu, cpu->pc - index * 4, &word) || (word >> 26) != QUARTZSTOREMULTIPLE ||
			((word >> 16) & 31) != QUARTZSTACKREGISTER)
			continue;
		address = cpu->gpr[QUARTZSTACKREGISTER] + (uint32)(int32)(int16)(word & 0xffff);
		for (saved = (word >> 21) & 31; saved < 32; saved++, address += 4)
			if (OsxGuestCpuReadVirtual(cpu, address, &value))
				cpu->gpr[saved] = value;
	}
	cpu->nextpc = cpu->lr & ~3u;
}

static int OsxGuestQuartzAttributeHasValue(uint32 attribute)
{
	switch (attribute) {
	case QUARTZCGLAUXBUFFERS: case QUARTZCGLCOLORSIZE: case QUARTZCGLALPHASIZE: case QUARTZCGLDEPTHSIZE:
	case QUARTZCGLSTENCILSIZE: case QUARTZCGLACCUMSIZE: case QUARTZCGLSAMPLEBUFFERS: case QUARTZCGLSAMPLES:
	case QUARTZCGLRENDERERID: case QUARTZCGLDISPLAYMASK: case QUARTZCGLVIRTUALSCREENCOUNT:
		return 1;
	}
	return 0;
}

static void OsxGuestQuartzMarkFloat(OsxQuartzBridge *bridge, OsxCpu *cpu)
{
	bridge->floatsegment = cpu->sr[0];
	bridge->floatprocess = bridge->process(bridge->processowner, cpu);
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
		if (attribute == QUARTZCGLFULLSCREEN || attribute == QUARTZCGLACCELERATED || attribute == QUARTZCGLPBUFFER || attribute == QUARTZCGLREMOTEPBUFFER)
			continue;
		if (attribute == QUARTZCGLCOLORFLOAT)
			OsxGuestQuartzMarkFloat(bridge, cpu);
		if (attribute == QUARTZCGLRENDERERID && OsxGuestCpuReadVirtual(cpu, address + (source + 1) * 4, &value) && value == QUARTZRENDERERHARDWARE) {
			source++;
			continue;
		}
		OsxGuestCpuWriteVirtual(cpu, address + target * 4, attribute);
		target++;
		if (!attribute)
			return;
		if (OsxGuestQuartzAttributeHasValue(attribute)) {
			source++;
			if (!OsxGuestCpuReadVirtual(cpu, address + source * 4, &value))
				return;
			if (attribute == QUARTZCGLSAMPLEBUFFERS || attribute == QUARTZCGLSAMPLES)
				value = 0;
			if (attribute == QUARTZCGLCOLORSIZE && value > QUARTZCGLCOLORSIZEINTEGER)
				OsxGuestQuartzMarkFloat(bridge, cpu);
			OsxGuestCpuWriteVirtual(cpu, address + target * 4, value);
			target++;
		}
	}
}

OsxQuartzBridge *OsxGuestCreateQuartzBridge(uint32 width, uint32 height, uint32 library, uint32 strings, int extensions, int allocatedextensions,
	uint32 allocate, uint32 clear, uint8 *vram, uint32 rowbytes, OsxHostVideo *video, uint8 **present, const uint32 *windowcalls,
	OsxQuartzProcessFunction process, void *processowner)
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
		bridge->process = process;
		bridge->processowner = processowner;
	}
	return bridge;
}

void OsxGuestQuartzResize(OsxQuartzBridge *bridge, uint32 width, uint32 height)
{
	DMCModeDesc mode;
	bridge->width = width;
	bridge->height = height;
	if (!MetalCompositorIsInitialized())
		return;
	OsxGuestGlModeDescription(&mode, width, height, bridge->rowbytes, *bridge->present);
	if (dmc_prepare_mode_switch(&mode) != kDMCNoErr)
		return;
	MetalCompositorResize(width, height, GLBRIDGEDEPTH32, bridge->rowbytes, bridge->rowbytes, *bridge->present, bridge->rowbytes * height);
	dmc_request_mode_switch(&mode);
	OsxGuestHostSuspendFrame(bridge->video, *bridge->present);
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

static int OsxGuestQuartzOwnerLive(const uint32 *processes, const uint32 *spaces, uint32 count, uint32 mask, uint32 segment, uint32 process)
{
	uint32 index;
	for (index = 0; index < count; index++)
		if (processes[index] == process && spaces[index] == (segment & mask))
			return 1;
	return 0;
}

void OsxGuestQuartzKeepProcesses(OsxQuartzBridge *bridge, const uint32 *processes, const uint32 *spaces, uint32 count, uint32 mask)
{
	OsxQuartzContext **context;
	OsxQuartzContext *dead;
	OsxQuartzPbuffer **pbuffer;
	OsxQuartzPbuffer *stale;
	context = &bridge->contexts;
	while (*context) {
		dead = *context;
		if (!OsxGuestQuartzOwnerLive(processes, spaces, count, mask, dead->segment, dead->process) && dead->flushstep == QUARTZFLUSHIDLE) {
			*context = dead->next;
			OsxGuestQuartzFreeContext(dead);
		} else
			context = &dead->next;
	}
	pbuffer = &bridge->pbuffers;
	while (*pbuffer) {
		stale = *pbuffer;
		if (!OsxGuestQuartzOwnerLive(processes, spaces, count, mask, stale->segment, stale->process)) {
			*pbuffer = stale->next;
			free(stale);
		} else
			pbuffer = &stale->next;
	}
	if (bridge->floatsegment && !OsxGuestQuartzOwnerLive(processes, spaces, count, mask, bridge->floatsegment, bridge->floatprocess)) {
		bridge->floatsegment = 0;
		bridge->floatprocess = 0;
	}
}

int OsxGuestQuartzFullScreen(OsxQuartzBridge *bridge)
{
	OsxQuartzContext *context;
	for (context = bridge->contexts; context; context = context->next)
		if (context->fullscreen)
			return 1;
	return 0;
}

static void OsxGuestQuartzDropStale(OsxQuartzBridge *bridge, uint32 segment, uint32 process)
{
	OsxQuartzContext **context;
	OsxQuartzContext *dead;
	context = &bridge->contexts;
	while (*context) {
		dead = *context;
		if (dead->segment == segment && dead->process != process && dead->flushstep == QUARTZFLUSHIDLE) {
			*context = dead->next;
			OsxGuestQuartzFreeContext(dead);
		} else
			context = &dead->next;
	}
}

static OsxQuartzPbuffer *OsxGuestQuartzFindPbuffer(OsxQuartzBridge *bridge, uint32 address, const OsxQuartzContext *context)
{
	OsxQuartzPbuffer *pbuffer;
	for (pbuffer = bridge->pbuffers; pbuffer; pbuffer = pbuffer->next)
		if (pbuffer->address == address && pbuffer->segment == context->segment && pbuffer->process == context->process)
			return pbuffer;
	return NULL;
}

static int OsxGuestQuartzAttachPbuffer(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 address,
	uint32 width, uint32 height)
{
	OsxQuartzPbuffer *pbuffer;
	pbuffer = OsxGuestQuartzFindPbuffer(bridge, address, context);
	if (!pbuffer) {
		pbuffer = (OsxQuartzPbuffer *)calloc(1, sizeof(*pbuffer));
		if (!pbuffer)
			return 0;
		pbuffer->address = address;
		pbuffer->segment = context->segment;
		pbuffer->process = context->process;
		pbuffer->next = bridge->pbuffers;
		bridge->pbuffers = pbuffer;
	}
	if (pbuffer->width != width || pbuffer->height != height) {
		if (pbuffer->texture)
			OsxGl::OsxGuestEngineTextureDelete(context->gl, cpu, pbuffer->texture);
		pbuffer->texture = OsxGl::OsxGuestEngineTextureCreate(context->gl, cpu, width, height, bridge->floatsegment == context->segment && bridge->floatprocess == context->process);
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
	if (!destination)
		return 0;
	pbuffer = OsxGuestQuartzFindPbuffer(bridge, cpu->gpr[4], destination);
	if (!pbuffer)
		return 0;
	OsxGl::OsxGuestEngineCopyTexture(destination->gl, cpu, pbuffer->texture, pbuffer->width, pbuffer->height, internalformat, target);
	cpu->gpr[3] = 0;
	OsxGuestQuartzReturn(cpu);
	return 1;
}

static uint8 *OsxGuestQuartzPixels(OsxQuartzContext *context, uint32 size);

static void OsxGuestQuartzPresent(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context, uint32 depth)
{
	uint8 *pixels;
	context->fullscreen = !context->pbuffer && context->viewport[2] == bridge->width && context->viewport[3] == bridge->height;
	if (!context->fullscreen)
		return;
	OsxGuestQuartzEnsureTarget(bridge, cpu, context, bridge->width, bridge->height);
	pixels = OsxGuestQuartzPixels(context, bridge->width * bridge->height * 4);
	if (pixels)
		OsxGuestGlPublishFrame(context->gl, cpu, pixels, bridge->width, bridge->height, bridge->vram, bridge->rowbytes, depth);
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
	OsxGuestQuartzReturn(cpu);
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
	if (context->idle) {
		memcpy(cpu->gpr, context->idlegpr, sizeof(context->idlegpr));
		cpu->lr = context->idlelr;
		cpu->ctr = context->idlectr;
		context->idle = 0;
		context->flushstep = QUARTZFLUSHIDLE;
		return;
	}
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
		!bridge->windowcalls[OSXQUARTZWINDOWLIST] || !bridge->windowcalls[OSXQUARTZWINDOWLEVEL] ||
		!bridge->windowcalls[OSXQUARTZOPAQUESHAPE] || !bridge->windowcalls[OSXQUARTZREGIONBOUNDS] ||
		!bridge->windowcalls[OSXQUARTZRELEASEREGION])
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
	cpu->gpr[5] = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHOWNLEVEL, bridge->windowcalls[OSXQUARTZWINDOWLEVEL]);
}

static void OsxGuestQuartzOwnLevelDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 level;
	if (cpu->gpr[3] || !OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT, &level)) {
		context->geometryvalid = 0;
		OsxGuestQuartzGeometryFinish(cpu, context);
		return;
	}
	context->ownlevel = (int32)level;
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

static void OsxGuestQuartzSkipOccluder(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	context->windowlistindex++;
	OsxGuestQuartzNextOccluder(bridge, cpu, context);
}

static int OsxGuestQuartzRectsMeet(const int32 *first, const int32 *second)
{
	return first[0] < second[0] + second[2] && first[0] + first[2] > second[0] && first[1] < second[1] + second[3] && first[1] + first[3] > second[1];
}

static void OsxGuestQuartzOccluderDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	int32 *frame;
	frame = context->pendingrects[context->pendingcount];
	if (cpu->gpr[3] || !OsxGuestQuartzReadRect(cpu, context->flushstack + QUARTZFLUSHSCRATCH, frame) ||
		!OsxGuestQuartzRectsMeet(frame, context->windowrect)) {
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowlist[context->windowlistindex];
	cpu->gpr[5] = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHLEVEL, bridge->windowcalls[OSXQUARTZWINDOWLEVEL]);
}

static void OsxGuestQuartzLevelDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	uint32 leveladdress;
	uint32 level;
	leveladdress = context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT;
	if (cpu->gpr[3] || !OsxGuestCpuReadVirtual(cpu, leveladdress, &level)) {
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
		return;
	}
	if ((int32)level <= context->ownlevel) {
		context->pendingcount++;
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
		return;
	}
	if (!OsxGuestCpuWriteVirtual(cpu, leveladdress, 0)) {
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
		return;
	}
	cpu->gpr[3] = context->connection;
	cpu->gpr[4] = context->windowlist[context->windowlistindex];
	cpu->gpr[5] = leveladdress;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHOPAQUESHAPE, bridge->windowcalls[OSXQUARTZOPAQUESHAPE]);
}

static void OsxGuestQuartzOpaqueShapeDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	if (cpu->gpr[3] || !OsxGuestCpuReadVirtual(cpu, context->flushstack + QUARTZFLUSHSCRATCH + QUARTZLISTCOUNT, &context->pendingregion) ||
		!context->pendingregion) {
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
		return;
	}
	cpu->gpr[3] = context->pendingregion;
	cpu->gpr[4] = context->flushstack + QUARTZFLUSHSCRATCH;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHREGIONBOUNDS, bridge->windowcalls[OSXQUARTZREGIONBOUNDS]);
}

static void OsxGuestQuartzRegionBoundsDone(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzContext *context)
{
	int32 frame[4];
	int32 *opaque;
	opaque = context->pendingrects[context->pendingcount];
	memcpy(frame, opaque, sizeof(frame));
	if (!cpu->gpr[3] && OsxGuestQuartzReadRect(cpu, context->flushstack + QUARTZFLUSHSCRATCH, opaque) && opaque[2] > 0 && opaque[3] > 0) {
		if (opaque[0] >= 0 && opaque[1] >= 0 && opaque[0] + opaque[2] <= frame[2] && opaque[1] + opaque[3] <= frame[3]) {
			opaque[0] += frame[0];
			opaque[1] += frame[1];
		}
		context->pendingcount++;
	}
	cpu->gpr[3] = context->pendingregion;
	OsxGuestQuartzGuestCall(cpu, context, QUARTZFLUSHRELEASEREGION, bridge->windowcalls[OSXQUARTZRELEASEREGION]);
}

static uint32 OsxGuestQuartzPixelValue(const uint8 *row, int32 x, uint32 depth)
{
	uint8 pixel[4];
	if (depth == OSXDEPTHTHOUSANDS) {
		OsxGuestDisplayExpandPixels(pixel, row + x * 2, 1);
		row = pixel;
		x = 0;
	}
	return ((uint32)row[x * 4 + 1] << 16) | ((uint32)row[x * 4 + 2] << 8) | row[x * 4 + 3];
}

static void OsxGuestQuartzBlendPixel(uint8 *target, const uint8 *gl, uint32 screen, uint32 backing)
{
	uint32 base;
	uint32 value;
	int channel;
	target[0] = gl[0];
	for (channel = 1; channel < 4; channel++) {
		base = (backing >> ((3 - channel) * 8)) & 0xff;
		value = (screen >> ((3 - channel) * 8)) & 0xff;
		if (base)
			value = gl[channel] * value / base;
		if (value > 0xff)
			value = 0xff;
		target[channel] = (uint8)value;
	}
}

static int OsxGuestQuartzInsideRect(const int32 *rect, int32 x, int32 y)
{
	return x >= rect[0] && x < rect[0] + rect[2] && y >= rect[1] && y < rect[1] + rect[3];
}

static int OsxGuestQuartzInsideOccluder(OsxQuartzContext *context, const int32 *active, uint32 overlaps, int32 x, int32 y)
{
	uint32 occluder;
	for (occluder = 0; occluder < overlaps; occluder++)
		if (OsxGuestQuartzInsideRect(context->occluders[active[occluder]], x, y))
			return 1;
	return 0;
}

static uint32 OsxGuestQuartzBackingPixel(OsxQuartzBridge *bridge, OsxQuartzContext *context, const int32 *active, uint32 overlaps,
	int32 left, int32 top, int32 right, int32 bottom, uint32 depth)
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
			if (OsxGuestQuartzInsideOccluder(context, active, overlaps, x, y))
				continue;
			value = OsxGuestQuartzPixelValue(bridge->vram + y * bridge->rowbytes, x, depth);
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

static void OsxGuestQuartzComposeWindow(OsxQuartzBridge *bridge, OsxQuartzContext *context, uint8 *present, uint32 depth)
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
	uint8 *pixels;
	uint8 *target;
	uint32 screen;
	uint32 backing;
	int32 surface[4];
	int32 visiblebounds[4];
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
	surfacetop = context->windowrect[1] + context->surfacerect[1];
	overlaps = 0;
	surface[0] = surfaceleft;
	surface[1] = surfacetop;
	surface[2] = width;
	surface[3] = height;
	for (index = 0; index < context->occludercount; index++)
		if (OsxGuestQuartzRectsMeet(context->occluders[index], surface))
			active[overlaps++] = (int32)index;
	visiblebounds[0] = surfaceleft;
	visiblebounds[1] = surfacetop;
	visiblebounds[2] = surfaceleft + width;
	visiblebounds[3] = surfacetop + height;
	if (visiblebounds[0] < 0)
		visiblebounds[0] = 0;
	if (visiblebounds[1] < 0)
		visiblebounds[1] = 0;
	if (visiblebounds[2] > (int32)bridge->width)
		visiblebounds[2] = (int32)bridge->width;
	if (visiblebounds[3] > (int32)bridge->height)
		visiblebounds[3] = (int32)bridge->height;
	backing = OsxGuestQuartzBackingPixel(bridge, context, active, overlaps, visiblebounds[0], visiblebounds[1], visiblebounds[2], visiblebounds[3], depth);
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
		for (x = first; x < last; x++) {
			if (OsxGuestQuartzInsideOccluder(context, active, overlaps, surfaceleft + x, surfacetop + y))
				continue;
			target = present + (surfacetop + y) * bridge->rowbytes + (surfaceleft + x) * 4;
			screen = OsxGuestQuartzPixelValue(bridge->vram + (surfacetop + y) * bridge->rowbytes, surfaceleft + x, depth);
			if (screen == backing)
				memcpy(target, pixels + ((height - 1 - y) * width + x) * 4, 4);
			else if (overlaps)
				OsxGuestQuartzBlendPixel(target, pixels + ((height - 1 - y) * width + x) * 4, screen, backing);
		}
	}
}

void OsxGuestQuartzComposeWindows(OsxQuartzBridge *bridge, uint8 *present, uint32 depth)
{
	OsxQuartzContext *context;
	if (!bridge)
		return;
	for (context = bridge->contexts; context; context = context->next)
		OsxGuestQuartzComposeWindow(bridge, context, present, depth);
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
		OsxGuestQuartzReturn(cpu);
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
	case QUARTZFLUSHOWNLEVEL:
		OsxGuestQuartzOwnLevelDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHLEVEL:
		OsxGuestQuartzLevelDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHOPAQUESHAPE:
		OsxGuestQuartzOpaqueShapeDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHREGIONBOUNDS:
		OsxGuestQuartzRegionBoundsDone(bridge, cpu, context);
		break;
	case QUARTZFLUSHRELEASEREGION:
		OsxGuestQuartzSkipOccluder(bridge, cpu, context);
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





int OsxGuestQuartzIdleGeometry(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzEventLoopFunction eventloop, void *owner)
{
	OsxQuartzContext *context;
	OsxQuartzContext *stalecontext;
	uint64 now;
	uint32 idlestack;
	stalecontext = NULL;
	now = OsxGuestHostNanoseconds();
	for (context = bridge->contexts; context; context = context->next) {
		if (context->segment != cpu->sr[0])
			continue;
		if (context->idle) {
			if (cpu->pc != context->idlepc || cpu->gpr[1] != context->flushstack)
				return 0;
			OsxGuestQuartzChainAdvance(bridge, cpu, context);
			return context->idle;
		}
		if (context->windowshown && context->surfaceknown && context->flushstep == QUARTZFLUSHIDLE &&
			now - context->geometrytime > QUARTZGEOMETRYPERIOD)
			stalecontext = context;
	}
	if (!stalecontext || !eventloop(owner, cpu))
		return 0;
	idlestack = (cpu->gpr[1] - QUARTZFLUSHFRAME) & ~15u;
	if (!OsxGuestCpuWriteVirtual(cpu, idlestack, cpu->gpr[1]))
		return 0;
	memcpy(stalecontext->idlegpr, cpu->gpr, sizeof(stalecontext->idlegpr));
	stalecontext->idlelr = cpu->lr;
	stalecontext->idlectr = cpu->ctr;
	stalecontext->idlepc = cpu->pc;
	stalecontext->flushstack = idlestack;
	stalecontext->idle = 1;
	OsxGuestQuartzGeometryBegin(bridge, cpu, stalecontext);
	return 1;
}

int OsxGuestQuartzBridgeCall(OsxQuartzBridge *bridge, OsxCpu *cpu, const uint32 *tails, uint32 call, uint32 depth, FILE *log)
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
	uint32 process;
	int result;
	context = NULL;
	for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
		if (candidate->flushstep != QUARTZFLUSHIDLE && candidate->segment == cpu->sr[0] && candidate->flushstack == cpu->gpr[1]) {
			OsxGuestQuartzChainAdvance(bridge, cpu, candidate);
			return 1;
		}
	}
	if (call == 1013) {
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, depth, log);
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
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, depth, log);
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
	if (call == 1011) {
		cpu->gpr[3] = 0;
		OsxGuestQuartzReturn(cpu);
		return 1;
	}
	if (call == 1017 || call == 1012) {
		cpu->gpr[0] = cpu->lr;
		value = 0;
		if (cpu->gpr[5] == QUARTZCGLACCELERATED || (call == 1017 && cpu->gpr[5] == QUARTZCGLFULLSCREEN))
			value = 1;
		else if (cpu->gpr[5] == QUARTZCGLVIDEOMEMORY || cpu->gpr[5] == QUARTZCGLTEXTUREMEMORY)
			value = QUARTZRENDERERMEMORY;
		else if (call == 1017 && cpu->gpr[5] == QUARTZCGLRENDERERID && cpu->gpr[4] == 0)
			value = QUARTZRENDERERHARDWARE;
		if (value && OsxGuestCpuWriteVirtual(cpu, cpu->gpr[6], value)) {
			cpu->gpr[3] = 0;
			OsxGuestQuartzReturn(cpu);
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
				OsxGuestQuartzPresent(bridge, cpu, candidate, depth);
				cpu->gpr[3] = 0;
				OsxGuestQuartzReturn(cpu);
				break;
			}
		}
		return 1;
	}
	if (call == 1019) {
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, depth, log);
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				if (!OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 24, &address) ||
					!OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 28, &value) ||
					!address || !value || address > 4096 || value > 4096)
					break;
				if (OsxGuestQuartzAttachPbuffer(bridge, cpu, candidate, cpu->gpr[4], address, value)) {
					cpu->gpr[3] = 0;
					OsxGuestQuartzReturn(cpu);
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
		OsxGuestQuartzBridgeCall(bridge, cpu, tails, 1023, depth, log);
		cpu->gpr[0] = value;
		cpu->gpr[3] = engine;
		OsxGuestQuartzReturn(cpu);
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
		process = bridge->process(bridge->processowner, cpu);
		OsxGuestQuartzDropStale(bridge, cpu->sr[0], process);
		share = NULL;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment != cpu->sr[0] || candidate->process != process)
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
			context->process = process;
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
		OsxGuestQuartzReturn(cpu);
		if (call == 591 || (call == 91 && !context->drawableflush && !OsxGuestQuartzWindowContext(bridge, context)))
			OsxGuestQuartzPresent(bridge, cpu, context, depth);
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
