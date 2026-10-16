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

static void InsertNextMedia(OsxMachine *machine, OsxAtaDrive *drive);

static OsxAtaDrive *SelectedDrive(OsxAtaBus *bus)
{
	return &bus->drives[(bus->select >> 4) & 1];
}

static void RaiseIrq(OsxMachine *machine, OsxAtaBus *bus)
{
	bus->pendingstatus = bus->status;
	bus->status = ATABUSY | ATAREADY;
	bus->irqdeadline = OsxCpuTimebase(machine->cpu) + OSXATADELAY;
}

void OsxAtaPoll(OsxMachine *machine, uint64 now)
{
	OsxAtaBus *bus;
	int index;
	int unit;
	for (index = 0; index < OSXATABUSES; index++) {
		bus = &machine->ata[index];
		for (unit = 0; unit < 2; unit++)
			if (bus->drives[unit].insertdeadline && now >= bus->drives[unit].insertdeadline)
				InsertNextMedia(machine, &bus->drives[unit]);
		if (bus->irqdeadline == 0 || now < bus->irqdeadline)
			continue;
		bus->irqdeadline = 0;
		bus->status = bus->pendingstatus;
		bus->irqline = 1;
		if (!(bus->control & 2))
			OsxPicSetLine(machine, bus->irq, 1);
	}
}

static void LowerIrq(OsxMachine *machine, OsxAtaBus *bus)
{
	if (bus->irqdeadline) {
		bus->irqdeadline = 0;
		bus->status = bus->pendingstatus;
	}
	bus->irqline = 0;
	OsxPicSetLine(machine, bus->irq, 0);
}

static void SetSignature(OsxAtaBus *bus, OsxAtaDrive *drive)
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

static void ResetBus(OsxAtaBus *bus)
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
	SetSignature(bus, &bus->drives[0]);
}

void OsxAtaReset(OsxMachine *machine)
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
		ResetBus(bus);
	}
	machine->ata[0].irq = OSXIRQIDE0;
	machine->ata[1].irq = OSXIRQIDE1;
	machine->ata[0].dmachannel = OSXDBDMAIDE0;
	machine->ata[1].dmachannel = OSXDBDMAIDE1;
	machine->dbdma[OSXDBDMAIDE0].irq = OSXIRQIDE0DMA;
	machine->dbdma[OSXDBDMAIDE1].irq = OSXIRQIDE1DMA;
}

static void PutString(uint8 *identify, int word, int words, const char *text)
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

static void PutWord(uint8 *identify, int word, uint32 value)
{
	identify[word * 2] = (uint8)value;
	identify[word * 2 + 1] = (uint8)(value >> 8);
}

