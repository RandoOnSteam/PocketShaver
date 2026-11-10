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
#include "main.h"
#include "sys.h"
#include "prefs.h"
#include "osxmachine.h"
#include "osxguest.h"
#include "osxglbridge.h"
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
#define BOOTSCREENGRAY 0xbf
#define BOOTCLUTSPINNERENTRY 3
#define BOOTCLUTFULL 0xff
#define BOOTCLUTYELLOW 0xcc
#define BOOTSPINNERSCAN 0x800
#define BOOTLOADDELAY 0x38800005u
#define BOOTLOADNODELAY 0x38800000u
#define BOOTBRANCHLINK 0x48000001u
#define BOOTBRANCHMASK 0xfc000003u
#define BOOTSPINNERFLAGS 4
#define BOOTSPINNERWIDTH 0x10
#define BOOTSPINNERX 0x18
#define BOOTSPINNERY 0x1c
#define BOOTSPINNERCORNER 15
#define BOOTSPINNERBELOW 200
#define CLIPTOOLBOX "/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/HIToolbox"
#define CLIPSYSTEM "/usr/lib/libSystem.B.dylib"
#define QUARTZCOREGRAPHICS "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"
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

static uint32 OsxGuestBig32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static uint32 OsxGuestBig16(const uint8 *data)
{
	return ((uint32)data[0] << 8) | data[1];
}

static void OsxGuestStoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static int OsxGuestReadVolume(OsxHfsVolume *volume, loff_t offset, void *buffer, uint32 length)
{
	return Sys_read(volume->file, buffer, volume->base + offset, length) == length;
}

static void OsxGuestParseFork(const uint8 *data, uint32 fileid, uint8 forktype, OsxHfsFork *fork)
{
	int index;
	fork->logical = ((uint64)OsxGuestBig32(data) << 32) | OsxGuestBig32(data + 4);
	fork->totalblocks = OsxGuestBig32(data + 12);
	fork->fileid = fileid;
	fork->forktype = forktype;
	fork->extentcount = 0;
	for (index = 0; index < 8; index++) {
		fork->extents[index].start = OsxGuestBig32(data + 16 + index * 8);
		fork->extents[index].count = OsxGuestBig32(data + 20 + index * 8);
		if (fork->extents[index].count)
			fork->extentcount = index + 1;
	}
}

static int OsxGuestReadFork(OsxHfsVolume *volume, OsxHfsFork *fork, uint64 offset, uint8 *buffer, uint32 length)
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
			if (!OsxGuestReadVolume(volume, (loff_t)fork->extents[index].start * volume->blocksize + (loff_t)inner, buffer, take))
				return 0;
			buffer += take;
			offset += take;
			length -= take;
		}
		position += extentbytes;
	}
	return length == 0;
}

static uint32 OsxGuestBlocksInFork(OsxHfsFork *fork)
{
	uint32 total;
	int index;
	total = 0;
	for (index = 0; index < fork->extentcount; index++)
		total += fork->extents[index].count;
	return total;
}

static void OsxGuestCompleteFork(OsxHfsVolume *volume, OsxHfsFork *fork)
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
	have = OsxGuestBlocksInFork(fork);
	while (have < fork->totalblocks && volume->overflowfirstleaf) {
		progress = 0;
		node = volume->overflowfirstleaf;
		while (node && !progress) {
			if (!OsxGuestReadFork(volume, &volume->overflow, (uint64)node * volume->overflownodesize, volume->node, volume->overflownodesize))
				return;
			data = volume->node;
			records = OsxGuestBig16(data + 10);
			for (record = 0; record < records && !progress; record++) {
				offset = OsxGuestBig16(data + volume->overflownodesize - 2 * (record + 1));
				keylength = OsxGuestBig16(data + offset);
				if (data[offset + 2] != fork->forktype || OsxGuestBig32(data + offset + 4) != fork->fileid)
					continue;
				keystart = OsxGuestBig32(data + offset + 8);
				if (keystart != have)
					continue;
				for (index = 0; index < 8 && fork->extentcount < HFSMAXEXTENTS; index++) {
					fork->extents[fork->extentcount].start = OsxGuestBig32(data + offset + 2 + keylength + index * 8);
					fork->extents[fork->extentcount].count = OsxGuestBig32(data + offset + 2 + keylength + index * 8 + 4);
					if (fork->extents[fork->extentcount].count)
						fork->extentcount++;
				}
				progress = 1;
			}
			node = OsxGuestBig32(data);
		}
		if (!progress)
			return;
		have = OsxGuestBlocksInFork(fork);
	}
}

static uint32 OsxGuestFoldCharacter(uint32 character)
{
	if (character == 0)
		return 0xffff;
	if (character >= 'A' && character <= 'Z')
		return character + 32;
	return character;
}

static int OsxGuestCompareCatalogKey(const uint8 *key, uint32 parent, const uint16 *name, uint32 namelength)
{
	uint32 keyparent;
	uint32 keynamelength;
	uint32 index;
	uint32 left;
	uint32 right;
	keyparent = OsxGuestBig32(key + 2);
	if (keyparent < parent)
		return -1;
	if (keyparent > parent)
		return 1;
	keynamelength = OsxGuestBig16(key + 6);
	for (index = 0; index < keynamelength && index < namelength; index++) {
		left = OsxGuestFoldCharacter(OsxGuestBig16(key + 8 + index * 2));
		right = OsxGuestFoldCharacter(name[index]);
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

static int OsxGuestDescendCatalog(OsxHfsVolume *volume, uint32 parent, const uint16 *name, uint32 namelength)
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
		if (!OsxGuestReadFork(volume, &volume->catalog, (uint64)node * volume->catalognodesize, volume->node, volume->catalognodesize))
			return 0;
		data = volume->node;
		kind = (int)(signed char)data[8];
		records = OsxGuestBig16(data + 10);
		if (kind == -1)
			return 1;
		if (kind != 0)
			return 0;
		child = 0;
		found = 0;
		for (index = 0; index < records; index++) {
			offset = OsxGuestBig16(data + volume->catalognodesize - 2 * (index + 1));
			if (OsxGuestCompareCatalogKey(data + offset, parent, name, namelength) > 0)
				break;
			keylength = OsxGuestBig16(data + offset);
			child = OsxGuestBig32(data + offset + 2 + keylength);
			found = 1;
		}
		if (!found)
			return 0;
		node = child;
	}
	return 0;
}

static int OsxGuestFindCatalogRecord(OsxHfsVolume *volume, uint32 parent, const char *text, uint8 *record)
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
	if (!OsxGuestDescendCatalog(volume, parent, name, namelength))
		return 0;
	data = volume->node;
	records = OsxGuestBig16(data + 10);
	for (index = 0; index < records; index++) {
		offset = OsxGuestBig16(data + volume->catalognodesize - 2 * (index + 1));
		if (OsxGuestCompareCatalogKey(data + offset, parent, name, namelength) == 0) {
			memcpy(record, data + offset + 2 + OsxGuestBig16(data + offset), 248);
			return 1;
		}
	}
	return 0;
}

static int OsxGuestListKextFolders(OsxHfsVolume *volume, uint32 parent, char *names, int maximum)
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
	if (!OsxGuestDescendCatalog(volume, parent, NULL, 0))
		return 0;
	for (;;) {
		data = volume->node;
		records = OsxGuestBig16(data + 10);
		for (index = 0; index < records; index++) {
			offset = OsxGuestBig16(data + volume->catalognodesize - 2 * (index + 1));
			key = data + offset;
			if (OsxGuestBig32(key + 2) < parent)
				continue;
			if (OsxGuestBig32(key + 2) > parent)
				return count;
			length = OsxGuestBig16(key + 6);
			if (OsxGuestBig16(key + 2 + OsxGuestBig16(key)) != HFSFOLDERRECORD || length < 6 || length >= BOOTKEXTNAME || count >= maximum)
				continue;
			ascii = 1;
			for (position = 0; position < length; position++) {
				character = OsxGuestBig16(key + 8 + position * 2);
				if (character == 0 || character > 0x7e)
					ascii = 0;
				names[count * BOOTKEXTNAME + position] = (char)character;
			}
			names[count * BOOTKEXTNAME + length] = 0;
			if (ascii && strcmp(names + count * BOOTKEXTNAME + length - 5, ".kext") == 0)
				count++;
		}
		offset = OsxGuestBig32(data);
		if (offset == 0 || !OsxGuestReadFork(volume, &volume->catalog, (uint64)offset * volume->catalognodesize, volume->node, volume->catalognodesize))
			return count;
	}
}

static int OsxGuestLocateVolume(OsxHfsVolume *volume, void *file, int partition, uint8 *block)
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
		mapblock = OsxGuestBig16(block + 2);
		if (mapblock != 2048)
			mapblock = 512;
		for (index = 1; index < 64; index++) {
			if (Sys_read(file, block, (loff_t)index * mapblock, 512) != 512 || block[0] != 'P' || block[1] != 'M')
				return 0;
			if (strncmp((const char *)block + 48, "Apple_HFS", 32) == 0 &&
				(partition < 0 || partition == (int)index)) {
				volume->base = (loff_t)OsxGuestBig32(block + 8) * mapblock;
				volume->partition = (int)index;
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
	} else if (partition > 0)
		return 0;
	if (!OsxGuestReadVolume(volume, 1024, block, 512))
		return 0;
	if (block[0] == 'B' && block[1] == 'D') {
		if (OsxGuestBig16(block + 124) != 0x482b)
			return 1;
		allocationblock = OsxGuestBig32(block + 20);
		firstblock = OsxGuestBig16(block + 28);
		embedstart = OsxGuestBig16(block + 126);
		volume->base += (loff_t)firstblock * 512 + (loff_t)embedstart * allocationblock;
		if (!OsxGuestReadVolume(volume, 1024, block, 512))
			return 0;
	}
	return block[0] == 'H' && (block[1] == '+' || block[1] == 'X');
}

static int OsxGuestOpenVolume(OsxHfsVolume *volume, void *file, int partition)
{
	uint8 block[512];
	if (!OsxGuestLocateVolume(volume, file, partition, block) || block[0] != 'H')
		return 0;
	volume->blocksize = OsxGuestBig32(block + 40);
	OsxGuestParseFork(block + 0xc0, 3, 0, &volume->overflow);
	OsxGuestParseFork(block + 0x110, 4, 0, &volume->catalog);
	volume->node = (uint8 *)malloc(65536);
	if (!OsxGuestReadFork(volume, &volume->overflow, 0, volume->node, 512))
		return 0;
	volume->overflownodesize = OsxGuestBig16(volume->node + 32);
	volume->overflowfirstleaf = OsxGuestBig32(volume->node + 24);
	if (volume->overflow.extentcount == 0)
		volume->overflowfirstleaf = 0;
	if (!OsxGuestReadFork(volume, &volume->catalog, 0, volume->node, 512))
		return 0;
	volume->catalognodesize = OsxGuestBig16(volume->node + 32);
	volume->catalogroot = OsxGuestBig32(volume->node + 16);
	OsxGuestCompleteFork(volume, &volume->catalog);
	return 1;
}

static void OsxGuestCloseVolume(OsxHfsVolume *volume)
{
	free(volume->node);
}

static int OsxGuestProbeBootImage(const char *path, int *partition, int *attached)
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
	if (OsxGuestLocateVolume(&volume, file, *partition, block)) {
		*partition = volume.partition;
		if (block[0] == 'B') {
			if (OsxGuestBig32(block + 92) != 0)
				systems = OSXBOOTCLASSIC;
		} else if (OsxGuestOpenVolume(&volume, file, *partition)) {
			if (OsxGuestFindCatalogRecord(&volume, HFSROOTFOLDER, "mach_kernel", record) &&
				OsxGuestBig16(record) == HFSFILERECORD)
				systems = OSXBOOTNATIVE;
			folder = OsxGuestBig32(block + 92);
			if (folder == 0 && systems == 0)
				folder = OsxGuestBig32(block + 80);
			if (folder != 0 && OsxGuestFindCatalogRecord(&volume, folder, "System", record) &&
				OsxGuestBig16(record) == HFSFILERECORD &&
				OsxGuestFindCatalogRecord(&volume, folder, "Finder", record) && OsxGuestBig16(record) == HFSFILERECORD)
				systems |= OSXBOOTCLASSIC;
		}
	}
	OsxGuestCloseVolume(&volume);
	Sys_close(file);
	return systems;
}

int OsxGuestSelectBootImage(int *diskindex, int *partition)
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
	OsxGuestNvramReadString(XPRAM, 0, device, sizeof(device));
	OsxGuestNvramReadString(XPRAM, 9, command, sizeof(command));
	wantclassic = OsxGuestNvramWantsClassic(device, command);
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
		systems = OsxGuestProbeBootImage(path, &candidatepartition, &attached);
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
		systems = OsxGuestProbeBootImage(path, &candidatepartition, &attached);
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

static int OsxGuestLookupPath(OsxHfsVolume *volume, const char *path, uint8 *record)
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
		if (!OsxGuestFindCatalogRecord(volume, parent, component, record))
			return 0;
		parent = OsxGuestBig32(record + 8);
	}
	return 1;
}

static uint8 *OsxGuestReadRecord(OsxHfsVolume *volume, const uint8 *record, uint32 *size)
{
	OsxHfsFork *fork;
	uint8 *data;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	OsxGuestParseFork(record + 88, OsxGuestBig32(record + 8), 0, fork);
	OsxGuestCompleteFork(volume, fork);
	data = (uint8 *)malloc((size_t)fork->logical + 1);
	if (data == NULL || !OsxGuestReadFork(volume, fork, 0, data, (uint32)fork->logical)) {
		free(data);
		free(fork);
		return NULL;
	}
	data[fork->logical] = 0;
	*size = (uint32)fork->logical;
	free(fork);
	return data;
}

static uint8 *OsxGuestReadFile(OsxHfsVolume *volume, const char *path, uint32 *size)
{
	uint8 record[248];
	if (!OsxGuestLookupPath(volume, path, record) || OsxGuestBig16(record) != HFSFILERECORD)
		return NULL;
	return OsxGuestReadRecord(volume, record, size);
}

static int OsxGuestAddGuestFilePatch(OsxHfsVolume *volume, const OsxHfsFork *fork, OsxAtaBus *bus,
	const OsxAtaDrive *drive, uint32 offset, const uint8 *bytes, int length)
{
	uint64 extentbytes;
	int extent;
	OsxDiskPatch *patch;
	if (!bus->patches || bus->patchcount >= OSXDISKPATCHES)
		return 0;
	for (extent = 0; extent < fork->extentcount; extent++) {
		extentbytes = (uint64)fork->extents[extent].count * volume->blocksize;
		if (offset < extentbytes) {
			if (length > extentbytes - offset)
				return 0;
			patch = &bus->patches[bus->patchcount++];
			patch->offset = volume->base + (uint64)fork->extents[extent].start * volume->blocksize + offset;
			patch->length = length;
			patch->atapionly = 0;
			patch->drive = drive;
			memcpy(patch->bytes, bytes, length);
			return 1;
		}
		offset -= (uint32)extentbytes;
	}
	return 0;
}

