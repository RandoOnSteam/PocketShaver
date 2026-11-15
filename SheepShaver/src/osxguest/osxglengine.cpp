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
#define GUESTDEPTHMAXIMUMINT 4294967295.0
#define GUESTDEPTHMAXIMUMSHORT 65535.0
#define GUESTDEPTHMAXIMUMBYTE 255.0
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
#define GUESTTEXTURECUBEMAP GFXACCELHOSTCUBEMAP
#define GUESTTEXTURE3D GFXACCELHOSTTEXTURE3D
#define GUESTPROXYTEXTURECUBEMAP 0x851bu
#define GUESTSTATECUBEMAPBINDING 0x8514u
#define GUESTSTATE3DBINDING 0x806au
#define GUESTSTATEMAXCUBEMAPTEXTURESIZE 0x851cu
#define GUESTSTATEMAX3DTEXTURESIZE 0x8073u
#define GUESTTEXTUREBORDERCOLOR 0x1004u
#define GUESTTEXTUREKINDS 3
#define GUESTKINDRECTANGLE 0
#define GUESTKINDCUBEMAP 1
#define GUESTKIND3D 2
#define GUESTTEXTUREUNITS 8
#define GUESTPROXYLIMIT 1024
#define GUESTPROGRAMERRORPOSITION 0x864bu
#define GUESTHOSTLIMITMAXIMUM 2048
#define GUESTFIXEDTEXTURECOORDINATES 8
#define GUESTFIXEDTEXTUREIMAGEUNITS 16
#define GUESTATTRIBUTES 16
#define GUESTREADPAGES 4
#define GUESTATTRIBUTEPOINTER 512u
#define GUESTATTRIBUTEENABLE 513u
#define GUESTATTRIBUTEDISABLE 514u
#define GUESTDRAWARRAYS 66u
#define GUESTDRAWELEMENTS 68u
#define GUESTDRAWRANGEELEMENTS 406u
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
	uint32_t guestreadpages[GUESTREADPAGES];
	uint8_t *guestreadhosts[GUESTREADPAGES];
	uint32_t guestreadnext;
	bool guestblendseparate;
	uint32_t guestblendsrcalpha;
	uint32_t guestblenddstalpha;
	std::vector<uint8_t> guestbytes;
	OsxGuestSharedTextures *sharedtextures;
	uint32_t boundtextures[GUESTTEXTUREKINDS][GUESTTEXTUREUNITS];
	uint32_t guestenabledunits[GUESTTEXTUREKINDS];
	bool guesttexture2denabled[GUESTTEXTUREUNITS];
	bool shadowactive[GUESTTEXTUREUNITS];
	GLTextureObject guestshadowtexture;
	int guestmaxviewportdimensions[2];
	int guestmaxrectangletexturesize;
	int guestmaxtexturecoordinates;
	int guestmaxtextureimageunits;
	int guestprogramerrorposition;
	GLVertexArrayPointer guestattributes[GUESTATTRIBUTES];
	uint32_t guestattributenormalized;
	std::vector<float> guestattributevalues;
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

