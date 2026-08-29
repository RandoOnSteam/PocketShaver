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
#include "display_mode_controller.h"

static int32_t SDLGPUCompositorSubscribe(const struct DMCSubscriber *subscriber);
static int32_t SDLGPUCompositorModeEnter(const struct DMCModeSnapshot *incoming,
	void *context);
static DMCEnterFn s_sdlgpu_compositor_mode_enter = NULL;
static bool s_sdlgpu_compositor_subscribing = false;

#define dmc_subscribe SDLGPUCompositorSubscribe
#include "../gl/gl_compositor.cpp"
#undef dmc_subscribe

static int32_t SDLGPUCompositorSubscribe(const struct DMCSubscriber *subscriber)
{
	if (!subscriber)
		return dmc_subscribe(subscriber);
	struct DMCSubscriber wrapped = *subscriber;
	s_sdlgpu_compositor_mode_enter = subscriber->on_mode_enter;
	wrapped.on_mode_enter = SDLGPUCompositorModeEnter;
	s_sdlgpu_compositor_subscribing = true;
	int32_t result = dmc_subscribe(&wrapped);
	s_sdlgpu_compositor_subscribing = false;
	return result;
}

static int32_t SDLGPUCompositorModeEnter(const struct DMCModeSnapshot *incoming,
	void *context)
{
	if (s_sdlgpu_compositor_subscribing || !s_sdlgpu_compositor_mode_enter)
		return 0;
	return s_sdlgpu_compositor_mode_enter(incoming, context);
}

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
extern "C" double MetalCompositorWindowedContentInsetTop() {
	return 0.0;
}
extern "C" void MetalCompositorReapplyWindowPinning() {
}
#endif
