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
#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define NativeAGLChoosePixelFormat SDLGPUOriginalAGLChoosePixelFormat
#define NativeAGLCreateContext SDLGPUOriginalAGLCreateContext
#define NativeAGLSetCurrentContext SDLGPUOriginalAGLSetCurrentContext
#define NativeAGLSetDrawable SDLGPUOriginalAGLSetDrawable
#define NativeAGLDestroyContext SDLGPUOriginalAGLDestroyContext
#define NativeAGLDestroyPixelFormat SDLGPUOriginalAGLDestroyPixelFormat
#define NativeAGLSetOffScreen SDLGPUOriginalAGLSetOffScreen
#define NativeAGLSetFullScreen SDLGPUOriginalAGLSetFullScreen
#define NativeAGLResetLibrary SDLGPUOriginalAGLResetLibrary
#endif
#include "../gl_engine.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef NativeAGLResetLibrary
#undef NativeAGLSetFullScreen
#undef NativeAGLSetOffScreen
#undef NativeAGLDestroyPixelFormat
#undef NativeAGLDestroyContext
#undef NativeAGLSetDrawable
#undef NativeAGLSetCurrentContext
#undef NativeAGLCreateContext
#undef NativeAGLChoosePixelFormat

uint32_t NativeAGLChoosePixelFormat(uint32_t gdevs, uint32_t ndev,
	uint32_t attribs)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLChoosePixelFormat(gdevs, ndev, attribs);
	SDLGPUTransitionTraceEnd("agl-choose-pixel-format", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}

uint32_t NativeAGLCreateContext(uint32_t pixel_format, uint32_t share_context)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLCreateContext(pixel_format, share_context);
	SDLGPUTransitionTraceEnd("agl-create-context", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}

uint32_t NativeAGLSetCurrentContext(uint32_t context)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLSetCurrentContext(context);
	SDLGPUTransitionTraceEnd("agl-set-current-context", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}

uint32_t NativeAGLSetDrawable(uint32_t context, uint32_t drawable)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLSetDrawable(context, drawable);
	const char *event_name = "agl-bind-drawable";
	if (drawable == 0)
		event_name = "agl-unbind-drawable";
	SDLGPUTransitionTraceEnd(event_name, kGfxEngineGL, 0, 0, start_tick);
	return result;
}

uint32_t NativeAGLDestroyContext(uint32_t context)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLDestroyContext(context);
	SDLGPUTransitionTraceEnd("agl-destroy-context", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}

uint32_t NativeAGLDestroyPixelFormat(uint32_t pixel_format)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLDestroyPixelFormat(pixel_format);
	SDLGPUTransitionTraceEnd("agl-destroy-pixel-format", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}

uint32_t NativeAGLSetOffScreen(uint32_t context, uint32_t width,
	uint32_t height, uint32_t row_bytes, uint32_t base_address)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLSetOffScreen(context, width, height,
		row_bytes, base_address);
	SDLGPUTransitionTraceEnd("agl-set-offscreen", kGfxEngineGL, (int)width,
		(int)height, start_tick);
	return result;
}

uint32_t NativeAGLSetFullScreen(uint32_t context, uint32_t width,
	uint32_t height, uint32_t frequency, uint32_t device)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLSetFullScreen(context, width, height,
		frequency, device);
	SDLGPUTransitionTraceEnd("agl-set-fullscreen", kGfxEngineGL, (int)width,
		(int)height, start_tick);
	return result;
}

uint32_t NativeAGLResetLibrary()
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	uint32_t result = SDLGPUOriginalAGLResetLibrary();
	SDLGPUTransitionTraceEnd("agl-reset-library", kGfxEngineGL, 0, 0,
		start_tick);
	return result;
}
#endif
