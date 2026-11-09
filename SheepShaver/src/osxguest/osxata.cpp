/*
 *  osxata.cpp - OSX ATA bus emulation
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
#include "osxmachine.h"
#if defined(BINCUE)
#include "bincue.h"
#endif

#define ATABUSY 0x80
#define ATAREADY 0x40
#define ATASEEK 0x10
#define ATADRQ 0x08
#define ATAERROR 0x01
#define ATAABORT 0x04

#define ATADIRECTIONNONE 0
#define ATADIRECTIONIN 1
#define ATADIRECTIONOUT 2

#define ATADMANONE 0
#define ATADMAREAD 1
#define ATADMAWRITE 2

#define SENSENONE 0x00
#define SENSENOTREADY 0x02
#define SENSEILLEGALREQUEST 0x05
#define SENSEUNITATTENTION 0x06

static void OsxGuestInsertNextMedia(OsxMachine *machine, OsxAtaDrive *drive);

static OsxAtaDrive *OsxGuestSelectedDrive(OsxAtaBus *bus)
{
	return &bus->drives[(bus->select >> 4) & 1];
}

static void OsxGuestRaiseIrq(OsxMachine *machine, OsxAtaBus *bus)
{
	bus->pendingstatus = bus->status;
	bus->status = ATABUSY | ATAREADY;
	bus->irqdeadline = OsxGuestCpuTimebase(machine->cpu) + OSXATADELAY;
}

void OsxGuestAtaPoll(OsxMachine *machine, uint64 now)
{
	OsxAtaBus *bus;
	int index;
	int unit;
	for (index = 0; index < OSXATABUSES; index++) {
		bus = &machine->ata[index];
		for (unit = 0; unit < 2; unit++)
			if (bus->drives[unit].insertdeadline && now >= bus->drives[unit].insertdeadline)
				OsxGuestInsertNextMedia(machine, &bus->drives[unit]);
		if (bus->irqdeadline == 0 || now < bus->irqdeadline)
			continue;
		bus->irqdeadline = 0;
		bus->status = bus->pendingstatus;
		bus->irqline = 1;
		if (!(bus->control & 2))
			OsxGuestPicSetLine(machine, bus->irq, 1);
	}
}

static void OsxGuestLowerIrq(OsxMachine *machine, OsxAtaBus *bus)
{
	if (bus->irqdeadline) {
		bus->irqdeadline = 0;
		bus->status = bus->pendingstatus;
	}
	bus->irqline = 0;
	OsxGuestPicSetLine(machine, bus->irq, 0);
}

static void OsxGuestSetSignature(OsxAtaBus *bus, OsxAtaDrive *drive)
{
	bus->count = 1;
	bus->sector = 1;
	if (drive->atapi) {
		bus->cylinderlow = 0x14;
		bus->cylinderhigh = 0xeb;
	} else {
		bus->cylinderlow = 0;
		bus->cylinderhigh = 0;
	}
	bus->select &= 0x10;
}

static void OsxGuestResetBus(OsxAtaBus *bus)
{
	bus->error = 1;
	bus->feature = 0;
	bus->status = ATAREADY | ATASEEK;
	bus->transferdirection = ATADIRECTIONNONE;
	bus->dmapending = ATADMANONE;
	bus->packetphase = 0;
	bus->atapitransfer = 0;
	bus->bufferlength = 0;
	bus->bufferposition = 0;
	bus->drqremaining = 0;
	bus->remaining = 0;
	bus->select = 0xa0;
	OsxGuestSetSignature(bus, &bus->drives[0]);
}

void OsxGuestAtaReset(OsxMachine *machine)
{
	OsxAtaBus *bus;
	int index;
	for (index = 0; index < OSXATABUSES; index++) {
		bus = &machine->ata[index];
		if (bus->buffer == NULL)
			bus->buffer = (uint8 *)malloc(OSXATABUFFER);
		bus->control = 0;
		bus->timing = 0;
		bus->irqline = 0;
		OsxGuestResetBus(bus);
	}
	machine->ata[0].irq = OSXIRQIDE0;
	machine->ata[1].irq = OSXIRQIDE1;
	machine->ata[0].dmachannel = OSXDBDMAIDE0;
	machine->ata[1].dmachannel = OSXDBDMAIDE1;
	machine->dbdma[OSXDBDMAIDE0].irq = OSXIRQIDE0DMA;
	machine->dbdma[OSXDBDMAIDE1].irq = OSXIRQIDE1DMA;
}

static void OsxGuestPutString(uint8 *identify, int word, int words, const char *text)
{
	int index;
	int length;
	char character;
	length = (int)strlen(text);
	for (index = 0; index < words * 2; index++) {
		if (index < length)
			character = text[index];
		else
			character = ' ';
		identify[word * 2 + (index ^ 1)] = (uint8)character;
	}
}

static void OsxGuestPutWord(uint8 *identify, int word, uint32 value)
{
	identify[word * 2] = (uint8)value;
	identify[word * 2 + 1] = (uint8)(value >> 8);
}

static void OsxGuestBuildIdentify(OsxAtaDrive *drive, uint8 *identify)
{
	uint64 cylinders;
	uint32 capacity;
	memset(identify, 0, 512);
	if (drive->atapi) {
		OsxGuestPutWord(identify, 0, 0x85c0);
		OsxGuestPutWord(identify, 49, 0x0300);
		OsxGuestPutWord(identify, 53, 0x0007);
		OsxGuestPutWord(identify, 63, 0x0407);
		OsxGuestPutWord(identify, 64, 0x0003);
		OsxGuestPutWord(identify, 65, 0x00b4);
		OsxGuestPutWord(identify, 66, 0x00b4);
		OsxGuestPutWord(identify, 67, 0x012c);
		OsxGuestPutWord(identify, 68, 0x00b4);
		OsxGuestPutWord(identify, 71, 30);
		OsxGuestPutWord(identify, 72, 30);
		OsxGuestPutWord(identify, 80, 0x001e);
	} else {
		cylinders = drive->sectors / (16 * 63);
		if (cylinders > 16383)
			cylinders = 16383;
		capacity = (uint32)drive->sectors;
		if (drive->sectors > 0x0fffffff)
			capacity = 0x0fffffff;
		OsxGuestPutWord(identify, 0, 0x0040);
		OsxGuestPutWord(identify, 1, (uint32)cylinders);
		OsxGuestPutWord(identify, 3, 16);
		OsxGuestPutWord(identify, 4, 512 * 63);
		OsxGuestPutWord(identify, 5, 512);
		OsxGuestPutWord(identify, 6, 63);
		OsxGuestPutWord(identify, 20, 3);
		OsxGuestPutWord(identify, 21, 512);
		OsxGuestPutWord(identify, 22, 4);
		OsxGuestPutWord(identify, 47, 0x8010);
		OsxGuestPutWord(identify, 48, 0);
		OsxGuestPutWord(identify, 49, 0x0300);
		OsxGuestPutWord(identify, 51, 0x0200);
		OsxGuestPutWord(identify, 52, 0x0200);
		OsxGuestPutWord(identify, 53, 0x0007);
		OsxGuestPutWord(identify, 54, (uint32)cylinders);
		OsxGuestPutWord(identify, 55, 16);
		OsxGuestPutWord(identify, 56, 63);
		OsxGuestPutWord(identify, 57, (uint32)(cylinders * 16 * 63) & 0xffff);
		OsxGuestPutWord(identify, 58, (uint32)(cylinders * 16 * 63) >> 16);
		if (drive->multiple)
			OsxGuestPutWord(identify, 59, 0x0100 | drive->multiple);
		OsxGuestPutWord(identify, 60, capacity & 0xffff);
		OsxGuestPutWord(identify, 61, capacity >> 16);
		OsxGuestPutWord(identify, 63, 0x0407);
		OsxGuestPutWord(identify, 64, 0x0003);
		OsxGuestPutWord(identify, 65, 0x0078);
		OsxGuestPutWord(identify, 66, 0x0078);
		OsxGuestPutWord(identify, 67, 0x0078);
		OsxGuestPutWord(identify, 68, 0x0078);
		OsxGuestPutWord(identify, 80, 0x00f0);
		OsxGuestPutWord(identify, 81, 0x0016);
		OsxGuestPutWord(identify, 82, 0x4000);
		OsxGuestPutWord(identify, 83, 0x4000);
		OsxGuestPutWord(identify, 84, 0x4000);
		OsxGuestPutWord(identify, 85, 0x4000);
		OsxGuestPutWord(identify, 86, 0x4000);
		OsxGuestPutWord(identify, 87, 0x4000);
	}
	OsxGuestPutString(identify, 10, 10, "SHEEPSHAVER0001");
	OsxGuestPutString(identify, 23, 4, "1.0");
	OsxGuestPutString(identify, 27, 20, drive->model);
}

static int OsxGuestOpenMedia(OsxAtaDrive *drive, const char *path, int atapi)
{
	loff_t size;
	drive->file = Sys_open(path, atapi != 0, atapi != 0);
	if (drive->file == NULL) {
		drive->readonly = 1;
		drive->file = Sys_open(path, true, atapi != 0);
		if (drive->file == NULL)
			return 0;
	} else {
		drive->readonly = atapi != 0;
	}
	size = SysGetFileSize(drive->file);
	if (atapi)
		drive->sectors = (uint64)size / OSXATAPISECTOR;
	else
		drive->sectors = (uint64)size / OSXATASECTOR;
	drive->dvd = atapi && drive->sectors > OSXATAPICDSECTORS;
#if defined(BINCUE)
	if (atapi)
		drive->imagepath = strdup(path);
#endif
	drive->mediapresent = !atapi || (SysIsDiskInserted(drive->file) && drive->sectors > 0);
	return 1;
}

static void OsxGuestPollMedia(OsxMachine *machine, OsxAtaDrive *drive)
{ /* This prevents thrashing from
		getting ASC 0x11 "unrecovered read error" 
		instead of ASC 0x3A "not ready, medium not present" */
	if (drive->mediapresent || drive->file == NULL || !SysIsDiskInserted(drive->file))
		return;
	drive->sectors = (uint64)SysGetFileSize(drive->file) / OSXATAPISECTOR;
	if (drive->sectors == 0)
		return;
	drive->dvd = drive->sectors > OSXATAPICDSECTORS;
	drive->mediapresent = 1;
	drive->unitattention = 1;
	drive->mediachanged = 1;
	if (machine->log)
		fprintf(machine->log, "cdrom media detected\n");
}

