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

static void *SDLGPUVendRaveOverlayTexture(uint32_t engine_id,
	uint32_t texture_index, uint32_t width, uint32_t height,
	uint32_t pixel_format);

#define gfxaccel_resources_vend_overlay_texture_indexed SDLGPUVendRaveOverlayTexture
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define RaveCreateMetalOverlay SDLGPUOriginalRaveCreateMetalOverlay
#define RaveInitMetalResources SDLGPUOriginalRaveInitMetalResources
#define RaveReleaseMetalResources SDLGPUOriginalRaveReleaseMetalResources
#define rave_release_overlay_for_detach SDLGPUOriginalRaveReleaseOverlayForDetach
#endif
#include "../gl/rave_gl_renderer.cpp"
#undef gfxaccel_resources_vend_overlay_texture_indexed

static void *SDLGPUVendRaveOverlayTexture(uint32_t engine_id,
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
#undef rave_release_overlay_for_detach
#undef RaveReleaseMetalResources
#undef RaveInitMetalResources
#undef RaveCreateMetalOverlay

void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width,
	int32_t height)
{
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalRaveCreateMetalOverlay(left, top, width, height);
	SDLGPUTransitionTraceEnd("rave-overlay-create", kGfxEngineRAVE, width,
		height, trace_start_tick);
}

void RaveInitMetalResources(RaveDrawPrivate *priv)
{
	int width = 0;
	int height = 0;
	if (priv) {
		width = priv->width;
		height = priv->height;
	}
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalRaveInitMetalResources(priv);
	SDLGPUTransitionTraceEnd("rave-resource-init", kGfxEngineRAVE, width,
		height, trace_start_tick);
}

void RaveReleaseMetalResources(RaveDrawPrivate *priv)
{
	int width = 0;
	int height = 0;
	if (priv) {
		width = priv->width;
		height = priv->height;
	}
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalRaveReleaseMetalResources(priv);
	SDLGPUTransitionTraceEnd("rave-resource-release", kGfxEngineRAVE, width,
		height, trace_start_tick);
}

extern "C" void rave_release_overlay_for_detach(void)
{
	int width = (int)s_ow;
	int height = (int)s_oh;
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalRaveReleaseOverlayForDetach();
	SDLGPUTransitionTraceEnd("rave-overlay-detach", kGfxEngineRAVE, width,
		height, trace_start_tick);
}
#endif
