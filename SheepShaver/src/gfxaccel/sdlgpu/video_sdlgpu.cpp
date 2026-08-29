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

#include "my_sdl.h"
#include <cmath>
#include "dsp_pixmap_offsets.h"
#include "sdlgpu_transition_trace.h"

static SDL_Surface *s_sdlgpu_video_renderer_surface = NULL;

extern "C" void SDLGPUReleaseVideoWindow(SDL_Window *window);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#define XLM_RUN_MODE 0x2810
#define XLM_IRQ_NEST 0x2818

#ifndef USE_CPU_EMUL_SERVICES
static int SDLGPUProfiledRedraw(void *arg);
#endif
static SDL_Thread *SDLGPUCreateThread(SDL_ThreadFunction function,
	const char *name, void *data);
#endif

static SDL_Renderer *SDLGPUCreateVideoRenderer(SDL_Window *window, const char *)
{
	if (!window)
		return NULL;
	int width = 0;
	int height = 0;
	if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0)
		return NULL;
	s_sdlgpu_video_renderer_surface = SDL_CreateSurface(width, height,
		SDL_PIXELFORMAT_ARGB8888);
	if (!s_sdlgpu_video_renderer_surface)
		return NULL;
	SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(s_sdlgpu_video_renderer_surface);
	if (!renderer) {
		SDL_DestroySurface(s_sdlgpu_video_renderer_surface);
		s_sdlgpu_video_renderer_surface = NULL;
	}
	return renderer;
}

static void SDLGPUDestroyVideoRenderer(SDL_Renderer *renderer)
{
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (s_sdlgpu_video_renderer_surface) {
		SDL_DestroySurface(s_sdlgpu_video_renderer_surface);
		s_sdlgpu_video_renderer_surface = NULL;
	}
}

static void SDLGPUDestroyVideoWindow(SDL_Window *window)
{
	SDLGPUReleaseVideoWindow(window);
	SDL_DestroyWindow(window);
}

static bool SDLGPUConvertEventCoordinates(SDL_Renderer *renderer, SDL_Event *event)
{
	if (!renderer || !event || event->type != SDL_EVENT_MOUSE_MOTION)
		return true;
	SDL_Window *window = SDL_GetWindowFromID(event->motion.windowID);
	if (!window)
		return false;
	int window_width = 0;
	int window_height = 0;
	int logical_width = 0;
	int logical_height = 0;
	SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
	if (!SDL_GetWindowSize(window, &window_width, &window_height) ||
		!SDL_GetRenderLogicalPresentation(renderer, &logical_width, &logical_height,
			&mode) || window_width <= 0 || window_height <= 0 ||
		logical_width <= 0 || logical_height <= 0)
		return false;
	float scale_x = (float)window_width / (float)logical_width;
	float scale_y = (float)window_height / (float)logical_height;
	float scale = scale_x;
	if (scale_y < scale)
		scale = scale_y;
	if (mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE && scale >= 1.0f)
		scale = std::floor(scale);
	if (scale <= 0.0f)
		return false;
	float output_width = (float)logical_width * scale;
	float output_height = (float)logical_height * scale;
	float offset_x = ((float)window_width - output_width) * 0.5f;
	float offset_y = ((float)window_height - output_height) * 0.5f;
	event->motion.x = (event->motion.x - offset_x) / scale;
	event->motion.y = (event->motion.y - offset_y) / scale;
	return true;
}

#undef SDL_WINDOW_OPENGL
#undef SDL_WINDOW_METAL
#define SDL_WINDOW_OPENGL 0
#define SDL_WINDOW_METAL 0
#define SDL_CreateRenderer SDLGPUCreateVideoRenderer
#define SDL_DestroyRenderer SDLGPUDestroyVideoRenderer
#define SDL_DestroyWindow SDLGPUDestroyVideoWindow
#define SDL_ConvertEventToRenderCoordinates SDLGPUConvertEventCoordinates
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef SDL_CreateThread
#define SDL_CreateThread SDLGPUCreateThread
#define VideoVBL SDLGPUOriginalVideoVBL
#endif
#include "../../../../BasiliskII/src/SDL/video_sdl3.cpp"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#undef VideoVBL
#undef SDL_CreateThread
#endif
#undef SDL_ConvertEventToRenderCoordinates
#undef SDL_DestroyWindow
#undef SDL_DestroyRenderer
#undef SDL_CreateRenderer
#undef SDL_WINDOW_METAL
#undef SDL_WINDOW_OPENGL