void OsxGuestAtaCloseMedia(OsxAtaDrive *drive)
{
#if defined(BINCUE)
	if (drive->cdimage)
		close_bincue(drive->cdimage);
#endif
	if (drive->file)
		Sys_close(drive->file);
	free(drive->imagepath);
	drive->file = NULL;
	drive->cdimage = NULL;
	drive->imagepath = NULL;
}

int OsxGuestAtaAttach(OsxMachine *machine, int busindex, int unit, const char *path, int atapi)
{
	OsxAtaDrive *drive;
	drive = &machine->ata[busindex].drives[unit];
	if (!OsxGuestOpenMedia(drive, path, atapi))
		return 0;
	drive->atapi = atapi;
	drive->present = 1;
	drive->multiple = 16;
	drive->sensekey = SENSENONE;
	drive->asc = 0;
	drive->ascq = 0;
	drive->mediaindex = 0;
	drive->mediachanged = 0;
	drive->insertdeadline = 0;
	if (atapi)
		strcpy(drive->model, "SheepShaver ATAPI CD-ROM");
	else
		strcpy(drive->model, "SheepShaver ATA Disk");
	if (unit == 0)
		OsxGuestSetSignature(&machine->ata[busindex], drive);
	return 1;
}

static void OsxGuestEjectMedia(OsxMachine *machine, OsxAtaDrive *drive)
{
	if (!drive->mediapresent)
		return;
	OsxGuestAtaCloseMedia(drive);
	drive->mediapresent = 0;
	drive->mediachanged = 1;
	drive->sectors = 0;
	if (machine->cdromcount > 0)
		drive->insertdeadline = OsxGuestCpuTimebase(machine->cpu) + (uint64)OSXTIMEBASEFREQUENCY * 3;
	if (machine->log)
		fprintf(machine->log, "cdrom ejected\n");
}

static void OsxGuestInsertNextMedia(OsxMachine *machine, OsxAtaDrive *drive)
{
	drive->insertdeadline = 0;
	drive->mediaindex = (drive->mediaindex + 1) % machine->cdromcount;
	if (!OsxGuestOpenMedia(drive, machine->cdrompaths[drive->mediaindex], 1))
		return;
	drive->unitattention = 1;
	drive->mediachanged = 1;
	if (machine->log)
		fprintf(machine->log, "cdrom inserted %s\n", machine->cdrompaths[drive->mediaindex]);
}

static void OsxGuestCompleteCommand(OsxMachine *machine, OsxAtaBus *bus, int failed)
{
	bus->transferdirection = ATADIRECTIONNONE;
	bus->drqremaining = 0;
	bus->dmapending = ATADMANONE;
	if (failed) {
		bus->status = ATAREADY | ATAERROR;
		bus->error = ATAABORT;
	} else {
		bus->status = ATAREADY | ATASEEK;
	}
	OsxGuestRaiseIrq(machine, bus);
}

static void OsxGuestAtapiComplete(OsxMachine *machine, OsxAtaBus *bus, OsxAtaDrive *drive, int failed)
{
	bus->transferdirection = ATADIRECTIONNONE;
	bus->drqremaining = 0;
	bus->dmapending = ATADMANONE;
	bus->atapitransfer = 0;
	bus->count = 3;
	if (failed) {
		bus->status = ATAREADY | ATAERROR;
		bus->error = (uint8)(drive->sensekey << 4);
	} else {
		bus->status = ATAREADY | ATASEEK;
		bus->error = 0;
	}
	OsxGuestRaiseIrq(machine, bus);
}