static uint64 OsxGuestFileHash(const uint8 *image, uint32 size)
{
	uint32 index;
	uint64 hash;
	hash = 14695981039346656037ULL;
	for (index = 0; index < size; index++)
		hash = (hash ^ image[index]) * 1099511628211ULL;
	return hash;
}

static int OsxGuestPrepareFlurry(OsxMachine *machine, OsxHfsVolume *volume, int bootbus, int bootunit)
{
	uint8 record[248];
	uint8 replacement[16];
	uint8 *image;
	uint32 size;
	uint32 index;
	uint32 call;
	uint32 stub;
	uint32 stubstart;
	uint32 stubsize;
	uint32 stubcount;
	uint32 symboltable;
	uint32 symbolbias;
	uint32 slicebase;
	uint32 interval;
	uint32 initoffset;
	uint64 hash;
	int originalcount;
	OsxHfsFork *fork;
	OsxAtaBus *bus;
	const OsxAtaDrive *drive;
	if (!OsxGuestLookupPath(volume, "/System/Library/Screen Savers/Flurry.saver/Contents/MacOS/Flurry", record) ||
		OsxGuestBig16(record) != HFSFILERECORD || OsxGuestBig32(record + 88) != 0)
		return 0;
	size = OsxGuestBig32(record + 92);
	if (size != 64020 && size != 60524 && size != 60584 && size != 140032)
		return 0;
	image = OsxGuestReadRecord(volume, record, &size);
	if (!image)
		return 0;
	hash = OsxGuestFileHash(image, size);
	stubstart = OSXFLURRYSTUB;
	stubsize = OSXFLURRYSTUBSIZE;
	stubcount = OSXFLURRYSTUBCOUNT;
	interval = 0xadd8;
	slicebase = 0;
	switch (hash) {
	case 0xc77ec9fd0cbf6851ULL:
		initoffset = 0x14a8;
		stubstart = 0x95ec;
		stubsize = 36;
		symboltable = 59344;
		symbolbias = 99;
		break;
	case 0x709f32ff31048fadULL:
		initoffset = 0x1568;
		symboltable = 58156;
		symbolbias = 0;
		break;
	case 0x8110edabd1489199ULL:
	case 0x6d58e084d36befc7ULL:
		initoffset = 0x1560;
		symboltable = 58180;
		symbolbias = 2;
		break;
	case 0xdfc6e7005e22bd2fULL:
		initoffset = 0x110c;
		slicebase = 0x12000;
		stubstart = 0x9518;
		stubcount = 69;
		symboltable = slicebase + 58316;
		symbolbias = 4;
		interval = 0x1cdb8;
		break;
	default:
		free(image);
		return 0;
	}
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	if (!fork) {
		free(image);
		return 0;
	}
	OsxGuestParseFork(record + 88, OsxGuestBig32(record + 8), 0, fork);
	OsxGuestCompleteFork(volume, fork);
	bus = &machine->ata[bootbus];
	drive = &bus->drives[bootunit];
	originalcount = bus->patchcount;
	for (index = 0; index < stubcount; index++) {
		call = OsxGuestBig32(image + symboltable + index * 4) - symbolbias;
		if (call < 45 || call > 85 || call == 63 || call == 84)
			continue;
		stub = stubstart + (index + 1) * stubsize - 4;
		OsxGuestStoreBig32(replacement, OSXFLURRYTRAP | (call << 8));
		if (!OsxGuestAddGuestFilePatch(volume, fork, bus, drive, slicebase + stub, replacement, 4))
			break;
		machine->flurrystubs[call] = stub;
	}
	OsxGuestStoreBig32(replacement, OSXFLURRYTRAP);
	if (index == stubcount && OsxGuestAddGuestFilePatch(volume, fork, bus, drive, slicebase + initoffset, replacement, 4)) {
		OsxGuestStoreBig32(replacement, 0x3f911111);
		OsxGuestStoreBig32(replacement + 4, 0x11111111);
		if (OsxGuestAddGuestFilePatch(volume, fork, bus, drive, interval, replacement, 8))
			machine->flurryinit = initoffset;
	}
	if (!machine->flurryinit)
		bus->patchcount = originalcount;
	free(fork);
	free(image);
	return bus->patchcount != originalcount;
}

static int OsxGuestAddGuestFileBytes(OsxHfsVolume *volume, const OsxHfsFork *fork, OsxAtaBus *bus,
	const OsxAtaDrive *drive, uint32 offset, const uint8 *bytes, uint32 length)
{
	uint32 chunk;
	while (length) {
		chunk = 16;
		if (chunk > length)
			chunk = length;
		if (!OsxGuestAddGuestFilePatch(volume, fork, bus, drive, offset, bytes, (int)chunk))
			return 0;
		offset += chunk;
		bytes += chunk;
		length -= chunk;
	}
	return 1;
}

static uint32 OsxGuestFindPrivateFolder(OsxHfsVolume *volume)
{
	static const char privatename[] = "HFS+ Private Data";
	uint32 records;
	uint32 index;
	uint32 offset;
	uint32 position;
	uint8 *data;
	uint8 *key;
	uint8 *body;
	if (!OsxGuestDescendCatalog(volume, HFSROOTFOLDER, NULL, 0))
		return 0;
	for (;;) {
		data = volume->node;
		records = OsxGuestBig16(data + 10);
		for (index = 0; index < records; index++) {
			offset = OsxGuestBig16(data + volume->catalognodesize - 2 * (index + 1));
			key = data + offset;
			if (OsxGuestBig32(key + 2) < HFSROOTFOLDER)
				continue;
			if (OsxGuestBig32(key + 2) > HFSROOTFOLDER)
				return 0;
			body = key + 2 + OsxGuestBig16(key);
			if (OsxGuestBig16(body) != HFSFOLDERRECORD || OsxGuestBig16(key + 6) != 21 || OsxGuestBig16(key + 8) != 0)
				continue;
			position = 0;
			while (position < 17 && OsxGuestBig16(key + 16 + position * 2) == (uint8)privatename[position])
				position++;
			if (position == 17)
				return OsxGuestBig32(body + 8);
		}
		offset = OsxGuestBig32(data);
		if (offset == 0 || !OsxGuestReadFork(volume, &volume->catalog, (uint64)offset * volume->catalognodesize, volume->node, volume->catalognodesize))
			return 0;
	}
}

static int OsxGuestResolveHardLink(OsxHfsVolume *volume, uint8 *record)
{
	char name[32];
	uint32 folder;
	if (memcmp(record + 48, "hlnk", 4) != 0)
		return 1;
	folder = OsxGuestFindPrivateFolder(volume);
	if (!folder)
		return 0;
	sprintf(name, "iNode%u", OsxGuestBig32(record + 44));
	return OsxGuestFindCatalogRecord(volume, folder, name, record);
}

static int OsxGuestPatchGuestRecord(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive,
	const uint8 *record, uint32 expectedsize, uint64 expectedhash, const uint32 patches[][2], int count)
{
	uint8 replacement[4];
	uint8 *image;
	uint32 size;
	uint64 hash;
	int index;
	int originalcount;
	OsxHfsFork *fork;
	if (OsxGuestBig32(record + 88) != 0 || OsxGuestBig32(record + 92) != expectedsize)
		return 0;
	image = OsxGuestReadRecord(volume, record, &size);
	if (!image)
		return 0;
	hash = OsxGuestFileHash(image, size);
	free(image);
	if (hash != expectedhash)
		return 0;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	if (!fork)
		return 0;
	OsxGuestParseFork(record + 88, OsxGuestBig32(record + 8), 0, fork);
	OsxGuestCompleteFork(volume, fork);
	originalcount = bus->patchcount;
	for (index = 0; index < count; index++) {
		OsxGuestStoreBig32(replacement, patches[index][1]);
		if (!OsxGuestAddGuestFilePatch(volume, fork, bus, drive, patches[index][0], replacement, 4))
			break;
	}
	if (index != count)
		bus->patchcount = originalcount;
	free(fork);
	return index == count;
}

static int OsxGuestPatchSaverFile(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive,
	const char *path, uint32 expectedsize, uint64 expectedhash, const uint32 patches[][2], int count)
{
	uint8 record[248];
	if (!OsxGuestLookupPath(volume, path, record) || OsxGuestBig16(record) != HFSFILERECORD)
		return 0;
	return OsxGuestPatchGuestRecord(volume, bus, drive, record, expectedsize, expectedhash, patches, count);
}

static void OsxGuestPrepareJaguarLogin(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive)
{
	/*
		This fixes an issue in 10.2 where the menu bar never gets 
		installed properly if a LSUIElement calls SetFrontProcess()
		before Finder's startup event flush.
		
		This happens in stock 10.2 by Internet Reminder where on run 3+
		it calls SetFrontProcess() then sits in its event loop forever.
		Doing this causes the menubar gets partially rendered, or 
		namely only the parts where the LSUIElements are, and
		stays that way because SetProcessInternal() deactivates Finder
		and the menu bar is never installed.
	*/
	const uint32 loginwindowpatches[][2] = { { 0xa4d8, 0x48000018 } };
	uint8 record[248];
	if (!OsxGuestLookupPath(volume, "/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow", record) ||
		OsxGuestBig16(record) != HFSFILERECORD || !OsxGuestResolveHardLink(volume, record) || OsxGuestBig16(record) != HFSFILERECORD)
		return;
	OsxGuestPatchGuestRecord(volume, bus, drive, record, 581716, 0x1aaa075266432b21ULL, loginwindowpatches, 1);
}

static int OsxGuestPrepareSaverSoftware(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive)
{
	const char *pictures = "/System/Library/Frameworks/ScreenSaver.framework/Versions/A/Resources/Pictures Folder.saver/Contents/MacOS/Pictures Folder";
	const char *quartz = "/System/Library/Frameworks/ScreenSaver.framework/Versions/A/Resources/.Quartz Composer.saver/Contents/MacOS/.Quartz Composer";
	const char *framework = "/System/Library/Frameworks/ScreenSaver.framework/Versions/A/ScreenSaver";
	const uint32 picturepatches[][2] = { { 0xd9b44, 5 }, { 0xd9b48, 0 }, { 0xd9b6c, 0 },
		{ 0xd9b80, 5 }, { 0xd9b84, 0 } };
	const uint32 tigerquartz[][2] = { { 0x1250, 0x38600001 }, { 0x1360, 0x38000005 }, { 0x137c, 0x38000005 } };
	const uint32 tigerframework[][2] = { { 0x26cc, 0x38600001 }, { 0x2a3c, 0x38600001 } };
	const uint32 tigerlatestquartz[][2] = { { 0x2014, 0x38600001 }, { 0x2124, 0x38000005 }, { 0x2140, 0x38000005 } };
	const uint32 leopardquartz[][2] = { { 0x14828, 0x38600001 } };
	const uint32 leopardframework[][2] = { { 0x27f44, 0x38600001 } };
	int originalcount;
	int patchedtigerlatest;
	patchedtigerlatest = 0;
	OsxGuestPatchSaverFile(volume, bus, drive, pictures, 1014160, 0x42c190735c60f442ULL, picturepatches, 5);
	originalcount = bus->patchcount;
	if (OsxGuestPatchSaverFile(volume, bus, drive, quartz, 33976, 0x4c766095c8d63338ULL, tigerquartz, 3)) {
		if (!OsxGuestPatchSaverFile(volume, bus, drive, framework, 60652, 0xc090355bd97bedcbULL, tigerframework, 2))
			bus->patchcount = originalcount;
	} else if (OsxGuestPatchSaverFile(volume, bus, drive, quartz, 34092, 0x66628d56f0924315ULL, tigerlatestquartz, 3)) {
		patchedtigerlatest = 1;
	} else if (OsxGuestPatchSaverFile(volume, bus, drive, quartz, 121728, 0x720742ad028b4e2fULL, leopardquartz, 1)) {
		if (!OsxGuestPatchSaverFile(volume, bus, drive, framework, 293936, 0x580d148d44d3a1ebULL, leopardframework, 1))
			bus->patchcount = originalcount;
	}
	return patchedtigerlatest;
}

static uint8 *OsxGuestScanForRom(void *file, uint32 *size)
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

static const char *OsxGuestBootImagePath(int diskindex)
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
		OsxGuestProbeBootImage(path, &partition, &attached);
		if (attached && used == diskindex)
			return path;
		if (attached)
			used++;
	}
	return NULL;
}

uint8 *OsxGuestReadClassicRom(uint32 *size)
{
	OsxHfsVolume volume;
	uint8 block[512];
	uint8 record[248];
	const char *path;
	void *file;
	uint8 *rom;
	int diskindex;
	int partition;
	if (!(OsxGuestSelectBootImage(&diskindex, &partition) & OSXBOOTCLASSIC))
		return NULL;
	path = OsxGuestBootImagePath(diskindex);
	if (path == NULL)
		return NULL;
	if (path[0] == '*')
		path++;
	file = Sys_open(path, true, false);
	if (file == NULL)
		return NULL;
	rom = NULL;
	if (OsxGuestOpenVolume(&volume, file, partition) && OsxGuestReadVolume(&volume, 1024, block, 512) &&
		OsxGuestFindCatalogRecord(&volume, OsxGuestBig32(block + 80), "Mac OS ROM", record) && OsxGuestBig16(record) == HFSFILERECORD)
		rom = OsxGuestReadRecord(&volume, record, size);
	if (rom == NULL)
		rom = OsxGuestScanForRom(file, size);
	OsxGuestCloseVolume(&volume);
	Sys_close(file);
	return rom;
}

static int OsxGuestPowerPCSlice(const uint8 *data, uint32 size, uint32 *offset, uint32 *length)
{
	uint32 count;
	uint32 index;
	*offset = 0;
	*length = size;
	if (data == NULL || size < 8 || OsxGuestBig32(data) != 0xcafebabe)
		return 1;
	count = OsxGuestBig32(data + 4);
	for (index = 0; index < count && 8 + index * 20 + 20 <= size; index++) {
		*offset = OsxGuestBig32(data + 8 + index * 20 + 8);
		*length = OsxGuestBig32(data + 8 + index * 20 + 12);
		if (OsxGuestBig32(data + 8 + index * 20) == 18 && *offset <= size && *length <= size - *offset)
			return 1;
	}
	return 0;
}

static int OsxGuestThinPowerPC(uint8 *data, uint32 *size)
{
	uint32 offset;
	uint32 length;
	if (!OsxGuestPowerPCSlice(data, *size, &offset, &length))
		return 0;
	if (data && offset)
		memmove(data, data + offset, length);
	*size = length;
	return 1;
}

static uint8 *OsxGuestReadFileRange(OsxHfsVolume *volume, const char *path, uint32 offset, uint32 length)
{
	uint8 record[248];
	OsxHfsFork *fork;
	uint8 *data;
	if (!OsxGuestLookupPath(volume, path, record) || OsxGuestBig16(record) != HFSFILERECORD)
		return NULL;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	OsxGuestParseFork(record + 88, OsxGuestBig32(record + 8), 0, fork);
	OsxGuestCompleteFork(volume, fork);
	data = NULL;
	if (offset < fork->logical) {
		if ((uint64)length > fork->logical - offset)
			length = (uint32)(fork->logical - offset);
		data = (uint8 *)malloc((size_t)length + 1);
		if (data && !OsxGuestReadFork(volume, fork, offset, data, length)) {
			free(data);
			data = NULL;
		}
		if (data)
			data[length] = 0;
	}
	free(fork);
	return data;
}

