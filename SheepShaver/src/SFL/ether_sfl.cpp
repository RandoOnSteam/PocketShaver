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
} SFLETHER;

static SFLETHER SflEther;

static void SflEtherReceive(void *context, const unsigned char *frame, int length)
{
	if (!ether_driver_opened)
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

void EtherInit(void)
{
	const char *mode;
	int port;
	SflEther.open = false;
	SflEther.net = NULL;
	if (PrefsFindBool("nonet"))
		return;
	mode = PrefsFindString("ether");
	if (mode == NULL)
		return;
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
		return;
	}
	SflEther.open = true;
}

void EtherExit(void)
{
	if (!SflEther.open)
		return;
	wsatomicintstore(&SflEther.stop, 1);
	SflNetClose(SflEther.net);
	SflEther.net = NULL;
	WSSignal_Destroy(&SflEther.ack);
	SflEther.open = false;
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
