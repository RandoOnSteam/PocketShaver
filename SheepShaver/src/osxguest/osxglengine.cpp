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
#include "gl_ext.h"
#include "gl_agl_renderer_policy.h"
#include "gl_clip_plane_policy.h"
#include "gl_color_index_policy.h"
#include "gl_drawable_owner_policy.h"
#include "gl_metal_draw_state.h"
#include "gl_offscreen_policy.h"
#include "gl_pixel_unpack_policy.h"
#include "debug.h"
#include "osxcpu.h"
#include "osxglengine.h"

#define OSXGLSCRATCHBASE 0xfffc0000u
#define OSXGLSCRATCHWORD 512u
#define OSXGLSTACKSCRATCH 32u

#define ReadMacInt8 GuestRead8
#define ReadMacInt16 GuestRead16
#define ReadMacInt32 GuestRead32
#define WriteMacInt8 GuestWrite8
#define WriteMacInt16 GuestWrite16
#define WriteMacInt32 GuestWrite32
#define Mac2HostAddr GuestHostAddress
#define Mac_sysalloc GuestAllocate
#define Mac_memset GuestSet
#define Mac2Host_memcpy GuestCopyIn
#define Host2Mac_memcpy GuestCopyOut
#define GLGUESTBYTES GuestBytes
#define gfxaccel_resources_vend_overlay_texture_indexed GuestOverlayVend
#define gfxaccel_resources_release_overlay_texture GuestOverlayRelease
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

uint8 GuestRead8(uint32 address);
uint16 GuestRead16(uint32 address);
uint32 GuestRead32(uint32 address);
void GuestWrite8(uint32 address, uint32 value);
void GuestWrite16(uint32 address, uint32 value);
void GuestWrite32(uint32 address, uint32 value);
uint8 *GuestHostAddress(uint32 address);
uint32 GuestAllocate(uint32 size);
void *GuestSet(uint32 address, int value, size_t length);
void *GuestCopyIn(void *destination, uint32 source, size_t length);
void *GuestCopyOut(uint32 destination, const void *source, size_t length);
const uint8 *GuestBytes(uint32 address, int32_t length);
void *GuestOverlayVend(int engine, int index, uint32_t width, uint32_t height, uint32_t format);
void GuestOverlayRelease(int engine, void *texture);

bool RaveGetLastCL8ColorTableRGBSnapshot(unsigned char *table)
{
	(void)table;
	return false;
}

#include "../gfxaccel/gl_state.cpp"
#include "../gfxaccel/gl_defer.cpp"
#include "../gfxaccel/gl_dispatch.cpp"
#include "../gfxaccel/gl_thunks.cpp"
#define ReadMacDouble EngineReadMacDouble
#include "../gfxaccel/gl_engine.cpp"
#undef ReadMacDouble
#include "../gfxaccel/gl/gl_ffp_renderer.cpp"

typedef struct GuestTarget {
	GLuint overlays[2];
	GLuint current;
	GLuint framebuffer;
	GLuint depth;
	GLuint pending;
	uint32_t width;
	uint32_t height;
	uint32_t write;
	int32_t left;
	int32_t top;
	int32_t drawwidth;
	int32_t drawheight;
	bool frameactive;
	bool committed;
	bool attached;
	GLuint readframebuffer;
	GLuint programs[2];
	bool programenabled[2];
	bool envset[2][96];
	float env[2][96][4];
} GuestTarget;

#define GUESTPROGRAMS 19

typedef void (APIENTRY *GuestBindProgram)(GLenum, GLuint);
typedef void (APIENTRY *GuestProgramNames)(GLsizei, GLuint *);
typedef GLboolean (APIENTRY *GuestIsProgram)(GLuint);
typedef void (APIENTRY *GuestProgramDoubles)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (APIENTRY *GuestProgramFloats)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *GuestProgramVector)(GLenum, GLuint, void *);
typedef void (APIENTRY *GuestProgramString)(GLenum, GLenum, GLsizei, const void *);
typedef void (APIENTRY *GuestProgramQuery)(GLenum, GLenum, void *);

