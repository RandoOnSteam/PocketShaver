/*
 *  sflnet.c - Ethernet frame transport of the SFL platform layer
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

#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "server.h"
#include "thread.h"
#include "sflnet.h"
#include "sflslirp.h"

#define SFLNETWAITMICROS 100000
#define SFLNETBROADCAST 0xffffffffUL
#define SFLNETHEADERSIZE 14
#define SFLNETBUFFERSIZE 2048
#define SFLNETQUEUESIZE 64
#define SFLNETSLIRPPOLLMS 5
#define SFLNETMODEUDP 0
#define SFLNETMODESLIRP 1

typedef struct SFLNETFRAME
{
	int length;
	unsigned char data[SFLNETFRAMESIZE];
} SFLNETFRAME;

typedef struct SFLNETQUEUE
{
	SFLNETFRAME frames[SFLNETQUEUESIZE];
	int head;
	int count;
} SFLNETQUEUE;

typedef struct SFLNET
{
	WSSOCKETLIBRARY library;
	WSSOCKET socket;
	WSTHREAD thread;
	WSSIGNAL exited;
	WSCRITICALSECTION inputlock;
	wsatomicint_t stop;
	SFLNETRECEIVE receive;
	void *context;
	int mode;
	int port;
	unsigned char address[SFLNETADDRESSSIZE];
	unsigned char buffer[SFLNETBUFFERSIZE];
	SFLNETQUEUE input;
	SFLNETQUEUE output;
	SFLNETFRAME frame;
	wsbool librarycreated;
	wsbool socketcreated;
	wsbool threadcreated;
} SFLNET;

static wsbool SflNetQueuePush(SFLNETQUEUE *queue, const unsigned char *data, int length)
{
	SFLNETFRAME *frame;
	if (queue->count == SFLNETQUEUESIZE || length > SFLNETFRAMESIZE)
		return FALSE;
	frame = &queue->frames[(queue->head + queue->count) % SFLNETQUEUESIZE];
	memcpy(frame->data, data, (size_t)length);
	frame->length = length;
	queue->count++;
	return TRUE;
}

static wsbool SflNetQueuePop(SFLNETQUEUE *queue, SFLNETFRAME *frame)
{
	if (queue->count == 0)
		return FALSE;
	*frame = queue->frames[queue->head];
	queue->head = (queue->head + 1) % SFLNETQUEUESIZE;
	queue->count--;
	return TRUE;
}

static wsbool SflNetWanted(SFLNET *pThis, const unsigned char *frame, size_t length)
{
	if (length < SFLNETHEADERSIZE)
		return FALSE;
	if (memcmp(frame + SFLNETADDRESSSIZE, pThis->address, SFLNETADDRESSSIZE) == 0)
		return FALSE;
	if (frame[0] & 1)
		return TRUE;
	return memcmp(frame, pThis->address, SFLNETADDRESSSIZE) == 0;
}

static void SflNetUdpLoop(SFLNET *pThis)
{
	WSSOCKADDRv4 from;
	size_t length;
	while (!wsatomicintload(&pThis->stop)) {
		if (!WSSocket_WaitForRead(&pThis->socket, SFLNETWAITMICROS, NULL))
			continue;
		length = WSSocket_ReadEndpointWithAddr(&pThis->socket, pThis->buffer, sizeof(pThis->buffer), &from, NULL, NULL);
		if (length == (size_t)-1 || length > SFLNETFRAMESIZE)
			continue;
		if (SflNetWanted(pThis, pThis->buffer, length))
			pThis->receive(pThis->context, pThis->buffer, (int)length);
	}
}

static void SflNetSlirpOutput(void *context, const unsigned char *frame, int length)
{
	SFLNET *pThis;
	pThis = (SFLNET *)context;
	SflNetQueuePush(&pThis->output, frame, length);
}

static void SflNetSlirpLoop(SFLNET *pThis)
{
	wsbool more;
	while (!wsatomicintload(&pThis->stop)) {
		do {
			WSCriticalSection_Lock(&pThis->inputlock);
			more = SflNetQueuePop(&pThis->input, &pThis->frame);
			WSCriticalSection_Unlock(&pThis->inputlock);
			if (more)
				SflSlirpInput(pThis->frame.data, pThis->frame.length);
		} while (more);
		SflSlirpPoll(SFLNETSLIRPPOLLMS);
		while (SflNetQueuePop(&pThis->output, &pThis->frame) && !wsatomicintload(&pThis->stop))
			pThis->receive(pThis->context, pThis->frame.data, pThis->frame.length);
	}
}

static WSTHREADENTRYCALLBACK(SflNetThread)
{
	SFLNET *pThis;
	pThis = (SFLNET *)cbparam;
	if (pThis->mode == SFLNETMODESLIRP)
		SflNetSlirpLoop(pThis);
	else
		SflNetUdpLoop(pThis);
	WSSignal_Raise(&pThis->exited);
	return WSTHREADENTRYPROCRETCODE(0);
}

void SflNetClose(void *net)
{
	SFLNET *pThis;
	pThis = (SFLNET *)net;
	if (pThis == NULL)
		return;
	wsatomicintstore(&pThis->stop, 1);
	if (pThis->threadcreated) {
		WSSignal_Wait(&pThis->exited, WSSIGNAL_WAITINFINITE);
		WSThread_Destroy(&pThis->thread);
	}
	WSSignal_Destroy(&pThis->exited);
	WSCriticalSection_Destruct(&pThis->inputlock);
	if (pThis->socketcreated)
		WSSocket_Destroy(&pThis->socket);
	if (pThis->librarycreated)
		WSSocketLibrary_Destroy(&pThis->library);
	WSFREE(pThis);
}

static SFLNET *SflNetAllocate(int mode, const unsigned char *address, SFLNETRECEIVE receive, void *context)
{
	SFLNET *pThis;
	WSASSERT(receive != NULL);
	pThis = WSALLOC1(SFLNET);
	if (pThis == NULL)
		return NULL;
	WSMemoryClear(pThis, sizeof(*pThis));
	pThis->mode = mode;
	pThis->receive = receive;
	pThis->context = context;
	WSMemoryCopy(pThis->address, address, SFLNETADDRESSSIZE);
	wsatomicintstore(&pThis->stop, 0);
	WSSignal_Create(&pThis->exited);
	WSCriticalSection_Construct(&pThis->inputlock);
	WSSocket_Construct(&pThis->socket);
	return pThis;
}

static void *SflNetStart(SFLNET *pThis)
{
	pThis->threadcreated = WSThread_Create(&pThis->thread, SflNetThread, pThis);
	if (!pThis->threadcreated) {
		SflNetClose(pThis);
		return NULL;
	}
	return pThis;
}

void *SflNetOpenUdp(int port, const unsigned char *address, SFLNETRECEIVE receive, void *context)
{
	SFLNET *pThis;
	WSSOCKADDRv4 bindaddress;
	pThis = SflNetAllocate(SFLNETMODEUDP, address, receive, context);
	if (pThis == NULL)
		return NULL;
	pThis->port = port;
	pThis->librarycreated = WSSocketLibrary_Create(&pThis->library);
	if (!pThis->librarycreated) {
		SflNetClose(pThis);
		return NULL;
	}
	WSSockAddrv4_Construct(&bindaddress, WSSOCKADDRv4_ANY, (uint16_t)port);
	pThis->socketcreated = WSSocket_CreateEndpointWithAddr(&pThis->socket, &bindaddress,
		WSSOCKETENDPOINT_REUSEADDR | WSSOCKETENDPOINT_BROADCAST, &pThis->library, NULL);
	if (!pThis->socketcreated) {
		SflNetClose(pThis);
		return NULL;
	}
	return SflNetStart(pThis);
}

void *SflNetOpenSlirp(const unsigned char *address, SFLNETRECEIVE receive, void *context)
{
	SFLNET *pThis;
	pThis = SflNetAllocate(SFLNETMODESLIRP, address, receive, context);
	if (pThis == NULL)
		return NULL;
	if (!SflSlirpInit(SflNetSlirpOutput, pThis)) {
		SflNetClose(pThis);
		return NULL;
	}
	return SflNetStart(pThis);
}

int SflNetSend(void *net, const unsigned char *frame, int length)
{
	SFLNET *pThis;
	WSSOCKADDRv4 to;
	size_t sent;
	wsbool queued;
	pThis = (SFLNET *)net;
	if (pThis->mode == SFLNETMODESLIRP) {
		WSCriticalSection_Lock(&pThis->inputlock);
		queued = SflNetQueuePush(&pThis->input, frame, length);
		WSCriticalSection_Unlock(&pThis->inputlock);
		return queued;
	}
	WSSockAddrv4_Construct(&to, SFLNETBROADCAST, (uint16_t)pThis->port);
	sent = WSSocket_WriteEndpointWithAddr(&pThis->socket, frame, (size_t)length, &to, NULL, NULL);
	return sent == (size_t)length;
}

unsigned long SflNetProcessId(void)
{
	return (unsigned long)WSTaskGetId();
}
