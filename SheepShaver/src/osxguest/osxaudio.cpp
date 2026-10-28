/*
 *  osxaudio.cpp - OSX guest Screamer (AWACS) audio emulation
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
#include "prefs.h"
#include "osxmachine.h"
#include <math.h>

#define AUDIOCONTROL 0x00
#define AUDIOCODECCONTROL 0x10
#define AUDIOCODECSTATUS 0x20
#define AUDIOCLIPCOUNT 0x30
#define AUDIOBYTESWAP 0x40
#define AUDIOCODECBUSY 0x01000000
#define AUDIOSTATUSVALID 0x00400000
#define AUDIOSTATUSSCREAMER 0x00003100
#define AUDIOREADBACKREGISTER 7
#define AUDIOMUTEREGISTER 1
#define AUDIOHEADPHONEREGISTER 2
#define AUDIOSPEAKERREGISTER 4
#define AUDIOMUTEHEADPHONE 0x200
#define AUDIOMUTESPEAKER 0x080
#define AUDIOLATENCYDIVISOR 10
#define AUDIOFRAMEBYTES 4
#define AUDIOPOLLTICKS (OSXTIMEBASEFREQUENCY / 1000)

static uint32 SampleRate(uint32 control)
{
	const uint32 rates[8] = {44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350};
	return rates[(control >> 8) & 7];
}

static int32 AttenuationGain(uint32 steps)
{
	return (int32)(pow(10.0, -1.5 * (double)steps / 20.0) * 65536.0);
}

static void UpdateGain(OsxAudio *audio)
{
	uint32 mute;
	uint32 volume;
	mute = audio->codec[AUDIOMUTEREGISTER];
	if (!(mute & AUDIOMUTESPEAKER))
		volume = audio->codec[AUDIOSPEAKERREGISTER];
	else if (!(mute & AUDIOMUTEHEADPHONE))
		volume = audio->codec[AUDIOHEADPHONEREGISTER];
	else {
		audio->leftgain = 0;
		audio->rightgain = 0;
		return;
	}
	audio->leftgain = AttenuationGain((volume >> 6) & 15);
	audio->rightgain = AttenuationGain(volume & 15);
}

static void AudioCallback(void *context, Uint8 *stream, int length)
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
}

static void OpenHost(OsxMachine *machine)
{
	OsxAudio *audio;
	SDL_AudioSpec spec;
	audio = &machine->audio;
	if (audio->opened)
		SDL_CloseAudio();
	audio->opened = 0;
	audio->head = 0;
	audio->tail = 0;
	audio->openrate = audio->rate;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		audio->disabled = 1;
		return;
	}
	spec.freq = (int)audio->rate;
	spec.format = AUDIO_S16MSB;
	spec.channels = 2;
	spec.samples = 1024;
	spec.callback = AudioCallback;
	spec.userdata = audio;
	if (SDL_OpenAudio(&spec, NULL) < 0) {
		if (machine->log)
			fprintf(machine->log, "audio open failed: %s\n", SDL_GetError());
		audio->disabled = 1;
		return;
	}
	audio->opened = 1;
	SDL_PauseAudio(0);
	if (machine->log)
		fprintf(machine->log, "audio opened %u Hz\n", audio->rate);
}

void OsxAudioReset(OsxMachine *machine)
{
	OsxAudio *audio;
	audio = &machine->audio;
	if (audio->opened)
		SDL_CloseAudio();
	audio->opened = 0;
	audio->running = 0;
	audio->control = 0;
	audio->codeccontrol = 0;
	memset(audio->codec, 0, sizeof(audio->codec));
	audio->readback = 0;
	audio->byteswap = 0;
	audio->rate = SampleRate(0);
	audio->openrate = 0;
	audio->disabled = PrefsFindBool("nosound");
	audio->head = 0;
	audio->tail = 0;
	audio->lasttime = 0;
	UpdateGain(audio);
	machine->dbdma[OSXDBDMAAUDIOOUT].irq = OSXIRQAUDIOOUTDMA;
	machine->dbdma[OSXDBDMAAUDIOIN].irq = OSXIRQAUDIOINDMA;
}

void OsxAudioExit(OsxMachine *machine)
{
	if (machine->audio.opened)
		SDL_CloseAudio();
	machine->audio.opened = 0;
}

uint32 OsxAudioRead(OsxMachine *machine, uint32 offset)
{
	OsxAudio *audio;
	audio = &machine->audio;
	switch (offset & 0xf0) {
	case AUDIOCONTROL:
		return audio->control;
	case AUDIOCODECCONTROL:
		return audio->codeccontrol & ~AUDIOCODECBUSY;
	case AUDIOCODECSTATUS:
		if (audio->readback)
			return AUDIOSTATUSVALID | ((audio->codec[audio->readback - 1] & 0xfff) << 4);
		return AUDIOSTATUSVALID | AUDIOSTATUSSCREAMER;
	case AUDIOBYTESWAP:
		return audio->byteswap;
	}
	return 0;
}

void OsxAudioWrite(OsxMachine *machine, uint32 offset, uint32 value)
{
	OsxAudio *audio;
	uint32 address;
	uint32 data;
	audio = &machine->audio;
	switch (offset & 0xf0) {
	case AUDIOCONTROL:
		audio->control = value;
		audio->rate = SampleRate(value);
		break;
	case AUDIOCODECCONTROL:
		audio->codeccontrol = value & ~AUDIOCODECBUSY;
		address = (value >> 12) & 0x3ff;
		data = value & 0xfff;
		if (address == AUDIOREADBACKREGISTER) {
			audio->readback = 0;
			if (data & 1)
				audio->readback = ((data >> 1) & 7) + 1;
			break;
		}
		if (address < 8) {
			audio->codec[address] = data;
			UpdateGain(audio);
		}
		break;
	case AUDIOBYTESWAP:
		audio->byteswap = value;
		break;
	}
}

static void StoreBlock(OsxAudio *audio, uint32 length)
{
	uint8 *data;
	uint32 index;
	uint32 position;
	int32 sample;
	int32 gain;
	data = audio->block;
	for (index = 0; index + 1 < length; index += 2) {
		if (audio->byteswap & 1)
			sample = (int16)(data[index] | (data[index + 1] << 8));
		else
			sample = (int16)((data[index] << 8) | data[index + 1]);
		gain = audio->leftgain;
		if (index & 2)
			gain = audio->rightgain;
		sample = (int32)(((int64)sample * gain) >> 16);
		position = audio->head + index;
		audio->ring[position & (OSXAUDIORING - 1)] = (uint8)(sample >> 8);
		audio->ring[(position + 1) & (OSXAUDIORING - 1)] = (uint8)sample;
	}
}

void OsxAudioPoll(OsxMachine *machine, uint64 now)
{
	OsxAudio *audio;
	uint32 target;
	uint32 level;
	uint32 want;
	int got;
	int channel;
	audio = &machine->audio;
	channel = OSXDBDMAAUDIOOUT;
#if OSXG5
	if (machine->g5)
		channel = OSXG5DBDMAI2SOUT;
#endif
	if (!(machine->dbdma[channel].status & OSXDBDMASTATUSACTIVE)) {
		audio->running = 0;
		audio->lasttime = now;
		return;
	}
	if (!audio->disabled && (!audio->opened || audio->openrate != audio->rate))
		OpenHost(machine);
	level = 0;
	if (audio->opened) {
		if (audio->running && now < audio->lasttime)
			return;
		audio->lasttime = now + AUDIOPOLLTICKS;
		target = audio->rate * AUDIOFRAMEBYTES / AUDIOLATENCYDIVISOR;
		SDL_LockAudio();
		level = audio->head - audio->tail;
		while (!audio->running && level + AUDIOFRAMEBYTES <= target) {
			memset(audio->ring + (audio->head & (OSXAUDIORING - 1)), 0, AUDIOFRAMEBYTES);
			audio->head += AUDIOFRAMEBYTES;
			level += AUDIOFRAMEBYTES;
		}
		SDL_UnlockAudio();
		audio->running = 1;
	} else {
		if (now <= audio->lasttime)
			return;
		target = (uint32)((now - audio->lasttime) * audio->rate / OSXTIMEBASEFREQUENCY) * AUDIOFRAMEBYTES;
		if (target < OSXAUDIOBLOCK)
			return;
		audio->lasttime = now;
	}
	while (level + AUDIOFRAMEBYTES <= target) {
		want = (target - level) & ~(uint32)(AUDIOFRAMEBYTES - 1);
		if (want > OSXAUDIOBLOCK)
			want = OSXAUDIOBLOCK;
		got = OsxDbdmaTransfer(machine, channel, audio->block, want, 0);
		if (got <= 0)
			return;
#if OSXG5
		machine->g5state.i2sframes += (uint32)got / AUDIOFRAMEBYTES;
#endif
		if (audio->opened) {
			SDL_LockAudio();
			StoreBlock(audio, (uint32)got);
			audio->head += (uint32)got;
			SDL_UnlockAudio();
		}
		level += (uint32)got;
	}
}
