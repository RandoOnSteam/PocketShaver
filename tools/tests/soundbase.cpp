#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

#define USE_SDL3 1
#define USE_SDL_AUDIO 1
#include "audio.h"

struct M68kRegisters {
	uint32 d[8];
	uint32 a[8];
};

#define CHECK(expression) do { checks++; if (!(expression)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #expression); } } while (0)

class SoundBaseTest {
public:
	audio_status AudioStatus;
	uint8 ram[131072];
	uint8 lowmem[4096];
	uint8 via[MACIO_VIA_SIZE];
	uint32 RAMBase;
	uint32 RAMBaseMac;
	uint32 RAMSize;
	uint32 allocation;
	uint32 requested;
	int checks;
	int failures;

	SoundBaseTest(uint32 rambase)
	{
		memset(&AudioStatus, 0, sizeof(AudioStatus));
		memset(ram, 0, sizeof(ram));
		memset(lowmem, 0, sizeof(lowmem));
		memset(via, 0, sizeof(via));
		RAMBase = rambase;
		RAMBaseMac = rambase;
		RAMSize = sizeof(ram);
		allocation = RAMBase + 4096;
		requested = 0;
		checks = 0;
		failures = 0;
	}

	uint8 *Mac2HostAddr(uint32 address)
	{
		if (address < sizeof(lowmem))
			return lowmem + address;
		if (address >= MACIO_VIA_BASE && address - MACIO_VIA_BASE < MACIO_VIA_SIZE)
			return via + address - MACIO_VIA_BASE;
		CHECK(address - RAMBase < RAMSize);
		return ram + address - RAMBase;
	}

	uint32 ReadMacInt32(uint32 address)
	{
		uint8 *source;
		source = Mac2HostAddr(address);
		return ((uint32)source[0] << 24) | ((uint32)source[1] << 16) | ((uint32)source[2] << 8) | source[3];
	}

	void WriteMacInt32(uint32 address, uint32 value)
	{
		uint8 *target;
		target = Mac2HostAddr(address);
		target[0] = (uint8)(value >> 24);
		target[1] = (uint8)(value >> 16);
		target[2] = (uint8)(value >> 8);
		target[3] = (uint8)value;
	}

	uint8 ReadMacInt8(uint32 address)
	{
		return *Mac2HostAddr(address);
	}

	void WriteMacInt8(uint32 address, uint8 value)
	{
		*Mac2HostAddr(address) = value;
	}

	void Execute68kTrap(uint16 trap, M68kRegisters *registers)
	{
		CHECK(trap == 0xa71e);
		requested = registers->d[0];
		registers->a[0] = allocation;
	}

#include "soundbase-source.inc"

	void CheckFrame(uint32 frame, uint32 source)
	{
		uint32 sample;
		uint32 index;
		for (sample = 0; sample < AUDIO_SOUNDBASE_SAMPLES; sample++) {
			index = (sample + AUDIO_SOUNDBASE_PHASE) % AUDIO_SOUNDBASE_SAMPLES;
			CHECK(AudioStatus.soundring[(frame & (AUDIO_SOUNDBASE_RING - 1)) * AUDIO_SOUNDBASE_SAMPLES + sample] == ReadMacInt8(source + index * 2));
		}
	}

