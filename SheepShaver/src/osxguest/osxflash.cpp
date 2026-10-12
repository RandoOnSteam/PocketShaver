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
