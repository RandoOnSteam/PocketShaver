/*
 *  name_registry.cpp - Name Registry handling
 *
 *  SheepShaver (C) Christian Bauer and Marc Hellwig
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

#include <string.h>

#include "sysdeps.h"
#include "name_registry.h"
#include "main.h"
#include "macos_util.h"
#include "user_strings.h"
#include "emul_op.h"
#include "thunks.h"
#include "usbhid.h"
#include "pcibridge.h"
#include "usbuim.h"
#include "xlowmem.h"

#define DEBUG 0
#include "debug.h"

#define PCI_PUBLISH_BUS_NODE 0

/* Hand our own UIM to the guest. Off leaves a bare "usb" node that no driver
   binds to, which is the last state known to boot; flip it to tell a UIM
   problem apart from an unrelated boot problem. */
#define USB_PUBLISH_UIM 1

/* Publish the USB controller node at all. 0 is the pre-USB behaviour. */
#define USB_PUBLISH_NODE 1

/* Advertise device_type = "usb" on the node. This is what makes the ROM's
   USB Expert adopt the node as a bus; 0 leaves the hardware description
   published but invisible to the USB Family. */
#define USB_NODE_DEVICE_TYPE 1

/* Seconds after the first idle-loop call before the node appears. Kept short:
   anything that snapshots the machine's configuration when it launches - Apple
   System Profiler does - will not see USB if the bus turns up after it. */
#define USB_NODE_PUBLISH_DELAY 2

// Helper for RegEntryID
typedef SheepArray<sizeof(RegEntryID)> SheepRegEntryID;

#ifdef ENABLE_USB
static uint64 usb_first_idle;
static bool usb_node_published;
static SheepRegEntryID *pci_node;
static SheepRegEntryID *usb_node;
#endif /* ENABLE_USB */

// Function pointers
typedef int16 (*rcec_ptr)(const RegEntryID *, const char *, RegEntryID *);
static uint32 rcec_tvect = 0;
static inline int16 RegistryCStrEntryCreate(uintptr arg1, const char *arg2, uint32 arg3)
{
	SheepString arg2str(arg2);
	return (int16)CallMacOS3(rcec_ptr, rcec_tvect, (const RegEntryID *)arg1, arg2str.addr(), arg3);
}
typedef int16 (*rpc_ptr)(const RegEntryID *, const char *, const void *, uint32);
static uint32 rpc_tvect = 0;
static inline int16 RegistryPropertyCreate(uintptr arg1, const char *arg2, uintptr arg3, uint32 arg4)
{
	SheepString arg2str(arg2);
	return (int16)CallMacOS4(rpc_ptr, rpc_tvect, (const RegEntryID *)arg1, arg2str.addr(), (const void *)arg3, arg4);
}
static inline int16 RegistryPropertyCreateStr(uintptr arg1, const char *arg2, const char *arg3)
{
	SheepString arg3str(arg3);
	return RegistryPropertyCreate(arg1, arg2, arg3str.addr(), strlen(arg3) + 1);
}

// Video driver stub
static const uint8 video_driver[] = {
#include "VideoDriverStub.i"
};

// Ethernet driver stub
static const uint8 ethernet_driver[] = {
#ifdef USE_ETHER_FULL_DRIVER
#include "EthernetDriverFull.i"
#else
#include "EthernetDriverStub.i"
#endif
};



// Helper for a <uint32, uint32> pair
struct SheepPair : public SheepArray<8> {
	SheepPair(uint32 base, uint32 size) : SheepArray<8>()
		{ WriteMacInt32(addr(), base); WriteMacInt32(addr() + 4, size); }
};



/*
 *  Patch Name Registry during startup
 */