static void **GuestProgramProcs(void)
{
	static void *procs[GUESTPROGRAMS];
	static bool loaded = false;
	static const char *const names[GUESTPROGRAMS] = {
		"glBindProgramARB", "glDeleteProgramsARB", "glGenProgramsARB", "glIsProgramARB",
		"glProgramEnvParameter4dARB", "glProgramEnvParameter4dvARB", "glProgramEnvParameter4fARB",
		"glProgramEnvParameter4fvARB", "glProgramLocalParameter4dARB", "glProgramLocalParameter4dvARB",
		"glProgramLocalParameter4fARB", "glProgramLocalParameter4fvARB", "glGetProgramEnvParameterdvARB",
		"glGetProgramEnvParameterfvARB", "glGetProgramLocalParameterdvARB", "glGetProgramLocalParameterfvARB",
		"glProgramStringARB", "glGetProgramStringARB", "glGetProgramivARB"
	};
	int index;
	if (!loaded) {
		for (index = 0; index < GUESTPROGRAMS; index++)
			procs[index] = GfxGLGetProcAddress(names[index]);
		loaded = true;
	}
	return procs;
}

static GLenum GuestProgramTarget(int index)
{
	if (index == 0)
		return 0x8620;
	return 0x8804;
}

static int GuestProgramIndex(GLenum target)
{
	if (target == 0x8620)
		return 0;
	if (target == 0x8804)
		return 1;
	return -1;
}

static void GuestProgramsApply(GuestTarget *target)
{
	void **procs;
	int type;
	int slot;
	procs = GuestProgramProcs();
	if (!procs[0] || !procs[6])
		return;
	for (type = 0; type < 2; type++) {
		((GuestBindProgram)procs[0])(GuestProgramTarget(type), target->programs[type]);
		if (target->programenabled[type])
			glEnable(GuestProgramTarget(type));
		else
			glDisable(GuestProgramTarget(type));
		for (slot = 0; slot < 96; slot++) {
			if (target->envset[type][slot])
				((GuestProgramFloats)procs[6])(GuestProgramTarget(type), slot, target->env[type][slot][0],
					target->env[type][slot][1], target->env[type][slot][2], target->env[type][slot][3]);
		}
	}
}

static GuestTarget *TargetOf(GLContext *context)
{
	return (GuestTarget *)context->guesttarget;
}

static void TargetSave(GLContext *context)
{
	GuestTarget *target;
	target = TargetOf(context);
	target->overlays[0] = s_ov[0];
	target->overlays[1] = s_ov[1];
	target->current = s_cur;
	target->framebuffer = s_fbo;
	target->depth = s_depth;
	target->width = s_ow;
	target->height = s_oh;
	target->write = s_wr;
	target->left = s_dl;
	target->top = s_dt;
	target->drawwidth = s_dw;
	target->drawheight = s_dh;
	target->frameactive = s_frame_active;
	target->committed = s_frame_committed;
}

static void TargetLoad(GLContext *context)
{
	GuestTarget *target;
	target = TargetOf(context);
	s_ov[0] = target->overlays[0];
	s_ov[1] = target->overlays[1];
	s_cur = target->current;
	s_fbo = target->framebuffer;
	s_depth = target->depth;
	s_ow = target->width;
	s_oh = target->height;
	s_wr = target->write;
	s_dl = target->left;
	s_dt = target->top;
	s_dw = target->drawwidth;
	s_dh = target->drawheight;
	s_frame_active = target->frameactive;
	s_frame_committed = target->committed;
}

void *GuestOverlayVend(int engine, int index, uint32_t width, uint32_t height, uint32_t format)
{
	GuestTarget *target;
	GLuint name;
	(void)engine;
	(void)format;
	target = TargetOf(gl_current_context);
	if (index != 0)
		return (void *)(uintptr_t)target->pending;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	target->pending = name;
	return (void *)(uintptr_t)name;
}

