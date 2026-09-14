#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define GlideDispatch SDLGPUOriginalGlideDispatch
#endif
#include "../glide_dispatch.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef GlideDispatch

extern "C" uint32_t GlideDispatch(uint32_t r3, uint32_t r4, uint32_t r5,
	uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
	uint32_t sp, double f1, double f2, double f3, double f4)
{
	Uint32 opcode = ReadMacInt32(glide_scratch_addr);
	Uint64 start_tick = SDLGPUTransitionTraceDispatchBegin(kGfxEngineGlide,
		opcode);
	uint32_t result = SDLGPUOriginalGlideDispatch(r3, r4, r5, r6, r7,
		r8, r9, r10, sp, f1, f2, f3, f4);
	SDLGPUTransitionTraceRecordDispatch(kGfxEngineGlide, opcode, start_tick);
	return result;
}
#endif
