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
#include "osxhost.h"
#include "main.h"
#include "sys.h"
#include "prefs.h"
#include "osxmachine.h"
#include "xpram.h"
#include "osxguest.h"
#include "osxglbridge.h"
#if OSXGLBRIDGE
#include "metal_compositor.h"
#include "osxglengine.h"
#endif
#if OSXHOSTX64
#include <tmmintrin.h>
#endif
#if OSXHOSTX64 && !defined(_MSC_VER)
#define OSXSSSE3 __attribute__((target("ssse3")))
#else
#define OSXSSSE3
#endif

#define OSXSLICE 262144

#define OSXDEFAULTRAM (256 * 1024 * 1024)
#define OSXFRAMERATE 60
#define OSXSETTLEMS 3000
#define OSXAMPSETTLEMS 225

typedef struct OsxSession {
	OsxMachine *machine;
	OsxHostVideo *video;
#if OSXGLBRIDGE
	OsxGlBridge *gl;
	OsxQuartzBridge *quartz;
	uint8 *glpresent;
	uint32 glbase;
	uint32 glsegment;
#endif
	uint8 *shadow;
	int nvramready;
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
	return OsxGuestSelectBootImage(&diskindex, &partition) == OSXBOOTNATIVE;
}

static void OsxGuestConsoleCharacter(OsxSession *session, int character);

static void OsxGuestConsoleHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxGuestConsoleCharacter((OsxSession *)context, (int)(cpu->gpr[3] & 0xff));
}

static void OsxGuestTerminalHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxGuestConsoleCharacter((OsxSession *)context, (int)(cpu->gpr[3] & 0xff));
}

static void OsxGuestConsoleCharacter(OsxSession *session, int character)
{
	OsxMachine *machine;
	machine = session->machine;
	if (character == '\n' || session->consolelength >= (int)sizeof(session->consoleline) - 1) {
		session->consoleline[session->consolelength] = 0;
		fprintf(stdout, "%s\n", session->consoleline);
		fflush(stdout);
		if (machine->console) {
			fprintf(machine->console, "[%6.2f] %s\n", (double)OsxGuestCpuTimebase(machine->cpu) / OSXTIMEBASEFREQUENCY, session->consoleline);
			fflush(machine->console);
		}
		session->consolelength = 0;
		return;
	}
	if (character != '\r')
		session->consoleline[session->consolelength++] = (char)character;
}

static void OsxGuestString(OsxCpu *cpu, uint32 address, char *output, int length)
{
	uint32 word;
	int index;
	char character;
	for (index = 0; index < length - 1; index++) {
		if (!OsxGuestCpuReadVirtual(cpu, (address + index) & ~3u, &word))
			break;
		character = (char)(word >> (24 - ((address + index) & 3) * 8));
		if (character == 0)
			break;
		output[index] = character;
	}
	output[index] = 0;
}

static void OsxGuestFormatGuest(OsxMachine *machine, OsxCpu *cpu, char *output, int length)
{
	char format[512];
	char piece[256];
	char specifier[16];
	int used;
	int argument;
	int index;
	int width;
	uint32 value;
	OsxGuestString(cpu, cpu->gpr[3], format, sizeof(format));
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
			OsxGuestString(cpu, value, piece, 128);
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

static void OsxGuestTrapHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	uint32 *entry;
	session = (OsxSession *)context;
	entry = session->traps[session->trapcount & 31];
	entry[0] = cpu->gpr[3];
	entry[1] = 0;
	entry[2] = 0;
	OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 0x100, &entry[1]);
	OsxGuestCpuReadVirtual(cpu, cpu->gpr[4] + 0x104, &entry[2]);
	entry[3] = cpu->gpr[5];
	entry[4] = cpu->gpr[6];
	session->trapcount++;
}

static void OsxGuestTraceStartHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	session = (OsxSession *)context;
	if (session->trace == NULL)
		session->trace = fopen("osxtrace.log", "w");
	cpu->tracefile = session->trace;
}

static void OsxGuestTraceStopHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->tracefile = NULL;
}

static void OsxGuestNewWorldEpochHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->gpr[3] = 1;
	cpu->nextpc = cpu->lr;
}

static void OsxGuestRefuseSleepHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->gpr[3] = 0;
	cpu->nextpc = cpu->lr;
}

static void OsxGuestReturnHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->nextpc = cpu->lr;
}

#if OSXG5
static void OsxGuestNapHook(OsxCpu *cpu, void *context, uint32 address)
{
	cpu->msr |= OSXMSREE;
	cpu->nextpc = cpu->lr;
	if (!cpu->externalline && !cpu->decpending) {
		cpu->halted = 1;
		cpu->slice = 0;
	}
}
#endif

static void OsxGuestAllowStalledClassLookup(OsxMachine *machine)
{
	uint8 *code;
	uint32 address;
	uint32 index;
	address = OsxGuestFindKernelSymbol(machine, "_getMetaClassWithName__11OSMetaClassPC8OSSymbol");
	if (address == 0 || address + 0x100 > machine->ramsize)
		return;
	code = machine->ram + address;
	for (index = 4; index < 0x100 && memcmp(code + index - 4, "\x4e\x80\x00\x20", 4) != 0; index += 4)
		if ((code[index - 4] >> 2) == 16 && memcmp(code + index, "\x3b\xa0\x00\x00", 4) == 0)
			memcpy(code + index, "\x60\x00\x00\x00", 4);
}

static void OsxGuestHookKernelSymbol(OsxMachine *machine, const char *name, OsxHookFunction function)
{
	if (OsxGuestFindKernelSymbol(machine, name))
		OsxGuestCpuAddHook(machine->cpu, OsxGuestFindKernelSymbol(machine, name), function, NULL);
}

static void OsxGuestSettleHook(OsxCpu *cpu, void *context, uint32 address)
{
	if ((cpu->gpr[3] == OSXSETTLEMS || cpu->gpr[3] == OSXAMPSETTLEMS) && cpu->lr >= ((OsxMachine *)context)->kernelend)
		cpu->gpr[3] = 0;
}



