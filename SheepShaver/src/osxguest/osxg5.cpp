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
#include "osxhost.h"

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
#define I2CTAS3004 0x35
#define I2CCS8420 0x10
#define CS8420INCREMENT 0x80
#define CS8420VERSION 0x7f
#define TASVOLUME 0x04
#define TASANALOG 0x40
#define I2SINTERRUPT 0x00
#define I2SFORMAT 0x10
#define I2SFRAMECOUNT 0x40
#define I2SWORDSIZES 0x60
#define I2SCLOCKSSTOPPED 0x01000000u
#define I2SPENDING 0x55550000u
#define K2FCR1 0x3c
#define K2FCR1I2SCELL 0x00000400u
#define K2FCR1I2SRESET 0x00000800u
#define K2FCR1I2SCLOCK 0x00001000u

#define GPIOINDATA 0x02
#define GPIOPMU 0x09

#define K2COUNTERFREQUENCY 18432000
#define K2COUNTER 0x15038

#define DARTENABLE 0x200
#define DARTFLUSH 0x400
#define DARTVALID 0x80000000u

#define ATAIRQDMA 0x80000000u
#define ATAIRQDRIVE 0x40000000u

static void OsxGuestGmacReset(OsxMachine *machine);

static uint32 OsxGuestLoadLittle(const uint8 *bytes, int size)
{
	uint32 value;
	int index;
	value = 0;
	for (index = size - 1; index >= 0; index--)
		value = (value << 8) | bytes[index];
	return value;
}

static void OsxGuestStoreLittle(uint8 *bytes, int size, uint32 value)
{
	int index;
	for (index = 0; index < size; index++)
		bytes[index] = (uint8)(value >> (index * 8));
}

static uint32 OsxGuestBusFromBytes(const uint8 *bytes, int size)
{
	uint32 value;
	int index;
	value = 0;
	for (index = 0; index < size; index++)
		value = (value << 8) | bytes[index];
	return value;
}

static void OsxGuestBytesFromBus(uint8 *bytes, int size, uint32 value)
{
	int index;
	for (index = 0; index < size; index++)
		bytes[index] = (uint8)(value >> ((size - 1 - index) * 8));
}

static uint32 OsxGuestAllOnes(int size)
{
	return 0xffffffffu >> ((4 - size) * 8);
}

static void OsxGuestSetFunction(OsxG5Function *function, uint32 device, uint32 vendordevice, uint32 classrevision, uint32 bar, uint32 barsize)
{
	memset(function, 0, sizeof(OsxG5Function));
	function->device = device;
	OsxGuestStoreLittle(function->config, 4, vendordevice);
	OsxGuestStoreLittle(function->config + 4, 4, 0x02000006);
	OsxGuestStoreLittle(function->config + 8, 4, classrevision);
	OsxGuestStoreLittle(function->config + 0x10, 4, bar);
	if (barsize)
		function->barmask[0] = ~(barsize - 1);
}

static void OsxGuestMpicReset(OsxMpic *mpic)
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

void OsxGuestG5Reset(OsxMachine *machine)
{
	OsxG5 *g5;
	uint32 highsize;
	g5 = &machine->g5state;
	highsize = g5->highsize;
	memset(g5, 0, sizeof(OsxG5));
	g5->highsize = highsize;
	OsxGuestSetFunction(&g5->functions[0], OSXG5DEVICEHOST, 0x0057106b, 0x06000000, 0, 0);
	OsxGuestSetFunction(&g5->functions[1], OSXG5DEVICEMACIO, 0x0041106b, 0xff000000, OSXG5MACIOBASE, OSXG5MACIOSIZE);
	OsxGuestSetFunction(&g5->functions[2], OSXG5DEVICEATA0, 0x0043106b, 0x01018f00, OSXG5ATABASE, OSXG5ATASIZE);
	OsxGuestSetFunction(&g5->functions[3], OSXG5DEVICEATA1, 0x0043106b, 0x01018f00, OSXG5ATABASE + OSXG5ATASIZE, OSXG5ATASIZE);
	OsxGuestSetFunction(&g5->functions[4], OSXG5DEVICEDISPLAY, 0x11111234, 0x03000002, OSXVRAMBASE | 8, OSXVRAMSIZE);
	OsxGuestSetFunction(&g5->functions[5], OSXG5DEVICEGMAC, 0x004c106b, 0x02000000, OSXG5GMACBASE, OSXG5GMACSIZE);
	OsxGuestStoreLittle(g5->functions[5].config + 0x3c, 4, 0x0100 | OSXG5IRQGMAC);
	OsxGuestStoreLittle(g5->functions[0].config + 0x80, 4, 0x01000000);
	OsxGuestStoreLittle(g5->functions[2].config + 0x3c, 4, 0x0100 | OSXG5IRQATA0);
	OsxGuestStoreLittle(g5->functions[3].config + 0x3c, 4, 0x0100 | OSXG5IRQATA1);
	OsxGuestMpicReset(&g5->mpic);
	g5->i2c.device = I2CPULSAR;
	g5->i2c.width = 1;
	g5->k2i2c.device = I2CTAS3004;
	g5->k2i2c.width = OSXG5I2CWIDTH;
	g5->k2i2c.secondary = I2CCS8420;
	g5->k2i2c.receiver[CS8420VERSION] = 0x11;
	g5->i2sinterrupt = I2SCLOCKSSTOPPED;
	machine->dbdma[OSXG5DBDMAI2SOUT].irq = OSXG5IRQDIRECT | OSXG5IRQI2STX;
	machine->dbdma[OSXG5DBDMAI2SIN].irq = OSXG5IRQDIRECT | OSXG5IRQI2SRX;
	g5->gpio[GPIOPMU] = GPIOINDATA;
	machine->dbdma[OSXDBDMAIDE0].irq = OSXIRQIDE0DMA;
	machine->dbdma[OSXDBDMAIDE1].irq = OSXIRQIDE1DMA;
	OsxGuestGmacReset(machine);
}

static int OsxGuestMpicPriority(uint32 ivpr)
{
	return (int)((ivpr >> 16) & 15);
}