static uint32 OsxGuestCacheBase(OsxHfsVolume *volume, const char *path)
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
	header = OsxGuestReadFileRange(volume, CLIPCACHE, 0, 32);
	if (header == NULL)
		return 0;
	imageoffset = OsxGuestBig32(header + 24);
	imagecount = OsxGuestBig32(header + 28);
	if (memcmp(header, "dyld_v1", 7) != 0 || imagecount == 0 || imagecount > CLIPCACHEIMAGES) {
		free(header);
		return 0;
	}
	free(header);
	images = OsxGuestReadFileRange(volume, CLIPCACHE, imageoffset, imagecount * 32);
	if (images == NULL)
		return 0;
	low = 0xffffffffu;
	high = 0;
	for (index = 0; index < imagecount; index++) {
		offset = OsxGuestBig32(images + index * 32 + 24);
		if (offset < low)
			low = offset;
		if (offset > high)
			high = offset;
	}
	address = 0;
	paths = OsxGuestReadFileRange(volume, CLIPCACHE, low, high - low + HFSMAXNAME * 4);
	for (index = 0; paths && index < imagecount && address == 0; index++)
		if (strcmp((const char *)paths + OsxGuestBig32(images + index * 32 + 24) - low, path) == 0)
			address = OsxGuestBig32(images + index * 32 + 4);
	free(paths);
	free(images);
	return address;
}

static uint32 OsxGuestTextCommand(const uint8 *image, uint32 size)
{
	uint32 commands;
	uint32 position;
	uint32 index;
	commands = OsxGuestBig32(image + 16);
	position = 28;
	for (index = 0; index < commands && position + 32 <= size; index++) {
		if (OsxGuestBig32(image + position) == 1 && strcmp((const char *)image + position + 8, "__TEXT") == 0)
			return position;
		position += OsxGuestBig32(image + position + 4);
	}
	return 0;
}

static void OsxGuestFindSymbols(const uint8 *image, uint32 size, uint32 base, const char *const *names, int count, uint32 *values, uint32 *textlow, uint32 *texthigh)
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
	if (size < 28 || OsxGuestBig32(image) != 0xfeedface)
		return;
	text = OsxGuestTextCommand(image, size);
	slide = 0;
	if (base && text)
		slide = base - OsxGuestBig32(image + text + 24);
	if (text && textlow) {
		*textlow = OsxGuestBig32(image + text + 24) + slide;
		*texthigh = *textlow + OsxGuestBig32(image + text + 28);
	}
	commands = OsxGuestBig32(image + 16);
	position = 28;
	for (index = 0; index < commands && position + 24 <= size; index++) {
		if (OsxGuestBig32(image + position) == 2) {
			symbols = OsxGuestBig32(image + position + 8);
			symbolcount = OsxGuestBig32(image + position + 12);
			strings = OsxGuestBig32(image + position + 16);
			for (; symbolcount > 0 && symbols + 12 <= size; symbolcount--, symbols += 12) {
				name = strings + OsxGuestBig32(image + symbols);
				if ((image[symbols + 4] & 0x0f) != 0x0f || name >= size)
					continue;
				for (wanted = 0; wanted < count; wanted++)
					if (values[wanted] == 0 && strcmp((const char *)image + name, names[wanted]) == 0)
						values[wanted] = OsxGuestBig32(image + symbols + 8) + slide;
			}
		}
		position += OsxGuestBig32(image + position + 4);
	}
}

#define QUARTZLIBRARY "/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib"
#define QUARTZFRAMEWORK "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL"
#define QUARTZMAXPATCHES 1024
#define QUARTZMOVELINK 0x7c0802a6u
#define QUARTZBRANCHLINK 0x4e800020u
#define QUARTZBRANCHCOUNT 0x4e800420u
#define QUARTZCALLCOUNT 0x4e800421u
#define QUARTZCALLLINK 0x4e800021u

typedef struct OsxQuartzImage {
	uint8 *file;
	uint8 *image;
	uint32 size;
	uint32 slice;
	uint32 text;
	uint32 address;
	uint32 patchcount;
	uint32 patches[QUARTZMAXPATCHES][2];
} OsxQuartzImage;

static int OsxGuestQuartzOpenImage(OsxHfsVolume *volume, const char *path, OsxQuartzImage *image)
{
	uint32 size;
	image->file = OsxGuestReadFile(volume, path, &size);
	if (!image->file || !OsxGuestPowerPCSlice(image->file, size, &image->slice, &image->size) || image->size < 28)
		return 0;
	image->image = image->file + image->slice;
	if (OsxGuestBig32(image->image) != 0xfeedface)
		return 0;
	image->text = OsxGuestTextCommand(image->image, image->size);
	if (!image->text)
		return 0;
	image->address = OsxGuestBig32(image->image + image->text + 24);
	return 1;
}

static int OsxGuestQuartzAddPatch(OsxQuartzImage *image, uint32 offset, uint32 value)
{
	if (image->patchcount >= QUARTZMAXPATCHES || offset + 4 > image->size)
		return 0;
	image->patches[image->patchcount][0] = offset;
	image->patches[image->patchcount][1] = value;
	image->patchcount++;
	return 1;
}

static uint32 OsxGuestQuartzSymbolTable(const OsxQuartzImage *image, uint32 *count, uint32 *strings)
{
	uint32 position;
	uint32 index;
	position = 28;
	for (index = 0; index < OsxGuestBig32(image->image + 16) && position + 24 <= image->size; index++) {
		if (OsxGuestBig32(image->image + position) == 2) {
			*count = OsxGuestBig32(image->image + position + 12);
			*strings = OsxGuestBig32(image->image + position + 16);
			return OsxGuestBig32(image->image + position + 8);
		}
		position += OsxGuestBig32(image->image + position + 4);
	}
	*count = 0;
	*strings = 0;
	return 0;
}

static uint32 OsxGuestQuartzSymbol(const OsxQuartzImage *image, const char *name)
{
	uint32 symbols;
	uint32 count;
	uint32 strings;
	symbols = OsxGuestQuartzSymbolTable(image, &count, &strings);
	for (; count > 0 && symbols + 12 <= image->size; count--, symbols += 12)
		if ((image->image[symbols + 4] & 0x0e) == 0x0e && strings + OsxGuestBig32(image->image + symbols) < image->size &&
			strcmp((const char *)image->image + strings + OsxGuestBig32(image->image + symbols), name) == 0)
			return OsxGuestBig32(image->image + symbols + 8) - image->address;
	return 0;
}

static uint32 OsxGuestQuartzLinkWord(const OsxQuartzImage *image, uint32 entry)
{
	uint32 position;
	for (position = entry; entry && position < entry + 16 && position + 4 <= image->size; position += 4)
		if (OsxGuestBig32(image->image + position) == QUARTZMOVELINK)
			return position;
	return 0;
}

