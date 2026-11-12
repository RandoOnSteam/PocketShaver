/*
 *  osxcpu.cpp - OSX CPU, FPU and AltiVec emulation
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
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#include <time.h>
#include <sys/mman.h>
#endif
#include "osxcpu.h"
#include <fenv.h>

#define OSXHUGEPAGEBYTES 0x200000
#define OSXSHORTSLEEPSLACK 150
#define OSXHIGHRESOLUTIONTIMER 0x00000002

#define OSXPAGEMASK 0xfffff000
#define OSXACCESSFAULT 0
#define OSXACCESSHOST 1
#define OSXACCESSBUS 2

#if defined(_MSC_VER)
#define OSXFORCEINLINE __forceinline
#elif defined(__GNUC__)
#define OSXFORCEINLINE inline __attribute__((always_inline))
#else
#define OSXFORCEINLINE inline
#endif

static inline uint32 OsxGuestLoadHost32(const uint8 *host)
{
	uint32 value;
	memcpy(&value, host, 4);
	return tswap32(value);
}

static inline uint32 OsxGuestLoadHost16(const uint8 *host)
{
	uint16 value;
	memcpy(&value, host, 2);
	return tswap16(value);
}

static inline void OsxGuestStoreHost32(uint8 *host, uint32 value)
{
	value = tswap32(value);
	memcpy(host, &value, 4);
}

static inline void OsxGuestStoreHost16(uint8 *host, uint32 value)
{
	uint16 half;
	half = tswap16((uint16)value);
	memcpy(host, &half, 2);
}

static inline uint32 OsxGuestLoadHostSized(const uint8 *host, int size)
{
	if (size == 4)
		return OsxGuestLoadHost32(host);
	if (size == 2)
		return OsxGuestLoadHost16(host);
	return host[0];
}

static inline void OsxGuestStoreHostSized(uint8 *host, int size, uint32 value)
{
	if (size == 4)
		OsxGuestStoreHost32(host, value);
	else if (size == 2)
		OsxGuestStoreHost16(host, value);
	else
		host[0] = (uint8)value;
}

#if (OSXHOSTX64 || OSXHOSTX86) && defined(__GNUC__) && !defined(_WIN32)
#define OSXHOSTTSC 1
#else
#define OSXHOSTTSC 0
#endif

static uint64 OsxGuestHostTicks(void)
{
#if defined(_WIN32)
	/* QueryPerformanceCounter() takes up around 20%
	of process time! Use rdtsc directly if possible. */
#if defined(_MSC_VER) && OSXHOSTARM64
	return (uint64)_ReadStatusReg(ARM64_CNTVCT);
#elif defined(_MSC_VER)
	return __rdtsc();
#else
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64)counter.QuadPart;
#endif
#elif OSXHOSTTSC
	return __builtin_ia32_rdtsc();
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec * 1000000000ULL + (uint64)now.tv_nsec;
#endif
}

static uint64 OsxGuestMultiplyDivide(uint64 value, uint64 multiplier, uint64 divisor)
{
	return (value / divisor) * multiplier + ((value % divisor) * multiplier) / divisor;
}

static uint64 OsxGuestHostCounter(void);

static uint64 OsxGuestHostFrequency(uint64 ticks, uint64 counter)
{
#ifdef _WIN32
	LARGE_INTEGER now;
	LARGE_INTEGER frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return OsxGuestMultiplyDivide(OsxGuestHostTicks() - ticks, (uint64)frequency.QuadPart, (uint64)now.QuadPart - counter);
#elif OSXHOSTTSC
	return OsxGuestMultiplyDivide(OsxGuestHostTicks() - ticks, 1000000000ULL, OsxGuestHostCounter() - counter);
#else
	return 1000000000ULL;
#endif
}

static uint64 OsxGuestHostCounter(void)
{
#ifdef _WIN32
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64)counter.QuadPart;
#elif OSXHOSTTSC
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec * 1000000000ULL + (uint64)now.tv_nsec;
#else
	return 0;
#endif
}

static uint64 OsxGuestCalibrateHostFrequency(OsxCpu *cpu)
{
#ifdef _WIN32
	DWORD megahertz;
	DWORD length;
	cpu->calibrationticks = OsxGuestHostTicks();
	cpu->calibrationcounter = OsxGuestHostCounter();
	length = sizeof(megahertz);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", RRF_RT_REG_DWORD, NULL, &megahertz, &length) == ERROR_SUCCESS && megahertz != 0)
		return (uint64)megahertz * 1000000ULL;
	return 3000000000ULL;
#elif OSXHOSTTSC
	cpu->calibrationticks = OsxGuestHostTicks();
	cpu->calibrationcounter = OsxGuestHostCounter();
	return 3000000000ULL;
#else
	return 1000000000ULL;
#endif
}

uint64 OsxGuestHostNanoseconds(void)
{
#ifdef _WIN32
	LARGE_INTEGER counter;
	LARGE_INTEGER frequency;
	uint64 ticks;
	uint64 rate;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	ticks = (uint64)counter.QuadPart;
	rate = (uint64)frequency.QuadPart;
	return (ticks / rate) * 1000000000ULL + ((ticks % rate) * 1000000000ULL) / rate;
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec * 1000000000ULL + (uint64)now.tv_nsec;
#endif
}

void OsxGuestHostHugePages(void *memory, size_t size)
{
#if defined(__linux__) && defined(MADV_HUGEPAGE)
	size_t first;
	size_t last;
	first = ((size_t)memory + OSXHUGEPAGEBYTES - 1) & ~(size_t)(OSXHUGEPAGEBYTES - 1);
	last = ((size_t)memory + size) & ~(size_t)(OSXHUGEPAGEBYTES - 1);
	if (memory != NULL && last > first)
		madvise((void *)first, last - first, MADV_HUGEPAGE);
#else
	(void)memory;
	(void)size;
#endif
}

void OsxGuestHostShortSleep(uint32 microseconds)
{
#if defined(_WIN32)
	HANDLE timer;
	LARGE_INTEGER duration;
	if (microseconds <= OSXSHORTSLEEPSLACK)
		return;
	timer = CreateWaitableTimerExW(NULL, NULL, OSXHIGHRESOLUTIONTIMER, TIMER_ALL_ACCESS);
	if (timer == NULL)
		return;
	duration.QuadPart = -(LONGLONG)(microseconds - OSXSHORTSLEEPSLACK) * 10;
	if (SetWaitableTimer(timer, &duration, 0, NULL, NULL, FALSE))
		WaitForSingleObject(timer, INFINITE);
	CloseHandle(timer);
#else
	struct timespec duration;
	if (microseconds <= OSXSHORTSLEEPSLACK)
		return;
	duration.tv_sec = 0;
	duration.tv_nsec = (long)(microseconds - OSXSHORTSLEEPSLACK) * 1000;
	nanosleep(&duration, NULL);
#endif
}

int OsxGuestHostFeature(int feature)
{
#if defined(_WIN32)
	return IsProcessorFeaturePresent(feature) != 0;
#elif OSXHOSTX64 || OSXHOSTX86
	if (feature == OSXFEATURESSE2)
		return __builtin_cpu_supports("sse2") != 0;
	if (feature == OSXFEATURESSSE3)
		return __builtin_cpu_supports("ssse3") != 0;
	if (feature == OSXFEATURESSE41)
		return __builtin_cpu_supports("sse4.1") != 0;
	return __builtin_cpu_supports("avx2") != 0;
#else
	return 0;
#endif
}

uint8 *OsxGuestJitAllocate(uint32 size)
{
#ifdef _WIN32
	return (uint8 *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
	void *memory;
	memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
		return NULL;
	return (uint8 *)memory;
#endif
}

void OsxGuestJitRelease(uint8 *memory, uint32 size)
{
#ifdef _WIN32
	VirtualFree(memory, 0, MEM_RELEASE);
#else
	munmap(memory, size);
#endif
}

void OsxGuestJitFlush(uint8 *memory, uint32 size)
{
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), memory, size);
#elif defined(__GNUC__)
	__builtin___clear_cache((char *)memory, (char *)memory + size);
#endif
}

OSXJITABI uint64 OsxGuestCpuTimebase(OsxCpu *cpu)
{
	uint64 elapsed;
	if (cpu->deterministic)
		return cpu->instructions / cpu->deterministic + cpu->virtualticks;
	elapsed = OsxGuestHostTicks() - cpu->hostbase;
	return (elapsed >> 32) * cpu->hostscale + (((elapsed & 0xffffffffULL) * cpu->hostscale) >> 32) + cpu->tboffset;
}

void OsxGuestCpuCalibrateTimebase(OsxCpu *cpu)
{
	uint64 current;
	uint64 frequency;
	if (cpu->deterministic || OsxGuestHostCounter() == cpu->calibrationcounter)
		return;
	frequency = OsxGuestHostFrequency(cpu->calibrationticks, cpu->calibrationcounter);
	current = OsxGuestCpuTimebase(cpu);
	cpu->hostbase = OsxGuestHostTicks();
	cpu->hostscale = (cpu->tbfrequency << 32) / frequency;
	cpu->tboffset = current;
}

uint64 OsxGuestCpuNextDeadline(OsxCpu *cpu)
{
	return cpu->decdeadline;
}

void OsxGuestCpuFlushTlb(OsxCpu *cpu)
{
	int mode;
	int kind;
	int index;
	for (mode = 0; mode < OSXMODES; mode++)
		for (kind = 0; kind < OSXTLBKINDS; kind++)
			for (index = 0; index < OSXTLBSIZE; index++)
				cpu->tlb[mode][kind][index].page = OSXTLBINVALID;
	cpu->fetchtag = 0;
}

static void OsxGuestInvalidateTlbPage(OsxCpu *cpu, uint32 address)
{
	int mode;
	int kind;
	int index;
	index = (address >> 12) & OSXTLBMASK;
	for (mode = 0; mode < OSXMODEREAL; mode++)
		for (kind = 0; kind < OSXTLBKINDS; kind++)
			cpu->tlb[mode][kind][index].page = OSXTLBINVALID;
	cpu->fetchtag = 0;
}

#define OSXBATVALID 3u
#define OSXBATLENGTHBITS 0x1ffcu
#define OSXBATLENGTHSHIFT 15
#define OSXBATEFFECTIVE 0xfffe0000u
#define OSXBATMINIMUM 0x20000u

static void OsxGuestInvalidateTlbBlock(OsxCpu *cpu, uint32 upper, int firstkind, int lastkind)
{
	uint32 blockmask;
	uint32 start;
	uint32 length;
	uint32 page;
	uint32 offset;
	int mode;
	int kind;
	int index;
	if (!(upper & OSXBATVALID))
		return;
	blockmask = (upper & OSXBATLENGTHBITS) << OSXBATLENGTHSHIFT;
	start = upper & OSXBATEFFECTIVE & ~blockmask;
	length = blockmask + OSXBATMINIMUM;
	if ((length >> 12) < OSXTLBSIZE) {
		for (offset = 0; offset < length; offset += 0x1000) {
			index = ((start + offset) >> 12) & OSXTLBMASK;
			for (mode = 0; mode < OSXMODEREAL; mode++)
				for (kind = firstkind; kind <= lastkind; kind++)
					if (cpu->tlb[mode][kind][index].page - start < length)
						cpu->tlb[mode][kind][index].page = OSXTLBINVALID;
		}
	} else {
		for (mode = 0; mode < OSXMODEREAL; mode++)
			for (kind = firstkind; kind <= lastkind; kind++)
				for (index = 0; index < OSXTLBSIZE; index++) {
					page = cpu->tlb[mode][kind][index].page;
					if (page - start < length)
						cpu->tlb[mode][kind][index].page = OSXTLBINVALID;
				}
	}
	cpu->fetchtag = 0;
}

void OsxGuestCpuUpdateModes(OsxCpu *cpu)
{
	int privilege;
	if (cpu->msr & OSXMSRPR)
		privilege = OSXMODEUSER;
	else
		privilege = OSXMODESUPERVISOR;
	if (cpu->msr & OSXMSRDR) {
		cpu->datamode = privilege;
		cpu->datasr = cpu->sr;
	} else {
		cpu->datamode = OSXMODEREAL;
		cpu->datasr = cpu->zerosr;
	}
	if (cpu->msr & OSXMSRIR) {
		cpu->fetchmode = privilege;
		cpu->fetchsr = cpu->sr;
	} else {
		cpu->fetchmode = OSXMODEREAL;
		cpu->fetchsr = cpu->zerosr;
	}
	cpu->readtlb = cpu->tlb[cpu->datamode][OSXTLBREAD];
	cpu->writetlb = cpu->tlb[cpu->datamode][OSXTLBWRITE];
	cpu->fetchtlb = cpu->tlb[cpu->fetchmode][OSXTLBFETCH];
	cpu->fetchtag = 0;
}

void OsxGuestCpuSetModel(OsxCpu *cpu, uint32 pvr)
{
	cpu->pvr = pvr;
	cpu->altivec = (pvr >> 16) == 0x000c;
}

void OsxGuestCpuReset(OsxCpu *cpu, uint32 pvr, uint64 tbfrequency)
{
	memset(cpu->gpr, 0, sizeof(cpu->gpr));
	memset(cpu->fpr, 0, sizeof(cpu->fpr));
	memset(cpu->sr, 0, sizeof(cpu->sr));
	memset(cpu->zerosr, 0, sizeof(cpu->zerosr));
	memset(cpu->ibat, 0, sizeof(cpu->ibat));
	memset(cpu->dbat, 0, sizeof(cpu->dbat));
	memset(cpu->sprg, 0, sizeof(cpu->sprg));
	memset(cpu->thrm, 0, sizeof(cpu->thrm));
	memset(cpu->mmcr, 0, sizeof(cpu->mmcr));
	memset(cpu->pmc, 0, sizeof(cpu->pmc));
	cpu->pc = 0xfff00100;
	cpu->nextpc = cpu->pc;
	cpu->cr = 0;
	cpu->xer = 0;
	cpu->lr = 0;
	cpu->ctr = 0;
	cpu->msr = OSXMSRIP;
	cpu->fpscr = 0;
	cpu->srr0 = 0;
	cpu->srr1 = 0;
	cpu->dar = 0;
	cpu->dsisr = 0;
	cpu->sdr1 = 0;
	cpu->hid0 = 0;
	cpu->hid1 = 0;
	OsxGuestCpuSetModel(cpu, pvr);
	memset(cpu->vr, 0, sizeof(cpu->vr));
	cpu->vscr = 0x00010000;
	cpu->vrsave = 0;
	cpu->msscr[0] = 0;
	cpu->msscr[1] = 0;
	cpu->l2cr = 0;
	cpu->ictc = 0;
	cpu->iabr = 0;
	cpu->dabr = 0;
	cpu->ear = 0;
	cpu->sia = 0;
	cpu->reservation = 0;
	cpu->reservationvalid = 0;
	cpu->tbfrequency = tbfrequency;
	cpu->hostscale = (tbfrequency << 32) / OsxGuestCalibrateHostFrequency(cpu);
	cpu->hostbase = OsxGuestHostTicks();
	cpu->tboffset = 0;
	cpu->decvalue = 0xffffffff;
	cpu->decstamp = 0;
	cpu->decdeadline = OSXNEVER;
	cpu->decpending = 0;
	cpu->externalline = 0;
	cpu->halted = 0;
	cpu->slice = 0;
	cpu->instructions = 0;
	cpu->hookcount = 0;
	cpu->syscallobserver = NULL;
	cpu->syscallcontext = NULL;
	cpu->tracefile = NULL;
	cpu->logfile = NULL;
	cpu->faultlogs = 0;
	cpu->unknownspr = 0;
	cpu->nativejit = NULL;
	OsxGuestCpuFlushTlb(cpu);
	OsxGuestCpuUpdateModes(cpu);
}

#if OSXPROFILE
uint32 OsxGuestCpuTmpFormKey(uint32 instruction)
{
	uint32 opcode;
	uint32 sub;
	opcode = instruction >> 26;
	sub = 0;
	if (opcode == 4 && (instruction & 0x3f) >= 32)
		sub = instruction & 0x3f;
	else if (opcode == 4)
		sub = instruction & 0x7ff;
	else if (opcode == 19 || opcode == 31 || opcode == 59 || opcode == 63)
		sub = (instruction >> 1) & 0x3ff;
	return opcode * 2048 + sub;
}
#endif

void OsxGuestCpuRaiseException(OsxCpu *cpu, uint32 vector, uint32 srr1bits, uint32 srr0)
{
	uint32 newmsr;
#if OSXPROFILE
	cpu->tmpexception[(vector >> 8) & 63]++;
#endif
	cpu->exceptions++;
	cpu->srr0 = srr0;
	cpu->srr1 = (cpu->msr & OSXMSRSAVED) | srr1bits;
	newmsr = cpu->msr & (OSXMSRILE | OSXMSRME | OSXMSRIP);
	if (newmsr & OSXMSRILE)
		newmsr |= OSXMSRLE;
	cpu->msr = newmsr;
	if (newmsr & OSXMSRIP)
		cpu->nextpc = 0xfff00000 | vector;
	else
		cpu->nextpc = vector;
	cpu->halted = 0;
	OsxGuestCpuUpdateModes(cpu);
}

static inline int OsxGuestPhysicalHost(OsxCpu *cpu, uint32 physical, uint8 **host)
{
	if (physical < cpu->ramsize) {
		*host = cpu->ram + physical;
		return 1;
	}
#if OSXG5
	if (physical - cpu->ramsize < cpu->highram) {
		*host = cpu->ram + physical;
		return 1;
	}
#endif
	if (physical - cpu->vrambase < cpu->vramsize) {
		*host = cpu->vram + (physical - cpu->vrambase);
		return 1;
	}
	return 0;
}

static uint32 OsxGuestPtegAddress(uint32 sdr1, uint32 hash)
{
	uint32 htaborg;
	uint32 htabmask;
	htaborg = sdr1 & 0xffff0000;
	htabmask = sdr1 & 0x1ff;
	return (htaborg & 0xfe000000) | ((((htaborg >> 16) & 0x1ff) | ((hash >> 10) & htabmask)) << 16) | ((hash & 0x3ff) << 6);
}

static void OsxGuestRecordFault(OsxCpu *cpu, uint32 address, uint32 reason, int instruction)
{
	OsxFaultRecord *record;
	if (cpu->msr & OSXMSRPR)
		return;
	record = &cpu->faults[cpu->faultlogs & (OSXFAULTRING - 1)];
	record->pc = cpu->pc;
	record->address = address;
	record->reason = reason;
	record->instruction = instruction;
	cpu->faultlogs++;
}

