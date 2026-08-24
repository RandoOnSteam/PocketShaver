#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define DSpDispatch SDLGPUOriginalDSpDispatch
#endif
#include "../dsp_dispatch.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef DSpDispatch

extern "C" uint32_t DSpDispatch(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
	uint32_t r7, uint32_t r8)
{
	Uint32 opcode = ReadMacInt32(dsp_scratch_addr);
	Uint64 start_tick = SDLGPUTransitionTraceDispatchBegin(kGfxEngineDSp,
		opcode);
	uint32_t result = SDLGPUOriginalDSpDispatch(r3, r4, r5, r6, r7, r8);
	SDLGPUTransitionTraceRecordDispatch(kGfxEngineDSp, opcode, start_tick);
	return result;
}
#endif