static void BuildIdentify(OsxAtaDrive *drive, uint8 *identify)
{
	uint64 cylinders;
	uint32 capacity;
	memset(identify, 0, 512);
	if (drive->atapi) {
		PutWord(identify, 0, 0x85c0);
		PutWord(identify, 49, 0x0300);
		PutWord(identify, 53, 0x0007);
		PutWord(identify, 63, 0x0407);
		PutWord(identify, 64, 0x0003);
		PutWord(identify, 65, 0x00b4);
		PutWord(identify, 66, 0x00b4);
		PutWord(identify, 67, 0x012c);
		PutWord(identify, 68, 0x00b4);
		PutWord(identify, 71, 30);
		PutWord(identify, 72, 30);
		PutWord(identify, 80, 0x001e);
	} else {
		cylinders = drive->sectors / (16 * 63);
		if (cylinders > 16383)
			cylinders = 16383;
		capacity = (uint32)drive->sectors;
		if (drive->sectors > 0x0fffffff)
			capacity = 0x0fffffff;
		PutWord(identify, 0, 0x0040);
		PutWord(identify, 1, (uint32)cylinders);
		PutWord(identify, 3, 16);
		PutWord(identify, 4, 512 * 63);
		PutWord(identify, 5, 512);
		PutWord(identify, 6, 63);
		PutWord(identify, 20, 3);
		PutWord(identify, 21, 512);
		PutWord(identify, 22, 4);
		PutWord(identify, 47, 0x8010);
		PutWord(identify, 48, 0);
		PutWord(identify, 49, 0x0300);
		PutWord(identify, 51, 0x0200);
		PutWord(identify, 52, 0x0200);
		PutWord(identify, 53, 0x0007);
		PutWord(identify, 54, (uint32)cylinders);
		PutWord(identify, 55, 16);
		PutWord(identify, 56, 63);
		PutWord(identify, 57, (uint32)(cylinders * 16 * 63) & 0xffff);
		PutWord(identify, 58, (uint32)(cylinders * 16 * 63) >> 16);
		if (drive->multiple)
			PutWord(identify, 59, 0x0100 | drive->multiple);
		PutWord(identify, 60, capacity & 0xffff);
		PutWord(identify, 61, capacity >> 16);
		PutWord(identify, 63, 0x0407);
		PutWord(identify, 64, 0x0003);
		PutWord(identify, 65, 0x0078);
		PutWord(identify, 66, 0x0078);
		PutWord(identify, 67, 0x0078);
		PutWord(identify, 68, 0x0078);
		PutWord(identify, 80, 0x00f0);
		PutWord(identify, 81, 0x0016);
		PutWord(identify, 82, 0x4000);
		PutWord(identify, 83, 0x4000);
		PutWord(identify, 84, 0x4000);
		PutWord(identify, 85, 0x4000);
		PutWord(identify, 86, 0x4000);
		PutWord(identify, 87, 0x4000);
	}
	PutString(identify, 10, 10, "SHEEPSHAVER0001");
	PutString(identify, 23, 4, "1.0");
	PutString(identify, 27, 20, drive->model);
}

static int OpenMedia(OsxAtaDrive *drive, const char *path, int atapi)
{
	loff_t size;
	drive->file = Sys_open(path, atapi != 0, false);
	if (drive->file == NULL) {
		drive->readonly = 1;
		drive->file = Sys_open(path, true, false);
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
	drive->mediapresent = 1;
	return 1;
}

int OsxAtaAttach(OsxMachine *machine, int busindex, int unit, const char *path, int atapi)
{
	OsxAtaDrive *drive;
	drive = &machine->ata[busindex].drives[unit];
	if (!OpenMedia(drive, path, atapi))
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
		SetSignature(&machine->ata[busindex], drive);
	return 1;
}

static void EjectMedia(OsxMachine *machine, OsxAtaDrive *drive)
{
	if (!drive->mediapresent)
		return;
	Sys_close(drive->file);
	drive->file = NULL;
	drive->mediapresent = 0;
	drive->mediachanged = 1;
	drive->sectors = 0;
	if (machine->cdromcount > 0)
		drive->insertdeadline = OsxCpuTimebase(machine->cpu) + (uint64)OSXTIMEBASEFREQUENCY * 3;
	if (machine->log)
		fprintf(machine->log, "cdrom ejected\n");
}

static void InsertNextMedia(OsxMachine *machine, OsxAtaDrive *drive)
{
	drive->insertdeadline = 0;
	drive->mediaindex = (drive->mediaindex + 1) % machine->cdromcount;
	if (!OpenMedia(drive, machine->cdrompaths[drive->mediaindex], 1))
		return;
	drive->unitattention = 1;
	drive->mediachanged = 1;
	if (machine->log)
		fprintf(machine->log, "cdrom inserted %s\n", machine->cdrompaths[drive->mediaindex]);
}

static void CompleteCommand(OsxMachine *machine, OsxAtaBus *bus, int failed)
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
	RaiseIrq(machine, bus);
}

static void AtapiComplete(OsxMachine *machine, OsxAtaBus *bus, OsxAtaDrive *drive, int failed)
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
	RaiseIrq(machine, bus);
}

static void AtapiFail(OsxMachine *machine, OsxAtaBus *bus, OsxAtaDrive *drive, uint8 sensekey, uint8 asc)
{
	drive->sensekey = sensekey;
	drive->asc = asc;
	drive->ascq = 0;
	AtapiComplete(machine, bus, drive, 1);
}