void OsxGuestCpuDumpFaults(OsxCpu *cpu, FILE *output)
{
	OsxFaultRecord *record;
	int index;
	int start;
	start = 0;
	if (cpu->faultlogs > OSXFAULTRING)
		start = cpu->faultlogs - OSXFAULTRING;
	for (index = start; index < cpu->faultlogs; index++) {
		record = &cpu->faults[index & (OSXFAULTRING - 1)];
		if (record->instruction)
			fprintf(output, "isi pc %08x reason %08x\n", record->pc, record->reason);
		else
			fprintf(output, "dsi pc %08x address %08x dsisr %08x\n", record->pc, record->address, record->reason);
	}
}

static void OsxGuestTranslationFault(OsxCpu *cpu, uint32 address, int kind, uint32 reason)
{
	if (kind == OSXTLBFETCH) {
		OsxGuestRecordFault(cpu, cpu->pc, reason, 1);
		OsxGuestCpuRaiseException(cpu, OSXVECTORISI, reason, cpu->pc);
		return;
	}
	cpu->dar = address;
	if (kind == OSXTLBWRITE)
		cpu->dsisr = reason | OSXDSISRSTORE;
	else
		cpu->dsisr = reason;
	OsxGuestRecordFault(cpu, address, cpu->dsisr, 0);
	OsxGuestCpuRaiseException(cpu, OSXVECTORDSI, 0, cpu->pc);
}

static int OsxGuestCheckProtection(int key, uint32 pp, int kind)
{
	if (key == 0) {
		if (kind == OSXTLBWRITE && pp == 3)
			return 0;
		return 1;
	}
	if (pp == 0)
		return 0;
	if (kind == OSXTLBWRITE && pp != 2)
		return 0;
	return 1;
}

int OsxGuestCpuTranslate(OsxCpu *cpu, uint32 address, int kind, int raise, uint32 *physical)
{
	uint32 *bats;
	uint32 upper;
	uint32 lower;
	uint32 blockmask;
	uint32 segment;
	uint32 vsid;
	uint32 pageindex;
	uint32 hash;
	uint32 pteg;
	uint32 want;
	uint32 word0;
	uint32 word1;
	uint32 updated;
	uint8 *host;
	int translated;
	int user;
	int index;
	int pass;
	int slot;
	int key;
#if OSXG5
	if (cpu->core970)
		return OsxGuest970TranslateVirtual((Osx970 *)cpu->core970, address, kind, physical);
#endif
	if (kind == OSXTLBFETCH) {
		translated = (cpu->msr & OSXMSRIR) != 0;
		bats = cpu->ibat;
	} else {
		translated = (cpu->msr & OSXMSRDR) != 0;
		bats = cpu->dbat;
	}
	if (!translated) {
		*physical = address;
		return 1;
	}
	user = (cpu->msr & OSXMSRPR) != 0;
	for (index = 0; index < 8; index += 2) {
		upper = bats[index];
		if (user && !(upper & 1))
			continue;
		if (!user && !(upper & 2))
			continue;
		blockmask = (upper & 0x1ffc) << 15;
		if ((address & 0xfffe0000 & ~blockmask) != (upper & 0xfffe0000 & ~blockmask))
			continue;
		lower = bats[index + 1];
		if ((lower & 3) == 0 || (kind == OSXTLBWRITE && (lower & 1))) {
			if (raise)
				OsxGuestTranslationFault(cpu, address, kind, OSXDSISRPROTECTION);
			return 0;
		}
		*physical = (lower & 0xfffe0000 & ~blockmask) | (address & (blockmask | 0x1ffff));
		return 1;
	}
	segment = cpu->sr[address >> 28];
	if (segment & 0x80000000) {
		if (raise) {
			if (kind == OSXTLBFETCH)
				OsxGuestTranslationFault(cpu, address, kind, OSXISIGUARDED);
			else
				OsxGuestTranslationFault(cpu, address, kind, OSXDSISRDIRECTSTORE);
		}
		return 0;
	}
	if (kind == OSXTLBFETCH && (segment & 0x10000000)) {
		if (raise)
			OsxGuestTranslationFault(cpu, address, kind, OSXISIGUARDED);
		return 0;
	}
	if (user)
		key = (segment >> 29) & 1;
	else
		key = (segment >> 30) & 1;
	vsid = segment & 0x00ffffff;
	pageindex = (address >> 12) & 0xffff;
	hash = (vsid & 0x7ffff) ^ pageindex;
	for (pass = 0; pass < 2; pass++) {
		pteg = OsxGuestPtegAddress(cpu->sdr1, hash);
		want = 0x80000000 | (vsid << 7) | ((uint32)pass << 6) | (pageindex >> 10);
		for (slot = 0; slot < 8; slot++) {
			if (!OsxGuestPhysicalHost(cpu, pteg + slot * 8, &host))
				break;
			word0 = OsxGuestLoadHost32(host);
			if (word0 != want)
				continue;
			word1 = OsxGuestLoadHost32(host + 4);
			if (!OsxGuestCheckProtection(key, word1 & 3, kind) || (kind == OSXTLBFETCH && (word1 & 0x08))) {
				if (raise) {
					if (kind == OSXTLBFETCH && (word1 & 0x08))
						OsxGuestTranslationFault(cpu, address, kind, OSXISIGUARDED);
					else
						OsxGuestTranslationFault(cpu, address, kind, OSXDSISRPROTECTION);
				}
				return 0;
			}
			updated = word1 | 0x100;
			if (kind == OSXTLBWRITE)
				updated |= 0x80;
			if (updated != word1)
				OsxGuestStoreHost32(host + 4, updated);
			*physical = (word1 & OSXPAGEMASK) | (address & 0xfff);
			return 1;
		}
		hash = ~hash;
	}
	if (raise)
		OsxGuestTranslationFault(cpu, address, kind, OSXDSISRNOTFOUND);
	return 0;
}

void OsxGuestCpuWatch(OsxCpu *cpu, FILE *file, uint32 rowbytes, uint32 left, uint32 top, uint32 right, uint32 bottom)
{
	cpu->watchfile = file;
	cpu->watchrowbytes = rowbytes;
	cpu->watchleft = left;
	cpu->watchright = right;
	cpu->watchlow = cpu->vrambase + top * rowbytes;
	cpu->watchhigh = cpu->vrambase + bottom * rowbytes;
	OsxGuestCpuFlushTlb(cpu);
}

static void OsxGuestWatchWrite(OsxCpu *cpu, uint32 physical, int size, uint32 value)
{
	uint32 column;
	if (physical < cpu->watchlow || physical >= cpu->watchhigh)
		return;
	column = ((physical - cpu->vrambase) % cpu->watchrowbytes) / 4;
	if (column < cpu->watchleft || column >= cpu->watchright)
		return;
	fprintf(cpu->watchfile, "%llu %llu pc %08x lr %08x pr %u sr %06x x %u y %u size %d value %08x\n", (unsigned long long)OsxGuestCpuTimebase(cpu), (unsigned long long)cpu->instructions, cpu->pc, cpu->lr, (cpu->msr >> 14) & 1, cpu->sr[0] & 0xffffff, column, (physical - cpu->vrambase) / cpu->watchrowbytes, size, value);
}

static void OsxGuestFillTlbEntry(OsxCpu *cpu, uint32 address, int kind, uint8 *host)
{
	OsxTlbEntry *entry;
	uint32 *segments;
	if (kind == OSXTLBFETCH) {
		entry = &cpu->fetchtlb[(address >> 12) & OSXTLBMASK];
		segments = cpu->fetchsr;
	} else if (kind == OSXTLBWRITE) {
		entry = &cpu->writetlb[(address >> 12) & OSXTLBMASK];
		segments = cpu->datasr;
	} else {
		entry = &cpu->readtlb[(address >> 12) & OSXTLBMASK];
		segments = cpu->datasr;
	}
	entry->page = address & OSXPAGEMASK;
	entry->segment = segments[address >> 28];
	entry->host = host - (address & 0xfff);
}

static int OsxGuestAccessSlow(OsxCpu *cpu, uint32 address, int kind, uint8 **host, uint32 *physical)
{
	cpu->slowaccesses++;
	if (!OsxGuestCpuTranslate(cpu, address, kind, 1, physical))
		return OSXACCESSFAULT;
	if (!OsxGuestPhysicalHost(cpu, *physical, host))
		return OSXACCESSBUS;
	if (kind == OSXTLBWRITE && cpu->watchfile && *physical - cpu->vrambase < cpu->vramsize)
		return OSXACCESSHOST;
	OsxGuestFillTlbEntry(cpu, address, kind, *host);
	return OSXACCESSHOST;
}

OSXJITABI int OsxGuestCpuFillTlb(OsxCpu *cpu, uint32 address, int kind)
{
	uint8 *host;
	uint32 physical;
	if (!OsxGuestCpuTranslate(cpu, address, kind, 0, &physical) || !OsxGuestPhysicalHost(cpu, physical, &host))
		return 0;
	if (kind == OSXTLBWRITE && cpu->watchfile && physical - cpu->vrambase < cpu->vramsize)
		return 0;
	OsxGuestFillTlbEntry(cpu, address, kind, host);
	return 1;
}

static int OsxGuestReadSlow(OsxCpu *cpu, uint32 address, int size, uint32 *value)
{
	uint8 *host;
	uint32 physical;
	uint32 result;
	uint32 byte;
	int status;
	int index;
	if ((address & 0xfff) + (uint32)size > 0x1000) {
		result = 0;
		for (index = 0; index < size; index++) {
			if (!OsxGuestReadSlow(cpu, address + index, 1, &byte))
				return 0;
			result = (result << 8) | byte;
		}
		*value = result;
		return 1;
	}
	status = OsxGuestAccessSlow(cpu, address, OSXTLBREAD, &host, &physical);
	if (status == OSXACCESSFAULT)
		return 0;
	if (status == OSXACCESSHOST)
		*value = OsxGuestLoadHostSized(host, size);
	else
		*value = cpu->busread(cpu->buscontext, physical, size);
	return 1;
}

static int OsxGuestWriteSlow(OsxCpu *cpu, uint32 address, int size, uint32 value)
{
	uint8 *host;
	uint32 physical;
	int status;
	int index;
	if ((address & 0xfff) + (uint32)size > 0x1000) {
		for (index = 0; index < size; index++)
			if (!OsxGuestWriteSlow(cpu, address + index, 1, (value >> ((size - 1 - index) * 8)) & 0xff))
				return 0;
		return 1;
	}
	status = OsxGuestAccessSlow(cpu, address, OSXTLBWRITE, &host, &physical);
	if (status == OSXACCESSFAULT)
		return 0;
	if (cpu->watchfile)
		OsxGuestWatchWrite(cpu, physical, size, value);
	if (status == OSXACCESSHOST)
		OsxGuestStoreHostSized(host, size, value);
	else
		cpu->buswrite(cpu->buscontext, physical, size, value);
	return 1;
}

static OSXFORCEINLINE uint8 *OsxGuestReadHost(OsxCpu *cpu, uint32 address, uint32 limit)
{
	OsxTlbEntry *entry;
	entry = &cpu->readtlb[(address >> 12) & OSXTLBMASK];
	if (entry->page == (address & OSXPAGEMASK) && entry->segment == cpu->datasr[address >> 28] && (address & 0xfff) <= limit)
		return entry->host + (address & 0xfff);
	return NULL;
}

static OSXFORCEINLINE uint8 *OsxGuestWriteHost(OsxCpu *cpu, uint32 address, uint32 limit)
{
	OsxTlbEntry *entry;
	entry = &cpu->writetlb[(address >> 12) & OSXTLBMASK];
	if (entry->page == (address & OSXPAGEMASK) && entry->segment == cpu->datasr[address >> 28] && (address & 0xfff) <= limit)
		return entry->host + (address & 0xfff);
	return NULL;
}

static OSXFORCEINLINE int OsxGuestCpuRead32(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = OsxGuestReadHost(cpu, address, 0xffc);
	if (host) {
		*value = OsxGuestLoadHost32(host);
		return 1;
	}
	return OsxGuestReadSlow(cpu, address, 4, value);
}

static OSXFORCEINLINE int OsxGuestCpuRead16(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = OsxGuestReadHost(cpu, address, 0xffe);
	if (host) {
		*value = OsxGuestLoadHost16(host);
		return 1;
	}
	return OsxGuestReadSlow(cpu, address, 2, value);
}

static OSXFORCEINLINE int OsxGuestCpuRead8(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = OsxGuestReadHost(cpu, address, 0xfff);
	if (host) {
		*value = host[0];
		return 1;
	}
	return OsxGuestReadSlow(cpu, address, 1, value);
}

static OSXFORCEINLINE int OsxGuestCpuWrite32(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = OsxGuestWriteHost(cpu, address, 0xffc);
	if (host) {
		OsxGuestStoreHost32(host, value);
		return 1;
	}
	return OsxGuestWriteSlow(cpu, address, 4, value);
}

static OSXFORCEINLINE int OsxGuestCpuWrite16(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = OsxGuestWriteHost(cpu, address, 0xffe);
	if (host) {
		OsxGuestStoreHost16(host, value);
		return 1;
	}
	return OsxGuestWriteSlow(cpu, address, 2, value);
}

int OsxGuestCpuReadData(OsxCpu *cpu, uint32 address, int size, uint32 *value)
{
	if (size == 4)
		return OsxGuestCpuRead32(cpu, address, value);
	if (size == 2)
		return OsxGuestCpuRead16(cpu, address, value);
	return OsxGuestCpuRead8(cpu, address, value);
}

static OSXFORCEINLINE int OsxGuestCpuWrite8(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = OsxGuestWriteHost(cpu, address, 0xfff);
	if (host) {
		host[0] = (uint8)value;
		return 1;
	}
	return OsxGuestWriteSlow(cpu, address, 1, value);
}

int OsxGuestCpuWriteData(OsxCpu *cpu, uint32 address, int size, uint32 value)
{
	if (size == 4)
		return OsxGuestCpuWrite32(cpu, address, value);
	if (size == 2)
		return OsxGuestCpuWrite16(cpu, address, value);
	return OsxGuestCpuWrite8(cpu, address, value);
}

int OsxGuestCpuReadQuad(OsxCpu *cpu, uint32 address, uint32 *words)
{
	uint8 *host;
	uint32 values[4];
	int index;
	host = OsxGuestReadHost(cpu, address, 0xff0);
	if (host) {
		words[0] = OsxGuestLoadHost32(host);
		words[1] = OsxGuestLoadHost32(host + 4);
		words[2] = OsxGuestLoadHost32(host + 8);
		words[3] = OsxGuestLoadHost32(host + 12);
		return 1;
	}
	for (index = 0; index < 4; index++)
		if (!OsxGuestCpuRead32(cpu, address + index * 4, &values[index]))
			return 0;
	for (index = 0; index < 4; index++)
		words[index] = values[index];
	return 1;
}

int OsxGuestCpuWriteQuad(OsxCpu *cpu, uint32 address, const uint32 *words)
{
	uint8 *host;
	int index;
	host = OsxGuestWriteHost(cpu, address, 0xff0);
	if (host) {
		OsxGuestStoreHost32(host, words[0]);
		OsxGuestStoreHost32(host + 4, words[1]);
		OsxGuestStoreHost32(host + 8, words[2]);
		OsxGuestStoreHost32(host + 12, words[3]);
		return 1;
	}
	for (index = 0; index < 4; index++)
		if (!OsxGuestCpuWrite32(cpu, address + index * 4, words[index]))
			return 0;
	return 1;
}

static int OsxGuestRead64(OsxCpu *cpu, uint32 address, uint64 *value)
{
	uint8 *host;
	uint32 high;
	uint32 low;
	host = OsxGuestReadHost(cpu, address, 0xff8);
	if (host) {
		*value = ((uint64)OsxGuestLoadHost32(host) << 32) | OsxGuestLoadHost32(host + 4);
		return 1;
	}
	if (!OsxGuestReadSlow(cpu, address, 4, &high))
		return 0;
	if (!OsxGuestReadSlow(cpu, address + 4, 4, &low))
		return 0;
	*value = ((uint64)high << 32) | low;
	return 1;
}

static int OsxGuestWrite64(OsxCpu *cpu, uint32 address, uint64 value)
{
	uint8 *host;
	uint32 physical;
	host = OsxGuestWriteHost(cpu, address, 0xff8);
	if (host) {
		OsxGuestStoreHost32(host, (uint32)(value >> 32));
		OsxGuestStoreHost32(host + 4, (uint32)value);
		return 1;
	}
	if (((address + 4) & OSXPAGEMASK) != (address & OSXPAGEMASK) && !OsxGuestCpuTranslate(cpu, address + 4, OSXTLBWRITE, 1, &physical))
		return 0;
	if (!OsxGuestWriteSlow(cpu, address, 4, (uint32)(value >> 32)))
		return 0;
	return OsxGuestWriteSlow(cpu, address + 4, 4, (uint32)value);
}

uint8 *OsxGuestCpuVirtualHost(OsxCpu *cpu, uint32 address, int kind)
{
	uint32 physical;
	uint8 *host;
	if (!OsxGuestCpuTranslate(cpu, address, kind, 0, &physical) || !OsxGuestPhysicalHost(cpu, physical, &host))
		return NULL;
	return host;
}

int OsxGuestCpuReadVirtual(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = OsxGuestCpuVirtualHost(cpu, address, OSXTLBREAD);
	if (!host)
		return 0;
	*value = OsxGuestLoadHost32(host);
	return 1;
}

int OsxGuestCpuWriteVirtual(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = OsxGuestCpuVirtualHost(cpu, address, OSXTLBWRITE);
	if (!host)
		return 0;
	OsxGuestStoreHost32(host, value);
	return 1;
}

int OsxGuestCpuFetchPage(OsxCpu *cpu)
{
	OsxTlbEntry *entry;
	uint8 *host;
	uint32 physical;
	int status;
	entry = &cpu->fetchtlb[(cpu->pc >> 12) & OSXTLBMASK];
	if (entry->page == (cpu->pc & OSXPAGEMASK) && entry->segment == cpu->fetchsr[cpu->pc >> 28]) {
		cpu->fetchhost = entry->host;
	} else {
		status = OsxGuestAccessSlow(cpu, cpu->pc, OSXTLBFETCH, &host, &physical);
		if (status == OSXACCESSFAULT)
			return 0;
		if (status == OSXACCESSBUS) {
			OsxGuestCpuRaiseException(cpu, OSXVECTORMACHINECHECK, 0, cpu->pc);
			return 0;
		}
		cpu->fetchhost = host - (cpu->pc & 0xfff);
	}
	cpu->fetchtag = (cpu->pc & OSXPAGEMASK) | 1;
	return 1;
}

