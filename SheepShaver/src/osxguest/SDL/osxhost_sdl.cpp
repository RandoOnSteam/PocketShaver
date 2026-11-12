/*
 *  osxhost_sdl.cpp - SDL host video, input, audio and clipboard for the Mac OS X guest
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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
#include "sysdeps.h"
#include "my_sdl.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include "osxhost.h"
#include "osxmachine.h"
#include "bincue.h"
#ifdef ENABLE_EMULATOR_MONITOR
#include "emulator_monitor.h"
#endif

SDL_Surface *SDLSetVideoModeQuietly(int width, int height, int depth, Uint32 flags);
#if SDL_VERSION_ATLEAST(2, 0, 0)
extern SDL_Window *sdl_window;
#define OSXVIDEOFLAGS 0
#else
#define OSXVIDEOFLAGS SDL_SWSURFACE
#endif
typedef struct OsxKeyMapping {
	int sdlkey;
	int adbcode;
} OsxKeyMapping;

struct OsxHostVideo {
	SDL_Surface *screen;
	int captured;
	int width;
	int height;
	int rowbytes;
	OsxHostInput input;
	void *context;
	uint8 *capture;
#ifdef ENABLE_EMULATOR_MONITOR
	EmulatorMonitor *monitor;
#endif
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_Surface *window;
	SDL_Surface *staging;
#endif
};

#if SDL_VERSION_ATLEAST(2, 0, 0) /* SDL2+ -> SDL1 names we use */
	#define SDLK_KP0 SDLK_KP_0
	#define SDLK_KP1 SDLK_KP_1
	#define SDLK_KP2 SDLK_KP_2
	#define SDLK_KP3 SDLK_KP_3
	#define SDLK_KP4 SDLK_KP_4
	#define SDLK_KP5 SDLK_KP_5
	#define SDLK_KP6 SDLK_KP_6
	#define SDLK_KP7 SDLK_KP_7
	#define SDLK_KP8 SDLK_KP_8
	#define SDLK_KP9 SDLK_KP_9
	#define SDLK_LSUPER SDLK_LGUI
	#define SDLK_RSUPER SDLK_RGUI