static void ApplyPatches(OsxAtaBus *bus, OsxAtaDrive *drive, uint8 *data, uint64 start, uint32 length)
{
	OsxDiskPatch *patch;
	int index;
	int byte;
	for (index = 0; index < bus->patchcount; index++) {
		patch = &bus->patches[index];
		if (patch->atapionly && !drive->atapi)
			continue;
		for (byte = 0; byte < patch->length; byte++)
			if (patch->offset + byte >= start && patch->offset + byte < start + length)
				data[patch->offset + byte - start] = patch->bytes[byte];
	}
}

static int FillBuffer(OsxAtaBus *bus, OsxAtaDrive *drive)
{
	uint32 sectors;
	uint32 bytes;
	sectors = bus->remaining;
	if (sectors > OSXATABUFFER / bus->sectorsize)
		sectors = OSXATABUFFER / bus->sectorsize;
	bytes = sectors * bus->sectorsize;
	if (bus->lba + sectors > drive->sectors)
		return 0;
	if (Sys_read(drive->file, bus->buffer, (loff_t)(bus->lba * bus->sectorsize), bytes) != bytes)
		return 0;
	ApplyPatches(bus, drive, bus->buffer, bus->lba * bus->sectorsize, bytes);
	bus->lba += sectors;
	bus->remaining -= sectors;
	bus->bufferlength = bytes;
	bus->bufferposition = 0;
	return 1;
}

static void StartDrqBlock(OsxMachine *machine, OsxAtaBus *bus)
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
	RaiseIrq(machine, bus);
}

static void DataInBlockDone(OsxMachine *machine, OsxAtaBus *bus)
{
	OsxAtaDrive *drive;
	drive = SelectedDrive(bus);
	if (bus->bufferposition >= bus->bufferlength) {
		if (bus->remaining == 0) {
			if (bus->atapitransfer) {
				AtapiComplete(machine, bus, drive, 0);
			} else {
				bus->transferdirection = ATADIRECTIONNONE;
				bus->status = ATAREADY | ATASEEK;
			}
			return;
		}
		if (!FillBuffer(bus, drive)) {
			if (bus->atapitransfer)
				AtapiFail(machine, bus, drive, 0x03, 0x11);
			else
				CompleteCommand(machine, bus, 1);
			return;
		}
	}
	StartDrqBlock(machine, bus);
}

static void BeginSectorsIn(OsxMachine *machine, OsxAtaBus *bus, uint64 lba, uint32 count, uint32 sectorsize, uint32 drqblock, int dma)
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
		OsxAtaDmaReady(machine, (int)(bus - machine->ata));
		return;
	}
	DataInBlockDone(machine, bus);
}

static void BeginBufferIn(OsxMachine *machine, OsxAtaBus *bus, uint32 length, uint32 drqblock, int dma)
{
	bus->remaining = 0;
	bus->bufferlength = length;
	bus->bufferposition = 0;
	bus->transferdirection = ATADIRECTIONIN;
	bus->drqblock = drqblock;
	if (dma) {
		bus->dmapending = ATADMAREAD;
		bus->status = ATAREADY | ATABUSY;
		OsxAtaDmaReady(machine, (int)(bus - machine->ata));
		return;
	}
	if (length == 0) {
		AtapiComplete(machine, bus, SelectedDrive(bus), 0);
		return;
	}
	StartDrqBlock(machine, bus);
}

static uint64 CurrentLba(OsxAtaBus *bus)
{
	uint32 cylinder;
	uint32 head;
	if (bus->select & 0x40)
		return ((uint64)(bus->select & 0x0f) << 24) | ((uint32)bus->cylinderhigh << 16) | ((uint32)bus->cylinderlow << 8) | bus->sector;
	cylinder = bus->cylinderlow | ((uint32)bus->cylinderhigh << 8);
	head = bus->select & 0x0f;
	return ((uint64)cylinder * 16 + head) * 63 + bus->sector - 1;
}

static uint32 CurrentCount(OsxAtaBus *bus)
{
	if (bus->count == 0)
		return 256;
	return bus->count;
}

