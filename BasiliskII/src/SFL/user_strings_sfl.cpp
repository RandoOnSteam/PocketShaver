/*
 *  user_strings_sfl.cpp - Platform strings for the SFL platform layer
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
#include "user_strings.h"

static const char *SflPlatformString(int num)
{
	switch (num) {
	case STR_EXTFS_VOLUME_NAME:
		return "Host";
#ifdef SHEEPSHAVER
	case STR_RAM_MMAP_ERR:
		return "Cannot allocate Mac RAM.";
	case STR_ROM_MMAP_ERR:
		return "Cannot allocate the Mac ROM area.";
	case STR_KD_SHMAT_ERR:
		return "Cannot allocate Kernel Data.";
	case STR_DR_CACHE_MMAP_ERR:
		return "Cannot allocate the DR Cache.";
	case STR_SHEEP_MEM_MMAP_ERR:
		return "Cannot allocate the SheepShaver data area.";
#endif
	case STR_NO_AUDIO_WARN:
		return "No audio device found, audio output will be disabled.";
	case STR_OPEN_WINDOW_ERR:
		return "Cannot open Mac window.";
#ifdef SHEEPSHAVER
	case STR_SFL_ADDRESS_SPACE_ERR:
		return "Cannot allocate the guest address space table.";
#endif
	}
	return NULL;
}

const char *GetString(int num)
{
	const char *platform;
	int index;
	platform = SflPlatformString(num);
	if (platform != NULL)
		return platform;
	index = 0;
	while (common_strings[index].num >= 0) {
		if (common_strings[index].num == num)
			return common_strings[index].str;
		index++;
	}
	return NULL;
}
