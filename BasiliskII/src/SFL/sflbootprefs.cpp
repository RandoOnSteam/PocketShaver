#include "sysdeps.h"
#include "prefs.h"
#include "gui.h"
#include "sflbootgui.h"
#ifdef SHEEPSHAVER
extern int OsxGuestRequested(void);
#endif

#define SFLBOOTSCREENDEFAULT "win/800/600"
#define SFLBOOTSCREENSIZE 64
#define SFLBOOTFONTFACE "Segoe UI"
#define SFLBOOTHEADINGPOINTS 16
#define SFLBOOTBODYPOINTS 14

int SflBootPrefInt(const char *name)
{
	return PrefsFindInt32(name);
}

void SflBootSetString(const char *name, const char *value, int index)
{
	PrefsReplaceString(name, value, index);
}

void SflBootSetFirstString(const char *name, const char *value)
{
	if (*value != 0)
		PrefsReplaceString(name, value, 0);
	else if (PrefsFindString(name) != NULL)
		PrefsRemoveItem(name);
}

void SflBootSetBool(const char *name, int value)
{
	PrefsReplaceBool(name, value != 0);
}

void SflBootSetInt(const char *name, int value)
{
	PrefsReplaceInt32(name, value);
}

void SflBootRemove(const char *name)
{
	while (PrefsFindString(name))
		PrefsRemoveItem(name);
}

static const char *SflBootScreen(void)
{
	const char *screen;
	screen = PrefsFindString("screen");
	if (screen == NULL || strchr(screen, '/') == NULL)
		screen = SFLBOOTSCREENDEFAULT;
	return screen;
}

static void SflBootCopyValue(char *value, size_t valuesize, const char *text)
{
	value[0] = 0;
	strncat(value, text, valuesize - 1);
}

void SflBootSpecialValue(const char *name, char *value, size_t valuesize)
{
	const char *screen;
	const char *mode;
	if (strcmp(name, "screenmode") == 0) {
		screen = SflBootScreen();
		SflBootCopyValue(value, valuesize, screen);
		*strchr(value, '/') = 0;
	} else if (strcmp(name, "screensize") == 0) {
		SflBootCopyValue(value, valuesize, strchr(SflBootScreen(), '/') + 1);
	} else if (PrefsFindBool("nonet")) {
		SflBootCopyValue(value, valuesize, "off");
	} else {
		mode = PrefsFindString("ether");
#ifndef SHEEPSHAVER
		if (PrefsFindBool("udptunnel"))
			mode = "udptunnel";
#endif
		if (mode == NULL)
			mode = "slirp";
		SflBootCopyValue(value, valuesize, mode);
	}
}

static void SflBootReplaceScreen(const char *mode, const char *size)
{
	char screen[SFLBOOTSCREENSIZE];
	sprintf(screen, "%s/%s", mode, size);
	PrefsReplaceString("screen", screen);
}

void SflBootSetSpecial(const char *name, const char *value)
{
	char current[SFLBOOTSCREENSIZE];
	if (strcmp(name, "screenmode") == 0) {
		SflBootSpecialValue("screensize", current, sizeof(current));
		SflBootReplaceScreen(value, current);
	} else if (strcmp(name, "screensize") == 0) {
		SflBootSpecialValue("screenmode", current, sizeof(current));
		SflBootReplaceScreen(current, value);
	} else if (strcmp(value, "off") == 0) {
		PrefsReplaceBool("nonet", true);
	} else {
		PrefsReplaceBool("nonet", false);
#ifndef SHEEPSHAVER
		PrefsReplaceBool("udptunnel", strcmp(value, "udptunnel") == 0);
#endif
		PrefsReplaceString("ether", value);
	}
}

int SflBootIsSheepShaver(void)
{
#ifdef SHEEPSHAVER
	return 1;
#else
	return 0;
#endif
}

int SflBootIsNativeBoot(void)
{
#ifdef SHEEPSHAVER
	return OsxGuestRequested();
#else
	return 0;
#endif
}

bool PrefsEditor(void)
{
	return true;
}

int SflBootCreateFonts(WSFONT *heading, WSFONT *body)
{
#if defined(WSWINGUI)
	WSFONTCHANGE change;
	memset(&change, 0, sizeof(change));
	change.facename = SFLBOOTFONTFACE;
	change.fontsize = SFLBOOTHEADINGPOINTS;
	change.bold = 1;
	if (!WSFont_CreateWithChange(heading, &change))
		return 0;
	change.fontsize = SFLBOOTBODYPOINTS;
	change.bold = 0;
	if (!WSFont_CreateWithChange(body, &change)) {
		WSFont_Destroy(heading);
		return 0;
	}
	return 1;
#else
	(void)heading;
	(void)body;
	return 0;
#endif
}

const char *SflBootInstructionsText(void)
{
	return
	"1. Select a compatible Macintosh ROM file.\n\n"
	"2. In Disks, select Create and make an empty disk (recommended minimum size is around 500 MB).\n\n"
	"3. Add a Mac OS install disc image and mount it. The maximum supported Mac OS version is 9.0.4.\n\n"
	"4. Boot, let Mac OS format the empty disk, and launch the Mac OS Installer from the disc.\n\n"
	"5. Important: in the Mac OS Installer, at the Install software step, click Options and uncheck Update Apple Hard Disk Drivers, then install Mac OS.\n\n"
	"6. After installation, quit, remove the install disc from Disks, and boot.\n\n"
	"7. Quit the Mac OS Setup Assistant. It cannot be completed without getting stuck.\n\n"
	"8. Important: to get audio working, select Built-in as the Sound Out option in the Sound control panel. This only has to be done once.\n\n"
	"Mac OS X boots from its own core and does not need a ROM.\n";
}

const char *SflBootLicenseText(void)
{
	return
	"This program is free software; you can redistribute it and/or modify\n"
	"it under the terms of the GNU General Public License as published by\n"
	"the Free Software Foundation; either version 2 of the License, or\n"
	"(at your option) any later version.\n"
	"\n"
	"This program is distributed in the hope that it will be useful,\n"
	"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
	"GNU General Public License for more details.\n"
	"\n"
	"You should have received a copy of the GNU General Public License\n"
	"along with this program; if not, write to the Free Software\n"
	"Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA\n";
}

int SflBootValidateRedir(const char *text)
{
	char protocol[4];
	char extra;
	int hostport;
	int guestport;
	int first;
	int second;
	int third;
	int fourth;
	if (sscanf(text, "%3[^:]:%d:%d.%d.%d.%d:%d%c", protocol, &hostport, &first, &second, &third, &fourth, &guestport, &extra) != 7)
		return 0;
	if (strcmp(protocol, "tcp") != 0 && strcmp(protocol, "udp") != 0)
		return 0;
	if (hostport <= 0 || hostport > 65535 || guestport <= 0 || guestport > 65535)
		return 0;
	return first >= 0 && first <= 255 && second >= 0 && second <= 255 && third >= 0 && third <= 255 && fourth >= 0 && fourth <= 255;
}