static int OsxGuestMpicPending(OsxMpic *mpic, int source)
{
	if (source < MPICSOURCESK2 && (mpic->ivpr[source] & MPICSENSE))
		return mpic->line[source];
	return mpic->pending[source];
}

static int OsxGuestMpicBest(OsxMpic *mpic)
{
	int floor;
	int best;
	int index;
	int priority;
	floor = (int)mpic->ctpr;
	if (mpic->inservicecount > 0) {
		priority = OsxGuestMpicPriority(mpic->ivpr[mpic->inservice[mpic->inservicecount - 1]]);
		if (priority > floor)
			floor = priority;
	}
	best = -1;
	for (index = 0; index < OSXG5MPICSOURCES; index++) {
		if (!OsxGuestMpicPending(mpic, index) || (mpic->ivpr[index] & MPICMASK) || !(mpic->idr[index] & 1))
			continue;
		priority = OsxGuestMpicPriority(mpic->ivpr[index]);
		if (priority > floor) {
			floor = priority;
			best = index;
		}
	}
	return best;
}

static void OsxGuestMpicUpdate(OsxMachine *machine)
{
	int line;
	line = OsxGuestMpicBest(&machine->g5state.mpic) >= 0;
	machine->cpu->externalline = line;
	if (line)
		machine->cpu->halted = 0;
}

static void OsxGuestMpicSetSource(OsxMachine *machine, int source, int level)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (level && !mpic->line[source])
		mpic->pending[source] = 1;
	mpic->line[source] = (uint8)(level != 0);
	OsxGuestMpicUpdate(machine);
}

static uint32 OsxGuestMpicAcknowledge(OsxMachine *machine)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	source = OsxGuestMpicBest(mpic);
	if (source < 0)
		return mpic->spvr & 0xff;
	if (source >= MPICSOURCESK2 || !(mpic->ivpr[source] & MPICSENSE))
		mpic->pending[source] = 0;
	if (mpic->inservicecount < 16)
		mpic->inservice[mpic->inservicecount++] = (uint8)source;
	OsxGuestMpicUpdate(machine);
	return mpic->ivpr[source] & 0xff;
}

static void OsxGuestMpicEndOfInterrupt(OsxMachine *machine)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (mpic->inservicecount > 0)
		mpic->inservicecount--;
	OsxGuestMpicUpdate(machine);
}

static int OsxGuestMpicInService(OsxMpic *mpic, int source)
{
	int index;
	for (index = 0; index < mpic->inservicecount; index++)
		if (mpic->inservice[index] == source)
			return 1;
	return 0;
}

static uint32 OsxGuestMpicSourceVector(OsxMpic *mpic, int source)
{
	uint32 value;
	value = mpic->ivpr[source] & ~MPICACTIVITY;
	if (OsxGuestMpicPending(mpic, source) || OsxGuestMpicInService(mpic, source))
		value |= MPICACTIVITY;
	return value;
}

static uint32 OsxGuestMpicCpuRead(OsxMachine *machine, uint32 reg)
{
	switch (reg) {
	case 0x80: return machine->g5state.mpic.ctpr;
	case 0x90: return 0;
	case 0xa0: return OsxGuestMpicAcknowledge(machine);
	}
	return 0;
}

static void OsxGuestMpicCpuWrite(OsxMachine *machine, uint32 reg, uint32 value)
{
	OsxMpic *mpic;
	mpic = &machine->g5state.mpic;
	if (reg >= 0x40 && reg < 0x80) {
		if (value & 1)
			mpic->pending[OSXG5MPICIPI + ((reg - 0x40) >> 4)] = 1;
		OsxGuestMpicUpdate(machine);
		return;
	}
	if (reg == 0x80) {
		mpic->ctpr = value & 15;
		OsxGuestMpicUpdate(machine);
		return;
	}
	if (reg == 0xb0)
		OsxGuestMpicEndOfInterrupt(machine);
}

static uint32 OsxGuestMpicRead(OsxMachine *machine, uint32 offset)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	if (offset < 0x1000)
		return OsxGuestMpicCpuRead(machine, offset & 0xff0);
	if (offset >= 0x20000 && offset < 0x21000)
		return OsxGuestMpicCpuRead(machine, offset & 0xff0);
	if (offset >= 0x10000 && offset < 0x10000 + MPICSOURCESK2 * 0x20) {
		source = (int)((offset - 0x10000) >> 5);
		if (offset & 0x10)
			return mpic->idr[source];
		return OsxGuestMpicSourceVector(mpic, source);
	}
	if (offset >= 0x10a0 && offset < 0x10e0)
		return OsxGuestMpicSourceVector(mpic, OSXG5MPICIPI + (int)((offset - 0x10a0) >> 4));
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

static void OsxGuestMpicWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxMpic *mpic;
	int source;
	mpic = &machine->g5state.mpic;
	if (offset < 0x1000 || (offset >= 0x20000 && offset < 0x21000)) {
		OsxGuestMpicCpuWrite(machine, offset & 0xff0, value);
		return;
	}
	if (offset >= 0x10000 && offset < 0x10000 + MPICSOURCESK2 * 0x20) {
		source = (int)((offset - 0x10000) >> 5);
		if (offset & 0x10)
			mpic->idr[source] = value;
		else
			mpic->ivpr[source] = value & ~MPICACTIVITY;
		OsxGuestMpicUpdate(machine);
		return;
	}
	if (offset >= 0x10a0 && offset < 0x10e0) {
		mpic->ivpr[OSXG5MPICIPI + ((offset - 0x10a0) >> 4)] = value & ~MPICACTIVITY;
		mpic->idr[OSXG5MPICIPI + ((offset - 0x10a0) >> 4)] = 1;
		OsxGuestMpicUpdate(machine);
		return;
	}
	if (offset >= 0x1100 && offset < 0x1200) {
		mpic->timers[(offset - 0x1100) >> 4] = value;
		return;
	}
	switch (offset) {
	case 0x1020:
		if (value & MPICRESET)
			OsxGuestMpicReset(mpic);
		mpic->gcr = value & ~MPICRESET;
		OsxGuestMpicUpdate(machine);
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

void OsxGuestG5SetLine(OsxMachine *machine, int source, int level)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	switch (source) {
	case OSXIRQIDE0:
		machine->dbdma[OSXDBDMAIDE0].status = (machine->dbdma[OSXDBDMAIDE0].status & ~0x80u) | (level ? 0x80u : 0);
		OsxGuestMpicSetSource(machine, OSXG5IRQATA0, level);
		break;
	case OSXIRQIDE1:
		machine->dbdma[OSXDBDMAIDE1].status = (machine->dbdma[OSXDBDMAIDE1].status & ~0x80u) | (level ? 0x80u : 0);
		OsxGuestMpicSetSource(machine, OSXG5IRQATA1, level);
		break;
	case OSXIRQIDE0DMA:
		if (level)
			g5->atairq[0] |= ATAIRQDMA;
		OsxGuestMpicSetSource(machine, OSXG5IRQATA0DMA, level);
		break;
	case OSXIRQIDE1DMA:
		if (level)
			g5->atairq[1] |= ATAIRQDMA;
		OsxGuestMpicSetSource(machine, OSXG5IRQATA1DMA, level);
		break;
	case OSXIRQCUDA:
		OsxGuestMpicSetSource(machine, OSXG5IRQPMU, level);
		break;
	default:
		if (source & OSXG5IRQDIRECT)
			OsxGuestMpicSetSource(machine, source & (OSXG5IRQDIRECT - 1), level);
		break;
	}
}

