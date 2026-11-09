/*
 *  osxaudio_sdl.cpp - SDL host audio output for the Mac OS X guest
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
#include "my_sdl.h"
#include "osxmachine.h"
#include "osxhost.h"
#include "bincue.h"

#define AUDIOFRAMEBYTES 4
#define AUDIOMARGINDIVISOR 200
#define AUDIOCDVOLUME 128
#define AUDIOCDBLOCK 8192

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void SDLCALL OsxGuestAudioCallback(void *context, SDL_AudioStream *stream, int length, int total)
{
	OsxAudio *audio;
	uint32 count;
	uint32 offset;
	uint32 first;
	audio = (OsxAudio *)context;
	if (length <= 0)
		return;
	count = audio->head - audio->tail;
	if (HaveAudioToMix_bincue()) {
		unsigned char mixed[AUDIOCDBLOCK];
		int total;
		total = length;
		if (total > (int)sizeof(mixed))
			total = (int)sizeof(mixed);
		total &= ~3;
		if (count > (uint32)total)
			count = (uint32)total;
		offset = audio->tail & (OSXAUDIORING - 1);
		first = OSXAUDIORING - offset;
		if (first > count)
			first = count;
		memcpy(mixed, audio->ring + offset, first);
		memcpy(mixed + first, audio->ring, count - first);
		memset(mixed + count, 0, total - count);
		audio->tail += count;
		MixAudio_bincue(mixed, total);
		SDL_PutAudioStreamData(stream, mixed, total);
		return;
	}
	if (count > (uint32)length + audio->rate / AUDIOMARGINDIVISOR * AUDIOFRAMEBYTES)
		count = (uint32)length + audio->rate / AUDIOMARGINDIVISOR * AUDIOFRAMEBYTES;
	offset = audio->tail & (OSXAUDIORING - 1);
	first = OSXAUDIORING - offset;
	if (first > count)
		first = count;
	SDL_PutAudioStreamData(stream, audio->ring + offset, (int)first);
	if (count > first)
		SDL_PutAudioStreamData(stream, audio->ring, (int)(count - first));
	audio->tail += count;
}

void OsxGuestAudioLockHost(OsxAudio *audio)
{
	SDL_LockAudioStream((SDL_AudioStream *)audio->stream);
}

void OsxGuestAudioUnlockHost(OsxAudio *audio)
{
	SDL_UnlockAudioStream((SDL_AudioStream *)audio->stream);
}

void OsxGuestAudioCloseHost(OsxAudio *audio)
{
	if (audio->stream) {
		SDL_DestroyAudioStream((SDL_AudioStream *)audio->stream);
		CloseAudio_bincue();
	}
	audio->stream = NULL;
}

void *OsxGuestAudioStartHost(OsxAudio *audio)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *stream;
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		return NULL;
	spec.freq = (int)audio->rate;
	spec.format = SDL_AUDIO_S16BE;
	spec.channels = 2;
	OpenAudio_bincue(spec.freq, spec.format, spec.channels, 0, AUDIOCDVOLUME);
	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, OsxGuestAudioCallback, audio);
	if (stream)
		SDL_ResumeAudioStreamDevice(stream);
	return stream;
}
#else
static void OsxGuestAudioCallback(void *context, Uint8 *stream, int length)
{
	OsxAudio *audio;
	uint32 available;
	uint32 count;
	uint32 index;
	audio = (OsxAudio *)context;
	available = audio->head - audio->tail;
	count = (uint32)length;
	if (count > available)
		count = available;
	for (index = 0; index < count; index++)
		stream[index] = audio->ring[(audio->tail + index) & (OSXAUDIORING - 1)];
	audio->tail += count;
	memset(stream + count, 0, (uint32)length - count);
	MixAudio_bincue(stream, length);
}

void OsxGuestAudioLockHost(OsxAudio *audio)
{
	SDL_LockAudio();
}

void OsxGuestAudioUnlockHost(OsxAudio *audio)
{
	SDL_UnlockAudio();
}

void OsxGuestAudioCloseHost(OsxAudio *audio)
{
	if (audio->stream) {
		SDL_CloseAudio();
		CloseAudio_bincue();
	}
	audio->stream = NULL;
}

void *OsxGuestAudioStartHost(OsxAudio *audio)
{
	SDL_AudioSpec spec;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return NULL;
	spec.freq = (int)audio->rate;
	spec.format = AUDIO_S16MSB;
	spec.channels = 2;
	spec.samples = 1024;
	spec.callback = OsxGuestAudioCallback;
	spec.userdata = audio;
	OpenAudio_bincue(spec.freq, spec.format, spec.channels, 0, AUDIOCDVOLUME);
	if (SDL_OpenAudio(&spec, NULL) < 0)
		return NULL;
	SDL_PauseAudio(0);
	return audio;
}
#endif

