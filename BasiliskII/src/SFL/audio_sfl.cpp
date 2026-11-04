/*
 *  audio_sfl.cpp - Sound driver of the SFL platform layer
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
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"
#include "bincue.h"
#include "sflui.h"
#include "thread.h"
#include "file.h"

#define SFLSOUNDMAXVOLUME 0x0100
#define SFLSOUNDBLOCKFRAMES 4096
#define SFLSOUNDWAITMS 200
#define SFLSOUNDCAPTUREFRAMES 512
#define SFLSOUNDCDVOLUME 128
#define SFLSOUNDSTARTUPFILE "startup.wav"
#define SFLSOUNDSTARTUPTAILMS 500
#define SFLSOUNDSTARTUPFRAMES 1024
#define SFLSOUNDWAVHEADER 12
#define SFLSOUNDWAVCHUNK 8
#define SFLSOUNDWAVPCM 1

typedef struct SFLSTARTUP {
	uint8 *contents;
	uint8 *samples;
	int bytes;
	int position;
	int rate;
	int channels;
	int bits;
	WSSIGNAL finished;
	WSTHREAD thread;
} SFLSTARTUP;

typedef struct SFLSOUND {
	SFLSTARTUP *startup;
	void *device;
	WSSIGNAL done;
	wsatomicint_t closing;
	int rateindex;
	int sizeindex;
	int channelindex;
	int blockbytes;
	int mainvolume;
	int speakervolume;
	bool mainmute;
	bool speakermute;
	bool created;
} SFLSOUND;

typedef struct SFLSOUNDCAPTURE {
	void *device;
	int bits;
	int twos;
} SFLSOUNDCAPTURE;

static SFLSOUND SflSound;

static void SflSoundScale(uint8 *buffer, int bytes, int bits)
{
	int volume;
	int index;
	int sample;
	volume = SflSound.mainvolume * SflSound.speakervolume / SFLSOUNDMAXVOLUME;
	if (volume >= SFLSOUNDMAXVOLUME)
		return;
	if (bits == 16) {
		for (index = 0; index + 1 < bytes; index += 2) {
			sample = (int16)((buffer[index] << 8) | buffer[index + 1]);
			sample = sample * volume / SFLSOUNDMAXVOLUME;
			buffer[index] = (uint8)(sample >> 8);
			buffer[index + 1] = (uint8)sample;
		}
	} else {
		for (index = 0; index < bytes; index++)
			buffer[index] = (uint8)(0x80 + ((int)buffer[index] - 0x80) * volume / SFLSOUNDMAXVOLUME);
	}
}

static void SflSoundSwap16(uint8 *buffer, int bytes)
{
	int index;
	uint8 swap;
	for (index = 0; index + 1 < bytes; index += 2) {
		swap = buffer[index];
		buffer[index] = buffer[index + 1];
		buffer[index + 1] = swap;
	}
}

static void SflSoundToHost(uint8 *buffer, int bytes, int bits)
{
#ifndef WORDS_BIGENDIAN
	if (bits == 16)
		SflSoundSwap16(buffer, bytes);
#endif
}

static void SflSoundGuest(uint8 *buffer, int bytes)
{
	uint32 info;
	uint8 *source;
	int count;
	int index;
	bool doublemono;
	if (AudioStatus.num_sources == 0 || wsatomicintload(&SflSound.closing))
		return;
	SetInterruptFlag(INTFLAG_AUDIO);
	TriggerInterrupt();
	if (!WSSignal_Wait(&SflSound.done, SFLSOUNDWAITMS) || wsatomicintload(&SflSound.closing))
		return;
	info = ReadMacInt32(audio_data + adatStreamInfo);
	if (info == 0 || SflSound.mainmute || SflSound.speakermute)
		return;
	count = ReadMacInt32(info + scd_sampleCount) * (AudioStatus.sample_size >> 3) * AudioStatus.channels;
	if (count > bytes)
		count = bytes;
	if (count <= 0)
		return;
	source = Mac2HostAddr(ReadMacInt32(info + scd_buffer));
	doublemono = AudioStatus.channels == 2 && ReadMacInt16(info + scd_numChannels) == 1 && ReadMacInt16(info + scd_sampleSize) == 8;
	if (doublemono) {
		for (index = 0; index + 1 < count; index += 2) {
			buffer[index] = source[index >> 1];
			buffer[index + 1] = source[index >> 1];
		}
	} else
		memcpy(buffer, source, count);
	SflSoundScale(buffer, count, AudioStatus.sample_size);
}

static void SflSoundFill(void *context, unsigned char *buffer, int bytes)
{
	if (AudioStatus.sample_size == 8)
		memset(buffer, 0x80, bytes);
	else
		memset(buffer, 0, bytes);
	SflSoundGuest(buffer, bytes);
	MixAudio_bincue(buffer, bytes);
	SflSoundToHost(buffer, bytes, AudioStatus.sample_size);
}

static void SflSoundSetStatus(void)
{
	AudioStatus.sample_rate = audio_sample_rates[SflSound.rateindex];
	AudioStatus.sample_size = audio_sample_sizes[SflSound.sizeindex];
	AudioStatus.channels = audio_channel_counts[SflSound.channelindex];
}

static bool SflSoundOpen(void)
{
	if (audio_sample_sizes.empty()) {
		audio_sample_rates.push_back(11025 << 16);
		audio_sample_rates.push_back(22050 << 16);
		audio_sample_rates.push_back(44100 << 16);
		audio_sample_sizes.push_back(8);
		audio_sample_sizes.push_back(16);
		audio_channel_counts.push_back(1);
		audio_channel_counts.push_back(2);
		SflSound.rateindex = audio_sample_rates.size() - 1;
		SflSound.sizeindex = audio_sample_sizes.size() - 1;
		SflSound.channelindex = audio_channel_counts.size() - 1;
	}
	SflSoundSetStatus();
	audio_frames_per_block = SFLSOUNDBLOCKFRAMES >> PrefsFindInt32("sound_buffer");
	SflSound.blockbytes = audio_frames_per_block * (AudioStatus.sample_size >> 3) * AudioStatus.channels;
	wsatomicintstore(&SflSound.closing, 0);
	SflSound.device = SflAudioOpen(AudioStatus.sample_rate >> 16, AudioStatus.channels, AudioStatus.sample_size,
		SflSound.blockbytes, SflSoundFill, NULL);
	if (SflSound.device == NULL) {
		WarningAlert(GetString(STR_NO_AUDIO_WARN));
		return false;
	}
	if (AudioStatus.sample_size == 8)
		OpenAudio_bincue(AudioStatus.sample_rate >> 16, BINCUE_SFL_AUDIO_U8, AudioStatus.channels, 0x80, SFLSOUNDCDVOLUME);
	else
		OpenAudio_bincue(AudioStatus.sample_rate >> 16, BINCUE_SFL_AUDIO_S16MSB, AudioStatus.channels, 0, SFLSOUNDCDVOLUME);
	audio_open = true;
	return true;
}

static void SflSoundClose(void)
{
	wsatomicintstore(&SflSound.closing, 1);
	WSSignal_Raise(&SflSound.done);
	if (SflSound.device) {
		SflAudioClose(SflSound.device);
		CloseAudio_bincue();
	}
	SflSound.device = NULL;
	audio_open = false;
}

void AudioInit(void)
{
	AudioStatus.sample_rate = 44100 << 16;
	AudioStatus.sample_size = 16;
	AudioStatus.channels = 2;
	AudioStatus.mixer = 0;
	AudioStatus.num_sources = 0;
	audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;
	SflSound.mainvolume = SFLSOUNDMAXVOLUME;
	SflSound.speakervolume = SFLSOUNDMAXVOLUME;
	if (!SflSound.created) {
		WSSignal_Create(&SflSound.done);
		SflSound.created = true;
	}
	if (PrefsFindBool("nosound"))
		return;
	SflSoundOpen();
}

void AudioExit(void)
{
	SflSoundClose();
	if (SflSound.startup != NULL) {
		WSSignal_Raise(&SflSound.startup->finished);
		WSThread_WaitForExit(&SflSound.startup->thread);
		WSThread_Destroy(&SflSound.startup->thread);
		WSSignal_Destroy(&SflSound.startup->finished);
		delete[] SflSound.startup->contents;
		delete SflSound.startup;
	}
}

void audio_enter_stream()
{
}

void audio_exit_stream()
{
}

void AudioInterrupt(void)
{
	M68kRegisters r;
	if (AudioStatus.mixer) {
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
	} else
		WriteMacInt32(audio_data + adatStreamInfo, 0);
	WSSignal_Raise(&SflSound.done);
}

bool audio_set_sample_rate(int index)
{
	SflSoundClose();
	SflSound.rateindex = index;
	return SflSoundOpen();
}

bool audio_set_sample_size(int index)
{
	SflSoundClose();
	SflSound.sizeindex = index;
	return SflSoundOpen();
}

bool audio_set_channels(int index)
{
	SflSoundClose();
	SflSound.channelindex = index;
	return SflSoundOpen();
}

bool audio_get_main_mute(void)
{
	return SflSound.mainmute;
}

uint32 audio_get_main_volume(void)
{
	return ((uint32)SflSound.mainvolume << 16) + SflSound.mainvolume;
}

bool audio_get_speaker_mute(void)
{
	return SflSound.speakermute;
}

uint32 audio_get_speaker_volume(void)
{
	return ((uint32)SflSound.speakervolume << 16) + SflSound.speakervolume;
}

void audio_set_main_mute(bool mute)
{
	SflSound.mainmute = mute;
}

void audio_set_main_volume(uint32 vol)
{
	SflSound.mainvolume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (SflSound.mainvolume > SFLSOUNDMAXVOLUME)
		SflSound.mainvolume = SFLSOUNDMAXVOLUME;
}

void audio_set_speaker_mute(bool mute)
{
	SflSound.speakermute = mute;
}

void audio_set_speaker_volume(uint32 vol)
{
	SflSound.speakervolume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (SflSound.speakervolume > SFLSOUNDMAXVOLUME)
		SflSound.speakervolume = SFLSOUNDMAXVOLUME;
}

static uint32 SflSoundLittle32(const uint8 *bytes)
{
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32)bytes[3] << 24);
}

static uint16 SflSoundLittle16(const uint8 *bytes)
{
	return (uint16)(bytes[0] | (bytes[1] << 8));
}

static uint8 *SflSoundLoadFile(const char *path, int *size)
{
	WSFILE file;
	uint8 *contents;
	size_t filesize;
	if (!WSFile_Create(&file, path, FILE_READ | FILE_BINARY))
		return NULL;
	contents = NULL;
	filesize = WSFile_Size(&file);
	if (filesize > SFLSOUNDWAVHEADER) {
		contents = new uint8[filesize];
		*size = (int)WSFile_Read(&file, contents, filesize);
	}
	WSFile_Destroy(&file);
	return contents;
}

static bool SflSoundParseWave(SFLSTARTUP *startup, uint8 *contents, int size)
{
	int offset;
	int chunkbytes;
	bool haveformat;
	if (memcmp(contents, "RIFF", 4) != 0 || memcmp(contents + 8, "WAVE", 4) != 0)
		return false;
	haveformat = false;
	for (offset = SFLSOUNDWAVHEADER; offset + SFLSOUNDWAVCHUNK <= size; offset += SFLSOUNDWAVCHUNK + ((chunkbytes + 1) & ~1)) {
		chunkbytes = (int)SflSoundLittle32(contents + offset + 4);
		if (chunkbytes < 0 || chunkbytes > size - offset - SFLSOUNDWAVCHUNK)
			chunkbytes = size - offset - SFLSOUNDWAVCHUNK;
		if (memcmp(contents + offset, "fmt ", 4) == 0 && chunkbytes >= 16) {
			if (SflSoundLittle16(contents + offset + 8) != SFLSOUNDWAVPCM)
				return false;
			startup->channels = SflSoundLittle16(contents + offset + 10);
			startup->rate = (int)SflSoundLittle32(contents + offset + 12);
			startup->bits = SflSoundLittle16(contents + offset + 22);
			haveformat = true;
		} else if (memcmp(contents + offset, "data", 4) == 0 && haveformat) {
			startup->samples = contents + offset + SFLSOUNDWAVCHUNK;
			startup->bytes = chunkbytes;
#ifdef WORDS_BIGENDIAN
			if (startup->bits == 16)
				SflSoundSwap16(startup->samples, chunkbytes);
#endif
			return startup->bits == 8 || startup->bits == 16;
		}
	}
	return false;
}

static void SflSoundStartupFill(void *context, unsigned char *buffer, int bytes)
{
	SFLSTARTUP *startup;
	int count;
	startup = (SFLSTARTUP *)context;
	count = startup->bytes - startup->position;
	if (count > bytes)
		count = bytes;
	if (count > 0)
		memcpy(buffer, startup->samples + startup->position, count);
	if (startup->bits == 8)
		memset(buffer + count, 0x80, bytes - count);
	else
		memset(buffer + count, 0, bytes - count);
	startup->position += count;
	if (count < bytes)
		WSSignal_Raise(&startup->finished);
}

static WSTHREADENTRYCALLBACK(SflSoundStartupThread)
{
	SFLSTARTUP *startup;
	void *device;
	startup = (SFLSTARTUP *)cbparam;
	device = SflAudioOpen(startup->rate, startup->channels, startup->bits,
		SFLSOUNDSTARTUPFRAMES * startup->channels * (startup->bits >> 3), SflSoundStartupFill, startup);
	if (device != NULL) {
		WSSignal_Wait(&startup->finished, -1);
		WSThreadSleep(SFLSOUNDSTARTUPTAILMS);
		SflAudioClose(device);
	}
	return WSTHREADENTRYPROCRETCODE(0);
}

void PlayStartupSound()
{
	SFLSTARTUP *startup;
	uint8 *contents;
	int size;
	if (SflSound.startup != NULL)
		return;
	contents = SflSoundLoadFile(SFLSOUNDSTARTUPFILE, &size);
	if (contents == NULL)
		return;
	startup = new SFLSTARTUP;
	startup->contents = contents;
	startup->position = 0;
	if (!SflSoundParseWave(startup, contents, size)) {
		delete startup;
		delete[] contents;
		return;
	}
	WSSignal_Create(&startup->finished);
	if (!WSThread_Create(&startup->thread, SflSoundStartupThread, startup)) {
		WSSignal_Destroy(&startup->finished);
		delete startup;
		delete[] contents;
		return;
	}
	SflSound.startup = startup;
}

void *AudioCaptureOpen(int rate, int bits, int channels, int twos)
{
	SFLSOUNDCAPTURE *capture;
	int framebytes;
	capture = new SFLSOUNDCAPTURE;
	capture->bits = bits;
	capture->twos = twos;
	framebytes = (bits >> 3) * channels;
	capture->device = SflAudioCaptureOpen(rate, channels, bits, SFLSOUNDCAPTUREFRAMES * framebytes, rate * framebytes);
	if (capture->device == NULL) {
		delete capture;
		return NULL;
	}
	return capture;
}

uint32 AudioCaptureRead(void *capture, uint8 *buffer, uint32 bytes)
{
	SFLSOUNDCAPTURE *soundcapture;
	int copied;
	int index;
	soundcapture = (SFLSOUNDCAPTURE *)capture;
	copied = SflAudioCaptureRead(soundcapture->device, buffer, (int)bytes);
	if (soundcapture->bits == 16)
		SflSoundToHost(buffer, copied, 16);
	else if (soundcapture->twos) {
		for (index = 0; index < copied; index++)
			buffer[index] ^= 0x80;
	}
	return (uint32)copied;
}

void AudioCaptureClear(void *capture)
{
	SflAudioCaptureClear(((SFLSOUNDCAPTURE *)capture)->device);
}

void AudioCaptureClose(void *capture)
{
	SFLSOUNDCAPTURE *soundcapture;
	soundcapture = (SFLSOUNDCAPTURE *)capture;
	SflAudioCaptureClose(soundcapture->device);
	delete soundcapture;
}