static void OsxGuestForgetReadPages(OsxGuestState *state)
{
	int index;
	for (index = 0; index < GUESTREADPAGES; index++)
		state->guestreadpages[index] = 1;
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

static void OsxGuestDisableKindTextures(GLContext *context);
static void OsxGuestApplyKindTextures(GLContext *context);

static void OsxGuestCaptureHostLimits(GLContext *context)
{
	OsxGuestStateOf(context)->guestmaxviewportdimensions[0] = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxviewportdimensions[1] = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxrectangletexturesize = GUESTHOSTLIMITMAXIMUM;
	OsxGuestStateOf(context)->guestmaxtexturecoordinates = GUESTFIXEDTEXTURECOORDINATES;
	OsxGuestStateOf(context)->guestmaxtextureimageunits = GUESTFIXEDTEXTUREIMAGEUNITS;
	OsxGuestStateOf(context)->guestprogramerrorposition = -1;
	GfxAccelHostShadowTextureCreate(context, &OsxGuestStateOf(context)->guestshadowtexture);
}

static void OsxGuestEngineLeave(GLContext *context)
{
	OsxGuestDisableKindTextures(context);
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
	OsxGuestApplyKindTextures(next);
}

void OsxGuestEngineDestroy(void *engine)
{
	GLContext *context;
	context = (GLContext *)engine;
	OsxGuestEngineBind(context, (OsxCpu *)OsxGuestStateOf(context)->guestcpu);
	GfxAccelHostShadowTextureDestroy(&OsxGuestStateOf(context)->guestshadowtexture);
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
	last = GLDispatch(cpu->gpr[first], cpu->gpr[first + 1], cpu->gpr[first + 2], cpu->gpr[first + 3],
		cpu->gpr[first + 4], cpu->gpr[first + 5], cpu->gpr[first + 6], last, floats, count);
	if (!OsxGuestStateOf(gl_current_context)->guestfaultraised)
		cpu->gpr[3] = last;
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

static uint32 OsxGuestKindTarget(int kind)
{
	static const uint32 targets[GUESTTEXTUREKINDS] = { GUESTTEXTURERECTANGLE, GUESTTEXTURECUBEMAP, GUESTTEXTURE3D };
	return targets[kind];
}

static int OsxGuestTextureKind(uint32 target)
{
	if (target == GUESTTEXTURERECTANGLE)
		return GUESTKINDRECTANGLE;
	if (target == GUESTTEXTURECUBEMAP || (target >= GFXACCELHOSTCUBEMAPFIRSTFACE && target <= GFXACCELHOSTCUBEMAPLASTFACE))
		return GUESTKINDCUBEMAP;
	if (target == GUESTTEXTURE3D)
		return GUESTKIND3D;
	return -1;
}

static int OsxGuestKindEnabled(GLContext *context, int kind, int unit)
{
	return (OsxGuestStateOf(context)->guestenabledunits[kind] >> unit) & 1;
}

static void OsxGuestApplyKindTexture(GLContext *context, int kind, int unit, int enabled)
{
	GfxAccelHostTextureApply(context, unit, OsxGuestKindTarget(kind), OsxGuestStateOf(context)->boundtextures[kind][unit], enabled);
}

static void OsxGuestApplyKindTextures(GLContext *context)
{
	int kind;
	int unit;
	for (kind = 0; kind < GUESTTEXTUREKINDS; kind++)
		for (unit = 0; unit < GfxAccelHostTextureUnitCount(context); unit++)
			if (OsxGuestStateOf(context)->boundtextures[kind][unit] || OsxGuestKindEnabled(context, kind, unit))
				OsxGuestApplyKindTexture(context, kind, unit, OsxGuestKindEnabled(context, kind, unit));
}

static void OsxGuestDisableKindTextures(GLContext *context)
{
	int kind;
	int unit;
	for (kind = 0; kind < GUESTTEXTUREKINDS; kind++)
		for (unit = 0; unit < GfxAccelHostTextureUnitCount(context); unit++)
			if (OsxGuestKindEnabled(context, kind, unit))
				OsxGuestApplyKindTexture(context, kind, unit, 0);
}

static void OsxGuestKindDefaults(GLContext *context, int kind, uint32 name)
{
	GLTextureObject *object;
	object = &context->texture_objects[name];
	if (object->name == name)
		return;
	object->name = name;
	object->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	object->mag_filter = GL_LINEAR;
	object->wrap_s = GL_REPEAT;
	object->wrap_t = GL_REPEAT;
	if (kind != GUESTKINDRECTANGLE)
		return;
	object->min_filter = GL_LINEAR;
	object->wrap_s = GL_CLAMP_TO_EDGE;
	object->wrap_t = GL_CLAMP_TO_EDGE;
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

static uint32 OsxGuestDepthValue(uint32 type, float depth, uint32 *elementbytes)
{
	uint32 bits;
	*elementbytes = 4;
	switch (type) {
	case GL_UNSIGNED_INT:
		return (uint32)((double)depth * GUESTDEPTHMAXIMUMINT);
	case GL_FLOAT:
		memcpy(&bits, &depth, sizeof(bits));
		return bits;
	case GL_UNSIGNED_SHORT:
		*elementbytes = 2;
		return (uint32)((double)depth * GUESTDEPTHMAXIMUMSHORT);
	case GL_UNSIGNED_BYTE:
		*elementbytes = 1;
		return (uint32)((double)depth * GUESTDEPTHMAXIMUMBYTE);
	}
	*elementbytes = 0;
	return 0;
}

static int OsxGuestReadDepth(GLContext *context, OsxCpu *cpu)
{
	float *depths;
	uint32 elementbytes;
	uint32 packalignment;
	uint32 guestrowbytes;
	uint32 address;
	uint32 value;
	int width;
	int height;
	int row;
	int column;
	width = (int)cpu->gpr[6];
	height = (int)cpu->gpr[7];
	OsxGuestDepthValue(cpu->gpr[9], 0.0f, &elementbytes);
	if (!elementbytes || width <= 0 || height <= 0)
		return 0;
	depths = GfxAccelHostReadDepthRect(context, (int)cpu->gpr[4], (int)cpu->gpr[5], width, height);
	if (!depths)
		return 1;
	packalignment = (uint32)context->pixel_store.pack_alignment;
	if (!packalignment)
		packalignment = 4;
	guestrowbytes = (uint32)width;
	if (context->pixel_store.pack_row_length > 0)
		guestrowbytes = (uint32)context->pixel_store.pack_row_length;
	guestrowbytes = (guestrowbytes * elementbytes + packalignment - 1) & ~(packalignment - 1);
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			if (depths[row * width + column] < 0.0f)
				continue;
			value = OsxGuestDepthValue(cpu->gpr[9], depths[row * width + column], &elementbytes);
			address = cpu->gpr[10] + (uint32)row * guestrowbytes + (uint32)column * elementbytes;
			if (elementbytes == 4)
				OsxGuestWrite32(address, value);
			else if (elementbytes == 2)
				OsxGuestWrite16(address, value);
			else
				OsxGuestWrite8(address, value);
		}
	}
	free(depths);
	return 1;
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

static GLTextureObject *OsxGuestBoundObject(GLContext *context, uint32 target)
{
	uint32 name;
	int kind;
	name = context->tex_units[context->active_texture].bound_texture_2d;
	kind = OsxGuestTextureKind(target);
	if (kind >= 0)
		name = OsxGuestStateOf(context)->boundtextures[kind][context->active_texture];
	if (!name)
		return NULL;
	return &context->texture_objects[name];
}

static void OsxGuestCopyFramebufferToKindTexture(GLContext *context, uint32 target, int level, uint32 internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage)
{
	GLTextureObject *object;
	object = OsxGuestBoundObject(context, target);
	if (!object)
		return;
	if (replacewholeimage && !level) {
		object->width = width;
		object->height = height;
	}
	GfxAccelHostTextureCopyFramebuffer(context, context->active_texture, target, object, level, internalformat, sourcex, sourcey, width, height, xoffset, yoffset, replacewholeimage);
}

static void OsxGuestSyncShadow(GLContext *context, int unit)
{
	OsxGuestState *state;
	GLTextureUnit *textureunit;
	GLTextureObjectMap::iterator guesttexture;
	int kind;
	int special;
	state = OsxGuestStateOf(context);
	textureunit = &context->tex_units[unit];
	guesttexture = context->texture_objects.find(textureunit->bound_texture_2d);
	special = 0;
	for (kind = 0; kind < GUESTTEXTUREKINDS; kind++)
		special |= OsxGuestKindEnabled(context, kind, unit);
	if (special) {
		if (!state->shadowactive[unit]) {
			state->guesttexture2denabled[unit] = textureunit->enabled_2d;
			state->shadowactive[unit] = true;
		}
		textureunit->enabled_2d = true;
		textureunit->bound_object_2d = &state->guestshadowtexture;
		if (state->guesttexture2denabled[unit] && guesttexture != context->texture_objects.end() && guesttexture->second.metal_texture)
			textureunit->bound_object_2d = &guesttexture->second;
		return;
	}
	if (!state->shadowactive[unit])
		return;
	state->shadowactive[unit] = false;
	textureunit->enabled_2d = state->guesttexture2denabled[unit];
	if (textureunit->bound_object_2d == &state->guestshadowtexture) {
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
	case GUESTSTATERECTANGLEBINDING: values[0] = (int)state->boundtextures[GUESTKINDRECTANGLE][context->active_texture]; return 1;
	case GUESTSTATECUBEMAPBINDING: values[0] = (int)state->boundtextures[GUESTKINDCUBEMAP][context->active_texture]; return 1;
	case GUESTSTATE3DBINDING: values[0] = (int)state->boundtextures[GUESTKIND3D][context->active_texture]; return 1;
	case GUESTSTATEMAXCUBEMAPTEXTURESIZE: values[0] = GUESTPROXYLIMIT; return 1;
	case GUESTSTATEMAX3DTEXTURESIZE: values[0] = GUESTPROXYLIMIT; return 1;
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
	int kind;
	state = OsxGuestStateOf(context);
	shared = state->sharedtextures;
	image = sub == GL_SUB_TEX_IMAGE2D;
	target = cpu->gpr[4];
	kind = OsxGuestTextureKind(target);
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
	if (image && (target == GUESTPROXYTEXTURE2D || target == GUESTPROXYTEXTURERECTANGLE || target == GUESTPROXYTEXTURECUBEMAP)) {
		limit = GUESTPROXYLIMIT;
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
	if (target != GL_TEXTURE_2D && (kind < 0 || kind == GUESTKIND3D))
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
	if (pixels && (intensity || kind >= 0))
		converted = ConvertPixelsToBGRA8(pixels, width, height, format, type, context->pixel_store, context, &bytecount);
	if (target == GL_TEXTURE_2D) {
		if (image)
			NativeGLTexImage2D(context, target, level, (int32_t)internalformat, width, height, (int32_t)cpu->gpr[9], format, type, pixels);
		else
			NativeGLTexSubImage2D(context, target, level, xoffset, yoffset, width, height, format, type, pixels);
	}
	if (converted && intensity)
		OsxGuestIntensityAlpha(converted, bytecount);
	if (kind >= 0) {
		if (!level && image) {
			object->width = width;
			object->height = height;
		}
		if (!pixels && image) {
			bytecount = width * height * 4;
			converted = (uint8_t *)calloc(1, (size_t)bytecount);
		}
		if (converted)
			GfxAccelHostTextureImage(context->active_texture, target, object, level, xoffset, yoffset, width, height, converted, image);
	} else if (converted && image)
		GfxAccelHostUploadTexture(context, object, level, width, height, converted, bytecount);
	else if (converted)
		GfxAccelHostUploadSubTexture(context, object, level, xoffset, yoffset, width, height, converted, width * 4);
	free(converted);
	context->pixel_store.unpack_row_length = savedrowlength;
	return 1;
}

static int OsxGuestCompressedImageCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	GLTextureObject *object;
	const uint8 *data;
	uint8_t *pixels;
	uint32 target;
	uint32 format;
	uint32 address;
	int image;
	int kind;
	int level;
	int xoffset;
	int yoffset;
	int width;
	int height;
	int size;
	int blockbytes;
	int bytecount;
	image = sub == GL_SUB_COMPRESSED_TEX_IMAGE2D_ARB;
	if (!image && sub != GL_SUB_COMPRESSED_TEX_SUB_IMAGE2D_ARB)
		return 0;
	target = cpu->gpr[4];
	kind = OsxGuestTextureKind(target);
	if (target != GL_TEXTURE_2D && kind != GUESTKINDCUBEMAP)
		return 0;
	level = (int)cpu->gpr[5];
	xoffset = 0;
	yoffset = 0;
	format = cpu->gpr[6];
	width = (int)cpu->gpr[7];
	height = (int)cpu->gpr[8];
	size = (int)cpu->gpr[10];
	address = OsxGuestStackWord(cpu, 56);
	if (!image) {
		xoffset = (int)cpu->gpr[6];
		yoffset = (int)cpu->gpr[7];
		width = (int)cpu->gpr[8];
		height = (int)cpu->gpr[9];
		format = cpu->gpr[10];
		size = (int)OsxGuestStackWord(cpu, 56);
		address = OsxGuestStackWord(cpu, 60);
	}
	blockbytes = GLDXTLARGEBLOCK;
	if (format <= GLDXT1ALPHA)
		blockbytes = GLDXTSMALLBLOCK;
	object = OsxGuestBoundObject(context, target);
	if (!object || format < GLDXT1 || format > GLDXT5 || width <= 0 || height <= 0 || size < ((width + 3) / 4) * ((height + 3) / 4) * blockbytes)
		return 1;
	data = OsxGuestBytes(address, size);
	if (OsxGuestStateOf(context)->guestfaultraised)
		return 1;
	pixels = dxt_decompress_image(data, width, height, format, &bytecount);
	if (!pixels)
		return 1;
	if (image && level)
		object->has_mipmaps = true;
	else if (image) {
		object->width = width;
		object->height = height;
	}
	if (image)
		object->internal_format_opaque = format == GLDXT1;
	if (kind == GUESTKINDCUBEMAP)
		GfxAccelHostTextureImage(context->active_texture, target, object, level, xoffset, yoffset, width, height, pixels, image);
	else if (image)
		GfxAccelHostUploadTexture(context, object, level, width, height, pixels, bytecount);
	else
		GfxAccelHostUploadSubTexture(context, object, level, xoffset, yoffset, width, height, pixels, width * 4);
	free(pixels);
	return 1;
}

static int OsxGuestVolumeCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	GLTextureObject *object;
	std::vector<uint8_t> volume;
	uint8_t *converted;
	uint32 format;
	uint32 type;
	uint32 pixels;
	uint32 slicebytes;
	int image;
	int level;
	int xoffset;
	int yoffset;
	int zoffset;
	int width;
	int height;
	int depth;
	int slice;
	int bytecount;
	image = sub == GL_SUB_TEX_IMAGE3D_EXT;
	if ((!image && sub != GL_SUB_TEX_SUB_IMAGE3D_EXT) || cpu->gpr[4] != GUESTTEXTURE3D)
		return 0;
	level = (int)cpu->gpr[5];
	xoffset = 0;
	yoffset = 0;
	zoffset = 0;
	width = (int)cpu->gpr[7];
	height = (int)cpu->gpr[8];
	depth = (int)cpu->gpr[9];
	format = OsxGuestStackWord(cpu, 56);
	type = OsxGuestStackWord(cpu, 60);
	pixels = OsxGuestStackWord(cpu, 64);
	if (!image) {
		xoffset = (int)cpu->gpr[6];
		yoffset = (int)cpu->gpr[7];
		zoffset = (int)cpu->gpr[8];
		width = (int)cpu->gpr[9];
		height = (int)cpu->gpr[10];
		depth = (int)OsxGuestStackWord(cpu, 56);
		format = OsxGuestStackWord(cpu, 60);
		type = OsxGuestStackWord(cpu, 64);
		pixels = OsxGuestStackWord(cpu, 68);
	}
	object = OsxGuestBoundObject(context, GUESTTEXTURE3D);
	if (!object || width <= 0 || height <= 0 || depth <= 0)
		return 1;
	if (image && !level) {
		object->width = width;
		object->height = height;
		object->depth = depth;
	}
	volume.resize((size_t)width * height * depth * 4);
	slicebytes = (uint32)(ComputeRowStride(width, GLPixelSourceBytesPerPixel(format, type), context->pixel_store.unpack_alignment, context->pixel_store.unpack_row_length) * height);
	for (slice = 0; pixels && slice < depth; slice++) {
		converted = ConvertPixelsToBGRA8(pixels + (uint32)slice * slicebytes, width, height, format, type, context->pixel_store, context, &bytecount);
		if (!converted)
			continue;
		memcpy(&volume[(size_t)slice * width * height * 4], converted, (size_t)width * height * 4);
		free(converted);
	}
	GfxAccelHostTextureVolume(OsxGuestTargetOf(context), context->active_texture, object, level, xoffset, yoffset, zoffset, width, height, depth, vec_data(volume), image);
	return 1;
}

static int OsxGuestParameterCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	GLTextureObject *object;
	double number;
	float value;
	uint32 bits;
	uint32 target;
	uint32 pname;
	target = cpu->gpr[4];
	pname = cpu->gpr[5];
	if (pname == GUESTTEXTURESTORAGEHINT || pname == GUESTTEXTUREBORDERCOLOR)
		return 1;
	if (OsxGuestTextureKind(target) < 0 && (target != GL_TEXTURE_2D || pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T))
		return 0;
	object = OsxGuestBoundObject(context, target);
	if (!object)
		return 1;
	value = (float)(int32)cpu->gpr[6];
	if (sub == GL_SUB_TEX_PARAMETERF) {
		memcpy(&number, &cpu->fpr[1], sizeof(number));
		value = (float)number;
	} else if (sub == GL_SUB_TEX_PARAMETERIV)
		value = (float)(int32)OsxGuestRead32(cpu->gpr[6]);
	else if (sub == GL_SUB_TEX_PARAMETERFV) {
		bits = OsxGuestRead32(cpu->gpr[6]);
		memcpy(&value, &bits, sizeof(value));
	}
	if (pname == GL_TEXTURE_MIN_FILTER)
		object->min_filter = (uint32)value;
	else if (pname == GL_TEXTURE_MAG_FILTER)
		object->mag_filter = (uint32)value;
	else if (pname == GL_TEXTURE_WRAP_S)
		object->wrap_s = (uint32)value;
	else if (pname == GL_TEXTURE_WRAP_T)
		object->wrap_t = (uint32)value;
	object->sampler_applied = false;
	GfxAccelHostTextureParameter(context, context->active_texture, target, object, pname, value);
	return 1;
}

static uint32 OsxGuestTextureQueryValue(GLContext *context, uint32 target, uint32 pname)
{
	OsxGuestState *state;
	GLTextureObject *object;
	state = OsxGuestStateOf(context);
	if (target == GUESTPROXYTEXTURE2D || target == GUESTPROXYTEXTURERECTANGLE || target == GUESTPROXYTEXTURECUBEMAP) {
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

static void OsxGuestForgetDeletedTextures(GLContext *context)
{
	OsxGuestState *state;
	int kind;
	int unit;
	state = OsxGuestStateOf(context);
	for (kind = 0; kind < GUESTTEXTUREKINDS; kind++) {
		for (unit = 0; unit < GUESTTEXTUREUNITS; unit++) {
			if (!state->boundtextures[kind][unit] || context->texture_objects.find(state->boundtextures[kind][unit]) != context->texture_objects.end())
				continue;
			state->boundtextures[kind][unit] = 0;
			if (unit < GfxAccelHostTextureUnitCount(context))
				OsxGuestApplyKindTexture(context, kind, unit, OsxGuestKindEnabled(context, kind, unit));
		}
	}
}

static int OsxGuestStateCall(GLContext *context, OsxCpu *cpu, uint32 sub)
{
	OsxGuestState *state;
	float single;
	uint32 bits;
	int values[2];
	int count;
	int index;
	int unit;
	int kind;
	state = OsxGuestStateOf(context);
	unit = context->active_texture;
	kind = OsxGuestTextureKind(cpu->gpr[4]);
	switch (sub) {
	case GL_SUB_GET_INTEGERV:
		count = OsxGuestStateQuery(context, cpu->gpr[4], values);
		for (index = 0; index < count; index++)
			OsxGuestWrite32(cpu->gpr[5] + (uint32)index * 4, (uint32)values[index]);
		return count;
	case GL_SUB_GET_FLOATV:
		if (cpu->gpr[4] != GUESTSTATEMAXRECTANGLETEXTURESIZE && cpu->gpr[4] != GUESTSTATEMAXVIEWPORTDIMENSIONS && cpu->gpr[4] != GUESTSTATEMAXTEXTURECOORDINATES && cpu->gpr[4] != GUESTSTATEMAXTEXTUREIMAGEUNITS &&
			cpu->gpr[4] != GUESTSTATEMAXCUBEMAPTEXTURESIZE && cpu->gpr[4] != GUESTSTATEMAX3DTEXTURESIZE)
			return 0;
		count = OsxGuestStateQuery(context, cpu->gpr[4], values);
		for (index = 0; index < count; index++) {
			single = (float)values[index];
			memcpy(&bits, &single, sizeof(bits));
			OsxGuestWrite32(cpu->gpr[5] + (uint32)index * 4, bits);
		}
		return 1;
	case GL_SUB_IS_ENABLED:
		if (kind >= 0) {
			cpu->gpr[3] = OsxGuestKindEnabled(context, kind, unit);
			return 1;
		}
		if (cpu->gpr[4] == GL_TEXTURE_2D && state->shadowactive[unit]) {
			cpu->gpr[3] = state->guesttexture2denabled[unit];
			return 1;
		}
		return 0;
	case GL_SUB_ENABLE:
	case GL_SUB_DISABLE:
		if (kind >= 0) {
			state->guestenabledunits[kind] &= ~(1u << unit);
			if (sub == GL_SUB_ENABLE)
				state->guestenabledunits[kind] |= 1u << unit;
			OsxGuestApplyKindTexture(context, kind, unit, sub == GL_SUB_ENABLE);
			OsxGuestSyncShadow(context, unit);
			return 1;
		}
		if (cpu->gpr[4] != GL_TEXTURE_2D)
			return 0;
		OsxGuestEngineDispatch(cpu, sub, 4);
		if (state->shadowactive[unit])
			state->guesttexture2denabled[unit] = sub == GL_SUB_ENABLE;
		OsxGuestSyncShadow(context, unit);
		return 1;
	case GL_SUB_BIND_TEXTURE:
		if (kind >= 0) {
			state->boundtextures[kind][unit] = cpu->gpr[5];
			if (cpu->gpr[5])
				OsxGuestKindDefaults(context, kind, cpu->gpr[5]);
			OsxGuestApplyKindTexture(context, kind, unit, OsxGuestKindEnabled(context, kind, unit));
			return 1;
		}
		if (cpu->gpr[4] != GL_TEXTURE_2D)
			return 0;
		OsxGuestEngineDispatch(cpu, sub, 4);
		OsxGuestSyncShadow(context, unit);
		return 1;
	case GL_SUB_DELETE_TEXTURES:
		OsxGuestEngineDispatch(cpu, sub, 4);
		OsxGuestForgetDeletedTextures(context);
		return 1;
	case GL_SUB_PIXEL_STOREI:
		if (cpu->gpr[4] != GUESTUNPACKROWBYTES)
			return 0;
		state->guestunpackrowbytes = (int32_t)cpu->gpr[5];
		return 1;
	case GL_SUB_TEX_IMAGE2D:
	case GL_SUB_TEX_SUB_IMAGE2D:
		return OsxGuestImageCall(context, cpu, sub);
	case GL_SUB_TEX_IMAGE3D_EXT:
	case GL_SUB_TEX_SUB_IMAGE3D_EXT:
		return OsxGuestVolumeCall(context, cpu, sub);
	case GL_SUB_COMPRESSED_TEX_IMAGE2D_ARB:
	case GL_SUB_COMPRESSED_TEX_SUB_IMAGE2D_ARB:
		return OsxGuestCompressedImageCall(context, cpu, sub);
	case GL_SUB_TEX_PARAMETERF:
	case GL_SUB_TEX_PARAMETERI:
	case GL_SUB_TEX_PARAMETERFV:
	case GL_SUB_TEX_PARAMETERIV:
		return OsxGuestParameterCall(context, cpu, sub);
	case GL_SUB_GET_TEX_PARAMETERIV:
		if (kind < 0)
			return 0;
		OsxGuestWrite32(cpu->gpr[6], OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[5]));
		return 1;
	case GL_SUB_GET_TEX_PARAMETERFV:
		if (kind < 0)
			return 0;
		single = (float)OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[5]);
		memcpy(&bits, &single, sizeof(bits));
		OsxGuestWrite32(cpu->gpr[6], bits);
		return 1;
	case GL_SUB_GET_TEX_LEVEL_PARAMETERIV:
		if (kind < 0 && cpu->gpr[4] != GUESTPROXYTEXTURE2D && cpu->gpr[4] != GUESTPROXYTEXTURERECTANGLE && cpu->gpr[4] != GUESTPROXYTEXTURECUBEMAP)
			return 0;
		OsxGuestWrite32(cpu->gpr[7], OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[6]));
		return 1;
	case GL_SUB_GET_TEX_LEVEL_PARAMETERFV:
		if (kind < 0 && cpu->gpr[4] != GUESTPROXYTEXTURE2D && cpu->gpr[4] != GUESTPROXYTEXTURERECTANGLE && cpu->gpr[4] != GUESTPROXYTEXTURECUBEMAP)
			return 0;
		single = (float)OsxGuestTextureQueryValue(context, cpu->gpr[4], cpu->gpr[6]);
		memcpy(&bits, &single, sizeof(bits));
		OsxGuestWrite32(cpu->gpr[7], bits);
		return 1;
	case GL_SUB_COPY_TEX_IMAGE2D:
		if (kind < 0 || kind == GUESTKIND3D)
			return 0;
		GfxAccelHostBeginFrame(context);
		OsxGuestCopyFramebufferToKindTexture(context, cpu->gpr[4], (int)cpu->gpr[5], cpu->gpr[6], (int)cpu->gpr[7], (int)cpu->gpr[8], (int)cpu->gpr[9], (int)cpu->gpr[10], 0, 0, 1);
		return 1;
	case GL_SUB_COPY_TEX_SUB_IMAGE2D:
		if (kind < 0 || kind == GUESTKIND3D)
			return 0;
		GfxAccelHostBeginFrame(context);
		OsxGuestCopyFramebufferToKindTexture(context, cpu->gpr[4], (int)cpu->gpr[5], 0, (int)cpu->gpr[8], (int)cpu->gpr[9], (int)cpu->gpr[10], (int)OsxGuestStackWord(cpu, 56), (int)cpu->gpr[6], (int)cpu->gpr[7], 0);
		return 1;
	}
	return 0;
}

