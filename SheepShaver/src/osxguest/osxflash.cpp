/*
 *  osxflash.cpp - OSX flash utils
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
#include "osxmachine.h"

#define FLASHREADARRAY 0
#define FLASHSTATUS 1
#define FLASHERASESETUP 2
#define FLASHPROGRAMSETUP 3
#define FLASHREADY 0x80

void OsxNvramReadString(const uint8 *nvram, int index, char *output, int size)
{
	uint32 header = 0x1834 + index * 4;
	uint32 offset = ((uint32)nvram[header] << 8) | nvram[header + 1];
	uint32 length = ((uint32)nvram[header + 2] << 8) | nvram[header + 3];
	output[0] = 0;
	if (offset < 0x185c || offset + length > 0x2000 || length >= (uint32)size)
		return;
	memcpy(output, nvram + offset, length);
	output[length] = 0;
}

int OsxNvramWantsClassic(const char *device, const char *command)
{
	if (strstr(device, "AAPL,ROM") != NULL)
		return 1;
	return strstr(command, "mac-boot") != NULL && strstr(device, ":tbxi") == NULL;
}

static int SetFirmwareBoot(uint8 *nvram, const char *device, const char *command)
{
	uint8 block[0x800];
	const uint8 *strings[10];
	uint32 lengths[10];
	uint32 index;
	uint32 header;
	uint32 offset;
	uint32 total = 0;
	uint32 cursor = 0x2000;
	uint32 checksum = 0;
	uint32 start = ((uint32)nvram[0x1806] << 8) | nvram[0x1807];
	if (start < 0x185c || start > 0x2000)
		return 0;
	memcpy(block, nvram + 0x1800, sizeof(block));
	for (index = 0; index < 10; index++) {
		header = 0x34 + index * 4;
		offset = ((uint32)block[header] << 8) | block[header + 1];
		lengths[index] = ((uint32)block[header + 2] << 8) | block[header + 3];
		strings[index] = block;
		if (index == 0) {
			strings[index] = (const uint8 *)device;
			lengths[index] = strlen(device);
		} else if (index == 9) {
			strings[index] = (const uint8 *)command;
			lengths[index] = strlen(command);
		} else if (offset >= start && offset + lengths[index] <= 0x2000)
			strings[index] = block + offset - 0x1800;
		else
			lengths[index] = 0;
		total += lengths[index];
	}
	if (total > 0x2000 - start)
		return 0;
	memset(nvram + start, 0, 0x2000 - start);
	for (index = 0; index < 10; index++) {
		cursor -= lengths[index];
		header = 0x1834 + index * 4;
		nvram[header] = cursor >> 8;
		nvram[header + 1] = cursor & 0xff;
		nvram[header + 2] = lengths[index] >> 8;
		nvram[header + 3] = lengths[index] & 0xff;
		memcpy(nvram + cursor, strings[index], lengths[index]);
	}
	nvram[0x1808] = cursor >> 8;
	nvram[0x1809] = cursor & 0xff;
	nvram[0x1804] = 0;
	nvram[0x1805] = 0;
	for (index = 0x1800; index < 0x2000; index += 2)
		checksum += ((uint32)nvram[index] << 8) | nvram[index + 1];
	checksum = 0xffff - checksum % 0xffff;
	nvram[0x1804] = checksum >> 8;
	nvram[0x1805] = checksum & 0xff;
	return 1;
}

int OsxNvramPrepareBoot(OsxMachine *machine, int bus, int unit, int partition)
{
	char device[256];
	char command[256];
	OsxNvramReadString(machine->nvram, 0, device, sizeof(device));
	OsxNvramReadString(machine->nvram, 9, command, sizeof(command));
	if (device[0] != 0 && !OsxNvramWantsClassic(device, command))
		return 1;
	sprintf(device, "pci/mac-io@10/ata@%x/@%x:%d", 0x20000 + bus * 0x1000, unit, partition);
	return SetFirmwareBoot(machine->nvram, device, "0 bootr");
}

void OsxNvramFinishBoot(OsxMachine *machine, const uint8 *startup)
{
	char device[256];
	char command[256];
	OsxNvramReadString(machine->nvram, 0, device, sizeof(device));
	OsxNvramReadString(machine->nvram, 9, command, sizeof(command));
	if (OsxNvramWantsClassic(device, command) &&
		memcmp(machine->nvram + 0x1378, startup, 4) == 0)
		memset(machine->nvram + 0x1378, 0, 4);
}

void OsxFlashLoad(OsxMachine *machine, const char *path)
{
	FILE *file;
	memset(machine->flash, 0, OSXFLASHSIZE);
	strncpy(machine->nvrampath, path, sizeof(machine->nvrampath) - 1);
	machine->flashmode = FLASHREADARRAY;
	machine->flashdirty = 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return;
	if (fread(machine->flash, 1, OSXFLASHSIZE, file) != OSXFLASHSIZE)
		memset(machine->flash, 0, OSXFLASHSIZE);
	fclose(file);
}

void OsxFlashSave(OsxMachine *machine)
{
	FILE *file;
	if (!machine->flashdirty || machine->nvrampath[0] == 0)
		return;
	file = fopen(machine->nvrampath, "wb");
	if (file == NULL)
		return;
	fwrite(machine->flash, 1, OSXFLASHSIZE, file);
	fclose(file);
	machine->flashdirty = 0;
}

uint32 OsxFlashRead(OsxMachine *machine, uint32 offset, int size)
{
	uint32 value;
	int index;
	if (machine->flashmode != FLASHREADARRAY)
		return FLASHREADY;
	value = 0;
	for (index = 0; index < size; index++)
		value = (value << 8) | machine->flash[(offset + index) & (OSXFLASHSIZE - 1)];
	return value;
}

void OsxFlashWrite(OsxMachine *machine, uint32 offset, int size, uint32 value)
{
	uint32 bank;
	uint8 data;
	offset &= OSXFLASHSIZE - 1;
	data = (uint8)value;
	switch (machine->flashmode) {
	case FLASHERASESETUP:
		if (data == 0xd0) {
			bank = offset & ~(uint32)(OSXFLASHBANK - 1);
			memset(machine->flash + bank, 0xff, OSXFLASHBANK);
			machine->flashdirty = 1;
		}
		machine->flashmode = FLASHSTATUS;
		return;
	case FLASHPROGRAMSETUP:
		machine->flash[offset] &= data;
		machine->flashdirty = 1;
		machine->flashmode = FLASHSTATUS;
		return;
	}
	switch (data) {
	case 0x20:
		machine->flashmode = FLASHERASESETUP;
		break;
	case 0x40:
	case 0x10:
		machine->flashmode = FLASHPROGRAMSETUP;
		break;
	case 0x70:
	case 0x50:
		machine->flashmode = FLASHSTATUS;
		break;
	default:
		machine->flashmode = FLASHREADARRAY;
		break;
	}
}
