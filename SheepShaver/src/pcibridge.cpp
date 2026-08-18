/*
 *  pcibridge.cpp - MPC106 "Grackle" PCI host bridge
 *
 *  See pcibridge.h. Both ports are trapping MMIO windows (mmio.h), so the
 *  address latch and the data port behave like registers rather than RAM -
 *  which they must, since a config cycle is a write to one followed by an
 *  access to the other.
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
#include "pcibridge.h"
#include "usbhid.h"
#include "mmio.h"

#include <string.h>

#define DEBUG 0
#include "debug.h"

enum {
	PCI_CFG_ENABLE = 0x80000000u
};

/* Configuration space of the emulated OHCI function. Only the fields a
   driver actually reads are modelled; the rest read as zero. */
enum {
	PCI_VENDOR_ID = 0x1033,		/* NEC */
	PCI_DEVICE_ID = 0x0035,		/* uPD720100 OHCI */
	PCI_REVISION = 0x10,
	PCI_CLASS_CODE = 0x0c0310,	/* serial bus / USB / OHCI */
	PCI_BAR_MASK = ~(USBHID_BAR_SIZE - 1)
};

static uint32 pci_addr_latch;
static uint16 pci_command;
static uint16 pci_status = 0x0210;	/* devsel timing medium, 66 MHz capable */
static uint32 pci_bar0 = USBHID_BAR;
static uint8 pci_int_line = 0xff;
static uint8 pci_latency;
static uint8 pci_cacheline;
static bool pci_installed;

static bool pci_addr_is_ours(uint32 latch)
{
	uint32 bus, dev, fn;

	if (!(latch & PCI_CFG_ENABLE))
		return false;
	bus = (latch >> 16) & 0xff;
	dev = (latch >> 11) & 0x1f;
	fn = (latch >> 8) & 0x07;
	return bus == 0 && dev == PCI_USB_DEV && fn == PCI_USB_FN;
}

static uint32 pci_config_read(uint32 reg)
{
	switch (reg & 0xfc) {
	case 0x00: return ((uint32)PCI_DEVICE_ID << 16) | PCI_VENDOR_ID;
	case 0x04: return ((uint32)pci_status << 16) | pci_command;
	case 0x08: return ((uint32)PCI_CLASS_CODE << 8) | PCI_REVISION;
	case 0x0c: return ((uint32)pci_latency << 8) | pci_cacheline;
	case 0x10: return pci_bar0;			/* BAR0: 32-bit memory */
	case 0x2c: return ((uint32)PCI_DEVICE_ID << 16) | PCI_VENDOR_ID;
	case 0x3c: return ((uint32)1 << 8) | pci_int_line;	/* INTA# */
	default: return 0;
	}
}

static void pci_config_write(uint32 reg, uint32 val, uint32 byte_mask)
{
	uint32 old;

	switch (reg & 0xfc) {
	case 0x04:
		old = pci_command;
		pci_command = (uint16)((old & ~byte_mask) | (val & byte_mask));
		USBHIDLog("PCI command %04x -> %04x%s%s", old, pci_command,
			(pci_command & 2) ? " MEM" : "", (pci_command & 4) ? " MASTER" : "");
		break;
	case 0x0c:
		if (byte_mask & 0x000000ff)
			pci_cacheline = (uint8)val;
		if (byte_mask & 0x0000ff00)
			pci_latency = (uint8)(val >> 8);
		break;
	case 0x10:
		/* Size probe: all-ones in, size mask back. Anything else re-bases the
		   aperture, which we do not support - the window is fixed. */
		if ((val | ~byte_mask) == 0xffffffffu)
			pci_bar0 = PCI_BAR_MASK;
		else
			pci_bar0 = USBHID_BAR;
		break;
	case 0x3c:
		if (byte_mask & 0x000000ff)
			pci_int_line = (uint8)val;
		break;
	default:
		break;
	}
}

/*
 *  The two ports. Values are little-endian on the wire, which is what the
 *  mmio layer hands us, so no swapping happens anywhere in here.
 */

static uint32 pci_addr_read(uint32 off, int size)
{
	uint32 v = pci_addr_latch;

	if (off & 3)
		v >>= (off & 3) * 8;
	if (size < 4)
		v &= (1u << (size * 8)) - 1;
	return v;
}

static void pci_addr_write(uint32 off, int size, uint32 val)
{
	if (size == 4 && (off & 3) == 0)
		pci_addr_latch = val;
	else {
		int shift = (int)(off & 3) * 8;
		uint32 mask = ((1u << (size * 8)) - 1) << shift;
		pci_addr_latch = (pci_addr_latch & ~mask) | ((val << shift) & mask);
	}
}

static uint32 pci_data_read(uint32 off, int size)
{
	uint32 reg = pci_addr_latch & 0xfc;
	uint32 v;

	/* The byte lanes within the dword are selected by the low address bits
	   of the data port, exactly as on the real bridge. */
	if (!pci_addr_is_ours(pci_addr_latch))
		v = 0xffffffffu;		/* no device answers this cycle */
	else
		v = pci_config_read(reg);

	v >>= (off & 3) * 8;
	if (size < 4)
		v &= (1u << (size * 8)) - 1;
	USBHIDLog("cfg rd %08x reg=%02x.%d = %08x", pci_addr_latch, reg + (off & 3),
		size, v);
	return v;
}

static void pci_data_write(uint32 off, int size, uint32 val)
{
	uint32 reg = pci_addr_latch & 0xfc;
	int shift = (int)(off & 3) * 8;
	uint32 mask = size >= 4 ? 0xffffffffu : (((1u << (size * 8)) - 1) << shift);

	USBHIDLog("cfg wr %08x reg=%02x.%d = %08x", pci_addr_latch, reg + (off & 3),
		size, val);
	if (!pci_addr_is_ours(pci_addr_latch))
		return;
	pci_config_write(reg, val << shift, mask);
}

void PCIBridgeInstall(void)
{
	if (pci_installed)
		return;
	if (!MMIOMapWindow(PCI_CONFIG_ADDR, 0x1000, pci_addr_read, pci_addr_write)
		|| !MMIOMapWindow(PCI_CONFIG_DATA, 0x1000, pci_data_read, pci_data_write)) {
		USBHIDLog("PCI bridge map FAILED (%08x/%08x)",
			PCI_CONFIG_ADDR, PCI_CONFIG_DATA);
		return;
	}
	pci_installed = true;
	USBHIDLog("PCI bridge: config addr %08x data %08x, OHCI at 0:%d.%d",
		PCI_CONFIG_ADDR, PCI_CONFIG_DATA, PCI_USB_DEV, PCI_USB_FN);
}

bool PCIBridgeReady(void)
{
	return pci_installed;
}