void OsxGuestEngineCall(void *engine, OsxCpu *cpu, uint32 sub)
{
	OsxGuestEngineBind(engine, cpu);
	if (OsxGuestEngineFastCall(cpu, sub))
		return;
	OsxGuestForgetReadPages(OsxGuestStateOf(gl_current_context));
	if (OsxGuestStateCall(gl_current_context, cpu, sub))
		return;
	if (sub == GL_SUB_READ_PIXELS && cpu->gpr[8] == GL_DEPTH_COMPONENT && OsxGuestReadDepth(gl_current_context, cpu))
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
	GfxAccelHostTargetReadPixels((GLContext *)engine, width, height, pixels);
}

int OsxGuestEngineAdoptFrame(void *engine, OsxCpu *cpu, uint32 width, uint32 height)
{
	OsxGuestEngineFlush(engine, cpu);
	return GfxAccelHostTargetAdopt((GLContext *)engine, width, height);
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
	OsxGuestApplyKindTextures(gl_current_context);
}

void OsxGuestEngineBeforePresent(void)
{
	if (gl_current_context)
		OsxGuestDisableKindTextures(gl_current_context);
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
	if (!texture) {
		GfxAccelHostTargetReleaseOverlays(OsxGuestTargetOf(context));
		return;
	}
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
	std::vector<uint8_t> blank;
	int kind;
	OsxGuestEngineBind(engine, cpu);
	context = (GLContext *)engine;
	kind = OsxGuestTextureKind(texturetarget);
	object = OsxGuestBoundObject(context, texturetarget);
	if (!object || !GfxAccelHostFramebufferAvailable())
		return;
	if (kind < 0 && (!object->metal_texture || object->width != (int)width || object->height != (int)height)) {
		blank.resize((size_t)width * height * 4);
		object->width = (int)width;
		object->height = (int)height;
		GfxAccelHostUploadTexture(context, object, 0, (int)width, (int)height, vec_data(blank), (int)blank.size());
	}
	GfxAccelHostReadFramebufferAttach(OsxGuestTargetOf(context), texture);
	if (kind >= 0)
		OsxGuestCopyFramebufferToKindTexture(context, texturetarget, 0, internalformat, 0, 0, (int)width, (int)height, 0, 0, 1);
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

void OsxGuestEngineSamples(void *engine, OsxCpu *cpu, uint32 samples)
{
	OsxGuestEngineBind(engine, cpu);
	GfxAccelHostSamples((GLContext *)engine, (int)samples);
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
	OsxGuestForgetReadPages(OsxGuestStateOf(gl_current_context));
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

static const uint8 *OsxGuestSpan(uint32 address, uint32 length);

static float OsxGuestComponent(const uint8 *bytes, uint32 type, int normalized)
{
	uint32 bits;
	float value;
	switch (type) {
	case GL_FLOAT:
		bits = ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16) | ((uint32)bytes[2] << 8) | bytes[3];
		memcpy(&value, &bits, sizeof(value));
		return value;
	case GL_BYTE:
		if (normalized)
			return (float)(int8_t)bytes[0] / 127.f;
		return (float)(int8_t)bytes[0];
	case GL_UNSIGNED_BYTE:
		if (normalized)
			return (float)bytes[0] / 255.f;
		return (float)bytes[0];
	case GL_SHORT:
		if (normalized)
			return (float)(int16_t)((bytes[0] << 8) | bytes[1]) / 32767.f;
		return (float)(int16_t)((bytes[0] << 8) | bytes[1]);
	case GL_UNSIGNED_SHORT:
		if (normalized)
			return (float)((bytes[0] << 8) | bytes[1]) / 65535.f;
		return (float)((bytes[0] << 8) | bytes[1]);
	}
	return 0.f;
}

static void OsxGuestDrawAttributes(GLContext *context, uint32 mode, uint32 first, uint32 count, uint32 indextype, uint32 indices)
{
	OsxGuestState *state;
	const GLVertexArrayPointer *array;
	const uint8 *bytes;
	float *values;
	uint32 enabled[GUESTATTRIBUTES];
	uint32 enabledcount;
	uint32 attribute;
	uint32 vertex;
	uint32 index;
	uint32 slot;
	uint32 address;
	int component;
	int width;
	state = OsxGuestStateOf(context);
	enabledcount = 0;
	for (attribute = GUESTATTRIBUTES; attribute-- > 0;)
		if (state->guestattributes[attribute].enabled && state->guestattributes[attribute].pointer)
			enabled[enabledcount++] = attribute;
	state->guestattributevalues.resize((size_t)count * enabledcount * 4 + 4);
	values = vec_data(state->guestattributevalues);
	for (vertex = 0; vertex < count; vertex++) {
		index = first + vertex;
		if (indextype == GL_UNSIGNED_BYTE)
			index = OsxGuestRead8(indices + vertex);
		else if (indextype == GL_UNSIGNED_SHORT)
			index = OsxGuestRead16(indices + vertex * 2);
		else if (indextype)
			index = OsxGuestRead32(indices + vertex * 4);
		for (slot = 0; slot < enabledcount; slot++, values += 4) {
			array = &state->guestattributes[enabled[slot]];
			width = gl_type_size(array->type);
			address = array->pointer + index * gl_array_stride(*array);
			values[0] = 0.f;
			values[1] = 0.f;
			values[2] = 0.f;
			values[3] = 1.f;
			bytes = NULL;
			if (array->type != GL_DOUBLE && array->type != GL_INT && array->type != GL_UNSIGNED_INT)
				bytes = OsxGuestSpan(address, (uint32)(array->size * width));
			for (component = 0; component < array->size && component < 4; component++) {
				if (bytes)
					values[component] = OsxGuestComponent(bytes + component * width, array->type, (state->guestattributenormalized >> enabled[slot]) & 1);
				else if (state->guestattributenormalized & (1u << enabled[slot]))
					values[component] = gl_read_comp(address + (uint32)(component * width), array->type);
				else
					values[component] = gl_read_raw(address + (uint32)(component * width), array->type);
			}
		}
	}
	if (!state->guestfaultraised)
		GfxAccelHostDrawAttributes(context, OsxGuestTargetOf(context), mode, count, enabled, enabledcount, vec_data(state->guestattributevalues));
}

int OsxGuestEngineAttributeCall(void *engine, OsxCpu *cpu, uint32 call)
{
	GLContext *context;
	OsxGuestState *state;
	GLVertexArrayPointer *array;
	uint32 *arguments;
	if ((call < GUESTATTRIBUTEPOINTER || call > GUESTATTRIBUTEDISABLE) && call != GUESTDRAWARRAYS && call != GUESTDRAWELEMENTS && call != GUESTDRAWRANGEELEMENTS)
		return 0;
	context = (GLContext *)engine;
	state = OsxGuestStateOf(context);
	arguments = cpu->gpr + 4;
	if (!GfxAccelHostProgramEntryAvailable(OsxGuestTargetOf(context), GFXACCELHOSTATTRIBUTEENTRY))
		return 0;
	if (call >= GUESTATTRIBUTEPOINTER && call <= GUESTATTRIBUTEDISABLE) {
		if (arguments[0] >= GUESTATTRIBUTES)
			return 1;
		array = &state->guestattributes[arguments[0]];
		if (call == GUESTATTRIBUTEPOINTER) {
			array->size = (int)arguments[1];
			array->type = arguments[2];
			array->stride = (int)arguments[4];
			array->pointer = arguments[5];
			state->guestattributenormalized &= ~(1u << arguments[0]);
			if (arguments[3])
				state->guestattributenormalized |= 1u << arguments[0];
		} else
			array->enabled = call == GUESTATTRIBUTEENABLE;
		return 1;
	}
	if (!state->guestattributes[0].enabled || !GfxAccelHostProgramEnabled(OsxGuestTargetOf(context), GFXACCELHOSTVERTEXPROGRAM))
		return 0;
	OsxGuestEngineBind(engine, cpu);
	OsxGuestForgetReadPages(state);
	if (call == GUESTDRAWARRAYS)
		OsxGuestDrawAttributes(context, arguments[0], arguments[1], arguments[2], 0, 0);
	else if (call == GUESTDRAWELEMENTS)
		OsxGuestDrawAttributes(context, arguments[0], 0, arguments[1], arguments[2], arguments[3]);
	else
		OsxGuestDrawAttributes(context, arguments[0], 0, arguments[3], arguments[4], arguments[5]);
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
	OsxGuestState *state;
	OsxCpu *cpu;
	uint32 physical;
	uint32 page;
	uint32 index;
	state = OsxGuestStateOf(gl_current_context);
	page = address & ~4095u;
	for (index = 0; index < GUESTREADPAGES; index++)
		if (state->guestreadpages[index] == page)
			return state->guestreadhosts[index];
	if (state->guestfaultraised)
		return NULL;
	cpu = (OsxCpu *)state->guestcpu;
	if (!OsxGuestCpuTranslate(cpu, address, OSXTLBREAD, 1, &physical) || physical >= cpu->ramsize) {
		state->guestfaultraised = true;
		return NULL;
	}
	if ((physical | 4095u) >= cpu->ramsize)
		return NULL;
	index = state->guestreadnext;
	state->guestreadnext = (index + 1) % GUESTREADPAGES;
	state->guestreadpages[index] = page;
	state->guestreadhosts[index] = cpu->ram + (physical & ~4095u);
	return state->guestreadhosts[index];
}

static const uint8 *OsxGuestSpan(uint32 address, uint32 length)
{
	uint8 *page;
	if (address - OSXGLSCRATCHBASE < sizeof(OsxGuestStateOf(gl_current_context)->guestscratch)) {
		if (address - OSXGLSCRATCHBASE + length > sizeof(OsxGuestStateOf(gl_current_context)->guestscratch))
			return NULL;
		return OsxGuestStateOf(gl_current_context)->guestscratch + (address - OSXGLSCRATCHBASE);
	}
	if ((address & 4095) + length > 4096)
		return NULL;
	page = OsxGuestReadPage(address);
	if (!page)
		return NULL;
	return page + (address & 4095);
}

uint8 OsxGuestRead8(uint32 address)
{
	const uint8 *byte;
	byte = OsxGuestSpan(address, 1);
	if (!byte)
		return 0;
	return *byte;
}

uint16 OsxGuestRead16(uint32 address)
{
	const uint8 *bytes;
	bytes = OsxGuestSpan(address, 2);
	if (!bytes)
		return (uint16)((OsxGuestRead8(address) << 8) | OsxGuestRead8(address + 1));
	return (uint16)((bytes[0] << 8) | bytes[1]);
}

uint32 OsxGuestRead32(uint32 address)
{
	uint32 value;
	const uint8 *bytes;
	bytes = OsxGuestSpan(address, 4);
	if (bytes) {
		memcpy(&value, bytes, sizeof(value));
		return bswap_32(value);
	}
	if ((address & 4095) <= 4092)
		return 0;
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
