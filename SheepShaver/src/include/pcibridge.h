/*
 *  pcibridge.h - MPC106 "Grackle" PCI host bridge
 *
 *  The Mac OS ROM in use identifies as iMac,1 / PowerMac1,1, both of which are
 *  Grackle machines, so its PCI configuration cycles are the CHRP pair:
 *  a 32-bit address latch at 0xFEC00000 and a data port at 0xFEE00000, both
 *  little-endian. Nothing above the bridge works without it - the OHCI UIM's
 *  very first act is ExpMgrConfigWriteWord(entry, 4, 6) to enable memory
 *  decode and bus mastering, and that is a config cycle.
 *
 *  Only the one function we emulate answers; every other bus/device/function
 *  reads back all-ones, which is how an empty slot presents itself.
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

#ifndef PCIBRIDGE_H
#define PCIBRIDGE_H

#include "sysdeps.h"

/* Guest physical addresses of the Grackle config ports. */
#ifndef PCI_CONFIG_ADDR
#define PCI_CONFIG_ADDR 0xFEC00000u
#endif
#ifndef PCI_CONFIG_DATA
#define PCI_CONFIG_DATA 0xFEE00000u
#endif

/* Bus 0, device 1, function 0 - the same function "assigned-addresses"
   and "reg" name in the Name Registry. */
enum {
	PCI_USB_DEV = 1,
	PCI_USB_FN = 0
};

extern void PCIBridgeInstall(void);
extern bool PCIBridgeReady(void);

#endif
