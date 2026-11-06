/*
 *  sys.h - System dependent routines (mostly I/O)
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#ifndef SYS_H
#define SYS_H

// Supported media types
enum {
	MEDIA_FLOPPY		= 1,
	MEDIA_CD			= 2,
	MEDIA_HD			= 4,
	MEDIA_REMOVABLE		= MEDIA_FLOPPY | MEDIA_CD
};

extern void SysInit(void);
extern void SysExit(void);

extern void SysAddFloppyPrefs(void);
extern void SysAddDiskPrefs(void);
extern void SysAddCDROMPrefs(void);
extern void SysAddSerialPrefs(void);

/*
 *  These routines are used for reading from and writing to disk files
 *  or devices in sony.cpp, disk.cpp and cdrom.cpp. Their purpose is to
 *  hide all OS-specific details of file and device access. The routines
 *  must also hide all restrictions on the location or alignment of the
 *  data buffer or on the transfer length that may be imposed by the
 *  underlying OS.
 *  A file/device is identified by a "filename" (character string) that
 *  may (but need not) map to a valid file path. After Sys_open(), the
 *  file/device is identified by an abstract "file handle" (void *),
 *  that is freed by Sys_close().
 */

extern void *Sys_open(const char *name, bool read_only, bool is_cdrom = false);
extern void Sys_close(void *fh);
extern size_t Sys_read(void *fh, void *buffer, loff_t offset, size_t length);
extern size_t Sys_write(void *fh, void *buffer, loff_t offset, size_t length);
extern loff_t SysGetFileSize(void *fh);
extern loff_t SysSeekDescriptor(int filedescriptor, loff_t offset, int origin);
extern loff_t SysGetDescriptorSize(int filedescriptor);
extern void SysEject(void *fh);
extern bool SysFormat(void *fh);
extern bool SysIsReadOnly(void *fh);
extern const char *SysImageKind(void *fh);
extern bool SysIsFixedDisk(void *fh);
extern bool SysIsDiskInserted(void *fh);

extern void SysPreventRemoval(void *fh);
extern void SysAllowRemoval(void *fh);
extern bool SysCDReadTOC(void *fh, uint8 *toc);
#ifndef HAVE_SYS_CD_READ_SECTOR
#if defined(_WIN32) || defined(__linux__) || defined(USE_SFL)
#define HAVE_SYS_CD_READ_SECTOR 1
#else
#define HAVE_SYS_CD_READ_SECTOR 0
#endif
#endif
extern int SysCDReadSector(void *fh, void *buffer, uint32 sector, uint8 type, uint8 flags);
extern bool SysCDGetPosition(void *fh, uint8 *pos);
extern bool SysCDPlay(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f);
extern bool SysCDPause(void *fh);
extern bool SysCDResume(void *fh);
extern bool SysCDStop(void *fh, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f);
extern bool SysCDScan(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse);
extern void SysCDSetVolume(void *fh, uint8 left, uint8 right);
extern void SysCDGetVolume(void *fh, uint8 &left, uint8 &right);
#if defined(USE_SFL)
extern bool SysCDReadMCN(void *fh, char *mcn);
extern bool SysCDReadSubcodes(void *fh, void *buffer, uint32 sector, uint32 count);
extern bool SysCDReadISRC(void *fh, int track, char *isrc);
extern bool SysCDGetSpeed(void *fh, unsigned int *kbps);
extern bool SysCDSetSpeed(void *fh, uint32 kbps);
extern bool SysCDGetFeatures(void *fh, unsigned int *maxkbps, unsigned int *flags);
#endif

#endif