static void OsxGuestAtapiFail(OsxMachine *machine, OsxAtaBus *bus, OsxAtaDrive *drive, uint8 sensekey, uint8 asc)
{
	drive->sensekey = sensekey;
	drive->asc = asc;
	drive->ascq = 0;
	OsxGuestAtapiComplete(machine, bus, drive, 1);
}

static void OsxGuestApplyPatches(OsxAtaBus *bus, OsxAtaDrive *drive, uint8 *data, uint64 start, uint32 length)
{
	OsxDiskPatch *patch;
	int index;
	int byte;
	for (index = 0; index < bus->patchcount; index++) {
		patch = &bus->patches[index];
		if (patch->atapionly && !drive->atapi)
			continue;
		if ((patch->drive && patch->drive != drive) || patch->offset >= start + length || patch->offset + patch->length <= start)
			continue;
		for (byte = 0; byte < patch->length; byte++)
			if (patch->offset + byte >= start && patch->offset + byte < start + length)
				data[patch->offset + byte - start] = patch->bytes[byte];
	}
}

static int OsxGuestReadCDSector(OsxAtaDrive *drive, void *buffer, uint32 sector, uint8 type, uint8 flags)
{
#if defined(BINCUE)
	if (drive->imagepath) {
		drive->cdimage = OpenCDImage(drive->imagepath);
		free(drive->imagepath);
		drive->imagepath = NULL;
	}
	if (drive->cdimage)
		return ReadCDSectorBinCue(drive->cdimage, buffer, sector, type, flags);
#endif
#if HAVE_SYS_CD_READ_SECTOR
	return SysCDReadSector(drive->file, buffer, sector, type, flags);
#else
	return -1;
#endif
}
static int OsxGuestFillBuffer(OsxAtaBus *bus, OsxAtaDrive *drive)
{
	uint32 sectors;
	uint32 bytes;
	sectors = bus->remaining;
	if (sectors > OSXATABUFFER / bus->sectorsize)
		sectors = OSXATABUFFER / bus->sectorsize;
	bytes = sectors * bus->sectorsize;
	if (bus->lba + sectors > drive->sectors)
		return 0;
	if (drive->atapi && bus->packet[0] == 0xbe) {
		int length;
		uint32 sector;
		bytes = 0;
		for (sector = 0; sector < sectors; sector++) {
			length = OsxGuestReadCDSector(drive, bus->buffer + bytes, (uint32)(bus->lba + sector),
				(bus->packet[1] >> 2) & 7, bus->packet[9]);
			if (length < 0)
				return 0;
			if (length == OSXATAPISECTOR)
				OsxGuestApplyPatches(bus, drive, bus->buffer + bytes, (bus->lba + sector) * OSXATAPISECTOR, length);
			bytes += length;
		}
	} else {
		if (Sys_read(drive->file, bus->buffer, (loff_t)(bus->lba * bus->sectorsize), bytes) != bytes)
			return 0;
		OsxGuestApplyPatches(bus, drive, bus->buffer, bus->lba * bus->sectorsize, bytes);
	}
	bus->lba += sectors;
	bus->remaining -= sectors;
	bus->bufferlength = bytes;
	bus->bufferposition = 0;
	return 1;
}

static void OsxGuestStartDrqBlock(OsxMachine *machine, OsxAtaBus *bus)
{
	uint32 chunk;
	chunk = bus->bufferlength - bus->bufferposition;
	if (chunk > bus->drqblock)
		chunk = bus->drqblock;
	bus->drqremaining = chunk;
	if (bus->atapitransfer) {
		bus->cylinderlow = (uint8)chunk;
		bus->cylinderhigh = (uint8)(chunk >> 8);
		bus->count = 2;
	}
	bus->status = ATAREADY | ATASEEK | ATADRQ;
	OsxGuestRaiseIrq(machine, bus);
}

static void OsxGuestDataInBlockDone(OsxMachine *machine, OsxAtaBus *bus)
{
	OsxAtaDrive *drive;
	drive = OsxGuestSelectedDrive(bus);
	while (bus->bufferposition >= bus->bufferlength) {
		if (bus->remaining == 0) {
			if (bus->atapitransfer) {
				OsxGuestAtapiComplete(machine, bus, drive, 0);
			} else {
				bus->transferdirection = ATADIRECTIONNONE;
				bus->status = ATAREADY | ATASEEK;
			}
			return;
		}
		if (!OsxGuestFillBuffer(bus, drive)) {
			if (bus->atapitransfer)
				OsxGuestAtapiFail(machine, bus, drive, 0x03, 0x11);
			else
				OsxGuestCompleteCommand(machine, bus, 1);
			return;
		}
	}
	OsxGuestStartDrqBlock(machine, bus);
}

static void OsxGuestBeginSectorsIn(OsxMachine *machine, OsxAtaBus *bus, uint64 lba, uint32 count, uint32 sectorsize, uint32 drqblock, int dma)
{
	bus->lba = lba;
	bus->remaining = count;
	bus->sectorsize = sectorsize;
	bus->bufferlength = 0;
	bus->bufferposition = 0;
	bus->transferdirection = ATADIRECTIONIN;
	bus->drqblock = drqblock;
	if (dma) {
		bus->dmapending = ATADMAREAD;
		bus->status = ATAREADY | ATABUSY;
		OsxGuestAtaDmaReady(machine, (int)(bus - machine->ata));
		return;
	}
	OsxGuestDataInBlockDone(machine, bus);
}

static void OsxGuestBeginBufferIn(OsxMachine *machine, OsxAtaBus *bus, uint32 length, uint32 drqblock, int dma)
{
	bus->remaining = 0;
	bus->bufferlength = length;
	bus->bufferposition = 0;
	bus->transferdirection = ATADIRECTIONIN;
	bus->drqblock = drqblock;
	if (dma) {
		bus->dmapending = ATADMAREAD;
		bus->status = ATAREADY | ATABUSY;
		OsxGuestAtaDmaReady(machine, (int)(bus - machine->ata));
		return;
	}
	if (length == 0) {
		OsxGuestAtapiComplete(machine, bus, OsxGuestSelectedDrive(bus), 0);
		return;
	}
	OsxGuestStartDrqBlock(machine, bus);
}

static uint64 OsxGuestCurrentLba(OsxAtaBus *bus)
{
	uint32 cylinder;
	uint32 head;
	if (bus->select & 0x40)
		return ((uint64)(bus->select & 0x0f) << 24) | ((uint32)bus->cylinderhigh << 16) | ((uint32)bus->cylinderlow << 8) | bus->sector;
	cylinder = bus->cylinderlow | ((uint32)bus->cylinderhigh << 8);
	head = bus->select & 0x0f;
	return ((uint64)cylinder * 16 + head) * 63 + bus->sector - 1;
}

static uint32 OsxGuestCurrentCount(OsxAtaBus *bus)
{
	if (bus->count == 0)
		return 256;
	return bus->count;
}

