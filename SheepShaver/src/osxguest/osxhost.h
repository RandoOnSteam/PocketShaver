/*
 *  osxhost.h - host video, audio and clipboard interface for the Mac OS X guest
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
#ifndef OSXHOST_H
#define OSXHOST_H

#define OSXHOSTQUIT 1
#define OSXHOSTMOUSE 2
#define OSXHOSTBUTTON 3
#define OSXHOSTWHEEL 4
#define OSXHOSTKEY 5
#define OSXHOSTREDRAW 6
#define OSXHOSTRUNS 16

struct OsxHostVideo;
typedef void (*OsxHostInput)(void *context, int operation, int first, int second);

OsxHostVideo *OsxGuestHostOpen(int width, int height, OsxHostInput input, void *context);
void OsxGuestHostClose(OsxHostVideo *video);
void OsxGuestHostSuspendFrame(OsxHostVideo *video);
uint8 *OsxGuestHostLock(OsxHostVideo *video, int *pitch, int *raw);
int OsxGuestHostPresent(OsxHostVideo *video, const uint32 *runstart, const uint32 *runend, int runs);
void OsxGuestHostEvents(OsxHostVideo *video, const uint8 *vram);
void OsxGuestHostSleep(uint32 milliseconds);

struct OsxAudio;
void OsxGuestAudioLockHost(OsxAudio *audio);
void OsxGuestAudioUnlockHost(OsxAudio *audio);
void OsxGuestAudioCloseHost(OsxAudio *audio);
void *OsxGuestAudioStartHost(OsxAudio *audio);
char *OsxGuestHostClipboardGet(void);
void OsxGuestHostClipboardSet(const char *text);

void *OsxGuestHostNetOpen(const uint8 *address);
void OsxGuestHostNetClose(void *network);
int OsxGuestHostNetReady(void *network);
void OsxGuestHostNetPoll(void *network);
int OsxGuestHostNetReceive(void *network, uint8 *packet);
void OsxGuestHostNetSend(void *network, const uint8 *packet, int length);

#endif
