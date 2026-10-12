/*
 *  osxbus.cpp - OSX DBDMA bus emulation
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
#include "osxmachine.h"

#define DBDMARUN 0x8000
#define DBDMAPAUSE 0x4000
#define DBDMAFLUSH 0x2000
#define DBDMAWAKE 0x1000
#define DBDMADEAD 0x0800
#define DBDMAACTIVE 0x0400
#define DBDMABRANCH 0x0100
#define DBDMAWRITABLE 0xf0ff

#define DBDMAOUTPUTMORE 0
#define DBDMAOUTPUTLAST 1
#define DBDMAINPUTMORE 2
#define DBDMAINPUTLAST 3
#define DBDMASTOREQUAD 4
#define DBDMALOADQUAD 5
#define DBDMANOP 6
#define DBDMASTOP 7

#define DBDMAMAXSTEPS 4096

static uint32 SwapForSize(uint32 value, int size)
{
	if (size == 4)
		return bswap_32(value);
	if (size == 2)
		return ((value & 0xff) << 8) | ((value >> 8) & 0xff);
	return value & 0xff;
}

static uint32 ReadLittleRegister(uint32 reg, uint32 offset, int size)
{
	uint32 bytes;
	uint32 result;
	int index;
	bytes = reg;
	result = 0;
	for (index = 0; index < size; index++)
		result = (result << 8) | ((bytes >> (((offset + index) & 3) * 8)) & 0xff);
	return result;
}

static uint32 WriteLittleRegister(uint32 reg, uint32 offset, int size, uint32 value)
{
	int index;
	uint32 shift;
	for (index = 0; index < size; index++) {
		shift = ((offset + index) & 3) * 8;
		reg = (reg & ~(0xffu << shift)) | (((value >> ((size - 1 - index) * 8)) & 0xff) << shift);
	}
	return reg;
}

static inline uint32 ReadGuest32(OsxMachine *machine, uint32 address)
{
	uint8 *host;
	if (address > machine->ramsize - 4)
		return 0;
	host = machine->ram + address;
	return ((uint32)host[0] << 24) | ((uint32)host[1] << 16) | ((uint32)host[2] << 8) | host[3];
}

static inline uint32 ReadGuestLittle32(OsxMachine *machine, uint32 address)
{
	return bswap_32(ReadGuest32(machine, address));
}

static inline void WriteGuestLittle32(OsxMachine *machine, uint32 address, uint32 value)
{
	uint8 *host;
	if (address > machine->ramsize - 4)
		return;
	host = machine->ram + address;
	host[0] = (uint8)value;
	host[1] = (uint8)(value >> 8);
	host[2] = (uint8)(value >> 16);
	host[3] = (uint8)(value >> 24);
}

void OsxPciReset(OsxMachine *machine)
{
	OsxPciFunction *function;
	memset(machine->pci, 0, sizeof(machine->pci));
	function = &machine->pci[0];
	function->vendordevice = 0x00021057;
	function->classrevision = 0x06000040;
	function->command = 0x00000006;
	function = &machine->pci[1];
	function->vendordevice = 0x0010106b;
	function->classrevision = 0xff000001;
	function->command = 0x02800006;
	function->bar0 = OSXHEATHROWBASE;
	function->bar0mask = ~(OSXHEATHROWSIZE - 1);
	function = &machine->pci[2];
	function->vendordevice = 0x11111234;
	function->classrevision = 0x03000002;
	function->command = 0x02800003;
	function->bar0 = OSXVRAMBASE | 8;
	function->bar0mask = ~(OSXVRAMSIZE - 1);
	machine->configaddress = 0;
}

static OsxPciFunction *SelectedFunction(OsxMachine *machine, uint32 *reg)
{
	uint32 address;
	uint32 device;
	address = machine->configaddress;
	*reg = address & 0xfc;
	if (!(address & 0x80000000u) || ((address >> 16) & 0xff) != 0 || ((address >> 8) & 7) != 0)
		return NULL;
	device = (address >> 11) & 0x1f;
	if (device == OSXPCIDEVICEGRACKLE)
		return &machine->pci[0];
	if (device == OSXPCIDEVICEHEATHROW)
		return &machine->pci[1];
	if (device == OSXPCIDEVICEDISPLAY)
		return &machine->pci[2];
	return NULL;
}

static uint32 ConfigDword(OsxPciFunction *function, uint32 reg)
{
	switch (reg) {
	case 0x00: return function->vendordevice;
	case 0x04: return function->command;
	case 0x08: return function->classrevision;
	case 0x0c: return function->latency;
	case 0x10: return function->bar0;
	case 0x3c: return function->interrupt;
	}
	return 0;
}

uint32 OsxPciConfigRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxPciFunction *function;
	uint32 reg;
	function = SelectedFunction(machine, &reg);
	if (function == NULL)
		return 0xffffffffu >> ((4 - size) * 8);
	return ReadLittleRegister(ConfigDword(function, reg), offset, size);
}

void OsxPciConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxPciFunction *function;
	uint32 reg;
	uint32 merged;
	function = SelectedFunction(machine, &reg);
	if (function == NULL)
		return;
	merged = WriteLittleRegister(ConfigDword(function, reg), offset, size, value);
	switch (reg) {
	case 0x04:
		function->command = (function->command & 0xffff0000u) | (merged & 0xffff);
		break;
	case 0x0c:
		function->latency = merged & 0x0000ffffu;
		break;
	case 0x10:
		if (function->bar0mask)
			function->bar0 = (merged & function->bar0mask) | (function->bar0 & 0xf);
		break;
	case 0x3c:
		function->interrupt = (function->interrupt & 0xffffff00u) | (merged & 0xff);
		break;
	}
}

static void UpdateCpuInterrupt(OsxMachine *machine)
{
	OsxPic *pic;
	uint32 pending;
	pic = &machine->pic;
	pending = (pic->events[0] | (pic->levels[0] & OSXIRQLEVELMASK)) & pic->mask[0];
	pending |= pic->events[1] & pic->mask[1];
	machine->cpu->externalline = pending != 0;
	if (pending)
		machine->cpu->halted = 0;
}

void OsxPicSetLine(OsxMachine *machine, int source, int level)
{
	OsxPic *pic;
	int bank;
	uint32 bit;
	pic = &machine->pic;
	bank = source >> 5;
	bit = 1u << (source & 31);
	if (level) {
		if (!(pic->levels[bank] & bit) && !(bank == 0 && (bit & OSXIRQLEVELMASK)))
			pic->events[bank] |= bit;
		pic->levels[bank] |= bit;
	} else {
		pic->levels[bank] &= ~bit;
	}
	UpdateCpuInterrupt(machine);
}

static uint32 PicRead(OsxMachine *machine, uint32 offset)
{
	int bank;
	if (offset < 0x20)
		bank = 1;
	else
		bank = 0;
	switch (offset & 0xc) {
	case 0x0: return machine->pic.events[bank];
	case 0x4: return machine->pic.mask[bank];
	case 0xc: return machine->pic.levels[bank];
	}
	return 0;
}

static void PicWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	int bank;
	if (offset < 0x20)
		bank = 1;
	else
		bank = 0;
	switch (offset & 0xc) {
	case 0x4:
		machine->pic.mask[bank] = value;
		break;
	case 0x8:
		if (bank == 0)
			value &= ~OSXIRQLEVELMASK;
		machine->pic.events[bank] &= ~value;
		break;
	}
	UpdateCpuInterrupt(machine);
}

static int DbdmaCondition(uint32 status, uint32 select, uint32 mode)
{
	int match;
	if (mode == 0)
		return 0;
	if (mode == 3)
		return 1;
	match = ((status & (select >> 16) & 0xff) == (select & (select >> 16) & 0xff));
	if (mode == 1)
		return match;
	return !match;
}

static void DbdmaStop(OsxMachine *machine, int channel)
{
	machine->dbdma[channel].status &= ~(DBDMAACTIVE | DBDMAFLUSH);
}

static void DbdmaComplete(OsxMachine *machine, OsxDbdma *dma, uint32 descriptor, uint32 word0, uint32 residual)
{
	uint32 status;
	uint32 command;
	status = dma->status | DBDMAACTIVE | DBDMARUN;
	WriteGuestLittle32(machine, descriptor + 12, (status << 16) | (residual & 0xffff));
	command = (word0 >> 16) & 0xffff;
	if (DbdmaCondition(dma->status, dma->interruptselect, (command >> 4) & 3) && dma->irq >= 0) {
		OsxPicSetLine(machine, dma->irq, 1);
		OsxPicSetLine(machine, dma->irq, 0);
	}
	if (DbdmaCondition(dma->status, dma->branchselect, (command >> 2) & 3)) {
		dma->commandpointer = ReadGuestLittle32(machine, descriptor + 8);
		dma->status |= DBDMABRANCH;
	} else {
		dma->commandpointer = descriptor + 16;
		dma->status &= ~DBDMABRANCH;
	}
}

static void DbdmaRunControl(OsxMachine *machine, int channel)
{
	OsxDbdma *dma;
	uint32 descriptor;
	uint32 word0;
	uint32 command;
	uint32 address;
	uint32 value;
	int steps;
	dma = &machine->dbdma[channel];
	for (steps = 0; steps < DBDMAMAXSTEPS; steps++) {
		if (!(dma->status & DBDMARUN) || !(dma->status & DBDMAACTIVE) || (dma->status & DBDMAPAUSE))
			return;
		descriptor = dma->commandpointer;
		word0 = ReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		address = ReadGuestLittle32(machine, descriptor + 4);
		if (command == DBDMASTOP) {
			DbdmaStop(machine, channel);
			return;
		}
		if (command <= DBDMAINPUTLAST)
			return;
		if (command == DBDMASTOREQUAD) {
			value = ReadGuestLittle32(machine, descriptor + 8);
			WriteGuestLittle32(machine, address & ~3u, value);
		} else if (command == DBDMALOADQUAD) {
			value = ReadGuestLittle32(machine, address & ~3u);
			WriteGuestLittle32(machine, descriptor + 8, value);
		}
		DbdmaComplete(machine, dma, descriptor, word0, 0);
	}
}

int OsxDbdmaTransfer(OsxMachine *machine, int channel, uint8 *data, uint32 length, int towardmemory)
{
	OsxDbdma *dma;
	uint32 descriptor;
	uint32 word0;
	uint32 command;
	uint32 address;
	uint32 request;
	uint32 residual;
	uint32 consumed;
	uint32 chunk;
	uint32 done;
	int steps;
	dma = &machine->dbdma[channel];
	consumed = 0;
	for (steps = 0; steps < DBDMAMAXSTEPS && consumed < length; steps++) {
		if (!(dma->status & DBDMARUN) || !(dma->status & DBDMAACTIVE))
			break;
		DbdmaRunControl(machine, channel);
		if (!(dma->status & DBDMAACTIVE))
			break;
		descriptor = dma->commandpointer;
		word0 = ReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		if (command > DBDMAINPUTLAST)
			break;
		address = ReadGuestLittle32(machine, descriptor + 4);
		request = word0 & 0xffff;
		residual = ReadGuestLittle32(machine, descriptor + 12) & 0xffff;
		if (residual == 0 || residual > request)
			residual = request;
		done = request - residual;
		chunk = residual;
		if (chunk > length - consumed)
			chunk = length - consumed;
		if (address + done + chunk <= machine->ramsize) {
			if (towardmemory) {
				memcpy(machine->ram + address + done, data + consumed, chunk);
				OsxJitInvalidate(machine->cpu, address + done, chunk);
			} else
				memcpy(data + consumed, machine->ram + address + done, chunk);
		}
		consumed += chunk;
		residual -= chunk;
		if (residual != 0) {
			WriteGuestLittle32(machine, descriptor + 12, ((dma->status | DBDMAACTIVE | DBDMARUN) << 16) | residual);
			break;
		}
		DbdmaComplete(machine, dma, descriptor, word0, 0);
	}
	DbdmaRunControl(machine, channel);
	return (int)consumed;
}

void OsxDbdmaDump(OsxMachine *machine, int channel)
{
	uint32 pointer;
	int index;
	pointer = machine->dbdma[channel].commandpointer;
	if (machine->log)
		fprintf(machine->log, "  pic events %08x %08x mask %08x %08x levels %08x %08x external %d msr %08x\n", machine->pic.events[0], machine->pic.events[1], machine->pic.mask[0], machine->pic.mask[1], machine->pic.levels[0], machine->pic.levels[1], machine->cpu->externalline, machine->cpu->msr);
	for (index = -4; index < 4 && machine->log; index++)
		fprintf(machine->log, "  dbdma %08x: %08x %08x %08x %08x\n", pointer + index * 16, ReadGuestLittle32(machine, pointer + index * 16), ReadGuestLittle32(machine, pointer + index * 16 + 4), ReadGuestLittle32(machine, pointer + index * 16 + 8), ReadGuestLittle32(machine, pointer + index * 16 + 12));
}

void OsxDbdmaFinish(OsxMachine *machine, int channel)
{
	DbdmaRunControl(machine, channel);
}

void OsxDbdmaKick(OsxMachine *machine, int channel)
{
	DbdmaRunControl(machine, channel);
	if (channel == OSXDBDMAIDE0)
		OsxAtaDmaReady(machine, 0);
	else if (channel == OSXDBDMAIDE1)
		OsxAtaDmaReady(machine, 1);
}

static uint32 DbdmaRead(OsxMachine *machine, int channel, uint32 reg)
{
	OsxDbdma *dma;
	dma = &machine->dbdma[channel];
	if (machine->log && machine->dmatrace && reg == 4)
		fprintf(machine->log, "dbdma read status channel %d %04x\n", channel, dma->status);
	switch (reg) {
	case 0x04: return dma->status;
	case 0x0c: return dma->commandpointer;
	case 0x10: return dma->interruptselect;
	case 0x14: return dma->branchselect;
	case 0x18: return dma->waitselect;
	}
	return 0;
}

static void DbdmaWrite(OsxMachine *machine, int channel, uint32 reg, uint32 value)
{
	OsxDbdma *dma;
	uint32 mask;
	uint32 previous;
	dma = &machine->dbdma[channel];
	if (machine->log && machine->dmatrace)
		fprintf(machine->log, "dbdma write channel %d reg %02x value %08x status %04x pointer %08x\n", channel, reg, value, dma->status, dma->commandpointer);
	switch (reg) {
	case 0x00:
		mask = (value >> 16) & DBDMAWRITABLE;
		previous = dma->status;
		dma->status = (dma->status & ~mask) | (value & mask);
		if ((dma->status & DBDMARUN) && !(previous & DBDMARUN))
			dma->status = (dma->status | DBDMAACTIVE) & ~DBDMADEAD;
		if (!(dma->status & DBDMARUN))
			dma->status &= ~(DBDMAACTIVE | DBDMADEAD);
		if (dma->status & DBDMAFLUSH)
			dma->status &= ~DBDMAFLUSH;
		if ((dma->status & DBDMAWAKE) && (dma->status & DBDMARUN))
			dma->status = (dma->status | DBDMAACTIVE) & ~DBDMAWAKE;
		if (dma->status & DBDMAACTIVE)
			OsxDbdmaKick(machine, channel);
		break;
	case 0x0c:
		if (!(dma->status & DBDMAACTIVE))
			dma->commandpointer = value & ~15u;
		break;
	case 0x10:
		dma->interruptselect = value & 0x00ff00ffu;
		break;
	case 0x14:
		dma->branchselect = value & 0x00ff00ffu;
		break;
	case 0x18:
		dma->waitselect = value & 0x00ff00ffu;
		break;
	}
}

uint32 OsxHeathrowRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 value;
	if (offset >= 0x10 && offset < 0x30)
		return SwapForSize(PicRead(machine, offset & ~3u), size);
	if (offset >= 0x34 && offset < 0x3c)
		return SwapForSize(machine->heathrowfcr[(offset - 0x34) >> 2], size);
	if (offset >= 0x8000 && offset < 0x9000) {
		value = DbdmaRead(machine, (offset >> 8) & 15, offset & 0xfc);
		return ReadLittleRegister(value, offset & 3, size);
	}
	if (offset >= 0x16000 && offset < 0x18000)
		return OsxCudaRead(machine, (offset >> 9) & 15);
	if (offset >= 0x20000 && offset < 0x22000)
		return OsxAtaRead(machine, (offset >> 12) & 1, offset & 0xfff, size);
	if (offset >= 0x60000)
		return machine->nvram[((offset - 0x60000) >> 4) & (OSXNVRAMSIZE - 1)];
	if (offset >= 0x13000 && offset < 0x14000)
		return 0x04;
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "heathrow read %05x size %d\n", offset, size);
	return 0;
}

void OsxHeathrowWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	uint32 reg;
	int channel;
	if (offset >= 0x10 && offset < 0x30) {
		PicWrite(machine, offset & ~3u, SwapForSize(value, size));
		return;
	}
	if (offset >= 0x34 && offset < 0x3c) {
		machine->heathrowfcr[(offset - 0x34) >> 2] = SwapForSize(value, size);
		return;
	}
	if (offset >= 0x8000 && offset < 0x9000) {
		channel = (offset >> 8) & 15;
		reg = WriteLittleRegister(DbdmaRead(machine, channel, offset & 0xfc), offset & 3, size, value);
		if ((offset & 0xfc) == 0)
			reg = WriteLittleRegister(0, offset & 3, size, value);
		DbdmaWrite(machine, channel, offset & 0xfc, reg);
		return;
	}
	if (offset >= 0x16000 && offset < 0x18000) {
		OsxCudaWrite(machine, (offset >> 9) & 15, value & 0xff);
		return;
	}
	if (offset >= 0x20000 && offset < 0x22000) {
		OsxAtaWrite(machine, (offset >> 12) & 1, offset & 0xfff, size, value);
		return;
	}
	if (offset >= 0x60000) {
		machine->nvram[((offset - 0x60000) >> 4) & (OSXNVRAMSIZE - 1)] = (uint8)value;
		return;
	}
	if (offset >= 0x13000 && offset < 0x14000)
		return;
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "heathrow write %05x size %d value %08x\n", offset, size, value);
}

uint32 OsxBusRead(void *context, uint32 address, int size)
{
	OsxMachine *machine;
	machine = (OsxMachine *)context;
	if (address - OSXHEATHROWBASE < OSXHEATHROWSIZE)
		return OsxHeathrowRead(machine, address - OSXHEATHROWBASE, size);
	if (address >= OSXCONFIGDATABASE && address < OSXCONFIGDATABASE + 0x00100000)
		return OsxPciConfigRead(machine, address & 3, size);
	if (address >= OSXCONFIGADDRESSBASE && address < OSXCONFIGADDRESSBASE + 0x00200000)
		return ReadLittleRegister(machine->configaddress, address & 3, size);
	if (address == OSXMACHINEIDADDRESS)
		return 0x00100000;
	if (address - OSXFLASHBASE < OSXFLASHSIZE)
		return OsxFlashRead(machine, address - OSXFLASHBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "bus read %08x size %d pc %08x\n", address, size, machine->cpu->pc);
	return 0xffffffffu >> ((4 - size) * 8);
}

void OsxBusWrite(void *context, uint32 address, int size, uint32 value)
{
	OsxMachine *machine;
	machine = (OsxMachine *)context;
	if (address - OSXHEATHROWBASE < OSXHEATHROWSIZE) {
		OsxHeathrowWrite(machine, address - OSXHEATHROWBASE, size, value);
		return;
	}
	if (address >= OSXCONFIGDATABASE && address < OSXCONFIGDATABASE + 0x00100000) {
		OsxPciConfigWrite(machine, address & 3, size, value);
		return;
	}
	if (address >= OSXCONFIGADDRESSBASE && address < OSXCONFIGADDRESSBASE + 0x00200000) {
		machine->configaddress = WriteLittleRegister(machine->configaddress, address & 3, size, value);
		return;
	}
	if (address - OSXFLASHBASE < OSXFLASHSIZE) {
		OsxFlashWrite(machine, address - OSXFLASHBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "bus write %08x size %d value %08x pc %08x\n", address, size, value, machine->cpu->pc);
}
