/*
 *  osxdisplay.cpp - native display driver for Mac OS X guest resolution switching
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

#define DISPLAYNOERROR 0
#define DISPLAYCONTROLERROR -17
#define DISPLAYSTATUSERROR -18
#define DISPLAYPARAMERROR -50

#define DISPLAYCOMMANDCONTROL 4
#define DISPLAYCOMMANDSTATUS 5
#define DISPLAYCOMMANDKILL 6
#define DISPLAYCOMMANDKILLERROR -27

#define DISPLAYCSCODE 26
#define DISPLAYCSPARAM 28

#define DISPLAYSETMODE 2
#define DISPLAYSETENTRIES 3
#define DISPLAYSETGAMMA 4
#define DISPLAYGRAYPAGE 5
#define DISPLAYSETGRAY 6
#define DISPLAYSETINTERRUPT 7
#define DISPLAYDIRECTSETENTRIES 8
#define DISPLAYSWITCHMODE 10
#define DISPLAYSAVEPREFERRED 16

#define DISPLAYGETMODE 2
#define DISPLAYGETPAGES 4
#define DISPLAYGETBASEADDRESS 5
#define DISPLAYGETCURRENTMODE 10
#define DISPLAYGETCONNECTION 12
#define DISPLAYGETMODETIMING 13
#define DISPLAYGETPREFERRED 16
#define DISPLAYGETNEXTRESOLUTION 17
#define DISPLAYGETVIDEOPARAMETERS 18

#define DISPLAYSWITCHDEPTH 0
#define DISPLAYSWITCHDATA 2
#define DISPLAYSWITCHPAGE 6
#define DISPLAYSWITCHBASE 8

#define DISPLAYRESOLUTIONPREVIOUS 0
#define DISPLAYRESOLUTIONMODE 4
#define DISPLAYRESOLUTIONWIDTH 8
#define DISPLAYRESOLUTIONHEIGHT 12
#define DISPLAYRESOLUTIONREFRESH 16
#define DISPLAYRESOLUTIONDEPTH 20
#define DISPLAYRESOLUTIONFLAGS 22
#define DISPLAYRESOLUTIONRESERVED 26

#define DISPLAYPARAMETERSMODE 0
#define DISPLAYPARAMETERSDEPTH 4
#define DISPLAYPARAMETERSBLOCK 6
#define DISPLAYPARAMETERSPAGES 10
#define DISPLAYPARAMETERSDEVICE 14
#define DISPLAYPARAMETERSRESERVED 18

#define DISPLAYBLOCKOFFSET 0
#define DISPLAYBLOCKROWBYTES 4
#define DISPLAYBLOCKBOUNDS 6
#define DISPLAYBLOCKVERSION 14
#define DISPLAYBLOCKPACKTYPE 16
#define DISPLAYBLOCKPACKSIZE 18
#define DISPLAYBLOCKHRES 22
#define DISPLAYBLOCKVRES 26
#define DISPLAYBLOCKPIXELTYPE 30
#define DISPLAYBLOCKPIXELSIZE 32
#define DISPLAYBLOCKCOMPONENTS 34
#define DISPLAYBLOCKCOMPONENTSIZE 36
#define DISPLAYBLOCKPLANEBYTES 38

#define DISPLAYCONNECTIONTYPE 0
#define DISPLAYCONNECTIONTAGGEDTYPE 2
#define DISPLAYCONNECTIONTAGGEDDATA 3
#define DISPLAYCONNECTIONFLAGS 4
#define DISPLAYCONNECTIONCOMPONENT 8
#define DISPLAYCONNECTIONRESERVED 12

#define DISPLAYTIMINGMODE 0
#define DISPLAYTIMINGRESERVED 4
#define DISPLAYTIMINGFORMAT 8
#define DISPLAYTIMINGDATA 12
#define DISPLAYTIMINGFLAGS 16

#define DISPLAYDEPTHMODE 0x80u
#define DISPLAYMODECURRENT 0u
#define DISPLAYMODEFIRST 0xfffffffeu
#define DISPLAYMODENOMORE 0xfffffffdu
#define DISPLAYMODEBASE 0x81u
#define DISPLAYREFRESH (60u << 16)
#define DISPLAYRESOLUTION 0x00480000u
#define DISPLAYPIXELDIRECT 0x10
#define DISPLAYPIXELSIZE 32
#define DISPLAYCOMPONENTS 3
#define DISPLAYCOMPONENTSIZE 8
#define DISPLAYDEVICEDIRECT 2
#define DISPLAYMULTIMODECONNECT 6
#define DISPLAYTAGGEDTYPE 6
#define DISPLAYTAGGEDDATA 0x23
#define DISPLAYALLMODESVALID 1u
#define DISPLAYALLMODESSAFE 2u
#define DISPLAYTIMINGDECLARED 0x6465636cu
#define DISPLAYTIMINGVALID 1u
#define DISPLAYTIMINGSAFE 2u
#define DISPLAYTIMINGDEFAULT 4u
#define DISPLAYTIMINGSHOWN 8u
#define DISPLAYTIMINGUNKNOWN 0

#define DISPLAYDRIVERLOADTOC 0x80402808u
#define DISPLAYDRIVERLOADENTRY 0x800028d8u
#define DISPLAYDRIVERMOVECOUNT 0x7c0903a6u
#define DISPLAYDRIVERBRANCHCOUNT 0x4e800420u
#define DISPLAYDRIVERRETURN 0x4e800020u
#define DISPLAYDRIVERNOOP 0x60000000u

typedef struct OsxDisplayMode {
	uint32 width;
	uint32 height;
	uint32 timing;
} OsxDisplayMode;

static const uint8 OsxDisplayDriverImage[] = {
#include "../VideoDriverStub.i"
};

static const OsxDisplayMode OsxDisplayModes[] = {
	{ 640, 480, 150 },
	{ 800, 600, 182 },
	{ 1024, 768, 204 },
	{ 1152, 870, 220 },
	{ 1280, 960, 250 },
	{ 1280, 1024, 262 },
	{ 1600, 1200, 286 }
};

#define DISPLAYMODECOUNT (sizeof(OsxDisplayModes) / sizeof(OsxDisplayModes[0]))

static uint32 OsxGuestDisplayBig32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static void OsxGuestDisplayStoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

uint8 *OsxGuestDisplayDriver(uint32 *length)
{
	uint8 *image;
	uint32 offset;
	image = (uint8 *)malloc(sizeof(OsxDisplayDriverImage));
	memcpy(image, OsxDisplayDriverImage, sizeof(OsxDisplayDriverImage));
	*length = sizeof(OsxDisplayDriverImage);
	for (offset = 0; offset + 16 <= sizeof(OsxDisplayDriverImage); offset += 4) {
		if (OsxGuestDisplayBig32(image + offset) == DISPLAYDRIVERLOADTOC && OsxGuestDisplayBig32(image + offset + 4) == DISPLAYDRIVERLOADENTRY &&
			OsxGuestDisplayBig32(image + offset + 8) == DISPLAYDRIVERMOVECOUNT && OsxGuestDisplayBig32(image + offset + 12) == DISPLAYDRIVERBRANCHCOUNT) {
			OsxGuestDisplayStoreBig32(image + offset, OSXDISPLAYTRAP);
			OsxGuestDisplayStoreBig32(image + offset + 4, DISPLAYDRIVERRETURN);
			OsxGuestDisplayStoreBig32(image + offset + 8, DISPLAYDRIVERNOOP);
			OsxGuestDisplayStoreBig32(image + offset + 12, DISPLAYDRIVERNOOP);
			return image;
		}
	}
	free(image);
	return NULL;
}

static int OsxGuestDisplayModeSize(OsxMachine *machine, uint32 mode, const OsxDisplayMode **found)
{
	uint32 index;
	if (mode == DISPLAYMODECURRENT)
		mode = machine->displaymode;
	if (mode < DISPLAYMODEBASE || mode >= DISPLAYMODEBASE + DISPLAYMODECOUNT)
		return mode == DISPLAYMODEBASE + DISPLAYMODECOUNT && machine->displaycustom;
	index = mode - DISPLAYMODEBASE;
	*found = &OsxDisplayModes[index];
	return OsxDisplayModes[index].width <= machine->maxwidth && OsxDisplayModes[index].height <= machine->maxheight;
}

static void OsxGuestDisplayDimensions(OsxMachine *machine, uint32 mode, uint32 *width, uint32 *height, uint32 *timing)
{
	const OsxDisplayMode *found;
	found = NULL;
	OsxGuestDisplayModeSize(machine, mode, &found);
	*width = machine->maxwidth;
	*height = machine->maxheight;
	*timing = DISPLAYTIMINGUNKNOWN;
	if (found) {
		*width = found->width;
		*height = found->height;
		*timing = found->timing;
	}
}

void OsxGuestDisplayReset(OsxMachine *machine)
{
	uint32 index;
	machine->maxwidth = machine->width;
	machine->maxheight = machine->height;
	machine->displaycustom = 1;
	machine->displaymode = DISPLAYMODEBASE + DISPLAYMODECOUNT;
	for (index = 0; index < DISPLAYMODECOUNT; index++) {
		if (OsxDisplayModes[index].width == machine->width && OsxDisplayModes[index].height == machine->height) {
			machine->displaycustom = 0;
			machine->displaymode = DISPLAYMODEBASE + index;
		}
	}
	machine->displaypreferred = machine->displaymode;
}

static uint32 OsxGuestDisplayRead(OsxCpu *cpu, uint32 address, int size)
{
	uint32 value;
	uint8 *host;
	int index;
	value = 0;
	for (index = 0; index < size; index++) {
		host = OsxGuestCpuVirtualHost(cpu, address + index, OSXTLBREAD);
		value <<= 8;
		if (host)
			value |= *host;
	}
	return value;
}

static void OsxGuestDisplayWrite(OsxCpu *cpu, uint32 address, int size, uint32 value)
{
	uint8 *host;
	int index;
	for (index = size - 1; index >= 0; index--) {
		host = OsxGuestCpuVirtualHost(cpu, address + index, OSXTLBWRITE);
		if (host)
			*host = (uint8)value;
		value >>= 8;
	}
}

static uint32 OsxGuestDisplayNextMode(OsxMachine *machine, uint32 previous)
{
	const OsxDisplayMode *found;
	uint32 mode;
	if (previous == DISPLAYMODECURRENT)
		return machine->displaymode;
	mode = DISPLAYMODEBASE;
	if (previous != DISPLAYMODEFIRST)
		mode = previous + 1;
	for (; mode <= DISPLAYMODEBASE + DISPLAYMODECOUNT; mode++)
		if (OsxGuestDisplayModeSize(machine, mode, &found))
			return mode;
	return DISPLAYMODENOMORE;
}

static int OsxGuestDisplayNextResolution(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	const OsxDisplayMode *found;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYRESOLUTIONPREVIOUS, 4);
	if (mode != DISPLAYMODECURRENT && mode != DISPLAYMODEFIRST && !OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	mode = OsxGuestDisplayNextMode(machine, mode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONMODE, 4, mode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONFLAGS, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONRESERVED, 4, 0);
	if (mode == DISPLAYMODENOMORE)
		return DISPLAYNOERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONWIDTH, 4, width);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONHEIGHT, 4, height);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONREFRESH, 4, DISPLAYREFRESH);
	OsxGuestDisplayWrite(cpu, param + DISPLAYRESOLUTIONDEPTH, 2, DISPLAYDEPTHMODE);
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayVideoParameters(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 block;
	uint32 width;
	uint32 height;
	uint32 timing;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSMODE, 4);
	if (!OsxGuestDisplayModeSize(machine, mode, &found) || OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSDEPTH, 2) != DISPLAYDEPTHMODE)
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	block = OsxGuestDisplayRead(cpu, param + DISPLAYPARAMETERSBLOCK, 4);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKOFFSET, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKROWBYTES, 2, machine->rowbytes);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS + 4, 2, height);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKBOUNDS + 6, 2, width);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKVERSION, 2, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPACKTYPE, 2, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPACKSIZE, 4, 0);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKHRES, 4, DISPLAYRESOLUTION);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKVRES, 4, DISPLAYRESOLUTION);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPIXELTYPE, 2, DISPLAYPIXELDIRECT);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPIXELSIZE, 2, DISPLAYPIXELSIZE);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKCOMPONENTS, 2, DISPLAYCOMPONENTS);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKCOMPONENTSIZE, 2, DISPLAYCOMPONENTSIZE);
	OsxGuestDisplayWrite(cpu, block + DISPLAYBLOCKPLANEBYTES, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSPAGES, 4, 1);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSDEVICE, 4, DISPLAYDEVICEDIRECT);
	OsxGuestDisplayWrite(cpu, param + DISPLAYPARAMETERSRESERVED, 4, 0);
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayModeTiming(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	uint32 flags;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYTIMINGMODE, 4);
	if (!OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	flags = DISPLAYTIMINGVALID | DISPLAYTIMINGSAFE | DISPLAYTIMINGSHOWN;
	if (mode == machine->displaypreferred)
		flags |= DISPLAYTIMINGDEFAULT;
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGRESERVED, 4, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGFORMAT, 4, DISPLAYTIMINGDECLARED);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGDATA, 4, timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYTIMINGFLAGS, 4, flags);
	return DISPLAYNOERROR;
}

static void OsxGuestDisplayCurrentMode(OsxMachine *machine, OsxCpu *cpu, uint32 param)
{
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDEPTH, 2, DISPLAYDEPTHMODE);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDATA, 4, machine->displaymode);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHPAGE, 2, 0);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
}

static int OsxGuestDisplayStatus(OsxMachine *machine, OsxCpu *cpu, uint32 code, uint32 param)
{
	switch (code) {
	case DISPLAYGETMODE:
	case DISPLAYGETCURRENTMODE:
		OsxGuestDisplayCurrentMode(machine, cpu, param);
		return DISPLAYNOERROR;
	case DISPLAYGETPAGES:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHPAGE, 2, 1);
		return DISPLAYNOERROR;
	case DISPLAYGETBASEADDRESS:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
		return DISPLAYNOERROR;
	case DISPLAYGETCONNECTION:
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTYPE, 2, DISPLAYMULTIMODECONNECT);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTAGGEDTYPE, 1, DISPLAYTAGGEDTYPE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONTAGGEDDATA, 1, DISPLAYTAGGEDDATA);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONFLAGS, 4, DISPLAYALLMODESVALID | DISPLAYALLMODESSAFE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONCOMPONENT, 4, 0);
		OsxGuestDisplayWrite(cpu, param + DISPLAYCONNECTIONRESERVED, 4, 0);
		return DISPLAYNOERROR;
	case DISPLAYGETMODETIMING:
		return OsxGuestDisplayModeTiming(machine, cpu, param);
	case DISPLAYGETPREFERRED:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDEPTH, 2, DISPLAYDEPTHMODE);
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHDATA, 4, machine->displaypreferred);
		return DISPLAYNOERROR;
	case DISPLAYGETNEXTRESOLUTION:
		return OsxGuestDisplayNextResolution(machine, cpu, param);
	case DISPLAYGETVIDEOPARAMETERS:
		return OsxGuestDisplayVideoParameters(machine, cpu, param);
	}
	return DISPLAYSTATUSERROR;
}

static int OsxGuestDisplaySwitch(OsxMachine *machine, OsxCpu *cpu, uint32 param, int *resized)
{
	const OsxDisplayMode *found;
	uint32 mode;
	uint32 width;
	uint32 height;
	uint32 timing;
	mode = OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDATA, 4);
	if (OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDEPTH, 2) != DISPLAYDEPTHMODE || !OsxGuestDisplayModeSize(machine, mode, &found))
		return DISPLAYPARAMERROR;
	OsxGuestDisplayDimensions(machine, mode, &width, &height, &timing);
	OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
	if (mode == machine->displaymode)
		return DISPLAYNOERROR;
	machine->displaymode = mode;
	machine->width = width;
	machine->height = height;
	memset(machine->vram, 0, machine->rowbytes * machine->maxheight);
	*resized = 1;
	return DISPLAYNOERROR;
}

static int OsxGuestDisplayControl(OsxMachine *machine, OsxCpu *cpu, uint32 code, uint32 param, int *resized)
{
	switch (code) {
	case DISPLAYSETMODE:
		OsxGuestDisplayWrite(cpu, param + DISPLAYSWITCHBASE, 4, OSXVRAMBASE);
		return DISPLAYNOERROR;
	case DISPLAYSETENTRIES:
	case DISPLAYSETGAMMA:
	case DISPLAYGRAYPAGE:
	case DISPLAYSETGRAY:
	case DISPLAYSETINTERRUPT:
	case DISPLAYDIRECTSETENTRIES:
		return DISPLAYNOERROR;
	case DISPLAYSWITCHMODE:
		return OsxGuestDisplaySwitch(machine, cpu, param, resized);
	case DISPLAYSAVEPREFERRED:
		machine->displaypreferred = OsxGuestDisplayRead(cpu, param + DISPLAYSWITCHDATA, 4);
		return DISPLAYNOERROR;
	}
	return DISPLAYCONTROLERROR;
}

int OsxGuestDisplayDriverCall(OsxMachine *machine, OsxCpu *cpu)
{
	uint32 contents;
	uint32 code;
	uint32 param;
	int result;
	int resized;
	contents = cpu->gpr[5];
	resized = 0;
	result = DISPLAYNOERROR;
	code = OsxGuestDisplayRead(cpu, contents + DISPLAYCSCODE, 2);
	param = OsxGuestDisplayRead(cpu, contents + DISPLAYCSPARAM, 4);
	if (cpu->gpr[6] == DISPLAYCOMMANDCONTROL)
		result = OsxGuestDisplayControl(machine, cpu, code, param, &resized);
	else if (cpu->gpr[6] == DISPLAYCOMMANDSTATUS)
		result = OsxGuestDisplayStatus(machine, cpu, code, param);
	else if (cpu->gpr[6] == DISPLAYCOMMANDKILL)
		result = DISPLAYCOMMANDKILLERROR;
	if (machine->log)
		fprintf(machine->log, "display command %u code %u result %d mode %x %ux%u\n", cpu->gpr[6], code, result, machine->displaymode, machine->width, machine->height);
	cpu->gpr[3] = (uint32)result;
	return resized;
}
