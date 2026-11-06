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

OsxHostVideo *OsxHostOpen(int width, int height, OsxHostInput input, void *context);
void OsxHostClose(OsxHostVideo *video);
uint8 *OsxHostLock(OsxHostVideo *video, int *pitch, int *raw);
int OsxHostPresent(OsxHostVideo *video, const uint32 *runstart, const uint32 *runend, int runs);
void OsxHostEvents(OsxHostVideo *video, const uint8 *vram);
void OsxHostSleep(uint32 milliseconds);

struct OsxAudio;
void OsxAudioLockHost(OsxAudio *audio);
void OsxAudioUnlockHost(OsxAudio *audio);
void OsxAudioCloseHost(OsxAudio *audio);
void *OsxAudioStartHost(OsxAudio *audio);
char *OsxHostClipboardGet(void);
void OsxHostClipboardSet(const char *text);

void *OsxHostNetOpen(const uint8 *address);
void OsxHostNetClose(void *network);
int OsxHostNetReady(void *network);
void OsxHostNetPoll(void *network);
int OsxHostNetReceive(void *network, uint8 *packet);
void OsxHostNetSend(void *network, const uint8 *packet, int length);

#endif