int OsxGuestCpuAddHook(OsxCpu *cpu, uint32 address, OsxHookFunction function, void *context)
{
	OsxHook *hook;
	if (cpu->hookcount >= OSXMAXHOOKS || address + 4 > cpu->ramsize)
		return 0;
	hook = &cpu->hooks[cpu->hookcount];
	hook->address = address;
	hook->original = OsxGuestLoadHost32(cpu->ram + address);
	hook->function = function;
	hook->context = context;
	OsxGuestStoreHost32(cpu->ram + address, OSXHOOKOPCODE | (uint32)cpu->hookcount);
	OsxGuestJitInvalidate(cpu, address, 4);
	cpu->hookcount++;
	return 1;
}

static OSXFORCEINLINE void OsxGuestSetCr0(OsxCpu *cpu, uint32 result)
{
	uint32 field;
	if ((int32)result < 0)
		field = 8;
	else if (result != 0)
		field = 4;
	else
		field = 2;
	field |= cpu->xer >> 31;
	cpu->cr = (cpu->cr & 0x0fffffff) | (field << 28);
}

static OSXFORCEINLINE void OsxGuestSetCrField(OsxCpu *cpu, int field, uint32 value)
{
	int shift;
	shift = (7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | (value << shift);
}

static OSXFORCEINLINE uint32 OsxGuestCrBit(OsxCpu *cpu, int bit)
{
	return (cpu->cr >> (31 - bit)) & 1;
}

static OSXFORCEINLINE void OsxGuestSetCrBit(OsxCpu *cpu, int bit, uint32 value)
{
	uint32 mask;
	mask = 0x80000000u >> bit;
	if (value)
		cpu->cr |= mask;
	else
		cpu->cr &= ~mask;
}

static OSXFORCEINLINE void OsxGuestSetCarry(OsxCpu *cpu, int carry)
{
	if (carry)
		cpu->xer |= OSXXERCA;
	else
		cpu->xer &= ~OSXXERCA;
}

static OSXFORCEINLINE void OsxGuestSetOverflow(OsxCpu *cpu, int overflow)
{
	if (overflow)
		cpu->xer |= OSXXEROV | OSXXERSO;
	else
		cpu->xer &= ~OSXXEROV;
}

static OSXFORCEINLINE uint32 OsxGuestRotateLeft(uint32 value, uint32 count)
{
	count &= 31;
	if (count == 0)
		return value;
	return (value << count) | (value >> (32 - count));
}

static OSXFORCEINLINE uint32 OsxGuestRotateMask(uint32 begin, uint32 end)
{
	uint32 first;
	uint32 last;
	first = 0xffffffffu >> begin;
	last = 0xffffffffu << (31 - end);
	if (begin <= end)
		return first & last;
	return first | last;
}

static uint32 OsxGuestCountLeadingZeros(uint32 value)
{
	uint32 count;
	if (value == 0)
		return 32;
	count = 0;
	while (!(value & 0x80000000u)) {
		value <<= 1;
		count++;
	}
	return count;
}

static void OsxGuestProgramException(OsxCpu *cpu, uint32 reason)
{
	OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, reason, cpu->pc);
}

static void OsxGuestIllegalInstruction(OsxCpu *cpu, uint32 instruction)
{
	if (cpu->logfile && !(cpu->msr & OSXMSRPR))
		fprintf(cpu->logfile, "illegal instruction %08x at %08x\n", instruction, cpu->pc);
	OsxGuestProgramException(cpu, OSXPROGRAMILLEGAL);
}

static inline int OsxGuestRequireSupervisor(OsxCpu *cpu)
{
	if (cpu->msr & OSXMSRPR) {
		OsxGuestProgramException(cpu, OSXPROGRAMPRIVILEGED);
		return 0;
	}
	return 1;
}

static inline int OsxGuestRequireFloat(OsxCpu *cpu)
{
	if (!(cpu->msr & OSXMSRFP)) {
		OsxGuestCpuRaiseException(cpu, OSXVECTORFPUNAVAILABLE, 0, cpu->pc);
		return 0;
	}
	return 1;
}

static uint32 OsxGuestAddExtended(OsxCpu *cpu, uint32 left, uint32 right, uint32 carryin, int instruction)
{
	uint64 sum;
	uint32 result;
	sum = (uint64)left + right + carryin;
	result = (uint32)sum;
	OsxGuestSetCarry(cpu, (int)(sum >> 32));
	if (instruction & 0x400)
		OsxGuestSetOverflow(cpu, (int)((~(left ^ right) & (left ^ result)) >> 31));
	return result;
}

static uint32 OsxGuestAddPlain(OsxCpu *cpu, uint32 left, uint32 right, int instruction)
{
	uint32 result;
	result = left + right;
	if (instruction & 0x400)
		OsxGuestSetOverflow(cpu, (int)((~(left ^ right) & (left ^ result)) >> 31));
	return result;
}

static OSXFORCEINLINE void OsxGuestCompareSigned(OsxCpu *cpu, int field, int32 left, int32 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	OsxGuestSetCrField(cpu, field, value | (cpu->xer >> 31));
}

static OSXFORCEINLINE void OsxGuestCompareUnsigned(OsxCpu *cpu, int field, uint32 left, uint32 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	OsxGuestSetCrField(cpu, field, value | (cpu->xer >> 31));
}

static OSXFORCEINLINE int OsxGuestBranchConditionTaken(OsxCpu *cpu, uint32 options, uint32 bit)
{
	int counterok;
	int conditionok;
	if (!(options & 4)) {
		cpu->ctr--;
		if (options & 2)
			counterok = cpu->ctr == 0;
		else
			counterok = cpu->ctr != 0;
	} else {
		counterok = 1;
	}
	if (options & 16)
		conditionok = 1;
	else
		conditionok = OsxGuestCrBit(cpu, (int)bit) == ((options >> 3) & 1);
	return counterok && conditionok;
}

static uint32 OsxGuestReadTimebaseRegister(OsxCpu *cpu, uint32 number)
{
	uint64 timebase;
	timebase = OsxGuestCpuTimebase(cpu);
	if (number == 269)
		return (uint32)(timebase >> 32);
	return (uint32)timebase;
}

static void OsxGuestWriteTimebase(OsxCpu *cpu, uint32 number, uint32 value)
{
	uint64 current;
	uint64 wanted;
	current = OsxGuestCpuTimebase(cpu);
	if (number == 285)
		wanted = ((uint64)value << 32) | (current & 0xffffffffu);
	else
		wanted = (current & 0xffffffff00000000ULL) | value;
	cpu->tboffset += wanted - current;
	if (cpu->decdeadline != OSXNEVER)
		cpu->decdeadline += wanted - current;
	cpu->decstamp += wanted - current;
}

static void OsxGuestWriteDecrementer(OsxCpu *cpu, uint32 value)
{
	cpu->decvalue = value;
	cpu->decstamp = OsxGuestCpuTimebase(cpu);
	if ((int32)value >= 0)
		cpu->decdeadline = cpu->decstamp + value + 1;
	else
		cpu->decdeadline = OSXNEVER;
}

static int OsxGuestReadSpr(OsxCpu *cpu, uint32 number, uint32 *value)
{
	switch (number) {
	case 1: *value = cpu->xer; return 1;
	case 8: *value = cpu->lr; return 1;
	case 9: *value = cpu->ctr; return 1;
	case 18: *value = cpu->dsisr; return 1;
	case 19: *value = cpu->dar; return 1;
	case 22: *value = (uint32)(cpu->decvalue - (uint32)(OsxGuestCpuTimebase(cpu) - cpu->decstamp)); return 1;
	case 25: *value = cpu->sdr1; return 1;
	case 26: *value = cpu->srr0; return 1;
	case 27: *value = cpu->srr1; return 1;
	case 268: case 269: *value = OsxGuestReadTimebaseRegister(cpu, number); return 1;
	case 272: case 273: case 274: case 275: *value = cpu->sprg[number - 272]; return 1;
	case 282: *value = cpu->ear; return 1;
	case 256: *value = cpu->vrsave; return 1;
	case 287: *value = cpu->pvr; return 1;
	case 528: case 529: case 530: case 531: case 532: case 533: case 534: case 535:
		*value = cpu->ibat[number - 528];
		return 1;
	case 536: case 537: case 538: case 539: case 540: case 541: case 542: case 543:
		*value = cpu->dbat[number - 536];
		return 1;
	case 936: case 952: *value = cpu->mmcr[0]; return 1;
	case 940: case 956: *value = cpu->mmcr[1]; return 1;
	case 937: case 953: *value = cpu->pmc[0]; return 1;
	case 938: case 954: *value = cpu->pmc[1]; return 1;
	case 941: case 957: *value = cpu->pmc[2]; return 1;
	case 942: case 958: *value = cpu->pmc[3]; return 1;
	case 939: case 955: *value = cpu->sia; return 1;
	case 1008: *value = cpu->hid0; return 1;
	case 1009: *value = cpu->hid1; return 1;
	case 1010: *value = cpu->iabr; return 1;
	case 1013: *value = cpu->dabr; return 1;
	case 1014: case 1015: *value = cpu->msscr[number - 1014]; return 1;
	case 1017: *value = cpu->l2cr & ~1u; return 1;
	case 1019: *value = cpu->ictc; return 1;
	case 1020: case 1021:
		*value = cpu->thrm[number - 1020];
		if (cpu->thrm[2] & 1)
			*value = (*value & ~0x80000000u) | 0x40000000u;
		return 1;
	case 1022: *value = cpu->thrm[2]; return 1;
	}
	return 0;
}

static int OsxGuestWriteSpr(OsxCpu *cpu, uint32 number, uint32 value)
{
	switch (number) {
	case 1: cpu->xer = value & 0xe000007f; return 1;
	case 8: cpu->lr = value; return 1;
	case 9: cpu->ctr = value; return 1;
	case 18: cpu->dsisr = value; return 1;
	case 19: cpu->dar = value; return 1;
	case 22: OsxGuestWriteDecrementer(cpu, value); return 1;
	case 25:
		if (cpu->sdr1 != value) {
			cpu->sdr1 = value;
			OsxGuestCpuFlushTlb(cpu);
		}
		return 1;
	case 26: cpu->srr0 = value; return 1;
	case 27: cpu->srr1 = value; return 1;
	case 272: case 273: case 274: case 275: cpu->sprg[number - 272] = value; return 1;
	case 256: cpu->vrsave = value; return 1;
	case 282: cpu->ear = value; return 1;
	case 284: case 285: OsxGuestWriteTimebase(cpu, number, value); return 1;
	case 528: case 529: case 530: case 531: case 532: case 533: case 534: case 535:
		if (cpu->ibat[number - 528] != value) {
			OsxGuestInvalidateTlbBlock(cpu, cpu->ibat[(number - 528) & ~1], OSXTLBFETCH, OSXTLBFETCH);
			cpu->ibat[number - 528] = value;
			OsxGuestInvalidateTlbBlock(cpu, cpu->ibat[(number - 528) & ~1], OSXTLBFETCH, OSXTLBFETCH);
		}
		return 1;
	case 536: case 537: case 538: case 539: case 540: case 541: case 542: case 543:
		if (cpu->dbat[number - 536] != value) {
			OsxGuestInvalidateTlbBlock(cpu, cpu->dbat[(number - 536) & ~1], OSXTLBREAD, OSXTLBWRITE);
			cpu->dbat[number - 536] = value;
			OsxGuestInvalidateTlbBlock(cpu, cpu->dbat[(number - 536) & ~1], OSXTLBREAD, OSXTLBWRITE);
		}
		return 1;
	case 952: cpu->mmcr[0] = value; return 1;
	case 956: cpu->mmcr[1] = value; return 1;
	case 953: cpu->pmc[0] = value; return 1;
	case 954: cpu->pmc[1] = value; return 1;
	case 957: cpu->pmc[2] = value; return 1;
	case 958: cpu->pmc[3] = value; return 1;
	case 955: cpu->sia = value; return 1;
	case 1008: cpu->hid0 = value & ~0x00000c00u; return 1;
	case 1009: cpu->hid1 = value; return 1;
	case 1010: cpu->iabr = value; return 1;
	case 1013: cpu->dabr = value; return 1;
	case 1014: case 1015: cpu->msscr[number - 1014] = value; return 1;
	case 1017: cpu->l2cr = value & ~0x00200000u; return 1;
	case 1019: cpu->ictc = value; return 1;
	case 1020: case 1021: case 1022: cpu->thrm[number - 1020] = value; return 1;
	}
	return 0;
}

static void OsxGuestMoveFromSpr(OsxCpu *cpu, uint32 instruction)
{
	uint32 number;
	uint32 value;
	number = ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0);
	if ((number & 0x10) && !OsxGuestRequireSupervisor(cpu))
		return;
	if (!OsxGuestReadSpr(cpu, number, &value)) {
		if (cpu->logfile && !(cpu->msr & OSXMSRPR))
			fprintf(cpu->logfile, "mfspr unknown %u at %08x\n", number, cpu->pc);
		cpu->unknownspr = number;
		value = 0;
	}
	cpu->gpr[(instruction >> 21) & 31] = value;
}

static void OsxGuestMoveToSpr(OsxCpu *cpu, uint32 instruction)
{
	uint32 number;
	number = ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0);
	if ((number & 0x10) && !OsxGuestRequireSupervisor(cpu))
		return;
	if (!OsxGuestWriteSpr(cpu, number, cpu->gpr[(instruction >> 21) & 31])) {
		if (cpu->logfile && !(cpu->msr & OSXMSRPR))
			fprintf(cpu->logfile, "mtspr unknown %u at %08x\n", number, cpu->pc);
		cpu->unknownspr = number;
	}
}

static void OsxGuestWriteMsr(OsxCpu *cpu, uint32 value)
{
	uint32 changed;
	changed = cpu->msr ^ value;
	cpu->msr = value;
	if (changed & (OSXMSRPR | OSXMSRIR | OSXMSRDR)) {
		OsxGuestCpuUpdateModes(cpu);
		cpu->jitflush = 1;
	}
	if (changed & OSXMSRSE)
		cpu->jitflush = 1;
	if ((value & OSXMSRPOW) && (cpu->hid0 & (OSXHID0DOZE | OSXHID0NAP | OSXHID0SLEEP))) {
		cpu->halted = 1;
		cpu->slice = 0;
		cpu->jitflush = 1;
	}
	if ((changed & OSXMSREE) && (value & OSXMSREE) && (cpu->externalline || cpu->decpending)) {
		cpu->slice = 0;
		cpu->jitflush = 1;
	}
}

static void OsxGuestStoreString(OsxCpu *cpu, uint32 address, int first, uint32 count)
{
	uint32 index;
	int reg;
	uint32 shift;
	reg = first;
	shift = 24;
	for (index = 0; index < count; index++) {
		if (!OsxGuestCpuWrite8(cpu, address + index, (cpu->gpr[reg] >> shift) & 0xff))
			return;
		if (shift == 0) {
			shift = 24;
			reg = (reg + 1) & 31;
		} else {
			shift -= 8;
		}
	}
}

static void OsxGuestLoadString(OsxCpu *cpu, uint32 address, int first, uint32 count)
{
	uint32 index;
	uint32 byte;
	int reg;
	uint32 shift;
	reg = first;
	shift = 24;
	for (index = 0; index < count; index++) {
		if (!OsxGuestCpuRead8(cpu, address + index, &byte))
			return;
		if (shift == 24)
			cpu->gpr[reg] = byte << 24;
		else
			cpu->gpr[reg] |= byte << shift;
		if (shift == 0) {
			shift = 24;
			reg = (reg + 1) & 31;
		} else {
			shift -= 8;
		}
	}
}

static void OsxGuestZeroCacheBlock(OsxCpu *cpu, uint32 address)
{
	uint8 *host;
	uint32 physical;
	int status;
	int index;
	address &= ~31u;
	host = OsxGuestWriteHost(cpu, address, 0xfe0);
	if (host) {
		memset(host, 0, 32);
		return;
	}
	status = OsxGuestAccessSlow(cpu, address, OSXTLBWRITE, &host, &physical);
	if (status == OSXACCESSFAULT)
		return;
	if (status == OSXACCESSHOST) {
		if (cpu->watchfile)
			OsxGuestWatchWrite(cpu, physical, 32, 0);
		memset(host, 0, 32);
		return;
	}
	for (index = 0; index < 32; index += 4)
		cpu->buswrite(cpu->buscontext, physical + index, 4, 0);
}

static uint64 OsxGuestSingleToDouble(uint32 word)
{
	uint64 sign;
	uint64 fraction;
	uint32 exponent;
	int32 adjusted;
	sign = (uint64)(word >> 31) << 63;
	exponent = (word >> 23) & 0xff;
	fraction = word & 0x7fffff;
	if (exponent > 0 && exponent < 255)
		return sign | ((uint64)(exponent - 127 + 1023) << 52) | (fraction << 29);
	if (exponent == 255)
		return sign | 0x7ff0000000000000ULL | (fraction << 29);
	if (fraction == 0)
		return sign;
	adjusted = -126;
	while (!(fraction & 0x800000)) {
		fraction <<= 1;
		adjusted--;
	}
	fraction &= 0x7fffff;
	return sign | ((uint64)(adjusted + 1023) << 52) | (fraction << 29);
}

static uint32 OsxGuestDoubleToSingle(uint64 bits)
{
	uint32 exponent;
	uint64 fraction;
	int32 adjusted;
	exponent = (uint32)(bits >> 52) & 0x7ff;
	if (exponent > 896 || exponent == 0)
		return (uint32)((bits >> 32) & 0xc0000000u) | (uint32)((bits >> 29) & 0x3fffffffu);
	if (exponent < 874)
		return (uint32)(bits >> 32) & 0x80000000u;
	fraction = (bits & 0x000fffffffffffffULL) | 0x0010000000000000ULL;
	adjusted = (int32)exponent - 1023;
	while (adjusted < -126) {
		fraction >>= 1;
		adjusted++;
	}
	return ((uint32)(bits >> 32) & 0x80000000u) | (uint32)((fraction >> 29) & 0x7fffff);
}

