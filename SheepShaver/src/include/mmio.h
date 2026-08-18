/*
 *  mmio.h - Memory-mapped device windows in guest physical space
 *
 *  A window's pages are committed but PAGE_NOACCESS, so every guest load and
 *  store into one faults into sigsegv_handler() and is serviced by the owning
 *  device with real register semantics: read side effects, write-1-to-clear
 *  and the access width all survive. Backing a device with ordinary RAM and
 *  diffing it later cannot express any of those.
 *
 *  Nothing is added to the emulator's memory fast path: an address outside
 *  every window never faults, so it never reaches this code.
 *
 *  Offsets and values are little-endian - exactly what a host access of that
 *  width at that offset would move. A PowerPC driver that byte-swaps
 *  (EndianSwap32Bit) therefore sees its registers the right way round, which
 *  is how the Mac OS ROM's OHCI UIM talks to a PCI register file.
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

#ifndef MMIO_H
#define MMIO_H

#include "sysdeps.h"

typedef uint32 (*mmio_read_proc)(uint32 offset, int size);
typedef void (*mmio_write_proc)(uint32 offset, int size, uint32 value);

// Reserve base..base+size as a trapping window. Returns false if the guest
// range could not be claimed, in which case the device must not be published.
extern bool MMIOMapWindow(uint32 base, uint32 size,
	mmio_read_proc read_proc, mmio_write_proc write_proc);

// Fault-path entry points. These take the faulting *host* address, which is
// what a fault reports, and match it against the mapping directly - no
// guest-address round trip, so a wild host pointer cannot alias a window.
// MMIOIsWindow() answers from the fault handler; the other two perform the
// access the faulting instruction was making.
extern bool MMIOIsWindow(void *host_addr);
extern uint32 MMIORead(void *host_addr, int size);
extern void MMIOWrite(void *host_addr, int size, uint32 value);

#endif
