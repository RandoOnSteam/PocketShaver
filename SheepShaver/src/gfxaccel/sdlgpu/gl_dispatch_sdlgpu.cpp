#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define GLDispatch SDLGPUOriginalGLDispatch
#endif
#include "../gl_dispatch.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef GLDispatch

uint32_t GLDispatch(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
	uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
	const uint32_t *float_bits, int num_float_args)
{
	Uint32 opcode = ReadMacInt32(gl_scratch_addr);
	Uint64 start_tick = SDLGPUTransitionTraceDispatchBegin(kGfxEngineGL,
		opcode);
	uint32_t result = SDLGPUOriginalGLDispatch(r3, r4, r5, r6, r7, r8,
		r9, r10, float_bits, num_float_args);
	SDLGPUTransitionTraceRecordDispatch(kGfxEngineGL, opcode, start_tick);
	return result;
}
#endif