static uint32 SDLGPUVideoModePixelDepth(uint32 apple_mode)
{
	switch (apple_mode) {
		case APPLE_1_BIT:
			return 1;
		case APPLE_2_BIT:
			return 2;
		case APPLE_4_BIT:
			return 4;
		case APPLE_8_BIT:
			return 8;
		case APPLE_16_BIT:
			return 16;
		case APPLE_32_BIT:
			return 32;
	}
	return 0;
}

static void SDLGPUWriteGuestDisplayMode(int mode_index, uint32 gdevice,
	uint32 pixmap)
{
	const VideoInfo &mode = VModes[mode_index];
	const uint32 depth = SDLGPUVideoModePixelDepth(mode.viAppleMode);
	uint16 pixel_type = 0;
	uint16 component_count = 1;
	uint16 component_size = (uint16)depth;
	if (depth == 16) {
		pixel_type = 0x10;
		component_count = 3;
		component_size = 5;
	} else if (depth == 32) {
		pixel_type = 0x10;
		component_count = 3;
		component_size = 8;
	}

	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR, screen_base);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES,
		(uint16)(0x8000u | mode.viRowBytes));
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP, 0);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT, 0);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT,
		mode.viYsize);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT,
		mode.viXsize);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PMVERSION, 0);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PACKTYPE, 0);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PACKSIZE, 0);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_HRES, 0x00480000u);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_VRES, 0x00480000u);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE, pixel_type);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE,
		(uint16)depth);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT,
		component_count);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE,
		component_size);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES, 0);

	WriteMacInt16(gdevice + GDEVICE_OFF_GDRECT + 0, 0);
	WriteMacInt16(gdevice + GDEVICE_OFF_GDRECT + 2, 0);
	WriteMacInt16(gdevice + GDEVICE_OFF_GDRECT + 4, mode.viYsize);
	WriteMacInt16(gdevice + GDEVICE_OFF_GDRECT + 6, mode.viXsize);
	WriteMacInt32(gdevice + GDEVICE_OFF_GDMODE,
		video_rel_depth_from_abs(mode.viAppleMode));
	WriteMacInt16(0x0102, 72);
	WriteMacInt16(0x0104, 72);
	WriteMacInt16(0x0106, (uint16)mode.viRowBytes);
	WriteMacInt32(0x0824, screen_base);
	WriteMacInt32(0x0898, screen_base);
	if (depth <= 8)
		video_set_palette();
}

bool SDLGPUVideoSwitchGuestDisplay(int mode_index)
{
	if (mode_index < 0 || mode_index >= 64 ||
		VModes[mode_index].viType == DIS_INVALID ||
		SDLGPUVideoModePixelDepth(VModes[mode_index].viAppleMode) == 0) {
		return false;
	}

	const int previous_mode = cur_mode;
	if (video_switch_to_mode_index(mode_index) != noErr)
		return false;

	const uint32 main_device_handle = ReadMacInt32(LMADDR_MAIN_DEVICE);
	const uint32 pixmap = video_get_live_main_device_pixmap();
	if (main_device_handle == 0 || main_device_handle == 0xffffffffu ||
		pixmap == 0) {
		if (video_switch_guest_display(mode_index))
			return true;
		(void)video_switch_to_mode_index(previous_mode);
		return false;
	}

	const uint32 gdevice = ReadMacInt32(main_device_handle);
	if (gdevice == 0 || gdevice == 0xffffffffu) {
		if (video_switch_guest_display(mode_index))
			return true;
		(void)video_switch_to_mode_index(previous_mode);
		return false;
	}

	SDLGPUWriteGuestDisplayMode(mode_index, gdevice, pixmap);
	return true;
}

