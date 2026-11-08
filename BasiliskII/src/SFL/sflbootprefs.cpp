/*
 *  sflbootgui.c - Prefs for the boot configuration window
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
#include "gui.h"
#include "sflbootgui.h"
#include "sflmain.h"
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif
#ifdef SHEEPSHAVER
extern bool FindChrpPayload(const uint8 *data, uint32 *imageoffset, uint32 *imagesize);
#endif
#ifdef SHEEPSHAVER
extern int OsxGuestRequested(void);
#endif

#define SFLBOOTSCREENDEFAULT "win/800/600"
#define SFLBOOTSCREENSIZE 64
#define SFLBOOTFONTFACE "Segoe UI"
#define SFLBOOTHEADINGPOINTS 16
#define SFLBOOTBODYPOINTS 14
#define SFLBOOTPATHLENGTH 1024
#define SFLBOOTSECTOR 512
#define SFLBOOTROMLIMIT 0x800000
#define SFLBOOTCHRPHEADER "<CHRP-BOOT>"
#define SFLBOOTROMNAME "Mac OS ROM"

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

int SflBootCreateFonts(WSFONT *heading, WSFONT *body, int dpi, int basedpi)
{
#if defined(WSWINGUI)
	WSFONTCHANGE change;
	memset(&change, 0, sizeof(change));
	change.facename = SFLBOOTFONTFACE;
	change.fontsize = SFLBOOTHEADINGPOINTS * dpi / basedpi;
	change.bold = 1;
	if (!WSFont_CreateWithChange(heading, &change))
		return 0;
	change.fontsize = SFLBOOTBODYPOINTS * dpi / basedpi;
	change.bold = 0;
	if (!WSFont_CreateWithChange(body, &change)) {
		WSFont_Destroy(heading);
		return 0;
	}
	return 1;
#else
	(void)heading;
	(void)body;
	(void)dpi;
	(void)basedpi;
	return 0;
#endif
}

void SflBootShareFolder(char *directory, size_t size)
{
	const char *prefs;
	const char *slash;
	const char *back;
	size_t length;
	directory[0] = 0;
	prefs = SflMainPrefsPath();
	if (prefs != NULL) {
		slash = strrchr(prefs, '/');
		back = strrchr(prefs, '\\');
		if (back != NULL && (slash == NULL || back > slash))
			slash = back;
		if (slash != NULL) {
			length = (size_t)(slash - prefs);
			if (length < size) {
				memcpy(directory, prefs, length);
				directory[length] = 0;
				return;
			}
		}
	}
#if defined(_WIN32)
	if (_getcwd(directory, (int)size) == NULL)
		strcpy(directory, ".");
#else
	if (getcwd(directory, size) == NULL)
		strcpy(directory, ".");
#endif
}

void SflBootOpenShareFolder(void)
{
	char directory[SFLBOOTPATHLENGTH];
	char url[SFLBOOTPATHLENGTH + 8];
	SflBootShareFolder(directory, sizeof(directory));
#if defined(_WIN32)
	strcpy(url, directory);
#else
	sprintf(url, "file://%s", directory);
#endif
	WSLaunchURL(url, strlen(url));
}

const char *SflBootDiskExtensions(void)
{
	return "|dsk|dmg|cdr|iso|cue|toast|img|chd|qcow|qcow2|hfv|";
}

const char *SflBootCdExtensions(void)
{
	return "|iso|cdr|toast|cue|chd|";
}

int SflBootExtractRom(const char *disc, char *destination, size_t size)
{
#ifdef SHEEPSHAVER
	FILE *source;
	FILE *output;
	unsigned char sector[SFLBOOTSECTOR];
	unsigned char *image;
	uint32 imageoffset;
	uint32 imagesize;
	size_t bytes;
	size_t total;
	int found;
	source = fopen(disc, "rb");
	if (source == NULL)
		return 0;
	found = 0;
	while (fread(sector, 1, sizeof(sector), source) == sizeof(sector)) {
		if (memcmp(sector, SFLBOOTCHRPHEADER, sizeof(SFLBOOTCHRPHEADER) - 1) == 0) {
			found = 1;
			break;
		}
	}
	if (!found) {
		fclose(source);
		return 0;
	}
	image = (unsigned char *)malloc(SFLBOOTROMLIMIT + 1);
	if (image == NULL) {
		fclose(source);
		return 0;
	}
	memcpy(image, sector, sizeof(sector));
	bytes = fread(image + sizeof(sector), 1, SFLBOOTROMLIMIT - sizeof(sector), source);
	fclose(source);
	total = bytes + sizeof(sector);
	image[total] = 0;
	if (!FindChrpPayload(image, &imageoffset, &imagesize) || (size_t)imageoffset + imagesize > total) {
		free(image);
		return 0;
	}
	SflBootShareFolder(destination, size);
	if (strlen(destination) + sizeof(SFLBOOTROMNAME) + 2 > size) {
		free(image);
		return 0;
	}
#if defined(_WIN32)
	strcat(destination, "\\");
#else
	strcat(destination, "/");
#endif
	strcat(destination, SFLBOOTROMNAME);
	output = fopen(destination, "wb");
	if (output == NULL) {
		free(image);
		return 0;
	}
	bytes = (size_t)imageoffset + imagesize;
	found = fwrite(image, 1, bytes, output) == bytes;
	if (fclose(output) != 0)
		found = 0;
	free(image);
	return found;
#else
	(void)disc;
	(void)destination;
	(void)size;
	return 0;
#endif
}

const char *SflBootBootstrapText(void)
{
	return "Select a Mac OS install disc file to bootstrap the emulator, or select a Macintosh ROM file below.\n\nBootstrapping is not the same as installing Mac OS onto a disk file.";
}

const char *SflBootRelativeMouseText(void)
{
	return
	"What is relative mouse mode?\n\n"
	"When relative mouse mode is enabled, mouse input is no longer processed in terms of absolute position on the screen but rather only in relation to the previous mouse input value.\n\n"
	"This is necessary in some games and software where it must be possible to limitlessly steer with the mouse in a single direction, such as in 3D games.\n\n"
	"You will notice when software you are running is requiring relative mouse mode when steering the mouse does nothing unless relative mouse mode is toggled on.\n\n"
	"How do I toggle it on / off?\n\n"
	"By pressing control + F5 (during emulation).\n\n"
	"How does automatic mode work?\n\n"
	"The emulator watches for software hiding the mouse cursor. While the cursor is hidden, relative mouse mode is on. It toggles off as soon as the cursor is shown again.\n"
	"Some software can trigger relative mouse mode, even though the software itself does not require it. It is recommended that you use the Manual setting, if this is an issue.\n\n"
	"Can any software be run with relative mouse mode on?\n\n"
	"Yes.\n";
}

const char *SflBootCompatibilityText(void)
{
	return
	"When in doubt if your file is compatible, attempt bootstrapping. The emulator will search your Mac OS install disc file for a ROM it can use.\n\n"
	"Later on, when installing Mac OS onto your virtual hard drive, the used Mac OS install disc does not necessarily have to be the same as the one used to bootstrap.\n\n"
	"Compatible\n"
	"- Mac OS 8.1 bundled on iMac, Rev A\n"
	"- Mac OS 8.5 Retail CD, iMac Update 1.0\n"
	"- Mac OS 8.5 bundled on iMac, Rev B\n"
	"- Mac OS 8.5.1 bundled on Power Macintosh G3 (Blue and White), Macintosh Server G3 (Blue and White)\n"
	"- Mac OS 8.5.1 bundled on Colors iMac 266 MHz\n"
	"- Mac OS 8.6 Retail CD, bundled on Colors iMac 333 MHz, Power Macintosh G3 (Blue and White)\n"
	"- Mac OS 8.6 bundled on PowerBook G3 Series 8.6\n\n"
	"Not bootstrap compatible, install compatible\n"
	"- Mac OS 8.6 Power Mac G4 ROM 1.8.1 Update CD\n"
	"- Mac OS 8.6 bundled on iMac (Slot Loading), iBook\n"
	"- Mac OS 8.6 bundled on Power Mac G4 (AGP)\n"
	"- Mac OS 9.0 Retail CD, bundled on Power Macintosh G3 (Blue and White), iMac, PowerBook G3 Bronze\n"
	"- Mac OS 9.0 bundled on iBook, Power Mac G4 (AGP), iMac (Slot-Loading)\n"
	"- Mac OS 9.0Z bundled on Power Mac G4 (AGP)\n"
	"- Mac OS 9.0.2 bundled on Power Mac G4 (AGP), iBook, PowerBook (FireWire)\n"
	"- Mac OS 9.0.3 bundled on iMac (Slot Loading)\n"
	"- Mac OS 9.0.4 Retail/Software Update CD, bundled on PowerBook (FireWire) (Summer 2000)\n"
	"- Mac OS 9.0.4 Ethernet Update 1.0\n"
	"- Mac OS 9.0.4 Internal Edition\n"
	"- Mac OS 9.0.4 bundled on iMac (Summer 2000), Power Mac G4 (Summer 2000)\n"
	"- Mac OS 9.0.4 bundled on Power Mac G4 MP (Summer 2000), Power Mac G4 (Gigabit Ethernet)\n"
	"- Mac OS 9.0.4 bundled on Power Mac G4 Cube\n"
	"- Mac OS 9.0.4 bundled on iBook (Summer 2000)\n"
	"- Mac OS 9.0.4 International bundled on Power Mac G4 Cube\n\n"
	"Not compatible\n"
	"- Mac OS 8.0 and lower\n"
	"- Any Mac OS 8.1 install disc other than the one bundled on iMac, Rev A\n"
	"- Mac OS 8.6 bundled on Power Mac G4 (PCI)\n"
	"- Mac OS 9.1 and higher\n";
}

const char *SflBootInstructionsText(void)
{
	return
	"1. Bootstrap the emulator with a compatible Mac OS install disc file, or select a Macintosh ROM file.\n\n"
	"2. In Disks, select Create and make an empty disk (recommended minimum size is around 500 MB).\n\n"
	"3. Select Import and add a Mac OS install disc file, and make sure it is mounted. This does not have to be the same disk image as in step 1. But in general, it must be an OS version equal or higher to what you used in the bootstrapping process. The maximum Mac OS version supported is 9.0.4.\n\n"
	"4. Boot, let Mac OS format your empty virtual hard drive and launch the Mac OS Installer app from the disc.\n\n"
	"5. IMPORTANT: In the Mac OS Installer app, at the 'Install software' step (not the 'Select destination' step), click 'Options...', uncheck Update Apple Hard Disk Drivers and install Mac OS.\n\n"
	"6. After installation, restart the emulator, unmount the Mac OS installation CD and boot.\n\n"
	"7. Quit the Mac OS Setup Assistant app. The assistant cannot be completed without getting stuck.\n\n"
	"8. IMPORTANT: To get audio working, you have to explicitly select Built-in as the Sound out option in the Sound control panel (not Sound and monitors), which, depending on Mac OS version, is either located in (Mac HD) > System Folder > Control Panels or (Mac HD) > Apple Extras > Sound Control Panel. This only has to be done once.\n\n"
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