void OsxGuestG5SetPmuRequest(OsxMachine *machine, int level)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (level)
		g5->gpio[GPIOPMU] &= ~GPIOINDATA;
	else
		g5->gpio[GPIOPMU] |= GPIOINDATA;
	OsxGuestMpicSetSource(machine, OSXG5IRQPMUGPIO, level);
}

static OsxG5Function *OsxGuestFindFunction(OsxMachine *machine, uint32 offset, uint32 *reg)
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

static uint32 OsxGuestConfigRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxG5Function *function;
	uint32 reg;
	function = OsxGuestFindFunction(machine, offset, &reg);
	if (function == NULL || reg + size > 256)
		return OsxGuestAllOnes(size);
	return OsxGuestBusFromBytes(function->config + reg, size);
}

static void OsxGuestConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxG5Function *function;
	uint8 updated[256];
	uint32 reg;
	uint32 dword;
	uint32 bar;
	int index;
	function = OsxGuestFindFunction(machine, offset, &reg);
	if (function == NULL || reg + size > 256)
		return;
	memcpy(updated, function->config, 256);
	OsxGuestBytesFromBus(updated + reg, size, value);
	memcpy(function->config + 4, updated + 4, 2);
	memcpy(function->config + 0x0c, updated + 0x0c, 2);
	function->config[0x3c] = updated[0x3c];
	for (index = 0; index < 6; index++) {
		bar = 0x10 + index * 4;
		if (reg + size <= bar || reg >= bar + 4)
			continue;
		dword = OsxGuestLoadLittle(updated + bar, 4);
		dword = (dword & function->barmask[index]) | (OsxGuestLoadLittle(function->config + bar, 4) & 0xf);
		OsxGuestStoreLittle(function->config + bar, 4, dword);
	}
}

static uint32 OsxGuestHtSelfRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 reg;
	reg = offset >> 2;
	if (reg + size > 256)
		return 0;
	return OsxGuestBusFromBytes(machine->g5state.functions[0].config + reg, size);
}

static void OsxGuestI2cStop(OsxKeyWest *i2c)
{
	i2c->active = 0;
	i2c->status &= ~I2CSTATUSBUSY;
	i2c->control = 0;
}

static int OsxGuestI2cStart(OsxKeyWest *i2c)
{
	int mode;
	mode = i2c->mode & I2CMODEMASK;
	i2c->active = 0;
	i2c->status &= ~I2CSTATUSBUSY;
	i2c->selected = 0;
	if (i2c->secondary && (i2c->addr >> 1) == i2c->secondary)
		i2c->selected = 1;
	else if ((i2c->addr >> 1) != i2c->device)
		return 0;
	i2c->active = 1;
	i2c->status |= I2CSTATUSBUSY;
	if (!(i2c->addr & 1) || mode == I2CMODECOMBINED)
		i2c->pointerset = 0;
	if (mode == I2CMODESTANDARDSUB || mode == I2CMODECOMBINED) {
		i2c->pointer = i2c->subaddr;
		i2c->map = i2c->subaddr;
		i2c->pointerset = 1;
	}
	i2c->position = 0;
	return 1;
}

static uint8 OsxGuestI2cFetch(OsxKeyWest *i2c)
{
	uint8 value;
	if (i2c->selected) {
		value = i2c->receiver[i2c->map & CS8420VERSION];
		if (i2c->map & CS8420INCREMENT)
			i2c->map = (uint8)((i2c->map & CS8420INCREMENT) | ((i2c->map + 1) & CS8420VERSION));
		return value;
	}
	if (i2c->width == 1)
		return i2c->memory[i2c->pointer++];
	value = 0;
	if (i2c->position < i2c->width)
		value = i2c->memory[i2c->pointer * i2c->width + i2c->position];
	i2c->position++;
	return value;
}

static void OsxGuestI2cStore(OsxKeyWest *i2c, uint8 value)
{
	if (i2c->selected) {
		if ((i2c->map & CS8420VERSION) != CS8420VERSION)
			i2c->receiver[i2c->map & CS8420VERSION] = value;
		if (i2c->map & CS8420INCREMENT)
			i2c->map = (uint8)((i2c->map & CS8420INCREMENT) | ((i2c->map + 1) & CS8420VERSION));
		return;
	}
	if (i2c->width == 1) {
		i2c->memory[i2c->pointer++] = value;
		return;
	}
	if (i2c->position < i2c->width)
		i2c->memory[i2c->pointer * i2c->width + i2c->position] = value;
	if (i2c->pointer == TASVOLUME)
		i2c->volumewritten = 1;
	i2c->position++;
}

static void OsxGuestI2cReadByte(OsxKeyWest *i2c)
{
	i2c->data = OsxGuestI2cFetch(i2c);
	i2c->status |= I2CSTATUSLASTAAK;
	i2c->lastnak = !(i2c->control & I2CCONTROLAAK);
	i2c->isr |= I2CIRQDATA;
}