	void Run(uint32 viabase)
	{
		uint32 sample;
		uint32 frame;
		uint32 mainbase;
		uint32 alternate;
		uint32 port;
		uint8 output[4096];
		WriteMacInt32(AUDIO_SOUNDBASE_VIA, viabase);
		if (viabase == MACIO_VIA_BASE)
			WriteMacInt8(viabase + AUDIO_SOUNDBASE_PORTA, 0xa5);
		AudioSoundBaseInstall();
		port = AudioSoundBasePort();
		mainbase = AudioStatus.soundbuffer;
		alternate = mainbase - AUDIO_SOUNDBASE_ALTERNATE;
		CHECK(requested == AUDIO_SOUNDBASE_ALTERNATE + AUDIO_SOUNDBASE_BYTES);
		CHECK(mainbase == allocation + AUDIO_SOUNDBASE_ALTERNATE);
		CHECK(ReadMacInt32(0x266) == mainbase);
		if (viabase == MACIO_VIA_BASE) {
			CHECK(ReadMacInt8(port) == 0xad);
			WriteMacInt32(AUDIO_SOUNDBASE_VIA, MACIO_VIA_BASE + 0x6000);
			CHECK(AudioSoundBasePort() == MACIO_VIA_BASE + 0x7e00);
			WriteMacInt32(AUDIO_SOUNDBASE_VIA, viabase);
		} else {
			viabase = ReadMacInt32(AUDIO_SOUNDBASE_VIA);
			CHECK(viabase == allocation + AUDIO_SOUNDBASE_VIAOFFSET);
			CHECK(viabase >= alternate + AUDIO_SOUNDBASE_BYTES);
			CHECK(viabase + AUDIO_SOUNDBASE_VIABYTES <= mainbase);
			CHECK(port == viabase + AUDIO_SOUNDBASE_PORTA);
			CHECK(ReadMacInt8(port) == AUDIO_SOUNDBASE_MAINPAGE);
		}
		for (sample = 0; sample < AUDIO_SOUNDBASE_BYTES; sample++) {
			CHECK(ReadMacInt8(mainbase + sample) == 0x80);
			CHECK(ReadMacInt8(alternate + sample) == 0x80);
		}
		AudioSoundBaseTick();
		CHECK(AudioStatus.soundframes == 0);
		for (sample = 0; sample < AUDIO_SOUNDBASE_SAMPLES; sample++) {
			WriteMacInt8(mainbase + sample * 2, (uint8)sample);
			WriteMacInt8(alternate + sample * 2, (uint8)(255 - sample));
			WriteMacInt8(mainbase + sample * 2 + 1, 0xf1);
			WriteMacInt8(alternate + sample * 2 + 1, 0xe2);
		}
		for (frame = 0; frame < 12; frame++) {
			if (frame & 1)
				WriteMacInt8(port, ReadMacInt8(port) & ~AUDIO_SOUNDBASE_MAINPAGE);
			else
				WriteMacInt8(port, ReadMacInt8(port) | AUDIO_SOUNDBASE_MAINPAGE);
			AudioSoundBaseTick();
			CHECK(AudioStatus.soundframes == frame + 1);
			if (frame & 1)
				CheckFrame(frame, alternate);
			else
				CheckFrame(frame, mainbase);
			CHECK(ReadMacInt32(0x266) == mainbase);
		}
		memset(output, 0, sizeof(output));
		AudioSoundBaseMix(output, sizeof(output), 16, 2, 44100, AUDIO_SOUNDBASE_UNITY);
		CHECK(AudioStatus.soundread > 0);
		CHECK(output[0] == (uint8)((AUDIO_SOUNDBASE_PHASE - 128) & 255));
		CHECK(output[1] == 0);
		CHECK(memcmp(output, output + 2, 2) == 0);
		memset(output, 0, sizeof(output));
		AudioSoundBaseMix(output, sizeof(output), 16, 2, 44100, 0);
		for (sample = 0; sample < sizeof(output); sample++)
			CHECK(output[sample] == 0);
		AudioStatus.soundframes = 0;
		AudioStatus.soundread = 0;
		AudioStatus.soundposition = 0;
		AudioStatus.soundplaying = 0;
		WriteMacInt8(port, AUDIO_SOUNDBASE_MAINPAGE);
		WriteMacInt32(0x266, 0xffffffff);
		AudioSoundBaseTick();
		CHECK(ReadMacInt32(0x266) == mainbase);
		CheckFrame(0, mainbase);
		WriteMacInt32(0x266, RAMBase + RAMSize - AUDIO_SOUNDBASE_BYTES + 1);
		AudioSoundBaseTick();
		CHECK(ReadMacInt32(0x266) == mainbase);
		WriteMacInt32(0x266, RAMBase);
		WriteMacInt8(port, 0);
		frame = AudioStatus.soundframes;
		AudioSoundBaseTick();
		CHECK(AudioStatus.soundframes == frame);
		WriteMacInt32(0x266, mainbase);
		WriteMacInt32(AUDIO_SOUNDBASE_VIA, 0xffffffff);
		AudioSoundBaseTick();
		CheckFrame(frame, mainbase);
		WriteMacInt32(AUDIO_SOUNDBASE_VIA, MACIO_VIA_BASE + MACIO_VIA_SIZE - 1);
		CHECK(AudioSoundBasePort() == 0);
		WriteMacInt32(AUDIO_SOUNDBASE_VIA, viabase);
		WriteMacInt8(port, AUDIO_SOUNDBASE_MAINPAGE);
		AudioStatus.soundframes = AUDIO_SOUNDBASE_RING - 1;
		AudioStatus.soundread = 0;
		AudioSoundBaseTick();
		CHECK(AudioStatus.soundframes == AUDIO_SOUNDBASE_RING - 1);
		AudioStatus.soundread = AUDIO_SOUNDBASE_RING - 2;
		AudioSoundBaseTick();
		CHECK(AudioStatus.soundframes == AUDIO_SOUNDBASE_RING);
		AudioSoundBaseTick();
		CheckFrame(AUDIO_SOUNDBASE_RING, mainbase);
		allocation = 0;
		AudioSoundBaseInstall();
		CHECK(AudioStatus.soundbuffer == 0);
		CHECK(ReadMacInt32(0x266) == mainbase);
		WriteMacInt32(0x266, 0xffffffff);
		frame = AudioStatus.soundframes;
		AudioSoundBaseTick();
		CHECK(AudioStatus.soundframes == frame);
		printf("checks=%d failures=%d\n", checks, failures);
	}

