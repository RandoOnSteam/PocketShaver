/*
 *  mmio.cpp - Memory-mapped device windows in guest physical space
 *
 *  See mmio.h. Windows are few and fixed (one per emulated device), so the
 *  lookup is a linear scan over a small table; it only ever runs from the
 *  fault path, never from a normal guest access.
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

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "mmio.h"
#include "vm_alloc.h"

#define DEBUG 0
#include "debug.h"

enum {
	MMIO_MAX_WINDOWS = 4,
	// Protection is per page; reservation is per allocation granule. Both
	// matter - see MMIOMapWindow.
	MMIO_PAGE = 0x1000,
	MMIO_GRANULARITY = 0x10000
};

// Servicing a device access means decoding the faulting host instruction,
// which only exists for x86. Elsewhere no window can be mapped and the
// devices simply do not publish themselves, exactly as before they existed.
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#define MMIO_SUPPORTED 1
#else
#define MMIO_SUPPORTED 0
#endif

struct mmio_window {
	uint8 *host;
	uint32 size;
	mmio_read_proc read_proc;
	mmio_write_proc write_proc;
};

static mmio_window mmio_windows[MMIO_MAX_WINDOWS];
static int mmio_window_count;

static mmio_window *mmio_find(void *host_addr, uint32 *offset)
{
	int i;

	for (i = 0; i < mmio_window_count; i++) {
		uintptr off = (uintptr)host_addr - (uintptr)mmio_windows[i].host;
		if (off < mmio_windows[i].size) {
			*offset = (uint32)off;
			return &mmio_windows[i];
		}
	}
	return NULL;
}

bool MMIOMapWindow(uint32 base, uint32 size,
	mmio_read_proc read_proc, mmio_write_proc write_proc)
{
	void *host;
	uint32 granule_base, granule_size;

	if (!MMIO_SUPPORTED)
		return false;
	if (mmio_window_count >= MMIO_MAX_WINDOWS)
		return false;

	// Trapping is per page, but the host only *reserves* in allocation
	// granules. Those are different sizes, and the difference matters: a
	// device sitting in a shared granule (the SCC lives in the same 64 KB of
	// mac-io as the VIA) must not make its neighbours inaccessible. Reserve
	// the whole granule read/write - which is what those addresses were
	// before - and take access away from this device's pages only.
	if (base & (MMIO_PAGE - 1))
		return false;
	size = (size + MMIO_PAGE - 1) & ~(uint32)(MMIO_PAGE - 1);

	granule_base = base & ~(uint32)(MMIO_GRANULARITY - 1);
	granule_size = ((base + size + MMIO_GRANULARITY - 1)
		& ~(uint32)(MMIO_GRANULARITY - 1)) - granule_base;

	// Claiming the granule also keeps the Win32 lazy-backing path in
	// sigsegv.cpp away: it declines anything already MEM_COMMIT. A second
	// device in the same granule finds it mapped already, which is not an
	// error - only the protect below has to succeed.
	vm_acquire_fixed(Mac2HostAddr(granule_base), granule_size);

	host = Mac2HostAddr(base);
	if (vm_protect(host, size, VM_PAGE_NOACCESS) < 0)
		return false;

	mmio_windows[mmio_window_count].host = (uint8 *)host;
	mmio_windows[mmio_window_count].size = size;
	mmio_windows[mmio_window_count].read_proc = read_proc;
	mmio_windows[mmio_window_count].write_proc = write_proc;
	mmio_window_count++;
	return true;
}

bool MMIOIsWindow(void *host_addr)
{
	uint32 offset;

	return mmio_find(host_addr, &offset) != NULL;
}

uint32 MMIORead(void *host_addr, int size)
{
	uint32 offset;
	mmio_window *w = mmio_find(host_addr, &offset);

	if (w == NULL)
		return 0;
	return w->read_proc(offset, size);
}

void MMIOWrite(void *host_addr, int size, uint32 value)
{
	uint32 offset;
	mmio_window *w = mmio_find(host_addr, &offset);

	if (w != NULL)
		w->write_proc(offset, size, value);
}
