/*
 *  osxglbridge.h - host OpenGL bridge for Mac OS X guest Quartz OpenGL contexts
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
#ifndef OSXGLBRIDGE_H
#define OSXGLBRIDGE_H

#define OSXQUARTZTRAP 0x44c00002u
#define OSXQUARTZSTRINGS 0x400u
#define OSXQUARTZRENDERERSTRING 0x800u
#define OSXQUARTZVENDORSTRING 0x880u
#define OSXQUARTZVERSIONSTRING 0x900u
#define OSXQUARTZSTRINGEND 0x920u
#define OSXQUARTZVERSION "1.3 APPLE-bridge"
#define OSXQUARTZEXTENSIONS "GL_ARB_multitexture GL_ARB_texture_rectangle GL_EXT_texture_rectangle " \
	"GL_ARB_texture_non_power_of_two GL_APPLE_client_storage GL_APPLE_packed_pixels GL_EXT_bgra GL_EXT_abgr " \
	"GL_EXT_blend_func_separate GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_secondary_color GL_ARB_texture_env_combine " \
	"GL_ARB_texture_env_add GL_ARB_texture_env_crossbar GL_ARB_texture_env_dot3 GL_ARB_texture_border_clamp " \
	"GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_ARB_texture_mirrored_repeat GL_EXT_texture_lod_bias " \
	"GL_SGIS_generate_mipmap GL_EXT_texture_filter_anisotropic GL_ARB_texture_float GL_APPLE_float_pixels " \
	"GL_ARB_fragment_program GL_APPLE_flush_render GL_APPLE_pixel_buffer"

#ifndef OSXGLBRIDGE
#define OSXGLBRIDGE 0
#endif
#if OSXGLBRIDGE

typedef struct OsxQuartzBridge OsxQuartzBridge;
typedef uint32 (*OsxQuartzProcessFunction)(void *owner, OsxCpu *cpu);
typedef int (*OsxQuartzEventLoopFunction)(void *owner, OsxCpu *cpu);
OsxQuartzBridge *OsxGuestCreateQuartzBridge(uint32 width, uint32 height, uint32 library, uint32 strings, int extensions, int allocatedextensions,
	uint32 allocate, uint32 clear, uint8 *vram, uint32 rowbytes, struct OsxHostVideo *video, uint8 **present, const uint32 *windowcalls,
	OsxQuartzProcessFunction process, void *processowner);
void OsxGuestQuartzComposeWindows(OsxQuartzBridge *bridge, uint8 *present, uint32 depth);
void OsxGuestDestroyQuartzBridge(OsxQuartzBridge *bridge);
void OsxGuestQuartzResize(OsxQuartzBridge *bridge, uint32 width, uint32 height);
int OsxGuestQuartzFullScreen(OsxQuartzBridge *bridge);
int OsxGuestQuartzIdleGeometry(OsxQuartzBridge *bridge, OsxCpu *cpu, OsxQuartzEventLoopFunction eventloop, void *owner);
void OsxGuestQuartzKeepProcesses(OsxQuartzBridge *bridge, const uint32 *processes, const uint32 *spaces, uint32 count, uint32 mask);
int OsxGuestQuartzBridgeCall(OsxQuartzBridge *bridge, OsxCpu *cpu, const uint32 *tails, uint32 call, uint32 depth, FILE *log);
void OsxGuestGlShutdownHost(void);

#endif

namespace OsxGl {
void *OsxGuestEngineCreate(void *share);
void OsxGuestEngineDestroy(void *engine);
void OsxGuestEngineBind(void *engine, OsxCpu *cpu);
void OsxGuestEngineResize(void *engine, OsxCpu *cpu, uint32 width, uint32 height);
void OsxGuestEngineCall(void *engine, OsxCpu *cpu, uint32 sub);
void OsxGuestEngineFlush(void *engine, OsxCpu *cpu);
void OsxGuestEngineReadPixels(void *engine, OsxCpu *cpu, uint32 width, uint32 height, void *pixels);
void OsxGuestEngineHostString(void *engine, OsxCpu *cpu, uint32 name, char *text, uint32 size);
void OsxGuestEngineBeforePresent(void);
void OsxGuestEngineAfterPresent(void);
unsigned OsxGuestEngineTextureCreate(void *engine, OsxCpu *cpu, uint32 width, uint32 height, int floatpixels);
void OsxGuestEngineTextureDelete(void *engine, OsxCpu *cpu, unsigned texture);
void OsxGuestEngineAttachTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height);
void OsxGuestEngineCopyTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height, uint32 internalformat, uint32 texturetarget);
void OsxGuestEngineBlendSeparate(void *engine, OsxCpu *cpu, int separate);
int OsxGuestEngineProgramCall(void *engine, OsxCpu *cpu, uint32 call);
void OsxGuestEngineProgramEnable(void *engine, OsxCpu *cpu, uint32 capability, int enable);
}
#endif