#endif /* SDL_VERSION_ATLEAST(2, 0, 0) */
#if SDL_VERSION_ATLEAST(3, 0, 0)
#define OSXSDLSUCCESS true
#define OSXEVENTQUIT SDL_EVENT_QUIT
#define OSXEVENTMOTION SDL_EVENT_MOUSE_MOTION
#define OSXEVENTBUTTONDOWN SDL_EVENT_MOUSE_BUTTON_DOWN
#define OSXEVENTBUTTONUP SDL_EVENT_MOUSE_BUTTON_UP
#define OSXEVENTWHEEL SDL_EVENT_MOUSE_WHEEL
#define OSXEVENTWINDOW SDL_EVENT_WINDOW_EXPOSED
#define OSXEVENTKEYDOWN SDL_EVENT_KEY_DOWN
#define OSXEVENTKEYUP SDL_EVENT_KEY_UP
#define OSXEVENTKEY(event) ((event).key.key)
#else
#define OSXSDLSUCCESS 0
#define OSXEVENTQUIT SDL_QUIT
#define OSXEVENTMOTION SDL_MOUSEMOTION
#define OSXEVENTBUTTONDOWN SDL_MOUSEBUTTONDOWN
#define OSXEVENTBUTTONUP SDL_MOUSEBUTTONUP
#define OSXEVENTWHEEL SDL_MOUSEWHEEL
#define OSXEVENTWINDOW SDL_WINDOWEVENT
#define OSXEVENTKEYDOWN SDL_KEYDOWN
#define OSXEVENTKEYUP SDL_KEYUP
#define OSXEVENTKEY(event) ((event).key.keysym.sym)
#endif
static int OsxGuestMapKey(int key)
{
	static const OsxKeyMapping mappings[] = {
		{ 'a', 0x00 }, { 's', 0x01 }, { 'd', 0x02 }, { 'f', 0x03 }, { 'h', 0x04 },
		{ 'g', 0x05 }, { 'z', 0x06 }, { 'x', 0x07 }, { 'c', 0x08 }, { 'v', 0x09 },
		{ 'b', 0x0b }, { 'q', 0x0c }, { 'w', 0x0d }, { 'e', 0x0e }, { 'r', 0x0f },
		{ 'y', 0x10 }, { 't', 0x11 }, { SDLK_1, 0x12 }, { SDLK_2, 0x13 }, { SDLK_3, 0x14 },
		{ SDLK_4, 0x15 }, { SDLK_6, 0x16 }, { SDLK_5, 0x17 }, { SDLK_EQUALS, 0x18 }, { SDLK_9, 0x19 },
		{ SDLK_7, 0x1a }, { SDLK_MINUS, 0x1b }, { SDLK_8, 0x1c }, { SDLK_0, 0x1d }, { SDLK_RIGHTBRACKET, 0x1e },
		{ 'o', 0x1f }, { 'u', 0x20 }, { SDLK_LEFTBRACKET, 0x21 }, { 'i', 0x22 }, { 'p', 0x23 },
		{ SDLK_RETURN, 0x24 }, { 'l', 0x25 }, { 'j', 0x26 }, { '\'', 0x27 }, { 'k', 0x28 },
		{ SDLK_SEMICOLON, 0x29 }, { SDLK_BACKSLASH, 0x2a }, { SDLK_COMMA, 0x2b }, { SDLK_SLASH, 0x2c },
		{ 'n', 0x2d }, { 'm', 0x2e }, { SDLK_PERIOD, 0x2f }, { SDLK_TAB, 0x30 }, { SDLK_SPACE, 0x31 },
		{ '`', 0x32 }, { SDLK_BACKSPACE, 0x33 }, { SDLK_ESCAPE, 0x35 }, { SDLK_LCTRL, 0x36 },
		{ SDLK_RCTRL, 0x36 }, { SDLK_LALT, 0x37 }, { SDLK_RALT, 0x37 }, { SDLK_LSHIFT, 0x38 },
		{ SDLK_RSHIFT, 0x38 }, { SDLK_CAPSLOCK, 0x39 }, { SDLK_LSUPER, 0x3a }, { SDLK_RSUPER, 0x3a },
		{ SDLK_LEFT, 0x3b }, { SDLK_RIGHT, 0x3c }, { SDLK_DOWN, 0x3d }, { SDLK_UP, 0x3e },
		{ SDLK_KP_PERIOD, 0x41 }, { SDLK_KP_MULTIPLY, 0x43 }, { SDLK_KP_PLUS, 0x45 }, { SDLK_KP_DIVIDE, 0x4b },
		{ SDLK_KP_ENTER, 0x4c }, { SDLK_KP_MINUS, 0x4e }, { SDLK_KP_EQUALS, 0x51 }, { SDLK_KP0, 0x52 },
		{ SDLK_KP1, 0x53 }, { SDLK_KP2, 0x54 }, { SDLK_KP3, 0x55 }, { SDLK_KP4, 0x56 }, { SDLK_KP5, 0x57 },
		{ SDLK_KP6, 0x58 }, { SDLK_KP7, 0x59 }, { SDLK_KP8, 0x5b }, { SDLK_KP9, 0x5c }, { SDLK_F5, 0x60 },
		{ SDLK_F6, 0x61 }, { SDLK_F7, 0x62 }, { SDLK_F3, 0x63 }, { SDLK_F8, 0x64 }, { SDLK_F9, 0x65 },
		{ SDLK_F11, 0x67 }, { SDLK_F10, 0x6d }, { SDLK_F12, 0x6f }, { SDLK_HOME, 0x73 }, { SDLK_PAGEUP, 0x74 },
		{ SDLK_DELETE, 0x75 }, { SDLK_F4, 0x76 }, { SDLK_END, 0x77 }, { SDLK_F2, 0x78 }, { SDLK_PAGEDOWN, 0x79 },
		{ SDLK_F1, 0x7a }
	};
	int index;
	for (index = 0; index < (int)(sizeof(mappings) / sizeof(mappings[0])); index++)
		if (mappings[index].sdlkey == key)
			return mappings[index].adbcode;
	return -1;
}

