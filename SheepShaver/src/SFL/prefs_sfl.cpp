/*
 *  prefs_sfl.cpp - Preferences file for the SFL platform layer
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
#include "prefs.h"
#include "sflmain.h"

#define SFLPREFSFILENAME "SheepShaver_prefs"

prefs_desc platform_prefs_items[] = {
	{"idlewait", TYPE_BOOLEAN, false, "sleep when idle"},
	{"enableextfs", TYPE_BOOLEAN, false, "enable extfs system"},
	{"ether", TYPE_STRING, false, "network mode (slirp or udptunnel)"},
	{"udpport", TYPE_INT32, false, "UDP port of the network tunnel"},
	{NULL, TYPE_END, false, NULL}
};

static const char *SflPrefsPath(void)
{
	const char *path;
	path = SflMainPrefsPath();
	if (path != NULL)
		return path;
	return SFLPREFSFILENAME;
}

void LoadPrefs(const char *vmdir)
{
	FILE *file;
	file = fopen(SflPrefsPath(), "r");
	if (file == NULL) {
		SavePrefs();
		return;
	}
	LoadPrefsFromStream(file);
	fclose(file);
}

void SavePrefs(void)
{
	FILE *file;
	file = fopen(SflPrefsPath(), "w");
	if (file == NULL)
		return;
	SavePrefsToStream(file);
	fclose(file);
}

void AddPlatformPrefsDefaults(void)
{
	PrefsAddBool("idlewait", true);
	PrefsReplaceBool("enableextfs", false);
	PrefsReplaceString("extfs", "");
	PrefsAddInt32("udpport", 6066);
}
