/*
 *  osxg5.cpp - OSX PowerMac7,3 U3/HyperTransport/K2 board emulation
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

#if OSXG5

#define MPICMASK 0x80000000u
#define MPICACTIVITY 0x40000000u
#define MPICSENSE 0x00400000u
#define MPICRESET 0x80000000u
#define MPICSOURCESK2 64

#define I2CMODE 0x00
#define I2CCONTROL 0x10
#define I2CSTATUS 0x20
#define I2CISR 0x30
#define I2CIER 0x40
#define I2CADDR 0x50
#define I2CSUBADDR 0x60
#define I2CDATA 0x70
#define I2CMODESTANDARD 0x04
#define I2CMODESTANDARDSUB 0x08
#define I2CMODECOMBINED 0x0c
#define I2CMODEMASK 0x0c
#define I2CCONTROLAAK 0x01
#define I2CCONTROLXADDR 0x02
#define I2CCONTROLSTOP 0x04
#define I2CSTATUSBUSY 0x01
#define I2CSTATUSLASTAAK 0x02
#define I2CSTATUSLASTRW 0x04
#define I2CSTATUSIDLE 0x18
#define I2CIRQDATA 0x01
#define I2CIRQADDR 0x02
#define I2CIRQSTOP 0x04
#define I2CPULSAR 0x69

#define GPIOINDATA 0x02
#define GPIOPMU 0x09

#define K2COUNTERFREQUENCY 18432000
#define K2COUNTER 0x15038

#define ATAIRQDMA 0x80000000u
#define ATAIRQDRIVE 0x40000000u

static void GmacReset(OsxMachine *machine);

static uint32 LoadLittle(const uint8 *bytes, int size)
{
	uint32 value;
	int index;
	value = 0;
	for (index = size - 1; index >= 0; index--)
		value = (value << 8) | bytes[index];
	return value;
}

static void StoreLittle(uint8 *bytes, int size, uint32 value)
{
	int index;
	for (index = 0; index < size; index++)
		bytes[index] = (uint8)(value >> (index * 8));
}

static uint32 BusFromBytes(const uint8 *bytes, int size)
{
	uint32 value;
	int index;
	value = 0;
	for (index = 0; index < size; index++)
		value = (value << 8) | bytes[index];
	return value;
}

static void BytesFromBus(uint8 *bytes, int size, uint32 value)
{
	int index;
	for (index = 0; index < size; index++)
		bytes[index] = (uint8)(value >> ((size - 1 - index) * 8));
}

static uint32 AllOnes(int size)
{
	return 0xffffffffu >> ((4 - size) * 8);
}

static void SetFunction(OsxG5Function *function, uint32 device, uint32 vendordevice, uint32 classrevision, uint32 bar, uint32 barsize)
{
	memset(function, 0, sizeof(OsxG5Function));
	function->device = device;
	StoreLittle(function->config, 4, vendordevice);
	StoreLittle(function->config + 4, 4, 0x02000006);
	StoreLittle(function->config + 8, 4, classrevision);
	StoreLittle(function->config + 0x10, 4, bar);
	if (barsize)
		function->barmask[0] = ~(barsize - 1);
}

static void MpicReset(OsxMpic *mpic)
{
	int index;
	for (index = 0; index < OSXG5MPICSOURCES; index++) {
		mpic->ivpr[index] = 0xa0000000u;
		mpic->idr[index] = 0;
		mpic->pending[index] = 0;
	}
	mpic->inservicecount = 0;
	mpic->gcr = 0;
	mpic->vid = 2;
	mpic->spvr = 0xff;
	mpic->tfrr = 4160000;
	mpic->ctpr = 15;
	memset(mpic->timers, 0, sizeof(mpic->timers));
}

void OsxG5Reset(OsxMachine *machine)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	memset(g5, 0, sizeof(OsxG5));
	SetFunction(&g5->functions[0], OSXG5DEVICEHOST, 0x0057106b, 0x06000000, 0, 0);
	SetFunction(&g5->functions[1], OSXG5DEVICEMACIO, 0x0041106b, 0xff000000, OSXG5MACIOBASE, OSXG5MACIOSIZE);
	SetFunction(&g5->functions[2], OSXG5DEVICEATA0, 0x0043106b, 0x01018f00, OSXG5ATABASE, OSXG5ATASIZE);
	SetFunction(&g5->functions[3], OSXG5DEVICEATA1, 0x0043106b, 0x01018f00, OSXG5ATABASE + OSXG5ATASIZE, OSXG5ATASIZE);
	SetFunction(&g5->functions[4], OSXG5DEVICEDISPLAY, 0x11111234, 0x03000002, OSXVRAMBASE | 8, OSXVRAMSIZE);
	SetFunction(&g5->functions[5], OSXG5DEVICEGMAC, 0x004c106b, 0x02000000, OSXG5GMACBASE, OSXG5GMACSIZE);
	StoreLittle(g5->functions[5].config + 0x3c, 4, 0x0100 | OSXG5IRQGMAC);
	StoreLittle(g5->functions[0].config + 0x80, 4, 0x01000000);
	StoreLittle(g5->functions[2].config + 0x3c, 4, 0x0100 | OSXG5IRQATA0);
	StoreLittle(g5->functions[3].config + 0x3c, 4, 0x0100 | OSXG5IRQATA1);
	MpicReset(&g5->mpic);
	g5->gpio[GPIOPMU] = GPIOINDATA;
	machine->dbdma[OSXDBDMAIDE0].irq = OSXIRQIDE0DMA;
	machine->dbdma[OSXDBDMAIDE1].irq = OSXIRQIDE1DMA;
	GmacReset(machine);
}

static int MpicPriority(uint32 ivpr)
{
	return (int)((ivpr >> 16) & 15);
}

static int MpicPending(OsxMpic *mpic, int source)
{
	if (source < MPICSOURCESK2 && (mpic->ivpr[source] & MPICSENSE))
		return mpic->line[source];
	return mpic->pending[source];
}

static int MpicBest(OsxMpic *mpic)
{
	int floor;
	int best;
	int index;
	int priority;
	floor = (int)mpic->ctpr;
	if (mpic->inservicecount > 0) {
		priority = MpicPriority(mpic->ivpr[mpic->inservice[mpic->inservicecount - 1]]);
		if (priority > floor)
			floor = priority;
	}
	best = -1;
	for (index = 0; index < OSXG5MPICSOURCES; index++) {
		if (!MpicPending(mpic, index) || (mpic->ivpr[index] & MPICMASK) || !(mpic->idr[index] & 1))
			continue;
		priority = MpicPriority(mpic->ivpr[index]);
		if (priority > floor) {
			floor = priority;
			best = index;
		}
	}
	return best;
}

static void MpicUpdate(OsxMachine *machine)
{
	int line;
	line = MpicBest(&machine->g5state.mpic) >= 0;
	machine->cpu->externalline = line;
	if (line)
		machine->cpu->halted = 0;
}

static void MpicSetSource(OsxMachine *machine, int source, int level)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (level && !mpic->line[source])
		mpic->pending[source] = 1;
	mpic->line[source] = (uint8)(level != 0);
	MpicUpdate(machine);
}

static uint32 MpicAcknowledge(OsxMachine *machine)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	source = MpicBest(mpic);
	if (source < 0)
		return mpic->spvr & 0xff;
	if (source >= MPICSOURCESK2 || !(mpic->ivpr[source] & MPICSENSE))
		mpic->pending[source] = 0;
	if (mpic->inservicecount < 16)
		mpic->inservice[mpic->inservicecount++] = (uint8)source;
	MpicUpdate(machine);
	return mpic->ivpr[source] & 0xff;
}

static void MpicEndOfInterrupt(OsxMachine *machine)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (mpic->inservicecount > 0)
		mpic->inservicecount--;
	MpicUpdate(machine);
}

static int MpicInService(OsxMpic *mpic, int source)
{
	int index;
	for (index = 0; index < mpic->inservicecount; index++)
		if (mpic->inservice[index] == source)
			return 1;
	return 0;
}

static uint32 MpicSourceVector(OsxMpic *mpic, int source)
{
	uint32 value;
	value = mpic->ivpr[source] & ~MPICACTIVITY;
	if (MpicPending(mpic, source) || MpicInService(mpic, source))
		value |= MPICACTIVITY;
	return value;
}

static uint32 MpicCpuRead(OsxMachine *machine, uint32 reg)
{
	switch (reg) {
	case 0x80: return machine->g5state.mpic.ctpr;
	case 0x90: return 0;
	case 0xa0: return MpicAcknowledge(machine);
	}
	return 0;
}

static void MpicCpuWrite(OsxMachine *machine, uint32 reg, uint32 value)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (reg >= 0x40 && reg < 0x80) {
		if (value & 1)
			mpic->pending[OSXG5MPICIPI + ((reg - 0x40) >> 4)] = 1;
		MpicUpdate(machine);
		return;
	}
	if (reg == 0x80) {
		mpic->ctpr = value & 15;
		MpicUpdate(machine);
		return;
	}
	if (reg == 0xb0)
		MpicEndOfInterrupt(machine);
}

static uint32 MpicRead(OsxMachine *machine, uint32 offset)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	if (offset < 0x1000)
		return MpicCpuRead(machine, offset & 0xff0);
	if (offset >= 0x20000 && offset < 0x21000)
		return MpicCpuRead(machine, offset & 0xff0);
	if (offset >= 0x10000 && offset < 0x10000 + MPICSOURCESK2 * 0x20) {
		source = (int)((offset - 0x10000) >> 5);
		if (offset & 0x10)
			return mpic->idr[source];
		return MpicSourceVector(mpic, source);
	}
	if (offset >= 0x10a0 && offset < 0x10e0)
		return MpicSourceVector(mpic, OSXG5MPICIPI + (int)((offset - 0x10a0) >> 4));
	if (offset >= 0x1100 && offset < 0x1200)
		return mpic->timers[(offset - 0x1100) >> 4];
	switch (offset) {
	case 0x1000: return ((uint32)(MPICSOURCESK2 - 1) << 16) | 2;
	case 0x1020: return mpic->gcr;
	case 0x1080: return mpic->vid;
	case 0x10e0: return mpic->spvr;
	case 0x10f0: return mpic->tfrr;
	}
	return 0;
}

static void MpicWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	if (offset < 0x1000 || (offset >= 0x20000 && offset < 0x21000)) {
		MpicCpuWrite(machine, offset & 0xff0, value);
		return;
	}
	if (offset >= 0x10000 && offset < 0x10000 + MPICSOURCESK2 * 0x20) {
		source = (int)((offset - 0x10000) >> 5);
		if (offset & 0x10)
			mpic->idr[source] = value;
		else
			mpic->ivpr[source] = value & ~MPICACTIVITY;
		MpicUpdate(machine);
		return;
	}
	if (offset >= 0x10a0 && offset < 0x10e0) {
		mpic->ivpr[OSXG5MPICIPI + ((offset - 0x10a0) >> 4)] = value & ~MPICACTIVITY;
		mpic->idr[OSXG5MPICIPI + ((offset - 0x10a0) >> 4)] = 1;
		MpicUpdate(machine);
		return;
	}
	if (offset >= 0x1100 && offset < 0x1200) {
		mpic->timers[(offset - 0x1100) >> 4] = value;
		return;
	}
	switch (offset) {
	case 0x1020:
		if (value & MPICRESET)
			MpicReset(mpic);
		mpic->gcr = value & ~MPICRESET;
		MpicUpdate(machine);
		break;
	case 0x1080:
		mpic->vid = value;
		break;
	case 0x10e0:
		mpic->spvr = value;
		break;
	case 0x10f0:
		mpic->tfrr = value;
		break;
	}
}

void OsxG5SetLine(OsxMachine *machine, int source, int level)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	switch (source) {
	case OSXIRQIDE0:
		machine->dbdma[OSXDBDMAIDE0].status = (machine->dbdma[OSXDBDMAIDE0].status & ~0x80u) | (level ? 0x80u : 0);
		MpicSetSource(machine, OSXG5IRQATA0, level);
		break;
	case OSXIRQIDE1:
		machine->dbdma[OSXDBDMAIDE1].status = (machine->dbdma[OSXDBDMAIDE1].status & ~0x80u) | (level ? 0x80u : 0);
		MpicSetSource(machine, OSXG5IRQATA1, level);
		break;
	case OSXIRQIDE0DMA:
		if (level)
			g5->atairq[0] |= ATAIRQDMA;
		MpicSetSource(machine, OSXG5IRQATA0DMA, level);
		break;
	case OSXIRQIDE1DMA:
		if (level)
			g5->atairq[1] |= ATAIRQDMA;
		MpicSetSource(machine, OSXG5IRQATA1DMA, level);
		break;
	case OSXIRQCUDA:
		MpicSetSource(machine, OSXG5IRQPMU, level);
		break;
	}
}

void OsxG5SetPmuRequest(OsxMachine *machine, int level)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (level)
		g5->gpio[GPIOPMU] &= ~GPIOINDATA;
	else
		g5->gpio[GPIOPMU] |= GPIOINDATA;
	MpicSetSource(machine, OSXG5IRQPMUGPIO, level);
}

static OsxG5Function *FindFunction(OsxMachine *machine, uint32 offset, uint32 *reg)
{
	OsxG5 *g5;
	uint32 devfn;
	int index;
	g5 = &machine->g5state;
	if (offset & 0x01000000)
		return NULL;
	devfn = (offset >> 8) & 0xff;
	*reg = offset & 0xff;
	if (devfn & 7)
		return NULL;
	for (index = 0; index < OSXG5FUNCTIONS; index++)
		if (g5->functions[index].device == (devfn >> 3))
			return &g5->functions[index];
	return NULL;
}

static uint32 ConfigRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxG5Function *function;
	uint32 reg;
	function = FindFunction(machine, offset, &reg);
	if (function == NULL || reg + size > 256)
		return AllOnes(size);
	return BusFromBytes(function->config + reg, size);
}

static void ConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxG5Function *function;
	uint8 updated[256];
	uint32 reg;
	uint32 dword;
	uint32 bar;
	int index;
	function = FindFunction(machine, offset, &reg);
	if (function == NULL || reg + size > 256)
		return;
	memcpy(updated, function->config, 256);
	BytesFromBus(updated + reg, size, value);
	memcpy(function->config + 4, updated + 4, 2);
	memcpy(function->config + 0x0c, updated + 0x0c, 2);
	function->config[0x3c] = updated[0x3c];
	for (index = 0; index < 6; index++) {
		bar = 0x10 + index * 4;
		if (reg + size <= bar || reg >= bar + 4)
			continue;
		dword = LoadLittle(updated + bar, 4);
		dword = (dword & function->barmask[index]) | (LoadLittle(function->config + bar, 4) & 0xf);
		StoreLittle(function->config + bar, 4, dword);
	}
}

static uint32 HtSelfRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 reg;
	reg = offset >> 2;
	if (reg + size > 256)
		return 0;
	return BusFromBytes(machine->g5state.functions[0].config + reg, size);
}

static void I2cStop(OsxKeyWest *i2c)
{
	i2c->active = 0;
	i2c->status &= ~I2CSTATUSBUSY;
	i2c->control = 0;
}

static int I2cStart(OsxKeyWest *i2c)
{
	int mode;
	mode = i2c->mode & I2CMODEMASK;
	i2c->active = 0;
	i2c->status &= ~I2CSTATUSBUSY;
	if ((i2c->addr >> 1) != I2CPULSAR)
		return 0;
	i2c->active = 1;
	i2c->status |= I2CSTATUSBUSY;
	if (!(i2c->addr & 1) || mode == I2CMODECOMBINED)
		i2c->pointerset = 0;
	if (mode == I2CMODESTANDARDSUB || mode == I2CMODECOMBINED) {
		i2c->pointer = i2c->subaddr;
		i2c->pointerset = 1;
	}
	return 1;
}

static void I2cReadByte(OsxKeyWest *i2c)
{
	i2c->data = i2c->clock[i2c->pointer++];
	i2c->status |= I2CSTATUSLASTAAK;
	i2c->lastnak = !(i2c->control & I2CCONTROLAAK);
	i2c->isr |= I2CIRQDATA;
}

static uint32 I2cRead(OsxKeyWest *i2c, uint32 reg)
{
	switch (reg) {
	case I2CMODE: return i2c->mode;
	case I2CCONTROL: return i2c->control;
	case I2CSTATUS: return i2c->status | I2CSTATUSIDLE;
	case I2CISR: return i2c->isr;
	case I2CIER: return i2c->ier;
	case I2CADDR: return i2c->addr;
	case I2CSUBADDR: return i2c->subaddr;
	case I2CDATA: return i2c->data;
	}
	return 0;
}

static void I2cControl(OsxKeyWest *i2c, uint8 value)
{
	uint8 rising;
	int ack;
	rising = value & ~i2c->control;
	i2c->control = value;
	if (rising & I2CCONTROLXADDR) {
		ack = I2cStart(i2c);
		if (ack)
			i2c->status |= I2CSTATUSLASTAAK;
		else
			i2c->status &= ~I2CSTATUSLASTAAK;
		if (i2c->addr & 1)
			i2c->status |= I2CSTATUSLASTRW;
		else
			i2c->status &= ~I2CSTATUSLASTRW;
		i2c->readpending = ack && (i2c->addr & 1);
		i2c->isr |= I2CIRQADDR;
		if (!ack) {
			I2cStop(i2c);
			i2c->isr |= I2CIRQSTOP;
		}
	}
	if (rising & I2CCONTROLSTOP) {
		I2cStop(i2c);
		i2c->isr |= I2CIRQSTOP;
	}
}

static void I2cAcknowledge(OsxKeyWest *i2c, uint8 value)
{
	uint8 previous;
	int mode;
	previous = i2c->isr;
	i2c->isr &= ~(value & 0x0f);
	if ((value & I2CIRQADDR) && i2c->active && i2c->readpending) {
		i2c->readpending = 0;
		I2cReadByte(i2c);
	}
	mode = i2c->mode & I2CMODEMASK;
	if ((value & I2CIRQDATA) && i2c->active && (i2c->addr & 1) && (mode == I2CMODECOMBINED || mode == I2CMODESTANDARD || mode == I2CMODESTANDARDSUB) && (previous & I2CIRQDATA)) {
		if (i2c->lastnak) {
			I2cStop(i2c);
			i2c->isr |= I2CIRQSTOP;
		} else {
			I2cReadByte(i2c);
		}
	}
}

static void I2cData(OsxKeyWest *i2c, uint8 value)
{
	i2c->data = value;
	if (i2c->active && !(i2c->addr & 1)) {
		if (i2c->pointerset) {
			i2c->clock[i2c->pointer++] = value;
		} else {
			i2c->pointer = value;
			i2c->pointerset = 1;
		}
		i2c->status |= I2CSTATUSLASTAAK;
		i2c->isr |= I2CIRQDATA;
		return;
	}
	i2c->status &= ~I2CSTATUSLASTAAK;
	I2cStop(i2c);
	i2c->isr |= I2CIRQDATA | I2CIRQSTOP;
}

static void I2cWrite(OsxKeyWest *i2c, uint32 reg, uint8 value)
{
	switch (reg) {
	case I2CMODE: i2c->mode = value; break;
	case I2CCONTROL: I2cControl(i2c, value); break;
	case I2CSTATUS: i2c->status = value; break;
	case I2CISR: I2cAcknowledge(i2c, value); break;
	case I2CIER: i2c->ier = value; break;
	case I2CADDR: i2c->addr = value; break;
	case I2CSUBADDR: i2c->subaddr = value; break;
	case I2CDATA: I2cData(i2c, value); break;
	}
}

static uint64 K2Counter(OsxMachine *machine)
{
	uint64 timebase;
	timebase = OsxCpuTimebase(machine->cpu);
	return (timebase / OSXTIMEBASEFREQUENCY) * K2COUNTERFREQUENCY + (timebase % OSXTIMEBASEFREQUENCY) * K2COUNTERFREQUENCY / OSXTIMEBASEFREQUENCY;
}

static uint32 MacIoRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (offset >= 0x8000 && offset < 0x9000)
		return OsxHeathrowRead(machine, offset, size);
	if (offset == K2COUNTER && size == 4)
		return bswap_32((uint32)K2Counter(machine));
	if (offset == K2COUNTER + 4 && size == 4)
		return bswap_32((uint32)(K2Counter(machine) >> 32));
	if (offset >= OSXG5MPICOFFSET)
		return bswap_32(MpicRead(machine, (offset - OSXG5MPICOFFSET) & ~3u)) >> ((4 - size) * 8);
	if (offset >= 0x16000 && offset < 0x18000)
		return OsxCudaRead(machine, (offset >> 9) & 15);
	if (offset < OSXG5GPIOBASE)
		return g5->fcr[offset >> 2] >> ((4 - size) * 8);
	if (offset - OSXG5GPIOBASE + size <= OSXG5GPIOSIZE)
		return BusFromBytes(g5->gpio + offset - OSXG5GPIOBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "k2 read %05x size %d\n", offset, size);
	return 0;
}

static void MacIoWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (offset >= 0x8000 && offset < 0x9000) {
		OsxHeathrowWrite(machine, offset, size, value);
		return;
	}
	if (offset >= OSXG5MPICOFFSET) {
		if (size == 4)
			MpicWrite(machine, (offset - OSXG5MPICOFFSET) & ~3u, bswap_32(value));
		return;
	}
	if (offset >= 0x16000 && offset < 0x18000) {
		OsxCudaWrite(machine, (offset >> 9) & 15, value & 0xff);
		return;
	}
	if (offset < OSXG5GPIOBASE) {
		if (size == 4)
			g5->fcr[offset >> 2] = value;
		return;
	}
	if (offset - OSXG5GPIOBASE + size <= OSXG5GPIOSIZE) {
		BytesFromBus(g5->gpio + offset - OSXG5GPIOBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "k2 write %05x size %d value %08x\n", offset, size, value);
}

static uint32 AtaRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxG5 *g5;
	int bus;
	uint32 reg;
	uint32 value;
	g5 = &machine->g5state;
	bus = (int)(offset / OSXG5ATASIZE);
	reg = offset % OSXG5ATASIZE;
	if (reg < 0x1000)
		return bswap_32(g5->atafcr[bus]) >> ((4 - size) * 8);
	if (reg < 0x2000)
		return OsxHeathrowRead(machine, 0x8000 + (OSXDBDMAIDE0 + bus) * 0x100 + (reg & 0xff), size);
	if ((reg & 0xfff) == 0x300) {
		value = g5->atairq[bus] & ATAIRQDMA;
		if (machine->ata[bus].irqline)
			value |= ATAIRQDRIVE;
		return bswap_32(value) >> ((4 - size) * 8);
	}
	return OsxHeathrowRead(machine, 0x20000 + bus * 0x1000 + (reg & 0xfff), size);
}

static void AtaWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxG5 *g5;
	int bus;
	uint32 reg;
	g5 = &machine->g5state;
	bus = (int)(offset / OSXG5ATASIZE);
	reg = offset % OSXG5ATASIZE;
	if (reg < 0x1000) {
		if (size == 4)
			g5->atafcr[bus] = bswap_32(value);
		return;
	}
	if (reg < 0x2000) {
		OsxHeathrowWrite(machine, 0x8000 + (OSXDBDMAIDE0 + bus) * 0x100 + (reg & 0xff), size, value);
		return;
	}
	if ((reg & 0xfff) == 0x300) {
		if (size == 4 && (bswap_32(value) & ATAIRQDMA))
			g5->atairq[bus] &= ~ATAIRQDMA;
		return;
	}
	OsxHeathrowWrite(machine, 0x20000 + bus * 0x1000 + (reg & 0xfff), size, value);
}

static uint32 U3Read(OsxMachine *machine, uint32 offset, int size)
{
	if (offset - (OSXG5U3I2C - OSXG5U3BASE) < 0x1000)
		return I2cRead(&machine->g5state.i2c, (offset - (OSXG5U3I2C - OSXG5U3BASE)) & 0xf0) << ((size - 1) * 8);
	if (offset - (OSXG5HTSELF - OSXG5U3BASE) < 0x1000)
		return HtSelfRead(machine, offset - (OSXG5HTSELF - OSXG5U3BASE), size);
	if (offset == 0 && size == 4)
		return 0x33;
	return 0;
}

static void U3Write(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	if (offset - (OSXG5U3I2C - OSXG5U3BASE) < 0x1000)
		I2cWrite(&machine->g5state.i2c, (offset - (OSXG5U3I2C - OSXG5U3BASE)) & 0xf0, (uint8)(value >> ((size - 1) * 8)));
	else if (offset - (OSXG5HTSELF - OSXG5U3BASE) < 0x400)
		ConfigWrite(machine, (offset - (OSXG5HTSELF - OSXG5U3BASE)) >> 2, size, value);
}

static uint8 ChrpChecksum(const uint8 *header)
{
	uint32 sum;
	int index;
	sum = header[0];
	for (index = 2; index < 16; index++) {
		sum += header[index];
		if (sum > 0xff)
			sum = (sum & 0xff) + 1;
	}
	return (uint8)sum;
}

static int FlashBankValid(const uint8 *bank)
{
	return bank[0] == 0x5a && bank[1] == ChrpChecksum(bank) && BusFromBytes(bank + 16, 4) == OsxAdler32(bank + 20, OSXFLASHBANK - 20);
}

void OsxG5PrepareNvram(OsxMachine *machine)
{
	uint8 *bank;
	bank = machine->flash;
	if (FlashBankValid(bank) || FlashBankValid(bank + OSXFLASHBANK))
		return;
	memcpy(bank, machine->nvram, OSXFLASHBANK);
	memset(bank, 0, 0x30);
	bank[0] = 0x5a;
	bank[3] = 2;
	memcpy(bank + 4, "nvram", 5);
	BytesFromBus(bank + 20, 4, 1);
	bank[1] = ChrpChecksum(bank);
	bank[0x20] = 0x7f;
	bank[0x22] = (uint8)(((OSXPRAMOFFSET - 0x30) / 16) >> 8);
	bank[0x23] = (uint8)((OSXPRAMOFFSET - 0x30) / 16);
	memcpy(bank + 0x24, "wwwwwwwwwwww", 12);
	bank[0x21] = ChrpChecksum(bank + 0x20);
	BytesFromBus(bank + 16, 4, OsxAdler32(bank + 20, OSXFLASHBANK - 20));
	memset(bank + OSXFLASHBANK, 0xff, OSXFLASHBANK);
	machine->flashdirty = 1;
}

#define GMACSTAT 0x000c
#define GMACIMASK 0x0010
#define GMACIACK 0x0014
#define GMACSTAT2 0x001c
#define GMACSWRST 0x1010
#define GMACSTATTXINTME 0x00000001
#define GMACSTATTXALL 0x00000002
#define GMACSTATTXDONE 0x00000004
#define GMACSTATRXDONE 0x00000010
#define GMACSTATRXNOBUF 0x00000020
#define GMACSTATTXNR 0xfff80000u
#define GMACSTATLATCH 0x00000073
#define GMACTXKICK 0x0000
#define GMACTXCFG 0x0004
#define GMACTXDBLOW 0x0008
#define GMACTXDBHI 0x000c
#define GMACTXDONE 0x0100
#define GMACRXCFG 0x0000
#define GMACRXDBLOW 0x0004
#define GMACRXDBHI 0x0008
#define GMACRXKICK 0x0100
#define GMACRXDONE 0x0104
#define GMACMACTXCFG 0x0030
#define GMACMACRXCFG 0x0034
#define GMACMACMAXFSZ 0x0054
#define GMACMACADDR0 0x0080
#define GMACMIFFRAME 0x000c
#define GMACMIFCFG 0x0010
#define GMACTXSOF 0x80000000u
#define GMACTXEOF 0x40000000u
#define GMACTXCENAB 0x20000000u
#define GMACTXINTME 1
#define GMACPHY 1

static uint32 GmacRingMask(uint32 config)
{
	return (1u << (((config & 0x1e) >> 1) + 5)) - 1;
}

static uint16 GmacChecksum(const uint8 *data, uint32 length)
{
	uint32 sum;
	uint32 index;
	sum = 0;
	for (index = 0; index + 1 < length; index += 2)
		sum += ((uint32)data[index] << 8) | data[index + 1];
	if (length & 1)
		sum += (uint32)data[length - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16)~sum;
}

static void GmacUpdate(OsxMachine *machine)
{
	OsxGmac *gmac;
	gmac = &machine->g5state.gmac;
	MpicSetSource(machine, OSXG5IRQGMAC, (gmac->gregs[GMACSTAT >> 2] & ~GMACSTATTXNR & ~gmac->gregs[GMACIMASK >> 2]) != 0);
}

static void GmacStatus(OsxMachine *machine, uint32 bits)
{
	machine->g5state.gmac.gregs[GMACSTAT >> 2] |= bits;
	GmacUpdate(machine);
}

static uint8 *GmacGuest(OsxMachine *machine, uint32 address, uint32 length)
{
	if (address >= machine->ramsize || length > machine->ramsize - address)
		return NULL;
	return machine->ram + address;
}

static void GmacResetRx(OsxGmac *gmac)
{
	gmac->rxdma[GMACRXDONE >> 2] = 0;
	gmac->rxdma[GMACRXKICK >> 2] = 0;
	gmac->rxdma[GMACRXCFG >> 2] = 0x1000010;
	gmac->rxkicked = 0;
}

static void GmacResetTx(OsxGmac *gmac)
{
	gmac->txdma[GMACTXDONE >> 2] = 0;
	gmac->txdma[GMACTXKICK >> 2] = 0;
	gmac->txdma[GMACTXCFG >> 2] = 0x118010;
	gmac->txsize = 0;
	gmac->txfirst = 0;
}

static void GmacReset(OsxMachine *machine)
{
	OsxGmac *gmac;
	uint8 address[6];
	gmac = &machine->g5state.gmac;
	GmacResetRx(gmac);
	GmacResetTx(gmac);
	gmac->gregs[GMACIMASK >> 2] = 0x0fffffff;
	gmac->gregs[GMACSTAT >> 2] = 0;
	OsxBmacHardwareAddress(address);
	gmac->mac[GMACMACADDR0 >> 2] = ((uint32)address[4] << 8) | address[5];
	gmac->mac[(GMACMACADDR0 >> 2) + 1] = ((uint32)address[2] << 8) | address[3];
	gmac->mac[(GMACMACADDR0 >> 2) + 2] = ((uint32)address[0] << 8) | address[1];
	gmac->mac[GMACMACMAXFSZ >> 2] = 0x5ee;
	gmac->mif[GMACMIFCFG >> 2] = 0x100;
}

static uint16 GmacPhyRead(uint32 phy, uint32 reg)
{
	if (phy != GMACPHY)
		return 0xffff;
	switch (reg) {
	case 2: return 0x0040;
	case 3: return 0x6210;
	case 1: return 0x402c;
	case 4:
	case 5: return 0x0100;
	case 0x18: return 3;
	}
	return 0;
}

static uint32 GmacPhyFrame(uint32 value)
{
	if ((value >> 30) != 1)
		return 0xffff;
	if (((value >> 28) & 3) == 1)
		return value | 0x10000;
	if (((value >> 28) & 3) == 2)
		return GmacPhyRead((value >> 23) & 0x1f, (value >> 18) & 0x1f) | 0x10000;
	return 0xffff | 0x10000;
}

static void GmacTransmit(OsxMachine *machine)
{
	OsxGmac *gmac;
	uint8 *descriptor;
	uint8 *data;
	uint32 base;
	uint32 mask;
	uint32 done;
	uint32 kick;
	uint32 control;
	uint32 length;
	uint32 bits;
	uint32 start;
	uint32 stuff;
	uint16 checksum;
	gmac = &machine->g5state.gmac;
	if (!(gmac->txdma[GMACTXCFG >> 2] & 1) || !(gmac->mac[GMACMACTXCFG >> 2] & 1))
		return;
	mask = GmacRingMask(gmac->txdma[GMACTXCFG >> 2]);
	base = gmac->txdma[GMACTXDBLOW >> 2];
	done = gmac->txdma[GMACTXDONE >> 2] & mask;
	kick = gmac->txdma[GMACTXKICK >> 2] & mask;
	while (done != kick) {
		descriptor = GmacGuest(machine, base + done * 16, 16);
		if (descriptor == NULL)
			break;
		control = LoadLittle(descriptor, 4);
		if (control & GMACTXSOF) {
			gmac->txsize = 0;
			gmac->txfirst = control;
		}
		length = control & 0x7fff;
		if (gmac->txsize + length > OSXG5GMACFRAME)
			length = OSXG5GMACFRAME - gmac->txsize;
		data = GmacGuest(machine, LoadLittle(descriptor + 8, 4), length);
		if (data)
			memcpy(gmac->txdata + gmac->txsize, data, length);
		gmac->txsize += length;
		if ((control & GMACTXEOF) && gmac->txsize > 0) {
			start = (gmac->txfirst >> 15) & 0x3f;
			stuff = (gmac->txfirst >> 21) & 0xff;
			if ((gmac->txfirst & GMACTXCENAB) && start + 2 <= gmac->txsize && stuff + 2 <= gmac->txsize) {
				checksum = GmacChecksum(gmac->txdata + start, gmac->txsize - start);
				gmac->txdata[stuff] = (uint8)(checksum >> 8);
				gmac->txdata[stuff + 1] = (uint8)checksum;
			}
			if (machine->bmac.ready)
				EtherSlirpSend(gmac->txdata, (int)gmac->txsize);
			machine->bmac.busydeadline = OsxCpuTimebase(machine->cpu) + OSXTIMEBASEFREQUENCY * 2;
			gmac->txsize = 0;
			gmac->txfirst = 0;
		}
		bits = GMACSTATTXDONE;
		if (LoadLittle(descriptor + 4, 4) & GMACTXINTME)
			bits |= GMACSTATTXINTME;
		done = (done + 1) & mask;
		gmac->txdma[GMACTXDONE >> 2] = done;
		GmacStatus(machine, bits);
	}
	GmacStatus(machine, GMACSTATTXALL);
}

static int GmacRxFull(OsxGmac *gmac, uint32 mask)
{
	return !gmac->rxkicked || (gmac->rxdma[GMACRXKICK >> 2] & mask) == ((gmac->rxdma[GMACRXDONE >> 2] + 1) & mask);
}

static int GmacAccepts(OsxGmac *gmac, const uint8 *frame)
{
	uint32 config;
	config = gmac->mac[GMACMACRXCFG >> 2];
	if ((config & 0x08) || (frame[0] & 1))
		return 1;
	return gmac->mac[GMACMACADDR0 >> 2] == (((uint32)frame[4] << 8) | frame[5]) &&
		gmac->mac[(GMACMACADDR0 >> 2) + 1] == (((uint32)frame[2] << 8) | frame[3]) &&
		gmac->mac[(GMACMACADDR0 >> 2) + 2] == (((uint32)frame[0] << 8) | frame[1]);
}

static int GmacDeliver(OsxMachine *machine, const uint8 *frame, uint32 length)
{
	OsxGmac *gmac;
	uint8 *descriptor;
	uint8 *buffer;
	uint32 mask;
	uint32 done;
	uint32 config;
	uint32 fcs;
	uint32 status;
	uint32 offset;
	gmac = &machine->g5state.gmac;
	config = gmac->rxdma[GMACRXCFG >> 2];
	mask = GmacRingMask(config);
	if (GmacRxFull(gmac, mask))
		return 0;
	if (!GmacAccepts(gmac, frame))
		return 1;
	done = gmac->rxdma[GMACRXDONE >> 2] & mask;
	descriptor = GmacGuest(machine, gmac->rxdma[GMACRXDBLOW >> 2] + done * 16, 16);
	if (descriptor == NULL)
		return 1;
	buffer = GmacGuest(machine, (LoadLittle(descriptor + 8, 4) & ~7u) + ((config & 0x1c00) >> 10), length);
	if (buffer) {
		memcpy(buffer, frame, length);
		OsxJitInvalidate(machine->cpu, (uint32)(buffer - machine->ram), length);
	}
	fcs = 4;
	if (gmac->mac[GMACMACRXCFG >> 2] & 4)
		fcs = 0;
	offset = (config >> 13) & 0x7f;
	status = (length + fcs) << 16;
	if (offset < length)
		status |= GmacChecksum(frame + offset, length - offset);
	StoreLittle(descriptor, 4, status);
	StoreLittle(descriptor + 4, 4, 0);
	done = (done + 1) & mask;
	gmac->rxdma[GMACRXDONE >> 2] = done;
	status = GMACSTATRXDONE;
	if (GmacRxFull(gmac, mask))
		status |= GMACSTATRXNOBUF;
	GmacStatus(machine, status);
	return 1;
}

void OsxG5GmacReceive(OsxMachine *machine)
{
	OsxGmac *gmac;
	OsxBmac *bmac;
	int length;
	gmac = &machine->g5state.gmac;
	bmac = &machine->bmac;
	while (bmac->ready && (gmac->rxdma[GMACRXCFG >> 2] & 1) && (gmac->mac[GMACMACRXCFG >> 2] & 1)) {
		if (bmac->pendinglength == 0) {
			length = EtherSlirpReceive(bmac->pending);
			if (length <= 0)
				return;
			if (length > 1514)
				length = 1514;
			if (length < 60) {
				memset(bmac->pending + length, 0, 60 - length);
				length = 60;
			}
			bmac->pendinglength = length;
			bmac->busydeadline = OsxCpuTimebase(machine->cpu) + OSXTIMEBASEFREQUENCY * 2;
		}
		if (!GmacDeliver(machine, bmac->pending, (uint32)bmac->pendinglength))
			return;
		bmac->pendinglength = 0;
	}
}

static uint32 GmacRead(OsxMachine *machine, uint32 offset)
{
	OsxGmac *gmac;
	uint32 value;
	gmac = &machine->g5state.gmac;
	if (offset < 0x2000) {
		if (offset >= 0x20 && (offset < 0x1000 || offset > 0x1010))
			return 0;
		value = gmac->gregs[offset >> 2];
		if (offset == GMACSTAT) {
			gmac->gregs[GMACSTAT >> 2] &= ~GMACSTATLATCH;
			GmacUpdate(machine);
		}
		if (offset == GMACSTAT || offset == GMACSTAT2)
			value = (value & ~GMACSTATTXNR) | (gmac->txdma[GMACTXDONE >> 2] << 19);
		return value;
	}
	if (offset < 0x3000)
		return gmac->txdma[(offset - 0x2000) >> 2];
	if (offset < 0x4000)
		return 0xffffffffu;
	if (offset < 0x6000)
		return gmac->rxdma[(offset - 0x4000) >> 2];
	if (offset < 0x6200)
		return gmac->mac[(offset - 0x6000) >> 2];
	if (offset < 0x6220)
		return gmac->mif[(offset - 0x6200) >> 2];
	return 0;
}

static void GmacWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxGmac *gmac;
	uint32 reg;
	gmac = &machine->g5state.gmac;
	if (offset < 0x2000) {
		if (offset == GMACIACK) {
			gmac->gregs[GMACSTAT >> 2] &= ~(value & GMACSTATLATCH);
			GmacUpdate(machine);
			return;
		}
		if (offset == GMACIMASK) {
			gmac->gregs[GMACIMASK >> 2] = value;
			GmacUpdate(machine);
			return;
		}
		if (offset == GMACSWRST) {
			if ((value & 3) == 3)
				GmacReset(machine);
			else if (value & 2)
				GmacResetRx(gmac);
			else if (value & 1)
				GmacResetTx(gmac);
			gmac->gregs[GMACSWRST >> 2] = value & ~3u;
			GmacUpdate(machine);
			return;
		}
		if (offset >= 0x1000 && offset < 0x1010)
			gmac->gregs[offset >> 2] = value;
		return;
	}
	if (offset < 0x3000) {
		reg = offset - 0x2000;
		if (reg >= GMACTXDONE || (reg >= 0x24 && reg < 0x38))
			return;
		gmac->txdma[reg >> 2] = value;
		if (reg == GMACTXKICK)
			GmacTransmit(machine);
		return;
	}
	if (offset < 0x4000)
		return;
	if (offset < 0x6000) {
		reg = offset - 0x4000;
		if (reg == GMACRXDONE || (reg >= 0x18 && reg < 0x20) || (reg >= 0x24 && reg < 0x2c) || reg >= 0x110)
			return;
		gmac->rxdma[reg >> 2] = value;
		if (reg == GMACRXKICK) {
			gmac->rxkicked = 1;
			OsxG5GmacReceive(machine);
		}
		return;
	}
	if (offset < 0x6200) {
		reg = offset - 0x6000;
		if (reg < 0x8 || (reg >= 0x10 && reg < 0x1c) || reg == 0x114 || reg == 0x134)
			return;
		gmac->mac[reg >> 2] = value;
		return;
	}
	if (offset < 0x6220) {
		reg = offset - 0x6200;
		if (reg == GMACMIFFRAME)
			gmac->mif[reg >> 2] = GmacPhyFrame(value);
		else if (reg == GMACMIFCFG)
			gmac->mif[reg >> 2] = (value & ~0x200u) | 0x100;
		return;
	}
}

uint32 OsxG5BusRead(OsxMachine *machine, uint32 address, int size)
{
	if (address - OSXG5GMACBASE < OSXG5GMACSIZE)
		return bswap_32(GmacRead(machine, (address - OSXG5GMACBASE) & ~3u)) >> ((4 - size) * 8);
	if (address - OSXG5MACIOBASE < OSXG5MACIOSIZE)
		return MacIoRead(machine, address - OSXG5MACIOBASE, size);
	if (address - OSXG5ATABASE < OSXG5ATASIZE * 2)
		return AtaRead(machine, address - OSXG5ATABASE, size);
	if (address - OSXG5HTCONFIG < OSXG5HTCONFIGSIZE)
		return ConfigRead(machine, address - OSXG5HTCONFIG, size);
	if (address - OSXG5U3BASE < OSXG5U3SIZE)
		return U3Read(machine, address - OSXG5U3BASE, size);
	if (address - OSXFLASHBASE < OSXFLASHSIZE)
		return OsxFlashRead(machine, address - OSXFLASHBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "g5 bus read %08x size %d pc %08x\n", address, size, machine->cpu->pc);
	return AllOnes(size);
}

void OsxG5BusWrite(OsxMachine *machine, uint32 address, int size, uint32 value)
{
	if (address - OSXG5GMACBASE < OSXG5GMACSIZE) {
		if (size == 4)
			GmacWrite(machine, address - OSXG5GMACBASE, bswap_32(value));
		return;
	}
	if (address - OSXG5MACIOBASE < OSXG5MACIOSIZE) {
		MacIoWrite(machine, address - OSXG5MACIOBASE, size, value);
		return;
	}
	if (address - OSXG5ATABASE < OSXG5ATASIZE * 2) {
		AtaWrite(machine, address - OSXG5ATABASE, size, value);
		return;
	}
	if (address - OSXG5HTCONFIG < OSXG5HTCONFIGSIZE) {
		ConfigWrite(machine, address - OSXG5HTCONFIG, size, value);
		return;
	}
	if (address - OSXG5U3BASE < OSXG5U3SIZE) {
		U3Write(machine, address - OSXG5U3BASE, size, value);
		return;
	}
	if (address - OSXFLASHBASE < OSXFLASHSIZE) {
		OsxFlashWrite(machine, address - OSXFLASHBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "g5 bus write %08x size %d value %08x pc %08x\n", address, size, value, machine->cpu->pc);
}

#endif