static void OsxGuestBeginWrite(OsxMachine *machine, OsxAtaBus *bus, uint32 drqblock, int dma)
{
	OsxAtaDrive *drive;
	drive = OsxGuestSelectedDrive(bus);
	if (drive->readonly) {
		OsxGuestCompleteCommand(machine, bus, 1);
		return;
	}
	bus->lba = OsxGuestCurrentLba(bus);
	bus->remaining = OsxGuestCurrentCount(bus);
	bus->sectorsize = OSXATASECTOR;
	if (bus->lba + bus->remaining > drive->sectors) {
		OsxGuestCompleteCommand(machine, bus, 1);
		return;
	}
	bus->transferdirection = ATADIRECTIONOUT;
	if (dma) {
		bus->dmapending = ATADMAWRITE;
		bus->status = ATAREADY | ATABUSY;
		OsxGuestAtaDmaReady(machine, (int)(bus - machine->ata));
		return;
	}
	bus->drqblock = drqblock;
	bus->bufferposition = 0;
	bus->bufferlength = bus->remaining * OSXATASECTOR;
	if (bus->bufferlength > drqblock)
		bus->bufferlength = drqblock;
	bus->drqremaining = bus->bufferlength;
	bus->status = ATAREADY | ATASEEK | ATADRQ;
}

static void OsxGuestWriteBlockDone(OsxMachine *machine, OsxAtaBus *bus)
{
	OsxAtaDrive *drive;
	uint32 sectors;
	drive = OsxGuestSelectedDrive(bus);
	sectors = bus->bufferlength / OSXATASECTOR;
	if (Sys_write(drive->file, bus->buffer, (loff_t)(bus->lba * OSXATASECTOR), bus->bufferlength) != bus->bufferlength) {
		OsxGuestCompleteCommand(machine, bus, 1);
		return;
	}
	bus->lba += sectors;
	bus->remaining -= sectors;
	if (bus->remaining == 0) {
		OsxGuestCompleteCommand(machine, bus, 0);
		return;
	}
	bus->bufferposition = 0;
	bus->bufferlength = bus->remaining * OSXATASECTOR;
	if (bus->bufferlength > bus->drqblock)
		bus->bufferlength = bus->drqblock;
	bus->drqremaining = bus->bufferlength;
	bus->status = ATAREADY | ATASEEK | ATADRQ;
	OsxGuestRaiseIrq(machine, bus);
}

static uint32 OsxGuestAllocation(uint32 wanted, uint32 available)
{
	if (wanted < available)
		return wanted;
	return available;
}

static void OsxGuestPutBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static void OsxGuestPutBig16(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 8);
	data[1] = (uint8)value;
}

static void OsxGuestPutAddress(uint8 *data, uint32 lba, int msf)
{
	uint32 frames;
	if (!msf) {
		OsxGuestPutBig32(data, lba);
		return;
	}
	frames = lba + 150;
	data[0] = 0;
	data[1] = (uint8)(frames / (75 * 60));
	data[2] = (uint8)((frames / 75) % 60);
	data[3] = (uint8)(frames % 75);
}

static uint32 OsxGuestBuildToc(OsxAtaDrive *drive, uint8 *data, uint8 *packet)
{
	uint8 toc[804];
	uint8 *entry;
	uint32 length;
	uint32 format;
	uint32 frames;
	uint32 address;
	int index;
	int tracks;
	int msf;
	msf = (packet[1] & 2) != 0;
	format = packet[2] & 0x0f;
	if (format == 0)
		format = packet[9] >> 6;
	memset(toc, 0, sizeof(toc));
	if (!SysCDReadTOC(drive->file, toc)) {
		toc[2] = 1;
		toc[3] = 1;
		toc[5] = 0x14;
		toc[6] = 1;
		OsxGuestPutAddress(toc + 8, 0, 1);
		toc[13] = 0x14;
		toc[14] = 0xaa;
		OsxGuestPutAddress(toc + 16, (uint32)drive->sectors, 1);
	}
	tracks = toc[3] - toc[2] + 1;
	memset(data, 0, 4 + 11 * 102);
	data[2] = toc[2];
	data[3] = toc[3];
	length = 4;
	if (format == 1) {
		data[2] = 1;
		data[3] = 1;
		memcpy(data + 4, toc + 4, 8);
		if (!msf) {
			frames = ((uint32)toc[9] * 60 + toc[10]) * 75 + toc[11];
			OsxGuestPutBig32(data + 8, frames - 150);
		}
		length += 8;
	} else if (format == 2) {
		data[2] = 1;
		data[3] = 1;
		for (index = 0; index < 3; index++) {
			data[length] = 1;
			data[length + 1] = toc[5];
			data[length + 3] = (uint8)(0xa0 + index);
			if (index == 0)
				data[length + 8] = toc[2];
			else if (index == 1)
				data[length + 8] = toc[3];
			else
				memcpy(data + length + 8, toc + 4 + tracks * 8 + 5, 3);
			length += 11;
		}
		for (index = 0; index < tracks; index++) {
			entry = toc + 4 + index * 8;
			data[length] = 1;
			data[length + 1] = entry[1];
			data[length + 3] = entry[2];
			memcpy(data + length + 8, entry + 5, 3);
			length += 11;
		}
	} else if (format == 0) {
		for (index = 0; index <= tracks; index++) {
			entry = toc + 4 + index * 8;
			if (entry[2] < packet[6])
				continue;
			memcpy(data + length, entry, 8);
			frames = ((uint32)entry[5] * 60 + entry[6]) * 75 + entry[7];
			address = frames - 150;
			OsxGuestPutAddress(data + length + 4, address, msf);
			length += 8;
		}
	} else {
		return 0;
	}
	OsxGuestPutBig16(data, length - 2);
	return length;
}

static uint32 OsxGuestBuildModePages(OsxAtaDrive *drive, uint8 *data, uint32 page)
{
	uint32 length;
	length = 0;
	if (page == 0x01 || page == 0x3f) {
		memset(data + length, 0, 8);
		data[length] = 0x01;
		data[length + 1] = 0x06;
		data[length + 3] = 0x05;
		length += 8;
	}
	if (page == 0x0e || page == 0x3f) {
		memset(data + length, 0, 16);
		data[length] = 0x0e;
		data[length + 1] = 0x0e;
		data[length + 2] = 0x04;
		data[length + 8] = 0x01;
		data[length + 9] = 0xff;
		data[length + 10] = 0x02;
		data[length + 11] = 0xff;
		if (drive->file) {
			uint8 left = 0;
			uint8 right = 0;
			SysCDGetVolume(drive->file, left, right);
			data[length + 9] = left;
			data[length + 11] = right;
		}
		length += 16;
	}
	if (page == 0x2a || page == 0x3f) {
		memset(data + length, 0, 28);
		data[length] = 0x2a;
		data[length + 1] = 0x1a;
		data[length + 2] = 0x0b;
		data[length + 4] = 0x71;
		data[length + 5] = 0x60;
		data[length + 6] = 0x29;
		OsxGuestPutBig16(data + length + 8, 706);
		OsxGuestPutBig16(data + length + 10, 2);
		OsxGuestPutBig16(data + length + 12, 512);
		OsxGuestPutBig16(data + length + 14, 706);
		length += 28;
	}
	return length;
}