	void CheckFormats(void)
	{
		struct FormatCase {
			int bits;
			int source;
			int input;
			int expected[3];
		};
		FormatCase cases[] = {
			{8, 160, 128, {128, 144, 160}},
			{8, 160, 245, {245, 255, 255}},
			{8, 96, 128, {128, 112, 96}},
			{8, 96, 10, {10, 0, 0}},
			{16, 160, 0, {0, 4096, 8192}},
			{16, 160, 30000, {30000, 32767, 32767}},
			{16, 96, 0, {0, -4096, -8192}},
			{16, 96, -30000, {-30000, -32768, -32768}}
		};
		int frequencies[] = {11025, 22050, 44100, 48000};
		uint8 output[66];
		int test;
		int channels;
		int rate;
		int gain;
		int sample;
		int bytes;
		int value;
		for (test = 0; test < sizeof(cases) / sizeof(cases[0]); test++) {
			for (channels = 1; channels <= 2; channels++) {
				for (rate = 0; rate < sizeof(frequencies) / sizeof(frequencies[0]); rate++) {
					for (gain = 0; gain < 3; gain++) {
						memset(AudioStatus.soundring, cases[test].source, sizeof(AudioStatus.soundring));
						AudioStatus.soundframes = 4;
						AudioStatus.soundread = 0;
						AudioStatus.soundposition = 0;
						AudioStatus.soundplaying = 1;
						bytes = 16 * channels * (cases[test].bits / 8);
						memset(output, 0x5a, sizeof(output));
						for (sample = 0; sample < 16 * channels; sample++) {
							if (cases[test].bits == 16) {
								output[1 + sample * 2] = (uint8)((uint16)cases[test].input >> 8);
								output[2 + sample * 2] = (uint8)cases[test].input;
							} else
								output[1 + sample] = (uint8)cases[test].input;
						}
						AudioSoundBaseMix(output + 1, bytes, cases[test].bits, channels, frequencies[rate], gain * 128);
						for (sample = 0; sample < 16 * channels; sample++) {
							if (cases[test].bits == 16)
								value = (int16)((output[1 + sample * 2] << 8) | output[2 + sample * 2]);
							else
								value = output[1 + sample];
							CHECK(value == cases[test].expected[gain]);
						}
						CHECK(output[0] == 0x5a);
						CHECK(output[1 + bytes] == 0x5a);
						CHECK(AudioStatus.soundread == 0);
						CHECK(AudioStatus.soundframes == 4);
						CHECK(AudioStatus.soundplaying == 1);
					}
				}
			}
		}
		printf("formats cumulative checks=%d failures=%d\n", checks, failures);
	}
};

int main(void)
{
	SoundBaseTest test(0);
	SoundBaseTest shifted(0x100000);
	SoundBaseTest shadow(0);
	SoundBaseTest shiftedshadow(0x100000);
	test.Run(MACIO_VIA_BASE);
	shifted.Run(MACIO_VIA_BASE);
	shadow.Run(0x50f00000);
	shiftedshadow.Run(0x50f00000);
	test.CheckFormats();
	shifted.CheckFormats();
	shadow.CheckFormats();
	shiftedshadow.CheckFormats();
	return test.failures != 0 || shifted.failures != 0 || shadow.failures != 0 || shiftedshadow.failures != 0;
}