OSXJITABI uint64 OsxGuestCpuSingleToDouble(uint32 word)
{
	return OsxGuestSingleToDouble(word);
}

OSXJITABI uint32 OsxGuestCpuDoubleToSingle(uint64 bits)
{
	return OsxGuestDoubleToSingle(bits);
}

static OSXFORCEINLINE uint32 OsxGuestOffsetAddress(OsxCpu *cpu, uint32 opcode, int ra, uint32 base, uint32 offset)
{
	if (opcode & 1)
		return cpu->gpr[ra] + offset;
	return base + offset;
}

static OSXFORCEINLINE void OsxGuestUpdateBase(OsxCpu *cpu, uint32 opcode, int ra, uint32 address)
{
	if (opcode & 1)
		cpu->gpr[ra] = address;
}

static void OsxGuestExecuteFloatLoadStore(OsxCpu *cpu, uint32 instruction, uint32 opcode, uint32 address)
{
	uint32 value;
	uint64 wide;
	int target;
	if (!OsxGuestRequireFloat(cpu))
		return;
	target = (instruction >> 21) & 31;
	switch (opcode & ~1u) {
	case 48:
		if (!OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->fpr[target].bits = OsxGuestSingleToDouble(value);
		break;
	case 50:
		if (!OsxGuestRead64(cpu, address, &wide))
			return;
		cpu->fpr[target].bits = wide;
		break;
	case 52:
		if (!OsxGuestCpuWrite32(cpu, address, OsxGuestDoubleToSingle(cpu->fpr[target].bits)))
			return;
		break;
	case 54:
		if (!OsxGuestWrite64(cpu, address, cpu->fpr[target].bits))
			return;
		break;
	}
	OsxGuestUpdateBase(cpu, opcode, (instruction >> 16) & 31, address);
}

static void OsxGuestLoadMultiple(OsxCpu *cpu, uint32 instruction, uint32 address)
{
	int first;
	int reg;
	first = (instruction >> 21) & 31;
	for (reg = first; reg < 32; reg++)
		if (!OsxGuestCpuRead32(cpu, address + (uint32)(reg - first) * 4, &cpu->gpr[reg]))
			return;
}

static void OsxGuestStoreMultiple(OsxCpu *cpu, uint32 instruction, uint32 address)
{
	int first;
	int reg;
	first = (instruction >> 21) & 31;
	for (reg = first; reg < 32; reg++)
		if (!OsxGuestCpuWrite32(cpu, address + (uint32)(reg - first) * 4, cpu->gpr[reg]))
			return;
}

static void OsxGuestExecuteGroup19(OsxCpu *cpu, uint32 instruction)
{
	uint32 extended;
	uint32 target;
	int bita;
	int bitb;
	int bitd;
	uint32 left;
	uint32 right;
	uint32 result;
	extended = (instruction >> 1) & 0x3ff;
	bitd = (instruction >> 21) & 31;
	bita = (instruction >> 16) & 31;
	bitb = (instruction >> 11) & 31;
	left = OsxGuestCrBit(cpu, bita);
	right = OsxGuestCrBit(cpu, bitb);
	switch (extended) {
	case 0:
		OsxGuestSetCrField(cpu, bitd >> 2, (cpu->cr >> ((7 - (bita >> 2)) * 4)) & 0xf);
		return;
	case 16:
		target = cpu->lr & ~3u;
		if (OsxGuestBranchConditionTaken(cpu, (uint32)bitd, (uint32)bita))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 528:
		target = cpu->ctr & ~3u;
		if (OsxGuestBranchConditionTaken(cpu, (uint32)bitd | 4, (uint32)bita))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 50:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->nextpc = cpu->srr0 & ~3u;
		OsxGuestWriteMsr(cpu, (cpu->srr1 & OSXMSRSAVED) | (cpu->msr & OSXMSRILE));
		if ((cpu->msr & OSXMSREE) && (cpu->externalline || cpu->decpending))
			cpu->slice = 0;
		return;
	case 150:
		return;
	case 33: result = !(left | right); break;
	case 129: result = left & !right; break;
	case 193: result = left ^ right; break;
	case 225: result = !(left & right); break;
	case 257: result = left & right; break;
	case 289: result = !(left ^ right); break;
	case 417: result = left | !right; break;
	case 449: result = left | right; break;
	default:
		OsxGuestIllegalInstruction(cpu, instruction);
		return;
	}
	OsxGuestSetCrBit(cpu, bitd, result);
}

static void OsxGuestTrapCheck(OsxCpu *cpu, uint32 options, uint32 left, uint32 right)
{
	int trap;
	trap = 0;
	if ((options & 16) && (int32)left < (int32)right)
		trap = 1;
	if ((options & 8) && (int32)left > (int32)right)
		trap = 1;
	if ((options & 4) && left == right)
		trap = 1;
	if ((options & 2) && left < right)
		trap = 1;
	if ((options & 1) && left > right)
		trap = 1;
	if (trap)
		OsxGuestProgramException(cpu, OSXPROGRAMTRAP);
}

static void OsxGuestExecuteGroup31(OsxCpu *cpu, uint32 instruction)
{
	uint32 extended;
	int rd;
	int ra;
	int rb;
	uint32 source;
	uint32 operand;
	uint32 address;
	uint32 result;
	uint32 value;
	uint32 shift;
	uint32 mask;
	uint64 wide;
	int64 product;
	int setcr;
	extended = (instruction >> 1) & 0x3ff;
	rd = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	rb = (instruction >> 11) & 31;
	setcr = instruction & 1;
	source = cpu->gpr[rd];
	operand = cpu->gpr[rb];
	if (ra)
		address = cpu->gpr[ra] + operand;
	else
		address = operand;
	switch (extended) {
	case 0:
		OsxGuestCompareSigned(cpu, rd >> 2, (int32)cpu->gpr[ra], (int32)operand);
		return;
	case 32:
		OsxGuestCompareUnsigned(cpu, rd >> 2, cpu->gpr[ra], operand);
		return;
	case 4:
		OsxGuestTrapCheck(cpu, (uint32)rd, cpu->gpr[ra], operand);
		return;
	case 8: case 520:
		result = OsxGuestAddExtended(cpu, ~cpu->gpr[ra], operand, 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 10: case 522:
		result = OsxGuestAddExtended(cpu, cpu->gpr[ra], operand, 0, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 136: case 648:
		result = OsxGuestAddExtended(cpu, ~cpu->gpr[ra], operand, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 138: case 650:
		result = OsxGuestAddExtended(cpu, cpu->gpr[ra], operand, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 200: case 712:
		result = OsxGuestAddExtended(cpu, ~cpu->gpr[ra], 0, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 202: case 714:
		result = OsxGuestAddExtended(cpu, cpu->gpr[ra], 0, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 232: case 744:
		result = OsxGuestAddExtended(cpu, ~cpu->gpr[ra], 0xffffffffu, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 234: case 746:
		result = OsxGuestAddExtended(cpu, cpu->gpr[ra], 0xffffffffu, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 40: case 552:
		result = operand - cpu->gpr[ra];
		if (instruction & 0x400)
			OsxGuestSetOverflow(cpu, (int)(((cpu->gpr[ra] ^ operand) & (operand ^ result)) >> 31));
		cpu->gpr[rd] = result;
		break;
	case 266: case 778:
		result = OsxGuestAddPlain(cpu, cpu->gpr[ra], operand, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 104: case 616:
		result = 0u - cpu->gpr[ra];
		if (instruction & 0x400)
			OsxGuestSetOverflow(cpu, cpu->gpr[ra] == 0x80000000u);
		cpu->gpr[rd] = result;
		break;
	case 11:
		wide = (uint64)cpu->gpr[ra] * operand;
		result = (uint32)(wide >> 32);
		cpu->gpr[rd] = result;
		break;
	case 75:
		product = (int64)(int32)cpu->gpr[ra] * (int64)(int32)operand;
		result = (uint32)((uint64)product >> 32);
		cpu->gpr[rd] = result;
		break;
	case 235: case 747:
		product = (int64)(int32)cpu->gpr[ra] * (int64)(int32)operand;
		result = (uint32)product;
		if (instruction & 0x400)
			OsxGuestSetOverflow(cpu, product != (int64)(int32)result);
		cpu->gpr[rd] = result;
		break;
	case 491: case 1003:
		if (operand == 0 || (cpu->gpr[ra] == 0x80000000u && operand == 0xffffffffu)) {
			if ((int32)cpu->gpr[ra] < 0)
				result = 0xffffffffu;
			else
				result = 0;
			if (instruction & 0x400)
				OsxGuestSetOverflow(cpu, 1);
		} else {
			result = (uint32)((int32)cpu->gpr[ra] / (int32)operand);
			if (instruction & 0x400)
				OsxGuestSetOverflow(cpu, 0);
		}
		cpu->gpr[rd] = result;
		break;
	case 459: case 971:
		if (operand == 0) {
			result = 0;
			if (instruction & 0x400)
				OsxGuestSetOverflow(cpu, 1);
		} else {
			result = cpu->gpr[ra] / operand;
			if (instruction & 0x400)
				OsxGuestSetOverflow(cpu, 0);
		}
		cpu->gpr[rd] = result;
		break;
	case 28: result = source & operand; cpu->gpr[ra] = result; break;
	case 60: result = source & ~operand; cpu->gpr[ra] = result; break;
	case 444: result = source | operand; cpu->gpr[ra] = result; break;
	case 412: result = source | ~operand; cpu->gpr[ra] = result; break;
	case 316: result = source ^ operand; cpu->gpr[ra] = result; break;
	case 124: result = ~(source | operand); cpu->gpr[ra] = result; break;
	case 476: result = ~(source & operand); cpu->gpr[ra] = result; break;
	case 284: result = ~(source ^ operand); cpu->gpr[ra] = result; break;
	case 954: result = (uint32)(int32)(int8)source; cpu->gpr[ra] = result; break;
	case 922: result = (uint32)(int32)(int16)source; cpu->gpr[ra] = result; break;
	case 26: result = OsxGuestCountLeadingZeros(source); cpu->gpr[ra] = result; break;
	case 24:
		shift = operand & 0x3f;
		if (shift > 31)
			result = 0;
		else
			result = source << shift;
		cpu->gpr[ra] = result;
		break;
	case 536:
		shift = operand & 0x3f;
		if (shift > 31)
			result = 0;
		else
			result = source >> shift;
		cpu->gpr[ra] = result;
		break;
	case 792:
		shift = operand & 0x3f;
		if (shift > 31) {
			if ((int32)source < 0)
				result = 0xffffffffu;
			else
				result = 0;
			OsxGuestSetCarry(cpu, (int32)source < 0);
		} else if (shift == 0) {
			result = source;
			OsxGuestSetCarry(cpu, 0);
		} else {
			result = (uint32)((int32)source >> shift);
			OsxGuestSetCarry(cpu, (int32)source < 0 && (source & ((1u << shift) - 1)) != 0);
		}
		cpu->gpr[ra] = result;
		break;
	case 824:
		shift = (uint32)rb;
		if (shift == 0) {
			result = source;
			OsxGuestSetCarry(cpu, 0);
		} else {
			result = (uint32)((int32)source >> shift);
			OsxGuestSetCarry(cpu, (int32)source < 0 && (source & ((1u << shift) - 1)) != 0);
		}
		cpu->gpr[ra] = result;
		break;
	case 19:
		cpu->gpr[rd] = cpu->cr;
		return;
	case 144:
		mask = (instruction >> 12) & 0xff;
		mask = (mask | (mask << 12)) & 0x000f000fu;
		mask = (mask | (mask << 6)) & 0x03030303u;
		mask = ((mask | (mask << 3)) & 0x11111111u) * 0xf;
		cpu->cr = (cpu->cr & ~mask) | (source & mask);
		return;
	case 512:
		OsxGuestSetCrField(cpu, rd >> 2, cpu->xer >> 28);
		cpu->xer &= 0x0fffffffu;
		return;
	case 83:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->msr;
		return;
	case 146:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		OsxGuestWriteMsr(cpu, source);
		return;
	case 339:
		OsxGuestMoveFromSpr(cpu, instruction);
		return;
	case 467:
		OsxGuestMoveToSpr(cpu, instruction);
		return;
	case 371:
		cpu->gpr[rd] = OsxGuestReadTimebaseRegister(cpu, ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0));
		return;
	case 595:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->sr[ra & 15];
		return;
	case 659:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->sr[operand >> 28];
		return;
	case 210:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->sr[ra & 15] = source;
		cpu->fetchtag = 0;
		if ((uint32)(ra & 15) == cpu->pc >> 28)
			cpu->jitflush = 1;
		return;
	case 242:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		cpu->sr[operand >> 28] = source;
		cpu->fetchtag = 0;
		if (operand >> 28 == cpu->pc >> 28)
			cpu->jitflush = 1;
		return;
	case 306:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		OsxGuestInvalidateTlbPage(cpu, operand);
		return;
	case 370:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		OsxGuestCpuFlushTlb(cpu);
		return;
	case 566:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		return;
	case 470:
		if (!OsxGuestRequireSupervisor(cpu))
			return;
		return;
	case 54: case 86: case 246: case 278: case 598: case 758: case 854:
		return;
	case 982:
		cpu->fetchtag = 0;
		if (cpu->nativejit && OsxGuestCpuTranslate(cpu, address, OSXTLBREAD, 1, &value) && OsxGuestJitInvalidate(cpu, value & ~31u, 32))
			cpu->jitflush = 1;
		return;
	case 1014:
		OsxGuestZeroCacheBlock(cpu, address);
		return;
	case 20:
		if (address & 3) {
			cpu->dar = address;
			cpu->dsisr = 0;
			OsxGuestCpuRaiseException(cpu, OSXVECTORALIGNMENT, 0, cpu->pc);
			return;
		}
		if (!OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->reservation = address;
		cpu->reservationvalid = 1;
		cpu->gpr[rd] = value;
		return;
	case 150:
		if (address & 3) {
			cpu->dar = address;
			cpu->dsisr = 0;
			OsxGuestCpuRaiseException(cpu, OSXVECTORALIGNMENT, 0, cpu->pc);
			return;
		}
		if (cpu->reservationvalid && cpu->reservation == address) {
			if (!OsxGuestCpuWrite32(cpu, address, source))
				return;
			cpu->reservationvalid = 0;
			OsxGuestSetCrField(cpu, 0, 2 | (cpu->xer >> 31));
		} else {
			cpu->reservationvalid = 0;
			OsxGuestSetCrField(cpu, 0, cpu->xer >> 31);
		}
		return;
	case 23: case 55:
		if (!OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 55)
			cpu->gpr[ra] = address;
		return;
	case 87: case 119:
		if (!OsxGuestCpuRead8(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 119)
			cpu->gpr[ra] = address;
		return;
	case 279: case 311:
		if (!OsxGuestCpuRead16(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 311)
			cpu->gpr[ra] = address;
		return;
	case 343: case 375:
		if (!OsxGuestCpuRead16(cpu, address, &value))
			return;
		cpu->gpr[rd] = (uint32)(int32)(int16)value;
		if (extended == 375)
			cpu->gpr[ra] = address;
		return;
	case 151: case 183:
		if (!OsxGuestCpuWrite32(cpu, address, source))
			return;
		if (extended == 183)
			cpu->gpr[ra] = address;
		return;
	case 215: case 247:
		if (!OsxGuestCpuWrite8(cpu, address, source))
			return;
		if (extended == 247)
			cpu->gpr[ra] = address;
		return;
	case 407: case 439:
		if (!OsxGuestCpuWrite16(cpu, address, source))
			return;
		if (extended == 439)
			cpu->gpr[ra] = address;
		return;
	case 534:
		if (!OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->gpr[rd] = bswap_32(value);
		return;
	case 790:
		if (!OsxGuestCpuRead16(cpu, address, &value))
			return;
		cpu->gpr[rd] = ((value & 0xff) << 8) | (value >> 8);
		return;
	case 662:
		OsxGuestCpuWrite32(cpu, address, bswap_32(source));
		return;
	case 918:
		OsxGuestCpuWrite16(cpu, address, ((source & 0xff) << 8) | ((source >> 8) & 0xff));
		return;
	case 533:
		OsxGuestLoadString(cpu, address, rd, cpu->xer & 0x7f);
		return;
	case 661:
		OsxGuestStoreString(cpu, address, rd, cpu->xer & 0x7f);
		return;
	case 597:
		if (ra)
			address = cpu->gpr[ra];
		else
			address = 0;
		if (rb == 0)
			OsxGuestLoadString(cpu, address, rd, 32);
		else
			OsxGuestLoadString(cpu, address, rd, (uint32)rb);
		return;
	case 725:
		if (ra)
			address = cpu->gpr[ra];
		else
			address = 0;
		if (rb == 0)
			OsxGuestStoreString(cpu, address, rd, 32);
		else
			OsxGuestStoreString(cpu, address, rd, (uint32)rb);
		return;
	case 535: case 567:
		if (!OsxGuestRequireFloat(cpu) || !OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->fpr[rd].bits = OsxGuestSingleToDouble(value);
		if (extended == 567)
			cpu->gpr[ra] = address;
		return;
	case 599: case 631:
		if (!OsxGuestRequireFloat(cpu) || !OsxGuestRead64(cpu, address, &wide))
			return;
		cpu->fpr[rd].bits = wide;
		if (extended == 631)
			cpu->gpr[ra] = address;
		return;
	case 663: case 695:
		if (!OsxGuestRequireFloat(cpu) || !OsxGuestCpuWrite32(cpu, address, OsxGuestDoubleToSingle(cpu->fpr[rd].bits)))
			return;
		if (extended == 695)
			cpu->gpr[ra] = address;
		return;
	case 727: case 759:
		if (!OsxGuestRequireFloat(cpu) || !OsxGuestWrite64(cpu, address, cpu->fpr[rd].bits))
			return;
		if (extended == 759)
			cpu->gpr[ra] = address;
		return;
	case 983:
		if (!OsxGuestRequireFloat(cpu))
			return;
		OsxGuestCpuWrite32(cpu, address, (uint32)cpu->fpr[rd].bits);
		return;
	case 310:
		if (!OsxGuestCpuRead32(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		return;
	case 438:
		OsxGuestCpuWrite32(cpu, address, source);
		return;
	default:
		if (!OsxGuestCpuExecuteVectorMemory(cpu, instruction, extended, address))
			OsxGuestIllegalInstruction(cpu, instruction);
		return;
	}
	if (setcr)
		OsxGuestSetCr0(cpu, result);
}

static OSXFORCEINLINE void OsxGuestExecute(OsxCpu *cpu, uint32 instruction);

static void OsxGuestExecuteVectorQuad(OsxCpu *cpu, uint32 instruction)
{
	uint32 *vector;
	uint32 address;
	uint8 *host;
	int ra;
	ra = (instruction >> 16) & 31;
	address = cpu->gpr[(instruction >> 11) & 31];
	if (ra)
		address += cpu->gpr[ra];
	if (!cpu->altivec || !(cpu->msr & OSXMSRVEC)) {
		OsxGuestCpuExecuteVectorMemory(cpu, instruction, (instruction >> 1) & 0x3ff, address);
		return;
	}
	address &= ~15u;
	vector = cpu->vr[(instruction >> 21) & 31];
	if (instruction & 0x100) {
		host = OsxGuestWriteHost(cpu, address, 0xff0);
		if (host) {
			OsxGuestStoreHost32(host, vector[0]);
			OsxGuestStoreHost32(host + 4, vector[1]);
			OsxGuestStoreHost32(host + 8, vector[2]);
			OsxGuestStoreHost32(host + 12, vector[3]);
			return;
		}
		OsxGuestCpuWriteQuad(cpu, address, vector);
		return;
	}
	host = OsxGuestReadHost(cpu, address, 0xff0);
	if (host) {
		vector[0] = OsxGuestLoadHost32(host);
		vector[1] = OsxGuestLoadHost32(host + 4);
		vector[2] = OsxGuestLoadHost32(host + 8);
		vector[3] = OsxGuestLoadHost32(host + 12);
		return;
	}
	OsxGuestCpuReadQuad(cpu, address, vector);
}

static void OsxGuestExecuteHook(OsxCpu *cpu, uint32 instruction)
{
	uint32 index;
	OsxHook *hook;
	index = instruction & 0x03ffffff;
	if ((cpu->msr & OSXMSRPR) || index >= (uint32)cpu->hookcount) {
		OsxGuestIllegalInstruction(cpu, instruction);
		return;
	}
	hook = &cpu->hooks[index];
	hook->function(cpu, hook->context, hook->address);
	if (cpu->nextpc == cpu->pc + 4)
		OsxGuestExecute(cpu, hook->original);
}

static OSXFORCEINLINE void OsxGuestExecute(OsxCpu *cpu, uint32 instruction)
{
	uint32 opcode;
	int rd;
	int ra;
	uint32 immediate;
	uint32 signedimmediate;
	uint32 base;
	uint32 result;
	uint32 rotated;
	uint32 mask;
	uint32 target;
	uint32 address;
	uint32 value;
	uint64 sum;
	opcode = instruction >> 26;
	rd = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	immediate = instruction & 0xffff;
	signedimmediate = (uint32)(int32)(int16)immediate;
	if (ra)
		base = cpu->gpr[ra];
	else
		base = 0;
	switch (opcode) {
	case 3:
		OsxGuestTrapCheck(cpu, (uint32)rd, cpu->gpr[ra], signedimmediate);
		return;
	case 6:
		OsxGuestExecuteHook(cpu, instruction);
		return;
	case 7:
		cpu->gpr[rd] = (uint32)((int32)cpu->gpr[ra] * (int32)signedimmediate);
		return;
	case 8:
		sum = (uint64)(uint32)~cpu->gpr[ra] + signedimmediate + 1;
		cpu->gpr[rd] = (uint32)sum;
		OsxGuestSetCarry(cpu, (int)(sum >> 32));
		return;
	case 10:
		OsxGuestCompareUnsigned(cpu, rd >> 2, cpu->gpr[ra], immediate);
		return;
	case 11:
		OsxGuestCompareSigned(cpu, rd >> 2, (int32)cpu->gpr[ra], (int32)signedimmediate);
		return;
	case 12:
	case 13:
		sum = (uint64)cpu->gpr[ra] + signedimmediate;
		cpu->gpr[rd] = (uint32)sum;
		OsxGuestSetCarry(cpu, (int)(sum >> 32));
		if (opcode == 13)
			OsxGuestSetCr0(cpu, (uint32)sum);
		return;
	case 14:
		cpu->gpr[rd] = base + signedimmediate;
		return;
	case 15:
		cpu->gpr[rd] = base + (immediate << 16);
		return;
	case 16:
		if (instruction & 2)
			target = signedimmediate & ~3u;
		else
			target = cpu->pc + (signedimmediate & ~3u);
		if (OsxGuestBranchConditionTaken(cpu, (uint32)rd, (uint32)ra))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 17:
		if (cpu->syscallobserver && cpu->syscallobserver(cpu, cpu->syscallcontext, instruction))
			return;
		OsxGuestCpuRaiseException(cpu, OSXVECTORSYSCALL, 0, cpu->pc + 4);
		return;
	case 18:
		target = instruction & 0x03fffffc;
		if (target & 0x02000000)
			target |= 0xfc000000;
		if (!(instruction & 2))
			target += cpu->pc;
		cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 19:
		OsxGuestExecuteGroup19(cpu, instruction);
		return;
	case 20:
		rotated = OsxGuestRotateLeft(cpu->gpr[rd], (instruction >> 11) & 31);
		mask = OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		result = (rotated & mask) | (cpu->gpr[ra] & ~mask);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			OsxGuestSetCr0(cpu, result);
		return;
	case 21:
		rotated = OsxGuestRotateLeft(cpu->gpr[rd], (instruction >> 11) & 31);
		result = rotated & OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			OsxGuestSetCr0(cpu, result);
		return;
	case 23:
		rotated = OsxGuestRotateLeft(cpu->gpr[rd], cpu->gpr[(instruction >> 11) & 31] & 31);
		result = rotated & OsxGuestRotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			OsxGuestSetCr0(cpu, result);
		return;
	case 24:
		cpu->gpr[ra] = cpu->gpr[rd] | immediate;
		return;
	case 25:
		cpu->gpr[ra] = cpu->gpr[rd] | (immediate << 16);
		return;
	case 26:
		cpu->gpr[ra] = cpu->gpr[rd] ^ immediate;
		return;
	case 27:
		cpu->gpr[ra] = cpu->gpr[rd] ^ (immediate << 16);
		return;
	case 28:
		result = cpu->gpr[rd] & immediate;
		cpu->gpr[ra] = result;
		OsxGuestSetCr0(cpu, result);
		return;
	case 29:
		result = cpu->gpr[rd] & (immediate << 16);
		cpu->gpr[ra] = result;
		OsxGuestSetCr0(cpu, result);
		return;
	case 4:
		OsxGuestCpuExecuteVector(cpu, instruction);
		return;
	case 31:
		if ((instruction & 0x7fe) == 0xce || (instruction & 0x7fe) == 0x1ce) {
			OsxGuestExecuteVectorQuad(cpu, instruction);
			return;
		}
		OsxGuestExecuteGroup31(cpu, instruction);
		return;
	case 32:
	case 33:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuRead32(cpu, address, &value)) {
			cpu->gpr[rd] = value;
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		}
		return;
	case 34:
	case 35:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuRead8(cpu, address, &value)) {
			cpu->gpr[rd] = value;
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		}
		return;
	case 40:
	case 41:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuRead16(cpu, address, &value)) {
			cpu->gpr[rd] = value;
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		}
		return;
	case 42:
	case 43:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuRead16(cpu, address, &value)) {
			cpu->gpr[rd] = (uint32)(int32)(int16)value;
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		}
		return;
	case 36:
	case 37:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuWrite32(cpu, address, cpu->gpr[rd]))
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		return;
	case 38:
	case 39:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuWrite8(cpu, address, cpu->gpr[rd]))
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		return;
	case 44:
	case 45:
		address = OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate);
		if (OsxGuestCpuWrite16(cpu, address, cpu->gpr[rd]))
			OsxGuestUpdateBase(cpu, opcode, ra, address);
		return;
	case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
		OsxGuestExecuteFloatLoadStore(cpu, instruction, opcode, OsxGuestOffsetAddress(cpu, opcode, ra, base, signedimmediate));
		return;
	case 46:
		OsxGuestLoadMultiple(cpu, instruction, base + signedimmediate);
		return;
	case 47:
		OsxGuestStoreMultiple(cpu, instruction, base + signedimmediate);
		return;
	case 59:
	case 63:
		if (!OsxGuestRequireFloat(cpu))
			return;
		OsxGuestCpuExecuteFloat(cpu, instruction);
		return;
	}
	OsxGuestIllegalInstruction(cpu, instruction);
}

static void OsxGuestExecuteTraced(OsxCpu *cpu, uint32 instruction)
{
	uint64 exceptions;
	exceptions = cpu->exceptions;
	OsxGuestExecute(cpu, instruction);
	if (cpu->exceptions == exceptions)
		OsxGuestCpuRaiseException(cpu, OSXVECTORTRACE, 0, cpu->nextpc);
}

OSXJITABI int OsxGuestCpuExecuteInline(OsxCpu *cpu, uint32 instruction)
{
	uint32 expected;
	expected = cpu->pc + 4;
#if OSXPROFILE
	cpu->tmpform[0][OsxGuestCpuTmpFormKey(instruction)]++;
#endif
	cpu->nextpc = expected;
	cpu->jitflush = 0;
	OsxGuestExecute(cpu, instruction);
	if (cpu->nextpc == expected && !cpu->jitflush)
		return 0;
	cpu->jitflush = 0;
	cpu->pc = cpu->nextpc;
	return 1;
}

void OsxGuestCpuExecuteInstruction(OsxCpu *cpu, uint32 instruction)
{
#if OSXPROFILE
	cpu->tmpform[1][OsxGuestCpuTmpFormKey(instruction)]++;
#endif
	cpu->nextpc = cpu->pc + 4;
	if (cpu->msr & OSXMSRSE)
		OsxGuestExecuteTraced(cpu, instruction);
	else
		OsxGuestExecute(cpu, instruction);
	cpu->pc = cpu->nextpc;
	cpu->instructions++;
}

void OsxGuestCpuCheckInterrupts(OsxCpu *cpu, uint64 now)
{
	if (!cpu->decpending && cpu->decdeadline != OSXNEVER && now >= cpu->decdeadline) {
		cpu->decpending = 1;
		cpu->decdeadline = OSXNEVER;
	}
	if (!(cpu->msr & OSXMSREE))
		return;
	if (cpu->externalline) {
		cpu->externalcount++;
		OsxGuestCpuRaiseException(cpu, OSXVECTOREXTERNAL, 0, cpu->pc);
		cpu->pc = cpu->nextpc;
		return;
	}
	if (cpu->decpending) {
		cpu->decpending = 0;
		cpu->decrementercount++;
		OsxGuestCpuRaiseException(cpu, OSXVECTORDECREMENTER, 0, cpu->pc);
		cpu->pc = cpu->nextpc;
	}
}

void OsxGuestCpuRun(OsxCpu *cpu, int32 count)
{
	uint32 instruction;
	if (cpu->halted)
		return;
	cpu->slice = count;
	while (cpu->slice > 0) {
		cpu->slice--;
		if (((cpu->pc & OSXPAGEMASK) | 1) != cpu->fetchtag && !OsxGuestCpuFetchPage(cpu)) {
			cpu->pc = cpu->nextpc;
			continue;
		}
		instruction = OsxGuestLoadHost32(cpu->fetchhost + (cpu->pc & 0xfff));
		if (cpu->tracefile)
			fprintf(cpu->tracefile, "%08x %08x r3=%08x r4=%08x lr=%08x\n", cpu->pc, instruction, cpu->gpr[3], cpu->gpr[4], cpu->lr);
		cpu->nextpc = cpu->pc + 4;
		if (cpu->msr & OSXMSRSE)
			OsxGuestExecuteTraced(cpu, instruction);
		else
			OsxGuestExecute(cpu, instruction);
		cpu->pc = cpu->nextpc;
		cpu->instructions++;
	}
}

void OsxGuestCpuDump(OsxCpu *cpu, FILE *output)
{
	int index;
	fprintf(output, "pc %08x msr %08x lr %08x ctr %08x cr %08x xer %08x\n", cpu->pc, cpu->msr, cpu->lr, cpu->ctr, cpu->cr, cpu->xer);
	fprintf(output, "srr0 %08x srr1 %08x dar %08x dsisr %08x sdr1 %08x dec %08x\n", cpu->srr0, cpu->srr1, cpu->dar, cpu->dsisr, cpu->sdr1, cpu->decvalue);
	for (index = 0; index < 32; index += 4)
		fprintf(output, "r%-2d %08x %08x %08x %08x\n", index, cpu->gpr[index], cpu->gpr[index + 1], cpu->gpr[index + 2], cpu->gpr[index + 3]);
	for (index = 0; index < 16; index += 4)
		fprintf(output, "sr%-2d %08x %08x %08x %08x\n", index, cpu->sr[index], cpu->sr[index + 1], cpu->sr[index + 2], cpu->sr[index + 3]);
	for (index = 0; index < 8; index += 2)
		fprintf(output, "ibat%d %08x %08x dbat%d %08x %08x\n", index / 2, cpu->ibat[index], cpu->ibat[index + 1], index / 2, cpu->dbat[index], cpu->dbat[index + 1]);
	fprintf(output, "sprg %08x %08x %08x %08x hid0 %08x instructions %llu\n", cpu->sprg[0], cpu->sprg[1], cpu->sprg[2], cpu->sprg[3], cpu->hid0, (unsigned long long)cpu->instructions);
}

#pragma fenv_access (on)

#define FPSCRFX 0x80000000u
#define FPSCRFEX 0x40000000u
#define FPSCRVX 0x20000000u
#define FPSCROX 0x10000000u
#define FPSCRUX 0x08000000u
#define FPSCRZX 0x04000000u
#define FPSCRXX 0x02000000u
#define FPSCRVXSNAN 0x01000000u
#define FPSCRVXISI 0x00800000u
#define FPSCRVXIDI 0x00400000u
#define FPSCRVXZDZ 0x00200000u
#define FPSCRVXIMZ 0x00100000u
#define FPSCRVXVC 0x00080000u
#define FPSCRFPRF 0x0001f000u
#define FPSCRVXSOFT 0x00000400u
#define FPSCRVXSQRT 0x00000200u
#define FPSCRVXCVI 0x00000100u
#define FPSCRVXALL 0x01f80700u
#define FPSCRSTICKY 0x9ff80700u
#define FPSCRENABLES 0x000000f8u

#define FLOATEXPONENT 0x7ff0000000000000ULL
#define FLOATFRACTION 0x000fffffffffffffULL
#define FLOATQUIET 0x0008000000000000ULL
#define FLOATSIGN 0x8000000000000000ULL
#define FLOATDEFAULTNAN 0x7ff8000000000000ULL

static inline int OsxGuestIsNan(uint64 bits)
{
	return (bits & FLOATEXPONENT) == FLOATEXPONENT && (bits & FLOATFRACTION) != 0;
}

static inline int OsxGuestIsSignalingNan(uint64 bits)
{
	return OsxGuestIsNan(bits) && !(bits & FLOATQUIET);
}

static inline int OsxGuestIsInfinity(uint64 bits)
{
	return (bits & ~FLOATSIGN) == FLOATEXPONENT;
}

static inline int OsxGuestIsZero(uint64 bits)
{
	return (bits & ~FLOATSIGN) == 0;
}

static inline double OsxGuestBitsToDouble(uint64 bits)
{
	OsxFloat value;
	value.bits = bits;
	return value.value;
}

static inline uint64 OsxGuestDoubleToBits(double number)
{
	OsxFloat value;
	value.value = number;
	return value.bits;
}

static uint32 OsxGuestClassifyResult(uint64 bits, int single)
{
	uint32 exponent;
	int negative;
	negative = (bits & FLOATSIGN) != 0;
	exponent = (uint32)(bits >> 52) & 0x7ff;
	if (single && exponent != 0 && exponent < 897) {
		if (negative)
			return 0x18;
		return 0x14;
	}
	if (exponent == 0x7ff) {
		if (bits & FLOATFRACTION)
			return 0x11;
		if (negative)
			return 0x09;
		return 0x05;
	}
	if (exponent == 0) {
		if ((bits & FLOATFRACTION) == 0) {
			if (negative)
				return 0x12;
			return 0x02;
		}
		if (negative)
			return 0x18;
		return 0x14;
	}
	if (negative)
		return 0x08;
	return 0x04;
}

static void OsxGuestUpdateSummary(OsxCpu *cpu)
{
	uint32 fpscr;
	fpscr = cpu->fpscr;
	if (fpscr & FPSCRVXALL)
		fpscr |= FPSCRVX;
	else
		fpscr &= ~FPSCRVX;
	if ((((fpscr >> 22) & 0x7c) | ((fpscr >> 29) & 0x80)) & fpscr & FPSCRENABLES)
		fpscr |= FPSCRFEX;
	else
		fpscr &= ~FPSCRFEX;
	cpu->fpscr = fpscr;
}

static void OsxGuestRaiseFloatFlags(OsxCpu *cpu, uint32 flags)
{
	uint32 fresh;
	fresh = flags & ~cpu->fpscr;
	if (fresh)
		cpu->fpscr |= flags | FPSCRFX;
	OsxGuestUpdateSummary(cpu);
}

static uint64 OsxGuestPropagateNan(OsxCpu *cpu, uint64 first, uint64 second, uint64 third, int count)
{
	uint32 flags;
	flags = 0;
	if (OsxGuestIsSignalingNan(first) || OsxGuestIsSignalingNan(second) || (count > 2 && OsxGuestIsSignalingNan(third)))
		flags |= FPSCRVXSNAN;
	if (flags)
		OsxGuestRaiseFloatFlags(cpu, flags);
	if (OsxGuestIsNan(first))
		return first | FLOATQUIET;
	if (OsxGuestIsNan(second))
		return second | FLOATQUIET;
	return third | FLOATQUIET;
}

static uint64 OsxGuestInvalidResult(OsxCpu *cpu, uint32 reason)
{
	OsxGuestRaiseFloatFlags(cpu, reason);
	return FLOATDEFAULTNAN;
}

static uint64 OsxGuestRoundSingle(uint64 bits)
{
	float narrow;
	if (OsxGuestIsNan(bits))
		return bits & 0xffffffffe0000000ULL;
	narrow = (float)OsxGuestBitsToDouble(bits);
	return OsxGuestDoubleToBits((double)narrow);
}

static void OsxGuestFinishResult(OsxCpu *cpu, int target, uint64 bits, int single, int inputsfinite)
{
	if (single)
		bits = OsxGuestRoundSingle(bits);
	if (inputsfinite && OsxGuestIsInfinity(bits))
		OsxGuestRaiseFloatFlags(cpu, FPSCROX | FPSCRXX);
	cpu->fpr[target].bits = bits;
	cpu->fpscr = (cpu->fpscr & ~FPSCRFPRF) | (OsxGuestClassifyResult(bits, single) << 12);
}

void OsxGuestCpuSetFloatCompare(OsxCpu *cpu, int field, double left, double right, int ordered)
{
	uint32 value;
	uint64 leftbits;
	uint64 rightbits;
	int shift;
	leftbits = OsxGuestDoubleToBits(left);
	rightbits = OsxGuestDoubleToBits(right);
	if (OsxGuestIsNan(leftbits) || OsxGuestIsNan(rightbits)) {
		value = 1;
		if (OsxGuestIsSignalingNan(leftbits) || OsxGuestIsSignalingNan(rightbits)) {
			if (ordered)
				OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXVC);
			else
				OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN);
		} else if (ordered) {
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXVC);
		}
	} else if (left < right) {
		value = 8;
	} else if (left > right) {
		value = 4;
	} else {
		value = 2;
	}
	cpu->fpscr = (cpu->fpscr & ~0x0000f000u) | (value << 12);
	shift = (7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | (value << shift);
}

static uint64 OsxGuestConvertToInteger(OsxCpu *cpu, uint64 bits, int truncate)
{
	double value;
	double rounded;
	uint32 mode;
	int32 integer;
	if (OsxGuestIsNan(bits)) {
		if (OsxGuestIsSignalingNan(bits))
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXSNAN | FPSCRVXCVI);
		else
			OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	value = OsxGuestBitsToDouble(bits);
	if (truncate)
		mode = 1;
	else
		mode = cpu->fpscr & 3;
	if (mode == 0)
		rounded = rint(value);
	else if (mode == 1)
		rounded = trunc(value);
	else if (mode == 2)
		rounded = ceil(value);
	else
		rounded = floor(value);
	if (rounded > 2147483647.0) {
		OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff800007fffffffULL;
	}
	if (rounded < -2147483648.0) {
		OsxGuestRaiseFloatFlags(cpu, FPSCRVXCVI);
		return 0xfff8000080000000ULL;
	}
	if (rounded != value)
		cpu->fpscr |= FPSCRXX | FPSCRFX;
	integer = (int32)rounded;
	return 0xfff8000000000000ULL | (uint32)integer;
}

static void OsxGuestMoveToFpscrField(OsxCpu *cpu, int field, uint32 value)
{
	int shift;
	uint32 mask;
	shift = (7 - field) * 4;
	mask = 0xfu << shift;
	if (field == 0)
		mask &= ~(FPSCRFEX | FPSCRVX);
	cpu->fpscr = (cpu->fpscr & ~mask) | ((value << shift) & mask);
}

static void OsxGuestExecuteArithmetic(OsxCpu *cpu, uint32 instruction, int single)
{
	uint32 extended;
	int target;
	uint64 a;
	uint64 b;
	uint64 c;
	uint64 result;
	double left;
	double right;
	double multiplier;
	int finite;
	target = (instruction >> 21) & 31;
	a = cpu->fpr[(instruction >> 16) & 31].bits;
	b = cpu->fpr[(instruction >> 11) & 31].bits;
	c = cpu->fpr[(instruction >> 6) & 31].bits;
	extended = (instruction >> 1) & 31;
	left = OsxGuestBitsToDouble(a);
	right = OsxGuestBitsToDouble(b);
	multiplier = OsxGuestBitsToDouble(c);
	switch (extended) {
	case 18:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b) && !OsxGuestIsZero(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsZero(a) && OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXZDZ);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXIDI);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(left / right);
		}
		break;
	case 20:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b) && ((a ^ b) & FLOATSIGN) == 0)
			result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
		else
			result = OsxGuestDoubleToBits(left - right);
		break;
	case 21:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, a, b, 0, 2);
		else if (OsxGuestIsInfinity(a) && OsxGuestIsInfinity(b) && ((a ^ b) & FLOATSIGN) != 0)
			result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
		else
			result = OsxGuestDoubleToBits(left + right);
		break;
	case 22:
		finite = !OsxGuestIsInfinity(b);
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXSQRT);
		else
			result = OsxGuestDoubleToBits(sqrt(right));
		break;
	case 23:
		finite = 0;
		if (!OsxGuestIsNan(a) && left >= 0.0)
			result = c;
		else
			result = b;
		cpu->fpr[target].bits = result;
		if (instruction & 1)
			cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
		return;
	case 24:
		finite = 0;
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(1.0 / right);
		}
		single = 1;
		break;
	case 25:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(c);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(c))
			result = OsxGuestPropagateNan(cpu, a, c, 0, 2);
		else if ((OsxGuestIsInfinity(a) && OsxGuestIsZero(c)) || (OsxGuestIsZero(a) && OsxGuestIsInfinity(c)))
			result = OsxGuestInvalidResult(cpu, FPSCRVXIMZ);
		else
			result = OsxGuestDoubleToBits(left * multiplier);
		break;
	case 26:
		finite = 0;
		if (OsxGuestIsNan(b))
			result = OsxGuestPropagateNan(cpu, b, 0, 0, 1);
		else if ((b & FLOATSIGN) && !OsxGuestIsZero(b))
			result = OsxGuestInvalidResult(cpu, FPSCRVXSQRT);
		else {
			if (OsxGuestIsZero(b))
				OsxGuestRaiseFloatFlags(cpu, FPSCRZX);
			result = OsxGuestDoubleToBits(1.0 / sqrt(right));
		}
		break;
	case 28:
	case 29:
	case 30:
	case 31:
		finite = !OsxGuestIsInfinity(a) && !OsxGuestIsInfinity(b) && !OsxGuestIsInfinity(c);
		if (OsxGuestIsNan(a) || OsxGuestIsNan(b) || OsxGuestIsNan(c)) {
			result = OsxGuestPropagateNan(cpu, a, b, c, 3);
		} else if ((OsxGuestIsInfinity(a) && OsxGuestIsZero(c)) || (OsxGuestIsZero(a) && OsxGuestIsInfinity(c))) {
			result = OsxGuestInvalidResult(cpu, FPSCRVXIMZ);
		} else {
			if (extended == 28 || extended == 30)
				right = -right;
			result = OsxGuestDoubleToBits(fma(left, multiplier, right));
			if (OsxGuestIsNan(result))
				result = OsxGuestInvalidResult(cpu, FPSCRVXISI);
			else if (extended >= 30)
				result ^= FLOATSIGN;
		}
		break;
	default:
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	OsxGuestFinishResult(cpu, target, result, single, finite);
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void OsxGuestExecuteControl(OsxCpu *cpu, uint32 instruction)
{
	uint32 extended;
	int target;
	int sourcefield;
	int field;
	uint64 b;
	uint32 mask;
	uint32 value;
	uint32 bit;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	b = cpu->fpr[(instruction >> 11) & 31].bits;
	switch (extended) {
	case 0:
		OsxGuestCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, OsxGuestBitsToDouble(b), 0);
		return;
	case 32:
		OsxGuestCpuSetFloatCompare(cpu, target >> 2, cpu->fpr[(instruction >> 16) & 31].value, OsxGuestBitsToDouble(b), 1);
		return;
	case 12:
		OsxGuestFinishResult(cpu, target, b, 1, !OsxGuestIsInfinity(b));
		break;
	case 14:
		cpu->fpr[target].bits = OsxGuestConvertToInteger(cpu, b, 0);
		break;
	case 15:
		cpu->fpr[target].bits = OsxGuestConvertToInteger(cpu, b, 1);
		break;
	case 40:
		cpu->fpr[target].bits = b ^ FLOATSIGN;
		break;
	case 72:
		cpu->fpr[target].bits = b;
		break;
	case 136:
		cpu->fpr[target].bits = b | FLOATSIGN;
		break;
	case 264:
		cpu->fpr[target].bits = b & ~FLOATSIGN;
		break;
	case 583:
		cpu->fpr[target].bits = 0xfff8000000000000ULL | cpu->fpscr;
		break;
	case 64:
		sourcefield = ((instruction >> 18) & 7);
		value = (cpu->fpscr >> ((7 - sourcefield) * 4)) & 0xf;
		cpu->cr = (cpu->cr & ~(0xfu << ((7 - (target >> 2)) * 4))) | (value << ((7 - (target >> 2)) * 4));
		cpu->fpscr &= ~((0xfu << ((7 - sourcefield) * 4)) & FPSCRSTICKY);
		OsxGuestUpdateSummary(cpu);
		return;
	case 38:
	case 70:
		bit = 0x80000000u >> target;
		if (bit != FPSCRFEX && bit != FPSCRVX) {
			if (extended == 38)
				cpu->fpscr |= bit;
			else
				cpu->fpscr &= ~bit;
		}
		OsxGuestUpdateSummary(cpu);
		break;
	case 134:
		OsxGuestMoveToFpscrField(cpu, target >> 2, (instruction >> 12) & 0xf);
		OsxGuestUpdateSummary(cpu);
		break;
	case 711:
		mask = 0;
		value = (instruction >> 17) & 0xff;
		for (field = 0; field < 8; field++)
			if (value & (0x80u >> field))
				mask |= 0xf0000000u >> (field * 4);
		mask &= ~(FPSCRFEX | FPSCRVX);
		cpu->fpscr = (cpu->fpscr & ~mask) | ((uint32)b & mask);
		OsxGuestUpdateSummary(cpu);
		break;
	default:
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL, cpu->pc);
		return;
	}
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void OsxGuestExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	if ((instruction >> 26) == 59) {
		OsxGuestExecuteArithmetic(cpu, instruction, 1);
		return;
	}
	if (instruction & 0x20)
		OsxGuestExecuteArithmetic(cpu, instruction, 0);
	else
		OsxGuestExecuteControl(cpu, instruction);
}