static uint32 OsxGuestI2cRead(OsxKeyWest *i2c, uint32 reg)
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

static void OsxGuestI2cControl(OsxKeyWest *i2c, uint8 value)
{
	uint8 rising;
	int ack;
	rising = value & ~i2c->control;
	i2c->control = value;
	if (rising & I2CCONTROLXADDR) {
		ack = OsxGuestI2cStart(i2c);
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
			OsxGuestI2cStop(i2c);
			i2c->isr |= I2CIRQSTOP;
		}
	}
	if (rising & I2CCONTROLSTOP) {
		OsxGuestI2cStop(i2c);
		i2c->isr |= I2CIRQSTOP;
	}
}

static void OsxGuestI2cAcknowledge(OsxKeyWest *i2c, uint8 value)
{
	uint8 previous;
	int mode;
	previous = i2c->isr;
	i2c->isr &= ~(value & 0x0f);
	if ((value & I2CIRQADDR) && i2c->active && i2c->readpending) {
		i2c->readpending = 0;
		OsxGuestI2cReadByte(i2c);
	}
	mode = i2c->mode & I2CMODEMASK;
	if (!(value & I2CIRQDATA) || !i2c->active)
		return;
	if ((i2c->addr & 1) && (mode == I2CMODECOMBINED || mode == I2CMODESTANDARD)) {
		if (!(previous & I2CIRQDATA))
			return;
		if (i2c->lastnak) {
			OsxGuestI2cStop(i2c);
			i2c->isr |= I2CIRQSTOP;
		} else {
			OsxGuestI2cReadByte(i2c);
		}
		return;
	}
	if (i2c->addr & 1) {
		OsxGuestI2cReadByte(i2c);
		return;
	}
	i2c->isr |= I2CIRQDATA;
}

static void OsxGuestI2cData(OsxKeyWest *i2c, uint8 value)
{
	i2c->data = value;
	if (i2c->active && !(i2c->addr & 1)) {
		if (i2c->pointerset) {
			OsxGuestI2cStore(i2c, value);
		} else {
			i2c->pointer = value;
			i2c->map = value;
			i2c->pointerset = 1;
			i2c->position = 0;
		}
		i2c->status |= I2CSTATUSLASTAAK;
		i2c->isr |= I2CIRQDATA;
		return;
	}
	i2c->status &= ~I2CSTATUSLASTAAK;
	OsxGuestI2cStop(i2c);
	i2c->isr |= I2CIRQDATA | I2CIRQSTOP;
}

static void OsxGuestI2cWrite(OsxKeyWest *i2c, uint32 reg, uint8 value)
{
	switch (reg) {
	case I2CMODE: i2c->mode = value; break;
	case I2CCONTROL: OsxGuestI2cControl(i2c, value); break;
	case I2CSTATUS: i2c->status = value; break;
	case I2CISR: OsxGuestI2cAcknowledge(i2c, value); break;
	case I2CIER: i2c->ier = value; break;
	case I2CADDR: i2c->addr = value; break;
	case I2CSUBADDR: i2c->subaddr = value; break;
	case I2CDATA: OsxGuestI2cData(i2c, value); break;
	}
}

static uint8 *OsxGuestPhysicalHost(OsxMachine *machine, uint64 physical, uint32 length)
{
	uint64 high;
	if (physical < machine->ramsize && length <= machine->ramsize - physical)
		return machine->ram + physical;
	high = physical - OSXG5HIGHRAM;
	if (physical >= OSXG5HIGHRAM && high < machine->g5state.highsize && length <= machine->g5state.highsize - high)
		return machine->ram + machine->ramsize + high;
	return NULL;
}

uint8 *OsxGuestG5DmaHost(OsxMachine *machine, uint32 bus, uint32 length)
{
	OsxG5 *g5;
	uint8 *table;
	uint32 pages;
	uint32 entry;
	g5 = &machine->g5state;
	if (!(g5->dartcontrol & DARTENABLE))
		return OsxGuestPhysicalHost(machine, bus, length);
	if ((bus & 0xfff) + length > 0x1000)
		return NULL;
	pages = g5->dartcontrol & 0x1ff;
	if (pages == 0)
		pages = 512;
	if ((bus >> 12) >= pages * 1024)
		return NULL;
	table = OsxGuestPhysicalHost(machine, ((uint64)((g5->dartcontrol >> 12) & 0xfffff) << 12) + (bus >> 12) * 4, 4);
	if (table == NULL)
		return NULL;
	entry = OsxGuestBusFromBytes(table, 4);
	if (!(entry & DARTVALID)) {
		g5->dartexception = bus >> 12;
		return NULL;
	}
	return OsxGuestPhysicalHost(machine, ((uint64)(entry & 0x00ffffff) << 12) | (bus & 0xfff), length);
}

int OsxGuestG5DmaCopy(OsxMachine *machine, uint32 bus, uint8 *data, uint32 length, int towardmemory)
{
	uint8 *host;
	uint32 chunk;
	uint32 done;
	done = 0;
	while (done < length) {
		chunk = 0x1000 - ((bus + done) & 0xfff);
		if (chunk > length - done)
			chunk = length - done;
		host = OsxGuestG5DmaHost(machine, bus + done, chunk);
		if (host == NULL)
			return 0;
		if (towardmemory) {
			memcpy(host, data + done, chunk);
			OsxGuestJitInvalidate(machine->cpu, (uint32)(host - machine->ram), chunk);
		} else {
			memcpy(data + done, host, chunk);
		}
		done += chunk;
	}
	return 1;
}

static uint64 OsxGuestK2Counter(OsxMachine *machine)
{
	uint64 timebase;
	timebase = OsxGuestCpuTimebase(machine->cpu);
	return (timebase / OSXTIMEBASEFREQUENCY) * K2COUNTERFREQUENCY + (timebase % OSXTIMEBASEFREQUENCY) * K2COUNTERFREQUENCY / OSXTIMEBASEFREQUENCY;
}