static void OsxGuestCallHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char text[64];
	uint32 word;
	int index;
	session = (OsxSession *)context;
	for (index = 0; index < session->callcount; index++) {
		if (session->calladdresses[index] != address)
			continue;
		OsxGuestString(cpu, cpu->gpr[10], text, sizeof(text));
		word = 0;
		OsxGuestCpuReadVirtual(cpu, cpu->gpr[3] + 0x28, &word);
		if (session->machine->log)
			fprintf(session->machine->log, "call %s [%s] state %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x lr %08x\n", session->callnames[index], text, word, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->lr);
	}
}

static void OsxGuestInstallCallHooks(OsxSession *session)
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
		address = OsxGuestFindKernelSymbol(session->machine, name);
		if (address == 0)
			continue;
		strcpy(session->callnames[session->callcount], name);
		session->calladdresses[session->callcount] = address;
		session->callcount++;
		OsxGuestCpuAddHook(session->machine->cpu, address, OsxGuestCallHook, session);
	}
}

static void OsxGuestParseKextHooks(OsxSession *session)
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

static void OsxGuestInstallKextHooks(OsxSession *session)
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
		if (!OsxGuestKextSymbolOffset(machine, session->kextbundles[index], session->kextsymbols[index], &offset)) {
			session->kextstates[index] = -1;
			continue;
		}
		if (!OsxGuestKextTextAddress(machine, session->kextbundles[index], &text))
			continue;
		if (!OsxGuestCpuTranslate(cpu, text + offset + session->kextoffsets[index], OSXTLBREAD, 0, &physical))
			continue;
		sprintf(session->callnames[session->callcount], "%.40s+%x", session->kextsymbols[index], session->kextoffsets[index]);
		session->calladdresses[session->callcount] = physical;
		session->callcount++;
		OsxGuestCpuAddHook(cpu, physical, OsxGuestCallHook, session);
		session->kextstates[index] = 1;
	}
}

static void OsxGuestInstallPowerHook(OsxSession *session);

static void OsxGuestKmodStartHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxGuestInstallPowerHook((OsxSession *)context);
	OsxGuestInstallKextHooks((OsxSession *)context);
}

static void OsxGuestXmlHook(OsxCpu *cpu, void *context, uint32 address)
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
		if (!OsxGuestCpuReadVirtual(cpu, (cpu->gpr[3] + index) & ~3u, &word))
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

static void OsxGuestPanicHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char message[1024];
	int index;
	session = (OsxSession *)context;
	OsxGuestFormatGuest(session->machine, cpu, message, sizeof(message));
	fprintf(stdout, "PANIC: %s\n", message);
	if (session->machine->log) {
		fprintf(session->machine->log, "PANIC: %s lr %08x\n", message, cpu->lr);
		OsxGuestCpuDumpFaults(cpu, session->machine->log);
		for (index = session->trapcount - 32; index < session->trapcount; index++)
			if (index >= 0)
				fprintf(session->machine->log, "trap %x srr0 %08x srr1 %08x dsisr %08x dar %08x\n", session->traps[index & 31][0], session->traps[index & 31][1], session->traps[index & 31][2], session->traps[index & 31][3], session->traps[index & 31][4]);
		fflush(session->machine->log);
	}
}

static void OsxGuestKprintfHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxSession *session;
	char message[1024];
	session = (OsxSession *)context;
	OsxGuestFormatGuest(session->machine, cpu, message, sizeof(message));
	if (session->machine->console) {
		fprintf(session->machine->console, "kprintf: %s", message);
		fflush(session->machine->console);
	}
}

static int OsxGuestFindPointerSymbol(OsxSession *session, const char *name, uint32 *offset)
{
	if (session->pointerbundle)
		return OsxGuestKextSymbolOffset(session->machine, session->pointerbundle, name, offset);
	*offset = OsxGuestFindKernelSymbol(session->machine, name);
	return *offset != 0;
}

