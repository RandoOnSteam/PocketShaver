#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <SDL.h>
#include <SDL_thread.h>
#ifdef main
#undef main
#endif

typedef Uint8 uint8;
typedef Sint32 int32;
#include "emulator_monitor.h"

void ADBMouseMoved(int horizontal, int vertical)
{
	printf("mouse %d %d\n", horizontal, vertical);
}

void ADBMouseDown(int button)
{
	printf("down %d\n", button);
}

void ADBMouseUp(int button)
{
	printf("up %d\n", button);
}

void ADBKeyDown(int code)
{
	printf("keydown %d\n", code);
}

void ADBKeyUp(int code)
{
	printf("keyup %d\n", code);
}

const char *PrefsFindString(const char *name, int index)
{
	return NULL;
}

bool PrefsFindBool(const char *name)
{
	return false;
}

int32 PrefsFindInt32(const char *name)
{
	return 0;
}

const char *GetString(int number)
{
	return "Tests";
}

int main(int argc, char **argv)
{
	EmulatorMonitor monitor;
	EmulatorMonitorView view;
	SDL_Event event;
	Uint32 started;
	Uint32 runtime;
	bool running;

	setvbuf(stdout, NULL, _IONBF, 0);
	runtime = 30000;
	if (argc > 1)
		runtime = (Uint32)strtoul(argv[1], NULL, 10);
#if SDL_VERSION_ATLEAST(2, 0, 0) && !SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_SetMainReady();
#endif
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if (!SDL_Init(SDL_INIT_VIDEO))
#else
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
#endif
		return 2;
	memset(&view, 0, sizeof(view));
	view.width = 16;
	view.height = 16;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	view.window = SDL_CreateWindow("Monitor tests", 16, 16, SDL_WINDOW_HIDDEN);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	view.window = SDL_CreateWindow("Monitor tests", 0, 0, 16, 16, SDL_WINDOW_HIDDEN);
#else
	view.hostsurface = SDL_SetVideoMode(16, 16, 32, SDL_SWSURFACE);
#endif
	if (!monitor.Start())
		return 3;
	puts("ready");
	started = SDL_GetTicks();
	running = true;
	while (running && SDL_GetTicks() - started < runtime) {
		while (SDL_PollEvent(&event)) {
			if (!monitor.HandleEvent(event, view)) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
				if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
#else
				if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
#endif
					running = false;
			}
		}
		SDL_Delay(1);
	}
	puts("stopping");
	monitor.Stop();
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_DestroyWindow((SDL_Window *)view.window);
#endif
	SDL_Quit();
	puts("stopped");
	return 0;
}
