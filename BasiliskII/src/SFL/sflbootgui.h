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
int SflBootCreateFonts(WSFONT *heading, WSFONT *body);
const char *SflBootLicenseText(void);
const char *SflBootInstructionsText(void);
#ifdef __cplusplus
}
#endif
#endif
