/*
 *  osxcpu.cpp - OSX CPU emulation
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
#else
#include <time.h>
#endif
#include "osxcpu.h"

#define OSXPAGEMASK 0xfffff000
#define OSXACCESSFAULT 0
#define OSXACCESSHOST 1
#define OSXACCESSBUS 2

static inline uint32 LoadHost32(const uint8 *host)
{
	uint32 value;
	memcpy(&value, host, 4);
	return tswap32(value);
}

static inline uint32 LoadHost16(const uint8 *host)
{
	uint16 value;
	memcpy(&value, host, 2);
	return tswap16(value);
}

static inline void StoreHost32(uint8 *host, uint32 value)
{
	value = tswap32(value);
	memcpy(host, &value, 4);
}

static inline void StoreHost16(uint8 *host, uint32 value)
{
	uint16 half;
	half = tswap16((uint16)value);
	memcpy(host, &half, 2);
}

static inline uint32 LoadHostSized(const uint8 *host, int size)
{
	if (size == 4)
		return LoadHost32(host);
	if (size == 2)
		return LoadHost16(host);
	return host[0];
}

static inline void StoreHostSized(uint8 *host, int size, uint32 value)
{
	if (size == 4)
		StoreHost32(host, value);
	else if (size == 2)
		StoreHost16(host, value);
	else
		host[0] = (uint8)value;
}

static uint64 HostTicks(void)
{
#ifdef _WIN32
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64)counter.QuadPart;
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec * 1000000000ULL + (uint64)now.tv_nsec;
#endif
}

static uint64 HostFrequency(void)
{
#ifdef _WIN32
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	return (uint64)frequency.QuadPart;
#else
	return 1000000000ULL;
#endif
}

uint64 OsxHostNanoseconds(void)
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

uint64 OsxCpuTimebase(OsxCpu *cpu)
{
	uint64 elapsed;
	if (cpu->deterministic)
		return cpu->instructions / cpu->deterministic + cpu->virtualticks;
	elapsed = HostTicks() - cpu->hostbase;
	return (elapsed / cpu->hostfrequency) * cpu->tbfrequency + ((elapsed % cpu->hostfrequency) * cpu->tbfrequency) / cpu->hostfrequency + cpu->tboffset;
}

uint64 OsxCpuNextDeadline(OsxCpu *cpu)
{
	return cpu->decdeadline;
}

void OsxCpuFlushTlb(OsxCpu *cpu)
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

static void InvalidateTlbPage(OsxCpu *cpu, uint32 address)
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

void OsxCpuUpdateModes(OsxCpu *cpu)
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

void OsxCpuReset(OsxCpu *cpu, uint32 pvr, uint64 tbfrequency)
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
	cpu->pvr = pvr;
	cpu->l2cr = 0;
	cpu->ictc = 0;
	cpu->iabr = 0;
	cpu->dabr = 0;
	cpu->ear = 0;
	cpu->sia = 0;
	cpu->reservation = 0;
	cpu->reservationvalid = 0;
	cpu->tbfrequency = tbfrequency;
	cpu->hostbase = HostTicks();
	cpu->hostfrequency = HostFrequency();
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
	OsxCpuFlushTlb(cpu);
	OsxCpuUpdateModes(cpu);
}

void OsxCpuRaiseException(OsxCpu *cpu, uint32 vector, uint32 srr1bits, uint32 srr0)
{
	uint32 newmsr;
	cpu->exceptions++;
	cpu->srr0 = srr0;
	cpu->srr1 = (cpu->msr & 0x0000ff73) | srr1bits;
	newmsr = cpu->msr & (OSXMSRILE | OSXMSRME | OSXMSRIP);
	if (newmsr & OSXMSRILE)
		newmsr |= OSXMSRLE;
	cpu->msr = newmsr;
	if (newmsr & OSXMSRIP)
		cpu->nextpc = 0xfff00000 | vector;
	else
		cpu->nextpc = vector;
	cpu->halted = 0;
	OsxCpuUpdateModes(cpu);
}

static inline int PhysicalHost(OsxCpu *cpu, uint32 physical, uint8 **host)
{
	if (physical < cpu->ramsize) {
		*host = cpu->ram + physical;
		return 1;
	}
	if (physical - cpu->vrambase < cpu->vramsize) {
		*host = cpu->vram + (physical - cpu->vrambase);
		return 1;
	}
	return 0;
}

static uint32 PtegAddress(uint32 sdr1, uint32 hash)
{
	uint32 htaborg;
	uint32 htabmask;
	htaborg = sdr1 & 0xffff0000;
	htabmask = sdr1 & 0x1ff;
	return (htaborg & 0xfe000000) | ((((htaborg >> 16) & 0x1ff) | ((hash >> 10) & htabmask)) << 16) | ((hash & 0x3ff) << 6);
}

static void RecordFault(OsxCpu *cpu, uint32 address, uint32 reason, int instruction)
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

void OsxCpuDumpFaults(OsxCpu *cpu, FILE *output)
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

static void TranslationFault(OsxCpu *cpu, uint32 address, int kind, uint32 reason)
{
	if (kind == OSXTLBFETCH) {
		RecordFault(cpu, cpu->pc, reason, 1);
		OsxCpuRaiseException(cpu, OSXVECTORISI, reason, cpu->pc);
		return;
	}
	cpu->dar = address;
	if (kind == OSXTLBWRITE)
		cpu->dsisr = reason | OSXDSISRSTORE;
	else
		cpu->dsisr = reason;
	RecordFault(cpu, address, cpu->dsisr, 0);
	OsxCpuRaiseException(cpu, OSXVECTORDSI, 0, cpu->pc);
}

static int CheckProtection(int key, uint32 pp, int kind)
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

int OsxCpuTranslate(OsxCpu *cpu, uint32 address, int kind, int raise, uint32 *physical)
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
				TranslationFault(cpu, address, kind, OSXDSISRPROTECTION);
			return 0;
		}
		*physical = (lower & 0xfffe0000 & ~blockmask) | (address & (blockmask | 0x1ffff));
		return 1;
	}
	segment = cpu->sr[address >> 28];
	if (segment & 0x80000000) {
		if (raise) {
			if (kind == OSXTLBFETCH)
				TranslationFault(cpu, address, kind, OSXISIGUARDED);
			else
				TranslationFault(cpu, address, kind, OSXDSISRDIRECTSTORE);
		}
		return 0;
	}
	if (kind == OSXTLBFETCH && (segment & 0x10000000)) {
		if (raise)
			TranslationFault(cpu, address, kind, OSXISIGUARDED);
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
		pteg = PtegAddress(cpu->sdr1, hash);
		want = 0x80000000 | (vsid << 7) | ((uint32)pass << 6) | (pageindex >> 10);
		for (slot = 0; slot < 8; slot++) {
			if (!PhysicalHost(cpu, pteg + slot * 8, &host))
				break;
			word0 = LoadHost32(host);
			if (word0 != want)
				continue;
			word1 = LoadHost32(host + 4);
			if (!CheckProtection(key, word1 & 3, kind) || (kind == OSXTLBFETCH && (word1 & 0x08))) {
				if (raise) {
					if (kind == OSXTLBFETCH && (word1 & 0x08))
						TranslationFault(cpu, address, kind, OSXISIGUARDED);
					else
						TranslationFault(cpu, address, kind, OSXDSISRPROTECTION);
				}
				return 0;
			}
			updated = word1 | 0x100;
			if (kind == OSXTLBWRITE)
				updated |= 0x80;
			if (updated != word1)
				StoreHost32(host + 4, updated);
			*physical = (word1 & OSXPAGEMASK) | (address & 0xfff);
			return 1;
		}
		hash = ~hash;
	}
	if (raise)
		TranslationFault(cpu, address, kind, OSXDSISRNOTFOUND);
	return 0;
}

static int AccessSlow(OsxCpu *cpu, uint32 address, int kind, uint8 **host, uint32 *physical)
{
	OsxTlbEntry *entry;
	uint32 *segments;
	cpu->slowaccesses++;
	if (!OsxCpuTranslate(cpu, address, kind, 1, physical))
		return OSXACCESSFAULT;
	if (!PhysicalHost(cpu, *physical, host))
		return OSXACCESSBUS;
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
	entry->host = *host - (address & 0xfff);
	return OSXACCESSHOST;
}

static int ReadSlow(OsxCpu *cpu, uint32 address, int size, uint32 *value)
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
			if (!ReadSlow(cpu, address + index, 1, &byte))
				return 0;
			result = (result << 8) | byte;
		}
		*value = result;
		return 1;
	}
	status = AccessSlow(cpu, address, OSXTLBREAD, &host, &physical);
	if (status == OSXACCESSFAULT)
		return 0;
	if (status == OSXACCESSHOST)
		*value = LoadHostSized(host, size);
	else
		*value = cpu->busread(cpu->buscontext, physical, size);
	return 1;
}

static int WriteSlow(OsxCpu *cpu, uint32 address, int size, uint32 value)
{
	uint8 *host;
	uint32 physical;
	int status;
	int index;
	if ((address & 0xfff) + (uint32)size > 0x1000) {
		for (index = 0; index < size; index++)
			if (!WriteSlow(cpu, address + index, 1, (value >> ((size - 1 - index) * 8)) & 0xff))
				return 0;
		return 1;
	}
	status = AccessSlow(cpu, address, OSXTLBWRITE, &host, &physical);
	if (status == OSXACCESSFAULT)
		return 0;
	if (status == OSXACCESSHOST)
		StoreHostSized(host, size, value);
	else
		cpu->buswrite(cpu->buscontext, physical, size, value);
	return 1;
}

static inline uint8 *ReadHost(OsxCpu *cpu, uint32 address, uint32 limit)
{
	OsxTlbEntry *entry;
	entry = &cpu->readtlb[(address >> 12) & OSXTLBMASK];
	if (entry->page == (address & OSXPAGEMASK) && entry->segment == cpu->datasr[address >> 28] && (address & 0xfff) <= limit)
		return entry->host + (address & 0xfff);
	return NULL;
}

static inline uint8 *WriteHost(OsxCpu *cpu, uint32 address, uint32 limit)
{
	OsxTlbEntry *entry;
	entry = &cpu->writetlb[(address >> 12) & OSXTLBMASK];
	if (entry->page == (address & OSXPAGEMASK) && entry->segment == cpu->datasr[address >> 28] && (address & 0xfff) <= limit)
		return entry->host + (address & 0xfff);
	return NULL;
}

static inline int Read32(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = ReadHost(cpu, address, 0xffc);
	if (host) {
		*value = LoadHost32(host);
		return 1;
	}
	return ReadSlow(cpu, address, 4, value);
}

static inline int Read16(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = ReadHost(cpu, address, 0xffe);
	if (host) {
		*value = LoadHost16(host);
		return 1;
	}
	return ReadSlow(cpu, address, 2, value);
}

static inline int Read8(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint8 *host;
	host = ReadHost(cpu, address, 0xfff);
	if (host) {
		*value = host[0];
		return 1;
	}
	return ReadSlow(cpu, address, 1, value);
}

static inline int Write32(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = WriteHost(cpu, address, 0xffc);
	if (host) {
		StoreHost32(host, value);
		return 1;
	}
	return WriteSlow(cpu, address, 4, value);
}

static inline int Write16(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = WriteHost(cpu, address, 0xffe);
	if (host) {
		StoreHost16(host, value);
		return 1;
	}
	return WriteSlow(cpu, address, 2, value);
}

static inline int Write8(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint8 *host;
	host = WriteHost(cpu, address, 0xfff);
	if (host) {
		host[0] = (uint8)value;
		return 1;
	}
	return WriteSlow(cpu, address, 1, value);
}

static int Read64(OsxCpu *cpu, uint32 address, uint64 *value)
{
	uint8 *host;
	uint32 high;
	uint32 low;
	host = ReadHost(cpu, address, 0xff8);
	if (host) {
		*value = ((uint64)LoadHost32(host) << 32) | LoadHost32(host + 4);
		return 1;
	}
	if (!ReadSlow(cpu, address, 4, &high))
		return 0;
	if (!ReadSlow(cpu, address + 4, 4, &low))
		return 0;
	*value = ((uint64)high << 32) | low;
	return 1;
}

static int Write64(OsxCpu *cpu, uint32 address, uint64 value)
{
	uint8 *host;
	uint32 physical;
	host = WriteHost(cpu, address, 0xff8);
	if (host) {
		StoreHost32(host, (uint32)(value >> 32));
		StoreHost32(host + 4, (uint32)value);
		return 1;
	}
	if (((address + 4) & OSXPAGEMASK) != (address & OSXPAGEMASK) && !OsxCpuTranslate(cpu, address + 4, OSXTLBWRITE, 1, &physical))
		return 0;
	if (!WriteSlow(cpu, address, 4, (uint32)(value >> 32)))
		return 0;
	return WriteSlow(cpu, address + 4, 4, (uint32)value);
}

int OsxCpuReadVirtual(OsxCpu *cpu, uint32 address, uint32 *value)
{
	uint32 physical;
	uint8 *host;
	if (!OsxCpuTranslate(cpu, address, OSXTLBREAD, 0, &physical))
		return 0;
	if (!PhysicalHost(cpu, physical, &host))
		return 0;
	*value = LoadHost32(host);
	return 1;
}

int OsxCpuWriteVirtual(OsxCpu *cpu, uint32 address, uint32 value)
{
	uint32 physical;
	uint8 *host;
	if (!OsxCpuTranslate(cpu, address, OSXTLBWRITE, 0, &physical))
		return 0;
	if (!PhysicalHost(cpu, physical, &host))
		return 0;
	StoreHost32(host, value);
	return 1;
}

int OsxCpuFetchPage(OsxCpu *cpu)
{
	OsxTlbEntry *entry;
	uint8 *host;
	uint32 physical;
	int status;
	entry = &cpu->fetchtlb[(cpu->pc >> 12) & OSXTLBMASK];
	if (entry->page == (cpu->pc & OSXPAGEMASK) && entry->segment == cpu->fetchsr[cpu->pc >> 28]) {
		cpu->fetchhost = entry->host;
	} else {
		status = AccessSlow(cpu, cpu->pc, OSXTLBFETCH, &host, &physical);
		if (status == OSXACCESSFAULT)
			return 0;
		if (status == OSXACCESSBUS) {
			OsxCpuRaiseException(cpu, OSXVECTORMACHINECHECK, 0, cpu->pc);
			return 0;
		}
		cpu->fetchhost = host - (cpu->pc & 0xfff);
	}
	cpu->fetchtag = (cpu->pc & OSXPAGEMASK) | 1;
	return 1;
}

int OsxCpuAddHook(OsxCpu *cpu, uint32 address, OsxHookFunction function, void *context)
{
	OsxHook *hook;
	if (cpu->hookcount >= OSXMAXHOOKS || address + 4 > cpu->ramsize)
		return 0;
	hook = &cpu->hooks[cpu->hookcount];
	hook->address = address;
	hook->original = LoadHost32(cpu->ram + address);
	hook->function = function;
	hook->context = context;
	StoreHost32(cpu->ram + address, OSXHOOKOPCODE | (uint32)cpu->hookcount);
	OsxJitInvalidate(cpu, address, 4);
	cpu->hookcount++;
	return 1;
}

static inline void SetCr0(OsxCpu *cpu, uint32 result)
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

static inline void SetCrField(OsxCpu *cpu, int field, uint32 value)
{
	int shift;
	shift = (7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | (value << shift);
}

static inline uint32 CrBit(OsxCpu *cpu, int bit)
{
	return (cpu->cr >> (31 - bit)) & 1;
}

static inline void SetCrBit(OsxCpu *cpu, int bit, uint32 value)
{
	uint32 mask;
	mask = 0x80000000u >> bit;
	if (value)
		cpu->cr |= mask;
	else
		cpu->cr &= ~mask;
}

static inline void SetCarry(OsxCpu *cpu, int carry)
{
	if (carry)
		cpu->xer |= OSXXERCA;
	else
		cpu->xer &= ~OSXXERCA;
}

static inline void SetOverflow(OsxCpu *cpu, int overflow)
{
	if (overflow)
		cpu->xer |= OSXXEROV | OSXXERSO;
	else
		cpu->xer &= ~OSXXEROV;
}

static inline uint32 RotateLeft(uint32 value, uint32 count)
{
	count &= 31;
	if (count == 0)
		return value;
	return (value << count) | (value >> (32 - count));
}

static inline uint32 RotateMask(uint32 begin, uint32 end)
{
	uint32 first;
	uint32 last;
	first = 0xffffffffu >> begin;
	last = 0xffffffffu << (31 - end);
	if (begin <= end)
		return first & last;
	return first | last;
}

static uint32 CountLeadingZeros(uint32 value)
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

static void ProgramException(OsxCpu *cpu, uint32 reason)
{
	OsxCpuRaiseException(cpu, OSXVECTORPROGRAM, reason, cpu->pc);
}

static void IllegalInstruction(OsxCpu *cpu, uint32 instruction)
{
	if (cpu->logfile && !(cpu->msr & OSXMSRPR))
		fprintf(cpu->logfile, "illegal instruction %08x at %08x\n", instruction, cpu->pc);
	ProgramException(cpu, OSXPROGRAMILLEGAL);
}

static inline int RequireSupervisor(OsxCpu *cpu)
{
	if (cpu->msr & OSXMSRPR) {
		ProgramException(cpu, OSXPROGRAMPRIVILEGED);
		return 0;
	}
	return 1;
}

static inline int RequireFloat(OsxCpu *cpu)
{
	if (!(cpu->msr & OSXMSRFP)) {
		OsxCpuRaiseException(cpu, OSXVECTORFPUNAVAILABLE, 0, cpu->pc);
		return 0;
	}
	return 1;
}

static uint32 AddExtended(OsxCpu *cpu, uint32 left, uint32 right, uint32 carryin, int instruction)
{
	uint64 sum;
	uint32 result;
	sum = (uint64)left + right + carryin;
	result = (uint32)sum;
	SetCarry(cpu, (int)(sum >> 32));
	if (instruction & 0x400)
		SetOverflow(cpu, (int)((~(left ^ right) & (left ^ result)) >> 31));
	return result;
}

static uint32 AddPlain(OsxCpu *cpu, uint32 left, uint32 right, int instruction)
{
	uint32 result;
	result = left + right;
	if (instruction & 0x400)
		SetOverflow(cpu, (int)((~(left ^ right) & (left ^ result)) >> 31));
	return result;
}

static void CompareSigned(OsxCpu *cpu, int field, int32 left, int32 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	SetCrField(cpu, field, value | (cpu->xer >> 31));
}

static void CompareUnsigned(OsxCpu *cpu, int field, uint32 left, uint32 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	SetCrField(cpu, field, value | (cpu->xer >> 31));
}

static int BranchConditionTaken(OsxCpu *cpu, uint32 options, uint32 bit)
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
		conditionok = CrBit(cpu, (int)bit) == ((options >> 3) & 1);
	return counterok && conditionok;
}

static uint32 ReadTimebaseRegister(OsxCpu *cpu, uint32 number)
{
	uint64 timebase;
	timebase = OsxCpuTimebase(cpu);
	if (number == 269)
		return (uint32)(timebase >> 32);
	return (uint32)timebase;
}

static void WriteTimebase(OsxCpu *cpu, uint32 number, uint32 value)
{
	uint64 current;
	uint64 wanted;
	current = OsxCpuTimebase(cpu);
	if (number == 285)
		wanted = ((uint64)value << 32) | (current & 0xffffffffu);
	else
		wanted = (current & 0xffffffff00000000ULL) | value;
	cpu->tboffset += wanted - current;
	if (cpu->decdeadline != OSXNEVER)
		cpu->decdeadline += wanted - current;
	cpu->decstamp += wanted - current;
}

static void WriteDecrementer(OsxCpu *cpu, uint32 value)
{
	cpu->decvalue = value;
	cpu->decstamp = OsxCpuTimebase(cpu);
	if ((int32)value >= 0)
		cpu->decdeadline = cpu->decstamp + value + 1;
	else
		cpu->decdeadline = OSXNEVER;
}

static int ReadSpr(OsxCpu *cpu, uint32 number, uint32 *value)
{
	switch (number) {
	case 1: *value = cpu->xer; return 1;
	case 8: *value = cpu->lr; return 1;
	case 9: *value = cpu->ctr; return 1;
	case 18: *value = cpu->dsisr; return 1;
	case 19: *value = cpu->dar; return 1;
	case 22: *value = (uint32)(cpu->decvalue - (uint32)(OsxCpuTimebase(cpu) - cpu->decstamp)); return 1;
	case 25: *value = cpu->sdr1; return 1;
	case 26: *value = cpu->srr0; return 1;
	case 27: *value = cpu->srr1; return 1;
	case 268: case 269: *value = ReadTimebaseRegister(cpu, number); return 1;
	case 272: case 273: case 274: case 275: *value = cpu->sprg[number - 272]; return 1;
	case 282: *value = cpu->ear; return 1;
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

static int WriteSpr(OsxCpu *cpu, uint32 number, uint32 value)
{
	switch (number) {
	case 1: cpu->xer = value & 0xe000007f; return 1;
	case 8: cpu->lr = value; return 1;
	case 9: cpu->ctr = value; return 1;
	case 18: cpu->dsisr = value; return 1;
	case 19: cpu->dar = value; return 1;
	case 22: WriteDecrementer(cpu, value); return 1;
	case 25:
		cpu->sdr1 = value;
		OsxCpuFlushTlb(cpu);
		return 1;
	case 26: cpu->srr0 = value; return 1;
	case 27: cpu->srr1 = value; return 1;
	case 272: case 273: case 274: case 275: cpu->sprg[number - 272] = value; return 1;
	case 282: cpu->ear = value; return 1;
	case 284: case 285: WriteTimebase(cpu, number, value); return 1;
	case 528: case 529: case 530: case 531: case 532: case 533: case 534: case 535:
		cpu->ibat[number - 528] = value;
		OsxCpuFlushTlb(cpu);
		return 1;
	case 536: case 537: case 538: case 539: case 540: case 541: case 542: case 543:
		cpu->dbat[number - 536] = value;
		OsxCpuFlushTlb(cpu);
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
	case 1017: cpu->l2cr = value & ~0x00200000u; return 1;
	case 1019: cpu->ictc = value; return 1;
	case 1020: case 1021: case 1022: cpu->thrm[number - 1020] = value; return 1;
	}
	return 0;
}

static void MoveFromSpr(OsxCpu *cpu, uint32 instruction)
{
	uint32 number;
	uint32 value;
	number = ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0);
	if ((number & 0x10) && !RequireSupervisor(cpu))
		return;
	if (!ReadSpr(cpu, number, &value)) {
		if (cpu->logfile)
			fprintf(cpu->logfile, "mfspr unknown %u at %08x\n", number, cpu->pc);
		cpu->unknownspr = number;
		value = 0;
	}
	cpu->gpr[(instruction >> 21) & 31] = value;
}

static void MoveToSpr(OsxCpu *cpu, uint32 instruction)
{
	uint32 number;
	number = ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0);
	if ((number & 0x10) && !RequireSupervisor(cpu))
		return;
	if (!WriteSpr(cpu, number, cpu->gpr[(instruction >> 21) & 31])) {
		if (cpu->logfile)
			fprintf(cpu->logfile, "mtspr unknown %u at %08x\n", number, cpu->pc);
		cpu->unknownspr = number;
	}
}

static void WriteMsr(OsxCpu *cpu, uint32 value)
{
	uint32 changed;
	changed = cpu->msr ^ value;
	cpu->msr = value;
	if (changed & (OSXMSRPR | OSXMSRIR | OSXMSRDR))
		OsxCpuUpdateModes(cpu);
	if ((value & OSXMSRPOW) && (cpu->hid0 & (OSXHID0DOZE | OSXHID0NAP | OSXHID0SLEEP))) {
		cpu->halted = 1;
		cpu->slice = 0;
	}
	if ((changed & OSXMSREE) && (value & OSXMSREE))
		cpu->slice = 0;
}

static void StoreString(OsxCpu *cpu, uint32 address, int first, uint32 count)
{
	uint32 index;
	int reg;
	uint32 shift;
	reg = first;
	shift = 24;
	for (index = 0; index < count; index++) {
		if (!Write8(cpu, address + index, (cpu->gpr[reg] >> shift) & 0xff))
			return;
		if (shift == 0) {
			shift = 24;
			reg = (reg + 1) & 31;
		} else {
			shift -= 8;
		}
	}
}

static void LoadString(OsxCpu *cpu, uint32 address, int first, uint32 count)
{
	uint32 values[32];
	uint32 index;
	uint32 byte;
	int reg;
	int used;
	uint32 shift;
	reg = first;
	shift = 24;
	used = 0;
	memset(values, 0, sizeof(values));
	for (index = 0; index < count; index++) {
		if (!Read8(cpu, address + index, &byte))
			return;
		values[used] |= byte << shift;
		if (shift == 0) {
			shift = 24;
			used++;
		} else {
			shift -= 8;
		}
	}
	if (shift != 24)
		used++;
	for (index = 0; index < (uint32)used; index++) {
		cpu->gpr[reg] = values[index];
		reg = (reg + 1) & 31;
	}
}

static void ZeroCacheBlock(OsxCpu *cpu, uint32 address)
{
	uint8 *host;
	uint32 physical;
	int status;
	int index;
	address &= ~31u;
	host = WriteHost(cpu, address, 0xfe0);
	if (host) {
		memset(host, 0, 32);
		return;
	}
	status = AccessSlow(cpu, address, OSXTLBWRITE, &host, &physical);
	if (status == OSXACCESSFAULT)
		return;
	if (status == OSXACCESSHOST) {
		memset(host, 0, 32);
		return;
	}
	for (index = 0; index < 32; index += 4)
		cpu->buswrite(cpu->buscontext, physical + index, 4, 0);
}

static uint64 SingleToDouble(uint32 word)
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

static uint32 DoubleToSingle(uint64 bits)
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

static void ExecuteLoadStore(OsxCpu *cpu, uint32 instruction, uint32 opcode, uint32 address, int update)
{
	uint32 value;
	uint64 wide;
	int target;
	int base;
	target = (instruction >> 21) & 31;
	base = (instruction >> 16) & 31;
	switch (opcode) {
	case 32:
		if (!Read32(cpu, address, &value))
			return;
		cpu->gpr[target] = value;
		break;
	case 34:
		if (!Read8(cpu, address, &value))
			return;
		cpu->gpr[target] = value;
		break;
	case 36:
		if (!Write32(cpu, address, cpu->gpr[target]))
			return;
		break;
	case 38:
		if (!Write8(cpu, address, cpu->gpr[target]))
			return;
		break;
	case 40:
		if (!Read16(cpu, address, &value))
			return;
		cpu->gpr[target] = value;
		break;
	case 42:
		if (!Read16(cpu, address, &value))
			return;
		cpu->gpr[target] = (uint32)(int32)(int16)value;
		break;
	case 44:
		if (!Write16(cpu, address, cpu->gpr[target]))
			return;
		break;
	case 48:
		if (!RequireFloat(cpu) || !Read32(cpu, address, &value))
			return;
		cpu->fpr[target].bits = SingleToDouble(value);
		break;
	case 50:
		if (!RequireFloat(cpu) || !Read64(cpu, address, &wide))
			return;
		cpu->fpr[target].bits = wide;
		break;
	case 52:
		if (!RequireFloat(cpu) || !Write32(cpu, address, DoubleToSingle(cpu->fpr[target].bits)))
			return;
		break;
	case 54:
		if (!RequireFloat(cpu) || !Write64(cpu, address, cpu->fpr[target].bits))
			return;
		break;
	}
	if (update)
		cpu->gpr[base] = address;
}

static void LoadMultiple(OsxCpu *cpu, uint32 instruction, uint32 address)
{
	uint32 values[32];
	int first;
	int reg;
	first = (instruction >> 21) & 31;
	for (reg = first; reg < 32; reg++)
		if (!Read32(cpu, address + (uint32)(reg - first) * 4, &values[reg]))
			return;
	for (reg = first; reg < 32; reg++)
		cpu->gpr[reg] = values[reg];
}

static void StoreMultiple(OsxCpu *cpu, uint32 instruction, uint32 address)
{
	int first;
	int reg;
	first = (instruction >> 21) & 31;
	for (reg = first; reg < 32; reg++)
		if (!Write32(cpu, address + (uint32)(reg - first) * 4, cpu->gpr[reg]))
			return;
}

static void ExecuteGroup19(OsxCpu *cpu, uint32 instruction)
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
	left = CrBit(cpu, bita);
	right = CrBit(cpu, bitb);
	switch (extended) {
	case 0:
		SetCrField(cpu, bitd >> 2, (cpu->cr >> ((7 - (bita >> 2)) * 4)) & 0xf);
		return;
	case 16:
		target = cpu->lr & ~3u;
		if (BranchConditionTaken(cpu, (uint32)bitd, (uint32)bita))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 528:
		target = cpu->ctr & ~3u;
		if (BranchConditionTaken(cpu, (uint32)bitd | 4, (uint32)bita))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 50:
		if (!RequireSupervisor(cpu))
			return;
		cpu->nextpc = cpu->srr0 & ~3u;
		WriteMsr(cpu, (cpu->srr1 & 0x0000ff73) | (cpu->msr & OSXMSRILE));
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
		IllegalInstruction(cpu, instruction);
		return;
	}
	SetCrBit(cpu, bitd, result);
}

static void TrapCheck(OsxCpu *cpu, uint32 options, uint32 left, uint32 right)
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
		ProgramException(cpu, OSXPROGRAMTRAP);
}

static void ExecuteGroup31(OsxCpu *cpu, uint32 instruction)
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
	int field;
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
		CompareSigned(cpu, rd >> 2, (int32)cpu->gpr[ra], (int32)operand);
		return;
	case 32:
		CompareUnsigned(cpu, rd >> 2, cpu->gpr[ra], operand);
		return;
	case 4:
		TrapCheck(cpu, (uint32)rd, cpu->gpr[ra], operand);
		return;
	case 8: case 520:
		result = AddExtended(cpu, ~cpu->gpr[ra], operand, 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 10: case 522:
		result = AddExtended(cpu, cpu->gpr[ra], operand, 0, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 136: case 648:
		result = AddExtended(cpu, ~cpu->gpr[ra], operand, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 138: case 650:
		result = AddExtended(cpu, cpu->gpr[ra], operand, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 200: case 712:
		result = AddExtended(cpu, ~cpu->gpr[ra], 0, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 202: case 714:
		result = AddExtended(cpu, cpu->gpr[ra], 0, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 232: case 744:
		result = AddExtended(cpu, ~cpu->gpr[ra], 0xffffffffu, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 234: case 746:
		result = AddExtended(cpu, cpu->gpr[ra], 0xffffffffu, (cpu->xer >> 29) & 1, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 40: case 552:
		result = operand - cpu->gpr[ra];
		if (instruction & 0x400)
			SetOverflow(cpu, (int)(((cpu->gpr[ra] ^ operand) & (operand ^ result)) >> 31));
		cpu->gpr[rd] = result;
		break;
	case 266: case 778:
		result = AddPlain(cpu, cpu->gpr[ra], operand, (int)instruction);
		cpu->gpr[rd] = result;
		break;
	case 104: case 616:
		result = 0u - cpu->gpr[ra];
		if (instruction & 0x400)
			SetOverflow(cpu, cpu->gpr[ra] == 0x80000000u);
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
			SetOverflow(cpu, product != (int64)(int32)result);
		cpu->gpr[rd] = result;
		break;
	case 491: case 1003:
		if (operand == 0 || (cpu->gpr[ra] == 0x80000000u && operand == 0xffffffffu)) {
			if ((int32)cpu->gpr[ra] < 0)
				result = 0xffffffffu;
			else
				result = 0;
			if (instruction & 0x400)
				SetOverflow(cpu, 1);
		} else {
			result = (uint32)((int32)cpu->gpr[ra] / (int32)operand);
			if (instruction & 0x400)
				SetOverflow(cpu, 0);
		}
		cpu->gpr[rd] = result;
		break;
	case 459: case 971:
		if (operand == 0) {
			result = 0;
			if (instruction & 0x400)
				SetOverflow(cpu, 1);
		} else {
			result = cpu->gpr[ra] / operand;
			if (instruction & 0x400)
				SetOverflow(cpu, 0);
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
	case 26: result = CountLeadingZeros(source); cpu->gpr[ra] = result; break;
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
			SetCarry(cpu, (int32)source < 0);
		} else if (shift == 0) {
			result = source;
			SetCarry(cpu, 0);
		} else {
			result = (uint32)((int32)source >> shift);
			SetCarry(cpu, (int32)source < 0 && (source & ((1u << shift) - 1)) != 0);
		}
		cpu->gpr[ra] = result;
		break;
	case 824:
		shift = (uint32)rb;
		if (shift == 0) {
			result = source;
			SetCarry(cpu, 0);
		} else {
			result = (uint32)((int32)source >> shift);
			SetCarry(cpu, (int32)source < 0 && (source & ((1u << shift) - 1)) != 0);
		}
		cpu->gpr[ra] = result;
		break;
	case 19:
		cpu->gpr[rd] = cpu->cr;
		return;
	case 144:
		mask = 0;
		for (field = 0; field < 8; field++)
			if (instruction & (0x80000u >> field))
				mask |= 0xf0000000u >> (field * 4);
		cpu->cr = (cpu->cr & ~mask) | (source & mask);
		return;
	case 512:
		SetCrField(cpu, rd >> 2, cpu->xer >> 28);
		cpu->xer &= 0x0fffffffu;
		return;
	case 83:
		if (!RequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->msr;
		return;
	case 146:
		if (!RequireSupervisor(cpu))
			return;
		WriteMsr(cpu, source);
		return;
	case 339:
		MoveFromSpr(cpu, instruction);
		return;
	case 467:
		MoveToSpr(cpu, instruction);
		return;
	case 371:
		cpu->gpr[rd] = ReadTimebaseRegister(cpu, ((instruction >> 16) & 0x1f) | ((instruction >> 6) & 0x3e0));
		return;
	case 595:
		if (!RequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->sr[ra & 15];
		return;
	case 659:
		if (!RequireSupervisor(cpu))
			return;
		cpu->gpr[rd] = cpu->sr[operand >> 28];
		return;
	case 210:
		if (!RequireSupervisor(cpu))
			return;
		cpu->sr[ra & 15] = source;
		cpu->fetchtag = 0;
		return;
	case 242:
		if (!RequireSupervisor(cpu))
			return;
		cpu->sr[operand >> 28] = source;
		cpu->fetchtag = 0;
		return;
	case 306:
		if (!RequireSupervisor(cpu))
			return;
		InvalidateTlbPage(cpu, operand);
		return;
	case 370:
		if (!RequireSupervisor(cpu))
			return;
		OsxCpuFlushTlb(cpu);
		return;
	case 566:
		if (!RequireSupervisor(cpu))
			return;
		return;
	case 470:
		if (!RequireSupervisor(cpu))
			return;
		return;
	case 54: case 86: case 246: case 278: case 598: case 854:
		return;
	case 982:
		cpu->fetchtag = 0;
		if (cpu->nativejit && OsxCpuTranslate(cpu, address, OSXTLBREAD, 1, &value))
			OsxJitInvalidate(cpu, value & ~31u, 32);
		return;
	case 1014:
		ZeroCacheBlock(cpu, address);
		return;
	case 20:
		if (address & 3) {
			cpu->dar = address;
			cpu->dsisr = 0;
			OsxCpuRaiseException(cpu, OSXVECTORALIGNMENT, 0, cpu->pc);
			return;
		}
		if (!Read32(cpu, address, &value))
			return;
		cpu->reservation = address;
		cpu->reservationvalid = 1;
		cpu->gpr[rd] = value;
		return;
	case 150:
		if (address & 3) {
			cpu->dar = address;
			cpu->dsisr = 0;
			OsxCpuRaiseException(cpu, OSXVECTORALIGNMENT, 0, cpu->pc);
			return;
		}
		if (cpu->reservationvalid && cpu->reservation == address) {
			if (!Write32(cpu, address, source))
				return;
			cpu->reservationvalid = 0;
			SetCrField(cpu, 0, 2 | (cpu->xer >> 31));
		} else {
			cpu->reservationvalid = 0;
			SetCrField(cpu, 0, cpu->xer >> 31);
		}
		return;
	case 23: case 55:
		if (!Read32(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 55)
			cpu->gpr[ra] = address;
		return;
	case 87: case 119:
		if (!Read8(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 119)
			cpu->gpr[ra] = address;
		return;
	case 279: case 311:
		if (!Read16(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		if (extended == 311)
			cpu->gpr[ra] = address;
		return;
	case 343: case 375:
		if (!Read16(cpu, address, &value))
			return;
		cpu->gpr[rd] = (uint32)(int32)(int16)value;
		if (extended == 375)
			cpu->gpr[ra] = address;
		return;
	case 151: case 183:
		if (!Write32(cpu, address, source))
			return;
		if (extended == 183)
			cpu->gpr[ra] = address;
		return;
	case 215: case 247:
		if (!Write8(cpu, address, source))
			return;
		if (extended == 247)
			cpu->gpr[ra] = address;
		return;
	case 407: case 439:
		if (!Write16(cpu, address, source))
			return;
		if (extended == 439)
			cpu->gpr[ra] = address;
		return;
	case 534:
		if (!Read32(cpu, address, &value))
			return;
		cpu->gpr[rd] = bswap_32(value);
		return;
	case 790:
		if (!Read16(cpu, address, &value))
			return;
		cpu->gpr[rd] = ((value & 0xff) << 8) | (value >> 8);
		return;
	case 662:
		Write32(cpu, address, bswap_32(source));
		return;
	case 918:
		Write16(cpu, address, ((source & 0xff) << 8) | ((source >> 8) & 0xff));
		return;
	case 533:
		LoadString(cpu, address, rd, cpu->xer & 0x7f);
		return;
	case 661:
		StoreString(cpu, address, rd, cpu->xer & 0x7f);
		return;
	case 597:
		if (ra)
			address = cpu->gpr[ra];
		else
			address = 0;
		if (rb == 0)
			LoadString(cpu, address, rd, 32);
		else
			LoadString(cpu, address, rd, (uint32)rb);
		return;
	case 725:
		if (ra)
			address = cpu->gpr[ra];
		else
			address = 0;
		if (rb == 0)
			StoreString(cpu, address, rd, 32);
		else
			StoreString(cpu, address, rd, (uint32)rb);
		return;
	case 535: case 567:
		if (!RequireFloat(cpu) || !Read32(cpu, address, &value))
			return;
		cpu->fpr[rd].bits = SingleToDouble(value);
		if (extended == 567)
			cpu->gpr[ra] = address;
		return;
	case 599: case 631:
		if (!RequireFloat(cpu) || !Read64(cpu, address, &wide))
			return;
		cpu->fpr[rd].bits = wide;
		if (extended == 631)
			cpu->gpr[ra] = address;
		return;
	case 663: case 695:
		if (!RequireFloat(cpu) || !Write32(cpu, address, DoubleToSingle(cpu->fpr[rd].bits)))
			return;
		if (extended == 695)
			cpu->gpr[ra] = address;
		return;
	case 727: case 759:
		if (!RequireFloat(cpu) || !Write64(cpu, address, cpu->fpr[rd].bits))
			return;
		if (extended == 759)
			cpu->gpr[ra] = address;
		return;
	case 983:
		if (!RequireFloat(cpu))
			return;
		Write32(cpu, address, (uint32)cpu->fpr[rd].bits);
		return;
	case 310:
		if (!Read32(cpu, address, &value))
			return;
		cpu->gpr[rd] = value;
		return;
	case 438:
		Write32(cpu, address, source);
		return;
	default:
		IllegalInstruction(cpu, instruction);
		return;
	}
	if (setcr)
		SetCr0(cpu, result);
}

static void Execute(OsxCpu *cpu, uint32 instruction);

static void ExecuteHook(OsxCpu *cpu, uint32 instruction)
{
	uint32 index;
	OsxHook *hook;
	index = instruction & 0x03ffffff;
	if ((cpu->msr & OSXMSRPR) || index >= (uint32)cpu->hookcount) {
		IllegalInstruction(cpu, instruction);
		return;
	}
	hook = &cpu->hooks[index];
	hook->function(cpu, hook->context, hook->address);
	if (cpu->nextpc == cpu->pc + 4)
		Execute(cpu, hook->original);
}

static void Execute(OsxCpu *cpu, uint32 instruction)
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
		TrapCheck(cpu, (uint32)rd, cpu->gpr[ra], signedimmediate);
		return;
	case 6:
		ExecuteHook(cpu, instruction);
		return;
	case 7:
		cpu->gpr[rd] = (uint32)((int32)cpu->gpr[ra] * (int32)signedimmediate);
		return;
	case 8:
		sum = (uint64)(uint32)~cpu->gpr[ra] + signedimmediate + 1;
		cpu->gpr[rd] = (uint32)sum;
		SetCarry(cpu, (int)(sum >> 32));
		return;
	case 10:
		CompareUnsigned(cpu, rd >> 2, cpu->gpr[ra], immediate);
		return;
	case 11:
		CompareSigned(cpu, rd >> 2, (int32)cpu->gpr[ra], (int32)signedimmediate);
		return;
	case 12:
	case 13:
		sum = (uint64)cpu->gpr[ra] + signedimmediate;
		cpu->gpr[rd] = (uint32)sum;
		SetCarry(cpu, (int)(sum >> 32));
		if (opcode == 13)
			SetCr0(cpu, (uint32)sum);
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
		if (BranchConditionTaken(cpu, (uint32)rd, (uint32)ra))
			cpu->nextpc = target;
		if (instruction & 1)
			cpu->lr = cpu->pc + 4;
		return;
	case 17:
		if (cpu->syscallobserver)
			cpu->syscallobserver(cpu, cpu->syscallcontext);
		OsxCpuRaiseException(cpu, OSXVECTORSYSCALL, 0, cpu->pc + 4);
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
		ExecuteGroup19(cpu, instruction);
		return;
	case 20:
		rotated = RotateLeft(cpu->gpr[rd], (instruction >> 11) & 31);
		mask = RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		result = (rotated & mask) | (cpu->gpr[ra] & ~mask);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			SetCr0(cpu, result);
		return;
	case 21:
		rotated = RotateLeft(cpu->gpr[rd], (instruction >> 11) & 31);
		result = rotated & RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			SetCr0(cpu, result);
		return;
	case 23:
		rotated = RotateLeft(cpu->gpr[rd], cpu->gpr[(instruction >> 11) & 31] & 31);
		result = rotated & RotateMask((instruction >> 6) & 31, (instruction >> 1) & 31);
		cpu->gpr[ra] = result;
		if (instruction & 1)
			SetCr0(cpu, result);
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
		SetCr0(cpu, result);
		return;
	case 29:
		result = cpu->gpr[rd] & (immediate << 16);
		cpu->gpr[ra] = result;
		SetCr0(cpu, result);
		return;
	case 31:
		ExecuteGroup31(cpu, instruction);
		return;
	case 32: case 34: case 36: case 38: case 40: case 42: case 44: case 48: case 50: case 52: case 54:
		ExecuteLoadStore(cpu, instruction, opcode, base + signedimmediate, 0);
		return;
	case 33: case 35: case 37: case 39: case 41: case 43: case 45: case 49: case 51: case 53: case 55:
		ExecuteLoadStore(cpu, instruction, opcode - 1, cpu->gpr[ra] + signedimmediate, 1);
		return;
	case 46:
		LoadMultiple(cpu, instruction, base + signedimmediate);
		return;
	case 47:
		StoreMultiple(cpu, instruction, base + signedimmediate);
		return;
	case 59:
	case 63:
		if (!RequireFloat(cpu))
			return;
		OsxCpuExecuteFloat(cpu, instruction);
		return;
	}
	IllegalInstruction(cpu, instruction);
}

void OsxCpuExecuteInstruction(OsxCpu *cpu, uint32 instruction)
{
	cpu->nextpc = cpu->pc + 4;
	Execute(cpu, instruction);
	cpu->pc = cpu->nextpc;
	cpu->instructions++;
}

void OsxCpuCheckInterrupts(OsxCpu *cpu)
{
	if (!cpu->decpending && cpu->decdeadline != OSXNEVER && OsxCpuTimebase(cpu) >= cpu->decdeadline) {
		cpu->decpending = 1;
		cpu->decdeadline = OSXNEVER;
	}
	if (!(cpu->msr & OSXMSREE))
		return;
	if (cpu->externalline) {
		cpu->externalcount++;
		OsxCpuRaiseException(cpu, OSXVECTOREXTERNAL, 0, cpu->pc);
		cpu->pc = cpu->nextpc;
		return;
	}
	if (cpu->decpending) {
		cpu->decpending = 0;
		cpu->decrementercount++;
		OsxCpuRaiseException(cpu, OSXVECTORDECREMENTER, 0, cpu->pc);
		cpu->pc = cpu->nextpc;
	}
}

void OsxCpuRun(OsxCpu *cpu, int32 count)
{
	uint32 instruction;
	if (cpu->halted)
		return;
	cpu->slice = count;
	while (cpu->slice > 0) {
		cpu->slice--;
		if (((cpu->pc & OSXPAGEMASK) | 1) != cpu->fetchtag && !OsxCpuFetchPage(cpu)) {
			cpu->pc = cpu->nextpc;
			continue;
		}
		instruction = LoadHost32(cpu->fetchhost + (cpu->pc & 0xfff));
		if (cpu->tracefile)
			fprintf(cpu->tracefile, "%08x %08x r3=%08x r4=%08x lr=%08x\n", cpu->pc, instruction, cpu->gpr[3], cpu->gpr[4], cpu->lr);
		cpu->nextpc = cpu->pc + 4;
		Execute(cpu, instruction);
		cpu->pc = cpu->nextpc;
		cpu->instructions++;
	}
}

void OsxCpuDump(OsxCpu *cpu, FILE *output)
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
