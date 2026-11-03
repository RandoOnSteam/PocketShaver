/*
 *  posix_emu.h - POSIX file calls used by extfs, implemented over file.h
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

#ifndef SFLPOSIXEMU_H
#define SFLPOSIXEMU_H

#include <time.h>
#include <errno.h>
#include <fcntl.h>

#define SFLPOSIXNAMESIZE 256
#define SFLPOSIXIFDIR 0x4000
#define SFLPOSIXIFREG 0x8000

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

#undef S_ISDIR
#define S_ISDIR(mode) (((mode) & SFLPOSIXIFDIR) != 0)
#undef st_atime
#undef st_mtime
#undef st_ctime
#undef st_crtime

struct SflPosixStat {
	uint32 st_mode;
	loff_t st_size;
	time_t st_mtime;
	time_t st_crtime;
};

struct SflPosixDirent {
	char d_name[SFLPOSIXNAMESIZE];
};

typedef struct SFLPOSIXDIR SFLPOSIXDIR;

int SflPosixStat(const char *path, struct SflPosixStat *info);
int SflPosixAccess(const char *path, int mode);
int SflPosixOpen(const char *path, int flag, int mode = 0);
int SflPosixOpenFork(const char *path, int flag);
int SflPosixCreat(const char *path, int mode);
int SflPosixClose(int fd);
ssize_t SflPosixRead(int fd, void *buffer, size_t length);
ssize_t SflPosixWrite(int fd, const void *buffer, size_t length);
int SflPosixFtruncate(int fd, loff_t size);
int SflPosixMkdir(const char *path, int mode);
int SflPosixRmdir(const char *path);
int SflPosixRemove(const char *path);
int SflPosixRename(const char *from, const char *to);
SFLPOSIXDIR *SflPosixOpenDir(const char *path);
struct SflPosixDirent *SflPosixReadDir(SFLPOSIXDIR *dir);
int SflPosixCloseDir(SFLPOSIXDIR *dir);

#define stat SflPosixStat
#define access SflPosixAccess
#define open SflPosixOpen
#define creat SflPosixCreat
#define close SflPosixClose
#define read SflPosixRead
#define write SflPosixWrite
#define ftruncate SflPosixFtruncate
#define mkdir SflPosixMkdir
#define rmdir SflPosixRmdir
#define DIR SFLPOSIXDIR
#define dirent SflPosixDirent
#define opendir SflPosixOpenDir
#define readdir SflPosixReadDir
#define closedir SflPosixCloseDir

#endif
