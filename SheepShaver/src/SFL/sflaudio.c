/*
 *  sflaudio.c - Sound output of the SFL platform layer
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
#include "gui.h"
#include "thread.h"
#include "audio.h"
#include "sflui.h"

#define SFLAUDIOIDLEMS 2
#define SFLAUDIOBUFFERBLOCKS 2

typedef struct SFLAUDIO
{
	WSAUDIO audio;
	WSTHREAD thread;
	WSSIGNAL exited;
	wsatomicint_t stop;
	SFLAUDIOFILL fill;
	void *context;
} SFLAUDIO;

typedef struct SFLAUDIOCAPTURE
{
	void *audio;
	WSCRITICALSECTION lock;
	unsigned char *ring;
	int ringbytes;
	int readoffset;
	int storedbytes;
} SFLAUDIOCAPTURE;

static void SflAudioFillProc(void *userdata, uint8_t *buffer, size_t size)
{
	SFLAUDIO *pThis;
	pThis = (SFLAUDIO *)userdata;
	pThis->fill(pThis->context, buffer, (int)size);
}

static WSTHREADENTRYCALLBACK(SflAudioPump)
{
	SFLAUDIO *pThis;
	pThis = (SFLAUDIO *)cbparam;
	while (!wsatomicintload(&pThis->stop)) {
		if (!WSAudio_ProcessWithLock(&pThis->audio, SflAudioFillProc, pThis))
			WSThreadSleep(SFLAUDIOIDLEMS);
	}
	WSSignal_Raise(&pThis->exited);
	return WSTHREADENTRYPROCRETCODE(0);
}

static void *SflAudioCreate(int rate, int channels, int bits, int bufferbytes, int flags, SFLAUDIOFILL fill,
	void *context)
{
	SFLAUDIO *pThis;
	WSAUDIOSPEC spec;
	WSASSERT(fill != NULL);
	pThis = WSALLOC1(SFLAUDIO);
	if (pThis == NULL)
		return NULL;
	pThis->fill = fill;
	pThis->context = context;
	wsatomicintstore(&pThis->stop, 0);
	WSAudioStreamFormat_Construct(&spec.format, channels, bits, rate, (size_t)bufferbytes);
	spec.proc = SflAudioFillProc;
	spec.procuserdata = pThis;
	spec.device = NULL;
	spec.flags = flags;
	if (!WSAudio_Create(&pThis->audio, &spec)) {
		WSFREE(pThis);
		return NULL;
	}
	WSSignal_Create(&pThis->exited);
	if (!WSThread_Create(&pThis->thread, SflAudioPump, pThis)) {
		WSSignal_Destroy(&pThis->exited);
		WSAudio_Destroy(&pThis->audio);
		WSFREE(pThis);
		return NULL;
	}
	WSAudio_Play(&pThis->audio);
	return pThis;
}

void *SflAudioOpen(int rate, int channels, int bits, int blockbytes, SFLAUDIOFILL fill, void *context)
{
	return SflAudioCreate(rate, channels, bits, blockbytes * SFLAUDIOBUFFERBLOCKS, 0, fill, context);
}

void SflAudioClose(void *audio)
{
	SFLAUDIO *pThis;
	pThis = (SFLAUDIO *)audio;
	if (pThis == NULL)
		return;
	wsatomicintstore(&pThis->stop, 1);
	WSSignal_Wait(&pThis->exited, -1);
	WSThread_Destroy(&pThis->thread);
	WSSignal_Destroy(&pThis->exited);
	WSAudio_Destroy(&pThis->audio);
	WSFREE(pThis);
}

static void SflAudioCaptureStore(void *context, unsigned char *buffer, int bytes)
{
	SFLAUDIOCAPTURE *pThis;
	int writeoffset;
	int chunk;
	pThis = (SFLAUDIOCAPTURE *)context;
	WSCriticalSection_Lock(&pThis->lock);
	if (bytes > pThis->ringbytes) {
		buffer += bytes - pThis->ringbytes;
		bytes = pThis->ringbytes;
	}
	if (pThis->storedbytes + bytes > pThis->ringbytes) {
		pThis->readoffset = (pThis->readoffset + pThis->storedbytes + bytes - pThis->ringbytes) % pThis->ringbytes;
		pThis->storedbytes = pThis->ringbytes - bytes;
	}
	writeoffset = (pThis->readoffset + pThis->storedbytes) % pThis->ringbytes;
	pThis->storedbytes += bytes;
	while (bytes > 0) {
		chunk = pThis->ringbytes - writeoffset;
		if (chunk > bytes)
			chunk = bytes;
		WSMemoryCopy(pThis->ring + writeoffset, buffer, chunk);
		buffer += chunk;
		bytes -= chunk;
		writeoffset = (writeoffset + chunk) % pThis->ringbytes;
	}
	WSCriticalSection_Unlock(&pThis->lock);
}

void *SflAudioCaptureOpen(int rate, int channels, int bits, int blockbytes, int ringbytes)
{
	SFLAUDIOCAPTURE *pThis;
	WSASSERT(ringbytes > 0);
	pThis = WSALLOC1(SFLAUDIOCAPTURE);
	if (pThis == NULL)
		return NULL;
	pThis->ring = WSALLOC(unsigned char, ringbytes);
	if (pThis->ring == NULL) {
		WSFREE(pThis);
		return NULL;
	}
	pThis->ringbytes = ringbytes;
	pThis->readoffset = 0;
	pThis->storedbytes = 0;
	WSCriticalSection_Construct(&pThis->lock);
	pThis->audio = SflAudioCreate(rate, channels, bits, blockbytes * SFLAUDIOBUFFERBLOCKS, WSAUDIO_INPUT,
		SflAudioCaptureStore, pThis);
	if (pThis->audio == NULL) {
		WSCriticalSection_Destruct(&pThis->lock);
		WSFREE(pThis->ring);
		WSFREE(pThis);
		return NULL;
	}
	return pThis;
}

int SflAudioCaptureRead(void *capture, unsigned char *buffer, int bytes)
{
	SFLAUDIOCAPTURE *pThis;
	int copied;
	int chunk;
	pThis = (SFLAUDIOCAPTURE *)capture;
	copied = 0;
	WSCriticalSection_Lock(&pThis->lock);
	if (bytes > pThis->storedbytes)
		bytes = pThis->storedbytes;
	while (copied < bytes) {
		chunk = pThis->ringbytes - pThis->readoffset;
		if (chunk > bytes - copied)
			chunk = bytes - copied;
		WSMemoryCopy(buffer + copied, pThis->ring + pThis->readoffset, chunk);
		copied += chunk;
		pThis->readoffset = (pThis->readoffset + chunk) % pThis->ringbytes;
	}
	pThis->storedbytes -= copied;
	WSCriticalSection_Unlock(&pThis->lock);
	return copied;
}

void SflAudioCaptureClear(void *capture)
{
	SFLAUDIOCAPTURE *pThis;
	pThis = (SFLAUDIOCAPTURE *)capture;
	WSCriticalSection_Lock(&pThis->lock);
	pThis->readoffset = 0;
	pThis->storedbytes = 0;
	WSCriticalSection_Unlock(&pThis->lock);
}

void SflAudioCaptureClose(void *capture)
{
	SFLAUDIOCAPTURE *pThis;
	pThis = (SFLAUDIOCAPTURE *)capture;
	SflAudioClose(pThis->audio);
	WSCriticalSection_Destruct(&pThis->lock);
	WSFREE(pThis->ring);
	WSFREE(pThis);
}