static uint32 OsxGuestQuartzStubSlot(const OsxQuartzImage *image, uint32 start, uint32 *tail, int *called)
{
	uint32 position;
	uint32 word;
	uint32 target;
	int link;
	int found;
	found = 0;
	word = 0;
	for (position = start; position + 4 <= image->size && position < start + 1024; position += 4) {
		word = OsxGuestBig32(image->image + position);
		if (word == QUARTZBRANCHLINK)
			return 0;
		if (word == QUARTZBRANCHCOUNT || word == QUARTZCALLCOUNT || word == QUARTZCALLLINK) {
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;
	*tail = position;
	*called = word != QUARTZBRANCHCOUNT;
	link = word == QUARTZCALLLINK;
	target = 32;
	while (position > start) {
		position -= 4;
		word = OsxGuestBig32(image->image + position);
		if (target == 32) {
			if ((link && (word & 0xfc1fffffu) == 0x7c0803a6u) || (!link && (word & 0xfc1fffffu) == 0x7c0903a6u))
				target = (word >> 21) & 31;
		} else if ((word >> 26) == 32 && ((word >> 21) & 31) == target && ((word >> 16) & 31) != 1 &&
			!(word & 3) && (word & 0xffff) >= 4)
			return (word & 0xffff) / 4;
	}
	return 0;
}

static uint32 OsxGuestQuartzHeaderEnd(const OsxQuartzImage *image)
{
	uint32 index;
	uint32 first;
	uint32 address;
	first = OsxGuestBig32(image->image + image->text + 28);
	for (index = 0; index < OsxGuestBig32(image->image + image->text + 48); index++) {
		address = OsxGuestBig32(image->image + image->text + 56 + index * 68 + 32) - image->address;
		if (address < first)
			first = address;
	}
	return first;
}

static int OsxGuestQuartzLibraryPatches(OsxMachine *machine, OsxQuartzImage *image, uint32 stringsize)
{
	uint8 *wanted;
	uint32 symbols;
	uint32 count;
	uint32 strings;
	uint32 name;
	uint32 slot;
	uint32 tail;
	uint32 highest;
	uint32 pads;
	uint32 cursor;
	uint32 end;
	uint32 position;
	const char *symbol;
	int called;
	int result;
	wanted = (uint8 *)calloc(OSXQUARTZSLOTS, 1);
	if (!wanted)
		return 0;
	highest = 0;
	symbols = OsxGuestQuartzSymbolTable(image, &count, &strings);
	for (; count > 0 && symbols + 12 <= image->size; count--, symbols += 12) {
		name = strings + OsxGuestBig32(image->image + symbols);
		if ((image->image[symbols + 4] & 0x0e) != 0x0e || name + 4 >= image->size ||
			OsxGuestBig32(image->image + symbols + 8) - image->address >= image->size)
			continue;
		symbol = (const char *)image->image + name;
		if (symbol[0] != '_' || symbol[1] != 'g' || symbol[2] != 'l' || symbol[3] < 'A' || symbol[3] > 'Z')
			continue;
		slot = OsxGuestQuartzStubSlot(image, OsxGuestBig32(image->image + symbols + 8) - image->address, &tail, &called);
		if (!slot || slot >= OSXQUARTZSLOTS)
			continue;
		if (slot > highest)
			highest = slot;
		if (called)
			wanted[slot] = 1;
		else if (!machine->quartztails[slot])
			machine->quartztails[slot] = tail;
	}
	if (highest < 500) {
		for (slot = 400; slot < OSXQUARTZSLOTS; slot++) {
			wanted[slot] = 0;
			machine->quartztails[slot] = 0;
		}
	}
	pads = 0;
	result = 1;
	for (slot = 1; slot < OSXQUARTZSLOTS; slot++) {
		if (machine->quartztails[slot])
			result &= OsxGuestQuartzAddPatch(image, machine->quartztails[slot], OSXQUARTZTRAP | (slot << 8));
		else if (wanted[slot])
			pads++;
	}
	end = OsxGuestQuartzHeaderEnd(image);
	cursor = 28 + OsxGuestBig32(image->image + 20);
	for (position = cursor; position + 4 <= end; position += 4)
		if (OsxGuestBig32(image->image + position))
			result = 0;
	cursor = (cursor + 15) & ~15u;
	machine->quartzextensions = cursor + stringsize + pads * 4 <= end;
	if (machine->quartzextensions) {
		machine->quartzstrings = cursor;
		cursor += stringsize;
	} else if (cursor + stringsize - OSXQUARTZRENDERERSTRING + OSXQUARTZSTRINGS + pads * 4 <= end) {
		machine->quartzstrings = cursor + OSXQUARTZSTRINGS - OSXQUARTZRENDERERSTRING;
		cursor += stringsize - OSXQUARTZRENDERERSTRING + OSXQUARTZSTRINGS;
	}
	for (slot = 1; slot < OSXQUARTZSLOTS && cursor + 4 <= end; slot++) {
		if (wanted[slot] && !machine->quartztails[slot]) {
			machine->quartztails[slot] = cursor;
			result &= OsxGuestQuartzAddPatch(image, cursor, OSXQUARTZTRAP | (slot << 8));
			cursor += 4;
		}
	}
	free(wanted);
	machine->quartztails[0] = OsxGuestQuartzLinkWord(image, OsxGuestQuartzSymbol(image, "_CGLSetCurrentContext"));
	if (!machine->quartztails[0])
		return 0;
	return result && OsxGuestQuartzAddPatch(image, machine->quartztails[0], OSXQUARTZTRAP);
}

#define QUARTZHOOKS 11

static int OsxGuestQuartzFrameworkPatches(OsxQuartzImage *image)
{
	static const char *const hooks[QUARTZHOOKS] = { "_CGLGetParameter", "_CGLSetPBuffer", "_CGLTexImagePBuffer", "_CGLFlushDrawable", "_CGLDescribeRenderer",
		"_CGLChoosePixelFormat", "_CGLDestroyContext", "_CGLSetOffScreen", "_CGLSetSurface", "_CGLDescribePixelFormat", "_CGLSetFullScreen" };
	static const uint32 calls[QUARTZHOOKS] = { 1023, 1019, 1020, 1018, 1017, 1016, 1015, 1014, 1013, 1012, 1011 };
	uint32 entry;
	uint32 position;
	uint32 word;
	int stepped;
	int index;
	int result;
	result = 1;
	for (index = 0; index < QUARTZHOOKS; index++) {
		entry = OsxGuestQuartzLinkWord(image, OsxGuestQuartzSymbol(image, hooks[index]));
		if (entry)
			result &= OsxGuestQuartzAddPatch(image, entry, OSXQUARTZTRAP | (calls[index] << 8));
	}
	entry = OsxGuestQuartzSymbol(image, "_cglAssignDispatch");
	stepped = 0;
	for (position = entry; entry && position < entry + 64 && position + 4 <= image->size; position += 4) {
		word = OsxGuestBig32(image->image + position);
		if (word == QUARTZMOVELINK)
			break;
		if (word == 0x38630004u)
			stepped = 1;
		if (word == QUARTZBRANCHLINK) {
			if (stepped)
				result &= OsxGuestQuartzAddPatch(image, position, OSXQUARTZTRAP | (1022u << 8));
			break;
		}
	}
	entry = OsxGuestQuartzSymbol(image, "_CGLSetPBufferVolatileState");
	for (position = entry; entry && position < entry + 256 && position + 4 <= image->size; position += 4) {
		if (OsxGuestBig32(image->image + position) == 0x39202721u) {
			result &= OsxGuestQuartzAddPatch(image, position, 0x39200000u);
			break;
		}
	}
	return result;
}

static uint32 OsxGuestCacheFileOffset(OsxHfsVolume *volume, uint32 address)
{
	uint8 *header;
	uint8 *mappings;
	uint32 offset;
	uint32 count;
	uint32 index;
	uint32 result;
	result = 0;
	header = OsxGuestReadFileRange(volume, CLIPCACHE, 0, 32);
	if (!header)
		return 0;
	offset = OsxGuestBig32(header + 16);
	count = OsxGuestBig32(header + 20);
	free(header);
	if (count > 16)
		return 0;
	mappings = OsxGuestReadFileRange(volume, CLIPCACHE, offset, count * 32);
	for (index = 0; mappings && index < count; index++) {
		offset = OsxGuestBig32(mappings + index * 32 + 4);
		if (address >= offset && address - offset < OsxGuestBig32(mappings + index * 32 + 12))
			result = OsxGuestBig32(mappings + index * 32 + 20) + address - offset;
	}
	free(mappings);
	return result;
}

static int OsxGuestQuartzApplyImage(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive, const char *path,
	const OsxQuartzImage *image, uint32 base, const uint8 *cached, const uint8 *bytes, uint32 bytesoffset, uint32 length)
{
	uint8 record[248];
	uint8 replacement[4];
	OsxHfsFork *fork;
	uint32 index;
	uint32 extent;
	int result;
	if (!OsxGuestLookupPath(volume, path, record) || OsxGuestBig16(record) != HFSFILERECORD)
		return 0;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	if (!fork)
		return 0;
	OsxGuestParseFork(record + 88, OsxGuestBig32(record + 8), 0, fork);
	OsxGuestCompleteFork(volume, fork);
	extent = OsxGuestBig32(image->image + image->text + 36);
	result = 1;
	for (index = 0; result && index < image->patchcount; index++) {
		if (cached && (image->patches[index][0] + 4 > extent ||
			OsxGuestBig32(cached + image->patches[index][0]) != OsxGuestBig32(image->image + image->patches[index][0])))
			result = 0;
		OsxGuestStoreBig32(replacement, image->patches[index][1]);
		if (result)
			result = OsxGuestAddGuestFilePatch(volume, fork, bus, drive, base + image->patches[index][0], replacement, 4);
	}
	if (result && bytes)
		result = OsxGuestAddGuestFileBytes(volume, fork, bus, drive, base + bytesoffset, bytes, length);
	free(fork);
	return result;
}

static int OsxGuestQuartzApplyCache(OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive,
	const OsxQuartzImage *image, uint32 address, const uint8 *bytes, uint32 bytesoffset, uint32 length)
{
	uint8 *cached;
	uint32 base;
	int result;
	base = OsxGuestCacheFileOffset(volume, address);
	if (!base)
		return 0;
	cached = OsxGuestReadFileRange(volume, CLIPCACHE, base, OsxGuestBig32(image->image + image->text + 36));
	if (!cached)
		return 0;
	result = OsxGuestQuartzApplyImage(volume, bus, drive, CLIPCACHE, image, base, cached, bytes, bytesoffset, length);
	free(cached);
	return result;
}

static int OsxGuestPrepareQuartz(OsxMachine *machine, OsxHfsVolume *volume, OsxAtaBus *bus, const OsxAtaDrive *drive)
{
	static const uint32 rendererpatch[][2] = {
		{0x13090, 0x38000f00},
		{0x13094, 0x3840075d},
		{0x130b0, 0x91230034},
		{0x130c8, 0x91230030},
		{0x12094, 0x3b8005d8},
		{0x12564, 0x61290f00},
		{0x12628, 0x614a0f00},
		{0x126c4, 0x60000f00},
		{0x13124, 0x38400f00}
	};
	static const uint32 enginepatch[][2] = {
		{0x3200, 0x2f800f00},
		{0x3324, 0x38600f00},
		{0x332c, 0x3ba00f00}
	};
	static const char extensions[] = OSXQUARTZEXTENSIONS;
	uint8 strings[OSXQUARTZSTRINGEND - OSXQUARTZSTRINGS];
	OsxQuartzImage *library;
	OsxQuartzImage *framework;
	const uint8 *stringbytes;
	uint32 stringoffset;
	uint32 stringlength;
	uint32 cachelibrary;
	uint32 cacheframework;
	int originalcount;
	int renderercount;
	int result;
	memset(strings, 0, sizeof(strings));
	memcpy(strings, extensions, sizeof(extensions));
	memcpy(strings + OSXQUARTZVERSIONSTRING - OSXQUARTZSTRINGS, OSXQUARTZVERSION, sizeof(OSXQUARTZVERSION));
	library =(OsxQuartzImage *)calloc(1, sizeof(OsxQuartzImage));
	framework = (OsxQuartzImage *)calloc(1, sizeof(OsxQuartzImage));
	originalcount = bus->patchcount;
	memset(machine->quartztails, 0, sizeof(machine->quartztails));
	machine->quartzstrings = 0;
	result = library && framework && OsxGuestQuartzOpenImage(volume, QUARTZLIBRARY, library) &&
		OsxGuestQuartzOpenImage(volume, QUARTZFRAMEWORK, framework) &&
		OsxGuestQuartzLibraryPatches(machine, library, sizeof(strings)) && OsxGuestQuartzFrameworkPatches(framework);
	if (result) {
		stringbytes = NULL;
		stringoffset = machine->quartzstrings;
		stringlength = sizeof(strings);
		if (machine->quartzstrings) {
			stringbytes = strings;
			if (!machine->quartzextensions) {
				stringbytes = strings + OSXQUARTZRENDERERSTRING - OSXQUARTZSTRINGS;
				stringoffset += OSXQUARTZRENDERERSTRING - OSXQUARTZSTRINGS;
				stringlength -= OSXQUARTZRENDERERSTRING - OSXQUARTZSTRINGS;
			}
		}
		machine->quartzbase = library->address;
		cachelibrary = OsxGuestCacheBase(volume, QUARTZLIBRARY);
		cacheframework = OsxGuestCacheBase(volume, QUARTZFRAMEWORK);
		result = OsxGuestQuartzApplyImage(volume, bus, drive, QUARTZLIBRARY, library, library->slice, NULL, stringbytes,
			stringoffset, stringlength) &&
			OsxGuestQuartzApplyImage(volume, bus, drive, QUARTZFRAMEWORK, framework, framework->slice, NULL, NULL, 0, 0);
		if (result && cachelibrary && cacheframework) {
			machine->quartzbase = cachelibrary;
			result = OsxGuestQuartzApplyCache(volume, bus, drive, library, cachelibrary, stringbytes, stringoffset, stringlength) &&
				OsxGuestQuartzApplyCache(volume, bus, drive, framework, cacheframework, NULL, 0, 0);
		}
	}
	if (library)
		free(library->file);
	if (framework)
		free(framework->file);
	free(library);
	free(framework);
	if (!result) {
		bus->patchcount = originalcount;
		return 0;
	}
	renderercount = bus->patchcount;
	if (!OsxGuestPatchSaverFile(volume, bus, drive,
		"/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLEngine.bundle/GLEngine",
		7204432, 0xaedd8ac3552c17bbULL, enginepatch, sizeof(enginepatch) / sizeof(enginepatch[0])) ||
		!OsxGuestPatchSaverFile(volume, bus, drive,
		"/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLRendererFloat.bundle/GLRendererFloat",
		573388, 0x295fd7ed200daf77ULL, rendererpatch, sizeof(rendererpatch) / sizeof(rendererpatch[0])))
		bus->patchcount = renderercount;
	if (machine->log)
		fprintf(machine->log, "quartz prepared %d disk overlays base %08x strings %x\n", bus->patchcount - originalcount, machine->quartzbase, machine->quartzstrings);
	return 1;
}

static void OsxGuestFindClipboardFunctions(OsxMachine *machine, OsxHfsVolume *volume)
{
	static const char *const systemnames[3] = { "_malloc", "_free", "_bzero" };
	static const char *const windownames[OSXQUARTZWINDOWCALLS] = { "_CGSGetScreenRectForWindow", "_CGSGetSurfaceBounds", "_CGSGetOnScreenWindowList", "_CGSGetWindowOpacity" };
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
	image = OsxGuestReadFile(volume, CLIPSYSTEM, &size);
	if (image && OsxGuestThinPowerPC(image, &size))
		OsxGuestFindSymbols(image, size, OsxGuestCacheBase(volume, CLIPSYSTEM), systemnames, 3, clip->functions + OSXCLIPMALLOC, NULL, NULL);
	free(image);
	image = OsxGuestReadFile(volume, CLIPTOOLBOX, &size);
	if (image && OsxGuestThinPowerPC(image, &size))
		OsxGuestFindSymbols(image, size, OsxGuestCacheBase(volume, CLIPTOOLBOX), toolboxnames, 5, clip->functions + OSXCLIPGETCURRENT, &clip->textlow, &clip->texthigh);
	free(image);
	image = OsxGuestReadFile(volume, CLIPDOCK, &size);
	if (image && OsxGuestThinPowerPC(image, &size) && size >= 28 && OsxGuestBig32(image) == 0xfeedface && OsxGuestTextCommand(image, size)) {
		clip->functions[OSXCLIPDOCK] = OsxGuestBig32(image + OsxGuestTextCommand(image, size) + 24);
		clip->functions[OSXCLIPDOCKCOMMANDS] = OsxGuestBig32(image + 20);
	}
	free(image);
	memset(machine->quartzwindow, 0, sizeof(machine->quartzwindow));
	image = OsxGuestReadFile(volume, QUARTZCOREGRAPHICS, &size);
	if (image && OsxGuestThinPowerPC(image, &size))
		OsxGuestFindSymbols(image, size, OsxGuestCacheBase(volume, QUARTZCOREGRAPHICS), windownames, OSXQUARTZWINDOWCALLS, machine->quartzwindow, NULL, NULL);
	free(image);
	clip->ready = clip->texthigh > clip->textlow;
	for (index = 0; index < OSXCLIPFUNCTIONS; index++)
		if (clip->functions[index] == 0)
			clip->ready = 0;
	if (machine->log)
		fprintf(machine->log, "clipboard ready %d toolbox %08x-%08x scrap %08x malloc %08x dock %08x\n", clip->ready, clip->textlow, clip->texthigh, clip->functions[OSXCLIPGETCURRENT], clip->functions[OSXCLIPMALLOC], clip->functions[OSXCLIPDOCK]);
}

static int OsxGuestPlistString(const uint8 *plist, const char *key, char *output, uint32 size)
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

static void OsxGuestCollectKexts(OsxHfsVolume *volume, const char *folder, OsxKextList *list, int depth)
{
	uint8 record[248];
	char path[BOOTKEXTPATH];
	char required[BOOTKEXTNAME];
	char *names;
	OsxKextCandidate *kext;
	int count;
	int index;
	if (depth > 3 || !OsxGuestLookupPath(volume, folder, record) || OsxGuestBig16(record) != HFSFOLDERRECORD)
		return;
	names = (char *)malloc(BOOTMAXKEXTS * BOOTKEXTNAME);
	count = OsxGuestListKextFolders(volume, OsxGuestBig32(record + 8), names, BOOTMAXKEXTS);
	for (index = 0; index < count && list->count < BOOTMAXKEXTS; index++) {
		kext = &list->kexts[list->count];
		kext->executable[0] = 0;
		if (strlen(folder) + strlen(names + index * BOOTKEXTNAME) + 32 >= BOOTKEXTPATH)
			continue;
		sprintf(kext->path, "%s/%s", folder, names + index * BOOTKEXTNAME);
		sprintf(path, "%s/Contents/Info.plist", kext->path);
		kext->plist = OsxGuestReadFile(volume, path, &kext->plistsize);
		kext->contents = kext->plist != NULL;
		if (kext->plist == NULL) {
			sprintf(path, "%s/Info.plist", kext->path);
			kext->plist = OsxGuestReadFile(volume, path, &kext->plistsize);
		}
		if (kext->plist == NULL || !OsxGuestPlistString(kext->plist, "CFBundleIdentifier", kext->identifier, BOOTKEXTNAME)) {
			free(kext->plist);
			continue;
		}
		OsxGuestPlistString(kext->plist, "CFBundleExecutable", kext->executable, BOOTKEXTNAME);
		kext->selected = OsxGuestPlistString(kext->plist, "OSBundleRequired", required, BOOTKEXTNAME) && (list->safeboot || strcmp(required, "Safe Boot") != 0);
		list->count++;
		sprintf(path, "%s/PlugIns", kext->path);
		if (kext->contents)
			sprintf(path, "%s/Contents/PlugIns", kext->path);
		OsxGuestCollectKexts(volume, path, list, depth + 1);
	}
	free(names);
}

static int OsxGuestSelectLibraries(OsxKextList *list, OsxKextCandidate *kext)
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

static int OsxGuestContainsBytes(const uint8 *data, uint32 size, const char *pattern)
{
	uint32 length;
	uint32 index;
	length = (uint32)strlen(pattern);
	for (index = 0; data != NULL && index + length <= size; index++)
		if (data[index] == (uint8)pattern[0] && memcmp(data + index, pattern, length) == 0)
			return 1;
	return 0;
}

static void OsxGuestUseBootFramebuffer(OsxKextCandidate *kext)
{
	char *found;
	found = (char *)kext->plist;
	while ((found = strstr(found, "<string>IONDRVFramebuffer</string>")) != NULL)
		memcpy(found + 8, "IOBoot", 6);
}

static uint32 OsxGuestLiteralLength(uint32 length)
{
	return length + (length + 7) / 8;
}

static uint8 *OsxGuestStoreEntry(uint8 *package, uint8 *field, uint32 position, const uint8 *data, uint32 length)
{
	uint8 *output;
	uint32 index;
	OsxGuestStoreBig32(field, position);
	OsxGuestStoreBig32(field + 4, OsxGuestLiteralLength(length));
	OsxGuestStoreBig32(field + 8, length);
	output = package + position;
	for (index = 0; index < length; index++) {
		if ((index & 7) == 0)
			*output++ = 0xff;
		*output++ = data[index];
	}
	return output;
}

static uint8 *OsxGuestBuildDriverPackage(OsxHfsVolume *volume, int safeboot, const char *const *only, int onlycount, uint32 *size)
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
	OsxGuestCollectKexts(volume, BOOTEXTENSIONS, &list, 0);
	for (index = 0; only && index < list.count; index++) {
		list.kexts[index].selected = 0;
		for (selected = 0; selected < onlycount; selected++)
			if (strcmp(list.kexts[index].identifier, only[selected]) == 0)
				list.kexts[index].selected = 1;
	}
	do {
		changed = 0;
		for (index = 0; index < list.count; index++)
			if (list.kexts[index].selected && OsxGuestSelectLibraries(&list, &list.kexts[index]))
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
			modules[index] = OsxGuestReadFile(volume, path, &modulesizes[index]);
			if (!OsxGuestThinPowerPC(modules[index], &modulesizes[index])) {
				free(modules[index]);
				modules[index] = NULL;
				modulesizes[index] = 0;
			}
		}
		if (strcmp(kext->identifier, "com.apple.iokit.IONDRVSupport") == 0 && !OsxGuestContainsBytes(modules[index], modulesizes[index], "IOBootNDRV"))
			OsxGuestUseBootFramebuffer(kext);
		total += 32 + OsxGuestLiteralLength(kext->plistsize + 1) + OsxGuestLiteralLength(modulesizes[index]);
		selected++;
	}
	package = NULL;
	if (selected) {
		package = (uint8 *)calloc(total, 1);
		OsxGuestStoreBig32(package, 0x4d4b5854);
		OsxGuestStoreBig32(package + 4, 0x4d4f5358);
		OsxGuestStoreBig32(package + 8, total);
		OsxGuestStoreBig32(package + 16, 0x01008000);
		OsxGuestStoreBig32(package + 20, (uint32)selected);
		OsxGuestStoreBig32(package + 24, 0xffffffff);
		OsxGuestStoreBig32(package + 28, 0xffffffff);
		entry = package + 32;
		position = 32 + 32 * (uint32)selected;
		for (index = 0; index < list.count; index++) {
			kext = &list.kexts[index];
			if (!kext->selected)
				continue;
			position = (uint32)(OsxGuestStoreEntry(package, entry, position, kext->plist, kext->plistsize + 1) - package);
			if (modules[index])
				position = (uint32)(OsxGuestStoreEntry(package, entry + 16, position, modules[index], modulesizes[index]) - package);
			entry += 32;
		}
		OsxGuestStoreBig32(package + 12, OsxGuestAdler32(package + 16, total - 16));
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

static void OsxGuestAddRange(OsxBootState *state, const char *name, uint32 address, uint32 length)
{
	OsxMemoryRange *range;
	if (state->rangecount >= BOOTMAXRANGES)
		return;
	range = &state->ranges[state->rangecount++];
	strncpy(range->name, name, TREENAMELENGTH - 1);
	range->address = address;
	range->length = length;
}

static uint32 OsxGuestBootAllocate(OsxBootState *state, uint32 length)
{
	uint32 address;
	address = state->nextfree;
	state->nextfree = (address + length + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	return address;
}

static int OsxGuestLoadKernel(OsxBootState *state, const uint8 *image, uint32 size)
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
	if (size < 28 || OsxGuestBig32(image) != 0xfeedface)
		return 0;
	commands = OsxGuestBig32(image + 16);
	offset = 28;
	kernelend = 0;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		command = OsxGuestBig32(image + offset);
		commandsize = OsxGuestBig32(image + offset + 4);
		if (command == 1) {
			memcpy(segment, image + offset + 8, 16);
			segment[16] = 0;
			vmaddr = OsxGuestBig32(image + offset + 24);
			vmsize = OsxGuestBig32(image + offset + 28);
			fileoffset = OsxGuestBig32(image + offset + 32);
			filesize = OsxGuestBig32(image + offset + 36);
			if (vmsize && vmaddr + vmsize <= machine->ramsize) {
				target = machine->ram + vmaddr;
				if (filesize && fileoffset + filesize <= size)
					memcpy(target, image + fileoffset, filesize);
				if (filesize < vmsize)
					memset(target + filesize, 0, vmsize - filesize);
				sprintf(rangename, "Kernel-%s", segment);
				OsxGuestAddRange(state, rangename, vmaddr, vmsize);
				if (vmaddr + vmsize > kernelend)
					kernelend = vmaddr + vmsize;
			}
		} else if (command == 5) {
			machine->kernelentry = OsxGuestBig32(image + offset + 16);
		} else if (command == 2) {
			symoffset = OsxGuestBig32(image + offset + 8);
			symcount = OsxGuestBig32(image + offset + 12);
			stroffset = OsxGuestBig32(image + offset + 16);
			strsize = OsxGuestBig32(image + offset + 20);
		}
		offset += commandsize;
	}
	state->nextfree = (kernelend + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	total = 28 + OsxGuestBig32(image + 20);
	address = OsxGuestBootAllocate(state, total);
	memcpy(machine->ram + address, image, total);
	OsxGuestAddRange(state, "Kernel-__HEADER", address, total);
	if (symcount && symoffset + symcount * 12 <= size && stroffset + strsize <= size) {
		total = 24 + symcount * 12 + strsize;
		address = OsxGuestBootAllocate(state, total);
		target = machine->ram + address;
		OsxGuestStoreBig32(target, 2);
		OsxGuestStoreBig32(target + 4, 24);
		OsxGuestStoreBig32(target + 8, address + 24);
		OsxGuestStoreBig32(target + 12, symcount);
		OsxGuestStoreBig32(target + 16, address + 24 + symcount * 12);
		OsxGuestStoreBig32(target + 20, strsize);
		memcpy(target + 24, image + symoffset, symcount * 12);
		memcpy(target + 24 + symcount * 12, image + stroffset, strsize);
		OsxGuestAddRange(state, "Kernel-__SYMTAB", address, total);
		machine->kernelsize = 52 + symcount * 12 + strsize;
		machine->kernelimage = (uint8 *)malloc(machine->kernelsize);
		if (machine->kernelimage == NULL)
			return 0;
		target = machine->kernelimage;
		memcpy(target, image, 28);
		OsxGuestStoreBig32(target + 16, 1);
		OsxGuestStoreBig32(target + 20, 24);
		OsxGuestStoreBig32(target + 28, 2);
		OsxGuestStoreBig32(target + 32, 24);
		OsxGuestStoreBig32(target + 36, 52);
		OsxGuestStoreBig32(target + 40, symcount);
		OsxGuestStoreBig32(target + 44, 52 + symcount * 12);
		OsxGuestStoreBig32(target + 48, strsize);
		memcpy(target + 52, image + symoffset, symcount * 12);
		memcpy(target + 52 + symcount * 12, image + stroffset, strsize);
	}
	return machine->kernelentry != 0;
}

uint32 OsxGuestFindKernelSymbol(OsxMachine *machine, const char *name)
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
	commands = OsxGuestBig32(image + 16);
	offset = 28;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		if (OsxGuestBig32(image + offset) == 2) {
			symoffset = OsxGuestBig32(image + offset + 8);
			symcount = OsxGuestBig32(image + offset + 12);
			stroffset = OsxGuestBig32(image + offset + 16);
			strsize = OsxGuestBig32(image + offset + 20);
		}
		offset += OsxGuestBig32(image + offset + 4);
	}
	for (symbol = 0; symbol < symcount; symbol++) {
		stringindex = OsxGuestBig32(image + symoffset + symbol * 12);
		if (stringindex < strsize && strcmp((const char *)image + stroffset + stringindex, name) == 0)
			return OsxGuestBig32(image + symoffset + symbol * 12 + 8);
	}
	return 0;
}