static void OsxGuestTasUpdateGain(OsxMachine *machine)
{
	OsxKeyWest *tas;
	const uint8 *volume;
	tas = &machine->g5state.k2i2c;
	volume = tas->memory + TASVOLUME * OSXG5I2CWIDTH;
	if (tas->memory[TASANALOG * OSXG5I2CWIDTH] & 1) {
		machine->audio.leftgain = 0;
		machine->audio.rightgain = 0;
	} else if (tas->volumewritten) {
		machine->audio.leftgain = (int32)(((uint32)volume[0] << 16) | ((uint32)volume[1] << 8) | volume[2]);
		machine->audio.rightgain = (int32)(((uint32)volume[3] << 16) | ((uint32)volume[4] << 8) | volume[5]);
	}
}

static int OsxGuestI2sDivider(uint32 field, int sclk)
{
	if (sclk) {
		if (field == 8)
			return 1;
		if (field == 9)
			return 3;
		return (int)(field + 1) * 2;
	}
	if (field == 0x14)
		return 1;
	if (field == 0x13)
		return 3;
	if (field == 0x12)
		return 5;
	return (int)(field + 1) * 2;
}

static void OsxGuestI2sUpdateRate(OsxMachine *machine)
{
	static const uint32 clocks[4] = { 18432000, 45158400, 49152000, 0 };
	uint32 format;
	uint32 source;
	uint32 rate;
	format = machine->g5state.i2sformat;
	source = clocks[(format >> 30) & 3];
	rate = 44100;
	if (source)
		rate = source / (uint32)(OsxGuestI2sDivider((format >> 24) & 0x1f, 0) * OsxGuestI2sDivider((format >> 20) & 0x0f, 1) * 64);
	if (rate >= 8000 && rate <= 96000)
		machine->audio.rate = rate;
}

static void OsxGuestI2sUpdateClocks(OsxMachine *machine)
{
	uint32 fcr1;
	fcr1 = bswap_32(machine->g5state.fcr[K2FCR1 >> 2]);
	if (!(fcr1 & K2FCR1I2SCELL) || !(fcr1 & K2FCR1I2SCLOCK) || (fcr1 & K2FCR1I2SRESET))
		machine->g5state.i2sinterrupt |= I2SCLOCKSSTOPPED;
}

static uint32 OsxGuestI2sRead(OsxMachine *machine, uint32 reg)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	switch (reg) {
	case I2SINTERRUPT: return g5->i2sinterrupt;
	case I2SFORMAT: return g5->i2sformat;
	case I2SWORDSIZES: return g5->i2swordsizes;
	case I2SFRAMECOUNT: return g5->i2sframes + g5->i2sframebias;
	}
	return 0;
}

static void OsxGuestI2sWrite(OsxMachine *machine, uint32 reg, uint32 value)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	switch (reg) {
	case I2SINTERRUPT:
		g5->i2sinterrupt = (g5->i2sinterrupt & I2SPENDING & ~(value & I2SPENDING)) | (value & ~I2SPENDING);
		break;
	case I2SFORMAT:
		g5->i2sformat = value;
		OsxGuestI2sUpdateRate(machine);
		break;
	case I2SWORDSIZES:
		g5->i2swordsizes = value;
		break;
	case I2SFRAMECOUNT:
		g5->i2sframebias = value - g5->i2sframes;
		break;
	}
}

static uint32 OsxGuestMacIoRead(OsxMachine *machine, uint32 offset, int size)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (offset >= 0x10000 && offset < 0x10100)
		return bswap_32(OsxGuestI2sRead(machine, offset & 0xf0)) >> ((4 - size) * 8);
	if (offset >= 0x18000 && offset < 0x19000)
		return OsxGuestI2cRead(&g5->k2i2c, offset & 0xf0) << ((size - 1) * 8);
	if (offset >= 0x8000 && offset < 0x9000)
		return OsxGuestHeathrowRead(machine, offset, size);
	if (offset == K2COUNTER && size == 4)
		return bswap_32((uint32)OsxGuestK2Counter(machine));
	if (offset == K2COUNTER + 4 && size == 4)
		return bswap_32((uint32)(OsxGuestK2Counter(machine) >> 32));
	if (offset >= OSXG5MPICOFFSET)
		return bswap_32(OsxGuestMpicRead(machine, (offset - OSXG5MPICOFFSET) & ~3u)) >> ((4 - size) * 8);
	if (offset >= 0x16000 && offset < 0x18000)
		return OsxGuestCudaRead(machine, (offset >> 9) & 15);
	if (offset < OSXG5GPIOBASE)
		return g5->fcr[offset >> 2] >> ((4 - size) * 8);
	if (offset - OSXG5GPIOBASE + size <= OSXG5GPIOSIZE)
		return OsxGuestBusFromBytes(g5->gpio + offset - OSXG5GPIOBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "k2 read %05x size %d\n", offset, size);
	return 0;
}

static void OsxGuestMacIoWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	OsxG5 *g5;
	g5 = &machine->g5state;
	if (offset >= 0x8000 && offset < 0x9000) {
		OsxGuestHeathrowWrite(machine, offset, size, value);
		return;
	}
	if (offset >= OSXG5MPICOFFSET) {
		if (size == 4)
			OsxGuestMpicWrite(machine, (offset - OSXG5MPICOFFSET) & ~3u, bswap_32(value));
		return;
	}
	if (offset >= 0x16000 && offset < 0x18000) {
		OsxGuestCudaWrite(machine, (offset >> 9) & 15, value & 0xff);
		return;
	}
	if (offset >= 0x10000 && offset < 0x10100) {
		if (size == 4)
			OsxGuestI2sWrite(machine, offset & 0xf0, bswap_32(value));
		return;
	}
	if (offset >= 0x18000 && offset < 0x19000) {
		OsxGuestI2cWrite(&g5->k2i2c, offset & 0xf0, (uint8)(value >> ((size - 1) * 8)));
		OsxGuestMpicSetSource(machine, OSXG5IRQK2I2C, (g5->k2i2c.isr & g5->k2i2c.ier) != 0);
		OsxGuestTasUpdateGain(machine);
		return;
	}
	if (offset < OSXG5GPIOBASE) {
		if (size == 4)
			g5->fcr[offset >> 2] = value;
		OsxGuestI2sUpdateClocks(machine);
		return;
	}
	if (offset - OSXG5GPIOBASE + size <= OSXG5GPIOSIZE) {
		OsxGuestBytesFromBus(g5->gpio + offset - OSXG5GPIOBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "k2 write %05x size %d value %08x\n", offset, size, value);
}

