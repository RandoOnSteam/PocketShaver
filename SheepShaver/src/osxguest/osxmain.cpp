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
#if defined(_M_X64) || defined(__x86_64__)
#include <tmmintrin.h>
#endif
#pragma comment(lib, "winmm.lib")
#endif
#ifdef ENABLE_EMULATOR_MONITOR
#include "../../../BasiliskII/src/SDL/emulator_monitor.h"
#endif

#define OSXSLICE 262144

SDL_Surface *SDLSetVideoModeQuietly(int width, int height, int depth, Uint32 flags);
#if SDL_VERSION_ATLEAST(2, 0, 0)
extern SDL_Window *sdl_window;
#define OSXVIDEOFLAGS 0
#else
#define OSXVIDEOFLAGS SDL_SWSURFACE
#endif
#define OSXDEFAULTRAM (256 * 1024 * 1024)
#define OSXFRAMERATE 60
#define OSXPRESENTRUNS 16

typedef struct OsxKeyMapping {
	int sdlkey;
	int adbcode;
} OsxKeyMapping;

typedef struct OsxSession {
	OsxMachine *machine;
	SDL_Surface *screen;
	uint8 *shadow;
	int nvramready;
	int sdlready;
	int pixelshuffle;
	FILE *slicelog;
	FILE *script;
	FILE *msglog;
#if OSXMSGTRACE
	uint32 pending[256][3];
#endif
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
	uint32 wheeloffset;
	const char *pointerbundle;
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
#if OSXPROFILE
	FILE *tmpsamplefile;
	uint32 tmpframes;
	uint32 tmprows;
#endif
} OsxSession;

int OsxGuestRequested(void)
{
	int diskindex;
	int partition;
	XPRAMInit(NULL);
	return OsxSelectBootImage(&diskindex, &partition) == OSXBOOTNATIVE;
}

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

static void RefuseSleepHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->gpr[3] = 0;
	cpu->nextpc = cpu->lr;
}

static void ReturnHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->nextpc = cpu->lr;
}

#if OSXG5
static void NapHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->msr |= OSXMSREE;
	cpu->nextpc = cpu->lr;
	if (!cpu->externalline && !cpu->decpending) {
		cpu->halted = 1;
		cpu->slice = 0;
	}
}
#endif