void GuestOverlayRelease(int engine, void *texture)
{
	GLuint name;
	(void)engine;
	name = (GLuint)(uintptr_t)texture;
	glDeleteTextures(1, &name);
}

void *EngineCreate(void)
{
	GLContext *context;
	InitFuncSignatures();
	gl_scratch_addr = OSXGLSCRATCHBASE;
	context = GLContextNew(640, 480);
	context->guesttarget = calloc(1, sizeof(GuestTarget));
	return context;
}

void EngineBind(void *engine, OsxCpu *cpu)
{
	GLContext *next;
	GfxGLExt *extensions;
	next = (GLContext *)engine;
	next->guestcpu = cpu;
	if (gl_current_context == next)
		return;
	if (!SharedMetalDevice())
		return;
	extensions = &gfx_gl_ext();
	if (gl_current_context) {
		TargetSave(gl_current_context);
		if (s_frame_active && extensions->fbo)
			extensions->BindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	TargetLoad(next);
	gl_current_context = next;
	if (!next->metal)
		GLMetalInit(next);
	if (s_frame_active && extensions->fbo)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	GLMetalInvalidateStateCache(next);
	GuestProgramsApply(TargetOf(next));
}

void EngineDestroy(void *engine)
{
	GLContext *context;
	context = (GLContext *)engine;
	EngineBind(context, (OsxCpu *)context->guestcpu);
	if (TargetOf(context)->attached) {
		s_ov[0] = 0;
		s_ov[1] = 0;
		s_cur = 0;
	}
	GLMetalReleaseOverlayTextures(true);
	if (context->metal)
		GLMetalRelease(context);
	free(context->guesttarget);
	delete context;
	gl_current_context = NULL;
}

void EngineResize(void *engine, OsxCpu *cpu, uint32 width, uint32 height)
{
	EngineBind(engine, cpu);
	gl_overlay_bind(0, 0, (int32_t)width, (int32_t)height);
}

static void EngineDispatch(OsxCpu *cpu, uint32 sub, int first)
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
	GuestWrite32(gl_scratch_addr, sub);
	last = 0;
	if (first == 3)
		last = cpu->gpr[10];
	cpu->gpr[3] = GLDispatch(cpu->gpr[first], cpu->gpr[first + 1], cpu->gpr[first + 2], cpu->gpr[first + 3],
		cpu->gpr[first + 4], cpu->gpr[first + 5], cpu->gpr[first + 6], last, floats, count);
}

static float EngineFloat(OsxCpu *cpu, int index)
{
	double value;
	memcpy(&value, &cpu->fpr[index], sizeof(value));
	return (float)value;
}

static int EngineFastCall(OsxCpu *cpu, uint32 sub)
{
	GLContext *context;
	context = gl_current_context;
	if (context->in_display_list)
		return 0;
	switch (sub) {
	case GL_SUB_VERTEX2F:
		NativeGLVertex2f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2));
		return 1;
	case GL_SUB_VERTEX3F:
		NativeGLVertex3f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2), EngineFloat(cpu, 3));
		return 1;
	case GL_SUB_COLOR3F:
		NativeGLColor3f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2), EngineFloat(cpu, 3));
		return 1;
	case GL_SUB_COLOR4F:
		NativeGLColor4f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2), EngineFloat(cpu, 3), EngineFloat(cpu, 4));
		return 1;
	case GL_SUB_NORMAL3F:
		NativeGLNormal3f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2), EngineFloat(cpu, 3));
		return 1;
	case GL_SUB_TEX_COORD2F:
		NativeGLTexCoord2f(context, EngineFloat(cpu, 1), EngineFloat(cpu, 2));
		return 1;
	}
	return 0;
}

void EngineCall(void *engine, OsxCpu *cpu, uint32 sub)
{
	EngineBind(engine, cpu);
	if (EngineFastCall(cpu, sub))
		return;
	gl_current_context->guestreadpage = 1;
	EngineDispatch(cpu, sub, 4);
}