static OsxTreeNode *OsxGuestNewNode(OsxTreeNode *parent, const char *name)
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

static void OsxGuestAddProperty(OsxTreeNode *node, const char *name, const void *value, uint32 length)
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

static void OsxGuestAddString(OsxTreeNode *node, const char *name, const char *value)
{
	OsxGuestAddProperty(node, name, value, (uint32)strlen(value) + 1);
}

static void OsxGuestAddStrings(OsxTreeNode *node, const char *name, const char *first, const char *second)
{
	char buffer[256];
	uint32 length;
	length = (uint32)strlen(first) + 1;
	memcpy(buffer, first, length);
	memcpy(buffer + length, second, strlen(second) + 1);
	length += (uint32)strlen(second) + 1;
	OsxGuestAddProperty(node, name, buffer, length);
}

static void OsxGuestAddCells(OsxTreeNode *node, const char *name, const uint32 *cells, int count)
{
	uint8 buffer[256];
	int index;
	for (index = 0; index < count; index++)
		OsxGuestStoreBig32(buffer + index * 4, cells[index]);
	OsxGuestAddProperty(node, name, buffer, (uint32)count * 4);
}

static void OsxGuestAddCell(OsxTreeNode *node, const char *name, uint32 value)
{
	OsxGuestAddCells(node, name, &value, 1);
}

static OsxTreeNode *OsxGuestAddDisplayNode(OsxTreeNode *parent, OsxMachine *machine, int ignorendrv)
{
	OsxTreeNode *node;
	uint8 *driver;
	uint32 length;
	node = OsxGuestNewNode(parent, "SSHV,display");
	OsxGuestAddString(node, "device_type", "display");
	OsxGuestAddString(node, "compatible", "display");
	OsxGuestAddString(node, "model", "SheepShaver,Framebuffer");
	OsxGuestAddCell(node, "address", OSXVRAMBASE);
	OsxGuestAddCell(node, "width", machine->width);
	OsxGuestAddCell(node, "height", machine->height);
	OsxGuestAddCell(node, "linebytes", machine->rowbytes);
	OsxGuestAddCell(node, "depth", machine->depth);
	OsxGuestAddProperty(node, "AAPL,boot-display", "", 0);
	if (ignorendrv) {
		OsxGuestAddProperty(node, "AAPL,iokit-ignore-ndrv", "", 0);
		return node;
	}
	driver = OsxGuestDisplayDriver(&length);
	if (driver)
		OsxGuestAddProperty(node, "driver,AAPL,MacOS,PowerPC", driver, length);
	free(driver);
	return node;
}

static OsxTreeProperty *OsxGuestFindProperty(OsxTreeNode *node, const char *name)
{
	OsxTreeProperty *property;
	for (property = node->firstproperty; property; property = property->next)
		if (strcmp(property->name, name) == 0)
			return property;
	return NULL;
}

static void OsxGuestAddPhandles(OsxTreeNode *node, uint32 *phandle)
{
	OsxTreeNode *child;
#if OSXG5
	if (OsxGuestFindProperty(node, "AAPL,phandle") == NULL)
#endif
	OsxGuestAddCell(node, "AAPL,phandle", (*phandle)++);
	for (child = node->firstchild; child; child = child->next)
		OsxGuestAddPhandles(child, phandle);
}

static uint32 OsxGuestTreeSize(OsxTreeNode *node)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 size;
	size = 8;
	for (property = node->firstproperty; property; property = property->next)
		size += TREENAMELENGTH + 4 + ((property->length + 3) & ~3u);
	for (child = node->firstchild; child; child = child->next)
		size += OsxGuestTreeSize(child);
	return size;
}

static uint8 *OsxGuestFlattenTree(OsxTreeNode *node, uint8 *output)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 padded;
	OsxGuestStoreBig32(output, (uint32)node->propertycount);
	OsxGuestStoreBig32(output + 4, (uint32)node->childcount);
	output += 8;
	for (property = node->firstproperty; property; property = property->next) {
		memset(output, 0, TREENAMELENGTH);
		memcpy(output, property->name, strlen(property->name));
		OsxGuestStoreBig32(output + TREENAMELENGTH, property->length);
		output += TREENAMELENGTH + 4;
		padded = (property->length + 3) & ~3u;
		memset(output, 0, padded);
		if (property->length)
			memcpy(output, property->value, property->length);
		output += padded;
	}
	for (child = node->firstchild; child; child = child->next)
		output = OsxGuestFlattenTree(child, output);
	return output;
}

static void OsxGuestDestroyTree(OsxTreeNode *node)
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
		OsxGuestDestroyTree(child);
	}
	free(node);
}

static void OsxGuestAddAtaNode(OsxTreeNode *macio, const char *name, uint32 offset, uint32 dmaoffset, uint32 irq, uint32 dmairq)
{
	OsxTreeNode *node;
	uint32 cells[4];
	node = OsxGuestNewNode(macio, name);
	OsxGuestAddString(node, "device_type", "ata");
	OsxGuestAddString(node, "compatible", "heathrow-ata");
	OsxGuestAddString(node, "model", "ata-3");
	cells[0] = offset;
	cells[1] = 0x1000;
	cells[2] = dmaoffset;
	cells[3] = 0x100;
	OsxGuestAddCells(node, "reg", cells, 4);
	cells[0] = irq;
	cells[1] = dmairq;
	OsxGuestAddCells(node, "AAPL,interrupts", cells, 2);
	OsxGuestAddCells(node, "interrupts", cells, 2);
	OsxGuestAddCell(node, "#address-cells", 1);
	OsxGuestAddCell(node, "#size-cells", 0);
}

static void OsxGuestAddBmacNode(OsxTreeNode *macio)
{
	OsxTreeNode *node;
	uint32 cells[6];
	uint8 address[6];
	node = OsxGuestNewNode(macio, "bmac");
	OsxGuestAddString(node, "device_type", "network");
	OsxGuestAddString(node, "compatible", "bmac+");
	cells[0] = OSXBMACOFFSET;
	cells[1] = OSXBMACSIZE;
	cells[2] = 0x8000 + OSXDBDMABMACTX * 0x100;
	cells[3] = 0x100;
	cells[4] = 0x8000 + OSXDBDMABMACRX * 0x100;
	cells[5] = 0x100;
	OsxGuestAddCells(node, "reg", cells, 6);
	cells[0] = OSXIRQBMAC;
	cells[1] = OSXIRQBMACTXDMA;
	cells[2] = OSXIRQBMACRXDMA;
	OsxGuestAddCells(node, "AAPL,interrupts", cells, 3);
	OsxGuestAddCells(node, "interrupts", cells, 3);
	OsxGuestBmacHardwareAddress(address);
	OsxGuestAddProperty(node, "local-mac-address", address, sizeof(address));
}

static void OsxGuestAddAudioNode(OsxTreeNode *macio)
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
	node = OsxGuestNewNode(macio, "awacs");
	OsxGuestAddString(node, "device_type", "sound");
	OsxGuestAddString(node, "compatible", "awacs");
	cells[0] = OSXAUDIOOFFSET;
	cells[1] = OSXAUDIOSIZE;
	cells[2] = 0x8000 + OSXDBDMAAUDIOOUT * 0x100;
	cells[3] = 0x100;
	cells[4] = 0x8000 + OSXDBDMAAUDIOIN * 0x100;
	cells[5] = 0x100;
	OsxGuestAddCells(node, "reg", cells, 6);
	cells[0] = OSXIRQAUDIO;
	cells[1] = OSXIRQAUDIOOUTDMA;
	cells[2] = OSXIRQAUDIOINDMA;
	OsxGuestAddCells(node, "AAPL,interrupts", cells, 3);
	OsxGuestAddCells(node, "interrupts", cells, 3);
	node = OsxGuestNewNode(node, "sound");
	OsxGuestAddString(node, "device_type", "sound");
	OsxGuestAddProperty(node, "model", "343S0184", 8);
	OsxGuestAddCell(node, "#-detects", 3);
	OsxGuestAddCell(node, "#-features", 0);
	OsxGuestAddCell(node, "#-inputs", 2);
	OsxGuestAddCell(node, "#-outputs", 2);
	OsxGuestAddCell(node, "device-id", 0);
	OsxGuestAddProperty(node, "sound-objects", objects, sizeof(objects));
}

#if OSXG5
static void OsxGuestAddPciRegister(OsxTreeNode *node, uint32 device, uint32 base, uint32 size, uint32 prefetch)
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
	OsxGuestAddCells(node, "reg", cells, 10);
	cells[0] = 0x82000010 | prefetch | (device << 11);
	cells[1] = 0;
	cells[2] = base;
	cells[3] = 0;
	cells[4] = size;
	OsxGuestAddCells(node, "assigned-addresses", cells, 5);
}

static void OsxGuestAddPciIdentity(OsxTreeNode *node, uint32 vendor, uint32 device, uint32 classcode)
{
	OsxGuestAddCell(node, "vendor-id", vendor);
	OsxGuestAddCell(node, "device-id", device);
	OsxGuestAddCell(node, "revision-id", 0);
	OsxGuestAddCell(node, "class-code", classcode);
}

