/*
 *	(C) 2026 Ryan Norton (battlemageloveryt@gmail.com)
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
#include "sdlgpu_gl_compat.h"
#include "../include/gfxaccel_resources.h"

static void *SDLGPUVendGLOverlayTexture(uint32_t engine_id,
	uint32_t texture_index, uint32_t width, uint32_t height,
	uint32_t pixel_format);

#define gfxaccel_resources_vend_overlay_texture_indexed SDLGPUVendGLOverlayTexture
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define gl_overlay_bind SDLGPUGLOverlayBind
#define gl_overlay_unbind SDLGPUGLOverlayUnbind
#define gl_release_overlay_for_detach SDLGPUGLReleaseOverlayForDetach
#endif
#include "../gl/gl_ffp_renderer.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef gl_release_overlay_for_detach
#undef gl_overlay_unbind
#undef gl_overlay_bind
#endif
#undef gfxaccel_resources_vend_overlay_texture_indexed

static void *SDLGPUVendGLOverlayTexture(uint32_t engine_id,
	uint32_t texture_index, uint32_t width, uint32_t height,
	uint32_t pixel_format)
{
	void *texture = gfxaccel_resources_vend_overlay_texture_indexed(
		engine_id, texture_index, width, height, pixel_format);
	if (texture)
		SDLGPUSetTexturePresentationYFlip((GLuint)(uintptr_t)texture, true);
	return texture;
}

#if SDLGPU_TRANSITION_LOGGING_ENABLED
extern "C" void gl_overlay_bind(int32_t left, int32_t top, int32_t width,
	int32_t height)
{
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUGLOverlayBind(left, top, width, height);
	SDLGPUTransitionTraceEnd("gl-overlay-bind", kGfxEngineGL, width, height,
		trace_start_tick);
}

extern "C" void gl_overlay_unbind(void)
{
	int width = (int)s_ow;
	int height = (int)s_oh;
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUGLOverlayUnbind();
	SDLGPUTransitionTraceEnd("gl-overlay-unbind", kGfxEngineGL, width, height,
		trace_start_tick);
}

extern "C" void gl_release_overlay_for_detach(void)
{
	int width = (int)s_ow;
	int height = (int)s_oh;
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUGLReleaseOverlayForDetach();
	SDLGPUTransitionTraceEnd("gl-overlay-detach", kGfxEngineGL, width, height,
		trace_start_tick);
}
#endif
