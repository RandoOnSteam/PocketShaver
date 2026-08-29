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

static void *SDLGPUVendGlideOverlayTexture(uint32_t engine_id,
	uint32_t texture_index, uint32_t width, uint32_t height,
	uint32_t pixel_format);

#define gfxaccel_resources_vend_overlay_texture_indexed SDLGPUVendGlideOverlayTexture
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define GlideMetalInit SDLGPUOriginalGlideMetalInit
#define GlideMetalShutdown SDLGPUOriginalGlideMetalShutdown
#define GlideMetalWinOpen SDLGPUOriginalGlideMetalWinOpen
#define GlideMetalWinClose SDLGPUOriginalGlideMetalWinClose
#endif
#include "../gl/glide_gl_renderer.cpp"
#undef gfxaccel_resources_vend_overlay_texture_indexed

static void *SDLGPUVendGlideOverlayTexture(uint32_t engine_id,
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
#undef GlideMetalWinClose
#undef GlideMetalWinOpen
#undef GlideMetalShutdown
#undef GlideMetalInit

extern "C" int GlideMetalInit(void)
{
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	int result = SDLGPUOriginalGlideMetalInit();
	SDLGPUTransitionTraceEnd("glide-init", kGfxEngineGlide, 0, 0,
		trace_start_tick);
	return result;
}

extern "C" void GlideMetalShutdown(void)
{
	int width = (int)glide_width;
	int height = (int)glide_height;
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalGlideMetalShutdown();
	SDLGPUTransitionTraceEnd("glide-shutdown", kGfxEngineGlide, width, height,
		trace_start_tick);
}

extern "C" int GlideMetalWinOpen(int width, int height, int origin_upper_left)
{
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	int result = SDLGPUOriginalGlideMetalWinOpen(width, height,
		origin_upper_left);
	SDLGPUTransitionTraceEnd("glide-window-open", kGfxEngineGlide, width,
		height, trace_start_tick);
	return result;
}

extern "C" void GlideMetalWinClose(void)
{
	int width = (int)glide_width;
	int height = (int)glide_height;
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalGlideMetalWinClose();
	SDLGPUTransitionTraceEnd("glide-window-close", kGfxEngineGlide, width,
		height, trace_start_tick);
}
#endif
