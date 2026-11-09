/*
 *  osxhost_sfl.cpp - SFL host video, input, audio and clipboard for the Mac OS X guest
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
#include "osxhost.h"
#include "osxmachine.h"
#include "sflui.h"
#include "sflnet.h"
#include "gui.h"
#include "thread.h"
#include "bincue.h"

struct OsxHostVideo {
	uint8 *pixels;
	int pitch;
	OsxHostInput input;
	void *context;
};

static void OsxGuestSflInput(void *context, int operation, int first, int second)
{
	OsxHostVideo *video = (OsxHostVideo *)context;
	switch (operation) {
	case SFLUIINPUTMOUSE:
		video->input(video->context, OSXHOSTMOUSE, first, second);
		break;
	case SFLUIINPUTBUTTON:
		video->input(video->context, OSXHOSTBUTTON, first, second);
		break;
	case SFLUIINPUTWHEEL:
		video->input(video->context, OSXHOSTWHEEL, first, 0);
		break;
	case SFLUIINPUTKEY:
		video->input(video->context, OSXHOSTKEY, first, second);
		break;
	case SFLUIINPUTQUIT:
		video->input(video->context, OSXHOSTQUIT, first, second);
		break;
	}
}

OsxHostVideo *OsxGuestHostOpen(int width, int height, OsxHostInput input, void *context)
{
	OsxHostVideo *video = new OsxHostVideo;
	video->pitch = width * 4;
	video->input = input;
	video->context = context;
	video->pixels = (uint8 *)calloc(height, video->pitch);
	if (!video->pixels || !SflUiOpen("SheepShaver - Mac OS X", width, height, 0, 1)) {
		free(video->pixels);
		delete video;
		return NULL;
	}
	SflUiSetInput(OsxGuestSflInput, video);
	WSTimerBeginPeriod(1);
	SflUiSetFrame(video->pixels, width, height, video->pitch, 32);
	return video;
}

void OsxGuestHostSuspendFrame(OsxHostVideo *video)
{
	int width;
	int height;
	int depth;
	int hostwidth;
	int hostheight;
	SflUiGetStatus(&width, &height, &depth, &hostwidth, &hostheight);
	SflUiSetFrame(NULL, width, height, video->pitch, 32);
}

void OsxGuestHostClose(OsxHostVideo *video)
{
	WSTimerEndPeriod(1);
	SflUiSetInput(NULL, NULL);
	SflUiSetFrame(NULL, 0, 0, 0, 0);
	SflUiClose();
	free(video->pixels);
	delete video;
}

uint8 *OsxGuestHostLock(OsxHostVideo *video, int *pitch, int *raw)
{
	*pitch = video->pitch;
	*raw = 1;
	return video->pixels;
}

int OsxGuestHostPresent(OsxHostVideo *video, const uint32 *runstart, const uint32 *runend, int runs)
{
	return runs && SflUiPresent(runstart[0], runend[runs - 1]);
}

void OsxGuestHostEvents(OsxHostVideo *video, const uint8 *vram)
{
	SflUiPumpEvents();
	if (SflUiIsQuitting())
		video->input(video->context, OSXHOSTQUIT, 0, 0);
}

void OsxGuestHostSleep(uint32 milliseconds)
{
	WSThreadSleep(milliseconds);
}

#define AUDIOCDVOLUME 128

struct OsxSflAudio {
	WSCRITICALSECTION lock;
	void *device;
	OsxAudio *guest;
};

static void OsxGuestAudioFill(void *context, unsigned char *buffer, int length)
{
	OsxSflAudio *stream = (OsxSflAudio *)context;
	OsxAudio *audio = stream->guest;
	uint32 count;
	uint32 first;
	uint32 offset;
#ifndef WORDS_BIGENDIAN
	int index;
	unsigned char swap;
#endif
	WSCriticalSection_Lock(&stream->lock);
	count = audio->head - audio->tail;
	if (count > (uint32)length)
		count = length;
	count &= ~1U;
	offset = audio->tail & (OSXAUDIORING - 1);
	first = OSXAUDIORING - offset;
	if (first > count)
		first = count;
	memcpy(buffer, audio->ring + offset, first);
	memcpy(buffer + first, audio->ring, count - first);
	audio->tail += count;
	WSCriticalSection_Unlock(&stream->lock);
	memset(buffer + count, 0, length - count);
	MixAudio_bincue(buffer, length);
#ifndef WORDS_BIGENDIAN
	for (index = 0; index + 1 < length; index += 2) {
		swap = buffer[index];
		buffer[index] = buffer[index + 1];
		buffer[index + 1] = swap;
	}
#endif
}

void OsxGuestAudioLockHost(OsxAudio *audio)
{
	WSCriticalSection_Lock(&((OsxSflAudio *)audio->stream)->lock);
}

void OsxGuestAudioUnlockHost(OsxAudio *audio)
{
	WSCriticalSection_Unlock(&((OsxSflAudio *)audio->stream)->lock);
}

void OsxGuestAudioCloseHost(OsxAudio *audio)
{
	OsxSflAudio *stream = (OsxSflAudio *)audio->stream;
	if (stream) {
		SflAudioClose(stream->device);
		CloseAudio_bincue();
		WSCriticalSection_Destruct(&stream->lock);
		delete stream;
		audio->stream = NULL;
	}
}

void *OsxGuestAudioStartHost(OsxAudio *audio)
{
	OsxSflAudio *stream = new OsxSflAudio;
	stream->guest = audio;
	WSCriticalSection_Construct(&stream->lock);
	OpenAudio_bincue((int)audio->rate, BINCUE_SFL_AUDIO_S16MSB, 2, 0, AUDIOCDVOLUME);
	stream->device = SflAudioOpen((int)audio->rate, 2, 16, 4096, OsxGuestAudioFill, stream);
	if (!stream->device) {
		CloseAudio_bincue();
		WSCriticalSection_Destruct(&stream->lock);
		delete stream;
		return NULL;
	}
	return stream;
}

char *OsxGuestHostClipboardGet(void)
{
	char *text;
	size_t length;
	if (SflUiClipboardGet(&text, &length))
		return text;
	return NULL;
}

void OsxGuestHostClipboardSet(const char *text)
{
	SflUiClipboardSet(text, strlen(text));
}

#define OSXNETQUEUE 32

struct OsxSflNetwork {
	WSCRITICALSECTION lock;
	void *device;
	unsigned int read;
	unsigned int write;
	int lengths[OSXNETQUEUE];
	uint8 frames[OSXNETQUEUE][SFLNETFRAMESIZE];
};

static void OsxGuestNetReceive(void *context, const uint8 *frame, int length)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	unsigned int index;
	WSCriticalSection_Lock(&network->lock);
	if (network->write - network->read < OSXNETQUEUE && length > 0 && length <= SFLNETFRAMESIZE) {
		index = network->write++ % OSXNETQUEUE;
		network->lengths[index] = length;
		memcpy(network->frames[index], frame, length);
	}
	WSCriticalSection_Unlock(&network->lock);
}

void *OsxGuestHostNetOpen(const uint8 *address)
{
	OsxSflNetwork *network = new OsxSflNetwork;
	WSCriticalSection_Construct(&network->lock);
	network->read = 0;
	network->write = 0;
	network->device = SflNetOpenSlirp(address, OsxGuestNetReceive, network);
	if (!network->device) {
		WSCriticalSection_Destruct(&network->lock);
		delete network;
		return NULL;
	}
	return network;
}

void OsxGuestHostNetClose(void *context)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	SflNetClose(network->device);
	WSCriticalSection_Destruct(&network->lock);
	delete network;
}

int OsxGuestHostNetReady(void *context)
{
	return 1;
}

void OsxGuestHostNetPoll(void *context)
{
}

int OsxGuestHostNetReceive(void *context, uint8 *packet)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	int length = 0;
	unsigned int index;
	WSCriticalSection_Lock(&network->lock);
	if (network->read != network->write) {
		index = network->read++ % OSXNETQUEUE;
		length = network->lengths[index];
		memcpy(packet, network->frames[index], length);
	}
	WSCriticalSection_Unlock(&network->lock);
	return length;
}

void OsxGuestHostNetSend(void *context, const uint8 *packet, int length)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	SflNetSend(network->device, packet, length);
}