void OsxGuestCpuExecuteFloat(OsxCpu *cpu, uint32 instruction)
{
	static const int roundingmodes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
	if (!(cpu->fpscr & 3)) {
		OsxGuestExecuteFloat(cpu, instruction);
		return;
	}
	fesetround(roundingmodes[cpu->fpscr & 3]);
	OsxGuestExecuteFloat(cpu, instruction);
	fesetround(FE_TONEAREST);
}

#define VSCRSAT 0x00000001
#define VECADDM 0
#define VECSUBM 1
#define VECADDUS 2
#define VECADDSS 3
#define VECSUBUS 4
#define VECSUBSS 5
#define VECMAXU 6
#define VECMAXS 7
#define VECMINU 8
#define VECMINS 9
#define VECAVGU 10
#define VECAVGS 11
#define VECRL 12
#define VECSL 13
#define VECSR 14
#define VECSRA 15
#define VECCMPEQ 0
#define VECCMPGTU 1
#define VECCMPGTS 2

static inline uint32 OsxGuestElementMask(uint32 size)
{
	return (uint32)(((uint64)1 << (size * 8)) - 1);
}

static inline int32 OsxGuestSigned(uint32 value, uint32 size)
{
	uint32 bits;
	bits = 32 - size * 8;
	return (int32)(value << bits) >> bits;
}

