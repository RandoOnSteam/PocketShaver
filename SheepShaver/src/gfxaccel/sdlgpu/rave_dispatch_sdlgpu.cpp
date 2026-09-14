#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define RaveDispatch SDLGPUOriginalRaveDispatch
#endif
#include "../rave_dispatch.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef RaveDispatch

uint32 RaveDispatch(uint32 r3, uint32 r4, uint32 r5, uint32 r6,
	uint32 r7, uint32 r8)
{
	Uint32 opcode = ReadMacInt32(rave_scratch_addr);
	Uint64 start_tick = SDLGPUTransitionTraceDispatchBegin(kGfxEngineRAVE,
		opcode);
	uint32 result = SDLGPUOriginalRaveDispatch(r3, r4, r5, r6, r7, r8);
	SDLGPUTransitionTraceRecordDispatch(kGfxEngineRAVE, opcode, start_tick);
	return result;
}
#endif