static void BeginWrite(OsxMachine *machine, OsxAtaBus *bus, uint32 drqblock, int dma)
{
	OsxAtaDrive *drive;
	drive = SelectedDrive(bus);
	if (drive->readonly) {
		CompleteCommand(machine, bus, 1);
		return;
	}
	bus->lba = CurrentLba(bus);
	bus->remaining = CurrentCount(bus);
	bus->sectorsize = OSXATASECTOR;
	if (bus->lba + bus->remaining > drive->sectors) {
		CompleteCommand(machine, bus, 1);
		return;
	}
	bus->transferdirection = ATADIRECTIONOUT;
	if (dma) {
		bus->dmapending = ATADMAWRITE;
		bus->status = ATAREADY | ATABUSY;
		OsxAtaDmaReady(machine, (int)(bus - machine->ata));
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

static void WriteBlockDone(OsxMachine *machine, OsxAtaBus *bus)
{
	OsxAtaDrive *drive;
	uint32 sectors;
	drive = SelectedDrive(bus);
	sectors = bus->bufferlength / OSXATASECTOR;
	if (Sys_write(drive->file, bus->buffer, (loff_t)(bus->lba * OSXATASECTOR), bus->bufferlength) != bus->bufferlength) {
		CompleteCommand(machine, bus, 1);
		return;
	}
	bus->lba += sectors;
	bus->remaining -= sectors;
	if (bus->remaining == 0) {
		CompleteCommand(machine, bus, 0);
		return;
	}
	bus->bufferposition = 0;
	bus->bufferlength = bus->remaining * OSXATASECTOR;
	if (bus->bufferlength > bus->drqblock)
		bus->bufferlength = bus->drqblock;
	bus->drqremaining = bus->bufferlength;
	bus->status = ATAREADY | ATASEEK | ATADRQ;
	RaiseIrq(machine, bus);
}

static uint32 Allocation(uint32 wanted, uint32 available)
{
	if (wanted < available)
		return wanted;
	return available;
}

static void PutBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static void PutBig16(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 8);
	data[1] = (uint8)value;
}

static void PutAddress(uint8 *data, uint32 lba, int msf)
{
	uint32 frames;
	if (!msf) {
		PutBig32(data, lba);
		return;
	}
	frames = lba + 150;
	data[0] = 0;
	data[1] = (uint8)(frames / (75 * 60));
	data[2] = (uint8)((frames / 75) % 60);
	data[3] = (uint8)(frames % 75);
}

static uint32 BuildToc(OsxAtaDrive *drive, uint8 *data, uint8 *packet)
{
	uint32 length;
	uint32 format;
	uint32 frames;
	int msf;
	msf = (packet[1] & 2) != 0;
	format = packet[2] & 0x0f;
	if (format == 0)
		format = packet[9] >> 6;
	if (format == 1) {
		memset(data, 0, 12);
		data[1] = 10;
		data[2] = 1;
		data[3] = 1;
		data[5] = 0x14;
		data[6] = 1;
		PutAddress(data + 8, 0, msf);
		return 12;
	}
	if (format == 2) {
		memset(data, 0, 4 + 11 * 4);
		data[2] = 1;
		data[3] = 1;
		length = 4;
		data[length] = 1;
		data[length + 1] = 0x14;
		data[length + 3] = 0xa0;
		data[length + 8] = 1;
		length += 11;
		data[length] = 1;
		data[length + 1] = 0x14;
		data[length + 3] = 0xa1;
		data[length + 8] = 1;
		length += 11;
		data[length] = 1;
		data[length + 1] = 0x14;
		data[length + 3] = 0xa2;
		frames = (uint32)drive->sectors + 150;
		data[length + 8] = (uint8)(frames / (75 * 60));
		data[length + 9] = (uint8)((frames / 75) % 60);
		data[length + 10] = (uint8)(frames % 75);
		length += 11;
		data[length] = 1;
		data[length + 1] = 0x14;
		data[length + 3] = 1;
		data[length + 9] = 2;
		length += 11;
		PutBig16(data, length - 2);
		return length;
	}
	memset(data, 0, 20);
	data[2] = 1;
	data[3] = 1;
	length = 4;
	if (packet[6] <= 1) {
		data[length + 1] = 0x14;
		data[length + 2] = 1;
		PutAddress(data + length + 4, 0, msf);
		length += 8;
	}
	data[length + 1] = 0x14;
	data[length + 2] = 0xaa;
	PutAddress(data + length + 4, (uint32)drive->sectors, msf);
	length += 8;
	PutBig16(data, length - 2);
	return length;
}

static uint32 BuildModePages(uint8 *data, uint32 page)
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
		PutBig16(data + length + 8, 706);
		PutBig16(data + length + 10, 2);
		PutBig16(data + length + 12, 512);
		PutBig16(data + length + 14, 706);
		length += 28;
	}
	return length;
}

