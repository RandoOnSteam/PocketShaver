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
#define NativeDrawPrivateNew SDLGPUOriginalRaveDrawPrivateNew
#define NativeDrawPrivateDelete SDLGPUOriginalRaveDrawPrivateDelete
#endif
#include "../rave_draw_context.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef NativeDrawPrivateDelete
#undef NativeDrawPrivateNew

int32 NativeDrawPrivateNew(uint32 draw_context, uint32 device,
	uint32 rect, uint32 clip, uint32 flags)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	int32 result = SDLGPUOriginalRaveDrawPrivateNew(draw_context, device, rect,
		clip, flags);
	SDLGPUTransitionTraceEnd("rave-context-create", kGfxEngineRAVE, 0, 0,
		start_tick);
	return result;
}

int32 NativeDrawPrivateDelete(uint32 draw_private)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	int32 result = SDLGPUOriginalRaveDrawPrivateDelete(draw_private);
	SDLGPUTransitionTraceEnd("rave-context-destroy", kGfxEngineRAVE, 0, 0,
		start_tick);
	return result;
}
#endif
