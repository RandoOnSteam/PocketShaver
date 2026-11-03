/*
 *  extfs_sfl.cpp - Host file system support of the SFL platform layer
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
#include "cpu_emulation.h"
#include "macos_util.h"
#include "extfs.h"
#include "extfs_defs.h"
#include "sys.h"
#include "sflroman.h"
#include "gui.h"
#include "file.h"
#include "posix_emu.h"

#define SFLEXTFSFINF ".finf"
#define SFLEXTFSRSRC ".rsrc"
#define SFLEXTFSNAMESIZE (MAX_PATH_LENGTH * 3)
#define SFLEXTFSHEX "0123456789ABCDEF"

typedef struct SFLEXTFSTYPE
{
	const char *ext;
	uint32 type;
	uint32 creator;
} SFLEXTFSTYPE;

typedef struct SFLEXTFS
{
	char guestname[SFLEXTFSNAMESIZE];
	char hostname[SFLEXTFSNAMESIZE];
} SFLEXTFS;

static SFLEXTFS SflExtFS;

static const SFLEXTFSTYPE SflExtFSTypes[] = {
	{".Z", FOURCC('Z','I','V','M'), FOURCC('L','Z','I','V')},
	{".gz", FOURCC('G','z','i','p'), FOURCC('G','z','i','p')},
	{".hqx", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".bin", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".pdf", FOURCC('P','D','F',' '), FOURCC('C','A','R','O')},
	{".ps", FOURCC('T','E','X','T'), FOURCC('t','t','x','t')},
	{".sit", FOURCC('S','I','T','!'), FOURCC('S','I','T','x')},
	{".tar", FOURCC('T','A','R','F'), FOURCC('T','A','R',' ')},
	{".zip", FOURCC('Z','I','P',' '), FOURCC('Z','I','P',' ')},
	{".aiff", FOURCC('A','I','F','F'), FOURCC('T','V','O','D')},
	{".mid", FOURCC('M','I','D','I'), FOURCC('T','V','O','D')},
	{".mp3", FOURCC('M','P','G',' '), FOURCC('T','V','O','D')},
	{".wav", FOURCC('W','A','V','E'), FOURCC('T','V','O','D')},
	{".bmp", FOURCC('B','M','P','f'), FOURCC('o','g','l','e')},
	{".gif", FOURCC('G','I','F','f'), FOURCC('o','g','l','e')},
	{".jpg", FOURCC('J','P','E','G'), FOURCC('o','g','l','e')},
	{".jpeg", FOURCC('J','P','E','G'), FOURCC('o','g','l','e')},
	{".pict", FOURCC('P','I','C','T'), FOURCC('o','g','l','e')},
	{".png", FOURCC('P','N','G','f'), FOURCC('o','g','l','e')},
	{".tif", FOURCC('T','I','F','F'), FOURCC('o','g','l','e')},
	{".tiff", FOURCC('T','I','F','F'), FOURCC('o','g','l','e')},
	{".htm", FOURCC('T','E','X','T'), FOURCC('M','O','S','S')},
	{".html", FOURCC('T','E','X','T'), FOURCC('M','O','S','S')},
	{".txt", FOURCC('T','E','X','T'), FOURCC('t','t','x','t')},
	{".c", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".cpp", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".h", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".mov", FOURCC('M','o','o','V'), FOURCC('T','V','O','D')},
	{".dsk", FOURCC('D','D','i','m'), FOURCC('d','d','s','k')},
	{".img", FOURCC('r','o','h','d'), FOURCC('d','d','s','k')},
	{NULL, 0, 0}
};

void extfs_init(void)
{
}

void extfs_exit(void)
{
}

void add_path_component(char *path, const char *component)
{
	size_t length;
	length = strlen(path);
	if (length > 0 && length < MAX_PATH_LENGTH - 1 && path[length - 1] != '/' && path[length - 1] != WSPATHSEPARATORCHAR) {
		path[length] = WSPATHSEPARATORCHAR;
		path[length + 1] = 0;
	}
	strncat(path, component, MAX_PATH_LENGTH - 1 - strlen(path));
}

static const char *SflExtFSLastPart(const char *path)
{
	const char *last;
	const char *other;
	last = strrchr(path, '/');
	other = strrchr(path, WSPATHSEPARATORCHAR);
	if (other > last)
		last = other;
	if (last == NULL)
		return path;
	return last + 1;
}

static void SflExtFSHelperPath(const char *path, char *helper, const char *folder, bool onlyfolder)
{
	const char *last;
	size_t prefix;
	last = SflExtFSLastPart(path);
	prefix = (size_t)(last - path);
	if (prefix > MAX_PATH_LENGTH - 1)
		prefix = MAX_PATH_LENGTH - 1;
	memcpy(helper, path, prefix);
	helper[prefix] = 0;
	strncat(helper, folder, MAX_PATH_LENGTH - 1 - strlen(helper));
	if (onlyfolder)
		return;
	add_path_component(helper, last);
}

static int SflExtFSOpenHelper(const char *path, const char *folder, int flag)
{
	char helper[MAX_PATH_LENGTH];
	char helperfolder[MAX_PATH_LENGTH];
	int fd;
	SflExtFSHelperPath(path, helper, folder, false);
	if ((flag & O_ACCMODE) == O_RDONLY)
		return open(helper, flag);
	fd = open(helper, flag | O_CREAT);
	if (fd >= 0 || errno != ENOENT)
		return fd;
	SflExtFSHelperPath(path, helperfolder, folder, true);
	mkdir(helperfolder, 0777);
	return open(helper, flag | O_CREAT);
}

static int SflExtFSOpenFork(const char *path, int flag)
{
#if defined(WSFILE_HASFORKS)
	return SflPosixOpenFork(path, flag);
#else
	return SflExtFSOpenHelper(path, SFLEXTFSRSRC, flag);
#endif
}

static void SflExtFSDefaultInfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	size_t length;
	size_t extlength;
	int index;
	Mac_memset(finfo, 0, SIZEOF_FInfo);
	if (fxinfo)
		Mac_memset(fxinfo, 0, SIZEOF_FXInfo);
	WriteMacInt16(finfo + fdFlags, kHasBeenInited);
	WriteMacInt32(finfo + fdLocation, (uint32)-1);
	if (is_dir)
		return;
	length = strlen(path);
	for (index = 0; SflExtFSTypes[index].ext != NULL; index++) {
		extlength = strlen(SflExtFSTypes[index].ext);
		if (length >= extlength && strcmp(path + length - extlength, SflExtFSTypes[index].ext) == 0) {
			WriteMacInt32(finfo + fdType, SflExtFSTypes[index].type);
			WriteMacInt32(finfo + fdCreator, SflExtFSTypes[index].creator);
			return;
		}
	}
}

void get_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	uint8 info[WSFILEFINDERINFOSIZE];
	ssize_t actual;
	int fd;
	SflExtFSDefaultInfo(path, finfo, fxinfo, is_dir);
#if defined(WSFILE_HASFORKS)
	if (WSFileGetFinderInfo(path, info) != WSFILE_ERROR_NONE)
		return;
	if (ReadMacInt32(finfo + fdType) != 0 && *(uint32 *)info == 0)
		return;
	Host2Mac_memcpy(finfo, info, SIZEOF_FInfo);
	if (fxinfo)
		Host2Mac_memcpy(fxinfo, info + SIZEOF_FInfo, SIZEOF_FXInfo);
#else
	fd = SflExtFSOpenHelper(path, SFLEXTFSFINF, O_RDONLY);
	if (fd < 0)
		return;
	actual = read(fd, info, sizeof(info));
	close(fd);
	if (actual < SIZEOF_FInfo)
		return;
	Host2Mac_memcpy(finfo, info, SIZEOF_FInfo);
	if (fxinfo && actual >= SIZEOF_FInfo + SIZEOF_FXInfo)
		Host2Mac_memcpy(fxinfo, info + SIZEOF_FInfo, SIZEOF_FXInfo);
#endif
}

void set_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	uint8 info[WSFILEFINDERINFOSIZE];
	int fd;
	memset(info, 0, sizeof(info));
#if defined(WSFILE_HASFORKS)
	WSFileGetFinderInfo(path, info);
#else
	fd = SflExtFSOpenHelper(path, SFLEXTFSFINF, O_RDONLY);
	if (fd >= 0) {
		read(fd, info, sizeof(info));
		close(fd);
	}
#endif
	Mac2Host_memcpy(info, finfo, SIZEOF_FInfo);
	if (fxinfo)
		Mac2Host_memcpy(info + SIZEOF_FInfo, fxinfo, SIZEOF_FXInfo);
#if defined(WSFILE_HASFORKS)
	WSFileSetFinderInfo(path, info);
#else
	fd = SflExtFSOpenHelper(path, SFLEXTFSFINF, O_RDWR);
	if (fd < 0)
		return;
	write(fd, info, sizeof(info));
	close(fd);
#endif
}

uint32 get_rfork_size(const char *path)
{
	loff_t size;
	int fd;
	fd = SflExtFSOpenFork(path, O_RDONLY);
	if (fd < 0)
		return 0;
	size = SysGetDescriptorSize(fd);
	close(fd);
	if (size < 0)
		return 0;
	return (uint32)size;
}

int open_rfork(const char *path, int flag)
{
	return SflExtFSOpenFork(path, flag);
}

void close_rfork(const char *path, int fd)
{
	close(fd);
}

ssize_t extfs_read(int fd, void *buffer, size_t length)
{
	return read(fd, buffer, length);
}

ssize_t extfs_write(int fd, void *buffer, size_t length)
{
	return write(fd, buffer, length);
}

static void SflExtFSRemoveHelpers(const char *path)
{
	char helper[MAX_PATH_LENGTH];
	SflExtFSHelperPath(path, helper, SFLEXTFSFINF, false);
	SflPosixRemove(helper);
	SflExtFSHelperPath(path, helper, SFLEXTFSRSRC, false);
	SflPosixRemove(helper);
}

bool extfs_remove(const char *path)
{
	char helper[MAX_PATH_LENGTH];
	struct SflPosixStat info;
	if (stat(path, &info) < 0)
		return false;
	SflExtFSRemoveHelpers(path);
	if (!S_ISDIR(info.st_mode))
		return SflPosixRemove(path) == 0;
	strncpy(helper, path, MAX_PATH_LENGTH - 1);
	helper[MAX_PATH_LENGTH - 1] = 0;
	add_path_component(helper, SFLEXTFSFINF);
	rmdir(helper);
	strncpy(helper, path, MAX_PATH_LENGTH - 1);
	helper[MAX_PATH_LENGTH - 1] = 0;
	add_path_component(helper, SFLEXTFSRSRC);
	rmdir(helper);
	return rmdir(path) == 0;
}

static void SflExtFSRenameHelper(const char *old_path, const char *new_path, const char *folder)
{
	char oldhelper[MAX_PATH_LENGTH];
	char newhelper[MAX_PATH_LENGTH];
	struct SflPosixStat info;
	SflExtFSHelperPath(old_path, oldhelper, folder, false);
	if (stat(oldhelper, &info) < 0)
		return;
	SflExtFSHelperPath(new_path, newhelper, folder, true);
	mkdir(newhelper, 0777);
	SflExtFSHelperPath(new_path, newhelper, folder, false);
	SflPosixRename(oldhelper, newhelper);
}

bool extfs_rename(const char *old_path, const char *new_path)
{
	SflExtFSRenameHelper(old_path, new_path, SFLEXTFSFINF);
	SflExtFSRenameHelper(old_path, new_path, SFLEXTFSRSRC);
	return SflPosixRename(old_path, new_path) == 0;
}

static int SflExtFSHexValue(char digit)
{
	const char *found;
	if (digit >= 'a' && digit <= 'f')
		digit = (char)(digit - 'a' + 'A');
	found = strchr(SFLEXTFSHEX, digit);
	if (found == NULL || digit == 0)
		return -1;
	return (int)(found - SFLEXTFSHEX);
}

static bool SflExtFSNeedsEscape(uint8 character)
{
	if (character < 0x20 || character >= 0x80)
		return true;
	return strchr("/\\:*?\"<>|%", character) != NULL;
}

const char *host_encoding_to_macroman(const char *filename)
{
#if defined(WSMACCLASSICORCARBON)
	return filename;
#else
	size_t used;
	size_t index;
	size_t run;
	int high;
	int low;
	used = 0;
	run = 0;
	for (index = 0; filename[index] != 0 && used < sizeof(SflExtFS.guestname) - 1; index++) {
		high = -1;
		low = -1;
		if (filename[index] == '%' && filename[index + 1] != 0 && filename[index + 2] != 0) {
			high = SflExtFSHexValue(filename[index + 1]);
			low = SflExtFSHexValue(filename[index + 2]);
		}
		if (high < 0 || low < 0)
			continue;
		used += SflUtf8ToRoman(filename + run, index - run, (uint8 *)SflExtFS.guestname + used,
			sizeof(SflExtFS.guestname) - 1 - used, false);
		if (used < sizeof(SflExtFS.guestname) - 1)
			SflExtFS.guestname[used++] = (char)((high << 4) | low);
		index += 2;
		run = index + 1;
	}
	used += SflUtf8ToRoman(filename + run, index - run, (uint8 *)SflExtFS.guestname + used,
		sizeof(SflExtFS.guestname) - 1 - used, false);
	SflExtFS.guestname[used] = 0;
	return SflExtFS.guestname;
#endif
}

const char *macroman_to_host_encoding(const char *filename)
{
#if defined(WSMACCLASSICORCARBON)
	return filename;
#else
	size_t used;
	size_t index;
	uint8 character;
	used = 0;
	for (index = 0; filename[index] != 0 && used < sizeof(SflExtFS.hostname) - 4; index++) {
		character = (uint8)filename[index];
		if (SflExtFSNeedsEscape(character)) {
			SflExtFS.hostname[used++] = '%';
			SflExtFS.hostname[used++] = SFLEXTFSHEX[character >> 4];
			SflExtFS.hostname[used++] = SFLEXTFSHEX[character & 0x0f];
		} else
			SflExtFS.hostname[used++] = (char)character;
	}
	SflExtFS.hostname[used] = 0;
	return SflExtFS.hostname;
#endif
}