static uint32 BuildConfiguration(OsxAtaDrive *drive, uint8 *data)
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
	PutBig32(data + length + 4, OSXATAPISECTOR);
	PutBig16(data + length + 8, 1);
	length += 12;
	data[length + 1] = 0x1e;
	data[length + 2] = 0x08 | (uint8)!drive->dvd;
	data[length + 3] = 4;
	length += 8;
	data[length + 1] = 0x1f;
	data[length + 2] = (uint8)drive->dvd;
	length += 4;
	PutBig32(data, length - 4);
	return length;
}

static uint32 BuildDvdStructure(OsxAtaDrive *drive, uint8 *data, uint32 format)
{
	if (format == 0) {
		memset(data, 0, 4 + 2048);
		PutBig16(data, 2048 + 2);
		data[4] = 0x01;
		data[5] = 0x02;
		data[6] = 0x01;
		PutBig32(data + 8, 0x30000);
		PutBig32(data + 12, 0x30000 + (uint32)drive->sectors - 1);
		return 4 + 2048;
	}
	if (format == 1) {
		memset(data, 0, 8);
		PutBig16(data, 6);
		return 8;
	}
	return 0;
}

static uint8 ToBcd(uint32 value)
{
	return (uint8)(((value / 10) << 4) | (value % 10));
}

static int BuildRawSectors(OsxAtaBus *bus, OsxAtaDrive *drive, uint32 lba, uint32 count, uint8 flags)
{
	uint32 sector;
	uint32 frames;
	uint32 size;
	uint8 *output;
	size = 0;
	if (flags & 0x80)
		size += 12;
	if (flags & 0x20)
		size += 4;
	if (flags & 0x10)
		size += OSXATAPISECTOR;
	if (flags & 0x08)
		size += 288;
	if (size == 0 || size * count > OSXATABUFFER || (uint64)lba + count > drive->sectors)
		return 0;
	output = bus->buffer;
	for (sector = 0; sector < count; sector++) {
		if (flags & 0x80) {
			memset(output, 0xff, 12);
			output[0] = 0;
			output[11] = 0;
			output += 12;
		}
		if (flags & 0x20) {
			frames = lba + sector + 150;
			output[0] = ToBcd(frames / (75 * 60));
			output[1] = ToBcd((frames / 75) % 60);
			output[2] = ToBcd(frames % 75);
			output[3] = 1;
			output += 4;
		}
		if (flags & 0x10) {
			if (Sys_read(drive->file, output, (loff_t)(lba + sector) * OSXATAPISECTOR, OSXATAPISECTOR) != OSXATAPISECTOR)
				return 0;
			output += OSXATAPISECTOR;
		}
		if (flags & 0x08) {
			memset(output, 0, 288);
			output += 288;
		}
	}
	bus->remaining = 0;
	bus->bufferlength = size * count;
	return 1;
}