static int OsxGuestHostButton(int button)
{
	if (button == SDL_BUTTON_LEFT)
		return 0;
	if (button == SDL_BUTTON_RIGHT)
		return 1;
	if (button == SDL_BUTTON_MIDDLE)
		return 2;
#if !SDL_VERSION_ATLEAST(2, 0, 0)
	if (button == SDL_BUTTON_WHEELUP)
		return 3;
	if (button == SDL_BUTTON_WHEELDOWN)
		return 4;
#endif
	return -1;
}
#if SDL_VERSION_ATLEAST(2, 0, 0)
static int OsxGuestHostWheelButton(int wheely)
{
	if (wheely > 0)
		return 3; /* up */
	if (wheely < 0)
		return 4; /* down */
	return -1;
}
#endif
#ifdef ENABLE_EMULATOR_MONITOR
static void OsxGuestMonitorInput(void *context, int operation, int first, int second)
{
	OsxHostVideo *video = (OsxHostVideo *)context;
	switch (operation) {
	case EMULATOR_MONITOR_MOUSE:
		video->input(video->context, OSXHOSTMOUSE, first, second);
		break;
	case EMULATOR_MONITOR_MOUSEDOWN:
		video->input(video->context, OSXHOSTBUTTON, first, 1);
		break;
	case EMULATOR_MONITOR_MOUSEUP:
		video->input(video->context, OSXHOSTBUTTON, first, 0);
		break;
	case EMULATOR_MONITOR_WHEEL:
		video->input(video->context, OSXHOSTWHEEL, first, 0);
		break;
	case EMULATOR_MONITOR_KEYDOWN:
		video->input(video->context, OSXHOSTKEY, first, 1);
		break;
	case EMULATOR_MONITOR_KEYUP:
		video->input(video->context, OSXHOSTKEY, first, 0);
		break;
	}
}
#endif

OsxHostVideo *OsxGuestHostOpen(int width, int height, OsxHostInput input, void *context)
{
	OsxHostVideo *video;
	if (SDL_Init(SDL_INIT_VIDEO) != OSXSDLSUCCESS)
		return NULL;
	video = new OsxHostVideo;
	memset(video, 0, sizeof(*video));
	video->width = width;
	video->height = height;
	video->rowbytes = width * 4;
	video->input = input;
	video->context = context;
#if !SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_WM_SetCaption("SheepShaver - Mac OS X", "SheepShaver");
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
#endif
	video->screen = SDLSetVideoModeQuietly(width, height, 32, OSXVIDEOFLAGS);
	if (!video->screen) {
		OsxGuestHostClose(video);
		return NULL;
	}
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetWindowTitle(sdl_window, "SheepShaver - Mac OS X");
#endif
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (video->screen != SDL_GetWindowSurface(sdl_window))
		video->staging = video->screen;
	SDL_HideCursor();
#else
	SDL_ShowCursor(SDL_DISABLE);
#endif
#ifdef ENABLE_EMULATOR_MONITOR
	video->monitor = new EmulatorMonitor();
	if (!video->monitor->Start()) {
		delete video->monitor;
		video->monitor = NULL;
	}
#endif
	return video;
}

void OsxGuestHostSuspendFrame(OsxHostVideo *video, uint8 *present)
{
	video->capture = present;
}

void OsxGuestHostCapturePointer(OsxHostVideo *video, int capture)
{
	video->captured = capture;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_SetWindowRelativeMouseMode(sdl_window, capture != 0);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	if (capture)
		SDL_SetRelativeMouseMode(SDL_TRUE);
	else
		SDL_SetRelativeMouseMode(SDL_FALSE);
#else
	if (capture)
		SDL_WM_GrabInput(SDL_GRAB_ON);
	else
		SDL_WM_GrabInput(SDL_GRAB_OFF);
#endif
}

void OsxGuestHostResize(OsxHostVideo *video, int width, int height)
{
	video->width = width;
	video->height = height;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (video->staging)
		SDL_DestroySurface(video->staging);
	video->staging = NULL;
#else
	video->screen = SDLSetVideoModeQuietly(width, height, 32, OSXVIDEOFLAGS);
#endif
}

void OsxGuestHostClose(OsxHostVideo *video)
{
#ifdef ENABLE_EMULATOR_MONITOR
	delete video->monitor;
#endif
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (video->staging)
		SDL_DestroySurface(video->staging);
#endif
	SDL_Quit();
	delete video;
}