void EngineFlush(void *engine, OsxCpu *cpu)
{
	EngineBind(engine, cpu);
	if (s_frame_active)
		glFlush();
}

void EngineReadPixels(void *engine, OsxCpu *cpu, uint32 width, uint32 height, void *pixels)
{
	GfxGLExt *extensions;
	EngineFlush(engine, cpu);
	extensions = &gfx_gl_ext();
	if (!s_fbo || !extensions->fbo) {
		memset(pixels, 0, (size_t)width * height * 4);
		return;
	}
	if (!s_frame_active)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, 0x8035, pixels);
	if (!s_frame_active)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void EngineHostString(void *engine, OsxCpu *cpu, uint32 name, char *text, uint32 size)
{
	const char *value;
	EngineBind(engine, cpu);
	memset(text, 0, size);
	value = (const char *)glGetString(name);
	if (value)
		strncpy(text, value, size - 1);
}

int EngineFlurryCall(void *engine, OsxCpu *cpu, uint32 call)
{
	uint32 *a;
	int sub;
	double one;
	EngineBind(engine, cpu);
	gl_current_context->guestreadpage = 1;
	a = cpu->gpr + 3;
	switch (call) {
	case 45: sub = GL_SUB_ALPHA_FUNC; break;
	case 46: sub = GL_SUB_BEGIN; break;
	case 47: sub = GL_SUB_BIND_TEXTURE; break;
	case 48: sub = GL_SUB_BLEND_FUNC; break;
	case 49: sub = GL_SUB_CLEAR; break;
	case 50: sub = GL_SUB_CLEAR_COLOR; break;
	case 51: sub = GL_SUB_COLOR4F; break;
	case 52: sub = GL_SUB_COLOR4FV; break;
	case 53: sub = GL_SUB_COLOR_POINTER; break;
	case 54: sub = GL_SUB_DISABLE; break;
	case 55: sub = GL_SUB_DRAW_ARRAYS; break;
	case 56: sub = GL_SUB_DRAW_PIXELS; break;
	case 57: sub = GL_SUB_ENABLE; break;
	case 58: sub = GL_SUB_ENABLE_CLIENT_STATE; break;
	case 59: sub = GL_SUB_END; break;
	case 61: sub = GL_SUB_GEN_TEXTURES; break;
	case 62: sub = GL_SUB_GET_ERROR; break;
	case 64: sub = GL_SUB_LOAD_IDENTITY; break;
	case 65: sub = GL_SUB_MATRIX_MODE; break;
	case 66: return 1;
	case 67: sub = GL_SUB_PIXEL_ZOOM; break;
	case 68: sub = GL_SUB_POP_MATRIX; break;
	case 69: sub = GL_SUB_PUSH_MATRIX; break;
	case 70: sub = GL_SUB_RASTER_POS2I; break;
	case 71: sub = GL_SUB_RECTD; break;
	case 72: sub = GL_SUB_ROTATEF; break;
	case 73: sub = GL_SUB_SCALEF; break;
	case 74: sub = GL_SUB_SCISSOR; break;
	case 75: sub = GL_SUB_SHADE_MODEL; break;
	case 76: sub = GL_SUB_TEX_COORD_POINTER; break;
	case 77: sub = GL_SUB_TEX_ENVF; break;
	case 78: sub = GL_SUB_TEX_PARAMETERI; break;
	case 79: sub = GL_SUB_TRANSLATEF; break;
	case 80: sub = GL_SUB_VERTEX2F; break;
	case 81: sub = GL_SUB_VERTEX_POINTER; break;
	case 82: sub = GL_SUB_VIEWPORT; break;
	case 83:
		GuestWrite32(gl_scratch_addr, GL_SUB_TEX_PARAMETERI);
		gl_ppc_sp = 0;
		gl_ppc_stack_arg_offset = 0;
		GLDispatch(a[0], 0x8191, 1, 0, 0, 0, 0, 0, NULL, 0);
		GuestWrite32(OSXGLSCRATCHBASE + OSXGLSTACKSCRATCH + 24 + 32, a[6]);
		gl_ppc_sp = OSXGLSCRATCHBASE + OSXGLSTACKSCRATCH;
		GuestWrite32(gl_scratch_addr, GL_SUB_TEX_IMAGE2D);
		GLDispatch(a[0], 0, a[1], a[2], a[3], 0, a[4], a[5], NULL, 0);
		a[0] = 0;
		return 1;
	case 85:
		one = -1.0;
		memcpy(&cpu->fpr[5], &one, sizeof(one));
		one = 1.0;
		memcpy(&cpu->fpr[6], &one, sizeof(one));
		sub = GL_SUB_ORTHO;
		break;
	default:
		return 0;
	}
	EngineDispatch(cpu, (uint32)sub, 3);
	return 1;
}