static void OsxGuestAddInterrupts(OsxTreeNode *node, uint32 first, uint32 firstsense, uint32 second, uint32 secondsense, int count)
{
	uint32 cells[4];
	cells[0] = first;
	cells[1] = firstsense;
	cells[2] = second;
	cells[3] = secondsense;
	OsxGuestAddCells(node, "interrupts", cells, count * 2);
}

static OsxTreeNode *OsxGuestAddKauaiNode(OsxTreeNode *ht, uint32 device, uint32 base, uint32 irq, uint32 dmairq)
{
	OsxTreeNode *node;
	node = OsxGuestNewNode(ht, "ata-6");
	OsxGuestAddString(node, "device_type", "ata");
	OsxGuestAddString(node, "compatible", "K2-UATA");
	OsxGuestAddString(node, "model", "ata-6");
	OsxGuestAddPciRegister(node, device, base, OSXG5ATASIZE, 0);
	OsxGuestAddPciIdentity(node, 0x106b, 0x0043, 0x01018f);
	OsxGuestAddInterrupts(node, irq, 1, dmairq, 0, 2);
	OsxGuestAddCell(node, "#address-cells", 1);
	OsxGuestAddCell(node, "#size-cells", 0);
	return node;
}

static void OsxGuestAddPmuNodes(OsxTreeNode *macio, OsxTreeNode **interrupting)
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
	gpio = OsxGuestNewNode(macio, "gpio");
	OsxGuestAddString(gpio, "device_type", "gpio");
	OsxGuestAddString(gpio, "compatible", "mac-io-gpio");
	cells[0] = OSXG5GPIOBASE;
	cells[1] = 0x30;
	OsxGuestAddCells(gpio, "reg", cells, 2);
	OsxGuestAddCell(gpio, "#address-cells", 1);
	OsxGuestAddCell(gpio, "#size-cells", 0);
	node = OsxGuestNewNode(gpio, "extint-gpio1");
	OsxGuestAddString(node, "device_type", "extint-gpio1");
	OsxGuestAddStrings(node, "compatible", "keywest-gpio1", "gpio");
	OsxGuestAddCell(node, "reg", 9);
	OsxGuestAddInterrupts(node, OSXG5IRQPMUGPIO, 1, 0, 0, 1);
	interrupting[0] = node;
	node = OsxGuestNewNode(gpio, "programmer-switch");
	OsxGuestAddString(node, "device_type", "programmer-switch");
	OsxGuestAddInterrupts(node, 0x37, 0, 0, 0, 1);
	interrupting[1] = node;
	pmu = OsxGuestNewNode(macio, "via-pmu");
	OsxGuestAddString(pmu, "device_type", "via-pmu");
	OsxGuestAddString(pmu, "compatible", "pmu");
	cells[0] = 0x16000;
	cells[1] = 0x2000;
	OsxGuestAddCells(pmu, "reg", cells, 2);
	OsxGuestAddInterrupts(pmu, OSXG5IRQPMU, 1, 0, 0, 1);
	OsxGuestAddCell(pmu, "pmu-version", 0xd07f0c);
	OsxGuestAddCell(pmu, "#address-cells", 1);
	OsxGuestAddCell(pmu, "#size-cells", 0);
	interrupting[2] = pmu;
	node = OsxGuestNewNode(pmu, "adb");
	OsxGuestAddString(node, "device_type", "adb");
	OsxGuestAddString(node, "compatible", "pmu-99");
	node = OsxGuestNewNode(pmu, "rtc");
	OsxGuestAddString(node, "device_type", "rtc");
	OsxGuestAddString(node, "compatible", "rtc,via-pmu");
	node = OsxGuestNewNode(pmu, "power-mgt");
	OsxGuestAddString(node, "device_type", "power-mgt");
	OsxGuestAddString(node, "compatible", "via-pmu-99");
	OsxGuestAddString(node, "registry-name", "extint-gpio1");
	OsxGuestAddProperty(node, "prim-info", priminfo, sizeof(priminfo));
}

static OsxTreeNode *OsxGuestAddU3Nodes(OsxTreeNode *root)
{
	OsxTreeNode *u3;
	OsxTreeNode *i2c;
	OsxTreeNode *node;
	uint32 cells[2];
	u3 = OsxGuestNewNode(root, "u3");
	OsxGuestAddString(u3, "device_type", "memory-controller");
	OsxGuestAddString(u3, "compatible", "u3");
	OsxGuestAddCell(u3, "device-rev", 0xb3);
	cells[0] = OSXG5U3BASE;
	cells[1] = OSXG5U3SIZE;
	OsxGuestAddCells(u3, "reg", cells, 2);
	OsxGuestAddCell(u3, "#address-cells", 1);
	OsxGuestAddCell(u3, "#size-cells", 1);
	i2c = OsxGuestNewNode(u3, "i2c");
	OsxGuestAddString(i2c, "device_type", "i2c");
	OsxGuestAddStrings(i2c, "compatible", "keywest-i2c", "uni-n-i2c");
	cells[0] = OSXG5U3I2C;
	cells[1] = 0x1000;
	OsxGuestAddCells(i2c, "reg", cells, 2);
	OsxGuestAddCell(i2c, "AAPL,address", OSXG5U3I2C);
	OsxGuestAddCell(i2c, "AAPL,address-step", 0x10);
	OsxGuestAddCell(i2c, "AAPL,i2c-rate", 100);
	OsxGuestAddString(i2c, "AAPL,driver-name", ".i2c-uni-n");
	OsxGuestAddInterrupts(i2c, OSXG5IRQU3I2C, 1, 0, 0, 1);
	OsxGuestAddCell(i2c, "#address-cells", 1);
	OsxGuestAddCell(i2c, "#size-cells", 0);
	node = OsxGuestNewNode(u3, "dart");
	OsxGuestAddString(node, "device_type", "dart");
	OsxGuestAddStrings(node, "compatible", "u3-dart", "dart");
	cells[0] = OSXG5DART;
	cells[1] = OSXG5DARTSIZE;
	OsxGuestAddCells(node, "reg", cells, 2);
	return i2c;
}

static OsxTreeNode *OsxGuestAddK2I2cNode(OsxTreeNode *macio)
{
	OsxTreeNode *i2c;
	OsxTreeNode *node;
	uint32 cells[2];
	node = OsxGuestNewNode(macio, "i2c");
	OsxGuestAddString(node, "device_type", "i2c");
	OsxGuestAddStrings(node, "compatible", "k2-i2c", "keywest-i2c");
	OsxGuestAddProperty(node, "built-in", "", 0);
	cells[0] = 0x18000;
	cells[1] = 0x1000;
	OsxGuestAddCells(node, "reg", cells, 2);
	OsxGuestAddCell(node, "AAPL,address", OSXG5MACIOBASE + 0x18000);
	OsxGuestAddCell(node, "AAPL,address-step", 0x10);
	OsxGuestAddCell(node, "AAPL,i2c-rate", 100);
	OsxGuestAddString(node, "AAPL,driver-name", ".i2c-mac-io");
	OsxGuestAddCell(node, "#address-cells", 1);
	OsxGuestAddCell(node, "#size-cells", 0);
	OsxGuestAddInterrupts(node, OSXG5IRQK2I2C, 1, 0, 0, 1);
	i2c = node;
	node = OsxGuestNewNode(node, "deq");
	OsxGuestAddString(node, "device_type", "deq");
	OsxGuestAddCell(node, "reg", 0x6a);
	OsxGuestAddCell(node, "i2c-address", 0x6a);
	return i2c;
}

static OsxTreeNode *OsxGuestAddI2sNodes(OsxTreeNode *macio)
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
	i2s = OsxGuestNewNode(macio, "i2s");
	OsxGuestAddString(i2s, "device_type", "i2s");
	OsxGuestAddProperty(i2s, "built-in", "", 0);
	cells[0] = 0x10000;
	cells[1] = 0x1000;
	cells[2] = 0x8000;
	cells[3] = 0x400;
	OsxGuestAddCells(i2s, "reg", cells, 4);
	OsxGuestAddCell(i2s, "#address-cells", 1);
	OsxGuestAddProperty(i2s, "ranges", "", 0);
	OsxGuestAddCell(i2s, "AAPL,address", OSXG5MACIOBASE + 0x10000);
	bus = OsxGuestNewNode(i2s, "i2s-a");
	OsxGuestAddString(bus, "device_type", "soundbus");
	OsxGuestAddString(bus, "compatible", "i2sbus");
	OsxGuestAddProperty(bus, "built-in", "", 0);
	cells[0] = 0;
	cells[1] = 0x1000;
	cells[2] = 0;
	cells[3] = 0x100;
	cells[4] = 0x100;
	cells[5] = 0x100;
	OsxGuestAddCells(bus, "reg", cells, 6);
	cells[0] = 0x1e;
	cells[1] = 1;
	cells[2] = OSXG5IRQI2STX;
	cells[3] = 0;
	cells[4] = OSXG5IRQI2SRX;
	cells[5] = 0;
	OsxGuestAddCells(bus, "interrupts", cells, 6);
	OsxGuestAddCells(bus, "AAPL,requested-priorities", priorities, 3);
	OsxGuestAddCells(bus, "AAPL,clock-id", clockid, 4);
	OsxGuestAddCells(bus, "AAPL,clock-data", clockdata, (int)(sizeof(clockdata) / sizeof(clockdata[0])));
	node = OsxGuestNewNode(bus, "sound");
	OsxGuestAddString(node, "device_type", "soundchip");
	OsxGuestAddString(node, "compatible", "AOAK2");
	OsxGuestAddProperty(node, "built-in", "", 0);
	OsxGuestAddCell(node, "layout-id", 36);
	OsxGuestAddCell(node, "object-model-version", 2);
	OsxGuestAddCell(node, "vendor-id", 0x106b);
	return bus;
}

static void OsxGuestAddI2sFunctions(OsxTreeNode *macio, OsxTreeNode *bus)
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
	macioh = OsxGuestBig32(OsxGuestFindProperty(macio, "AAPL,phandle")->value);
	bush = OsxGuestBig32(OsxGuestFindProperty(bus, "AAPL,phandle")->value);
	for (index = 0; index < 14; index++)
		OsxGuestAddCell(bus, busnames[index], macioh);
	for (index = 0; index < 10; index++) {
		cells[0] = bush;
		cells[1] = setvalues[index][0];
		cells[2] = 3;
		cells[3] = 0x3c;
		cells[4] = setvalues[index][1];
		cells[5] = setvalues[index][2];
		OsxGuestAddCells(macio, setnames[index], cells, 6);
	}
	for (index = 0; index < 4; index++) {
		cells[0] = bush;
		cells[1] = 0x08000000;
		cells[2] = 0x1a;
		cells[3] = 0x3c;
		cells[4] = getvalues[index][0];
		cells[5] = getvalues[index][1];
		cells[6] = 1;
		OsxGuestAddCells(macio, getnames[index], cells, 7);
	}
}

