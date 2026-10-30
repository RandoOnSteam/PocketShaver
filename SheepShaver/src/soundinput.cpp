/*
 *  soundinput.cpp - Sound Manager recording (SPB) from the host microphone
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "thunks.h"
#include "audio.h"
#include "soundinput.h"

#define SOUNDINMAXREFS 16
#define SOUNDINFIRSTUNIT 0x4000
#define SOUNDINTICKMS 10
#define SOUNDINUNITY 0x00010000u
#define SOUNDINRATE22K 0x56ee8ba3u
#define SOUNDINRATE44K 0xac440000u
#define SOUNDINDEVICEBUFFER 8192
#define SOUNDINFILEBUFFER 32768
#define SOUNDINSCRATCH 4096
#define SOUNDINTICKTAG 0x5449434bu
#define SOUNDINHANDLED 0xffffffffu
#define SOUNDINGROUP 0x14

#define SPBREFNUM 0
#define SPBCOUNT 4
#define SPBMILLISECONDS 8
#define SPBBUFFERLENGTH 12
#define SPBBUFFERPTR 16
#define SPBCOMPLETION 20
#define SPBINTERRUPT 24
#define SPBERROR 32

#define SISAMPLERATE 0x73726174u
#define SISAMPLESIZE 0x7373697au
#define SINUMBERCHANNELS 0x6368616eu
#define SICOMPRESSIONTYPE 0x636f6d70u
#define SICHANNELAVAILABLE 0x63686176u
#define SISAMPLERATEAVAILABLE 0x73726176u
#define SISAMPLESIZEAVAILABLE 0x73736176u
#define SICOMPRESSIONAVAILABLE 0x636d6176u
#define SICONTINUOUS 0x636f6e74u
#define SIASYNC 0x6173796eu
#define SIINPUTSOURCE 0x736f7572u
#define SIINPUTAVAILABLE 0x696e6176u
#define SIINPUTGAINMIN 0x69676d6eu
#define SIINPUTGAINMAX 0x69676d78u
#define SIAGCONOFF 0x61676320u
#define SIOSTYPEINPUTSOURCE 0x696e7074u
#define SIDEVICECONNECTED 0x64636f6eu
#define SIINPUTSOURCENAMES 0x736e616du
#define SIINPUTGAIN 0x6761696eu
#define SIPLAYTHRUONOFF 0x706c7468u
#define SILEVELMETERONOFF 0x6c6d6574u
#define SIRECORDINGQUALITY 0x7175616cu
#define SITWOSCOMPLEMENTONOFF 0x74776f73u
#define SIOPTIONSDIALOG 0x6f707464u
#define SIHARDWAREBUSY 0x68776273u
#define SIACTIVECHANNELS 0x63686163u
#define SIUSERINTERRUPTPROC 0x75736572u
#define SIDEVICENAME 0x6e616d65u
#define SOUNDINNONE 0x4e4f4e45u
#define SOUNDINRAW 0x72617720u
#define SOUNDINTWOS 0x74776f73u
#define SOUNDINMIC 0x6d696320u
#define SOUNDINBEST 0x62657374u
#define SOUNDINBETTER 0x62657472u
#define SOUNDINGOOD 0x676f6f64u
#define SOUNDINSND 0x736e6420u

#define SIBADSOUNDINDEVICE (-221)
#define SINOBUFFERSPECIFIED (-222)
#define SIBADCOMPRESSION (-223)
#define SIINVALIDSAMPLERATE (-225)
#define SIINVALIDSAMPLESIZE (-226)
#define SIDEVICEBUSY (-227)
#define SIBADDEVICENAME (-228)
#define SIBADREFNUM (-229)
#define SIUNKNOWNINFOTYPE (-231)
#define SIUNKNOWNQUALITY (-232)
#define SOUNDINPARAMERR (-50)
#define SOUNDINMEMFULL (-108)

typedef struct SoundInputJob {
	int active;
	int paused;
	int stoprequested;
	int incallbacks;
	int flushing;
	int tofile;
	int donepending;
	uint32 pb;
	uint32 ref;
	uint32 total;
	uint32 done;
	uint32 fill;
	uint32 buffer;
	uint32 bufferlength;
	uint32 completion;
	uint32 interrupt;
	uint32 fileref;
	int peak;
} SoundInputJob;

typedef struct SoundInput {
	void *capture;
	uint32 captureformat;
	uint32 rate;
	int bits;
	int channels;
	int twos;
	int source;
	uint32 gain;
	int playthrough;
	int meter;
	int agc;
	int meterpeak;
	uint32 refs[SOUNDINMAXREFS];
	uint32 nextref;
	uint32 writeref;
	SoundInputJob job;
	int installed;
	int ingestalt;
	uint32 completionthunk;
	uint32 interruptthunk;
	uint32 tickproc;
	uint32 task;
	int taskinstalled;
	int taskarmed;
	uint32 devicebuffer;
	uint32 filebuffer;
	uint32 writepb;
	uint8 scratch[SOUNDINSCRATCH];
} SoundInput;

static SoundInput *State(void)
{
	return (SoundInput *)AudioStatus.soundinput;
}

static uint32 FrameBytes(SoundInput *si)
{
	return (uint32)(si->channels * si->bits / 8);
}

static double RateHz(SoundInput *si)
{
	return (double)si->rate / 65536.0;
}

static uint32 MillisecondsToBytes(SoundInput *si, uint32 milliseconds)
{
	return (uint32)((double)milliseconds * RateHz(si) / 1000.0) * FrameBytes(si);
}

static uint32 BytesToMilliseconds(SoundInput *si, uint32 bytes)
{
	return (uint32)((double)(bytes / FrameBytes(si)) * 1000.0 / RateHz(si));
}

static uint32 CaptureFormat(SoundInput *si)
{
	return (si->rate >> 16) | ((uint32)si->bits << 20) | ((uint32)si->channels << 26) | ((uint32)si->twos << 29);
}

static void CaptureUpdate(SoundInput *si)
{
	int wanted;
	wanted = si->job.active || (si->meter && si->writeref);
	if (si->capture && (!wanted || si->captureformat != CaptureFormat(si))) {
		AudioCaptureClose(si->capture);
		si->capture = NULL;
	}
	if (wanted && si->capture == NULL) {
		si->captureformat = CaptureFormat(si);
		si->capture = AudioCaptureOpen((int)(si->rate >> 16), si->bits, si->channels, si->twos);
	}
}

static uint32 Capture(SoundInput *si, uint8 *data, uint32 bytes, int *peak)
{
	uint32 got;
	uint32 index;
	int32 value;
	int32 maximum;
	if (si->capture == NULL)
		return 0;
	bytes -= bytes % FrameBytes(si);
	if (bytes == 0)
		return 0;
	got = AudioCaptureRead(si->capture, data, bytes);
	maximum = 0;
	for (index = 0; index < got; index += (uint32)(si->bits / 8)) {
		if (si->bits == 16)
			value = (int16)((data[index] << 8) | data[index + 1]);
		else if (si->twos)
			value = (int32)(int8)data[index] << 8;
		else
			value = ((int32)data[index] - 128) << 8;
		if (si->gain != SOUNDINUNITY) {
			value = (int32)(((int64)value * (int64)si->gain) >> 16);
			if (value > 32767)
				value = 32767;
			if (value < -32768)
				value = -32768;
			if (si->bits == 16) {
				data[index] = (uint8)(value >> 8);
				data[index + 1] = (uint8)value;
			} else if (si->twos)
				data[index] = (uint8)(value >> 8);
			else
				data[index] = (uint8)((value >> 8) + 128);
		}
		if (value < 0)
			value = -value;
		if (value > maximum)
			maximum = value;
	}
	maximum >>= 7;
	if (maximum > 255)
		maximum = 255;
	if (maximum > *peak)
		*peak = maximum;
	if (maximum > si->meterpeak)
		si->meterpeak = maximum;
	return got;
}

static uint32 Produce(SoundInput *si, uint32 address, uint32 bytes)
{
	return Capture(si, Mac2HostAddr(address), bytes, &si->job.peak);
}

static void MeasureLevel(SoundInput *si)
{
	int peak;
	peak = 0;
	while (Capture(si, si->scratch, SOUNDINSCRATCH, &peak) != 0)
		continue;
}

static uint32 SystemAllocate(uint32 size)
{
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = size;
	Execute68kTrap(0xa71e, &r);
	return r.a[0];
}

static uint32 NewHandleClear(uint32 size)
{
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.d[0] = size;
	Execute68kTrap(0xa322, &r);
	if (r.d[0] != 0)
		return 0;
	return r.a[0];
}

static void PutPString(uint32 target, const char *text)
{
	size_t length;
	length = strlen(text);
	WriteMacInt8(target, (uint8)length);
	Host2Mac_memcpy(target + 1, text, length);
}

static void CallCompletion(SoundInput *si, uint32 pb, uint32 routine)
{
	M68kRegisters r;
	if (routine == 0)
		return;
	memset(&r, 0, sizeof(r));
	r.a[0] = pb;
	r.a[1] = routine;
	Execute68k(si->completionthunk, &r);
}

static void CallInterrupt(SoundInput *si, int peak)
{
	M68kRegisters r;
	memset(&r, 0, sizeof(r));
	r.a[0] = si->job.pb;
	r.a[1] = si->job.buffer;
	r.a[2] = si->job.interrupt;
	r.d[0] = (uint32)peak;
	r.d[1] = si->job.fill;
	si->job.incallbacks = 1;
	Execute68k(si->interruptthunk, &r);
	si->job.incallbacks = 0;
}

static int16 FileFlush(SoundInput *si)
{
	M68kRegisters r;
	if (!si->job.tofile || si->job.fill == 0)
		return 0;
	if (si->writepb == 0)
		si->writepb = SystemAllocate(64);
	if (si->writepb == 0)
		return SOUNDINMEMFULL;
	Mac_memset(si->writepb, 0, 64);
	WriteMacInt16(si->writepb + 24, (uint16)si->job.fileref);
	WriteMacInt32(si->writepb + 32, si->job.buffer);
	WriteMacInt32(si->writepb + 36, si->job.fill);
	memset(&r, 0, sizeof(r));
	r.a[0] = si->writepb;
	si->job.flushing = 1;
	Execute68kTrap(0xa003, &r);
	si->job.flushing = 0;
	si->job.fill = 0;
	return (int16)r.d[0];
}

static void FinishJob(SoundInput *si, int16 error)
{
	SoundInputJob job;
	int16 written;
	if (!si->job.active)
		return;
	if (si->job.tofile) {
		written = FileFlush(si);
		if (error == 0)
			error = written;
	}
	job = si->job;
	memset(&si->job, 0, sizeof(si->job));
	CaptureUpdate(si);
	WriteMacInt32(job.pb + SPBCOUNT, job.done);
	WriteMacInt32(job.pb + SPBMILLISECONDS, BytesToMilliseconds(si, job.done));
	WriteMacInt16(job.pb + SPBERROR, (uint16)error);
	CallCompletion(si, job.pb, job.completion);
}

static void TaskPrime(SoundInput *si)
{
	M68kRegisters r;
	if (!si->taskinstalled || si->taskarmed)
		return;
	memset(&r, 0, sizeof(r));
	r.a[0] = si->task;
	r.d[0] = SOUNDINTICKMS;
	Execute68kTrap(0xa05a, &r);
	si->taskarmed = 1;
}

static void TaskInstall(SoundInput *si)
{
	M68kRegisters r;
	if (si->taskinstalled)
		return;
	if (si->task == 0)
		si->task = SystemAllocate(22);
	if (si->task == 0)
		return;
	Mac_memset(si->task, 0, 22);
	WriteMacInt32(si->task + 6, si->tickproc);
	memset(&r, 0, sizeof(r));
	r.a[0] = si->task;
	Execute68kTrap(0xa458, &r);
	si->taskinstalled = 1;
	si->taskarmed = 0;
}

static void Pump(SoundInput *si)
{
	SoundInputJob *job;
	uint32 room;
	uint32 made;
	int finished;
	int peak;
	job = &si->job;
	while (job->active && !job->paused && !job->stoprequested && !job->donepending) {
		room = job->bufferlength - job->fill;
		if (job->total && job->total - job->done < room)
			room = job->total - job->done;
		if (room == 0)
			break;
		made = Produce(si, job->buffer + job->fill, room);
		if (made == 0)
			break;
		job->fill += made;
		job->done += made;
		finished = job->total && job->done >= job->total;
		if (job->tofile) {
			if (finished)
				job->donepending = 1;
			continue;
		}
		if (job->fill >= job->bufferlength || finished) {
			if (job->interrupt) {
				peak = job->peak;
				job->peak = 0;
				CallInterrupt(si, peak);
				if (!job->active)
					return;
			}
			job->fill = 0;
		}
		if (finished) {
			FinishJob(si, 0);
			return;
		}
	}
	if (job->active && job->stoprequested && !job->tofile)
		FinishJob(si, 0);
}

static void FileService(SoundInput *si)
{
	if (!si->job.active || !si->job.tofile || si->job.incallbacks || si->job.flushing)
		return;
	if (si->job.donepending)
		FinishJob(si, 0);
	else if (si->job.fill >= si->job.bufferlength / 2)
		FileFlush(si);
}

static int ValidRef(SoundInput *si, uint32 ref)
{
	int index;
	for (index = 0; index < SOUNDINMAXREFS; index++)
		if (si->refs[index] == ref && ref != 0)
			return 1;
	return 0;
}

static int16 RecordSynchronously(SoundInput *si)
{
	SoundInputJob *job;
	uint64 deadline;
	uint32 room;
	uint32 made;
	uint32 milliseconds;
	int peak;
	job = &si->job;
	milliseconds = 5000;
	if (job->total)
		milliseconds = BytesToMilliseconds(si, job->total);
	deadline = GetTicks_usec() + (uint64)milliseconds * 1000 + 2000000;
	while (job->active && GetTicks_usec() < deadline) {
		room = job->bufferlength - job->fill;
		if (job->total && job->total - job->done < room)
			room = job->total - job->done;
		made = Produce(si, job->buffer + job->fill, room);
		job->fill += made;
		job->done += made;
		if (job->total && job->done >= job->total)
			break;
		if (job->fill >= job->bufferlength) {
			if (job->tofile)
				FileFlush(si);
			else {
				if (job->interrupt) {
					peak = job->peak;
					job->peak = 0;
					CallInterrupt(si, peak);
				}
				job->fill = 0;
			}
		}
		if (made == 0)
			Delay_usec(2000);
	}
	FinishJob(si, 0);
	return 0;
}

static int16 StartRecord(SoundInput *si, uint32 pb, int async, int tofile, uint32 fileref)
{
	SoundInputJob *job;
	uint32 ref;
	uint32 count;
	uint32 milliseconds;
	uint32 buffer;
	uint32 bufferlength;
	if (pb == 0)
		return SOUNDINPARAMERR;
	ref = ReadMacInt32(pb + SPBREFNUM);
	if (!ValidRef(si, ref))
		return SIBADREFNUM;
	if (si->job.active)
		return SIDEVICEBUSY;
	count = ReadMacInt32(pb + SPBCOUNT);
	milliseconds = ReadMacInt32(pb + SPBMILLISECONDS);
	bufferlength = ReadMacInt32(pb + SPBBUFFERLENGTH);
	buffer = ReadMacInt32(pb + SPBBUFFERPTR);
	job = &si->job;
	memset(job, 0, sizeof(*job));
	job->pb = pb;
	job->ref = ref;
	job->completion = ReadMacInt32(pb + SPBCOMPLETION);
	job->interrupt = ReadMacInt32(pb + SPBINTERRUPT);
	job->total = count;
	if (count == 0 && milliseconds != 0)
		job->total = MillisecondsToBytes(si, milliseconds);
	job->tofile = tofile;
	job->fileref = fileref;
	if (tofile) {
		if (si->filebuffer == 0)
			si->filebuffer = SystemAllocate(SOUNDINFILEBUFFER);
		buffer = si->filebuffer;
		bufferlength = SOUNDINFILEBUFFER - SOUNDINFILEBUFFER % FrameBytes(si);
	} else if (job->interrupt && (buffer == 0 || bufferlength == 0)) {
		if (si->devicebuffer == 0)
			si->devicebuffer = SystemAllocate(SOUNDINDEVICEBUFFER);
		buffer = si->devicebuffer;
		bufferlength = (uint32)(RateHz(si) / 16.0) * FrameBytes(si);
		if (bufferlength > SOUNDINDEVICEBUFFER)
			bufferlength = SOUNDINDEVICEBUFFER - SOUNDINDEVICEBUFFER % FrameBytes(si);
		if (bufferlength < FrameBytes(si))
			bufferlength = FrameBytes(si);
	}
	if (bufferlength == 0)
		bufferlength = job->total;
	if (buffer == 0 || bufferlength == 0) {
		memset(job, 0, sizeof(*job));
		if (tofile)
			return SOUNDINMEMFULL;
		return SINOBUFFERSPECIFIED;
	}
	job->buffer = buffer;
	job->bufferlength = bufferlength;
	if (!tofile && !job->interrupt && (job->total == 0 || job->total > bufferlength))
		job->total = bufferlength;
	job->active = 1;
	WriteMacInt16(pb + SPBERROR, 0);
	CaptureUpdate(si);
	if (si->capture)
		AudioCaptureClear(si->capture);
	if (!async)
		return RecordSynchronously(si);
	WriteMacInt16(pb + SPBERROR, 1);
	TaskInstall(si);
	TaskPrime(si);
	return 0;
}

static int16 GetInfo(SoundInput *si, uint32 type, uint32 data)
{
	static const uint32 rates[4] = {0x2b110000u, 0x56220000u, SOUNDINRATE22K, SOUNDINRATE44K};
	static const uint8 names[] = {0x00, 0x01, 0x08, 'B', 'u', 'i', 'l', 't', '-', 'i', 'n'};
	uint32 handle;
	int index;
	if (type == SISAMPLERATE)
		WriteMacInt32(data, si->rate);
	else if (type == SISAMPLESIZE)
		WriteMacInt16(data, (uint16)si->bits);
	else if (type == SINUMBERCHANNELS || type == SIACTIVECHANNELS)
		WriteMacInt16(data, (uint16)si->channels);
	else if (type == SICOMPRESSIONTYPE)
		WriteMacInt32(data, SOUNDINNONE);
	else if (type == SICHANNELAVAILABLE)
		WriteMacInt16(data, 2);
	else if (type == SISAMPLERATEAVAILABLE || type == SISAMPLESIZEAVAILABLE || type == SICOMPRESSIONAVAILABLE) {
		handle = NewHandleClear(16);
		if (handle == 0)
			return SOUNDINMEMFULL;
		if (type == SISAMPLERATEAVAILABLE) {
			for (index = 0; index < 4; index++)
				WriteMacInt32(ReadMacInt32(handle) + index * 4, rates[index]);
			WriteMacInt16(data, 4);
		} else if (type == SISAMPLESIZEAVAILABLE) {
			WriteMacInt16(ReadMacInt32(handle), 8);
			WriteMacInt16(ReadMacInt32(handle) + 2, 16);
			WriteMacInt16(data, 2);
		} else {
			WriteMacInt32(ReadMacInt32(handle), SOUNDINNONE);
			WriteMacInt16(data, 1);
		}
		WriteMacInt32(data + 2, handle);
	} else if (type == SICONTINUOUS || type == SIASYNC || type == SIINPUTAVAILABLE || type == SIDEVICECONNECTED)
		WriteMacInt16(data, 1);
	else if (type == SIINPUTSOURCE)
		WriteMacInt16(data, (uint16)si->source);
	else if (type == SIOSTYPEINPUTSOURCE)
		WriteMacInt32(data, SOUNDINMIC);
	else if (type == SIINPUTSOURCENAMES) {
		handle = NewHandleClear(sizeof(names));
		if (handle == 0)
			return SOUNDINMEMFULL;
		Host2Mac_memcpy(ReadMacInt32(handle), names, sizeof(names));
		WriteMacInt32(data, handle);
	} else if (type == SIINPUTGAIN)
		WriteMacInt32(data, si->gain);
	else if (type == SIINPUTGAINMIN)
		WriteMacInt32(data, 0x00008000);
	else if (type == SIINPUTGAINMAX)
		WriteMacInt32(data, 0x00018000);
	else if (type == SIPLAYTHRUONOFF)
		WriteMacInt16(data, (uint16)si->playthrough);
	else if (type == SIAGCONOFF)
		WriteMacInt16(data, (uint16)si->agc);
	else if (type == SILEVELMETERONOFF) {
		if (si->meter && !si->job.active)
			MeasureLevel(si);
		WriteMacInt16(data, (uint16)si->meter);
		WriteMacInt16(data + 2, 0);
		if (si->meter)
			WriteMacInt16(data + 2, (uint16)si->meterpeak);
		si->meterpeak = 0;
	} else if (type == SITWOSCOMPLEMENTONOFF)
		WriteMacInt16(data, (uint16)si->twos);
	else if (type == SIOPTIONSDIALOG)
		WriteMacInt16(data, 0);
	else if (type == SIHARDWAREBUSY)
		WriteMacInt16(data, (uint16)si->job.active);
	else if (type == SIDEVICENAME)
		PutPString(data, "Built-in");
	else
		return SIUNKNOWNINFOTYPE;
	return 0;
}

static int16 SetInfo(SoundInput *si, uint32 type, uint32 data)
{
	uint32 value;
	value = ReadMacInt32(data);
	if (type == SISAMPLERATE) {
		if (value < (4000u << 16) || value > (48000u << 16))
			return SIINVALIDSAMPLERATE;
		si->rate = value;
	} else if (type == SISAMPLESIZE) {
		if ((value >> 16) != 8 && (value >> 16) != 16)
			return SIINVALIDSAMPLESIZE;
		si->bits = (int)(value >> 16);
	} else if (type == SINUMBERCHANNELS) {
		if ((value >> 16) != 1 && (value >> 16) != 2)
			return SOUNDINPARAMERR;
		si->channels = (int)(value >> 16);
	} else if (type == SICOMPRESSIONTYPE) {
		if (value != SOUNDINNONE && value != SOUNDINRAW && value != SOUNDINTWOS)
			return SIBADCOMPRESSION;
	} else if (type == SIINPUTSOURCE)
		si->source = (int)(value >> 16);
	else if (type == SIINPUTGAIN)
		si->gain = value;
	else if (type == SIPLAYTHRUONOFF)
		si->playthrough = (int)(value >> 16);
	else if (type == SIAGCONOFF)
		si->agc = (value >> 16) != 0;
	else if (type == SILEVELMETERONOFF)
		si->meter = (value >> 16) != 0;
	else if (type == SITWOSCOMPLEMENTONOFF)
		si->twos = (value >> 16) != 0;
	else if (type == SIRECORDINGQUALITY) {
		si->rate = SOUNDINRATE44K;
		si->bits = 16;
		si->channels = 1;
		if (value == SOUNDINBEST)
			si->channels = 2;
		else if (value == SOUNDINGOOD) {
			si->rate = SOUNDINRATE22K;
			si->bits = 8;
		} else if (value != SOUNDINBETTER)
			return SIUNKNOWNQUALITY;
	} else if (type != SIOSTYPEINPUTSOURCE && type != SIUSERINTERRUPTPROC)
		return SIUNKNOWNINFOTYPE;
	return 0;
}

static int NameMatches(uint32 name)
{
	static const char device[] = "built-in";
	uint32 length;
	uint32 index;
	if (name == 0 || ReadMacInt8(name) == 0)
		return 1;
	length = ReadMacInt8(name);
	if (length != sizeof(device) - 1)
		return 0;
	for (index = 0; index < length; index++)
		if ((ReadMacInt8(name + 1 + index) | 0x20) != (uint8)device[index])
			return 0;
	return 1;
}

static int16 OpenDevice(SoundInput *si, uint32 name, int writer, uint32 refpointer)
{
	uint32 ref;
	int slot;
	if (!NameMatches(name))
		return SIBADDEVICENAME;
	if (writer && si->writeref)
		return SIDEVICEBUSY;
	for (slot = 0; slot < SOUNDINMAXREFS && si->refs[slot] != 0; slot++)
		continue;
	if (slot == SOUNDINMAXREFS)
		return SIDEVICEBUSY;
	ref = 0;
	while (ref == 0 || ValidRef(si, ref)) {
		ref = (uint32)(int32)(int16)~(SOUNDINFIRSTUNIT + (si->nextref & 0x0fff));
		si->nextref++;
	}
	si->refs[slot] = ref;
	if (writer)
		si->writeref = ref;
	if (refpointer)
		WriteMacInt32(refpointer, ref);
	return 0;
}

static int16 CloseDevice(SoundInput *si, uint32 ref)
{
	int slot;
	if (!ValidRef(si, ref))
		return SIBADREFNUM;
	if (si->job.active && si->job.ref == ref)
		FinishJob(si, 0);
	for (slot = 0; slot < SOUNDINMAXREFS; slot++)
		if (si->refs[slot] == ref)
			si->refs[slot] = 0;
	if (si->writeref == ref)
		si->writeref = 0;
	return 0;
}

static void RecordingStatus(SoundInput *si, uint32 arguments)
{
	uint32 framebytes;
	framebytes = FrameBytes(si);
	if (ReadMacInt32(arguments + 20)) {
		WriteMacInt16(ReadMacInt32(arguments + 20), 0);
		if (si->job.active && si->job.paused)
			WriteMacInt16(ReadMacInt32(arguments + 20), 2);
		else if (si->job.active)
			WriteMacInt16(ReadMacInt32(arguments + 20), 1);
	}
	if (ReadMacInt32(arguments + 16)) {
		if (!si->job.active && si->meter)
			MeasureLevel(si);
		WriteMacInt16(ReadMacInt32(arguments + 16), (uint16)si->meterpeak);
		si->meterpeak = 0;
	}
	if (ReadMacInt32(arguments + 12))
		WriteMacInt32(ReadMacInt32(arguments + 12), si->job.total / framebytes);
	if (ReadMacInt32(arguments + 8))
		WriteMacInt32(ReadMacInt32(arguments + 8), si->job.done / framebytes);
	if (ReadMacInt32(arguments + 4))
		WriteMacInt32(ReadMacInt32(arguments + 4), BytesToMilliseconds(si, si->job.total));
	if (ReadMacInt32(arguments))
		WriteMacInt32(ReadMacInt32(arguments), BytesToMilliseconds(si, si->job.done));
}

static int SpbCall(SoundInput *si, uint32 selector, uint32 sp, uint32 *pop)
{
	uint32 arguments;
	uint32 pointer;
	int16 error;
	arguments = sp + 4;
	error = 0;
	if (selector == 0x0000) {
		WriteMacInt32(arguments, 0x03108000);
		*pop = 0;
		return 1;
	}
	if (selector == 0x0110)
		*pop = 2;
	else if (selector == 0x030c)
		*pop = 6;
	else if (selector == 0x0514) {
		*pop = 10;
		if ((int16)ReadMacInt16(arguments + 8) != 1)
			error = SIBADSOUNDINDEVICE;
		else {
			if (ReadMacInt32(arguments + 4))
				PutPString(ReadMacInt32(arguments + 4), "Built-in");
			if (ReadMacInt32(arguments))
				WriteMacInt32(ReadMacInt32(arguments), NewHandleClear(256));
		}
	} else if (selector == 0x0518) {
		*pop = 10;
		error = OpenDevice(si, ReadMacInt32(arguments + 6), ReadMacInt16(arguments + 4) != 0, ReadMacInt32(arguments));
	} else if (selector == 0x021c) {
		*pop = 4;
		error = CloseDevice(si, ReadMacInt32(arguments));
	} else if (selector == 0x0320) {
		*pop = 6;
		error = StartRecord(si, ReadMacInt32(arguments + 2), ReadMacInt8(arguments) != 0, 0, 0);
	} else if (selector == 0x0424) {
		*pop = 8;
		error = StartRecord(si, ReadMacInt32(arguments + 2), ReadMacInt8(arguments) != 0, 1, ReadMacInt16(arguments + 6));
	} else if (selector == 0x0228 || selector == 0x022c) {
		*pop = 4;
		if (si->job.active)
			si->job.paused = selector == 0x0228;
		if (selector == 0x022c)
			TaskPrime(si);
	} else if (selector == 0x0230) {
		*pop = 4;
		if (si->job.active && !si->job.tofile && (si->job.incallbacks || !si->taskinstalled))
			si->job.stoprequested = 1;
		else if (si->job.active) {
			Pump(si);
			FinishJob(si, 0);
		}
	} else if (selector == 0x0e34) {
		*pop = 28;
		RecordingStatus(si, arguments);
	} else if (selector == 0x0638 || selector == 0x063c) {
		*pop = 12;
		if (!ValidRef(si, ReadMacInt32(arguments + 8)))
			error = SIBADREFNUM;
		else if (selector == 0x0638)
			error = GetInfo(si, ReadMacInt32(arguments + 4), ReadMacInt32(arguments));
		else
			error = SetInfo(si, ReadMacInt32(arguments + 4), ReadMacInt32(arguments));
	} else if (selector == 0x0440 || selector == 0x0444) {
		*pop = 8;
		pointer = ReadMacInt32(arguments);
		if (!ValidRef(si, ReadMacInt32(arguments + 4)) || pointer == 0)
			error = SIBADREFNUM;
		else if (selector == 0x0440)
			WriteMacInt32(pointer, MillisecondsToBytes(si, ReadMacInt32(pointer)));
		else
			WriteMacInt32(pointer, BytesToMilliseconds(si, ReadMacInt32(pointer)));
	} else
		return 0;
	WriteMacInt16(arguments + *pop, (uint16)error);
	CaptureUpdate(si);
	return 1;
}

static int32 Tick(uint32 *task);

void SoundInputDispatch(M68kRegisters *r)
{
	SoundInput *si;
	uint32 pop;
	si = State();
	if (si == NULL)
		return;
	if (r->d[0] == SOUNDINTICKTAG) {
		r->d[0] = (uint32)Tick(&r->a[0]);
		return;
	}
	if ((r->d[0] & 0xffff) != SOUNDINGROUP)
		return;
	FileService(si);
	pop = 0;
	if (SpbCall(si, (r->d[0] >> 16) & 0xffff, r->a[7] + 60, &pop)) {
		WriteMacInt32(r->a[7], SOUNDINHANDLED);
		WriteMacInt32(r->a[7] + 4, pop);
	}
}

static int32 Tick(uint32 *task)
{
	SoundInput *si;
	si = State();
	if (si == NULL)
		return 0;
	*task = si->task;
	si->taskarmed = 0;
	if (!si->taskinstalled)
		return 0;
	if (!si->job.incallbacks && !si->job.flushing)
		Pump(si);
	if (si->taskarmed || !si->job.active || si->job.donepending)
		return 0;
	si->taskarmed = 1;
	return SOUNDINTICKMS;
}

static void Emit16(uint8 *code, int *count, uint32 value)
{
	code[(*count)++] = (uint8)(value >> 8);
	code[(*count)++] = (uint8)value;
}

static void Emit32(uint8 *code, int *count, uint32 value)
{
	Emit16(code, count, value >> 16);
	Emit16(code, count, value);
}

static void MakeThunks(SoundInput *si)
{
	static const uint8 completion[] = {0x2f, 0x08, 0x4e, 0x91, 0x4e, 0x75};
	static const uint8 interrupt[] = {0x4e, 0x92, 0x4e, 0x75};
	uint8 tick[16];
	int count;
	if (si->completionthunk)
		return;
	count = 0;
	Emit16(tick, &count, 0x203c);
	Emit32(tick, &count, SOUNDINTICKTAG);
	Emit16(tick, &count, M68K_EMUL_OP_SOUNDIN_SPB);
	Emit16(tick, &count, 0x4a80);
	Emit16(tick, &count, 0x6702);
	Emit16(tick, &count, 0xa05a);
	Emit16(tick, &count, 0x4e75);
	si->completionthunk = SheepProc(completion, sizeof(completion));
	si->interruptthunk = SheepProc(interrupt, sizeof(interrupt));
	si->tickproc = SheepProc(tick, (uint32)count);
}

void SoundInputInstall(void)
{
	SoundInput *si;
	M68kRegisters r;
	uint8 code[56];
	uint32 dispatcher;
	uint32 stub;
	int count;
	si = State();
	if (si == NULL || si->installed)
		return;
	si->installed = 1;
	memset(&r, 0, sizeof(r));
	r.d[0] = 0xa800;
	Execute68kTrap(0xa746, &r);
	dispatcher = r.a[0];
	if (ReadMacInt32(dispatcher) != 0x207802b6 || ReadMacInt32(dispatcher + 4) != 0x20680110) {
		printf("Sound input: unexpected Sound Manager dispatcher %08x %08x, not installed\n", ReadMacInt32(dispatcher), ReadMacInt32(dispatcher + 4));
		return;
	}
	MakeThunks(si);
	count = 0;
	Emit32(code, &count, 0x48e7fffe);
	Emit16(code, &count, M68K_EMUL_OP_SOUNDIN_SPB);
	Emit32(code, &count, 0x4cdf7fff);
	Emit16(code, &count, 0x0c80);
	Emit32(code, &count, SOUNDINHANDLED);
	Emit16(code, &count, 0x670e);
	Emit32(code, &count, 0x207802b6);
	Emit32(code, &count, 0x20680110);
	Emit16(code, &count, 0x4ef9);
	Emit32(code, &count, dispatcher + 8);
	Emit16(code, &count, 0x205f);
	Emit16(code, &count, 0xdec1);
	Emit16(code, &count, 0x4ed0);
	stub = SheepProc(code, (uint32)count);
	WriteMacInt16(dispatcher, 0x4ef9);
	WriteMacInt32(dispatcher + 2, stub);
	WriteMacInt16(dispatcher + 6, 0x4e71);
	printf("Sound input: host microphone installed\n");
}

int SoundInputGestalt(uint32 trapword, uint32 selector, uint32 *response)
{
	SoundInput *si;
	M68kRegisters r;
	si = State();
	if (si == NULL || !si->installed || si->ingestalt || (trapword & 0xffff) != 0xa1ad || selector != SOUNDINSND)
		return 0;
	memset(&r, 0, sizeof(r));
	r.d[0] = SOUNDINSND;
	si->ingestalt = 1;
	Execute68kTrap(0xa1ad, &r);
	si->ingestalt = 0;
	if ((int16)r.d[0] != 0)
		return 0;
	*response = r.a[0] | 0x000000f0;
	return 1;
}

void SoundInputReset(void)
{
	SoundInput *si;
	si = State();
	if (si == NULL)
		return;
	if (si->capture)
		AudioCaptureClose(si->capture);
	si->capture = NULL;
	memset(&si->job, 0, sizeof(si->job));
	memset(si->refs, 0, sizeof(si->refs));
	si->writeref = 0;
	si->meter = 0;
	si->installed = 0;
	si->task = 0;
	si->taskinstalled = 0;
	si->taskarmed = 0;
	si->devicebuffer = 0;
	si->filebuffer = 0;
	si->writepb = 0;
}

void SoundInputInit(void)
{
	SoundInput *si;
	if (AudioStatus.soundinput || !PrefsFindBool("mic"))
		return;
	si = (SoundInput *)calloc(1, sizeof(SoundInput));
	if (si == NULL)
		return;
	si->rate = SOUNDINRATE22K;
	si->bits = 8;
	si->channels = 1;
	si->source = 1;
	si->gain = SOUNDINUNITY;
	si->nextref = 1;
	AudioStatus.soundinput = si;
}

void SoundInputExit(void)
{
	SoundInput *si;
	si = State();
	if (si == NULL)
		return;
	if (si->capture)
		AudioCaptureClose(si->capture);
	free(si);
	AudioStatus.soundinput = NULL;
}