static inline void OsxGuestUnpack(const uint32 *vector, uint32 size, uint32 *elements)
{
	uint32 index;
	if (size == 4) {
		elements[0] = vector[0];
		elements[1] = vector[1];
		elements[2] = vector[2];
		elements[3] = vector[3];
		return;
	}
	if (size == 2) {
		for (index = 0; index < 4; index++) {
			elements[index * 2] = vector[index] >> 16;
			elements[index * 2 + 1] = vector[index] & 0xffff;
		}
		return;
	}
	for (index = 0; index < 4; index++) {
		elements[index * 4] = vector[index] >> 24;
		elements[index * 4 + 1] = (vector[index] >> 16) & 0xff;
		elements[index * 4 + 2] = (vector[index] >> 8) & 0xff;
		elements[index * 4 + 3] = vector[index] & 0xff;
	}
}

static inline void OsxGuestPack(uint32 *vector, uint32 size, const uint32 *elements)
{
	uint32 index;
	if (size == 4) {
		vector[0] = elements[0];
		vector[1] = elements[1];
		vector[2] = elements[2];
		vector[3] = elements[3];
		return;
	}
	if (size == 2) {
		for (index = 0; index < 4; index++)
			vector[index] = (elements[index * 2] << 16) | (elements[index * 2 + 1] & 0xffff);
		return;
	}
	for (index = 0; index < 4; index++)
		vector[index] = (elements[index * 4] << 24) | ((elements[index * 4 + 1] & 0xff) << 16) | ((elements[index * 4 + 2] & 0xff) << 8) | (elements[index * 4 + 3] & 0xff);
}

static inline uint32 OsxGuestGetByte(const uint32 *vector, uint32 index)
{
	return (vector[index >> 2] >> (24 - ((index & 3) << 3))) & 0xff;
}

static inline void OsxGuestSetByte(uint32 *vector, uint32 index, uint32 value)
{
	uint32 shift;
	shift = 24 - ((index & 3) << 3);
	vector[index >> 2] = (vector[index >> 2] & ~(0xffu << shift)) | ((value & 0xff) << shift);
}

static inline uint32 OsxGuestGetHalf(const uint32 *vector, uint32 index)
{
	return (vector[index >> 1] >> (16 - ((index & 1) << 4))) & 0xffff;
}

static inline void OsxGuestSetHalf(uint32 *vector, uint32 index, uint32 value)
{
	uint32 shift;
	shift = 16 - ((index & 1) << 4);
	vector[index >> 1] = (vector[index >> 1] & ~(0xffffu << shift)) | ((value & 0xffff) << shift);
}

static inline void OsxGuestUnpackBytes(const uint32 *vector, uint8 *bytes)
{
	uint32 index;
	for (index = 0; index < 4; index++) {
		bytes[index * 4] = (uint8)(vector[index] >> 24);
		bytes[index * 4 + 1] = (uint8)(vector[index] >> 16);
		bytes[index * 4 + 2] = (uint8)(vector[index] >> 8);
		bytes[index * 4 + 3] = (uint8)vector[index];
	}
}

static inline void OsxGuestPackBytes(uint32 *vector, const uint8 *bytes)
{
	uint32 index;
	for (index = 0; index < 4; index++)
		vector[index] = ((uint32)bytes[index * 4] << 24) | ((uint32)bytes[index * 4 + 1] << 16) | ((uint32)bytes[index * 4 + 2] << 8) | bytes[index * 4 + 3];
}

static uint32 OsxGuestSaturateSigned(OsxCpu *cpu, int64 value, uint32 size)
{
	int64 high;
	int64 low;
	high = ((int64)1 << (size * 8 - 1)) - 1;
	low = -high - 1;
	if (value > high) {
		cpu->vscr |= VSCRSAT;
		value = high;
	} else if (value < low) {
		cpu->vscr |= VSCRSAT;
		value = low;
	}
	return (uint32)value & OsxGuestElementMask(size);
}

static uint32 OsxGuestSaturateUnsigned(OsxCpu *cpu, int64 value, uint32 size)
{
	int64 high;
	high = (int64)OsxGuestElementMask(size);
	if (value > high) {
		cpu->vscr |= VSCRSAT;
		value = high;
	} else if (value < 0) {
		cpu->vscr |= VSCRSAT;
		value = 0;
	}
	return (uint32)value;
}

static inline float OsxGuestGetFloat(const uint32 *vector, uint32 index)
{
	float value;
	memcpy(&value, &vector[index], 4);
	return value;
}

static inline void OsxGuestSetFloat(uint32 *vector, uint32 index, float value)
{
	memcpy(&vector[index], &value, 4);
}

static inline int OsxGuestVectorIsNan(float value)
{
	return value != value;
}

static void OsxGuestIntegerVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 count;
	uint32 mask;
	uint32 bits;
	uint32 amount;
	uint32 index;
	int64 sum;
	count = 16 / size;
	mask = OsxGuestElementMask(size);
	bits = size * 8;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	switch (operation) {
	case VECADDM:
		for (index = 0; index < count; index++)
			r[index] = (a[index] + b[index]) & mask;
		break;
	case VECSUBM:
		for (index = 0; index < count; index++)
			r[index] = (a[index] - b[index]) & mask;
		break;
	case VECADDUS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateUnsigned(cpu, (int64)a[index] + b[index], size);
		break;
	case VECADDSS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateSigned(cpu, (int64)OsxGuestSigned(a[index], size) + OsxGuestSigned(b[index], size), size);
		break;
	case VECSUBUS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateUnsigned(cpu, (int64)a[index] - b[index], size);
		break;
	case VECSUBSS:
		for (index = 0; index < count; index++)
			r[index] = OsxGuestSaturateSigned(cpu, (int64)OsxGuestSigned(a[index], size) - OsxGuestSigned(b[index], size), size);
		break;
	case VECMAXU:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (a[index] > b[index])
				r[index] = a[index];
		}
		break;
	case VECMINU:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (a[index] < b[index])
				r[index] = a[index];
		}
		break;
	case VECMAXS:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (OsxGuestSigned(a[index], size) > OsxGuestSigned(b[index], size))
				r[index] = a[index];
		}
		break;
	case VECMINS:
		for (index = 0; index < count; index++) {
			r[index] = b[index];
			if (OsxGuestSigned(a[index], size) < OsxGuestSigned(b[index], size))
				r[index] = a[index];
		}
		break;
	case VECAVGU:
		for (index = 0; index < count; index++)
			r[index] = (uint32)(((uint64)a[index] + b[index] + 1) >> 1);
		break;
	case VECAVGS:
		for (index = 0; index < count; index++) {
			sum = (int64)OsxGuestSigned(a[index], size) + OsxGuestSigned(b[index], size) + 1;
			r[index] = (uint32)((sum - (sum & 1)) / 2) & mask;
		}
		break;
	case VECRL:
		for (index = 0; index < count; index++) {
			amount = b[index] & (bits - 1);
			r[index] = a[index];
			if (amount)
				r[index] = ((a[index] << amount) | (a[index] >> (bits - amount))) & mask;
		}
		break;
	case VECSL:
		for (index = 0; index < count; index++)
			r[index] = (a[index] << (b[index] & (bits - 1))) & mask;
		break;
	case VECSR:
		for (index = 0; index < count; index++)
			r[index] = a[index] >> (b[index] & (bits - 1));
		break;
	case VECSRA:
		for (index = 0; index < count; index++)
			r[index] = (uint32)(OsxGuestSigned(a[index], size) >> (b[index] & (bits - 1))) & mask;
		break;
	}
	OsxGuestPack(result, size, r);
}

