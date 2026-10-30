/*
 *  suitedisk.c - SheepBench disk suite: sequential and random I/O on each writable volume, every read checked
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Files.h>
#include <Devices.h>
#include <Script.h>
#include "bench.h"

#define DISKFILEBYTES (32L * 1024L * 1024L)
#define DISKNOCACHE 0x20
#define DISKSEQWRITE 0
#define DISKSEQREAD 1
#define DISKRANDREAD 2
#define DISKRANDWRITE 3
#define DISKMODES 4
#define DISKSIZES 4
#define DISKMAXVOLUMES 3
#define DISKSMALLBLOCKS 4096

typedef struct DiskContext {
	short file;
	long block;
	long blocks;
	long next;
	uint32_t seed;
	int mode;
	unsigned char *buffer;
	long bad;
} DiskContext;

static uint32_t DiskBase(long index)
{
	return (uint32_t)(index / 128) * 0x01010101UL ^ 0xa5c35a3cUL;
}

static void DiskFillBase(unsigned char *buffer, long size)
{
	uint32_t *words;
	long index;
	words = (uint32_t *)buffer;
	for (index = 0; index < size / 4; index++)
		words[index] = DiskBase(index);
}

static void DiskFill(unsigned char *buffer, long block, long size)
{
	uint32_t *words;
	words = (uint32_t *)buffer;
	words[0] = (uint32_t)block ^ 0x5eed0000UL;
	words[size / 4 - 1] = ~(uint32_t)block;
}

static Boolean DiskMatches(unsigned char *buffer, long block, long size)
{
	uint32_t *words;
	long index;
	long count;
	Boolean ok;
	words = (uint32_t *)buffer;
	count = size / 4;
	ok = words[0] == ((uint32_t)block ^ 0x5eed0000UL) && words[count - 1] == ~(uint32_t)block;
	for (index = 1; index < count - 1 && ok; index += 61)
		ok = words[index] == DiskBase(index);
	words[0] = 0;
	words[count - 1] = 0;
	return ok;
}

static OSErr DiskIo(DiskContext *disk, long block, Boolean write)
{
	ParamBlockRec request;
	memset(&request, 0, sizeof(request));
	request.ioParam.ioRefNum = disk->file;
	request.ioParam.ioBuffer = (Ptr)disk->buffer;
	request.ioParam.ioReqCount = disk->block;
	request.ioParam.ioPosMode = fsFromStart | DISKNOCACHE;
	request.ioParam.ioPosOffset = block * disk->block;
	if (write)
		return PBWriteSync(&request);
	return PBReadSync(&request);
}

static uint32_t DiskBody(void *context, uint32_t iterations)
{
	DiskContext *disk;
	uint32_t index;
	long block;
	disk = (DiskContext *)context;
	for (index = 0; index < iterations; index++) {
		if (disk->mode == DISKSEQWRITE || disk->mode == DISKSEQREAD) {
			block = disk->next;
			disk->next = (disk->next + 1) % disk->blocks;
		} else {
			disk->seed = disk->seed * 1664525UL + 1013904223UL;
			block = (long)((disk->seed >> 8) % (uint32_t)disk->blocks);
		}
		if (disk->mode == DISKSEQWRITE || disk->mode == DISKRANDWRITE) {
			DiskFill(disk->buffer, block, disk->block);
			DiskIo(disk, block, true);
		} else {
			DiskIo(disk, block, false);
			if (!DiskMatches(disk->buffer, block, disk->block))
				disk->bad++;
		}
	}
	return iterations;
}

static void DiskKind(Bench *bench, short drive, char *kind)
{
	const char *text;
	const char *end;
	const char *found;
	char line[200];
	size_t length;
	strcpy(kind, "unknown");
	text = bench->hostinfo;
	while (text && *text) {
		end = strchr(text, '\n');
		length = strlen(text);
		if (end)
			length = (size_t)(end - text);
		if (length >= sizeof(line))
			length = sizeof(line) - 1;
		memcpy(line, text, length);
		line[length] = 0;
		found = strstr(line, "drive=");
		if (found && atoi(found + 6) == drive && (found = strstr(line, "kind=")) != NULL) {
			sscanf(found + 5, "%31s", kind);
			return;
		}
		text = NULL;
		if (end)
			text = end + 1;
	}
}

static void DiskSizeName(char *text, long size)
{
	if (size >= 1024)
		sprintf(text, "%ld KB", size / 1024);
	else
		sprintf(text, "%ld B", size);
}

static void DiskRunMode(Bench *bench, DiskContext *base, const char *label, long size, int mode)
{
	static const char *modes[DISKMODES] = {"sequential write", "sequential read", "random read", "random write"};
	DiskContext disk;
	BenchMeasurement measurement;
	BenchRow *row;
	char sizename[16];
	char name[160];
	char why[100];
	Boolean iops;
	int unit;
	long block;
	iops = (mode == DISKRANDREAD || mode == DISKRANDWRITE) && size <= 4096;
	unit = BENCHUNITMBPERSEC;
	if (iops)
		unit = BENCHUNITPERSEC;
	DiskSizeName(sizename, size);
	snprintf(name, sizeof(name), "%s: %s, %s", label, modes[mode], sizename);
	row = BenchBegin(bench, "disk", name, unit);
	if (row == NULL)
		return;
	disk = *base;
	disk.block = size;
	disk.blocks = DISKFILEBYTES / size;
	disk.next = 0;
	disk.mode = mode;
	disk.seed = 99 + (uint32_t)size;
	disk.bad = 0;
	DiskFillBase(disk.buffer, size);
	if (mode == DISKSEQWRITE) {
		for (block = 0; block < disk.blocks && block < DISKSMALLBLOCKS; block++) {
			DiskFill(disk.buffer, block, size);
			DiskIo(&disk, block, true);
		}
		disk.blocks = block;
	} else if (size < 8192)
		disk.blocks = DISKSMALLBLOCKS;
	BenchMeasure(bench, DiskBody, &disk, 1, &measurement);
	if (disk.bad) {
		snprintf(why, sizeof(why), "%ld reads returned other data than was written", disk.bad);
		BenchFail(bench, row, why);
		return;
	}
	if (iops)
		BenchRecord(bench, row, &measurement, 1);
	else
		BenchRecord(bench, row, &measurement, (double)size);
}

static Boolean DiskLayDown(DiskContext *disk)
{
	long written;
	disk->block = 1048576;
	DiskFillBase(disk->buffer, disk->block);
	for (written = 0; written < DISKFILEBYTES / disk->block; written++) {
		DiskFill(disk->buffer, written, disk->block);
		if (DiskIo(disk, written, true) != noErr)
			return false;
	}
	return true;
}

static void DiskRunVolume(Bench *bench, short volume, const char *volumename, short drive)
{
	static const long sizes[DISKSIZES] = {512, 4096, 65536, 1048576};
	DiskContext disk;
	BenchMemory memory;
	FSSpec spec;
	char kind[32];
	char label[96];
	char name[160];
	char why[100];
	OSErr err;
	int size;
	int mode;
	DiskKind(bench, drive, kind);
	snprintf(label, sizeof(label), "%s (%s)", volumename, kind);
	snprintf(name, sizeof(name), "%s: test file", label);
	FSMakeFSSpec(volume, 2, "\pSheepBench Disk Test", &spec);
	FSpDelete(&spec);
	memset(&disk, 0, sizeof(disk));
	err = FSpCreate(&spec, 'ShBn', 'tmp ', smSystemScript);
	if (err == noErr)
		err = FSpOpenDF(&spec, fsRdWrPerm, &disk.file);
	if (err != noErr) {
		snprintf(why, sizeof(why), "cannot create the test file (error %d)", (int)err);
		BenchSkip(bench, BenchBegin(bench, "disk", name, BENCHUNITMBPERSEC), why);
		return;
	}
	if (BenchAllocate(&memory, 1048576L + 32) == NULL)
		BenchSkip(bench, BenchBegin(bench, "disk", name, BENCHUNITMBPERSEC), "not enough memory");
	else if (SetEOF(disk.file, DISKFILEBYTES) != noErr)
		BenchSkip(bench, BenchBegin(bench, "disk", name, BENCHUNITMBPERSEC), "not enough free space (32 MB)");
	else {
		disk.buffer = (unsigned char *)(((unsigned long)memory.pointer + 31) & ~31UL);
		if (!DiskLayDown(&disk))
			BenchSkip(bench, BenchBegin(bench, "disk", name, BENCHUNITMBPERSEC), "writing the test file failed");
		else {
			for (size = 0; size < DISKSIZES && !bench->cancelled; size++) {
				for (mode = 0; mode < DISKMODES && !bench->cancelled; mode++)
					DiskRunMode(bench, &disk, label, sizes[size], mode);
			}
		}
	}
	FSClose(disk.file);
	FSpDelete(&spec);
	FlushVol(NULL, volume);
	BenchRelease(&memory);
}

void SuiteDisk(Bench *bench)
{
	HParamBlockRec volume;
	Str255 name;
	char volumename[64];
	short index;
	int length;
	int count;
	count = 0;
	BenchHostRefresh(bench);
	for (index = 1; count < DISKMAXVOLUMES && !bench->cancelled; index++) {
		memset(&volume, 0, sizeof(volume));
		volume.volumeParam.ioNamePtr = name;
		volume.volumeParam.ioVolIndex = index;
		if (PBHGetVInfoSync(&volume) != noErr)
			break;
		if (volume.volumeParam.ioVAtrb & 0x8080)
			continue;
		if ((double)volume.volumeParam.ioVFrBlk * (double)volume.volumeParam.ioVAlBlkSiz < 40.0 * 1024 * 1024)
			continue;
		length = name[0];
		if (length > 63)
			length = 63;
		memcpy(volumename, name + 1, length);
		volumename[length] = 0;
		DiskRunVolume(bench, volume.volumeParam.ioVRefNum, volumename, volume.volumeParam.ioVDrvInfo);
		count++;
	}
	if (count == 0)
		BenchSkip(bench, BenchBegin(bench, "disk", "volumes", BENCHUNITMBPERSEC), "no writable volume with 40 MB free");
}