uint8 *OsxGuestHostLock(OsxHostVideo *video, int *pitch, int *raw)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	video->window = SDL_GetWindowSurface(sdl_window);
	if (!video->window)
		return NULL;
	if (video->window->w == video->width && video->window->h == video->height)
		video->screen = video->window;
	else {
		if (!video->staging)
			video->staging = SDL_CreateSurface(video->width, video->height, SDL_PIXELFORMAT_XRGB8888);
		video->screen = video->staging;
	}
#endif
	if (!video->screen || video->screen->w < video->width || video->screen->h < video->height ||
	    SDL_LockSurface(video->screen) != OSXSDLSUCCESS)
		return NULL;
	*pitch = video->screen->pitch;
	*raw = 0;
	return (uint8 *)video->screen->pixels;
}

#if SDL_VERSION_ATLEAST(2, 0, 0)
static void OsxGuestSdlRestoreContext(void)
{
	SDL_GLContext context;
	SDL_Window *window;
	context = SDL_GL_GetCurrentContext();
	window = SDL_GL_GetCurrentWindow();
	if (context) {
		SDL_GL_MakeCurrent(window, NULL);
		SDL_GL_MakeCurrent(window, context);
	}
}
#endif

int OsxGuestHostPresent(OsxHostVideo *video, const uint32 *runstart, const uint32 *runend, int runs)
{
	int index;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Rect rects[OSXHOSTRUNS];
#endif
	SDL_UnlockSurface(video->screen);
	if (!runs)
		return 0;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	OsxGuestSdlRestoreContext();
#endif
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (video->window != video->screen)
		return SDL_BlitSurfaceScaled(video->screen, NULL, video->window, NULL, SDL_SCALEMODE_LINEAR) &&
		       SDL_UpdateWindowSurface(sdl_window);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	for (index = 0; index < runs; index++) {
		rects[index].x = 0;
		rects[index].y = (int)runstart[index];
		rects[index].w = video->width;
		rects[index].h = (int)(runend[index] - runstart[index] + 1);
	}
	return SDL_UpdateWindowSurfaceRects(sdl_window, rects, runs) == OSXSDLSUCCESS;
#else
	for (index = 0; index < runs; index++)
		SDL_UpdateRect(video->screen, 0, runstart[index], video->width, runend[index] - runstart[index] + 1);
	return 1;
#endif
}

void OsxGuestHostSleep(uint32 milliseconds)
{
	SDL_Delay(milliseconds);
}

void OsxGuestHostEvents(OsxHostVideo *video, const uint8 *vram)
{
	OsxHostInput input = video->input;
	void *context = video->context;
	SDL_Event event;
	int code;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int width;
	int height;
#endif
	while (SDL_PollEvent(&event)) {
#ifdef ENABLE_EMULATOR_MONITOR
		if (video->monitor) {
			EmulatorMonitorView view;
			view.inputfunction = OsxGuestMonitorInput;
			view.inputcontext = video;
			view.framebuffer = (uint8 *)vram;
			if (video->capture)
				view.framebuffer = video->capture;
			view.palette = NULL;
			view.window = NULL;
			view.width = (int)video->width;
			view.height = (int)video->height;
			view.depth = 32;
			view.rowbytes = (int)video->rowbytes;
			view.hostsurface = video->screen;
			view.hostwidth = view.width;
			view.hostheight = view.height;
			if (video->monitor->HandleEvent(event, view))
				continue;
		}
#endif
		switch (event.type) {
		case OSXEVENTQUIT:
			input(context, OSXHOSTQUIT, 0, 0);
			break;
		case OSXEVENTMOTION:
			if (video->captured) {
				input(context, OSXHOSTMOTION, (int)event.motion.xrel, (int)event.motion.yrel);
				break;
			}
#if SDL_VERSION_ATLEAST(3, 0, 0)
			SDL_GetWindowSize(sdl_window, &width, &height);
			event.motion.x = event.motion.x * video->width / width;
			event.motion.y = event.motion.y * video->height / height;
#endif
			input(context, OSXHOSTMOUSE, (int)event.motion.x, (int)event.motion.y);
			break;
		case OSXEVENTBUTTONDOWN:
			input(context, OSXHOSTBUTTON, OsxGuestHostButton(event.button.button), 1);
			break;
		case OSXEVENTBUTTONUP:
			input(context, OSXHOSTBUTTON, OsxGuestHostButton(event.button.button), 0);
			break;
		#if SDL_VERSION_ATLEAST(2, 0, 0)
		case OSXEVENTWHEEL:
			input(context, OSXHOSTBUTTON, OsxGuestHostWheelButton((int)event.wheel.y), 1);
			break;
		#if SDL_VERSION_ATLEAST(3, 0, 0)
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_RESTORED:
		#endif
		case OSXEVENTWINDOW:
#if !SDL_VERSION_ATLEAST(3, 0, 0)
			video->screen = SDL_GetWindowSurface(sdl_window);
#endif
			input(context, OSXHOSTREDRAW, 0, 0);
			break;
		#endif
		case OSXEVENTKEYDOWN:
			code = OsxGuestMapKey(OSXEVENTKEY(event));
			if (code >= 0)
				input(context, OSXHOSTKEY, code, 1);
			break;
		case OSXEVENTKEYUP:
			code = OsxGuestMapKey(OSXEVENTKEY(event));
			if (code >= 0)
				input(context, OSXHOSTKEY, code, 0);
			break;
		}
	}
}

