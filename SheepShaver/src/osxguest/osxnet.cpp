/*
 *  osxnet.cpp - OSX guest BMac ethernet emulation
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
#include "prefs.h"
#include "osxmachine.h"

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

static uint8 ReverseBits(uint8 value)
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

void OsxBmacHardwareAddress(uint8 *address)
{
	memcpy(address, BmacAddress, sizeof(BmacAddress));
}

void OsxBmacReset(OsxMachine *machine)
{
	OsxBmac *bmac;
	int index;
	bmac = &machine->bmac;
	memset(bmac->registers, 0, sizeof(bmac->registers));
	memset(bmac->srom, 0xff, sizeof(bmac->srom));
	for (index = 0; index < 3; index++)
		bmac->srom[BMACSROMMACWORD + index] = (uint16)(ReverseBits(BmacAddress[index * 2]) | (ReverseBits(BmacAddress[index * 2 + 1]) << 8));
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
		bmac->slirpthread = EtherSlirpOpen();
		bmac->attached = bmac->slirpthread != NULL;
	}
	if (machine->log)
		fprintf(machine->log, "bmac backend attached %d\n", bmac->attached);
}

static int BackendReady(OsxBmac *bmac)
{
	int state;
	if (bmac->ready || !bmac->attached)
		return bmac->ready;
	state = EtherSlirpReady(bmac->slirpthread);
	if (state > 0)
		bmac->ready = 1;
	else if (state < 0)
		bmac->attached = 0;
	return bmac->ready;
}

static void SromWrite(OsxBmac *bmac, uint16 value)
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

static void RaiseDevice(OsxMachine *machine, uint16 event)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	bmac->status |= event;
	if (bmac->registers[BMACINTDISABLE >> 4] & event)
		return;
	OsxPicSetLine(machine, OSXIRQBMAC, 1);
	OsxPicSetLine(machine, OSXIRQBMAC, 0);
}

uint32 OsxBmacRead(OsxMachine *machine, uint32 offset)
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

void OsxBmacWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	offset &= 0xff0;
	bmac->registers[offset >> 4] = (uint16)value;
	if (offset == BMACSROMCSR)
		SromWrite(bmac, (uint16)value);
	else if (offset == BMACRXCFG && (value & BMACRXENABLE))
		OsxBmacReceive(machine);
}

#if OSXNETTRACE
static void NetTrace(OsxMachine *machine, const char *dir, const uint8 *f, int n)
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
	fprintf(machine->log, "NET %s t=%llu len %d %02x:%02x:%02x:%02x:%02x:%02x<-%02x:%02x:%02x:%02x:%02x:%02x type %04x", dir, (unsigned long long)OsxCpuTimebase(machine->cpu), n, f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11], type);
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

void OsxBmacTransmit(OsxMachine *machine)
{
	OsxBmac *bmac;
	int length;
	bmac = &machine->bmac;
	for (;;) {
		length = OsxDbdmaOutputFrame(machine, OSXDBDMABMACTX, bmac->frame, sizeof(bmac->frame));
		if (length < 0)
			return;
		if (BackendReady(bmac) && (bmac->registers[BMACTXCFG >> 4] & BMACTXENABLE) && length >= 14) {
#if OSXNETTRACE
			NetTrace(machine, "tx", bmac->frame, length);
#endif
			EtherSlirpSend(bmac->frame, length);
			bmac->busydeadline = OsxCpuTimebase(machine->cpu) + BMACBUSYPERIOD;
		}
		RaiseDevice(machine, BMACFRAMESENT);
	}
}

void OsxBmacReceive(OsxMachine *machine)
{
	OsxBmac *bmac;
	int length;
	bmac = &machine->bmac;
	while (BackendReady(bmac) && (bmac->registers[BMACRXCFG >> 4] & BMACRXENABLE)) {
		if (bmac->pendinglength == 0) {
			length = EtherSlirpReceive(bmac->pending);
			if (length <= 0)
				return;
#if OSXNETTRACE
			NetTrace(machine, "rx", bmac->pending, length);
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
			bmac->busydeadline = OsxCpuTimebase(machine->cpu) + BMACBUSYPERIOD;
		}
		if (!OsxDbdmaInputFrame(machine, OSXDBDMABMACRX, bmac->pending, (uint32)bmac->pendinglength))
			return;
		bmac->pendinglength = 0;
#if OSXNETTRACE
		if (machine->log)
			fprintf(machine->log, "NET rxdone t=%llu\n", (unsigned long long)OsxCpuTimebase(machine->cpu));
#endif
		bmac->status |= BMACFRAMERECEIVED;
	}
}

void OsxBmacPoll(OsxMachine *machine, uint64 now)
{
	OsxBmac *bmac;
	bmac = &machine->bmac;
	if (!bmac->attached || now < bmac->polldeadline)
		return;
	if (!BackendReady(bmac)) {
		bmac->polldeadline = now + BMACIDLEPOLL;
		return;
	}
	if (now < bmac->busydeadline)
		bmac->polldeadline = now + BMACBUSYPOLL;
	else
		bmac->polldeadline = now + BMACIDLEPOLL;
	EtherSlirpPoll();
	OsxBmacReceive(machine);
}

uint64 OsxBmacNextDeadline(OsxMachine *machine)
{
	if (!machine->bmac.attached)
		return OSXNEVER;
	return machine->bmac.polldeadline;
}
