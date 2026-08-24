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
#ifndef GFXACCEL_SDLGPU_TRANSITION_TRACE_H
#define GFXACCEL_SDLGPU_TRANSITION_TRACE_H

#ifndef SDLGPU_TRANSITION_LOGGING_ENABLED
#define SDLGPU_TRANSITION_LOGGING_ENABLED 0
#endif

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

Uint64 SDLGPUTransitionTraceBegin(void);
void SDLGPUTransitionTraceEnd(const char *event_name, Uint32 engine_id,
	int width, int height, Uint64 start_tick);
void SDLGPUTransitionTraceRecordPacing(Uint64 start_tick);
void SDLGPUTransitionTraceRecordVBLCallback(Uint64 start_tick);
void SDLGPUTransitionTraceRecordVideoVBL(Uint64 start_tick, Uint64 nqd_ticks,
	Uint64 compositor_ticks, Uint64 lock_ticks, Uint64 service_ticks);
void SDLGPUTransitionTraceRecordRedraw(Uint64 start_tick);
void SDLGPUTransitionTraceRecordGuestSample(Uint32 pc, Uint32 pc68k,
	Uint32 level68k, Uint32 run_mode, Uint32 irq_nest, Uint32 irq_flags,
	int tick_inhibited);
Uint64 SDLGPUTransitionTraceDispatchBegin(Uint32 engine_id, Uint32 opcode);
void SDLGPUTransitionTraceRecordDispatch(Uint32 engine_id, Uint32 opcode,
	Uint64 start_tick);

#ifdef __cplusplus
}
#endif
#endif

#endif
