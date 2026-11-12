/*
 *  osxglengine.cpp - gfxaccel GL engine built for Mac OS X guest address spaces
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
#define OSXGLGUEST 1
#include "sysdeps.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include "cpu_emulation.h"
#include "vec_data.h"
#include "macos_util.h"
#include "video.h"
#include "rom_patches.h"
#include "thunks.h"
#include "gfxaccel_backend.h"
#include "gfx_log.h"
#include "gfxaccel_resources.h"
#include "display_mode_controller.h"
#include "metal_compositor.h"
#include "metal_device_shared.h"
#include "rave_metal_renderer.h"
#include "dsp_draw_context.h"
#if defined(GFXACCEL_USE_SDLGPU)
#include "sdlgpu_gl_compat.h"
#endif
#include "gl_ext.h"
#include "gl_agl_renderer_policy.h"
#include "gl_clip_plane_policy.h"
#include "gl_color_index_policy.h"
#include "gl_drawable_owner_policy.h"
#include "gl_metal_draw_state.h"
#include "gl_offscreen_policy.h"
#define GLPixelUnpackRGBA5551ToBGRA GLPixelUnpackClassicRGBA5551ToBGRA
#include "gl_pixel_unpack_policy.h"
#undef GLPixelUnpackRGBA5551ToBGRA
static inline void GLPixelUnpackRGBA5551ToBGRA(uint16_t packed, bool reversed, uint32_t format, uint8_t out_bgra[4])
{
	const uint8_t channelbits[4] = {5, 5, 5, 1};
	GLPixelUnpackPackedToBGRA(packed, channelbits, 4, reversed, format, out_bgra);
}
#include "debug.h"
#include "osxcpu.h"
#include "osxglbridge.h"

#define OSXGLSCRATCHBASE 0xfffc0000u
#define OSXGLSCRATCHWORD 512u
#define OSXGLSTACKSCRATCH 32u

#define ReadMacInt8 OsxGuestRead8
#define ReadMacInt16 OsxGuestRead16
#define ReadMacInt32 OsxGuestRead32
#define WriteMacInt8 OsxGuestWrite8
#define WriteMacInt16 OsxGuestWrite16
#define WriteMacInt32 OsxGuestWrite32
#define Mac2HostAddr OsxGuestHostAddress
#define Mac_sysalloc OsxGuestAllocate
#define Mac_memset OsxGuestSet
#define Mac2Host_memcpy OsxGuestCopyIn
#define Host2Mac_memcpy OsxGuestCopyOut
#define GLGUESTBYTES OsxGuestBytes
#define gfxaccel_resources_vend_overlay_texture_indexed OsxGuestOverlayVend
#define gfxaccel_resources_release_overlay_texture OsxGuestOverlayRelease
#define dmc_set_active_owner(owner) kDMCNoErr
#define GLContextGetOffscreenDrawable OsxGLContextGetOffscreenDrawable
#define GLFFPRenderPassActive OsxGLFFPRenderPassActive
#define GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentDirtyRect OsxGLCompositeDirtyRect
#define GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentIfNotSuppressed OsxGLCompositeIfNotSuppressed
#define NQDReadMainDevicePixMapForGLBridge OsxNQDReadMainDevicePixMap
#define gl_overlay_bind osx_gl_overlay_bind
#define gl_overlay_unbind osx_gl_overlay_unbind
#define gl_overlay_present osx_gl_overlay_present
#define gl_has_active_overlay osx_gl_has_active_overlay
#define gl_get_overlay_dims osx_gl_get_overlay_dims
#define gl_release_overlay_for_detach osx_gl_release_overlay_for_detach

namespace OsxGl {

uint8 OsxGuestRead8(uint32 address);
uint16 OsxGuestRead16(uint32 address);
uint32 OsxGuestRead32(uint32 address);
static uint8 *OsxGuestByte(uint32 address, int kind);
void OsxGuestWrite8(uint32 address, uint32 value);
void OsxGuestWrite16(uint32 address, uint32 value);
void OsxGuestWrite32(uint32 address, uint32 value);
uint8 *OsxGuestHostAddress(uint32 address);
uint32 OsxGuestAllocate(uint32 size);
void *OsxGuestSet(uint32 address, int value, size_t length);
void *OsxGuestCopyIn(void *destination, uint32 source, size_t length);
void *OsxGuestCopyOut(uint32 destination, const void *source, size_t length);
const uint8 *OsxGuestBytes(uint32 address, int32_t length);
void *OsxGuestOverlayVend(int engine, int index, uint32_t width, uint32_t height, uint32_t format);
void OsxGuestOverlayRelease(int engine, void *texture);

bool RaveGetLastCL8ColorTableRGBSnapshot(unsigned char *table)
{
	(void)table;
	return false;
}

#include "gl_engine.h"
#include "gfxaccelhost.h"
void OsxGuestCopyFramebufferToRectangleTexture(GLContext *context, int level, uint32 internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage);
void OsxGuestBindRectangleTexture(GLContext *context);
void OsxGuestEnableRectangleTexture(GLContext *context, bool enabled);
void OsxGuestUploadRectangleTexture(GLContext *context, GLTextureObject *textureobject, int level, int xoffset, int yoffset, int width, int height, const uint8_t *pixels, int replacewholeimage);
void OsxGuestApplyRectangleSampler(GLContext *context, GLTextureObject *textureobject);
#define GUESTPIXELFORMATBGR 0x80e0u
#define GUESTPIXELFORMATBGRA 0x80e1u
#define GUESTPIXELFORMATLUMINANCE 0x1909u
#define GUESTINTERNALFORMATINTENSITYFIRST 0x8049u
#define GUESTINTERNALFORMATINTENSITYLAST 0x804du
#define GUESTPIXELTYPEUNSIGNEDSHORT5551 0x8034u
#define GUESTPIXELTYPEUNSIGNEDINT8888 GFXACCELHOSTUNSIGNEDINT8888
#define GUESTPIXELTYPEUNSIGNEDSHORT565 0x8363u
#define GUESTPIXELTYPEUNSIGNEDSHORT565REVERSED 0x8364u
#define GUESTPIXELTYPEUNSIGNEDSHORT1555REVERSED 0x8366u
#define GUESTPIXELTYPEUNSIGNEDINT8888REVERSED 0x8367u
#define GUESTPIXELTYPEHALFFLOAT 0x140bu
#define GUESTHALFFLOATONE 0x3c00u
#define GUESTHALFFLOATMANTISSAMASK 0x3ffu
#define GUESTSTATEMATRIXMODE 0x0ba0u
#define GUESTSTATEBLEND 0x0be2u
#define GUESTSTATEDRAWBUFFER 0x0c01u
#define GUESTSTATEREADBUFFER 0x0c02u
#define GUESTSTATEMAXVIEWPORTDIMENSIONS 0x0d3au
#define GUESTSTATEBLENDEQUATION 0x8009u
#define GUESTSTATERECTANGLEBINDING 0x84f6u
#define GUESTSTATEMAXRECTANGLETEXTURESIZE 0x84f8u
#define GUESTSTATEMAXTEXTURECOORDINATES 0x8871u
#define GUESTSTATEMAXTEXTUREIMAGEUNITS 0x8872u
#define GUESTUNPACKROWBYTES 0x8a16u
#define GUESTPROXYTEXTURE2D 0x8064u
#define GUESTPROXYTEXTURERECTANGLE 0x84f7u
#define GUESTTEXTURESTORAGEHINT 0x85bcu
#define GUESTPROGRAMLENGTH 0x8627u
#define GUESTTEXTURERECTANGLE 0x84f5u
#define GUESTPROGRAMERRORPOSITION 0x864bu
#define GUESTHOSTLIMITMAXIMUM 2048
#define GUESTFIXEDTEXTURECOORDINATES 8
#define GUESTFIXEDTEXTUREIMAGEUNITS 16
#include "../gfxaccel/gl_state.cpp"
#include "../gfxaccel/gl_defer.cpp"
#include "../gfxaccel/gl_dispatch.cpp"
#include "../gfxaccel/gl_thunks.cpp"
#define ReadMacDouble EngineReadMacDouble
#include "../gfxaccel/gl_engine.cpp"
#undef ReadMacDouble
static void OsxGuestBlendFunc(GLenum sourcefactor, GLenum destinationfactor);
#pragma push_macro("glBlendFunc")
#undef glBlendFunc
#define glBlendFunc OsxGuestBlendFunc
static void OsxGuestRasterPosition(GLint unusedx, GLint unusedy);
#pragma push_macro("glRasterPos2i")
#undef glRasterPos2i
#define glRasterPos2i OsxGuestRasterPosition
static void OsxGuestMatrixMode(GLenum mode);
#pragma push_macro("glMatrixMode")
#undef glMatrixMode
#define glMatrixMode OsxGuestMatrixMode
static void OsxGuestLoadMatrix(const GLfloat *matrix);
#pragma push_macro("glLoadMatrixf")
#undef glLoadMatrixf
#define glLoadMatrixf OsxGuestLoadMatrix
static void OsxGuestViewport(GLint x, GLint y, GLsizei width, GLsizei height);
#pragma push_macro("glViewport")
#undef glViewport
#define glViewport OsxGuestViewport
#include "../gfxaccel/gl/gl_ffp_renderer.cpp"
#undef glBlendFunc
#pragma pop_macro("glBlendFunc")
#undef glRasterPos2i
#pragma pop_macro("glRasterPos2i")
#undef glMatrixMode
#pragma pop_macro("glMatrixMode")
#undef glLoadMatrixf
#pragma pop_macro("glLoadMatrixf")
#undef glViewport
#pragma pop_macro("glViewport")
#include "../gfxaccel/gfxaccelhost.cpp"
#include "../gfxaccel/gl/gfxaccelhostopengl.cpp"

struct OsxGuestSharedTextures {
	GLTextureObjectMap storage;
	GLContext *holder;
	uint32_t nexttexturename;
	std::map<uint32_t, bool> intensitytextures;
	int referencecount;
};

struct OsxGuestState {
	void *guestcpu;
	void *guesttarget;
	uint8_t guestscratch[1024];
	uint32_t guestscratchused;
	int32_t guestunpackrowbytes;
	uint32_t guestreadpage;
	uint8_t *guestreadhost;
	bool guestblendseparate;
	uint32_t guestblendsrcalpha;
	uint32_t guestblenddstalpha;
	std::vector<uint8_t> guestbytes;
	OsxGuestSharedTextures *sharedtextures;
	uint32_t boundrectangletexture[8];
	uint32_t guestrectangleenabledunits;
	bool guesttexture2denabled[8];
	bool rectangleshadowactive[8];
	GLTextureObject guestrectangleshadowtexture;
	int guestmaxviewportdimensions[2];
	int guestmaxrectangletexturesize;
	int guestmaxtexturecoordinates;
	int guestmaxtextureimageunits;
	int guestprogramerrorposition;
	int guestproxywidth;
	int guestproxyheight;
	uint32_t guestproxyinternalformat;
	bool guestfaultraised;
};

struct OsxGuestContext : GLContext {
	OsxGuestState state;
};

static OsxGuestState *OsxGuestStateOf(GLContext *context)
{
	return &static_cast<OsxGuestContext *>(context)->state;
}

static void OsxGuestMatrixMode(GLenum mode)
{
	GfxAccelHostMatrixMode(OsxGuestStateOf(gl_current_context)->guesttarget, mode);
}

static void OsxGuestLoadMatrix(const GLfloat *matrix)
{
	GfxAccelHostLoadMatrix(gl_current_context, OsxGuestStateOf(gl_current_context)->guesttarget, matrix);
}

static void OsxGuestViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	GfxAccelHostViewport(gl_current_context, OsxGuestStateOf(gl_current_context)->guesttarget, x, y, width, height);
}

static void OsxGuestBlendFunc(GLenum sourcefactor, GLenum destinationfactor)
{
	OsxGuestState *state;
	int separate;
	state = OsxGuestStateOf(gl_current_context);
	separate = 0;
	if (state->guestblendseparate)
		separate = 1;
	GfxAccelHostBlendFunc(sourcefactor, destinationfactor, separate, state->guestblendsrcalpha, state->guestblenddstalpha);
}

static void OsxGuestRasterPosition(GLint unusedx, GLint unusedy)
{
	(void)unusedx;
	(void)unusedy;
	GfxAccelHostRasterPosition(gl_current_context);
}

static void OsxGuestAcquireTextures(GLContext *context)
{
	OsxGuestSharedTextures *shared;
	shared = OsxGuestStateOf(context)->sharedtextures;
	if (shared->holder == context)
		return;
	if (shared->holder) {
		context->texture_objects.swap(shared->holder->texture_objects);
		context->next_texture_name = shared->holder->next_texture_name;
	} else {
		context->texture_objects.swap(shared->storage);
		if (shared->nexttexturename)
			context->next_texture_name = shared->nexttexturename;
	}
	shared->holder = context;
}

static void *OsxGuestTargetOf(GLContext *context)
{
	return OsxGuestStateOf(context)->guesttarget;
}

void *OsxGuestOverlayVend(int engine, int index, uint32_t width, uint32_t height, uint32_t format)
{
	(void)engine;
	(void)format;
	return GfxAccelHostOverlayVend(OsxGuestTargetOf(gl_current_context), index, width, height);
}

void OsxGuestOverlayRelease(int engine, void *texture)
{
	(void)engine;
	GfxAccelHostOverlayRelease(texture);
}

void *OsxGuestEngineCreate(void *share)
{
	GLContext *initial;
	OsxGuestContext *created;
	GLContext *context;
	InitFuncSignatures();
	gl_scratch_addr = OSXGLSCRATCHBASE;
	initial = GLContextNew(640, 480);
	created = new OsxGuestContext();
	*static_cast<GLContext *>(created) = *initial;
	delete initial;
	context = created;
	OsxGuestStateOf(context)->guesttarget = GfxAccelHostTargetCreate();
	if (share) {
		OsxGuestStateOf(context)->sharedtextures = OsxGuestStateOf((GLContext *)share)->sharedtextures;
		OsxGuestStateOf(context)->sharedtextures->referencecount++;
	} else {
		OsxGuestStateOf(context)->sharedtextures = new OsxGuestSharedTextures();
		OsxGuestStateOf(context)->sharedtextures->referencecount = 1;
		OsxGuestStateOf(context)->sharedtextures->holder = NULL;
	}
	return context;
}

static void OsxGuestDisableRectangleTextures(GLContext *context);
static void OsxGuestApplyRectangleTextures(GLContext *context);

static void OsxGuestCaptureHostLimits(GLContext *context)
{
	OsxGuestStateOf(context)->guestmaxviewportdimensions[0] = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxviewportdimensions[1] = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxrectangletexturesize = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxtexturecoordinates = GUESTFIXEDTEXTURECOORDINATES;
	OsxGuestStateOf(context)->guestmaxtextureimageunits = GUESTFIXEDTEXTUREIMAGEUNITS;
	OsxGuestStateOf(context)->guestprogramerrorposition = -1;
	GfxAccelHostShadowTextureCreate(context, &OsxGuestStateOf(context)->guestrectangleshadowtexture);
}

static void OsxGuestEngineLeave(GLContext *context)
{
	OsxGuestDisableRectangleTextures(context);
	GfxAccelHostTargetLeave(OsxGuestTargetOf(context));
}

void OsxGuestEngineBind(void *engine, OsxCpu *cpu)
{
	GLContext *next;
	next = (GLContext *)engine;
	OsxGuestStateOf(next)->guestcpu = cpu;
	OsxGuestAcquireTextures(next);
	OsxGuestStateOf(next)->guestfaultraised = false;
	if (gl_current_context == next)
		return;
	if (!GfxAccelHostDeviceReady())
		return;
	if (gl_current_context)
		OsxGuestEngineLeave(gl_current_context);
	GfxAccelHostTargetEnter(OsxGuestTargetOf(next));
	gl_current_context = next;
	if (!next->metal) {
		GfxAccelHostInit(next);
		OsxGuestCaptureHostLimits(next);
	}
	GfxAccelHostTargetResume(next, OsxGuestTargetOf(next));
	OsxGuestApplyRectangleTextures(next);
}

void OsxGuestEngineDestroy(void *engine)
{
	GLContext *context;
	context = (GLContext *)engine;
	OsxGuestEngineBind(context, (OsxCpu *)OsxGuestStateOf(context)->guestcpu);
	GfxAccelHostShadowTextureDestroy(&OsxGuestStateOf(context)->guestrectangleshadowtexture);
	GfxAccelHostTargetReleaseOverlays(OsxGuestTargetOf(context));
	if (OsxGuestStateOf(context)->sharedtextures->referencecount > 1) {
		OsxGuestStateOf(context)->sharedtextures->referencecount--;
		if (OsxGuestStateOf(context)->sharedtextures->holder == context) {
			OsxGuestStateOf(context)->sharedtextures->storage.swap(context->texture_objects);
			OsxGuestStateOf(context)->sharedtextures->nexttexturename = context->next_texture_name;
			OsxGuestStateOf(context)->sharedtextures->holder = NULL;
		}
		OsxGuestStateOf(context)->sharedtextures = new OsxGuestSharedTextures();
		OsxGuestStateOf(context)->sharedtextures->referencecount = 1;
	}
	if (gl_current_context == context)
		OsxGuestEngineLeave(context);
	if (context->metal)
		GfxAccelHostRelease(context);
	delete OsxGuestStateOf(context)->sharedtextures;
	GfxAccelHostTargetDestroy(OsxGuestTargetOf(context));
	delete static_cast<OsxGuestContext *>(context);
	gl_current_context = NULL;
}

void OsxGuestEngineResize(void *engine, OsxCpu *cpu, uint32 width, uint32 height)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostOverlayBind((int32_t)width, (int32_t)height);
}

static void OsxGuestEngineDispatch(OsxCpu *cpu, uint32 sub, int first)
{
	uint32 floats[13];
	const GLFuncSignature *signature;
	double value;
	float single;
	int index;
	int count;
	uint32 last;
	memset(floats, 0, sizeof(floats));
	count = 0;
	if (sub < GL_MAX_SUBOPCODE) {
		signature = &gl_func_signatures[sub];
		for (index = 0; index < signature->num_args && index < 16 && count < 13; index++) {
			if (!(signature->float_mask & (1u << index)))
				continue;
			memcpy(&value, &cpu->fpr[1 + count], sizeof(value));
			single = (float)value;
			memcpy(&floats[count], &single, sizeof(single));
			count++;
		}
	}
	gl_ppc_sp = cpu->gpr[1];
	gl_ppc_stack_arg_offset = first - 3;
	OsxGuestWrite32(gl_scratch_addr, sub);
	last = 0;
	if (first == 3)
		last = cpu->gpr[10];
	cpu->gpr[3] = GLDispatch(cpu->gpr[first], cpu->gpr[first + 1], cpu->gpr[first + 2], cpu->gpr[first + 3],
		cpu->gpr[first + 4], cpu->gpr[first + 5], cpu->gpr[first + 6], last, floats, count);
}

static float OsxGuestEngineFloat(OsxCpu *cpu, int index)
{
	double value;
	memcpy(&value, &cpu->fpr[index], sizeof(value));
	return (float)value;
}

static int OsxGuestEngineFastCall(OsxCpu *cpu, uint32 sub)
{
	GLContext *context;
	context = gl_current_context;
	if (context->in_display_list)
		return 0;
	switch (sub) {
	case GL_SUB_VERTEX2F:
		NativeGLVertex2f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2));
		return 1;
	case GL_SUB_VERTEX3F:
		NativeGLVertex3f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2), OsxGuestEngineFloat(cpu, 3));
		return 1;
	case GL_SUB_COLOR3F:
		NativeGLColor3f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2), OsxGuestEngineFloat(cpu, 3));
		return 1;
	case GL_SUB_COLOR4F:
		NativeGLColor4f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2), OsxGuestEngineFloat(cpu, 3), OsxGuestEngineFloat(cpu, 4));
		return 1;
	case GL_SUB_NORMAL3F:
		NativeGLNormal3f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2), OsxGuestEngineFloat(cpu, 3));
		return 1;
	case GL_SUB_TEX_COORD2F:
		NativeGLTexCoord2f(context, OsxGuestEngineFloat(cpu, 1), OsxGuestEngineFloat(cpu, 2));
		return 1;
	}
	return 0;
}

void OsxGuestBindRectangleTexture(GLContext *context)
{
	GfxAccelHostRectangleBind(context, context->active_texture, OsxGuestStateOf(context)->boundrectangletexture[context->active_texture]);
}

void OsxGuestEnableRectangleTexture(GLContext *context, bool enabled)
{
	if (enabled) {
		GfxAccelHostRectangleEnable(context->active_texture, 1);
		OsxGuestStateOf(context)->guestrectangleenabledunits |= 1u << context->active_texture;
	} else {
		GfxAccelHostRectangleEnable(context->active_texture, 0);
		OsxGuestStateOf(context)->guestrectangleenabledunits &= ~(1u << context->active_texture);
	}
}

void OsxGuestApplyRectangleSampler(GLContext *context, GLTextureObject *textureobject)
{
	GfxAccelHostRectangleSampler(context->active_texture, textureobject);
}

void OsxGuestUploadRectangleTexture(GLContext *context, GLTextureObject *textureobject, int level, int xoffset, int yoffset, int width, int height, const uint8_t *pixels, int replacewholeimage)
{
	GfxAccelHostRectangleUpload(context->active_texture, textureobject, level, xoffset, yoffset, width, height, pixels, replacewholeimage);
}

static void OsxGuestApplyRectangleTextures(GLContext *context)
{
	int unit;
	for (unit = 0; unit < GfxAccelHostTextureUnitCount(context); unit++) {
		if (!OsxGuestStateOf(context)->boundrectangletexture[unit] && !(OsxGuestStateOf(context)->guestrectangleenabledunits & (1u << unit)))
			continue;
		GfxAccelHostRectangleApply(context, unit, OsxGuestStateOf(context)->boundrectangletexture[unit], (OsxGuestStateOf(context)->guestrectangleenabledunits & (1u << unit)) != 0);
	}
}

static void OsxGuestDisableRectangleTextures(GLContext *context)
{
	int unit;
	if (!OsxGuestStateOf(context)->guestrectangleenabledunits)
		return;
	for (unit = 0; unit < GfxAccelHostTextureUnitCount(context); unit++) {
		if (!(OsxGuestStateOf(context)->guestrectangleenabledunits & (1u << unit)))
			continue;
		GfxAccelHostRectangleDisable(unit);
	}
	GfxAccelHostRectangleFinish();
}

static int OsxGuestPixelChannels(uint32 format, int *channels)
{
	channels[0] = 2;
	channels[1] = 1;
	channels[2] = 0;
	channels[3] = 3;
	if (format == GL_RGBA)
		return 4;
	if (format == GL_RGB)
		return 3;
	if (format == GUESTPIXELFORMATBGRA) {
		channels[0] = 0;
		channels[2] = 2;
		return 4;
	}
	if (format == GUESTPIXELFORMATBGR) {
		channels[0] = 0;
		channels[2] = 2;
		return 3;
	}
	if (format == GL_LUMINANCE_ALPHA) {
		channels[1] = 3;
		return 2;
	}
	if (format == GL_ALPHA) {
		channels[0] = 3;
		return 1;
	}
	if (format == GL_LUMINANCE || format == GL_RED)
		return 1;
	return 0;
}

static uint32 OsxGuestPackedPixel(uint32 type, const uint8_t *values, uint32 *elementbytes)
{
	*elementbytes = 2;
	switch (type) {
	case GUESTPIXELTYPEUNSIGNEDINT8888:
		*elementbytes = 4;
		return ((uint32)values[0] << 24) | ((uint32)values[1] << 16) | ((uint32)values[2] << 8) | values[3];
	case GUESTPIXELTYPEUNSIGNEDINT8888REVERSED:
		*elementbytes = 4;
		return ((uint32)values[3] << 24) | ((uint32)values[2] << 16) | ((uint32)values[1] << 8) | values[0];
	case GUESTPIXELTYPEUNSIGNEDSHORT5551:
		return (uint32)((values[0] >> 3) << 11) | (uint32)((values[1] >> 3) << 6) | (uint32)((values[2] >> 3) << 1) | (uint32)(values[3] >> 7);
	case GUESTPIXELTYPEUNSIGNEDSHORT1555REVERSED:
		return (uint32)((values[3] >> 7) << 15) | (uint32)((values[2] >> 3) << 10) | (uint32)((values[1] >> 3) << 5) | (uint32)(values[0] >> 3);
	case GUESTPIXELTYPEUNSIGNEDSHORT565:
		return (uint32)((values[0] >> 3) << 11) | (uint32)((values[1] >> 2) << 5) | (uint32)(values[2] >> 3);
	case GUESTPIXELTYPEUNSIGNEDSHORT565REVERSED:
		return (uint32)((values[2] >> 3) << 11) | (uint32)((values[1] >> 2) << 5) | (uint32)(values[0] >> 3);
	}
	*elementbytes = 0;
	return 0;
}

static uint32 OsxGuestHalfFloat(uint8_t value)
{
	int highest;
	uint32 mantissa;
	if (!value)
		return 0;
	if (value == 255)
		return GUESTHALFFLOATONE;
	highest = 7;
	while (!(value & (1 << highest)))
		highest--;
	mantissa = ((uint32)value << (10 - highest)) & GUESTHALFFLOATMANTISSAMASK;
	return ((uint32)(highest + 7) << 10) + mantissa;
}

static uint32 OsxGuestScalarPixel(uint32 type, uint8_t value, uint32 *elementbytes)
{
	float number;
	uint32 bits;
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		*elementbytes = 1;
		return value;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
		*elementbytes = 2;
		return (uint32)value * 257;
	case GL_UNSIGNED_INT:
	case GL_INT:
		*elementbytes = 4;
		return (uint32)value * 16843009u;
	case GL_FLOAT:
		*elementbytes = 4;
		number = (float)value / 255.0f;
		memcpy(&bits, &number, sizeof(bits));
		return bits;
	case GUESTPIXELTYPEHALFFLOAT:
		*elementbytes = 2;
		return OsxGuestHalfFloat(value);
	}
	*elementbytes = 0;
	return 0;
}

static int OsxGuestReadPixels(GLContext *context, OsxCpu *cpu)
{
	std::vector<uint8_t> packedrow;
	uint8_t *framebufferpixels;
	uint8_t values[4];
	int channels[4];
	int channelcount;
	int bytecount;
	int component;
	int width;
	int height;
	int row;
	int column;
	uint32 elementbytes;
	uint32 pixelbytes;
	uint32 packalignment;
	uint32 guestrowbytes;
	uint32 packedoffset;
	uint32 element;
	uint32 count;
	uint32 type;
	width = (int)cpu->gpr[6];
	height = (int)cpu->gpr[7];
	type = cpu->gpr[9];
	channelcount = OsxGuestPixelChannels(cpu->gpr[8], channels);
	values[0] = 0;
	values[1] = 0;
	values[2] = 0;
	values[3] = 0;
	OsxGuestPackedPixel(type, values, &elementbytes);
	count = 1;
	if (!elementbytes) {
		OsxGuestScalarPixel(type, 0, &elementbytes);
		count = (uint32)channelcount;
	} else if (channelcount < 3)
		return 0;
	if (!channelcount || !elementbytes || width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return 0;
	pixelbytes = count * elementbytes;
	packalignment = (uint32)context->pixel_store.pack_alignment;
	if (!packalignment)
		packalignment = 4;
	guestrowbytes = (uint32)width;
	if (context->pixel_store.pack_row_length > 0)
		guestrowbytes = (uint32)context->pixel_store.pack_row_length;
	guestrowbytes = (guestrowbytes * pixelbytes + packalignment - 1) & ~(packalignment - 1);
	for (row = 0; row < height; row++)
		if (!OsxGuestByte(cpu->gpr[10] + (uint32)row * guestrowbytes, OSXTLBWRITE) || !OsxGuestByte(cpu->gpr[10] + (uint32)row * guestrowbytes + (uint32)width * pixelbytes - 1, OSXTLBWRITE))
			return 1;
	bytecount = 0;
	framebufferpixels = GfxAccelHostReadFramebufferRect(context, (int)cpu->gpr[4], (int)cpu->gpr[5], width, height, &bytecount);
	if (!framebufferpixels)
		return 1;
	packedrow.resize((size_t)width * pixelbytes);
	for (row = 0; row < height; row++) {
		packedoffset = 0;
		for (column = 0; column < width; column++) {
			const uint8_t *pixel = framebufferpixels + ((size_t)row * width + column) * 4;
			for (component = 0; component < 4; component++) {
				values[component] = 255;
				if (component < channelcount)
					values[component] = pixel[channels[component]];
			}
			if (count == 1) {
				element = OsxGuestPackedPixel(type, values, &elementbytes);
				if (elementbytes == 4) {
					packedrow[packedoffset++] = (uint8_t)(element >> 24);
					packedrow[packedoffset++] = (uint8_t)(element >> 16);
				}
				packedrow[packedoffset++] = (uint8_t)(element >> 8);
				packedrow[packedoffset++] = (uint8_t)element;
				continue;
			}
			for (component = 0; component < channelcount; component++) {
				element = OsxGuestScalarPixel(type, values[component], &elementbytes);
				if (elementbytes == 4) {
					packedrow[packedoffset++] = (uint8_t)(element >> 24);
					packedrow[packedoffset++] = (uint8_t)(element >> 16);
				}
				if (elementbytes >= 2)
					packedrow[packedoffset++] = (uint8_t)(element >> 8);
				packedrow[packedoffset++] = (uint8_t)element;
			}
		}
		OsxGuestCopyOut(cpu->gpr[10] + (uint32)row * guestrowbytes, vec_data(packedrow), packedrow.size());
	}
	free(framebufferpixels);
	return 1;
}

void OsxGuestCopyFramebufferToRectangleTexture(GLContext *context, int level, uint32 internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage)
{
	GLTextureObject *textureobject;
	uint32 guesttexturename;
	guesttexturename = OsxGuestStateOf(context)->boundrectangletexture[context->active_texture];
	if (!guesttexturename)
		return;
	textureobject = &context->texture_objects[guesttexturename];
	if (replacewholeimage && !level) {
		textureobject->width = width;
		textureobject->height = height;
	}
	GfxAccelHostRectangleCopyFramebuffer(context->active_texture, level, internalformat, sourcex, sourcey, width, height, xoffset, yoffset, replacewholeimage);
}



static void OsxGuestSyncRectangleShadow(GLContext *context, int unit)
{
	OsxGuestState *state;
	GLTextureUnit *textureunit;
	GLTextureObjectMap::iterator guesttexture;
	state = OsxGuestStateOf(context);
	textureunit = &context->tex_units[unit];
	guesttexture = context->texture_objects.find(textureunit->bound_texture_2d);
	if (state->boundrectangletexture[unit]) {
		if (!state->rectangleshadowactive[unit]) {
			state->guesttexture2denabled[unit] = textureunit->enabled_2d;
			state->rectangleshadowactive[unit] = true;
		}
		textureunit->enabled_2d = true;
		textureunit->bound_object_2d = &state->guestrectangleshadowtexture;
		if (state->guesttexture2denabled[unit] && guesttexture != context->texture_objects.end() && guesttexture->second.metal_texture)
			textureunit->bound_object_2d = &guesttexture->second;
		return;
	}
	if (!state->rectangleshadowactive[unit])
		return;
	state->rectangleshadowactive[unit] = false;
	textureunit->enabled_2d = state->guesttexture2denabled[unit];
	if (textureunit->bound_object_2d == &state->guestrectangleshadowtexture) {
		textureunit->bound_object_2d = NULL;
		if (guesttexture != context->texture_objects.end())
			textureunit->bound_object_2d = &guesttexture->second;
	}
}

static int OsxGuestStateQuery(GLContext *context, uint32 name, int *values)
{
	OsxGuestState *state;
	state = OsxGuestStateOf(context);
	switch (name) {
	case GUESTSTATEMATRIXMODE: values[0] = (int)context->matrix_mode; return 1;
	case GUESTSTATEDRAWBUFFER: values[0] = (int)context->draw_buffer; return 1;
	case GUESTSTATEREADBUFFER: values[0] = (int)context->read_buffer; return 1;
	case GUESTSTATEBLENDEQUATION: values[0] = (int)context->blend_equation; return 1;
	case GUESTSTATEBLEND: values[0] = (int)context->blend; return 1;
	case GL_PACK_ALIGNMENT: values[0] = context->pixel_store.pack_alignment; return 1;
	case GL_PACK_ROW_LENGTH: values[0] = context->pixel_store.pack_row_length; return 1;
	case GL_UNPACK_ALIGNMENT: values[0] = context->pixel_store.unpack_alignment; return 1;
	case GL_UNPACK_ROW_LENGTH: values[0] = context->pixel_store.unpack_row_length; return 1;
	case GUESTUNPACKROWBYTES: values[0] = state->guestunpackrowbytes; return 1;
	case GUESTSTATERECTANGLEBINDING: values[0] = (int)state->boundrectangletexture[context->active_texture]; return 1;
	case GUESTSTATEMAXRECTANGLETEXTURESIZE: values[0] = state->guestmaxrectangletexturesize; return 1;
	case GUESTSTATEMAXVIEWPORTDIMENSIONS:
		values[0] = state->guestmaxviewportdimensions[0];
		values[1] = state->guestmaxviewportdimensions[1];
		return 2;
	case GUESTSTATEMAXTEXTURECOORDINATES: values[0] = state->guestmaxtexturecoordinates; return 1;
	case GUESTSTATEMAXTEXTUREIMAGEUNITS: values[0] = state->guestmaxtextureimageunits; return 1;
	case GUESTPROGRAMERRORPOSITION: values[0] = state->guestprogramerrorposition; return 1;
	}
	return 0;
}

static uint32 OsxGuestStackWord(OsxCpu *cpu, uint32 offset)
{
	uint32 word;
	word = 0;
	OsxGuestCpuReadVirtual(cpu, cpu->gpr[1] + offset, &word);
	return word;
}

static GLTextureObject *OsxGuestBoundObject(GLContext *context, uint32 target)
{
	uint32 name;
	name = context->tex_units[context->active_texture].bound_texture_2d;
	if (target == GUESTTEXTURERECTANGLE)
		name = OsxGuestStateOf(context)->boundrectangletexture[context->active_texture];
	if (!name)
		return NULL;
	return &context->texture_objects[name];
}

static void OsxGuestIntensityAlpha(uint8_t *bgrapixels, int bytecount)
{
	int byteindex;
	for (byteindex = 0; byteindex + 3 < bytecount; byteindex += 4)
		bgrapixels[byteindex + 3] = bgrapixels[byteindex + 2];
}

static int OsxGuestImageCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	OsxGuestState *state;
	OsxGuestSharedTextures *shared;
	GLTextureObject *object;
	uint8_t *converted;
	int channels[4];
	int bytecount;
	int savedrowlength;
	int level;
	int xoffset;
	int yoffset;
	int width;
	int height;
	int limit;
	uint32 target;
	uint32 internalformat;
	uint32 format;
	uint32 type;
	uint32 pixels;
	uint32 elementbytes;
	bool image;
	bool intensity;
	state = OsxGuestStateOf(context);
	shared = state->sharedtextures;
	image = sub == GL_SUB_TEX_IMAGE2D;
	target = cpu->gpr[4];
	level = (int)cpu->gpr[5];
	internalformat = cpu->gpr[6];
	xoffset = (int)cpu->gpr[6];
	yoffset = (int)cpu->gpr[7];
	width = (int)cpu->gpr[8];
	height = (int)cpu->gpr[9];
	if (image) {
		xoffset = 0;
		yoffset = 0;
		width = (int)cpu->gpr[7];
		height = (int)cpu->gpr[8];
	}
	format = cpu->gpr[10];
	type = OsxGuestStackWord(cpu, 56);
	pixels = OsxGuestStackWord(cpu, 60);
	if (image && (target == GUESTPROXYTEXTURE2D || target == GUESTPROXYTEXTURERECTANGLE)) {
		limit = 1024;
		if (target == GUESTPROXYTEXTURERECTANGLE)
			limit = state->guestmaxrectangletexturesize;
		if (!level) {
			state->guestproxywidth = 0;
			state->guestproxyheight = 0;
			state->guestproxyinternalformat = 0;
			if (width <= limit && height <= limit) {
				state->guestproxywidth = width;
				state->guestproxyheight = height;
				state->guestproxyinternalformat = internalformat;
			}
		}
		return 1;
	}
	if (target != GL_TEXTURE_2D && target != GUESTTEXTURERECTANGLE)
		return 0;
	object = OsxGuestBoundObject(context, target);
	if (!object)
		return 1;
	savedrowlength = context->pixel_store.unpack_row_length;
	if (state->guestunpackrowbytes) {
		OsxGuestPackedPixel(type, (const uint8_t *)channels, &elementbytes);
		if (elementbytes)
			context->pixel_store.unpack_row_length = state->guestunpackrowbytes / (int)elementbytes;
		else {
			OsxGuestScalarPixel(type, 0, &elementbytes);
			context->pixel_store.unpack_row_length = state->guestunpackrowbytes / ((int)elementbytes * OsxGuestPixelChannels(format, channels));
		}
	}
	if (image) {
		intensity = format == GUESTPIXELFORMATLUMINANCE && internalformat >= GUESTINTERNALFORMATINTENSITYFIRST && internalformat <= GUESTINTERNALFORMATINTENSITYLAST;
		shared->intensitytextures[object->name] = intensity;
		object->internal_format_opaque = false;
	} else
		intensity = shared->intensitytextures[object->name];
	converted = NULL;
	bytecount = 0;
	if (pixels && (intensity || target == GUESTTEXTURERECTANGLE))
		converted = ConvertPixelsToBGRA8(pixels, width, height, format, type, context->pixel_store, context, &bytecount);
	if (target == GL_TEXTURE_2D) {
		if (image)
			NativeGLTexImage2D(context, target, level, (int32_t)internalformat, width, height, (int32_t)cpu->gpr[9], format, type, pixels);
		else
			NativeGLTexSubImage2D(context, target, level, xoffset, yoffset, width, height, format, type, pixels);
	}
	if (converted && intensity)
		OsxGuestIntensityAlpha(converted, bytecount);
	if (target == GUESTTEXTURERECTANGLE) {
		if (!level && image) {
			object->width = width;
			object->height = height;
		}
		if (!pixels && image) {
			bytecount = width * height * 4;
			converted = (uint8_t *)calloc(1, (size_t)bytecount);
		}
		if (converted)
			OsxGuestUploadRectangleTexture(context, object, level, xoffset, yoffset, width, height, converted, image);
	} else if (converted && image)
		GfxAccelHostUploadTexture(context, object, level, width, height, converted, bytecount);
	else if (converted)
		GfxAccelHostUploadSubTexture(context, object, level, xoffset, yoffset, width, height, converted, width * 4);
	free(converted);
	context->pixel_store.unpack_row_length = savedrowlength;
	return 1;
}

static int OsxGuestRectangleParameterCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	GLTextureObject *object;
	double number;
	uint32 value;
	uint32 pname;
	object = OsxGuestBoundObject(context, GUESTTEXTURERECTANGLE);
	if (!object)
		return 1;
	pname = cpu->gpr[5];
	value = cpu->gpr[6];
	if (sub == GL_SUB_TEX_PARAMETERF) {
		memcpy(&number, &cpu->fpr[1], sizeof(number));
		value = (uint32)number;
	} else if (sub == GL_SUB_TEX_PARAMETERIV)
		value = OsxGuestRead32(cpu->gpr[6]);
	else if (sub == GL_SUB_TEX_PARAMETERFV) {
		float single;
		uint32 bits;
		bits = OsxGuestRead32(cpu->gpr[6]);
		memcpy(&single, &bits, sizeof(single));
		value = (uint32)single;
	}
	if (pname == GL_TEXTURE_MIN_FILTER)
		object->min_filter = value;
	else if (pname == GL_TEXTURE_MAG_FILTER)
		object->mag_filter = value;
	else if (pname == GL_TEXTURE_WRAP_S)
		object->wrap_s = value;
	else if (pname == GL_TEXTURE_WRAP_T)
		object->wrap_t = value;
	else
		return 1;
	OsxGuestApplyRectangleSampler(context, object);
	return 1;
}

static uint32 OsxGuestTextureQueryValue(GLContext *context, uint32 target, uint32 pname)
{
	OsxGuestState *state;
	GLTextureObject *object;
	state = OsxGuestStateOf(context);
	if (target == GUESTPROXYTEXTURE2D || target == GUESTPROXYTEXTURERECTANGLE) {
		if (pname == GL_TEXTURE_WIDTH)
			return (uint32)state->guestproxywidth;
		if (pname == GL_TEXTURE_HEIGHT)
			return (uint32)state->guestproxyheight;
		if (pname == GL_TEXTURE_INTERNAL_FORMAT)
			return state->guestproxyinternalformat;
		return 0;
	}
	object = OsxGuestBoundObject(context, target);
	if (!object)
		return 0;
	switch (pname) {
	case GL_TEXTURE_WIDTH: return (uint32)object->width;
	case GL_TEXTURE_HEIGHT: return (uint32)object->height;
	case GL_TEXTURE_INTERNAL_FORMAT: return GL_RGBA;
	case GL_TEXTURE_MIN_FILTER: return object->min_filter;
	case GL_TEXTURE_MAG_FILTER: return object->mag_filter;
	case GL_TEXTURE_WRAP_S: return object->wrap_s;
	case GL_TEXTURE_WRAP_T: return object->wrap_t;
	}
	return 0;
}

static int OsxGuestStateCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	OsxGuestState *state;
	GLTextureObject *object;
	float single;
	uint32 bits;
	int values[2];
	int count;
	int index;
	int unit;
	state = OsxGuestStateOf(context);
	unit = context->active_texture;
	switch (sub) {
	case GL_SUB_GET_INTEGERV:
		count = OsxGuestStateQuery(context, cpu->gpr[4], values);
		for (index = 0; index < count; index++)
			OsxGuestWrite32(cpu->gpr[5] + (uint32)index * 4, (uint32)values[index]);
		return count;
	case GL_SUB_GET_FLOATV:
		if (cpu->gpr[4] != GUESTSTATEMAXRECTANGLETEXTURESIZE && cpu->gpr[4] != GUESTSTATEMAXVIEWPORTDIMENSIONS && cpu->gpr[4] != GUESTSTATEMAXTEXTURECOORDINATES && cpu->gpr[4] != GUESTSTATEMAXTEXTUREIMAGEUNITS)
			return 0;
		count = OsxGuestStateQuery(context, cpu->gpr[4], values);
		for (index = 0; index < count; index++) {
			single = (float)values[index];
			memcpy(&bits, &single, sizeof(bits));
			OsxGuestWrite32(cpu->gpr[5] + (uint32)index * 4, bits);
		}
		return 1;
	case GL_SUB_IS_ENABLED:
		if (cpu->gpr[4] == GUESTTEXTURERECTANGLE) {
			cpu->gpr[3] = (state->guestrectangleenabledunits >> unit) & 1;
			return 1;
		}
		if (cpu->gpr[4] == GL_TEXTURE_2D && state->rectangleshadowactive[unit]) {
			cpu->gpr[3] = state->guesttexture2denabled[unit];
			return 1;
		}
		return 0;
	case GL_SUB_ENABLE:
	case GL_SUB_DISABLE:
		if (cpu->gpr[4] == GUESTTEXTURERECTANGLE) {
			OsxGuestEnableRectangleTexture(context, sub == GL_SUB_ENABLE);
			OsxGuestSyncRectangleShadow(context, unit);
			return 1;
		}
		if (cpu->gpr[4] != GL_TEXTURE_2D)
			return 0;
		OsxGuestEngineDispatch(cpu, sub, 4);
		if (state->rectangleshadowactive[unit])
			state->guesttexture2denabled[unit] = sub == GL_SUB_ENABLE;
		OsxGuestSyncRectangleShadow(context, unit);
		return 1;
	case GL_SUB_BIND_TEXTURE:
		if (cpu->gpr[4] == GUESTTEXTURERECTANGLE) {
			state->boundrectangletexture[unit] = cpu->gpr[5];
			if (cpu->gpr[5]) {
				object = &context->texture_objects[cpu->gpr[5]];
				if (object->name != cpu->gpr[5]) {
					object->name = cpu->gpr[5];
					object->min_filter = GL_LINEAR;
					object->mag_filter = GL_LINEAR;
					object->wrap_s = GL_CLAMP_TO_EDGE;
					object->wrap_t = GL_CLAMP_TO_EDGE;
				}
			}
			OsxGuestBindRectangleTexture(context);
			OsxGuestSyncRectangleShadow(context, unit);
			return 1;
		}
		if (cpu->gpr[4] != GL_TEXTURE_2D)
			return 0;
		OsxGuestEngineDispatch(cpu, sub, 4);
		OsxGuestSyncRectangleShadow(context, unit);
		return 1;
	case GL_SUB_PIXEL_STOREI:
		if (cpu->gpr[4] != GUESTUNPACKROWBYTES)
			return 0;
		state->guestunpackrowbytes = (int32_t)cpu->gpr[5];
		return 1;
	case GL_SUB_TEX_IMAGE2D:
	case GL_SUB_TEX_SUB_IMAGE2D:
		return OsxGuestImageCall(context, cpu, sub);
	case GL_SUB_TEX_PARAMETERF:
	case GL_SUB_TEX_PARAMETERI:
	case GL_SUB_TEX_PARAMETERFV:
	case GL_SUB_TEX_PARAMETERIV:
		if (cpu->gpr[5] == GUESTTEXTURESTORAGEHINT)
			return 1;
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE)
			return 0;
		return OsxGuestRectangleParameterCall(context, cpu, sub);
	case GL_SUB_GET_TEX_PARAMETERIV:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE)
			return 0;
		OsxGuestWrite32(cpu->gpr[6], OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[5]));
		return 1;
	case GL_SUB_GET_TEX_PARAMETERFV:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE)
			return 0;
		single = (float)OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[5]);
		memcpy(&bits, &single, sizeof(bits));
		OsxGuestWrite32(cpu->gpr[6], bits);
		return 1;
	case GL_SUB_GET_TEX_LEVEL_PARAMETERIV:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE && cpu->gpr[4] != GUESTPROXYTEXTURE2D && cpu->gpr[4] != GUESTPROXYTEXTURERECTANGLE)
			return 0;
		OsxGuestWrite32(cpu->gpr[7], OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[6]));
		return 1;
	case GL_SUB_GET_TEX_LEVEL_PARAMETERFV:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE && cpu->gpr[4] != GUESTPROXYTEXTURE2D && cpu->gpr[4] != GUESTPROXYTEXTURERECTANGLE)
			return 0;
		single = (float)OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[6]);
		memcpy(&bits, &single, sizeof(bits));
		OsxGuestWrite32(cpu->gpr[7], bits);
		return 1;
	case GL_SUB_COPY_TEX_IMAGE2D:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE)
			return 0;
		GfxAccelHostBeginFrame(context);
		OsxGuestCopyFramebufferToRectangleTexture(context, (int)cpu->gpr[5], cpu->gpr[6], (int)cpu->gpr[7], (int)cpu->gpr[8], (int)cpu->gpr[9], (int)cpu->gpr[10], 0, 0, 1);
		return 1;
	case GL_SUB_COPY_TEX_SUB_IMAGE2D:
		if (cpu->gpr[4] != GUESTTEXTURERECTANGLE)
			return 0;
		GfxAccelHostBeginFrame(context);
		OsxGuestCopyFramebufferToRectangleTexture(context, (int)cpu->gpr[5], 0, (int)cpu->gpr[8], (int)cpu->gpr[9], (int)cpu->gpr[10], (int)OsxGuestStackWord(cpu, 56), (int)cpu->gpr[6], (int)cpu->gpr[7], 0);
		return 1;
	}
	return 0;
}

void OsxGuestEngineCall(void *engine, OsxCpu *cpu, uint32 sub)
{
	OsxGuestEngineBind(engine, cpu);
	if (OsxGuestEngineFastCall(cpu, sub))
		return;
	OsxGuestStateOf(gl_current_context)->guestreadpage = 1;
	if (OsxGuestStateCall(gl_current_context, cpu, sub))
		return;
	if (sub == GL_SUB_READ_PIXELS && OsxGuestReadPixels(gl_current_context, cpu))
		return;
	OsxGuestEngineDispatch(cpu, sub, 4);
}

void OsxGuestEngineFlush(void *engine, OsxCpu *cpu)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostTargetFlush();
}

void OsxGuestEngineReadPixels(void *engine, OsxCpu *cpu, uint32 width, uint32 height, void *pixels)
{
	OsxGuestEngineFlush(engine, cpu);
	GfxAccelHostTargetReadPixels(width, height, pixels);
}

void OsxGuestEngineHostString(void *engine, OsxCpu *cpu, uint32 name, char *text, uint32 size)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostHostString(name, text, size);
}

void OsxGuestEngineAfterPresent(void)
{
	if (!gl_current_context || !GfxAccelHostTargetFrameActive())
		return;
	GfxAccelHostTargetResume(gl_current_context, OsxGuestTargetOf(gl_current_context));
	OsxGuestApplyRectangleTextures(gl_current_context);
}

void OsxGuestEngineBeforePresent(void)
{
	if (gl_current_context)
		OsxGuestDisableRectangleTextures(gl_current_context);
}

unsigned OsxGuestEngineTextureCreate(void *engine, OsxCpu *cpu, uint32 width, uint32 height, int floatpixels)
{
	unsigned name;
	OsxGuestEngineBind(engine, cpu);
	name = GfxAccelHostTextureCreate(width, height, floatpixels);
	GfxAccelHostInvalidateState(gl_current_context);
	return name;
}

void OsxGuestEngineTextureDelete(void *engine, OsxCpu *cpu, unsigned texture)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostTextureDelete(texture);
}

void OsxGuestEngineAttachTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height)
{
	GLContext *context;
	OsxGuestEngineBind(engine, cpu);
	context = (GLContext *)engine;
	if (GfxAccelHostTargetFrameActive())
		GfxAccelHostEndFrame(context);
	GfxAccelHostTargetAttachTexture(OsxGuestTargetOf(context), texture, width, height);
	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = (int32_t)width;
	context->viewport[3] = (int32_t)height;
	context->scissor_box[0] = 0;
	context->scissor_box[1] = 0;
	context->scissor_box[2] = (int32_t)width;
	context->scissor_box[3] = (int32_t)height;
}

void OsxGuestEngineCopyTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height, uint32 internalformat, uint32 texturetarget)
{
	GLContext *context;
	GLTextureObject *object;
	uint32_t name;
	std::vector<uint8_t> blank;
	OsxGuestEngineBind(engine, cpu);
	context = (GLContext *)engine;
	name = context->tex_units[context->active_texture].bound_texture_2d;
	if (texturetarget == GUESTTEXTURERECTANGLE)
		name = OsxGuestStateOf(context)->boundrectangletexture[context->active_texture];
	if (!name || !GfxAccelHostFramebufferAvailable())
		return;
	object = &context->texture_objects[name];
	object->name = name;
	if (texturetarget != GUESTTEXTURERECTANGLE && (!object->metal_texture || object->width != (int)width || object->height != (int)height)) {
		blank.resize((size_t)width * height * 4);
		object->width = (int)width;
		object->height = (int)height;
		GfxAccelHostUploadTexture(context, object, 0, (int)width, (int)height, vec_data(blank), (int)blank.size());
	}
	GfxAccelHostReadFramebufferAttach(OsxGuestTargetOf(context), texture);
	if (texturetarget == GUESTTEXTURERECTANGLE)
		OsxGuestCopyFramebufferToRectangleTexture(context, 0, internalformat, 0, 0, (int)width, (int)height, 0, 0, 1);
	else
		GfxAccelHostTextureCopyFromRead(object, internalformat, (int)width, (int)height);
	GfxAccelHostReadFramebufferRelease();
	GfxAccelHostInvalidateState(context);
}

void OsxGuestEngineBlendSeparate(void *engine, OsxCpu *cpu, int separate)
{
	GLContext *context;
	OsxGuestEngineBind(engine, cpu);
	context = (GLContext *)engine;
	OsxGuestStateOf(context)->guestblendseparate = separate != 0;
	if (separate) {
		context->blend_src = cpu->gpr[4];
		context->blend_dst = cpu->gpr[5];
		OsxGuestStateOf(context)->guestblendsrcalpha = cpu->gpr[6];
		OsxGuestStateOf(context)->guestblenddstalpha = cpu->gpr[7];
	}
	GfxAccelHostInvalidateState(context);
}

void OsxGuestEngineProgramEnable(void *engine, OsxCpu *cpu, uint32 capability, int enable)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostProgramEnable(OsxGuestTargetOf((GLContext *)engine), capability, enable);
}

static double OsxGuestArgumentFloat(OsxCpu *cpu, int index)
{
	double value;
	memcpy(&value, &cpu->fpr[index], sizeof(value));
	return value;
}

static double OsxGuestReadDouble(uint32 address)
{
	uint64 bits;
	double value;
	bits = ((uint64)OsxGuestRead32(address) << 32) | OsxGuestRead32(address + 4);
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void OsxGuestWriteDouble(uint32 address, double value)
{
	uint64 bits;
	memcpy(&bits, &value, sizeof(bits));
	OsxGuestWrite32(address, (uint32)(bits >> 32));
	OsxGuestWrite32(address + 4, (uint32)bits);
}

static void OsxGuestProgramDeclarations(std::vector<uint8_t> &text)
{
	static const char alias[] = "ALIAS";
	static const char *const bindings[] = { "result.", "OUTPUT", "vertex.", "ATTRIB", "program.", "PARAM", "state.", "PARAM" };
	std::vector<uint8_t> output;
	const char *keyword;
	size_t position;
	size_t binding;
	size_t index;
	position = 0;
	while (position < text.size()) {
		keyword = NULL;
		if (text.size() - position >= sizeof(alias) && memcmp(&text[position], alias, sizeof(alias) - 1) == 0 && text[position + sizeof(alias) - 1] <= ' ' &&
			(position == 0 || text[position - 1] <= ' ' || text[position - 1] == ';')) {
			binding = position + sizeof(alias);
			while (binding < text.size() && text[binding] != '=' && text[binding] != ';')
				binding++;
			if (binding < text.size() && text[binding] == '=')
				binding++;
			while (binding < text.size() && (text[binding] == ' ' || text[binding] == '\t'))
				binding++;
			for (index = 0; index < sizeof(bindings) / sizeof(bindings[0]); index += 2)
				if (text.size() - binding >= strlen(bindings[index]) && memcmp(&text[binding], bindings[index], strlen(bindings[index])) == 0)
					keyword = bindings[index + 1];
		}
		if (keyword) {
			output.insert(output.end(), keyword, keyword + strlen(keyword));
			position += sizeof(alias) - 1;
		} else
			output.push_back(text[position++]);
	}
	text.swap(output);
}

int OsxGuestEngineProgramCall(void *engine, OsxCpu *cpu, uint32 call)
{
	void *target;
	std::vector<uint8_t> buffer;
	uint32 *arguments;
	float floats[4];
	double doubles[4];
	int length;
	uint32 count;
	uint32 index;
	uint32 bits;
	int program;
	program = -1;
	if (call >= 472 && call <= 475)
		program = (int)call - 472;
	else if (call >= 519 && call <= 533)
		program = (int)call - 515;
	if (program < 0)
		return 0;
	target = OsxGuestTargetOf((GLContext *)engine);
	if (!GfxAccelHostProgramEntryAvailable(target, program))
		return 0;
	OsxGuestEngineBind(engine, cpu);
	OsxGuestStateOf(gl_current_context)->guestreadpage = 1;
	arguments = cpu->gpr + 4;
	switch (call) {
	case 472:
		GfxAccelHostProgramBind(target, arguments[0], arguments[1]);
		break;
	case 473:
		count = arguments[0];
		buffer.resize((size_t)count * 4 + 4);
		for (index = 0; index < count; index++)
			((uint32 *)vec_data(buffer))[index] = OsxGuestRead32(arguments[1] + index * 4);
		GfxAccelHostProgramsDelete(target, count, (const uint32_t *)vec_data(buffer));
		break;
	case 474:
		count = arguments[0];
		buffer.resize((size_t)count * 4 + 4);
		GfxAccelHostProgramsGenerate(target, count, (uint32_t *)vec_data(buffer));
		for (index = 0; index < count; index++)
			OsxGuestWrite32(arguments[1] + index * 4, ((uint32 *)vec_data(buffer))[index]);
		break;
	case 475:
		cpu->gpr[3] = GfxAccelHostProgramIs(target, arguments[0]);
		break;
	case 519: case 523:
		for (index = 0; index < 4; index++)
			doubles[index] = OsxGuestArgumentFloat(cpu, 1 + (int)index);
		GfxAccelHostProgramParameterDoubles(target, call == 523, arguments[0], arguments[1], doubles);
		break;
	case 521: case 525:
		for (index = 0; index < 4; index++)
			floats[index] = (float)OsxGuestArgumentFloat(cpu, 1 + (int)index);
		GfxAccelHostProgramParameterFloats(target, call == 525, arguments[0], arguments[1], floats);
		break;
	case 520: case 524:
		for (index = 0; index < 4; index++)
			doubles[index] = OsxGuestReadDouble(arguments[2] + index * 8);
		GfxAccelHostProgramParameterDoubles(target, call == 524, arguments[0], arguments[1], doubles);
		break;
	case 522: case 526:
		for (index = 0; index < 4; index++) {
			bits = OsxGuestRead32(arguments[2] + index * 4);
			memcpy(&floats[index], &bits, sizeof(bits));
		}
		GfxAccelHostProgramParameterFloats(target, call == 526, arguments[0], arguments[1], floats);
		break;
	case 527: case 529:
		GfxAccelHostProgramGetDoubles(target, call == 529, arguments[0], arguments[1], doubles);
		for (index = 0; index < 4; index++)
			OsxGuestWriteDouble(arguments[2] + index * 8, doubles[index]);
		break;
	case 528: case 530:
		GfxAccelHostProgramGetFloats(target, call == 530, arguments[0], arguments[1], floats);
		for (index = 0; index < 4; index++) {
			memcpy(&bits, &floats[index], sizeof(bits));
			OsxGuestWrite32(arguments[2] + index * 4, bits);
		}
		break;
	case 531:
		buffer.resize((size_t)arguments[2]);
		OsxGuestCopyIn(vec_data(buffer), arguments[3], arguments[2]);
		OsxGuestProgramDeclarations(buffer);
		OsxGuestStateOf((GLContext *)engine)->guestprogramerrorposition = GfxAccelHostProgramString(target, arguments[0], arguments[1], (uint32_t)buffer.size(), vec_data(buffer));
		break;
	case 532:
		length = GfxAccelHostProgramQuery(target, arguments[0], GUESTPROGRAMLENGTH);
		buffer.resize((size_t)length + 1);
		GfxAccelHostProgramGetString(target, arguments[0], arguments[1], vec_data(buffer));
		OsxGuestCopyOut(arguments[2], vec_data(buffer), (size_t)length);
		break;
	case 533:
		length = GfxAccelHostProgramQuery(target, arguments[0], arguments[1]);
		OsxGuestWrite32(arguments[2], (uint32)length);
		break;
	default:
		return 0;
	}
	return 1;
}

static uint8 *OsxGuestScratch(uint32 address)
{
	if (address < OSXGLSCRATCHBASE || address - OSXGLSCRATCHBASE >= sizeof(OsxGuestStateOf(gl_current_context)->guestscratch))
		return NULL;
	return OsxGuestStateOf(gl_current_context)->guestscratch + (address - OSXGLSCRATCHBASE);
}

static uint8 *OsxGuestByte(uint32 address, int kind)
{
	OsxCpu *cpu;
	uint32 physical;
	uint8 *scratch;
	scratch = OsxGuestScratch(address);
	if (scratch)
		return scratch;
	if (OsxGuestStateOf(gl_current_context)->guestfaultraised)
		return NULL;
	cpu = (OsxCpu *)OsxGuestStateOf(gl_current_context)->guestcpu;
	if (!OsxGuestCpuTranslate(cpu, address, kind, 1, &physical) || physical >= cpu->ramsize) {
		OsxGuestStateOf(gl_current_context)->guestfaultraised = true;
		return NULL;
	}
	return cpu->ram + physical;
}

static uint8 *OsxGuestReadPage(uint32 address)
{
	GLContext *context;
	OsxCpu *cpu;
	uint32 physical;
	context = gl_current_context;
	if ((address & ~4095u) == OsxGuestStateOf(context)->guestreadpage)
		return OsxGuestStateOf(context)->guestreadhost;
	if (OsxGuestStateOf(context)->guestfaultraised)
		return NULL;
	cpu = (OsxCpu *)OsxGuestStateOf(context)->guestcpu;
	if (!OsxGuestCpuTranslate(cpu, address, OSXTLBREAD, 1, &physical) || physical >= cpu->ramsize) {
		OsxGuestStateOf(context)->guestfaultraised = true;
		return NULL;
	}
	if ((physical | 4095u) >= cpu->ramsize)
		return NULL;
	OsxGuestStateOf(context)->guestreadpage = address & ~4095u;
	OsxGuestStateOf(context)->guestreadhost = cpu->ram + (physical & ~4095u);
	return OsxGuestStateOf(context)->guestreadhost;
}

uint8 OsxGuestRead8(uint32 address)
{
	uint8 *byte;
	if (!OsxGuestScratch(address)) {
		byte = OsxGuestReadPage(address);
		if (byte)
			return byte[address & 4095];
		return 0;
	}
	byte = OsxGuestByte(address, OSXTLBREAD);
	if (!byte)
		return 0;
	return *byte;
}

uint16 OsxGuestRead16(uint32 address)
{
	return (uint16)((OsxGuestRead8(address) << 8) | OsxGuestRead8(address + 1));
}

uint32 OsxGuestRead32(uint32 address)
{
	uint32 value;
	uint8 *page;
	page = OsxGuestScratch(address);
	if (page) {
		memcpy(&value, page, sizeof(value));
		return bswap_32(value);
	}
	if ((address & 4095) <= 4092) {
		page = OsxGuestReadPage(address);
		if (page) {
			memcpy(&value, page + (address & 4095), sizeof(value));
			return bswap_32(value);
		}
		return 0;
	}
	value = 0;
	OsxGuestCpuReadVirtual((OsxCpu *)OsxGuestStateOf(gl_current_context)->guestcpu, address, &value);
	return value;
}

void OsxGuestWrite8(uint32 address, uint32 value)
{
	uint8 *byte;
	byte = OsxGuestByte(address, OSXTLBWRITE);
	if (byte)
		*byte = (uint8)value;
}

void OsxGuestWrite16(uint32 address, uint32 value)
{
	OsxGuestWrite8(address, value >> 8);
	OsxGuestWrite8(address + 1, value);
}

void OsxGuestWrite32(uint32 address, uint32 value)
{
	value = bswap_32(value);
	OsxGuestCopyOut(address, &value, sizeof(value));
}

uint8 *OsxGuestHostAddress(uint32 address)
{
	return OsxGuestScratch(address);
}

uint32 OsxGuestAllocate(uint32 size)
{
	GLContext *context;
	uint32 address;
	context = gl_current_context;
	if (OsxGuestStateOf(context)->guestscratchused < OSXGLSCRATCHWORD)
		OsxGuestStateOf(context)->guestscratchused = OSXGLSCRATCHWORD;
	if (size > sizeof(OsxGuestStateOf(context)->guestscratch) - OsxGuestStateOf(context)->guestscratchused)
		return 0;
	address = OSXGLSCRATCHBASE + OsxGuestStateOf(context)->guestscratchused;
	OsxGuestStateOf(context)->guestscratchused += size;
	return address;
}

void *OsxGuestSet(uint32 address, int value, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++)
		OsxGuestWrite8(address + (uint32)index, value);
	return OsxGuestHostAddress(address);
}

void *OsxGuestCopyIn(void *destination, uint32 source, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++)
		((uint8 *)destination)[index] = OsxGuestRead8(source + (uint32)index);
	return destination;
}

void *OsxGuestCopyOut(uint32 destination, const void *source, size_t length)
{
	uint8 *page;
	size_t index;
	size_t count;
	for (index = 0; index < length; index += count) {
		count = 4096 - ((destination + (uint32)index) & 4095);
		if (count > length - index)
			count = length - index;
		page = OsxGuestByte(destination + (uint32)index, OSXTLBWRITE);
		if (page)
			memcpy(page, (const uint8 *)source + index, count);
	}
	return OsxGuestHostAddress(destination);
}

const uint8 *OsxGuestBytes(uint32 address, int32_t length)
{
	if (length <= 0)
		return NULL;
	OsxGuestStateOf(gl_current_context)->guestbytes.resize((size_t)length);
	OsxGuestCopyIn(vec_data(OsxGuestStateOf(gl_current_context)->guestbytes), address, (size_t)length);
	return vec_data(OsxGuestStateOf(gl_current_context)->guestbytes);
}

}