static uint32 OsxGuestBuildSubChannel(OsxAtaDrive *drive, uint8 *data, const uint8 *packet)
{
	uint8 position[16];
#if defined(USE_SFL)
	char identifier[16];
#endif
	uint32 frames;
	memset(data, 0, 32);
	data[1] = 0x15;
	if (!(packet[2] & 0x40))
		return 4;
	switch (packet[3]) {
	case 0x01:
		if (!SysCDGetPosition(drive->file, position)) {
			data[3] = 12;
			data[4] = 1;
			return 16;
		}
		memcpy(data, position, 16);
		data[3] = 12;
		data[4] = 1;
		if (!(packet[1] & 2)) {
			frames = ((uint32)position[9] * 60 + position[10]) * 75 + position[11] - 150;
			OsxGuestPutBig32(data + 8, frames);
			frames = ((uint32)position[13] * 60 + position[14]) * 75 + position[15];
			OsxGuestPutBig32(data + 12, frames);
		}
		return 16;
#if defined(USE_SFL)
	case 0x02:
		data[3] = 20;
		data[4] = 2;
		memset(identifier, 0, sizeof(identifier));
		if (SysCDReadMCN(drive->file, identifier)) {
			data[8] = 0x80;
			memcpy(data + 9, identifier, 13);
		}
		return 24;
	case 0x03:
		data[3] = 20;
		data[4] = 3;
		data[6] = packet[6];
		memset(identifier, 0, sizeof(identifier));
		if (SysCDReadISRC(drive->file, packet[6], identifier)) {
			data[8] = 0x80;
			memcpy(data + 9, identifier, 12);
		}
		return 24;
#endif
	}
	return 4;
}

static void OsxGuestAtapiModeSelect(OsxAtaDrive *drive, const uint8 *packet, const uint8 *buffer)
{
	const uint8 *page;
	if (!drive->file)
		return;
	if (packet[0] == 0x55)
		page = buffer + 8 + (((uint32)buffer[6] << 8) | buffer[7]);
	else if (packet[0] == 0x15)
		page = buffer + 4 + buffer[3];
	else
		return;
	if ((page[0] & 0x3f) == 0x0e)
		SysCDSetVolume(drive->file, page[9], page[11]);
}

static uint32 OsxGuestBuildConfiguration(OsxAtaDrive *drive, uint8 *data)
{
	uint32 length;
	memset(data, 0, 96);
	length = 8;
	data[6] = 0x00;
	data[7] = 0x08;
	if (drive->dvd)
		data[7] = 0x10;
	data[length + 1] = 0x00;
	data[length + 2] = 0x03;
	data[length + 3] = 8;
	data[length + 4] = 0x00;
	data[length + 5] = 0x10;
	data[length + 6] = (uint8)drive->dvd;
	data[length + 8] = 0x00;
	data[length + 9] = 0x08;
	data[length + 10] = (uint8)!drive->dvd;
	length += 12;
	data[length + 1] = 0x01;
	data[length + 2] = 0x0b;
	data[length + 3] = 8;
	data[length + 7] = 0x02;
	data[length + 8] = 0x01;
	length += 12;
	data[length + 1] = 0x02;
	data[length + 2] = 0x07;
	data[length + 3] = 4;
	data[length + 4] = 0x02;
	length += 8;
	data[length + 1] = 0x03;
	data[length + 2] = 0x0b;
	data[length + 3] = 4;
	data[length + 4] = 0x29;
	length += 8;
	data[length + 1] = 0x10;
	data[length + 2] = 0x01;
	data[length + 3] = 8;
	OsxGuestPutBig32(data + length + 4, OSXATAPISECTOR);
	OsxGuestPutBig16(data + length + 8, 1);
	length += 12;
	data[length + 1] = 0x1e;
	data[length + 2] = 0x08 | (uint8)!drive->dvd;
	data[length + 3] = 4;
	length += 8;
	data[length + 1] = 0x1f;
	data[length + 2] = (uint8)drive->dvd;
	length += 4;
	OsxGuestPutBig32(data, length - 4);
	return length;
}

static uint32 OsxGuestBuildDvdStructure(OsxAtaDrive *drive, uint8 *data, uint32 format)
{
	if (format == 0) {
		memset(data, 0, 4 + 2048);
		OsxGuestPutBig16(data, 2048 + 2);
		data[4] = 0x01;
		data[5] = 0x02;
		data[6] = 0x01;
		OsxGuestPutBig32(data + 8, 0x30000);
		OsxGuestPutBig32(data + 12, 0x30000 + (uint32)drive->sectors - 1);
		return 4 + 2048;
	}
	if (format == 1) {
		memset(data, 0, 8);
		OsxGuestPutBig16(data, 6);
		return 8;
	}
	return 0;
}

static void OsxGuestAtapiPlay(OsxMachine *machine, OsxAtaBus *bus, OsxAtaDrive *drive, const uint8 *packet)
{
	uint32 startframe;
	uint32 framecount;
	uint32 endframe;
	int result;
	OsxGuestAudioOpenForCdrom(machine);
	if (packet[0] == 0x47)
		result = SysCDPlay(drive->file, packet[3], packet[4], packet[5], packet[6], packet[7], packet[8]);
	else {
		startframe = ((uint32)packet[2] << 24) | ((uint32)packet[3] << 16) | ((uint32)packet[4] << 8) | packet[5];
		if (packet[0] == 0x45)
			framecount = ((uint32)packet[7] << 8) | packet[8];
		else
			framecount = ((uint32)packet[6] << 24) | ((uint32)packet[7] << 16) | ((uint32)packet[8] << 8) | packet[9];
		startframe += 150;
		endframe = startframe + framecount;
		result = SysCDPlay(drive->file, (uint8)(startframe / 4500), (uint8)((startframe / 75) % 60), (uint8)(startframe % 75),
			(uint8)(endframe / 4500), (uint8)((endframe / 75) % 60), (uint8)(endframe % 75));
	}
	if (result)
		OsxGuestAtapiComplete(machine, bus, drive, 0);
	else
		OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x64);
}