static OsxTreeNode *OsxGuestBuildTreeG5(OsxBootState *state, OsxTreeNode **memorymap)
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
	root = OsxGuestNewNode(NULL, "device-tree");
	OsxGuestAddString(root, "model", "PowerMac7,3");
	OsxGuestAddProperty(root, "compatible", rootcompatible, sizeof(rootcompatible));
	OsxGuestAddString(root, "device_type", "bootrom");
	OsxGuestAddCell(root, "#address-cells", 1);
	OsxGuestAddCell(root, "#size-cells", 1);
	OsxGuestAddCell(root, "clock-frequency", OSXBUSFREQUENCY);
	OsxGuestAddString(root, "copyright", "Copyright 1983-2003 Apple Computer, Inc. All Rights Reserved");

	node = OsxGuestNewNode(root, "cpus");
	OsxGuestAddCell(node, "#address-cells", 1);
	OsxGuestAddCell(node, "#size-cells", 0);
	cpu = OsxGuestNewNode(node, "PowerPC,G5");
	OsxGuestAddString(cpu, "device_type", "cpu");
	OsxGuestAddCell(cpu, "reg", 0);
	OsxGuestAddCell(cpu, "cpu-version", machine->pvr);
	OsxGuestAddString(cpu, "state", "running");
	OsxGuestAddProperty(cpu, "64-bit", "", 0);
	OsxGuestAddProperty(cpu, "altivec", "", 0);
	OsxGuestAddCell(cpu, "clock-frequency", OSXCPUFREQUENCYG4);
	OsxGuestAddCell(cpu, "bus-frequency", OSXBUSFREQUENCY);
	OsxGuestAddCell(cpu, "timebase-frequency", OSXTIMEBASEFREQUENCY);
	OsxGuestAddCell(cpu, "d-cache-size", 0x8000);
	OsxGuestAddCell(cpu, "i-cache-size", 0x10000);
	OsxGuestAddCell(cpu, "d-cache-sets", 128);
	OsxGuestAddCell(cpu, "i-cache-sets", 512);
	OsxGuestAddCell(cpu, "d-cache-block-size", 128);
	OsxGuestAddCell(cpu, "i-cache-block-size", 128);
	OsxGuestAddCell(cpu, "reservation-granule-size", 128);
	OsxGuestAddCell(cpu, "tlb-size", 1024);
	OsxGuestAddCell(cpu, "tlb-sets", 256);
	OsxGuestAddCell(cpu, "slb-size", 64);
	OsxGuestAddProperty(cpu, "graphics", "", 0);
	OsxGuestAddProperty(cpu, "external-control", "", 0);
	OsxGuestAddProperty(cpu, "general-purpose", "", 0);

	node = OsxGuestNewNode(root, "memory");
	OsxGuestAddString(node, "device_type", "memory");
	cells[0] = 0;
	cells[1] = machine->ramsize;
	OsxGuestAddCells(node, "reg", cells, 2);

	chosen = OsxGuestNewNode(root, "chosen");
	OsxGuestAddString(chosen, "bootpath", state->rootpath);
	OsxGuestAddString(chosen, "rootpath", state->rootpath);
	*memorymap = OsxGuestNewNode(chosen, "memory-map");

	node = OsxGuestNewNode(root, "rom");
	cells[0] = 0xff800000;
	cells[1] = 0x00800000;
	OsxGuestAddCells(node, "reg", cells, 2);
	node = OsxGuestNewNode(node, "boot-rom");
	cells[0] = 0xfff00000;
	cells[1] = 0x00100000;
	OsxGuestAddCells(node, "reg", cells, 2);
	cells[2] = 0x00051000;
	OsxGuestAddCells(node, "info", cells, 3);
	OsxGuestAddString(node, "BootROM-version", "$0005.10f1");

	node = OsxGuestNewNode(root, "openprom");
	OsxGuestAddString(node, "device_type", "BootROM");
	OsxGuestAddString(node, "model", "OpenFirmware 3");

	node = OsxGuestNewNode(root, "options");
	OsxGuestAddString(node, "boot-args", "");
	OsxGuestAddString(node, "auto-boot?", "true");

	node = OsxGuestNewNode(root, "aliases");
	OsxGuestAddString(node, "mac-io", "/ht@f2000000/mac-io@7");
	OsxGuestAddString(node, "via-pmu", "/ht@f2000000/mac-io@7/via-pmu@16000");
	OsxGuestAddString(node, "rtc", "/ht@f2000000/mac-io@7/via-pmu@16000/rtc");
	OsxGuestAddString(node, "nvram", "/nvram@fff80000");

	node = OsxGuestNewNode(root, "nvram");
	OsxGuestAddString(node, "device_type", "nvram");
	OsxGuestAddString(node, "compatible", "nvram,flash");
	cells[0] = OSXFLASHBASE;
	cells[1] = OSXFLASHSIZE;
	OsxGuestAddCells(node, "reg", cells, 2);
	OsxGuestAddCell(node, "#bytes", OSXFLASHBANK);

	interrupting[8] = OsxGuestAddU3Nodes(root);

	ht = OsxGuestNewNode(root, "ht");
	OsxGuestAddString(ht, "device_type", "ht");
	OsxGuestAddString(ht, "compatible", "u3-ht");
	cells[0] = OSXG5HTCONFIG;
	cells[1] = OSXG5HTCONFIGSIZE;
	cells[2] = OSXG5HTSELF;
	cells[3] = 0x1000;
	OsxGuestAddCells(ht, "reg", cells, 4);
	OsxGuestAddCell(ht, "#address-cells", 3);
	OsxGuestAddCell(ht, "#size-cells", 2);
	cells[0] = 0;
	cells[1] = 0;
	OsxGuestAddCells(ht, "bus-range", cells, 2);
	OsxGuestAddCell(ht, "clock-frequency", 400000000);
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
	OsxGuestAddCells(ht, "ranges", cells, 12);
	OsxGuestAddString(ht, "AAPL,unit-string", "f2000000");

	macio = OsxGuestNewNode(ht, "mac-io");
	OsxGuestAddString(macio, "device_type", "mac-io");
	OsxGuestAddString(macio, "compatible", "K2-Keylargo");
	OsxGuestAddString(macio, "model", "AAPL,K2");
	OsxGuestAddPciRegister(macio, OSXG5DEVICEMACIO, OSXG5MACIOBASE, OSXG5MACIOSIZE, 0);
	cells[0] = 0;
	cells[1] = 0x82000010 | (OSXG5DEVICEMACIO << 11);
	cells[2] = 0;
	cells[3] = OSXG5MACIOBASE;
	cells[4] = OSXG5MACIOSIZE;
	OsxGuestAddCells(macio, "ranges", cells, 5);
	OsxGuestAddCell(macio, "AAPL,address", OSXG5MACIOBASE);
	OsxGuestAddPciIdentity(macio, 0x106b, 0x0041, 0xff0000);
	OsxGuestAddCell(macio, "#address-cells", 1);
	OsxGuestAddCell(macio, "#size-cells", 1);

	mpic = OsxGuestNewNode(macio, "interrupt-controller");
	OsxGuestAddString(mpic, "device_type", "open-pic");
	OsxGuestAddString(mpic, "compatible", "chrp,open-pic");
	OsxGuestAddProperty(mpic, "built-in", "", 0);
	cells[0] = OSXG5MPICOFFSET;
	cells[1] = OSXG5MPICSIZE;
	OsxGuestAddCells(mpic, "reg", cells, 2);
	OsxGuestAddCell(mpic, "#interrupt-cells", 2);
	OsxGuestAddCell(mpic, "#address-cells", 0);
	OsxGuestAddProperty(mpic, "interrupt-controller", "", 0);
	OsxGuestAddCell(mpic, "clock-frequency", 4166666);

	OsxGuestAddPmuNodes(macio, interrupting);
	interrupting[7] = OsxGuestAddK2I2cNode(macio);
	interrupting[6] = OsxGuestAddI2sNodes(macio);
	interrupting[3] = OsxGuestAddKauaiNode(ht, OSXG5DEVICEATA0, OSXG5ATABASE, OSXG5IRQATA0, OSXG5IRQATA0DMA);
	interrupting[4] = OsxGuestAddKauaiNode(ht, OSXG5DEVICEATA1, OSXG5ATABASE + OSXG5ATASIZE, OSXG5IRQATA1, OSXG5IRQATA1DMA);

	node = OsxGuestNewNode(ht, "ethernet");
	OsxGuestAddString(node, "device_type", "network");
	OsxGuestAddString(node, "compatible", "K2-GMAC");
	OsxGuestAddPciRegister(node, OSXG5DEVICEGMAC, OSXG5GMACBASE, OSXG5GMACSIZE, 0);
	OsxGuestAddPciIdentity(node, 0x106b, 0x004c, 0x020000);
	OsxGuestAddInterrupts(node, OSXG5IRQGMAC, 1, 0, 0, 1);
	OsxGuestBmacHardwareAddress(address);
	OsxGuestAddProperty(node, "local-mac-address", address, sizeof(address));
	interrupting[5] = node;

	node = OsxGuestAddDisplayNode(ht, machine, 0);
	OsxGuestAddPciRegister(node, OSXG5DEVICEDISPLAY, OSXVRAMBASE, OSXVRAMSIZE, 0);
	OsxGuestAddPciIdentity(node, 0x1234, 0x1111, 0x030000);

	OsxGuestAddPhandles(root, &state->phandle);
	property = OsxGuestFindProperty(mpic, "AAPL,phandle");
	parent = OsxGuestBig32(property->value);
	for (index = 0; index < 9; index++)
		OsxGuestAddCell(interrupting[index], "interrupt-parent", parent);
	OsxGuestAddI2sFunctions(macio, interrupting[6]);
	OsxGuestAddCell(cpu, "AAPL,parentIC", parent);
	return root;
}
#endif

static OsxTreeNode *OsxGuestBuildTree(OsxBootState *state, OsxTreeNode **memorymap)
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
	root = OsxGuestNewNode(NULL, "device-tree");
	if (state->machine->pvr == OSXPVRG4) {
		OsxGuestAddString(root, "model", "PowerMac1,2");
		OsxGuestAddStrings(root, "compatible", "PowerMac1,2", "MacRISC");
	} else {
		OsxGuestAddString(root, "model", "PowerMac1,1");
		OsxGuestAddStrings(root, "compatible", "PowerMac1,1", "MacRISC");
	}
	OsxGuestAddCell(root, "#address-cells", 1);
	OsxGuestAddCell(root, "#size-cells", 1);
	OsxGuestAddCell(root, "clock-frequency", OSXBUSFREQUENCY);
	OsxGuestAddString(root, "copyright", "Copyright 1983-1997 Apple Computer, Inc. All Rights Reserved");

	node = OsxGuestNewNode(root, "cpus");
	OsxGuestAddCell(node, "#address-cells", 1);
	OsxGuestAddCell(node, "#size-cells", 0);
#if OSXG5
	if (state->machine->pvr == OSX970PVR) {
		node = OsxGuestNewNode(node, "PowerPC,G5");
		OsxGuestAddProperty(node, "altivec", "", 0);
		OsxGuestAddProperty(node, "64-bit", "", 0);
		OsxGuestAddString(node, "device_type", "cpu");
		OsxGuestAddCell(node, "reg", 0);
		OsxGuestAddCell(node, "cpu-version", state->machine->pvr);
		OsxGuestAddCell(node, "clock-frequency", OSXCPUFREQUENCYG4);
		OsxGuestAddCell(node, "bus-frequency", OSXBUSFREQUENCY);
		OsxGuestAddCell(node, "timebase-frequency", OSXTIMEBASEFREQUENCY);
		OsxGuestAddCell(node, "d-cache-size", 0x8000);
		OsxGuestAddCell(node, "i-cache-size", 0x10000);
		OsxGuestAddCell(node, "d-cache-sets", 128);
		OsxGuestAddCell(node, "i-cache-sets", 512);
		OsxGuestAddCell(node, "d-cache-block-size", 128);
		OsxGuestAddCell(node, "i-cache-block-size", 128);
		OsxGuestAddCell(node, "reservation-granule-size", 128);
		OsxGuestAddCell(node, "tlb-size", 1024);
		OsxGuestAddCell(node, "tlb-sets", 256);
		OsxGuestAddCell(node, "slb-size", 64);
		OsxGuestAddProperty(node, "graphics", "", 0);
		OsxGuestAddProperty(node, "external-control", "", 0);
		OsxGuestAddProperty(node, "general-purpose", "", 0);
	} else {
#endif
	if (state->machine->pvr == OSXPVRG4) {
		node = OsxGuestNewNode(node, "PowerPC,G4");
		OsxGuestAddProperty(node, "altivec", "", 0);
	} else {
		node = OsxGuestNewNode(node, "PowerPC,750");
	}
	OsxGuestAddString(node, "device_type", "cpu");
	OsxGuestAddCell(node, "reg", 0);
	OsxGuestAddCell(node, "cpu-version", state->machine->pvr);
	if (state->machine->pvr == OSXPVRG4)
		OsxGuestAddCell(node, "clock-frequency", OSXCPUFREQUENCYG4);
	else
		OsxGuestAddCell(node, "clock-frequency", OSXCPUFREQUENCY);
	OsxGuestAddCell(node, "bus-frequency", OSXBUSFREQUENCY);
	OsxGuestAddCell(node, "timebase-frequency", OSXTIMEBASEFREQUENCY);
	OsxGuestAddCell(node, "d-cache-size", 0x8000);
	OsxGuestAddCell(node, "i-cache-size", 0x8000);
	OsxGuestAddCell(node, "d-cache-sets", 128);
	OsxGuestAddCell(node, "i-cache-sets", 128);
	OsxGuestAddCell(node, "d-cache-block-size", 32);
	OsxGuestAddCell(node, "i-cache-block-size", 32);
	OsxGuestAddCell(node, "reservation-granule-size", 32);
	OsxGuestAddCell(node, "tlb-size", 128);
	OsxGuestAddCell(node, "tlb-sets", 64);
	OsxGuestAddProperty(node, "graphics", "", 0);
	OsxGuestAddProperty(node, "external-control", "", 0);
	OsxGuestAddProperty(node, "general-purpose", "", 0);
#if OSXG5
	}
#endif

	node = OsxGuestNewNode(root, "memory");
	OsxGuestAddString(node, "device_type", "memory");
	cells[0] = 0;
	cells[1] = machine->ramsize;
	OsxGuestAddCells(node, "reg", cells, 2);

	chosen = OsxGuestNewNode(root, "chosen");
	OsxGuestAddString(chosen, "bootpath", state->rootpath);
	OsxGuestAddString(chosen, "rootpath", state->rootpath);
	*memorymap = OsxGuestNewNode(chosen, "memory-map");

	node = OsxGuestNewNode(root, "rom");
	cells[0] = 0xff800000;
	cells[1] = 0x00800000;
	OsxGuestAddCells(node, "reg", cells, 2);
	node = OsxGuestNewNode(node, "boot-rom");
	cells[0] = 0xfff00000;
	cells[1] = 0x00100000;
	OsxGuestAddCells(node, "reg", cells, 2);
	cells[2] = 0x00033000;
	OsxGuestAddCells(node, "info", cells, 3);
	OsxGuestAddString(node, "BootROM-version", "$0003.30f1");

	node = OsxGuestNewNode(root, "openprom");
	OsxGuestAddString(node, "device_type", "BootROM");
	OsxGuestAddString(node, "model", "Open Firmware, 2.4");

	node = OsxGuestNewNode(root, "options");
	OsxGuestAddString(node, "boot-args", "");
	OsxGuestAddString(node, "auto-boot?", "true");

	node = OsxGuestNewNode(root, "aliases");
	OsxGuestAddString(node, "pci", "/pci@80000000");

	pci = OsxGuestNewNode(root, "pci");
	OsxGuestAddString(pci, "device_type", "pci");
	OsxGuestAddStrings(pci, "compatible", "grackle", "MOT,PPC106");
	OsxGuestAddString(pci, "model", "MOT,PPC106");
	cells[0] = 0x80000000;
	cells[1] = 0x7f000000;
	OsxGuestAddCells(pci, "reg", cells, 2);
	OsxGuestAddCell(pci, "#address-cells", 3);
	OsxGuestAddCell(pci, "#size-cells", 2);
	cells[0] = 0;
	cells[1] = 0;
	OsxGuestAddCells(pci, "bus-range", cells, 2);
	OsxGuestAddCell(pci, "clock-frequency", 33333333);
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
	OsxGuestAddCells(pci, "ranges", cells, 12);
	OsxGuestAddString(pci, "AAPL,unit-string", "80000000");

	macio = OsxGuestNewNode(pci, "mac-io");
	OsxGuestAddString(macio, "device_type", "mac-io");
	OsxGuestAddString(macio, "compatible", "heathrow");
	OsxGuestAddString(macio, "model", "AAPL,343S1201");
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
	OsxGuestAddCells(macio, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[1] = 0;
	cells[2] = OSXHEATHROWBASE;
	cells[3] = 0;
	cells[4] = OSXHEATHROWSIZE;
	OsxGuestAddCells(macio, "assigned-addresses", cells, 5);
	cells[0] = 0;
	cells[1] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[2] = 0;
	cells[3] = OSXHEATHROWBASE;
	cells[4] = OSXHEATHROWSIZE;
	OsxGuestAddCells(macio, "ranges", cells, 5);
	OsxGuestAddCell(macio, "AAPL,address", OSXHEATHROWBASE);
	OsxGuestAddCell(macio, "vendor-id", 0x106b);
	OsxGuestAddCell(macio, "device-id", 0x0010);
	OsxGuestAddCell(macio, "revision-id", 1);
	OsxGuestAddCell(macio, "class-code", 0xff0000);
	OsxGuestAddCell(macio, "#address-cells", 1);
	OsxGuestAddCell(macio, "#size-cells", 1);
	OsxGuestAddString(macio, "AAPL,unit-string", "10");

	node = OsxGuestNewNode(macio, "nvram");
	OsxGuestAddString(node, "device_type", "nvram");
	cells[0] = 0x60000;
	cells[1] = OSXNVRAMSIZE * 16;
	OsxGuestAddCells(node, "reg", cells, 2);

	node = OsxGuestNewNode(macio, "escc");
	OsxGuestAddString(node, "device_type", "escc");
	cells[0] = 0x13000;
	cells[1] = 0x1000;
	OsxGuestAddCells(node, "reg", cells, 2);

	cuda = OsxGuestNewNode(macio, "via-cuda");
	OsxGuestAddString(cuda, "device_type", "via-cuda");
	cells[0] = 0x16000;
	cells[1] = 0x2000;
	OsxGuestAddCells(cuda, "reg", cells, 2);
	OsxGuestAddCell(cuda, "AAPL,interrupts", OSXIRQCUDA);
	OsxGuestAddCell(cuda, "interrupts", OSXIRQCUDA);
	node = OsxGuestNewNode(cuda, "adb");
	OsxGuestAddString(node, "device_type", "adb");
	OsxGuestAddString(node, "compatible", "adb");
	node = OsxGuestNewNode(cuda, "pram");
	OsxGuestAddString(node, "device_type", "pram");
	node = OsxGuestNewNode(cuda, "rtc");
	OsxGuestAddString(node, "device_type", "rtc");
	OsxGuestAddString(node, "compatible", "rtc");

	OsxGuestAddAtaNode(macio, "ata", 0x20000, 0x8b00, OSXIRQIDE0, OSXIRQIDE0DMA);
	OsxGuestAddAtaNode(macio, "ata", 0x21000, 0x8c00, OSXIRQIDE1, OSXIRQIDE1DMA);
	OsxGuestAddBmacNode(macio);
	OsxGuestAddAudioNode(macio);

	node = OsxGuestAddDisplayNode(pci, machine, state->ignorendrv);
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
	OsxGuestAddCells(node, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEDISPLAY << 11);
	cells[1] = 0;
	cells[2] = OSXVRAMBASE;
	cells[3] = 0;
	cells[4] = OSXVRAMSIZE;
	OsxGuestAddCells(node, "assigned-addresses", cells, 5);
	OsxGuestAddCell(node, "vendor-id", 0x1234);
	OsxGuestAddCell(node, "device-id", 0x1111);
	OsxGuestAddCell(node, "revision-id", 2);
	OsxGuestAddCell(node, "class-code", 0x030000);
	OsxGuestAddString(node, "AAPL,unit-string", "12");
	return root;
}

