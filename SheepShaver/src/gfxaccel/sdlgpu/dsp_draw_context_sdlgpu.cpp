#include "sysdeps.h"
#include "video.h"

extern uint32 SDLGPUCreateBackdropWindow(uint32 width, uint32 height);
extern bool SDLGPUDisposeBackdropWindow(uint32 window);

#define video_create_guest_fullscreen_window SDLGPUCreateBackdropWindow
#define video_dispose_guest_window SDLGPUDisposeBackdropWindow
#include "../dsp_draw_context.cpp"
#undef video_dispose_guest_window
#undef video_create_guest_fullscreen_window

uint32 SDLGPUCreateBackdropWindow(uint32 width, uint32 height)
{
	return video_create_guest_fullscreen_window(width, height);
}

bool SDLGPUDisposeBackdropWindow(uint32 window)
{
	return video_dispose_guest_window(window);
}
