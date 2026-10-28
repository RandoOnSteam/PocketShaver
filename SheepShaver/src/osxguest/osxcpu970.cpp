/*
 *  osxcpu970.cpp - Mac OS X guest PowerPC 970 (G5) 64-bit CPU core
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
#include <fenv.h>
#include <stddef.h>
#include "osxcpu.h"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#include <windows.h>
#define OSX970JIT 1
#else
#define OSX970JIT 0
#endif

#if OSXG5

#if defined(_MSC_VER)
#define OSXFORCEINLINE __forceinline
#elif defined(__GNUC__)
#define OSXFORCEINLINE inline __attribute__((always_inline))
#else
#define OSXFORCEINLINE inline
#endif

static void UpdateModes(Osx970 *core);

static uint64 LoadBig(const uint8 *host, int size)
{
	uint32 word;
	uint32 low;
	uint16 half;
	if (size == 4) {
		memcpy(&word, host, 4);
		return tswap32(word);
	}
	if (size == 1)
		return host[0];
	if (size == 2) {
		memcpy(&half, host, 2);
		return tswap16(half);
	}
	memcpy(&word, host, 4);
	memcpy(&low, host + 4, 4);
	return ((uint64)tswap32(word) << 32) | tswap32(low);
}

static void StoreBig(uint8 *host, int size, uint64 value)
{
	uint32 word;
	uint16 half;
	if (size == 4) {
		word = tswap32((uint32)value);
		memcpy(host, &word, 4);
		return;
	}
	if (size == 1) {
		host[0] = (uint8)value;
		return;
	}
	if (size == 2) {
		half = tswap16((uint16)value);
		memcpy(host, &half, 2);
		return;
	}
	word = tswap32((uint32)(value >> 32));
	memcpy(host, &word, 4);
	word = tswap32((uint32)value);
	memcpy(host + 4, &word, 4);
}

static int SixtyFour(Osx970 *core)
{
	return (core->msr & OSX970MSRSF) != 0;
}

static uint64 Effective(Osx970 *core, uint64 address)
{
	if (SixtyFour(core))
		return address;
	return address & 0xffffffffULL;
}

static uint64 RotateLeft64(uint64 value, uint32 count)
{
	count &= 63;
	if (count == 0)
		return value;
	return (value << count) | (value >> (64 - count));
}

static uint64 Mask64(uint32 begin, uint32 end)
{
	uint64 first;
	uint64 last;
	first = 0xffffffffffffffffULL >> begin;
	last = 0xffffffffffffffffULL << (63 - end);
	if (begin <= end)
		return first & last;
	return first | last;
}

static uint64 Duplicate32(uint64 value)
{
	uint64 low;
	low = value & 0xffffffffULL;
	return (low << 32) | low;
}

static uint64 SignExtend32(uint64 value)
{
	return (uint64)(int64)(int32)(uint32)value;
}

static uint64 MultiplyHigh(uint64 left, uint64 right)
{
	uint64 a;
	uint64 b;
	uint64 c;
	uint64 d;
	uint64 middle;
	uint64 cross;
	a = left >> 32;
	b = left & 0xffffffffULL;
	c = right >> 32;
	d = right & 0xffffffffULL;
	cross = b * d;
	middle = a * d + (cross >> 32);
	cross = b * c + (middle & 0xffffffffULL);
	return a * c + (middle >> 32) + (cross >> 32);
}

static uint64 MultiplyHighSigned(uint64 left, uint64 right)
{
	uint64 high;
	high = MultiplyHigh(left, right);
	if ((int64)left < 0)
		high -= right;
	if ((int64)right < 0)
		high -= left;
	return high;
}

static int CountLeadingZeros64(uint64 value)
{
	int count;
	count = 0;
	if (value == 0)
		return 64;
	while (!(value & 0x8000000000000000ULL)) {
		value <<= 1;
		count++;
	}
	return count;
}

static void SetCrField(OsxCpu *cpu, int field, uint32 value)
{
	uint32 shift;
	shift = (uint32)(7 - field) * 4;
	cpu->cr = (cpu->cr & ~(0xfu << shift)) | ((value & 0xf) << shift);
}

static void CompareSigned(Osx970 *core, int field, int64 left, int64 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	SetCrField(core->cpu, field, value | (core->cpu->xer >> 31));
}

static void CompareUnsigned(Osx970 *core, int field, uint64 left, uint64 right)
{
	uint32 value;
	if (left < right)
		value = 8;
	else if (left > right)
		value = 4;
	else
		value = 2;
	SetCrField(core->cpu, field, value | (core->cpu->xer >> 31));
}

static void SetCr0(Osx970 *core, uint64 result)
{
	if (SixtyFour(core))
		CompareSigned(core, 0, (int64)result, 0);
	else
		CompareSigned(core, 0, (int64)(int32)(uint32)result, 0);
}

static void SetCarry(Osx970 *core, int carry)
{
	if (carry)
		core->cpu->xer |= OSXXERCA;
	else
		core->cpu->xer &= ~OSXXERCA;
}

static void SetOverflow(Osx970 *core, int overflow)
{
	if (overflow)
		core->cpu->xer |= OSXXEROV | OSXXERSO;
	else
		core->cpu->xer &= ~OSXXEROV;
}

static uint64 AddExtended(Osx970 *core, uint64 left, uint64 right, uint64 carryin, int setcarry, int setoverflow)
{
	uint64 result;
	uint64 low;
	int carry;
	int overflow;
	result = left + right + carryin;
	if (SixtyFour(core)) {
		carry = result < left || (carryin && result == left);
		overflow = (int)((~(left ^ right) & (left ^ result)) >> 63);
	} else {
		low = (left & 0xffffffffULL) + (right & 0xffffffffULL) + carryin;
		carry = (int)(low >> 32);
		overflow = (int)(((~(left ^ right) & (left ^ result)) >> 31) & 1);
	}
	if (setcarry)
		SetCarry(core, carry);
	if (setoverflow)
		SetOverflow(core, overflow);
	return result;
}

void Osx970FlushTlb(Osx970 *core)
{
	int mode;
	int kind;
	int index;
	if (core->fillcount <= OSX970FILLS) {
		for (index = 0; index < core->fillcount; index++)
			core->filled[index]->page = OSX970TLBINVALID;
	} else {
		for (mode = 0; mode < OSXMODES; mode++)
			for (kind = 0; kind < OSXTLBKINDS; kind++)
				for (index = 0; index < OSX970TLBSIZE; index++)
					core->tlb[mode][kind][index].page = OSX970TLBINVALID;
	}
	core->fillcount = 0;
	core->fetchpage = OSX970TLBINVALID;
}

void Osx970Reset(Osx970 *core, OsxCpu *cpu, uint32 pvr)
{
	memset(core, 0, sizeof(Osx970));
	core->cpu = cpu;
	core->msr = OSX970MSRSF | OSX970MSRHV;
	core->pc = 0x100;
	core->nextpc = 0x104;
	core->hid5 = 0x80;
	cpu->pvr = pvr;
	cpu->altivec = 1;
	core->fillcount = OSX970FILLS + 1;
	Osx970FlushTlb(core);
	UpdateModes(core);
}

void Osx970Raise(Osx970 *core, uint32 vector, uint64 srr1bits)
{
	core->cpu->exceptions++;
	core->srr0 = core->pc;
	core->srr1 = (core->msr & ~OSX970SRR1CLEAR) | srr1bits;
	core->msr = (core->msr & (OSX970MSRHV | OSXMSRME)) | OSX970MSRSF;
	core->nextpc = core->hior + vector;
	core->cpu->halted = 0;
	UpdateModes(core);
}

static void RaiseAfter(Osx970 *core, uint32 vector)
{
	core->cpu->exceptions++;
	core->srr0 = core->nextpc;
	core->srr1 = core->msr & ~OSX970SRR1CLEAR;
	core->msr = (core->msr & (OSX970MSRHV | OSXMSRME)) | OSX970MSRSF;
	core->nextpc = core->hior + vector;
	UpdateModes(core);
}

static void DataFault(Osx970 *core, uint64 address, int kind, uint32 reason)
{
	core->dar = address;
	core->dsisr = reason;
	if (kind == OSXTLBWRITE)
		core->dsisr |= OSXDSISRSTORE;
	Osx970Raise(core, OSXVECTORDSI, 0);
}

static void TranslationFault(Osx970 *core, uint64 address, int kind, uint32 reason)
{
	if (kind == OSXTLBFETCH)
		Osx970Raise(core, OSXVECTORISI, reason);
	else
		DataFault(core, address, kind, reason);
}

static void SegmentFault(Osx970 *core, uint64 address, int kind)
{
	if (kind == OSXTLBFETCH) {
		Osx970Raise(core, OSX970VECTORISEGMENT, 0);
		return;
	}
	core->dar = address;
	core->dsisr = 0;
	Osx970Raise(core, OSX970VECTORDSEGMENT, 0);
}

static int Protected(int key, uint32 pp, int kind)
{
	if (key == 0) {
		if (kind == OSXTLBWRITE && pp == 3)
			return 1;
		return 0;
	}
	if (pp == 0)
		return 1;
	if (kind == OSXTLBWRITE && pp != 2)
		return 1;
	return 0;
}

static int PhysicalHost(Osx970 *core, uint64 physical, uint8 **host)
{
	OsxCpu *cpu;
	cpu = core->cpu;
	if (physical < cpu->ramsize) {
		*host = cpu->ram + physical;
		return 1;
	}
	if (physical >= cpu->vrambase && physical - cpu->vrambase < cpu->vramsize) {
		*host = cpu->vram + (physical - cpu->vrambase);
		return 1;
	}
	return 0;
}

int Osx970Translate(Osx970 *core, uint64 address, int kind, int raise, uint64 *physical)
{
	Osx970Slb *slb;
	uint64 esid;
	uint64 vsid;
	uint64 hash;
	uint64 mask;
	uint64 pteg;
	uint64 compare;
	uint64 word0;
	uint64 word1;
	uint64 updated;
	uint8 *host;
	int translated;
	int index;
	int pass;
	int slot;
	int key;
	if (kind == OSXTLBFETCH)
		translated = (core->msr & OSXMSRIR) != 0;
	else
		translated = (core->msr & OSXMSRDR) != 0;
	if (!translated) {
		*physical = address & 0x0fffffffffffffffULL;
		return 1;
	}
	esid = (address & 0xfffffffff0000000ULL) | OSX970SLBVALID;
	slb = &core->slb[core->slbhint];
	if (slb->esid != esid) {
		slb = NULL;
		for (index = 0; index < OSX970SLBSIZE; index++) {
			if (core->slb[index].esid == esid) {
				slb = &core->slb[index];
				core->slbhint = index;
				break;
			}
		}
	}
	if (slb == NULL) {
		if (raise)
			SegmentFault(core, address, kind);
		return 0;
	}
	if (kind == OSXTLBFETCH && (slb->vsid & OSX970SLBN)) {
		if (raise)
			TranslationFault(core, address, kind, OSXISIGUARDED);
		return 0;
	}
	if (core->msr & OSXMSRPR)
		key = (slb->vsid & OSX970SLBKP) != 0;
	else
		key = (slb->vsid & OSX970SLBKS) != 0;
	vsid = (slb->vsid & 0x3ffffffffffff000ULL) >> 12;
	mask = ((uint64)1 << (11 + (core->sdr1 & 0x1f))) - 1;
	hash = vsid ^ ((address & 0x0fffffffULL) >> 12);
	compare = (slb->vsid & 0x3ffffffffffff000ULL) | ((address >> 16) & 0xf80) | 1;
	for (pass = 0; pass < 2; pass++) {
		pteg = (core->sdr1 & 0x0ffffffffffc0000ULL) | ((hash & mask) << 7);
		for (slot = 0; slot < 8; slot++) {
			if (!PhysicalHost(core, pteg + (uint64)slot * 16, &host))
				break;
			word0 = LoadBig(host, 8);
			if ((word0 ^ (compare | ((uint64)pass << 1))) & 0xffffffffffffff83ULL)
				continue;
			word1 = LoadBig(host + 8, 8);
			if (Protected(key, (uint32)(word1 & 3), kind) || (kind == OSXTLBFETCH && (word1 & 0x0c))) {
				if (raise) {
					if (kind == OSXTLBFETCH && (word1 & 0x0c))
						TranslationFault(core, address, kind, OSXISIGUARDED);
					else
						TranslationFault(core, address, kind, OSXDSISRPROTECTION);
				}
				return 0;
			}
			updated = word1 | 0x100;
			if (kind == OSXTLBWRITE)
				updated |= 0x80;
			if (updated != word1)
				StoreBig(host + 8, 8, updated);
			*physical = (word1 & 0x0ffffffffffff000ULL) | (address & 0xfff);
			return 1;
		}
		hash = ~hash;
	}
	if (raise)
		TranslationFault(core, address, kind, OSXDSISRNOTFOUND);
	return 0;
}

static int Mode(Osx970 *core, int kind)
{
	uint64 bit;
	bit = OSXMSRDR;
	if (kind == OSXTLBFETCH)
		bit = OSXMSRIR;
	if (!(core->msr & bit))
		return OSXMODEREAL;
	if (core->msr & OSXMSRPR)
		return OSXMODEUSER;
	return OSXMODESUPERVISOR;
}

static void UpdateModes(Osx970 *core)
{
	core->modes[OSXTLBREAD] = Mode(core, OSXTLBREAD);
	core->modes[OSXTLBWRITE] = core->modes[OSXTLBREAD];
	core->modes[OSXTLBFETCH] = Mode(core, OSXTLBFETCH);
	core->fetchpage = OSX970TLBINVALID;
}

void Osx970InvalidatePhysical(Osx970 *core, uint64 physical, uint64 length)
{
	uint64 page;
	uint64 last;
	if (core->codepages == NULL || length == 0 || physical >= core->cpu->ramsize)
		return;
	if (length > core->cpu->ramsize - physical)
		length = core->cpu->ramsize - physical;
	last = (physical + length - 1) >> 12;
	for (page = physical >> 12; page <= last; page++) {
		if (core->codepages[page]) {
			core->codepages[page] = 0;
			core->versions[page]++;
		}
	}
}

static uint8 *HostSlow(Osx970 *core, uint64 address, int kind, int raise, uint64 *physical, int *bus, Osx970TlbEntry *entry)
{
	uint8 *host;
	if (!Osx970Translate(core, address, kind, raise, physical))
		return NULL;
	if (kind == OSXTLBWRITE)
		Osx970InvalidatePhysical(core, *physical, 1);
	if (!PhysicalHost(core, *physical, &host)) {
		*bus = 1;
		return NULL;
	}
	entry->page = address & ~0xfffULL;
	if (core->fillcount < OSX970FILLS)
		core->filled[core->fillcount++] = entry;
	else
		core->fillcount = OSX970FILLS + 1;
	entry->host = (uint8 *)((uintptr_t)host - (uintptr_t)address);
	return host;
}

static OSXFORCEINLINE uint8 *Host(Osx970 *core, uint64 address, int kind, int raise, uint64 *physical, int *bus)
{
	Osx970TlbEntry *entry;
	*bus = 0;
	entry = &core->tlb[core->modes[kind]][kind][(address >> 12) & OSX970TLBMASK];
	if (entry->page == (address & ~0xfffULL))
		return (uint8 *)((uintptr_t)entry->host + (uintptr_t)address);
	return HostSlow(core, address, kind, raise, physical, bus, entry);
}

int Osx970Read(Osx970 *core, uint64 address, int size, uint64 *value)
{
	uint8 *host;
	uint64 physical;
	uint64 result;
	uint64 byte;
	int bus;
	int index;
	address = Effective(core, address);
	if ((address & 0xfff) + (uint64)size > 0x1000) {
		result = 0;
		for (index = 0; index < size; index++) {
			if (!Osx970Read(core, address + (uint64)index, 1, &byte))
				return 0;
			result = (result << 8) | byte;
		}
		*value = result;
		return 1;
	}
	host = Host(core, address, OSXTLBREAD, 1, &physical, &bus);
	if (host) {
		*value = LoadBig(host, size);
		return 1;
	}
	if (!bus)
		return 0;
	if (physical > 0xffffffffULL) {
		*value = 0;
		return 1;
	}
	if (size == 8) {
		result = core->cpu->busread(core->cpu->buscontext, (uint32)physical, 4);
		*value = (result << 32) | core->cpu->busread(core->cpu->buscontext, (uint32)physical + 4, 4);
		return 1;
	}
	*value = core->cpu->busread(core->cpu->buscontext, (uint32)physical, size);
	return 1;
}

int Osx970Write(Osx970 *core, uint64 address, int size, uint64 value)
{
	uint8 *host;
	uint64 physical;
	int bus;
	int index;
	address = Effective(core, address);
	if ((address & 0xfff) + (uint64)size > 0x1000) {
		for (index = 0; index < size; index++)
			if (!Osx970Write(core, address + (uint64)index, 1, (value >> ((size - 1 - index) * 8)) & 0xff))
				return 0;
		return 1;
	}
	host = Host(core, address, OSXTLBWRITE, 1, &physical, &bus);
	if (host) {
		StoreBig(host, size, value);
		return 1;
	}
	if (!bus)
		return 0;
	if (physical > 0xffffffffULL)
		return 1;
	if (size == 8) {
		core->cpu->buswrite(core->cpu->buscontext, (uint32)physical, 4, (uint32)(value >> 32));
		core->cpu->buswrite(core->cpu->buscontext, (uint32)physical + 4, 4, (uint32)value);
		return 1;
	}
	core->cpu->buswrite(core->cpu->buscontext, (uint32)physical, size, (uint32)value);
	return 1;
}

static int Fetch(Osx970 *core, uint32 *instruction)
{
	uint64 physical;
	uint8 *host;
	int bus;
	if ((core->pc & ~0xfffULL) != core->fetchpage) {
		host = Host(core, core->pc, OSXTLBFETCH, 1, &physical, &bus);
		if (host == NULL) {
			if (bus)
				Osx970Raise(core, OSXVECTORMACHINECHECK, 0);
			return 0;
		}
		core->fetchhost = host - (core->pc & 0xfff);
		core->fetchpage = core->pc & ~0xfffULL;
	}
	*instruction = (uint32)LoadBig(core->fetchhost + (core->pc & 0xfff), 4);
	return 1;
}

static int Supervisor(Osx970 *core)
{
	if (!(core->msr & OSXMSRPR))
		return 1;
	Osx970Raise(core, OSXVECTORPROGRAM, OSXPROGRAMPRIVILEGED);
	return 0;
}

static void Illegal(Osx970 *core)
{
	Osx970Raise(core, OSXVECTORPROGRAM, OSXPROGRAMILLEGAL);
}

static void WriteMsr(Osx970 *core, uint64 value)
{
	core->msr = value;
	UpdateModes(core);
	if ((value & OSXMSRPOW) && (core->hid0 & (OSX970HID0DOZE | OSX970HID0NAP | OSX970HID0DEEPNAP))) {
		core->cpu->halted = 1;
		core->cpu->slice = 0;
	}
	if ((value & OSXMSREE) && (core->cpu->externalline || core->cpu->decpending))
		core->cpu->slice = 0;
}

static uint64 Timebase(Osx970 *core)
{
	return OsxCpuTimebase(core->cpu);
}

static void WriteTimebase(Osx970 *core, uint32 number, uint64 value)
{
	OsxCpu *cpu;
	uint64 current;
	uint64 wanted;
	cpu = core->cpu;
	current = Timebase(core);
	if (number == 285)
		wanted = (value << 32) | (current & 0xffffffffULL);
	else
		wanted = (current & 0xffffffff00000000ULL) | (value & 0xffffffffULL);
	cpu->tboffset += wanted - current;
	if (cpu->decdeadline != OSXNEVER)
		cpu->decdeadline += wanted - current;
	cpu->decstamp += wanted - current;
}

static void WriteDecrementer(Osx970 *core, uint32 value)
{
	OsxCpu *cpu;
	cpu = core->cpu;
	cpu->decvalue = value;
	cpu->decstamp = Timebase(core);
	if ((int32)value >= 0)
		cpu->decdeadline = cpu->decstamp + value + 1;
	else
		cpu->decdeadline = OSXNEVER;
}

static int ReadSpr(Osx970 *core, uint32 number, uint64 *value)
{
	OsxCpu *cpu;
	cpu = core->cpu;
	switch (number) {
	case 1: *value = cpu->xer; return 1;
	case 8: *value = core->lr; return 1;
	case 9: *value = core->ctr; return 1;
	case 18: *value = core->dsisr; return 1;
	case 19: *value = core->dar; return 1;
	case 22: *value = (uint32)(cpu->decvalue - (uint32)(Timebase(core) - cpu->decstamp)); return 1;
	case 25: *value = core->sdr1; return 1;
	case 26: *value = core->srr0; return 1;
	case 27: *value = core->srr1; return 1;
	case 256: *value = cpu->vrsave; return 1;
	case 259: *value = core->sprg[3]; return 1;
	case 268: *value = Timebase(core); return 1;
	case 269: *value = Timebase(core) >> 32; return 1;
	case 272: case 273: case 274: case 275: *value = core->sprg[number - 272]; return 1;
	case 276: *value = core->scomc; return 1;
	case 277: *value = core->scomd; return 1;
	case 287: *value = cpu->pvr; return 1;
	case 304: case 305: *value = core->hsprg[number - 304]; return 1;
	case 311: *value = core->hior; return 1;
	case 314: *value = core->hsrr0; return 1;
	case 315: *value = core->hsrr1; return 1;
	case 1008: *value = core->hid0; return 1;
	case 1009: *value = core->hid1; return 1;
	case 1010: *value = core->iabr; return 1;
	case 1012: *value = core->hid4; return 1;
	case 1013: *value = core->dabr; return 1;
	case 1014: *value = core->hid5; return 1;
	case 1023: *value = core->pir; return 1;
	}
	if (number >= 768 && number <= 799) {
		*value = 0;
		return 1;
	}
	return 0;
}

static int WriteSpr(Osx970 *core, uint32 number, uint64 value)
{
	OsxCpu *cpu;
	cpu = core->cpu;
	switch (number) {
	case 1: cpu->xer = (uint32)value & 0xe000007f; return 1;
	case 8: core->lr = value; return 1;
	case 9: core->ctr = value; return 1;
	case 18: core->dsisr = (uint32)value; return 1;
	case 19: core->dar = value; return 1;
	case 22: WriteDecrementer(core, (uint32)value); return 1;
	case 25: core->sdr1 = value; Osx970FlushTlb(core); return 1;
	case 26: core->srr0 = value; return 1;
	case 27: core->srr1 = value; return 1;
	case 256: cpu->vrsave = (uint32)value; return 1;
	case 272: case 273: case 274: case 275: core->sprg[number - 272] = value; return 1;
	case 276: core->scomc = value; return 1;
	case 277: core->scomd = value; return 1;
	case 284: case 285: WriteTimebase(core, number, value); return 1;
	case 304: case 305: core->hsprg[number - 304] = value; return 1;
	case 311: core->hior = value; return 1;
	case 314: core->hsrr0 = value; return 1;
	case 315: core->hsrr1 = value; return 1;
	case 1008: core->hid0 = value; return 1;
	case 1009: core->hid1 = value; return 1;
	case 1010: core->iabr = value; return 1;
	case 1012: core->hid4 = value; return 1;
	case 1013: core->dabr = value; return 1;
	case 1014: core->hid5 = value; return 1;
	case 1023: return 1;
	}
	if (number >= 768 && number <= 799)
		return 1;
	return 0;
}

static uint32 SprNumber(uint32 instruction)
{
	return ((instruction >> 16) & 31) | (((instruction >> 11) & 31) << 5);
}

static void MoveFromSpr(Osx970 *core, uint32 instruction)
{
	uint32 number;
	uint64 value;
	number = SprNumber(instruction);
	if ((number & 0x10) && !Supervisor(core))
		return;
	if (!ReadSpr(core, number, &value)) {
		if (core->cpu->logfile)
			fprintf(core->cpu->logfile, "970 mfspr %u at %016llx\n", number, (unsigned long long)core->pc);
		value = 0;
	}
	core->gpr[(instruction >> 21) & 31] = value;
}

static void MoveToSpr(Osx970 *core, uint32 instruction)
{
	uint32 number;
	number = SprNumber(instruction);
	if ((number & 0x10) && !Supervisor(core))
		return;
	if (!WriteSpr(core, number, core->gpr[(instruction >> 21) & 31]) && core->cpu->logfile)
		fprintf(core->cpu->logfile, "970 mtspr %u at %016llx\n", number, (unsigned long long)core->pc);
}

static void Trap(Osx970 *core, uint32 options, uint64 left, uint64 right, int doubleword)
{
	int64 signedleft;
	int64 signedright;
	int trap;
	if (doubleword) {
		signedleft = (int64)left;
		signedright = (int64)right;
	} else {
		signedleft = (int64)(int32)(uint32)left;
		signedright = (int64)(int32)(uint32)right;
		left &= 0xffffffffULL;
		right &= 0xffffffffULL;
	}
	trap = 0;
	if ((options & 16) && signedleft < signedright)
		trap = 1;
	if ((options & 8) && signedleft > signedright)
		trap = 1;
	if ((options & 4) && left == right)
		trap = 1;
	if ((options & 2) && left < right)
		trap = 1;
	if ((options & 1) && left > right)
		trap = 1;
	if (trap)
		Osx970Raise(core, OSXVECTORPROGRAM, OSXPROGRAMTRAP);
}

static int BranchTaken(Osx970 *core, uint32 options, uint32 bit, int decrement)
{
	uint64 counter;
	int counterok;
	int conditionok;
	counterok = 1;
	if (decrement && !(options & 4)) {
		core->ctr--;
		counter = core->ctr;
		if (!SixtyFour(core))
			counter &= 0xffffffffULL;
		counterok = (counter != 0) != ((options & 2) != 0);
	}
	conditionok = 1;
	if (!(options & 16))
		conditionok = ((core->cpu->cr >> (31 - bit)) & 1) == ((options >> 3) & 1);
	return counterok && conditionok;
}

static void SetCrBit(OsxCpu *cpu, uint32 bit, uint32 value)
{
	if (value)
		cpu->cr |= 0x80000000u >> bit;
	else
		cpu->cr &= ~(0x80000000u >> bit);
}

static void ExecuteGroup19(Osx970 *core, uint32 instruction)
{
	OsxCpu *cpu;
	uint32 extended;
	uint32 a;
	uint32 b;
	uint32 result;
	uint64 target;
	cpu = core->cpu;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 0:
		SetCrField(cpu, (int)((instruction >> 23) & 7), (cpu->cr >> ((7 - ((instruction >> 18) & 7)) * 4)) & 0xf);
		return;
	case 16:
	case 528:
		if (extended == 16)
			target = core->lr & ~3ULL;
		else
			target = core->ctr & ~3ULL;
		if (BranchTaken(core, (instruction >> 21) & 31, (instruction >> 16) & 31, extended == 16))
			core->nextpc = Effective(core, target);
		if (instruction & 1)
			core->lr = Effective(core, core->pc + 4);
		return;
	case 18:
		if (!Supervisor(core))
			return;
		core->nextpc = core->srr0 & ~3ULL;
		WriteMsr(core, (core->srr1 & ~OSX970MSRHV) | (core->msr & OSX970MSRHV));
		core->nextpc = Effective(core, core->nextpc);
		return;
	case 50:
		if (!Supervisor(core))
			return;
		core->nextpc = core->srr0 & 0xfffffffcULL;
		WriteMsr(core, (core->msr & OSX970MSRHV) | (core->srr1 & 0xffffffffULL));
		return;
	case 274:
		if (!Supervisor(core))
			return;
		core->nextpc = core->hsrr0 & ~3ULL;
		WriteMsr(core, core->hsrr1);
		core->nextpc = Effective(core, core->nextpc);
		return;
	case 150:
		core->fetchpage = OSX970TLBINVALID;
		return;
	}
	a = (cpu->cr >> (31 - ((instruction >> 16) & 31))) & 1;
	b = (cpu->cr >> (31 - ((instruction >> 11) & 31))) & 1;
	switch (extended) {
	case 33: result = !(a | b); break;
	case 129: result = a & !b; break;
	case 193: result = a ^ b; break;
	case 225: result = !(a & b); break;
	case 257: result = a & b; break;
	case 289: result = !(a ^ b); break;
	case 417: result = a | !b; break;
	case 449: result = a | b; break;
	default:
		Illegal(core);
		return;
	}
	SetCrBit(cpu, (instruction >> 21) & 31, result);
}

static void ExecuteRotate(Osx970 *core, uint32 instruction)
{
	uint64 source;
	uint64 rotated;
	uint64 mask;
	uint32 shift;
	uint32 field;
	uint32 bound;
	int ra;
	source = core->gpr[(instruction >> 21) & 31];
	ra = (instruction >> 16) & 31;
	shift = ((instruction >> 11) & 31) | (((instruction >> 1) & 1) << 5);
	field = (instruction >> 5) & 0x3f;
	bound = ((field & 1) << 5) | (field >> 1);
	switch ((instruction >> 2) & 7) {
	case 0:
		core->gpr[ra] = RotateLeft64(source, shift) & Mask64(bound, 63);
		break;
	case 1:
		core->gpr[ra] = RotateLeft64(source, shift) & Mask64(0, bound);
		break;
	case 2:
		core->gpr[ra] = RotateLeft64(source, shift) & Mask64(bound, 63 - shift);
		break;
	case 3:
		mask = Mask64(bound, 63 - shift);
		core->gpr[ra] = (RotateLeft64(source, shift) & mask) | (core->gpr[ra] & ~mask);
		break;
	case 4:
		rotated = RotateLeft64(source, (uint32)(core->gpr[(instruction >> 11) & 31] & 63));
		if ((instruction >> 1) & 1)
			core->gpr[ra] = rotated & Mask64(0, bound);
		else
			core->gpr[ra] = rotated & Mask64(bound, 63);
		break;
	default:
		Illegal(core);
		return;
	}
	if (instruction & 1)
		SetCr0(core, core->gpr[ra]);
}

static uint64 IndexedAddress(Osx970 *core, uint32 instruction)
{
	uint32 ra;
	uint64 base;
	ra = (instruction >> 16) & 31;
	base = 0;
	if (ra)
		base = core->gpr[ra];
	return Effective(core, base + core->gpr[(instruction >> 11) & 31]);
}

static void UpdateBase(Osx970 *core, uint32 instruction, uint64 address)
{
	uint32 ra;
	ra = (instruction >> 16) & 31;
	if (ra)
		core->gpr[ra] = address;
}

static int VectorEnabled(Osx970 *core)
{
	if (core->msr & OSXMSRVEC)
		return 1;
	Osx970Raise(core, OSXVECTORALTIVEC, 0);
	return 0;
}

static int FloatEnabled(Osx970 *core)
{
	if (core->msr & OSXMSRFP)
		return 1;
	Osx970Raise(core, OSXVECTORFPUNAVAILABLE, 0);
	return 0;
}

static uint32 VectorByte(const uint32 *vector, uint32 index)
{
	return (vector[index >> 2] >> ((3 - (index & 3)) * 8)) & 0xff;
}

static void SetVectorByte(uint32 *vector, uint32 index, uint32 value)
{
	uint32 shift;
	shift = (3 - (index & 3)) * 8;
	vector[index >> 2] = (vector[index >> 2] & ~(0xffu << shift)) | ((value & 0xff) << shift);
}

static int ExecuteVectorMemory(Osx970 *core, uint32 instruction, uint32 extended)
{
	uint32 *vector;
	uint32 element;
	uint32 index;
	uint32 size;
	uint64 address;
	uint64 value;
	switch (extended) {
	case 342: case 374: case 822:
		return 1;
	case 6: case 38: case 7: case 39: case 71: case 103: case 359: case 135: case 167: case 199: case 231: case 487:
		break;
	default:
		return 0;
	}
	if (!VectorEnabled(core))
		return 1;
	vector = core->cpu->vr[(instruction >> 21) & 31];
	address = IndexedAddress(core, instruction);
	switch (extended) {
	case 6: case 38:
		element = (uint32)(address & 15);
		if (extended == 38)
			element = 16 - element;
		for (index = 0; index < 16; index++)
			SetVectorByte(vector, index, element + index);
		return 1;
	case 103: case 359:
		address &= ~15ULL;
		for (index = 0; index < 4; index++) {
			if (!Osx970Read(core, address + index * 4, 4, &value))
				return 1;
			vector[index] = (uint32)value;
		}
		return 1;
	case 231: case 487:
		address &= ~15ULL;
		for (index = 0; index < 4; index++)
			if (!Osx970Write(core, address + index * 4, 4, vector[index]))
				return 1;
		return 1;
	}
	size = 1;
	if (extended == 39 || extended == 167)
		size = 2;
	if (extended == 71 || extended == 199)
		size = 4;
	address &= ~(uint64)(size - 1);
	element = (uint32)(address & 15);
	if (extended == 7 || extended == 39 || extended == 71) {
		if (!Osx970Read(core, address, (int)size, &value))
			return 1;
		for (index = 0; index < size; index++)
			SetVectorByte(vector, element + index, (uint32)(value >> ((size - 1 - index) * 8)));
		return 1;
	}
	value = 0;
	for (index = 0; index < size; index++)
		value = (value << 8) | VectorByte(vector, element + index);
	Osx970Write(core, address, (int)size, value);
	return 1;
}

static int ExecuteFloatMemory(Osx970 *core, uint32 instruction, uint32 extended, uint64 address)
{
	OsxCpu *cpu;
	uint64 value;
	int target;
	cpu = core->cpu;
	target = (instruction >> 21) & 31;
	if (!FloatEnabled(core))
		return 0;
	switch (extended) {
	case 48:
		if (!Osx970Read(core, address, 4, &value))
			return 0;
		cpu->fpr[target].bits = OsxCpuSingleToDouble((uint32)value);
		return 1;
	case 50:
		if (!Osx970Read(core, address, 8, &value))
			return 0;
		cpu->fpr[target].bits = value;
		return 1;
	case 52:
		return Osx970Write(core, address, 4, OsxCpuDoubleToSingle(cpu->fpr[target].bits));
	case 54:
		return Osx970Write(core, address, 8, cpu->fpr[target].bits);
	case 983:
		return Osx970Write(core, address, 4, (uint32)cpu->fpr[target].bits);
	}
	return 0;
}

static void LoadStoreString(Osx970 *core, uint32 instruction, uint64 address, uint32 count, int store)
{
	uint32 reg;
	uint32 index;
	uint64 byte;
	reg = (instruction >> 21) & 31;
	for (index = 0; index < count; index++) {
		if (index && !(index & 3))
			reg = (reg + 1) & 31;
		if (store) {
			byte = (core->gpr[reg] >> (24 - (index & 3) * 8)) & 0xff;
			if (!Osx970Write(core, address + index, 1, byte))
				return;
		} else {
			if (!(index & 3))
				core->gpr[reg] = 0;
			if (!Osx970Read(core, address + index, 1, &byte))
				return;
			core->gpr[reg] |= byte << (24 - (index & 3) * 8);
		}
	}
}

static void ZeroBlock(Osx970 *core, uint64 address, uint32 size)
{
	uint32 index;
	address &= ~(uint64)(size - 1);
	for (index = 0; index < size; index += 8)
		if (!Osx970Write(core, address + index, 8, 0))
			return;
}

static void Arithmetic(Osx970 *core, uint32 instruction, uint32 kind)
{
	OsxCpu *cpu;
	uint64 a;
	uint64 b;
	uint64 result;
	uint64 carry;
	int64 product;
	int oe;
	int rt;
	cpu = core->cpu;
	rt = (instruction >> 21) & 31;
	a = core->gpr[(instruction >> 16) & 31];
	b = core->gpr[(instruction >> 11) & 31];
	oe = (instruction >> 10) & 1;
	carry = (cpu->xer >> 29) & 1;
	result = 0;
	switch (kind) {
	case 8: result = AddExtended(core, ~a, b, 1, 1, oe); break;
	case 10: result = AddExtended(core, a, b, 0, 1, oe); break;
	case 40: result = AddExtended(core, ~a, b, 1, 0, oe); break;
	case 104: result = AddExtended(core, ~a, 0, 1, 0, oe); break;
	case 136: result = AddExtended(core, ~a, b, carry, 1, oe); break;
	case 138: result = AddExtended(core, a, b, carry, 1, oe); break;
	case 200: result = AddExtended(core, ~a, 0, carry, 1, oe); break;
	case 202: result = AddExtended(core, a, 0, carry, 1, oe); break;
	case 232: result = AddExtended(core, ~a, 0xffffffffffffffffULL, carry, 1, oe); break;
	case 234: result = AddExtended(core, a, 0xffffffffffffffffULL, carry, 1, oe); break;
	case 266: result = AddExtended(core, a, b, 0, 0, oe); break;
	case 9: result = MultiplyHigh(a, b); break;
	case 73: result = MultiplyHighSigned(a, b); break;
	case 11: result = ((a & 0xffffffffULL) * (b & 0xffffffffULL)) >> 32; break;
	case 75: result = (uint64)(uint32)((uint64)((int64)(int32)(uint32)a * (int64)(int32)(uint32)b) >> 32); break;
	case 233:
		result = a * b;
		if (oe)
			SetOverflow(core, MultiplyHighSigned(a, b) != (uint64)((int64)result >> 63));
		break;
	case 235:
		product = (int64)(int32)(uint32)a * (int64)(int32)(uint32)b;
		result = (uint64)product;
		if (oe)
			SetOverflow(core, product != (int64)(int32)product);
		break;
	case 457:
		if (b == 0)
			result = 0;
		else
			result = a / b;
		if (oe)
			SetOverflow(core, b == 0);
		break;
	case 459:
		if ((uint32)b == 0)
			result = 0;
		else
			result = (uint32)a / (uint32)b;
		if (oe)
			SetOverflow(core, (uint32)b == 0);
		break;
	case 489:
		if (b == 0 || (a == 0x8000000000000000ULL && b == 0xffffffffffffffffULL)) {
			result = 0;
			if (oe)
				SetOverflow(core, 1);
		} else {
			result = (uint64)((int64)a / (int64)b);
			if (oe)
				SetOverflow(core, 0);
		}
		break;
	case 491:
		if ((uint32)b == 0 || ((uint32)a == 0x80000000u && (uint32)b == 0xffffffffu)) {
			result = 0;
			if (oe)
				SetOverflow(core, 1);
		} else {
			result = (uint32)((int32)(uint32)a / (int32)(uint32)b);
			if (oe)
				SetOverflow(core, 0);
		}
		break;
	}
	core->gpr[rt] = result;
	if (instruction & 1)
		SetCr0(core, result);
}

static int ArithmeticKind(uint32 kind)
{
	switch (kind) {
	case 8: case 9: case 10: case 11: case 40: case 73: case 75: case 104: case 136: case 138:
	case 200: case 202: case 232: case 233: case 234: case 235: case 266: case 457: case 459: case 489: case 491:
		return 1;
	}
	return 0;
}

static void ShiftRightAlgebraic(Osx970 *core, int ra, uint64 source, uint32 count, int doubleword)
{
	int64 value;
	uint64 lost;
	if (doubleword)
		value = (int64)source;
	else
		value = (int64)(int32)(uint32)source;
	if ((doubleword && (count & 0x40)) || (!doubleword && (count & 0x20))) {
		core->gpr[ra] = (uint64)(value >> 63);
		SetCarry(core, value < 0);
		return;
	}
	lost = 0;
	if (count)
		lost = (uint64)value & (((uint64)1 << count) - 1);
	core->gpr[ra] = (uint64)(value >> count);
	SetCarry(core, value < 0 && lost != 0);
}

static void ExecuteGroup31(Osx970 *core, uint32 instruction)
{
	OsxCpu *cpu;
	uint32 extended;
	uint32 count;
	uint32 mask;
	uint32 field;
	uint64 address;
	uint64 value;
	uint64 rs;
	uint64 rb;
	uint64 slbvalue;
	int rt;
	int ra;
	int rbnum;
	int index;
	cpu = core->cpu;
	extended = (instruction >> 1) & 0x3ff;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	rbnum = (instruction >> 11) & 31;
	rs = core->gpr[rt];
	rb = core->gpr[rbnum];
	if (ArithmeticKind(extended & 0x1ff)) {
		Arithmetic(core, instruction, extended & 0x1ff);
		return;
	}
	if (ExecuteVectorMemory(core, instruction, extended))
		return;
	switch (extended) {
	case 0:
		if ((instruction >> 21) & 1)
			CompareSigned(core, (int)((instruction >> 23) & 7), (int64)core->gpr[ra], (int64)rb);
		else
			CompareSigned(core, (int)((instruction >> 23) & 7), (int64)(int32)(uint32)core->gpr[ra], (int64)(int32)(uint32)rb);
		return;
	case 32:
		if ((instruction >> 21) & 1)
			CompareUnsigned(core, (int)((instruction >> 23) & 7), core->gpr[ra], rb);
		else
			CompareUnsigned(core, (int)((instruction >> 23) & 7), core->gpr[ra] & 0xffffffffULL, rb & 0xffffffffULL);
		return;
	case 4:
		Trap(core, (uint32)rt, core->gpr[ra], rb, 0);
		return;
	case 68:
		Trap(core, (uint32)rt, core->gpr[ra], rb, 1);
		return;
	case 19:
		if (instruction & 0x00100000) {
			field = (instruction >> 12) & 0xff;
			mask = 0;
			for (index = 0; index < 8; index++)
				if (field & (0x80u >> index))
					mask |= 0xf0000000u >> (index * 4);
			core->gpr[rt] = cpu->cr & mask;
		} else
			core->gpr[rt] = cpu->cr;
		return;
	case 144:
		field = (instruction >> 12) & 0xff;
		mask = 0;
		for (index = 0; index < 8; index++)
			if (field & (0x80u >> index))
				mask |= 0xf0000000u >> (index * 4);
		cpu->cr = (cpu->cr & ~mask) | ((uint32)rs & mask);
		return;
	case 512:
		SetCrField(cpu, (int)((instruction >> 23) & 7), cpu->xer >> 28);
		cpu->xer &= 0x0fffffffu;
		return;
	case 83:
		if (Supervisor(core))
			core->gpr[rt] = core->msr;
		return;
	case 146:
		if (!Supervisor(core))
			return;
		if (instruction & 0x00010000)
			WriteMsr(core, (core->msr & ~(uint64)(OSXMSREE | OSXMSRRI)) | (rs & (OSXMSREE | OSXMSRRI)));
		else
			WriteMsr(core, (core->msr & 0xffffffff00000000ULL) | (rs & 0xffffffffULL));
		return;
	case 178:
		if (!Supervisor(core))
			return;
		if (instruction & 0x00010000)
			WriteMsr(core, (core->msr & ~(uint64)(OSXMSREE | OSXMSRRI)) | (rs & (OSXMSREE | OSXMSRRI)));
		else
			WriteMsr(core, (rs & ~OSX970MSRHV) | (core->msr & OSX970MSRHV));
		return;
	case 339:
		MoveFromSpr(core, instruction);
		return;
	case 467:
		MoveToSpr(core, instruction);
		return;
	case 371:
		if (SprNumber(instruction) == 269)
			core->gpr[rt] = Timebase(core) >> 32;
		else
			core->gpr[rt] = Timebase(core);
		return;
	case 210: case 242:
		if (!Supervisor(core))
			return;
		if (extended == 210)
			field = (instruction >> 16) & 15;
		else
			field = (uint32)(rb >> 28) & 15;
		core->slb[field].esid = ((uint64)field << 28) | OSX970SLBVALID;
		core->slb[field].vsid = ((rs & 0x0fffffffULL) << 12) | (((rs >> 27) & 0xf) << 8);
		Osx970FlushTlb(core);
		return;
	case 595: case 659:
		if (!Supervisor(core))
			return;
		if (extended == 595)
			field = (instruction >> 16) & 15;
		else
			field = (uint32)(rb >> 28) & 15;
		core->gpr[rt] = 0;
		for (index = 0; index < OSX970SLBSIZE; index++)
			if (core->slb[index].esid == (((uint64)field << 28) | OSX970SLBVALID))
				core->gpr[rt] = ((core->slb[index].vsid >> 12) & 0x0fffffffULL) | (((core->slb[index].vsid >> 8) & 0xf) << 27);
		return;
	case 402:
		if (!Supervisor(core))
			return;
		index = (int)(rb & (OSX970SLBSIZE - 1));
		core->slb[index].esid = rb & (0xfffffffff0000000ULL | OSX970SLBVALID);
		core->slb[index].vsid = rs & 0x3ffffffffffffff0ULL;
		Osx970FlushTlb(core);
		return;
	case 434:
		if (!Supervisor(core))
			return;
		slbvalue = (rb & 0xfffffffff0000000ULL) | OSX970SLBVALID;
		for (index = 0; index < OSX970SLBSIZE; index++)
			if (core->slb[index].esid == slbvalue)
				core->slb[index].esid &= ~OSX970SLBVALID;
		Osx970FlushTlb(core);
		return;
	case 498:
		if (!Supervisor(core))
			return;
		for (index = 1; index < OSX970SLBSIZE; index++)
			core->slb[index].esid &= ~OSX970SLBVALID;
		Osx970FlushTlb(core);
		return;
	case 915:
		if (!Supervisor(core))
			return;
		index = (int)(rb & (OSX970SLBSIZE - 1));
		core->gpr[rt] = core->slb[index].esid;
		return;
	case 851:
		if (!Supervisor(core))
			return;
		index = (int)(rb & (OSX970SLBSIZE - 1));
		core->gpr[rt] = core->slb[index].vsid;
		return;
	case 274: case 306: case 370:
		if (!Supervisor(core))
			return;
		Osx970FlushTlb(core);
		return;
	case 566: case 598: case 854:
		return;
	case 54: case 86: case 246: case 278: case 470:
		return;
	case 982:
		core->fetchpage = OSX970TLBINVALID;
		if (Osx970Translate(core, IndexedAddress(core, instruction), OSXTLBREAD, 0, &value))
			Osx970InvalidatePhysical(core, value & ~127ULL, 128);
		return;
	case 1014:
		if (instruction & 0x00200000)
			ZeroBlock(core, IndexedAddress(core, instruction), 128);
		else if (((core->hid5 >> 7) & 3) == 1)
			ZeroBlock(core, IndexedAddress(core, instruction), 32);
		else
			ZeroBlock(core, IndexedAddress(core, instruction), 128);
		return;
	case 20: case 84:
		address = IndexedAddress(core, instruction);
		count = 4;
		if (extended == 84)
			count = 8;
		if (address & (count - 1)) {
			core->dar = address;
			core->dsisr = 0;
			Osx970Raise(core, OSXVECTORALIGNMENT, 0);
			return;
		}
		if (!Osx970Read(core, address, (int)count, &value))
			return;
		core->gpr[rt] = value;
		core->reservation = address;
		core->reservationvalid = 1;
		return;
	case 150: case 214:
		address = IndexedAddress(core, instruction);
		count = 4;
		if (extended == 214)
			count = 8;
		if (address & (count - 1)) {
			core->dar = address;
			core->dsisr = 0;
			Osx970Raise(core, OSXVECTORALIGNMENT, 0);
			return;
		}
		if (core->reservationvalid && core->reservation == address) {
			if (!Osx970Write(core, address, (int)count, rs))
				return;
			core->reservationvalid = 0;
			SetCrField(cpu, 0, 2 | (cpu->xer >> 31));
			return;
		}
		core->reservationvalid = 0;
		SetCrField(cpu, 0, cpu->xer >> 31);
		return;
	case 24:
		if (rb & 0x20)
			core->gpr[ra] = 0;
		else
			core->gpr[ra] = (uint32)((uint32)rs << (rb & 31));
		break;
	case 536:
		if (rb & 0x20)
			core->gpr[ra] = 0;
		else
			core->gpr[ra] = (uint32)rs >> (rb & 31);
		break;
	case 27:
		if (rb & 0x40)
			core->gpr[ra] = 0;
		else
			core->gpr[ra] = rs << (rb & 63);
		break;
	case 539:
		if (rb & 0x40)
			core->gpr[ra] = 0;
		else
			core->gpr[ra] = rs >> (rb & 63);
		break;
	case 792:
		ShiftRightAlgebraic(core, ra, rs, (uint32)(rb & 0x3f), 0);
		break;
	case 824:
		ShiftRightAlgebraic(core, ra, rs, (uint32)rbnum, 0);
		break;
	case 794:
		ShiftRightAlgebraic(core, ra, rs, (uint32)(rb & 0x7f), 1);
		break;
	case 826: case 827:
		ShiftRightAlgebraic(core, ra, rs, (uint32)rbnum | ((extended & 1) << 5), 1);
		break;
	case 26:
		core->gpr[ra] = (uint64)(CountLeadingZeros64(rs & 0xffffffffULL) - 32);
		break;
	case 58:
		core->gpr[ra] = (uint64)CountLeadingZeros64(rs);
		break;
	case 28: core->gpr[ra] = rs & rb; break;
	case 60: core->gpr[ra] = rs & ~rb; break;
	case 124: core->gpr[ra] = ~(rs | rb); break;
	case 284: core->gpr[ra] = ~(rs ^ rb); break;
	case 316: core->gpr[ra] = rs ^ rb; break;
	case 412: core->gpr[ra] = rs | ~rb; break;
	case 444: core->gpr[ra] = rs | rb; break;
	case 476: core->gpr[ra] = ~(rs & rb); break;
	case 922: core->gpr[ra] = (uint64)(int64)(int16)rs; break;
	case 954: core->gpr[ra] = (uint64)(int64)(int8)rs; break;
	case 986: core->gpr[ra] = SignExtend32(rs); break;
	case 533: case 597: case 661: case 725:
		if (extended == 533 || extended == 661) {
			address = IndexedAddress(core, instruction);
			count = cpu->xer & 0x7f;
		} else {
			address = 0;
			if (ra)
				address = core->gpr[ra];
			address = Effective(core, address);
			count = (uint32)rbnum;
			if (count == 0)
				count = 32;
		}
		LoadStoreString(core, instruction, address, count, extended == 661 || extended == 725);
		return;
	case 534: case 790: case 662: case 918:
		address = IndexedAddress(core, instruction);
		count = 4;
		if (extended == 790 || extended == 918)
			count = 2;
		if (extended == 534 || extended == 790) {
			if (!Osx970Read(core, address, (int)count, &value))
				return;
			if (count == 4)
				core->gpr[rt] = ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value >> 8) & 0xff00) | ((value >> 24) & 0xff);
			else
				core->gpr[rt] = ((value & 0xff) << 8) | ((value >> 8) & 0xff);
			return;
		}
		if (count == 4)
			value = ((rs & 0xff) << 24) | ((rs & 0xff00) << 8) | ((rs >> 8) & 0xff00) | ((rs >> 24) & 0xff);
		else
			value = ((rs & 0xff) << 8) | ((rs >> 8) & 0xff);
		Osx970Write(core, address, (int)count, value);
		return;
	case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759: case 983:
		address = IndexedAddress(core, instruction);
		if (extended == 535 || extended == 567)
			field = 48;
		else if (extended == 599 || extended == 631)
			field = 50;
		else if (extended == 663 || extended == 695)
			field = 52;
		else if (extended == 983)
			field = 983;
		else
			field = 54;
		if (ExecuteFloatMemory(core, instruction, field, address) && (extended & 32))
			UpdateBase(core, instruction, address);
		return;
	default:
		address = IndexedAddress(core, instruction);
		switch (extended) {
		case 21: case 53: count = 8; field = 0; break;
		case 23: case 55: count = 4; field = 0; break;
		case 87: case 119: count = 1; field = 0; break;
		case 279: case 311: count = 2; field = 0; break;
		case 343: case 375: count = 2; field = 1; break;
		case 341: case 373: count = 4; field = 1; break;
		case 149: case 181: count = 8; field = 2; break;
		case 151: case 183: count = 4; field = 2; break;
		case 215: case 247: count = 1; field = 2; break;
		case 407: case 439: count = 2; field = 2; break;
		default:
			Illegal(core);
			return;
		}
		if (field == 2) {
			if (!Osx970Write(core, address, (int)count, rs))
				return;
		} else {
			if (!Osx970Read(core, address, (int)count, &value))
				return;
			if (field == 1 && count == 2)
				value = (uint64)(int64)(int16)value;
			if (field == 1 && count == 4)
				value = SignExtend32(value);
			core->gpr[rt] = value;
		}
		if (extended & 32)
			UpdateBase(core, instruction, address);
		return;
	}
	if (instruction & 1)
		SetCr0(core, core->gpr[ra]);
}

static void ExecuteFloat64(Osx970 *core, uint32 instruction)
{
	OsxCpu *cpu;
	uint32 extended;
	int64 integer;
	double value;
	int target;
	cpu = core->cpu;
	extended = (instruction >> 1) & 0x3ff;
	target = (instruction >> 21) & 31;
	value = cpu->fpr[(instruction >> 11) & 31].value;
	if (extended == 846) {
		cpu->fpr[target].value = (double)(int64)cpu->fpr[(instruction >> 11) & 31].bits;
	} else {
		if (value != value)
			integer = (int64)0x8000000000000000ULL;
		else if (value >= 9223372036854775807.0)
			integer = (int64)0x7fffffffffffffffULL;
		else if (value <= -9223372036854775808.0)
			integer = (int64)0x8000000000000000ULL;
		else if (extended == 815)
			integer = (int64)value;
		else
			integer = (int64)nearbyint(value);
		cpu->fpr[target].bits = (uint64)integer;
	}
	if (instruction & 1)
		cpu->cr = (cpu->cr & 0xf0ffffffu) | ((cpu->fpscr >> 4) & 0x0f000000u);
}

static void Execute(Osx970 *core, uint32 instruction);

static void Mirror(Osx970 *core)
{
	OsxCpu *cpu;
	int index;
	cpu = core->cpu;
	for (index = 0; index < 32; index++)
		cpu->gpr[index] = (uint32)core->gpr[index];
	cpu->pc = (uint32)core->pc;
	cpu->nextpc = (uint32)core->nextpc;
	cpu->lr = (uint32)core->lr;
	cpu->ctr = (uint32)core->ctr;
	cpu->msr = (uint32)core->msr;
	cpu->srr0 = (uint32)core->srr0;
	cpu->srr1 = (uint32)core->srr1;
	cpu->dar = (uint32)core->dar;
	cpu->dsisr = core->dsisr;
	cpu->sdr1 = (uint32)core->sdr1;
	for (index = 0; index < 4; index++)
		cpu->sprg[index] = (uint32)core->sprg[index];
	for (index = 0; index < 16; index++)
		cpu->sr[index] = (uint32)(core->slb[index].vsid >> 12);
}

static void Absorb(Osx970 *core)
{
	OsxCpu *cpu;
	int index;
	cpu = core->cpu;
	for (index = 0; index < 32; index++)
		if (cpu->gpr[index] != (uint32)core->gpr[index])
			core->gpr[index] = cpu->gpr[index];
	if (cpu->nextpc != (uint32)core->nextpc)
		core->nextpc = cpu->nextpc;
	if (cpu->lr != (uint32)core->lr)
		core->lr = cpu->lr;
	if (cpu->ctr != (uint32)core->ctr)
		core->ctr = cpu->ctr;
	if (cpu->msr != (uint32)core->msr) {
		core->msr = (core->msr & 0xffffffff00000000ULL) | cpu->msr;
		UpdateModes(core);
	}
}

int Osx970TranslateVirtual(Osx970 *core, uint32 address, int kind, uint32 *physical)
{
	uint64 result;
	uint64 msr;
	int translated;
	msr = core->msr;
	core->msr |= OSXMSRDR | OSXMSRIR;
	core->msr &= ~(uint64)OSXMSRPR;
	if (!(msr & OSX970MSRSF))
		core->msr &= ~OSX970MSRSF;
	translated = Osx970Translate(core, address, kind, 0, &result);
	core->msr = msr;
	if (!translated) {
		if (address >= core->cpu->ramsize)
			return 0;
		result = address;
	}
	if (result > 0xffffffffULL)
		return 0;
	*physical = (uint32)result;
	return 1;
}

static void ExecuteHook(Osx970 *core, uint32 instruction)
{
	OsxCpu *cpu;
	OsxHook *hook;
	uint32 index;
	cpu = core->cpu;
	index = instruction & 0x03ffffff;
	if ((core->msr & OSXMSRPR) || index >= (uint32)cpu->hookcount) {
		Illegal(core);
		return;
	}
	hook = &cpu->hooks[index];
	Mirror(core);
	cpu->nextpc = cpu->pc + 4;
	hook->function(cpu, hook->context, hook->address);
	Absorb(core);
	if (core->nextpc == Effective(core, core->pc + 4))
		Execute(core, hook->original);
}

static void ExecuteShared(Osx970 *core, uint32 instruction, int vector)
{
	OsxCpu *cpu;
	uint64 exceptions;
	cpu = core->cpu;
	exceptions = cpu->exceptions;
	cpu->pc = (uint32)core->pc;
	cpu->msr = (uint32)core->msr;
	if (vector)
		OsxCpuExecuteVector(cpu, instruction);
	else
		OsxCpuExecuteFloat(cpu, instruction);
	if (cpu->exceptions != exceptions) {
		cpu->exceptions = exceptions;
		Illegal(core);
	}
}

static void Execute(Osx970 *core, uint32 instruction)
{
	uint32 opcode;
	uint32 immediate;
	uint32 extended;
	uint64 simmediate;
	uint64 address;
	uint64 value;
	uint64 target;
	int rt;
	int ra;
	int index;
	opcode = instruction >> 26;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	immediate = instruction & 0xffff;
	simmediate = (uint64)(int64)(int16)immediate;
	switch (opcode) {
	case 2:
		Trap(core, (uint32)rt, core->gpr[ra], simmediate, 1);
		return;
	case 3:
		Trap(core, (uint32)rt, core->gpr[ra], simmediate, 0);
		return;
	case 4:
		if (VectorEnabled(core))
			ExecuteShared(core, instruction, 1);
		return;
	case 7:
		core->gpr[rt] = (uint64)((int64)core->gpr[ra] * (int64)simmediate);
		return;
	case 8:
		core->gpr[rt] = AddExtended(core, ~core->gpr[ra], simmediate, 1, 1, 0);
		return;
	case 10:
		if (instruction & 0x00200000)
			CompareUnsigned(core, rt >> 2, core->gpr[ra], immediate);
		else
			CompareUnsigned(core, rt >> 2, core->gpr[ra] & 0xffffffffULL, immediate);
		return;
	case 11:
		if (instruction & 0x00200000)
			CompareSigned(core, rt >> 2, (int64)core->gpr[ra], (int64)simmediate);
		else
			CompareSigned(core, rt >> 2, (int64)(int32)(uint32)core->gpr[ra], (int64)simmediate);
		return;
	case 12: case 13:
		core->gpr[rt] = AddExtended(core, core->gpr[ra], simmediate, 0, 1, 0);
		if (opcode == 13)
			SetCr0(core, core->gpr[rt]);
		return;
	case 14: case 15:
		if (opcode == 15)
			simmediate <<= 16;
		if (ra)
			core->gpr[rt] = core->gpr[ra] + simmediate;
		else
			core->gpr[rt] = simmediate;
		return;
	case 16:
		target = (uint64)(int64)(int16)(immediate & 0xfffc);
		if (!(instruction & 2))
			target += core->pc;
		if (BranchTaken(core, (uint32)rt, (uint32)ra, 1))
			core->nextpc = Effective(core, target);
		if (instruction & 1)
			core->lr = Effective(core, core->pc + 4);
		return;
	case 6:
		ExecuteHook(core, instruction);
		return;
	case 17:
		if (core->cpu->syscallobserver) {
			Mirror(core);
			core->cpu->syscallobserver(core->cpu, core->cpu->syscallcontext);
		}
		RaiseAfter(core, OSXVECTORSYSCALL);
		return;
	case 18:
		target = (uint64)(int64)((int32)(instruction << 6) >> 6) & ~3ULL;
		if (!(instruction & 2))
			target += core->pc;
		core->nextpc = Effective(core, target);
		if (instruction & 1)
			core->lr = Effective(core, core->pc + 4);
		return;
	case 19:
		ExecuteGroup19(core, instruction);
		return;
	case 20: case 21: case 23:
		if (opcode == 23)
			value = core->gpr[(instruction >> 11) & 31] & 31;
		else
			value = (instruction >> 11) & 31;
		target = Mask64(((instruction >> 6) & 31) + 32, ((instruction >> 1) & 31) + 32);
		address = RotateLeft64(Duplicate32(core->gpr[rt]), (uint32)value) & target;
		if (opcode == 20)
			address |= core->gpr[ra] & ~target;
		core->gpr[ra] = address;
		if (instruction & 1)
			SetCr0(core, address);
		return;
	case 24: core->gpr[ra] = core->gpr[rt] | immediate; return;
	case 25: core->gpr[ra] = core->gpr[rt] | ((uint64)immediate << 16); return;
	case 26: core->gpr[ra] = core->gpr[rt] ^ immediate; return;
	case 27: core->gpr[ra] = core->gpr[rt] ^ ((uint64)immediate << 16); return;
	case 28:
		core->gpr[ra] = core->gpr[rt] & immediate;
		SetCr0(core, core->gpr[ra]);
		return;
	case 29:
		core->gpr[ra] = core->gpr[rt] & ((uint64)immediate << 16);
		SetCr0(core, core->gpr[ra]);
		return;
	case 30:
		ExecuteRotate(core, instruction);
		return;
	case 31:
		ExecuteGroup31(core, instruction);
		return;
	case 59: case 63:
		if (!FloatEnabled(core))
			return;
		extended = (instruction >> 1) & 0x3ff;
		if (opcode == 63 && (extended == 814 || extended == 815 || extended == 846))
			ExecuteFloat64(core, instruction);
		else
			ExecuteShared(core, instruction, 0);
		return;
	}
	address = simmediate;
	if (opcode == 58 || opcode == 62)
		address &= ~3ULL;
	if (ra)
		address += core->gpr[ra];
	address = Effective(core, address);
	switch (opcode) {
	case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43:
		if (opcode < 34)
			index = 4;
		else if (opcode < 36)
			index = 1;
		else
			index = 2;
		if (!Osx970Read(core, address, index, &value))
			return;
		if (opcode >= 42)
			value = (uint64)(int64)(int16)value;
		core->gpr[rt] = value;
		if (opcode & 1)
			UpdateBase(core, instruction, address);
		return;
	case 36: case 37: case 38: case 39: case 44: case 45:
		if (opcode < 38)
			index = 4;
		else if (opcode < 40)
			index = 1;
		else
			index = 2;
		if (!Osx970Write(core, address, index, core->gpr[rt]))
			return;
		if (opcode & 1)
			UpdateBase(core, instruction, address);
		return;
	case 46:
		for (index = rt; index < 32; index++) {
			if (!Osx970Read(core, address, 4, &value))
				return;
			core->gpr[index] = value;
			address = Effective(core, address + 4);
		}
		return;
	case 47:
		for (index = rt; index < 32; index++) {
			if (!Osx970Write(core, address, 4, core->gpr[index]))
				return;
			address = Effective(core, address + 4);
		}
		return;
	case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
		if (ExecuteFloatMemory(core, instruction, opcode & ~1u, address) && (opcode & 1))
			UpdateBase(core, instruction, address);
		return;
	case 58:
		if ((instruction & 3) == 3) {
			Illegal(core);
			return;
		}
		if ((instruction & 3) == 2) {
			if (!Osx970Read(core, address, 4, &value))
				return;
			core->gpr[rt] = SignExtend32(value);
			return;
		}
		if (!Osx970Read(core, address, 8, &value))
			return;
		core->gpr[rt] = value;
		if (instruction & 1)
			UpdateBase(core, instruction, address);
		return;
	case 62:
		if ((instruction & 3) > 1) {
			Illegal(core);
			return;
		}
		if (!Osx970Write(core, address, 8, core->gpr[rt]))
			return;
		if (instruction & 1)
			UpdateBase(core, instruction, address);
		return;
	}
	Illegal(core);
}

int Osx970Step(Osx970 *core, uint32 instruction)
{
	uint64 msr;
	uint64 expected;
	msr = core->msr;
	expected = Effective(core, core->pc + 4);
	core->nextpc = expected;
	Execute(core, instruction);
	core->pc = core->nextpc;
	if (core->nextpc != expected || core->msr != msr || core->cpu->halted)
		return 1;
	return 0;
}

void Osx970ExecuteInstruction(Osx970 *core, uint32 instruction)
{
	uint64 exceptions;
	uint64 msr;
	exceptions = core->cpu->exceptions;
	msr = core->msr;
	core->nextpc = Effective(core, core->pc + 4);
	Execute(core, instruction);
	if ((msr & OSXMSRSE) && core->cpu->exceptions == exceptions) {
		core->pc = core->nextpc;
		Osx970Raise(core, OSXVECTORTRACE, 0);
	}
	core->pc = core->nextpc;
	core->cpu->instructions++;
}

void Osx970CheckInterrupts(Osx970 *core, uint64 now)
{
	OsxCpu *cpu;
	cpu = core->cpu;
	if (!cpu->decpending && cpu->decdeadline != OSXNEVER && now >= cpu->decdeadline) {
		cpu->decpending = 1;
		cpu->decdeadline = OSXNEVER;
	}
	if (cpu->halted && (cpu->decpending || cpu->externalline) && (core->msr & OSXMSRPOW)) {
		cpu->halted = 0;
		core->msr &= ~(uint64)OSXMSRPOW;
	}
	if (!(core->msr & OSXMSREE))
		return;
	if (cpu->externalline) {
		cpu->externalcount++;
		Osx970Raise(core, OSXVECTOREXTERNAL, 0);
		core->pc = core->nextpc;
		return;
	}
	if (cpu->decpending) {
		cpu->decpending = 0;
		cpu->decrementercount++;
		Osx970Raise(core, OSXVECTORDECREMENTER, 0);
		core->pc = core->nextpc;
	}
}

#if OSX970JIT
#define OSX970JITBLOCKS 65536
#define OSX970JITBYTES (16 * 1024 * 1024)
#define OSX970JITRESERVE 32768
#define OSX970JITLIMIT 64
#define OSX970JITHOT 3
#define OSX970JITCACHE 4096
#define OSX970JITEXITS 256
#define OSX970RAX 0
#define OSX970RCX 1
#define OSX970RDX 2
#define OSX970RBX 3
#define OSX970RSI 6
#define OSX970R8 8
#define OSX970R9 9
#define OSX970R10 10
#define OSX970GPR(reg) ((uint32)(offsetof(Osx970, gpr) + (uint32)(reg) * 8))
#define OSX970FIELD(member) ((uint32)offsetof(Osx970, member))
#define OSX970CPUFIELD(member) ((uint32)offsetof(OsxCpu, member))

typedef int (*Osx970Code)(Osx970 *core);

typedef struct Osx970Block {
	uint64 pc;
	uint32 physical;
	uint32 key;
	uint32 version;
	uint32 count;
	Osx970Code code;
	uint8 *body;
} Osx970Block;

typedef struct Osx970Cache {
	uint64 pc;
	uint32 key;
	uint32 pad;
	uint8 *host;
	uint8 *body;
} Osx970Cache;

typedef struct Osx970Jit {
	Osx970Cache cache[OSX970JITCACHE];
	uint8 *arena;
	uint8 *stepthunk;
	uint32 used;
	uint64 compiled;
	Osx970Block blocks[OSX970JITBLOCKS];
} Osx970Jit;

typedef struct Osx970Cold {
	uint8 *jump;
	uint8 *resume;
	uint64 pc;
	uint32 instruction;
	uint32 count;
} Osx970Cold;

typedef struct Osx970Emitter {
	uint8 *cursor;
	uint8 *stepthunk;
	uint8 *exits[OSX970JITEXITS];
	int exitcount;
	Osx970Cold colds[OSX970JITEXITS];
	int coldcount;
	uint8 *links[OSX970JITEXITS];
	int linkcount;
	uint64 page;
	uint32 key;
	int fetchmode;
	int vector;
} Osx970Emitter;

static void EmitByte(Osx970Emitter *emitter, uint32 value)
{
	*emitter->cursor++ = (uint8)value;
}

static void EmitDword(Osx970Emitter *emitter, uint32 value)
{
	memcpy(emitter->cursor, &value, 4);
	emitter->cursor += 4;
}

static void EmitQword(Osx970Emitter *emitter, uint64 value)
{
	memcpy(emitter->cursor, &value, 8);
	emitter->cursor += 8;
}

static void EmitRex(Osx970Emitter *emitter, int wide, int reg, int index, int base)
{
	uint32 value;
	value = 0x40;
	if (wide)
		value |= 8;
	if (reg & 8)
		value |= 4;
	if (index & 8)
		value |= 2;
	if (base & 8)
		value |= 1;
	if (value != 0x40)
		EmitByte(emitter, value);
}

static void EmitOpcode(Osx970Emitter *emitter, uint32 opcode)
{
	if (opcode > 0xff)
		EmitByte(emitter, opcode >> 8);
	EmitByte(emitter, opcode & 0xff);
}

static void EmitMemory(Osx970Emitter *emitter, int wide, uint32 opcode, int reg, int base, uint32 displacement)
{
	int small;
	small = (int32)displacement >= -128 && (int32)displacement < 128;
	EmitRex(emitter, wide, reg, 0, base);
	EmitOpcode(emitter, opcode);
	if (small)
		EmitByte(emitter, 0x40 | ((reg & 7) << 3) | (base & 7));
	else
		EmitByte(emitter, 0x80 | ((reg & 7) << 3) | (base & 7));
	if ((base & 7) == 4)
		EmitByte(emitter, 0x24);
	if (small)
		EmitByte(emitter, displacement & 0xff);
	else
		EmitDword(emitter, displacement);
}

static void EmitRegister(Osx970Emitter *emitter, int wide, uint32 opcode, int reg, int rm)
{
	EmitRex(emitter, wide, reg, 0, rm);
	EmitOpcode(emitter, opcode);
	EmitByte(emitter, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

static void EmitIndexed(Osx970Emitter *emitter, int wide, uint32 opcode, int reg)
{
	EmitRex(emitter, wide, reg, OSX970RAX, OSX970RDX);
	EmitOpcode(emitter, opcode);
	EmitByte(emitter, 0x04 | ((reg & 7) << 3));
	EmitByte(emitter, (OSX970RAX << 3) | OSX970RDX);
}

static void EmitImmediate(Osx970Emitter *emitter, int reg, uint64 value)
{
	if ((uint64)(int64)(int32)(uint32)value == value) {
		EmitRex(emitter, 1, 0, 0, reg);
		EmitByte(emitter, 0xc7);
		EmitByte(emitter, 0xc0 | (reg & 7));
		EmitDword(emitter, (uint32)value);
		return;
	}
	EmitRex(emitter, 1, 0, 0, reg);
	EmitByte(emitter, 0xb8 | (reg & 7));
	EmitQword(emitter, value);
}

static void EmitImmediate32(Osx970Emitter *emitter, int reg, uint32 value)
{
	EmitRex(emitter, 0, 0, 0, reg);
	EmitByte(emitter, 0xb8 | (reg & 7));
	EmitDword(emitter, value);
}

static void EmitLoadGpr(Osx970Emitter *emitter, int reg, int gpr)
{
	EmitMemory(emitter, 1, 0x8b, reg, OSX970RBX, OSX970GPR(gpr));
}

static void EmitStoreGpr(Osx970Emitter *emitter, int reg, int gpr)
{
	EmitMemory(emitter, 1, 0x89, reg, OSX970RBX, OSX970GPR(gpr));
}

static uint8 *EmitJump(Osx970Emitter *emitter, uint32 condition)
{
	if (condition) {
		EmitByte(emitter, 0x0f);
		EmitByte(emitter, condition);
	} else
		EmitByte(emitter, 0xe9);
	EmitDword(emitter, 0);
	return emitter->cursor - 4;
}

static void PatchJump(uint8 *field, uint8 *target)
{
	int32 distance;
	distance = (int32)(target - (field + 4));
	memcpy(field, &distance, 4);
}

static void EmitAccount(Osx970Emitter *emitter, uint32 count)
{
	EmitMemory(emitter, 1, 0x83, 0, OSX970RSI, OSX970CPUFIELD(instructions));
	EmitByte(emitter, count);
	EmitMemory(emitter, 0, 0x83, 5, OSX970RSI, OSX970CPUFIELD(slice));
	EmitByte(emitter, count);
}

static void EmitExit(Osx970Emitter *emitter, uint32 count)
{
	EmitAccount(emitter, count);
	if (emitter->exitcount >= OSX970JITEXITS)
		return;
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0);
}

static void EmitSetPc(Osx970Emitter *emitter, uint64 pc);

static void EmitDynamicLookup(Osx970Emitter *emitter)
{
	uint32 tlb;
	if (emitter->exitcount >= OSX970JITEXITS - 8)
		return;
	tlb = OSX970FIELD(tlb) + (uint32)((emitter->fetchmode * OSXTLBKINDS + OSXTLBFETCH) * OSX970TLBSIZE) * (uint32)sizeof(Osx970TlbEntry);
	EmitRegister(emitter, 0, 0x89, OSX970RDX, OSX970RAX);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe8);
	EmitByte(emitter, 2);
	EmitByte(emitter, 0x25);
	EmitDword(emitter, OSX970JITCACHE - 1);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe0);
	EmitByte(emitter, 5);
	EmitMemory(emitter, 1, 0x8b, OSX970RCX, OSX970RBX, OSX970FIELD(jitcache));
	EmitRegister(emitter, 1, 0x01, OSX970RAX, OSX970RCX);
	EmitByte(emitter, 0x48);
	EmitByte(emitter, 0x39);
	EmitByte(emitter, 0x11);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x85);
	EmitByte(emitter, 0x81);
	EmitByte(emitter, 0x79);
	EmitByte(emitter, 0x08);
	EmitDword(emitter, emitter->key);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x85);
	EmitRegister(emitter, 1, 0x89, OSX970RDX, OSX970RAX);
	EmitByte(emitter, 0x48);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe8);
	EmitByte(emitter, 12);
	EmitByte(emitter, 0x25);
	EmitDword(emitter, OSX970TLBMASK);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe0);
	EmitByte(emitter, 4);
	EmitRegister(emitter, 1, 0x89, OSX970RDX, OSX970R8);
	EmitByte(emitter, 0x49);
	EmitByte(emitter, 0x81);
	EmitByte(emitter, 0xe0);
	EmitDword(emitter, 0xfffff000u);
	EmitByte(emitter, 0x4c);
	EmitByte(emitter, 0x39);
	EmitByte(emitter, 0x84);
	EmitByte(emitter, 0x03);
	EmitDword(emitter, tlb);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x85);
	EmitByte(emitter, 0x4c);
	EmitByte(emitter, 0x8b);
	EmitByte(emitter, 0x84);
	EmitByte(emitter, 0x03);
	EmitDword(emitter, tlb + 8);
	EmitByte(emitter, 0x4c);
	EmitByte(emitter, 0x3b);
	EmitByte(emitter, 0x41);
	EmitByte(emitter, 0x10);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x85);
	EmitByte(emitter, 0xff);
	EmitByte(emitter, 0x61);
	EmitByte(emitter, 0x18);
}

static void EmitChain(Osx970Emitter *emitter, uint32 count, uint64 target)
{
	EmitAccount(emitter, count);
	EmitSetPc(emitter, target);
	if (emitter->exitcount >= OSX970JITEXITS - 8 || emitter->linkcount >= OSX970JITEXITS)
		return;
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x8e);
	if ((target & ~0xfffULL) == emitter->page) {
		emitter->links[emitter->linkcount++] = EmitJump(emitter, 0);
		return;
	}
	EmitImmediate(emitter, OSX970RDX, target);
	EmitDynamicLookup(emitter);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0);
}

static void EmitDynamicExit(Osx970Emitter *emitter, uint32 count)
{
	EmitAccount(emitter, count);
	if (emitter->exitcount >= OSX970JITEXITS - 8)
		return;
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0x8e);
	EmitDynamicLookup(emitter);
	emitter->exits[emitter->exitcount++] = EmitJump(emitter, 0);
}

static void EmitColdExit(Osx970Emitter *emitter, uint32 condition, uint32 count)
{
	if (emitter->coldcount >= OSX970JITEXITS)
		return;
	emitter->colds[emitter->coldcount].jump = EmitJump(emitter, condition);
	emitter->colds[emitter->coldcount].resume = NULL;
	emitter->colds[emitter->coldcount].count = count;
	emitter->coldcount++;
}

static void EmitColdStep(Osx970Emitter *emitter, uint32 condition, uint64 pc, uint32 instruction, uint32 count)
{
	if (emitter->coldcount >= OSX970JITEXITS)
		return;
	emitter->colds[emitter->coldcount].jump = EmitJump(emitter, condition);
	emitter->colds[emitter->coldcount].resume = NULL;
	emitter->colds[emitter->coldcount].pc = pc;
	emitter->colds[emitter->coldcount].instruction = instruction;
	emitter->colds[emitter->coldcount].count = count;
	emitter->coldcount++;
}

static void EmitSetPc(Osx970Emitter *emitter, uint64 pc)
{
	if (pc < 0x80000000ULL) {
		EmitMemory(emitter, 1, 0xc7, 0, OSX970RBX, OSX970FIELD(pc));
		EmitDword(emitter, (uint32)pc);
		return;
	}
	if (pc <= 0xffffffffULL)
		EmitImmediate32(emitter, OSX970RAX, (uint32)pc);
	else
		EmitImmediate(emitter, OSX970RAX, pc);
	EmitMemory(emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(pc));
}

static void EmitCallStep(Osx970Emitter *emitter, uint64 pc, uint32 instruction)
{
	if (pc <= 0xffffffffULL)
		EmitImmediate32(emitter, OSX970R8, (uint32)pc);
	else
		EmitImmediate(emitter, OSX970R8, pc);
	EmitImmediate32(emitter, OSX970RDX, instruction);
	EmitByte(emitter, 0xe8);
	EmitDword(emitter, 0);
	PatchJump(emitter->cursor - 4, emitter->stepthunk);
	EmitRegister(emitter, 0, 0x85, OSX970RAX, OSX970RAX);
}

static void EmitStep(Osx970Emitter *emitter, uint64 pc, uint32 instruction, uint32 count)
{
	EmitCallStep(emitter, pc, instruction);
	EmitColdExit(emitter, 0x85, count);
}

static void EmitCrFromFlags(Osx970Emitter *emitter, int field, int issigned)
{
	uint32 shift;
	shift = (uint32)(7 - field) * 4;
	EmitImmediate32(emitter, OSX970RDX, 4);
	EmitImmediate32(emitter, OSX970RCX, 8);
	if (issigned)
		EmitRegister(emitter, 0, 0x0f4c, OSX970RDX, OSX970RCX);
	else
		EmitRegister(emitter, 0, 0x0f42, OSX970RDX, OSX970RCX);
	EmitImmediate32(emitter, OSX970RCX, 2);
	EmitRegister(emitter, 0, 0x0f44, OSX970RDX, OSX970RCX);
	EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RSI, OSX970CPUFIELD(xer));
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe9);
	EmitByte(emitter, 31);
	EmitRegister(emitter, 0, 0x09, OSX970RCX, OSX970RDX);
	EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
	EmitByte(emitter, 0x25);
	EmitDword(emitter, ~(0xfu << shift));
	if (shift) {
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xe2);
		EmitByte(emitter, shift);
	}
	EmitRegister(emitter, 0, 0x09, OSX970RDX, OSX970RAX);
	EmitMemory(emitter, 0, 0x89, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
}

static void EmitRecord(Osx970Emitter *emitter, int sixtyfour)
{
	EmitRex(emitter, sixtyfour, 0, 0, OSX970RAX);
	EmitByte(emitter, 0x83);
	EmitByte(emitter, 0xf8);
	EmitByte(emitter, 0);
	EmitCrFromFlags(emitter, 0, 1);
}

static void EmitNot(Osx970Emitter *emitter, int reg)
{
	EmitRegister(emitter, 1, 0xf7, 2, reg);
}

static void EmitTlbLookup(Osx970Emitter *emitter, int store, int size, int mode, uint64 pc, uint32 instruction, uint32 count)
{
	uint32 tlb;
	EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RDX);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xea);
	EmitByte(emitter, 12);
	EmitByte(emitter, 0x81);
	EmitByte(emitter, 0xe2);
	EmitDword(emitter, OSX970TLBMASK);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe2);
	EmitByte(emitter, 4);
	if (store)
		tlb = OSX970FIELD(tlb) + (uint32)((mode * OSXTLBKINDS + OSXTLBWRITE) * OSX970TLBSIZE) * (uint32)sizeof(Osx970TlbEntry);
	else
		tlb = OSX970FIELD(tlb) + (uint32)((mode * OSXTLBKINDS + OSXTLBREAD) * OSX970TLBSIZE) * (uint32)sizeof(Osx970TlbEntry);
	EmitByte(emitter, 0x4c);
	EmitByte(emitter, 0x8d);
	EmitByte(emitter, 0x40);
	EmitByte(emitter, (uint32)(size - 1));
	EmitByte(emitter, 0x49);
	EmitByte(emitter, 0x81);
	EmitByte(emitter, 0xe0);
	EmitDword(emitter, 0xfffff000u);
	EmitByte(emitter, 0x4c);
	EmitByte(emitter, 0x39);
	EmitByte(emitter, 0x84);
	EmitByte(emitter, 0x13);
	EmitDword(emitter, tlb);
	EmitColdStep(emitter, 0x85, pc, instruction, count);
	EmitByte(emitter, 0x48);
	EmitByte(emitter, 0x8b);
	EmitByte(emitter, 0x94);
	EmitByte(emitter, 0x13);
	EmitDword(emitter, tlb + 8);
}

static void EmitHostOffset(Osx970Emitter *emitter, uint32 opcode, int reg, uint32 displacement)
{
	EmitRex(emitter, 0, reg, OSX970RAX, OSX970RDX);
	EmitOpcode(emitter, opcode);
	EmitByte(emitter, 0x44 | ((reg & 7) << 3));
	EmitByte(emitter, (OSX970RAX << 3) | OSX970RDX);
	EmitByte(emitter, displacement);
}

static void EmitResume(Osx970Emitter *emitter, int first)
{
	int index;
	for (index = first; index < emitter->coldcount; index++)
		emitter->colds[index].resume = emitter->cursor;
}

static void EmitAlignmentCheck(Osx970Emitter *emitter, uint32 mask, uint64 pc, uint32 instruction, uint32 count)
{
	EmitByte(emitter, 0xa8);
	EmitByte(emitter, mask);
	EmitColdStep(emitter, 0x85, pc, instruction, count);
}

static void EmitIndexedAddress(Osx970Emitter *emitter, uint32 instruction, int sixtyfour)
{
	int ra;
	ra = (instruction >> 16) & 31;
	EmitLoadGpr(emitter, OSX970RAX, (instruction >> 11) & 31);
	if (ra)
		EmitMemory(emitter, 1, 0x03, OSX970RAX, OSX970RBX, OSX970GPR(ra));
	if (!sixtyfour)
		EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RAX);
}

static int EmitSpecialMemory(Osx970Emitter *emitter, uint32 instruction, uint64 pc, uint32 count, int sixtyfour, int mode)
{
	uint32 opcode;
	uint32 extended;
	uint32 index;
	uint32 total;
	uint8 *fail;
	uint8 *done;
	uint8 *skip;
	int first;
	int rt;
	int ra;
	opcode = instruction >> 26;
	extended = (instruction >> 1) & 0x3ff;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	first = emitter->coldcount;
	if (emitter->coldcount >= OSX970JITEXITS - 8)
		return 0;
	if (opcode == 46 || opcode == 47) {
		total = (uint32)(32 - rt) * 4;
		if (opcode == 46 && ra >= rt)
			return 0;
		if (ra) {
			EmitLoadGpr(emitter, OSX970RAX, ra);
			EmitByte(emitter, 0x48);
			EmitByte(emitter, 0x05);
			EmitDword(emitter, (uint32)(int32)(int16)(instruction & 0xffff));
		} else
			EmitImmediate(emitter, OSX970RAX, (uint64)(int64)(int16)(instruction & 0xffff));
		if (!sixtyfour)
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RAX);
		EmitAlignmentCheck(emitter, 3, pc, instruction, count);
		EmitTlbLookup(emitter, opcode == 47, (int)total, mode, pc, instruction, count);
		for (index = 0; index < 32 - (uint32)rt; index++) {
			if (opcode == 46) {
				EmitHostOffset(emitter, 0x8b, OSX970RCX, index * 4);
				EmitByte(emitter, 0x0f);
				EmitByte(emitter, 0xc9);
				EmitStoreGpr(emitter, OSX970RCX, rt + (int)index);
			} else {
				EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RBX, OSX970GPR(rt + (int)index));
				EmitByte(emitter, 0x0f);
				EmitByte(emitter, 0xc9);
				EmitHostOffset(emitter, 0x89, OSX970RCX, index * 4);
			}
		}
		EmitResume(emitter, first);
		return 1;
	}
	if (opcode != 31)
		return 0;
	if (extended == 20) {
		EmitIndexedAddress(emitter, instruction, sixtyfour);
		EmitAlignmentCheck(emitter, 3, pc, instruction, count);
		EmitTlbLookup(emitter, 0, 4, mode, pc, instruction, count);
		EmitHostOffset(emitter, 0x8b, OSX970RCX, 0);
		EmitByte(emitter, 0x0f);
		EmitByte(emitter, 0xc9);
		EmitStoreGpr(emitter, OSX970RCX, rt);
		EmitMemory(emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(reservation));
		EmitMemory(emitter, 0, 0xc7, 0, OSX970RBX, OSX970FIELD(reservationvalid));
		EmitDword(emitter, 1);
		EmitResume(emitter, first);
		return 1;
	}
	if (extended == 150 && (instruction & 1)) {
		EmitIndexedAddress(emitter, instruction, sixtyfour);
		EmitAlignmentCheck(emitter, 3, pc, instruction, count);
		EmitMemory(emitter, 0, 0x83, 7, OSX970RBX, OSX970FIELD(reservationvalid));
		EmitByte(emitter, 0);
		fail = EmitJump(emitter, 0x84);
		EmitMemory(emitter, 1, 0x39, OSX970RAX, OSX970RBX, OSX970FIELD(reservation));
		done = EmitJump(emitter, 0x85);
		EmitTlbLookup(emitter, 1, 4, mode, pc, instruction, count);
		EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RBX, OSX970GPR(rt));
		EmitByte(emitter, 0x0f);
		EmitByte(emitter, 0xc9);
		EmitHostOffset(emitter, 0x89, OSX970RCX, 0);
		EmitMemory(emitter, 0, 0xc7, 0, OSX970RBX, OSX970FIELD(reservationvalid));
		EmitDword(emitter, 0);
		EmitImmediate32(emitter, OSX970RDX, 2);
		skip = EmitJump(emitter, 0);
		PatchJump(fail, emitter->cursor);
		PatchJump(done, emitter->cursor);
		EmitMemory(emitter, 0, 0xc7, 0, OSX970RBX, OSX970FIELD(reservationvalid));
		EmitDword(emitter, 0);
		EmitRegister(emitter, 0, 0x31, OSX970RDX, OSX970RDX);
		PatchJump(skip, emitter->cursor);
		EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RSI, OSX970CPUFIELD(xer));
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xe9);
		EmitByte(emitter, 31);
		EmitRegister(emitter, 0, 0x09, OSX970RCX, OSX970RDX);
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
		EmitByte(emitter, 0x25);
		EmitDword(emitter, 0x0fffffffu);
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xe2);
		EmitByte(emitter, 28);
		EmitRegister(emitter, 0, 0x09, OSX970RDX, OSX970RAX);
		EmitMemory(emitter, 0, 0x89, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
		EmitResume(emitter, first);
		return 1;
	}
	if ((extended == 103 || extended == 359 || extended == 231 || extended == 487) && emitter->vector) {
		EmitIndexedAddress(emitter, instruction, sixtyfour);
		EmitByte(emitter, 0x48);
		EmitByte(emitter, 0x83);
		EmitByte(emitter, 0xe0);
		EmitByte(emitter, 0xf0);
		EmitTlbLookup(emitter, extended == 231 || extended == 487, 16, mode, pc, instruction, count);
		for (index = 0; index < 4; index++) {
			if (extended == 103 || extended == 359) {
				EmitHostOffset(emitter, 0x8b, OSX970RCX, index * 4);
				EmitByte(emitter, 0x0f);
				EmitByte(emitter, 0xc9);
				EmitMemory(emitter, 0, 0x89, OSX970RCX, OSX970RSI, OSX970CPUFIELD(vr) + (uint32)rt * 16 + index * 4);
			} else {
				EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RSI, OSX970CPUFIELD(vr) + (uint32)rt * 16 + index * 4);
				EmitByte(emitter, 0x0f);
				EmitByte(emitter, 0xc9);
				EmitHostOffset(emitter, 0x89, OSX970RCX, index * 4);
			}
		}
		EmitResume(emitter, first);
		return 1;
	}
	return 0;
}

static int EmitMemoryAccess(Osx970Emitter *emitter, uint32 instruction, uint64 pc, uint32 count, int sixtyfour, int mode)
{
	uint32 opcode;
	uint32 extended;
	uint32 displacement;
	uint32 tlb;
	int size;
	int store;
	int kind;
	int update;
	int indexed;
	int rt;
	int ra;
	opcode = instruction >> 26;
	extended = (instruction >> 1) & 0x3ff;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	indexed = opcode == 31;
	update = 0;
	store = 0;
	kind = 0;
	size = 4;
	if (indexed) {
		switch (extended) {
		case 23: case 55: size = 4; break;
		case 87: case 119: size = 1; break;
		case 279: case 311: size = 2; break;
		case 343: case 375: size = 2; kind = 1; break;
		case 21: case 53: size = 8; break;
		case 151: case 183: size = 4; store = 1; break;
		case 215: case 247: size = 1; store = 1; break;
		case 407: case 439: size = 2; store = 1; break;
		case 149: case 181: size = 8; store = 1; break;
		default:
			return 0;
		}
		update = (extended & 32) != 0;
	} else {
		switch (opcode) {
		case 32: case 33: size = 4; break;
		case 34: case 35: size = 1; break;
		case 40: case 41: size = 2; break;
		case 42: case 43: size = 2; kind = 1; break;
		case 36: case 37: size = 4; store = 1; break;
		case 38: case 39: size = 1; store = 1; break;
		case 44: case 45: size = 2; store = 1; break;
		case 58:
			if ((instruction & 3) == 3)
				return 0;
			size = 8;
			if ((instruction & 3) == 2) {
				size = 4;
				kind = 2;
			}
			break;
		case 62:
			if ((instruction & 3) > 1)
				return 0;
			size = 8;
			store = 1;
			break;
		default:
			return 0;
		}
		if (opcode == 58 || opcode == 62)
			update = instruction & 1;
		else
			update = opcode & 1;
	}
	if (update && ra == 0)
		return 0;
	if (indexed) {
		EmitLoadGpr(emitter, OSX970RAX, (instruction >> 11) & 31);
		if (ra)
			EmitMemory(emitter, 1, 0x03, OSX970RAX, OSX970RBX, OSX970GPR(ra));
	} else {
		displacement = (uint32)(int32)(int16)(instruction & 0xffff);
		if (opcode == 58 || opcode == 62)
			displacement &= ~3u;
		if (ra) {
			EmitLoadGpr(emitter, OSX970RAX, ra);
			if (displacement) {
				EmitByte(emitter, 0x48);
				EmitByte(emitter, 0x05);
				EmitDword(emitter, displacement);
			}
		} else
			EmitImmediate(emitter, OSX970RAX, (uint64)(int64)(int32)displacement);
	}
	if (!sixtyfour)
		EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RAX);
	if (update)
		EmitRegister(emitter, 1, 0x89, OSX970RAX, OSX970R10);
	EmitTlbLookup(emitter, store, size, mode, pc, instruction, count);
	if (store) {
		EmitMemory(emitter, size == 8, 0x8b, OSX970R9, OSX970RBX, OSX970GPR(rt));
		if (size == 8) {
			EmitByte(emitter, 0x49);
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0xc9);
			EmitIndexed(emitter, 1, 0x89, OSX970R9);
		} else if (size == 4) {
			EmitByte(emitter, 0x41);
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0xc9);
			EmitIndexed(emitter, 0, 0x89, OSX970R9);
		} else if (size == 2) {
			EmitByte(emitter, 0x66);
			EmitByte(emitter, 0x41);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 8);
			EmitByte(emitter, 0x66);
			EmitIndexed(emitter, 0, 0x89, OSX970R9);
		} else
			EmitIndexed(emitter, 0, 0x88, OSX970R9);
	} else {
		if (size == 8) {
			EmitIndexed(emitter, 1, 0x8b, OSX970RAX);
			EmitByte(emitter, 0x48);
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0xc8);
		} else if (size == 4) {
			EmitIndexed(emitter, 0, 0x8b, OSX970RAX);
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0xc8);
			if (kind == 2)
				EmitRegister(emitter, 1, 0x63, OSX970RAX, OSX970RAX);
		} else if (size == 2) {
			EmitIndexed(emitter, 0, 0x0fb7, OSX970RAX);
			EmitByte(emitter, 0x66);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0xc0);
			EmitByte(emitter, 8);
			if (kind == 1)
				EmitRegister(emitter, 1, 0x0fbf, OSX970RAX, OSX970RAX);
			else
				EmitRegister(emitter, 0, 0x0fb7, OSX970RAX, OSX970RAX);
		} else
			EmitIndexed(emitter, 0, 0x0fb6, OSX970RAX);
		EmitStoreGpr(emitter, OSX970RAX, rt);
	}
	if (update)
		EmitMemory(emitter, 1, 0x89, OSX970R10, OSX970RBX, OSX970GPR(ra));
	emitter->colds[emitter->coldcount - 1].resume = emitter->cursor;
	return 1;
}

static int EmitBranch(Osx970Emitter *emitter, Osx970 *core, uint32 instruction, uint64 pc, uint32 count, int sixtyfour)
{
	uint32 opcode;
	uint32 extended;
	uint32 options;
	uint32 bit;
	uint64 target;
	uint8 *skips[2];
	int skipcount;
	int registertarget;
	int decrement;
	int index;
	opcode = instruction >> 26;
	extended = (instruction >> 1) & 0x3ff;
	options = 20;
	bit = 0;
	registertarget = 0;
	target = 0;
	if (opcode == 18) {
		target = (uint64)(int64)((int32)(instruction << 6) >> 6) & ~3ULL;
		if (!(instruction & 2))
			target += pc;
	} else if (opcode == 16) {
		options = (instruction >> 21) & 31;
		bit = (instruction >> 16) & 31;
		target = (uint64)(int64)(int16)(instruction & 0xfffc);
		if (!(instruction & 2))
			target += pc;
	} else if (opcode == 19 && (extended == 16 || extended == 528)) {
		options = (instruction >> 21) & 31;
		bit = (instruction >> 16) & 31;
		registertarget = 1;
		if (extended == 528 && !(options & 4))
			return 0;
	} else
		return 0;
	target = Effective(core, target);
	decrement = (opcode == 16 || extended == 16) && !(options & 4);
	if (registertarget) {
		if (extended == 16)
			EmitMemory(emitter, 1, 0x8b, OSX970RDX, OSX970RBX, OSX970FIELD(lr));
		else
			EmitMemory(emitter, 1, 0x8b, OSX970RDX, OSX970RBX, OSX970FIELD(ctr));
		EmitByte(emitter, 0x48);
		EmitByte(emitter, 0x83);
		EmitByte(emitter, 0xe2);
		EmitByte(emitter, 0xfc);
		if (!sixtyfour)
			EmitRegister(emitter, 0, 0x89, OSX970RDX, OSX970RDX);
	}
	if (instruction & 1) {
		EmitImmediate(emitter, OSX970RAX, Effective(core, pc + 4));
		EmitMemory(emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(lr));
	}
	skipcount = 0;
	if (decrement) {
		EmitMemory(emitter, 1, 0xff, 1, OSX970RBX, OSX970FIELD(ctr));
		EmitMemory(emitter, sixtyfour, 0x83, 7, OSX970RBX, OSX970FIELD(ctr));
		EmitByte(emitter, 0);
		if (options & 2)
			skips[skipcount++] = EmitJump(emitter, 0x85);
		else
			skips[skipcount++] = EmitJump(emitter, 0x84);
	}
	if (!(options & 16)) {
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
		EmitByte(emitter, 0x0f);
		EmitByte(emitter, 0xba);
		EmitByte(emitter, 0xe0);
		EmitByte(emitter, 31 - bit);
		if (options & 8)
			skips[skipcount++] = EmitJump(emitter, 0x83);
		else
			skips[skipcount++] = EmitJump(emitter, 0x82);
	}
	if (registertarget) {
		EmitMemory(emitter, 1, 0x89, OSX970RDX, OSX970RBX, OSX970FIELD(pc));
		EmitDynamicExit(emitter, count);
	} else
		EmitChain(emitter, count, target);
	for (index = 0; index < skipcount; index++)
		PatchJump(skips[index], emitter->cursor);
	return 1;
}

static void EmitCarryFromCl(Osx970Emitter *emitter)
{
	EmitByte(emitter, 0x0f);
	EmitByte(emitter, 0xb6);
	EmitByte(emitter, 0xc9);
	EmitByte(emitter, 0xc1);
	EmitByte(emitter, 0xe1);
	EmitByte(emitter, 29);
	EmitMemory(emitter, 0, 0x8b, OSX970RDX, OSX970RSI, OSX970CPUFIELD(xer));
	EmitByte(emitter, 0x81);
	EmitByte(emitter, 0xe2);
	EmitDword(emitter, ~(uint32)OSXXERCA);
	EmitRegister(emitter, 0, 0x09, OSX970RCX, OSX970RDX);
	EmitMemory(emitter, 0, 0x89, OSX970RDX, OSX970RSI, OSX970CPUFIELD(xer));
}

static void EmitCarryIn(Osx970Emitter *emitter)
{
	EmitMemory(emitter, 0, 0x0fba, 4, OSX970RSI, OSX970CPUFIELD(xer));
	EmitByte(emitter, 29);
}

static int EmitCarry(Osx970Emitter *emitter, uint32 instruction, int sixtyfour)
{
	uint32 opcode;
	uint32 extended;
	uint32 immediate;
	int rt;
	int ra;
	int rb;
	opcode = instruction >> 26;
	extended = (instruction >> 1) & 0x3ff;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	rb = (instruction >> 11) & 31;
	immediate = (uint32)(int32)(int16)(instruction & 0xffff);
	if (opcode == 12 || opcode == 13 || (opcode == 31 && extended == 10)) {
		EmitLoadGpr(emitter, OSX970RAX, ra);
		if (sixtyfour) {
			if (opcode == 31)
				EmitMemory(emitter, 1, 0x03, OSX970RAX, OSX970RBX, OSX970GPR(rb));
			else {
				EmitByte(emitter, 0x48);
				EmitByte(emitter, 0x05);
				EmitDword(emitter, immediate);
			}
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0x92);
			EmitByte(emitter, 0xc1);
		} else {
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RCX);
			if (opcode == 31)
				EmitMemory(emitter, 0, 0x03, OSX970RCX, OSX970RBX, OSX970GPR(rb));
			else {
				EmitByte(emitter, 0x81);
				EmitByte(emitter, 0xc1);
				EmitDword(emitter, immediate);
			}
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0x92);
			EmitByte(emitter, 0xc1);
			if (opcode == 31)
				EmitMemory(emitter, 1, 0x03, OSX970RAX, OSX970RBX, OSX970GPR(rb));
			else {
				EmitByte(emitter, 0x48);
				EmitByte(emitter, 0x05);
				EmitDword(emitter, immediate);
			}
		}
		EmitStoreGpr(emitter, OSX970RAX, rt);
		EmitCarryFromCl(emitter);
		if (opcode == 13 || (opcode == 31 && (instruction & 1)))
			EmitRecord(emitter, sixtyfour);
		return 1;
	}
	if (opcode == 8 || (opcode == 31 && extended == 8)) {
		if (opcode == 8)
			EmitImmediate(emitter, OSX970RAX, (uint64)(int64)(int32)immediate);
		else
			EmitLoadGpr(emitter, OSX970RAX, rb);
		if (sixtyfour)
			EmitMemory(emitter, 1, 0x2b, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		else {
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RCX);
			EmitMemory(emitter, 0, 0x2b, OSX970RCX, OSX970RBX, OSX970GPR(ra));
		}
		EmitByte(emitter, 0x0f);
		EmitByte(emitter, 0x93);
		EmitByte(emitter, 0xc1);
		if (!sixtyfour)
			EmitMemory(emitter, 1, 0x2b, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		EmitStoreGpr(emitter, OSX970RAX, rt);
		EmitCarryFromCl(emitter);
		if (opcode == 31 && (instruction & 1))
			EmitRecord(emitter, sixtyfour);
		return 1;
	}
	if (opcode == 31 && (extended == 138 || extended == 202)) {
		EmitLoadGpr(emitter, OSX970RAX, ra);
		if (!sixtyfour) {
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RCX);
			EmitCarryIn(emitter);
			if (extended == 138)
				EmitMemory(emitter, 0, 0x13, OSX970RCX, OSX970RBX, OSX970GPR(rb));
			else {
				EmitByte(emitter, 0x83);
				EmitByte(emitter, 0xd1);
				EmitByte(emitter, 0);
			}
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0x92);
			EmitByte(emitter, 0xc2);
		}
		EmitCarryIn(emitter);
		if (extended == 138)
			EmitMemory(emitter, 1, 0x13, OSX970RAX, OSX970RBX, OSX970GPR(rb));
		else {
			EmitByte(emitter, 0x48);
			EmitByte(emitter, 0x83);
			EmitByte(emitter, 0xd0);
			EmitByte(emitter, 0);
		}
		if (sixtyfour) {
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0x92);
			EmitByte(emitter, 0xc1);
		} else
			EmitRegister(emitter, 0, 0x89, OSX970RDX, OSX970RCX);
		EmitStoreGpr(emitter, OSX970RAX, rt);
		EmitCarryFromCl(emitter);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	}
	return 0;
}

static int EmitShiftCount(Osx970Emitter *emitter, uint32 instruction, int sixtyfour)
{
	uint32 extended;
	uint32 shift;
	int rs;
	int ra;
	int rb;
	extended = (instruction >> 1) & 0x3ff;
	rs = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	rb = (instruction >> 11) & 31;
	switch (extended) {
	case 24: case 536:
		EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RBX, OSX970GPR(rb));
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(rs));
		EmitByte(emitter, 0xd3);
		if (extended == 24)
			EmitByte(emitter, 0xe0);
		else
			EmitByte(emitter, 0xe8);
		EmitRegister(emitter, 0, 0x31, OSX970RDX, OSX970RDX);
		EmitByte(emitter, 0xf6);
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0x20);
		EmitRegister(emitter, 0, 0x0f45, OSX970RAX, OSX970RDX);
		break;
	case 824:
		shift = (uint32)rb;
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(rs));
		if (shift == 0) {
			EmitRegister(emitter, 1, 0x63, OSX970RAX, OSX970RAX);
			EmitRegister(emitter, 0, 0x31, OSX970RCX, OSX970RCX);
		} else {
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RCX);
			EmitByte(emitter, 0x81);
			EmitByte(emitter, 0xe1);
			EmitDword(emitter, (1u << shift) - 1);
			EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RDX);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0xea);
			EmitByte(emitter, 31);
			EmitRegister(emitter, 0, 0x85, OSX970RCX, OSX970RCX);
			EmitByte(emitter, 0x0f);
			EmitByte(emitter, 0x95);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0x20);
			EmitByte(emitter, 0xd1);
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0xf8);
			EmitByte(emitter, shift);
			EmitRegister(emitter, 1, 0x63, OSX970RAX, OSX970RAX);
		}
		EmitStoreGpr(emitter, OSX970RAX, ra);
		EmitCarryFromCl(emitter);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 26:
		EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RBX, OSX970GPR(rs));
		EmitRegister(emitter, 0, 0x0fbd, OSX970RAX, OSX970RCX);
		EmitImmediate32(emitter, OSX970RDX, 0xffffffffu);
		EmitRegister(emitter, 0, 0x0f44, OSX970RAX, OSX970RDX);
		EmitImmediate32(emitter, OSX970RDX, 31);
		EmitRegister(emitter, 0, 0x29, OSX970RAX, OSX970RDX);
		EmitRegister(emitter, 0, 0x89, OSX970RDX, OSX970RAX);
		break;
	default:
		return 0;
	}
	EmitStoreGpr(emitter, OSX970RAX, ra);
	if (instruction & 1)
		EmitRecord(emitter, sixtyfour);
	return 1;
}

static int EmitConditionLogic(Osx970Emitter *emitter, uint32 instruction)
{
	uint32 extended;
	uint32 target;
	uint32 first;
	uint32 second;
	extended = (instruction >> 1) & 0x3ff;
	switch (extended) {
	case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
		break;
	default:
		return 0;
	}
	target = 31 - ((instruction >> 21) & 31);
	first = 31 - ((instruction >> 16) & 31);
	second = 31 - ((instruction >> 11) & 31);
	EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
	EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RCX);
	if (first) {
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xe9);
		EmitByte(emitter, first);
	}
	EmitRegister(emitter, 0, 0x89, OSX970RAX, OSX970RDX);
	if (second) {
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xea);
		EmitByte(emitter, second);
	}
	if (extended == 129 || extended == 417) {
		EmitByte(emitter, 0xf7);
		EmitByte(emitter, 0xd2);
	}
	if (extended == 129 || extended == 225 || extended == 257)
		EmitRegister(emitter, 0, 0x21, OSX970RDX, OSX970RCX);
	else if (extended == 193 || extended == 289)
		EmitRegister(emitter, 0, 0x31, OSX970RDX, OSX970RCX);
	else
		EmitRegister(emitter, 0, 0x09, OSX970RDX, OSX970RCX);
	if (extended == 33 || extended == 225 || extended == 289) {
		EmitByte(emitter, 0xf7);
		EmitByte(emitter, 0xd1);
	}
	EmitByte(emitter, 0x83);
	EmitByte(emitter, 0xe1);
	EmitByte(emitter, 1);
	if (target) {
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xe1);
		EmitByte(emitter, target);
	}
	EmitByte(emitter, 0x25);
	EmitDword(emitter, ~(1u << target));
	EmitRegister(emitter, 0, 0x09, OSX970RCX, OSX970RAX);
	EmitMemory(emitter, 0, 0x89, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
	return 1;
}

static int EmitRotate64(Osx970Emitter *emitter, uint32 instruction, int sixtyfour)
{
	uint32 shift;
	uint32 field;
	uint32 bound;
	uint64 mask;
	if (((instruction >> 2) & 7) > 1)
		return 0;
	shift = ((instruction >> 11) & 31) | (((instruction >> 1) & 1) << 5);
	field = (instruction >> 5) & 0x3f;
	bound = ((field & 1) << 5) | (field >> 1);
	if (((instruction >> 2) & 7) == 0)
		mask = Mask64(bound, 63);
	else
		mask = Mask64(0, bound);
	EmitLoadGpr(emitter, OSX970RAX, (instruction >> 21) & 31);
	if (shift) {
		EmitByte(emitter, 0x48);
		EmitByte(emitter, 0xc1);
		EmitByte(emitter, 0xc0);
		EmitByte(emitter, shift);
	}
	EmitImmediate(emitter, OSX970RCX, mask);
	EmitRegister(emitter, 1, 0x21, OSX970RCX, OSX970RAX);
	EmitStoreGpr(emitter, OSX970RAX, (instruction >> 16) & 31);
	if (instruction & 1)
		EmitRecord(emitter, sixtyfour);
	return 1;
}

static uint32 RotateMask32(uint32 begin, uint32 end)
{
	return (0xffffffffu >> begin) & (0xffffffffu << (31 - end));
}

static int EmitNative(Osx970Emitter *emitter, uint32 instruction, uint64 pc, uint32 count, int sixtyfour, int mode)
{
	uint32 opcode;
	uint32 extended;
	uint32 immediate;
	uint32 mask;
	uint32 shift;
	uint32 number;
	int rt;
	int ra;
	int rb;
	int wide;
	opcode = instruction >> 26;
	rt = (instruction >> 21) & 31;
	ra = (instruction >> 16) & 31;
	rb = (instruction >> 11) & 31;
	immediate = instruction & 0xffff;
	switch (opcode) {
	case 14: case 15:
		if (opcode == 14)
			immediate = (uint32)(int32)(int16)immediate;
		else
			immediate <<= 16;
		if (ra) {
			EmitLoadGpr(emitter, OSX970RAX, ra);
			EmitByte(emitter, 0x48);
			EmitByte(emitter, 0x05);
			EmitDword(emitter, immediate);
		} else
			EmitImmediate(emitter, OSX970RAX, (uint64)(int64)(int32)immediate);
		EmitStoreGpr(emitter, OSX970RAX, rt);
		return 1;
	case 24: case 25: case 26: case 27: case 28: case 29:
		if (opcode & 1)
			immediate <<= 16;
		EmitLoadGpr(emitter, OSX970RAX, rt);
		EmitImmediate32(emitter, OSX970RCX, immediate);
		if (opcode <= 25)
			EmitRegister(emitter, 1, 0x09, OSX970RCX, OSX970RAX);
		else if (opcode <= 27)
			EmitRegister(emitter, 1, 0x31, OSX970RCX, OSX970RAX);
		else
			EmitRegister(emitter, 1, 0x21, OSX970RCX, OSX970RAX);
		EmitStoreGpr(emitter, OSX970RAX, ra);
		if (opcode >= 28)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 10: case 11:
		wide = (instruction >> 21) & 1;
		EmitMemory(emitter, wide, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		if (opcode == 11)
			immediate = (uint32)(int32)(int16)immediate;
		EmitRex(emitter, wide, 0, 0, OSX970RAX);
		EmitByte(emitter, 0x3d);
		EmitDword(emitter, immediate);
		EmitCrFromFlags(emitter, (int)((instruction >> 23) & 7), opcode == 11);
		return 1;
	case 20: case 21:
		shift = (instruction >> 11) & 31;
		if (((instruction >> 6) & 31) > ((instruction >> 1) & 31))
			return 0;
		mask = RotateMask32((instruction >> 6) & 31, (instruction >> 1) & 31);
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(rt));
		if (shift) {
			EmitByte(emitter, 0xc1);
			EmitByte(emitter, 0xc0);
			EmitByte(emitter, shift);
		}
		EmitByte(emitter, 0x25);
		EmitDword(emitter, mask);
		if (opcode == 20) {
			EmitLoadGpr(emitter, OSX970RCX, ra);
			EmitImmediate(emitter, OSX970RDX, ~(uint64)mask);
			EmitRegister(emitter, 1, 0x21, OSX970RDX, OSX970RCX);
			EmitRegister(emitter, 1, 0x09, OSX970RCX, OSX970RAX);
		}
		EmitStoreGpr(emitter, OSX970RAX, ra);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 8: case 12: case 13:
		return EmitCarry(emitter, instruction, sixtyfour);
	case 7:
		EmitLoadGpr(emitter, OSX970RAX, ra);
		EmitByte(emitter, 0x48);
		EmitByte(emitter, 0x69);
		EmitByte(emitter, 0xc0);
		EmitDword(emitter, (uint32)(int32)(int16)immediate);
		EmitStoreGpr(emitter, OSX970RAX, rt);
		return 1;
	case 46: case 47:
		return EmitSpecialMemory(emitter, instruction, pc, count, sixtyfour, mode);
	case 30:
		return EmitRotate64(emitter, instruction, sixtyfour);
	case 19:
		if (((instruction >> 1) & 0x3ff) == 150)
			return 1;
		return EmitConditionLogic(emitter, instruction);
	case 31:
		break;
	default:
		return EmitMemoryAccess(emitter, instruction, pc, count, sixtyfour, mode);
	}
	extended = (instruction >> 1) & 0x3ff;
	if (EmitCarry(emitter, instruction, sixtyfour) || EmitShiftCount(emitter, instruction, sixtyfour) || EmitSpecialMemory(emitter, instruction, pc, count, sixtyfour, mode))
		return 1;
	if (extended == 83) {
		if (mode == OSXMODEUSER)
			return 0;
		EmitMemory(emitter, 1, 0x8b, OSX970RAX, OSX970RBX, OSX970FIELD(msr));
		EmitStoreGpr(emitter, OSX970RAX, rt);
		return 1;
	}
	switch (extended) {
	case 266: case 40: case 104:
		if (extended == 266) {
			EmitLoadGpr(emitter, OSX970RAX, ra);
			EmitMemory(emitter, 1, 0x03, OSX970RAX, OSX970RBX, OSX970GPR(rb));
		} else if (extended == 40) {
			EmitLoadGpr(emitter, OSX970RAX, rb);
			EmitMemory(emitter, 1, 0x2b, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		} else {
			EmitLoadGpr(emitter, OSX970RAX, ra);
			EmitRegister(emitter, 1, 0xf7, 3, OSX970RAX);
		}
		EmitStoreGpr(emitter, OSX970RAX, rt);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 235:
		EmitMemory(emitter, 1, 0x63, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		EmitMemory(emitter, 1, 0x63, OSX970RCX, OSX970RBX, OSX970GPR(rb));
		EmitRegister(emitter, 1, 0x0faf, OSX970RAX, OSX970RCX);
		EmitStoreGpr(emitter, OSX970RAX, rt);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
		EmitLoadGpr(emitter, OSX970RAX, rt);
		EmitLoadGpr(emitter, OSX970RCX, rb);
		if (extended == 60 || extended == 412)
			EmitNot(emitter, OSX970RCX);
		if (extended == 28 || extended == 60 || extended == 476)
			EmitRegister(emitter, 1, 0x21, OSX970RCX, OSX970RAX);
		else if (extended == 284 || extended == 316)
			EmitRegister(emitter, 1, 0x31, OSX970RCX, OSX970RAX);
		else
			EmitRegister(emitter, 1, 0x09, OSX970RCX, OSX970RAX);
		if (extended == 124 || extended == 284 || extended == 476)
			EmitNot(emitter, OSX970RAX);
		EmitStoreGpr(emitter, OSX970RAX, ra);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 922: case 954: case 986:
		if (extended == 922)
			EmitMemory(emitter, 1, 0x0fbf, OSX970RAX, OSX970RBX, OSX970GPR(rt));
		else if (extended == 954)
			EmitMemory(emitter, 1, 0x0fbe, OSX970RAX, OSX970RBX, OSX970GPR(rt));
		else
			EmitMemory(emitter, 1, 0x63, OSX970RAX, OSX970RBX, OSX970GPR(rt));
		EmitStoreGpr(emitter, OSX970RAX, ra);
		if (instruction & 1)
			EmitRecord(emitter, sixtyfour);
		return 1;
	case 0: case 32:
		wide = (instruction >> 21) & 1;
		EmitMemory(emitter, wide, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(ra));
		EmitMemory(emitter, wide, 0x3b, OSX970RAX, OSX970RBX, OSX970GPR(rb));
		EmitCrFromFlags(emitter, (int)((instruction >> 23) & 7), extended == 0);
		return 1;
	case 19:
		if (instruction & 0x00100000)
			return 0;
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
		EmitStoreGpr(emitter, OSX970RAX, rt);
		return 1;
	case 144:
		mask = 0;
		for (number = 0; number < 8; number++)
			if ((instruction >> 12) & (0x80u >> number))
				mask |= 0xf0000000u >> (number * 4);
		EmitMemory(emitter, 0, 0x8b, OSX970RAX, OSX970RBX, OSX970GPR(rt));
		EmitByte(emitter, 0x25);
		EmitDword(emitter, mask);
		EmitMemory(emitter, 0, 0x8b, OSX970RCX, OSX970RSI, OSX970CPUFIELD(cr));
		EmitByte(emitter, 0x81);
		EmitByte(emitter, 0xe1);
		EmitDword(emitter, ~mask);
		EmitRegister(emitter, 0, 0x09, OSX970RCX, OSX970RAX);
		EmitMemory(emitter, 0, 0x89, OSX970RAX, OSX970RSI, OSX970CPUFIELD(cr));
		return 1;
	case 339: case 467:
		number = ((instruction >> 16) & 31) | (((instruction >> 11) & 31) << 5);
		if (extended == 339 && (number == 259 || (number >= 272 && number <= 275))) {
			if (number != 259 && mode == OSXMODEUSER)
				return 0;
			if (number == 259)
				number = 275;
			EmitMemory(emitter, 1, 0x8b, OSX970RAX, OSX970RBX, OSX970FIELD(sprg) + (number - 272) * 8);
			EmitStoreGpr(emitter, OSX970RAX, rt);
			return 1;
		}
		if (number != 8 && number != 9)
			return 0;
		if (extended == 339) {
			if (number == 8)
				EmitMemory(emitter, 1, 0x8b, OSX970RAX, OSX970RBX, OSX970FIELD(lr));
			else
				EmitMemory(emitter, 1, 0x8b, OSX970RAX, OSX970RBX, OSX970FIELD(ctr));
			EmitStoreGpr(emitter, OSX970RAX, rt);
		} else {
			EmitLoadGpr(emitter, OSX970RAX, rt);
			if (number == 8)
				EmitMemory(emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(lr));
			else
				EmitMemory(emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(ctr));
		}
		return 1;
	}
	if (instruction & 1)
		return 0;
	return EmitMemoryAccess(emitter, instruction, pc, count, sixtyfour, mode);
}

static int EndsBlock(uint32 instruction)
{
	uint32 opcode;
	uint32 extended;
	opcode = instruction >> 26;
	if (opcode == 18 || opcode == 17 || opcode == 6)
		return 1;
	if (opcode == 19) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 16 || extended == 528 || extended == 18 || extended == 50 || extended == 274 || extended == 150)
			return 1;
		return 0;
	}
	if (opcode == 31) {
		extended = (instruction >> 1) & 0x3ff;
		if (extended == 146 || extended == 178)
			return 1;
	}
	return 0;
}

static void Compile(Osx970 *core, Osx970Jit *jit, Osx970Block *block, uint64 pc, uint32 physical, uint32 key)
{
	Osx970Emitter emitter;
	uint8 *start;
	uint8 *body;
	uint8 *epilogue;
	uint8 *finish;
	uint8 *stale;
	uint32 instruction;
	uint32 count;
	uint32 page;
	int sixtyfour;
	int mode;
	int index;
	if (jit->used + OSX970JITRESERVE >= OSX970JITBYTES) {
		memset(jit->blocks, 0, sizeof(jit->blocks));
		memset(jit->cache, 0, sizeof(jit->cache));
		jit->used = (uint32)(jit->stepthunk - jit->arena) + 64;
	}
	start = jit->arena + jit->used;
	emitter.cursor = start;
	emitter.exitcount = 0;
	emitter.coldcount = 0;
	emitter.linkcount = 0;
	emitter.page = pc & ~0xfffULL;
	emitter.key = key;
	emitter.fetchmode = core->modes[OSXTLBFETCH];
	emitter.vector = (core->msr & OSXMSRVEC) != 0;
	emitter.stepthunk = jit->stepthunk;
	sixtyfour = (core->msr & OSX970MSRSF) != 0;
	mode = core->modes[OSXTLBREAD];
	page = physical >> 12;
	if (!core->codepages[page]) {
		core->codepages[page] = 1;
		for (index = 0; index < OSX970TLBSIZE; index++) {
			core->tlb[OSXMODEUSER][OSXTLBWRITE][index].page = OSX970TLBINVALID;
			core->tlb[OSXMODESUPERVISOR][OSXTLBWRITE][index].page = OSX970TLBINVALID;
			core->tlb[OSXMODEREAL][OSXTLBWRITE][index].page = OSX970TLBINVALID;
		}
	}
	EmitByte(&emitter, 0x53);
	EmitByte(&emitter, 0x56);
	EmitByte(&emitter, 0x57);
	EmitByte(&emitter, 0x48);
	EmitByte(&emitter, 0x83);
	EmitByte(&emitter, 0xec);
	EmitByte(&emitter, 0x20);
	EmitRegister(&emitter, 1, 0x89, OSX970RCX, OSX970RBX);
	EmitMemory(&emitter, 1, 0x8b, OSX970RSI, OSX970RBX, OSX970FIELD(cpu));
	body = emitter.cursor;
	EmitMemory(&emitter, 1, 0x8b, OSX970RAX, OSX970RBX, OSX970FIELD(versions));
	EmitMemory(&emitter, 0, 0x81, 7, OSX970RAX, page * 4);
	EmitDword(&emitter, core->versions[page]);
	stale = EmitJump(&emitter, 0x85);
	count = 0;
	while (count < OSX970JITLIMIT) {
		instruction = (uint32)LoadBig(core->cpu->ram + physical + count * 4, 4);
		if (!EmitBranch(&emitter, core, instruction, pc + count * 4, count + 1, sixtyfour) && !EmitNative(&emitter, instruction, pc + count * 4, count + 1, sixtyfour, mode))
			EmitStep(&emitter, pc + count * 4, instruction, count + 1);
		count++;
		if (EndsBlock(instruction) || ((physical + count * 4) & 0xfff) == 0 || emitter.exitcount >= OSX970JITEXITS - 4 || emitter.coldcount >= OSX970JITEXITS - 4)
			break;
	}
	EmitChain(&emitter, count, Effective(core, pc + count * 4));
	finish = emitter.cursor;
	EmitRegister(&emitter, 0, 0x31, OSX970RAX, OSX970RAX);
	epilogue = emitter.cursor;
	EmitByte(&emitter, 0x48);
	EmitByte(&emitter, 0x83);
	EmitByte(&emitter, 0xc4);
	EmitByte(&emitter, 0x20);
	EmitByte(&emitter, 0x5f);
	EmitByte(&emitter, 0x5e);
	EmitByte(&emitter, 0x5b);
	EmitByte(&emitter, 0xc3);
	for (index = 0; index < emitter.coldcount; index++) {
		PatchJump(emitter.colds[index].jump, emitter.cursor);
		if (emitter.colds[index].resume) {
			EmitCallStep(&emitter, emitter.colds[index].pc, emitter.colds[index].instruction);
			EmitByte(&emitter, 0x0f);
			EmitByte(&emitter, 0x84);
			EmitDword(&emitter, 0);
			PatchJump(emitter.cursor - 4, emitter.colds[index].resume);
		}
		EmitAccount(&emitter, emitter.colds[index].count);
		EmitByte(&emitter, 0xe9);
		EmitDword(&emitter, 0);
		PatchJump(emitter.cursor - 4, finish);
	}
	for (index = 0; index < emitter.linkcount; index++) {
		PatchJump(emitter.links[index], emitter.cursor);
		EmitByte(&emitter, 0x48);
		EmitByte(&emitter, 0x8d);
		EmitByte(&emitter, 0x05);
		EmitDword(&emitter, (uint32)(int32)(emitter.links[index] - (emitter.cursor + 4)));
		EmitMemory(&emitter, 1, 0x89, OSX970RAX, OSX970RBX, OSX970FIELD(jitlinksite));
		EmitByte(&emitter, 0xe9);
		EmitDword(&emitter, 0);
		PatchJump(emitter.cursor - 4, finish);
	}
	PatchJump(stale, finish);
	for (index = 0; index < emitter.exitcount; index++)
		PatchJump(emitter.exits[index], finish);
	FlushInstructionCache(GetCurrentProcess(), start, (SIZE_T)(emitter.cursor - start));
	jit->used += ((uint32)(emitter.cursor - start) + 15) & ~15u;
	jit->compiled++;
	block->pc = pc;
	block->physical = physical;
	block->key = key;
	block->version = core->versions[page];
	block->count = count;
	block->code = (Osx970Code)start;
	block->body = body;
}

int Osx970JitCreate(Osx970 *core)
{
	Osx970Jit *jit;
	Osx970Emitter thunk;
	uint32 pages;
	jit = (Osx970Jit *)calloc(1, sizeof(Osx970Jit));
	if (jit == NULL)
		return 0;
	jit->arena = (uint8 *)VirtualAlloc(NULL, OSX970JITBYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	pages = core->cpu->ramsize >> 12;
	core->codepages = (uint8 *)calloc(pages, 1);
	core->versions = (uint32 *)calloc(pages, sizeof(uint32));
	if (jit->arena == NULL || core->codepages == NULL || core->versions == NULL) {
		if (jit->arena)
			VirtualFree(jit->arena, 0, MEM_RELEASE);
		free(core->codepages);
		free(core->versions);
		core->codepages = NULL;
		core->versions = NULL;
		free(jit);
		return 0;
	}
	jit->stepthunk = jit->arena;
	thunk.cursor = jit->arena;
	EmitMemory(&thunk, 1, 0x89, OSX970R8, OSX970RBX, OSX970FIELD(pc));
	EmitRegister(&thunk, 1, 0x89, OSX970RBX, OSX970RCX);
	EmitImmediate(&thunk, OSX970RAX, (uint64)(uintptr_t)Osx970Step);
	EmitByte(&thunk, 0xff);
	EmitByte(&thunk, 0xe0);
	jit->used = 64;
	core->jit = jit;
	core->jitcache = jit->cache;
	return 1;
}

void Osx970JitDestroy(Osx970 *core)
{
	Osx970Jit *jit;
	jit = (Osx970Jit *)core->jit;
	if (jit == NULL)
		return;
	VirtualFree(jit->arena, 0, MEM_RELEASE);
	free(jit);
	free(core->codepages);
	free(core->versions);
	core->codepages = NULL;
	core->versions = NULL;
	core->jit = NULL;
}

static void InterpretBlock(Osx970 *core)
{
	OsxCpu *cpu;
	uint32 instruction;
	uint64 expected;
	int executed;
	cpu = core->cpu;
	executed = 0;
	do {
		instruction = (uint32)LoadBig(core->fetchhost + (core->pc & 0xfff), 4);
		expected = Effective(core, core->pc + 4);
		cpu->slice--;
		Osx970ExecuteInstruction(core, instruction);
		executed++;
	} while (core->pc == expected && executed < OSX970JITLIMIT && cpu->slice > 0 && (core->pc & 0xfff) != 0 && !cpu->halted);
}

static void JitRun(Osx970 *core, Osx970Jit *jit)
{
	OsxCpu *cpu;
	Osx970Block *block;
	uint32 instruction;
	uint32 physical;
	uint32 key;
	uint32 hash;
	uint32 version;
	uint8 *link;
	uint32 hot;
	Osx970Cache *cache;
	Osx970TlbEntry *entry;
	cpu = core->cpu;
	link = NULL;
	hot = OSX970JITHOT;
	if (core->jithot)
		hot = (uint32)core->jithot;
	while (cpu->slice > 0 && !cpu->halted) {
		if (!Fetch(core, &instruction)) {
			core->pc = core->nextpc;
			cpu->slice--;
			link = NULL;
			continue;
		}
		if ((core->msr & OSXMSRSE) || core->fetchhost < cpu->ram || core->fetchhost >= cpu->ram + cpu->ramsize || (core->pc & 3)) {
			cpu->slice--;
			Osx970ExecuteInstruction(core, instruction);
			link = NULL;
			continue;
		}
		physical = (uint32)(core->fetchhost - cpu->ram) + (uint32)(core->pc & 0xfff);
		key = (uint32)((core->msr & (OSXMSRPR | OSXMSRIR | OSXMSRDR | OSXMSRVEC)) | ((core->msr >> 32) & 0x80000000u));
		hash = (uint32)((((uint64)physical >> 2) * 0x9e3779b1u) ^ ((uint64)key * 0x85ebca6bu)) >> 16;
		block = &jit->blocks[hash & (OSX970JITBLOCKS - 1)];
		version = core->versions[physical >> 12];
		if (block->pc != core->pc || block->physical != physical || block->key != key || block->version != version) {
			block->pc = core->pc;
			block->physical = physical;
			block->key = key;
			block->version = version;
			block->count = 0;
			block->code = NULL;
		}
		if (block->code == NULL) {
			block->count++;
			if (block->count < hot) {
				InterpretBlock(core);
				link = NULL;
				continue;
			}
			Compile(core, jit, block, core->pc, physical, key);
			link = NULL;
		}
		if (link) {
			PatchJump(link, block->body);
			FlushInstructionCache(GetCurrentProcess(), link, 4);
		}
		cache = &jit->cache[(core->pc >> 2) & (OSX970JITCACHE - 1)];
		entry = &core->tlb[core->modes[OSXTLBFETCH]][OSXTLBFETCH][(core->pc >> 12) & OSX970TLBMASK];
		if (entry->page == (core->pc & ~0xfffULL)) {
			cache->pc = core->pc;
			cache->key = key;
			cache->host = entry->host;
			cache->body = block->body;
		}
		core->jitlinksite = NULL;
		block->code(core);
		link = core->jitlinksite;
	}
}

void Osx970Run(Osx970 *core, int32 count)
{
	OsxCpu *cpu;
	uint32 instruction;
	cpu = core->cpu;
	if (cpu->halted)
		return;
	UpdateModes(core);
	cpu->slice = count;
	if (core->jit)
		JitRun(core, (Osx970Jit *)core->jit);
	while (cpu->slice > 0 && !cpu->halted) {
		cpu->slice--;
		if (!Fetch(core, &instruction)) {
			core->pc = core->nextpc;
			continue;
		}
		Osx970ExecuteInstruction(core, instruction);
	}
	Mirror(core);
}
#else
int Osx970JitCreate(Osx970 *core)
{
	return 0;
}

void Osx970JitDestroy(Osx970 *core)
{
}

void Osx970Run(Osx970 *core, int32 count)
{
	OsxCpu *cpu;
	uint32 instruction;
	cpu = core->cpu;
	if (cpu->halted)
		return;
	UpdateModes(core);
	cpu->slice = count;
	while (cpu->slice > 0 && !cpu->halted) {
		cpu->slice--;
		if (!Fetch(core, &instruction)) {
			core->pc = core->nextpc;
			continue;
		}
		Osx970ExecuteInstruction(core, instruction);
	}
	Mirror(core);
}
#endif
#endif
