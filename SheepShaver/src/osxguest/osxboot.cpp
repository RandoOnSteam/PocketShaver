/*
 *  osxboot.cpp - OSX boot mechanics
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
#include "sys.h"
#include "prefs.h"
#include "osxmachine.h"
#include "osxguest.h"
#include "xpram.h"
#include "disk.h"
#include "rom_patches.h"

#define HFSFOLDERRECORD 1
#define HFSFILERECORD 2
#define HFSROOTFOLDER 2
#define HFSMAXEXTENTS 512
#define HFSMAXNAME 255
#define ROMSCANCHUNK (1024 * 1024)
#define ROMSCANHEADER (128 * 1024)
#define ROMSCANLIMIT (8 * 1024 * 1024)
#define BOOTPAGE 4096
#define BOOTMAXRANGES 64
#define BOOTARGSSIZE 0x200
#define CLIPTOOLBOX "/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/HIToolbox"
#define CLIPSYSTEM "/usr/lib/libSystem.B.dylib"
#define CLIPDOCK "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock"
#define CLIPCACHE "/private/var/db/dyld/dyld_shared_cache_ppc"
#define CLIPCACHEIMAGES 4096
#define TREENAMELENGTH 32
#define BOOTMAXKEXTS 1024
#define BOOTKEXTPATH 512
#define BOOTKEXTNAME 128
#define BOOTEXTENSIONS "/System/Library/Extensions"

typedef struct OsxHfsExtent {
	uint32 start;
	uint32 count;
} OsxHfsExtent;

typedef struct OsxHfsFork {
	uint64 logical;
	uint32 totalblocks;
	uint32 fileid;
	uint8 forktype;
	int extentcount;
	OsxHfsExtent extents[HFSMAXEXTENTS];
} OsxHfsFork;

typedef struct OsxKextCandidate {
	char path[BOOTKEXTPATH];
	char identifier[BOOTKEXTNAME];
	char executable[BOOTKEXTNAME];
	uint8 *plist;
	uint32 plistsize;
	int contents;
	int selected;
} OsxKextCandidate;

typedef struct OsxKextList {
	OsxKextCandidate *kexts;
	int count;
	int safeboot;
} OsxKextList;

typedef struct OsxHfsVolume {
	void *file;
	loff_t base;
	int partition;
	uint32 blocksize;
	OsxHfsFork catalog;
	OsxHfsFork overflow;
	uint32 catalognodesize;
	uint32 catalogroot;
	uint32 overflownodesize;
	uint32 overflowfirstleaf;
	uint8 *node;
} OsxHfsVolume;

typedef struct OsxTreeProperty {
	char name[TREENAMELENGTH];
	uint32 length;
	uint8 *value;
	struct OsxTreeProperty *next;
} OsxTreeProperty;

typedef struct OsxTreeNode {
	OsxTreeProperty *firstproperty;
	OsxTreeProperty *lastproperty;
	int propertycount;
	struct OsxTreeNode *firstchild;
	struct OsxTreeNode *lastchild;
	struct OsxTreeNode *next;
	int childcount;
} OsxTreeNode;

typedef struct OsxMemoryRange {
	char name[TREENAMELENGTH];
	uint32 address;
	uint32 length;
} OsxMemoryRange;

typedef struct OsxBootState {
	OsxMachine *machine;
	OsxMemoryRange ranges[BOOTMAXRANGES];
	int rangecount;
	uint32 nextfree;
	uint32 phandle;
	char rootpath[128];
	int ignorendrv;
} OsxBootState;

static uint32 Big32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static uint32 Big16(const uint8 *data)
{
	return ((uint32)data[0] << 8) | data[1];
}

static void StoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static int ReadVolume(OsxHfsVolume *volume, loff_t offset, void *buffer, uint32 length)
{
	return Sys_read(volume->file, buffer, volume->base + offset, length) == length;
}

static void ParseFork(const uint8 *data, uint32 fileid, uint8 forktype, OsxHfsFork *fork)
{
	int index;
	fork->logical = ((uint64)Big32(data) << 32) | Big32(data + 4);
	fork->totalblocks = Big32(data + 12);
	fork->fileid = fileid;
	fork->forktype = forktype;
	fork->extentcount = 0;
	for (index = 0; index < 8; index++) {
		fork->extents[index].start = Big32(data + 16 + index * 8);
		fork->extents[index].count = Big32(data + 20 + index * 8);
		if (fork->extents[index].count)
			fork->extentcount = index + 1;
	}
}

static int ReadFork(OsxHfsVolume *volume, OsxHfsFork *fork, uint64 offset, uint8 *buffer, uint32 length)
{
	uint64 position;
	uint64 extentbytes;
	uint64 inner;
	uint32 take;
	int index;
	position = 0;
	for (index = 0; index < fork->extentcount && length > 0; index++) {
		extentbytes = (uint64)fork->extents[index].count * volume->blocksize;
		if (offset < position + extentbytes) {
			inner = offset - position;
			take = length;
			if ((uint64)take > extentbytes - inner)
				take = (uint32)(extentbytes - inner);
			if (!ReadVolume(volume, (loff_t)fork->extents[index].start * volume->blocksize + (loff_t)inner, buffer, take))
				return 0;
			buffer += take;
			offset += take;
			length -= take;
		}
		position += extentbytes;
	}
	return length == 0;
}

static uint32 BlocksInFork(OsxHfsFork *fork)
{
	uint32 total;
	int index;
	total = 0;
	for (index = 0; index < fork->extentcount; index++)
		total += fork->extents[index].count;
	return total;
}

static void CompleteFork(OsxHfsVolume *volume, OsxHfsFork *fork)
{
	uint32 node;
	uint32 records;
	uint32 record;
	uint32 offset;
	uint32 have;
	uint32 keylength;
	uint32 keystart;
	uint32 index;
	uint8 *data;
	int progress;
	have = BlocksInFork(fork);
	while (have < fork->totalblocks && volume->overflowfirstleaf) {
		progress = 0;
		node = volume->overflowfirstleaf;
		while (node && !progress) {
			if (!ReadFork(volume, &volume->overflow, (uint64)node * volume->overflownodesize, volume->node, volume->overflownodesize))
				return;
			data = volume->node;
			records = Big16(data + 10);
			for (record = 0; record < records && !progress; record++) {
				offset = Big16(data + volume->overflownodesize - 2 * (record + 1));
				keylength = Big16(data + offset);
				if (data[offset + 2] != fork->forktype || Big32(data + offset + 4) != fork->fileid)
					continue;
				keystart = Big32(data + offset + 8);
				if (keystart != have)
					continue;
				for (index = 0; index < 8 && fork->extentcount < HFSMAXEXTENTS; index++) {
					fork->extents[fork->extentcount].start = Big32(data + offset + 2 + keylength + index * 8);
					fork->extents[fork->extentcount].count = Big32(data + offset + 2 + keylength + index * 8 + 4);
					if (fork->extents[fork->extentcount].count)
						fork->extentcount++;
				}
				progress = 1;
			}
			node = Big32(data);
		}
		if (!progress)
			return;
		have = BlocksInFork(fork);
	}
}

static uint32 FoldCharacter(uint32 character)
{
	if (character == 0)
		return 0xffff;
	if (character >= 'A' && character <= 'Z')
		return character + 32;
	return character;
}

static int CompareCatalogKey(const uint8 *key, uint32 parent, const uint16 *name, uint32 namelength)
{
	uint32 keyparent;
	uint32 keynamelength;
	uint32 index;
	uint32 left;
	uint32 right;
	keyparent = Big32(key + 2);
	if (keyparent < parent)
		return -1;
	if (keyparent > parent)
		return 1;
	keynamelength = Big16(key + 6);
	for (index = 0; index < keynamelength && index < namelength; index++) {
		left = FoldCharacter(Big16(key + 8 + index * 2));
		right = FoldCharacter(name[index]);
		if (left < right)
			return -1;
		if (left > right)
			return 1;
	}
	if (keynamelength < namelength)
		return -1;
	if (keynamelength > namelength)
		return 1;
	return 0;
}

static int DescendCatalog(OsxHfsVolume *volume, uint32 parent, const uint16 *name, uint32 namelength)
{
	uint32 node;
	uint32 records;
	uint32 index;
	uint32 offset;
	uint32 child;
	uint32 keylength;
	int depth;
	int kind;
	int found;
	uint8 *data;
	node = volume->catalogroot;
	for (depth = 0; depth < 16; depth++) {
		if (!ReadFork(volume, &volume->catalog, (uint64)node * volume->catalognodesize, volume->node, volume->catalognodesize))
			return 0;
		data = volume->node;
		kind = (int)(signed char)data[8];
		records = Big16(data + 10);
		if (kind == -1)
			return 1;
		if (kind != 0)
			return 0;
		child = 0;
		found = 0;
		for (index = 0; index < records; index++) {
			offset = Big16(data + volume->catalognodesize - 2 * (index + 1));
			if (CompareCatalogKey(data + offset, parent, name, namelength) > 0)
				break;
			keylength = Big16(data + offset);
			child = Big32(data + offset + 2 + keylength);
			found = 1;
		}
		if (!found)
			return 0;
		node = child;
	}
	return 0;
}

static int FindCatalogRecord(OsxHfsVolume *volume, uint32 parent, const char *text, uint8 *record)
{
	uint16 name[HFSMAXNAME];
	uint32 namelength;
	uint32 records;
	uint32 index;
	uint32 offset;
	uint8 *data;
	namelength = 0;
	while (text[namelength] && namelength < HFSMAXNAME) {
		name[namelength] = (uint8)text[namelength];
		namelength++;
	}
	if (!DescendCatalog(volume, parent, name, namelength))
		return 0;
	data = volume->node;
	records = Big16(data + 10);
	for (index = 0; index < records; index++) {
		offset = Big16(data + volume->catalognodesize - 2 * (index + 1));
		if (CompareCatalogKey(data + offset, parent, name, namelength) == 0) {
			memcpy(record, data + offset + 2 + Big16(data + offset), 248);
			return 1;
		}
	}
	return 0;
}

static int ListKextFolders(OsxHfsVolume *volume, uint32 parent, char *names, int maximum)
{
	uint32 records;
	uint32 index;
	uint32 offset;
	uint32 length;
	uint32 character;
	uint32 position;
	int count;
	int ascii;
	uint8 *data;
	uint8 *key;
	count = 0;
	if (!DescendCatalog(volume, parent, NULL, 0))
		return 0;
	for (;;) {
		data = volume->node;
		records = Big16(data + 10);
		for (index = 0; index < records; index++) {
			offset = Big16(data + volume->catalognodesize - 2 * (index + 1));
			key = data + offset;
			if (Big32(key + 2) < parent)
				continue;
			if (Big32(key + 2) > parent)
				return count;
			length = Big16(key + 6);
			if (Big16(key + 2 + Big16(key)) != HFSFOLDERRECORD || length < 6 || length >= BOOTKEXTNAME || count >= maximum)
				continue;
			ascii = 1;
			for (position = 0; position < length; position++) {
				character = Big16(key + 8 + position * 2);
				if (character == 0 || character > 0x7e)
					ascii = 0;
				names[count * BOOTKEXTNAME + position] = (char)character;
			}
			names[count * BOOTKEXTNAME + length] = 0;
			if (ascii && strcmp(names + count * BOOTKEXTNAME + length - 5, ".kext") == 0)
				count++;
		}
		offset = Big32(data);
		if (offset == 0 || !ReadFork(volume, &volume->catalog, (uint64)offset * volume->catalognodesize, volume->node, volume->catalognodesize))
			return count;
	}
}

static int LocateVolume(OsxHfsVolume *volume, void *file, int partition, uint8 *block)
{
	uint32 index;
	uint32 allocationblock;
	uint32 firstblock;
	uint32 embedstart;
	uint32 mapblock;
	int found = 0;
	volume->file = file;
	volume->base = 0;
	volume->partition = 0;
	volume->node = NULL;
	if (Sys_read(file, block, 0, 512) != 512)
		return 0;
	if (block[0] == 'E' && block[1] == 'R') {
		mapblock = Big16(block + 2);
		if (mapblock != 2048)
			mapblock = 512;
		for (index = 1; index < 64; index++) {
			if (Sys_read(file, block, (loff_t)index * mapblock, 512) != 512 || block[0] != 'P' || block[1] != 'M')
				return 0;
			if (strncmp((const char *)block + 48, "Apple_HFS", 32) == 0 &&
				(partition < 0 || partition == (int)index)) {
				volume->base = (loff_t)Big32(block + 8) * mapblock;
				volume->partition = (int)index;
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
	} else if (partition > 0)
		return 0;
	if (!ReadVolume(volume, 1024, block, 512))
		return 0;
	if (block[0] == 'B' && block[1] == 'D') {
		if (Big16(block + 124) != 0x482b)
			return 1;
		allocationblock = Big32(block + 20);
		firstblock = Big16(block + 28);
		embedstart = Big16(block + 126);
		volume->base += (loff_t)firstblock * 512 + (loff_t)embedstart * allocationblock;
		if (!ReadVolume(volume, 1024, block, 512))
			return 0;
	}
	return block[0] == 'H' && (block[1] == '+' || block[1] == 'X');
}

static int OpenVolume(OsxHfsVolume *volume, void *file, int partition)
{
	uint8 block[512];
	if (!LocateVolume(volume, file, partition, block) || block[0] != 'H')
		return 0;
	volume->blocksize = Big32(block + 40);
	ParseFork(block + 0xc0, 3, 0, &volume->overflow);
	ParseFork(block + 0x110, 4, 0, &volume->catalog);
	volume->node = (uint8 *)malloc(65536);
	if (!ReadFork(volume, &volume->overflow, 0, volume->node, 512))
		return 0;
	volume->overflownodesize = Big16(volume->node + 32);
	volume->overflowfirstleaf = Big32(volume->node + 24);
	if (volume->overflow.extentcount == 0)
		volume->overflowfirstleaf = 0;
	if (!ReadFork(volume, &volume->catalog, 0, volume->node, 512))
		return 0;
	volume->catalognodesize = Big16(volume->node + 32);
	volume->catalogroot = Big32(volume->node + 16);
	CompleteFork(volume, &volume->catalog);
	return 1;
}

static void CloseVolume(OsxHfsVolume *volume)
{
	free(volume->node);
}

static int ProbeBootImage(const char *path, int *partition, int *attached)
{
	OsxHfsVolume volume;
	uint8 block[512];
	uint8 record[248];
	void *file;
	int systems = 0;
	uint32 folder;
	*attached = 0;
	if (path[0] == '*')
		path++;
	file = Sys_open(path, true, false);
	if (file == NULL)
		return 0;
	if (SysGetFileSize(file) >= 512)
		*attached = 1;
	if (LocateVolume(&volume, file, *partition, block)) {
		*partition = volume.partition;
		if (block[0] == 'B') {
			if (Big32(block + 92) != 0)
				systems = OSXBOOTCLASSIC;
		} else if (OpenVolume(&volume, file, *partition)) {
			if (FindCatalogRecord(&volume, HFSROOTFOLDER, "mach_kernel", record) &&
				Big16(record) == HFSFILERECORD)
				systems = OSXBOOTNATIVE;
			folder = Big32(block + 92);
			if (folder == 0 && systems == 0)
				folder = Big32(block + 80);
			if (folder != 0 && FindCatalogRecord(&volume, folder, "System", record) &&
				Big16(record) == HFSFILERECORD &&
				FindCatalogRecord(&volume, folder, "Finder", record) && Big16(record) == HFSFILERECORD)
				systems |= OSXBOOTCLASSIC;
		}
	}
	CloseVolume(&volume);
	Sys_close(file);
	return systems;
}

int OsxSelectBootImage(int *diskindex, int *partition)
{
	char device[256];
	char command[256];
	const char *path;
	const char *address;
	const char *bootfrom;
	unsigned long controller;
	int unit;
	int wantedindex = -2147483647;
	int bootdrive;
	int bootdriver;
	int wantedpartition = -1;
	int wantclassic;
	int index;
	int used = 0;
	int candidatepartition;
	int systems;
	int attached;
	int secondary = 0;
	int secondaryindex = -2147483647;
	int secondarypartition = -1;
	int fallback = 0;
	int fallbackindex = -2147483647;
	int fallbackpartition = -1;
	*diskindex = -2147483647;
	*partition = -1;
	OsxNvramReadString(XPRAM, 0, device, sizeof(device));
	OsxNvramReadString(XPRAM, 9, command, sizeof(command));
	wantclassic = OsxNvramWantsClassic(device, command);
	address = strstr(device, "/ata@");
	if (address != NULL) {
		controller = strtoul(address + 5, NULL, 16);
		address = strchr(address + 5, '/');
		if (address != NULL) {
			address = strchr(address, '@');
			if (address != NULL) {
				unit = (int)strtol(address + 1, NULL, 16);
				address = strchr(address, ':');
				if (address != NULL)
					wantedpartition = (int)strtol(address + 1, NULL, 10);
				if (controller == 0x20000 && unit >= 0 && unit <= 1)
					wantedindex = unit;
				else if (controller == 0x21000 && unit == 1)
					wantedindex = 2;
				else if (controller == 0x21000 && unit == 0)
					wantedindex = -1;
			}
		}
	}
	bootdrive = PrefsFindInt32("bootdrive");
	bootdriver = PrefsFindInt32("bootdriver");
	if (bootdrive == 0)
		bootdrive = ((int)XPRAM[0x1378] << 8) | XPRAM[0x1379];
	if (bootdriver == 0)
		bootdriver = (int16)(((uint16)XPRAM[0x137a] << 8) | XPRAM[0x137b]);
	if (wantclassic || device[0] == 0) {
		if (bootdrive > 0 && (bootdriver == 0 || bootdriver == DiskRefNum)) {
			wantedindex = bootdrive - 1;
			if (bootdrive >= DiskFirstDrive)
				wantedindex = bootdrive - DiskFirstDrive;
			wantclassic = 1;
		} else if (bootdriver == -62) {
			wantedindex = -1;
			wantclassic = 1;
		}
	}
	bootfrom = PrefsFindString("osxboot");
	if (PrefsFindInt32("bootdriver") == -62 ||
		(device[0] == 0 && bootfrom != NULL && strcmp(bootfrom, "cdrom") == 0))
		wantedindex = -1;
	for (index = 0; (path = PrefsFindString("disk", index)) != NULL; index++) {
		candidatepartition = -1;
		if (wantedindex == used)
			candidatepartition = wantedpartition;
		systems = ProbeBootImage(path, &candidatepartition, &attached);
		if (used >= 3)
			systems &= OSXBOOTCLASSIC;
		if (systems != 0) {
			if (secondary == 0) {
				secondary = systems;
				if (systems & OSXBOOTNATIVE)
					secondary = OSXBOOTNATIVE;
				secondaryindex = used;
				secondarypartition = candidatepartition;
			}
			if (wantclassic && (wantedindex != used || (systems & OSXBOOTCLASSIC) != 0))
				systems &= OSXBOOTCLASSIC;
			else if (systems & OSXBOOTNATIVE)
				systems = OSXBOOTNATIVE;
			if (systems != 0 && fallback == 0) {
				fallback = systems;
				fallbackindex = used;
				fallbackpartition = candidatepartition;
			}
			if (systems != 0 && wantedindex == used) {
				*diskindex = used;
				*partition = candidatepartition;
				return systems;
			}
		}
		if (attached)
			used++;
	}
	for (index = 0; (path = PrefsFindString("cdrom", index)) != NULL; index++) {
		candidatepartition = -1;
		if (wantedindex == -1 && index == 0)
			candidatepartition = wantedpartition;
		systems = ProbeBootImage(path, &candidatepartition, &attached);
		if (index != 0)
			systems &= OSXBOOTCLASSIC;
		if (wantclassic && (wantedindex != -1 || index != 0 || (systems & OSXBOOTCLASSIC) != 0))
			systems &= OSXBOOTCLASSIC;
		else if (systems & OSXBOOTNATIVE)
			systems = OSXBOOTNATIVE;
		if (systems != 0 && (fallback == 0 || (wantedindex == -1 && index == 0))) {
			fallback = systems;
			fallbackindex = -1 - index;
			fallbackpartition = candidatepartition;
		}
	}
	if (fallback == 0) {
		fallback = secondary;
		fallbackindex = secondaryindex;
		fallbackpartition = secondarypartition;
	}
	*diskindex = fallbackindex;
	*partition = fallbackpartition;
	return fallback;
}

static int LookupPath(OsxHfsVolume *volume, const char *path, uint8 *record)
{
	char component[HFSMAXNAME + 1];
	uint32 parent;
	uint32 length;
	const char *cursor;
	parent = HFSROOTFOLDER;
	cursor = path;
	record[0] = 0;
	record[1] = 0;
	while (*cursor) {
		while (*cursor == '/')
			cursor++;
		length = 0;
		while (cursor[length] && cursor[length] != '/' && length < HFSMAXNAME) {
			component[length] = cursor[length];
			length++;
		}
		component[length] = 0;
		cursor += length;
		if (length == 0)
			break;
		if (!FindCatalogRecord(volume, parent, component, record))
			return 0;
		parent = Big32(record + 8);
	}
	return 1;
}

static uint8 *ReadRecord(OsxHfsVolume *volume, const uint8 *record, uint32 *size)
{
	OsxHfsFork *fork;
	uint8 *data;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	ParseFork(record + 88, Big32(record + 8), 0, fork);
	CompleteFork(volume, fork);
	data = (uint8 *)malloc((size_t)fork->logical + 1);
	if (data == NULL || !ReadFork(volume, fork, 0, data, (uint32)fork->logical)) {
		free(data);
		free(fork);
		return NULL;
	}
	data[fork->logical] = 0;
	*size = (uint32)fork->logical;
	free(fork);
	return data;
}

static uint8 *ReadFile(OsxHfsVolume *volume, const char *path, uint32 *size)
{
	uint8 record[248];
	if (!LookupPath(volume, path, record) || Big16(record) != HFSFILERECORD)
		return NULL;
	return ReadRecord(volume, record, size);
}

static uint8 *ScanForRom(void *file, uint32 *size)
{
	uint8 *chunk;
	uint8 *rom;
	loff_t position;
	loff_t total;
	uint32 got;
	uint32 offset;
	uint32 imageoffset;
	uint32 imagesize;
	chunk = (uint8 *)malloc(ROMSCANCHUNK);
	rom = (uint8 *)malloc(ROMSCANLIMIT + 1);
	total = SysGetFileSize(file);
	for (position = 0; chunk && rom && position < total; position += ROMSCANCHUNK) {
		got = (uint32)Sys_read(file, chunk, position, ROMSCANCHUNK);
		for (offset = 0; offset + 11 <= got; offset += 512) {
			if (memcmp(chunk + offset, "<CHRP-BOOT>", 11) != 0)
				continue;
			rom[Sys_read(file, rom, position + offset, ROMSCANHEADER)] = 0;
			if (!FindChrpPayload(rom, &imageoffset, &imagesize) || imageoffset + imagesize > ROMSCANLIMIT)
				continue;
			*size = (uint32)Sys_read(file, rom, position + offset, imageoffset + imagesize);
			if (*size == imageoffset + imagesize) {
				free(chunk);
				return rom;
			}
		}
	}
	free(chunk);
	free(rom);
	return NULL;
}

static const char *BootImagePath(int diskindex)
{
	const char *path;
	int index;
	int used;
	int partition;
	int attached;
	if (diskindex < 0)
		return PrefsFindString("cdrom", -1 - diskindex);
	used = 0;
	for (index = 0; (path = PrefsFindString("disk", index)) != NULL; index++) {
		partition = -1;
		ProbeBootImage(path, &partition, &attached);
		if (attached && used == diskindex)
			return path;
		if (attached)
			used++;
	}
	return NULL;
}

uint8 *OsxReadClassicRom(uint32 *size)
{
	OsxHfsVolume volume;
	uint8 block[512];
	uint8 record[248];
	const char *path;
	void *file;
	uint8 *rom;
	int diskindex;
	int partition;
	if (!(OsxSelectBootImage(&diskindex, &partition) & OSXBOOTCLASSIC))
		return NULL;
	path = BootImagePath(diskindex);
	if (path == NULL)
		return NULL;
	if (path[0] == '*')
		path++;
	file = Sys_open(path, true, false);
	if (file == NULL)
		return NULL;
	rom = NULL;
	if (OpenVolume(&volume, file, partition) && ReadVolume(&volume, 1024, block, 512) &&
		FindCatalogRecord(&volume, Big32(block + 80), "Mac OS ROM", record) && Big16(record) == HFSFILERECORD)
		rom = ReadRecord(&volume, record, size);
	if (rom == NULL)
		rom = ScanForRom(file, size);
	CloseVolume(&volume);
	Sys_close(file);
	return rom;
}

static int ThinPowerPC(uint8 *data, uint32 *size)
{
	uint32 count;
	uint32 index;
	uint32 offset;
	uint32 length;
	if (data == NULL || *size < 8 || Big32(data) != 0xcafebabe)
		return 1;
	count = Big32(data + 4);
	for (index = 0; index < count && 8 + index * 20 + 20 <= *size; index++) {
		offset = Big32(data + 8 + index * 20 + 8);
		length = Big32(data + 8 + index * 20 + 12);
		if (Big32(data + 8 + index * 20) == 18 && offset <= *size && length <= *size - offset) {
			memmove(data, data + offset, length);
			*size = length;
			return 1;
		}
	}
	return 0;
}

static uint8 *ReadFileRange(OsxHfsVolume *volume, const char *path, uint32 offset, uint32 length)
{
	uint8 record[248];
	OsxHfsFork *fork;
	uint8 *data;
	if (!LookupPath(volume, path, record) || Big16(record) != HFSFILERECORD)
		return NULL;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	ParseFork(record + 88, Big32(record + 8), 0, fork);
	CompleteFork(volume, fork);
	data = NULL;
	if (offset < fork->logical) {
		if ((uint64)length > fork->logical - offset)
			length = (uint32)(fork->logical - offset);
		data = (uint8 *)malloc((size_t)length + 1);
		if (data && !ReadFork(volume, fork, offset, data, length)) {
			free(data);
			data = NULL;
		}
		if (data)
			data[length] = 0;
	}
	free(fork);
	return data;
}

static uint32 CacheBase(OsxHfsVolume *volume, const char *path)
{
	uint8 *header;
	uint8 *images;
	uint8 *paths;
	uint32 imageoffset;
	uint32 imagecount;
	uint32 low;
	uint32 high;
	uint32 offset;
	uint32 address;
	uint32 index;
	header = ReadFileRange(volume, CLIPCACHE, 0, 32);
	if (header == NULL)
		return 0;
	imageoffset = Big32(header + 24);
	imagecount = Big32(header + 28);
	if (memcmp(header, "dyld_v1", 7) != 0 || imagecount == 0 || imagecount > CLIPCACHEIMAGES) {
		free(header);
		return 0;
	}
	free(header);
	images = ReadFileRange(volume, CLIPCACHE, imageoffset, imagecount * 32);
	if (images == NULL)
		return 0;
	low = 0xffffffffu;
	high = 0;
	for (index = 0; index < imagecount; index++) {
		offset = Big32(images + index * 32 + 24);
		if (offset < low)
			low = offset;
		if (offset > high)
			high = offset;
	}
	address = 0;
	paths = ReadFileRange(volume, CLIPCACHE, low, high - low + HFSMAXNAME * 4);
	for (index = 0; paths && index < imagecount && address == 0; index++)
		if (strcmp((const char *)paths + Big32(images + index * 32 + 24) - low, path) == 0)
			address = Big32(images + index * 32 + 4);
	free(paths);
	free(images);
	return address;
}

static uint32 TextCommand(const uint8 *image, uint32 size)
{
	uint32 commands;
	uint32 position;
	uint32 index;
	commands = Big32(image + 16);
	position = 28;
	for (index = 0; index < commands && position + 32 <= size; index++) {
		if (Big32(image + position) == 1 && strcmp((const char *)image + position + 8, "__TEXT") == 0)
			return position;
		position += Big32(image + position + 4);
	}
	return 0;
}

static void FindSymbols(const uint8 *image, uint32 size, uint32 base, const char *const *names, int count, uint32 *values, uint32 *textlow, uint32 *texthigh)
{
	uint32 commands;
	uint32 position;
	uint32 text;
	uint32 slide;
	uint32 symbols;
	uint32 symbolcount;
	uint32 strings;
	uint32 name;
	uint32 index;
	int wanted;
	if (size < 28 || Big32(image) != 0xfeedface)
		return;
	text = TextCommand(image, size);
	slide = 0;
	if (base && text)
		slide = base - Big32(image + text + 24);
	if (text && textlow) {
		*textlow = Big32(image + text + 24) + slide;
		*texthigh = *textlow + Big32(image + text + 28);
	}
	commands = Big32(image + 16);
	position = 28;
	for (index = 0; index < commands && position + 24 <= size; index++) {
		if (Big32(image + position) == 2) {
			symbols = Big32(image + position + 8);
			symbolcount = Big32(image + position + 12);
			strings = Big32(image + position + 16);
			for (; symbolcount > 0 && symbols + 12 <= size; symbolcount--, symbols += 12) {
				name = strings + Big32(image + symbols);
				if ((image[symbols + 4] & 0x0f) != 0x0f || name >= size)
					continue;
				for (wanted = 0; wanted < count; wanted++)
					if (values[wanted] == 0 && strcmp((const char *)image + name, names[wanted]) == 0)
						values[wanted] = Big32(image + symbols + 8) + slide;
			}
		}
		position += Big32(image + position + 4);
	}
}

static void FindClipboardFunctions(OsxMachine *machine, OsxHfsVolume *volume)
{
	static const char *const systemnames[3] = { "_malloc", "_free", "_bzero" };
	static const char *const toolboxnames[5] = {
		"_GetCurrentScrap", "_ClearCurrentScrap", "_PutScrapFlavor", "_GetScrapFlavorSize", "_GetScrapFlavorData"
	};
	OsxClip *clip;
	uint8 *image;
	uint32 size;
	int index;
	clip = &machine->clip;
	free(clip->pending);
	memset(clip, 0, sizeof(OsxClip));
	image = ReadFile(volume, CLIPSYSTEM, &size);
	if (image && ThinPowerPC(image, &size))
		FindSymbols(image, size, CacheBase(volume, CLIPSYSTEM), systemnames, 3, clip->functions + OSXCLIPMALLOC, NULL, NULL);
	free(image);
	image = ReadFile(volume, CLIPTOOLBOX, &size);
	if (image && ThinPowerPC(image, &size))
		FindSymbols(image, size, CacheBase(volume, CLIPTOOLBOX), toolboxnames, 5, clip->functions + OSXCLIPGETCURRENT, &clip->textlow, &clip->texthigh);
	free(image);
	image = ReadFile(volume, CLIPDOCK, &size);
	if (image && ThinPowerPC(image, &size) && size >= 28 && Big32(image) == 0xfeedface && TextCommand(image, size)) {
		clip->functions[OSXCLIPDOCK] = Big32(image + TextCommand(image, size) + 24);
		clip->functions[OSXCLIPDOCKCOMMANDS] = Big32(image + 20);
	}
	free(image);
	clip->ready = clip->texthigh > clip->textlow;
	for (index = 0; index < OSXCLIPFUNCTIONS; index++)
		if (clip->functions[index] == 0)
			clip->ready = 0;
	if (machine->log)
		fprintf(machine->log, "clipboard ready %d toolbox %08x-%08x scrap %08x malloc %08x dock %08x\n", clip->ready, clip->textlow, clip->texthigh, clip->functions[OSXCLIPGETCURRENT], clip->functions[OSXCLIPMALLOC], clip->functions[OSXCLIPDOCK]);
}

static int PlistString(const uint8 *plist, const char *key, char *output, uint32 size)
{
	char pattern[BOOTKEXTNAME];
	const char *found;
	const char *end;
	sprintf(pattern, "<key>%s</key>", key);
	found = strstr((const char *)plist, pattern);
	if (found == NULL)
		return 0;
	found += strlen(pattern);
	while (*found == ' ' || *found == '\t' || *found == '\r' || *found == '\n')
		found++;
	if (strncmp(found, "<string>", 8) != 0)
		return 0;
	found += 8;
	end = strstr(found, "</string>");
	if (end == NULL || (uint32)(end - found) >= size)
		return 0;
	memcpy(output, found, end - found);
	output[end - found] = 0;
	return 1;
}

static void CollectKexts(OsxHfsVolume *volume, const char *folder, OsxKextList *list, int depth)
{
	uint8 record[248];
	char path[BOOTKEXTPATH];
	char required[BOOTKEXTNAME];
	char *names;
	OsxKextCandidate *kext;
	int count;
	int index;
	if (depth > 3 || !LookupPath(volume, folder, record) || Big16(record) != HFSFOLDERRECORD)
		return;
	names = (char *)malloc(BOOTMAXKEXTS * BOOTKEXTNAME);
	count = ListKextFolders(volume, Big32(record + 8), names, BOOTMAXKEXTS);
	for (index = 0; index < count && list->count < BOOTMAXKEXTS; index++) {
		kext = &list->kexts[list->count];
		kext->executable[0] = 0;
		if (strlen(folder) + strlen(names + index * BOOTKEXTNAME) + 32 >= BOOTKEXTPATH)
			continue;
		sprintf(kext->path, "%s/%s", folder, names + index * BOOTKEXTNAME);
		sprintf(path, "%s/Contents/Info.plist", kext->path);
		kext->plist = ReadFile(volume, path, &kext->plistsize);
		kext->contents = kext->plist != NULL;
		if (kext->plist == NULL) {
			sprintf(path, "%s/Info.plist", kext->path);
			kext->plist = ReadFile(volume, path, &kext->plistsize);
		}
		if (kext->plist == NULL || !PlistString(kext->plist, "CFBundleIdentifier", kext->identifier, BOOTKEXTNAME)) {
			free(kext->plist);
			continue;
		}
		PlistString(kext->plist, "CFBundleExecutable", kext->executable, BOOTKEXTNAME);
		kext->selected = PlistString(kext->plist, "OSBundleRequired", required, BOOTKEXTNAME) && (list->safeboot || strcmp(required, "Safe Boot") != 0);
		list->count++;
		sprintf(path, "%s/PlugIns", kext->path);
		if (kext->contents)
			sprintf(path, "%s/Contents/PlugIns", kext->path);
		CollectKexts(volume, path, list, depth + 1);
	}
	free(names);
}

static int SelectLibraries(OsxKextList *list, OsxKextCandidate *kext)
{
	char identifier[BOOTKEXTNAME];
	const char *cursor;
	const char *end;
	const char *name;
	int changed;
	int index;
	changed = 0;
	cursor = strstr((const char *)kext->plist, "<key>OSBundleLibraries</key>");
	if (cursor == NULL)
		return 0;
	end = strstr(cursor, "</dict>");
	cursor = strstr(cursor, "<dict>");
	while (cursor != NULL && end != NULL && (cursor = strstr(cursor, "<key>")) != NULL && cursor < end) {
		cursor += 5;
		name = strstr(cursor, "</key>");
		if (name == NULL || name - cursor >= BOOTKEXTNAME)
			return changed;
		memcpy(identifier, cursor, name - cursor);
		identifier[name - cursor] = 0;
		for (index = 0; index < list->count; index++)
			if (!list->kexts[index].selected && strcmp(list->kexts[index].identifier, identifier) == 0) {
				list->kexts[index].selected = 1;
				changed = 1;
			}
	}
	return changed;
}

static int ContainsBytes(const uint8 *data, uint32 size, const char *pattern)
{
	uint32 length;
	uint32 index;
	length = (uint32)strlen(pattern);
	for (index = 0; data != NULL && index + length <= size; index++)
		if (data[index] == (uint8)pattern[0] && memcmp(data + index, pattern, length) == 0)
			return 1;
	return 0;
}

static void UseBootFramebuffer(OsxKextCandidate *kext)
{
	char *found;
	found = (char *)kext->plist;
	while ((found = strstr(found, "<string>IONDRVFramebuffer</string>")) != NULL)
		memcpy(found + 8, "IOBoot", 6);
}

static uint32 LiteralLength(uint32 length)
{
	return length + (length + 7) / 8;
}

static uint8 *StoreEntry(uint8 *package, uint8 *field, uint32 position, const uint8 *data, uint32 length)
{
	uint8 *output;
	uint32 index;
	StoreBig32(field, position);
	StoreBig32(field + 4, LiteralLength(length));
	StoreBig32(field + 8, length);
	output = package + position;
	for (index = 0; index < length; index++) {
		if ((index & 7) == 0)
			*output++ = 0xff;
		*output++ = data[index];
	}
	return output;
}

static uint8 *BuildDriverPackage(OsxHfsVolume *volume, int safeboot, const char *const *only, int onlycount, uint32 *size)
{
	OsxKextList list;
	OsxKextCandidate *kext;
	char path[BOOTKEXTPATH];
	uint8 **modules;
	uint32 *modulesizes;
	uint8 *package;
	uint8 *entry;
	uint32 total;
	uint32 position;
	int selected;
	int changed;
	int index;
	list.kexts = (OsxKextCandidate *)malloc(BOOTMAXKEXTS * sizeof(OsxKextCandidate));
	list.count = 0;
	list.safeboot = safeboot;
	CollectKexts(volume, BOOTEXTENSIONS, &list, 0);
	for (index = 0; only && index < list.count; index++) {
		list.kexts[index].selected = 0;
		for (selected = 0; selected < onlycount; selected++)
			if (strcmp(list.kexts[index].identifier, only[selected]) == 0)
				list.kexts[index].selected = 1;
	}
	do {
		changed = 0;
		for (index = 0; index < list.count; index++)
			if (list.kexts[index].selected && SelectLibraries(&list, &list.kexts[index]))
				changed = 1;
	} while (changed);
	modules = (uint8 **)calloc(list.count + 1, sizeof(uint8 *));
	modulesizes = (uint32 *)calloc(list.count + 1, sizeof(uint32));
	total = 32;
	selected = 0;
	for (index = 0; index < list.count; index++) {
		kext = &list.kexts[index];
		if (!kext->selected)
			continue;
		if (kext->executable[0]) {
			sprintf(path, "%s/%s", kext->path, kext->executable);
			if (kext->contents)
				sprintf(path, "%s/Contents/MacOS/%s", kext->path, kext->executable);
			modules[index] = ReadFile(volume, path, &modulesizes[index]);
			if (!ThinPowerPC(modules[index], &modulesizes[index])) {
				free(modules[index]);
				modules[index] = NULL;
				modulesizes[index] = 0;
			}
		}
		if (strcmp(kext->identifier, "com.apple.iokit.IONDRVSupport") == 0 && !ContainsBytes(modules[index], modulesizes[index], "IOBootNDRV"))
			UseBootFramebuffer(kext);
		total += 32 + LiteralLength(kext->plistsize + 1) + LiteralLength(modulesizes[index]);
		selected++;
	}
	package = NULL;
	if (selected) {
		package = (uint8 *)calloc(total, 1);
		StoreBig32(package, 0x4d4b5854);
		StoreBig32(package + 4, 0x4d4f5358);
		StoreBig32(package + 8, total);
		StoreBig32(package + 16, 0x01008000);
		StoreBig32(package + 20, (uint32)selected);
		StoreBig32(package + 24, 0xffffffff);
		StoreBig32(package + 28, 0xffffffff);
		entry = package + 32;
		position = 32 + 32 * (uint32)selected;
		for (index = 0; index < list.count; index++) {
			kext = &list.kexts[index];
			if (!kext->selected)
				continue;
			position = (uint32)(StoreEntry(package, entry, position, kext->plist, kext->plistsize + 1) - package);
			if (modules[index])
				position = (uint32)(StoreEntry(package, entry + 16, position, modules[index], modulesizes[index]) - package);
			entry += 32;
		}
		StoreBig32(package + 12, OsxAdler32(package + 16, total - 16));
		*size = total;
	}
	for (index = 0; index < list.count; index++) {
		free(list.kexts[index].plist);
		free(modules[index]);
	}
	free(modules);
	free(modulesizes);
	free(list.kexts);
	return package;
}

static void AddRange(OsxBootState *state, const char *name, uint32 address, uint32 length)
{
	OsxMemoryRange *range;
	if (state->rangecount >= BOOTMAXRANGES)
		return;
	range = &state->ranges[state->rangecount++];
	strncpy(range->name, name, TREENAMELENGTH - 1);
	range->address = address;
	range->length = length;
}

static uint32 Allocate(OsxBootState *state, uint32 length)
{
	uint32 address;
	address = state->nextfree;
	state->nextfree = (address + length + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	return address;
}

static int LoadKernel(OsxBootState *state, const uint8 *image, uint32 size)
{
	OsxMachine *machine;
	char rangename[TREENAMELENGTH];
	char segment[17];
	uint32 commands;
	uint32 offset;
	uint32 command;
	uint32 commandsize;
	uint32 vmaddr;
	uint32 vmsize;
	uint32 fileoffset;
	uint32 filesize;
	uint32 index;
	uint32 symoffset;
	uint32 symcount;
	uint32 stroffset;
	uint32 strsize;
	uint32 address;
	uint32 total;
	uint32 kernelend;
	uint8 *target;
	machine = state->machine;
	if (size < 28 || Big32(image) != 0xfeedface)
		return 0;
	commands = Big32(image + 16);
	offset = 28;
	kernelend = 0;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		command = Big32(image + offset);
		commandsize = Big32(image + offset + 4);
		if (command == 1) {
			memcpy(segment, image + offset + 8, 16);
			segment[16] = 0;
			vmaddr = Big32(image + offset + 24);
			vmsize = Big32(image + offset + 28);
			fileoffset = Big32(image + offset + 32);
			filesize = Big32(image + offset + 36);
			if (vmsize && vmaddr + vmsize <= machine->ramsize) {
				target = machine->ram + vmaddr;
				if (filesize && fileoffset + filesize <= size)
					memcpy(target, image + fileoffset, filesize);
				if (filesize < vmsize)
					memset(target + filesize, 0, vmsize - filesize);
				sprintf(rangename, "Kernel-%s", segment);
				AddRange(state, rangename, vmaddr, vmsize);
				if (vmaddr + vmsize > kernelend)
					kernelend = vmaddr + vmsize;
			}
		} else if (command == 5) {
			machine->kernelentry = Big32(image + offset + 16);
		} else if (command == 2) {
			symoffset = Big32(image + offset + 8);
			symcount = Big32(image + offset + 12);
			stroffset = Big32(image + offset + 16);
			strsize = Big32(image + offset + 20);
		}
		offset += commandsize;
	}
	state->nextfree = (kernelend + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	total = 28 + Big32(image + 20);
	address = Allocate(state, total);
	memcpy(machine->ram + address, image, total);
	AddRange(state, "Kernel-__HEADER", address, total);
	if (symcount && symoffset + symcount * 12 <= size && stroffset + strsize <= size) {
		total = 24 + symcount * 12 + strsize;
		address = Allocate(state, total);
		target = machine->ram + address;
		StoreBig32(target, 2);
		StoreBig32(target + 4, 24);
		StoreBig32(target + 8, address + 24);
		StoreBig32(target + 12, symcount);
		StoreBig32(target + 16, address + 24 + symcount * 12);
		StoreBig32(target + 20, strsize);
		memcpy(target + 24, image + symoffset, symcount * 12);
		memcpy(target + 24 + symcount * 12, image + stroffset, strsize);
		AddRange(state, "Kernel-__SYMTAB", address, total);
		machine->kernelsize = 52 + symcount * 12 + strsize;
		machine->kernelimage = (uint8 *)malloc(machine->kernelsize);
		if (machine->kernelimage == NULL)
			return 0;
		target = machine->kernelimage;
		memcpy(target, image, 28);
		StoreBig32(target + 16, 1);
		StoreBig32(target + 20, 24);
		StoreBig32(target + 28, 2);
		StoreBig32(target + 32, 24);
		StoreBig32(target + 36, 52);
		StoreBig32(target + 40, symcount);
		StoreBig32(target + 44, 52 + symcount * 12);
		StoreBig32(target + 48, strsize);
		memcpy(target + 52, image + symoffset, symcount * 12);
		memcpy(target + 52 + symcount * 12, image + stroffset, strsize);
	}
	return machine->kernelentry != 0;
}

uint32 OsxFindKernelSymbol(OsxMachine *machine, const char *name)
{
	const uint8 *image;
	uint32 size;
	uint32 commands;
	uint32 offset;
	uint32 index;
	uint32 symoffset;
	uint32 symcount;
	uint32 stroffset;
	uint32 strsize;
	uint32 symbol;
	uint32 stringindex;
	image = machine->kernelimage;
	size = machine->kernelsize;
	if (image == NULL || size < 28)
		return 0;
	commands = Big32(image + 16);
	offset = 28;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		if (Big32(image + offset) == 2) {
			symoffset = Big32(image + offset + 8);
			symcount = Big32(image + offset + 12);
			stroffset = Big32(image + offset + 16);
			strsize = Big32(image + offset + 20);
		}
		offset += Big32(image + offset + 4);
	}
	for (symbol = 0; symbol < symcount; symbol++) {
		stringindex = Big32(image + symoffset + symbol * 12);
		if (stringindex < strsize && strcmp((const char *)image + stroffset + stringindex, name) == 0)
			return Big32(image + symoffset + symbol * 12 + 8);
	}
	return 0;
}

static OsxTreeNode *NewNode(OsxTreeNode *parent, const char *name)
{
	OsxTreeNode *node;
	OsxTreeProperty *property;
	node = (OsxTreeNode *)calloc(1, sizeof(OsxTreeNode));
	property = (OsxTreeProperty *)calloc(1, sizeof(OsxTreeProperty));
	strcpy(property->name, "name");
	property->length = (uint32)strlen(name) + 1;
	property->value = (uint8 *)malloc(property->length);
	memcpy(property->value, name, property->length);
	node->firstproperty = property;
	node->lastproperty = property;
	node->propertycount = 1;
	if (parent) {
		if (parent->lastchild)
			parent->lastchild->next = node;
		else
			parent->firstchild = node;
		parent->lastchild = node;
		parent->childcount++;
	}
	return node;
}

static void AddProperty(OsxTreeNode *node, const char *name, const void *value, uint32 length)
{
	OsxTreeProperty *property;
	property = (OsxTreeProperty *)calloc(1, sizeof(OsxTreeProperty));
	strncpy(property->name, name, TREENAMELENGTH - 1);
	property->length = length;
	property->value = (uint8 *)malloc(length + 1);
	if (length)
		memcpy(property->value, value, length);
	node->lastproperty->next = property;
	node->lastproperty = property;
	node->propertycount++;
}

static void AddString(OsxTreeNode *node, const char *name, const char *value)
{
	AddProperty(node, name, value, (uint32)strlen(value) + 1);
}

static void AddStrings(OsxTreeNode *node, const char *name, const char *first, const char *second)
{
	char buffer[256];
	uint32 length;
	length = (uint32)strlen(first) + 1;
	memcpy(buffer, first, length);
	memcpy(buffer + length, second, strlen(second) + 1);
	length += (uint32)strlen(second) + 1;
	AddProperty(node, name, buffer, length);
}

static void AddCells(OsxTreeNode *node, const char *name, const uint32 *cells, int count)
{
	uint8 buffer[256];
	int index;
	for (index = 0; index < count; index++)
		StoreBig32(buffer + index * 4, cells[index]);
	AddProperty(node, name, buffer, (uint32)count * 4);
}

static void AddCell(OsxTreeNode *node, const char *name, uint32 value)
{
	AddCells(node, name, &value, 1);
}

static OsxTreeProperty *FindProperty(OsxTreeNode *node, const char *name)
{
	OsxTreeProperty *property;
	for (property = node->firstproperty; property; property = property->next)
		if (strcmp(property->name, name) == 0)
			return property;
	return NULL;
}

static void AddPhandles(OsxTreeNode *node, uint32 *phandle)
{
	OsxTreeNode *child;
#if OSXG5
	if (FindProperty(node, "AAPL,phandle") == NULL)
#endif
	AddCell(node, "AAPL,phandle", (*phandle)++);
	for (child = node->firstchild; child; child = child->next)
		AddPhandles(child, phandle);
}

static uint32 TreeSize(OsxTreeNode *node)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 size;
	size = 8;
	for (property = node->firstproperty; property; property = property->next)
		size += TREENAMELENGTH + 4 + ((property->length + 3) & ~3u);
	for (child = node->firstchild; child; child = child->next)
		size += TreeSize(child);
	return size;
}

static uint8 *FlattenTree(OsxTreeNode *node, uint8 *output)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 padded;
	StoreBig32(output, (uint32)node->propertycount);
	StoreBig32(output + 4, (uint32)node->childcount);
	output += 8;
	for (property = node->firstproperty; property; property = property->next) {
		memset(output, 0, TREENAMELENGTH);
		memcpy(output, property->name, strlen(property->name));
		StoreBig32(output + TREENAMELENGTH, property->length);
		output += TREENAMELENGTH + 4;
		padded = (property->length + 3) & ~3u;
		memset(output, 0, padded);
		if (property->length)
			memcpy(output, property->value, property->length);
		output += padded;
	}
	for (child = node->firstchild; child; child = child->next)
		output = FlattenTree(child, output);
	return output;
}

static void DestroyTree(OsxTreeNode *node)
{
	OsxTreeProperty *property;
	OsxTreeProperty *nextproperty;
	OsxTreeNode *child;
	OsxTreeNode *nextchild;
	for (property = node->firstproperty; property; property = nextproperty) {
		nextproperty = property->next;
		free(property->value);
		free(property);
	}
	for (child = node->firstchild; child; child = nextchild) {
		nextchild = child->next;
		DestroyTree(child);
	}
	free(node);
}

static void AddAtaNode(OsxTreeNode *macio, const char *name, uint32 offset, uint32 dmaoffset, uint32 irq, uint32 dmairq)
{
	OsxTreeNode *node;
	uint32 cells[4];
	node = NewNode(macio, name);
	AddString(node, "device_type", "ata");
	AddString(node, "compatible", "heathrow-ata");
	AddString(node, "model", "ata-3");
	cells[0] = offset;
	cells[1] = 0x1000;
	cells[2] = dmaoffset;
	cells[3] = 0x100;
	AddCells(node, "reg", cells, 4);
	cells[0] = irq;
	cells[1] = dmairq;
	AddCells(node, "AAPL,interrupts", cells, 2);
	AddCells(node, "interrupts", cells, 2);
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
}

static void AddBmacNode(OsxTreeNode *macio)
{
	OsxTreeNode *node;
	uint32 cells[6];
	uint8 address[6];
	node = NewNode(macio, "bmac");
	AddString(node, "device_type", "network");
	AddString(node, "compatible", "bmac+");
	cells[0] = OSXBMACOFFSET;
	cells[1] = OSXBMACSIZE;
	cells[2] = 0x8000 + OSXDBDMABMACTX * 0x100;
	cells[3] = 0x100;
	cells[4] = 0x8000 + OSXDBDMABMACRX * 0x100;
	cells[5] = 0x100;
	AddCells(node, "reg", cells, 6);
	cells[0] = OSXIRQBMAC;
	cells[1] = OSXIRQBMACTXDMA;
	cells[2] = OSXIRQBMACRXDMA;
	AddCells(node, "AAPL,interrupts", cells, 3);
	AddCells(node, "interrupts", cells, 3);
	OsxBmacHardwareAddress(address);
	AddProperty(node, "local-mac-address", address, sizeof(address));
}

static void AddAudioNode(OsxTreeNode *macio)
{
	OsxTreeNode *node;
	uint32 cells[6];
	const char objects[] =
		"detect bit-mask 4 bit-match 4 device 16 index 0 model InSenseBitsDetect\0"
		"detect bit-mask 8 bit-match 0 device 32 index 1 model InSenseBitsDetect\0"
		"detect bit-mask 1 bit-match 1 device 2 index 2 model InSenseBitsDetect\0"
		"input icon-id -16526 index 0 name-id -20520 port-connection 6 port-type 0x656D6963 zero-gain 0 model ExternalMic\0"
		"input index 1 model NoInput\0"
		"output device-mask 6 device-match 0 icon-id -16563 index 0 name-id -20525 port-connection 2 port-type 0x6973706B model OutputPort\0"
		"output device-mask 2 device-match 2 icon-id -16563 index 1 name-id -20524 port-connection 1 port-type 0x6864706E model OutputPort\0";
	node = NewNode(macio, "awacs");
	AddString(node, "device_type", "sound");
	AddString(node, "compatible", "awacs");
	cells[0] = OSXAUDIOOFFSET;
	cells[1] = OSXAUDIOSIZE;
	cells[2] = 0x8000 + OSXDBDMAAUDIOOUT * 0x100;
	cells[3] = 0x100;
	cells[4] = 0x8000 + OSXDBDMAAUDIOIN * 0x100;
	cells[5] = 0x100;
	AddCells(node, "reg", cells, 6);
	cells[0] = OSXIRQAUDIO;
	cells[1] = OSXIRQAUDIOOUTDMA;
	cells[2] = OSXIRQAUDIOINDMA;
	AddCells(node, "AAPL,interrupts", cells, 3);
	AddCells(node, "interrupts", cells, 3);
	node = NewNode(node, "sound");
	AddString(node, "device_type", "sound");
	AddProperty(node, "model", "343S0184", 8);
	AddCell(node, "#-detects", 3);
	AddCell(node, "#-features", 0);
	AddCell(node, "#-inputs", 2);
	AddCell(node, "#-outputs", 2);
	AddCell(node, "device-id", 0);
	AddProperty(node, "sound-objects", objects, sizeof(objects));
}

#if OSXG5
static void AddPciRegister(OsxTreeNode *node, uint32 device, uint32 base, uint32 size, uint32 prefetch)
{
	uint32 cells[10];
	cells[0] = device << 11;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0;
	cells[4] = 0;
	cells[5] = 0x02000010 | prefetch | (device << 11);
	cells[6] = 0;
	cells[7] = 0;
	cells[8] = 0;
	cells[9] = size;
	AddCells(node, "reg", cells, 10);
	cells[0] = 0x82000010 | prefetch | (device << 11);
	cells[1] = 0;
	cells[2] = base;
	cells[3] = 0;
	cells[4] = size;
	AddCells(node, "assigned-addresses", cells, 5);
}

static void AddPciIdentity(OsxTreeNode *node, uint32 vendor, uint32 device, uint32 classcode)
{
	AddCell(node, "vendor-id", vendor);
	AddCell(node, "device-id", device);
	AddCell(node, "revision-id", 0);
	AddCell(node, "class-code", classcode);
}

static void AddInterrupts(OsxTreeNode *node, uint32 first, uint32 firstsense, uint32 second, uint32 secondsense, int count)
{
	uint32 cells[4];
	cells[0] = first;
	cells[1] = firstsense;
	cells[2] = second;
	cells[3] = secondsense;
	AddCells(node, "interrupts", cells, count * 2);
}

static OsxTreeNode *AddKauaiNode(OsxTreeNode *ht, uint32 device, uint32 base, uint32 irq, uint32 dmairq)
{
	OsxTreeNode *node;
	node = NewNode(ht, "ata-6");
	AddString(node, "device_type", "ata");
	AddString(node, "compatible", "K2-UATA");
	AddString(node, "model", "ata-6");
	AddPciRegister(node, device, base, OSXG5ATASIZE, 0);
	AddPciIdentity(node, 0x106b, 0x0043, 0x01018f);
	AddInterrupts(node, irq, 1, dmairq, 0, 2);
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
	return node;
}

static void AddPmuNodes(OsxTreeNode *macio, OsxTreeNode **interrupting)
{
	static const uint8 priminfo[] = {
		0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x03, 0x0d, 0x40,
		0x00, 0x01, 0xe7, 0x05, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x26, 0x0d, 0x46, 0x00, 0x02, 0x78, 0x78, 0x3c, 0x00
	};
	OsxTreeNode *gpio;
	OsxTreeNode *pmu;
	OsxTreeNode *node;
	uint32 cells[2];
	gpio = NewNode(macio, "gpio");
	AddString(gpio, "device_type", "gpio");
	AddString(gpio, "compatible", "mac-io-gpio");
	cells[0] = OSXG5GPIOBASE;
	cells[1] = 0x30;
	AddCells(gpio, "reg", cells, 2);
	AddCell(gpio, "#address-cells", 1);
	AddCell(gpio, "#size-cells", 0);
	node = NewNode(gpio, "extint-gpio1");
	AddString(node, "device_type", "extint-gpio1");
	AddStrings(node, "compatible", "keywest-gpio1", "gpio");
	AddCell(node, "reg", 9);
	AddInterrupts(node, OSXG5IRQPMUGPIO, 1, 0, 0, 1);
	interrupting[0] = node;
	node = NewNode(gpio, "programmer-switch");
	AddString(node, "device_type", "programmer-switch");
	AddInterrupts(node, 0x37, 0, 0, 0, 1);
	interrupting[1] = node;
	pmu = NewNode(macio, "via-pmu");
	AddString(pmu, "device_type", "via-pmu");
	AddString(pmu, "compatible", "pmu");
	cells[0] = 0x16000;
	cells[1] = 0x2000;
	AddCells(pmu, "reg", cells, 2);
	AddInterrupts(pmu, OSXG5IRQPMU, 1, 0, 0, 1);
	AddCell(pmu, "pmu-version", 0xd07f0c);
	AddCell(pmu, "#address-cells", 1);
	AddCell(pmu, "#size-cells", 0);
	interrupting[2] = pmu;
	node = NewNode(pmu, "adb");
	AddString(node, "device_type", "adb");
	AddString(node, "compatible", "pmu-99");
	node = NewNode(pmu, "rtc");
	AddString(node, "device_type", "rtc");
	AddString(node, "compatible", "rtc,via-pmu");
	node = NewNode(pmu, "power-mgt");
	AddString(node, "device_type", "power-mgt");
	AddString(node, "compatible", "via-pmu-99");
	AddString(node, "registry-name", "extint-gpio1");
	AddProperty(node, "prim-info", priminfo, sizeof(priminfo));
}

static OsxTreeNode *AddU3Nodes(OsxTreeNode *root)
{
	OsxTreeNode *u3;
	OsxTreeNode *i2c;
	OsxTreeNode *node;
	uint32 cells[2];
	u3 = NewNode(root, "u3");
	AddString(u3, "device_type", "memory-controller");
	AddString(u3, "compatible", "u3");
	AddCell(u3, "device-rev", 0xb3);
	cells[0] = OSXG5U3BASE;
	cells[1] = OSXG5U3SIZE;
	AddCells(u3, "reg", cells, 2);
	AddCell(u3, "#address-cells", 1);
	AddCell(u3, "#size-cells", 1);
	i2c = NewNode(u3, "i2c");
	AddString(i2c, "device_type", "i2c");
	AddStrings(i2c, "compatible", "keywest-i2c", "uni-n-i2c");
	cells[0] = OSXG5U3I2C;
	cells[1] = 0x1000;
	AddCells(i2c, "reg", cells, 2);
	AddCell(i2c, "AAPL,address", OSXG5U3I2C);
	AddCell(i2c, "AAPL,address-step", 0x10);
	AddCell(i2c, "AAPL,i2c-rate", 100);
	AddString(i2c, "AAPL,driver-name", ".i2c-uni-n");
	AddInterrupts(i2c, OSXG5IRQU3I2C, 1, 0, 0, 1);
	AddCell(i2c, "#address-cells", 1);
	AddCell(i2c, "#size-cells", 0);
	node = NewNode(u3, "dart");
	AddString(node, "device_type", "dart");
	AddStrings(node, "compatible", "u3-dart", "dart");
	cells[0] = OSXG5DART;
	cells[1] = OSXG5DARTSIZE;
	AddCells(node, "reg", cells, 2);
	return i2c;
}

static OsxTreeNode *AddK2I2cNode(OsxTreeNode *macio)
{
	OsxTreeNode *i2c;
	OsxTreeNode *node;
	uint32 cells[2];
	node = NewNode(macio, "i2c");
	AddString(node, "device_type", "i2c");
	AddStrings(node, "compatible", "k2-i2c", "keywest-i2c");
	AddProperty(node, "built-in", "", 0);
	cells[0] = 0x18000;
	cells[1] = 0x1000;
	AddCells(node, "reg", cells, 2);
	AddCell(node, "AAPL,address", OSXG5MACIOBASE + 0x18000);
	AddCell(node, "AAPL,address-step", 0x10);
	AddCell(node, "AAPL,i2c-rate", 100);
	AddString(node, "AAPL,driver-name", ".i2c-mac-io");
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
	AddInterrupts(node, OSXG5IRQK2I2C, 1, 0, 0, 1);
	i2c = node;
	node = NewNode(node, "deq");
	AddString(node, "device_type", "deq");
	AddCell(node, "reg", 0x6a);
	AddCell(node, "i2c-address", 0x6a);
	return i2c;
}

static OsxTreeNode *AddI2sNodes(OsxTreeNode *macio)
{
	static const uint32 clockdata[] = {
		0x02b11000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x400, 0x44,
		4, 0x69303439, 0x6e756c6c, 0,
		0x02ee0000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x200, 0x44,
		2, 0x69303435, 0x6e756c6c, 0,
		0x01194000, 0x3c, 0x3000, 0x3c, 0x400, 0x44, 0x4000, 0,
		0, 0x6e756c6c, 0x6e756c6c, 0
	};
	static const uint32 clockid[] = { 0x69327330, 0x69303435, 0x69303439, 0x69303138 };
	static const uint32 priorities[] = { 2, 4, 4 };
	OsxTreeNode *i2s;
	OsxTreeNode *bus;
	OsxTreeNode *node;
	uint32 cells[6];
	i2s = NewNode(macio, "i2s");
	AddString(i2s, "device_type", "i2s");
	AddProperty(i2s, "built-in", "", 0);
	cells[0] = 0x10000;
	cells[1] = 0x1000;
	cells[2] = 0x8000;
	cells[3] = 0x400;
	AddCells(i2s, "reg", cells, 4);
	AddCell(i2s, "#address-cells", 1);
	AddProperty(i2s, "ranges", "", 0);
	AddCell(i2s, "AAPL,address", OSXG5MACIOBASE + 0x10000);
	bus = NewNode(i2s, "i2s-a");
	AddString(bus, "device_type", "soundbus");
	AddString(bus, "compatible", "i2sbus");
	AddProperty(bus, "built-in", "", 0);
	cells[0] = 0;
	cells[1] = 0x1000;
	cells[2] = 0;
	cells[3] = 0x100;
	cells[4] = 0x100;
	cells[5] = 0x100;
	AddCells(bus, "reg", cells, 6);
	cells[0] = 0x1e;
	cells[1] = 1;
	cells[2] = OSXG5IRQI2STX;
	cells[3] = 0;
	cells[4] = OSXG5IRQI2SRX;
	cells[5] = 0;
	AddCells(bus, "interrupts", cells, 6);
	AddCells(bus, "AAPL,requested-priorities", priorities, 3);
	AddCells(bus, "AAPL,clock-id", clockid, 4);
	AddCells(bus, "AAPL,clock-data", clockdata, (int)(sizeof(clockdata) / sizeof(clockdata[0])));
	node = NewNode(bus, "sound");
	AddString(node, "device_type", "soundchip");
	AddString(node, "compatible", "AOAK2");
	AddProperty(node, "built-in", "", 0);
	AddCell(node, "layout-id", 36);
	AddCell(node, "object-model-version", 2);
	AddCell(node, "vendor-id", 0x106b);
	return bus;
}

static void AddI2sFunctions(OsxTreeNode *macio, OsxTreeNode *bus)
{
	static const char *const setnames[10] = {
		"platform-do-enable", "platform-do-disable", "platform-do-clock-enable", "platform-do-clock-disable",
		"platform-do-sw-reset", "platform-do-clear-sw-reset", "platform-do-cell-enable", "platform-do-cell-disable",
		"platform-do-dig-hw-reset", "platform-do-codec-clock-mux"
	};
	static const uint32 setvalues[10][3] = {
		{ 0x08000000, 0x2000, 0x2000 }, { 0x08000000, 0, 0x2000 }, { 0x08800000, 0x1000, 0x1000 }, { 0x08800000, 0, 0x1000 },
		{ 0x08000000, 0x800, 0x800 }, { 0x08000000, 0, 0x800 }, { 0x08000000, 0x400, 0x400 }, { 0x08000000, 0, 0x400 },
		{ 0x08000000, 0, 0 }, { 0x08000000, 0, 0 }
	};
	static const char *const getnames[4] = {
		"platform-do-get-enable", "platform-do-get-clock-enable", "platform-do-get-sw-reset", "platform-do-get-cell-enable"
	};
	static const uint32 getvalues[4][2] = { { 0x2000, 0xd }, { 0x1000, 0xc }, { 0x800, 0xb }, { 0x400, 0xa } };
	static const char *const busnames[14] = {
		"platform-enable", "platform-disable", "platform-clock-enable", "platform-clock-disable", "platform-sw-reset",
		"platform-clear-sw-reset", "platform-cell-enable", "platform-cell-disable", "platform-get-enable",
		"platform-get-clock-enable", "platform-get-sw-reset", "platform-get-cell-enable", "platform-dig-hw-reset",
		"platform-codec-clock-mux"
	};
	uint32 cells[7];
	uint32 macioh;
	uint32 bush;
	int index;
	macioh = Big32(FindProperty(macio, "AAPL,phandle")->value);
	bush = Big32(FindProperty(bus, "AAPL,phandle")->value);
	for (index = 0; index < 14; index++)
		AddCell(bus, busnames[index], macioh);
	for (index = 0; index < 10; index++) {
		cells[0] = bush;
		cells[1] = setvalues[index][0];
		cells[2] = 3;
		cells[3] = 0x3c;
		cells[4] = setvalues[index][1];
		cells[5] = setvalues[index][2];
		AddCells(macio, setnames[index], cells, 6);
	}
	for (index = 0; index < 4; index++) {
		cells[0] = bush;
		cells[1] = 0x08000000;
		cells[2] = 0x1a;
		cells[3] = 0x3c;
		cells[4] = getvalues[index][0];
		cells[5] = getvalues[index][1];
		cells[6] = 1;
		AddCells(macio, getnames[index], cells, 7);
	}
}

static OsxTreeNode *BuildTreeG5(OsxBootState *state, OsxTreeNode **memorymap)
{
	static const char rootcompatible[] = "PowerMac7,3\0MacRISC4\0Power Macintosh";
	OsxMachine *machine;
	OsxTreeNode *root;
	OsxTreeNode *node;
	OsxTreeNode *cpu;
	OsxTreeNode *ht;
	OsxTreeNode *macio;
	OsxTreeNode *mpic;
	OsxTreeNode *chosen;
	OsxTreeNode *interrupting[9];
	OsxTreeProperty *property;
	uint8 address[6];
	uint32 cells[12];
	uint32 parent;
	int index;
	machine = state->machine;
	root = NewNode(NULL, "device-tree");
	AddString(root, "model", "PowerMac7,3");
	AddProperty(root, "compatible", rootcompatible, sizeof(rootcompatible));
	AddString(root, "device_type", "bootrom");
	AddCell(root, "#address-cells", 1);
	AddCell(root, "#size-cells", 1);
	AddCell(root, "clock-frequency", OSXBUSFREQUENCY);
	AddString(root, "copyright", "Copyright 1983-2003 Apple Computer, Inc. All Rights Reserved");

	node = NewNode(root, "cpus");
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
	cpu = NewNode(node, "PowerPC,G5");
	AddString(cpu, "device_type", "cpu");
	AddCell(cpu, "reg", 0);
	AddCell(cpu, "cpu-version", machine->pvr);
	AddString(cpu, "state", "running");
	AddProperty(cpu, "64-bit", "", 0);
	AddProperty(cpu, "altivec", "", 0);
	AddCell(cpu, "clock-frequency", OSXCPUFREQUENCYG4);
	AddCell(cpu, "bus-frequency", OSXBUSFREQUENCY);
	AddCell(cpu, "timebase-frequency", OSXTIMEBASEFREQUENCY);
	AddCell(cpu, "d-cache-size", 0x8000);
	AddCell(cpu, "i-cache-size", 0x10000);
	AddCell(cpu, "d-cache-sets", 128);
	AddCell(cpu, "i-cache-sets", 512);
	AddCell(cpu, "d-cache-block-size", 128);
	AddCell(cpu, "i-cache-block-size", 128);
	AddCell(cpu, "reservation-granule-size", 128);
	AddCell(cpu, "tlb-size", 1024);
	AddCell(cpu, "tlb-sets", 256);
	AddCell(cpu, "slb-size", 64);
	AddProperty(cpu, "graphics", "", 0);
	AddProperty(cpu, "external-control", "", 0);
	AddProperty(cpu, "general-purpose", "", 0);

	node = NewNode(root, "memory");
	AddString(node, "device_type", "memory");
	cells[0] = 0;
	cells[1] = machine->ramsize;
	AddCells(node, "reg", cells, 2);

	chosen = NewNode(root, "chosen");
	AddString(chosen, "bootpath", state->rootpath);
	AddString(chosen, "rootpath", state->rootpath);
	*memorymap = NewNode(chosen, "memory-map");

	node = NewNode(root, "rom");
	cells[0] = 0xff800000;
	cells[1] = 0x00800000;
	AddCells(node, "reg", cells, 2);
	node = NewNode(node, "boot-rom");
	cells[0] = 0xfff00000;
	cells[1] = 0x00100000;
	AddCells(node, "reg", cells, 2);
	cells[2] = 0x00051000;
	AddCells(node, "info", cells, 3);
	AddString(node, "BootROM-version", "$0005.10f1");

	node = NewNode(root, "openprom");
	AddString(node, "device_type", "BootROM");
	AddString(node, "model", "OpenFirmware 3");

	node = NewNode(root, "options");
	AddString(node, "boot-args", "");
	AddString(node, "auto-boot?", "true");

	node = NewNode(root, "aliases");
	AddString(node, "mac-io", "/ht@f2000000/mac-io@7");
	AddString(node, "via-pmu", "/ht@f2000000/mac-io@7/via-pmu@16000");
	AddString(node, "rtc", "/ht@f2000000/mac-io@7/via-pmu@16000/rtc");
	AddString(node, "nvram", "/nvram@fff80000");

	node = NewNode(root, "nvram");
	AddString(node, "device_type", "nvram");
	AddString(node, "compatible", "nvram,flash");
	cells[0] = OSXFLASHBASE;
	cells[1] = OSXFLASHSIZE;
	AddCells(node, "reg", cells, 2);
	AddCell(node, "#bytes", OSXFLASHBANK);

	interrupting[8] = AddU3Nodes(root);

	ht = NewNode(root, "ht");
	AddString(ht, "device_type", "ht");
	AddString(ht, "compatible", "u3-ht");
	cells[0] = OSXG5HTCONFIG;
	cells[1] = OSXG5HTCONFIGSIZE;
	cells[2] = OSXG5HTSELF;
	cells[3] = 0x1000;
	AddCells(ht, "reg", cells, 4);
	AddCell(ht, "#address-cells", 3);
	AddCell(ht, "#size-cells", 2);
	cells[0] = 0;
	cells[1] = 0;
	AddCells(ht, "bus-range", cells, 2);
	AddCell(ht, "clock-frequency", 400000000);
	cells[0] = 0x01000000;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0xf4000000;
	cells[4] = 0;
	cells[5] = 0x00400000;
	cells[6] = 0x02000000;
	cells[7] = 0;
	cells[8] = 0x80000000;
	cells[9] = 0x80000000;
	cells[10] = 0;
	cells[11] = 0x10000000;
	AddCells(ht, "ranges", cells, 12);
	AddString(ht, "AAPL,unit-string", "f2000000");

	macio = NewNode(ht, "mac-io");
	AddString(macio, "device_type", "mac-io");
	AddString(macio, "compatible", "K2-Keylargo");
	AddString(macio, "model", "AAPL,K2");
	AddPciRegister(macio, OSXG5DEVICEMACIO, OSXG5MACIOBASE, OSXG5MACIOSIZE, 0);
	cells[0] = 0;
	cells[1] = 0x82000010 | (OSXG5DEVICEMACIO << 11);
	cells[2] = 0;
	cells[3] = OSXG5MACIOBASE;
	cells[4] = OSXG5MACIOSIZE;
	AddCells(macio, "ranges", cells, 5);
	AddCell(macio, "AAPL,address", OSXG5MACIOBASE);
	AddPciIdentity(macio, 0x106b, 0x0041, 0xff0000);
	AddCell(macio, "#address-cells", 1);
	AddCell(macio, "#size-cells", 1);

	mpic = NewNode(macio, "interrupt-controller");
	AddString(mpic, "device_type", "open-pic");
	AddString(mpic, "compatible", "chrp,open-pic");
	AddProperty(mpic, "built-in", "", 0);
	cells[0] = OSXG5MPICOFFSET;
	cells[1] = OSXG5MPICSIZE;
	AddCells(mpic, "reg", cells, 2);
	AddCell(mpic, "#interrupt-cells", 2);
	AddCell(mpic, "#address-cells", 0);
	AddProperty(mpic, "interrupt-controller", "", 0);
	AddCell(mpic, "clock-frequency", 4166666);

	AddPmuNodes(macio, interrupting);
	interrupting[7] = AddK2I2cNode(macio);
	interrupting[6] = AddI2sNodes(macio);
	interrupting[3] = AddKauaiNode(ht, OSXG5DEVICEATA0, OSXG5ATABASE, OSXG5IRQATA0, OSXG5IRQATA0DMA);
	interrupting[4] = AddKauaiNode(ht, OSXG5DEVICEATA1, OSXG5ATABASE + OSXG5ATASIZE, OSXG5IRQATA1, OSXG5IRQATA1DMA);

	node = NewNode(ht, "ethernet");
	AddString(node, "device_type", "network");
	AddString(node, "compatible", "K2-GMAC");
	AddPciRegister(node, OSXG5DEVICEGMAC, OSXG5GMACBASE, OSXG5GMACSIZE, 0);
	AddPciIdentity(node, 0x106b, 0x004c, 0x020000);
	AddInterrupts(node, OSXG5IRQGMAC, 1, 0, 0, 1);
	OsxBmacHardwareAddress(address);
	AddProperty(node, "local-mac-address", address, sizeof(address));
	interrupting[5] = node;

	node = NewNode(ht, "SSHV,display");
	AddString(node, "device_type", "display");
	AddString(node, "compatible", "display");
	AddString(node, "model", "SheepShaver,Framebuffer");
	AddPciRegister(node, OSXG5DEVICEDISPLAY, OSXVRAMBASE, OSXVRAMSIZE, 0);
	AddPciIdentity(node, 0x1234, 0x1111, 0x030000);
	AddCell(node, "address", OSXVRAMBASE);
	AddCell(node, "width", machine->width);
	AddCell(node, "height", machine->height);
	AddCell(node, "linebytes", machine->rowbytes);
	AddCell(node, "depth", machine->depth);
	AddProperty(node, "AAPL,boot-display", "", 0);

	AddPhandles(root, &state->phandle);
	property = FindProperty(mpic, "AAPL,phandle");
	parent = Big32(property->value);
	for (index = 0; index < 9; index++)
		AddCell(interrupting[index], "interrupt-parent", parent);
	AddI2sFunctions(macio, interrupting[6]);
	AddCell(cpu, "AAPL,parentIC", parent);
	return root;
}
#endif

static OsxTreeNode *BuildTree(OsxBootState *state, OsxTreeNode **memorymap)
{
	OsxMachine *machine;
	OsxTreeNode *root;
	OsxTreeNode *node;
	OsxTreeNode *pci;
	OsxTreeNode *macio;
	OsxTreeNode *cuda;
	OsxTreeNode *chosen;
	uint32 cells[16];
	machine = state->machine;
	root = NewNode(NULL, "device-tree");
	if (state->machine->pvr == OSXPVRG4) {
		AddString(root, "model", "PowerMac1,2");
		AddStrings(root, "compatible", "PowerMac1,2", "MacRISC");
	} else {
		AddString(root, "model", "PowerMac1,1");
		AddStrings(root, "compatible", "PowerMac1,1", "MacRISC");
	}
	AddCell(root, "#address-cells", 1);
	AddCell(root, "#size-cells", 1);
	AddCell(root, "clock-frequency", OSXBUSFREQUENCY);
	AddString(root, "copyright", "Copyright 1983-1997 Apple Computer, Inc. All Rights Reserved");

	node = NewNode(root, "cpus");
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
#if OSXG5
	if (state->machine->pvr == OSX970PVR) {
		node = NewNode(node, "PowerPC,G5");
		AddProperty(node, "altivec", "", 0);
		AddProperty(node, "64-bit", "", 0);
		AddString(node, "device_type", "cpu");
		AddCell(node, "reg", 0);
		AddCell(node, "cpu-version", state->machine->pvr);
		AddCell(node, "clock-frequency", OSXCPUFREQUENCYG4);
		AddCell(node, "bus-frequency", OSXBUSFREQUENCY);
		AddCell(node, "timebase-frequency", OSXTIMEBASEFREQUENCY);
		AddCell(node, "d-cache-size", 0x8000);
		AddCell(node, "i-cache-size", 0x10000);
		AddCell(node, "d-cache-sets", 128);
		AddCell(node, "i-cache-sets", 512);
		AddCell(node, "d-cache-block-size", 128);
		AddCell(node, "i-cache-block-size", 128);
		AddCell(node, "reservation-granule-size", 128);
		AddCell(node, "tlb-size", 1024);
		AddCell(node, "tlb-sets", 256);
		AddCell(node, "slb-size", 64);
		AddProperty(node, "graphics", "", 0);
		AddProperty(node, "external-control", "", 0);
		AddProperty(node, "general-purpose", "", 0);
	} else {
#endif
	if (state->machine->pvr == OSXPVRG4) {
		node = NewNode(node, "PowerPC,G4");
		AddProperty(node, "altivec", "", 0);
	} else {
		node = NewNode(node, "PowerPC,750");
	}
	AddString(node, "device_type", "cpu");
	AddCell(node, "reg", 0);
	AddCell(node, "cpu-version", state->machine->pvr);
	if (state->machine->pvr == OSXPVRG4)
		AddCell(node, "clock-frequency", OSXCPUFREQUENCYG4);
	else
		AddCell(node, "clock-frequency", OSXCPUFREQUENCY);
	AddCell(node, "bus-frequency", OSXBUSFREQUENCY);
	AddCell(node, "timebase-frequency", OSXTIMEBASEFREQUENCY);
	AddCell(node, "d-cache-size", 0x8000);
	AddCell(node, "i-cache-size", 0x8000);
	AddCell(node, "d-cache-sets", 128);
	AddCell(node, "i-cache-sets", 128);
	AddCell(node, "d-cache-block-size", 32);
	AddCell(node, "i-cache-block-size", 32);
	AddCell(node, "reservation-granule-size", 32);
	AddCell(node, "tlb-size", 128);
	AddCell(node, "tlb-sets", 64);
	AddProperty(node, "graphics", "", 0);
	AddProperty(node, "external-control", "", 0);
	AddProperty(node, "general-purpose", "", 0);
#if OSXG5
	}
#endif

	node = NewNode(root, "memory");
	AddString(node, "device_type", "memory");
	cells[0] = 0;
	cells[1] = machine->ramsize;
	AddCells(node, "reg", cells, 2);

	chosen = NewNode(root, "chosen");
	AddString(chosen, "bootpath", state->rootpath);
	AddString(chosen, "rootpath", state->rootpath);
	*memorymap = NewNode(chosen, "memory-map");

	node = NewNode(root, "rom");
	cells[0] = 0xff800000;
	cells[1] = 0x00800000;
	AddCells(node, "reg", cells, 2);
	node = NewNode(node, "boot-rom");
	cells[0] = 0xfff00000;
	cells[1] = 0x00100000;
	AddCells(node, "reg", cells, 2);
	cells[2] = 0x00033000;
	AddCells(node, "info", cells, 3);
	AddString(node, "BootROM-version", "$0003.30f1");

	node = NewNode(root, "openprom");
	AddString(node, "device_type", "BootROM");
	AddString(node, "model", "Open Firmware, 2.4");

	node = NewNode(root, "options");
	AddString(node, "boot-args", "");
	AddString(node, "auto-boot?", "true");

	node = NewNode(root, "aliases");
	AddString(node, "pci", "/pci@80000000");

	pci = NewNode(root, "pci");
	AddString(pci, "device_type", "pci");
	AddStrings(pci, "compatible", "grackle", "MOT,PPC106");
	AddString(pci, "model", "MOT,PPC106");
	cells[0] = 0x80000000;
	cells[1] = 0x7f000000;
	AddCells(pci, "reg", cells, 2);
	AddCell(pci, "#address-cells", 3);
	AddCell(pci, "#size-cells", 2);
	cells[0] = 0;
	cells[1] = 0;
	AddCells(pci, "bus-range", cells, 2);
	AddCell(pci, "clock-frequency", 33333333);
	cells[0] = 0x01000000;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0xfe000000;
	cells[4] = 0;
	cells[5] = 0x00c00000;
	cells[6] = 0x02000000;
	cells[7] = 0;
	cells[8] = 0x80000000;
	cells[9] = 0x80000000;
	cells[10] = 0;
	cells[11] = 0x7d000000;
	AddCells(pci, "ranges", cells, 12);
	AddString(pci, "AAPL,unit-string", "80000000");

	macio = NewNode(pci, "mac-io");
	AddString(macio, "device_type", "mac-io");
	AddString(macio, "compatible", "heathrow");
	AddString(macio, "model", "AAPL,343S1201");
	cells[0] = OSXPCIDEVICEHEATHROW << 11;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0;
	cells[4] = 0;
	cells[5] = 0x02000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[6] = 0;
	cells[7] = 0;
	cells[8] = 0;
	cells[9] = OSXHEATHROWSIZE;
	AddCells(macio, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[1] = 0;
	cells[2] = OSXHEATHROWBASE;
	cells[3] = 0;
	cells[4] = OSXHEATHROWSIZE;
	AddCells(macio, "assigned-addresses", cells, 5);
	cells[0] = 0;
	cells[1] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[2] = 0;
	cells[3] = OSXHEATHROWBASE;
	cells[4] = OSXHEATHROWSIZE;
	AddCells(macio, "ranges", cells, 5);
	AddCell(macio, "AAPL,address", OSXHEATHROWBASE);
	AddCell(macio, "vendor-id", 0x106b);
	AddCell(macio, "device-id", 0x0010);
	AddCell(macio, "revision-id", 1);
	AddCell(macio, "class-code", 0xff0000);
	AddCell(macio, "#address-cells", 1);
	AddCell(macio, "#size-cells", 1);
	AddString(macio, "AAPL,unit-string", "10");

	node = NewNode(macio, "nvram");
	AddString(node, "device_type", "nvram");
	cells[0] = 0x60000;
	cells[1] = OSXNVRAMSIZE * 16;
	AddCells(node, "reg", cells, 2);

	node = NewNode(macio, "escc");
	AddString(node, "device_type", "escc");
	cells[0] = 0x13000;
	cells[1] = 0x1000;
	AddCells(node, "reg", cells, 2);

	cuda = NewNode(macio, "via-cuda");
	AddString(cuda, "device_type", "via-cuda");
	cells[0] = 0x16000;
	cells[1] = 0x2000;
	AddCells(cuda, "reg", cells, 2);
	AddCell(cuda, "AAPL,interrupts", OSXIRQCUDA);
	AddCell(cuda, "interrupts", OSXIRQCUDA);
	node = NewNode(cuda, "adb");
	AddString(node, "device_type", "adb");
	AddString(node, "compatible", "adb");
	node = NewNode(cuda, "pram");
	AddString(node, "device_type", "pram");
	node = NewNode(cuda, "rtc");
	AddString(node, "device_type", "rtc");
	AddString(node, "compatible", "rtc");

	AddAtaNode(macio, "ata", 0x20000, 0x8b00, OSXIRQIDE0, OSXIRQIDE0DMA);
	AddAtaNode(macio, "ata", 0x21000, 0x8c00, OSXIRQIDE1, OSXIRQIDE1DMA);
	AddBmacNode(macio);
	AddAudioNode(macio);

	node = NewNode(pci, "SSHV,display");
	AddString(node, "device_type", "display");
	AddString(node, "compatible", "display");
	AddString(node, "model", "SheepShaver,Framebuffer");
	cells[0] = OSXPCIDEVICEDISPLAY << 11;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0;
	cells[4] = 0;
	cells[5] = 0x02000010 | (OSXPCIDEVICEDISPLAY << 11);
	cells[6] = 0;
	cells[7] = 0;
	cells[8] = 0;
	cells[9] = OSXVRAMSIZE;
	AddCells(node, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEDISPLAY << 11);
	cells[1] = 0;
	cells[2] = OSXVRAMBASE;
	cells[3] = 0;
	cells[4] = OSXVRAMSIZE;
	AddCells(node, "assigned-addresses", cells, 5);
	AddCell(node, "vendor-id", 0x1234);
	AddCell(node, "device-id", 0x1111);
	AddCell(node, "revision-id", 2);
	AddCell(node, "class-code", 0x030000);
	AddCell(node, "address", OSXVRAMBASE);
	AddCell(node, "width", machine->width);
	AddCell(node, "height", machine->height);
	AddCell(node, "linebytes", machine->rowbytes);
	AddCell(node, "depth", machine->depth);
	AddProperty(node, "AAPL,boot-display", "", 0);
	if (state->ignorendrv)
		AddProperty(node, "AAPL,iokit-ignore-ndrv", "", 0);
	AddString(node, "AAPL,unit-string", "12");
	return root;
}

static void InstallBootArgs(OsxBootState *state, const char *commandline, uint32 tree, uint32 treelength)
{
	OsxMachine *machine;
	uint8 *args;
	machine = state->machine;
	args = machine->ram + machine->bootargs;
	memset(args, 0, BOOTARGSSIZE);
	args[0] = 0;
	args[1] = 1;
	args[2] = 0;
	args[3] = 1;
	strncpy((char *)args + 4, commandline, 255);
	StoreBig32(args + 0x104, 0);
	StoreBig32(args + 0x108, machine->ramsize);
#if OSXG5
	if (machine->g5state.highsize) {
		args[3] = 2;
		StoreBig32(args + 0x108, machine->ramsize >> 12);
		StoreBig32(args + 0x10c, (uint32)(OSXG5HIGHRAM >> 12));
		StoreBig32(args + 0x110, machine->g5state.highsize >> 12);
	}
#endif
	StoreBig32(args + 0x1d4, OSXVRAMBASE);
	StoreBig32(args + 0x1d8, strstr(commandline, "-v") == NULL);
	StoreBig32(args + 0x1dc, machine->rowbytes);
	StoreBig32(args + 0x1e0, machine->width);
	StoreBig32(args + 0x1e4, machine->height);
	StoreBig32(args + 0x1e8, machine->depth);
	StoreBig32(args + 0x1ec, 0);
	StoreBig32(args + 0x1f0, tree);
	StoreBig32(args + 0x1f4, treelength);
	StoreBig32(args + 0x1f8, state->nextfree);
}

static int NeedsVectorCpu(OsxHfsVolume *volume)
{
	uint8 *dyld;
	uint32 size;
	uint32 count;
	uint32 index;
	int compatible;
	dyld = ReadFile(volume, "/usr/lib/dyld", &size);
	if (dyld == NULL)
		return 0;
	compatible = 0;
	if (size >= 8 && Big32(dyld) == 0xcafebabe) {
		count = Big32(dyld + 4);
		for (index = 0; index < count && 8 + index * 20 + 20 <= size; index++)
			if (Big32(dyld + 8 + index * 20) == 18 && Big32(dyld + 8 + index * 20 + 4) <= 9)
				compatible = 1;
	} else if (size < 12 || Big32(dyld) != 0xfeedface || Big32(dyld + 8) <= 9) {
		compatible = 1;
	}
	free(dyld);
	return !compatible;
}

static uint8 *AddDisplayDrivers(OsxHfsVolume *volume, uint8 *mkext, uint32 *size)
{
	static const char *const bundles[2] = { "com.apple.iokit.IOGraphicsFamily", "com.apple.iokit.IONDRVSupport" };
	uint8 *donor;
	uint8 *merged;
	uint32 donorsize;
	if (OsxMkextHasBundle(mkext, *size, bundles[1]))
		return mkext;
	donor = BuildDriverPackage(volume, 0, bundles, 2, &donorsize);
	merged = NULL;
	if (donor)
		merged = OsxMkextAddBundles(mkext, *size, donor, donorsize, bundles, 2, NULL, 0, size);
	free(donor);
	if (merged == NULL)
		return mkext;
	free(mkext);
	return merged;
}

static uint8 *AddOldWorldDrivers(uint8 *mkext, uint32 *size, const char *bootpath)
{
	static const char *const bundles[] = {
		"com.apple.driver.AppleGossamerPE",
		"com.apple.driver.AppleGracklePCI",
		"com.apple.platform.AppleHeathrow",
		"com.apple.driver.HeathrowATA",
		"com.apple.driver.AppleCuda",
		"com.apple.driver.AppleBMacEthernet",
		"com.apple.driver.AppleADBMouse",
		"com.apple.driver.AppleADBButtons"
	};
	static const char *const aliases[] = {
		"__ZN14IOPMrootDomain27registerPMSettingControllerEPFiiiPvES0_", "__ZN9IOService15getPMRootDomainEv"
	};
	static const char *const lists[2] = { "disk", "cdrom" };
	OsxHfsVolume donor;
	const char *path;
	void *file;
	uint8 *donorkext;
	uint8 *merged;
	uint32 donorsize;
	int list;
	int index;
	if (OsxMkextHasBundle(mkext, *size, bundles[0]))
		return mkext;
	merged = NULL;
	for (list = 0; list < 2 && merged == NULL; list++) {
		for (index = 0; merged == NULL && (path = PrefsFindString(lists[list], index)) != NULL; index++) {
			if (strcmp(path, bootpath) == 0 || (file = Sys_open(path, true, false)) == NULL)
				continue;
			if (OpenVolume(&donor, file, -1)) {
				donorkext = ReadFile(&donor, "/System/Library/Extensions.mkext", &donorsize);
				if (donorkext && ThinPowerPC(donorkext, &donorsize) && OsxMkextHasBundle(donorkext, donorsize, bundles[0]))
					merged = OsxMkextAddBundles(mkext, *size, donorkext, donorsize, bundles, 8, aliases, 1, size);
				free(donorkext);
			}
			CloseVolume(&donor);
			Sys_close(file);
		}
	}
	if (merged == NULL)
		return mkext;
	free(mkext);
	return merged;
}

int OsxBootLoad(OsxMachine *machine, const char *imagepath, int bootbus, int bootunit, const char *commandline)
{
	OsxBootState state;
	OsxHfsVolume volume;
	OsxTreeNode *root;
	OsxTreeNode *memorymap;
	OsxTreeProperty *property;
	void *file;
	uint8 *kernel;
	uint8 *mkext;
	uint32 kernelsize;
	uint32 mkextsize;
	uint32 address;
	uint32 treesize;
	uint32 treeaddress;
	uint32 cells[2];
	char rangename[TREENAMELENGTH];
	int index;
	memset(&state, 0, sizeof(state));
	state.machine = machine;
	file = machine->ata[bootbus].drives[bootunit].file;
	if (!OpenVolume(&volume, file, machine->bootpartition)) {
		fprintf(stderr, "osx: no HFS+ volume in %s\n", imagepath);
		CloseVolume(&volume);
		return 0;
	}
	sprintf(state.rootpath, "/pci@80000000/mac-io@10/ata@%x/@%d:%d", 0x20000 + bootbus * 0x1000, bootunit, volume.partition);
	machine->pvr = OSXPVR;
	if (NeedsVectorCpu(&volume))
		machine->pvr = OSXPVRG4;
#if OSXG5
	if (PrefsFindBool("osxcpu970") || machine->g5)
		machine->pvr = OSX970PVR;
	if (machine->g5)
		sprintf(state.rootpath, "/ht@f2000000/ata-6@%x/@%d:%d", OSXG5DEVICEATA0 + bootbus, bootunit, volume.partition);
#endif
	OsxCpuSetModel(machine->cpu, machine->pvr);
	kernel = ReadFile(&volume, "/mach_kernel", &kernelsize);
	if (kernel == NULL || !ThinPowerPC(kernel, &kernelsize) || !LoadKernel(&state, kernel, kernelsize)) {
		fprintf(stderr, "osx: cannot load /mach_kernel\n");
		free(kernel);
		CloseVolume(&volume);
		return 0;
	}
	state.ignorendrv = ContainsBytes(kernel, kernelsize, "Darwin Kernel Version 1.");
	mkext = NULL;
	if (!state.ignorendrv)
		mkext = ReadFile(&volume, "/System/Library/Extensions.mkext", &mkextsize);
	if (mkext != NULL && !ThinPowerPC(mkext, &mkextsize)) {
		free(mkext);
		mkext = NULL;
	}
	if (mkext == NULL)
		mkext = BuildDriverPackage(&volume, state.ignorendrv, NULL, 0, &mkextsize);
	else if (!state.ignorendrv)
		mkext = AddDisplayDrivers(&volume, mkext, &mkextsize);
	if (mkext)
		mkext = AddOldWorldDrivers(mkext, &mkextsize, imagepath);
	if (mkext) {
		address = Allocate(&state, mkextsize);
		memcpy(machine->ram + address, mkext, mkextsize);
		sprintf(rangename, "DriversPackage-%x", address);
		AddRange(&state, rangename, address, mkextsize);
		machine->mkext = mkext;
		machine->mkextsize = mkextsize;
	} else {
		fprintf(stderr, "osx: no Extensions.mkext, booting without drivers\n");
	}
	FindClipboardFunctions(machine, &volume);
	CloseVolume(&volume);
	machine->bootargs = Allocate(&state, BOOTARGSSIZE);
	AddRange(&state, "BootArgs", machine->bootargs, BOOTARGSSIZE);
#if OSXG5
	state.phandle = 0xff000001;
	if (machine->g5)
		root = BuildTreeG5(&state, &memorymap);
	else
#endif
	root = BuildTree(&state, &memorymap);
	cells[0] = 0;
	cells[1] = 0;
	AddProperty(memorymap, "DeviceTree", cells, 8);
	for (index = 0; index < state.rangecount; index++)
		AddProperty(memorymap, state.ranges[index].name, cells, 8);
	state.phandle = 0xff000001;
	AddPhandles(root, &state.phandle);
	treesize = TreeSize(root);
	treeaddress = Allocate(&state, treesize);
	AddRange(&state, "DeviceTree", treeaddress, treesize);
	for (index = 0; index < state.rangecount; index++) {
		property = FindProperty(memorymap, state.ranges[index].name);
		if (property) {
			StoreBig32(property->value, state.ranges[index].address);
			StoreBig32(property->value + 4, state.ranges[index].length);
		}
	}
	FlattenTree(root, machine->ram + treeaddress);
	DestroyTree(root);
	machine->kernelend = state.nextfree;
	InstallBootArgs(&state, commandline, treeaddress, treesize);
	machine->scratch = state.nextfree + 0x10000;
	if (machine->log) {
		fprintf(machine->log, "kernel entry %08x bootargs %08x tree %08x+%x top %08x\n", machine->kernelentry, machine->bootargs, treeaddress, treesize, state.nextfree);
		for (index = 0; index < state.rangecount; index++)
			fprintf(machine->log, "  %-24s %08x %08x\n", state.ranges[index].name, state.ranges[index].address, state.ranges[index].length);
	}
	free(kernel);
	return 1;
}