static uint32 OsxGuestG5AtaRead(OsxMachine *machine, uint32 offset, int size)
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
		return OsxGuestHeathrowRead(machine, 0x8000 + (OSXDBDMAIDE0 + bus) * 0x100 + (reg & 0xff), size);
	if ((reg & 0xfff) == 0x300) {
		value = g5->atairq[bus] & ATAIRQDMA;
		if (machine->ata[bus].irqline)
			value |= ATAIRQDRIVE;
		return bswap_32(value) >> ((4 - size) * 8);
	}
	return OsxGuestHeathrowRead(machine, 0x20000 + bus * 0x1000 + (reg & 0xfff), size);
}

static void OsxGuestG5AtaWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
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
		OsxGuestHeathrowWrite(machine, 0x8000 + (OSXDBDMAIDE0 + bus) * 0x100 + (reg & 0xff), size, value);
		return;
	}
	if ((reg & 0xfff) == 0x300) {
		if (size == 4 && (bswap_32(value) & ATAIRQDMA))
			g5->atairq[bus] &= ~ATAIRQDMA;
		return;
	}
	OsxGuestHeathrowWrite(machine, 0x20000 + bus * 0x1000 + (reg & 0xfff), size, value);
}

static uint32 OsxGuestU3Read(OsxMachine *machine, uint32 offset, int size)
{
	if (offset - (OSXG5U3I2C - OSXG5U3BASE) < 0x1000)
		return OsxGuestI2cRead(&machine->g5state.i2c, (offset - (OSXG5U3I2C - OSXG5U3BASE)) & 0xf0) << ((size - 1) * 8);
	if (offset - (OSXG5HTSELF - OSXG5U3BASE) < 0x1000)
		return OsxGuestHtSelfRead(machine, offset - (OSXG5HTSELF - OSXG5U3BASE), size);
	if (offset == OSXG5DART - OSXG5U3BASE && size == 4)
		return machine->g5state.dartcontrol;
	if (offset == OSXG5DART - OSXG5U3BASE + 0x10 && size == 4)
		return machine->g5state.dartexception;
	if (offset == 0 && size == 4)
		return 0x33;
	return 0;
}

static void OsxGuestU3Write(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	if (offset - (OSXG5U3I2C - OSXG5U3BASE) < 0x1000) {
		OsxGuestI2cWrite(&machine->g5state.i2c, (offset - (OSXG5U3I2C - OSXG5U3BASE)) & 0xf0, (uint8)(value >> ((size - 1) * 8)));
		OsxGuestMpicSetSource(machine, OSXG5IRQU3I2C, (machine->g5state.i2c.isr & machine->g5state.i2c.ier) != 0);
	} else if (offset - (OSXG5HTSELF - OSXG5U3BASE) < 0x400)
		OsxGuestConfigWrite(machine, (offset - (OSXG5HTSELF - OSXG5U3BASE)) >> 2, size, value);
	else if (offset == OSXG5DART - OSXG5U3BASE && size == 4)
		machine->g5state.dartcontrol = value & ~DARTFLUSH;
	else if (offset == OSXG5DART - OSXG5U3BASE + 0x10 && size == 4)
		machine->g5state.dartexception = 0;
}

static uint8 OsxGuestChrpChecksum(const uint8 *header)
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

static int OsxGuestFlashBankValid(const uint8 *bank)
{
	return bank[0] == 0x5a && bank[1] == OsxGuestChrpChecksum(bank) && OsxGuestBusFromBytes(bank + 16, 4) == OsxGuestAdler32(bank + 20, OSXFLASHBANK - 20);
}