static void OsxGuestAtapiExecute(OsxMachine *machine, OsxAtaBus *bus)
{
	bool audioresult;
	OsxAtaDrive *drive;
	uint8 *packet;
	uint8 *data;
	uint32 length;
	uint32 allocation;
	uint32 lba;
	uint32 count;
	uint32 pages;
	int dma;
	drive = OsxGuestSelectedDrive(bus);
	packet = bus->packet;
	data = bus->buffer;
	dma = bus->feature & 1;
	bus->atapitransfer = 1;
	bus->remaining = 0;
	length = (uint32)bus->cylinderlow | ((uint32)bus->cylinderhigh << 8);
	if (length == 0 || length > 0xfffe)
		length = 0xfffe;
	length &= ~1u;
	if (machine->log && packet[0] != 0x28 && packet[0] != 0x00 && packet[0] != 0x4a)
		fprintf(machine->log, "atapi command %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7], packet[8], packet[9]);
	OsxGuestPollMedia(machine, drive);
	if (!drive->mediapresent && packet[0] != 0x12 && packet[0] != 0x03 && packet[0] != 0x4a && packet[0] != 0x46 && packet[0] != 0x1b && packet[0] != 0x1e && packet[0] != 0x5a && packet[0] != 0x1a && packet[0] != 0xbd) {
		OsxGuestAtapiFail(machine, bus, drive, SENSENOTREADY, 0x3a);
		return;
	}
	if (drive->unitattention && packet[0] != 0x12 && packet[0] != 0x03 && packet[0] != 0x4a) {
		drive->unitattention = 0;
		OsxGuestAtapiFail(machine, bus, drive, SENSEUNITATTENTION, 0x28);
		return;
	}
	switch (packet[0]) {
	case 0x1b:
		if ((packet[4] & 3) == 2) {
#if defined(USE_SFL)
			if (drive->file)
				SysEject(drive->file);
#endif
			OsxGuestEjectMedia(machine, drive);
		}
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
#if defined(USE_SFL)
	case 0x1e:
		if (packet[4] & 1)
			SysPreventRemoval(drive->file);
		else
			SysAllowRemoval(drive->file);
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
	case 0xbb:
		SysCDSetSpeed(drive->file, ((uint32)packet[2] << 8 | packet[3]) & 0xffff);
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
#else
	case 0x1e:
	case 0xbb:
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
#endif
	case 0x45:
	case 0xa5:
	case 0x47:
		OsxGuestAtapiPlay(machine, bus, drive, packet);
		return;
	case 0x4b:
		if (packet[8] & 1)
			audioresult = SysCDResume(drive->file);
		else
			audioresult = SysCDPause(drive->file);
		if (audioresult)
			OsxGuestAtapiComplete(machine, bus, drive, 0);
		else
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x64);
		return;
	case 0x4e:
		SysCDStop(drive->file, 0, 0, 0);
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
	case 0xba:
		lba = ((uint32)packet[2] << 24) | ((uint32)packet[3] << 16) | ((uint32)packet[4] << 8) | packet[5];
		lba += 150;
		if (SysCDScan(drive->file, (uint8)(lba / 4500), (uint8)((lba / 75) % 60), (uint8)(lba % 75), (packet[1] & 0x10) != 0))
			OsxGuestAtapiComplete(machine, bus, drive, 0);
		else
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x64);
		return;
	case 0x00:
	case 0x2b:
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
	case 0x03:
		memset(data, 0, 18);
		data[0] = 0x70;
		data[2] = drive->sensekey;
		data[7] = 10;
		data[12] = drive->asc;
		data[13] = drive->ascq;
		drive->sensekey = SENSENONE;
		drive->asc = 0;
		drive->ascq = 0;
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(packet[4], 18), length, dma);
		return;
	case 0x12:
		memset(data, 0, 36);
		data[0] = 0x05;
		data[1] = 0x80;
		data[2] = 0x00;
		data[3] = 0x21;
		data[4] = 31;
		memcpy(data + 8, "SHEEP   ", 8);
		memcpy(data + 16, "SheepShaver CD  ", 16);
		memcpy(data + 32, "1.0 ", 4);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(packet[4], 36), length, dma);
		return;
	case 0x1a:
		pages = OsxGuestBuildModePages(drive, data + 4, packet[2] & 0x3f);
		if (pages == 0) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		data[0] = (uint8)(pages + 3);
		data[1] = 0x01;
		data[2] = 0;
		data[3] = 0;
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(packet[4], pages + 4), length, dma);
		return;
	case 0x5a:
		pages = OsxGuestBuildModePages(drive, data + 8, packet[2] & 0x3f);
		if (pages == 0) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		memset(data, 0, 8);
		OsxGuestPutBig16(data, pages + 6);
		data[2] = 0x01;
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], pages + 8), length, dma);
		return;
	case 0x25:
		OsxGuestPutBig32(data, (uint32)drive->sectors - 1);
		OsxGuestPutBig32(data + 4, OSXATAPISECTOR);
		OsxGuestBeginBufferIn(machine, bus, 8, length, dma);
		return;
	case 0x28:
	case 0xa8:
	case 0xbe:
		lba = ((uint32)packet[2] << 24) | ((uint32)packet[3] << 16) | ((uint32)packet[4] << 8) | packet[5];
		if (packet[0] == 0x28)
			count = ((uint32)packet[7] << 8) | packet[8];
		else if (packet[0] == 0xa8)
			count = ((uint32)packet[6] << 24) | ((uint32)packet[7] << 16) | ((uint32)packet[8] << 8) | packet[9];
		else
			count = ((uint32)packet[6] << 16) | ((uint32)packet[7] << 8) | packet[8];
#if defined(USE_SFL)
		if (packet[0] == 0xbe && packet[9] == 0 && packet[10] == 1) {
			if (count > OSXATABUFFER / 96 || !SysCDReadSubcodes(drive->file, data, lba, count)) {
				OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
				return;
			}
			OsxGuestBeginBufferIn(machine, bus, count * 96, length, dma);
			return;
		}
