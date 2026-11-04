/*
 *  posix_sfl.cpp - POSIX file calls used by extfs, implemented over file.h
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
#include "gui.h"
#include "file.h"
#include "posix_emu.h"

#define SFLPOSIXMAXFILES 64

typedef struct SFLPOSIXFILES
{
	WSFILE files[SFLPOSIXMAXFILES];
	bool used[SFLPOSIXMAXFILES];
} SFLPOSIXFILES;

struct SFLPOSIXDIR
{
	WSFOLDER folder;
	struct SflPosixDirent entry;
};

static SFLPOSIXFILES SflPosixFiles;

static int SflPosixErrno(wsfileerror_t error)
{
	switch (error) {
	case WSFILE_ERROR_NONE:
		return 0;
	case WSFILE_ERROR_NOTFOUND:
		return ENOENT;
	case WSFILE_ERROR_EXISTS:
		return EEXIST;
	case WSFILE_ERROR_ACCESS:
		return EACCES;
	case WSFILE_ERROR_BUSY:
		return EBUSY;
	case WSFILE_ERROR_NOTEMPTY:
		return ENOTEMPTY;
	case WSFILE_ERROR_DISKFULL:
		return ENOSPC;
	case WSFILE_ERROR_READONLY:
		return EROFS;
	case WSFILE_ERROR_UNSUPPORTED:
		return EINVAL;
	default:
		return EIO;
	}
}

static int SflPosixFail(wsfileerror_t error)
{
	errno = SflPosixErrno(error);
	return -1;
}

static WSFILE *SflPosixFile(int fd)
{
	if (fd < 0 || fd >= SFLPOSIXMAXFILES || !SflPosixFiles.used[fd])
		return NULL;
	return &SflPosixFiles.files[fd];
}

static int SflPosixAllocate(void)
{
	int fd;
	for (fd = 0; fd < SFLPOSIXMAXFILES; fd++) {
		if (!SflPosixFiles.used[fd]) {
			WSFile_Construct(&SflPosixFiles.files[fd]);
			return fd;
		}
	}
	return -1;
}

static long SflPosixFlags(int flag)
{
	long flags;
	flags = FILE_BINARY;
	if ((flag & O_ACCMODE) == O_RDWR)
		flags |= FILE_READWRITE;
	else if ((flag & O_ACCMODE) == O_WRONLY)
		flags |= FILE_WRITE;
	else
		flags |= FILE_READ;
	return flags;
}

static int SflPosixOpenFlags(const char *path, long flags, bool create, bool truncate)
{
	WSFOLDERFILEINFO info;
	wsfileerror_t error;
	int fd;
	error = WSFileGetInfo(path, &info);
	if (error == WSFILE_ERROR_NONE && (info.nFlags & WSFILEINFO_FOLDER))
		return SflPosixFail(WSFILE_ERROR_ACCESS);
	if (error != WSFILE_ERROR_NONE && !create)
		return SflPosixFail(error);
	if (truncate || error != WSFILE_ERROR_NONE)
		flags |= FILE_CREATEALWAYS;
	fd = SflPosixAllocate();
	if (fd < 0) {
		errno = EMFILE;
		return -1;
	}
	if (!WSFile_Create(&SflPosixFiles.files[fd], path, flags)) {
		if (error == WSFILE_ERROR_NONE && (info.nFlags & WSFILEINFO_READONLY))
			return SflPosixFail(WSFILE_ERROR_READONLY);
		if (error == WSFILE_ERROR_NONE)
			return SflPosixFail(WSFILE_ERROR_BUSY);
		return SflPosixFail(WSFILE_ERROR_ACCESS);
	}
	SflPosixFiles.used[fd] = true;
	return fd;
}

int SflPosixOpen(const char *path, int flag, int mode)
{
	return SflPosixOpenFlags(path, SflPosixFlags(flag), (flag & O_CREAT) != 0, (flag & O_TRUNC) != 0);
}

int SflPosixOpenFork(const char *path, int flag)
{
	return SflPosixOpenFlags(path, SflPosixFlags(flag) | FILE_RESOURCEFORK, false, false);
}

int SflPosixCreat(const char *path, int mode)
{
	return SflPosixOpenFlags(path, FILE_READWRITE | FILE_BINARY, true, true);
}

int SflPosixClose(int fd)
{
	WSFILE *file;
	file = SflPosixFile(fd);
	if (file == NULL) {
		errno = EBADF;
		return -1;
	}
	WSFile_Destroy(file);
	SflPosixFiles.used[fd] = false;
	return 0;
}

ssize_t SflPosixRead(int fd, void *buffer, size_t length)
{
	WSFILE *file;
	file = SflPosixFile(fd);
	if (file == NULL) {
		errno = EBADF;
		return -1;
	}
	return (ssize_t)WSFile_Read(file, buffer, length);
}

ssize_t SflPosixWrite(int fd, const void *buffer, size_t length)
{
	WSFILE *file;
	size_t written;
	file = SflPosixFile(fd);
	if (file == NULL) {
		errno = EBADF;
		return -1;
	}
	written = WSFile_Write(file, buffer, length);
	if (written < length) {
		errno = ENOSPC;
		if (written == 0)
			return -1;
	}
	return (ssize_t)written;
}

int SflPosixFtruncate(int fd, loff_t size)
{
	WSFILE *file;
	wsfileerror_t error;
	file = SflPosixFile(fd);
	if (file == NULL) {
		errno = EBADF;
		return -1;
	}
	error = WSFile_SetSize64(file, (uint32_t)((uint64)size & 0xffffffff), (uint32_t)((uint64)size >> 32));
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	return 0;
}

loff_t SysSeekDescriptor(int filedescriptor, loff_t offset, int origin)
{
	WSFILE *file;
	uint32_t low;
	uint32_t high;
	long mode;
	file = SflPosixFile(filedescriptor);
	if (file == NULL)
		return -1;
	mode = FILE_FROMCURRENT;
	if (origin == SEEK_SET)
		mode = FILE_FROMSTART;
	else if (origin == SEEK_END)
		mode = FILE_FROMEND;
	if (!WSFile_Seek64(file, (uint32_t)((uint64)offset & 0xffffffff), (int32_t)((uint64)offset >> 32), &low, &high, mode))
		return -1;
	return (loff_t)(((uint64)high << 32) | low);
}

loff_t SysGetDescriptorSize(int filedescriptor)
{
	WSFILE *file;
	uint32_t low;
	uint32_t high;
	uint32_t endlow;
	uint32_t endhigh;
	file = SflPosixFile(filedescriptor);
	if (file == NULL)
		return -1;
	if (!WSFile_Tell64(file, &low, &high))
		return -1;
	if (!WSFile_Seek64(file, 0, 0, &endlow, &endhigh, FILE_FROMEND))
		return -1;
	WSFile_Seek64(file, low, (int32_t)high, &low, &high, FILE_FROMSTART);
	return (loff_t)(((uint64)endhigh << 32) | endlow);
}

int SflPosixStat(const char *path, struct SflPosixStat *info)
{
	WSFOLDERFILEINFO fileinfo;
	wsfileerror_t error;
	error = WSFileGetInfo(path, &fileinfo);
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	info->st_mode = SFLPOSIXIFREG;
	if (fileinfo.nFlags & WSFILEINFO_FOLDER)
		info->st_mode = SFLPOSIXIFDIR;
	info->st_size = (loff_t)fileinfo.ulSize;
	info->st_mtime = (time_t)fileinfo.ulModificationTime;
	info->st_crtime = (time_t)fileinfo.ulCreationTime;
	return 0;
}

int SflPosixAccess(const char *path, int mode)
{
	WSFOLDERFILEINFO info;
	wsfileerror_t error;
	error = WSFileGetInfo(path, &info);
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	if ((mode & W_OK) && (info.nFlags & WSFILEINFO_READONLY))
		return SflPosixFail(WSFILE_ERROR_ACCESS);
	return 0;
}

int SflPosixMkdir(const char *path, int mode)
{
	WSFOLDERFILEINFO info;
	if (WSFileGetInfo(path, &info) == WSFILE_ERROR_NONE)
		return SflPosixFail(WSFILE_ERROR_EXISTS);
	if (!WSFolderCreate(path))
		return SflPosixFail(WSFILE_ERROR_ACCESS);
	return 0;
}

int SflPosixRmdir(const char *path)
{
	wsfileerror_t error;
	error = WSFolderRemove(path);
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	return 0;
}

int SflPosixRemove(const char *path)
{
	wsfileerror_t error;
	error = WSFileRemove(path);
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	return 0;
}

int SflPosixRename(const char *from, const char *to)
{
	wsfileerror_t error;
	error = WSFileMove(from, to);
	if (error != WSFILE_ERROR_NONE)
		return SflPosixFail(error);
	return 0;
}

SFLPOSIXDIR *SflPosixOpenDir(const char *path)
{
	SFLPOSIXDIR *dir;
	dir = new SFLPOSIXDIR;
	WSFolder_Create(&dir->folder, path);
	if (!WSFolder_IsCreated(&dir->folder)) {
		delete dir;
		errno = ENOENT;
		return NULL;
	}
	return dir;
}

struct SflPosixDirent *SflPosixReadDir(SFLPOSIXDIR *dir)
{
	WSFOLDERFILEINFO info;
	if (!WSFolder_Next(&dir->folder, &info) || info.sName == NULL)
		return NULL;
	strncpy(dir->entry.d_name, info.sName, SFLPOSIXNAMESIZE - 1);
	dir->entry.d_name[SFLPOSIXNAMESIZE - 1] = 0;
	return &dir->entry;
}

int SflPosixCloseDir(SFLPOSIXDIR *dir)
{
	WSFolder_Destroy(&dir->folder);
	delete dir;
	return 0;
}