void OsxGuestG5PrepareNvram(OsxMachine *machine)
{
	uint8 *bank;
	bank = machine->flash;
	if (OsxGuestFlashBankValid(bank) || OsxGuestFlashBankValid(bank + OSXFLASHBANK))
		return;
	memcpy(bank, machine->nvram, OSXFLASHBANK);
	memset(bank, 0, 0x30);
	bank[0] = 0x5a;
	bank[3] = 2;
	memcpy(bank + 4, "nvram", 5);
	OsxGuestBytesFromBus(bank + 20, 4, 1);
	bank[1] = OsxGuestChrpChecksum(bank);
	bank[0x20] = 0x7f;
	bank[0x22] = (uint8)(((OSXPRAMOFFSET - 0x30) / 16) >> 8);
	bank[0x23] = (uint8)((OSXPRAMOFFSET - 0x30) / 16);
	memcpy(bank + 0x24, "wwwwwwwwwwww", 12);
	bank[0x21] = OsxGuestChrpChecksum(bank + 0x20);
	OsxGuestBytesFromBus(bank + 16, 4, OsxGuestAdler32(bank + 20, OSXFLASHBANK - 20));
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

static uint32 OsxGuestGmacRingMask(uint32 config)
{
	return (1u << (((config & 0x1e) >> 1) + 5)) - 1;
}

static uint16 OsxGuestGmacChecksum(const uint8 *data, uint32 length)
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

static void OsxGuestGmacUpdate(OsxMachine *machine)
{
	OsxGmac *gmac;
	gmac = &machine->g5state.gmac;
	OsxGuestMpicSetSource(machine, OSXG5IRQGMAC, (gmac->gregs[GMACSTAT >> 2] & ~GMACSTATTXNR & ~gmac->gregs[GMACIMASK >> 2]) != 0);
}

static void OsxGuestGmacStatus(OsxMachine *machine, uint32 bits)
{
	machine->g5state.gmac.gregs[GMACSTAT >> 2] |= bits;
	OsxGuestGmacUpdate(machine);
}


static void OsxGuestGmacResetRx(OsxGmac *gmac)
{
	gmac->rxdma[GMACRXDONE >> 2] = 0;
	gmac->rxdma[GMACRXKICK >> 2] = 0;
	gmac->rxdma[GMACRXCFG >> 2] = 0x1000010;
	gmac->rxkicked = 0;
}

static void OsxGuestGmacResetTx(OsxGmac *gmac)
{
	gmac->txdma[GMACTXDONE >> 2] = 0;
	gmac->txdma[GMACTXKICK >> 2] = 0;
	gmac->txdma[GMACTXCFG >> 2] = 0x118010;
	gmac->txsize = 0;
	gmac->txfirst = 0;
}

static void OsxGuestGmacReset(OsxMachine *machine)
{
	OsxGmac *gmac;
	uint8 address[6];
	gmac = &machine->g5state.gmac;
	OsxGuestGmacResetRx(gmac);
	OsxGuestGmacResetTx(gmac);
	gmac->gregs[GMACIMASK >> 2] = 0x0fffffff;
	gmac->gregs[GMACSTAT >> 2] = 0;
	OsxGuestBmacHardwareAddress(address);
	gmac->mac[GMACMACADDR0 >> 2] = ((uint32)address[4] << 8) | address[5];
	gmac->mac[(GMACMACADDR0 >> 2) + 1] = ((uint32)address[2] << 8) | address[3];
	gmac->mac[(GMACMACADDR0 >> 2) + 2] = ((uint32)address[0] << 8) | address[1];
	gmac->mac[GMACMACMAXFSZ >> 2] = 0x5ee;
	gmac->mif[GMACMIFCFG >> 2] = 0x100;
}

static uint16 OsxGuestGmacPhyRead(uint32 phy, uint32 reg)
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

static uint32 OsxGuestGmacPhyFrame(uint32 value)
{
	if ((value >> 30) != 1)
		return 0xffff;
	if (((value >> 28) & 3) == 1)
		return value | 0x10000;
	if (((value >> 28) & 3) == 2)
		return OsxGuestGmacPhyRead((value >> 23) & 0x1f, (value >> 18) & 0x1f) | 0x10000;
	return 0xffff | 0x10000;
}

static void OsxGuestGmacTransmit(OsxMachine *machine)
{
	OsxGmac *gmac;
	uint8 *descriptor;
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
	mask = OsxGuestGmacRingMask(gmac->txdma[GMACTXCFG >> 2]);
	base = gmac->txdma[GMACTXDBLOW >> 2];
	done = gmac->txdma[GMACTXDONE >> 2] & mask;
	kick = gmac->txdma[GMACTXKICK >> 2] & mask;
	while (done != kick) {
		descriptor = OsxGuestG5DmaHost(machine, base + done * 16, 16);
		if (descriptor == NULL)
			break;
		control = OsxGuestLoadLittle(descriptor, 4);
		if (control & GMACTXSOF) {
			gmac->txsize = 0;
			gmac->txfirst = control;
		}
		length = control & 0x7fff;
		if (gmac->txsize + length > OSXG5GMACFRAME)
			length = OSXG5GMACFRAME - gmac->txsize;
		OsxGuestG5DmaCopy(machine, OsxGuestLoadLittle(descriptor + 8, 4), gmac->txdata + gmac->txsize, length, 0);
		gmac->txsize += length;
		if ((control & GMACTXEOF) && gmac->txsize > 0) {
			start = (gmac->txfirst >> 15) & 0x3f;
			stuff = (gmac->txfirst >> 21) & 0xff;
			if ((gmac->txfirst & GMACTXCENAB) && start + 2 <= gmac->txsize && stuff + 2 <= gmac->txsize) {
				checksum = OsxGuestGmacChecksum(gmac->txdata + start, gmac->txsize - start);
				gmac->txdata[stuff] = (uint8)(checksum >> 8);
				gmac->txdata[stuff + 1] = (uint8)checksum;
			}
			if (machine->bmac.ready)
				OsxGuestHostNetSend(machine->bmac.slirpthread, gmac->txdata, (int)gmac->txsize);
			machine->bmac.busydeadline = OsxGuestCpuTimebase(machine->cpu) + OSXTIMEBASEFREQUENCY * 2;
			gmac->txsize = 0;
			gmac->txfirst = 0;
		}
		bits = GMACSTATTXDONE;
		if (OsxGuestLoadLittle(descriptor + 4, 4) & GMACTXINTME)
			bits |= GMACSTATTXINTME;
		done = (done + 1) & mask;
		gmac->txdma[GMACTXDONE >> 2] = done;
		OsxGuestGmacStatus(machine, bits);
	}
	OsxGuestGmacStatus(machine, GMACSTATTXALL);
}

static int OsxGuestGmacRxFull(OsxGmac *gmac, uint32 mask)
{
	return !gmac->rxkicked || (gmac->rxdma[GMACRXKICK >> 2] & mask) == ((gmac->rxdma[GMACRXDONE >> 2] + 1) & mask);
}

static int OsxGuestGmacAccepts(OsxGmac *gmac, const uint8 *frame)
{
	uint32 config;
	config = gmac->mac[GMACMACRXCFG >> 2];
	if ((config & 0x08) || (frame[0] & 1))
		return 1;
	return gmac->mac[GMACMACADDR0 >> 2] == (((uint32)frame[4] << 8) | frame[5]) &&
		gmac->mac[(GMACMACADDR0 >> 2) + 1] == (((uint32)frame[2] << 8) | frame[3]) &&
		gmac->mac[(GMACMACADDR0 >> 2) + 2] == (((uint32)frame[0] << 8) | frame[1]);
}

static int OsxGuestGmacDeliver(OsxMachine *machine, const uint8 *frame, uint32 length)
{
	OsxGmac *gmac;
	uint8 *descriptor;
	uint32 mask;
	uint32 done;
	uint32 config;
	uint32 fcs;
	uint32 status;
	uint32 offset;
	gmac = &machine->g5state.gmac;
	config = gmac->rxdma[GMACRXCFG >> 2];
	mask = OsxGuestGmacRingMask(config);
	if (OsxGuestGmacRxFull(gmac, mask))
		return 0;
	if (!OsxGuestGmacAccepts(gmac, frame))
		return 1;
	done = gmac->rxdma[GMACRXDONE >> 2] & mask;
	descriptor = OsxGuestG5DmaHost(machine, gmac->rxdma[GMACRXDBLOW >> 2] + done * 16, 16);
	if (descriptor == NULL)
		return 1;
	OsxGuestG5DmaCopy(machine, (OsxGuestLoadLittle(descriptor + 8, 4) & ~7u) + ((config & 0x1c00) >> 10), (uint8 *)frame, length, 1);
	fcs = 4;
	if (gmac->mac[GMACMACRXCFG >> 2] & 4)
		fcs = 0;
	offset = (config >> 13) & 0x7f;
	status = (length + fcs) << 16;
	if (offset < length)
		status |= OsxGuestGmacChecksum(frame + offset, length - offset);
	OsxGuestStoreLittle(descriptor, 4, status);
	OsxGuestStoreLittle(descriptor + 4, 4, 0);
	done = (done + 1) & mask;
	gmac->rxdma[GMACRXDONE >> 2] = done;
	status = GMACSTATRXDONE;
	if (OsxGuestGmacRxFull(gmac, mask))
		status |= GMACSTATRXNOBUF;
	OsxGuestGmacStatus(machine, status);
	return 1;
}

void OsxGuestG5GmacReceive(OsxMachine *machine)
{
	OsxGmac *gmac;
	OsxBmac *bmac;
	int length;
	gmac = &machine->g5state.gmac;
	bmac = &machine->bmac;
	while (bmac->ready && (gmac->rxdma[GMACRXCFG >> 2] & 1) && (gmac->mac[GMACMACRXCFG >> 2] & 1)) {
		if (bmac->pendinglength == 0) {
			length = OsxGuestHostNetReceive(bmac->slirpthread, bmac->pending);
			if (length <= 0)
				return;
			if (length > 1514)
				length = 1514;
			if (length < 60) {
				memset(bmac->pending + length, 0, 60 - length);
				length = 60;
			}
			bmac->pendinglength = length;
			bmac->busydeadline = OsxGuestCpuTimebase(machine->cpu) + OSXTIMEBASEFREQUENCY * 2;
		}
		if (!OsxGuestGmacDeliver(machine, bmac->pending, (uint32)bmac->pendinglength))
			return;
		bmac->pendinglength = 0;
	}
}

static uint32 OsxGuestGmacRead(OsxMachine *machine, uint32 offset)
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
			OsxGuestGmacUpdate(machine);
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

static void OsxGuestGmacWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxGmac *gmac;
	uint32 reg;
	gmac = &machine->g5state.gmac;
	if (offset < 0x2000) {
		if (offset == GMACIACK) {
			gmac->gregs[GMACSTAT >> 2] &= ~(value & GMACSTATLATCH);
			OsxGuestGmacUpdate(machine);
			return;
		}
		if (offset == GMACIMASK) {
			gmac->gregs[GMACIMASK >> 2] = value;
			OsxGuestGmacUpdate(machine);
			return;
		}
		if (offset == GMACSWRST) {
			if ((value & 3) == 3)
				OsxGuestGmacReset(machine);
			else if (value & 2)
				OsxGuestGmacResetRx(gmac);
			else if (value & 1)
				OsxGuestGmacResetTx(gmac);
			gmac->gregs[GMACSWRST >> 2] = value & ~3u;
			OsxGuestGmacUpdate(machine);
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
			OsxGuestGmacTransmit(machine);
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
			OsxGuestG5GmacReceive(machine);
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
			gmac->mif[reg >> 2] = OsxGuestGmacPhyFrame(value);
		else if (reg == GMACMIFCFG)
			gmac->mif[reg >> 2] = (value & ~0x200u) | 0x100;
		return;
	}
}

uint32 OsxGuestG5BusRead(OsxMachine *machine, uint32 address, int size)
{
	if (address - OSXG5GMACBASE < OSXG5GMACSIZE)
		return bswap_32(OsxGuestGmacRead(machine, (address - OSXG5GMACBASE) & ~3u)) >> ((4 - size) * 8);
	if (address - OSXG5MACIOBASE < OSXG5MACIOSIZE)
		return OsxGuestMacIoRead(machine, address - OSXG5MACIOBASE, size);
	if (address - OSXG5ATABASE < OSXG5ATASIZE * 2)
		return OsxGuestG5AtaRead(machine, address - OSXG5ATABASE, size);
	if (address - OSXG5HTCONFIG < OSXG5HTCONFIGSIZE)
		return OsxGuestConfigRead(machine, address - OSXG5HTCONFIG, size);
	if (address - OSXG5U3BASE < OSXG5U3SIZE)
		return OsxGuestU3Read(machine, address - OSXG5U3BASE, size);
	if (address - OSXFLASHBASE < OSXFLASHSIZE)
		return OsxGuestFlashRead(machine, address - OSXFLASHBASE, size);
	machine->unknownreads++;
	if (machine->log && machine->unknownreads < 200)
		fprintf(machine->log, "g5 bus read %08x size %d pc %08x\n", address, size, machine->cpu->pc);
	return OsxGuestAllOnes(size);
}

void OsxGuestG5BusWrite(OsxMachine *machine, uint32 address, int size, uint32 value)
{
	if (address - OSXG5GMACBASE < OSXG5GMACSIZE) {
		if (size == 4)
			OsxGuestGmacWrite(machine, address - OSXG5GMACBASE, bswap_32(value));
		return;
	}
	if (address - OSXG5MACIOBASE < OSXG5MACIOSIZE) {
		OsxGuestMacIoWrite(machine, address - OSXG5MACIOBASE, size, value);
		return;
	}
	if (address - OSXG5ATABASE < OSXG5ATASIZE * 2) {
		OsxGuestG5AtaWrite(machine, address - OSXG5ATABASE, size, value);
		return;
	}
	if (address - OSXG5HTCONFIG < OSXG5HTCONFIGSIZE) {
		OsxGuestConfigWrite(machine, address - OSXG5HTCONFIG, size, value);
		return;
	}
	if (address - OSXG5U3BASE < OSXG5U3SIZE) {
		OsxGuestU3Write(machine, address - OSXG5U3BASE, size, value);
		return;
	}
	if (address - OSXFLASHBASE < OSXFLASHSIZE) {
		OsxGuestFlashWrite(machine, address - OSXFLASHBASE, size, value);
		return;
	}
	machine->unknownwrites++;
	if (machine->log && machine->unknownwrites < 200)
		fprintf(machine->log, "g5 bus write %08x size %d value %08x pc %08x\n", address, size, value, machine->cpu->pc);
}

#endif