#ifdef ENABLE_USB
void DoPublishUSBNode(void)
{
	SheepVar32 u32;
	int16 err;
	if (usb_node == NULL)
		usb_node = new SheepRegEntryID;
	err = RegistryCStrEntryCreate(0, "Devices:device-tree:ohci-host",
		usb_node->addr());
	USBHIDLog("create ohci-host err=%d", err);
	if (!err) {
		if (USB_NODE_DEVICE_TYPE)
			RegistryPropertyCreateStr(usb_node->addr(), "device_type", "usb");
		// Apple System Profiler finds the bus by searching for exactly
		// that property - device_type = "usb", four bytes with the NUL
		// (its code 0x2bad0) - and then reads AAPL,BusNumber off the
		// node it found before walking its children as devices.
		u32.set_value(0);
		USBHIDLog("bus node device_type='usb' AAPL,BusNumber -> %d",
			RegistryPropertyCreate(usb_node->addr(), "AAPL,BusNumber",
				u32.addr(), 4));
		// Deliberately NOT "pciclass,0c0310": that is the ROM OHCIUIM's
		// match string, and it cannot run here. Our own UIM matches this
		// name and nothing else does, so no version race with the ROM.
		RegistryPropertyCreateStr(usb_node->addr(), "compatible", "SheepUSB");
		if (USB_PUBLISH_UIM) {
			uint32 drv_size;
			const uint8 *drv = USBUIMDriver(&drv_size);
			SheepArray<4096> the_uim;
			Host2Mac_memcpy(the_uim.addr(), drv, drv_size);
			RegistryPropertyCreate(usb_node->addr(),
				"driver,AAPL,MacOS,PowerPC", the_uim.addr(), drv_size);
		}
		USBHIDLog("ohci-host published, uim=%d", USB_PUBLISH_UIM);

		/* Bring the bus up. The node's RegEntryID goes with it: the
			Expert's LoadUIMForEntry keeps it as the parent for the
			per-device registry nodes it publishes. */
		if (USB_PUBLISH_UIM)
			USBUIMRegisterBus(usb_node->addr());
	}
}
void USBNodeResetPublish(void)
{
	usb_first_idle = 0;
	usb_node_published = false;
}

void USBNodePublishDeferred(void)
{
	if (usb_node_published) {
		static uint64 next_poll;
		uint64 now = GetTicks_usec();
		if (now < next_poll)
			return;
		next_poll = now + 16000;
		ExecuteNative(NATIVE_USB_UIM_POLL);
		return;
	}
	if (!USB_PUBLISH_NODE || !USBHIDReady())
		return;
	if (usb_first_idle == 0) {
		usb_first_idle = GetTicks_usec();
		USBHIDLog("first idle-loop call seen; publishing in %d s",
			USB_NODE_PUBLISH_DELAY);
		return;
	}
	if (GetTicks_usec() - usb_first_idle
			< (uint64)USB_NODE_PUBLISH_DELAY * 1000000)
		return;
	usb_node_published = true;
	USBHIDLog("publishing USB node from the idle loop");
	// Same reason PatchNameRegistry() does this: the Name Registry calls have
	// to run in PPC mode, and the idle hook is 68k context.
	ExecuteNative(NATIVE_USB_PUBLISH_NODE);
}
#endif /* ENABLE_USB */

