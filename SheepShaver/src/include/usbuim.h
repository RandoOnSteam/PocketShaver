/*
 *  usbuim.h - SheepShaver's own USB Universal Interface Module
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

#ifndef USBUIM_H
#define USBUIM_H

#include "sysdeps.h"

enum {
	USB_UIM_SLOTS = 25
};

// Called from the NATIVE_USB_UIM_DISPATCH trampoline. slot is the plugin
// table index the guest called; r3..r10 are that entry's arguments.
extern uint32 USBUIMDispatch(uint32 slot, uint32 table, uint32 caller,
	uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8,
	uint32 r9, uint32 r10);

// Front end for the USB Expert's notification routine: logs the record the
// USB Services Lib built and chains to the Expert. Returns its result.
extern uint32 USBUIMExpertNotify(uint32 record);

// Complete the root hub's outstanding status-change transfer. Call after
// changing the downstream port's state; does nothing if the hub driver has no
// transfer pending.
extern void USBUIMPortChanged(void);

// Called once per VBL. Dumps the USB Expert's own status log a few seconds
// after it first touches us, which is the only way to see why it stalls.
extern void USBUIMPoll(void);

// Called from the host tick thread: samples where the guest is executing,
// which is the only visibility left once it stops taking interrupts.
extern void USBUIMSampleGuest(void);

// Forget every guest pointer and queue. Called from MacOSUtilReset: after a
// guest reset the USB family's globals are gone, and calling the TVectors we
// resolved before it faults inside the ROM.
extern void USBUIMReset(void);

// Guest-visible driver image for the Name Registry node.
extern const uint8 *USBUIMDriver(uint32 *size);

// Prepare our UIM fragment with CFM and hand its dispatch table to
// USBAddBus. Must run in PPC mode.
// node is the RegEntryID of the published controller node; the Expert's
// LoadUIMForEntry needs it, and keeps it as the parent for device nodes.
extern void USBUIMRegisterBus(uint32 node);

#endif
