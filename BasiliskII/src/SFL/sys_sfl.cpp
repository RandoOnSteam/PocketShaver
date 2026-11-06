/*
 *  sys_sfl.cpp - Disk image access for the SFL platform layer
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
#include <ctype.h>
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "sys.h"
#include "qcow.h"
#include "chd.h"
#include "bincue.h"
#include "gui.h"
#include "file.h"
extern "C" {
#include "drive.h"
}

#define SFLSYSLAYOUTBYTES 256

typedef struct SFLDISK
{
	WSFILE file;
	QcowImage *qcow;
	ChdImage *chd;
	void *bincue;
	bool readonly;
	bool cdrom;
	bool hasdrive;
	CDROMDRIVE drive;
	loff_t startbyte;
	loff_t size;
} SFLDISK;

void SysInit(void)
{
	InitBinCue();
}

void SysExit(void)
{
	ExitBinCue();
}

void SysAddFloppyPrefs(void)
{
}

void SysAddDiskPrefs(void)
{
}

void SysAddCDROMPrefs(void)
{
}

void SysAddSerialPrefs(void)
{
}

static const char *SflDiskPath(const char *name)
{
	if (name[0] == '*')
		return name + 1;
	return name;
}

static bool SflDiskOpenImage(SFLDISK *disk, const char *path, bool readonly)
{
	QcowImage *qcow;
	ChdImage *chd;
	int qcowstatus;
	int chdstatus;
	qcow = NULL;
	chd = NULL;
	qcowstatus = QcowOpen(path, readonly, &qcow);
	chdstatus = CHDNOTCHD;
	if (qcowstatus == QCOWNOTQCOW)
		chdstatus = ChdOpen(path, readonly, &chd);
	if (qcowstatus == QCOWFAILED || chdstatus == CHDFAILED)
		return false;
	if (qcow != NULL) {
		disk->qcow = qcow;
		disk->readonly = QcowReadOnly(qcow) != 0;
		disk->size = QcowSize(qcow);
		return true;
	}
	if (chd != NULL) {
		disk->chd = chd;
		disk->readonly = ChdReadOnly(chd) != 0;
		disk->size = ChdSize(chd);
		return true;
	}
	return false;
}

static bool SflDiskSeek(SFLDISK *disk, loff_t offset, long mode)
{
	uint32_t low;
	uint32_t high;
	return WSFile_Seek64(&disk->file, (uint32_t)((uint64)offset & 0xffffffff), (int32_t)((uint64)offset >> 32),
		&low, &high, mode) != FALSE;
}

static loff_t SflDiskFileSize(SFLDISK *disk)
{
	uint32_t low;
	uint32_t high;
	if (!WSFile_Seek64(&disk->file, 0, 0, &low, &high, FILE_FROMEND))
		return 0;
	return (loff_t)(((uint64)high << 32) | low);
}

static bool SflDiskOpenFile(SFLDISK *disk, const char *path, bool readonly)
{
	uint8 layout[SFLSYSLAYOUTBYTES];
	loff_t size;
	if ((readonly && !disk->cdrom) || !WSFile_Create(&disk->file, path, FILE_READWRITE | FILE_BINARY)) {
		readonly = true;
		if (!WSFile_Create(&disk->file, path, FILE_READ | FILE_BINARY))
			return false;
	}
	disk->readonly = readonly;
	size = SflDiskFileSize(disk);
	memset(layout, 0, sizeof(layout));
	SflDiskSeek(disk, 0, FILE_FROMSTART);
	WSFile_Read(&disk->file, layout, sizeof(layout));
	FileDiskLayout(size, layout, disk->startbyte, disk->size);
	return true;
}

static const char *SflDiskDrivePath(const char *path)
{
#if defined(_WIN32)
	if (strncmp(path, "\\\\.\\", 4) == 0)
		path += 4;
	if (isalpha((unsigned char)path[0]) && path[1] == ':'
			&& (path[2] == 0 || ((path[2] == '\\' || path[2] == '/') && path[3] == 0)))
		return path;
	return NULL;
#elif defined(macintosh)
	size_t length = strlen(path);
	if (length != 0 && path[length - 1] == ':')
		return path;
	if (strncmp(path, "/dev/", 5) == 0 || strncmp(path, "disk", 4) == 0)
		return path;
	return NULL;
#else
	if (strncmp(path, "/dev/", 5) == 0)
		return path;
	return NULL;
#endif
}

static loff_t SflDriveSize(SFLDISK *disk)
{
	CDROMDRIVETOC toc;
	CDROMDRIVETOCTRACKDATA *leadout;
	int frames;
	if (!CdromDrive_FillTOC(&disk->drive, &toc))
		return 0;
	leadout = &toc.TrackData[toc.LastTrack - toc.FirstTrack + 1];
	frames = CdromDriveMSFToFrames(leadout->Address[1], leadout->Address[2], leadout->Address[3]) - 150;
	if (frames <= 0)
		return 0;
	return (loff_t)frames * 2048;
}

static size_t SflDriveRead(SFLDISK *disk, void *buffer, loff_t offset, size_t length)
{
	uint8 sector[CDROMDRIVE_SECTOR_BUFFER_SIZE];
	uint8 *output;
	uint32_t blocksize;
	size_t done;
	size_t got;
	size_t chunk;
	size_t skip;
	output = (uint8 *)buffer;
	if (CdromDrive_GetBlockSize(&disk->drive, &blocksize) && blocksize == 2048
			&& offset % 2048 == 0 && length % 2048 == 0) {
		got = 0;
		if (CdromDrive_Read(&disk->drive, buffer, (size_t)offset, length, &got))
			return got;
	}
	done = 0;
	while (done < length) {
		skip = (size_t)((offset + done) % 2048);
		chunk = 2048 - skip;
		if (chunk > length - done)
			chunk = length - done;
		if (CdromDrive_ReadSector(&disk->drive, sector, sizeof(sector),
				(uint32)((offset + done) / 2048), CDROMDRIVE_SECTOR_ANY, 0x10) < 2048)
			break;
		memcpy(output + done, sector + skip, chunk);
		done += chunk;
	}
	return done;
}

void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
	SFLDISK *disk;
	const char *path;
	WSASSERT(name != NULL);
	path = SflDiskPath(name);
	disk = new SFLDISK;
	WSFile_Construct(&disk->file);
	disk->qcow = NULL;
	disk->chd = NULL;
	disk->bincue = NULL;
	disk->readonly = read_only;
	disk->cdrom = is_cdrom;
	disk->hasdrive = false;
	disk->startbyte = 0;
	disk->size = 0;
	if (is_cdrom && SflDiskDrivePath(path) != NULL) {
		memset(&disk->drive, 0, sizeof(disk->drive));
		disk->hasdrive = CdromDrive_Create(&disk->drive, SflDiskDrivePath(path)) != 0;
		if (disk->hasdrive) {
			disk->readonly = true;
			disk->size = SflDriveSize(disk);
			return disk;
		}
	}
	if (is_cdrom || IsCueSheetPath(path))
		disk->bincue = OpenCDImage(path);
	if (disk->bincue != NULL) {
		disk->readonly = true;
		disk->cdrom = true;
		return disk;
	}
	if (SflDiskOpenImage(disk, path, read_only))
		return disk;
	if (SflDiskOpenFile(disk, path, read_only))
		return disk;
	delete disk;
	return NULL;
}

void Sys_close(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return;
	if (disk->qcow != NULL)
		QcowClose(disk->qcow);
	if (disk->chd != NULL)
		ChdClose(disk->chd);
	if (disk->bincue != NULL)
		close_bincue(disk->bincue);
	if (disk->hasdrive)
		CdromDrive_Destroy(&disk->drive);
	if (WSFile_IsCreated(&disk->file))
		WSFile_Destroy(&disk->file);
	delete disk;
}

size_t Sys_read(void *fh, void *buffer, loff_t offset, size_t length)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return 0;
	if (disk->qcow != NULL)
		return QcowRead(disk->qcow, buffer, offset, length);
	if (disk->chd != NULL)
		return ChdRead(disk->chd, buffer, offset, length);
	if (disk->bincue != NULL)
		return read_bincue(disk->bincue, buffer, offset, length);
	if (disk->hasdrive)
		return SflDriveRead(disk, buffer, offset, length);
	if (!SflDiskSeek(disk, offset + disk->startbyte, FILE_FROMSTART))
		return 0;
	return WSFile_Read(&disk->file, buffer, length);
}

size_t Sys_write(void *fh, void *buffer, loff_t offset, size_t length)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL || disk->readonly)
		return 0;
	if (disk->qcow != NULL)
		return QcowWrite(disk->qcow, buffer, offset, length);
	if (disk->chd != NULL)
		return ChdWrite(disk->chd, buffer, offset, length);
	if (!SflDiskSeek(disk, offset + disk->startbyte, FILE_FROMSTART))
		return 0;
	return WSFile_Write(&disk->file, buffer, length);
}

loff_t SysGetFileSize(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return 0;
	if (disk->bincue != NULL)
		return size_bincue(disk->bincue);
	return disk->size;
}

void SysEject(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk != NULL && disk->hasdrive) {
		CdromDrive_AllowRemoval(&disk->drive, TRUE);
		CdromDrive_Eject(&disk->drive, NULL);
	}
}

bool SysFormat(void *fh)
{
	return false;
}

bool SysIsReadOnly(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return true;
	return disk->readonly;
}

const char *SysImageKind(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return "none";
	if (disk->qcow != NULL)
		return "qcow2";
	if (disk->chd != NULL)
		return "chd";
	if (disk->bincue != NULL)
		return "bincue";
	if (disk->hasdrive)
		return "drive";
	if (disk->cdrom)
		return "cdrom";
	return "raw";
}

bool SysIsFixedDisk(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL)
		return false;
	return !disk->cdrom;
}

static CDROMDRIVE *SflDiskDrive(void *fh)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk == NULL || !disk->hasdrive)
		return NULL;
	return &disk->drive;
}

bool SysIsDiskInserted(void *fh)
{
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_IsDiskInDrive(SflDiskDrive(fh)) != 0;
	return fh != NULL;
}

void SysPreventRemoval(void *fh)
{
	if (SflDiskDrive(fh) != NULL)
		CdromDrive_AllowRemoval(SflDiskDrive(fh), FALSE);
}

void SysAllowRemoval(void *fh)
{
	if (SflDiskDrive(fh) != NULL)
		CdromDrive_AllowRemoval(SflDiskDrive(fh), TRUE);
}

static void *SflDiskBinCue(void *fh)
{
	if (fh == NULL)
		return NULL;
	return ((SFLDISK *)fh)->bincue;
}

int SysCDReadSector(void *fh, void *buffer, uint32 sector, uint8 type, uint8 flags)
{
	SFLDISK *disk;
	disk = (SFLDISK *)fh;
	if (disk->bincue)
		return ReadCDSectorBinCue(disk->bincue, buffer, sector, type, flags);
	if (!disk->hasdrive)
		return -1;
	return CdromDrive_ReadSector(&disk->drive, buffer, CDROMDRIVE_SECTOR_BUFFER_SIZE, sector, type, flags);
}

bool SysCDReadTOC(void *fh, uint8 *toc)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_FillTOC(SflDiskDrive(fh), (CDROMDRIVETOC *)toc) != 0;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return readtoc_bincue(bincue, toc);
}

bool SysCDGetPosition(void *fh, uint8 *pos)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL) {
		if (!CdromDrive_ReadPosition(SflDiskDrive(fh), pos))
			return false;
		if (pos[1] == 0)
			pos[1] = 0x15;
		return true;
	}
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return GetPosition_bincue(bincue, pos);
}

bool SysCDPlay(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL) {
		int start = CdromDriveMSFToFrames(start_m, start_s, start_f);
		return CdromDrive_Play(SflDiskDrive(fh), start,
			CdromDriveMSFToFrames(end_m, end_s, end_f) - start) != 0;
	}
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDPlay_bincue(bincue, start_m, start_s, start_f, end_m, end_s, end_f);
}

bool SysCDPause(void *fh)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_Pause(SflDiskDrive(fh)) != 0;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDPause_bincue(bincue);
}

bool SysCDResume(void *fh)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_Resume(SflDiskDrive(fh)) != 0;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDResume_bincue(bincue);
}

bool SysCDStop(void *fh, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_Stop(SflDiskDrive(fh)) != 0;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDStop_bincue(bincue);
}

bool SysCDScan(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL)
		return CdromDrive_Scan(SflDiskDrive(fh), CdromDriveMSFToFrames(start_m, start_s, start_f) - 150,
			reverse, 0, 10000) != 0;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDScan_bincue(bincue, start_m, start_s, start_f, reverse);
}

void SysCDSetVolume(void *fh, uint8 left, uint8 right)
{
	void *bincue;
	if (SflDiskDrive(fh) != NULL) {
		CdromDrive_SetVolume(SflDiskDrive(fh), left, right);
		return;
	}
	bincue = SflDiskBinCue(fh);
	if (bincue != NULL)
		CDSetVol_bincue(bincue, left, right);
}

void SysCDGetVolume(void *fh, uint8 &left, uint8 &right)
{
	void *bincue;
	left = 0;
	right = 0;
	if (SflDiskDrive(fh) != NULL) {
		CdromDrive_GetVolume(SflDiskDrive(fh), &left, &right);
		return;
	}
	bincue = SflDiskBinCue(fh);
	if (bincue != NULL)
		CDGetVol_bincue(bincue, &left, &right);
}

bool SysCDReadSubcodes(void *fh, void *buffer, uint32 sector, uint32 count)
{
	if (SflDiskDrive(fh) == NULL)
		return false;
	return CdromDrive_ReadSubcodes(SflDiskDrive(fh), buffer, sector, count) != 0;
}

bool SysCDReadMCN(void *fh, char *mcn)
{
	if (SflDiskDrive(fh) == NULL)
		return false;
	return CdromDrive_ReadMCN(SflDiskDrive(fh), mcn) != 0;
}

bool SysCDReadISRC(void *fh, int track, char *isrc)
{
	if (SflDiskDrive(fh) == NULL)
		return false;
	return CdromDrive_ReadISRC(SflDiskDrive(fh), track, isrc) != 0;
}

bool SysCDGetSpeed(void *fh, unsigned int *kbps)
{
	if (SflDiskDrive(fh) == NULL)
		return false;
	return CdromDrive_GetSpeed(SflDiskDrive(fh), kbps) != 0;
}

bool SysCDSetSpeed(void *fh, uint32 kbps)
{
	if (SflDiskDrive(fh) == NULL)
		return false;
	return CdromDrive_SetSpeed(SflDiskDrive(fh), kbps) != 0;
}

bool SysCDGetFeatures(void *fh, unsigned int *maxkbps, unsigned int *flags)
{
	CDROMDRIVEFEATURES features;
	if (SflDiskDrive(fh) == NULL || !CdromDrive_GetFeatures(SflDiskDrive(fh), &features))
		return false;
	*maxkbps = features.maxspeed;
	*flags = features.flags;
	return true;
}
