/*
 *  osxcpu.h - OSX CPU emulation
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
#ifndef OSXCPU_H
#define OSXCPU_H

#if defined(_M_X64) || defined(__x86_64__)
#define OSXHOSTX64 1
#else
#define OSXHOSTX64 0
#endif
#if defined(_M_IX86) || defined(__i386__)
#define OSXHOSTX86 1
#else
#define OSXHOSTX86 0
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
#define OSXHOSTARM64 1
#else
#define OSXHOSTARM64 0
#endif
#if OSXHOSTX64
#define OSXHOSTNAME "x64"
#elif OSXHOSTX86
#define OSXHOSTNAME "x86"
#elif OSXHOSTARM64
#define OSXHOSTNAME "arm64"
#else
#define OSXHOSTNAME "none"
#endif
#if OSXHOSTX64 && !defined(_WIN32)
#define OSXJITABI __attribute__((ms_abi))
#else
#define OSXJITABI
#endif
#define OSXFEATURESSSE3 36
#define OSXFEATUREFMA 39
#define OSXFEATURESSE41 37
#define OSXFEATURESSE2 10
#if !defined(OSXJITSEGMENTFLUSH)
#define OSXJITSEGMENTFLUSH 0
#endif
#if !defined(OSXJITTLBWIDE)
#define OSXJITTLBWIDE 1
#endif
#if !defined(OSXJITCAFIELD)
#define OSXJITCAFIELD 1
#endif
#if !defined(OSXJITCRFIELDS)
#define OSXJITCRFIELDS 1
#endif

#define OSXCRALLFIELDS 0xffu
#if defined(_MSC_VER)
#define OSXFORCEINLINE __forceinline
#elif defined(__GNUC__)
#define OSXFORCEINLINE inline __attribute__((always_inline))
#else
#define OSXFORCEINLINE inline
#endif

#if !defined(OSXTLBBITS)
#define OSXTLBBITS 10
#endif
#define OSXTLBSIZE (1 << OSXTLBBITS)
#define OSXTLBMASK (OSXTLBSIZE - 1)
#define OSXTLBINVALID 1

#define OSXTLBREAD 0
#define OSXTLBWRITE 1
#define OSXTLBFETCH 2
#define OSXTLBKINDS 3

#define OSXMODEUSER 0
#define OSXMODESUPERVISOR 1
#define OSXMODEREAL 2
#define OSXMODES 3

#define OSXMSRVEC 0x02000000
#define OSXMSRPOW 0x00040000
#define OSXMSRSAVED 0x0200ff73
#define OSXMSRILE 0x00010000
#define OSXMSREE 0x00008000
#define OSXMSRPR 0x00004000
#define OSXMSRFP 0x00002000
#define OSXMSRME 0x00001000
#define OSXMSRFE0 0x00000800
#define OSXMSRSE 0x00000400
#define OSXMSRBE 0x00000200
#define OSXMSRFE1 0x00000100
#define OSXMSRIP 0x00000040
#define OSXMSRIR 0x00000020
#define OSXMSRDR 0x00000010
#define OSXMSRRI 0x00000002
#define OSXMSRLE 0x00000001

#define OSXXERSO 0x80000000
#define OSXXEROV 0x40000000
#define OSXXERCA 0x20000000

#define OSXVECTORRESET 0x0100
#define OSXVECTORMACHINECHECK 0x0200
#define OSXVECTORDSI 0x0300
#define OSXVECTORISI 0x0400
#define OSXVECTOREXTERNAL 0x0500
#define OSXVECTORALIGNMENT 0x0600
#define OSXVECTORPROGRAM 0x0700
#define OSXVECTORFPUNAVAILABLE 0x0800
#define OSXVECTORDECREMENTER 0x0900
#define OSXVECTORSYSCALL 0x0c00
#define OSXVECTORTRACE 0x0d00
#define OSXVECTORALTIVEC 0x0f20

#define OSXPROGRAMFP 0x00100000
#define OSXPROGRAMILLEGAL 0x00080000
#define OSXPROGRAMPRIVILEGED 0x00040000
#define OSXPROGRAMTRAP 0x00020000

#define OSXDSISRNOTFOUND 0x40000000
#define OSXDSISRPROTECTION 0x08000000
#define OSXDSISRDIRECTSTORE 0x04000000
#define OSXDSISRSTORE 0x02000000

#define OSXISINOTFOUND 0x40000000
#define OSXISIGUARDED 0x10000000
#define OSXISIPROTECTION 0x08000000

#define OSXHID0DOZE 0x00800000
#define OSXHID0NAP 0x00400000
#define OSXHID0SLEEP 0x00200000

#define OSXMAXHOOKS 64
#define OSXHOOKOPCODE 0x18000000
#define OSXNEVER 0xffffffffffffffffULL

typedef struct OsxTlbEntry {
	uint32 page;
	uint32 segment;
	uint8 *host;
#if OSXJITTLBWIDE && OSXHOSTX86
	uint32 spare;
#endif
} OsxTlbEntry;

typedef union OsxFloat {
	double value;
	uint64 bits;
} OsxFloat;

#define OSXFAULTRING 64

typedef struct OsxFaultRecord {
	uint32 pc;
	uint32 address;
	uint32 reason;
	int instruction;
} OsxFaultRecord;

#define OSXPROFILE 0
#define OSXPROFILEJIT 0
#define OSXMSGTRACE 0
#define OSXNETTRACE 0
#ifndef OSXG5
#define OSXG5 0
#endif

typedef struct OsxCpu OsxCpu;

typedef void (*OsxHookFunction)(OsxCpu *cpu, void *context, uint32 address);
typedef void (OSXJITABI *OsxVectorFunction)(OsxCpu *cpu, uint32 instruction);
typedef int (*OsxSyscallFunction)(OsxCpu *cpu, void *context, uint32 instruction);
typedef uint32 (*OsxBusReadFunction)(void *context, uint32 address, int size);
typedef void (*OsxBusWriteFunction)(void *context, uint32 address, int size, uint32 value);

typedef struct OsxHook {
	uint32 address;
	uint32 original;
	OsxHookFunction function;
	void *context;
} OsxHook;

struct OsxCpu {
	uint32 gpr[32];
	OsxFloat fpr[32];
	uint32 pc;
	uint32 nextpc;
#if OSXJITCRFIELDS
	uint8 crf[8];
#else
	uint32 cr;
#endif
	uint32 xer;
#if OSXJITCAFIELD
	uint8 ca;
#endif
	uint32 lr;
	uint32 ctr;
	uint32 msr;
	uint32 fpscr;
	uint32 vr[32][4];
	uint32 vscr;
	uint32 jitscratch;
	int jitflush;
	int jithot;
#if OSXPROFILE
	uint32 tmpform[4][131072];
	uint32 tmpexception[64];
#endif
	uint32 vrsave;
	uint32 msscr[2];
	int altivec;
	uint32 srr0;
	uint32 srr1;
	uint32 sprg[4];
	uint32 dar;
	uint32 dsisr;
	uint32 sdr1;
	uint32 sr[16];
	uint32 zerosr[16];
	uint32 ibat[8];
	uint32 dbat[8];
	uint32 hid0;
	uint32 hid1;
	uint32 pvr;
	uint32 l2cr;
	uint32 ictc;
	uint32 thrm[3];
	uint32 iabr;
	uint32 dabr;
	uint32 ear;
	uint32 mmcr[2];
	uint32 pmc[4];
	uint32 sia;
	uint32 reservation;
	int reservationvalid;
	uint32 decvalue;
	uint64 decstamp;
	uint64 decdeadline;
	int decpending;
	uint64 tboffset;
	uint64 tbfrequency;
	uint64 hostbase;
	uint64 hostscale;
	uint64 calibrationticks;
	uint64 calibrationcounter;
	int externalline;
	int halted;
	int32 slice;
	uint64 instructions;
	uint32 externalcount;
	uint64 slowaccesses;
	uint64 exceptions;
	uint32 decrementercount;
	int datamode;
	int fetchmode;
	uint32 *datasr;
	uint32 *fetchsr;
	OsxTlbEntry *readtlb;
	OsxTlbEntry *writetlb;
	OsxTlbEntry *fetchtlb;
	uint32 fetchtag;
	uint8 *fetchhost;
	uint8 *ram;
	uint32 ramsize;
#if OSXG5
	uint32 highram;
#endif
	uint8 *vram;
	uint32 vrambase;
	uint32 vramsize;
	void *buscontext;
	OsxBusReadFunction busread;
	OsxBusWriteFunction buswrite;
	OsxHook hooks[OSXMAXHOOKS];
	int hookcount;
	OsxSyscallFunction syscallobserver;
	void *syscallcontext;
	FILE *tracefile;
	FILE *logfile;
	int faultlogs;
	OsxFaultRecord faults[OSXFAULTRING];
	uint32 unknownspr;
	void *nativejit;
	uint8 *jitlink;
	uint32 deterministic;
	FILE *watchfile;
	uint32 watchlow;
	uint32 watchhigh;
	uint32 watchleft;
	uint32 watchright;
	uint32 watchrowbytes;
	uint64 virtualticks;
#if OSXG5
	void *core970;
#endif
	OsxTlbEntry tlb[OSXMODES][OSXTLBKINDS][OSXTLBSIZE];
};

static OSXFORCEINLINE uint32 OsxGuestCpuCarry(OsxCpu *cpu)
{
#if OSXJITCAFIELD
	return cpu->ca;
#else
	return (cpu->xer >> 29) & 1;
#endif
}

static OSXFORCEINLINE void OsxGuestCpuSetCarry(OsxCpu *cpu, int carry)
{
#if OSXJITCAFIELD
	cpu->ca = (uint8)(carry != 0);
#else
	if (carry)
		cpu->xer |= OSXXERCA;
	else
		cpu->xer &= ~OSXXERCA;
#endif
}

static OSXFORCEINLINE uint32 OsxGuestCpuXer(OsxCpu *cpu)
{
#if OSXJITCAFIELD
	return cpu->xer | ((uint32)cpu->ca << 29);
#else
	return cpu->xer;
#endif
}

static OSXFORCEINLINE void OsxGuestCpuSetXer(OsxCpu *cpu, uint32 value)
{
#if OSXJITCAFIELD
	cpu->xer = value & ~OSXXERCA;
	cpu->ca = (uint8)((value >> 29) & 1);
#else
	cpu->xer = value;
#endif
}

static OSXFORCEINLINE uint32 OsxGuestCrField(OsxCpu *cpu, int field)
{
#if OSXJITCRFIELDS
	return cpu->crf[field];
#else
	return (cpu->cr >> ((7 - field) * 4)) & 0xf;
#endif
}

static OSXFORCEINLINE void OsxGuestSetCrField(OsxCpu *cpu, int field, uint32 value)
{
#if OSXJITCRFIELDS
	cpu->crf[field] = (uint8)value;
#else
	int shift;
	shift = (7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | (value << shift);
#endif
}

static OSXFORCEINLINE uint32 OsxGuestCrBit(OsxCpu *cpu, int bit)
{
	return (OsxGuestCrField(cpu, bit >> 2) >> (3 - (bit & 3))) & 1;
}

static OSXFORCEINLINE void OsxGuestSetCrBit(OsxCpu *cpu, int bit, uint32 value)
{
	uint32 mask;
	mask = 8u >> (bit & 3);
	if (value)
		OsxGuestSetCrField(cpu, bit >> 2, OsxGuestCrField(cpu, bit >> 2) | mask);
	else
		OsxGuestSetCrField(cpu, bit >> 2, OsxGuestCrField(cpu, bit >> 2) & ~mask);
}

static OSXFORCEINLINE uint32 OsxGuestCpuCr(OsxCpu *cpu)
{
#if OSXJITCRFIELDS
	uint32 value;
	int field;
	value = 0;
	for (field = 0; field < 8; field++)
		value = (value << 4) | cpu->crf[field];
	return value;
#else
	return cpu->cr;
#endif
}

static OSXFORCEINLINE void OsxGuestCpuSetCr(OsxCpu *cpu, uint32 value, uint32 fields)
{
#if OSXJITCRFIELDS
	int field;
	for (field = 0; field < 8; field++)
		if (fields & (0x80u >> field))
			cpu->crf[field] = (uint8)((value >> ((7 - field) * 4)) & 0xf);
#else
	uint32 mask;
	mask = (fields | (fields << 12)) & 0x000f000fu;
	mask = (mask | (mask << 6)) & 0x03030303u;
	mask = ((mask | (mask << 3)) & 0x11111111u) * 0xf;
	cpu->cr = (cpu->cr & ~mask) | (value & mask);
#endif
}

void OsxGuestCpuReset(OsxCpu *cpu, uint32 pvr, uint64 tbfrequency);
void OsxGuestCpuSetModel(OsxCpu *cpu, uint32 pvr);
int OsxGuestCpuReadData(OsxCpu *cpu, uint32 address, int size, uint32 *value);
int OsxGuestCpuWriteData(OsxCpu *cpu, uint32 address, int size, uint32 value);
int OsxGuestCpuReadQuad(OsxCpu *cpu, uint32 address, uint32 *words);
#if OSXPROFILE
uint32 OsxGuestCpuTmpFormKey(uint32 instruction);
#endif
OSXJITABI uint64 OsxGuestCpuSingleToDouble(uint32 word);
OSXJITABI uint32 OsxGuestCpuDoubleToSingle(uint64 bits);
int OsxGuestCpuWriteQuad(OsxCpu *cpu, uint32 address, const uint32 *words);
void OsxGuestCpuExecuteVector(OsxCpu *cpu, uint32 instruction);
int OsxGuestCpuExecuteVectorMemory(OsxCpu *cpu, uint32 instruction, uint32 extended, uint32 address);
OsxVectorFunction OsxGuestCpuVectorHandler(uint32 instruction);
void OsxGuestCpuRun(OsxCpu *cpu, int32 count);
int OsxGuestCpuFetchPage(OsxCpu *cpu);
void OsxGuestCpuExecuteInstruction(OsxCpu *cpu, uint32 instruction);
OSXJITABI int OsxGuestCpuExecuteInline(OsxCpu *cpu, uint32 instruction);
int OsxGuestHostFeature(int feature);
uint8 *OsxGuestJitAllocate(uint32 size);
void OsxGuestJitRelease(uint8 *memory, uint32 size);
void OsxGuestJitFlush(uint8 *memory, uint32 size);
int OsxGuestJitCreate(OsxCpu *cpu);
void OsxGuestJitDestroy(OsxCpu *cpu);
void OsxGuestJitRun(OsxCpu *cpu, int32 count);
int OsxGuestJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length);
#if OSXPROFILEJIT
void OsxGuestJitTmpStats(OsxCpu *cpu, FILE *log);
#endif
void OsxGuestCpuCheckInterrupts(OsxCpu *cpu, uint64 now);
void OsxGuestCpuRaiseException(OsxCpu *cpu, uint32 vector, uint32 srr1bits, uint32 srr0);
void OsxGuestCpuUpdateModes(OsxCpu *cpu);
void OsxGuestCpuFlushTlb(OsxCpu *cpu);
#if OSXJITSEGMENTFLUSH
void OsxGuestCpuSegmentsChanged(OsxCpu *cpu);
#endif
void OsxGuestCpuWatch(OsxCpu *cpu, FILE *file, uint32 rowbytes, uint32 left, uint32 top, uint32 right, uint32 bottom);
OSXJITABI uint64 OsxGuestCpuTimebase(OsxCpu *cpu);
uint64 OsxCpuHostTicks(void);
void OsxGuestCpuCalibrateTimebase(OsxCpu *cpu);
uint64 OsxGuestCpuNextDeadline(OsxCpu *cpu);
uint64 OsxGuestHostNanoseconds(void);
void OsxGuestHostHugePages(void *memory, size_t size);
void OsxGuestHostShortSleep(uint32 microseconds);
int OsxGuestCpuAddHook(OsxCpu *cpu, uint32 address, OsxHookFunction function, void *context);
OSXJITABI int OsxGuestCpuFillTlb(OsxCpu *cpu, uint32 address, int kind);
int OsxGuestCpuTranslate(OsxCpu *cpu, uint32 address, int kind, int raise, uint32 *physical);
uint8 *OsxGuestCpuVirtualHost(OsxCpu *cpu, uint32 address, int kind);
int OsxGuestCpuReadVirtual(OsxCpu *cpu, uint32 address, uint32 *value);
int OsxGuestCpuWriteVirtual(OsxCpu *cpu, uint32 address, uint32 value);
void OsxGuestCpuExecuteFloat(OsxCpu *cpu, uint32 instruction);
void OsxGuestCpuSetFloatCompare(OsxCpu *cpu, int field, double left, double right, int ordered);
void OsxGuestCpuDump(OsxCpu *cpu, FILE *output);
void OsxGuestCpuDumpFaults(OsxCpu *cpu, FILE *output);

#if OSXG5
#define OSX970MSRSF 0x8000000000000000ULL
#define OSX970MSRHV 0x1000000000000000ULL
#define OSX970SRR1CLEAR 0x00000000783f0000ULL
#define OSX970VECTORDSEGMENT 0x0380
#define OSX970VECTORISEGMENT 0x0480
#define OSX970HID0DOZE 0x0080000000000000ULL
#define OSX970HID0NAP 0x0040000000000000ULL
#define OSX970HID0DEEPNAP 0x0020000000000000ULL
#define OSX970SLBSIZE 64
#define OSX970SLBVALID 0x0000000008000000ULL
#define OSX970HIGHRAM 0x100000000ULL
#define OSX970SLBKS 0x0000000000000800ULL
#define OSX970SLBKP 0x0000000000000400ULL
#define OSX970SLBN 0x0000000000000200ULL
#define OSX970TLBBITS 10
#define OSX970TLBSIZE (1 << OSX970TLBBITS)
#define OSX970TLBMASK (OSX970TLBSIZE - 1)
#define OSX970TLBINVALID 1
#define OSX970FILLS 256
#define OSX970PVR 0x003c0301

typedef struct Osx970Slb {
	uint64 esid;
	uint64 vsid;
} Osx970Slb;

typedef struct Osx970TlbEntry {
	uint64 page;
	uint8 *host;
} Osx970TlbEntry;

typedef struct Osx970 {
	OsxCpu *cpu;
	uint64 gpr[32];
	uint64 pc;
	uint64 nextpc;
	uint64 lr;
	uint64 ctr;
	uint64 msr;
	uint64 srr0;
	uint64 srr1;
	uint64 hsrr0;
	uint64 hsrr1;
	uint64 dar;
	uint64 sdr1;
	uint64 hior;
	uint64 sprg[4];
	uint64 hsprg[2];
	uint64 hid0;
	uint64 hid1;
	uint64 hid4;
	uint64 hid5;
	uint64 dabr;
	uint64 iabr;
	uint64 scomc;
	uint64 scomd;
	uint64 reservation;
	int reservationvalid;
	uint32 dsisr;
	uint32 pir;
	uint64 fetchpage;
	uint8 *fetchhost;
	int modes[OSXTLBKINDS];
	uint8 *codepages;
	uint32 *versions;
	void *jit;
	uint8 *jitlinksite;
	void *jitcache;
	int slbhint;
	int jithot;
	Osx970Slb slb[OSX970SLBSIZE];
	Osx970TlbEntry *filled[OSX970FILLS];
	int fillcount;
	Osx970TlbEntry tlb[OSXMODES][OSXTLBKINDS][OSX970TLBSIZE];
} Osx970;

void OsxGuest970Reset(Osx970 *core, OsxCpu *cpu, uint32 pvr);
void OsxGuest970FlushTlb(Osx970 *core);
void OsxGuest970Raise(Osx970 *core, uint32 vector, uint64 srr1bits);
int OsxGuest970Translate(Osx970 *core, uint64 address, int kind, int raise, uint64 *physical);
int OsxGuest970Read(Osx970 *core, uint64 address, int size, uint64 *value);
int OsxGuest970Write(Osx970 *core, uint64 address, int size, uint64 value);
void OsxGuest970ExecuteInstruction(Osx970 *core, uint32 instruction);
void OsxGuest970CheckInterrupts(Osx970 *core, uint64 now);
void OsxGuest970Run(Osx970 *core, int32 count);
int OsxGuest970TranslateVirtual(Osx970 *core, uint32 address, int kind, uint32 *physical);
OSXJITABI int OsxGuest970Step(Osx970 *core, uint32 instruction);
void OsxGuest970InvalidatePhysical(Osx970 *core, uint64 physical, uint64 length);
int OsxGuest970JitCreate(Osx970 *core);
void OsxGuest970JitDestroy(Osx970 *core);
void Osx970JitRun(Osx970 *core, int32 count);
#endif

#endif
