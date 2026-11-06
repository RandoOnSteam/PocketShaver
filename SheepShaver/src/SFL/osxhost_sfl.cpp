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
#include "thread.h"

struct OsxHostVideo {
	uint8 *pixels;
	int pitch;
	OsxHostInput input;
	void *context;
};

static void OsxSflInput(void *context, int operation, int first, int second)
{
	OsxHostVideo *video = (OsxHostVideo *)context;
	switch (operation) {
	case SFLUIINPUTMOUSE:
		video->input(video->context, OSXHOSTMOUSE, first, second);
		break;
	case SFLUIINPUTBUTTON:
		video->input(video->context, OSXHOSTBUTTON, first, second);
		break;
	case SFLUIINPUTKEY:
		video->input(video->context, OSXHOSTKEY, first, second);
		break;
	case SFLUIINPUTQUIT:
		video->input(video->context, OSXHOSTQUIT, first, second);
		break;
	}
}

OsxHostVideo *OsxHostOpen(int width, int height, OsxHostInput input, void *context)
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
	SflUiSetInput(OsxSflInput, video);
	WSTimerBeginPeriod(1);
	SflUiSetFrame(video->pixels, width, height, video->pitch, 32);
	return video;
}

void OsxHostClose(OsxHostVideo *video)
{
	WSTimerEndPeriod(1);
	SflUiSetInput(NULL, NULL);
	SflUiSetFrame(NULL, 0, 0, 0, 0);
	SflUiClose();
	free(video->pixels);
	delete video;
}

uint8 *OsxHostLock(OsxHostVideo *video, int *pitch, int *raw)
{
	*pitch = video->pitch;
	*raw = 1;
	return video->pixels;
}

int OsxHostPresent(OsxHostVideo *video, const uint32 *runstart, const uint32 *runend, int runs)
{
	return runs && SflUiPresent(runstart[0], runend[runs - 1]);
}

void OsxHostEvents(OsxHostVideo *video, const uint8 *vram)
{
	SflUiPumpEvents();
	if (SflUiIsQuitting())
		video->input(video->context, OSXHOSTQUIT, 0, 0);
}

void OsxHostSleep(uint32 milliseconds)
{
	WSThreadSleep(milliseconds);
}

struct OsxSflAudio {
	WSCRITICALSECTION lock;
	void *device;
	OsxAudio *guest;
};

static void AudioFill(void *context, unsigned char *buffer, int length)
{
	OsxSflAudio *stream = (OsxSflAudio *)context;
	OsxAudio *audio = stream->guest;
	uint32 count;
	uint32 index;
	WSCriticalSection_Lock(&stream->lock);
	count = audio->head - audio->tail;
	if (count > (uint32)length)
		count = length;
	count &= ~1U;
	for (index = 0; index < count; index += 2) {
		buffer[index] = audio->ring[(audio->tail + index + 1) & (OSXAUDIORING - 1)];
		buffer[index + 1] = audio->ring[(audio->tail + index) & (OSXAUDIORING - 1)];
	}
	audio->tail += count;
	WSCriticalSection_Unlock(&stream->lock);
	memset(buffer + count, 0, length - count);
}

void OsxAudioLockHost(OsxAudio *audio)
{
	WSCriticalSection_Lock(&((OsxSflAudio *)audio->stream)->lock);
}

void OsxAudioUnlockHost(OsxAudio *audio)
{
	WSCriticalSection_Unlock(&((OsxSflAudio *)audio->stream)->lock);
}

void OsxAudioCloseHost(OsxAudio *audio)
{
	OsxSflAudio *stream = (OsxSflAudio *)audio->stream;
	if (stream) {
		SflAudioClose(stream->device);
		WSCriticalSection_Destruct(&stream->lock);
		delete stream;
		audio->stream = NULL;
	}
}

void *OsxAudioStartHost(OsxAudio *audio)
{
	OsxSflAudio *stream = new OsxSflAudio;
	stream->guest = audio;
	WSCriticalSection_Construct(&stream->lock);
	stream->device = SflAudioOpen((int)audio->rate, 2, 16, 4096, AudioFill, stream);
	if (!stream->device) {
		WSCriticalSection_Destruct(&stream->lock);
		delete stream;
		return NULL;
	}
	return stream;
}

char *OsxHostClipboardGet(void)
{
	char *text;
	size_t length;
	if (SflUiClipboardGet(&text, &length))
		return text;
	return NULL;
}

void OsxHostClipboardSet(const char *text)
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

static void NetReceive(void *context, const uint8 *frame, int length)
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

void *OsxHostNetOpen(const uint8 *address)
{
	OsxSflNetwork *network = new OsxSflNetwork;
	WSCriticalSection_Construct(&network->lock);
	network->read = 0;
	network->write = 0;
	network->device = SflNetOpenSlirp(address, NetReceive, network);
	if (!network->device) {
		WSCriticalSection_Destruct(&network->lock);
		delete network;
		return NULL;
	}
	return network;
}

void OsxHostNetClose(void *context)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	SflNetClose(network->device);
	WSCriticalSection_Destruct(&network->lock);
	delete network;
}

int OsxHostNetReady(void *context)
{
	return 1;
}

void OsxHostNetPoll(void *context)
{
}

int OsxHostNetReceive(void *context, uint8 *packet)
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

void OsxHostNetSend(void *context, const uint8 *packet, int length)
{
	OsxSflNetwork *network = (OsxSflNetwork *)context;
	SflNetSend(network->device, packet, length);
}