static void OsxGuestMultiplyVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 odd, int issigned)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[8];
	uint32 index;
	uint32 mask;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	mask = OsxGuestElementMask(size * 2);
	for (index = 0; index < 8 / size; index++) {
		if (issigned)
			r[index] = (uint32)((int64)OsxGuestSigned(a[index * 2 + odd], size) * OsxGuestSigned(b[index * 2 + odd], size)) & mask;
		else
			r[index] = (uint32)((uint64)a[index * 2 + odd] * b[index * 2 + odd]) & mask;
	}
	OsxGuestPack(result, size * 2, r);
}

static void OsxGuestMergeVector(uint32 *result, const uint32 *left, const uint32 *right, uint32 size, uint32 low)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 index;
	uint32 base;
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	base = low * (8 / size);
	for (index = 0; index < 8 / size; index++) {
		r[index * 2] = a[base + index];
		r[index * 2 + 1] = b[base + index];
	}
	OsxGuestPack(result, size, r);
}

static void OsxGuestPackVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int sourcesigned, int saturate, int targetsigned)
{
	uint32 source[32];
	uint32 r[16];
	uint32 index;
	uint32 count;
	uint32 mask;
	int64 wide;
	count = 16 / size;
	OsxGuestUnpack(left, size, source);
	OsxGuestUnpack(right, size, source + count);
	mask = OsxGuestElementMask(size / 2);
	for (index = 0; index < count * 2; index++) {
		if (!saturate) {
			r[index] = source[index] & mask;
			continue;
		}
		wide = (int64)source[index];
		if (sourcesigned)
			wide = OsxGuestSigned(source[index], size);
		if (targetsigned)
			r[index] = OsxGuestSaturateSigned(cpu, wide, size / 2);
		else
			r[index] = OsxGuestSaturateUnsigned(cpu, wide, size / 2);
	}
	OsxGuestPack(result, size / 2, r);
}

static void OsxGuestUnpackVector(uint32 *result, const uint32 *source, uint32 size, uint32 low)
{
	uint32 a[16];
	uint32 r[8];
	uint32 index;
	uint32 base;
	uint32 mask;
	OsxGuestUnpack(source, size, a);
	base = low * (8 / size);
	mask = OsxGuestElementMask(size * 2);
	for (index = 0; index < 8 / size; index++)
		r[index] = (uint32)OsxGuestSigned(a[base + index], size) & mask;
	OsxGuestPack(result, size * 2, r);
}

static void OsxGuestUnpackPixels(uint32 *result, const uint32 *source, uint32 low)
{
	uint32 index;
	uint32 pixel;
	uint32 alpha;
	for (index = 0; index < 4; index++) {
		pixel = OsxGuestGetHalf(source, low * 4 + index);
		alpha = 0;
		if (pixel & 0x8000)
			alpha = 0xff;
		result[index] = (alpha << 24) | (((pixel >> 10) & 0x1f) << 16) | (((pixel >> 5) & 0x1f) << 8) | (pixel & 0x1f);
	}
}

static void OsxGuestPackPixels(uint32 *result, const uint32 *left, const uint32 *right)
{
	uint32 r[8];
	uint32 index;
	uint32 word;
	for (index = 0; index < 8; index++) {
		word = left[index & 3];
		if (index >= 4)
			word = right[index & 3];
		r[index] = (((word >> 24) & 1) << 15) | (((word >> 19) & 0x1f) << 10) | (((word >> 11) & 0x1f) << 5) | ((word >> 3) & 0x1f);
	}
	OsxGuestPack(result, 2, r);
}

static void OsxGuestShiftBits(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 index;
	uint32 next;
	if (amount == 0) {
		for (index = 0; index < 4; index++)
			result[index] = source[index];
		return;
	}
	for (index = 0; index < 4; index++) {
		next = 0;
		if (left) {
			if (index < 3)
				next = source[index + 1];
			result[index] = (source[index] << amount) | (next >> (32 - amount));
		} else {
			if (index > 0)
				next = source[index - 1];
			result[index] = (source[index] >> amount) | (next << (32 - amount));
		}
	}
}

static void OsxGuestShiftOctets(uint32 *result, const uint32 *source, uint32 amount, int left)
{
	uint32 padded[8];
	uint32 index;
	uint32 word;
	uint32 shift;
	word = amount >> 2;
	shift = (amount & 3) * 8;
	if (left) {
		for (index = 0; index < 4; index++)
			padded[index] = source[index];
		padded[4] = 0;
		padded[5] = 0;
		padded[6] = 0;
		padded[7] = 0;
		if (shift == 0) {
			for (index = 0; index < 4; index++)
				result[index] = padded[index + word];
		} else {
			for (index = 0; index < 4; index++)
				result[index] = (padded[index + word] << shift) | (padded[index + word + 1] >> (32 - shift));
		}
		return;
	}
	for (index = 0; index < 4; index++)
		padded[index + 4] = source[index];
	padded[0] = 0;
	padded[1] = 0;
	padded[2] = 0;
	padded[3] = 0;
	if (shift == 0) {
		for (index = 0; index < 4; index++)
			result[index] = padded[index - word + 4];
	} else {
		for (index = 0; index < 4; index++)
			result[index] = (padded[index - word + 4] >> shift) | (padded[index - word + 3] << (32 - shift));
	}
}

static void OsxGuestCompareVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 size, int operation, int record)
{
	uint32 a[16];
	uint32 b[16];
	uint32 r[16];
	uint32 index;
	uint32 count;
	uint32 mask;
	uint32 alltrue;
	uint32 allfalse;
	int match;
	count = 16 / size;
	mask = OsxGuestElementMask(size);
	OsxGuestUnpack(left, size, a);
	OsxGuestUnpack(right, size, b);
	alltrue = 1;
	allfalse = 1;
	for (index = 0; index < count; index++) {
		if (operation == VECCMPGTU)
			match = a[index] > b[index];
		else if (operation == VECCMPGTS)
			match = OsxGuestSigned(a[index], size) > OsxGuestSigned(b[index], size);
		else
			match = a[index] == b[index];
		r[index] = 0;
		if (match) {
			r[index] = mask;
			allfalse = 0;
		} else {
			alltrue = 0;
		}
	}
	OsxGuestPack(result, size, r);
	if (record)
		cpu->cr = (cpu->cr & ~0xf0u) | (alltrue << 7) | (allfalse << 5);
}

static void OsxGuestCompareFloat(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, int record)
{
	uint32 index;
	uint32 alltrue;
	uint32 allfalse;
	float first;
	float second;
	int match;
	alltrue = 1;
	allfalse = 1;
	for (index = 0; index < 4; index++) {
		first = OsxGuestGetFloat(left, index);
		second = OsxGuestGetFloat(right, index);
		if (kind == 966) {
			result[index] = 0;
			if (!(first <= second))
				result[index] |= 0x80000000u;
			if (!(first >= -second))
				result[index] |= 0x40000000u;
			if (result[index])
				allfalse = 0;
			continue;
		}
		match = first == second;
		if (kind == 454)
			match = first >= second;
		else if (kind == 710)
			match = first > second;
		result[index] = 0;
		if (match) {
			result[index] = 0xffffffffu;
			allfalse = 0;
		} else {
			alltrue = 0;
		}
	}
	if (!record)
		return;
	if (kind == 966)
		cpu->cr = (cpu->cr & ~0xf0u) | (allfalse << 5);
	else
		cpu->cr = (cpu->cr & ~0xf0u) | (alltrue << 7) | (allfalse << 5);
}

static float OsxGuestRoundFloat(float value, uint32 kind)
{
	double source;
	uint32 bits;
	double rounded;
	source = value;
	if (OsxGuestVectorIsNan(value))
		return value;
	if (kind == 522) {
		rounded = floor(source + 0.5);
		if (rounded - source == 0.5 && fmod(rounded, 2.0) != 0.0)
			rounded -= 1.0;
		memcpy(&bits, &value, sizeof(bits));
		if (rounded == 0.0 && (bits & 0x80000000u))
			rounded = -0.0;
		return (float)rounded;
	}
	if (kind == 586) {
		if (source < 0)
			return (float)ceil(source);
		return (float)floor(source);
	}
	if (kind == 650)
		return (float)ceil(source);
	return (float)floor(source);
}

static uint32 OsxGuestVectorConvertToInteger(OsxCpu *cpu, float value, uint32 scale, int issigned)
{
	double scaled;
	if (OsxGuestVectorIsNan(value))
		return 0;
	scaled = ldexp((double)value, (int)scale);
	if (scaled > 8589934592.0)
		scaled = 8589934592.0;
	if (scaled < -8589934592.0)
		scaled = -8589934592.0;
	if (issigned)
		return OsxGuestSaturateSigned(cpu, (int64)scaled, 4);
	return OsxGuestSaturateUnsigned(cpu, (int64)scaled, 4);
}

static void OsxGuestFloatVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind, uint32 immediate)
{
	uint32 index;
	float first;
	float second;
	switch (kind) {
	case 10:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (OsxGuestVectorIsNan(first))
				result[index] = left[index] | 0x00400000u;
			else if (OsxGuestVectorIsNan(second))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, first + second);
		}
		return;
	case 74:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (OsxGuestVectorIsNan(first))
				result[index] = left[index] | 0x00400000u;
			else if (OsxGuestVectorIsNan(second))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, first - second);
		}
		return;
	case 1034:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (first > second || OsxGuestVectorIsNan(first))
				OsxGuestSetFloat(result, index, first);
			else
				OsxGuestSetFloat(result, index, second);
		}
		return;
	case 1098:
		for (index = 0; index < 4; index++) {
			first = OsxGuestGetFloat(left, index);
			second = OsxGuestGetFloat(right, index);
			if (first < second || OsxGuestVectorIsNan(first))
				OsxGuestSetFloat(result, index, first);
			else
				OsxGuestSetFloat(result, index, second);
		}
		return;
	case 266:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, 1.0f / OsxGuestGetFloat(right, index));
		return;
	case 330:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)(1.0 / sqrt((double)OsxGuestGetFloat(right, index))));
		return;
	case 394:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)pow(2.0, (double)OsxGuestGetFloat(right, index)));
		return;
	case 458:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)(log((double)OsxGuestGetFloat(right, index)) / log(2.0)));
		return;
	case 522: case 586: case 650: case 714:
		for (index = 0; index < 4; index++) {
			if (OsxGuestVectorIsNan(OsxGuestGetFloat(right, index)))
				result[index] = right[index] | 0x00400000u;
			else
				OsxGuestSetFloat(result, index, OsxGuestRoundFloat(OsxGuestGetFloat(right, index), kind));
		}
		return;
	case 778:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)ldexp((double)right[index], -(int)immediate));
		return;
	case 842:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)ldexp((double)(int32)right[index], -(int)immediate));
		return;
	case 906:
		for (index = 0; index < 4; index++)
			result[index] = OsxGuestVectorConvertToInteger(cpu, OsxGuestGetFloat(right, index), immediate, 0);
		return;
	case 970:
		for (index = 0; index < 4; index++)
			result[index] = OsxGuestVectorConvertToInteger(cpu, OsxGuestGetFloat(right, index), immediate, 1);
		return;
	}
}

static void OsxGuestSumVector(OsxCpu *cpu, uint32 *result, const uint32 *left, const uint32 *right, uint32 kind)
{
	uint32 a[16];
	uint32 index;
	uint32 element;
	int64 sum;
	switch (kind) {
	case 1544:
		OsxGuestUnpack(left, 1, a);
		for (index = 0; index < 4; index++) {
			sum = right[index];
			for (element = 0; element < 4; element++)
				sum += a[index * 4 + element];
			result[index] = OsxGuestSaturateUnsigned(cpu, sum, 4);
		}
		return;
	case 1800:
		OsxGuestUnpack(left, 1, a);
		for (index = 0; index < 4; index++) {
			sum = (int32)right[index];
			for (element = 0; element < 4; element++)
				sum += OsxGuestSigned(a[index * 4 + element], 1);
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1608:
		OsxGuestUnpack(left, 2, a);
		for (index = 0; index < 4; index++) {
			sum = (int32)right[index];
			for (element = 0; element < 2; element++)
				sum += OsxGuestSigned(a[index * 2 + element], 2);
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1672:
		for (index = 0; index < 4; index += 2) {
			sum = (int64)(int32)left[index] + (int32)left[index + 1] + (int32)right[index + 1];
			result[index] = 0;
			result[index + 1] = OsxGuestSaturateSigned(cpu, sum, 4);
		}
		return;
	case 1928:
		sum = (int32)right[3];
		for (index = 0; index < 4; index++)
			sum += (int32)left[index];
		result[0] = 0;
		result[1] = 0;
		result[2] = 0;
		result[3] = OsxGuestSaturateSigned(cpu, sum, 4);
		return;
	}
}

static void OsxGuestMultiplySum(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 kind)
{
	uint32 first[16];
	uint32 second[16];
	uint32 index;
	uint32 element;
	int64 sum;
	if (kind == 36 || kind == 37) {
		OsxGuestUnpack(a, 1, first);
		OsxGuestUnpack(b, 1, second);
		for (index = 0; index < 4; index++) {
			sum = (int64)c[index];
			for (element = 0; element < 4; element++) {
				if (kind == 36)
					sum += (int64)first[index * 4 + element] * second[index * 4 + element];
				else
					sum += (int64)OsxGuestSigned(first[index * 4 + element], 1) * second[index * 4 + element];
			}
			result[index] = (uint32)sum;
		}
		return;
	}
	OsxGuestUnpack(a, 2, first);
	OsxGuestUnpack(b, 2, second);
	for (index = 0; index < 4; index++) {
		sum = (int64)c[index];
		if (kind == 40 || kind == 41)
			sum = (int32)c[index];
		for (element = 0; element < 2; element++) {
			if (kind == 38 || kind == 39)
				sum += (int64)first[index * 2 + element] * second[index * 2 + element];
			else
				sum += (int64)OsxGuestSigned(first[index * 2 + element], 2) * OsxGuestSigned(second[index * 2 + element], 2);
		}
		if (kind == 39)
			result[index] = OsxGuestSaturateUnsigned(cpu, sum, 4);
		else if (kind == 41)
			result[index] = OsxGuestSaturateSigned(cpu, sum, 4);
		else
			result[index] = (uint32)sum;
	}
}

static void OsxGuestMultiplyHigh(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 kind)
{
	uint32 first[8];
	uint32 second[8];
	uint32 third[8];
	uint32 r[8];
	uint32 index;
	int64 product;
	OsxGuestUnpack(a, 2, first);
	OsxGuestUnpack(b, 2, second);
	OsxGuestUnpack(c, 2, third);
	if (kind == 34) {
		for (index = 0; index < 8; index++)
			r[index] = (uint32)((uint64)first[index] * second[index] + third[index]) & 0xffff;
	} else if (kind == 33) {
		for (index = 0; index < 8; index++) {
			product = (int64)OsxGuestSigned(first[index], 2) * OsxGuestSigned(second[index], 2) + 0x4000;
			r[index] = OsxGuestSaturateSigned(cpu, (product >> 15) + OsxGuestSigned(third[index], 2), 2);
		}
	} else {
		for (index = 0; index < 8; index++) {
			product = (int64)OsxGuestSigned(first[index], 2) * OsxGuestSigned(second[index], 2);
			r[index] = OsxGuestSaturateSigned(cpu, (product >> 15) + OsxGuestSigned(third[index], 2), 2);
		}
	}
	OsxGuestPack(result, 2, r);
}

static void OsxGuestPermute(uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c)
{
	uint8 source[32];
	uint8 select[16];
	uint32 index;
	OsxGuestUnpackBytes(a, source);
	OsxGuestUnpackBytes(b, source + 16);
	OsxGuestUnpackBytes(c, select);
	for (index = 0; index < 4; index++) {
		result[index] = ((uint32)source[select[index * 4] & 31] << 24)
			| ((uint32)source[select[index * 4 + 1] & 31] << 16)
			| ((uint32)source[select[index * 4 + 2] & 31] << 8)
			| source[select[index * 4 + 3] & 31];
	}
}

static void OsxGuestShiftDouble(uint32 *result, const uint32 *a, const uint32 *b, uint32 shift)
{
	uint8 source[32];
	OsxGuestUnpackBytes(a, source);
	OsxGuestUnpackBytes(b, source + 16);
	OsxGuestPackBytes(result, source + shift);
}

static void OsxGuestExecuteVectorA(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, const uint32 *c, uint32 instruction)
{
	uint32 kind;
	uint32 index;
	kind = instruction & 0x3f;
	switch (kind) {
	case 32: case 33: case 34:
		OsxGuestMultiplyHigh(cpu, result, a, b, c, kind);
		return;
	case 36: case 37: case 38: case 39: case 40: case 41:
		OsxGuestMultiplySum(cpu, result, a, b, c, kind);
		return;
	case 42:
		for (index = 0; index < 4; index++)
			result[index] = (a[index] & ~c[index]) | (b[index] & c[index]);
		return;
	case 43:
		OsxGuestPermute(result, a, b, c);
		return;
	case 44:
		OsxGuestShiftDouble(result, a, b, (instruction >> 6) & 15);
		return;
	case 46:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)((double)OsxGuestGetFloat(a, index) * OsxGuestGetFloat(c, index) + OsxGuestGetFloat(b, index)));
		return;
	case 47:
		for (index = 0; index < 4; index++)
			OsxGuestSetFloat(result, index, (float)-((double)OsxGuestGetFloat(a, index) * OsxGuestGetFloat(c, index) - OsxGuestGetFloat(b, index)));
		return;
	}
	OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
}

static void OsxGuestFill(uint32 *result, uint32 value, uint32 size)
{
	if (size == 1)
		value |= value << 8;
	if (size <= 2)
		value |= value << 16;
	result[0] = value;
	result[1] = value;
	result[2] = value;
	result[3] = value;
}

