/*
 *  emulator_monitor.h - Provides the ability to manipulate guest outside
 *	of the emulator
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

#ifndef EMULATOR_MONITOR_H
#define EMULATOR_MONITOR_H

#include "atomic.h"
#include "adb.h"
#include "prefs.h"
#include "user_strings.h"
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#if !SDL_VERSION_ATLEAST(3, 0, 0)
#if defined(USE_SDL2)
#include <SDL2/SDL_syswm.h>
#else
#include <SDL_syswm.h>
#endif
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define EMULATOR_MONITOR_STATUS 1
#define EMULATOR_MONITOR_SHOT 2
#define EMULATOR_MONITOR_MOUSE 3
#define EMULATOR_MONITOR_MOUSEDOWN 4
#define EMULATOR_MONITOR_MOUSEUP 5
#define EMULATOR_MONITOR_KEYDOWN 6
#define EMULATOR_MONITOR_KEYUP 7
#define EMULATOR_MONITOR_POWER 8
#define EMULATOR_MONITOR_HOSTSHOT 9
#define EMULATOR_MONITOR_HOSTMOUSE 10
#define EMULATOR_MONITOR_HOSTMOUSEDOWN 11
#define EMULATOR_MONITOR_HOSTMOUSEUP 12
#define EMULATOR_MONITOR_HOTKEYFULLSCREEN 13
#define EMULATOR_MONITOR_HOTKEYGRAB 14
#define EMULATOR_MONITOR_HOTKEYQUIT 15
#define EMULATOR_MONITOR_PORT 19840
#define EMULATOR_MONITOR_PRESS_MS 60
#define EMULATOR_MONITOR_SETTLE_MS 50
#define EMULATOR_MONITOR_TYPE_MS 30
#define EMULATOR_MONITOR_DRAG_STEPS 10
#define EMULATOR_MONITOR_DRAG_STEP_MS 20
#define EMULATOR_MONITOR_ADB_SHIFT 0x38
#if SDL_VERSION_ATLEAST(3, 0, 0)
#define EMULATOR_MONITOR_BUTTON_MASK(BUTTON) SDL_BUTTON_MASK(BUTTON)
#else
#define EMULATOR_MONITOR_BUTTON_MASK(BUTTON) SDL_BUTTON(BUTTON)
#endif

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifdef _WIN32
typedef SOCKET EmulatorMonitorSocket;
#define EMULATOR_MONITOR_INVALID_SOCKET INVALID_SOCKET
#else
typedef int EmulatorMonitorSocket;
#define EMULATOR_MONITOR_INVALID_SOCKET (-1)
#endif

struct EmulatorMonitorView {
	const Uint8 *framebuffer;
	const SDL_Color *palette;
	SDL_Surface *hostsurface;
	void *window;
	int width;
	int height;
	int depth;
	int rowbytes;
	int hostwidth;
	int hostheight;
};

class EmulatorMonitor {
public:
	EmulatorMonitor()
		: listensocket(EMULATOR_MONITOR_INVALID_SOCKET),
		  clientsocket(EMULATOR_MONITOR_INVALID_SOCKET), thread(NULL),
		  cancelrequested(0), eventtype((Uint32)-1), windowsocketsready(false),
		  hostmousex(0), hostmousey(0), hostbuttons(0)
	{
		extfsname[0] = 0;
	}

	~EmulatorMonitor()
	{
		Stop();
	}

	bool Start()
	{
		struct sockaddr_in address;
		const char *volumename;
		int enabled;

		volumename = GetString(STR_EXTFS_VOLUME_NAME);
		if (volumename == NULL)
			volumename = "";
		snprintf(extfsname, sizeof(extfsname), "%s", volumename);

#if SDL_VERSION_ATLEAST(2, 0, 0)
		eventtype = SDL_RegisterEvents(1);
		if (eventtype == 0 || eventtype == (Uint32)-1)
			return false;
#else
		eventtype = SDL_USEREVENT;
		SDL_EventState((Uint8)eventtype, SDL_ENABLE);
#endif
#ifdef _WIN32
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			return false;
		windowsocketsready = true;
#endif
		listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listensocket == EMULATOR_MONITOR_INVALID_SOCKET) {
			Stop();
			return false;
		}
		enabled = 1;
		setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR,
			(const char *)&enabled, sizeof(enabled));
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
#if TARGET_OS_IPHONE
		address.sin_addr.s_addr = htonl(INADDR_ANY);
#else
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
		address.sin_port = htons(EMULATOR_MONITOR_PORT);
		if (bind(listensocket, (struct sockaddr *)&address,
			sizeof(address)) != 0) {
			Stop();
			return false;
		}
		if (listen(listensocket, 1) != 0) {
			Stop();
			return false;
		}
		atomic_store_explicit(&cancelrequested, 0, memory_order_release);
#if SDL_VERSION_ATLEAST(2, 0, 0)
		thread = SDL_CreateThread(ThreadEntry, "emumonitor", this);
#else
		thread = SDL_CreateThread(ThreadEntry, this);
#endif
		if (thread == NULL) {
			Stop();
			return false;
		}
		return true;
	}

	void Stop()
	{
		atomic_store_explicit(&cancelrequested, 1, memory_order_release);
		CloseSocket(listensocket);
		listensocket = EMULATOR_MONITOR_INVALID_SOCKET;
		CloseSocket(clientsocket);
		clientsocket = EMULATOR_MONITOR_INVALID_SOCKET;
		if (thread != NULL) {
			SDL_WaitThread(thread, NULL);
			thread = NULL;
		}
#ifdef _WIN32
		if (windowsocketsready) {
			WSACleanup();
			windowsocketsready = false;
		}
#endif
	}

	bool HandleEvent(SDL_Event &event, const EmulatorMonitorView &view)
	{
		Request *request;

		if (event.type != eventtype)
			return false;
		request = (Request *)event.user.data1;
		if (request == NULL)
			return true;
		request->response[0] = 0;
		switch (request->operation) {
			case EMULATOR_MONITOR_STATUS:
				snprintf(request->response, sizeof(request->response),
					"{\"ok\":true,\"width\":%d,\"height\":%d,\"depth\":%d,\"hostwidth\":%d,\"hostheight\":%d}",
					view.width, view.height, view.depth, view.hostwidth, view.hostheight);
				break;
			case EMULATOR_MONITOR_SHOT:
				SaveFramebuffer(view, request);
				break;
			case EMULATOR_MONITOR_HOSTSHOT:
				SaveHostSurface(view, request);
				break;
			case EMULATOR_MONITOR_MOUSE:
				ADBMouseMoved(request->first, request->second);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_MOUSEDOWN:
				ADBMouseDown(request->first);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_MOUSEUP:
				ADBMouseUp(request->first);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_KEYDOWN:
				ADBKeyDown(request->first);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_KEYUP:
				ADBKeyUp(request->first);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_POWER:
				ADBKeyDown(0x7f);
				ADBKeyUp(0x7f);
				SetOk(request);
				break;
			case EMULATOR_MONITOR_HOSTMOUSE:
				PushHostMotion(view, request->first, request->second, request);
				break;
			case EMULATOR_MONITOR_HOSTMOUSEDOWN:
				PushHostButton(view, request->first, true, request);
				break;
			case EMULATOR_MONITOR_HOSTMOUSEUP:
				PushHostButton(view, request->first, false, request);
				break;
			case EMULATOR_MONITOR_HOTKEYFULLSCREEN:
#if SDL_VERSION_ATLEAST(3, 0, 0)
				PushHotkey(view, SDLK_RETURN, SDL_SCANCODE_RETURN, request);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
				PushHotkey(view, SDLK_RETURN, SDL_SCANCODE_RETURN, request);
#else
				PushHotkey(view, SDLK_RETURN, 0, request);
#endif
				break;
			case EMULATOR_MONITOR_HOTKEYGRAB:
#if SDL_VERSION_ATLEAST(2, 0, 0)
				PushHotkey(view, SDLK_F5, SDL_SCANCODE_F5, request);
#else
				PushHotkey(view, SDLK_F5, 0, request);
#endif
				break;
			case EMULATOR_MONITOR_HOTKEYQUIT:
#if SDL_VERSION_ATLEAST(2, 0, 0)
				PushHotkey(view, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, request);
#else
				PushHotkey(view, SDLK_ESCAPE, 0, request);
#endif
				break;
			default:
				SetError(request, "unknown operation");
				break;
		}
		atomic_store_explicit(&request->done, 1, memory_order_release);
		return true;
	}

private:
	struct Request {
		int operation;
		int first;
		int second;
		char text[768];
		char response[1024];
		atomic_sint done;
	};

	static int ThreadEntry(void *context)
	{
		EmulatorMonitor *monitor;
		monitor = (EmulatorMonitor *)context;
		return monitor->Run();
	}

	int Run()
	{
		while (!atomic_load_explicit(&cancelrequested, memory_order_acquire)) {
			clientsocket = accept(listensocket, NULL, NULL);
			if (clientsocket == EMULATOR_MONITOR_INVALID_SOCKET)
				break;
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
			int enabled;
			enabled = 1;
			setsockopt(clientsocket, SOL_SOCKET, SO_NOSIGPIPE,
				(const char *)&enabled, sizeof(enabled));
#endif
			Serve(clientsocket);
			CloseSocket(clientsocket);
			clientsocket = EMULATOR_MONITOR_INVALID_SOCKET;
		}
		return 0;
	}

	void Serve(EmulatorMonitorSocket clientsocket)
	{
		char command[1024];
		char response[4096];
		int received;
		int sendflags;
		int used;

		used = 0;
		while (used < (int)sizeof(command) - 1) {
			received = recv(clientsocket, command + used,
				(int)sizeof(command) - used - 1, 0);
			if (received <= 0)
				break;
			used += received;
			command[used] = 0;
			if (strchr(command, '\n') != NULL)
				break;
		}
		command[used] = 0;
		Trim(command);
		Execute(command, response, sizeof(response));
		strncat(response, "\n", sizeof(response) - strlen(response) - 1);
		sendflags = 0;
#ifdef MSG_NOSIGNAL
		sendflags = MSG_NOSIGNAL;
#endif
		send(clientsocket, response, (int)strlen(response), sendflags);
	}

	void Execute(const char *command, char *response, size_t responsesize)
	{
		int first;
		int second;
		int third;
		int fourth;
		int buttonvalue;
		int holdtime;
		int fieldcount;
		char direction[16];
		const char *text;

		if (strcmp(command, "ping") == 0) {
#ifdef _WIN32
			unsigned long processid = (unsigned long)GetCurrentProcessId();
#else
			unsigned long processid = (unsigned long)getpid();
#endif
#ifdef SHEEPSHAVER
			snprintf(response, responsesize,
				"{\"ok\":true,\"monitor\":\"PocketShaver\",\"port\":%d,\"pid\":%lu}",
				EMULATOR_MONITOR_PORT, processid);
#else
			snprintf(response, responsesize,
				"{\"ok\":true,\"monitor\":\"BasiliskII\",\"port\":%d,\"pid\":%lu}",
				EMULATOR_MONITOR_PORT, processid);
#endif
			return;
		}
		if (strcmp(command, "help") == 0) {
			snprintf(response, responsesize,
				"{\"ok\":true,\"commands\":[\"ping\",\"status\",\"extfs\",\"disks\",\"shot PATH\",\"mouse X Y\",\"mousedown BUTTON\",\"mouseup BUTTON\",\"click X Y [BUTTON]\",\"dblclick X Y [BUTTON]\",\"drag X1 Y1 X2 Y2 [BUTTON [HOLDMS]]\",\"hold X Y MS [BUTTON]\",\"key down CODE\",\"key up CODE\",\"key tap CODE\",\"type TEXT\",\"power\",\"host shot PATH\",\"host mouse X Y\",\"host mousedown BUTTON\",\"host mouseup BUTTON\",\"host click X Y [BUTTON]\",\"hotkey fullscreen\",\"hotkey grab\",\"quit\"]}");
			return;
		}
		if (strcmp(command, "extfs") == 0) {
			WriteExtFSRoot(response, responsesize);
			return;
		}
		if (strcmp(command, "disks") == 0) {
			WriteDisks(extfsname, response, responsesize);
			return;
		}
		if (strcmp(command, "status") == 0) {
			Dispatch(EMULATOR_MONITOR_STATUS, 0, 0, NULL, response, responsesize);
			return;
		}
		if (strncmp(command, "shot ", 5) == 0) {
			ExecuteShot(EMULATOR_MONITOR_SHOT, command + 5, response, responsesize);
			return;
		}
		if (strncmp(command, "host shot ", 10) == 0) {
			ExecuteShot(EMULATOR_MONITOR_HOSTSHOT, command + 10, response, responsesize);
			return;
		}
		if (sscanf(command, "host mouse %d %d", &first, &second) == 2) {
			Dispatch(EMULATOR_MONITOR_HOSTMOUSE, first, second, NULL, response, responsesize);
			return;
		}
		if (sscanf(command, "host mousedown %d", &first) == 1) {
			if (CheckButton(first, response, responsesize))
				Dispatch(EMULATOR_MONITOR_HOSTMOUSEDOWN, first, 0, NULL, response, responsesize);
			return;
		}
		if (sscanf(command, "host mouseup %d", &first) == 1) {
			if (CheckButton(first, response, responsesize))
				Dispatch(EMULATOR_MONITOR_HOSTMOUSEUP, first, 0, NULL, response, responsesize);
			return;
		}
		fieldcount = sscanf(command, "host click %d %d %d", &first, &second, &buttonvalue);
		if (fieldcount >= 2) {
			if (fieldcount == 2)
				buttonvalue = 0;
			if (CheckButton(buttonvalue, response, responsesize) &&
				Dispatch(EMULATOR_MONITOR_HOSTMOUSE, first, second, NULL, response, responsesize) &&
				Settle() &&
				Dispatch(EMULATOR_MONITOR_HOSTMOUSEDOWN, buttonvalue, 0, NULL, response, responsesize)) {
				SDL_Delay(EMULATOR_MONITOR_PRESS_MS);
				Dispatch(EMULATOR_MONITOR_HOSTMOUSEUP, buttonvalue, 0, NULL, response, responsesize);
			}
			return;
		}
		if (strcmp(command, "hotkey fullscreen") == 0) {
			Dispatch(EMULATOR_MONITOR_HOTKEYFULLSCREEN, 0, 0, NULL, response, responsesize);
			return;
		}
		if (strcmp(command, "quit") == 0) {
			Dispatch(EMULATOR_MONITOR_HOTKEYQUIT, 0, 0, NULL, response, responsesize);
			return;
		}
		if (strcmp(command, "hotkey grab") == 0) {
			Dispatch(EMULATOR_MONITOR_HOTKEYGRAB, 0, 0, NULL, response, responsesize);
			return;
		}
		if (sscanf(command, "mouse %d %d", &first, &second) == 2) {
			Dispatch(EMULATOR_MONITOR_MOUSE, first, second, NULL, response, responsesize);
			return;
		}
		if (sscanf(command, "mousedown %d", &first) == 1) {
			if (CheckButton(first, response, responsesize))
				Dispatch(EMULATOR_MONITOR_MOUSEDOWN, first, 0, NULL, response, responsesize);
			return;
		}
		if (sscanf(command, "mouseup %d", &first) == 1) {
			if (CheckButton(first, response, responsesize))
				Dispatch(EMULATOR_MONITOR_MOUSEUP, first, 0, NULL, response, responsesize);
			return;
		}
		fieldcount = sscanf(command, "click %d %d %d", &first, &second, &buttonvalue);
		if (fieldcount >= 2) {
			if (fieldcount == 2)
				buttonvalue = 0;
			if (CheckButton(buttonvalue, response, responsesize) &&
				Dispatch(EMULATOR_MONITOR_MOUSE, first, second, NULL, response, responsesize) &&
				Settle())
				Click(buttonvalue, response, responsesize);
			return;
		}
		fieldcount = sscanf(command, "dblclick %d %d %d", &first, &second, &buttonvalue);
		if (fieldcount >= 2) {
			if (fieldcount == 2)
				buttonvalue = 0;
			if (CheckButton(buttonvalue, response, responsesize) &&
				Dispatch(EMULATOR_MONITOR_MOUSE, first, second, NULL, response, responsesize) &&
				Settle() &&
				Click(buttonvalue, response, responsesize)) {
				SDL_Delay(EMULATOR_MONITOR_PRESS_MS);
				Click(buttonvalue, response, responsesize);
			}
			return;
		}
		fieldcount = sscanf(command, "drag %d %d %d %d %d %d", &first, &second, &third, &fourth, &buttonvalue, &holdtime);
		if (fieldcount >= 4) {
			if (fieldcount < 5)
				buttonvalue = 0;
			if (fieldcount < 6)
				holdtime = EMULATOR_MONITOR_PRESS_MS;
			if (CheckButton(buttonvalue, response, responsesize))
				Drag(first, second, third, fourth, buttonvalue, holdtime, response, responsesize);
			return;
		}
		fieldcount = sscanf(command, "hold %d %d %d %d", &first, &second, &holdtime, &buttonvalue);
		if (fieldcount >= 3) {
			if (fieldcount == 3)
				buttonvalue = 0;
			if (CheckButton(buttonvalue, response, responsesize) &&
				Dispatch(EMULATOR_MONITOR_MOUSE, first, second, NULL, response, responsesize) &&
				Settle() &&
				Dispatch(EMULATOR_MONITOR_MOUSEDOWN, buttonvalue, 0, NULL, response, responsesize)) {
				SDL_Delay(holdtime);
				Dispatch(EMULATOR_MONITOR_MOUSEUP, buttonvalue, 0, NULL, response, responsesize);
			}
			return;
		}
		if (strncmp(command, "type ", 5) == 0) {
			TypeText(command + 5, response, responsesize);
			return;
		}
		if (sscanf(command, "key %15s %i", direction, &first) == 2) {
			if (first < 0 || first > 0x7f) {
				WriteError(response, responsesize, "key code must be 0 through 127");
				return;
			}
			if (strcmp(direction, "down") == 0) {
				Dispatch(EMULATOR_MONITOR_KEYDOWN, first, 0, NULL, response, responsesize);
				return;
			}
			if (strcmp(direction, "up") == 0) {
				Dispatch(EMULATOR_MONITOR_KEYUP, first, 0, NULL, response, responsesize);
				return;
			}
			if (strcmp(direction, "tap") == 0) {
				if (Dispatch(EMULATOR_MONITOR_KEYDOWN, first, 0, NULL, response, responsesize)) {
					SDL_Delay(EMULATOR_MONITOR_PRESS_MS);
					Dispatch(EMULATOR_MONITOR_KEYUP, first, 0, NULL, response, responsesize);
				}
				return;
			}
		}
		if (strcmp(command, "power") == 0) {
			Dispatch(EMULATOR_MONITOR_POWER, 0, 0, NULL, response, responsesize);
			return;
		}
		WriteError(response, responsesize, "unknown command");
	}

	void ExecuteShot(int operation, const char *text, char *response, size_t responsesize)
	{
		while (*text == ' ')
			text++;
		if (*text == 0) {
			WriteError(response, responsesize, "missing path");
			return;
		}
		Dispatch(operation, 0, 0, text, response, responsesize);
	}

	bool Click(int button, char *response, size_t responsesize)
	{
		if (!Dispatch(EMULATOR_MONITOR_MOUSEDOWN, button, 0, NULL, response, responsesize))
			return false;
		SDL_Delay(EMULATOR_MONITOR_PRESS_MS);
		return Dispatch(EMULATOR_MONITOR_MOUSEUP, button, 0, NULL, response, responsesize);
	}

	void Drag(int startx, int starty, int endx, int endy, int button, int holdtime,
		char *response, size_t responsesize)
	{
		if (!Dispatch(EMULATOR_MONITOR_MOUSE, startx, starty, NULL, response, responsesize) ||
			!Settle() ||
			!Dispatch(EMULATOR_MONITOR_MOUSEDOWN, button, 0, NULL, response, responsesize))
			return;
		SDL_Delay(EMULATOR_MONITOR_PRESS_MS);
		for (int step = 1; step <= EMULATOR_MONITOR_DRAG_STEPS; step++) {
			if (!Dispatch(EMULATOR_MONITOR_MOUSE,
				startx + (endx - startx) * step / EMULATOR_MONITOR_DRAG_STEPS,
				starty + (endy - starty) * step / EMULATOR_MONITOR_DRAG_STEPS,
				NULL, response, responsesize))
				return;
			SDL_Delay(EMULATOR_MONITOR_DRAG_STEP_MS);
		}
		Settle();
		SDL_Delay(holdtime);
		Dispatch(EMULATOR_MONITOR_MOUSEUP, button, 0, NULL, response, responsesize);
	}

	static bool Settle()
	{
		SDL_Delay(EMULATOR_MONITOR_SETTLE_MS);
		return true;
	}

	void TypeText(const char *text, char *response, size_t responsesize)
	{
		int code;
		bool shifted;
		char character;

		SetOkResponse(response, responsesize);
		for (int index = 0; text[index] != 0; index++) {
			character = text[index];
			if (character == '\\' && text[index + 1] == 'n') {
				character = '\n';
				index++;
			} else if (character == '\\' && text[index + 1] == 't') {
				character = '\t';
				index++;
			} else if (character == '\\' && text[index + 1] == '\\')
				index++;
			if (!TextKeyCode(character, code, shifted)) {
				WriteError(response, responsesize, "unsupported character");
				return;
			}
			if (shifted && !Dispatch(EMULATOR_MONITOR_KEYDOWN, EMULATOR_MONITOR_ADB_SHIFT, 0, NULL, response, responsesize))
				return;
			if (!Dispatch(EMULATOR_MONITOR_KEYDOWN, code, 0, NULL, response, responsesize))
				return;
			SDL_Delay(EMULATOR_MONITOR_TYPE_MS);
			if (!Dispatch(EMULATOR_MONITOR_KEYUP, code, 0, NULL, response, responsesize))
				return;
			if (shifted && !Dispatch(EMULATOR_MONITOR_KEYUP, EMULATOR_MONITOR_ADB_SHIFT, 0, NULL, response, responsesize))
				return;
			SDL_Delay(EMULATOR_MONITOR_TYPE_MS);
		}
	}

	static bool TextKeyCode(char character, int &code, bool &shifted)
	{
		static const struct {
			char plain;
			char shifted;
			unsigned char code;
		} keys[] = {
			{'a', 'A', 0x00}, {'s', 'S', 0x01}, {'d', 'D', 0x02}, {'f', 'F', 0x03},
			{'h', 'H', 0x04}, {'g', 'G', 0x05}, {'z', 'Z', 0x06}, {'x', 'X', 0x07},
			{'c', 'C', 0x08}, {'v', 'V', 0x09}, {'`', '~', 0x0a}, {'b', 'B', 0x0b},
			{'q', 'Q', 0x0c}, {'w', 'W', 0x0d}, {'e', 'E', 0x0e}, {'r', 'R', 0x0f},
			{'y', 'Y', 0x10}, {'t', 'T', 0x11}, {'1', '!', 0x12}, {'2', '@', 0x13},
			{'3', '#', 0x14}, {'4', '$', 0x15}, {'6', '^', 0x16}, {'5', '%', 0x17},
			{'=', '+', 0x18}, {'9', '(', 0x19}, {'7', '&', 0x1a}, {'-', '_', 0x1b},
			{'8', '*', 0x1c}, {'0', ')', 0x1d}, {']', '}', 0x1e}, {'o', 'O', 0x1f},
			{'u', 'U', 0x20}, {'[', '{', 0x21}, {'i', 'I', 0x22}, {'p', 'P', 0x23},
			{'\n', 0, 0x24}, {'l', 'L', 0x25}, {'j', 'J', 0x26}, {'\'', '"', 0x27},
			{'k', 'K', 0x28}, {';', ':', 0x29}, {'\\', '|', 0x2a}, {',', '<', 0x2b},
			{'/', '?', 0x2c}, {'n', 'N', 0x2d}, {'m', 'M', 0x2e}, {'.', '>', 0x2f},
			{'\t', 0, 0x30}, {' ', 0, 0x31}
		};
		for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
			if (keys[index].plain == character) {
				code = keys[index].code;
				shifted = false;
				return true;
			}
			if (keys[index].shifted != 0 && keys[index].shifted == character) {
				code = keys[index].code;
				shifted = true;
				return true;
			}
		}
		return false;
	}

	bool Dispatch(int operation, int first, int second, const char *text,
		char *response, size_t responsesize)
	{
		Request request;
		SDL_Event event;

		memset(&request, 0, sizeof(request));
		request.operation = operation;
		request.first = first;
		request.second = second;
		if (text != NULL) {
			strncpy(request.text, text, sizeof(request.text) - 1);
			request.text[sizeof(request.text) - 1] = 0;
		}
		memset(&event, 0, sizeof(event));
		event.type = eventtype;
		event.user.type = eventtype;
		event.user.data1 = &request;
#if SDL_VERSION_ATLEAST(3, 0, 0)
		if (!SDL_PushEvent(&event)) {
#else
		if (SDL_PushEvent(&event) < 0) {
#endif
			WriteError(response, responsesize, SDL_GetError());
			return false;
		}
		while (!atomic_load_explicit(&request.done, memory_order_acquire) &&
			!atomic_load_explicit(&cancelrequested, memory_order_acquire))
			SDL_Delay(1);
		if (!atomic_load_explicit(&request.done, memory_order_acquire)) {
			WriteError(response, responsesize, "monitor stopped");
			return false;
		}
		strncpy(response, request.response, responsesize - 1);
		response[responsesize - 1] = 0;
		return strstr(response, "\"ok\":true") != NULL;
	}

	static Uint32 GetWindowId(const EmulatorMonitorView &view)
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		if (view.window != NULL)
			return SDL_GetWindowID((SDL_Window *)view.window);
#endif
		return 0;
	}

	static int SdlButton(int button)
	{
		if (button == 1)
			return SDL_BUTTON_RIGHT;
		if (button == 2)
			return SDL_BUTTON_MIDDLE;
		return SDL_BUTTON_LEFT;
	}

	void PushHostMotion(const EmulatorMonitorView &view, int x, int y, Request *request)
	{
		SDL_Event motion;
		memset(&motion, 0, sizeof(motion));
#if SDL_VERSION_ATLEAST(3, 0, 0)
		motion.type = SDL_EVENT_MOUSE_MOTION;
		motion.motion.windowID = GetWindowId(view);
		motion.motion.state = (SDL_MouseButtonFlags)hostbuttons;
		motion.motion.x = (float)x;
		motion.motion.y = (float)y;
		motion.motion.xrel = (float)(x - hostmousex);
		motion.motion.yrel = (float)(y - hostmousey);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		motion.type = SDL_MOUSEMOTION;
		motion.motion.windowID = GetWindowId(view);
		motion.motion.state = hostbuttons;
		motion.motion.x = x;
		motion.motion.y = y;
		motion.motion.xrel = x - hostmousex;
		motion.motion.yrel = y - hostmousey;
#else
		motion.type = SDL_MOUSEMOTION;
		motion.motion.state = (Uint8)hostbuttons;
		motion.motion.x = (Uint16)x;
		motion.motion.y = (Uint16)y;
		motion.motion.xrel = (Sint16)(x - hostmousex);
		motion.motion.yrel = (Sint16)(y - hostmousey);
#endif
		hostmousex = x;
		hostmousey = y;
		PushHostEvent(motion, request);
	}

	void PushHostButton(const EmulatorMonitorView &view, int button, bool pressed, Request *request)
	{
		SDL_Event buttonevent;
		const int sdlbutton = SdlButton(button);
		memset(&buttonevent, 0, sizeof(buttonevent));
		if (pressed)
			hostbuttons |= EMULATOR_MONITOR_BUTTON_MASK(sdlbutton);
		else
			hostbuttons &= ~EMULATOR_MONITOR_BUTTON_MASK(sdlbutton);
#if SDL_VERSION_ATLEAST(3, 0, 0)
		if (pressed)
			buttonevent.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		else
			buttonevent.type = SDL_EVENT_MOUSE_BUTTON_UP;
		buttonevent.button.windowID = GetWindowId(view);
		buttonevent.button.button = (Uint8)sdlbutton;
		buttonevent.button.down = pressed;
		buttonevent.button.clicks = 1;
		buttonevent.button.x = (float)hostmousex;
		buttonevent.button.y = (float)hostmousey;
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		if (pressed) {
			buttonevent.type = SDL_MOUSEBUTTONDOWN;
			buttonevent.button.state = SDL_PRESSED;
		} else {
			buttonevent.type = SDL_MOUSEBUTTONUP;
			buttonevent.button.state = SDL_RELEASED;
		}
		buttonevent.button.windowID = GetWindowId(view);
		buttonevent.button.button = (Uint8)sdlbutton;
		buttonevent.button.clicks = 1;
		buttonevent.button.x = hostmousex;
		buttonevent.button.y = hostmousey;
#else
		if (pressed) {
			buttonevent.type = SDL_MOUSEBUTTONDOWN;
			buttonevent.button.state = SDL_PRESSED;
		} else {
			buttonevent.type = SDL_MOUSEBUTTONUP;
			buttonevent.button.state = SDL_RELEASED;
		}
		buttonevent.button.button = (Uint8)sdlbutton;
		buttonevent.button.x = (Uint16)hostmousex;
		buttonevent.button.y = (Uint16)hostmousey;
#endif
		PushHostEvent(buttonevent, request);
	}

	void PushHotkey(const EmulatorMonitorView &view, int key, int scancode, Request *request)
	{
		SDL_Event keyevent;
		int hotkey = PrefsFindInt32("hotkey");
		int modifiers = 0;
		if (hotkey == 0)
			hotkey = 1;
		memset(&keyevent, 0, sizeof(keyevent));
#if SDL_VERSION_ATLEAST(3, 0, 0)
		if (hotkey & 1)
			modifiers |= SDL_KMOD_LCTRL;
		if (hotkey & 2)
			modifiers |= SDL_KMOD_LALT;
		if (hotkey & 4)
			modifiers |= SDL_KMOD_LGUI;
		keyevent.type = SDL_EVENT_KEY_DOWN;
		keyevent.key.windowID = GetWindowId(view);
		keyevent.key.key = (SDL_Keycode)key;
		keyevent.key.scancode = (SDL_Scancode)scancode;
		keyevent.key.mod = (SDL_Keymod)modifiers;
		keyevent.key.down = true;
		if (!PushHostEvent(keyevent, request))
			return;
		keyevent.type = SDL_EVENT_KEY_UP;
		keyevent.key.down = false;
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		if (hotkey & 1)
			modifiers |= KMOD_LCTRL;
		if (hotkey & 2)
			modifiers |= KMOD_LALT;
		if (hotkey & 4)
			modifiers |= KMOD_LGUI;
		keyevent.type = SDL_KEYDOWN;
		keyevent.key.windowID = GetWindowId(view);
		keyevent.key.state = SDL_PRESSED;
		keyevent.key.keysym.sym = (SDL_Keycode)key;
		keyevent.key.keysym.scancode = (SDL_Scancode)scancode;
		keyevent.key.keysym.mod = (Uint16)modifiers;
		if (!PushHostEvent(keyevent, request))
			return;
		keyevent.type = SDL_KEYUP;
		keyevent.key.state = SDL_RELEASED;
#else
		if (hotkey & 1)
			modifiers |= KMOD_LCTRL;
		if (hotkey & 2)
			modifiers |= KMOD_LALT;
		if (hotkey & 4)
			modifiers |= KMOD_LMETA;
		keyevent.type = SDL_KEYDOWN;
		keyevent.key.state = SDL_PRESSED;
		keyevent.key.keysym.sym = (SDLKey)key;
		keyevent.key.keysym.scancode = (Uint8)scancode;
		keyevent.key.keysym.mod = (SDLMod)modifiers;
		if (!PushHostEvent(keyevent, request))
			return;
		keyevent.type = SDL_KEYUP;
		keyevent.key.state = SDL_RELEASED;
#endif
		PushHostEvent(keyevent, request);
	}

	static bool PushHostEvent(SDL_Event &hostevent, Request *request)
	{
#if SDL_VERSION_ATLEAST(3, 0, 0)
		if (!SDL_PushEvent(&hostevent)) {
#else
		if (SDL_PushEvent(&hostevent) < 0) {
#endif
			SetError(request, SDL_GetError());
			return false;
		}
		SetOk(request);
		return true;
	}

	static void SaveHostSurface(const EmulatorMonitorView &view, Request *request)
	{
		if (view.hostsurface != NULL) {
			SaveSurface(view.hostsurface, request);
			return;
		}
#if defined(_WIN32)
		CaptureWindow(view, request);
#else
		SetError(request, "host surface unavailable");
#endif
	}

#if defined(_WIN32)
	static HWND GetWindowHandle(const EmulatorMonitorView &view)
	{
#if SDL_VERSION_ATLEAST(3, 0, 0)
		if (view.window == NULL)
			return NULL;
		return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)view.window),
			SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_SysWMinfo windowinfo;
		if (view.window == NULL)
			return NULL;
		SDL_VERSION(&windowinfo.version);
		if (!SDL_GetWindowWMInfo((SDL_Window *)view.window, &windowinfo))
			return NULL;
		return windowinfo.info.win.window;
#else
		SDL_SysWMinfo windowinfo;
		(void)view;
		SDL_VERSION(&windowinfo.version);
		if (SDL_GetWMInfo(&windowinfo) <= 0)
			return NULL;
		return windowinfo.window;
#endif
	}

	static void CaptureWindow(const EmulatorMonitorView &view, Request *request)
	{
		HWND windowhandle = GetWindowHandle(view);
		RECT clientrect;
		BITMAPINFO bitmapinfo;
		void *bits = NULL;
		HDC windowdc;
		HDC memorydc;
		HBITMAP bitmap;
		HGDIOBJ previousbitmap;
		SDL_Surface *surface;

		if (windowhandle == NULL || !GetClientRect(windowhandle, &clientrect) ||
			clientrect.right <= 0 || clientrect.bottom <= 0) {
			SetError(request, "host window unavailable");
			return;
		}
		memset(&bitmapinfo, 0, sizeof(bitmapinfo));
		bitmapinfo.bmiHeader.biSize = sizeof(bitmapinfo.bmiHeader);
		bitmapinfo.bmiHeader.biWidth = clientrect.right;
		bitmapinfo.bmiHeader.biHeight = -clientrect.bottom;
		bitmapinfo.bmiHeader.biPlanes = 1;
		bitmapinfo.bmiHeader.biBitCount = 32;
		bitmapinfo.bmiHeader.biCompression = BI_RGB;
		windowdc = GetDC(windowhandle);
		memorydc = CreateCompatibleDC(windowdc);
		bitmap = CreateDIBSection(windowdc, &bitmapinfo, DIB_RGB_COLORS, &bits, NULL, 0);
		if (bitmap == NULL || bits == NULL) {
			SetError(request, "unable to allocate capture bitmap");
			DeleteDC(memorydc);
			ReleaseDC(windowhandle, windowdc);
			return;
		}
		previousbitmap = SelectObject(memorydc, bitmap);
		if (PrintWindow(windowhandle, memorydc, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) {
			GdiFlush();
#if SDL_VERSION_ATLEAST(3, 0, 0)
			surface = SDL_CreateSurfaceFrom(clientrect.right, clientrect.bottom,
				SDL_PIXELFORMAT_XRGB8888, bits, clientrect.right * 4);
#else
			surface = SDL_CreateRGBSurfaceFrom(bits, clientrect.right, clientrect.bottom,
				32, clientrect.right * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
#endif
			if (surface != NULL) {
				SaveSurface(surface, request);
#if SDL_VERSION_ATLEAST(3, 0, 0)
				SDL_DestroySurface(surface);
#else
				SDL_FreeSurface(surface);
#endif
			} else
				SetError(request, SDL_GetError());
		} else
			SetError(request, "PrintWindow failed");
		SelectObject(memorydc, previousbitmap);
		DeleteObject(bitmap);
		DeleteDC(memorydc);
		ReleaseDC(windowhandle, windowdc);
	}
#endif

	static bool IsPngPath(const char *path)
	{
		size_t length = strlen(path);
		return length >= 4 && path[length - 4] == '.' &&
			(path[length - 3] == 'p' || path[length - 3] == 'P') &&
			(path[length - 2] == 'n' || path[length - 2] == 'N') &&
			(path[length - 1] == 'g' || path[length - 1] == 'G');
	}

	static void SaveSurface(SDL_Surface *surface, Request *request)
	{
		bool saved;
		if (IsPngPath(request->text))
			saved = SavePng(surface, request->text);
		else {
#if SDL_VERSION_ATLEAST(3, 0, 0)
			saved = SDL_SaveBMP(surface, request->text);
#else
			saved = SDL_SaveBMP(surface, request->text) == 0;
#endif
		}
		if (saved)
			SetPath(request, request->text);
		else
			SetError(request, SDL_GetError());
	}

	static SDL_Surface *CreateRgbSurface(int width, int height)
	{
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGB24);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		return SDL_CreateRGBSurfaceWithFormat(0, width, height, 24, SDL_PIXELFORMAT_RGB24);
#elif SDL_BYTEORDER == SDL_BIG_ENDIAN
		return SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 24,
			0xff0000, 0x00ff00, 0x0000ff, 0);
#else
		return SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 24,
			0x0000ff, 0x00ff00, 0xff0000, 0);
#endif
	}

	static void ConvertFramebufferRow(const EmulatorMonitorView &view, const Uint8 *source, Uint8 *target)
	{
		int column;
		if (view.depth == 32) {
			for (column = 0; column < view.width; column++) {
				target[0] = source[1];
				target[1] = source[2];
				target[2] = source[3];
				source += 4;
				target += 3;
			}
			return;
		}
		if (view.depth == 16) {
			for (column = 0; column < view.width; column++) {
				unsigned int pixel = ((unsigned int)source[0] << 8) | source[1];
				unsigned int red = (pixel >> 10) & 0x1f;
				unsigned int green = (pixel >> 5) & 0x1f;
				unsigned int blue = pixel & 0x1f;
				target[0] = (Uint8)((red << 3) | (red >> 2));
				target[1] = (Uint8)((green << 3) | (green >> 2));
				target[2] = (Uint8)((blue << 3) | (blue >> 2));
				source += 2;
				target += 3;
			}
			return;
		}
		for (column = 0; column < view.width; column++) {
			int bitoffset = column * view.depth;
			int index = (source[bitoffset >> 3] >> (8 - view.depth - (bitoffset & 7))) & ((1 << view.depth) - 1);
			target[0] = view.palette[index].r;
			target[1] = view.palette[index].g;
			target[2] = view.palette[index].b;
			target += 3;
		}
	}

	static void SaveFramebuffer(const EmulatorMonitorView &view, Request *request)
	{
		SDL_Surface *surface;
		int row;
		if (view.framebuffer == NULL || view.width <= 0 || view.height <= 0 ||
			(view.depth <= 8 && view.palette == NULL)) {
			SetError(request, "framebuffer unavailable");
			return;
		}
		surface = CreateRgbSurface(view.width, view.height);
		if (surface == NULL) {
			SetError(request, SDL_GetError());
			return;
		}
		SDL_LockSurface(surface);
		for (row = 0; row < view.height; row++)
			ConvertFramebufferRow(view, view.framebuffer + (size_t)row * view.rowbytes,
				(Uint8 *)surface->pixels + (size_t)row * surface->pitch);
		SDL_UnlockSurface(surface);
		SaveSurface(surface, request);
		FreeSurface(surface);
	}

	static SDL_Surface *ConvertToRgb(SDL_Surface *surface)
	{
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		return SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGB24, 0);
#else
		SDL_Surface *converted = CreateRgbSurface(surface->w, surface->h);
		if (converted != NULL && SDL_BlitSurface(surface, NULL, converted, NULL) != 0) {
			SDL_FreeSurface(converted);
			return NULL;
		}
		return converted;
#endif
	}

	static void FreeSurface(SDL_Surface *surface)
	{
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_DestroySurface(surface);
#else
		SDL_FreeSurface(surface);
#endif
	}

	static void PutBigEndian(std::vector<Uint8> &buffer, Uint32 value)
	{
		buffer.push_back((Uint8)(value >> 24));
		buffer.push_back((Uint8)(value >> 16));
		buffer.push_back((Uint8)(value >> 8));
		buffer.push_back((Uint8)value);
	}

	static void AppendChunk(std::vector<Uint8> &file, const char *type,
		const std::vector<Uint8> &data, const Uint32 *crctable)
	{
		Uint32 crc = 0xffffffff;
		size_t start;
		PutBigEndian(file, (Uint32)data.size());
		start = file.size();
		file.insert(file.end(), type, type + 4);
		file.insert(file.end(), data.begin(), data.end());
		for (size_t index = start; index < file.size(); index++)
			crc = crctable[(crc ^ file[index]) & 0xff] ^ (crc >> 8);
		PutBigEndian(file, crc ^ 0xffffffff);
	}

	static bool SavePng(SDL_Surface *surface, const char *path)
	{
		static const Uint8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		SDL_Surface *converted = ConvertToRgb(surface);
		std::vector<Uint8> header;
		std::vector<Uint8> compressed;
		std::vector<Uint8> file;
		std::vector<Uint8> empty;
		Uint32 crctable[256];
		Uint32 adlerlow = 1;
		Uint32 adlerhigh = 0;
		size_t rowbytes;
		size_t rawsize;
		size_t written = 0;
		FILE *output;
		bool saved;

		if (converted == NULL)
			return false;
		for (Uint32 entry = 0; entry < 256; entry++) {
			Uint32 value = entry;
			for (int bit = 0; bit < 8; bit++) {
				if (value & 1)
					value = 0xedb88320 ^ (value >> 1);
				else
					value >>= 1;
			}
			crctable[entry] = value;
		}
		rowbytes = (size_t)converted->w * 3;
		rawsize = (rowbytes + 1) * (size_t)converted->h;
		compressed.reserve(rawsize + rawsize / 65535 * 5 + 16);
		compressed.push_back(0x78);
		compressed.push_back(0x01);
		SDL_LockSurface(converted);
		for (int row = 0; row < converted->h; row++) {
			const Uint8 *pixels = (const Uint8 *)converted->pixels + (size_t)row * converted->pitch;
			for (size_t column = 0; column <= rowbytes; column++) {
				Uint8 value = 0;
				if (column > 0)
					value = pixels[column - 1];
				if (written % 65535 == 0) {
					size_t blocksize = rawsize - written;
					if (blocksize > 65535)
						blocksize = 65535;
					if (written + blocksize == rawsize)
						compressed.push_back(1);
					else
						compressed.push_back(0);
					compressed.push_back((Uint8)blocksize);
					compressed.push_back((Uint8)(blocksize >> 8));
					compressed.push_back((Uint8)~blocksize);
					compressed.push_back((Uint8)(~blocksize >> 8));
				}
				compressed.push_back(value);
				adlerlow = (adlerlow + value) % 65521;
				adlerhigh = (adlerhigh + adlerlow) % 65521;
				written++;
			}
		}
		SDL_UnlockSurface(converted);
		PutBigEndian(compressed, (adlerhigh << 16) | adlerlow);
		PutBigEndian(header, (Uint32)converted->w);
		PutBigEndian(header, (Uint32)converted->h);
		header.push_back(8);
		header.push_back(2);
		header.push_back(0);
		header.push_back(0);
		header.push_back(0);
		FreeSurface(converted);
		file.insert(file.end(), signature, signature + 8);
		AppendChunk(file, "IHDR", header, crctable);
		AppendChunk(file, "IDAT", compressed, crctable);
		AppendChunk(file, "IEND", empty, crctable);
		output = fopen(path, "wb");
		if (output == NULL) {
			SDL_SetError("unable to open %s", path);
			return false;
		}
		saved = fwrite(&file[0], 1, file.size(), output) == file.size();
		if (fclose(output) != 0)
			saved = false;
		if (!saved)
			SDL_SetError("unable to write %s", path);
		return saved;
	}

	static bool CheckButton(int button, char *response, size_t responsesize)
	{
		if (button >= 0 && button <= 2)
			return true;
		WriteError(response, responsesize, "button must be 0, 1, or 2");
		return false;
	}

	static void SetOkResponse(char *response, size_t responsesize)
	{
		snprintf(response, responsesize, "{\"ok\":true}");
	}

	static void SetOk(Request *request)
	{
		SetOkResponse(request->response, sizeof(request->response));
	}

	static void SetError(Request *request, const char *message)
	{
		WriteError(request->response, sizeof(request->response), message);
	}

	static void SetPath(Request *request, const char *path)
	{
		char escaped[768];
		Escape(path, escaped, sizeof(escaped));
		snprintf(request->response, sizeof(request->response),
			"{\"ok\":true,\"path\":\"%s\"}", escaped);
	}

	static void WriteExtFSRoot(char *response, size_t responsesize)
	{
		char escaped[1024];
		const char *extfspath = PrefsFindString("extfs");
#ifdef _WIN32
		char root[512];
		char *separator;
		DWORD length;
		if (PrefsFindBool("enableextfs")) {
			length = GetModuleFileNameA(NULL, root, sizeof(root));
			separator = strrchr(root, '\\');
			if (length > 0 && length < sizeof(root) && separator != NULL &&
				(size_t)(separator - root) + 17 < sizeof(root)) {
				strcpy(separator + 1, "Virtual Desktop");
				extfspath = root;
			}
		}
#endif
		if (extfspath == NULL || extfspath[0] == 0) {
			WriteError(response, responsesize, "extfs is not enabled");
			return;
		}
		Escape(extfspath, escaped, sizeof(escaped));
		snprintf(response, responsesize, "{\"ok\":true,\"path\":\"%s\"}", escaped);
	}

	static void WriteDisks(const char *volumename, char *response, size_t responsesize)
	{
		char escaped[1024];
		char fullpath[1024];
		const char *diskpath;
		size_t used;
		int index;

		Escape(volumename, escaped, sizeof(escaped));
		used = (size_t)snprintf(response, responsesize,
			"{\"ok\":true,\"extfsname\":\"%s\",\"disks\":[", escaped);
		for (index = 0; (diskpath = PrefsFindString("disk", index)) != NULL; index++) {
			if (diskpath[0] == '*')
				diskpath++;
#ifdef _WIN32
			if (_fullpath(fullpath, diskpath, sizeof(fullpath)) == NULL)
#else
			if (realpath(diskpath, fullpath) == NULL)
#endif
				snprintf(fullpath, sizeof(fullpath), "%s", diskpath);
			Escape(fullpath, escaped, sizeof(escaped));
			if (used + strlen(escaped) + 8 >= responsesize)
				break;
			if (index > 0)
				response[used++] = ',';
			used += (size_t)snprintf(response + used, responsesize - used,
				"\"%s\"", escaped);
		}
		snprintf(response + used, responsesize - used, "]}");
	}

	static void WriteError(char *response, size_t responsesize, const char *message)
	{
		char escaped[512];
		Escape(message, escaped, sizeof(escaped));
		snprintf(response, responsesize,
			"{\"ok\":false,\"error\":\"%s\"}", escaped);
	}

	static void Escape(const char *source, char *target, size_t targetsize)
	{
		size_t sourceindex;
		size_t targetindex;

		targetindex = 0;
		for (sourceindex = 0; source[sourceindex] != 0 &&
			targetindex + 2 < targetsize; sourceindex++) {
			if (source[sourceindex] == '"' || source[sourceindex] == '\\')
				target[targetindex++] = '\\';
			target[targetindex++] = source[sourceindex];
		}
		target[targetindex] = 0;
	}

	static void Trim(char *text)
	{
		size_t length;
		length = strlen(text);
		while (length > 0 && (text[length - 1] == '\r' ||
			text[length - 1] == '\n' || text[length - 1] == ' ' ||
			text[length - 1] == '\t')) {
			text[length - 1] = 0;
			length--;
		}
	}

	static void CloseSocket(EmulatorMonitorSocket monitorsocket)
	{
		if (monitorsocket == EMULATOR_MONITOR_INVALID_SOCKET)
			return;
#ifdef _WIN32
		shutdown(monitorsocket, SD_BOTH);
		closesocket(monitorsocket);
#else
		shutdown(monitorsocket, SHUT_RDWR);
		close(monitorsocket);
#endif
	}

	EmulatorMonitorSocket listensocket;
	EmulatorMonitorSocket clientsocket;
	SDL_Thread *thread;
	atomic_sint cancelrequested;
	Uint32 eventtype;
	bool windowsocketsready;
	int hostmousex;
	int hostmousey;
	int hostbuttons;
	char extfsname[256];
};

#endif
