/*
 *  ether_sfl.cpp - Ethernet support of the SFL platform layer
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
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "ether.h"
#include "ether_defs.h"
#include "gui.h"
#include "thread.h"
#include "sflnet.h"
#ifndef SHEEPSHAVER
#include <map>
#include "macos_util.h"
#endif

#define SFLETHERUDPTUNNEL "udptunnel"
#define SFLETHERSLIRP "slirp"
#define SFLETHERDEFAULTPORT 6066
#define SFLETHERACKMS 100

typedef struct SFLETHER
{
	void *net;
	WSSIGNAL ack;
	wsatomicint_t stop;
	uint8 address[SFLNETADDRESSSIZE];
	uint8 frame[SFLNETFRAMESIZE];
	int framelength;
	bool open;
#ifndef SHEEPSHAVER
	std::map<uint16, uint32> protocols;
#endif
} SFLETHER;

static SFLETHER SflEther;

static void SflEtherReceive(void *context, const unsigned char *frame, int length)
{
#ifdef SHEEPSHAVER
	if (!ether_driver_opened)
		return;
#endif
	if (length < 14 || length > SFLNETFRAMESIZE)
		return;
	memcpy(SflEther.frame, frame, length);
	SflEther.framelength = length;
	SetInterruptFlag(INTFLAG_ETHER);
	TriggerInterrupt();
	while (!wsatomicintload(&SflEther.stop)) {
		if (WSSignal_Wait(&SflEther.ack, SFLETHERACKMS))
			return;
	}
}

static void SflEtherMakeAddress(int port)
{
	unsigned long id;
	id = SflNetProcessId();
	SflEther.address[0] = 0x52;
	SflEther.address[1] = 0x54;
	SflEther.address[2] = (uint8)(port >> 8);
	SflEther.address[3] = (uint8)(id >> 16);
	SflEther.address[4] = (uint8)(id >> 8);
	SflEther.address[5] = (uint8)id;
}

static bool SflEtherOpen(void)
{
	const char *mode;
	int port;
	SflEther.open = false;
	SflEther.net = NULL;
#ifdef SHEEPSHAVER
	if (PrefsFindBool("nonet"))
		return false;
#endif
	mode = PrefsFindString("ether");
#ifndef SHEEPSHAVER
	if (PrefsFindBool("udptunnel"))
		mode = SFLETHERUDPTUNNEL;
#endif
	if (mode == NULL)
		return false;
	port = PrefsFindInt32("udpport");
	if (port <= 0)
		port = SFLETHERDEFAULTPORT;
	SflEtherMakeAddress(port);
	WSSignal_Create(&SflEther.ack);
	wsatomicintstore(&SflEther.stop, 0);
	if (strcmp(mode, SFLETHERUDPTUNNEL) == 0)
		SflEther.net = SflNetOpenUdp(port, SflEther.address, SflEtherReceive, NULL);
	else if (strcmp(mode, SFLETHERSLIRP) == 0)
		SflEther.net = SflNetOpenSlirp(SflEther.address, SflEtherReceive, NULL);
	if (SflEther.net == NULL) {
		WSSignal_Destroy(&SflEther.ack);
		return false;
	}
	SflEther.open = true;
	return true;
}

static void SflEtherClose(void)
{
	if (!SflEther.open)
		return;
	wsatomicintstore(&SflEther.stop, 1);
	SflNetClose(SflEther.net);
	SflEther.net = NULL;
	WSSignal_Destroy(&SflEther.ack);
	SflEther.open = false;
}

#ifdef SHEEPSHAVER
void EtherInit(void)
{
	SflEtherOpen();
}

void EtherExit(void)
{
	SflEtherClose();
}

void AO_get_ethernet_address(uint32 addr)
{
	Host2Mac_memcpy(addr, SflEther.address, SFLNETADDRESSSIZE);
}

void AO_enable_multicast(uint32 addr)
{
}

void AO_disable_multicast(uint32 addr)
{
}

void AO_transmit_packet(uint32 mp)
{
	uint8 packet[SFLNETFRAMESIZE + 2];
	int length;
	if (!SflEther.open)
		return;
	length = ether_msgb_to_buffer(mp, packet);
	if (length > SFLNETFRAMESIZE)
		return;
	if (SflNetSend(SflEther.net, packet, length))
		num_tx_packets++;
	else
		num_tx_buffer_full++;
}

void EtherIRQ(void)
{
	EthernetPacket packet;
	num_ether_irq++;
	OTEnterInterrupt();
	if (SflEther.framelength > 0) {
		Host2Mac_memcpy(packet.addr(), SflEther.frame, SflEther.framelength);
		ether_dispatch_packet(packet.addr(), SflEther.framelength);
		SflEther.framelength = 0;
	}
	OTLeaveInterrupt();
	WSSignal_Raise(&SflEther.ack);
}

void ether_reset(void)
{
}
#else
bool ether_init(void)
{
	if (!SflEtherOpen())
		return false;
	memcpy(ether_addr, SflEther.address, sizeof(ether_addr));
	return true;
}

void ether_exit(void)
{
	SflEtherClose();
}

void ether_reset(void)
{
	SflEther.protocols.clear();
}

int16 ether_add_multicast(uint32 pb)
{
	return noErr;
}

int16 ether_del_multicast(uint32 pb)
{
	return noErr;
}

int16 ether_attach_ph(uint16 type, uint32 handler)
{
	if (!SflEther.protocols.insert(std::make_pair(type, handler)).second)
		return lapProtErr;
	return noErr;
}

int16 ether_detach_ph(uint16 type)
{
	if (!SflEther.protocols.erase(type))
		return lapProtErr;
	return noErr;
}

int16 ether_write(uint32 wds)
{
	uint8 frame[SFLNETFRAMESIZE];
	int length = 0;
	for (;;) {
		uint16 size = ReadMacInt16(wds);
		if (!size)
			break;
		if (size > sizeof(frame) - length)
			return excessCollsns;
		Mac2Host_memcpy(frame + length, ReadMacInt32(wds + 2), size);
		length += size;
		wds += 6;
	}
	if (!SflEther.open || length < 14)
		return excessCollsns;
	memcpy(frame + 6, SflEther.address, SFLNETADDRESSSIZE);
	if (!SflNetSend(SflEther.net, frame, length))
		return excessCollsns;
	return noErr;
}

void EtherInterrupt(void)
{
	if (ether_data && SflEther.framelength >= 14) {
		uint16 type = (SflEther.frame[12] << 8) | SflEther.frame[13];
		uint16 protocol = type;
		if (protocol <= 1500)
			protocol = 0;
		std::map<uint16, uint32>::const_iterator handler = SflEther.protocols.find(protocol);
		if (handler != SflEther.protocols.end() && handler->second) {
			EthernetPacket packet;
			if (packet.addr()) {
				Host2Mac_memcpy(packet.addr(), SflEther.frame, SflEther.framelength);
				Host2Mac_memcpy(ether_data + ed_RHA, SflEther.frame, 14);
				M68kRegisters regs;
				memset(&regs, 0, sizeof(regs));
				regs.d[0] = type;
				regs.d[1] = SflEther.framelength - 14;
				regs.a[0] = packet.addr() + 14;
				regs.a[3] = ether_data + ed_RHA + 14;
				regs.a[4] = ether_data + ed_ReadPacket;
				Execute68k(handler->second, &regs);
			}
		}
	}
	SflEther.framelength = 0;
	WSSignal_Raise(&SflEther.ack);
}
#endif