static void AllowStalledClassLookup(OsxMachine *machine)
{
	uint8 *code;
	uint32 address;
	uint32 index;
	address = OsxFindKernelSymbol(machine, "_getMetaClassWithName__11OSMetaClassPC8OSSymbol");
	if (address == 0 || address + 0x100 > machine->ramsize)
		return;
	code = machine->ram + address;
	for (index = 4; index < 0x100 && memcmp(code + index - 4, "\x4e\x80\x00\x20", 4) != 0; index += 4)
		if ((code[index - 4] >> 2) == 16 && memcmp(code + index, "\x3b\xa0\x00\x00", 4) == 0)
			memcpy(code + index, "\x60\x00\x00\x00", 4);
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

static void InstallPowerHook(OsxSession *session);

static void KmodStartHook(OsxCpu *cpu, void *context, uint32 address)
{
	InstallPowerHook((OsxSession *)context);
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

static int FindPointerSymbol(OsxSession *session, const char *name, uint32 *offset)
{
	if (session->pointerbundle)
		return OsxKextSymbolOffset(session->machine, session->pointerbundle, name, offset);
	*offset = OsxFindKernelSymbol(session->machine, name);
	return *offset != 0;
}

static int FindPointerSymbols(OsxSession *session)
{
	static const char *const bundles[4] = { "com.apple.iokit.IOHIDFamily", "com.apple.iokit.IOHIDFamily", "com.apple.iokit.IOHIDSystem", NULL };
	static const char *const relatives[4] = {
		"__ZN11IOHIDSystem25relativePointerEventGatedEiii12UnsignedWide",
		"__ZN11IOHIDSystem25relativePointerEventGatedEiii12UnsignedWideP8OSObject",
		"__relativePointerEvent__11IOHIDSystemPB0in2G12UnsignedWide",
		"__relativePointerEvent__11IOHIDSystemPB0in2G12UnsignedWide"
	};
	static const char *const absolutes[4] = {
		"__ZN11IOHIDSystem25absolutePointerEventGatedEiP8IOGPointP9IOGBoundsbii12UnsignedWide",
		"__ZN11IOHIDSystem25absolutePointerEventGatedEiP8IOGPointP9IOGBoundsbii12UnsignedWideP8OSObject",
		"__absolutePointerEvent__11IOHIDSystemPB0iP8IOGPointP9IOGBoundsbiiG12UnsignedWide",
		"__absolutePointerEvent__11IOHIDSystemPB0iP8IOGPointP9IOGBoundsbiiG12UnsignedWide"
	};
	static const char *const wheels[4] = {
		"__ZN11IOHIDSystem21scrollWheelEventGatedEsss12UnsignedWide",
		"__ZN11IOHIDSystem21scrollWheelEventGatedEsssllllllm12UnsignedWideP8OSObject",
		"__scrollWheelEvent__11IOHIDSystemPB0sn2G12UnsignedWide",
		"__scrollWheelEvent__11IOHIDSystemPB0sn2G12UnsignedWide"
	};
	int index;
	for (index = 0; index < 4; index++) {
		session->pointerbundle = bundles[index];
		session->pointer.sender = index == 1;
		if (FindPointerSymbol(session, relatives[index], &session->pointeroffset) && FindPointerSymbol(session, absolutes[index], &session->absoluteoffset)) {
			session->pointer.compactwheel = 0;
			if (!FindPointerSymbol(session, wheels[index], &session->wheeloffset)) {
				session->wheeloffset = 0;
				if (session->pointer.sender && FindPointerSymbol(session, "__ZN11IOHIDSystem21scrollWheelEventGatedEssslll12UnsignedWideP8OSObject", &session->wheeloffset))
					session->pointer.compactwheel = 1;
				else
					session->wheeloffset = 0;
			}
			return 1;
		}
	}
	session->pointeroffset = 0;
	return 0;
}

static void InstallPointerHook(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 text;
	uint32 physical;
	machine = session->machine;
	cpu = machine->cpu;
	if (session->pointerhooked || (cpu->msr & OSXMSRPR) || !(cpu->msr & OSXMSRDR))
		return;
	if (session->pointeroffset == 0 && !FindPointerSymbols(session)) {
		session->pointerhooked = -1;
		return;
	}
	text = 0;
	if (session->pointerbundle && !OsxKextTextAddress(machine, session->pointerbundle, &text))
		return;
	if (!OsxCpuTranslate(cpu, text + session->pointeroffset, OSXTLBREAD, 0, &physical))
		return;
	session->pointer.machine = machine;
	session->pointer.absoluteaddress = text + session->absoluteoffset;
	session->pointer.wheeladdress = 0;
	if (session->wheeloffset)
		session->pointer.wheeladdress = text + session->wheeloffset;
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
#if !SDL_VERSION_ATLEAST(2, 0, 0)
	if (button == SDL_BUTTON_WHEELUP)
		return 3;
	if (button == SDL_BUTTON_WHEELDOWN)
		return 4;
#endif
	return -1;
}
#if SDL_VERSION_ATLEAST(2, 0, 0)
static int HostWheelButton(int wheely)
{
	if (wheely > 0)
		return 3; /* up */
	if (wheely < 0)
		return 4; /* down */
	return -1;
}
#endif
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

static void ConvertPixels(uint32 *target, const uint8 *source, uint32 width, int shuffle)
{
	uint32 column;
	column = 0;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	if (shuffle) {
		__m128i order;
		__m128i pixels;
		order = _mm_setr_epi8(3, 2, 1, -128, 7, 6, 5, -128, 11, 10, 9, -128, 15, 14, 13, -128);
		for (; column + 4 <= width; column += 4) {
			pixels = _mm_loadu_si128((const __m128i *)(source + column * 4));
			pixels = _mm_shuffle_epi8(pixels, order);
			_mm_storeu_si128((__m128i *)(target + column), pixels);
		}
	}
#endif
	for (; column < width; column++)
		target[column] = ((uint32)source[column * 4 + 1] << 16) | ((uint32)source[column * 4 + 2] << 8) | source[column * 4 + 3];
}

static void PresentFrame(OsxSession *session)
{
	OsxMachine *machine;
	uint8 *source;
	uint8 *shadowrow;
	uint32 *target;
	uint32 row;
	uint32 rowbytes;
	uint32 runstart[OSXPRESENTRUNS];
	uint32 runend[OSXPRESENTRUNS];
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Rect rects[OSXPRESENTRUNS];
#endif
	int runs;
	int open;
	uint32 openrow;
	int index;
	machine = session->machine;
	rowbytes = machine->rowbytes;
	runs = 0;
	open = 0;
	openrow = 0;
	if (session->screen == NULL || session->screen->w < (int)machine->width || session->screen->h < (int)machine->height || SDL_LockSurface(session->screen) != 0)
		return;
	for (row = 0; row < machine->height; row++) {
		source = machine->vram + row * rowbytes;
		shadowrow = session->shadow + row * rowbytes;
		if (memcmp(source, shadowrow, machine->width * 4) == 0) {
			if (open) {
				if (runs < OSXPRESENTRUNS) {
					runstart[runs] = openrow;
					runend[runs] = row - 1;
					runs++;
				} else
					runend[runs - 1] = row - 1;
				open = 0;
			}
			continue;
		}
		memcpy(shadowrow, source, machine->width * 4);
#if OSXPROFILE
		session->tmprows++;
#endif
		target = (uint32 *)((uint8 *)session->screen->pixels + row * session->screen->pitch);
		ConvertPixels(target, source, machine->width, session->pixelshuffle);
		if (!open) {
			openrow = row;
			open = 1;
		}
	}
	if (open) {
		if (runs < OSXPRESENTRUNS) {
			runstart[runs] = openrow;
			runend[runs] = machine->height - 1;
			runs++;
		} else
			runend[runs - 1] = machine->height - 1;
	}
	SDL_UnlockSurface(session->screen);
#if OSXPROFILE
	session->tmpframes += runs != 0;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	for (index = 0; index < runs; index++) {
		rects[index].x = 0;
		rects[index].y = (int)runstart[index];
		rects[index].w = (int)machine->width;
		rects[index].h = (int)(runend[index] - runstart[index] + 1);
	}
	if (runs)
		SDL_UpdateWindowSurfaceRects(sdl_window, rects, runs);
#else
	for (index = 0; index < runs; index++)
		SDL_UpdateRect(session->screen, 0, runstart[index], machine->width, runend[index] - runstart[index] + 1);
#endif
}

static void HandleEvents(OsxSession *session, void *monitor)
{
	SDL_Event event;
	int code;
	while (SDL_PollEvent(&event)) {
#ifdef ENABLE_EMULATOR_MONITOR
		if (monitor) {
			EmulatorMonitorView view;
			view.inputfunction = MonitorInput;
			view.inputcontext = session;
			view.framebuffer = session->machine->vram;
			view.palette = NULL;
			view.window = NULL;
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
		#if SDL_VERSION_ATLEAST(2, 0, 0)
		case SDL_MOUSEWHEEL:
			PressButton(session, HostWheelButton(event.wheel.y), 1);
			break;
		case SDL_WINDOWEVENT:
			session->screen = SDL_GetWindowSurface(sdl_window);
			memset(session->shadow, 0xff, session->machine->rowbytes * session->machine->height);
			break;
		#endif
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

static int SyscallObserver(OsxCpu *cpu, void *context)
{
	OsxSession *session;
	uint32 key;
	uint32 identifier;
	uint32 frame;
	char text[160];
	int index;
#if OSXMSGTRACE
	uint32 header[6];
#endif
	session = (OsxSession *)context;
	if (OsxClipSyscall(session->machine, cpu))
		return 1;
	if (cpu->gpr[0] == 4 && session->machine->log && session->writelogs < 2000) {
		GuestString(cpu, cpu->gpr[4], text, sizeof(text));
		if (cpu->gpr[5] < sizeof(text))
			text[cpu->gpr[5]] = 0;
		fprintf(session->machine->log, "write fd %u: %s\n", cpu->gpr[3], text);
		session->writelogs++;
	}
#if OSXMSGTRACE
	if (session->msglog) {
		index = ((cpu->sr[0] * 31u) ^ (cpu->gpr[1] >> 14)) & 255;
		if (session->pending[index][0] == (cpu->sr[0] & 0xffffff) + 1 && session->pending[index][1] == cpu->gpr[1] >> 14) {
			for (frame = 0; frame < 6; frame++)
				if (!OsxCpuReadVirtual(cpu, session->pending[index][2] + frame * 4, &header[frame]))
					header[frame] = 0xffffffffu;
			fprintf(session->msglog, "%llu sr %06x recv bits %x size %u remote %x local %x id %d\n", (unsigned long long)OsxCpuTimebase(cpu), cpu->sr[0] & 0xffffff, header[0], header[1], header[2], header[3], (int32)header[5]);
			session->pending[index][0] = 0;
		}
		if ((int32)cpu->gpr[0] == -31 && (cpu->gpr[4] & 2)) {
			session->pending[index][0] = (cpu->sr[0] & 0xffffff) + 1;
			session->pending[index][1] = cpu->gpr[1] >> 14;
			session->pending[index][2] = cpu->gpr[3];
		}
	}
#endif
	key = cpu->gpr[0] << 16;
	if ((int32)cpu->gpr[0] == -31 && OsxCpuReadVirtual(cpu, cpu->gpr[3] + 20, &identifier)) {
		key = 0xffe10000u | (identifier & 0xffff);
		if (session->msglog && OsxCpuReadVirtual(cpu, cpu->gpr[3] + 8, &frame)) {
			fprintf(session->msglog, "%llu sr %06x lr %08x option %x id %d remote %x size %u rsize %u rname %x timeout %u bt", (unsigned long long)OsxCpuTimebase(cpu), cpu->sr[0] & 0xffffff, cpu->lr, cpu->gpr[4], identifier, frame, cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
			frame = cpu->gpr[1];
			for (index = 0; index < 12; index++) {
				if (!OsxCpuReadVirtual(cpu, frame, &frame) || frame == 0 || !OsxCpuReadVirtual(cpu, frame + 8, &identifier))
					break;
				fprintf(session->msglog, " %08x", identifier);
			}
			fprintf(session->msglog, "\n");
		}
	}
#if OSXMSGTRACE
	else if (session->msglog) {
		fprintf(session->msglog, "%llu sr %06x lr %08x syscall %d args %x %x %x %x bt", (unsigned long long)OsxCpuTimebase(cpu), cpu->sr[0] & 0xffffff, cpu->lr, (int32)cpu->gpr[0], cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
		frame = cpu->gpr[1];
		for (index = 0; index < 12; index++) {
			if (!OsxCpuReadVirtual(cpu, frame, &frame) || frame == 0 || !OsxCpuReadVirtual(cpu, frame + 8, &identifier))
				break;
			fprintf(session->msglog, " %08x", identifier);
		}
		if (cpu->gpr[0] == 59) {
			GuestString(cpu, cpu->gpr[3], text, sizeof(text));
			fprintf(session->msglog, " exec %s", text);
		}
		fprintf(session->msglog, "\n");
	}
#endif
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
			return 0;
		}
	}
	for (index = 0; index < 64; index++) {
		if (session->syscallcounts[index] == 0) {
			session->syscallkeys[index] = key;
			session->syscallcounts[index] = 1;
			return 0;
		}
	}
	return 0;
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
#if OSXMSGTRACE
	uint64 before;
#endif
	machine = session->machine;
	if (machine->log)
		fprintf(machine->log, "script %llu host %u %s", (unsigned long long)OsxCpuTimebase(machine->cpu), (unsigned)SDL_GetTicks(), command);
	if (sscanf(command, "mouse %d %d", &first, &second) == 2)
		MovePointer(session, first, second);
	else if (sscanf(command, "down %d", &first) == 1)
		PressButton(session, first, 1);
	else if (sscanf(command, "up %d", &first) == 1)
		PressButton(session, first, 0);
	else if (sscanf(command, "wheel %d", &first) == 1)
		OsxCudaWheel(machine, first);
#if OSXMSGTRACE
	else if (sscanf(command, "det %d", &first) == 1 && first > 0 && machine->cpu->deterministic) {
		before = OsxCpuTimebase(machine->cpu);
		machine->cpu->deterministic = (uint32)first;
		machine->cpu->virtualticks += before - OsxCpuTimebase(machine->cpu);
	}
#endif
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

#if OSXPROFILE
static void TmpDumpForms(FILE *log, OsxCpu *cpu)
{
	static const char *const names[4] = { "callout", "dispatch", "sampled", "generic" };
	uint32 *counts;
	uint32 best;
	uint32 total;
	int path;
	int rank;
	int index;
	int chosen;
	for (path = 0; path < 4; path++) {
		counts = cpu->tmpform[path];
		total = 0;
		for (index = 0; index < 131072; index++)
			total += counts[index];
		fprintf(log, "TMPFORMS %s total %u", names[path], total);
		for (rank = 0; rank < 40; rank++) {
			best = 0;
			chosen = -1;
			for (index = 0; index < 131072; index++)
				if (counts[index] > best) {
					best = counts[index];
					chosen = index;
				}
			if (chosen < 0)
				break;
			fprintf(log, " %d/%d:%u", chosen >> 11, chosen & 2047, best);
			counts[chosen] = 0;
		}
		fprintf(log, "\n");
		memset(counts, 0, sizeof(cpu->tmpform[path]));
	}
	fprintf(log, "TMPEXCEPTIONS");
	for (index = 0; index < 64; index++)
		if (cpu->tmpexception[index])
			fprintf(log, " %03x:%u", index << 8, cpu->tmpexception[index]);
	fprintf(log, "\n");
	memset(cpu->tmpexception, 0, sizeof(cpu->tmpexception));
}
#endif

static void RunMachine(OsxSession *session, void *monitor)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint64 now;
	uint64 nextframe;
	uint64 frameticks;
	uint64 nextevent;
	uint64 eventticks;
	uint64 waitticks;
	uint64 cudadeadline;
	uint64 bmacdeadline;
	uint64 atadeadline;
	uint32 milliseconds;
	uint64 nextstatus;
	uint64 runstart;
	uint64 runtime;
	uint64 slices;
	uint64 stoptime;
	uint64 runinstructions;
	uint64 sliceticks;
	int32 slicesize;
	int32 seconds;
#if OSXPROFILE
	uint32 tmpword;
	uint32 tmprecord[3];
#endif
	machine = session->machine;
	cpu = machine->cpu;
	frameticks = OSXTIMEBASEFREQUENCY / OSXFRAMERATE;
	eventticks = OSXTIMEBASEFREQUENCY / 500;
	nextframe = 0;
	nextevent = 0;
	nextstatus = OSXTIMEBASEFREQUENCY;
	runtime = 0;
	slices = 0;
	seconds = PrefsFindInt32("osxseconds");
	stoptime = OSXNEVER;
	if (seconds > 0)
		stoptime = (uint64)seconds * OSXTIMEBASEFREQUENCY;
	now = OsxCpuTimebase(cpu);
	slicesize = 20000;
	while (!machine->quit) {
#if OSXG5
		if (cpu->core970)
			Osx970CheckInterrupts((Osx970 *)cpu->core970, now);
		else
			OsxCpuCheckInterrupts(cpu, now);
#else
		OsxCpuCheckInterrupts(cpu, now);
#endif
		if (cpu->halted) {
			waitticks = frameticks;
			if (nextevent <= now)
				waitticks = 0;
			else if (nextevent - now < waitticks)
				waitticks = nextevent - now;
			if (cpu->decdeadline != OSXNEVER && cpu->decdeadline > now && cpu->decdeadline - now < waitticks)
				waitticks = cpu->decdeadline - now;
			cudadeadline = OsxCudaNextDeadline(machine);
			bmacdeadline = OsxBmacNextDeadline(machine);
			if (bmacdeadline < cudadeadline)
				cudadeadline = bmacdeadline;
			atadeadline = 0;
			if (machine->ata[0].irqdeadline)
				atadeadline = machine->ata[0].irqdeadline;
			if (machine->ata[1].irqdeadline && (atadeadline == 0 || machine->ata[1].irqdeadline < atadeadline))
				atadeadline = machine->ata[1].irqdeadline;
			if (cudadeadline <= now)
				waitticks = 0;
			else if (cudadeadline - now < waitticks)
				waitticks = cudadeadline - now;
			if (atadeadline != 0) {
				if (atadeadline <= now)
					waitticks = 0;
				else if (atadeadline - now < waitticks)
					waitticks = atadeadline - now;
			}
			milliseconds = (uint32)(waitticks * 1000 / OSXTIMEBASEFREQUENCY);
			if (cpu->deterministic)
				cpu->virtualticks += waitticks + 1;
			else if (milliseconds > 0)
				SDL_Delay(milliseconds);
			now = OsxCpuTimebase(cpu);
		} else {
			runstart = now;
			runinstructions = cpu->instructions;
#if OSXG5
			if (cpu->core970)
				Osx970Run((Osx970 *)cpu->core970, slicesize);
			else
				OsxJitRun(cpu, slicesize);
#else
			OsxJitRun(cpu, slicesize);
#endif
			if (session->slicelog)
				LogSlice(session->slicelog, cpu);
			now = OsxCpuTimebase(cpu);
			if (!cpu->deterministic && cpu->instructions - runinstructions >= (uint32)slicesize && now > runstart) {
				sliceticks = (cpu->instructions - runinstructions) * (OSXTIMEBASEFREQUENCY / 4000) / (now - runstart);
				if (sliceticks < 256)
					sliceticks = 256;
				if (sliceticks > OSXSLICE)
					sliceticks = OSXSLICE;
				slicesize = (int32)sliceticks;
			}
			runtime += now - runstart;
			if (session->samplecount < 8192)
				session->samples[session->samplecount++] = cpu->pc | ((cpu->msr >> 14) & 1);
#if OSXPROFILE
			if (!cpu->halted && OsxCpuReadVirtual(cpu, cpu->pc, &tmpword))
				cpu->tmpform[2][OsxCpuTmpFormKey(tmpword)]++;
			if (session->tmpsamplefile) {
				tmprecord[0] = cpu->pc;
				tmprecord[1] = cpu->sr[0] & 0xffffff;
				tmprecord[2] = ((cpu->msr >> 14) & 1) | ((uint32)cpu->halted << 1) | ((uint32)(OsxCpuTimebase(cpu) / (OSXTIMEBASEFREQUENCY / 1000)) << 2);
				fwrite(tmprecord, sizeof(tmprecord), 1, session->tmpsamplefile);
			}
#endif
			slices++;
		}
		OsxCudaPoll(machine, now);
		OsxBmacPoll(machine, now);
		OsxAtaPoll(machine, now);
		OsxAudioPoll(machine, now);
		RunScript(session, now);
		if (now >= nextevent) {
			nextevent = now + eventticks;
			HandleEvents(session, monitor);
		}
		if (now >= nextframe) {
			nextframe = now + frameticks;
			PresentFrame(session);
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
#if OSXPROFILEJIT
				OsxJitTmpStats(cpu, machine->log);
#endif
#if OSXPROFILE
				TmpDumpForms(machine->log, cpu);
				fprintf(machine->log, "TMPRENDER frames %u rows %u\n", session->tmpframes, session->tmprows);
				session->tmpframes = 0;
				session->tmprows = 0;
				if (session->tmpsamplefile)
					fflush(session->tmpsamplefile);
#endif
				fflush(machine->log);
			}
		}
		if (now >= stoptime)
			machine->quit = 1;
	}
}

static uint8 *AllocateMemory(size_t size)
{
#ifdef _WIN32
	return (uint8 *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
	return (uint8 *)calloc(1, size);
#endif
}

static void ReleaseMemory(uint8 *memory)
{
#ifdef _WIN32
	VirtualFree(memory, 0, MEM_RELEASE);
#else
	free(memory);
#endif
}

static void DestroySession(OsxSession *session)
{
	OsxMachine *machine;
	int busindex;
	int unitindex;
	machine = session->machine;
	if (machine) {
		OsxAudioExit(machine);
		if (machine->cpu) {
			OsxJitDestroy(machine->cpu);
#if OSXG5
			if (machine->cpu->core970) {
				Osx970JitDestroy((Osx970 *)machine->cpu->core970);
				free(machine->cpu->core970);
			}
#endif
			if (machine->cpu->watchfile)
				fclose(machine->cpu->watchfile);
			free(machine->cpu);
		}
		for (busindex = 0; busindex < OSXATABUSES; busindex++) {
			for (unitindex = 0; unitindex < 2; unitindex++) {
				if (machine->ata[busindex].drives[unitindex].file)
					Sys_close(machine->ata[busindex].drives[unitindex].file);
			}
			free(machine->ata[busindex].buffer);
		}
		free(machine->ata[0].patches);
		free(machine->kernelimage);
		free(machine->mkext);
		ReleaseMemory(machine->ram);
		ReleaseMemory(machine->vram);
		if (machine->log)
			fclose(machine->log);
		if (machine->console)
			fclose(machine->console);
		free(machine);
	}
	if (session->script)
		fclose(session->script);
	if (session->slicelog)
		fclose(session->slicelog);
	if (session->msglog)
		fclose(session->msglog);
	if (session->trace)
		fclose(session->trace);
#if OSXPROFILE
	if (session->tmpsamplefile)
		fclose(session->tmpsamplefile);
#endif
	free(session->shadow);
	if (session->sdlready)
		SDL_Quit();
	if (session->nvramready)
		XPRAMExit();
}

static int RunSession(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	const char *bootimage;
	const char *bootargs;
	int bootbus;
	int bootunit;
	FILE *dump;
	void *monitor;
	int32 ramsize;
#if OSXG5
	Osx970 *core;
#endif
	machine = (OsxMachine *)calloc(1, sizeof(OsxMachine));
	session->machine = machine;
	if (machine == NULL)
		return 1;
	cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	machine->cpu = cpu;
	if (cpu == NULL)
		return 1;
	machine->log = fopen("osxguest.log", "w");
	machine->console = fopen("osxconsole.log", "w");
	ramsize = PrefsFindInt32("ramsize");
	if (ramsize > 0 && ramsize <= 1024)
		ramsize *= 1024 * 1024;
	if (ramsize < 128 * 1024 * 1024)
		ramsize = OSXDEFAULTRAM;
	machine->ramsize = (uint32)ramsize & ~0xfffffu;
#if OSXG5
	if (PrefsFindString("osxmodel") && strcmp(PrefsFindString("osxmodel"), "PowerMac7,3") == 0 && PrefsFindInt32("ramsize") > 2048 && PrefsFindInt32("ramsize") <= 4096) {
		machine->ramsize = 0x80000000u;
		machine->g5state.highsize = (uint32)(PrefsFindInt32("ramsize") - 2048) << 20;
	}
	machine->ram = AllocateMemory((size_t)machine->ramsize + machine->g5state.highsize);
#else
	machine->ram = AllocateMemory(machine->ramsize);
#endif
	machine->vram = AllocateMemory(OSXVRAMSIZE);
	if (machine->ram == NULL || machine->vram == NULL) {
		fprintf(stderr, "osx: cannot allocate guest memory\n");
		return 1;
	}
	ParseScreen(machine);
	OsxCpuReset(cpu, OSXPVR, OSXTIMEBASEFREQUENCY);
	if (PrefsFindInt32("osxdeterministic") > 0) {
		cpu->deterministic = (uint32)PrefsFindInt32("osxdeterministic");
		session->slicelog = fopen("osxslices.log", "w");
	}
	if (PrefsFindString("osxinputscript")) {
		session->script = fopen(PrefsFindString("osxinputscript"), "r");
		if (session->script)
			NextScriptLine(session);
	}
	cpu->ram = machine->ram;
	cpu->ramsize = machine->ramsize;
#if OSXG5
	cpu->highram = machine->g5state.highsize;
#endif
	cpu->vram = machine->vram;
	cpu->vrambase = OSXVRAMBASE;
	cpu->vramsize = OSXVRAMSIZE;
	cpu->buscontext = machine;
	cpu->busread = OsxBusRead;
	cpu->buswrite = OsxBusWrite;
	cpu->logfile = machine->log;
#if OSXG5
	if (PrefsFindString("osxmodel") && strcmp(PrefsFindString("osxmodel"), "PowerMac7,3") == 0)
		machine->g5 = 1;
#endif
	OsxPciReset(machine);
	OsxAtaReset(machine);
	OsxCudaReset(machine);
	OsxBmacReset(machine);
	OsxAudioReset(machine);
#if OSXG5
	if (machine->g5)
		OsxG5Reset(machine);
#endif
	XPRAMInit(NULL);
	session->nvramready = 1;
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
	AllowStalledClassLookup(machine);
	if (OsxFindKernelSymbol(machine, "_conslog_putc"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_conslog_putc"), ConsoleHook, session);
	if (OsxFindKernelSymbol(machine, "_kmputc"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kmputc"), TerminalHook, session);
	if (OsxFindKernelSymbol(machine, "_kprintf"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kprintf"), KprintfHook, session);
	if (OsxFindKernelSymbol(machine, "_trap"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_trap"), TrapHook, session);
	if (PrefsFindString("osxtracestart") && OsxFindKernelSymbol(machine, PrefsFindString("osxtracestart")))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, PrefsFindString("osxtracestart")), TraceStartHook, session);
	if (PrefsFindString("osxtracestop") && OsxFindKernelSymbol(machine, PrefsFindString("osxtracestop")))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, PrefsFindString("osxtracestop")), TraceStopHook, session);
	InstallCallHooks(session);
	if (PrefsFindString("osxlogxml"))
		OsxCpuAddHook(cpu, (uint32)strtoul(PrefsFindString("osxlogxml"), NULL, 16), XmlHook, session);
	ParseKextHooks(session);
	if (OsxFindKernelSymbol(machine, "_kmod_start_or_stop"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_kmod_start_or_stop"), KmodStartHook, session);
	session->backtracekey = 0xffe10afeu;
	if (PrefsFindString("osxbacktracekey"))
		session->backtracekey = (uint32)strtoul(PrefsFindString("osxbacktracekey"), NULL, 16);
	cpu->syscallobserver = SyscallObserver;
	cpu->syscallcontext = session;
	if (OsxFindKernelSymbol(machine, "_panic"))
		OsxCpuAddHook(cpu, OsxFindKernelSymbol(machine, "_panic"), PanicHook, session);
	memcpy(machine->nvramclassic, machine->nvram, OSXNVRAMSIZE);
	OsxNvramMakeNewWorld(machine->nvram);
#if OSXG5
	if (machine->g5)
		OsxG5PrepareNvram(machine);
#endif
	HookKernelSymbol(machine, "__ZN16IOPlatformExpert14getBootROMTypeEv", NewWorldEpochHook);
	HookKernelSymbol(machine, "_getBootROMType__16IOPlatformExpert", NewWorldEpochHook);
	HookKernelSymbol(machine, "_sleepSystem__14IOPMrootDomain", RefuseSleepHook);
	HookKernelSymbol(machine, "_handleSleepTimerExpiration__14IOPMrootDomain", ReturnHook);
	HookKernelSymbol(machine, "_sleepKernel__16IOPlatformExpert", ReturnHook);
	HookKernelSymbol(machine, "_PE_Determine_Clock_Speeds", ReturnHook);
#if OSXG5
	if (machine->pvr == OSX970PVR)
		HookKernelSymbol(machine, "_machine_idle", NapHook);
#endif
	if (machine->pvr == OSXPVRG4 || machine->ata[bootbus].drives[bootunit].atapi)
		HookKernelSymbol(machine, "_PEGetPlatformEpoch", NewWorldEpochHook);
	cpu->pc = machine->kernelentry;
	cpu->msr = 0;
	cpu->gpr[1] = machine->scratch + 0x8000;
	cpu->gpr[3] = machine->bootargs;
	cpu->gpr[4] = 0x426f6f58;
	OsxCpuUpdateModes(cpu);
#if OSXG5
	if (machine->pvr == OSX970PVR) {
		core = (Osx970 *)calloc(1, sizeof(Osx970));
		if (core == NULL)
			return 1;
		Osx970Reset(core, cpu, OSX970PVR);
		core->pc = machine->kernelentry;
		core->nextpc = core->pc + 4;
		core->msr = OSX970MSRHV;
		core->gpr[1] = machine->scratch + 0x8000;
		core->gpr[3] = machine->bootargs;
		core->gpr[4] = 0x426f6f58;
		cpu->core970 = core;
		if (PrefsFindBool("osxjit") && Osx970JitCreate(core))
			fprintf(machine->log, "cpu 970 jit\n");
		else
			fprintf(machine->log, "cpu 970 interpreter\n");
	} else if (PrefsFindBool("osxjit")) {
#else
	if (PrefsFindBool("osxjit")) {
#endif
		if (OsxJitCreate(cpu))
			fprintf(machine->log, "cpu native x64 soft TLB\n");
		else
			fprintf(machine->log, "cpu interpreter\n");
	}
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "osx: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	session->sdlready = 1;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	session->pixelshuffle = IsProcessorFeaturePresent(36) != 0;
#endif
#if !SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_WM_SetCaption("SheepShaver - Mac OS X", "SheepShaver");
#endif
	session->screen = SDLSetVideoModeQuietly((int)machine->width, (int)machine->height, 32, OSXVIDEOFLAGS);
	if (session->screen == NULL) {
		fprintf(stderr, "osx: SDL_SetVideoMode failed: %s\n", SDL_GetError());
		return 1;
	}
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetWindowTitle(sdl_window, "SheepShaver - Mac OS X");
#endif
	SDL_ShowCursor(SDL_DISABLE);
	session->shadow = (uint8 *)malloc(machine->rowbytes * machine->height);
	if (session->shadow == NULL)
		return 1;
	memset(session->shadow, 0xff, machine->rowbytes * machine->height);
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
#if OSXPROFILE
	session->tmpsamplefile = fopen("osxsamples.bin", "wb");
#endif
	RunMachine(session, monitor);
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	OsxNvramFinishBoot(machine, XPRAM + 0x1378);
	OsxNvramRestoreOldWorld(machine->nvram, machine->nvramclassic);
	memcpy(XPRAM, machine->nvram, OSXNVRAMSIZE);
	OsxFlashSave(machine);
	if (machine->log)
		OsxCpuDump(cpu, machine->log);
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
	if (machine->quit == 2)
		return 2;
	return 0;
}

int OsxGuestMain(void)
{
	OsxSession session;
	int result;
	memset(&session, 0, sizeof(session));
	result = RunSession(&session);
	DestroySession(&session);
	return result;
}