static void AtapiExecute(OsxMachine *machine, OsxAtaBus *bus)
{
	OsxAtaDrive *drive;
	uint8 *packet;
	uint8 *data;
	uint32 length;
	uint32 allocation;
	uint32 lba;
	uint32 count;
	uint32 pages;
	int dma;
	drive = SelectedDrive(bus);
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
	if (!drive->mediapresent && packet[0] != 0x12 && packet[0] != 0x03 && packet[0] != 0x4a && packet[0] != 0x46 && packet[0] != 0x1b && packet[0] != 0x1e && packet[0] != 0x5a && packet[0] != 0x1a && packet[0] != 0xbd) {
		AtapiFail(machine, bus, drive, SENSENOTREADY, 0x3a);
		return;
	}
	if (drive->unitattention && packet[0] != 0x12 && packet[0] != 0x03 && packet[0] != 0x4a) {
		drive->unitattention = 0;
		AtapiFail(machine, bus, drive, SENSEUNITATTENTION, 0x28);
		return;
	}
	switch (packet[0]) {
	case 0x1b:
		if ((packet[4] & 3) == 2)
			EjectMedia(machine, drive);
		AtapiComplete(machine, bus, drive, 0);
		return;
	case 0x00:
	case 0x1e:
	case 0x2b:
	case 0xbb:
	case 0x4e:
	case 0x45:
	case 0x47:
		AtapiComplete(machine, bus, drive, 0);
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
		BeginBufferIn(machine, bus, Allocation(packet[4], 18), length, dma);
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
		BeginBufferIn(machine, bus, Allocation(packet[4], 36), length, dma);
		return;
	case 0x1a:
		pages = BuildModePages(data + 4, packet[2] & 0x3f);
		if (pages == 0) {
			AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		data[0] = (uint8)(pages + 3);
		data[1] = 0x01;
		data[2] = 0;
		data[3] = 0;
		BeginBufferIn(machine, bus, Allocation(packet[4], pages + 4), length, dma);
		return;
	case 0x5a:
		pages = BuildModePages(data + 8, packet[2] & 0x3f);
		if (pages == 0) {
			AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
			return;
		}
		memset(data, 0, 8);
		PutBig16(data, pages + 6);
		data[2] = 0x01;
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], pages + 8), length, dma);
		return;
	case 0x25:
		PutBig32(data, (uint32)drive->sectors - 1);
		PutBig32(data + 4, OSXATAPISECTOR);
		BeginBufferIn(machine, bus, 8, length, dma);
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
		if (packet[0] == 0xbe && (packet[9] & 0xf8) != 0x10) {
			if (!BuildRawSectors(bus, drive, lba, count, packet[9])) {
				AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
				return;
			}
			BeginBufferIn(machine, bus, bus->bufferlength, length, dma);
			return;
		}
		if ((uint64)lba + count > drive->sectors) {
			AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x21);
			return;
		}
		if (count == 0) {
			AtapiComplete(machine, bus, drive, 0);
			return;
		}
		BeginSectorsIn(machine, bus, lba, count, OSXATAPISECTOR, length, dma);
		return;
	case 0x43:
		count = BuildToc(drive, data, packet);
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], count), length, dma);
		return;
	case 0x42:
		memset(data, 0, 16);
		data[1] = 0x15;
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], 4), length, dma);
		return;
	case 0x46:
		count = BuildConfiguration(drive, data);
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], count), length, dma);
		return;
	case 0x4a:
		allocation = ((uint32)packet[7] << 8) | packet[8];
		memset(data, 0, 8);
		if (!(packet[1] & 1)) {
			AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x24);
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
			BeginBufferIn(machine, bus, Allocation(allocation, 8), length, dma);
		} else {
			data[1] = 2;
			data[2] = 0x80;
			BeginBufferIn(machine, bus, Allocation(allocation, 4), length, dma);
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
		PutBig32(data + 20, (uint32)drive->sectors);
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], 34), length, dma);
		return;
	case 0x52:
		memset(data, 0, 36);
		data[1] = 34;
		data[2] = 1;
		data[3] = 1;
		data[5] = 0x04;
		data[6] = 0x01;
		PutBig32(data + 24, (uint32)drive->sectors);
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[7] << 8) | packet[8], 36), length, dma);
		return;
	case 0xad:
		count = 0;
		if (drive->dvd)
			count = BuildDvdStructure(drive, data, packet[7]);
		if (count == 0) {
			AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x30);
			return;
		}
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[8] << 8) | packet[9], count), length, dma);
		return;
	case 0xbd:
		memset(data, 0, 8);
		BeginBufferIn(machine, bus, Allocation(((uint32)packet[8] << 8) | packet[9], 8), length, dma);
		return;
	case 0x55:
	case 0x15:
		if (packet[0] == 0x55)
			count = ((uint32)packet[7] << 8) | packet[8];
		else
			count = packet[4];
		if (count == 0) {
			AtapiComplete(machine, bus, drive, 0);
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
		RaiseIrq(machine, bus);
		return;
	}
	AtapiFail(machine, bus, drive, SENSEILLEGALREQUEST, 0x20);
}