void EngineAfterPresent(void)
{
	GfxGLExt *extensions;
	if (!gl_current_context || !s_frame_active)
		return;
	extensions = &gfx_gl_ext();
	if (extensions->fbo)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	GLMetalInvalidateStateCache(gl_current_context);
	GuestProgramsApply(TargetOf(gl_current_context));
}

unsigned EngineTextureCreate(void *engine, OsxCpu *cpu, uint32 width, uint32 height, int floatpixels)
{
	GLuint name;
	EngineBind(engine, cpu);
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (floatpixels)
		glTexImage2D(GL_TEXTURE_2D, 0, 0x881a, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLMetalInvalidateStateCache(gl_current_context);
	return name;
}

void EngineTextureDelete(void *engine, OsxCpu *cpu, unsigned texture)
{
	GLuint name;
	EngineBind(engine, cpu);
	name = texture;
	glDeleteTextures(1, &name);
}

void EngineAttachTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height)
{
	GLContext *context;
	GuestTarget *target;
	EngineBind(engine, cpu);
	context = (GLContext *)engine;
	target = TargetOf(context);
	if (s_frame_active)
		GLMetalEndFrame(context);
	if (!target->attached && s_ov[0])
		GLMetalReleaseOverlayTextures(false);
	target->attached = true;
	s_ov[0] = texture;
	s_ov[1] = texture;
	s_cur = texture;
	s_ow = width;
	s_oh = height;
	s_wr = 0;
	s_dl = 0;
	s_dt = 0;
	s_dw = (int32_t)width;
	s_dh = (int32_t)height;
	s_frame_committed = false;
	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = (int32_t)width;
	context->viewport[3] = (int32_t)height;
	context->scissor_box[0] = 0;
	context->scissor_box[1] = 0;
	context->scissor_box[2] = (int32_t)width;
	context->scissor_box[3] = (int32_t)height;
}

void EngineCopyTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height, uint32 internalformat)
{
	GLContext *context;
	GuestTarget *target;
	GfxGLExt *extensions;
	GLTextureObject *object;
	GLuint name;
	std::vector<uint8_t> blank;
	EngineBind(engine, cpu);
	context = (GLContext *)engine;
	target = TargetOf(context);
	extensions = &gfx_gl_ext();
	name = context->tex_units[context->active_texture].bound_texture_2d;
	if (!name || !extensions->fbo)
		return;
	object = &context->texture_objects[name];
	object->name = name;
	if (!object->metal_texture || object->width != (int)width || object->height != (int)height) {
		blank.resize((size_t)width * height * 4);
		object->width = (int)width;
		object->height = (int)height;
		GLMetalUploadTexture(context, object, 0, (int)width, (int)height, vec_data(blank), (int)blank.size());
	}
	if (!target->readframebuffer)
		extensions->GenFramebuffers(1, &target->readframebuffer);
	extensions->BindFramebuffer(0x8ca8, target->readframebuffer);
	extensions->FramebufferTexture2D(0x8ca8, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)object->metal_texture);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, internalformat, 0, 0, (GLsizei)width, (GLsizei)height, 0);
	if (s_frame_active)
		extensions->BindFramebuffer(0x8ca8, s_fbo);
	else
		extensions->BindFramebuffer(0x8ca8, 0);
	GLMetalInvalidateStateCache(context);
}