static int OsxGuestFindPointerSymbols(OsxSession *session)
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
		if (OsxGuestFindPointerSymbol(session, relatives[index], &session->pointeroffset) && OsxGuestFindPointerSymbol(session, absolutes[index], &session->absoluteoffset)) {
			session->pointer.compactwheel = 0;
			if (!OsxGuestFindPointerSymbol(session, wheels[index], &session->wheeloffset)) {
				session->wheeloffset = 0;
				if (session->pointer.sender && OsxGuestFindPointerSymbol(session, "__ZN11IOHIDSystem21scrollWheelEventGatedEssslll12UnsignedWideP8OSObject", &session->wheeloffset))
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

static void OsxGuestInstallPointerHook(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint32 text;
	uint32 physical;
	machine = session->machine;
	cpu = machine->cpu;
	if (session->pointerhooked || (cpu->msr & OSXMSRPR) || !(cpu->msr & OSXMSRDR))
		return;
	if (session->pointeroffset == 0 && !OsxGuestFindPointerSymbols(session)) {
		session->pointerhooked = -1;
		return;
	}
	text = 0;
	if (session->pointerbundle && !OsxGuestKextTextAddress(machine, session->pointerbundle, &text))
		return;
	if (!OsxGuestCpuTranslate(cpu, text + session->pointeroffset, OSXTLBREAD, 0, &physical))
		return;
	session->pointer.machine = machine;
	session->pointer.absoluteaddress = text + session->absoluteoffset;
	session->pointer.wheeladdress = 0;
	if (session->wheeloffset)
		session->pointer.wheeladdress = text + session->wheeloffset;
	OsxGuestCpuAddHook(cpu, physical, OsxGuestPointerHook, &session->pointer);
	machine->cuda.mouseabsolute = 1;
	session->pointerhooked = 1;
	if (machine->log)
		fprintf(machine->log, "pointer hook at %08x physical %08x\n", text + session->pointeroffset, physical);
}

static void OsxGuestPowerRegisterHook(OsxCpu *cpu, void *context, uint32 address)
{
	if (cpu->gpr[4] == cpu->gpr[5])
		cpu->gpr[4] = 0;
}

static void OsxGuestInstallPowerHook(OsxSession *session)
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
	if (!OsxGuestKextSymbolOffset(machine, "com.apple.driver.AppleGossamerPE", "__ZN10GossamerPE16PMRegisterDeviceEP9IOServiceS1_", &offset)) {
		session->powerhooked = -1;
		return;
	}
	if (!OsxGuestKextTextAddress(machine, "com.apple.driver.AppleGossamerPE", &text) || !OsxGuestCpuTranslate(cpu, text + offset, OSXTLBREAD, 0, &physical))
		return;
	OsxGuestCpuAddHook(cpu, physical, OsxGuestPowerRegisterHook, NULL);
	session->powerhooked = 1;
}

static void OsxGuestMovePointer(OsxSession *session, int x, int y)
{
	OsxGuestCudaMouse(session->machine, x - session->hostx, y - session->hosty, session->buttons & 1);
	session->hostx = x;
	session->hosty = y;
}

static void OsxGuestPressButton(OsxSession *session, int button, int down)
{
	if (button == 3 || button == 4) {
		if (down && button == 3)
			OsxGuestCudaWheel(session->machine, 1);
		else if (down)
			OsxGuestCudaWheel(session->machine, -1);
		return;
	}
	if (button < 0 || button > 2)
		return;
	if (down)
		session->buttons |= 1 << button;
	else
		session->buttons &= ~(1 << button);
	OsxGuestCudaMouse(session->machine, 0, 0, session->buttons);
}

static void OsxGuestHostInput(void *context, int operation, int first, int second)
{
	OsxSession *session = (OsxSession *)context;
	switch (operation) {
	case OSXHOSTQUIT:
		session->machine->quit = 1;
		break;
	case OSXHOSTMOUSE:
		OsxGuestMovePointer(session, first, second);
		break;
	case OSXHOSTBUTTON:
		OsxGuestPressButton(session, first, second);
		break;
	case OSXHOSTWHEEL:
		OsxGuestCudaWheel(session->machine, first);
		break;
	case OSXHOSTKEY:
		OsxGuestCudaKey(session->machine, first, second);
		break;
	case OSXHOSTREDRAW:
		memset(session->shadow, 0xff, session->machine->rowbytes * session->machine->height);
		break;
	}
}

#if OSXHOSTX64
static OSXSSSE3 uint32 OsxGuestShufflePixels(uint32 *target, const uint8 *source, uint32 width)
{
	__m128i order;
	__m128i pixels;
	uint32 column;
	order = _mm_setr_epi8(3, 2, 1, -128, 7, 6, 5, -128, 11, 10, 9, -128, 15, 14, 13, -128);
	for (column = 0; column + 4 <= width; column += 4) {
		pixels = _mm_loadu_si128((const __m128i *)(source + column * 4));
		pixels = _mm_shuffle_epi8(pixels, order);
		_mm_storeu_si128((__m128i *)(target + column), pixels);
	}
	return column;
}
#endif

static void OsxGuestConvertPixels(uint32 *target, const uint8 *source, uint32 width, int shuffle)
{
	uint32 column;
	column = 0;
#if OSXHOSTX64
	if (shuffle)
		column = OsxGuestShufflePixels(target, source, width);
#endif
	for (; column < width; column++)
		target[column] = ((uint32)source[column * 4 + 1] << 16) | ((uint32)source[column * 4 + 2] << 8) | source[column * 4 + 3];
}

static void OsxGuestPresentFrame(OsxSession *session)
{
	OsxMachine *machine;
	const uint8 *source;
	uint8 *shadowrow;
	uint32 *target;
	uint32 row;
	uint32 rowbytes;
	uint32 runstart[OSXHOSTRUNS];
	uint32 runend[OSXHOSTRUNS];
	uint8 *pixels;
	int pitch;
	int raw;
	int runs;
	int open;
	uint32 openrow;
	int presented;
	machine = session->machine;
	rowbytes = machine->rowbytes;
#if OSXGLBRIDGE
	if (MetalCompositorIsInitialized()) {
		OsxGuestQuartzComposeWindows(session->quartz, machine->vram, session->glpresent, rowbytes * machine->height);
		OsxGl::OsxGuestEngineBeforePresent();
		MetalCompositorPresentWithoutVBL();
		OsxGl::OsxGuestEngineAfterPresent();
		return;
	}
#endif
	runs = 0;
	open = 0;
	openrow = 0;
	pixels = OsxGuestHostLock(session->video, &pitch, &raw);
	if (!pixels)
		return;
	for (row = 0; row < machine->height; row++) {
		source = machine->vram + row * rowbytes;
		shadowrow = session->shadow + row * rowbytes;
		if (memcmp(source, shadowrow, machine->width * 4) == 0) {
			if (open) {
				if (runs < OSXHOSTRUNS) {
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
		target = (uint32 *)(pixels + row * pitch);
		if (raw)
			memcpy(target, source, machine->width * 4);
		else
			OsxGuestConvertPixels(target, source, machine->width, session->pixelshuffle);
		if (!open) {
			openrow = row;
			open = 1;
		}
	}
	if (open) {
		if (runs < OSXHOSTRUNS) {
			runstart[runs] = openrow;
			runend[runs] = machine->height - 1;
			runs++;
		} else
			runend[runs - 1] = machine->height - 1;
	}
	presented = OsxGuestHostPresent(session->video, runstart, runend, runs);
#if OSXPROFILE
	session->tmpframes += presented;
#endif
}

static void OsxGuestParseScreen(OsxMachine *machine)
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

static void OsxGuestAttachDisks(OsxMachine *machine, const char **bootimage, int *bootbus, int *bootunit)
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
	OsxGuestSelectBootImage(&diskindex, &machine->bootpartition);
	used = 0;
	for (index = 0; (path = PrefsFindString("disk", index)) != NULL && used < 3; index++) {
		if (!OsxGuestAtaAttach(machine, diskslots[used] >> 1, diskslots[used] & 1, path, 0))
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
	cdromattached = path != NULL && OsxGuestAtaAttach(machine, 1, 0, path, 1);
	if (cdromattached && diskindex == -1) {
		*bootimage = path;
		*bootbus = 1;
		*bootunit = 0;
	}
}

static int OsxGuestSyscallObserver(OsxCpu *cpu, void *context, uint32 instruction)
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
#if OSXGLBRIDGE
	if (session->machine->quartz && (cpu->msr & OSXMSRPR) &&
		(instruction & 0xfffc00ffu) == OSXQUARTZTRAP) {
		if (!session->quartz)
			session->quartz = OsxGuestCreateQuartzBridge(session->machine->width, session->machine->height,
				session->machine->quartzbase, session->machine->quartzstrings, session->machine->quartzextensions, session->machine->quartzallocatedextensions,
				session->machine->clip.functions[OSXCLIPMALLOC],
				session->machine->clip.functions[OSXCLIPBZERO], session->machine->vram, session->machine->rowbytes, session->video, &session->glpresent, session->machine->quartzwindow);
		if (session->quartz && OsxGuestQuartzBridgeCall(session->quartz, cpu,
			session->machine->quartztails, (instruction >> 8) & 1023, session->machine->log))
			return 1;
	}
	if (session->machine->flurry && (cpu->msr & OSXMSRPR) &&
		(instruction & 0xffff00ffu) == OSXFLURRYTRAP) {
		uint32 call;
		uint32 stub;
		uint32 base;
		uint32 segment;
		uint32 next;
		int handled;
		call = (instruction >> 8) & 255;
		segment = cpu->sr[cpu->pc >> 28];
		if (!call) {
			float width;
			float height;
			base = cpu->pc - session->machine->flurryinit;
			memcpy(&width, &cpu->gpr[7], sizeof(width));
			memcpy(&height, &cpu->gpr[8], sizeof(height));
			if (!(base & 4095)) {
				if (width == session->machine->width && height == session->machine->height) {
					if (session->gl)
						OsxGuestDestroyGlBridge(session->gl);
					session->gl = OsxGuestCreateGlBridge(cpu, session->machine->width, session->machine->height, session->machine->vram, session->machine->rowbytes, session->video, &session->glpresent);
					session->glbase = base;
					session->glsegment = segment;
				}
				cpu->gpr[0] = cpu->lr;
				return 1;
			}
		} else if (call < sizeof(session->machine->flurrystubs) / sizeof(uint32)) {
			stub = session->machine->flurrystubs[call];
			base = cpu->pc - stub;
			if (stub && !(base & 4095)) {
				next = cpu->nextpc;
				handled = session->gl && base == session->glbase && segment == session->glsegment &&
					OsxGuestGlBridgeCall(session->gl, cpu, call);
				if (cpu->nextpc != next)
					return 1;
				if (handled) {
					cpu->nextpc = cpu->lr & ~3u;
					if (call == OSXFLURRYFLUSH)
						OsxGuestPresentFrame(session);
				} else
					cpu->nextpc = cpu->ctr & ~3u;
				return 1;
			}
		}
	}
#endif
	if (OsxGuestClipSyscall(session->machine, cpu))
		return 1;
	if (cpu->gpr[0] == 4 && session->machine->log && session->writelogs < 2000) {
		OsxGuestString(cpu, cpu->gpr[4], text, sizeof(text));
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
				if (!OsxGuestCpuReadVirtual(cpu, session->pending[index][2] + frame * 4, &header[frame]))
					header[frame] = 0xffffffffu;
			fprintf(session->msglog, "%llu sr %06x recv bits %x size %u remote %x local %x id %d\n", (unsigned long long)OsxGuestCpuTimebase(cpu), cpu->sr[0] & 0xffffff, header[0], header[1], header[2], header[3], (int32)header[5]);
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
	if ((int32)cpu->gpr[0] == -31 && OsxGuestCpuReadVirtual(cpu, cpu->gpr[3] + 20, &identifier)) {
		key = 0xffe10000u | (identifier & 0xffff);
		if (session->msglog && OsxGuestCpuReadVirtual(cpu, cpu->gpr[3] + 8, &frame)) {
			fprintf(session->msglog, "%llu sr %06x lr %08x option %x id %d remote %x size %u rsize %u rname %x timeout %u bt", (unsigned long long)OsxGuestCpuTimebase(cpu), cpu->sr[0] & 0xffffff, cpu->lr, cpu->gpr[4], identifier, frame, cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
			frame = cpu->gpr[1];
			for (index = 0; index < 12; index++) {
				if (!OsxGuestCpuReadVirtual(cpu, frame, &frame) || frame == 0 || !OsxGuestCpuReadVirtual(cpu, frame + 8, &identifier))
					break;
				fprintf(session->msglog, " %08x", identifier);
			}
			fprintf(session->msglog, "\n");
		}
	}
#if OSXMSGTRACE
	else if (session->msglog) {
		fprintf(session->msglog, "%llu sr %06x lr %08x syscall %d args %x %x %x %x bt", (unsigned long long)OsxGuestCpuTimebase(cpu), cpu->sr[0] & 0xffffff, cpu->lr, (int32)cpu->gpr[0], cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
		frame = cpu->gpr[1];
		for (index = 0; index < 12; index++) {
			if (!OsxGuestCpuReadVirtual(cpu, frame, &frame) || frame == 0 || !OsxGuestCpuReadVirtual(cpu, frame + 8, &identifier))
				break;
			fprintf(session->msglog, " %08x", identifier);
		}
		if (cpu->gpr[0] == 59) {
			OsxGuestString(cpu, cpu->gpr[3], text, sizeof(text));
			fprintf(session->msglog, " exec %s", text);
		}
		fprintf(session->msglog, "\n");
	}
#endif
	if (key == session->backtracekey && session->backtracecount < 16) {
		frame = cpu->gpr[1];
		session->backtraces[session->backtracecount][0] = cpu->lr;
		for (index = 1; index < 6; index++) {
			if (!OsxGuestCpuReadVirtual(cpu, frame, &frame) || !OsxGuestCpuReadVirtual(cpu, frame + 8, &session->backtraces[session->backtracecount][index]))
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

static void OsxGuestDumpSyscalls(OsxSession *session)
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

static int OsxGuestCompareSamples(const void *left, const void *right)
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

static void OsxGuestDumpSamples(OsxSession *session)
{
	uint32 best[8];
	int bestcount[8];
	int index;
	int run;
	int slot;
	int lowest;
	if (session->samplecount == 0 || session->machine->log == NULL)
		return;
	qsort(session->samples, session->samplecount, sizeof(uint32), OsxGuestCompareSamples);
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

static void OsxGuestParseDiskPatches(OsxMachine *machine)
{
	const char *text;
	const char *colon;
	OsxDiskPatch *patches;
	int count;
	int index;
	unsigned int value;
	count = 0;
	patches = (OsxDiskPatch *)calloc(OSXDISKPATCHES, sizeof(OsxDiskPatch));
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

static int OsxGuestKernelRead32(OsxCpu *cpu, uint32 address, uint32 *value)
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
	result = OsxGuestCpuReadVirtual(cpu, address, value);
	cpu->msr = savedmsr;
	memcpy(cpu->sr, savedsr, sizeof(savedsr));
	return result;
}

static uint32 OsxGuestAccessorOffset(OsxCpu *cpu, uint32 address, uint32 opcode, uint32 target)
{
	uint32 word;
	int index;
	for (index = 0; address != 0 && index < 48; index++) {
		if (!OsxGuestKernelRead32(cpu, address + index * 4, &word))
			return 0;
		if ((word >> 26) == opcode && ((word >> 21) & 31) == target)
			return word & 0xffff;
	}
	return 0;
}

static void OsxGuestDumpProcesses(OsxSession *session)
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
	if (machine->log == NULL || !OsxGuestKernelRead32(cpu, OsxGuestFindKernelSymbol(machine, "_allproc"), &proc))
		return;
	pidoffset = OsxGuestAccessorOffset(cpu, OsxGuestFindKernelSymbol(machine, "_proc_pid"), 32, 3);
	nameoffset = OsxGuestAccessorOffset(cpu, OsxGuestFindKernelSymbol(machine, "_proc_name"), 14, 4);
	if (pidoffset == 0 || nameoffset == 0)
		return;
	fprintf(machine->log, "ps");
	for (guard = 0; proc != 0 && guard < 64; guard++) {
		pid = 0;
		OsxGuestKernelRead32(cpu, proc + pidoffset, &pid);
		for (index = 0; index < 16; index++) {
			if (!OsxGuestKernelRead32(cpu, (proc + nameoffset + index) & ~3u, &word))
				break;
			name[index] = (char)(word >> (24 - ((proc + nameoffset + index) & 3) * 8));
			if (name[index] == 0)
				break;
		}
		name[index] = 0;
		fprintf(machine->log, " %u:%s", pid, name);
		if (!OsxGuestKernelRead32(cpu, proc, &proc))
			break;
	}
	fprintf(machine->log, "\n");
}

static void OsxGuestSaveScreenPixelsAsImage(OsxMachine *machine, const uint8 *source, const char *path)
{
	FILE *file;
	const uint8 *row;
	uint8 *pixels;
	uint32 y;
	uint32 x;
	file = fopen(path, "wb");
	if (file == NULL)
		return;
	pixels = (uint8 *)malloc(machine->width * 3);
	fprintf(file, "P6\n%u %u\n255\n", machine->width, machine->height);
	for (y = 0; y < machine->height; y++) {
		row = source + y * machine->rowbytes;
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

static int OsxGuestNextScriptLine(OsxSession *session)
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

static void OsxGuestSaveScriptScreenshot(OsxSession *session, const char *path)
{
#if OSXGLBRIDGE
	if (session->glpresent) {
		OsxGuestSaveScreenPixelsAsImage(session->machine, session->glpresent, path);
		return;
	}
#endif
	OsxGuestSaveScreenPixelsAsImage(session->machine, session->machine->vram, path);
}

static void OsxGuestExecuteScriptCommand(OsxSession *session, const char *command)
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
		fprintf(machine->log, "script %llu host %u %s", (unsigned long long)OsxGuestCpuTimebase(machine->cpu), (unsigned)(OsxGuestHostNanoseconds() / 1000000), command);
	if (sscanf(command, "mouse %d %d", &first, &second) == 2)
		OsxGuestMovePointer(session, first, second);
	else if (sscanf(command, "down %d", &first) == 1)
		OsxGuestPressButton(session, first, 1);
	else if (sscanf(command, "up %d", &first) == 1)
		OsxGuestPressButton(session, first, 0);
	else if (sscanf(command, "wheel %d", &first) == 1)
		OsxGuestCudaWheel(machine, first);
#if OSXMSGTRACE
	else if (sscanf(command, "det %d", &first) == 1 && first > 0 && machine->cpu->deterministic) {
		before = OsxGuestCpuTimebase(machine->cpu);
		machine->cpu->deterministic = (uint32)first;
		machine->cpu->virtualticks += before - OsxGuestCpuTimebase(machine->cpu);
	}
#endif
	else if (sscanf(command, "keydown %d", &first) == 1)
		OsxGuestCudaKey(machine, first, 1);
	else if (sscanf(command, "keyup %d", &first) == 1)
		OsxGuestCudaKey(machine, first, 0);
	else if (sscanf(command, "shot %255s", path) == 1)
		OsxGuestSaveScriptScreenshot(session, path);
	else if (sscanf(command, "watch %d %d %d %d %255s", &first, &second, &third, &fourth, path) == 5)
		OsxGuestCpuWatch(machine->cpu, fopen(path, "w"), machine->rowbytes, (uint32)first, (uint32)second, (uint32)third, (uint32)fourth);
	else if (strncmp(command, "unwatch", 7) == 0 && machine->cpu->watchfile) {
		fclose(machine->cpu->watchfile);
		machine->cpu->watchfile = NULL;
		OsxGuestCpuFlushTlb(machine->cpu);
	} else if (sscanf(command, "msglog %255s", path) == 1)
		session->msglog = fopen(path, "w");
	else if (strncmp(command, "msgunlog", 8) == 0 && session->msglog) {
		fclose(session->msglog);
		session->msglog = NULL;
	} else if (strncmp(command, "quit", 4) == 0)
		machine->quit = 1;
}

static void OsxGuestRunScript(OsxSession *session, uint64 now)
{
	while (session->script && now >= session->scriptticks) {
		OsxGuestExecuteScriptCommand(session, session->scriptcommand);
		OsxGuestNextScriptLine(session);
	}
}

static void OsxGuestLogSlice(FILE *slicelog, OsxCpu *cpu)
{
	uint32 hash;
	int index;
	hash = cpu->cr ^ cpu->lr ^ cpu->ctr ^ cpu->xer;
	for (index = 0; index < 32; index++)
		hash = hash * 31 + cpu->gpr[index] + (uint32)cpu->fpr[index].bits;
	fprintf(slicelog, "%llu %08x %08x\n", (unsigned long long)cpu->instructions, cpu->pc, hash);
}

#if OSXPROFILE
static void OsxGuestTmpDumpForms(FILE *log, OsxCpu *cpu)
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

static void OsxGuestRunMachine(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	uint64 now;
	uint64 nextframe;
	int presentnow;
	uint64 frameticks;
	uint64 nextevent;
	uint64 eventticks;
	uint64 eventwait;
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
	now = OsxGuestCpuTimebase(cpu);
	slicesize = 20000;
	while (!machine->quit) {
#if OSXG5
		if (cpu->core970)
			OsxGuest970CheckInterrupts((Osx970 *)cpu->core970, now);
		else
			OsxGuestCpuCheckInterrupts(cpu, now);
#else
		OsxGuestCpuCheckInterrupts(cpu, now);
#endif
		if (cpu->halted) {
			waitticks = 0;
			if (nextframe > now)
				waitticks = nextframe - now;
			eventwait = 0;
			if (nextevent > now)
				eventwait = nextevent - now;
			if (eventwait < waitticks)
				waitticks = eventwait;
			if (cpu->decdeadline != OSXNEVER && cpu->decdeadline > now && cpu->decdeadline - now < waitticks)
				waitticks = cpu->decdeadline - now;
			cudadeadline = OsxGuestCudaNextDeadline(machine);
			bmacdeadline = OsxGuestBmacNextDeadline(machine);
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
			if (waitticks == eventwait && now + waitticks + OSXTIMEBASEFREQUENCY / 1000 <= nextframe)
				milliseconds = (uint32)((waitticks * 1000 + OSXTIMEBASEFREQUENCY - 1) / OSXTIMEBASEFREQUENCY);
			if (cpu->deterministic)
				cpu->virtualticks += waitticks + 1;
			else if (milliseconds > 0)
				OsxGuestHostSleep(milliseconds);
			now = OsxGuestCpuTimebase(cpu);
		} else {
			runstart = now;
			runinstructions = cpu->instructions;
#if OSXG5
			if (cpu->core970)
				OsxGuest970Run((Osx970 *)cpu->core970, slicesize);
			else
				OsxGuestJitRun(cpu, slicesize);
#else
			OsxGuestJitRun(cpu, slicesize);
#endif
			if (session->slicelog)
				OsxGuestLogSlice(session->slicelog, cpu);
			now = OsxGuestCpuTimebase(cpu);
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
			if (!cpu->halted && OsxGuestCpuReadVirtual(cpu, cpu->pc, &tmpword))
				cpu->tmpform[2][OsxGuestCpuTmpFormKey(tmpword)]++;
			if (session->tmpsamplefile) {
				tmprecord[0] = cpu->pc;
				tmprecord[1] = cpu->sr[0] & 0xffffff;
				tmprecord[2] = ((cpu->msr >> 14) & 1) | ((uint32)cpu->halted << 1) | ((uint32)(OsxGuestCpuTimebase(cpu) / (OSXTIMEBASEFREQUENCY / 1000)) << 2);
				fwrite(tmprecord, sizeof(tmprecord), 1, session->tmpsamplefile);
			}
#endif
			slices++;
		}
		OsxGuestCudaPoll(machine, now);
		OsxGuestBmacPoll(machine, now);
		OsxGuestAtaPoll(machine, now);
		OsxGuestAudioPoll(machine, now);
		OsxGuestRunScript(session, now);
		if (now >= nextevent) {
			nextevent = now + eventticks;
			OsxGuestHostEvents(session->video, machine->vram);
		}
		presentnow = now >= nextframe;
		if (presentnow) {
			if (now >= nextframe)
				nextframe = now + frameticks - (now - nextframe) % frameticks;
			else
				nextframe = now + frameticks;
			OsxGuestPresentFrame(session);
		}
		if (now >= nextstatus) {
			OsxGuestInstallPointerHook(session);
			OsxGuestInstallPowerHook(session);
			OsxGuestInstallKextHooks(session);
			OsxGuestDumpSamples(session);
			OsxGuestDumpSyscalls(session);
			if ((cpu->decrementercount & 7) == 0)
				OsxGuestDumpProcesses(session);
			nextstatus = now + OSXTIMEBASEFREQUENCY * 2;
			OsxGuestCpuCalibrateTimebase(cpu);
			now = OsxGuestCpuTimebase(cpu);
			if (machine->log) {
				fprintf(machine->log, "status pc %08x lr %08x msr %08x instructions %llu halted %d dec %u ext %u slow %llu exceptions %llu runms %llu slices %llu\n", cpu->pc, cpu->lr, cpu->msr, (unsigned long long)cpu->instructions, cpu->halted, cpu->decrementercount, cpu->externalcount, (unsigned long long)cpu->slowaccesses, (unsigned long long)cpu->exceptions, (unsigned long long)(runtime * 1000 / OSXTIMEBASEFREQUENCY), (unsigned long long)slices);
#if OSXPROFILEJIT
				OsxGuestJitTmpStats(cpu, machine->log);
#endif
#if OSXPROFILE
				OsxGuestTmpDumpForms(machine->log, cpu);
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

static void OsxGuestDestroySession(OsxSession *session)
{
	OsxMachine *machine;
	int busindex;
	int unitindex;
	machine = session->machine;
	if (machine) {
		OsxGuestAudioExit(machine);
		if (machine->bmac.slirpthread)
			OsxGuestHostNetClose(machine->bmac.slirpthread);
		if (machine->cpu) {
			OsxGuestJitDestroy(machine->cpu);
#if OSXG5
			if (machine->cpu->core970) {
				OsxGuest970JitDestroy((Osx970 *)machine->cpu->core970);
				free(machine->cpu->core970);
			}
#endif
			if (machine->cpu->watchfile)
				fclose(machine->cpu->watchfile);
			free(machine->cpu);
		}
		for (busindex = 0; busindex < OSXATABUSES; busindex++) {
			for (unitindex = 0; unitindex < 2; unitindex++) {
				OsxGuestAtaCloseMedia(&machine->ata[busindex].drives[unitindex]);
			}
			free(machine->ata[busindex].buffer);
		}
		free(machine->ata[0].patches);
		free(machine->kernelimage);
		free(machine->mkext);
		free(machine->ram);
		free(machine->vram);
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
#if OSXGLBRIDGE
	if (session->gl)
		OsxGuestDestroyGlBridge(session->gl);
	if (session->quartz)
		OsxGuestDestroyQuartzBridge(session->quartz);
	OsxGuestGlShutdownHost();
	free(session->glpresent);
#endif
	if (session->video)
		OsxGuestHostClose(session->video);
}

static int OsxGuestRunSession(OsxSession *session)
{
	OsxMachine *machine;
	OsxCpu *cpu;
	const char *bootimage;
	const char *bootargs;
	int bootbus;
	int bootunit;
	FILE *dump;
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
	machine->ram = (uint8 *)calloc(1, (size_t)machine->ramsize + machine->g5state.highsize);
#else
	machine->ram = (uint8 *)calloc(1, machine->ramsize);
#endif
	machine->vram = (uint8 *)calloc(1, OSXVRAMSIZE);
	if (machine->ram == NULL || machine->vram == NULL) {
		ErrorAlert("Not enough memory for the Mac OS X guest.");
		return 1;
	}
	OsxGuestParseScreen(machine);
	OsxGuestCpuReset(cpu, OSXPVR, OSXTIMEBASEFREQUENCY);
	if (PrefsFindInt32("osxdeterministic") > 0) {
		cpu->deterministic = (uint32)PrefsFindInt32("osxdeterministic");
		session->slicelog = fopen("osxslices.log", "w");
	}
	if (PrefsFindString("osxinputscript")) {
		session->script = fopen(PrefsFindString("osxinputscript"), "r");
		if (session->script)
			OsxGuestNextScriptLine(session);
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
	cpu->busread = OsxGuestBusRead;
	cpu->buswrite = OsxGuestBusWrite;
	cpu->logfile = machine->log;
#if OSXG5
	if (PrefsFindString("osxmodel") && strcmp(PrefsFindString("osxmodel"), "PowerMac7,3") == 0)
		machine->g5 = 1;
#endif
	OsxGuestPciReset(machine);
	OsxGuestAtaReset(machine);
	OsxGuestCudaReset(machine);
	OsxGuestBmacReset(machine);
	OsxGuestAudioReset(machine);
#if OSXG5
	if (machine->g5)
		OsxGuestG5Reset(machine);
#endif
	XPRAMInit(NULL);
	session->nvramready = 1;
	memcpy(machine->nvram, XPRAM, OSXNVRAMSIZE);
	OsxGuestFlashLoad(machine, "osx_nvram.bin");
	OsxGuestAttachDisks(machine, &bootimage, &bootbus, &bootunit);
	OsxGuestParseDiskPatches(machine);
#if OSXGLBRIDGE
#if OSXGLBRIDGE
	machine->flurry = PrefsFindBool("osxflurry");
	machine->quartz = PrefsFindBool("osxquartz");
#endif
#endif
	if (bootimage == NULL) {
		ErrorAlert("No Mac OS X boot disc or disk image is configured.");
		return 1;
	}
	if (!OsxGuestNvramPrepareBoot(machine, bootbus, bootunit, machine->bootpartition)) {
		ErrorAlert("The Startup Disk selection could not be stored.");
		return 1;
	}
	session->video = OsxGuestHostOpen((int)machine->width, (int)machine->height, OsxGuestHostInput, session);
	if (!session->video) {
		ErrorAlert("The host video output could not be opened.");
		return 1;
	}
	bootargs = PrefsFindString("osxbootargs");
	if (bootargs == NULL)
		bootargs = "";
	if (!OsxGuestBootLoad(machine, bootimage, bootbus, bootunit, bootargs))
		return 1;
	OsxGuestAllowStalledClassLookup(machine);
	if (OsxGuestFindKernelSymbol(machine, "_IOSleep"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_IOSleep"), OsxGuestSettleHook, machine);
	if (OsxGuestFindKernelSymbol(machine, "_conslog_putc"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_conslog_putc"), OsxGuestConsoleHook, session);
	if (OsxGuestFindKernelSymbol(machine, "_kmputc"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_kmputc"), OsxGuestTerminalHook, session);
	if (OsxGuestFindKernelSymbol(machine, "_kprintf"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_kprintf"), OsxGuestKprintfHook, session);
	if (OsxGuestFindKernelSymbol(machine, "_trap"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_trap"), OsxGuestTrapHook, session);
	if (PrefsFindString("osxtracestart") && OsxGuestFindKernelSymbol(machine, PrefsFindString("osxtracestart")))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, PrefsFindString("osxtracestart")), OsxGuestTraceStartHook, session);
	if (PrefsFindString("osxtracestop") && OsxGuestFindKernelSymbol(machine, PrefsFindString("osxtracestop")))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, PrefsFindString("osxtracestop")), OsxGuestTraceStopHook, session);
	OsxGuestInstallCallHooks(session);
	if (PrefsFindString("osxlogxml"))
		OsxGuestCpuAddHook(cpu, (uint32)strtoul(PrefsFindString("osxlogxml"), NULL, 16), OsxGuestXmlHook, session);
	OsxGuestParseKextHooks(session);
	if (OsxGuestFindKernelSymbol(machine, "_kmod_start_or_stop"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_kmod_start_or_stop"), OsxGuestKmodStartHook, session);
	session->backtracekey = 0xffe10afeu;
	if (PrefsFindString("osxbacktracekey"))
		session->backtracekey = (uint32)strtoul(PrefsFindString("osxbacktracekey"), NULL, 16);
	cpu->syscallobserver = OsxGuestSyscallObserver;
	cpu->syscallcontext = session;
	if (OsxGuestFindKernelSymbol(machine, "_panic"))
		OsxGuestCpuAddHook(cpu, OsxGuestFindKernelSymbol(machine, "_panic"), OsxGuestPanicHook, session);
	memcpy(machine->nvramclassic, machine->nvram, OSXNVRAMSIZE);
	OsxGuestNvramMakeNewWorld(machine->nvram);
#if OSXG5
	if (machine->g5)
		OsxGuestG5PrepareNvram(machine);
#endif
	OsxGuestHookKernelSymbol(machine, "__ZN16IOPlatformExpert14getBootROMTypeEv", OsxGuestNewWorldEpochHook);
	OsxGuestHookKernelSymbol(machine, "_getBootROMType__16IOPlatformExpert", OsxGuestNewWorldEpochHook);
	OsxGuestHookKernelSymbol(machine, "_sleepSystem__14IOPMrootDomain", OsxGuestRefuseSleepHook);
	OsxGuestHookKernelSymbol(machine, "_handleSleepTimerExpiration__14IOPMrootDomain", OsxGuestReturnHook);
	OsxGuestHookKernelSymbol(machine, "_sleepKernel__16IOPlatformExpert", OsxGuestReturnHook);
	OsxGuestHookKernelSymbol(machine, "_PE_Determine_Clock_Speeds", OsxGuestReturnHook);
#if OSXG5
	if (machine->pvr == OSX970PVR)
		OsxGuestHookKernelSymbol(machine, "_machine_idle", OsxGuestNapHook);
#endif
	if (machine->pvr == OSXPVRG4 || machine->ata[bootbus].drives[bootunit].atapi)
		OsxGuestHookKernelSymbol(machine, "_PEGetPlatformEpoch", OsxGuestNewWorldEpochHook);
	cpu->pc = machine->kernelentry;
	cpu->msr = 0;
	cpu->gpr[1] = machine->scratch + 0x8000;
	cpu->gpr[3] = machine->bootargs;
	cpu->gpr[4] = 0x426f6f58;
	OsxGuestCpuUpdateModes(cpu);
#if OSXG5
	if (machine->pvr == OSX970PVR) {
		core = (Osx970 *)calloc(1, sizeof(Osx970));
		if (core == NULL)
			return 1;
		OsxGuest970Reset(core, cpu, OSX970PVR);
		core->pc = machine->kernelentry;
		core->nextpc = core->pc + 4;
		core->msr = OSX970MSRHV;
		core->gpr[1] = machine->scratch + 0x8000;
		core->gpr[3] = machine->bootargs;
		core->gpr[4] = 0x426f6f58;
		cpu->core970 = core;
		if (PrefsFindBool("osxjit") && OsxGuest970JitCreate(core))
			fprintf(machine->log, "cpu 970 jit\n");
		else
			fprintf(machine->log, "cpu 970 interpreter\n");
	} else if (PrefsFindBool("osxjit")) {
#else
	if (PrefsFindBool("osxjit")) {
#endif
		if (OsxGuestJitCreate(cpu))
			fprintf(machine->log, "cpu native %s soft TLB\n", OSXHOSTNAME);
		else
			fprintf(machine->log, "cpu interpreter\n");
	}
#if OSXHOSTX64
	session->pixelshuffle = OsxGuestHostFeature(OSXFEATURESSSE3);
#endif
	session->shadow = (uint8 *)malloc(machine->rowbytes * machine->height);
	if (session->shadow == NULL)
		return 1;
	memset(session->shadow, 0xff, machine->rowbytes * machine->height);
#if OSXPROFILE
	session->tmpsamplefile = fopen("osxsamples.bin", "wb");
#endif
	OsxGuestRunMachine(session);
	OsxGuestNvramFinishBoot(machine, XPRAM + 0x1378);
	OsxGuestNvramRestoreOldWorld(machine->nvram, machine->nvramclassic);
	memcpy(XPRAM, machine->nvram, OSXNVRAMSIZE);
	OsxGuestFlashSave(machine);
	if (machine->log)
		OsxGuestCpuDump(cpu, machine->log);
	if (PrefsFindString("osxdump")) {
		dump = fopen(PrefsFindString("osxdump"), "wb");
		if (dump) {
			fwrite(machine->ram, 1, machine->ramsize, dump);
			fclose(dump);
		}
	}
	if (machine->quit == 2)
		return 2;
	return 0;
}

int OsxGuestMain(void)
{
	OsxSession session;
	int result;
	memset(&session, 0, sizeof(session));
	result = OsxGuestRunSession(&session);
	OsxGuestDestroySession(&session);
	return result;
}