void *EtherSlirpOpen(void);
int EtherSlirpReady(void *thread);
void EtherSlirpPoll(void);
int EtherSlirpReceive(uint8 *packet);
void EtherSlirpSend(const uint8 *packet, int length);

void *OsxGuestHostNetOpen(const uint8 *address)
{
	return EtherSlirpOpen();
}

void OsxGuestHostNetClose(void *network)
{
}

int OsxGuestHostNetReady(void *network)
{
	return EtherSlirpReady(network);
}

void OsxGuestHostNetPoll(void *network)
{
	EtherSlirpPoll();
}

int OsxGuestHostNetReceive(void *network, uint8 *packet)
{
	return EtherSlirpReceive(packet);
}

void OsxGuestHostNetSend(void *network, const uint8 *packet, int length)
{
	EtherSlirpSend(packet, length);
}

#define AUDIOFRAMEBYTES 4
#define AUDIOMARGINDIVISOR 200
#define AUDIOCDVOLUME 128
#define AUDIOCDBLOCK 8192

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void SDLCALL OsxGuestAudioCallback(void *context, SDL_AudioStream *stream, int length, int total)
{
	OsxAudio *audio;
	uint32 count;
	uint32 offset;
	uint32 first;
	audio = (OsxAudio *)context;
	if (length <= 0)
		return;
	count = audio->head - audio->tail;
	if (HaveAudioToMix_bincue()) {
		unsigned char mixed[AUDIOCDBLOCK];
		int total;
		total = length;
		if (total > (int)sizeof(mixed))
			total = (int)sizeof(mixed);
		total &= ~3;
		if (count > (uint32)total)
			count = (uint32)total;
		offset = audio->tail & (OSXAUDIORING - 1);
		first = OSXAUDIORING - offset;
		if (first > count)
			first = count;
		memcpy(mixed, audio->ring + offset, first);
		memcpy(mixed + first, audio->ring, count - first);
		memset(mixed + count, 0, total - count);
		audio->tail += count;
		MixAudio_bincue(mixed, total);
		SDL_PutAudioStreamData(stream, mixed, total);
		return;
	}
	if (count > (uint32)length + audio->rate / AUDIOMARGINDIVISOR * AUDIOFRAMEBYTES)
		count = (uint32)length + audio->rate / AUDIOMARGINDIVISOR * AUDIOFRAMEBYTES;
	offset = audio->tail & (OSXAUDIORING - 1);
	first = OSXAUDIORING - offset;
	if (first > count)
		first = count;
	SDL_PutAudioStreamData(stream, audio->ring + offset, (int)first);
	if (count > first)
		SDL_PutAudioStreamData(stream, audio->ring, (int)(count - first));
	audio->tail += count;
}

void OsxGuestAudioLockHost(OsxAudio *audio)
{
	SDL_LockAudioStream((SDL_AudioStream *)audio->stream);
}

void OsxGuestAudioUnlockHost(OsxAudio *audio)
{
	SDL_UnlockAudioStream((SDL_AudioStream *)audio->stream);
}