#endif
		if (packet[0] == 0xbe && (((packet[1] >> 2) & 7) > 5 || (packet[9] & 7) || packet[10])) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		if ((uint64)lba + count > drive->sectors) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x21);
			return;
		}
		if (count == 0) {
			OsxGuestAtapiComplete(machine, bus, drive, 0);
			return;
		}
		if (packet[0] == 0xbe && !(packet[9] == 0x10 && ((packet[1] >> 2) & 7) == 2))
			OsxGuestBeginSectorsIn(machine, bus, lba, count, 2352, length, dma);
		else
			OsxGuestBeginSectorsIn(machine, bus, lba, count, OSXATAPISECTOR, length, dma);
		return;
	case 0x43:
		count = OsxGuestBuildToc(drive, data, packet);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], count), length, dma);
		return;
	case 0x42:
		count = OsxGuestBuildSubChannel(drive, data, packet);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], count), length, dma);
		return;
	case 0x46:
		count = OsxGuestBuildConfiguration(drive, data);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], count), length, dma);
		return;
	case 0x4a:
		allocation = ((uint32)packet[7] << 8) | packet[8];
		memset(data, 0, 8);
		if (!(packet[1] & 1)) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		data[3] = 0x10;
		if (packet[4] & 0x10) {
			data[1] = 6;
			data[2] = 0x04;
			data[5] = 0;
			if (drive->mediapresent)
				data[5] = 0x02;
			if (drive->mediachanged) {
				if (drive->mediapresent)
					data[4] = 0x02;
				else
					data[4] = 0x03;
				drive->mediachanged = 0;
			}
			OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(allocation, 8), length, dma);
		} else {
			data[1] = 2;
			data[2] = 0x80;
			OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(allocation, 4), length, dma);
		}
		return;
	case 0x51:
		memset(data, 0, 34);
		data[1] = 32;
		data[2] = 0x0e;
		data[3] = 1;
		data[4] = 1;
		data[5] = 1;
		data[6] = 1;
		data[16] = 0xff;
		data[17] = 0xff;
		data[18] = 0xff;
		data[19] = 0xff;
		OsxGuestPutBig32(data + 20, (uint32)drive->sectors);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], 34), length, dma);
		return;
	case 0x52:
		memset(data, 0, 36);
		data[1] = 34;
		data[2] = 1;
		data[3] = 1;
		data[5] = 0x04;
		data[6] = 0x01;
		OsxGuestPutBig32(data + 24, (uint32)drive->sectors);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[7] << 8) | packet[8], 36), length, dma);
		return;
	case 0xad:
		count = 0;
		if (drive->dvd)
			count = OsxGuestBuildDvdStructure(drive, data, packet[7]);
		if (count == 0) {
			OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x30);
			return;
		}
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[8] << 8) | packet[9], count), length, dma);
		return;
	case 0xbd:
		memset(data, 0, 8);
		OsxGuestBeginBufferIn(machine, bus, OsxGuestAllocation(((uint32)packet[8] << 8) | packet[9], 8), length, dma);
		return;
	case 0x55:
	case 0x15:
		if (packet[0] == 0x55)
			count = ((uint32)packet[7] << 8) | packet[8];
		else
			count = packet[4];
		if (count == 0) {
			OsxGuestAtapiComplete(machine, bus, drive, 0);
			return;
		}
		bus->transferdirection = ATADIRECTIONOUT;
		bus->remaining = 0;
		bus->bufferposition = 0;
		bus->bufferlength = count;
		bus->drqremaining = count;
		bus->cylinderlow = (uint8)count;
		bus->cylinderhigh = (uint8)(count >> 8);
		bus->count = 0;
		bus->status = ATAREADY | ATASEEK | ATADRQ;
		OsxGuestRaiseIrq(machine, bus);
		return;
	}
	OsxGuestAtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x20);
}

static void OsxGuestExecuteCommand(OsxMachine *machine, OsxAtaBus *bus, uint8 command)
{
	OsxAtaDrive *drive;
	uint64 lba;
	uint32 count;
	drive = OsxGuestSelectedDrive(bus);
	bus->command = command;
	if (!drive->present)
		return;
	OsxGuestLowerIrq(machine, bus);
	bus->error = 0;
	bus->atapitransfer = 0;
	if (drive->atapi) {
		switch (command) {
		case 0xa0:
			bus->packetphase = 1;
			bus->transferdirection = ATADIRECTIONOUT;
			bus->bufferposition = 0;
			bus->bufferlength = 12;
			bus->drqremaining = 12;
			bus->count = 1;
			bus->status = ATAREADY | ATASEEK | ATADRQ;
			return;
		case 0xa1:
			OsxGuestBuildIdentify(drive, bus->buffer);
			bus->remaining = 0;
			bus->bufferlength = 512;
			bus->bufferposition = 0;
			bus->transferdirection = ATADIRECTIONIN;
			bus->drqblock = 512;
			OsxGuestStartDrqBlock(machine, bus);
			return;
		case 0x08:
			OsxGuestSetSignature(bus, drive);
			bus->status = ATAREADY | ATASEEK;
			OsxGuestRaiseIrq(machine, bus);
			return;
		case 0x90:
			OsxGuestSetSignature(bus, drive);
			bus->error = 1;
			bus->status = ATAREADY | ATASEEK;
			OsxGuestRaiseIrq(machine, bus);
			return;
		case 0xef:
		case 0xe0:
		case 0xe1:
		case 0xe5:
		case 0xe7:
			if (command == 0xe5)
				bus->count = 0xff;
			OsxGuestCompleteCommand(machine, bus, 0);
			return;
		}
		OsxGuestSetSignature(bus, drive);
		OsxGuestCompleteCommand(machine, bus, 1);
		return;
	}
	switch (command) {
	case 0xec:
		OsxGuestBuildIdentify(drive, bus->buffer);
		bus->remaining = 0;
		bus->bufferlength = 512;
		bus->bufferposition = 0;
		bus->transferdirection = ATADIRECTIONIN;
		bus->drqblock = 512;
		OsxGuestStartDrqBlock(machine, bus);
		return;
	case 0x20:
	case 0x21:
	case 0xc4:
	case 0xc8:
	case 0xc9:
		lba = OsxGuestCurrentLba(bus);
		count = OsxGuestCurrentCount(bus);
		if (lba + count > drive->sectors) {
			OsxGuestCompleteCommand(machine, bus, 1);
			return;
		}
		if (command == 0xc8 || command == 0xc9)
			OsxGuestBeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR, 1);
		else if (command == 0xc4)
			OsxGuestBeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR * drive->multiple, 0);
		else
			OsxGuestBeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR, 0);
		return;
	case 0x30:
	case 0x31:
		OsxGuestBeginWrite(machine, bus, OSXATASECTOR, 0);
		return;
	case 0xc5:
		OsxGuestBeginWrite(machine, bus, OSXATASECTOR * drive->multiple, 0);
		return;
	case 0xca:
	case 0xcb:
		OsxGuestBeginWrite(machine, bus, OSXATASECTOR, 1);
		return;
	case 0xc6:
		if (bus->count == 0 || bus->count > 16 || (bus->count & (bus->count - 1))) {
			OsxGuestCompleteCommand(machine, bus, 1);
			return;
		}
		drive->multiple = bus->count;
		OsxGuestCompleteCommand(machine, bus, 0);
		return;
	case 0x90:
		OsxGuestSetSignature(bus, drive);
		bus->error = 1;
		bus->status = ATAREADY | ATASEEK;
		OsxGuestRaiseIrq(machine, bus);
		return;
	case 0xe5:
		bus->count = 0xff;
		OsxGuestCompleteCommand(machine, bus, 0);
		return;
	case 0x40:
	case 0x41:
	case 0x10:
	case 0x70:
	case 0x91:
	case 0xef:
	case 0xe7:
	case 0xea:
	case 0xe0:
	case 0xe1:
	case 0xe2:
	case 0xe3:
	case 0xe6:
	case 0x94:
	case 0x95:
	case 0x96:
	case 0x97:
	case 0x98:
	case 0x99:
		OsxGuestCompleteCommand(machine, bus, 0);
		return;
	}
	if (machine->log)
		fprintf(machine->log, "ata unknown command %02x\n", command);
	OsxGuestCompleteCommand(machine, bus, 1);
}

