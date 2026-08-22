/*
 *  usbhid.h - OHCI USB host controller + HID joystick
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

#ifndef USBHID_H
#define USBHID_H

#include "sysdeps.h"

/* Master switch for everything USB: the software UIM in usbuim.cpp, the HID
   joystick, the OHCI register window and the Name Registry node. */
#ifndef ENABLE_USB
#endif

/* Guest physical address of the OHCI register page. Above SheepMem
   (0x51000000) and below the alternate kernel window (0x5FFFE000). */
#ifndef USBHID_BAR
#define USBHID_BAR 0x5E000000u
#endif
#ifndef USBHID_BAR_SIZE
#define USBHID_BAR_SIZE 0x1000u
#endif

extern void USBHIDInstall(void);
extern void USBHIDReset(void);
extern void USBHIDVBL(void);
extern bool USBHIDReady(void);
extern uint32 USBHIDRegBase(void);
extern void USBHIDLog(const char *fmt, ...);

// The HID joystick device, independent of how it is presented. Both the OHCI
// register file and the software UIM in usbuim.cpp drive the same device.
// USBHIDDeviceControl answers one device request: it returns 0 and sets *len to
// the number of bytes written to data, or -1 to stall. USBHIDDeviceInterruptIn
// fills data with the current report and returns its length.
extern int USBHIDDeviceControl(const uint8 setup[8], uint8 *data, int *len,
	int dir_in);
extern int USBHIDDeviceInterruptIn(uint8 *data, int max);

#endif
