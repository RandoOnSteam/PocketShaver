/*
 *  osxmain.cpp - main() entry point for OSX emulation path
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
#include "sys.h"
#include "prefs.h"
#include "osxmachine.h"
#include "xpram.h"
#include "osxguest.h"
#ifdef _WIN32
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
#ifdef ENABLE_EMULATOR_MONITOR
#include "../../../BasiliskII/src/SDL/emulator_monitor.h"
#endif

#define OSXSLICE 20000
#define OSXCLOCKINSTRUCTIONS 16384

SDL_Surface *SDLSetVideoModeQuietly(int width, int height, int depth, Uint32 flags);
#define OSXDEFAULTRAM (256 * 1024 * 1024)
#define OSXFRAMERATE 60

typedef struct OsxKeyMapping {
	int sdlkey;
	int adbcode;
} OsxKeyMapping;

typedef struct OsxSession {
	OsxMachine *machine;
	SDL_Surface *screen;
	uint8 *shadow;
	FILE *slicelog;
	FILE *script;
	FILE *msglog;
	char scriptline[512];
	char *scriptcommand;
	uint64 scriptticks;
	char consoleline[512];
	int consolelength;
	int hostx;
	int hosty;
	int buttons;
	int grabbed;
	uint32 traps[32][5];
	FILE *trace;
	char callnames[16][64];
	uint32 calladdresses[16];
	int callcount;
	char kextbundles[16][64];
	char kextsymbols[16][128];
	uint32 kextoffsets[16];
	int kextstates[16];
	int kextcount;
	int pointerhooked;
	int powerhooked;
	uint32 pointeroffset;
	uint32 absoluteoffset;
	OsxPointer pointer;
	uint32 backtraces[16][6];
	int backtracecount;
	uint32 backtracekey;
	int writelogs;
	int xmllogs;
	uint32 syscallkeys[64];
	uint32 syscallcounts[64];
	uint32 samples[8192];
	int samplecount;
	int trapcount;
} OsxSession;

int OsxGuestRequested(void)
{
	int diskindex;
	int partition;
	XPRAMInit(NULL);
	return OsxSelectBootImage(&diskindex, &partition) == OSXBOOTNATIVE;
}

static int MapKey(int key)
{
	static const OsxKeyMapping mappings[] = {
		{ SDLK_a, 0x00 }, { SDLK_s, 0x01 }, { SDLK_d, 0x02 }, { SDLK_f, 0x03 }, { SDLK_h, 0x04 },
		{ SDLK_g, 0x05 }, { SDLK_z, 0x06 }, { SDLK_x, 0x07 }, { SDLK_c, 0x08 }, { SDLK_v, 0x09 },
		{ SDLK_b, 0x0b }, { SDLK_q, 0x0c }, { SDLK_w, 0x0d }, { SDLK_e, 0x0e }, { SDLK_r, 0x0f },
		{ SDLK_y, 0x10 }, { SDLK_t, 0x11 }, { SDLK_1, 0x12 }, { SDLK_2, 0x13 }, { SDLK_3, 0x14 },
		{ SDLK_4, 0x15 }, { SDLK_6, 0x16 }, { SDLK_5, 0x17 }, { SDLK_EQUALS, 0x18 }, { SDLK_9, 0x19 },
		{ SDLK_7, 0x1a }, { SDLK_MINUS, 0x1b }, { SDLK_8, 0x1c }, { SDLK_0, 0x1d }, { SDLK_RIGHTBRACKET, 0x1e },
		{ SDLK_o, 0x1f }, { SDLK_u, 0x20 }, { SDLK_LEFTBRACKET, 0x21 }, { SDLK_i, 0x22 }, { SDLK_p, 0x23 },
		{ SDLK_RETURN, 0x24 }, { SDLK_l, 0x25 }, { SDLK_j, 0x26 }, { SDLK_QUOTE, 0x27 }, { SDLK_k, 0x28 },
		{ SDLK_SEMICOLON, 0x29 }, { SDLK_BACKSLASH, 0x2a }, { SDLK_COMMA, 0x2b }, { SDLK_SLASH, 0x2c },
		{ SDLK_n, 0x2d }, { SDLK_m, 0x2e }, { SDLK_PERIOD, 0x2f }, { SDLK_TAB, 0x30 }, { SDLK_SPACE, 0x31 },
		{ SDLK_BACKQUOTE, 0x32 }, { SDLK_BACKSPACE, 0x33 }, { SDLK_ESCAPE, 0x35 }, { SDLK_LCTRL, 0x36 },
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

static void ConsoleCharacter(OsxSession *session, int character);

static void ConsoleHook(OsxCpu *cpu, void *context, uint32 address)
{
	ConsoleCharacter((OsxSession *)context, (int)(cpu->gpr[3] & 0xff));
}

static void TerminalHook(OsxCpu *cpu, void *context, uint32 address)
{
	ConsoleCharacter((OsxSession *)context, (int)(cpu->gpr[3] & 0xff));
}

static void ConsoleCharacter(OsxSession *session, int character)
{
	OsxMachine *machine;
	machine = session->machine;
	if (character == '\n' || session->consolelength >= (int)sizeof(session->consoleline) - 1) {
		session->consoleline[session->consolelength] = 0;
		fprintf(stdout, "%s\n", session->consoleline);
		fflush(stdout);
		if (machine->console) {
			fprintf(machine->console, "[%6.2f] %s\n", (double)OsxCpuTimebase(machine->cpu) / OSXTIMEBASEFREQUENCY, session->consoleline);
			fflush(machine->console);
		}
		session->consolelength = 0;
		return;
	}
	if (character != '\r')
		session->consoleline[session->consolelength++] = (char)character;
}

static void GuestString(OsxCpu *cpu, uint32 address, char *output, int length)
{
	uint32 word;
	int index;
	char character;
	for (index = 0; index < length - 1; index++) {
		if (!OsxCpuReadVirtual(cpu, (address + index) & ~3u, &word))
			break;
		character = (char)(word >> (24 - ((address + index) & 3) * 8));
		if (character == 0)
			break;
		output[index] = character;
	}
	output[index] = 0;
}

static void FormatGuest(OsxMachine *machine, OsxCpu *cpu, char *output, int length)
{
	char format[512];
	char piece[256];
	char specifier[16];
	int used;
	int argument;
	int index;
	int width;
	uint32 value;
	GuestString(cpu, cpu->gpr[3], format, sizeof(format));
	used = 0;
	argument = 4;
	for (index = 0; format[index] && used < length - 64; index++) {
		if (format[index] != '%') {
			output[used++] = format[index];
			continue;
		}
		width = 0;
		specifier[width++] = '%';
		index++;
		while (format[index] && strchr("0123456789-.l#", format[index]) && width < 12)
			specifier[width++] = format[index++];
		if (format[index] == 0)
			break;
		if (format[index] == '%') {
			output[used++] = '%';
			continue;
		}
		value = 0;
		if (argument <= 10)
			value = cpu->gpr[argument];
		argument++;
		if (format[index] == 's') {
			GuestString(cpu, value, piece, 128);
		} else {
			specifier[width] = 0;
			strcat(specifier, "u");
			if (format[index] == 'x' || format[index] == 'X' || format[index] == 'p')
				specifier[strlen(specifier) - 1] = 'x';
			if (format[index] == 'd' || format[index] == 'i')
				specifier[strlen(specifier) - 1] = 'd';
			if (format[index] == 'c')
				specifier[strlen(specifier) - 1] = 'c';
			sprintf(piece, specifier, value);
		}
		strncpy(output + used, piece, length - used - 1);
		used += (int)strlen(output + used);
	}
	output[used] = 0;
}

static void TrapHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	uint32 *entry;
	session = (OsxSession *)context;
	entry = session->traps[session->trapcount & 31];
	entry[0] = cpu->gpr[3];
	entry[1] = 0;
	entry[2] = 0;
	OsxCpuReadVirtual(cpu, cpu->gpr[4] + 0x100, &entry[1]);
	OsxCpuReadVirtual(cpu, cpu->gpr[4] + 0x104, &entry[2]);
	entry[3] = cpu->gpr[5];
	entry[4] = cpu->gpr[6];
	session->trapcount++;
}

static void TraceStartHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	session = (OsxSession *)context;
	if (session->trace == NULL)
		session->trace = fopen("osxtrace.log", "w");
	cpu->tracefile = session->trace;
}

static void TraceStopHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->tracefile = NULL;
}

static void NewWorldEpochHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->gpr[3] = 1;
	cpu->nextpc = cpu->lr;
}

static void ReturnHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->nextpc = cpu->lr;
}

static void HookKernelSymbol(OsxMachine *machine, const char *name, OsxHookFunction function)
{
	if (OsxFindKernelSymbol(machine, name))
		OsxCpuAddHook(machine->cpu, OsxFindKernelSymbol(machine, name), function, NULL);
}

static void CallHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char text[64];
	uint32 word;
	int index;
	session = (OsxSession *)context;
	for (index = 0; index < session->callcount; index++) {
		if (session->calladdresses[index] != address)
			continue;
		GuestString(cpu, cpu->gpr[10], text, sizeof(text));
		word = 0;
		OsxCpuReadVirtual(cpu, cpu->gpr[3] + 0x28, &word);
		if (session->machine->log)
			fprintf(session->machine->log, "call %s [%s] state %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x lr %08x\n", session->callnames[index], text, word, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->lr);
	}
}

static void InstallCallHooks(OsxSession *session)
{
	const char *list;
	char name[64];
	int length;
	uint32 address;
	list = PrefsFindString("osxlogcalls");
	while (list && *list && session->callcount < 16) {
		while (*list == ' ' || *list == ',')
			list++;
		length = 0;
		while (list[length] && list[length] != ' ' && list[length] != ',' && length < 63) {
			name[length] = list[length];
			length++;
		}
		name[length] = 0;
		list += length;
		if (length == 0)
			break;
		address = OsxFindKernelSymbol(session->machine, name);
		if (address == 0)
			continue;
		strcpy(session->callnames[session->callcount], name);
		session->calladdresses[session->callcount] = address;
		session->callcount++;
		OsxCpuAddHook(session->machine->cpu, address, CallHook, session);
	}
}

static void ParseKextHooks(OsxSession *session)
{
	const char *list;
	char entry[256];
	char *colon;
	char *plus;
	int length;
	list = PrefsFindString("osxlogkext");
	while (list && *list && session->kextcount < 16) {
		while (*list == ' ' || *list == ',')
			list++;
		length = 0;
		while (list[length] && list[length] != ' ' && list[length] != ',' && length < 255) {
			entry[length] = list[length];
			length++;
		}
		entry[length] = 0;
		list += length;
		colon = strchr(entry, ':');
		if (length == 0 || colon == NULL)
			continue;
		*colon = 0;
		plus = strchr(colon + 1, '+');
		session->kextoffsets[session->kextcount] = 0;
		if (plus) {
			*plus = 0;
			session->kextoffsets[session->kextcount] = (uint32)strtoul(plus + 1, NULL, 16);
		}
		strncpy(session->kextbundles[session->kextcount], entry, 63);
		strncpy(session->kextsymbols[session->kextcount], colon + 1, 127);
		session->kextstates[session->kextcount] = 0;
		session->kextcount++;
	}
}

static void InstallKextHooks(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 offset;
	uint32 text;
	uint32 physical;
	int index;
	machine = session->machine;
	cpu = machine->cpu;
	if ((cpu->msr & OSXMSRPR) || !(cpu->msr & OSXMSRDR))
		return;
	for (index = 0; index < session->kextcount && session->callcount < 16; index++) {
		if (session->kextstates[index])
			continue;
		if (!OsxKextSymbolOffset(machine, session->kextbundles[index], session->kextsymbols[index], &offset)) {
			session->kextstates[index] = -1;
			continue;
		}
		if (!OsxKextTextAddress(machine, session->kextbundles[index], &text))
			continue;
		if (!OsxCpuTranslate(cpu, text + offset + session->kextoffsets[index], OSXTLBREAD, 0, &physical))
			continue;
		sprintf(session->callnames[session->callcount], "%.40s+%x", session->kextsymbols[index], session->kextoffsets[index]);
		session->calladdresses[session->callcount] = physical;
		session->callcount++;
		OsxCpuAddHook(cpu, physical, CallHook, session);
		session->kextstates[index] = 1;
	}
}

static void KmodStartHook(OsxCpu *cpu, void *context, uint32 address)
{
	InstallKextHooks((OsxSession *)context);
}

static void XmlHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char *text;
	uint32 length;
	uint32 index;
	uint32 word;
	session = (OsxSession *)context;
	length = cpu->gpr[4];
	if (length > 65536 || session->machine->log == NULL || session->xmllogs > 40)
		return;
	text = (char *)malloc(length + 1);
	for (index = 0; index < length; index++) {
		if (!OsxCpuReadVirtual(cpu, (cpu->gpr[3] + index) & ~3u, &word))
			break;
		text[index] = (char)(word >> (24 - ((cpu->gpr[3] + index) & 3) * 8));
	}
	text[index] = 0;
	if (strstr(text, "0x930")) {
		fprintf(session->machine->log, "xml %u: %s\n", length, text);
		session->xmllogs++;
	}
	free(text);
}

static void PanicHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char message[1024];
	int index;
	session = (OsxSession *)context;
	FormatGuest(session->machine, cpu, message, sizeof(message));
	fprintf(stdout, "PANIC: %s\n", message);
	if (session->machine->log) {
		fprintf(session->machine->log, "PANIC: %s lr %08x\n", message, cpu->lr);
		OsxCpuDumpFaults(cpu, session->machine->log);
		for (index = session->trapcount - 32; index < session->trapcount; index++)
			if (index >= 0)
				fprintf(session->machine->log, "trap %x srr0 %08x srr1 %08x dsisr %08x dar %08x\n", session->traps[index & 31][0], session->traps[index & 31][1], session->traps[index & 31][2], session->traps[index & 31][3], session->traps[index & 31][4]);
		fflush(session->machine->log);
	}
}

static void KprintfHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char message[1024];
	session = (OsxSession *)context;
	FormatGuest(session->machine, cpu, message, sizeof(message));
	if (session->machine->console) {
		fprintf(session->machine->console, "kprintf: %s", message);
		fflush(session->machine->console);
	}
}

static int FindPointerSymbols(OsxSession *session, const char *suffix)
{
	char relative[160];
	char absolute[160];
	sprintf(relative, "__ZN11IOHIDSystem25relativePointerEventGatedEiii12UnsignedWide%s", suffix);
	sprintf(absolute, "__ZN11IOHIDSystem25absolutePointerEventGatedEiP8IOGPointP9IOGBoundsbii12UnsignedWide%s", suffix);
	return OsxKextSymbolOffset(session->machine, "com.apple.iokit.IOHIDFamily", relative, &session->pointeroffset) &&
		OsxKextSymbolOffset(session->machine, "com.apple.iokit.IOHIDFamily", absolute, &session->absoluteoffset);
}

static void InstallPointerHook(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 text;
	uint32 physical;
	uint32 wheeloffset;
	machine = session->machine;
	cpu = machine->cpu;
	if (session->pointerhooked || (cpu->msr & OSXMSRPR) || !(cpu->msr & OSXMSRDR))
		return;
	if (session->pointeroffset == 0 && !FindPointerSymbols(session, "")) {
		session->pointer.sender = 1;
		if (!FindPointerSymbols(session, "P8OSObject")) {
			session->pointerhooked = -1;
			return;
		}
	}
	if (!OsxKextTextAddress(machine, "com.apple.iokit.IOHIDFamily", &text))
		return;
	if (!OsxCpuTranslate(cpu, text + session->pointeroffset, OSXTLBREAD, 0, &physical))
		return;
	session->pointer.machine = machine;
	session->pointer.absoluteaddress = text + session->absoluteoffset;
	session->pointer.wheeladdress = 0;
	if (OsxKextSymbolOffset(machine, "com.apple.iokit.IOHIDFamily", "__ZN11IOHIDSystem21scrollWheelEventGatedEsss12UnsignedWide", &wheeloffset) ||
		OsxKextSymbolOffset(machine, "com.apple.iokit.IOHIDFamily", "__ZN11IOHIDSystem21scrollWheelEventGatedEsssllllllm12UnsignedWideP8OSObject", &wheeloffset))
		session->pointer.wheeladdress = text + wheeloffset;
	OsxCpuAddHook(cpu, physical, OsxPointerHook, &session->pointer);
	machine->cuda.mouseabsolute = 1;
	session->pointerhooked = 1;
	if (machine->log)
		fprintf(machine->log, "pointer hook at %08x physical %08x\n", text + session->pointeroffset, physical);
}

static void PowerRegisterHook(OsxCpu *cpu, void *context, uint32 address)
{
	if (cpu->gpr[4] == cpu->gpr[5])
		cpu->gpr[4] = 0;
}

static void InstallPowerHook(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 offset;
	uint32 text;
	uint32 physical;
	machine = session->machine;
	cpu = machine->cpu;
	if (session->powerhooked || (cpu->msr & OSXMSRPR) || !(cpu->msr & OSXMSRDR))
		return;
	if (!OsxKextSymbolOffset(machine, "com.apple.driver.AppleGossamerPE", "__ZN10GossamerPE16PMRegisterDeviceEP9IOServiceS1_", &offset)) {
		session->powerhooked = -1;
		return;
	}
	if (!OsxKextTextAddress(machine, "com.apple.driver.AppleGossamerPE", &text) || !OsxCpuTranslate(cpu, text + offset, OSXTLBREAD, 0, &physical))
		return;
	OsxCpuAddHook(cpu, physical, PowerRegisterHook, NULL);
	session->powerhooked = 1;
}

static void MovePointer(OsxSession *session, int x, int y)
{
	OsxCudaMouse(session->machine, x - session->hostx, y - session->hosty, session->buttons & 1);
	session->hostx = x;
	session->hosty = y;
}

static void PressButton(OsxSession *session, int button, int down)
{
	if (button == 3 || button == 4) {
		if (down && button == 3)
			OsxCudaWheel(session->machine, 1);
		else if (down)
			OsxCudaWheel(session->machine, -1);
		return;
	}
	if (button < 0 || button > 2)
		return;
	if (down)
		session->buttons |= 1 << button;
	else
		session->buttons &= ~(1 << button);
	OsxCudaMouse(session->machine, 0, 0, session->buttons);
}

static int HostButton(int button)
{
	if (button == SDL_BUTTON_LEFT)
		return 0;
	if (button == SDL_BUTTON_RIGHT)
		return 1;
	if (button == SDL_BUTTON_MIDDLE)
		return 2;
	if (button == SDL_BUTTON_WHEELUP)
		return 3;
	if (button == SDL_BUTTON_WHEELDOWN)
		return 4;
	return -1;
}

#ifdef ENABLE_EMULATOR_MONITOR
static void MonitorInput(void *context, int operation, int first, int second)
{
	OsxSession *session;
	session = (OsxSession *)context;
	switch (operation) {
	case EMULATOR_MONITOR_MOUSE:
		MovePointer(session, first, second);
		break;
	case EMULATOR_MONITOR_MOUSEDOWN:
		PressButton(session, first, 1);
		break;
	case EMULATOR_MONITOR_MOUSEUP:
		PressButton(session, first, 0);
		break;
	case EMULATOR_MONITOR_WHEEL:
		OsxCudaWheel(session->machine, first);
		break;
	case EMULATOR_MONITOR_KEYDOWN:
		OsxCudaKey(session->machine, first, 1);
		break;
	case EMULATOR_MONITOR_KEYUP:
		OsxCudaKey(session->machine, first, 0);
		break;
	}
}
#endif

static void PresentFrame(OsxSession *session)
{
	OsxMachine *machine;
	uint8 *source;
	uint8 *shadowrow;
	uint32 *target;
	uint32 row;
	uint32 column;
	uint32 pixel;
	uint32 rowbytes;
	int changed;
	machine = session->machine;
	rowbytes = machine->rowbytes;
	changed = 0;
	if (SDL_LockSurface(session->screen) != 0)
		return;
	for (row = 0; row < machine->height; row++) {
		source = machine->vram + row * rowbytes;
		shadowrow = session->shadow + row * rowbytes;
		if (memcmp(source, shadowrow, machine->width * 4) == 0)
			continue;
		memcpy(shadowrow, source, machine->width * 4);
		target = (uint32 *)((uint8 *)session->screen->pixels + row * session->screen->pitch);
		for (column = 0; column < machine->width; column++) {
			pixel = ((uint32)source[column * 4 + 1] << 16) | ((uint32)source[column * 4 + 2] << 8) | source[column * 4 + 3];
			target[column] = pixel;
		}
		changed = 1;
	}
	SDL_UnlockSurface(session->screen);
	if (changed)
		SDL_UpdateRect(session->screen, 0, 0, 0, 0);
}

static void HandleEvents(OsxSession *session, void *monitor)
{
	SDL_Event event;
	int code;
	while (SDL_PollEvent(&event)) {
#ifdef ENABLE_EMULATOR_MONITOR
		if (monitor) {
			EmulatorMonitorView view;
			memset(&view, 0, sizeof(view));
			view.inputfunction = MonitorInput;
			view.inputcontext = session;
			view.framebuffer = session->machine->vram;
			view.width = (int)session->machine->width;
			view.height = (int)session->machine->height;
			view.depth = 32;
			view.rowbytes = (int)session->machine->rowbytes;
			view.hostsurface = session->screen;
			view.hostwidth = view.width;
			view.hostheight = view.height;
			if (((EmulatorMonitor *)monitor)->HandleEvent(event, view))
				continue;
		}
#endif
		switch (event.type) {
		case SDL_QUIT:
			session->machine->quit = 1;
			break;
		case SDL_MOUSEMOTION:
			MovePointer(session, event.motion.x, event.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN:
			PressButton(session, HostButton(event.button.button), 1);
			break;
		case SDL_MOUSEBUTTONUP:
			PressButton(session, HostButton(event.button.button), 0);
			break;
		case SDL_KEYDOWN:
			code = MapKey(event.key.keysym.sym);
			if (code >= 0)
				OsxCudaKey(session->machine, code, 1);
			break;
		case SDL_KEYUP:
			code = MapKey(event.key.keysym.sym);
			if (code >= 0)
				OsxCudaKey(session->machine, code, 0);
			break;
		}
	}
}

static void ParseScreen(OsxMachine *machine)
{
	const char *screen;
	int width;
	int height;
	machine->width = 1024;
	machine->height = 768;
	screen = PrefsFindString("screen");
	if (screen && sscanf(screen, "win/%d/%d", &width, &height) == 2 && width >= 640 && height >= 480 && width <= 1920 && height <= 1200) {
		machine->width = (uint32)width;
		machine->height = (uint32)height;
	}
	machine->depth = 32;
	machine->rowbytes = machine->width * 4;
}

static void AttachDisks(OsxMachine *machine, const char **bootimage, int *bootbus, int *bootunit)
{
	static const int diskslots[3] = { 0, 1, 3 };
	const char *path;
	int diskindex;
	int index;
	int used;
	int cdromattached;
	*bootimage = NULL;
	*bootbus = 0;
	*bootunit = 0;
	OsxSelectBootImage(&diskindex, &machine->bootpartition);
	used = 0;
	for (index = 0; (path = PrefsFindString("disk", index)) != NULL && used < 3; index++) {
		if (!OsxAtaAttach(machine, diskslots[used] >> 1, diskslots[used] & 1, path, 0))
			continue;
		if (used == diskindex) {
			*bootimage = path;
			*bootbus = diskslots[used] >> 1;
			*bootunit = diskslots[used] & 1;
		}
		used++;
	}
	for (index = 0; index < 8 && (path = PrefsFindString("cdrom", index)) != NULL; index++)
		strncpy(machine->cdrompaths[index], path, 511);
	machine->cdromcount = index;
	path = PrefsFindString("cdrom", 0);
	cdromattached = path != NULL && OsxAtaAttach(machine, 1, 0, path, 1);
	if (cdromattached && diskindex == -1) {
		*bootimage = path;
		*bootbus = 1;
		*bootunit = 0;
	}
}

static void SyscallObserver(OsxCpu *cpu, void *context)
{
	OsxSession *session;
	uint32 key;
	uint32 identifier;
	uint32 frame;
	char text[160];
	int index;
	session = (OsxSession *)context;
	if (cpu->gpr[0] == 4 && session->machine->log && session->writelogs < 2000) {
		GuestString(cpu, cpu->gpr[4], text, sizeof(text));
		if (cpu->gpr[5] < sizeof(text))
			text[cpu->gpr[5]] = 0;
		fprintf(session->machine->log, "write fd %u: %s\n", cpu->gpr[3], text);
		session->writelogs++;
	}
	key = cpu->gpr[0] << 16;
	if ((int32)cpu->gpr[0] == -31 && OsxCpuReadVirtual(cpu, cpu->gpr[3] + 20, &identifier)) {
		key = 0xffe10000u | (identifier & 0xffff);
		if (session->msglog && OsxCpuReadVirtual(cpu, cpu->gpr[3] + 8, &frame)) {
			fprintf(session->msglog, "%llu sr %06x lr %08x option %x id %d remote %x size %u bt", (unsigned long long)OsxCpuTimebase(cpu), cpu->sr[0] & 0xffffff, cpu->lr, cpu->gpr[4], identifier, frame, cpu->gpr[5]);
			frame = cpu->gpr[1];
			for (index = 0; index < 12; index++) {
				if (!OsxCpuReadVirtual(cpu, frame, &frame) || frame == 0 || !OsxCpuReadVirtual(cpu, frame + 8, &identifier))
					break;
				fprintf(session->msglog, " %08x", identifier);
			}
			fprintf(session->msglog, "\n");
		}
	}
	if (key == session->backtracekey && session->backtracecount < 16) {
		frame = cpu->gpr[1];
		session->backtraces[session->backtracecount][0] = cpu->lr;
		for (index = 1; index < 6; index++) {
			if (!OsxCpuReadVirtual(cpu, frame, &frame) || !OsxCpuReadVirtual(cpu, frame + 8, &session->backtraces[session->backtracecount][index]))
				break;
		}
		session->backtracecount++;
	}
	for (index = 0; index < 64; index++) {
		if (session->syscallcounts[index] && session->syscallkeys[index] == key) {
			session->syscallcounts[index]++;
			return;
		}
	}
	for (index = 0; index < 64; index++) {
		if (session->syscallcounts[index] == 0) {
			session->syscallkeys[index] = key;
			session->syscallcounts[index] = 1;
			return;
		}
	}
}

static void DumpSyscalls(OsxSession *session)
{
	int index;
	if (session->machine->log == NULL)
		return;
	fprintf(session->machine->log, "syscalls");
	for (index = 0; index < 64; index++)
		if (session->syscallcounts[index] > 50)
			fprintf(session->machine->log, " %08x:%u", session->syscallkeys[index], session->syscallcounts[index]);
	fprintf(session->machine->log, "\n");
	memset(session->syscallcounts, 0, sizeof(session->syscallcounts));
	for (index = 0; index < session->backtracecount; index++)
		fprintf(session->machine->log, "backtrace %08x %08x %08x %08x %08x %08x\n", session->backtraces[index][0], session->backtraces[index][1], session->backtraces[index][2], session->backtraces[index][3], session->backtraces[index][4], session->backtraces[index][5]);
	session->backtracecount = 0;
}

static int CompareSamples(const void *left, const void *right)
{
	uint32 a;
	uint32 b;
	a = *(const uint32 *)left;
	b = *(const uint32 *)right;
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

static void DumpSamples(OsxSession *session)
{
	uint32 best[8];
	int bestcount[8];
	int index;
	int run;
	int slot;
	int lowest;
	if (session->samplecount == 0 || session->machine->log == NULL)
		return;
	qsort(session->samples, session->samplecount, sizeof(uint32), CompareSamples);
	memset(bestcount, 0, sizeof(bestcount));
	memset(best, 0, sizeof(best));
	run = 1;
	for (index = 1; index <= session->samplecount; index++) {
		if (index < session->samplecount && session->samples[index] == session->samples[index - 1]) {
			run++;
			continue;
		}
		lowest = 0;
		for (slot = 1; slot < 8; slot++)
			if (bestcount[slot] < bestcount[lowest])
				lowest = slot;
		if (run > bestcount[lowest]) {
			bestcount[lowest] = run;
			best[lowest] = session->samples[index - 1];
		}
		run = 1;
	}
	fprintf(session->machine->log, "samples");
	for (slot = 0; slot < 8; slot++)
		fprintf(session->machine->log, " %08x:%d", best[slot], bestcount[slot]);
	fprintf(session->machine->log, "\n");
	session->samplecount = 0;
}

static void ParseDiskPatches(OsxMachine *machine)
{
	const char *text;
	const char *colon;
	OsxDiskPatch *patches;
	int count;
	int index;
	unsigned int value;
	count = 0;
	patches = (OsxDiskPatch *)calloc(128, sizeof(OsxDiskPatch));
	for (index = 0; (text = PrefsFindString("osxpatch", index)) != NULL && count < 128; index++) {
		if (strncmp(text, "cdrom:", 6) == 0) {
			patches[count].atapionly = 1;
			text += 6;
		}
		colon = strchr(text, ':');
		if (colon == NULL)
			continue;
		patches[count].offset = (uint64)strtoull(text, NULL, 16);
		colon++;
		while (colon[0] && colon[1] && patches[count].length < 16 && sscanf(colon, "%2x", &value) == 1) {
			patches[count].bytes[patches[count].length++] = (uint8)value;
			colon += 2;
		}
		count++;
	}
	for (index = 0; index < OSXATABUSES; index++) {
		machine->ata[index].patches = patches;
		machine->ata[index].patchcount = count;
	}
}

static int KernelRead32(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint32 savedmsr;
	uint32 savedsr[16];
	int index;
	int result;
	savedmsr = cpu->msr;
	memcpy(savedsr, cpu->sr, sizeof(savedsr));
	for (index = 0; index < 16; index++)
		cpu->sr[index] = 0x20000000u | ((uint32)index << 20);
	cpu->msr = OSXMSRDR | OSXMSRIR;
	result = OsxCpuReadVirtual(cpu, address, value);
	cpu->msr = savedmsr;
	memcpy(cpu->sr, savedsr, sizeof(savedsr));
	return result;
}

static uint32 AccessorOffset(OsxCpu *cpu, uint32 address, uint32 opcode, uint32 target)
{
	uint32 word;
	int index;
	for (index = 0; address != 0 && index < 48; index++) {
		if (!KernelRead32(cpu, address + index * 4, &word))
			return 0;
		if ((word >> 26) == opcode && ((word >> 21) & 31) == target)
			return word & 0xffff;
	}
	return 0;
}

static void DumpProcesses(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 proc;
	uint32 word;
	uint32 pid;
	uint32 pidoffset;
	uint32 nameoffset;
	char name[20];
	int guard;
	int index;
	machine = session->machine;
	cpu = machine->cpu;
	if (machine->log == NULL || !KernelRead32(cpu, OsxFindKernelSymbol(machine, "_allproc"), &proc))
		return;
	pidoffset = AccessorOffset(cpu, OsxFindKernelSymbol(machine, "_proc_pid"), 32, 3);
	nameoffset = AccessorOffset(cpu, OsxFindKernelSymbol(machine, "_proc_name"), 14, 4);
	if (pidoffset == 0 || nameoffset == 0)
		return;
	fprintf(machine->log, "ps");
	for (guard = 0; proc != 0 && guard < 64; guard++) {
		pid = 0;
		KernelRead32(cpu, proc + pidoffset, &pid);
		for (index = 0; index < 16; index++) {
			if (!KernelRead32(cpu, (proc + nameoffset + index) & ~3u, &word))
				break;
			name[index] = (char)(word >> (24 - ((proc + nameoffset + index) & 3) * 8));
			if (name[index] == 0)
				break;
		}
		name[index] = 0;
		fprintf(machine->log, " %u:%s", pid, name);
		if (!KernelRead32(cpu, proc, &proc))
			break;
	}
	fprintf(machine->log, "\n");
}

static void SaveFramebuffer(OsxMachine *machine, const char *path)
{
	FILE *file;
	uint8 *row;
	uint8 *pixels;
	uint32 y;
	uint32 x;
	file = fopen(path, "wb");
	if (file == NULL)
		return;
	pixels = (uint8 *)malloc(machine->width * 3);
	fprintf(file, "P6\n%u %u\n255\n", machine->width, machine->height);
	for (y = 0; y < machine->height; y++) {
		row = machine->vram + y * machine->rowbytes;
		for (x = 0; x < machine->width; x++) {
			pixels[x * 3] = row[x * 4 + 1];
			pixels[x * 3 + 1] = row[x * 4 + 2];
			pixels[x * 3 + 2] = row[x * 4 + 3];
		}
		fwrite(pixels, 1, machine->width * 3, file);
	}
	free(pixels);
	fclose(file);
}

static int NextScriptLine(OsxSession *session)
{
	double seconds;
	int consumed;
	while (fgets(session->scriptline, sizeof(session->scriptline), session->script)) {
		if (sscanf(session->scriptline, "%lf %n", &seconds, &consumed) < 1)
			continue;
		session->scriptticks = (uint64)(seconds * OSXTIMEBASEFREQUENCY);
		session->scriptcommand = session->scriptline + consumed;
		return 1;
	}
	fclose(session->script);
	session->script = NULL;
	return 0;
}

static void ExecuteScriptCommand(OsxSession *session, const char *command)
{
	OsxMachine *machine;
	char path[256];
	int first;
	int second;
	int third;
	int fourth;
	machine = session->machine;
	if (machine->log)
		fprintf(machine->log, "script %llu %s", (unsigned long long)OsxCpuTimebase(machine->cpu), command);
	if (sscanf(command, "mouse %d %d", &first, &second) == 2)
		MovePointer(session, first, second);
	else if (sscanf(command, "down %d", &first) == 1)
		PressButton(session, first, 1);
	else if (sscanf(command, "up %d", &first) == 1)
		PressButton(session, first, 0);
	else if (sscanf(command, "wheel %d", &first) == 1)
		OsxCudaWheel(machine, first);
	else if (sscanf(command, "keydown %d", &first) == 1)
		OsxCudaKey(machine, first, 1);
	else if (sscanf(command, "keyup %d", &first) == 1)
		OsxCudaKey(machine, first, 0);
	else if (sscanf(command, "shot %255s", path) == 1)
		SaveFramebuffer(machine, path);
	else if (sscanf(command, "watch %d %d %d %d %255s", &first, &second, &third, &fourth, path) == 5)
		OsxCpuWatch(machine->cpu, fopen(path, "w"), machine->rowbytes, (uint32)first, (uint32)second, (uint32)third, (uint32)fourth);
	else if (strncmp(command, "unwatch", 7) == 0 && machine->cpu->watchfile) {
		fclose(machine->cpu->watchfile);
		machine->cpu->watchfile = NULL;
		OsxCpuFlushTlb(machine->cpu);
	} else if (sscanf(command, "msglog %255s", path) == 1)
		session->msglog = fopen(path, "w");
	else if (strncmp(command, "msgunlog", 8) == 0 && session->msglog) {
		fclose(session->msglog);
		session->msglog = NULL;
	} else if (strncmp(command, "quit", 4) == 0)
		machine->quit = 1;
}

static void RunScript(OsxSession *session, uint64 now)
{
	while (session->script && now >= session->scriptticks) {
		ExecuteScriptCommand(session, session->scriptcommand);
		NextScriptLine(session);
	}
}

static void LogSlice(FILE *slicelog, OsxCpu *cpu)
{
	uint32 hash;
	int index;
	hash = cpu->cr ^ cpu->lr ^ cpu->ctr ^ cpu->xer;
	for (index = 0; index < 32; index++)
		hash = hash * 31 + cpu->gpr[index] + (uint32)cpu->fpr[index].bits;
	fprintf(slicelog, "%llu %08x %08x\n", (unsigned long long)cpu->instructions, cpu->pc, hash);
}

static void RunMachine(OsxSession *session, void *monitor)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint64 now;
	uint64 nextframe;
	uint64 frameticks;
	uint64 waitticks;
	uint64 cudadeadline;
	uint32 milliseconds;
	uint64 nextstatus;
	uint64 runstart;
	uint64 runtime;
	uint64 slices;
	uint64 stoptime;
	uint64 clockinstructions;
	int32 seconds;
	machine = session->machine;
	cpu = machine->cpu;
	frameticks = OSXTIMEBASEFREQUENCY / OSXFRAMERATE;
	nextframe = 0;
	nextstatus = OSXTIMEBASEFREQUENCY;
	runtime = 0;
	slices = 0;
	seconds = PrefsFindInt32("osxseconds");
	stoptime = OSXNEVER;
	if (seconds > 0)
		stoptime = (uint64)seconds * OSXTIMEBASEFREQUENCY;
	now = OsxCpuTimebase(cpu);
	clockinstructions = cpu->instructions;
	while (!machine->quit) {
		OsxCpuCheckInterrupts(cpu, now);
		if (cpu->halted) {
			waitticks = frameticks;
			if (cpu->decdeadline != OSXNEVER && cpu->decdeadline > now && cpu->decdeadline - now < waitticks)
				waitticks = cpu->decdeadline - now;
			cudadeadline = OsxCudaNextDeadline(machine);
			if (OsxBmacNextDeadline(machine) < cudadeadline)
				cudadeadline = OsxBmacNextDeadline(machine);
			if (cudadeadline <= now)
				waitticks = 0;
			else if (cudadeadline - now < waitticks)
				waitticks = cudadeadline - now;
			if (machine->ata[0].irqdeadline || machine->ata[1].irqdeadline)
				waitticks = 0;
			milliseconds = (uint32)(waitticks * 1000 / OSXTIMEBASEFREQUENCY);
			if (cpu->deterministic)
				cpu->virtualticks += waitticks + 1;
			else if (milliseconds > 0)
				SDL_Delay(milliseconds);
			now = OsxCpuTimebase(cpu);
		} else {
			runstart = now;
			OsxJitRun(cpu, OSXSLICE);
			if (session->slicelog)
				LogSlice(session->slicelog, cpu);
			if (cpu->halted || cpu->instructions - clockinstructions >= OSXCLOCKINSTRUCTIONS) {
				now = OsxCpuTimebase(cpu);
				clockinstructions = cpu->instructions;
			}
			runtime += now - runstart;
			if (session->samplecount < 8192)
				session->samples[session->samplecount++] = cpu->pc | ((cpu->msr >> 14) & 1);
			slices++;
		}
		OsxCudaPoll(machine, now);
		OsxBmacPoll(machine, now);
		OsxAtaPoll(machine, now);
		OsxAudioPoll(machine, now);
		RunScript(session, now);
		if (now >= nextframe) {
			nextframe = now + frameticks;
			PresentFrame(session);
			HandleEvents(session, monitor);
		}
		if (now >= nextstatus) {
			InstallPointerHook(session);
			InstallPowerHook(session);
			InstallKextHooks(session);
			DumpSamples(session);
			DumpSyscalls(session);
			if ((cpu->decrementercount & 7) == 0)
				DumpProcesses(session);
			nextstatus = now + OSXTIMEBASEFREQUENCY * 2;
			OsxCpuCalibrateTimebase(cpu);
			now = OsxCpuTimebase(cpu);
			if (machine->log) {
				fprintf(machine->log, "status pc %08x lr %08x msr %08x instructions %llu halted %d dec %u ext %u slow %llu exceptions %llu runms %llu slices %llu\n", cpu->pc, cpu->lr, cpu->msr, (unsigned long long)cpu->instructions, cpu->halted, cpu->decrementercount, cpu->externalcount, (unsigned long long)cpu->slowaccesses, (unsigned long long)cpu->exceptions, (unsigned long long)(runtime * 1000 / OSXTIMEBASEFREQUENCY), (unsigned long long)slices);
				fflush(machine->log);
			}
		}
		if (now >= stoptime)
			machine->quit = 1;
	}
}

int OsxGuestMain(void)
{
	OsxSession session;
	OsxMachine *machine;
	OsxCpu *cpu;
	const char *bootimage;
	const char *bootargs;
	int bootbus;
	int bootunit;
	int busindex;
	int unitindex;
	FILE *dump;
	void *monitor;
	int32 ramsize;
	memset(&session, 0, sizeof(session));
	machine = (OsxMachine *)calloc(1, sizeof(OsxMachine));
	cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	session.machine = machine;
	machine->cpu = cpu;
	machine->log = fopen("osxguest.log", "w");
	machine->console = fopen("osxconsole.log", "w");
	ramsize = PrefsFindInt32("ramsize");
	if (ramsize > 0 && ramsize <= 1024)
		ramsize *= 1024 * 1024;
	if (ramsize < 128 * 1024 * 1024)
		ramsize = OSXDEFAULTRAM;
	machine->ramsize = (uint32)ramsize & ~0xfffffu;
	machine->ram = (uint8 *)calloc(1, machine->ramsize);
	machine->vram = (uint8 *)calloc(1, OSXVRAMSIZE);
	if (machine->ram == NULL || machine->vram == NULL) {
		fprintf(stderr, "osx: cannot allocate guest memory\n");
		return 1;
	}
	ParseScreen(machine);
	OsxCpuReset(cpu, OSXPVR, OSXTIMEBASEFREQUENCY);
	if (PrefsFindInt32("osxdeterministic") > 0) {
		cpu->deterministic = (uint32)PrefsFindInt32("osxdeterministic");
		session.slicelog = fopen("osxslices.log", "w");
	}
	if (PrefsFindString("osxinputscript")) {
		session.script = fopen(PrefsFindString("osxinputscript"), "r");
		if (session.script)
			NextScriptLine(&session);
	}
	cpu->ram = machine->ram;
	cpu->ramsize = machine->ramsize;
	cpu->vram = machine->vram;
	cpu->vrambase = OSXVRAMBASE;
	cpu->vramsize = OSXVRAMSIZE;
	cpu->buscontext = machine;
	cpu->busread = OsxBusRead;
	cpu->buswrite = OsxBusWrite;
	cpu->logfile = machine->log;
	OsxPciReset(machine);
	OsxAtaReset(machine);
	OsxCudaReset(machine);
	OsxBmacReset(machine);
	OsxAudioReset(machine);
	XPRAMInit(NULL);
	memcpy(machine->nvram, XPRAM, OSXNVRAMSIZE);
	OsxFlashLoad(machine, "osx_nvram.bin");
	AttachDisks(machine, &bootimage, &bootbus, &bootunit);
	ParseDiskPatches(machine);
	if (bootimage == NULL) {
		fprintf(stderr, "osx: no cdrom or disk image configured\n");
		return 1;
	}
	if (!OsxNvramPrepareBoot(machine, bootbus, bootunit, machine->bootpartition)) {
		fprintf(stderr, "osx: cannot store Startup Disk selection\n");
		return 1;
	}
	bootargs = PrefsFindString("osxbootargs");
	if (bootargs == NULL)
		bootargs = "-v";
	if (!OsxBootLoad(machine, bootimage, bootbus, bootunit, bootargs))
		return 1;
	if (OsxFindKernelSymbol(machine, "_conslog_putc"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_conslog_putc"), ConsoleHook, &session);
	if (OsxFindKernelSymbol(machine, "_kmputc"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kmputc"), TerminalHook, &session);
	if (OsxFindKernelSymbol(machine, "_kprintf"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kprintf"), KprintfHook, &session);
	if (OsxFindKernelSymbol(machine, "_trap"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_trap"), TrapHook, &session);
	if (PrefsFindString("osxtracestart") && OsxFindKernelSymbol(machine, PrefsFindString("osxtracestart")))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, PrefsFindString("osxtracestart")), TraceStartHook, &session);
	if (PrefsFindString("osxtracestop") && OsxFindKernelSymbol(machine, PrefsFindString("osxtracestop")))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, PrefsFindString("osxtracestop")), TraceStopHook, &session);
	InstallCallHooks(&session);
	if (PrefsFindString("osxlogxml"))
		OsxCpuAddHook(cpu, (uint32)strtoul(PrefsFindString("osxlogxml"), NULL, 16), XmlHook, &session);
	ParseKextHooks(&session);
	if (session.kextcount && OsxFindKernelSymbol(machine, "_kmod_start_or_stop"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kmod_start_or_stop"), KmodStartHook, &session);
	session.backtracekey = 0xffe10afeu;
	if (PrefsFindString("osxbacktracekey"))
		session.backtracekey = (uint32)strtoul(PrefsFindString("osxbacktracekey"), NULL, 16);
	cpu->syscallobserver = SyscallObserver;
	cpu->syscallcontext = &session;
	if (OsxFindKernelSymbol(machine, "_panic"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_panic"), PanicHook, &session);
	memcpy(machine->nvramclassic, machine->nvram, OSXNVRAMSIZE);
	OsxNvramMakeNewWorld(machine->nvram);
	HookKernelSymbol(machine, "__ZN16IOPlatformExpert14getBootROMTypeEv", NewWorldEpochHook);
	HookKernelSymbol(machine, "_PE_Determine_Clock_Speeds", ReturnHook);
	if (machine->pvr == OSXPVRG4 || machine->ata[bootbus].drives[bootunit].atapi)
		HookKernelSymbol(machine, "_PEGetPlatformEpoch", NewWorldEpochHook);
	cpu->pc = machine->kernelentry;
	cpu->msr = 0;
	cpu->gpr[1] = machine->scratch + 0x8000;
	cpu->gpr[3] = machine->bootargs;
	cpu->gpr[4] = 0x426f6f58;
	OsxCpuUpdateModes(cpu);
	if (PrefsFindBool("osxjit")) {
		if (OsxJitCreate(cpu))
			fprintf(machine->log, "cpu native x64 soft TLB\n");
		else
			fprintf(machine->log, "cpu interpreter\n");
	}
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "osx: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_WM_SetCaption("SheepShaver - Mac OS X", "SheepShaver");
	session.screen = SDLSetVideoModeQuietly((int)machine->width, (int)machine->height, 32, SDL_SWSURFACE);
	if (session.screen == NULL) {
		fprintf(stderr, "osx: SDL_SetVideoMode failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_ShowCursor(SDL_DISABLE);
	session.shadow = (uint8 *)calloc(1, machine->rowbytes * machine->height);
	memset(session.shadow, 0xff, machine->rowbytes * machine->height);
	monitor = NULL;
#ifdef ENABLE_EMULATOR_MONITOR
	monitor = new EmulatorMonitor();
	if (!((EmulatorMonitor *)monitor)->Start()) {
		delete (EmulatorMonitor *)monitor;
		monitor = NULL;
	}
#endif
#ifdef _WIN32
	timeBeginPeriod(1);
#endif
	RunMachine(&session, monitor);
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	OsxNvramFinishBoot(machine, XPRAM + 0x1378);
	OsxNvramRestoreOldWorld(machine->nvram, machine->nvramclassic);
	memcpy(XPRAM, machine->nvram, OSXNVRAMSIZE);
	XPRAMExit();
	OsxFlashSave(machine);
	OsxAudioExit(machine);
	OsxJitDestroy(cpu);
	if (machine->log) {
		OsxCpuDump(cpu, machine->log);
		fclose(machine->log);
	}
	if (PrefsFindString("osxdump")) {
		dump = fopen(PrefsFindString("osxdump"), "wb");
		if (dump) {
			fwrite(machine->ram, 1, machine->ramsize, dump);
			fclose(dump);
		}
	}
#ifdef ENABLE_EMULATOR_MONITOR
	if (monitor)
		delete (EmulatorMonitor *)monitor;
#endif
	if (machine->console)
		fclose(machine->console);
	for (busindex = 0; busindex < 2; busindex++) {
		for (unitindex = 0; unitindex < 2; unitindex++) {
			if (machine->ata[busindex].drives[unitindex].file)
				Sys_close(machine->ata[busindex].drives[unitindex].file);
		}
	}
	SDL_Quit();
	if (machine->quit == 2)
		return 2;
	return 0;
}
