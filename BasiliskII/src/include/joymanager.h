/*
 * joymanager.h - SDL-backed replacement for the classic 
 * .JoyManager driver. Note that this also contains utilities used
 * by other backends - ENABLE_JOYMANAGER only gates the .JoyManager
 * driver implementation.
 * 
 *	(C) 2026 Ryan Norton (battlemageloveryt@gmail.com)
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

#ifndef JOYMANAGER_H
#define JOYMANAGER_H

#ifndef ENABLE_JOYMANAGER
#define ENABLE_JOYMANAGER
#endif

#include "sysdeps.h"

const uint16 JoyManagerDriverFlags = 0x4c00;

/* high level API for client JoyManagerXXX() */
extern uint32 JoyManagerGuestStorageSize(void);
extern bool JoyManagerPrepare(void);
extern bool JoyManagerSetGuestStorage(uint32 addr, uint32 size);
extern void JoyManagerReset(void);
extern void JoyManagerVBL(void);
extern int16 JoyManagerOpen(uint32 pb, uint32 dce);
extern int16 JoyManagerControl(uint32 pb, uint32 dce);
extern int16 JoyManagerStatus(uint32 pb, uint32 dce);
extern int16 JoyManagerClose(uint32 pb, uint32 dce);
extern void JoyManagerSetDCE(uint32 dce);
extern void JoyManagerIntPoll(void);

/* low level API, typically for ADB joysticks */
typedef struct _SDL_Joystick JoyManagerDevice;
JoyManagerDevice *JoyManagerOpenDevice(int index);
void JoyManagerCloseDevice(JoyManagerDevice *joystick);
int JoyManagerNumDevices(void);
int JoyManagerNumButtons(JoyManagerDevice *joystick);
int JoyManagerNumAxes(JoyManagerDevice *joystick);
int JoyManagerNumHats(JoyManagerDevice *joystick);
bool JoyManagerInit(void);
int JoyManagerAxisLabel(int axis);
bool JoyManagerHasRudderThrottle(JoyManagerDevice *joystick);
bool JoyManagerDeviceAttached(JoyManagerDevice *joystick);
const char *JoyManagerDeviceName(JoyManagerDevice *joystick,
	int index);
int16 JoyManagerAxis(JoyManagerDevice *joystick, int axis);
int32 JoyManagerAxisRest(int32 v);
uint8 JoyManagerButton(JoyManagerDevice *joystick, int button);
uint8 JoyManagerHat(JoyManagerDevice *joystick, int hat);
void JoyManagerUpdate(void);
/* 	if (up && left) return 5;
	if (up && right) return 6;
	if (down && left) return 7;
	if (down && right) return 8;
	if (up) return 1;
	if (down) return 2;
	if (left) return 3;
	if (right) return 4; */
int JoyManagerHatPosition(uint8 hat);
#define JOY_TRACE 0
#endif
