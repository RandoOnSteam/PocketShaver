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

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define vbl_source_sdl_tick SDLGPUOriginalVBLSourceSDLTick
#define vbl_source_sync_3d_pacing_for_engine SDLGPUOriginalVBLSync3DPacingForEngine
#define vbl_source_sync_3d_pacing SDLGPUOriginalVBLSync3DPacing
#endif
#include "../gl/vbl_source_sdl.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef vbl_source_sync_3d_pacing
#undef vbl_source_sync_3d_pacing_for_engine
#undef vbl_source_sdl_tick

extern "C" void vbl_source_sdl_tick(double target_ts)
{
	Uint64 start_tick = SDL_GetPerformanceCounter();
	SDLGPUOriginalVBLSourceSDLTick(target_ts);
	SDLGPUTransitionTraceRecordVBLCallback(start_tick);
}

extern "C" int32_t vbl_source_sync_3d_pacing_for_engine(int32_t engine_id)
{
	Uint64 start_tick = SDL_GetPerformanceCounter();
	int32_t result = SDLGPUOriginalVBLSync3DPacingForEngine(engine_id);
	SDLGPUTransitionTraceRecordPacing(start_tick);
	return result;
}

extern "C" int32_t vbl_source_sync_3d_pacing(void)
{
	return vbl_source_sync_3d_pacing_for_engine(0);
}
#endif