static void OsxGuestSplat(uint32 *result, const uint32 *source, uint32 size, uint32 field)
{
	uint32 value;
	if (size == 4)
		value = source[field & 3];
	else if (size == 2)
		value = OsxGuestGetHalf(source, field & 7);
	else
		value = OsxGuestGetByte(source, field & 15);
	OsxGuestFill(result, value, size);
}

static int OsxGuestExecuteVectorX(OsxCpu *cpu, uint32 *result, const uint32 *a, const uint32 *b, uint32 instruction)
{
	uint32 kind;
	uint32 size;
	uint32 field;
	uint32 index;
	kind = instruction & 0x7ff;
	size = 1u << ((kind >> 6) & 3);
	field = (instruction >> 16) & 31;
	switch (kind) {
	case 0: case 64: case 128: OsxGuestIntegerVector(cpu, result, a, b, size, VECADDM); return 1;
	case 1024: case 1088: case 1152: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1024) >> 6) & 3), VECSUBM); return 1;
	case 512: case 576: case 640: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 512) >> 6) & 3), VECADDUS); return 1;
	case 768: case 832: case 896: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 768) >> 6) & 3), VECADDSS); return 1;
	case 1536: case 1600: case 1664: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1536) >> 6) & 3), VECSUBUS); return 1;
	case 1792: case 1856: case 1920: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1792) >> 6) & 3), VECSUBSS); return 1;
	case 2: case 66: case 130: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 2) >> 6) & 3), VECMAXU); return 1;
	case 258: case 322: case 386: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 258) >> 6) & 3), VECMAXS); return 1;
	case 514: case 578: case 642: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 514) >> 6) & 3), VECMINU); return 1;
	case 770: case 834: case 898: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 770) >> 6) & 3), VECMINS); return 1;
	case 1026: case 1090: case 1154: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1026) >> 6) & 3), VECAVGU); return 1;
	case 1282: case 1346: case 1410: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 1282) >> 6) & 3), VECAVGS); return 1;
	case 4: case 68: case 132: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 4) >> 6) & 3), VECRL); return 1;
	case 260: case 324: case 388: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 260) >> 6) & 3), VECSL); return 1;
	case 516: case 580: case 644: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 516) >> 6) & 3), VECSR); return 1;
	case 772: case 836: case 900: OsxGuestIntegerVector(cpu, result, a, b, 1u << (((kind - 772) >> 6) & 3), VECSRA); return 1;
	case 384:
		for (index = 0; index < 4; index++)
			result[index] = (uint32)(((uint64)a[index] + b[index]) >> 32);
		return 1;
	case 1408:
		for (index = 0; index < 4; index++)
			result[index] = a[index] >= b[index];
		return 1;
	case 8: OsxGuestMultiplyVector(result, a, b, 1, 1, 0); return 1;
	case 72: OsxGuestMultiplyVector(result, a, b, 2, 1, 0); return 1;
	case 264: OsxGuestMultiplyVector(result, a, b, 1, 1, 1); return 1;
	case 328: OsxGuestMultiplyVector(result, a, b, 2, 1, 1); return 1;
	case 520: OsxGuestMultiplyVector(result, a, b, 1, 0, 0); return 1;
	case 584: OsxGuestMultiplyVector(result, a, b, 2, 0, 0); return 1;
	case 776: OsxGuestMultiplyVector(result, a, b, 1, 0, 1); return 1;
	case 840: OsxGuestMultiplyVector(result, a, b, 2, 0, 1); return 1;
	case 12: case 76: case 140: OsxGuestMergeVector(result, a, b, size, 0); return 1;
	case 268: case 332: case 396: OsxGuestMergeVector(result, a, b, 1u << (((kind - 268) >> 6) & 3), 1); return 1;
	case 524: case 588: case 652:
		OsxGuestSplat(result, b, 1u << (((kind - 524) >> 6) & 3), field);
		return 1;
	case 780: case 844: case 908:
		size = 1u << (((kind - 780) >> 6) & 3);
		OsxGuestFill(result, (uint32)((int32)(field << 27) >> 27) & OsxGuestElementMask(size), size);
		return 1;
	case 14: OsxGuestPackVector(cpu, result, a, b, 2, 0, 0, 0); return 1;
	case 78: OsxGuestPackVector(cpu, result, a, b, 4, 0, 0, 0); return 1;
	case 142: OsxGuestPackVector(cpu, result, a, b, 2, 0, 1, 0); return 1;
	case 206: OsxGuestPackVector(cpu, result, a, b, 4, 0, 1, 0); return 1;
	case 270: OsxGuestPackVector(cpu, result, a, b, 2, 1, 1, 0); return 1;
	case 334: OsxGuestPackVector(cpu, result, a, b, 4, 1, 1, 0); return 1;
	case 398: OsxGuestPackVector(cpu, result, a, b, 2, 1, 1, 1); return 1;
	case 462: OsxGuestPackVector(cpu, result, a, b, 4, 1, 1, 1); return 1;
	case 782: OsxGuestPackPixels(result, a, b); return 1;
	case 526: OsxGuestUnpackVector(result, b, 1, 0); return 1;
	case 590: OsxGuestUnpackVector(result, b, 2, 0); return 1;
	case 654: OsxGuestUnpackVector(result, b, 1, 1); return 1;
	case 718: OsxGuestUnpackVector(result, b, 2, 1); return 1;
	case 846: OsxGuestUnpackPixels(result, b, 0); return 1;
	case 974: OsxGuestUnpackPixels(result, b, 1); return 1;
	case 1028:
		for (index = 0; index < 4; index++)
			result[index] = a[index] & b[index];
		return 1;
	case 1092:
		for (index = 0; index < 4; index++)
			result[index] = a[index] & ~b[index];
		return 1;
	case 1156:
		for (index = 0; index < 4; index++)
			result[index] = a[index] | b[index];
		return 1;
	case 1220:
		for (index = 0; index < 4; index++)
			result[index] = a[index] ^ b[index];
		return 1;
	case 1284:
		for (index = 0; index < 4; index++)
			result[index] = ~(a[index] | b[index]);
		return 1;
	case 452: OsxGuestShiftBits(result, a, b[3] & 7, 1); return 1;
	case 708: OsxGuestShiftBits(result, a, b[3] & 7, 0); return 1;
	case 1036: OsxGuestShiftOctets(result, a, (b[3] >> 3) & 15, 1); return 1;
	case 1100: OsxGuestShiftOctets(result, a, (b[3] >> 3) & 15, 0); return 1;
	case 1544: case 1800: case 1608: case 1672: case 1928:
		OsxGuestSumVector(cpu, result, a, b, kind);
		return 1;
	case 10: case 74: case 1034: case 1098: case 266: case 330: case 394: case 458:
	case 522: case 586: case 650: case 714: case 778: case 842: case 906: case 970:
		OsxGuestFloatVector(cpu, result, a, b, kind, field);
		return 1;
	case 1540:
		result[0] = 0;
		result[1] = 0;
		result[2] = 0;
		result[3] = cpu->vscr;
		return 1;
	case 1604:
		cpu->vscr = b[3] & 0x00010001u;
		return 0;
	}
	OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
	return 0;
}

static int OsxGuestVectorEnabled(OsxCpu *cpu)
{
	if (!cpu->altivec) {
		OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
		return 0;
	}
	if (!(cpu->msr & OSXMSRVEC)) {
		OsxGuestCpuRaiseException(cpu, OSXVECTORALTIVEC, 0, cpu->pc);
		return 0;
	}
	return 1;
}

static OSXJITABI void OsxGuestVectorGeneric(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	uint32 kind;
	int vd;
	int va;
	int vb;
#if OSXPROFILE
	cpu->tmpform[3][OsxGuestCpuTmpFormKey(instruction)]++;
#endif
	vd = (instruction >> 21) & 31;
	va = (instruction >> 16) & 31;
	vb = (instruction >> 11) & 31;
	kind = instruction & 0x3f;
	if (kind >= 32) {
		OsxGuestExecuteVectorA(cpu, result, cpu->vr[va], cpu->vr[vb], cpu->vr[(instruction >> 6) & 31], instruction);
		if (kind == 35 || kind == 45 || kind > 47)
			return;
	} else if (kind == 6) {
		kind = instruction & 0x3ff;
		if (kind == 198 || kind == 454 || kind == 710 || kind == 966)
			OsxGuestCompareFloat(cpu, result, cpu->vr[va], cpu->vr[vb], kind, (instruction >> 10) & 1);
		else if (kind == 6 || kind == 70 || kind == 134)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << (kind >> 6), VECCMPEQ, (instruction >> 10) & 1);
		else if (kind == 518 || kind == 582 || kind == 646)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 518) >> 6), VECCMPGTU, (instruction >> 10) & 1);
		else if (kind == 774 || kind == 838 || kind == 902)
			OsxGuestCompareVector(cpu, result, cpu->vr[va], cpu->vr[vb], 1u << ((kind - 774) >> 6), VECCMPGTS, (instruction >> 10) & 1);
		else {
			OsxGuestCpuRaiseException(cpu, OSXVECTORPROGRAM, 0x00080000, cpu->pc);
			return;
		}
	} else if (!OsxGuestExecuteVectorX(cpu, result, cpu->vr[va], cpu->vr[vb], instruction)) {
		return;
	}
	cpu->vr[vd][0] = result[0];
	cpu->vr[vd][1] = result[1];
	cpu->vr[vd][2] = result[2];
	cpu->vr[vd][3] = result[3];
}

void OsxGuestCpuExecuteVector(OsxCpu *cpu, uint32 instruction)
{
	if (OsxGuestVectorEnabled(cpu))
		OsxGuestVectorGeneric(cpu, instruction);
}

static inline uint32 *OsxGuestVectorD(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 21) & 31];
}

static inline const uint32 *OsxGuestVectorA(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 16) & 31];
}

static inline const uint32 *OsxGuestVectorB(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 11) & 31];
}

static inline const uint32 *OsxGuestVectorC(OsxCpu *cpu, uint32 instruction)
{
	return cpu->vr[(instruction >> 6) & 31];
}

static inline void OsxGuestStore(uint32 *target, const uint32 *result)
{
	target[0] = result[0];
	target[1] = result[1];
	target[2] = result[2];
	target[3] = result[3];
}

static OSXJITABI void OsxGuestVectorPermute(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestPermute(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), OsxGuestVectorC(cpu, instruction));
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorShiftDouble(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestShiftDouble(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), (instruction >> 6) & 15);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorMultiplyHigh(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestMultiplyHigh(cpu, result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), OsxGuestVectorC(cpu, instruction), instruction & 0x3f);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorSelect(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	const uint32 *c;
	uint32 result[4];
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	c = OsxGuestVectorC(cpu, instruction);
	for (index = 0; index < 4; index++)
		result[index] = (a[index] & ~c[index]) | (b[index] & c[index]);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorLogical(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	switch (instruction & 0x7ff) {
	case 1028:
		for (index = 0; index < 4; index++)
			d[index] = a[index] & b[index];
		return;
	case 1092:
		for (index = 0; index < 4; index++)
			d[index] = a[index] & ~b[index];
		return;
	case 1156:
		for (index = 0; index < 4; index++)
			d[index] = a[index] | b[index];
		return;
	case 1220:
		for (index = 0; index < 4; index++)
			d[index] = a[index] ^ b[index];
		return;
	}
	for (index = 0; index < 4; index++)
		d[index] = ~(a[index] | b[index]);
}

static OSXJITABI void OsxGuestVectorModular(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	uint32 high;
	uint32 low;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	high = 0x80808080u;
	if (instruction & 0x40)
		high = 0x80008000u;
	low = ~high;
	if (instruction & 0x400) {
		for (index = 0; index < 4; index++)
			d[index] = ((a[index] | high) - (b[index] & low)) ^ ((a[index] ^ ~b[index]) & high);
	} else {
		for (index = 0; index < 4; index++)
			d[index] = ((a[index] & low) + (b[index] & low)) ^ ((a[index] ^ b[index]) & high);
	}
}

static OSXJITABI void OsxGuestVectorModularWord(OsxCpu *cpu, uint32 instruction)
{
	const uint32 *a;
	const uint32 *b;
	uint32 *d;
	uint32 index;
	a = OsxGuestVectorA(cpu, instruction);
	b = OsxGuestVectorB(cpu, instruction);
	d = OsxGuestVectorD(cpu, instruction);
	if (instruction & 0x400) {
		for (index = 0; index < 4; index++)
			d[index] = a[index] - b[index];
	} else {
		for (index = 0; index < 4; index++)
			d[index] = a[index] + b[index];
	}
}

static OSXJITABI void OsxGuestVectorMerge(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	uint32 kind;
	kind = instruction & 0x7ff;
	OsxGuestMergeVector(result, OsxGuestVectorA(cpu, instruction), OsxGuestVectorB(cpu, instruction), 1u << ((kind >> 6) & 3), (kind >> 8) & 1);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static OSXJITABI void OsxGuestVectorSplat(OsxCpu *cpu, uint32 instruction)
{
	uint32 result[4];
	OsxGuestSplat(result, OsxGuestVectorB(cpu, instruction), 1u << ((((instruction & 0x7ff) - 524) >> 6) & 3), (instruction >> 16) & 31);
	OsxGuestStore(OsxGuestVectorD(cpu, instruction), result);
}

static int OsxGuestVectorValid(uint32 instruction)
{
	static const uint16 kinds[] = {
		0, 64, 128, 1024, 1088, 1152, 512, 576, 640, 768, 832, 896, 1536, 1600, 1664, 1792, 1856, 1920,
		2, 66, 130, 258, 322, 386, 514, 578, 642, 770, 834, 898, 1026, 1090, 1154, 1282, 1346, 1410,
		4, 68, 132, 260, 324, 388, 516, 580, 644, 772, 836, 900, 384, 1408, 8, 72, 264, 328, 520, 584, 776, 840,
		12, 76, 140, 268, 332, 396, 524, 588, 652, 780, 844, 908, 14, 78, 142, 206, 270, 334, 398, 462, 782,
		526, 590, 654, 718, 846, 974, 1028, 1092, 1156, 1220, 1284, 452, 708, 1036, 1100, 1544, 1800, 1608, 1672, 1928,
		10, 74, 1034, 1098, 266, 330, 394, 458, 522, 586, 650, 714, 778, 842, 906, 970, 1540, 1604
	};
	static const uint16 compares[] = { 198, 454, 710, 966, 6, 70, 134, 518, 582, 646, 774, 838, 902 };
	uint32 kind;
	uint32 index;
	kind = instruction & 0x3f;
	if (kind >= 32)
		return kind != 35 && kind != 45 && kind <= 47;
	if (kind == 6) {
		for (index = 0; index < sizeof(compares) / sizeof(compares[0]); index++)
			if ((instruction & 0x3ff) == compares[index])
				return 1;
		return 0;
	}
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
		if ((instruction & 0x7ff) == kinds[index])
			return 1;
	return 0;
}

OsxVectorFunction OsxGuestCpuVectorHandler(uint32 instruction)
{
	if ((instruction >> 26) != 4 || !OsxGuestVectorValid(instruction))
		return NULL;
	switch (instruction & 0x3f) {
	case 33:
		return OsxGuestVectorMultiplyHigh;
	case 42:
		return OsxGuestVectorSelect;
	case 43:
		return OsxGuestVectorPermute;
	case 44:
		return OsxGuestVectorShiftDouble;
	}
	switch (instruction & 0x7ff) {
	case 0: case 64: case 1024: case 1088:
		return OsxGuestVectorModular;
	case 128: case 1152:
		return OsxGuestVectorModularWord;
	case 1028: case 1092: case 1156: case 1220: case 1284:
		return OsxGuestVectorLogical;
	case 12: case 76: case 140: case 268: case 332: case 396:
		return OsxGuestVectorMerge;
	case 524: case 588: case 652:
		return OsxGuestVectorSplat;
	}
	return OsxGuestVectorGeneric;
}

int OsxGuestCpuExecuteVectorMemory(OsxCpu *cpu, uint32 instruction, uint32 extended, uint32 address)
{
	uint32 words[4];
	uint32 value;
	uint32 element;
	uint32 index;
	int vd;
	vd = (instruction >> 21) & 31;
	switch (extended) {
	case 342: case 374: case 822:
		return cpu->altivec;
	case 6: case 38: case 7: case 39: case 71: case 103: case 359: case 135: case 167: case 199: case 231: case 487:
		break;
	default:
		return 0;
	}
	if (!OsxGuestVectorEnabled(cpu))
		return 1;
	switch (extended) {
	case 103: case 359:
		if (OsxGuestCpuReadQuad(cpu, address & ~15u, words)) {
			cpu->vr[vd][0] = words[0];
			cpu->vr[vd][1] = words[1];
			cpu->vr[vd][2] = words[2];
			cpu->vr[vd][3] = words[3];
		}
		return 1;
	case 231: case 487:
		OsxGuestCpuWriteQuad(cpu, address & ~15u, cpu->vr[vd]);
		return 1;
	case 6: case 38:
		element = address & 15;
		if (extended == 38)
			element = 16 - element;
		for (index = 0; index < 4; index++)
			cpu->vr[vd][index] = (((element + index * 4) & 0xff) << 24) | (((element + index * 4 + 1) & 0xff) << 16) | (((element + index * 4 + 2) & 0xff) << 8) | ((element + index * 4 + 3) & 0xff);
		return 1;
	case 7:
		if (OsxGuestCpuReadData(cpu, address, 1, &value))
			OsxGuestSetByte(cpu->vr[vd], address & 15, value);
		return 1;
	case 39:
		address &= ~1u;
		if (OsxGuestCpuReadData(cpu, address, 2, &value))
			OsxGuestSetHalf(cpu->vr[vd], (address & 15) >> 1, value);
		return 1;
	case 71:
		address &= ~3u;
		if (OsxGuestCpuReadData(cpu, address, 4, &value))
			cpu->vr[vd][(address & 15) >> 2] = value;
		return 1;
	case 135:
		OsxGuestCpuWriteData(cpu, address, 1, OsxGuestGetByte(cpu->vr[vd], address & 15));
		return 1;
	case 167:
		address &= ~1u;
		OsxGuestCpuWriteData(cpu, address, 2, OsxGuestGetHalf(cpu->vr[vd], (address & 15) >> 1));
		return 1;
	case 199:
		address &= ~3u;
		OsxGuestCpuWriteData(cpu, address, 4, cpu->vr[vd][(address & 15) >> 2]);
		return 1;
	}
	return 1;
}