void EngineBlendSeparate(void *engine, OsxCpu *cpu, int separate)
{
	GLContext *context;
	EngineBind(engine, cpu);
	context = (GLContext *)engine;
	context->guestblendseparate = separate != 0;
	if (separate) {
		context->blend_src = cpu->gpr[4];
		context->blend_dst = cpu->gpr[5];
		context->guestblendsrcalpha = cpu->gpr[6];
		context->guestblenddstalpha = cpu->gpr[7];
	}
}

void EngineProgramEnable(void *engine, OsxCpu *cpu, uint32 capability, int enable)
{
	GuestTarget *target;
	int type;
	EngineBind(engine, cpu);
	target = TargetOf((GLContext *)engine);
	type = GuestProgramIndex(capability);
	target->programenabled[type] = enable != 0;
	if (enable)
		glEnable(capability);
	else
		glDisable(capability);
}

static double GuestArgumentFloat(OsxCpu *cpu, int index)
{
	double value;
	memcpy(&value, &cpu->fpr[index], sizeof(value));
	return value;
}

static double GuestReadDouble(uint32 address)
{
	uint64 bits;
	double value;
	bits = ((uint64)GuestRead32(address) << 32) | GuestRead32(address + 4);
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void GuestWriteDouble(uint32 address, double value)
{
	uint64 bits;
	memcpy(&bits, &value, sizeof(bits));
	GuestWrite32(address, (uint32)(bits >> 32));
	GuestWrite32(address + 4, (uint32)bits);
}

static void GuestRememberEnv(GuestTarget *target, uint32 programtarget, uint32 slot, const float *values)
{
	int type;
	type = GuestProgramIndex(programtarget);
	if (type < 0 || slot >= 96)
		return;
	memcpy(target->env[type][slot], values, sizeof(target->env[type][slot]));
	target->envset[type][slot] = true;
}

int EngineProgramCall(void *engine, OsxCpu *cpu, uint32 call)
{
	void **procs;
	GuestTarget *target;
	std::vector<uint8_t> buffer;
	uint32 *arguments;
	float floats[4];
	double doubles[4];
	GLint length;
	uint32 count;
	uint32 index;
	int program;
	int type;
	program = -1;
	if (call >= 472 && call <= 475)
		program = (int)call - 472;
	else if (call >= 519 && call <= 533)
		program = (int)call - 515;
	if (program < 0)
		return 0;
	procs = GuestProgramProcs();
	if (!procs[program])
		return 0;
	EngineBind(engine, cpu);
	gl_current_context->guestreadpage = 1;
	target = TargetOf((GLContext *)engine);
	arguments = cpu->gpr + 4;
	switch (call) {
	case 472:
		((GuestBindProgram)procs[0])(arguments[0], arguments[1]);
		type = GuestProgramIndex(arguments[0]);
		if (type >= 0)
			target->programs[type] = arguments[1];
		break;
	case 473:
		count = arguments[0];
		buffer.resize((size_t)count * 4 + 4);
		for (index = 0; index < count; index++)
			((uint32 *)vec_data(buffer))[index] = GuestRead32(arguments[1] + index * 4);
		((GuestProgramNames)procs[1])(count, (GLuint *)vec_data(buffer));
		break;
	case 474:
		count = arguments[0];
		buffer.resize((size_t)count * 4 + 4);
		((GuestProgramNames)procs[2])(count, (GLuint *)vec_data(buffer));
		for (index = 0; index < count; index++)
			GuestWrite32(arguments[1] + index * 4, ((uint32 *)vec_data(buffer))[index]);
		break;
	case 475:
		cpu->gpr[3] = ((GuestIsProgram)procs[3])(arguments[0]);
		break;
	case 519: case 523:
		((GuestProgramDoubles)procs[program])(arguments[0], arguments[1], GuestArgumentFloat(cpu, 1),
			GuestArgumentFloat(cpu, 2), GuestArgumentFloat(cpu, 3), GuestArgumentFloat(cpu, 4));
		if (call == 519) {
			for (index = 0; index < 4; index++)
				floats[index] = (float)GuestArgumentFloat(cpu, 1 + (int)index);
			GuestRememberEnv(target, arguments[0], arguments[1], floats);
		}
		break;
	case 521: case 525:
		for (index = 0; index < 4; index++)
			floats[index] = (float)GuestArgumentFloat(cpu, 1 + (int)index);
		((GuestProgramFloats)procs[program])(arguments[0], arguments[1], floats[0], floats[1], floats[2], floats[3]);
		if (call == 521)
			GuestRememberEnv(target, arguments[0], arguments[1], floats);
		break;
	case 520: case 524:
		for (index = 0; index < 4; index++) {
			doubles[index] = GuestReadDouble(arguments[2] + index * 8);
			floats[index] = (float)doubles[index];
		}
		((GuestProgramVector)procs[program])(arguments[0], arguments[1], doubles);
		if (call == 520)
			GuestRememberEnv(target, arguments[0], arguments[1], floats);
		break;
	case 522: case 526:
		for (index = 0; index < 4; index++) {
			uint32 bits;
			bits = GuestRead32(arguments[2] + index * 4);
			memcpy(&floats[index], &bits, sizeof(bits));
		}
		((GuestProgramVector)procs[program])(arguments[0], arguments[1], floats);
		if (call == 522)
			GuestRememberEnv(target, arguments[0], arguments[1], floats);
		break;
	case 527: case 529:
		((GuestProgramVector)procs[program])(arguments[0], arguments[1], doubles);
		for (index = 0; index < 4; index++)
			GuestWriteDouble(arguments[2] + index * 8, doubles[index]);
		break;
	case 528: case 530:
		((GuestProgramVector)procs[program])(arguments[0], arguments[1], floats);
		for (index = 0; index < 4; index++) {
			uint32 bits;
			memcpy(&bits, &floats[index], sizeof(bits));
			GuestWrite32(arguments[2] + index * 4, bits);
		}
		break;
	case 531:
		buffer.resize((size_t)arguments[2] + 1);
		GuestCopyIn(vec_data(buffer), arguments[3], arguments[2]);
		((GuestProgramString)procs[16])(arguments[0], arguments[1], arguments[2], vec_data(buffer));
		break;
	case 532:
		length = 0;
		((GuestProgramQuery)procs[18])(arguments[0], 0x8627, &length);
		buffer.resize((size_t)length + 1);
		((GuestProgramQuery)procs[17])(arguments[0], arguments[1], vec_data(buffer));
		GuestCopyOut(arguments[2], vec_data(buffer), (size_t)length);
		break;
	case 533:
		length = 0;
		((GuestProgramQuery)procs[18])(arguments[0], arguments[1], &length);
		GuestWrite32(arguments[2], (uint32)length);
		break;
	default:
		return 0;
	}
	return 1;
}

static uint8 *GuestScratch(uint32 address)
{
	if (address < OSXGLSCRATCHBASE || address - OSXGLSCRATCHBASE >= sizeof(gl_current_context->guestscratch))
		return NULL;
	return gl_current_context->guestscratch + (address - OSXGLSCRATCHBASE);
}

static uint8 *GuestByte(uint32 address, int kind)
{
	OsxCpu *cpu;
	uint32 physical;
	uint8 *scratch;
	scratch = GuestScratch(address);
	if (scratch)
		return scratch;
	cpu = (OsxCpu *)gl_current_context->guestcpu;
	if (!OsxCpuTranslate(cpu, address, kind, 1, &physical) || physical >= cpu->ramsize)
		return NULL;
	return cpu->ram + physical;
}

static uint8 *GuestReadPage(uint32 address)
{
	GLContext *context;
	OsxCpu *cpu;
	uint32 physical;
	context = gl_current_context;
	if ((address & ~4095u) == context->guestreadpage)
		return context->guestreadhost;
	cpu = (OsxCpu *)context->guestcpu;
	if (!OsxCpuTranslate(cpu, address, OSXTLBREAD, 1, &physical) || physical >= cpu->ramsize)
		return NULL;
	if ((physical | 4095u) >= cpu->ramsize)
		return NULL;
	context->guestreadpage = address & ~4095u;
	context->guestreadhost = cpu->ram + (physical & ~4095u);
	return context->guestreadhost;
}

uint8 GuestRead8(uint32 address)
{
	uint8 *byte;
	if (!GuestScratch(address)) {
		byte = GuestReadPage(address);
		if (byte)
			return byte[address & 4095];
		return 0;
	}
	byte = GuestByte(address, OSXTLBREAD);
	if (!byte)
		return 0;
	return *byte;
}

uint16 GuestRead16(uint32 address)
{
	return (uint16)((GuestRead8(address) << 8) | GuestRead8(address + 1));
}

uint32 GuestRead32(uint32 address)
{
	uint32 value;
	uint8 *page;
	page = GuestScratch(address);
	if (page) {
		memcpy(&value, page, sizeof(value));
		return bswap_32(value);
	}
	if ((address & 4095) <= 4092) {
		page = GuestReadPage(address);
		if (page) {
			memcpy(&value, page + (address & 4095), sizeof(value));
			return bswap_32(value);
		}
		return 0;
	}
	value = 0;
	OsxCpuReadVirtual((OsxCpu *)gl_current_context->guestcpu, address, &value);
	return value;
}

void GuestWrite8(uint32 address, uint32 value)
{
	uint8 *byte;
	byte = GuestByte(address, OSXTLBWRITE);
	if (byte)
		*byte = (uint8)value;
}

void GuestWrite16(uint32 address, uint32 value)
{
	GuestWrite8(address, value >> 8);
	GuestWrite8(address + 1, value);
}

void GuestWrite32(uint32 address, uint32 value)
{
	uint8 *scratch;
	scratch = GuestScratch(address);
	if (scratch) {
		value = bswap_32(value);
		memcpy(scratch, &value, sizeof(value));
		return;
	}
	OsxCpuWriteVirtual((OsxCpu *)gl_current_context->guestcpu, address, value);
}

uint8 *GuestHostAddress(uint32 address)
{
	return GuestScratch(address);
}

uint32 GuestAllocate(uint32 size)
{
	GLContext *context;
	uint32 address;
	context = gl_current_context;
	if (context->guestscratchused < OSXGLSCRATCHWORD)
		context->guestscratchused = OSXGLSCRATCHWORD;
	if (size > sizeof(context->guestscratch) - context->guestscratchused)
		return 0;
	address = OSXGLSCRATCHBASE + context->guestscratchused;
	context->guestscratchused += size;
	return address;
}

void *GuestSet(uint32 address, int value, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++)
		GuestWrite8(address + (uint32)index, value);
	return GuestHostAddress(address);
}

void *GuestCopyIn(void *destination, uint32 source, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++)
		((uint8 *)destination)[index] = GuestRead8(source + (uint32)index);
	return destination;
}

void *GuestCopyOut(uint32 destination, const void *source, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++)
		GuestWrite8(destination + (uint32)index, ((const uint8 *)source)[index]);
	return GuestHostAddress(destination);
}

const uint8 *GuestBytes(uint32 address, int32_t length)
{
	if (length <= 0)
		return NULL;
	gl_current_context->guestbytes.resize((size_t)length);
	GuestCopyIn(vec_data(gl_current_context->guestbytes), address, (size_t)length);
	return vec_data(gl_current_context->guestbytes);
}

}