static void ExecuteCommand(OsxMachine *machine, OsxAtaBus *bus, uint8 command)
{
	OsxAtaDrive *drive;
	uint64 lba;
	uint32 count;
	drive = SelectedDrive(bus);
	bus->command = command;
	if (!drive->present)
		return;
	LowerIrq(machine, bus);
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
			BuildIdentify(drive, bus->buffer);
			bus->remaining = 0;
			bus->bufferlength = 512;
			bus->bufferposition = 0;
			bus->transferdirection = ATADIRECTIONIN;
			bus->drqblock = 512;
			StartDrqBlock(machine, bus);
			return;
		case 0x08:
			SetSignature(bus, drive);
			bus->status = ATAREADY | ATASEEK;
			RaiseIrq(machine, bus);
			return;
		case 0x90:
			SetSignature(bus, drive);
			bus->error = 1;
			bus->status = ATAREADY | ATASEEK;
			RaiseIrq(machine, bus);
			return;
		case 0xef:
		case 0xe0:
		case 0xe1:
		case 0xe5:
		case 0xe7:
			if (command == 0xe5)
				bus->count = 0xff;
			CompleteCommand(machine, bus, 0);
			return;
		}
		SetSignature(bus, drive);
		CompleteCommand(machine, bus, 1);
		return;
	}
	switch (command) {
	case 0xec:
		BuildIdentify(drive, bus->buffer);
		bus->remaining = 0;
		bus->bufferlength = 512;
		bus->bufferposition = 0;
		bus->transferdirection = ATADIRECTIONIN;
		bus->drqblock = 512;
		StartDrqBlock(machine, bus);
		return;
	case 0x20:
	case 0x21:
	case 0xc4:
	case 0xc8:
	case 0xc9:
		lba = CurrentLba(bus);
		count = CurrentCount(bus);
		if (lba + count > drive->sectors) {
			CompleteCommand(machine, bus, 1);
			return;
		}
		if (command == 0xc8 || command == 0xc9)
			BeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR, 1);
		else if (command == 0xc4)
			BeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR * drive->multiple, 0);
		else
			BeginSectorsIn(machine, bus, lba, count, OSXATASECTOR, OSXATASECTOR, 0);
		return;
	case 0x30:
	case 0x31:
		BeginWrite(machine, bus, OSXATASECTOR, 0);
		return;
	case 0xc5:
		BeginWrite(machine, bus, OSXATASECTOR * drive->multiple, 0);
		return;
	case 0xca:
	case 0xcb:
		BeginWrite(machine, bus, OSXATASECTOR, 1);
		return;
	case 0xc6:
		if (bus->count == 0 || bus->count > 16 || (bus->count & (bus->count - 1))) {
			CompleteCommand(machine, bus, 1);
			return;
		}
		drive->multiple = bus->count;
		CompleteCommand(machine, bus, 0);
		return;
	case 0x90:
		SetSignature(bus, drive);
		bus->error = 1;
		bus->status = ATAREADY | ATASEEK;
		RaiseIrq(machine, bus);
		return;
	case 0xe5:
		bus->count = 0xff;
		CompleteCommand(machine, bus, 0);
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
		CompleteCommand(machine, bus, 0);
		return;
	}
	if (machine->log)
		fprintf(machine->log, "ata unknown command %02x\n", command);
	CompleteCommand(machine, bus, 1);
}

