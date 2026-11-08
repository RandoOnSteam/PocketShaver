/*
 *  sflbootgui.h - Original boot configuration window
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

#ifndef SFLBOOTGUI_H
#define SFLBOOTGUI_H
#ifdef __cplusplus
extern "C" {
#endif
int SflBootGuiRun(WSAPP *app);
int SflBootPrefInt(const char *name);
void SflBootSetString(const char *name, const char *value, int index);
void SflBootSetFirstString(const char *name, const char *value);
void SflBootSetBool(const char *name, int value);
void SflBootSetInt(const char *name, int value);
void SflBootRemove(const char *name);
void SflBootSpecialValue(const char *name, char *value, size_t valuesize);
void SflBootSetSpecial(const char *name, const char *value);
int SflBootIsSheepShaver(void);
int SflBootIsNativeBoot(void);
int SflBootSave(void);
int SflBootValidateRedir(const char *text);
int SflBootCreateFonts(WSFONT *heading, WSFONT *body, int dpi, int basedpi);
void SflBootShareFolder(char *directory, size_t size);
void SflBootOpenShareFolder(void);
int SflBootExtractRom(const char *disc, char *destination, size_t size);
const char *SflBootDiskExtensions(void);
const char *SflBootCdExtensions(void);
const char *SflBootBootstrapText(void);
const char *SflBootCompatibilityText(void);
const char *SflBootRelativeMouseText(void);
const char *SflBootLicenseText(void);
const char *SflBootInstructionsText(void);
#ifdef __cplusplus
}
#endif
#endif
