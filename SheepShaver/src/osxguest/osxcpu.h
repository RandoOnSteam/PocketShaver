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

#define OSXTLBBITS 10
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

typedef struct OsxCpu OsxCpu;

typedef void (*OsxHookFunction)(OsxCpu *cpu, void *context, uint32 address);
typedef void (*OsxVectorFunction)(OsxCpu *cpu, uint32 instruction);
typedef void (*OsxSyscallFunction)(OsxCpu *cpu, void *context);
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
	uint32 cr;
	uint32 xer;
	uint32 lr;
	uint32 ctr;
	uint32 msr;
	uint32 fpscr;
	uint32 vr[32][4];
	uint32 vscr;
	uint32 jitscratch;
	int jitflush;
#if OSXPROFILE
	uint32 tmpform[3][131072];
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
	OsxTlbEntry tlb[OSXMODES][OSXTLBKINDS][OSXTLBSIZE];
};

void OsxCpuReset(OsxCpu *cpu, uint32 pvr, uint64 tbfrequency);
void OsxCpuSetModel(OsxCpu *cpu, uint32 pvr);
int OsxCpuReadData(OsxCpu *cpu, uint32 address, int size, uint32 *value);
int OsxCpuWriteData(OsxCpu *cpu, uint32 address, int size, uint32 value);
int OsxCpuReadQuad(OsxCpu *cpu, uint32 address, uint32 *words);
#if OSXPROFILE
uint32 OsxCpuTmpFormKey(uint32 instruction);
#endif
uint64 OsxCpuSingleToDouble(uint32 word);
uint32 OsxCpuDoubleToSingle(uint64 bits);
int OsxCpuWriteQuad(OsxCpu *cpu, uint32 address, const uint32 *words);
void OsxCpuExecuteVector(OsxCpu *cpu, uint32 instruction);
int OsxCpuExecuteVectorMemory(OsxCpu *cpu, uint32 instruction, uint32 extended, uint32 address);
OsxVectorFunction OsxCpuVectorHandler(uint32 instruction);
void OsxCpuRun(OsxCpu *cpu, int32 count);
int OsxCpuFetchPage(OsxCpu *cpu);
void OsxCpuExecuteInstruction(OsxCpu *cpu, uint32 instruction);
int OsxCpuExecuteInline(OsxCpu *cpu, uint32 instruction);
int OsxJitCreate(OsxCpu *cpu);
void OsxJitDestroy(OsxCpu *cpu);
void OsxJitRun(OsxCpu *cpu, int32 count);
int OsxJitInvalidate(OsxCpu *cpu, uint32 address, uint32 length);
void OsxCpuCheckInterrupts(OsxCpu *cpu, uint64 now);
void OsxCpuRaiseException(OsxCpu *cpu, uint32 vector, uint32 srr1bits, uint32 srr0);
void OsxCpuUpdateModes(OsxCpu *cpu);
void OsxCpuFlushTlb(OsxCpu *cpu);
void OsxCpuWatch(OsxCpu *cpu, FILE *file, uint32 rowbytes, uint32 left, uint32 top, uint32 right, uint32 bottom);
uint64 OsxCpuTimebase(OsxCpu *cpu);
uint64 OsxCpuHostTicks(void);
void OsxCpuCalibrateTimebase(OsxCpu *cpu);
uint64 OsxCpuNextDeadline(OsxCpu *cpu);
uint64 OsxHostNanoseconds(void);
int OsxCpuAddHook(OsxCpu *cpu, uint32 address, OsxHookFunction function, void *context);
int OsxCpuFillTlb(OsxCpu *cpu, uint32 address, int kind);
int OsxCpuTranslate(OsxCpu *cpu, uint32 address, int kind, int raise, uint32 *physical);
int OsxCpuReadVirtual(OsxCpu *cpu, uint32 address, uint32 *value);
int OsxCpuWriteVirtual(OsxCpu *cpu, uint32 address, uint32 value);
void OsxCpuExecuteFloat(OsxCpu *cpu, uint32 instruction);
void OsxCpuSetFloatCompare(OsxCpu *cpu, int field, double left, double right, int ordered);
void OsxCpuDump(OsxCpu *cpu, FILE *output);
void OsxCpuDumpFaults(OsxCpu *cpu, FILE *output);

#endif