void OsxAtaDmaReady(OsxMachine *machine, int busindex)
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
	drive = SelectedDrive(bus);
	failed = 0;
	if (bus->dmapending == ATADMAREAD) {
		while (!failed) {
			if (bus->bufferposition >= bus->bufferlength) {
				if (bus->remaining == 0)
					break;
				if (!FillBuffer(bus, drive)) {
					failed = 1;
					break;
				}
			}
			consumed = (uint32)OsxDbdmaTransfer(machine, bus->dmachannel, bus->buffer + bus->bufferposition, bus->bufferlength - bus->bufferposition, 1);
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
			consumed = (uint32)OsxDbdmaTransfer(machine, bus->dmachannel, bus->buffer, bytes, 0);
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
			OsxDbdmaDump(machine, bus->dmachannel);
		}
		return;
	}
	if (machine->log)
		fprintf(machine->log, "ata dma done bus %d failed %d remaining %u position %u length %u status %04x\n", busindex, failed, bus->remaining, bus->bufferposition, bus->bufferlength, dma->status);
	OsxDbdmaFinish(machine, bus->dmachannel);
	if (bus->atapitransfer) {
		if (failed)
			AtapiFail(machine, bus, drive, 0x03, 0x11);
		else
			AtapiComplete(machine, bus, drive, 0);
	} else {
		CompleteCommand(machine, bus, failed);
	}
}

static uint32 ReadData(OsxMachine *machine, OsxAtaBus *bus)
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
		DataInBlockDone(machine, bus);
	return value;
}

static void WriteData(OsxMachine *machine, OsxAtaBus *bus, uint32 value)
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
	drive = SelectedDrive(bus);
	if (bus->packetphase) {
		bus->packetphase = 0;
		memcpy(bus->packet, bus->buffer, 12);
		bus->transferdirection = ATADIRECTIONNONE;
		AtapiExecute(machine, bus);
		return;
	}
	if (drive->atapi) {
		AtapiComplete(machine, bus, drive, 0);
		return;
	}
	WriteBlockDone(machine, bus);
}

uint32 OsxAtaRead(OsxMachine *machine, int busindex, uint32 offset, int size)
{
	OsxAtaBus *bus;
	OsxAtaDrive *drive;
	uint32 value;
	bus = &machine->ata[busindex];
	drive = SelectedDrive(bus);
	if (offset == 0x200)
		return bswap_32(bus->timing);
	if (!drive->present) {
		if (offset == 0x00)
			return 0xffffffffu >> ((4 - size) * 8);
		return 0;
	}
	switch (offset) {
	case 0x00:
		value = ReadData(machine, bus);
		if (size == 4)
			value = (value << 16) | ReadData(machine, bus);
		return value;
	case 0x10: return bus->error;
	case 0x20: return bus->count;
	case 0x30: return bus->sector;
	case 0x40: return bus->cylinderlow;
	case 0x50: return bus->cylinderhigh;
	case 0x60: return bus->select;
	case 0x70:
		if (bus->irqdeadline == 0 && bus->irqline)
			LowerIrq(machine, bus);
		return bus->status;
	case 0x160:
		return bus->status;
	}
	return 0;
}

void OsxAtaWrite(OsxMachine *machine, int busindex, uint32 offset, int size, uint32 value)
{
	OsxAtaBus *bus;
	bus = &machine->ata[busindex];
	switch (offset) {
	case 0x00:
		if (size == 4) {
			WriteData(machine, bus, value >> 16);
			WriteData(machine, bus, value & 0xffff);
		} else {
			WriteData(machine, bus, value);
		}
		return;
	case 0x10: bus->feature = (uint8)value; return;
	case 0x20: bus->count = (uint8)value; return;
	case 0x30: bus->sector = (uint8)value; return;
	case 0x40: bus->cylinderlow = (uint8)value; return;
	case 0x50: bus->cylinderhigh = (uint8)value; return;
	case 0x60: bus->select = (uint8)value; return;
	case 0x70:
		ExecuteCommand(machine, bus, (uint8)value);
		return;
	case 0x160:
		if ((value & 4) && !(bus->control & 4)) {
			ResetBus(bus);
			LowerIrq(machine, bus);
		}
		bus->control = (uint8)value;
		if ((value & 2) && bus->irqline)
			OsxPicSetLine(machine, bus->irq, 0);
		else if (!(value & 2) && bus->irqline)
			OsxPicSetLine(machine, bus->irq, 1);
		return;
	case 0x200:
		bus->timing = bswap_32(value);
		return;
	}
}
