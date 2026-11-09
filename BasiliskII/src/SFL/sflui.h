/*
 *  sflui.h - Window, input and sound interface of the SFL platform layer
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

#ifndef SFLUI_H
#define SFLUI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SFLAUDIOFILL)(void *context, unsigned char *buffer, int bytes);

int SflUiOpen(const char *title, int width, int height, int fullscreen, int frameskip);
void SflUiQuitFullScreen(void);
void SflUiGetDesktopSize(int *width, int *height);
void SflUiClose(void);
void *SflUiGetWindowHandle(void);
void SflUiSetFrame(unsigned char *buffer, int width, int height, int rowbytes, int bits);
void SflUiSetPalette(const unsigned char *rgb, int count);
void SflUiAlert(const char *title, const char *text);
void SflUiRequestQuit(void);
void SflUiPumpEvents(void);
#define SFLUIINPUTMOUSE 1
#define SFLUIINPUTBUTTON 2
#define SFLUIINPUTKEY 3
#define SFLUIINPUTQUIT 4
#define SFLUIINPUTWHEEL 5
typedef void (*SFLUIINPUT)(void *context, int operation, int first, int second);
void SflUiSetInput(SFLUIINPUT input, void *context);
int SflUiQueueInput(int operation, int first, int second);
int SflUiIsQuitting(void);
void SflUiNetworkTransfer(const unsigned char *frame, int length, int incoming);
void SflUiNotify(const char *text);
#define SFLRELATIVEUPDATE 0
#define SFLRELATIVETOGGLE 1
#define SFLRELATIVESTART 2
int SflEmulatorRelativeMouse(int operation);
int SflUiPresent(int firstrow, int lastrow);
int SflUiGetStatus(int *width, int *height, int *depth, int *hostwidth, int *hostheight);
unsigned char *SflUiCopyFrame(int host, int *width, int *height);
void SflUiHostMouse(int x, int y);
void SflUiHostButton(int down);
int SflUiClipboardGet(char **text, size_t *length);
void SflUiClipboardSet(const char *text, size_t length);

void *SflAudioOpen(int rate, int channels, int bits, int blockbytes, SFLAUDIOFILL fill, void *context);
void SflAudioClose(void *audio);
void *SflAudioCaptureOpen(int rate, int channels, int bits, int blockbytes, int ringbytes);
int SflAudioCaptureRead(void *capture, unsigned char *buffer, int bytes);
void SflAudioCaptureClear(void *capture);
void SflAudioCaptureClose(void *capture);

void SflEmulatorLoadPrefs(int argc, char **argv);
int SflEmulatorRomValid(const char *path);
int SflEmulatorSettingsRequested(void);
int SflEmulatorSetup(int argc, char **argv);
void SflEmulatorStart(void);
void SflEmulatorStop(void);
void SflEmulatorTick(void);
void SflEmulatorKey(int code, int down);
void SflEmulatorMouse(int x, int y);
void SflEmulatorButton(int down);
void SflEmulatorMouseButton(int button, int down);
void SflEmulatorWheel(int notches);
const char *SflEmulatorExtFSName(void);
const char *SflEmulatorPrefString(const char *name, int index);
int SflEmulatorPrefBool(const char *name);
void SflEmulatorClose(void);

#ifdef __cplusplus
}
#endif

#endif