static void OsxGuestInstallBootArgs(OsxBootState *state, const char *commandline, uint32 tree, uint32 treelength)
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
	OsxGuestStoreBig32(args + 0x104, 0);
	OsxGuestStoreBig32(args + 0x108, machine->ramsize);
#if OSXG5
	if (machine->g5state.highsize) {
		args[3] = 2;
		OsxGuestStoreBig32(args + 0x108, machine->ramsize >> 12);
		OsxGuestStoreBig32(args + 0x10c, (uint32)(OSXG5HIGHRAM >> 12));
		OsxGuestStoreBig32(args + 0x110, machine->g5state.highsize >> 12);
	}
#endif
	OsxGuestStoreBig32(args + 0x1d4, OSXVRAMBASE);
	OsxGuestStoreBig32(args + 0x1d8, strstr(commandline, "-v") == NULL);
	OsxGuestStoreBig32(args + 0x1dc, machine->rowbytes);
	OsxGuestStoreBig32(args + 0x1e0, machine->width);
	OsxGuestStoreBig32(args + 0x1e4, machine->height);
	OsxGuestStoreBig32(args + 0x1e8, machine->depth);
	OsxGuestStoreBig32(args + 0x1ec, 0);
	OsxGuestStoreBig32(args + 0x1f0, tree);
	OsxGuestStoreBig32(args + 0x1f4, treelength);
	OsxGuestStoreBig32(args + 0x1f8, state->nextfree);
}

static void OsxGuestPaintBootScreen(OsxMachine *machine, const char *commandline)
{
	uint32 row;
	uint32 column;
	uint8 *pixel;
	if (strstr(commandline, "-v") != NULL || machine->depth != 32)
		return;
	for (row = 0; row < machine->height; row++) {
		pixel = machine->vram + row * machine->rowbytes;
		for (column = 0; column < machine->width; column++, pixel += 4) {
			pixel[0] = 0;
			pixel[1] = BOOTSCREENGRAY;
			pixel[2] = BOOTSCREENGRAY;
			pixel[3] = BOOTSCREENGRAY;
		}
	}
}

static void OsxGuestCenterCornerSpinner(OsxMachine *machine)
{
	uint32 progress;
	uint32 width;
	uint8 *fields;
	progress = OsxGuestFindKernelSymbol(machine, "_default_progress");
	if (!progress)
		return;
	fields = machine->ram + progress;
	if (OsxGuestBig32(fields + BOOTSPINNERFLAGS) != 0 || OsxGuestBig32(fields + BOOTSPINNERX) != BOOTSPINNERCORNER ||
		OsxGuestBig32(fields + BOOTSPINNERY) != BOOTSPINNERCORNER)
		return;
	width = OsxGuestBig32(fields + BOOTSPINNERWIDTH);
	OsxGuestStoreBig32(fields + BOOTSPINNERX, (machine->width - width) / 2);
	OsxGuestStoreBig32(fields + BOOTSPINNERY, machine->height / 2 + BOOTSPINNERBELOW - width / 2);
}

static void OsxGuestMatchSpinnerToBootScreen(OsxMachine *machine)
{
	uint32 table;
	uint8 *entry;
	table = OsxGuestFindKernelSymbol(machine, "_appleClut8");
	if (!table)
		return;
	entry = machine->ram + table + BOOTCLUTSPINNERENTRY;
	if (entry[0] != BOOTCLUTFULL || entry[1] != BOOTCLUTFULL || entry[2] != BOOTCLUTYELLOW)
		return;
	entry[0] = BOOTSCREENGRAY;
	entry[1] = BOOTSCREENGRAY;
	entry[2] = BOOTSCREENGRAY;
}

static void OsxGuestStartSpinnerImmediately(OsxMachine *machine)
{
	uint32 function;
	uint32 position;
	function = OsxGuestFindKernelSymbol(machine, "_initialize_screen");
	if (!function)
		return;
	for (position = function; position < function + BOOTSPINNERSCAN; position += 4) {
		if (OsxGuestBig32(machine->ram + position) == BOOTLOADDELAY &&
			((OsxGuestBig32(machine->ram + position + 4) & BOOTBRANCHMASK) == BOOTBRANCHLINK ||
			(OsxGuestBig32(machine->ram + position + 8) & BOOTBRANCHMASK) == BOOTBRANCHLINK)) {
			OsxGuestStoreBig32(machine->ram + position, BOOTLOADNODELAY);
			return;
		}
	}
}

static int OsxGuestNeedsVectorCpu(OsxHfsVolume *volume)
{
	uint8 *dyld;
	uint32 size;
	uint32 count;
	uint32 index;
	int compatible;
	dyld = OsxGuestReadFile(volume, "/usr/lib/dyld", &size);
	if (dyld == NULL)
		return 0;
	compatible = 0;
	if (size >= 8 && OsxGuestBig32(dyld) == 0xcafebabe) {
		count = OsxGuestBig32(dyld + 4);
		for (index = 0; index < count && 8 + index * 20 + 20 <= size; index++)
			if (OsxGuestBig32(dyld + 8 + index * 20) == 18 && OsxGuestBig32(dyld + 8 + index * 20 + 4) <= 9)
				compatible = 1;
	} else if (size < 12 || OsxGuestBig32(dyld) != 0xfeedface || OsxGuestBig32(dyld + 8) <= 9) {
		compatible = 1;
	}
	free(dyld);
	return !compatible;
}

static uint8 *OsxGuestAddVolumeBundles(OsxHfsVolume *volume, uint8 *mkext, uint32 *size, const char *const *bundles, int count)
{
	uint8 *donor;
	uint8 *merged;
	uint32 donorsize;
	if (OsxGuestMkextHasBundle(mkext, *size, bundles[count - 1]))
		return mkext;
	donor = OsxGuestBuildDriverPackage(volume, 0, bundles, count, &donorsize);
	merged = NULL;
	if (donor)
		merged = OsxGuestMkextAddBundles(mkext, *size, donor, donorsize, bundles, count, NULL, 0, size);
	free(donor);
	if (merged == NULL)
		return mkext;
	free(mkext);
	return merged;
}

static uint8 *OsxGuestAddDisplayDrivers(OsxHfsVolume *volume, uint8 *mkext, uint32 *size)
{
	static const char *const bundles[2] = { "com.apple.iokit.IOGraphicsFamily", "com.apple.iokit.IONDRVSupport" };
	return OsxGuestAddVolumeBundles(volume, mkext, size, bundles, 2);
}

static uint8 *OsxGuestAddOldWorldDrivers(OsxHfsVolume *volume, uint8 *mkext, uint32 *size, const char *bootpath)
{
	static const char *const network[1] = { "com.apple.iokit.IONetworkingFamily" };
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
	if (OsxGuestMkextHasBundle(mkext, *size, bundles[0]))
		return mkext;
	merged = NULL;
	for (list = 0; list < 2 && merged == NULL; list++) {
		for (index = 0; merged == NULL && (path = PrefsFindString(lists[list], index)) != NULL; index++) {
			if (strcmp(path, bootpath) == 0 || (file = Sys_open(path, true, false)) == NULL)
				continue;
			if (OsxGuestOpenVolume(&donor, file, -1)) {
				donorkext = OsxGuestReadFile(&donor, "/System/Library/Extensions.mkext", &donorsize);
				if (donorkext && OsxGuestThinPowerPC(donorkext, &donorsize) && OsxGuestMkextHasBundle(donorkext, donorsize, bundles[0]))
					merged = OsxGuestMkextAddBundles(mkext, *size, donorkext, donorsize, bundles, 8, aliases, 1, size);
				free(donorkext);
			}
			OsxGuestCloseVolume(&donor);
			Sys_close(file);
		}
	}
	if (merged == NULL)
		return mkext;
	free(mkext);
	return OsxGuestAddVolumeBundles(volume, merged, size, network, 1);
}

int OsxGuestBootLoad(OsxMachine *machine, const char *imagepath, int bootbus, int bootunit, const char *commandline)
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
	if (!OsxGuestOpenVolume(&volume, file, machine->bootpartition)) {
		ErrorAlert("The Mac OS X boot image has no HFS+ volume.");
		OsxGuestCloseVolume(&volume);
		return 0;
	}
	sprintf(state.rootpath, "/pci@80000000/mac-io@10/ata@%x/@%d:%d", 0x20000 + bootbus * 0x1000, bootunit, volume.partition);
	machine->pvr = OSXPVR;
	if (OsxGuestNeedsVectorCpu(&volume))
		machine->pvr = OSXPVRG4;
#if OSXG5
	if (PrefsFindBool("osxcpu970") || machine->g5)
		machine->pvr = OSX970PVR;
	if (machine->g5)
		sprintf(state.rootpath, "/ht@f2000000/ata-6@%x/@%d:%d", OSXG5DEVICEATA0 + bootbus, bootunit, volume.partition);
#endif
	OsxGuestCpuSetModel(machine->cpu, machine->pvr);
	kernel = OsxGuestReadFile(&volume, "/mach_kernel", &kernelsize);
	if (kernel == NULL || !OsxGuestThinPowerPC(kernel, &kernelsize) || !OsxGuestLoadKernel(&state, kernel, kernelsize)) {
		ErrorAlert("The Mac OS X kernel (/mach_kernel) could not be loaded.");
		free(kernel);
		OsxGuestCloseVolume(&volume);
		return 0;
	}
	state.ignorendrv = OsxGuestContainsBytes(kernel, kernelsize, "Darwin Kernel Version 1.");
	mkext = NULL;
	if (!state.ignorendrv)
		mkext = OsxGuestReadFile(&volume, "/System/Library/Extensions.mkext", &mkextsize);
	if (mkext != NULL && !OsxGuestThinPowerPC(mkext, &mkextsize)) {
		free(mkext);
		mkext = NULL;
	}
	if (mkext == NULL)
		mkext = OsxGuestBuildDriverPackage(&volume, state.ignorendrv, NULL, 0, &mkextsize);
	else if (!state.ignorendrv)
		mkext = OsxGuestAddDisplayDrivers(&volume, mkext, &mkextsize);
	if (mkext)
		mkext = OsxGuestAddOldWorldDrivers(&volume, mkext, &mkextsize, imagepath);
#if OSXG5
	if (!machine->g5)
#endif
	if (OsxGuestContainsBytes(kernel, kernelsize, "Darwin Kernel Version 9.") &&
	    (!mkext || !OsxGuestMkextHasBundle(mkext, mkextsize, "com.apple.driver.AppleGossamerPE"))) {
		ErrorAlert("On non-G5 machine types, Mac OS X 10.5 needs the Old World platform drivers. Attach a Mac OS X 10.4 disc or disk image.");
		free(mkext);
		free(kernel);
		OsxGuestCloseVolume(&volume);
		return 0;
	}
	if (mkext) {
		address = OsxGuestBootAllocate(&state, mkextsize);
		memcpy(machine->ram + address, mkext, mkextsize);
		sprintf(rangename, "DriversPackage-%x", address);
		OsxGuestAddRange(&state, rangename, address, mkextsize);
		machine->mkext = mkext;
		machine->mkextsize = mkextsize;
	} else {
		fprintf(stderr, "osx: no Extensions.mkext, booting without drivers\n");
	}
	OsxGuestFindClipboardFunctions(machine, &volume);
	machine->quartzallocatedextensions = OsxGuestPrepareSaverSoftware(&volume, &machine->ata[bootbus], &machine->ata[bootbus].drives[bootunit]);
	OsxGuestPrepareJaguarLogin(&volume, &machine->ata[bootbus], &machine->ata[bootbus].drives[bootunit]);
	if (machine->flurry)
		machine->flurry = OsxGuestPrepareFlurry(machine, &volume, bootbus, bootunit);
	if (machine->quartz)
		machine->quartz = OsxGuestPrepareQuartz(machine, &volume, &machine->ata[bootbus], &machine->ata[bootbus].drives[bootunit]);
	OsxGuestCloseVolume(&volume);
	machine->bootargs = OsxGuestBootAllocate(&state, BOOTARGSSIZE);
	OsxGuestAddRange(&state, "BootArgs", machine->bootargs, BOOTARGSSIZE);
#if OSXG5
	state.phandle = 0xff000001;
	if (machine->g5)
		root = OsxGuestBuildTreeG5(&state, &memorymap);
	else
#endif
	root = OsxGuestBuildTree(&state, &memorymap);
	cells[0] = 0;
	cells[1] = 0;
	OsxGuestAddProperty(memorymap, "DeviceTree", cells, 8);
	for (index = 0; index < state.rangecount; index++)
		OsxGuestAddProperty(memorymap, state.ranges[index].name, cells, 8);
	state.phandle = 0xff000001;
	OsxGuestAddPhandles(root, &state.phandle);
	treesize = OsxGuestTreeSize(root);
	treeaddress = OsxGuestBootAllocate(&state, treesize);
	OsxGuestAddRange(&state, "DeviceTree", treeaddress, treesize);
	for (index = 0; index < state.rangecount; index++) {
		property = OsxGuestFindProperty(memorymap, state.ranges[index].name);
		if (property) {
			OsxGuestStoreBig32(property->value, state.ranges[index].address);
			OsxGuestStoreBig32(property->value + 4, state.ranges[index].length);
		}
	}
	OsxGuestFlattenTree(root, machine->ram + treeaddress);
	OsxGuestDestroyTree(root);
	machine->kernelend = state.nextfree;
	OsxGuestInstallBootArgs(&state, commandline, treeaddress, treesize);
	OsxGuestPaintBootScreen(machine, commandline);
	OsxGuestStartSpinnerImmediately(machine);
	OsxGuestMatchSpinnerToBootScreen(machine);
	/*CenterCornerSpinner(machine);*/ /* if desired */
	machine->scratch = state.nextfree + 0x10000;
	if (machine->log) {
		fprintf(machine->log, "kernel entry %08x bootargs %08x tree %08x+%x top %08x\n", machine->kernelentry, machine->bootargs, treeaddress, treesize, state.nextfree);
		for (index = 0; index < state.rangecount; index++)
			fprintf(machine->log, "  %-24s %08x %08x\n", state.ranges[index].name, state.ranges[index].address, state.ranges[index].length);
	}
	free(kernel);
	return 1;
}