#if SDLGPU_TRANSITION_LOGGING_ENABLED
extern Uint32 PPCSampleGuestPC(void);
extern Uint32 PPCSampleGuestGPR(int index);
extern bool tick_inhibit;

#ifndef USE_CPU_EMUL_SERVICES
static int SDLGPUProfiledRedraw(void *arg)
{
	(void)arg;
	uint64 start = GetTicks_usec();
	uint64 next = start + VIDEO_REFRESH_DELAY;
	while (!redraw_thread_cancel) {
		next += VIDEO_REFRESH_DELAY;
		int32 delay = int32(next - GetTicks_usec());
		if (delay > 0)
			Delay_usec(delay);
		else if (delay < -VIDEO_REFRESH_DELAY)
			next = GetTicks_usec();
		int tick_is_inhibited = 0;
		if (tick_inhibit)
			tick_is_inhibited = 1;
		SDLGPUTransitionTraceRecordGuestSample(PPCSampleGuestPC(),
			PPCSampleGuestGPR(24), PPCSampleGuestGPR(25),
			ReadMacInt32(XLM_RUN_MODE), ReadMacInt32(XLM_IRQ_NEST),
			(Uint32)InterruptFlags, tick_is_inhibited);
		if (thread_stop_req) {
			thread_stop_ack = true;
			continue;
		}
		Uint64 refresh_start = SDL_GetPerformanceCounter();
		do_video_refresh();
		SDLGPUTransitionTraceRecordRedraw(refresh_start);
	}
	return 0;
}
#endif

static SDL_Thread *SDLGPUCreateThread(SDL_ThreadFunction function,
	const char *name, void *data)
{
	SDL_ThreadFunction entry = function;
#ifndef USE_CPU_EMUL_SERVICES
	if (function == redraw_func)
		entry = SDLGPUProfiledRedraw;
#endif
	return SDL_CreateThreadRuntime(entry, name, data,
		(SDL_FunctionPointer)SDL_BeginThreadFunction,
		(SDL_FunctionPointer)SDL_EndThreadFunction);
}

void VideoVBL(void)
{
	Uint64 start_tick = SDL_GetPerformanceCounter();
	Uint64 nqd_ticks = 0;
	Uint64 compositor_ticks = 0;
	Uint64 lock_ticks = 0;
	Uint64 service_ticks = 0;
	Uint64 section_tick;
	if (emerg_quit)
		QuitEmulator();
#ifdef VIDEO_CHROMAKEY
	if (display_type == DISPLAY_CHROMAKEY)
		make_window_transparent(sdl_window);
	else
#endif
	if (toggle_fullscreen)
		do_toggle_fullscreen();
	if (nqd_metal_available) {
		section_tick = SDL_GetPerformanceCounter();
		NQDMetalFlush();
		nqd_ticks = SDL_GetPerformanceCounter() - section_tick;
	}
	section_tick = SDL_GetPerformanceCounter();
	if (MetalCompositorIsInitialized())
		MetalCompositorPresent();
	else {
		present_sdl_video_windowsurface();
	}
	compositor_ticks = SDL_GetPerformanceCounter() - section_tick;
	section_tick = SDL_GetPerformanceCounter();
	UNLOCK_FRAME_BUFFER;
	LOCK_FRAME_BUFFER;
	lock_ticks = SDL_GetPerformanceCounter() - section_tick;
	if (private_data != NULL && private_data->interruptsEnabled) {
		section_tick = SDL_GetPerformanceCounter();
		VSLDoInterruptService(private_data->vslServiceID);
		service_ticks = SDL_GetPerformanceCounter() - section_tick;
	}
	SDLGPUTransitionTraceRecordVideoVBL(start_tick, nqd_ticks,
		compositor_ticks, lock_ticks, service_ticks);
}
#endif