void OsxGuestAtaDmaReady(OsxMachine *machine, int busindex)
{
	OsxAtaBus *bus;
	OsxAtaDrive *drive;
	OsxDbdma *dma;
	uint32 consumed;
	uint32 sectors;
	uint32 bytes;
	int failed;
	bus = &machine->ata[busindex];
	dma = &machine->dbdma[bus->dmachannel];
	if (bus->dmapending == ATADMANONE || !(dma->status & 0x8000) || !(dma->status & 0x0400))
		return;
	drive = OsxGuestSelectedDrive(bus);
	failed = 0;
	if (bus->dmapending == ATADMAREAD) {
		while (!failed) {
			if (bus->bufferposition >= bus->bufferlength) {
				if (bus->remaining == 0)
					break;
				if (!OsxGuestFillBuffer(bus, drive)) {
					failed = 1;
					break;
				}
			}
			consumed = (uint32)OsxGuestDbdmaTransfer(machine, bus->dmachannel, bus->buffer + bus->bufferposition, bus->bufferlength - bus->bufferposition, 1);
			bus->bufferposition += consumed;
			if (bus->bufferposition < bus->bufferlength)
				break;
		}
	} else {
		while (bus->remaining > 0) {
			sectors = bus->remaining;
			if (sectors > OSXATABUFFER / OSXATASECTOR)
				sectors = OSXATABUFFER / OSXATASECTOR;
			bytes = sectors * OSXATASECTOR;
			consumed = (uint32)OsxGuestDbdmaTransfer(machine, bus->dmachannel, bus->buffer, bytes, 0);
			consumed &= ~(uint32)(OSXATASECTOR - 1);
			if (consumed && Sys_write(drive->file, bus->buffer, (loff_t)(bus->lba * OSXATASECTOR), consumed) != consumed) {
				failed = 1;
				break;
			}
			bus->lba += consumed / OSXATASECTOR;
			bus->remaining -= consumed / OSXATASECTOR;
			if (consumed < bytes)
				break;
		}
	}
	if (!failed && (bus->remaining > 0 || bus->bufferposition < bus->bufferlength)) {
		if (machine->log) {
			fprintf(machine->log, "ata dma wait bus %d remaining %u position %u length %u status %04x pointer %08x\n", busindex, bus->remaining, bus->bufferposition, bus->bufferlength, dma->status, dma->commandpointer);
			OsxGuestDbdmaDump(machine, bus->dmachannel);
		}
		return;
	}
	if (machine->log)
		fprintf(machine->log, "ata dma done bus %d failed %d remaining %u position %u length %u status %04x\n", busindex, failed, bus->remaining, bus->bufferposition, bus->bufferlength, dma->status);
	OsxGuestDbdmaFinish(machine, bus->dmachannel);
	if (bus->atapitransfer) {
		if (failed)
			OsxGuestAtapiFail(machine, bus, drive, 0x03, 0x11);
		else
			OsxGuestAtapiComplete(machine, bus, drive, 0);
	} else {
		OsxGuestCompleteCommand(machine, bus, failed);
	}
}

static uint32 OsxGuestReadData(OsxMachine *machine, OsxAtaBus *bus)
{
	uint32 value;
	if (bus->transferdirection != ATADIRECTIONIN || bus->drqremaining == 0)
		return 0xffff;
	value = ((uint32)bus->buffer[bus->bufferposition] << 8) | bus->buffer[bus->bufferposition + 1];
	if (bus->drqremaining == 1) {
		bus->bufferposition += 1;
		bus->drqremaining = 0;
	} else {
		bus->bufferposition += 2;
		bus->drqremaining -= 2;
	}
	if (bus->drqremaining == 0)
		OsxGuestDataInBlockDone(machine, bus);
	return value;
}

static void OsxGuestWriteData(OsxMachine *machine, OsxAtaBus *bus, uint32 value)
{
	OsxAtaDrive *drive;
	if (bus->transferdirection != ATADIRECTIONOUT || bus->drqremaining < 2)
		return;
	bus->buffer[bus->bufferposition] = (uint8)(value >> 8);
	bus->buffer[bus->bufferposition + 1] = (uint8)value;
	bus->bufferposition += 2;
	bus->drqremaining -= 2;
	if (bus->drqremaining != 0)
		return;
	drive = OsxGuestSelectedDrive(bus);
	if (bus->packetphase) {
		bus->packetphase = 0;
		memcpy(bus->packet, bus->buffer, 12);
		bus->transferdirection = ATADIRECTIONNONE;
		OsxGuestAtapiExecute(machine, bus);
		return;
	}
	if (drive->atapi) {
		OsxGuestAtapiModeSelect(drive, bus->packet, bus->buffer);
		OsxGuestAtapiComplete(machine, bus, drive, 0);
		return;
	}
	OsxGuestWriteBlockDone(machine, bus);
}

uint32 OsxGuestAtaRead(OsxMachine *machine, int busindex, uint32 offset, int size)
{
	OsxAtaBus *bus;
	OsxAtaDrive *drive;
	uint32 value;
	bus = &machine->ata[busindex];
	drive = OsxGuestSelectedDrive(bus);
	if (offset == 0x200)
		return bswap_32(bus->timing);
	if (!drive->present) {
		if (offset == 0x00)
			return 0xffffffffu >> ((4 - size) * 8);
		return 0;
	}
	switch (offset) {
	case 0x00:
		value = OsxGuestReadData(machine, bus);
		if (size == 4)
			value = (value << 16) | OsxGuestReadData(machine, bus);
		return value;
	case 0x10: return bus->error;
	case 0x20: return bus->count;
	case 0x30: return bus->sector;
	case 0x40: return bus->cylinderlow;
	case 0x50: return bus->cylinderhigh;
	case 0x60: return bus->select;
	case 0x70:
		if (bus->irqdeadline == 0 && bus->irqline)
			OsxGuestLowerIrq(machine, bus);
		return bus->status;
	case 0x160:
		return bus->status;
	}
	return 0;
}

void OsxGuestAtaWrite(OsxMachine *machine, int busindex, uint32 offset, int size, uint32 value)
{
	OsxAtaBus *bus;
	bus = &machine->ata[busindex];
	switch (offset) {
	case 0x00:
		if (size == 4) {
			OsxGuestWriteData(machine, bus, value >> 16);
			OsxGuestWriteData(machine, bus, value & 0xffff);
		} else {
			OsxGuestWriteData(machine, bus, value);
		}
		return;
	case 0x10: bus->feature = (uint8)value; return;
	case 0x20: bus->count = (uint8)value; return;
	case 0x30: bus->sector = (uint8)value; return;
	case 0x40: bus->cylinderlow = (uint8)value; return;
	case 0x50: bus->cylinderhigh = (uint8)value; return;
	case 0x60: bus->select = (uint8)value; return;
	case 0x70:
		OsxGuestExecuteCommand(machine, bus, (uint8)value);
		return;
	case 0x160:
		if ((value & 4) && !(bus->control & 4)) {
			OsxGuestResetBus(bus);
			OsxGuestLowerIrq(machine, bus);
		}
		bus->control = (uint8)value;
		if ((value & 2) && bus->irqline)
			OsxGuestPicSetLine(machine, bus->irq, 0);
		else if (!(value & 2) && bus->irqline)
			OsxGuestPicSetLine(machine, bus->irq, 1);
		return;
	case 0x200:
		bus->timing = bswap_32(value);
		return;
	}
}
