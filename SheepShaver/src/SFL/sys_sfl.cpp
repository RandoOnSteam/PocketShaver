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
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "sys.h"
#include "qcow.h"
#include "chd.h"
#include "bincue.h"
#include "gui.h"
#include "file.h"

#define SFLSYSLAYOUTBYTES 256

typedef struct SFLDISK
{
	WSFILE file;
	QcowImage *qcow;
	ChdImage *chd;
	void *bincue;
	bool readonly;
	bool cdrom;
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
	if (readonly || !WSFile_Create(&disk->file, path, FILE_READWRITE | FILE_BINARY)) {
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
	disk->startbyte = 0;
	disk->size = 0;
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

bool SysIsDiskInserted(void *fh)
{
	return fh != NULL;
}

void SysPreventRemoval(void *fh)
{
}

void SysAllowRemoval(void *fh)
{
}

static void *SflDiskBinCue(void *fh)
{
	if (fh == NULL)
		return NULL;
	return ((SFLDISK *)fh)->bincue;
}

bool SysCDReadTOC(void *fh, uint8 *toc)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return readtoc_bincue(bincue, toc);
}

bool SysCDGetPosition(void *fh, uint8 *pos)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return GetPosition_bincue(bincue, pos);
}

bool SysCDPlay(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDPlay_bincue(bincue, start_m, start_s, start_f, end_m, end_s, end_f);
}

bool SysCDPause(void *fh)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDPause_bincue(bincue);
}

bool SysCDResume(void *fh)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDResume_bincue(bincue);
}

bool SysCDStop(void *fh, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDStop_bincue(bincue);
}

bool SysCDScan(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue == NULL)
		return false;
	return CDScan_bincue(bincue, start_m, start_s, start_f, reverse);
}

void SysCDSetVolume(void *fh, uint8 left, uint8 right)
{
	void *bincue;
	bincue = SflDiskBinCue(fh);
	if (bincue != NULL)
		CDSetVol_bincue(bincue, left, right);
}

void SysCDGetVolume(void *fh, uint8 &left, uint8 &right)
{
	void *bincue;
	left = 0;
	right = 0;
	bincue = SflDiskBinCue(fh);
	if (bincue != NULL)
		CDGetVol_bincue(bincue, &left, &right);
}
