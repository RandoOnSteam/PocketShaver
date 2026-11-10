/*
 *  osxbus.cpp - OSX bus, DBDMA, NVRAM, audio, ethernet and display driver emulation
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
#include "osxhost.h"
#include "prefs.h"
#include <math.h>

#define DBDMARUN 0x8000
#define DBDMAPAUSE 0x4000
#define DBDMAFLUSH 0x2000
#define DBDMAWAKE 0x1000
#define DBDMADEAD 0x0800
#define DBDMAACTIVE 0x0400
#define DBDMABRANCH 0x0100
#define DBDMAWRITABLE 0xf0ff
#define DBDMAENDOFFRAME 0x0002

#define DBDMAOUTPUTMORE 0
#define DBDMAOUTPUTLAST 1
#define DBDMAINPUTMORE 2
#define DBDMAINPUTLAST 3
#define DBDMASTOREQUAD 4
#define DBDMALOADQUAD 5
#define DBDMANOP 6
#define DBDMASTOP 7

#define DBDMAMAXSTEPS 4096

static uint32 OsxGuestSwapForSize(uint32 value, int size)
{
	if (size == 4)
		return bswap_32(value);
	if (size == 2)
		return ((value & 0xff) << 8) | ((value >> 8) & 0xff);
	return value & 0xff;
}

static uint32 OsxGuestReadLittleRegister(uint32 reg, uint32 offset, int size)
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

static uint32 OsxGuestWriteLittleRegister(uint32 reg, uint32 offset, int size, uint32 value)
{
	int index;
	uint32 shift;
	for (index = 0; index < size; index++) {
		shift = ((offset + index) & 3) * 8;
		reg = (reg & ~(0xffu << shift)) | (((value >> ((size - 1 - index) * 8)) & 0xff) << shift);
	}
	return reg;
}

static inline uint32 OsxGuestReadGuest32(OsxMachine *machine, uint32 address)
{
	uint8 *host;
#if OSXG5
	if (machine->g5) {
		host = OsxGuestG5DmaHost(machine, address, 4);
		if (host == NULL)
			return 0;
		return ((uint32)host[0] << 24) | ((uint32)host[1] << 16) | ((uint32)host[2] << 8) | host[3];
	}
#endif
	if (address > machine->ramsize - 4)
		return 0;
	host = machine->ram + address;
	return ((uint32)host[0] << 24) | ((uint32)host[1] << 16) | ((uint32)host[2] << 8) | host[3];
}

static inline uint32 OsxGuestReadGuestLittle32(OsxMachine *machine, uint32 address)
{
	return bswap_32(OsxGuestReadGuest32(machine, address));
}

static inline void OsxGuestWriteGuestLittle32(OsxMachine *machine, uint32 address, uint32 value)
{
	uint8 *host;
#if OSXG5
	if (machine->g5) {
		host = OsxGuestG5DmaHost(machine, address, 4);
		if (host == NULL)
			return;
		host[0] = (uint8)value;
		host[1] = (uint8)(value >> 8);
		host[2] = (uint8)(value >> 16);
		host[3] = (uint8)(value >> 24);
		return;
	}
#endif
	if (address > machine->ramsize - 4)
		return;
	host = machine->ram + address;
	host[0] = (uint8)value;
	host[1] = (uint8)(value >> 8);
	host[2] = (uint8)(value >> 16);
	host[3] = (uint8)(value >> 24);
}

void OsxGuestPciReset(OsxMachine *machine)
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

static OsxPciFunction *OsxGuestSelectedFunction(OsxMachine *machine, uint32 *reg)
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

static uint32 OsxGuestConfigDword(OsxPciFunction *function, uint32 reg)
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

uint32 OsxGuestPciConfigRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxPciFunction *function;
	uint32 reg;
	function = OsxGuestSelectedFunction(machine, &reg);
	if (function == NULL)
		return 0xffffffffu >> ((4 - size) * 8);
	return OsxGuestReadLittleRegister(OsxGuestConfigDword(function, reg), offset, size);
}

void OsxGuestPciConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxPciFunction *function;
	uint32 reg;
	uint32 merged;
	function = OsxGuestSelectedFunction(machine, &reg);
	if (function == NULL)
		return;
	merged = OsxGuestWriteLittleRegister(OsxGuestConfigDword(function, reg), offset, size, value);
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

static void OsxGuestUpdateCpuInterrupt(OsxMachine *machine)
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

void OsxGuestPicSetLine(OsxMachine *machine, int source, int level)
{
	OsxPic *pic;
	int bank;
	uint32 bit;
#if OSXG5
	if (machine->g5) {
		OsxGuestG5SetLine(machine, source, level);
		return;
	}
#endif
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
	OsxGuestUpdateCpuInterrupt(machine);
}

static uint32 OsxGuestPicRead(OsxMachine *machine, uint32 offset)
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

static void OsxGuestPicWrite(OsxMachine *machine, uint32 offset, uint32 value)
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
	OsxGuestUpdateCpuInterrupt(machine);
}

static int OsxGuestDbdmaCondition(uint32 status, uint32 select, uint32 mode)
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

static void OsxGuestDbdmaStop(OsxMachine *machine, int channel)
{
	machine->dbdma[channel].status &= ~(DBDMAACTIVE | DBDMAFLUSH);
}

static void OsxGuestDbdmaComplete(OsxMachine *machine, OsxDbdma *dma, uint32 descriptor, uint32 word0, uint32 residual)
{
	uint32 status;
	uint32 command;
	status = dma->status | DBDMAACTIVE | DBDMARUN;
	OsxGuestWriteGuestLittle32(machine, descriptor + 12, (status << 16) | (residual & 0xffff));
	command = (word0 >> 16) & 0xffff;
	if (OsxGuestDbdmaCondition(dma->status, dma->interruptselect, (command >> 4) & 3) && dma->irq >= 0) {
		OsxGuestPicSetLine(machine, dma->irq, 1);
		OsxGuestPicSetLine(machine, dma->irq, 0);
	}
	if (OsxGuestDbdmaCondition(dma->status, dma->branchselect, (command >> 2) & 3)) {
		dma->commandpointer = OsxGuestReadGuestLittle32(machine, descriptor + 8);
		dma->status |= DBDMABRANCH;
	} else {
		dma->commandpointer = descriptor + 16;
		dma->status &= ~DBDMABRANCH;
	}
}

static void OsxGuestDbdmaRunControl(OsxMachine *machine, int channel)
{
	OsxDbdma *dma;
	uint32 descriptor;
	uint32 word0;
	uint32 command;
	uint32 address;
	uint32 value;
	int steps;
	int size;
	dma = &machine->dbdma[channel];
	for (steps = 0; steps < DBDMAMAXSTEPS; steps++) {
		if (!(dma->status & DBDMARUN) || !(dma->status & DBDMAACTIVE) || (dma->status & DBDMAPAUSE))
			return;
		descriptor = dma->commandpointer;
		word0 = OsxGuestReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		address = OsxGuestReadGuestLittle32(machine, descriptor + 4);
		if (command == DBDMASTOP) {
			OsxGuestDbdmaStop(machine, channel);
			return;
		}
		if (command <= DBDMAINPUTLAST)
			return;
		if (command == DBDMASTOREQUAD) {
			value = OsxGuestReadGuestLittle32(machine, descriptor + 8);
			OsxGuestWriteGuestLittle32(machine, address & ~3u, value);
		} else if (command == DBDMALOADQUAD) {
			size = 4;
			if ((word0 & 7) == 2)
				size = 2;
			if (address < machine->ramsize)
				value = OsxGuestReadGuestLittle32(machine, address & ~3u);
			else
				value = OsxGuestBusRead(machine, address, size);
			OsxGuestWriteGuestLittle32(machine, descriptor + 8, value);
		}
		OsxGuestDbdmaComplete(machine, dma, descriptor, word0, 0);
	}
}

int OsxGuestDbdmaTransfer(OsxMachine *machine, int channel, uint8 *data, uint32 length, int towardmemory)
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
		OsxGuestDbdmaRunControl(machine, channel);
		if (!(dma->status & DBDMAACTIVE))
			break;
		descriptor = dma->commandpointer;
		word0 = OsxGuestReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		if (command > DBDMAINPUTLAST)
			break;
		address = OsxGuestReadGuestLittle32(machine, descriptor + 4);
		request = word0 & 0xffff;
		residual = OsxGuestReadGuestLittle32(machine, descriptor + 12) & 0xffff;
		if (residual == 0 || residual > request)
			residual = request;
		done = request - residual;
		chunk = residual;
		if (chunk > length - consumed)
			chunk = length - consumed;
#if OSXG5
		if (machine->g5)
			OsxGuestG5DmaCopy(machine, address + done, data + consumed, chunk, towardmemory);
		else
#endif
		if (address + done + chunk <= machine->ramsize) {
			if (towardmemory) {
				memcpy(machine->ram + address + done, data + consumed, chunk);
				OsxGuestJitInvalidate(machine->cpu, address + done, chunk);
			} else
				memcpy(data + consumed, machine->ram + address + done, chunk);
		}
		consumed += chunk;
		residual -= chunk;
		if (residual != 0) {
			OsxGuestWriteGuestLittle32(machine, descriptor + 12, ((dma->status | DBDMAACTIVE | DBDMARUN) << 16) | residual);
			break;
		}
		OsxGuestDbdmaComplete(machine, dma, descriptor, word0, 0);
	}
	OsxGuestDbdmaRunControl(machine, channel);
	return (int)consumed;
}

int OsxGuestDbdmaOutputFrame(OsxMachine *machine, int channel, uint8 *data, uint32 limit)
{
	OsxDbdma *dma;
	uint32 descriptor;
	uint32 word0;
	uint32 command;
	uint32 address;
	uint32 chunk;
	uint32 consumed;
	int steps;
	dma = &machine->dbdma[channel];
	consumed = 0;
	for (steps = 0; steps < DBDMAMAXSTEPS; steps++) {
		OsxGuestDbdmaRunControl(machine, channel);
		if (!(dma->status & DBDMARUN) || !(dma->status & DBDMAACTIVE))
			return -1;
		descriptor = dma->commandpointer;
		word0 = OsxGuestReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		if (command != DBDMAOUTPUTMORE && command != DBDMAOUTPUTLAST)
			return -1;
		address = OsxGuestReadGuestLittle32(machine, descriptor + 4);
		chunk = word0 & 0xffff;
		if (chunk > limit - consumed)
			chunk = limit - consumed;
		if (address < machine->ramsize && chunk <= machine->ramsize - address)
			memcpy(data + consumed, machine->ram + address, chunk);
		consumed += chunk;
		OsxGuestDbdmaComplete(machine, dma, descriptor, word0, 0);
		if (command == DBDMAOUTPUTLAST) {
			OsxGuestDbdmaRunControl(machine, channel);
			return (int)consumed;
		}
	}
	return -1;
}

int OsxGuestDbdmaInputFrame(OsxMachine *machine, int channel, const uint8 *data, uint32 length)
{
	OsxDbdma *dma;
	uint32 descriptor;
	uint32 word0;
	uint32 command;
	uint32 address;
	uint32 request;
	uint32 chunk;
	uint32 consumed;
	int steps;
	dma = &machine->dbdma[channel];
	OsxGuestDbdmaRunControl(machine, channel);
	if (!(dma->status & DBDMARUN) || !(dma->status & DBDMAACTIVE))
		return 0;
	word0 = OsxGuestReadGuestLittle32(machine, dma->commandpointer);
	command = word0 >> 28;
	if (command != DBDMAINPUTMORE && command != DBDMAINPUTLAST)
		return 0;
	consumed = 0;
	for (steps = 0; steps < DBDMAMAXSTEPS; steps++) {
		descriptor = dma->commandpointer;
		word0 = OsxGuestReadGuestLittle32(machine, descriptor);
		command = word0 >> 28;
		if (command != DBDMAINPUTMORE && command != DBDMAINPUTLAST)
			return 1;
		address = OsxGuestReadGuestLittle32(machine, descriptor + 4);
		request = word0 & 0xffff;
		chunk = request;
		if (chunk > length - consumed)
			chunk = length - consumed;
		if (address < machine->ramsize && chunk <= machine->ramsize - address) {
			memcpy(machine->ram + address, data + consumed, chunk);
			OsxGuestJitInvalidate(machine->cpu, address, chunk);
		}
		consumed += chunk;
		if (consumed == length || command == DBDMAINPUTLAST) {
			if (command == DBDMAINPUTMORE)
				dma->status |= DBDMAENDOFFRAME;
			OsxGuestDbdmaComplete(machine, dma, descriptor, word0, request - chunk);
			dma->status &= ~DBDMAENDOFFRAME;
			OsxGuestDbdmaRunControl(machine, channel);
			return 1;
		}
		OsxGuestDbdmaComplete(machine, dma, descriptor, word0, 0);
		OsxGuestDbdmaRunControl(machine, channel);
		if (!(dma->status & DBDMAACTIVE))
			return 1;
	}
	return 1;
}

void OsxGuestDbdmaDump(OsxMachine *machine, int channel)
{
	uint32 pointer;
	int index;
	pointer = machine->dbdma[channel].commandpointer;
	if (machine->log)
		fprintf(machine->log, "  pic events %08x %08x mask %08x %08x levels %08x %08x external %d msr %08x\n", machine->pic.events[0], machine->pic.events[1], machine->pic.mask[0], machine->pic.mask[1], machine->pic.levels[0], machine->pic.levels[1], machine->cpu->externalline, machine->cpu->msr);
	for (index = -4; index < 4 && machine->log; index++)
		fprintf(machine->log, "  dbdma %08x: %08x %08x %08x %08x\n", pointer + index * 16, OsxGuestReadGuestLittle32(machine, pointer + index * 16), OsxGuestReadGuestLittle32(machine, pointer + index * 16 + 4), OsxGuestReadGuestLittle32(machine, pointer + index * 16 + 8), OsxGuestReadGuestLittle32(machine, pointer + index * 16 + 12));
}

void OsxGuestDbdmaFinish(OsxMachine *machine, int channel)
{
	OsxGuestDbdmaRunControl(machine, channel);
}

void OsxGuestDbdmaKick(OsxMachine *machine, int channel)
{
	OsxGuestDbdmaRunControl(machine, channel);
	if (channel == OSXDBDMAIDE0)
		OsxGuestAtaDmaReady(machine, 0);
	else if (channel == OSXDBDMAIDE1)
		OsxGuestAtaDmaReady(machine, 1);
	else if (channel == OSXDBDMABMACTX)
		OsxGuestBmacTransmit(machine);
	else if (channel == OSXDBDMABMACRX)
		OsxGuestBmacReceive(machine);
}

static uint32 OsxGuestDbdmaRead(OsxMachine *machine, int channel, uint32 reg)
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

static void OsxGuestDbdmaWrite(OsxMachine *machine, int channel, uint32 reg, uint32 value)
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
			OsxGuestDbdmaKick(machine, channel);
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

uint32 OsxGuestHeathrowRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 value;
	if (offset >= 0x10 && offset < 0x30)
		return OsxGuestSwapForSize(OsxGuestPicRead(machine, offset & ~3u), size);
	if (offset >= 0x34 && offset < 0x3c)
		return OsxGuestSwapForSize(machine->heathrowfcr[(offset - 0x34) >> 2], size);
	if (offset >= 0x8000 && offset < 0x9000) {
		value = OsxGuestDbdmaRead(machine, (offset >> 8) & 15, offset & 0xfc);
		return OsxGuestReadLittleRegister(value, offset & 3, size);
	}
	if (offset >= 0x16000 && offset < 0x18000)
		return OsxGuestCudaRead(machine, (offset >> 9) & 15);
	if (offset - OSXBMACOFFSET < OSXBMACSIZE)
		return OsxGuestSwapForSize(OsxGuestBmacRead(machine, offset - OSXBMACOFFSET), size);
	if (offset - OSXAUDIOOFFSET < OSXAUDIOSIZE)
		return OsxGuestSwapForSize(OsxGuestAudioRead(machine, offset - OSXAUDIOOFFSET), size);
	if (offset >= 0x20000 && offset < 0x22000)
		return OsxGuestAtaRead(machine, (offset >> 12) & 1, offset & 0xfff, size);
	if (offset >= 0x60000)
		return machine->nvram[((offset - 0x60000) >> 4) & (OSXNVRAMSIZE - 1)];
	if (offset >= 0x12000 && offset < 0x14000)
		return 0x04;
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "heathrow read %05x size %d\n", offset, size);
	return 0;
}

void OsxGuestHeathrowWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	uint32 reg;
	int channel;
	if (offset >= 0x10 && offset < 0x30) {
		OsxGuestPicWrite(machine, offset & ~3u, OsxGuestSwapForSize(value, size));
		return;
	}
	if (offset >= 0x34 && offset < 0x3c) {
		machine->heathrowfcr[(offset - 0x34) >> 2] = OsxGuestSwapForSize(value, size);
		return;
	}
	if (offset >= 0x8000 && offset < 0x9000) {
		channel = (offset >> 8) & 15;
		reg = OsxGuestWriteLittleRegister(OsxGuestDbdmaRead(machine, channel, offset & 0xfc), offset & 3, size, value);
		if ((offset & 0xfc) == 0)
			reg = OsxGuestWriteLittleRegister(0, offset & 3, size, value);
		OsxGuestDbdmaWrite(machine, channel, offset & 0xfc, reg);
		return;
	}
	if (offset >= 0x16000 && offset < 0x18000) {
		OsxGuestCudaWrite(machine, (offset >> 9) & 15, value & 0xff);
		return;
	}
	if (offset - OSXBMACOFFSET < OSXBMACSIZE) {
		OsxGuestBmacWrite(machine, offset - OSXBMACOFFSET, OsxGuestSwapForSize(value, size));
		return;
	}
	if (offset - OSXAUDIOOFFSET < OSXAUDIOSIZE) {
		OsxGuestAudioWrite(machine, offset - OSXAUDIOOFFSET, OsxGuestSwapForSize(value, size));
		return;
	}
	if (offset >= 0x20000 && offset < 0x22000) {
		OsxGuestAtaWrite(machine, (offset >> 12) & 1, offset & 0xfff, size, value);
		return;
	}
	if (offset >= 0x60000) {
		machine->nvram[((offset - 0x60000) >> 4) & (OSXNVRAMSIZE - 1)] = (uint8)value;
		return;
	}
	if (offset >= 0x12000 && offset < 0x14000)
		return;
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "heathrow write %05x size %d value %08x\n", offset, size, value);
}

uint32 OsxGuestBusRead(void *context, uint32 address, int size)
{
	OsxMachine *machine;
	machine = (OsxMachine *)context;
#if OSXG5
	if (machine->g5)
		return OsxGuestG5BusRead(machine, address, size);
#endif
	if (address - OSXHEATHROWBASE < OSXHEATHROWSIZE)
		return OsxGuestHeathrowRead(machine, address - OSXHEATHROWBASE, size);
	if (address >= OSXCONFIGDATABASE && address < OSXCONFIGDATABASE + 0x00100000)
		return OsxGuestPciConfigRead(machine, address & 3, size);
	if (address >= OSXCONFIGADDRESSBASE && address < OSXCONFIGADDRESSBASE + 0x00200000)
		return OsxGuestReadLittleRegister(machine->configaddress, address & 3, size);
	if (address == OSXMACHINEIDADDRESS)
		return 0x00100000;
	if (address - OSXFLASHBASE < OSXFLASHSIZE)
		return OsxGuestFlashRead(machine, address - OSXFLASHBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "bus read %08x size %d pc %08x\n", address, size, machine->cpu->pc);
	return 0xffffffffu >> ((4 - size) * 8);
}

void OsxGuestBusWrite(void *context, uint32 address, int size, uint32 value)
{
	OsxMachine *machine;
	machine = (OsxMachine *)context;
#if OSXG5
	if (machine->g5) {
		OsxGuestG5BusWrite(machine, address, size, value);
		return;
	}
#endif
	if (address - OSXHEATHROWBASE < OSXHEATHROWSIZE) {
		OsxGuestHeathrowWrite(machine, address - OSXHEATHROWBASE, size, value);
		return;
	}
	if (address >= OSXCONFIGDATABASE && address < OSXCONFIGDATABASE + 0x00100000) {
		OsxGuestPciConfigWrite(machine, address & 3, size, value);
		return;
	}
	if (address >= OSXCONFIGADDRESSBASE && address < OSXCONFIGADDRESSBASE + 0x00200000) {
		machine->configaddress = OsxGuestWriteLittleRegister(machine->configaddress, address & 3, size, value);
		return;
	}
	if (address - OSXFLASHBASE < OSXFLASHSIZE) {
		OsxGuestFlashWrite(machine, address - OSXFLASHBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "bus write %08x size %d value %08x pc %08x\n", address, size, value, machine->cpu->pc);
}

#define FLASHREADARRAY 0
#define FLASHSTATUS 1
#define FLASHERASESETUP 2
#define FLASHPROGRAMSETUP 3
#define FLASHREADY 0x80

#define NVRAMSIZE 0x2000
#define NVRAMXPRAMHEADER 0x12f0
#define NVRAMCOMMONHEADER 0x1400

static int OsxGuestFindPartition(const uint8 *nvram, const char *name, uint32 *start, uint32 *length)
{
	uint32 offset;
	uint32 size;
	offset = 0;
	while (offset + 16 <= NVRAMSIZE && nvram[offset] != 0) {
		size = (((uint32)nvram[offset + 2] << 8) | nvram[offset + 3]) * 16;
		if (size < 16 || offset + size > NVRAMSIZE)
			return 0;
		if (strncmp((const char *)nvram + offset + 4, name, 12) == 0) {
			*start = offset + 16;
			*length = size - 16;
			return 1;
		}
		offset += size;
	}
	return 0;
}

static const char *OsxGuestVariableName(int index)
{
	if (index == 9)
		return "boot-command";
	return "boot-device";
}

static void OsxGuestReadCommonString(const uint8 *nvram, const char *name, char *output, int size)
{
	uint32 start;
	uint32 length;
	uint32 cursor;
	uint32 namelength;
	const char *entry;
	output[0] = 0;
	if (!OsxGuestFindPartition(nvram, "common", &start, &length))
		return;
	namelength = (uint32)strlen(name);
	for (cursor = start; cursor < start + length && nvram[cursor] != 0; cursor += (uint32)strlen(entry) + 1) {
		entry = (const char *)nvram + cursor;
		if (strncmp(entry, name, namelength) == 0 && entry[namelength] == '=' && strlen(entry + namelength + 1) < (size_t)size) {
			strcpy(output, entry + namelength + 1);
			return;
		}
	}
}

int OsxGuestNvramIsNewWorld(const uint8 *nvram)
{
	uint32 start;
	uint32 length;
	return OsxGuestFindPartition(nvram, "common", &start, &length);
}

void OsxGuestNvramReadString(const uint8 *nvram, int index, char *output, int size)
{
	uint32 header = 0x1834 + index * 4;
	uint32 offset = ((uint32)nvram[header] << 8) | nvram[header + 1];
	uint32 length = ((uint32)nvram[header + 2] << 8) | nvram[header + 3];
	output[0] = 0;
	if (OsxGuestNvramIsNewWorld(nvram)) {
		OsxGuestReadCommonString(nvram, OsxGuestVariableName(index), output, size);
		return;
	}
	if (offset < 0x185c || offset + length > 0x2000 || length >= (uint32)size)
		return;
	memcpy(output, nvram + offset, length);
	output[length] = 0;
}

static void OsxGuestWritePartitionHeader(uint8 *nvram, uint32 offset, uint8 signature, uint32 size, const char *name)
{
	uint32 sum;
	int index;
	memset(nvram + offset, 0, 16);
	nvram[offset] = signature;
	nvram[offset + 2] = (uint8)((size / 16) >> 8);
	nvram[offset + 3] = (uint8)(size / 16);
	strncpy((char *)nvram + offset + 4, name, 12);
	sum = signature;
	for (index = 2; index < 16; index++) {
		sum += nvram[offset + index];
		if (sum > 0xff)
			sum = (sum + 1) & 0xff;
	}
	nvram[offset + 1] = (uint8)sum;
}

static int OsxGuestSetCommonBoot(uint8 *nvram, const char *device, const char *command)
{
	uint8 data[NVRAMSIZE];
	uint32 start;
	uint32 length;
	uint32 cursor;
	uint32 used;
	uint32 entrylength;
	const char *entry;
	if (!OsxGuestFindPartition(nvram, "common", &start, &length))
		return 0;
	used = 0;
	used += (uint32)sprintf((char *)data + used, "boot-device=%s", device) + 1;
	used += (uint32)sprintf((char *)data + used, "boot-command=%s", command) + 1;
	for (cursor = start; cursor < start + length && nvram[cursor] != 0; cursor += entrylength + 1) {
		entry = (const char *)nvram + cursor;
		entrylength = (uint32)strlen(entry);
		if (strncmp(entry, "boot-device=", 12) == 0 || strncmp(entry, "boot-command=", 13) == 0)
			continue;
		if (used + entrylength + 1 >= length)
			return 0;
		memcpy(data + used, entry, entrylength + 1);
		used += entrylength + 1;
	}
	if (used >= length)
		return 0;
	memset(nvram + start, 0, length);
	memcpy(nvram + start, data, used);
	return 1;
}

void OsxGuestNvramMakeNewWorld(uint8 *nvram)
{
	char device[256];
	char command[256];
	if (OsxGuestNvramIsNewWorld(nvram))
		return;
	OsxGuestNvramReadString(nvram, 0, device, sizeof(device));
	OsxGuestNvramReadString(nvram, 9, command, sizeof(command));
	memset(nvram, 0, NVRAMXPRAMHEADER);
	OsxGuestWritePartitionHeader(nvram, 0, 0x7f, NVRAMXPRAMHEADER, "wwwwwwwwwwww");
	OsxGuestWritePartitionHeader(nvram, NVRAMXPRAMHEADER, 0x5a, NVRAMCOMMONHEADER - NVRAMXPRAMHEADER, "APL,MacOS75");
	memset(nvram + NVRAMCOMMONHEADER, 0, NVRAMSIZE - NVRAMCOMMONHEADER);
	OsxGuestWritePartitionHeader(nvram, NVRAMCOMMONHEADER, 0x70, NVRAMSIZE - NVRAMCOMMONHEADER, "common");
	OsxGuestSetCommonBoot(nvram, device, command);
}

int OsxGuestNvramWantsClassic(const char *device, const char *command)
{
	if (strstr(device, "AAPL,ROM") != NULL)
		return 1;
	return strstr(command, "mac-boot") != NULL && strstr(device, ":tbxi") == NULL;
}

static int OsxGuestSetFirmwareBoot(uint8 *nvram, const char *device, const char *command)
{
	uint8 block[0x800];
	const uint8 *strings[10];
	uint32 lengths[10];
	uint32 index;
	uint32 header;
	uint32 offset;
	uint32 total = 0;
	uint32 cursor = 0x2000;
	uint32 checksum = 0;
	uint32 start = ((uint32)nvram[0x1806] << 8) | nvram[0x1807];
	if (OsxGuestNvramIsNewWorld(nvram))
		return OsxGuestSetCommonBoot(nvram, device, command);
	if (start < 0x185c || start > 0x2000)
		return 0;
	memcpy(block, nvram + 0x1800, sizeof(block));
	for (index = 0; index < 10; index++) {
		header = 0x34 + index * 4;
		offset = ((uint32)block[header] << 8) | block[header + 1];
		lengths[index] = ((uint32)block[header + 2] << 8) | block[header + 3];
		strings[index] = block;
		if (index == 0) {
			strings[index] = (const uint8 *)device;
			lengths[index] = strlen(device);
		} else if (index == 9) {
			strings[index] = (const uint8 *)command;
			lengths[index] = strlen(command);
		} else if (offset >= start && offset + lengths[index] <= 0x2000)
			strings[index] = block + offset - 0x1800;
		else
			lengths[index] = 0;
		total += lengths[index];
	}
	if (total > 0x2000 - start)
		return 0;
	memset(nvram + start, 0, 0x2000 - start);
	for (index = 0; index < 10; index++) {
		cursor -= lengths[index];
		header = 0x1834 + index * 4;
		nvram[header] = cursor >> 8;
		nvram[header + 1] = cursor & 0xff;
		nvram[header + 2] = lengths[index] >> 8;
		nvram[header + 3] = lengths[index] & 0xff;
		memcpy(nvram + cursor, strings[index], lengths[index]);
	}
	nvram[0x1808] = cursor >> 8;
	nvram[0x1809] = cursor & 0xff;
	nvram[0x1804] = 0;
	nvram[0x1805] = 0;
	for (index = 0x1800; index < 0x2000; index += 2)
		checksum += ((uint32)nvram[index] << 8) | nvram[index + 1];
	checksum = 0xffff - checksum % 0xffff;
	nvram[0x1804] = checksum >> 8;
	nvram[0x1805] = checksum & 0xff;
	return 1;
}

void OsxGuestNvramRestoreOldWorld(uint8 *nvram, const uint8 *classic)
{
	char device[256];
	char command[256];
	uint8 xpram[OSXPRAMSIZE];
	if (OsxGuestNvramIsNewWorld(classic) || !OsxGuestNvramIsNewWorld(nvram))
		return;
	OsxGuestNvramReadString(nvram, 0, device, sizeof(device));
	OsxGuestNvramReadString(nvram, 9, command, sizeof(command));
	memcpy(xpram, nvram + OSXPRAMOFFSET, OSXPRAMSIZE);
	memcpy(nvram, classic, NVRAMSIZE);
	memcpy(nvram + OSXPRAMOFFSET, xpram, OSXPRAMSIZE);
	OsxGuestSetFirmwareBoot(nvram, device, command);
}

int OsxGuestNvramPrepareBoot(OsxMachine *machine, int bus, int unit, int partition)
{
	char device[256];
	char command[256];
	OsxGuestNvramReadString(machine->nvram, 0, device, sizeof(device));
	OsxGuestNvramReadString(machine->nvram, 9, command, sizeof(command));
	if (device[0] != 0 && !OsxGuestNvramWantsClassic(device, command))
		return 1;
	sprintf(device, "pci/mac-io@10/ata@%x/@%x:%d", 0x20000 + bus * 0x1000, unit, partition);
	return OsxGuestSetFirmwareBoot(machine->nvram, device, "0 bootr");
}

void OsxGuestNvramFinishBoot(OsxMachine *machine, const uint8 *startup)
{
	char device[256];
	char command[256];
	OsxGuestNvramReadString(machine->nvram, 0, device, sizeof(device));
	OsxGuestNvramReadString(machine->nvram, 9, command, sizeof(command));
	if (OsxGuestNvramWantsClassic(device, command) &&
		memcmp(machine->nvram + 0x1378, startup, 4) == 0)
		memset(machine->nvram + 0x1378, 0, 4);
}

void OsxGuestFlashLoad(OsxMachine *machine, const char *path)
{
	FILE *file;
	strncpy(machine->nvrampath, path, sizeof(machine->nvrampath) - 1);
	machine->flashmode = FLASHREADARRAY;
	machine->flashdirty = 0;
	file = fopen(path, "rb");
	if (file == NULL || fread(machine->flash, 1, OSXFLASHSIZE, file) != OSXFLASHSIZE)
		memset(machine->flash, 0, OSXFLASHSIZE);
	if (file)
		fclose(file);
}

void OsxGuestFlashSave(OsxMachine *machine)
{
	FILE *file;
	if (!machine->flashdirty || machine->nvrampath[0] == 0)
		return;
	file = fopen(machine->nvrampath, "wb");
	if (file == NULL)
		return;
	fwrite(machine->flash, 1, OSXFLASHSIZE, file);
	fclose(file);
	machine->flashdirty = 0;
}

uint32 OsxGuestFlashRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 value;
	int index;
	if (machine->flashmode != FLASHREADARRAY)
		return FLASHREADY;
	value = 0;
	for (index = 0; index < size; index++)
		value = (value << 8) | machine->flash[(offset + index) & (OSXFLASHSIZE - 1)];
	return value;
}

void OsxGuestFlashWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	uint32 bank;
	uint8 data;
	offset &= OSXFLASHSIZE - 1;
	data = (uint8)value;
	switch (machine->flashmode) {
	case FLASHERASESETUP:
		if (data == 0xd0) {
			bank = offset & ~(uint32)(OSXFLASHBANK - 1);
			memset(machine->flash + bank, 0xff, OSXFLASHBANK);
			machine->flashdirty = 1;
		}
		machine->flashmode = FLASHSTATUS;
		return;
	case FLASHPROGRAMSETUP:
		machine->flash[offset] &= data;
		machine->flashdirty = 1;
		machine->flashmode = FLASHSTATUS;
		return;
	}
	switch (data) {
	case 0x20:
		machine->flashmode = FLASHERASESETUP;
		break;
	case 0x40:
	case 0x10:
		machine->flashmode = FLASHPROGRAMSETUP;
		break;
	case 0x70:
	case 0x50:
		machine->flashmode = FLASHSTATUS;
		break;
	default:
		machine->flashmode = FLASHREADARRAY;
		break;
	}
}

#define AUDIOCONTROL 0x00
#define AUDIOCODECCONTROL 0x10
#define AUDIOCODECSTATUS 0x20
#define AUDIOCLIPCOUNT 0x30
#define AUDIOBYTESWAP 0x40
#define AUDIOCODECBUSY 0x01000000
#define AUDIOSTATUSVALID 0x00400000
#define AUDIOSTATUSSCREAMER 0x00003100
#define AUDIOREADBACKREGISTER 7
#define AUDIOMUTEREGISTER 1
#define AUDIOHEADPHONEREGISTER 2
#define AUDIOSPEAKERREGISTER 4
#define AUDIOMUTEHEADPHONE 0x200
#define AUDIOMUTESPEAKER 0x080
#define AUDIOLATENCYDIVISOR 10
#define AUDIOFRAMEBYTES 4
#define AUDIOPOLLTICKS (OSXTIMEBASEFREQUENCY / 1000)

static uint32 OsxGuestSampleRate(uint32 control)
{
	const uint32 rates[8] = {44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350};
	return rates[(control >> 8) & 7];
}

static int32 OsxGuestAttenuationGain(uint32 steps)
{
	return (int32)(pow(10.0, -1.5 * (double)steps / 20.0) * 65536.0);
}

static void OsxGuestUpdateGain(OsxAudio *audio)
{
	uint32 mute;
	uint32 volume;
	mute = audio->codec[AUDIOMUTEREGISTER];
	if (!(mute & AUDIOMUTESPEAKER))
		volume = audio->codec[AUDIOSPEAKERREGISTER];
	else if (!(mute & AUDIOMUTEHEADPHONE))
		volume = audio->codec[AUDIOHEADPHONEREGISTER];
	else {
		audio->leftgain = 0;
		audio->rightgain = 0;
		return;
	}
	audio->leftgain = OsxGuestAttenuationGain((volume >> 6) & 15);
	audio->rightgain = OsxGuestAttenuationGain(volume & 15);
}

static void OsxGuestOpenHost(OsxMachine *machine)
{
	OsxAudio *audio;
	audio = &machine->audio;
	OsxGuestAudioCloseHost(audio);
	audio->head = 0;
	audio->tail = 0;
	audio->openrate = audio->rate;
	audio->stream = OsxGuestAudioStartHost(audio);
	if (audio->stream == NULL) {
		if (machine->log)
			fprintf(machine->log, "audio open failed\n");
		audio->disabled = 1;
		return;
	}
	if (machine->log)
		fprintf(machine->log, "audio opened %u Hz\n", audio->rate);
}

void OsxGuestAudioReset(OsxMachine *machine)
{
	OsxAudio *audio;
	audio = &machine->audio;
	OsxGuestAudioCloseHost(audio);
	audio->running = 0;
	audio->control = 0;
	audio->codeccontrol = 0;
	memset(audio->codec, 0, sizeof(audio->codec));
	audio->readback = 0;
	audio->byteswap = 0;
	audio->rate = OsxGuestSampleRate(0);
	audio->openrate = 0;
	audio->disabled = PrefsFindBool("nosound");
	audio->head = 0;
	audio->tail = 0;
	audio->lasttime = 0;
	OsxGuestUpdateGain(audio);
	machine->dbdma[OSXDBDMAAUDIOOUT].irq = OSXIRQAUDIOOUTDMA;
	machine->dbdma[OSXDBDMAAUDIOIN].irq = OSXIRQAUDIOINDMA;
}

void OsxGuestAudioExit(OsxMachine *machine)
{
	OsxGuestAudioCloseHost(&machine->audio);
}

uint32 OsxGuestAudioRead(OsxMachine *machine, uint32 offset)
{
	OsxAudio *audio;
	audio = &machine->audio;
	switch (offset & 0xf0) {
	case AUDIOCONTROL:
		return audio->control;
	case AUDIOCODECCONTROL:
		return audio->codeccontrol & ~AUDIOCODECBUSY;
	case AUDIOCODECSTATUS:
		if (audio->readback)
			return AUDIOSTATUSVALID | ((audio->codec[audio->readback - 1] & 0xfff) << 4);
		return AUDIOSTATUSVALID | AUDIOSTATUSSCREAMER;
	case AUDIOBYTESWAP:
		return audio->byteswap;
	}
	return 0;
}

void OsxGuestAudioWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxAudio *audio;
	uint32 address;
	uint32 data;
	audio = &machine->audio;
	switch (offset & 0xf0) {
	case AUDIOCONTROL:
		audio->control = value;
		audio->rate = OsxGuestSampleRate(value);
		break;
	case AUDIOCODECCONTROL:
		audio->codeccontrol = value & ~AUDIOCODECBUSY;
		address = (value >> 12) & 0x3ff;
		data = value & 0xfff;
		if (address == AUDIOREADBACKREGISTER) {
			audio->readback = 0;
			if (data & 1)
				audio->readback = ((data >> 1) & 7) + 1;
			break;
		}
		if (address < 8) {
			audio->codec[address] = data;
			OsxGuestUpdateGain(audio);
		}
		break;
	case AUDIOBYTESWAP:
		audio->byteswap = value;
		break;
	}
}

static void OsxGuestStoreBlock(OsxAudio *audio, uint32 length)
{
	uint8 *data;
	uint32 index;
	uint32 position;
	int32 sample;
	int32 gain;
	data = audio->block;
	for (index = 0; index + 1 < length; index += 2) {
		if (audio->byteswap & 1)
			sample = (int16)(data[index] | (data[index + 1] << 8));
		else
			sample = (int16)((data[index] << 8) | data[index + 1]);
		gain = audio->leftgain;
		if (index & 2)
			gain = audio->rightgain;
		sample = (int32)(((int64)sample * gain) >> 16);
		position = audio->head + index;
		audio->ring[position & (OSXAUDIORING - 1)] = (uint8)(sample >> 8);
		audio->ring[(position + 1) & (OSXAUDIORING - 1)] = (uint8)sample;
	}
}

void OsxGuestAudioOpenForCdrom(OsxMachine *machine)
{
	if (!machine->audio.disabled && !machine->audio.stream)
		OsxGuestOpenHost(machine);
}

void OsxGuestAudioPoll(OsxMachine *machine, uint64 now)
{
	OsxAudio *audio;
	uint32 target;
	uint32 level;
	uint32 want;
	int got;
	int channel;
	audio = &machine->audio;
	channel = OSXDBDMAAUDIOOUT;
#if OSXG5
	if (machine->g5)
		channel = OSXG5DBDMAI2SOUT;
#endif
	if (!(machine->dbdma[channel].status & OSXDBDMASTATUSACTIVE)) {
		audio->running = 0;
		audio->lasttime = now;
		return;
	}
	if (!audio->disabled && (!audio->stream || audio->openrate != audio->rate))
		OsxGuestOpenHost(machine);
	level = 0;
	if (audio->stream) {
		if (audio->running && now < audio->lasttime)
			return;
		audio->lasttime = now + AUDIOPOLLTICKS;
		target = audio->rate * AUDIOFRAMEBYTES / AUDIOLATENCYDIVISOR;
		OsxGuestAudioLockHost(audio);
		level = audio->head - audio->tail;
		while (!audio->running && level + AUDIOFRAMEBYTES <= target) {
			memset(audio->ring + (audio->head & (OSXAUDIORING - 1)), 0, AUDIOFRAMEBYTES);
			audio->head += AUDIOFRAMEBYTES;
			level += AUDIOFRAMEBYTES;
		}
		OsxGuestAudioUnlockHost(audio);
		audio->running = 1;
	} else {
		if (now <= audio->lasttime)
			return;
		target = (uint32)((now - audio->lasttime) * audio->rate / OSXTIMEBASEFREQUENCY) * AUDIOFRAMEBYTES;
		if (target < OSXAUDIOBLOCK)
			return;
		audio->lasttime = now;
	}
	while (level + AUDIOFRAMEBYTES <= target) {
		want = (target - level) & ~(uint32)(AUDIOFRAMEBYTES - 1);
		if (want > OSXAUDIOBLOCK)
			want = OSXAUDIOBLOCK;
		got = OsxGuestDbdmaTransfer(machine, channel, audio->block, want, 0);
		if (got <= 0)
			return;
#if OSXG5
		machine->g5state.i2sframes += (uint32)got / AUDIOFRAMEBYTES;
#endif
		if (audio->stream) {
			OsxGuestAudioLockHost(audio);
			OsxGuestStoreBlock(audio, (uint32)got);
			audio->head += (uint32)got;
			OsxGuestAudioUnlockHost(audio);
		}
		level += (uint32)got;
	}
}

#define BMACSTAT 0x200
#define BMACINTDISABLE 0x210
#define BMACTXRST 0x420
#define BMACTXCFG 0x430
#define BMACRXCFG 0x630
#define BMACXCVRIF 0x160
#define BMACCHIPID 0x170
#define BMACMIFCSR 0x180
#define BMACSROMCSR 0x190
#define BMACFRAMERECEIVED 0x0001
#define BMACFRAMESENT 0x0100
#define BMACTXENABLE 0x0001
#define BMACRXENABLE 0x0001
#define BMACLINKDOWN 0x0100
#define BMACMIFDATAIN 0x0008
#define BMACCHIPHEATHROW 0xb0
#define BMACSROMSELECT 0x0001
#define BMACSROMCLOCK 0x0002
#define BMACSROMDATAOUT 0x0004
#define BMACSROMDATAIN 0x0008
#define BMACSROMADDRESSBITS 6
#define BMACSROMREAD 6
#define BMACSROMMACWORD 10
#define BMACMINFRAME 60
#define BMACMAXFRAME 1514
#define BMACBUSYPOLL (OSXTIMEBASEFREQUENCY / 1000)
#define BMACIDLEPOLL (OSXTIMEBASEFREQUENCY / 100)
#define BMACBUSYPERIOD (OSXTIMEBASEFREQUENCY * 2)

static const uint8 BmacAddress[6] = { 0x00, 0x05, 0x02, 0x4f, 0x53, 0x58 };

static uint8 OsxGuestReverseBits(uint8 value)
{
	uint8 result;
	int index;
	result = 0;
	for (index = 0; index < 8; index++) {
		result = (uint8)((result << 1) | (value & 1));
		value >>= 1;
	}
	return result;
}

void OsxGuestBmacHardwareAddress(uint8 *address)
{
	memcpy(address, BmacAddress, sizeof(BmacAddress));
}

void OsxGuestBmacReset(OsxMachine *machine)
{
	OsxBmac *bmac;
	int index;
	bmac = &machine->bmac;
	memset(bmac->registers, 0, sizeof(bmac->registers));
	memset(bmac->srom, 0xff, sizeof(bmac->srom));
	for (index = 0; index < 3; index++)
		bmac->srom[BMACSROMMACWORD + index] = (uint16)(OsxGuestReverseBits(BmacAddress[index * 2]) | (OsxGuestReverseBits(BmacAddress[index * 2 + 1]) << 8));
	bmac->status = 0;
	bmac->srombits = 0;
	bmac->sromcommand = 0;
	bmac->sromoutput = 0;
	bmac->sromclock = 0;
	bmac->sromdata = 0;
	bmac->pendinglength = 0;
	machine->dbdma[OSXDBDMABMACTX].irq = OSXIRQBMACTXDMA;
	machine->dbdma[OSXDBDMABMACRX].irq = OSXIRQBMACRXDMA;
	if (!bmac->attached && PrefsFindString("ether") && strcmp(PrefsFindString("ether"), "slirp") == 0) {
		bmac->slirpthread = OsxGuestHostNetOpen(BmacAddress);
		bmac->attached = bmac->slirpthread != NULL;
	}
	if (machine->log)
		fprintf(machine->log, "bmac backend attached %d\n", bmac->attached);
}

static int OsxGuestBackendReady(OsxBmac *bmac)
{
	int state;
	if (bmac->ready || !bmac->attached)
		return bmac->ready;
	state = OsxGuestHostNetReady(bmac->slirpthread);
	if (state > 0)
		bmac->ready = 1;
	else if (state < 0)
		bmac->attached = 0;
	return bmac->ready;
}

static void OsxGuestSromWrite(OsxBmac *bmac, uint16 value)
{
	int rising;
	if (!(value & BMACSROMSELECT)) {
		bmac->srombits = 0;
		bmac->sromcommand = 0;
		bmac->sromoutput = 0;
		bmac->sromclock = 0;
		bmac->sromdata = 0;
		return;
	}
	rising = (value & BMACSROMCLOCK) && !bmac->sromclock;
	bmac->sromclock = (value & BMACSROMCLOCK) != 0;
	if (!rising)
		return;
	if (bmac->sromoutput) {
		bmac->sromdata = (bmac->sromword >> 15) & 1;
		bmac->sromword = (uint16)(bmac->sromword << 1);
		return;
	}
	bmac->sromcommand = (bmac->sromcommand << 1) | ((value & BMACSROMDATAIN) != 0);
	bmac->srombits++;
	if (bmac->srombits == 3 + BMACSROMADDRESSBITS) {
		if ((bmac->sromcommand >> BMACSROMADDRESSBITS) == BMACSROMREAD) {
			bmac->sromword = bmac->srom[bmac->sromcommand & ((1 << BMACSROMADDRESSBITS) - 1)];
			bmac->sromoutput = 1;
		}
	}
}

static void OsxGuestRaiseDevice(OsxMachine *machine, uint16 event)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	bmac->status |= event;
	if (bmac->registers[BMACINTDISABLE >> 4] & event)
		return;
	OsxGuestPicSetLine(machine, OSXIRQBMAC, 1);
	OsxGuestPicSetLine(machine, OSXIRQBMAC, 0);
}

uint32 OsxGuestBmacRead(OsxMachine *machine, uint32 offset)
{
	OsxBmac *bmac;
	uint32 value;
	bmac = &machine->bmac;
	offset &= 0xff0;
	switch (offset) {
	case BMACSTAT:
		value = bmac->status;
		bmac->status = 0;
		return value;
	case BMACTXRST:
		return 0;
	case BMACXCVRIF:
		value = bmac->registers[offset >> 4] | BMACLINKDOWN;
		if (bmac->attached)
			value &= ~BMACLINKDOWN;
		return value;
	case BMACCHIPID:
		return BMACCHIPHEATHROW;
	case BMACMIFCSR:
		return bmac->registers[offset >> 4] | BMACMIFDATAIN;
	case BMACSROMCSR:
		value = bmac->registers[offset >> 4] & ~BMACSROMDATAOUT;
		if (bmac->sromdata)
			value |= BMACSROMDATAOUT;
		return value;
	}
	return bmac->registers[offset >> 4];
}

void OsxGuestBmacWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	offset &= 0xff0;
	bmac->registers[offset >> 4] = (uint16)value;
	if (offset == BMACSROMCSR)
		OsxGuestSromWrite(bmac, (uint16)value);
	else if (offset == BMACRXCFG && (value & BMACRXENABLE))
		OsxGuestBmacReceive(machine);
}

#if OSXNETTRACE
static void OsxGuestNetTrace(OsxMachine *machine, const char *dir, const uint8 *f, int n)
{
	int type;
	int ihl;
	int proto;
	int sport;
	int dport;
	int i;
	if (!machine->log || n < 14)
		return;
	type = (f[12] << 8) | f[13];
	fprintf(machine->log, "NET %s t=%llu len %d %02x:%02x:%02x:%02x:%02x:%02x<-%02x:%02x:%02x:%02x:%02x:%02x type %04x", dir, (unsigned long long)OsxGuestCpuTimebase(machine->cpu), n, f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11], type);
	if (type == 0x0806 && n >= 42)
		fprintf(machine->log, " arp op %d sha %02x:%02x:%02x:%02x:%02x:%02x spa %d.%d.%d.%d tpa %d.%d.%d.%d", f[21], f[22],f[23],f[24],f[25],f[26],f[27], f[28],f[29],f[30],f[31], f[38],f[39],f[40],f[41]);
	if (type == 0x0800 && n >= 34) {
		ihl = (f[14] & 15) * 4;
		proto = f[23];
		fprintf(machine->log, " ip %d.%d.%d.%d->%d.%d.%d.%d proto %d", f[26],f[27],f[28],f[29],f[30],f[31],f[32],f[33], proto);
		if ((proto == 6 || proto == 17) && n >= 14 + ihl + 4) {
			sport = (f[14 + ihl] << 8) | f[15 + ihl];
			dport = (f[16 + ihl] << 8) | f[17 + ihl];
			fprintf(machine->log, " ports %d->%d", sport, dport);
			if (proto == 6 && n >= 14 + ihl + 14)
				fprintf(machine->log, " flags %02x", f[14 + ihl + 13]);
			if (proto == 17 && (sport == 53 || dport == 53) && n >= 14 + ihl + 20) {
				fprintf(machine->log, " dns id %02x%02x flags %02x%02x qd %d an %d name ", f[22 + ihl], f[23 + ihl], f[24 + ihl], f[25 + ihl], f[27 + ihl], f[29 + ihl]);
				for (i = 14 + ihl + 20; i < n && f[i]; i++) {
					if (f[i] < 32)
						fputc('.', machine->log);
					else
						fputc(f[i], machine->log);
				}
				if (i + 2 < n)
					fprintf(machine->log, " qtype %d", (f[i + 1] << 8) | f[i + 2]);
			}
			if (proto == 17 && (sport == 67 || sport == 68) && n >= 14 + ihl + 8 + 240) {
				fprintf(machine->log, " bootp op %d yiaddr %d.%d.%d.%d opts", f[14 + ihl + 8], f[14+ihl+8+16],f[14+ihl+8+17],f[14+ihl+8+18],f[14+ihl+8+19]);
				for (i = 14 + ihl + 8 + 240; i + 1 < n && f[i] != 255; i += 2 + f[i + 1]) {
					if (f[i] == 0) {
						i -= 1;
						continue;
					}
					fprintf(machine->log, " %d", f[i]);
					if ((f[i] == 53 || f[i] == 55) && i + 2 + f[i + 1] <= n) {
						int k;
						fprintf(machine->log, "[");
						for (k = 0; k < f[i + 1]; k++)
							fprintf(machine->log, "%d,", f[i + 2 + k]);
						fprintf(machine->log, "]");
					}
				}
			}
		}
	}
	fprintf(machine->log, "\n");
}
#endif

void OsxGuestBmacTransmit(OsxMachine *machine)
{
	OsxBmac *bmac;
	int length;
	bmac = &machine->bmac;
	for (;;) {
		length = OsxGuestDbdmaOutputFrame(machine, OSXDBDMABMACTX, bmac->frame, sizeof(bmac->frame));
		if (length < 0)
			return;
		if (OsxGuestBackendReady(bmac) && (bmac->registers[BMACTXCFG >> 4] & BMACTXENABLE) && length >= 14) {
#if OSXNETTRACE
			OsxGuestNetTrace(machine, "tx", bmac->frame, length);
#endif
			OsxGuestHostNetSend(bmac->slirpthread, bmac->frame, length);
			bmac->busydeadline = OsxGuestCpuTimebase(machine->cpu) + BMACBUSYPERIOD;
		}
		OsxGuestRaiseDevice(machine, BMACFRAMESENT);
	}
}

void OsxGuestBmacReceive(OsxMachine *machine)
{
	OsxBmac *bmac;
	int length;
	bmac = &machine->bmac;
	while (OsxGuestBackendReady(bmac) && (bmac->registers[BMACRXCFG >> 4] & BMACRXENABLE)) {
		if (bmac->pendinglength == 0) {
			length = OsxGuestHostNetReceive(bmac->slirpthread, bmac->pending);
			if (length <= 0)
				return;
#if OSXNETTRACE
			OsxGuestNetTrace(machine, "rx", bmac->pending, length);
#endif
			if (length > BMACMAXFRAME)
				length = BMACMAXFRAME;
			if (length < BMACMINFRAME) {
				memset(bmac->pending + length, 0, BMACMINFRAME - length);
				length = BMACMINFRAME;
			}
			bmac->pending[length] = (uint8)(length >> 8);
			bmac->pending[length + 1] = (uint8)length;
			bmac->pendinglength = length + 2;
			bmac->busydeadline = OsxGuestCpuTimebase(machine->cpu) + BMACBUSYPERIOD;
		}
		if (!OsxGuestDbdmaInputFrame(machine, OSXDBDMABMACRX, bmac->pending, (uint32)bmac->pendinglength))
			return;
		bmac->pendinglength = 0;
#if OSXNETTRACE
		if (machine->log)
			fprintf(machine->log, "NET rxdone t=%llu\n", (unsigned long long)OsxGuestCpuTimebase(machine->cpu));
#endif
		bmac->status |= BMACFRAMERECEIVED;
	}
}

void OsxGuestBmacPoll(OsxMachine *machine, uint64 now)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	if (!bmac->attached || now < bmac->polldeadline)
		return;
	if (!OsxGuestBackendReady(bmac)) {
		bmac->polldeadline = now + BMACIDLEPOLL;
		return;
	}
	if (now < bmac->busydeadline)
		bmac->polldeadline = now + BMACBUSYPOLL;
	else
		bmac->polldeadline = now + BMACIDLEPOLL;
	OsxGuestHostNetPoll(bmac->slirpthread);
#if OSXG5
	if (machine->g5) {
		OsxGuestG5GmacReceive(machine);
		return;
	}
#endif
	OsxGuestBmacReceive(machine);
}

uint64 OsxGuestBmacNextDeadline(OsxMachine *machine)
{
	if (!machine->bmac.attached)
		return OSXNEVER;
	return machine->bmac.polldeadline;
}

#define DISPLAYNOERROR 0
#define DISPLAYCONTROLERROR -17
#define DISPLAYSTATUSERROR -18
#define DISPLAYPARAMERROR -50

#define DISPLAYCOMMANDCONTROL 4
#define DISPLAYCOMMANDSTATUS 5
#define DISPLAYCOMMANDKILL 6
#define DISPLAYCOMMANDKILLERROR -27

#define DISPLAYCSCODE 26
#define DISPLAYCSPARAM 28

#define DISPLAYSETMODE 2
#define DISPLAYSETENTRIES 3
#define DISPLAYSETGAMMA 4
#define DISPLAYGRAYPAGE 5
#define DISPLAYSETGRAY 6
#define DISPLAYSETINTERRUPT 7
#define DISPLAYDIRECTSETENTRIES 8
#define DISPLAYSWITCHMODE 10
#define DISPLAYSAVEPREFERRED 16

#define DISPLAYGETMODE 2
#define DISPLAYGETPAGES 4
#define DISPLAYGETBASEADDRESS 5
#define DISPLAYGETCURRENTMODE 10
#define DISPLAYGETCONNECTION 12
#define DISPLAYGETMODETIMING 13
#define DISPLAYGETPREFERRED 16
#define DISPLAYGETNEXTRESOLUTION 17
#define DISPLAYGETVIDEOPARAMETERS 18

#define DISPLAYSWITCHDEPTH 0
#define DISPLAYSWITCHDATA 2
#define DISPLAYSWITCHPAGE 6
#define DISPLAYSWITCHBASE 8

#define DISPLAYRESOLUTIONPREVIOUS 0
#define DISPLAYRESOLUTIONMODE 4
#define DISPLAYRESOLUTIONWIDTH 8
#define DISPLAYRESOLUTIONHEIGHT 12
#define DISPLAYRESOLUTIONREFRESH 16
#define DISPLAYRESOLUTIONDEPTH 20
#define DISPLAYRESOLUTIONFLAGS 22
#define DISPLAYRESOLUTIONRESERVED 26

#define DISPLAYPARAMETERSMODE 0
#define DISPLAYPARAMETERSDEPTH 4
#define DISPLAYPARAMETERSBLOCK 6
#define DISPLAYPARAMETERSPAGES 10
#define DISPLAYPARAMETERSDEVICE 14
#define DISPLAYPARAMETERSRESERVED 18

#define DISPLAYBLOCKOFFSET 0
#define DISPLAYBLOCKROWBYTES 4
#define DISPLAYBLOCKBOUNDS 6
#define DISPLAYBLOCKVERSION 14
#define DISPLAYBLOCKPACKTYPE 16
#define DISPLAYBLOCKPACKSIZE 18
#define DISPLAYBLOCKHRES 22
#define DISPLAYBLOCKVRES 26
#define DISPLAYBLOCKPIXELTYPE 30
#define DISPLAYBLOCKPIXELSIZE 32
#define DISPLAYBLOCKCOMPONENTS 34
#define DISPLAYBLOCKCOMPONENTSIZE 36
#define DISPLAYBLOCKPLANEBYTES 38

#define DISPLAYCONNECTIONTYPE 0
#define DISPLAYCONNECTIONTAGGEDTYPE 2
#define DISPLAYCONNECTIONTAGGEDDATA 3
#define DISPLAYCONNECTIONFLAGS 4
#define DISPLAYCONNECTIONCOMPONENT 8
#define DISPLAYCONNECTIONRESERVED 12

#define DISPLAYTIMINGMODE 0
#define DISPLAYTIMINGRESERVED 4
#define DISPLAYTIMINGFORMAT 8
#define DISPLAYTIMINGDATA 12
#define DISPLAYTIMINGFLAGS 16

#define DISPLAYDEPTHMODE 0x80u
#define DISPLAYMODECURRENT 0u
#define DISPLAYMODEFIRST 0xfffffffeu
#define DISPLAYMODENOMORE 0xfffffffdu
#define DISPLAYMODEBASE 0x81u
#define DISPLAYREFRESH (60u << 16)
#define DISPLAYRESOLUTION 0x00480000u
#define DISPLAYPIXELDIRECT 0x10
#define DISPLAYPIXELSIZE 32
#define DISPLAYCOMPONENTS 3
#define DISPLAYCOMPONENTSIZE 8
#define DISPLAYDEVICEDIRECT 2
#define DISPLAYMULTIMODECONNECT 6
#define DISPLAYTAGGEDTYPE 6
#define DISPLAYTAGGEDDATA 0x23
#define DISPLAYALLMODESVALID 1u
#define DISPLAYALLMODESSAFE 2u
#define DISPLAYTIMINGDECLARED 0x6465636cu
#define DISPLAYTIMINGVALID 1u
#define DISPLAYTIMINGSAFE 2u
#define DISPLAYTIMINGDEFAULT 4u
#define DISPLAYTIMINGSHOWN 8u
#define DISPLAYTIMINGUNKNOWN 0

#define DISPLAYDRIVERLOADTOC 0x80402808u
#define DISPLAYDRIVERLOADENTRY 0x800028d8u
#define DISPLAYDRIVERMOVECOUNT 0x7c0903a6u
#define DISPLAYDRIVERBRANCHCOUNT 0x4e800420u
#define DISPLAYDRIVERRETURN 0x4e800020u
#define DISPLAYDRIVERNOOP 0x60000000u

typedef struct OsxDisplayMode {
	uint32 width;
	uint32 height;
	uint32 timing;
} OsxDisplayMode;

static const uint8 OsxDisplayDriverImage[] = {
#include "../VideoDriverStub.i"
};

static const OsxDisplayMode OsxDisplayModes[] = {
	{ 640, 480, 150 },
	{ 800, 600, 182 },
	{ 1024, 768, 204 },
	{ 1152, 870, 220 },
	{ 1280, 960, 250 },
	{ 1280, 1024, 262 },
	{ 1600, 1200, 286 }
};

#define DISPLAYMODECOUNT (sizeof(OsxDisplayModes) / sizeof(OsxDisplayModes[0]))

static uint32 OsxGuestDisplayBig32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static void OsxGuestDisplayStoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

uint8 *OsxGuestDisplayDriver(uint32 *length)
{
	uint8 *image;
	uint32 offset;
	image = (uint8 *)malloc(sizeof(OsxDisplayDriverImage));
	memcpy(image, OsxDisplayDriverImage, sizeof(OsxDisplayDriverImage));
	*length = sizeof(OsxDisplayDriverImage);
	for (offset = 0; offset + 16 <= sizeof(OsxDisplayDriverImage); offset += 4) {
		if (OsxGuestDisplayBig32(image + offset) == DISPLAYDRIVERLOADTOC && OsxGuestDisplayBig32(image + offset + 4) == DISPLAYDRIVERLOADENTRY &&
			OsxGuestDisplayBig32(image + offset + 8) == DISPLAYDRIVERMOVECOUNT && OsxGuestDisplayBig32(image + offset + 12) == DISPLAYDRIVERBRANCHCOUNT) {
			OsxGuestDisplayStoreBig32(image + offset, OSXDISPLAYTRAP);
			OsxGuestDisplayStoreBig32(image + offset + 4, DISPLAYDRIVERRETURN);
			OsxGuestDisplayStoreBig32(image + offset + 8, DISPLAYDRIVERNOOP);
			OsxGuestDisplayStoreBig32(image + offset + 12, DISPLAYDRIVERNOOP);
			return image;
		}
	}
	free(image);
	return NULL;
}

static int OsxGuestDisplayModeSize(OsxMachine *machine, uint32 mode, const OsxDisplayMode **found)
{
	uint32 index;
	if (mode == DISPLAYMODECURRENT)
		mode = machine->displaymode;
	if (mode < DISPLAYMODEBASE || mode >= DISPLAYMODEBASE + DISPLAYMODECOUNT)
		return mode == DISPLAYMODEBASE + DISPLAYMODECOUNT && machine->displaycustom;
	index = mode - DISPLAYMODEBASE;
	*found = &OsxDisplayModes[index];
	return OsxDisplayModes[index].width <= machine->maxwidth && OsxDisplayModes[index].height <= machine->maxheight;
}

static void OsxGuestDisplayDimensions(OsxMachine *machine, uint32 mode, uint32 *width, uint32 *height, uint32 *timing)
{
	const OsxDisplayMode *found;
	found = NULL;
	OsxGuestDisplayModeSize(machine, mode, &found);
	*width = machine->maxwidth;
	*height = machine->maxheight;
	*timing = DISPLAYTIMINGUNKNOWN;
	if (found) {
		*width = found->width;
		*height = found->height;
		*timing = found->timing;
	}
}

void OsxGuestDisplayReset(OsxMachine *machine)
{
	uint32 index;
	machine->maxwidth = machine->width;
	machine->maxheight = machine->height;
	machine->displaycustom = 1;
	machine->displaymode = DISPLAYMODEBASE + DISPLAYMODECOUNT;
	for (index = 0; index < DISPLAYMODECOUNT; index++) {
		if (OsxDisplayModes[index].width == machine->width && OsxDisplayModes[index].height == machine->height) {
			machine->displaycustom = 0;
			machine->displaymode = DISPLAYMODEBASE + index;
		}
	}
	machine->displaypreferred = machine->displaymode;
}

static uint32 OsxGuestDisplayRead(OsxCpu *cpu, uint32 address, int size)
{
	uint32 value;
	uint8 *host;
	int index;
	value = 0;
	for (index = 0; index < size; index++) {
		host = OsxGuestCpuVirtualHost(cpu, address + index, OSXTLBREAD);
		value <<= 8;
		if (host)
			value |= *host;
	}
	return value;
}

static void OsxGuestDisplayWrite(OsxCpu *cpu, uint32 address, int size, uint32 value)
{
	uint8 *host;
	int index;
	for (index = size - 1; index >= 0; index--) {
		host = OsxGuestCpuVirtualHost(cpu, address + index, OSXTLBWRITE);
		if (host)
			*host = (uint8)value;
		value >>= 8;
	}
}

static uint32 OsxGuestDisplayNextMode(OsxMachine *machine, uint32 previous)
{
	const OsxDisplayMode *found;
	uint32 mode;
	if (previous == DISPLAYMODECURRENT)
		return machine->displaymode;
	mode = DISPLAYMODEBASE;
	if (previous != DISPLAYMODEFIRST)
		mode = previous + 1;
	for (; mode <= DISPLAYMODEBASE + DISPLAYMODECOUNT; mode++)
		if (OsxGuestDisplayModeSize(machine, mode, &found))
			return mode;
	return DISPLAYMODENOMORE;
}

static int OsxGuestDisplayNextResolution(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	const OsxDisplayMode *found;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYRESOLUTIONPREVIOUS, 4);
	if (mode != DISPLAYMODECURRENT && mode != DISPLAYMODEFIRST && !OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	mode = OsxGuestDisplayNextMode(machine, mode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONMODE, 4, mode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONFLAGS, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONRESERVED, 4, 0);
	if (mode == DISPLAYMODENOMORE)
		return DISPLAYNOERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONWIDTH, 4, width);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONHEIGHT, 4, height);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONREFRESH, 4, DISPLAYREFRESH);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONDEPTH, 2, DISPLAYDEPTHMODE);
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayVideoParameters(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 block;
	uint32 width;
	uint32 height;
	uint32 timing;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSMODE, 4);
	if (!OsxGuestDisplayModeSize(machine, mode, &found) || OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSDEPTH, 2) != DISPLAYDEPTHMODE)
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	block = OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSBLOCK, 4);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKOFFSET, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKROWBYTES, 2, machine->rowbytes);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS + 4, 2, height);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS + 6, 2, width);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKVERSION, 2, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPACKTYPE, 2, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPACKSIZE, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKHRES, 4, DISPLAYRESOLUTION);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKVRES, 4, DISPLAYRESOLUTION);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPIXELTYPE, 2, DISPLAYPIXELDIRECT);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPIXELSIZE, 2, DISPLAYPIXELSIZE);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKCOMPONENTS, 2, DISPLAYCOMPONENTS);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKCOMPONENTSIZE, 2, DISPLAYCOMPONENTSIZE);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPLANEBYTES, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSPAGES, 4, 1);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSDEVICE, 4, DISPLAYDEVICEDIRECT);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSRESERVED, 4, 0);
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayModeTiming(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	uint32 flags;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYTIMINGMODE, 4);
	if (!OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	flags = DISPLAYTIMINGVALID | DISPLAYTIMINGSAFE | DISPLAYTIMINGSHOWN;
	if (mode == machine->displaypreferred)
		flags |= DISPLAYTIMINGDEFAULT;
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGRESERVED, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGFORMAT, 4, DISPLAYTIMINGDECLARED);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGDATA, 4, timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGFLAGS, 4, flags);
	return DISPLAYNOERROR;
}

static void OsxGuestDisplayCurrentMode(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDEPTH, 2, DISPLAYDEPTHMODE);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDATA, 4, machine->displaymode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHPAGE, 2, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
}

static int OsxGuestDisplayStatus(OsxMachine *machine, OsxCpu *cpu, uint32 code, uint32 param)
{
	switch (code) {
	case DISPLAYGETMODE:
	case DISPLAYGETCURRENTMODE:
		OsxGuestDisplayCurrentMode(machine, cpu, param);
		return DISPLAYNOERROR;
	case DISPLAYGETPAGES:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHPAGE, 2, 1);
		return DISPLAYNOERROR;
	case DISPLAYGETBASEADDRESS:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
		return DISPLAYNOERROR;
	case DISPLAYGETCONNECTION:
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTYPE, 2, DISPLAYMULTIMODECONNECT);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTAGGEDTYPE, 1, DISPLAYTAGGEDTYPE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTAGGEDDATA, 1, DISPLAYTAGGEDDATA);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONFLAGS, 4, DISPLAYALLMODESVALID | DISPLAYALLMODESSAFE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONCOMPONENT, 4, 0);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONRESERVED, 4, 0);
		return DISPLAYNOERROR;
	case DISPLAYGETMODETIMING:
		return OsxGuestDisplayModeTiming(machine, cpu, param);
	case DISPLAYGETPREFERRED:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDEPTH, 2, DISPLAYDEPTHMODE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDATA, 4, machine->displaypreferred);
		return DISPLAYNOERROR;
	case DISPLAYGETNEXTRESOLUTION:
		return OsxGuestDisplayNextResolution(machine, cpu, param);
	case DISPLAYGETVIDEOPARAMETERS:
		return OsxGuestDisplayVideoParameters(machine, cpu, param);
	}
	return DISPLAYSTATUSERROR;
}

static int OsxGuestDisplaySwitch(OsxMachine *machine, OsxCpu *cpu, uint32 param, int *resized)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDATA, 4);
	if (OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDEPTH, 2) != DISPLAYDEPTHMODE || !OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
	if (mode == machine->displaymode)
		return DISPLAYNOERROR;
	machine->displaymode = mode;
	machine->width = width;
	machine->height = height;
	memset(machine->vram, 0, machine->rowbytes * machine->maxheight);
	*resized = 1;
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayControl(OsxMachine *machine, OsxCpu *cpu, uint32 code, uint32 param, int *resized)
{
	switch (code) {
	case DISPLAYSETMODE:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
		return DISPLAYNOERROR;
	case DISPLAYSETENTRIES:
	case DISPLAYSETGAMMA:
	case DISPLAYGRAYPAGE:
	case DISPLAYSETGRAY:
	case DISPLAYSETINTERRUPT:
	case DISPLAYDIRECTSETENTRIES:
		return DISPLAYNOERROR;
	case DISPLAYSWITCHMODE:
		return OsxGuestDisplaySwitch(machine, cpu, param, resized);
	case DISPLAYSAVEPREFERRED:
		machine->displaypreferred = OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDATA, 4);
		return DISPLAYNOERROR;
	}
	return DISPLAYCONTROLERROR;
}

int OsxGuestDisplayDriverCall(OsxMachine *machine, OsxCpu *cpu)
{
	uint32 contents;
	uint32 code;
	uint32 param;
	int result;
	int resized;
	contents = cpu->gpr[5];
	resized = 0;
	result = DISPLAYNOERROR;
	code = OsxGuestDisplayRead(cpu, contents + DISPLAYCSCODE, 2);
	param = OsxGuestDisplayRead(cpu, contents + DISPLAYCSPARAM, 4);
	if (cpu->gpr[6] == DISPLAYCOMMANDCONTROL)
		result = OsxGuestDisplayControl(machine, cpu, code, param, &resized);
	else if (cpu->gpr[6] == DISPLAYCOMMANDSTATUS)
		result = OsxGuestDisplayStatus(machine, cpu, code, param);
	else if (cpu->gpr[6] == DISPLAYCOMMANDKILL)
		result = DISPLAYCOMMANDKILLERROR;
	if (machine->log)
		fprintf(machine->log, "display command %u code %u result %d mode %x %ux%u\n", cpu->gpr[6], code, result, machine->displaymode, machine->width, machine->height);
	cpu->gpr[3] = (uint32)result;
	return resized;
}
