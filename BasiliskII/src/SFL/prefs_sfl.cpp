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

#ifdef SHEEPSHAVER
#define SFLPREFSFILENAME "SheepShaver_prefs"
#else
#define SFLPREFSFILENAME "BasiliskII_prefs"
#endif

prefs_desc platform_prefs_items[] = {
	{"rightclick", TYPE_STRING, false, "right click modifier"},
	{"showfps", TYPE_BOOLEAN, false, "show frame rate"},
	{"shownetrate", TYPE_BOOLEAN, false, "show network transfer rate"},
	{"notifyip", TYPE_BOOLEAN, false, "report assigned IP address"},
	{"notifyclip", TYPE_BOOLEAN, false, "report clipboard transfers"},
	{"debugextfs", TYPE_BOOLEAN, false, "kept for other builds sharing this file"},
	{"extdrives", TYPE_STRING, false, "kept for other builds sharing this file"},
	{"pollmedia", TYPE_BOOLEAN, false, "kept for other builds sharing this file"},
	{"etherpermanentaddress", TYPE_BOOLEAN, false, "kept for other builds sharing this file"},
	{"ethermulticastmode", TYPE_INT32, false, "kept for other builds sharing this file"},
	{"routerenabled", TYPE_BOOLEAN, false, "kept for other builds sharing this file"},
	{"ftp_port_list", TYPE_STRING, false, "kept for other builds sharing this file"},
	{"tcp_port", TYPE_STRING, false, "kept for other builds sharing this file"},
	{"portfile0", TYPE_STRING, false, "kept for other builds sharing this file"},
	{"portfile1", TYPE_STRING, false, "kept for other builds sharing this file"},
#ifdef SHEEPSHAVER
	{"mousewheelmode", TYPE_INT32, false, "kept for other builds sharing this file"},
	{"mousewheellines", TYPE_INT32, false, "kept for other builds sharing this file"},
	{"altivec", TYPE_BOOLEAN, false, "enable AltiVec"},
	{"relmouse", TYPE_STRING, false, "relative mouse mode"},
#endif
#ifndef SHEEPSHAVER
	{"nonet", TYPE_BOOLEAN, false, "disable networking"},
#endif
	{"idlewait", TYPE_BOOLEAN, false, "sleep when idle"},
	{"enableextfs", TYPE_BOOLEAN, false, "enable extfs system"},
#ifdef SHEEPSHAVER
	{"ether", TYPE_STRING, false, "network mode (slirp or udptunnel)"},
	{"udpport", TYPE_INT32, false, "UDP port of the network tunnel"},
#endif
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
		return;
	}
	LoadPrefsFromStream(file);
	fclose(file);
}

extern "C" int SflBootSave(void)
{
	FILE *file;
	int success;
	file = fopen(SflPrefsPath(), "w");
	if (file == NULL)
		return 0;
	SavePrefsToStream(file);
	success = ferror(file) == 0;
	if (fclose(file) != 0) success = 0;
	return success;
}

void SavePrefs(void)
{
	SflBootSave();
}

void AddPlatformPrefsDefaults(void)
{
	PrefsAddString("rightclick", "control");
	PrefsAddBool("showfps", false);
	PrefsAddBool("shownetrate", false);
	PrefsAddBool("notifyip", true);
	PrefsAddBool("notifyclip", false);
#ifdef SHEEPSHAVER
	PrefsAddBool("altivec", true);
	PrefsAddString("relmouse", "manual");
#endif
	PrefsAddBool("idlewait", true);
	PrefsReplaceBool("enableextfs", false);
	PrefsReplaceString("extfs", "");
#ifdef SHEEPSHAVER
	PrefsAddInt32("udpport", 6066);
#endif
}