void DoPatchNameRegistry(void)
{
	SheepVar32 u32;
	D(bug("Patching Name Registry..."));

	// Create "device-tree"
	SheepRegEntryID device_tree;
	if (!RegistryCStrEntryCreate(0, "Devices:device-tree", device_tree.addr())) {
		u32.set_value(BusClockSpeed);
		RegistryPropertyCreate(device_tree.addr(), "clock-frequency", u32.addr(), 4);
		RegistryPropertyCreateStr(device_tree.addr(), "model", "Power Macintosh");

		// Create "AAPL,ROM"
		SheepRegEntryID aapl_rom;
		if (!RegistryCStrEntryCreate(device_tree.addr(), "AAPL,ROM", aapl_rom.addr())) {
			RegistryPropertyCreateStr(aapl_rom.addr(), "device_type", "rom");
			SheepPair reg(ROMBase, ROM_SIZE);
			RegistryPropertyCreate(aapl_rom.addr(), "reg", reg.addr(), 8);
		}

		// Create "PowerPC,60x"
		SheepRegEntryID power_pc;
		const char *str;
		switch (PVR >> 16) {
			case 1:		// 601
				str = "PowerPC,601";
				break;
			case 3:		// 603
				str = "PowerPC,603";
				break;
			case 4:		// 604
				str = "PowerPC,604";
				break;
			case 6:		// 603e
				str = "PowerPC,603e";
				break;
			case 7:		// 603ev
				str = "PowerPC,603ev";
				break;
			case 8:		// 750
				str = "PowerPC,750";
				break;
			case 9:		// 604e
				str = "PowerPC,604e";
				break;
			case 10:	// 604ev5
				str = "PowerPC,604ev";
				break;
			case 50:	// 821
				str = "PowerPC,821";
				break;
			case 80:	// 860
				str = "PowerPC,860";
				break;
			case 12:	// 7400, 7410, 7450, 7455, 7457
			case 0x800c:
			case 0x8000:
			case 0x8001:
			case 0x8002:
				str = "PowerPC,G4";
				break;
			default:
				str = "PowerPC,???";
				break;
		}
		if (!RegistryCStrEntryCreate(device_tree.addr(), str, power_pc.addr())) {
			u32.set_value(CPUClockSpeed);
			RegistryPropertyCreate(power_pc.addr(), "clock-frequency", u32.addr(), 4);
			u32.set_value(BusClockSpeed);
			RegistryPropertyCreate(power_pc.addr(), "bus-frequency", u32.addr(), 4);
			u32.set_value(TimebaseSpeed);
			RegistryPropertyCreate(power_pc.addr(), "timebase-frequency", u32.addr(), 4);
			u32.set_value(PVR);
			RegistryPropertyCreate(power_pc.addr(), "cpu-version", u32.addr(), 4);
			RegistryPropertyCreateStr(power_pc.addr(), "device_type", "cpu");
			switch (PVR >> 16) {
				case 1:		// 601
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 3:		// 603
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x2000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x2000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 4:		// 604
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x4000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x4000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 6:		// 603e
				case 7:		// 603ev
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x4000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x4000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 8:		// 750, 750FX
				case 0x7000:
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 9:		// 604e
				case 10:	// 604ev5
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 12:	// 7400, 7410, 7450, 7455, 7457
				case 0x800c:
				case 0x8000:
				case 0x8001:
				case 0x8002:
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(32);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(64);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				case 0x39:	// 970
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-block-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-sets", u32.addr(), 4);
					u32.set_value(0x8000);
					RegistryPropertyCreate(power_pc.addr(), "d-cache-size", u32.addr(), 4);
					u32.set_value(128);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-block-size", u32.addr(), 4);
					u32.set_value(512);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-sets", u32.addr(), 4);
					u32.set_value(0x10000);
					RegistryPropertyCreate(power_pc.addr(), "i-cache-size", u32.addr(), 4);
					u32.set_value(256);
					RegistryPropertyCreate(power_pc.addr(), "tlb-sets", u32.addr(), 4);
					u32.set_value(0x1000);
					RegistryPropertyCreate(power_pc.addr(), "tlb-size", u32.addr(), 4);
					break;
				default:
					break;
			}
			u32.set_value(32);
			RegistryPropertyCreate(power_pc.addr(), "reservation-granularity", u32.addr(), 4);
			SheepPair reg(0, 0);
			RegistryPropertyCreate(power_pc.addr(), "reg", reg.addr(), 8);
		}

		// Create "memory"
		SheepRegEntryID memory;
		if (!RegistryCStrEntryCreate(device_tree.addr(), "memory", memory.addr())) {
			SheepPair reg(RAMBase, RAMSize);
			RegistryPropertyCreateStr(memory.addr(), "device_type", "memory");
			RegistryPropertyCreate(memory.addr(), "reg", reg.addr(), 8);
		}

		// Create "video"
		SheepRegEntryID video;
		if (!RegistryCStrEntryCreate(device_tree.addr(), "video", video.addr())) {
			RegistryPropertyCreateStr(video.addr(), "AAPL,connector", "monitor");
			RegistryPropertyCreateStr(video.addr(), "device_type", "display");
			SheepArray<sizeof(video_driver)> the_video_driver;
			Host2Mac_memcpy(the_video_driver.addr(), video_driver, sizeof(video_driver));
			RegistryPropertyCreate(video.addr(), "driver,AAPL,MacOS,PowerPC", the_video_driver.addr(), sizeof(video_driver));
			RegistryPropertyCreateStr(video.addr(), "model", "SheepShaver Video");
		}

		// Create "ethernet"
		SheepRegEntryID ethernet;
		if (!RegistryCStrEntryCreate(device_tree.addr(), "ethernet", ethernet.addr())) {
			RegistryPropertyCreateStr(ethernet.addr(), "AAPL,connector", "ethernet");
			RegistryPropertyCreateStr(ethernet.addr(), "device_type", "network");
			SheepArray<sizeof(ethernet_driver)> the_ethernet_driver;
			Host2Mac_memcpy(the_ethernet_driver.addr(), ethernet_driver, sizeof(ethernet_driver));
			RegistryPropertyCreate(ethernet.addr(), "driver,AAPL,MacOS,PowerPC", the_ethernet_driver.addr(), sizeof(ethernet_driver));
			// local-mac-address
			// max-frame-size 2048
		}
#ifdef ENABLE_USB
		USBHIDInstall();
		if (false) {	// the USB node is published later, see PublishUSBNode()
			uintptr usb_parent = device_tree.addr();
			if (pci_node == NULL)
				pci_node = new SheepRegEntryID;
			if (usb_node == NULL)
				usb_node = new SheepRegEntryID;

			// Create "pci" - the host bridge the USB controller hangs off.
			if (PCI_PUBLISH_BUS_NODE)
				PCIBridgeInstall();
			if (PCI_PUBLISH_BUS_NODE
				&& !RegistryCStrEntryCreate(device_tree.addr(), "pci", pci_node->addr())) {
				RegistryPropertyCreateStr(pci_node->addr(), "device_type", "pci");
				RegistryPropertyCreateStr(pci_node->addr(), "compatible", "grackle");
				SheepPair bus_range(0, 0);	// bus 0, and the only one
				RegistryPropertyCreate(pci_node->addr(), "bus-range", bus_range.addr(), 8);
				SheepPair bridge_reg(PCI_CONFIG_ADDR, PCI_CONFIG_DATA - PCI_CONFIG_ADDR);
				RegistryPropertyCreate(pci_node->addr(), "reg", bridge_reg.addr(), 8);
				SheepPair cfg_ports(PCI_CONFIG_ADDR, PCI_CONFIG_DATA);
				RegistryPropertyCreate(pci_node->addr(), "AAPL,address", cfg_ports.addr(), 8);
				u32.set_value(3);
				RegistryPropertyCreate(pci_node->addr(), "#address-cells", u32.addr(), 4);
				u32.set_value(2);
				RegistryPropertyCreate(pci_node->addr(), "#size-cells", u32.addr(), 4);
				usb_parent = pci_node->addr();
				USBHIDLog("pci bridge node published");
			}

		}
#endif /* ENABLE_USB */
	}
	D(bug("done.\n"));
}

void PatchNameRegistry(void)
{
	// Find RegistryCStrEntryCreate() and RegistryPropertyCreate() TVECTs
	rcec_tvect = FindLibSymbol("\017NameRegistryLib", "\027RegistryCStrEntryCreate");
	D(bug("RegistryCStrEntryCreate TVECT at %08x\n", rcec_tvect));
	rpc_tvect = FindLibSymbol("\017NameRegistryLib", "\026RegistryPropertyCreate");
	D(bug("RegistryPropertyCreate TVECT at %08x\n", rpc_tvect));
	if (rcec_tvect == 0 || rpc_tvect == 0) {
		ErrorAlert(GetString(STR_NO_NAME_REGISTRY_ERR));
		QuitEmulator();
	}

	// Main routine must be executed in PPC mode
	ExecuteNative(NATIVE_PATCH_NAME_REGISTRY);
}