void OsxGuestAudioCloseHost(OsxAudio *audio)
{
	if (audio->stream) {
		SDL_DestroyAudioStream((SDL_AudioStream *)audio->stream);
		CloseAudio_bincue();
	}
	audio->stream = NULL;
}

void *OsxGuestAudioStartHost(OsxAudio *audio)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *stream;
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		return NULL;
	spec.freq = (int)audio->rate;
	spec.format = SDL_AUDIO_S16BE;
	spec.channels = 2;
	OpenAudio_bincue(spec.freq, spec.format, spec.channels, 0, AUDIOCDVOLUME);
	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, OsxGuestAudioCallback, audio);
	if (stream)
		SDL_ResumeAudioStreamDevice(stream);
	return stream;
}
#else
static void OsxGuestAudioCallback(void *context, Uint8 *stream, int length)
{
	OsxAudio *audio;
	uint32 available;
	uint32 count;
	uint32 index;
	audio = (OsxAudio *)context;
	available = audio->head - audio->tail;
	count = (uint32)length;
	if (count > available)
		count = available;
	for (index = 0; index < count; index++)
		stream[index] = audio->ring[(audio->tail + index) & (OSXAUDIORING - 1)];
	audio->tail += count;
	memset(stream + count, 0, (uint32)length - count);
	MixAudio_bincue(stream, length);
}

void OsxGuestAudioLockHost(OsxAudio *audio)
{
	SDL_LockAudio();
}

void OsxGuestAudioUnlockHost(OsxAudio *audio)
{
	SDL_UnlockAudio();
}

void OsxGuestAudioCloseHost(OsxAudio *audio)
{
	if (audio->stream) {
		SDL_CloseAudio();
		CloseAudio_bincue();
	}
	audio->stream = NULL;
}

void *OsxGuestAudioStartHost(OsxAudio *audio)
{
	SDL_AudioSpec spec;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return NULL;
	spec.freq = (int)audio->rate;
	spec.format = AUDIO_S16MSB;
	spec.channels = 2;
	spec.samples = 1024;
	spec.callback = OsxGuestAudioCallback;
	spec.userdata = audio;
	OpenAudio_bincue(spec.freq, spec.format, spec.channels, 0, AUDIOCDVOLUME);
	if (SDL_OpenAudio(&spec, NULL) < 0)
		return NULL;
	SDL_PauseAudio(0);
	return audio;
}
#endif

char *OsxGuestHostClipboardGet(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	char *text;
	char *copy;
	if (!SDL_HasClipboardText())
		return NULL;
	text = SDL_GetClipboardText();
	if (text == NULL)
		return NULL;
	copy = strdup(text);
	SDL_free(text);
	return copy;
#elif defined(_WIN32)
	char *data;
	HANDLE handle;
	const wchar_t *wide;
	char *text;
	int size;
	data = NULL;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
		return NULL;
	handle = GetClipboardData(CF_UNICODETEXT);
	wide = NULL;
	if (handle)
		wide = (const wchar_t *)GlobalLock(handle);
	if (wide) {
		size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
		text = (char *)malloc(size + 1);
		if (text && size > 0 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, size, NULL, NULL) > 0)
			data = text;
		else
			free(text);
		GlobalUnlock(handle);
	}
	CloseClipboard();
	return data;
#else
	return NULL;
#endif
}

void OsxGuestHostClipboardSet(const char *text)
{
#if !SDL_VERSION_ATLEAST(2, 0, 0) && defined(_WIN32)
	HGLOBAL handle;
	wchar_t *wide;
	int size;
	int index;
	int used;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetClipboardText(text);
#elif defined(_WIN32)
	size = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	handle = GlobalAlloc(GMEM_MOVEABLE, (size_t)size * 2 * sizeof(wchar_t));
	if (handle && OpenClipboard(NULL)) {
		wide = (wchar_t *)GlobalLock(handle);
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide + size, size);
		used = 0;
		for (index = 0; index < size; index++) {
			if (wide[size + index] == L'\n')
				wide[used++] = L'\r';
			wide[used++] = wide[size + index];
		}
		GlobalUnlock(handle);
		EmptyClipboard();
		if (!SetClipboardData(CF_UNICODETEXT, handle))
			GlobalFree(handle);
		CloseClipboard();
	} else if (handle)
		GlobalFree(handle);
#endif
}
