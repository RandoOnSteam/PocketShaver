#include "sdlgpu_transition_trace.h"
#include "../include/gfxaccel_resources.h"

#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define GlideStateBeginPresentation SDLGPUOriginalGlideStateBeginPresentation
#define GlideStateEndPresentation SDLGPUOriginalGlideStateEndPresentation
#endif
#include "../glide_state.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef GlideStateEndPresentation
#undef GlideStateBeginPresentation

bool GlideStateBeginPresentation(int width, int height)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	bool result = SDLGPUOriginalGlideStateBeginPresentation(width, height);
	SDLGPUTransitionTraceEnd("glide-mode-enter", kGfxEngineGlide, width,
		height, start_tick);
	return result;
}

void GlideStateEndPresentation(void)
{
	Uint64 start_tick = SDLGPUTransitionTraceBegin();
	SDLGPUOriginalGlideStateEndPresentation();
	SDLGPUTransitionTraceEnd("glide-mode-exit", kGfxEngineGlide, 0, 0,
		start_tick);
}
#endif
