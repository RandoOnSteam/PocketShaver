/*
 *  osxcuda.cpp - OSX CUDA/ADB emulation
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
#include <time.h>
#include "osxmachine.h"

#define VIAB 0
#define VIAA 1
#define VIADIRB 2
#define VIADIRA 3
#define VIAT1CL 4
#define VIAT1CH 5
#define VIAT1LL 6
#define VIAT1LH 7
#define VIAT2CL 8
#define VIAT2CH 9
#define VIASR 10
#define VIAACR 11
#define VIAPCR 12
#define VIAIFR 13
#define VIAIER 14
#define VIAANH 15

#define VIAIRQSR 0x04
#define VIAIRQT2 0x20
#define VIAIRQT1 0x40

#define VIAFREQUENCY 783360

#define CUDATREQ 0x08
#define CUDATACK 0x10
#define CUDATIP 0x20
#define CUDASROUT 0x10

#define CUDAPACKETADB 0
#define CUDAPACKETCUDA 1
#define CUDAPACKETERROR 2

#define CUDAAUTOPOLL 0x01
#define OSXDETERMINISTICEPOCH 1760000000
#define CUDAGETTIME 0x03
#define CUDAGETPRAM 0x07
#define CUDASETTIME 0x09
#define CUDAPOWERDOWN 0x0a
#define CUDASETPRAM 0x0c
#define CUDARESETSYSTEM 0x11
#define CUDAFILESERVERFLAG 0x13
#define CUDASETAUTORATE 0x14
#define CUDAGETAUTORATE 0x16
#define CUDASETDEVICELIST 0x19
#define CUDAGETDEVICELIST 0x1a
#define CUDASETONESECOND 0x1b
#define CUDASETPOWERMESSAGES 0x21
#define CUDAGETSETIIC 0x22
#define CUDAENABLEWAKEUP 0x23
#define CUDATIMERTICKLE 0x24
#define CUDACOMBINEDIIC 0x25

#define ADBKEYBOARDADDRESS 2
#define ADBMOUSEADDRESS 3
#define MACEPOCHOFFSET 2082844800u

static uint64 MicrosecondsToTicks(uint32 microseconds)
{
	return (uint64)microseconds * OSXTIMEBASEFREQUENCY / 1000000;
}

static uint32 ViaTicks(uint64 timebase)
{
	return (uint32)(timebase * VIAFREQUENCY / OSXTIMEBASEFREQUENCY);
}

static void UpdateIrq(OsxMachine *machine)
{
	OsxCuda *cuda;
	int line;
	cuda = &machine->cuda;
	line = (cuda->ifr & cuda->ier & 0x7f) != 0;
	if (line != cuda->irqline) {
		cuda->irqline = line;
		OsxPicSetLine(machine, OSXIRQCUDA, line);
	}
}

static void DelaySrInterrupt(OsxMachine *machine)
{
	machine->cuda.srdeadline = OsxCpuTimebase(machine->cpu) + MicrosecondsToTicks(30);
}

static void ResetAdbDevice(OsxAdbDevice *device)
{
	device->address = device->defaultaddress;
	device->handler = device->defaulthandler;
}

void OsxCudaReset(OsxMachine *machine)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	cuda->b = CUDATREQ | CUDATACK | CUDATIP;
	cuda->lastb = cuda->b;
	cuda->a = 0;
	cuda->dirb = 0;
	cuda->dira = 0;
	cuda->sr = 0;
	cuda->acr = 0;
	cuda->pcr = 0;
	cuda->ifr = 0;
	cuda->ier = 0;
	cuda->t1latch = 0xffff;
	cuda->t2latch = 0xffff;
	cuda->t1start = 0;
	cuda->t2start = 0;
	cuda->t1deadline = 0;
	cuda->t2deadline = 0;
	cuda->srdeadline = 0;
	cuda->polldeadline = 0;
	cuda->outlength = 0;
	cuda->inlength = 0;
	cuda->inposition = 0;
	cuda->autopoll = 0;
	cuda->pollrate = 11;
	cuda->pollmask = 0xffff;
	cuda->irqline = 0;
	cuda->timeoffset = 0;
	cuda->keyboard.defaultaddress = ADBKEYBOARDADDRESS;
	cuda->keyboard.defaulthandler = 1;
	cuda->mouse.defaultaddress = ADBMOUSEADDRESS;
	cuda->mouse.defaulthandler = 2;
	ResetAdbDevice(&cuda->keyboard);
	ResetAdbDevice(&cuda->mouse);
	cuda->keyread = 0;
	cuda->keywrite = 0;
	cuda->mousedx = 0;
	cuda->mousedy = 0;
	cuda->mousebutton = 0;
	cuda->mousereported = 0;
	cuda->mousepending = 0;
	cuda->mouseabsolute = 0;
	cuda->mousex = 0;
	cuda->mousey = 0;
	cuda->mouseread = 0;
	cuda->mousewrite = 0;
	cuda->pointerread = 0;
	cuda->pointerwrite = 0;
#if OSXG5
	cuda->pmu = machine->g5;
	cuda->pmustate = 0;
	cuda->pmuintbits = 0;
	cuda->pmuintmask = 0x90;
	cuda->adbreplysize = 0;
	cuda->tickdeadline = 0;
	if (cuda->pmu) {
		cuda->b = 0x18;
		cuda->lastb = cuda->b;
		cuda->dirb = 0xff;
	}
#endif
}

static void SendToHost(OsxMachine *machine, const uint8 *data, int length)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	memcpy(cuda->inbuffer, data, length);
	cuda->inlength = length;
	cuda->inposition = 0;
	if (cuda->b & CUDATIP)
		cuda->b &= ~CUDATREQ;
	DelaySrInterrupt(machine);
}

static int KeyboardHasData(OsxCuda *cuda)
{
	return cuda->keyread != cuda->keywrite;
}

static int MouseHasData(OsxCuda *cuda)
{
	if (cuda->mouseabsolute)
		return cuda->mouseread != cuda->mousewrite && (cuda->pointerwrite + 1) % OSXMOUSEQUEUE != cuda->pointerread;
	return cuda->mousedx != 0 || cuda->mousedy != 0 || cuda->mousepending != 0;
}

static int TalkKeyboard(OsxCuda *cuda, int reg, uint8 *output)
{
	if (reg == 0) {
		if (!KeyboardHasData(cuda))
			return 0;
		output[0] = cuda->keyqueue[cuda->keyread];
		cuda->keyread = (cuda->keyread + 1) % OSXADBQUEUE;
		output[1] = 0xff;
		if (KeyboardHasData(cuda)) {
			output[1] = cuda->keyqueue[cuda->keyread];
			cuda->keyread = (cuda->keyread + 1) % OSXADBQUEUE;
		}
		return 2;
	}
	if (reg == 2) {
		output[0] = 0xff;
		output[1] = 0xff;
		return 2;
	}
	if (reg == 3) {
		output[0] = (uint8)(0x60 | cuda->keyboard.address);
		output[1] = cuda->keyboard.handler;
		return 2;
	}
	return 0;
}

static int ClampDelta(int delta)
{
	if (delta > 63)
		return 63;
	if (delta < -64)
		return -64;
	return delta;
}

static int TalkMouse(OsxCuda *cuda, int reg, uint8 *output)
{
	int dx;
	int dy;
	OsxMouseEvent *event;
	if (reg == 0) {
		if (!MouseHasData(cuda))
			return 0;
		if (cuda->mouseabsolute) {
			event = &cuda->mousequeue[cuda->mouseread];
			cuda->pointerqueue[cuda->pointerwrite] = *event;
			cuda->pointerwrite = (cuda->pointerwrite + 1) % OSXMOUSEQUEUE;
			cuda->mouseread = (cuda->mouseread + 1) % OSXMOUSEQUEUE;
			output[0] = 0;
			if (!(event->button & 1))
				output[0] = 0x80;
			output[1] = 0x81;
			cuda->mousereported = event->button & 1;
			return 2;
		}
		dx = ClampDelta(cuda->mousedx);
		dy = ClampDelta(cuda->mousedy);
		cuda->mousedx -= dx;
		cuda->mousedy -= dy;
		if (cuda->mousepending) {
			cuda->mousereported = !cuda->mousereported;
			cuda->mousepending--;
		}
		output[0] = (uint8)(dy & 0x7f);
		if (!cuda->mousereported)
			output[0] |= 0x80;
		output[1] = (uint8)(0x80 | (dx & 0x7f));
		return 2;
	}
	if (reg == 3) {
		output[0] = (uint8)(0x60 | cuda->mouse.address);
		output[1] = cuda->mouse.handler;
		return 2;
	}
	return 0;
}

static void ListenRegister3(OsxAdbDevice *device, const uint8 *data, int length, int keyboard)
{
	if (length < 3)
		return;
	if (data[2] == 0x00 || data[2] == 0xfd || data[2] == 0xff) {
		device->address = data[1] & 0x0f;
		return;
	}
	if (data[2] == 0xfe) {
		device->address = data[1] & 0x0f;
		return;
	}
	device->address = data[1] & 0x0f;
	if (keyboard && (data[2] == 1 || data[2] == 2 || data[2] == 3))
		device->handler = data[2];
	if (!keyboard && (data[2] == 1 || data[2] == 2))
		device->handler = data[2];
}

static int AdbRequest(OsxMachine *machine, const uint8 *data, int length, uint8 *output)
{
	OsxCuda *cuda;
	int command;
	int address;
	int reg;
	cuda = &machine->cuda;
	command = data[0] & 0x0f;
	address = data[0] >> 4;
	if (command == 0) {
		ResetAdbDevice(&cuda->keyboard);
		ResetAdbDevice(&cuda->mouse);
		return 0;
	}
	reg = command & 3;
	if (address == cuda->keyboard.address) {
		if (command == 1) {
			cuda->keyread = cuda->keywrite;
			return 0;
		}
		if ((command & 0x0c) == 0x08) {
			if (reg == 3)
				ListenRegister3(&cuda->keyboard, data, length, 1);
			return 0;
		}
		if ((command & 0x0c) == 0x0c)
			return TalkKeyboard(cuda, reg, output);
		return 0;
	}
	if (address == cuda->mouse.address) {
		if (command == 1)
			return 0;
		if ((command & 0x0c) == 0x08) {
			if (reg == 3)
				ListenRegister3(&cuda->mouse, data, length, 0);
			return 0;
		}
		if ((command & 0x0c) == 0x0c)
			return TalkMouse(cuda, reg, output);
		return 0;
	}
	return -1;
}

static int64 HostSeconds(OsxMachine *machine)
{
	if (machine->cpu->deterministic)
		return OSXDETERMINISTICEPOCH + (int64)(OsxCpuTimebase(machine->cpu) / OSXTIMEBASEFREQUENCY);
	return (int64)time(NULL);
}

static void ReceiveCudaCommand(OsxMachine *machine, const uint8 *data, int length)
{
	OsxCuda *cuda;
	uint8 reply[OSXCUDABUFFER];
	uint32 address;
	uint32 now;
	int replylength;
	int index;
	cuda = &machine->cuda;
	reply[0] = CUDAPACKETCUDA;
	reply[1] = 0;
	reply[2] = data[0];
	replylength = 3;
	switch (data[0]) {
	case CUDAAUTOPOLL:
		if (length > 1)
			cuda->autopoll = data[1] != 0;
		break;
	case CUDASETAUTORATE:
		if (length > 1 && data[1] != 0)
			cuda->pollrate = data[1];
		break;
	case CUDAGETAUTORATE:
		reply[replylength++] = (uint8)cuda->pollrate;
		break;
	case CUDASETDEVICELIST:
		if (length > 2)
			cuda->pollmask = (uint16)((data[1] << 8) | data[2]);
		break;
	case CUDAGETDEVICELIST:
		reply[replylength++] = (uint8)(cuda->pollmask >> 8);
		reply[replylength++] = (uint8)cuda->pollmask;
		break;
	case CUDAGETTIME:
		now = (uint32)(HostSeconds(machine) + MACEPOCHOFFSET + cuda->timeoffset);
		reply[replylength++] = (uint8)(now >> 24);
		reply[replylength++] = (uint8)(now >> 16);
		reply[replylength++] = (uint8)(now >> 8);
		reply[replylength++] = (uint8)now;
		break;
	case CUDASETTIME:
		if (length > 4) {
			now = ((uint32)data[1] << 24) | ((uint32)data[2] << 16) | ((uint32)data[3] << 8) | data[4];
			cuda->timeoffset = (int64)now - (HostSeconds(machine) + MACEPOCHOFFSET);
		}
		break;
	case CUDAGETPRAM:
		if (length > 2) {
			address = ((uint32)data[1] << 8) | data[2];
			for (index = 0; index < 1 && address + index < OSXPRAMSIZE; index++)
				reply[replylength++] = machine->nvram[OSXPRAMOFFSET + address + index];
		}
		break;
	case CUDASETPRAM:
		if (length > 2) {
			address = ((uint32)data[1] << 8) | data[2];
			for (index = 3; index < length && address + index - 3 < OSXPRAMSIZE; index++)
				machine->nvram[OSXPRAMOFFSET + address + index - 3] = data[index];
		}
		break;
	case CUDAPOWERDOWN:
		machine->quit = 1;
		break;
	case CUDARESETSYSTEM:
		machine->quit = 2;
		break;
	case CUDAFILESERVERFLAG:
	case CUDASETONESECOND:
	case CUDASETPOWERMESSAGES:
	case CUDAENABLEWAKEUP:
	case CUDATIMERTICKLE:
		break;
	default:
		if (machine->log)
			fprintf(machine->log, "cuda unknown command %02x length %d\n", data[0], length);
		reply[0] = CUDAPACKETERROR;
		reply[1] = 0x02;
		reply[2] = CUDAPACKETCUDA;
		reply[3] = data[0];
		replylength = 4;
		break;
	}
	SendToHost(machine, reply, replylength);
}

static void ReceivePacket(OsxMachine *machine, const uint8 *data, int length)
{
	uint8 reply[OSXCUDABUFFER];
	int result;
	if (length < 1)
		return;
	if (data[0] == CUDAPACKETADB && length >= 2) {
		result = AdbRequest(machine, data + 1, length - 1, reply + 3);
		reply[0] = CUDAPACKETADB;
		reply[2] = data[1];
		if (result < 0) {
			reply[1] = 0x02;
			SendToHost(machine, reply, 3);
			return;
		}
		reply[1] = 0x00;
		SendToHost(machine, reply, 3 + result);
		return;
	}
	if (data[0] == CUDAPACKETCUDA && length >= 2) {
		ReceiveCudaCommand(machine, data + 1, length - 1);
		return;
	}
	reply[0] = CUDAPACKETERROR;
	reply[1] = 0x01;
	reply[2] = data[0];
	SendToHost(machine, reply, 3);
}

static void CudaUpdate(OsxMachine *machine)
{
	OsxCuda *cuda;
	int received;
	int length;
	cuda = &machine->cuda;
	received = 0;
	if (!(cuda->b & CUDATIP)) {
		if (cuda->acr & CUDASROUT) {
			if ((cuda->b & (CUDATACK | CUDATIP)) != (cuda->lastb & (CUDATACK | CUDATIP))) {
				if (cuda->outlength < OSXCUDABUFFER)
					cuda->outbuffer[cuda->outlength++] = cuda->sr;
				DelaySrInterrupt(machine);
			}
		} else if (cuda->inposition < cuda->inlength) {
			if ((cuda->b & (CUDATACK | CUDATIP)) != (cuda->lastb & (CUDATACK | CUDATIP))) {
				cuda->sr = cuda->inbuffer[cuda->inposition++];
				if (cuda->inposition >= cuda->inlength)
					cuda->b |= CUDATREQ;
				DelaySrInterrupt(machine);
			}
		}
	} else {
		if ((cuda->lastb & CUDATIP) && (cuda->b & CUDATACK) != (cuda->lastb & CUDATACK)) {
			if (cuda->b & CUDATACK)
				cuda->b |= CUDATREQ;
			else
				cuda->b &= ~CUDATREQ;
			DelaySrInterrupt(machine);
		} else {
			if (!(cuda->lastb & CUDATIP)) {
				received = cuda->outlength > 0;
				DelaySrInterrupt(machine);
			}
			if (cuda->inposition < cuda->inlength)
				cuda->b &= ~CUDATREQ;
		}
	}
	cuda->lastb = cuda->b;
	if (received) {
		length = cuda->outlength;
		cuda->outlength = 0;
		ReceivePacket(machine, cuda->outbuffer, length);
	}
}

static uint32 TimerCounter(OsxCuda *cuda, uint64 start, uint16 latch, int continuous, uint64 now)
{
	uint32 elapsed;
	uint32 period;
	elapsed = ViaTicks(now - start);
	if (continuous) {
		period = (uint32)latch + 2;
		return (uint32)(latch - (elapsed % period)) & 0xffff;
	}
	return (uint32)(latch - elapsed) & 0xffff;
}

static uint64 TimerDeadline(uint64 start, uint32 ticks)
{
	return start + ((uint64)ticks + 1) * OSXTIMEBASEFREQUENCY / VIAFREQUENCY;
}

#if OSXG5
#define PMUTACK 0x08
#define PMUTREQ 0x10
#define PMUSTATEIDLE 0
#define PMUSTATECOMMAND 1
#define PMUSTATERESPONSE 2
#define PMUINTADB 0x10
#define PMUINTADBAUTO 0x04
#define PMUINTTICK 0x80
#define PMUSETRTC 0x30
#define PMUWRITEXPRAM 0x32
#define PMUREADRTC 0x38
#define PMUREADXPRAM 0x3a
#define PMUADBCOMMAND 0x20
#define PMUADBPOLLOFF 0x21
#define PMUSETINTRMASK 0x70
#define PMUINTACK 0x78
#define PMUSHUTDOWN 0x7e
#define PMUPOWEREVENTS 0x8f
#define PMURESET 0xd0
#define PMUGETCOVER 0xdc
#define PMUSYSTEMREADY 0xdf
#define PMUDOWNLOADSTATUS 0xe2
#define PMUREADPMURAM 0xe8
#define PMUGETVERSION 0xea

static const signed char pmulengths[256][2] = {
	{ -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 1, 0 }, { 1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 1 }, { 0, 1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { 0, 0 },
	{ -1, 0 }, { 0, 0 }, { 2, 0 }, { 1, 0 }, { 1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, -1 }, { 0, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { 0, -1 },
	{ 4, 0 }, { 20, 0 }, { -1, 0 }, { 3, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 4 }, { 0, 20 }, { 2, -1 }, { 2, 1 }, { 3, -1 }, { -1, -1 }, { -1, -1 }, { 4, 0 },
	{ 1, 0 }, { 1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 1 }, { 0, 1 }, { -1, -1 }, { 1, 0 }, { 1, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 1, 0 }, { 0, 0 }, { 2, 0 }, { 2, 0 }, { -1, 0 }, { 1, 0 }, { 3, 0 }, { 1, 0 },
	{ 0, 1 }, { 1, 0 }, { 0, 2 }, { 0, 2 }, { 0, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 2, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 3 }, { 0, 3 }, { 0, 2 }, { 0, 8 }, { 0, -1 }, { 0, -1 }, { -1, -1 }, { -1, -1 },
	{ 1, 0 }, { 1, 0 }, { 1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, -1 }, { 0, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { 5, 1 }, { 4, 1 }, { 4, 1 },
	{ 4, 0 }, { -1, 0 }, { 0, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 5 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 1, 0 }, { 2, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 0, 1 }, { 0, 1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 2, 0 }, { 2, 0 }, { 2, 0 }, { 4, 0 }, { -1, 0 }, { 0, 0 }, { -1, 0 }, { -1, 0 },
	{ 1, 1 }, { 1, 0 }, { 3, 0 }, { 2, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
	{ 0, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ 1, 1 }, { 1, 1 }, { -1, -1 }, { -1, -1 }, { 0, 1 }, { 0, -1 }, { -1, -1 }, { -1, -1 },
	{ -1, 0 }, { 4, 0 }, { 0, 1 }, { -1, 0 }, { -1, 0 }, { 4, 0 }, { -1, 0 }, { -1, 0 },
	{ 3, -1 }, { -1, -1 }, { 0, 1 }, { -1, -1 }, { 0, -1 }, { -1, -1 }, { -1, -1 }, { 0, 0 },
	{ -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 },
	{ -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }
};

static void PmuUpdateRequest(OsxMachine *machine)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	OsxG5SetPmuRequest(machine, (cuda->pmuintbits & cuda->pmuintmask) != 0);
}

static void PmuRaiseSr(OsxMachine *machine)
{
	machine->cuda.ifr |= VIAIRQSR;
	UpdateIrq(machine);
}

static void PmuAdbCommand(OsxMachine *machine, const uint8 *data, int length)
{
	OsxCuda *cuda;
	uint8 request[OSXCUDABUFFER];
	int result;
	cuda = &machine->cuda;
	if (length < 2)
		return;
	if (data[0] == 0 && data[1] == 0x86) {
		if (length == 4) {
			cuda->pollmask = (uint16)((data[2] << 8) | data[3]);
			cuda->autopoll = cuda->pollmask != 0;
		}
		return;
	}
	result = 0;
	if (length >= 3 && data[2] <= length - 3) {
		request[0] = data[0];
		memcpy(request + 1, data + 3, length - 3);
		result = AdbRequest(machine, request, length - 2, cuda->adbreply + 1);
	}
	if (result < 0)
		result = 0;
	cuda->adbreply[0] = data[0];
	cuda->adbreplysize = result + 1;
	cuda->pmuintbits |= PMUINTADB;
	PmuUpdateRequest(machine);
}

static int PmuDispatch(OsxMachine *machine)
{
	OsxCuda *cuda;
	uint8 *in;
	uint8 *out;
	uint32 now;
	int length;
	int index;
	cuda = &machine->cuda;
	in = cuda->pmucommandbuffer;
	out = cuda->pmuresponse;
	length = cuda->pmucommandposition;
	if (length > OSXCUDABUFFER)
		length = OSXCUDABUFFER;
	cuda->pmuresponsesize = 0;
	switch (cuda->pmucommand) {
	case PMUINTACK:
		if (cuda->pmuintbits & PMUINTADB) {
			memcpy(out + 1, cuda->adbreply, cuda->adbreplysize);
			out[0] = cuda->pmuintbits & (PMUINTADB | PMUINTADBAUTO);
			cuda->pmuresponsesize = cuda->adbreplysize + 1;
			cuda->pmuintbits &= ~(PMUINTADB | PMUINTADBAUTO);
			cuda->adbreplysize = 0;
		} else {
			out[0] = cuda->pmuintbits;
			cuda->pmuintbits = 0;
			cuda->pmuresponsesize = 1;
		}
		PmuUpdateRequest(machine);
		return 1;
	case PMUSETINTRMASK:
		if (length == 1)
			cuda->pmuintmask = in[0];
		PmuUpdateRequest(machine);
		return 1;
	case PMUADBCOMMAND:
		PmuAdbCommand(machine, in, length);
		return 1;
	case PMUADBPOLLOFF:
		cuda->autopoll = 0;
		return 1;
	case PMURESET:
		machine->quit = 2;
		return 1;
	case PMUSHUTDOWN:
		out[0] = 0;
		cuda->pmuresponsesize = 1;
		if (length == 4 && memcmp(in, "MATT", 4) == 0)
			machine->quit = 1;
		return 1;
	case PMUREADRTC:
		now = (uint32)(HostSeconds(machine) + MACEPOCHOFFSET + cuda->timeoffset);
		out[0] = (uint8)(now >> 24);
		out[1] = (uint8)(now >> 16);
		out[2] = (uint8)(now >> 8);
		out[3] = (uint8)now;
		cuda->pmuresponsesize = 4;
		return 1;
	case PMUSETRTC:
		if (length == 4) {
			now = ((uint32)in[0] << 24) | ((uint32)in[1] << 16) | ((uint32)in[2] << 8) | in[3];
			cuda->timeoffset = (int64)now - (HostSeconds(machine) + MACEPOCHOFFSET);
		}
		return 1;
	case PMUREADXPRAM:
		for (index = 0; length == 2 && index < in[1] && index < OSXCUDABUFFER && in[0] + index < OSXPRAMSIZE; index++)
			out[index] = machine->nvram[OSXPRAMOFFSET + in[0] + index];
		cuda->pmuresponsesize = index;
		return 1;
	case PMUWRITEXPRAM:
		for (index = 0; length >= 2 && index < in[1] && index + 2 < length && in[0] + index < OSXPRAMSIZE; index++)
			machine->nvram[OSXPRAMOFFSET + in[0] + index] = in[index + 2];
		return 1;
	case PMUSYSTEMREADY:
		return 1;
	case PMUGETVERSION:
		out[0] = 1;
		cuda->pmuresponsesize = 1;
		return 1;
	case PMUPOWEREVENTS:
		if (length >= 1 && (in[0] == 0 || in[0] == 3)) {
			out[0] = 0;
			out[1] = 0;
			cuda->pmuresponsesize = 2;
		}
		return 1;
	case PMUGETCOVER:
		out[0] = 0;
		cuda->pmuresponsesize = 1;
		return 1;
	case PMUDOWNLOADSTATUS:
		out[0] = 0x62;
		cuda->pmuresponsesize = 1;
		return 1;
	case PMUREADPMURAM:
		out[0] = 0xd0;
		out[1] = 0x7f;
		out[2] = 0x0c;
		cuda->pmuresponsesize = 3;
		return 1;
	}
	return 0;
}

static void PmuExecute(OsxMachine *machine)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	if (PmuDispatch(machine))
		return;
	if (machine->log)
		fprintf(machine->log, "pmu unknown command %02x length %d\n", cuda->pmucommand, cuda->pmucommandposition);
	if (cuda->pmuresponselength > 0) {
		cuda->pmuresponsesize = cuda->pmuresponselength;
		memset(cuda->pmuresponse, 0, cuda->pmuresponselength);
	}
}

static void PmuUpdate(OsxMachine *machine)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	if (cuda->b == cuda->lastb)
		return;
	cuda->lastb = cuda->b;
	if ((cuda->b & (PMUTREQ | PMUTACK)) == PMUTREQ) {
		cuda->b |= PMUTACK;
		cuda->lastb = cuda->b;
		return;
	}
	if ((cuda->b & (PMUTREQ | PMUTACK)) != PMUTACK)
		return;
	cuda->b &= ~PMUTACK;
	cuda->lastb = cuda->b;
	if (cuda->pmustate == PMUSTATEIDLE) {
		if (!(cuda->acr & CUDASROUT))
			return;
		cuda->pmucommand = cuda->sr;
		PmuRaiseSr(machine);
		cuda->pmucommandlength = pmulengths[cuda->pmucommand][0];
		cuda->pmuresponselength = pmulengths[cuda->pmucommand][1];
		cuda->pmucommandposition = 0;
		cuda->pmuresponseposition = 0;
		cuda->pmustate = PMUSTATECOMMAND;
	} else if (cuda->pmustate == PMUSTATECOMMAND) {
		if (!(cuda->acr & CUDASROUT))
			return;
		if (cuda->pmucommandlength == -1)
			cuda->pmucommandlength = cuda->sr;
		else if (cuda->pmucommandposition++ < OSXCUDABUFFER)
			cuda->pmucommandbuffer[cuda->pmucommandposition - 1] = cuda->sr;
		PmuRaiseSr(machine);
	} else {
		if (cuda->acr & CUDASROUT)
			return;
		if (cuda->pmuresponselength == -1) {
			cuda->sr = (uint8)cuda->pmuresponsesize;
			cuda->pmuresponselength = cuda->pmuresponsesize;
		} else if (cuda->pmuresponseposition < cuda->pmuresponsesize) {
			cuda->sr = cuda->pmuresponse[cuda->pmuresponseposition++];
		}
		PmuRaiseSr(machine);
	}
	if (cuda->pmustate == PMUSTATECOMMAND && cuda->pmucommandlength == cuda->pmucommandposition) {
		PmuExecute(machine);
		cuda->pmustate = PMUSTATERESPONSE;
	}
	if (cuda->pmustate == PMUSTATERESPONSE && cuda->pmuresponselength == cuda->pmuresponseposition)
		cuda->pmustate = PMUSTATEIDLE;
}

static void PmuPoll(OsxMachine *machine, uint64 now)
{
	OsxCuda *cuda;
	int result;
	uint8 request;
	cuda = &machine->cuda;
	if (now >= cuda->tickdeadline) {
		cuda->tickdeadline = now + OSXTIMEBASEFREQUENCY;
		cuda->pmuintbits |= PMUINTTICK;
		PmuUpdateRequest(machine);
	}
	if (!cuda->autopoll || (cuda->pmuintbits & PMUINTADB) || cuda->pmustate != PMUSTATEIDLE)
		return;
	result = 0;
	request = 0;
	if (KeyboardHasData(cuda) && (cuda->pollmask & (1u << cuda->keyboard.address))) {
		request = (uint8)((cuda->keyboard.address << 4) | 0x0c);
		result = TalkKeyboard(cuda, 0, cuda->adbreply + 1);
	} else if (MouseHasData(cuda) && (cuda->pollmask & (1u << cuda->mouse.address))) {
		request = (uint8)((cuda->mouse.address << 4) | 0x0c);
		result = TalkMouse(cuda, 0, cuda->adbreply + 1);
	}
	if (result <= 0)
		return;
	cuda->adbreply[0] = request;
	cuda->adbreplysize = result + 1;
	cuda->pmuintbits |= PMUINTADB | PMUINTADBAUTO;
	PmuUpdateRequest(machine);
}
#endif

uint32 OsxCudaRead(OsxMachine *machine, uint32 reg)
{
	OsxCuda *cuda;
	uint32 value;
	uint64 now;
	cuda = &machine->cuda;
	switch (reg) {
	case VIAB:
		return cuda->b;
	case VIAA:
	case VIAANH:
		return cuda->a;
	case VIADIRB:
		return cuda->dirb;
	case VIADIRA:
		return cuda->dira;
	case VIAT1CL:
		now = OsxCpuTimebase(machine->cpu);
		cuda->ifr &= ~VIAIRQT1;
		UpdateIrq(machine);
		return TimerCounter(cuda, cuda->t1start, cuda->t1latch, (cuda->acr & 0x40) != 0, now) & 0xff;
	case VIAT1CH:
		now = OsxCpuTimebase(machine->cpu);
		return TimerCounter(cuda, cuda->t1start, cuda->t1latch, (cuda->acr & 0x40) != 0, now) >> 8;
	case VIAT1LL:
		return cuda->t1latch & 0xff;
	case VIAT1LH:
		return cuda->t1latch >> 8;
	case VIAT2CL:
		now = OsxCpuTimebase(machine->cpu);
		cuda->ifr &= ~VIAIRQT2;
		UpdateIrq(machine);
		return TimerCounter(cuda, cuda->t2start, cuda->t2latch, 0, now) & 0xff;
	case VIAT2CH:
		now = OsxCpuTimebase(machine->cpu);
		return TimerCounter(cuda, cuda->t2start, cuda->t2latch, 0, now) >> 8;
	case VIASR:
		value = cuda->sr;
		cuda->ifr &= ~VIAIRQSR;
		UpdateIrq(machine);
		return value;
	case VIAACR:
		return cuda->acr;
	case VIAPCR:
		return cuda->pcr;
	case VIAIFR:
		value = cuda->ifr;
		if (cuda->ifr & cuda->ier & 0x7f)
			value |= 0x80;
		return value;
	case VIAIER:
		return cuda->ier | 0x80;
	}
	return 0;
}

void OsxCudaWrite(OsxMachine *machine, uint32 reg, uint32 value)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	switch (reg) {
	case VIAB:
		cuda->b = (uint8)((cuda->b & ~cuda->dirb) | (value & cuda->dirb));
#if OSXG5
		if (cuda->pmu) {
			PmuUpdate(machine);
			break;
		}
#endif
		CudaUpdate(machine);
		break;
	case VIAA:
	case VIAANH:
		cuda->a = (uint8)value;
		break;
	case VIADIRB:
		cuda->dirb = (uint8)value;
		break;
	case VIADIRA:
		cuda->dira = (uint8)value;
		break;
	case VIAT1CL:
	case VIAT1LL:
		cuda->t1latch = (uint16)((cuda->t1latch & 0xff00) | value);
		break;
	case VIAT1CH:
		cuda->t1latch = (uint16)((cuda->t1latch & 0x00ff) | (value << 8));
		cuda->t1start = OsxCpuTimebase(machine->cpu);
		cuda->t1deadline = TimerDeadline(cuda->t1start, cuda->t1latch);
		cuda->ifr &= ~VIAIRQT1;
		UpdateIrq(machine);
		break;
	case VIAT1LH:
		cuda->t1latch = (uint16)((cuda->t1latch & 0x00ff) | (value << 8));
		cuda->ifr &= ~VIAIRQT1;
		UpdateIrq(machine);
		break;
	case VIAT2CL:
		cuda->t2latch = (uint16)((cuda->t2latch & 0xff00) | value);
		break;
	case VIAT2CH:
		cuda->t2latch = (uint16)((cuda->t2latch & 0x00ff) | (value << 8));
		cuda->t2start = OsxCpuTimebase(machine->cpu);
		cuda->t2deadline = TimerDeadline(cuda->t2start, cuda->t2latch);
		cuda->ifr &= ~VIAIRQT2;
		UpdateIrq(machine);
		break;
	case VIASR:
		cuda->sr = (uint8)value;
		cuda->ifr &= ~VIAIRQSR;
		UpdateIrq(machine);
		break;
	case VIAACR:
		cuda->acr = (uint8)value;
#if OSXG5
		if (cuda->pmu)
			break;
#endif
		CudaUpdate(machine);
		break;
	case VIAPCR:
		cuda->pcr = (uint8)value;
		break;
	case VIAIFR:
		cuda->ifr &= (uint8)~value;
		UpdateIrq(machine);
		break;
	case VIAIER:
		if (value & 0x80)
			cuda->ier |= (uint8)(value & 0x7f);
		else
			cuda->ier &= (uint8)~value;
		UpdateIrq(machine);
		break;
	}
}

static void AutoPoll(OsxMachine *machine)
{
	OsxCuda *cuda;
	uint8 packet[OSXCUDABUFFER];
	uint8 request;
	int result;
	cuda = &machine->cuda;
	if (!cuda->autopoll || cuda->inposition < cuda->inlength || !(cuda->b & CUDATIP) || cuda->outlength > 0)
		return;
	result = 0;
	if (KeyboardHasData(cuda) && (cuda->pollmask & (1u << cuda->keyboard.address))) {
		request = (uint8)((cuda->keyboard.address << 4) | 0x0c);
		result = TalkKeyboard(cuda, 0, packet + 3);
	} else if (MouseHasData(cuda) && (cuda->pollmask & (1u << cuda->mouse.address))) {
		request = (uint8)((cuda->mouse.address << 4) | 0x0c);
		result = TalkMouse(cuda, 0, packet + 3);
	}
	if (result <= 0)
		return;
	packet[0] = CUDAPACKETADB;
	packet[1] = 0x40;
	packet[2] = request;
	SendToHost(machine, packet, 3 + result);
}

static uint64 EarlierDeadline(uint64 current, uint64 deadline)
{
	if (deadline && deadline < current)
		return deadline;
	return current;
}

uint64 OsxCudaNextDeadline(OsxMachine *machine)
{
	OsxCuda *cuda;
	uint64 next;
	cuda = &machine->cuda;
	next = EarlierDeadline(OSXNEVER, cuda->srdeadline);
	next = EarlierDeadline(next, cuda->t1deadline);
	next = EarlierDeadline(next, cuda->t2deadline);
	if (cuda->autopoll)
		next = EarlierDeadline(next, cuda->polldeadline);
	return next;
}

void OsxCudaPoll(OsxMachine *machine, uint64 now)
{
	OsxCuda *cuda;
	cuda = &machine->cuda;
	if (cuda->srdeadline && now >= cuda->srdeadline) {
		cuda->srdeadline = 0;
		cuda->ifr |= VIAIRQSR;
		UpdateIrq(machine);
	}
	if (cuda->t1deadline && now >= cuda->t1deadline) {
		cuda->ifr |= VIAIRQT1;
		if (cuda->acr & 0x40) {
			cuda->t1start = cuda->t1deadline;
			cuda->t1deadline = TimerDeadline(cuda->t1start, (uint32)cuda->t1latch + 1);
		} else {
			cuda->t1deadline = 0;
		}
		UpdateIrq(machine);
	}
	if (cuda->t2deadline && now >= cuda->t2deadline) {
		cuda->t2deadline = 0;
		cuda->ifr |= VIAIRQT2;
		UpdateIrq(machine);
	}
	if (now >= cuda->polldeadline) {
		cuda->polldeadline = now + MicrosecondsToTicks(cuda->pollrate * 1000);
#if OSXG5
		if (cuda->pmu) {
			PmuPoll(machine, now);
			return;
		}
#endif
		AutoPoll(machine);
	}
}

void OsxCudaKey(OsxMachine *machine, int code, int down)
{
	OsxCuda *cuda;
	int next;
	cuda = &machine->cuda;
	next = (cuda->keywrite + 1) % OSXADBQUEUE;
	if (next == cuda->keyread)
		return;
	if (down)
		cuda->keyqueue[cuda->keywrite] = (uint8)(code & 0x7f);
	else
		cuda->keyqueue[cuda->keywrite] = (uint8)(code | 0x80);
	cuda->keywrite = next;
}

void OsxCudaWheel(OsxMachine *machine, int delta)
{
	OsxCuda *cuda;
	OsxMouseEvent *event;
	int next;
	int previous;
	cuda = &machine->cuda;
	if (!cuda->mouseabsolute)
		return;
	previous = (cuda->mousewrite + OSXMOUSEQUEUE - 1) % OSXMOUSEQUEUE;
	if (cuda->mouseread != cuda->mousewrite && cuda->mousequeue[previous].wheel &&
		(cuda->mousequeue[previous].wheel > 0) == (delta > 0)) {
		cuda->mousequeue[previous].wheel += delta;
		return;
	}
	next = (cuda->mousewrite + 1) % OSXMOUSEQUEUE;
	if (next == cuda->mouseread)
		return;
	event = &cuda->mousequeue[cuda->mousewrite];
	event->x = cuda->mousex;
	event->y = cuda->mousey;
	event->button = cuda->mousebutton;
	event->motion = 0;
	event->wheel = delta;
	cuda->mousewrite = next;
}

void OsxCudaMouse(OsxMachine *machine, int dx, int dy, int button)
{
	OsxCuda *cuda;
	OsxMouseEvent *event;
	int next;
	int previous;
	int motion;
	cuda = &machine->cuda;
	cuda->mousex += dx;
	cuda->mousey += dy;
	if (cuda->mouseabsolute) {
		if (dx == 0 && dy == 0 && button == cuda->mousebutton)
			return;
		motion = button == cuda->mousebutton;
		previous = (cuda->mousewrite + OSXMOUSEQUEUE - 1) % OSXMOUSEQUEUE;
		if (motion && cuda->mouseread != cuda->mousewrite && cuda->mousequeue[previous].motion) {
			event = &cuda->mousequeue[previous];
		} else {
			next = (cuda->mousewrite + 1) % OSXMOUSEQUEUE;
			if (next == cuda->mouseread)
				cuda->mouseread = (cuda->mouseread + 1) % OSXMOUSEQUEUE;
			event = &cuda->mousequeue[cuda->mousewrite];
			cuda->mousewrite = next;
		}
		event->x = cuda->mousex;
		event->y = cuda->mousey;
		event->button = button;
		event->motion = motion;
		event->wheel = 0;
		cuda->mousebutton = button;
		return;
	}
	cuda->mousedx += dx;
	cuda->mousedy += dy;
	if ((button ^ cuda->mousebutton) & 1)
		cuda->mousepending++;
	cuda->mousebutton = button;
}
