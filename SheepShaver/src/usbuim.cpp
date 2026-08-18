/*
 *  usbuim.cpp - SheepShaver's own USB Universal Interface Module
 *
 *  See usbuim.h. The dispatch table is Apple's UIM plug-in interface, which the
 *  slot layout and the argument shapes at the call sites both confirm: the USB
 *  Services Lib fetches slot n from table offset 4 + n*4, and reaches slot 2
 *  with (byte, byte, halfword, byte) - functionAddress, endpoint,
 *  maxPacketSize, speed - which is UIMCreateControlEndpoint.
 *
 *      slot  ROM UIM code   entry
 *      ----  ------------   -----
 *        0     0x000dd8     UIMInitialize
 *        1     0x00115c     UIMFinalize
 *        2     0x0013d0     UIMCreateControlEndpoint
 *        3     0x0015cc     UIMCreateControlTransfer
 *        6     0x0017bc     UIMCreateBulkEndpoint
 *        7     0x001844     UIMCreateBulkTransfer
 *       10     0x001e58     UIMCreateInterruptEndpoint
 *       11     0x002038     UIMCreateInterruptTransfer
 *       14     0x0021f4     UIMCreateIsochEndpoint
 *       15     0x00247c     UIMCreateIsochTransfer
 *       18     0x003154     abort endpoint
 *       19     0x002fc4     delete endpoint
 *       20-24  0x0037ac ..  root hub and endpoint maintenance
 *
 *  Slots 4,5, 8,9, 12,13 and 16,17 are NULL in the ROM UIM as well - each
 *  transfer type has four table entries of which only two are implemented.
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
#include "macos_util.h"
#include "thunks.h"
#include "usbuim.h"
#include "usbhid.h"
#include "xlowmem.h"

#include <stdio.h>
#include <string.h>

#define DEBUG 0
#include "debug.h"

/* The registry / USB Manager dumps in USBUIMPoll and the guest-PC sampler.
   They cost real guest time, so they are compiled out; set to 1 to bring back
   "profiler view", the AAPL,USBNodeType search and USBGetNextDeviceByClass. */
#define USB_UIM_DIAGNOSTICS 0

/* The whole UIM is behind the same switch as the rest of USB - see ENABLE_USB
   in usbhid.h. With it off, the no-op entry points at the end of the file
   stand in, so name_registry.cpp and macos_util.cpp need no guards. */
#ifdef ENABLE_USB

static const uint8 usb_uim_driver[] = {
#include "USBUIMStub.i"
};

const uint8 *USBUIMDriver(uint32 *size)
{
	*size = sizeof(usb_uim_driver);
	return usb_uim_driver;
}

/*
 *  The Expert keeps a running status log and says, in words, why it gave up on
 *  a bus. Reading it beats guessing from disassembly. The fragment names in the
 *  ROM sit one entry ahead of their container, so the library that exports
 *  gUSBStatusBuffer - the one with INIT_USBExpert and LoadUIMForEntry - is
 *  named USBFamilyExpertLib.
 */
static int uim_seen_init;
static uint32 uim_status_buf;

/* Resolve in guest context; FindLibSymbol executes guest code and therefore
   must not be called later from the host tick sampler. */
static void USBUIMFindExpertStatus(void)
{
	uim_status_buf = FindLibSymbol("\022USBFamilyExpertLib",
		"\020gUSBStatusBuffer");
	USBHIDLog("gUSBStatusBuffer = %08x", uim_status_buf);
}

/* Contents already reported, so a dump only speaks up when something changed.
   Counting lines instead was wrong: the buffer is followed by unrelated ROM
   text, which inflated the count and then suppressed later real messages. */
enum { USB_STATUS_WINDOW = 0x300 };
static char uim_status_seen[USB_STATUS_WINDOW];

static void USBUIMDumpExpertStatus(void)
{
	uint32 buf = uim_status_buf;
	char now[USB_STATUS_WINDOW];
	char line[160];
	int n = 0;
	int i;

	if (buf == 0)
		return;
	for (i = 0; i < USB_STATUS_WINDOW; i++)
		now[i] = (char)ReadMacInt8(buf + i);
	if (memcmp(now, uim_status_seen, USB_STATUS_WINDOW) == 0)
		return;
	memcpy(uim_status_seen, now, USB_STATUS_WINDOW);
	/* Printable runs, in order. */
	for (i = 0; i < USB_STATUS_WINDOW; i++) {
		uint8 c = (uint8)now[i];
		if (c >= 0x20 && c < 0x7f) {
			if (n < (int)sizeof(line) - 1)
				line[n++] = (char)c;
			continue;
		}
		if (n >= 8) {
			line[n] = 0;
			USBHIDLog("  expert: %s", line);
		}
		n = 0;
	}
}

/*
 *  Deferred transfer completions.
 *
 *  USBServicesLib issues a control transfer as separate setup, data and status
 *  UIM calls. Completing a stage inside the call that created it lets USL see
 *  no other stage outstanding and finish the request early. The ROM OHCI UIM
 *  also completes later, from its queued interrupt work.
 */
enum { USB_DONE_QUEUE = 64 };

static struct {
	uint32 proc;
	uint32 refcon;
	uint32 actual;
	int32 status;
} usb_done[USB_DONE_QUEUE];
static int usb_done_in;
static int usb_done_out;

static void USBUIMComplete(uint32 proc, uint32 refcon, int32 status,
	uint32 actual)
{
	int next = (usb_done_in + 1) % USB_DONE_QUEUE;

	if (proc == 0)
		return;
	if (next == usb_done_out) {
		USBHIDLog("completion queue full, dropping %08x", refcon);
		return;
	}
	usb_done[usb_done_in].proc = proc;
	usb_done[usb_done_in].refcon = refcon;
	usb_done[usb_done_in].status = status;
	usb_done[usb_done_in].actual = actual;
	usb_done_in = next;
}

/* Slot 21 runs this during USBIdleTask as well as from the idle-loop backstop.
   That is required for the Expert's synchronous transfers, which wait by
   calling USBIdleTask and cannot return to the outer Mac OS idle loop. */
static void USBUIMRunCompletions(void)
{
	typedef void (*completion_ptr)(uint32, uint32, uint32);
	static int busy;
	int budget = USB_DONE_QUEUE;

	if (busy)
		return;
	busy = 1;
	while (usb_done_out != usb_done_in && budget-- > 0) {
		uint32 proc = usb_done[usb_done_out].proc;
		uint32 refcon = usb_done[usb_done_out].refcon;
		uint32 status = (uint32)usb_done[usb_done_out].status;
		uint32 actual = usb_done[usb_done_out].actual;
		usb_done_out = (usb_done_out + 1) % USB_DONE_QUEUE;
		USBHIDLog("complete refcon=%08x status=%d actual=%u",
			refcon, (int)(int32)status, actual);
		CallMacOS3(completion_ptr, proc, refcon, status, actual);
	}
	busy = 0;
}

/*
 *  Everything above the UIM talks to the Expert through one notification
 *  routine. USBServicesLib builds its record unaligned on the guest stack:
 *
 *      +0x00 byte  type (0 device, 2 interface, 6 get-next-by-class,
 *                        0x17..0x19 status messages)
 *      +0x02 long  pointer to the device reference
 *      +0x06 long  root-hub path value
 *      +0x0a long  device descriptor, or status-message string
 *      +0x0e long  parent device reference
 *      +0x12 long  bus index
 */
static uint32 usb_expert_notify;

/* The queued flag is logged for context, but is not a drain gate: on this path
   the Expert leaves it set after the ring itself is empty. */
static uint32 usb_expert_pending;

/* Exported Expert idle routine, resolved from the same TOC slot INIT_USBExpert
   uses to create its task-time routine descriptor. */
static uint32 usb_expert_idle;

/* How long to keep draining the Expert's event queue after it was last given
   something. Its queue only ever gains an entry when USBServicesLib notifies the
   Expert, and that notification comes through USBUIMExpertNotify below - so
   between notifications there is nothing to drain and calling the routine anyway
   is just guest time spent on an empty queue. The window has to outlast the work
   one arrival sets off, which is a class-driver load from disk: measured at a
   couple of seconds for USBHub0Apple. */
static uint64 usb_expert_drain_until;
enum { USB_EXPERT_DRAIN_WINDOW = 8000000 };

/* Defined below, with the registry publishing it feeds. */
static void USBUIMQueueNode(uint32 type, uint32 devref, uint32 parentref,
	uint32 bus, uint32 desc);

uint32 USBUIMExpertNotify(uint32 record)
{
	typedef uint32 (*notify_ptr)(uint32);
	uint32 type = ReadMacInt8(record);
	uint32 result;

	/* Types 0x17-0x19 are the Expert's status messages coming back around;
	   USBUIMDumpExpertStatus prints them together, so do not duplicate them. */
	if (type < 0x17)
		USBHIDLog("expert notify type=%u devref=%08x %08x desc=%08x %08x bus=%08x",
			type, ReadMacInt32(ReadMacInt32(record + 2)),
			ReadMacInt32(record + 6), ReadMacInt32(record + 0x0a),
			ReadMacInt32(record + 0x0e), ReadMacInt32(record + 0x12));

	/* Creating registry entries allocates, so only copy the arrival here and
	   publish it later at task level from USBUIMPoll. */
	if (type == 0 || type == 2)
		USBUIMQueueNode(type, ReadMacInt32(ReadMacInt32(record + 2)),
			ReadMacInt32(record + 0x0e), ReadMacInt32(record + 0x12),
			ReadMacInt32(record + 0x0a));
	/* Whatever this is, the Expert may queue work for it, so arm the drain. */
	usb_expert_drain_until = GetTicks_usec() + USB_EXPERT_DRAIN_WINDOW;

	if (usb_expert_notify == 0)
		return 0;
	result = CallMacOS1(notify_ptr, usb_expert_notify, record);
	if (type < 0x17 && usb_expert_pending)
		USBHIDLog("  expert ring flag=%u", ReadMacInt8(usb_expert_pending));
	return result;
}

/* Resolved once the bus is registered; see USBUIMRegisterBus. */
static uint32 usb_idle_task;
static uint32 usb_next_by_class;
static uint64 usb_probe_at;

static int16 USBUIMSetProp(uint32 rpc, uint32 entry, const char *name,
	uint32 value, uint32 size)
{
	typedef int16 (*prop_ptr)(uint32, uint32, uint32, uint32);
	SheepString sname(name);
	int16 err;

	err = (int16)CallMacOS4(prop_ptr, rpc, entry, sname.addr(), value, size);
	if (size == 4)
		USBHIDLog("registry property %s=%08x -> %d", name,
			ReadMacInt32(value), err);
	else
		USBHIDLog("registry property %s size=%u -> %d", name, size, err);
	return err;
}

static void USBUIMSetPropStr(uint32 rpc, uint32 entry, const char *name,
	const char *text)
{
	SheepString svalue(text);

	USBHIDLog("registry string %s='%s'", name, text);
	USBUIMSetProp(rpc, entry, name, svalue.addr(),
		(uint32)strlen(text) + 1);
}

enum { USB_NODE_QUEUE = 8 };

static struct {
	uint32 devref;
	uint32 parentref;
	uint32 bus;
	uint8 desc[18];
	uint8 have_desc;
	uint8 type;
} usb_new[USB_NODE_QUEUE];
static int usb_new_in;
static int usb_new_out;

static void USBUIMQueueNode(uint32 type, uint32 devref, uint32 parentref,
	uint32 bus, uint32 desc)
{
	int next = (usb_new_in + 1) % USB_NODE_QUEUE;

	if (next == usb_new_out)
		return;
	usb_new[usb_new_in].type = (uint8)type;
	usb_new[usb_new_in].devref = devref;
	usb_new[usb_new_in].parentref = parentref;
	usb_new[usb_new_in].bus = bus;
	usb_new[usb_new_in].have_desc = 0;
	/* Copy the descriptor now. That pointer is the buffer USBServicesLib read
	   it into and the buffer is reused, so by the time the nodes are made from
	   the idle loop it holds something else. */
	if (type == 0 && desc) {
		Mac2Host_memcpy(usb_new[usb_new_in].desc, desc, 18);
		usb_new[usb_new_in].have_desc = 1;
	}
	usb_new_in = next;
}

/*
 *  Complete the USB Expert's own root-hub registry node.
 *
 *  The Expert publishes the whole device tree itself - measured, with the node
 *  names and property sets its code writes:
 *
 *      Devices:device-tree:ohci-host     device_type="usb", AAPL,BusNumber=0
 *      +- hub          name AAPL,USBNodeType deviceRef parent-PortNum
 *         +- composite name ... VendorID ProductID DeviceClass ... (22 more)
 *            +- hid    name ... InterfaceClass InterfaceSubClass ...
 *
 *  so creating nodes of our own was redundant and has been deleted. What is
 *  wrong is that the hub node has only four properties: the Expert's hub
 *  publisher (code 0x85d0) stops right after parent-PortNum and never writes
 *  parent-deviceRef, locationID, NumPorts, MfgStr, UserName, SerialNoStr,
 *  BusPower, RootHub or any of the twelve descriptor fields.
 *
 *  That one gap is what hides the whole bus from Apple System Profiler: its
 *  device handler (Profiler code 0x2afe4) reads VendorID at 0x2b010 and returns
 *  immediately if it is missing, and the hub is the bus node's only child - so
 *  the walk stops at the hub and never reaches the device behind it. Hence "No
 *  devices found on this bus".
 *
 *  The values are not invented: the root hub is our own software device, so they
 *  come straight out of hub_device_desc, and the names and sizes are the
 *  Expert's own (its property-name table at data 0x33f8..0x3646, call site
 *  0x85d0 for a hub and 0x7b68..0x7d70 for the descriptor fields).
 */
static void USBUIMPublishNodes(void)
{
	typedef int16 (*iter_ptr)(uint32);
	typedef int16 (*search_ptr)(uint32, uint32, uint32, uint32, uint32, uint32,
		uint32);
	static uint32 rpc, iter_new, iter_del, search;
	static uint64 fill_at;
	static int filled;

	if (usb_new_out == usb_new_in)
		return;
	/* Not while the bus is still being enumerated: twenty registry calls is
	   tens of milliseconds of guest time, and the hub driver is mid-conversation
	   when the arrival is queued. Wait until it has gone quiet, leaving the
	   arrival on the queue meanwhile. */
	if (fill_at == 0) {
		fill_at = GetTicks_usec() + 4000000;
		return;
	}
	if (GetTicks_usec() < fill_at)
		return;
	if (rpc == 0) {
		rpc = FindLibSymbol("\017NameRegistryLib",
			"\026RegistryPropertyCreate");
		iter_new = FindLibSymbol("\017NameRegistryLib",
			"\032RegistryEntryIterateCreate");
		iter_del = FindLibSymbol("\017NameRegistryLib",
			"\033RegistryEntryIterateDispose");
		search = FindLibSymbol("\017NameRegistryLib",
			"\023RegistryEntrySearch");
		USBHIDLog("RegistryPropertyCreate=%08x search=%08x", rpc, search);
	}

	while (usb_new_out != usb_new_in) {
		uint32 type = usb_new[usb_new_out].type;
		uint32 devref = usb_new[usb_new_out].devref;
		uint8 *desc = usb_new[usb_new_out].desc;
		int have = usb_new[usb_new_out].have_desc;
		int is_hub = have && desc[4] == 9;

		usb_new_out = (usb_new_out + 1) % USB_NODE_QUEUE;
		if (type != 0 || !is_hub || filled)
			continue;
		if (rpc == 0 || search == 0 || iter_new == 0 || iter_del == 0)
			continue;

		/* The Expert's node for it, found the way the Expert finds its own:
		   iterate op 1 over AAPL,USBNodeType = "usbh" (five bytes, C string). */
		{
			SheepArray<64> cookie;
			SheepArray<16> entry;
			SheepArray<4> done;
			SheepArray<5> want;
			SheepString prop_type("AAPL,USBNodeType");
			SheepVar32 v;
			int16 err;

			Host2Mac_memcpy(want.addr(), "usbh", 5);
			WriteMacInt8(done.addr(), 0);
			CallMacOS1(iter_ptr, iter_new, cookie.addr());
			err = (int16)CallMacOS7(search_ptr, search, cookie.addr(), 1,
				entry.addr(), done.addr(), prop_type.addr(), want.addr(), 5);
			CallMacOS1(iter_ptr, iter_del, cookie.addr());
			if (err || ReadMacInt8(done.addr())) {
				USBHIDLog("hub node not published by the Expert (err=%d done=%u)",
					err, ReadMacInt8(done.addr()));
				continue;
			}
			filled = 1;

			/* What its own hub publisher would have written. */
			USBUIMSetPropStr(rpc, entry.addr(), "UserName", "hub");
			USBUIMSetPropStr(rpc, entry.addr(), "MfgStr", "SheepShaver");
			USBUIMSetPropStr(rpc, entry.addr(), "ProductStr",
				"SheepShaver Root Hub");
			USBUIMSetPropStr(rpc, entry.addr(), "SerialNoStr", "");
			USBUIMSetPropStr(rpc, entry.addr(), "RootHub", "true");
			v.set_value(0);
			USBUIMSetProp(rpc, entry.addr(), "parent-deviceRef", v.addr(), 4);
			USBUIMSetProp(rpc, entry.addr(), "locationID", v.addr(), 4);
			v.set_value(1);
			USBUIMSetProp(rpc, entry.addr(), "NumPorts", v.addr(), 4);
			v.set_value(500);
			USBUIMSetProp(rpc, entry.addr(), "BusPower", v.addr(), 4);

			/* And the descriptor fields, the two Profiler insists on first. */
			v.set_value((uint32)desc[3] << 8 | desc[2]);
			USBUIMSetProp(rpc, entry.addr(), "USBReleaseNumber", v.addr(), 4);
			v.set_value(desc[4]);
			USBUIMSetProp(rpc, entry.addr(), "DeviceClass", v.addr(), 4);
			v.set_value(desc[5]);
			USBUIMSetProp(rpc, entry.addr(), "DeviceSubClass", v.addr(), 4);
			v.set_value(desc[6]);
			USBUIMSetProp(rpc, entry.addr(), "DeviceProtocol", v.addr(), 4);
			v.set_value(desc[7]);
			USBUIMSetProp(rpc, entry.addr(), "MaxPacketSize0", v.addr(), 4);
			v.set_value((uint32)desc[9] << 8 | desc[8]);
			USBUIMSetProp(rpc, entry.addr(), "VendorID", v.addr(), 4);
			v.set_value((uint32)desc[11] << 8 | desc[10]);
			USBUIMSetProp(rpc, entry.addr(), "ProductID", v.addr(), 4);
			v.set_value((uint32)desc[13] << 8 | desc[12]);
			USBUIMSetProp(rpc, entry.addr(), "DeviceReleaseNumber", v.addr(), 4);
			v.set_value(desc[14]);
			USBUIMSetProp(rpc, entry.addr(), "MfgStrIndex", v.addr(), 4);
			v.set_value(desc[15]);
			USBUIMSetProp(rpc, entry.addr(), "ProductStrIndex", v.addr(), 4);
			v.set_value(desc[16]);
			USBUIMSetProp(rpc, entry.addr(), "SerialNoStrIndex", v.addr(), 4);
			v.set_value(desc[17]);
			USBUIMSetProp(rpc, entry.addr(), "NumConfigurations", v.addr(), 4);
			USBHIDLog("Expert hub node completed from hub_device_desc");
		}
	}
}

#if USB_UIM_DIAGNOSTICS
typedef int16 (*regprop_ptr)(uint32, uint32, uint32, uint32);

static int USBUIMGetU32(uint32 propget, uint32 entry, const char *name,
	uint32 *out)
{
	SheepString sname(name);
	SheepVar32 psize;
	SheepVar32 pval;

	psize.set_value(4);
	pval.set_value(0);
	if (propget == 0)
		return 0;
	if (CallMacOS4(regprop_ptr, propget, entry, sname.addr(), pval.addr(),
			psize.addr()) != 0)
		return 0;
	*out = pval.value();
	return 1;
}

static void USBUIMGetStr(uint32 propget, uint32 entry, const char *name,
	char *out, int max)
{
	SheepString sname(name);
	SheepVar32 psize;
	SheepArray<128> pval;
	int i;

	out[0] = 0;
	if (propget == 0)
		return;
	psize.set_value(max - 1 < 128 ? (uint32)(max - 1) : 128);
	if (CallMacOS4(regprop_ptr, propget, entry, sname.addr(), pval.addr(),
			psize.addr()) != 0)
		return;
	for (i = 0; i < (int)psize.value() && i < max - 1; i++)
		out[i] = (char)ReadMacInt8(pval.addr() + i);
	out[i] = 0;
}

/*
 *  Dump the USB subtree of the Name Registry as it actually is.
 *
 *  Apple System Profiler's enumerator (its code 0x2bad0) searches the registry
 *  for device_type = "usb" - four bytes, NUL included - reads AAPL,BusNumber off
 *  the node it finds, then walks that node's children as devices (0x2b944 ->
 *  0x2afe4, which returns at once unless VendorID and ProductID both read) and
 *  each device's children as interfaces (0x2ad38, which needs InterfaceClass).
 *
 *  Repeated here at two levels the same way Profiler does it, printing each
 *  node's name, the three properties the walk turns on, and every property name
 *  the node actually carries - so a node that is missing one is visible rather
 *  than inferred. kRegIterChildren is 4, and Profiler passes it on every call.
 */
static uint32 usb_reg_iter_new;
static uint32 usb_reg_iter_set;
static uint32 usb_reg_iterate;
static uint32 usb_reg_iter_del;
static uint32 usb_reg_propget;
static uint32 usb_reg_pi_new;
static uint32 usb_reg_pi_next;
static uint32 usb_reg_pi_del;

static void USBUIMDumpNode(uint32 entry, const char *lead)
{
	typedef int16 (*propiternew_ptr)(uint32, uint32);
	typedef int16 (*propiter_ptr)(uint32, uint32, uint32);
	typedef int16 (*iter_ptr)(uint32);
	SheepArray<64> pcookie;
	SheepArray<4> pdone;
	SheepArray<40> pname;
	uint32 vid = 0xffffffff, pid = 0xffffffff, ic = 0xffffffff;
	char nm[64], ps[64], names[512];
	int len = 0;

	USBUIMGetStr(usb_reg_propget, entry, "name", nm, sizeof(nm));
	USBUIMGetStr(usb_reg_propget, entry, "ProductStr", ps, sizeof(ps));
	USBUIMGetU32(usb_reg_propget, entry, "VendorID", &vid);
	USBUIMGetU32(usb_reg_propget, entry, "ProductID", &pid);
	USBUIMGetU32(usb_reg_propget, entry, "InterfaceClass", &ic);
	USBHIDLog("%s'%s' VendorID=%08x ProductID=%08x InterfaceClass=%08x "
		"ProductStr='%s'", lead, nm, vid, pid, ic, ps);

	names[0] = 0;
	if (usb_reg_pi_new && usb_reg_pi_next && usb_reg_pi_del
			&& CallMacOS2(propiternew_ptr, usb_reg_pi_new, entry,
				pcookie.addr()) == 0) {
		int guard = 40;
		WriteMacInt8(pdone.addr(), 0);
		while (guard-- > 0) {
			int i;
			if ((int16)CallMacOS3(propiter_ptr, usb_reg_pi_next, pcookie.addr(),
					pname.addr(), pdone.addr()) != 0)
				break;
			for (i = 0; i < 39 && len < (int)sizeof(names) - 2; i++) {
				char c = (char)ReadMacInt8(pname.addr() + i);
				if (c == 0)
					break;
				names[len++] = c;
			}
			names[len++] = ' ';
			names[len] = 0;
			if (ReadMacInt8(pdone.addr()))
				break;
		}
		CallMacOS1(iter_ptr, usb_reg_pi_del, pcookie.addr());
	}
	USBHIDLog("%s  props: %s", lead, names);
}

static void USBUIMDumpProfilerView(void)
{
	typedef int16 (*iter_ptr)(uint32);
	typedef int16 (*iterset_ptr)(uint32, uint32);
	typedef int16 (*iterate_ptr)(uint32, uint32, uint32, uint32);
	typedef int16 (*search_ptr)(uint32, uint32, uint32, uint32, uint32, uint32,
		uint32);
	uint32 search;
	SheepArray<64> cookie;
	SheepArray<64> subcookie;
	SheepArray<16> bus;
	SheepArray<16> dev;
	SheepArray<16> intf;
	SheepArray<4> done;
	SheepArray<4> subdone;
	SheepString prop_dt("device_type");
	SheepArray<4> want;
	uint32 v;
	int16 err;
	int ndev = 0;

	usb_reg_iter_new = FindLibSymbol("\017NameRegistryLib",
		"\032RegistryEntryIterateCreate");
	usb_reg_iter_set = FindLibSymbol("\017NameRegistryLib",
		"\027RegistryEntryIterateSet");
	usb_reg_iterate = FindLibSymbol("\017NameRegistryLib",
		"\024RegistryEntryIterate");
	usb_reg_iter_del = FindLibSymbol("\017NameRegistryLib",
		"\033RegistryEntryIterateDispose");
	usb_reg_propget = FindLibSymbol("\017NameRegistryLib",
		"\023RegistryPropertyGet");
	usb_reg_pi_new = FindLibSymbol("\017NameRegistryLib",
		"\035RegistryPropertyIterateCreate");
	usb_reg_pi_next = FindLibSymbol("\017NameRegistryLib",
		"\027RegistryPropertyIterate");
	usb_reg_pi_del = FindLibSymbol("\017NameRegistryLib",
		"\036RegistryPropertyIterateDispose");
	search = FindLibSymbol("\017NameRegistryLib", "\023RegistryEntrySearch");

	if (!usb_reg_iter_new || !usb_reg_iter_set || !usb_reg_iterate
			|| !usb_reg_iter_del || !search) {
		USBHIDLog("profiler view unavailable (%08x %08x %08x %08x %08x)",
			usb_reg_iter_new, usb_reg_iter_set, usb_reg_iterate,
			usb_reg_iter_del, search);
		return;
	}
	USBHIDLog("propiter %08x %08x %08x", usb_reg_pi_new, usb_reg_pi_next,
		usb_reg_pi_del);
	Host2Mac_memcpy(want.addr(), "usb", 4);
	WriteMacInt8(done.addr(), 0);
	CallMacOS1(iter_ptr, usb_reg_iter_new, cookie.addr());
	err = (int16)CallMacOS7(search_ptr, search, cookie.addr(), 1, bus.addr(),
		done.addr(), prop_dt.addr(), want.addr(), 4);
	CallMacOS1(iter_ptr, usb_reg_iter_del, cookie.addr());
	if (err || ReadMacInt8(done.addr())) {
		USBHIDLog("profiler view: no device_type='usb' node (err=%d done=%u)",
			err, ReadMacInt8(done.addr()));
		return;
	}
	v = 0xffffffff;
	USBUIMGetU32(usb_reg_propget, bus.addr(), "AAPL,BusNumber", &v);
	USBHIDLog("profiler view: bus AAPL,BusNumber=%08x", v);
	USBUIMDumpNode(bus.addr(), "profiler view: bus ");

	CallMacOS1(iter_ptr, usb_reg_iter_new, cookie.addr());
	CallMacOS2(iterset_ptr, usb_reg_iter_set, cookie.addr(), bus.addr());
	WriteMacInt8(done.addr(), 0);
	while (ReadMacInt8(done.addr()) == 0 && ndev < 8) {
		int nintf = 0;
		if ((int16)CallMacOS4(iterate_ptr, usb_reg_iterate, cookie.addr(), 4,
				dev.addr(), done.addr()) != 0 || ReadMacInt8(done.addr()))
			break;
		ndev++;
		USBUIMDumpNode(dev.addr(), "profiler view:   child ");

		CallMacOS1(iter_ptr, usb_reg_iter_new, subcookie.addr());
		CallMacOS2(iterset_ptr, usb_reg_iter_set, subcookie.addr(), dev.addr());
		WriteMacInt8(subdone.addr(), 0);
		while (ReadMacInt8(subdone.addr()) == 0 && nintf < 8) {
			if ((int16)CallMacOS4(iterate_ptr, usb_reg_iterate,
					subcookie.addr(), 4, intf.addr(), subdone.addr()) != 0
					|| ReadMacInt8(subdone.addr()))
				break;
			nintf++;
			USBUIMDumpNode(intf.addr(), "profiler view:     grandchild ");
		}
		CallMacOS1(iter_ptr, usb_reg_iter_del, subcookie.addr());
	}
	CallMacOS1(iter_ptr, usb_reg_iter_del, cookie.addr());
	if (ndev == 0)
		USBHIDLog("profiler view: bus node has no children");
}

static void USBUIMDumpRegistryNodes(void)
{
	typedef int16 (*iter_ptr)(uint32);
	typedef int16 (*search_ptr)(uint32, uint32, uint32, uint32, uint32, uint32,
		uint32);
	typedef int16 (*propget_ptr)(uint32, uint32, uint32, uint32);
	static const char kind_[3][5] = { "usbd", "usbi", "usbh" };
	uint32 iter_new = FindLibSymbol("NameRegistryLib",
		"RegistryEntryIterateCreate");
	uint32 search = FindLibSymbol("NameRegistryLib",
		"RegistryEntrySearch");
	uint32 iter_del = FindLibSymbol("NameRegistryLib",
		"RegistryEntryIterateDispose");
	uint32 propget = FindLibSymbol("NameRegistryLib",
		"RegistryPropertyGet");
	SheepArray<64> cookie;
	SheepArray<16> found;
	SheepArray<4> done;
	SheepArray<5> want;
	SheepVar32 psize;
	SheepArray<64> pval;
	SheepString prop_type("AAPL,USBNodeType");
	SheepString prop_name("name");
	int i;

	if (!iter_new || !search || !iter_del) {
		USBHIDLog("registry search unavailable (%08x %08x %08x)",
			iter_new, search, iter_del);
		return;
	}
	for (i = 0; i < 3; i++) {
		int16 err;
		Host2Mac_memcpy(want.addr(), kind_[i], 5);
		WriteMacInt8(done.addr(), 0);
		CallMacOS1(iter_ptr, iter_new, cookie.addr());
		err = (int16)CallMacOS7(search_ptr, search, cookie.addr(), 1,
			found.addr(), done.addr(), prop_type.addr(), want.addr(), 5);
		if (err == 0 && ReadMacInt8(done.addr()) == 0) {
			char nm[64];
			int n = 0;
			nm[0] = 0;
			if (propget) {
				psize.set_value(sizeof(nm) - 1);
				if (CallMacOS4(propget_ptr, propget, found.addr(),
						prop_name.addr(), pval.addr(), psize.addr()) == 0) {
					uint32 len = psize.value();
					if (len > sizeof(nm) - 1)
						len = sizeof(nm) - 1;
					for (n = 0; n < (int)len; n++)
						nm[n] = (char)ReadMacInt8(pval.addr() + n);
					nm[n] = 0;
				}
			}
			USBHIDLog("registry: AAPL,USBNodeType '%s' found, name=%s",
				kind_[i], nm);
		} else
			USBHIDLog("registry: AAPL,USBNodeType '%s' none (err=%d done=%u)",
				kind_[i], err,
				ReadMacInt8(done.addr()));
		CallMacOS1(iter_ptr, iter_del, cookie.addr());
	}
}

/*
 *  Report what the USB Manager can see, using the same call Apple System
 *  Profiler makes. On a real Mac the USB extension gives the family time at
 *  SystemTask; here USBUIMPoll is that, so the answer to "is there a device"
 *  is available from the same place.
 */
static void USBUIMProbeDevices(void)
{
	typedef int16 (*next_ptr)(uint32, uint32, uint32, uint32, uint32);
	static const uint16 want[] = { 0, 9, 3 };	/* any, hub, HID */
	SheepVar32 dev, intf;
	unsigned i;

	if (usb_next_by_class == 0)
		return;
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		int16 err;
		dev.set_value(0);
		intf.set_value(0);
		err = (int16)CallMacOS5(next_ptr, usb_next_by_class, dev.addr(),
			intf.addr(), want[i], 0, 0);
		USBHIDLog("USBGetNextDeviceByClass(class=%u) -> %d device=%08x intf=%08x",
			want[i], err, dev.value(), intf.value());
	}
}
#endif

void USBUIMPoll(void)
{
	/* completion(refcon, OSStatus, actualByteCount) - the shape the ROM UIM
	   calls with at its own TD-done points (UIM code 0x35e0, 0x4f3c). */
	typedef void (*idle_ptr)(void);
	static int busy;

	/* Everything below calls back into the guest, and any of those calls can
	   reach SynchIdleTime again - which is where this is called from. Without
	   this the second visit starts the same work over and the guest never comes
	   back out. */
	if (busy)
		return;
	busy = 1;

	/* First, always. Everything below runs guest code and can take tens of
	   milliseconds; a completion left waiting behind it lets USL time the
	   request out and recycle the request block, and the next completion then
	   calls through a routine pointer that is no longer there. Measured: a
	   crash inside USBServicesLib at a bctrl with r11 = garbage, immediately
	   after a long stretch of registry work. */
	USBUIMRunCompletions();

	/* The two heartbeats.
	 *
	 * USBIdleTask is USL's own idle work (code 0x5cc8: drain the done queue,
	 * then call the registered idle proc through TOC-0x98), and usb_expert_idle
	 * is the Expert's event-queue drain, which a patch on toolbox trap 0xABF7
	 * would call - both once per SystemTask on a real Mac. The 60 Hz gate that
	 * gets them there is in USBNodePublishDeferred, in front of ExecuteNative,
	 * because the mode switch costs more than these calls do.
	 *
	 * USBIdleTask has to keep running: it is what reaches slot 21, which is
	 * where transfer completions and the joystick's report pipe are serviced.
	 * The Expert's drain only runs while an arrival is still being worked on -
	 * see usb_expert_drain_until.
	 */
	if (usb_idle_task)
		CallMacOS(idle_ptr, usb_idle_task);
	if (usb_expert_idle && GetTicks_usec() < usb_expert_drain_until)
		CallMacOS(idle_ptr, usb_expert_idle);

	/* The registry and USB Manager dumps that pinned all of the above down.
	   Kept, because they are the only way to see the family's view from here,
	   but off: each pass is nine FindLibSymbol calls - each one a guest
	   GetSharedLibrary plus FindSymbol - and dozens of registry calls. */
#if USB_UIM_DIAGNOSTICS
	if (usb_probe_at && GetTicks_usec() >= usb_probe_at) {
		static int probes;
		usb_probe_at = ++probes < 8 ? GetTicks_usec() + 4000000 : 0;
		USBHIDLog("--- probe %d: expert queued=%u ---", probes,
			usb_expert_pending ? ReadMacInt8(usb_expert_pending) : 0xff);
		USBUIMProbeDevices();
		USBUIMDumpRegistryNodes();
		USBUIMDumpProfilerView();
	}
#endif

	/* Registry work belongs here: task level, and it allocates. */
	USBUIMPublishNodes();
	busy = 0;
}

/*
 *  Called from the host tick thread, which keeps running when the guest stops
 *  taking interrupts. Reports where the guest is executing, named against the
 *  ROM fragment it lands in, so a spin shows up as the same address every time.
 */
extern uint32 PPCSampleGuestPC(void);
extern uint32 PPCSampleGuestGPR(int i);

void USBUIMSampleGuest(void)
{
#if USB_UIM_DIAGNOSTICS
	static int ticks;
	static int ticks_after_init;
	static uint32 last;
	uint32 pc, off;
	const char *where;

	/* Every tick for the first couple of seconds after the Expert first calls
	   us - that window is where bring-up either starts or is abandoned, and a
	   once-a-second sample walks straight past it - then back off. */
	ticks++;
	if (uim_seen_init) {
		/* Whatever the family makes of the bus, it says so here. Keep asking:
		   the interesting entries arrive over the seconds that follow, as
		   drivers are matched and pipes opened. */
		ticks_after_init++;
		if (ticks_after_init % 60 == 0)
			USBUIMDumpExpertStatus();
	}
	if (uim_seen_init && ticks_after_init < 120)
		;
	else if (ticks < 60 * 8 || ticks % 60)
		return;
	pc = PPCSampleGuestPC();
	off = pc - ROMBase;
	if (off < ROM_AREA_SIZE) {
		/* Fragment code sections, from docs/usb-ohci-uim.md section 1. */
		if (off >= 0x21de10 && off < 0x227d28) {
			USBHIDLog("guest in USBFamilyExpertLib code+%06x (pc=%08x)%s",
				off - 0x21de10, pc, pc == last ? " SPINNING" : "");
			last = pc;
			return;
		}
		if (off >= 0x214440 && off < 0x21c65c) {
			USBHIDLog("guest in USBServicesLib code+%06x (pc=%08x)%s",
				off - 0x214440, pc, pc == last ? " SPINNING" : "");
			last = pc;
			return;
		}
		where = "ROM";
	} else
		where = "RAM";
	/* Inside the ROM's 68k emulator the PPC pc only names an opcode handler.
	   r24 is the 68k instruction pointer and r25 the 68k interrupt level, and
	   those say which 68k code is looping and why it takes no interrupts. */
	USBHIDLog("guest in %s pc=%08x 68k=%08x lvl=%08x%s",
		where, pc, PPCSampleGuestGPR(24), PPCSampleGuestGPR(25),
		pc == last ? " SPINNING" : "");
	{
		bool settled = (pc == last);
		last = pc;
		if (!settled)
			return;
	}
	/* 68k D0..D7 are r8..r15, A0..A7 are r16..r23 (Execute68k sets them up
	   that way). Only worth printing once the PC has settled. */
	USBHIDLog("   d0-d7 %08x %08x %08x %08x %08x %08x %08x %08x",
		PPCSampleGuestGPR(8), PPCSampleGuestGPR(9), PPCSampleGuestGPR(10),
		PPCSampleGuestGPR(11), PPCSampleGuestGPR(12), PPCSampleGuestGPR(13),
		PPCSampleGuestGPR(14), PPCSampleGuestGPR(15));
	USBHIDLog("   a0-a7 %08x %08x %08x %08x %08x %08x %08x %08x",
		PPCSampleGuestGPR(16), PPCSampleGuestGPR(17), PPCSampleGuestGPR(18),
		PPCSampleGuestGPR(19), PPCSampleGuestGPR(20), PPCSampleGuestGPR(21),
		PPCSampleGuestGPR(22), PPCSampleGuestGPR(23));
	/* A 68k stack pointer of zero says this context was not entered by a
	   normal call. If the ROM got here from an exception, the vector that
	   fired points into the same code, so print the low-memory vectors once. */
	{
		static bool dumped_vectors;
		static const char *const vec_name[] = {
			"bus error", "address error", "illegal instr", "zero divide",
			"chk", "trapv", "privilege", "trace", "line A", "line F"
		};
		int v;
		if (!dumped_vectors) {
			dumped_vectors = true;
			for (v = 0; v < 10; v++)
				USBHIDLog("   vector %2d %-14s = %08x", v + 2, vec_name[v],
					ReadMacInt32(0x08 + v * 4));
		}
	}
#endif
}

/* CFM takes Pascal strings; write one into guest memory. */
static void PascalStr(uint32 addr, const char *s)
{
	int n = (int)strlen(s);

	if (n > 63)
		n = 63;
	WriteMacInt8(addr, (uint8)n);
	Host2Mac_memcpy(addr + 1, s, n);
}

/*
 *  Bring the bus up, preferring the Expert's own route.
 *
 *  LoadUIMForEntry is the Expert's exported "this registry node is a USB
 *  controller, load its UIM" entry, and it does more than USBAddBus: it
 *  allocates a bus record, copies the node's RegEntryID into it (Expert code
 *  0x113c), loads the plugin from the node's driver property with
 *  GetDriverForDevice, and only then calls USBAddBus (0x119c). That RegEntryID
 *  is the parent the Expert hangs its per-device registry nodes off - the
 *  AAPL,USBNodeType / deviceRef / AAPL,BusNumber entries that anything
 *  enumerating USB from the Name Registry reads. Calling USBAddBus ourselves
 *  gets a working bus but leaves the Expert with no registry entry for it, and
 *  so nothing to publish devices under.
 *
 *  Its earlier fnfErr was measured before USBServicesInitialise had run, so the
 *  order here matters: initialise USL first, then offer the node to the Expert,
 *  and only fall back to preparing the fragment ourselves if that fails.
 */
void USBUIMRegisterBus(uint32 node)
{
	typedef int16 (*gmf_ptr)(uint32, uint32, uint32, uint32, uint32, uint32, uint32);
	typedef int16 (*fsym_ptr)(uint32, uint32, uint32, uint32);
	typedef int16 (*addbus_ptr)(uint32, uint32, uint32);
	typedef int16 (*init_ptr)(uint32);
	typedef int16 (*loaduim_ptr)(uint32);
	static SheepArray<2048> *image;		/* must outlive the call */
	uint32 gmf, fsym, addbus, init_usl, notify, size;
	const uint8 *drv;
	SheepVar32 conn, main_addr, sym_addr;
	SheepArray<256> err_name;
	SheepArray<4> sym_class;
	/* Pascal strings, not SheepString: these take ConstStr63Param /
	   ConstStr255Param, and a C string's first byte reads as the length -
	   'S' is 83, past 63, which is what paramErr was telling us. */
	SheepArray<32> frag_name;
	SheepArray<32> sym_name;
	int16 err;

	PascalStr(frag_name.addr(), "SheepUSBUIM");
	PascalStr(sym_name.addr(), "ThePluginDispatchTable");

	gmf = FindLibSymbol("\014InterfaceLib", "\016GetMemFragment");
	fsym = FindLibSymbol("\014InterfaceLib", "\012FindSymbol");
	addbus = FindLibSymbol("\016USBServicesLib", "\011USBAddBus");
	USBHIDLog("GetMemFragment=%08x FindSymbol=%08x USBAddBus=%08x",
		gmf, fsym, addbus);
	if (!gmf || !fsym || !addbus)
		return;

	/* Bring the USB Services Lib up first. It has no PEF initialisation
	   routine of its own: INIT_USBExpert calls USBServicesInitialise, and only
	   after it has found at least one "usb" device-tree node - which did not
	   exist at startup, because we publish ours later. Until that call runs,
	   USBServicesLib's request queue pointer (its TOC+0xdf0) is still null, so
	   StartRootHub's PBEnqueueLast goes nowhere and the PBDequeueFirst right
	   after it hands back an uninitialised stack word as the request block.
	   That is the null completion routine the crash at USL code+0x258 was
	   calling. -6993 here means it was already up, which is equally fine.

	   Its one argument is the Expert's notification routine, stashed straight
	   into a global (USL code 0x1c18) and called back through later. The
	   Expert passes the same pointer it gave USBRegisterExpertNotification,
	   and that entry does nothing but store it at USBManagerLib's TOC+0x58 -
	   so read it back from there rather than reaching into the Expert's own
	   TOC, which exports nothing that names it. */
	notify = FindLibSymbol("\015USBManagerLib", "\027USBGetDeviceDescriptor");
	if (notify)
		notify = ReadMacInt32(ReadMacInt32(notify + 4) + 0x58);
	/* Cross-check against the Expert's own TOC slot, which is where
	   INIT_USBExpert reads it from (Expert code 0xbfc, TOC-0xec). The two must
	   agree; if they do not, one of the two offsets is wrong and the family
	   would be notified into nothing. */
	{
		uint32 loaduim = FindLibSymbol("\022USBFamilyExpertLib",
			"\017LoadUIMForEntry");
		uint32 expert_toc = loaduim ? ReadMacInt32(loaduim + 4) : 0;
		USBHIDLog("expert notification proc = %08x (expert TOC-0xec = %08x)"
			" tvector code=%08x toc=%08x", notify,
			expert_toc ? ReadMacInt32(expert_toc - 0xec) : 0,
			notify ? ReadMacInt32(notify) : 0,
			notify ? ReadMacInt32(notify + 4) : 0);
		if (expert_toc) {
			usb_expert_pending = ReadMacInt32(expert_toc - 0x78);
			/* The Expert's event-queue drain. INIT_USBExpert reads exactly this
			   slot (Expert code 0x890) to build the routine descriptor it hangs
			   off its toolbox-trap patch, so it is the Expert's own pointer to
			   its idle routine, not a guessed address. It takes no parameters -
			   NewRoutineDescriptor is called with procInfo 0. */
			usb_expert_idle = ReadMacInt32(expert_toc - 0xf4);
			/* That slot is a TOC offset read off the ROM Expert's own code, so
			   it only means anything while the Expert *is* the ROM's. If a
			   disk-based one ever replaces it the offset lands somewhere else
			   and we would call whatever happened to be there, which is a
			   guest crash with no way back to its cause. The Expert's code has
			   to be in ROM; anything else is not the TVector we wanted. */
			{
				uint32 code = usb_expert_idle
					? ReadMacInt32(usb_expert_idle) : 0;
				if (code < ROMBase || code >= ROMBase + ROM_AREA_SIZE) {
					USBHIDLog("expert idle TVector %08x has code=%08x, not in"
						" ROM - not calling it", usb_expert_idle, code);
					usb_expert_idle = 0;
				}
			}
		}
		/* Whether anything else already drives the Expert's event drain.
		 *
		 * INIT_USBExpert hangs it off a patch on toolbox trap 0xABF7 (Expert
		 * code 0x868: NewPtrSys, NewRoutineDescriptor, SetToolboxTrapAddress),
		 * and toolbox trap n lives at 0xE00 + (n & 0x3ff)*4. With a minimal
		 * Extensions folder that trap is the shared unimplemented-trap stub -
		 * every neighbour holds the same handler - so nothing calls it and the
		 * drain is ours to run. With a full Extensions folder it is a real 68k
		 * patch in RAM chaining into the ROM, so the drain is already being
		 * driven.
		 *
		 * Except that it is not: with a full Extensions folder 0xABF7 is a real
		 * 68k patch in RAM, and refusing to drive the drain because of it stops
		 * enumeration dead - the first "expert notify" is queued and nothing
		 * ever looks at it, so no hub driver, no port device and no HID.
		 * INIT_USBExpert bails here at "Couldn't find any USB Controllers/UIMS"
		 * long before its SetToolboxTrapAddress, so the Expert never installs
		 * that patch and whatever owns 0xABF7 is some other extension. Measured
		 * and reverted; the trap table is still logged because it is the only
		 * view of who else is in play.
		 */
		{
			uint32 patch = ReadMacInt32(0xe00 + 0x3f7 * 4);
			int t;
			USBHIDLog("expert pending flag=%08x  trap 0xABF7 -> %08x"
				" (patch stub %04x %08x %04x, chains to %08x)",
				usb_expert_pending, patch, ReadMacInt16(patch),
				ReadMacInt32(patch + 2), ReadMacInt16(patch + 6),
				ReadMacInt32(patch + 8));
			for (t = 0x3f4; t <= 0x3fa; t++)
				USBHIDLog("  trap 0xA%03X -> %08x", 0x800 + t,
					ReadMacInt32(0xe00 + t * 4));
		}
	}
	init_usl = FindLibSymbol("\016USBServicesLib", "\025USBServicesInitialise");
	if (init_usl && notify) {
		/* Hand USL our own TVector and chain: this is the only call the family
		   makes into the Expert, so it is the only place the conversation above
		   the UIM can be seen. */
		usb_expert_notify = notify;
		err = (int16)CallMacOS1(init_ptr, init_usl,
			NativeTVECT(NATIVE_USB_EXPERT_NOTIFY));
		USBHIDLog("USBServicesInitialise -> %d", err);
	}

	/* Offer the node to the Expert. If it takes it, it owns the bus record, the
	   RegEntryID and the plugin, and it has somewhere to publish device nodes. */
	{
		uint32 loaduim = FindLibSymbol("USBFamilyExpertLib",
			"LoadUIMForEntry");
		err = -1;
		if (loaduim && node)
			err = (int16)CallMacOS1(loaduim_ptr, loaduim, node);
		USBHIDLog("LoadUIMForEntry(node=%08x) -> %d", node, err);
		if (err == 0)
			goto registered;
	}

	/* Fallback: prepare the fragment ourselves and add the bus directly. This
	   works, but see the note above - the Expert ends up without a registry entry
	   for the bus. */
	drv = USBUIMDriver(&size);
	if (image == NULL)
		image = new SheepArray<2048>;
	Host2Mac_memcpy(image->addr(), drv, size);

	/* kPrivateCFragCopy: our own copy, not shared with anything else. */
	err = (int16)CallMacOS7(gmf_ptr, gmf, image->addr(), size,
		frag_name.addr(), 5, conn.addr(), main_addr.addr(), err_name.addr());
	USBHIDLog("GetMemFragment -> %d conn=%08x", err, conn.value());
	if (err)
		return;

	err = (int16)CallMacOS4(fsym_ptr, fsym, conn.value(), sym_name.addr(),
		sym_addr.addr(), sym_class.addr());
	USBHIDLog("FindSymbol(ThePluginDispatchTable) -> %d addr=%08x version=%d",
		err, sym_addr.value(),
		sym_addr.value() ? ReadMacInt32(sym_addr.value()) : 0);
	if (err || sym_addr.value() == 0)
		return;

	/* USBAddBus(busRef, dispatchTable, refcon). Only the last two matter: the
	   first is overwritten before use (USL code 0x3a10), the second must point
	   at a version-4 plugin table, and the third is kept as the bus refcon -
	   the Expert passes its own bus ordinal there, so 0 for our single bus. */
	err = (int16)CallMacOS3(addbus_ptr, addbus, 0, sym_addr.value(), 0);
	USBHIDLog("USBAddBus(table=%08x) -> %d", sym_addr.value(), err);

registered:

	/* Say that USB exists.
	 *
	 * INIT_USBExpert registers two Gestalt values once it has a working bus -
	 * 'usb ' = 3 and 'usbv' = its version - by calling NewGestaltValue through
	 * trap 0xABF1 with dispatch selector 0x404 (Expert code 0 and its callers at
	 * 0xc8c/0xc98; on the failure path it registers 'usb ' = 0 instead). It
	 * never got that far here, so nothing that asks Gestalt whether the machine
	 * has USB gets an answer - which is what Apple System Profiler does before
	 * it will show a USB section at all.
	 *
	 * NewGestaltValue is a documented InterfaceLib entry, so call it directly
	 * rather than through the dispatcher. ReplaceGestaltValue covers the case
	 * where something did register it first. */
	{
		typedef int16 (*gestalt_ptr)(uint32, uint32);
		typedef int16 (*gestalt_get_ptr)(uint32, uint32);
		uint32 newval = FindLibSymbol("\014InterfaceLib", "\017NewGestaltValue");
		uint32 replval = FindLibSymbol("\014InterfaceLib",
			"\023ReplaceGestaltValue");
		uint32 get = FindLibSymbol("\014InterfaceLib", "\007Gestalt");
		static const uint32 sel[2] = { 0x75736220, 0x75736276 };  /* 'usb ', 'usbv' */
		static const uint32 val[2] = { 3, 0x01218000 };		 /* working, 1.2.1 */
		SheepVar32 resp;
		int i;

		for (i = 0; i < 2; i++) {
			int16 before = get ? (int16)CallMacOS2(gestalt_get_ptr, get, sel[i],
				resp.addr()) : 0;
			int16 e = -1;
			if (newval)
				e = (int16)CallMacOS2(gestalt_ptr, newval, sel[i], val[i]);
			if (e && replval)
				e = (int16)CallMacOS2(gestalt_ptr, replval, sel[i], val[i]);
			USBHIDLog("Gestalt '%c%c%c%c' was %d (%08x), NewGestaltValue -> %d",
				(char)(sel[i] >> 24), (char)(sel[i] >> 16),
				(char)(sel[i] >> 8), (char)sel[i], before,
				before ? 0 : resp.value(), e);
		}
		/* Apple System Profiler will not even look for USB on a machine it
		   knows predates it: its gate (code 0xc180) switches on gestaltMachineType
		   and returns "no USB" for 0x0c, 0x27-0x2a, 0x2e, 0x2f, 0x37, 0x41,
		   0x4b, 0x74-0x7b and 0x7e. Anything else reaches the registry walk, so
		   this says whether the walk is even attempted here. */
		if (get) {
			int16 e = (int16)CallMacOS2(gestalt_get_ptr, get, 0x6d616368,
				resp.addr());
			USBHIDLog("Gestalt 'mach' -> %d value=%08x (%u)", e, resp.value(),
				resp.value());
		}
	}

	/* Everything above the UIM now reports through the Expert's own status log,
	   which says in words what it makes of the bus. Resolve it here rather than
	   from slot 0: the Expert's LoadUIM never runs on this path. */
	USBUIMFindExpertStatus();
	uim_seen_init = 1;

	/* Give the family a task-level heartbeat and, a few seconds later, ask the
	   USB Manager what it can see. Both run from USBUIMPoll. */
	usb_idle_task = FindLibSymbol("\016USBServicesLib", "\013USBIdleTask");
	usb_next_by_class = FindLibSymbol("\015USBManagerLib",
		"\027USBGetNextDeviceByClass");
	usb_probe_at = GetTicks_usec() + 5000000;
	USBHIDLog("USBIdleTask=%08x USBGetNextDeviceByClass=%08x",
		usb_idle_task, usb_next_by_class);

	/* What InputSprocket needs before a USB stick can appear in its list.
	 *
	 * Its two USB HID device modules live inside InputSprocket Extension
	 * (containers 0x20e10 and 0x32140 - both export ISpDriver_FindAndLoadDevices
	 * and friends), so no extra module has to be installed. What they import is
	 * HIDLib - HIDOpenReportDescriptor, HIDGetCaps, HIDGetUsageValue,
	 * HIDGetButtonCaps - plus USBInstallDeviceNotification and
	 * USBGetDriverConnectionID. HIDLib is *not* in USB HID Driver, which only
	 * publishes TheHIDDeviceDispatchTable; it is a shared library of its own.
	 * If it does not resolve then CFM cannot prepare those modules at all and
	 * the device can never show up, however well it enumerates. */
	USBHIDLog("InputSprocket prerequisites:"
		" HIDLib.HIDOpenReportDescriptor=%08x HIDLib.HIDGetCaps=%08x"
		" USBInstallDeviceNotification=%08x USBGetDriverConnectionID=%08x"
		" ISpInstallUSBHIDDefer=%08x",
		FindLibSymbol("\006HIDLib", "\027HIDOpenReportDescriptor"),
		FindLibSymbol("\006HIDLib", "\012HIDGetCaps"),
		FindLibSymbol("\015USBManagerLib", "\034USBInstallDeviceNotification"),
		FindLibSymbol("\015USBManagerLib", "\030USBGetDriverConnectionID"),
		FindLibSymbol("\025InputSprocketDeferLib", "\025ISpInstallUSBHIDDefer"));
}

/*
 *  The software root hub.
 *
 *  With a UIM of our own there is no host controller in the picture at all, so
 *  the hub at the top of the bus has to be answered here. That is not a
 *  shortcut: the ROM's OHCI UIM does exactly the same thing - its slot 3 and
 *  slot 20 both compare the function address against the root hub address it
 *  keeps at globals+0x488 and, when they match, answer the request from
 *  software instead of building a transfer descriptor (UIM code 0x15cc ->
 *  0x4db8). A root hub has no wire to put a packet on either way.
 *
 *  Transfer arguments, from the ROM UIM's own use of them:
 *
 *    slot 3 UIMCreateControlTransfer(refcon, completion, buffer, rounding,
 *                                   functionAddress, endpoint, length, dir)
 *    slot 7 UIMCreateInterruptTransfer(same shape)
 *
 *  where the UIM stores completion at TD+0x10 and refcon at TD+0x14 and later
 *  calls completion(refcon, OSStatus, actualCount) through glue 0x5a40. dir is
 *  Mac OS's kUSBOut 0 / kUSBIn 1 / kUSBNone 2, and a control transfer arrives
 *  as three separate calls: kUSBNone with the eight-byte device request, then
 *  the data stage, then a zero-length status stage.
 */

/* kUSBInternalErr territory: what an entry we do not implement should report,
   so the family gives up on it rather than waiting for a completion. */
enum { kUIMUnimplemented = -6600 };

enum {
	USB_DIR_OUT = 0,
	USB_DIR_IN = 1,
	USB_DIR_NONE = 2
};

/* One pending device request per function address. USL runs a control
   transfer's stages strictly in order, so the last request seen is all the
   state a control endpoint needs. */
enum { USB_MAX_FUNCTIONS = 128 };
static uint8 usb_setup[USB_MAX_FUNCTIONS][8];

/* Address USL assigns to the root hub with SET_ADDRESS. Zero until then, which
   is also the address every unconfigured device answers on. */
static uint8 root_hub_addr;

/* wPortStatus/wPortChange for the one downstream port. */
static uint16 port_status;
static uint16 port_change;

static const uint8 hub_device_desc[] = {
	18, 1,				/* bLength, DEVICE */
	0x10, 0x01,			/* bcdUSB 1.10 */
	9, 0, 0,			/* class hub, no subclass, no protocol */
	8,				/* bMaxPacketSize0 */
	0xac, 0x05,			/* idVendor - Apple, so the hub driver's own
					   match tables recognise the shape */
	0x01, 0x80,			/* idProduct */
	0x00, 0x01,			/* bcdDevice 1.00 */
	1, 2, 0,			/* iManufacturer, iProduct, iSerialNumber */
	1				/* bNumConfigurations */
};

static const uint8 hub_config_desc[] = {
	9, 2,				/* bLength, CONFIGURATION */
	25, 0,				/* wTotalLength */
	1, 1, 0,			/* bNumInterfaces, bConfigurationValue, iConfiguration */
	0xe0, 0,			/* self-powered + remote wakeup, bMaxPower */
	9, 4,				/* bLength, INTERFACE */
	0, 0, 1,			/* number, alternate, bNumEndpoints */
	9, 0, 0, 0,			/* class hub, subclass, protocol, iInterface */
	7, 5,				/* bLength, ENDPOINT */
	0x81, 0x03,			/* endpoint 1 IN, interrupt */
	1, 0,				/* wMaxPacketSize */
	0xff				/* bInterval */
};

static const uint8 hub_class_desc[] = {
	9, 0x29,			/* bLength, HUB */
	1,				/* bNbrPorts */
	0x09, 0x00,			/* individual power switching and over-current */
	1, 0,				/* bPwrOn2PwrGood (2ms), bHubContrCurrent */
	0x00,				/* DeviceRemovable - port 1 is removable */
	0xff				/* PortPwrCtrlMask */
};

static const uint8 hub_lang_desc[] = { 4, 3, 0x09, 0x04 };

/* Descriptor 3 is UTF-16LE; build it rather than spelling out the padding. */
static int USBStringDesc(const char *s, uint8 *out)
{
	int n = (int)strlen(s);
	int i;

	out[0] = (uint8)(2 + n * 2);
	out[1] = 3;
	for (i = 0; i < n; i++) {
		out[2 + i * 2] = (uint8)s[i];
		out[3 + i * 2] = 0;
	}
	return out[0];
}

/* Hub class feature selectors. */
enum {
	PORT_CONNECTION = 0,
	PORT_ENABLE = 1,
	PORT_SUSPEND = 2,
	PORT_OVER_CURRENT = 3,
	PORT_RESET = 4,
	PORT_POWER = 8,
	PORT_LOW_SPEED = 9,
	C_PORT_CONNECTION = 16,
	C_PORT_ENABLE = 17,
	C_PORT_SUSPEND = 18,
	C_PORT_OVER_CURRENT = 19,
	C_PORT_RESET = 20
};

/*
 *  Answer one device request aimed at the root hub. Returns the number of bytes
 *  written to buf, or -1 to stall. Requests with no data stage return 0.
 */
static int USBRootHubRequest(const uint8 *req, uint8 *buf, int max)
{
	uint32 type = req[0];
	uint32 request = req[1];
	uint32 value = req[2] | (req[3] << 8);
	uint32 index = req[4] | (req[5] << 8);
	uint8 str[64];
	const uint8 *src = NULL;
	int len = 0;

	switch ((type << 8) | request) {
	case 0x8006:				/* GET_DESCRIPTOR, device */
		switch (value >> 8) {
		case 1:
			src = hub_device_desc;
			len = sizeof(hub_device_desc);
			break;
		case 2:
			src = hub_config_desc;
			len = sizeof(hub_config_desc);
			break;
		case 3:
			if ((value & 0xff) == 0) {
				src = hub_lang_desc;
				len = sizeof(hub_lang_desc);
			} else {
				len = USBStringDesc((value & 0xff) == 1 ?
					"SheepShaver" : "USB Root Hub", str);
				src = str;
			}
			break;
		default:
			return -1;
		}
		break;

	case 0xa006:				/* GET_DESCRIPTOR, hub class */
		src = hub_class_desc;
		len = sizeof(hub_class_desc);
		break;

	case 0x0005:				/* SET_ADDRESS */
		root_hub_addr = (uint8)(value & 0x7f);
		USBHIDLog("  root hub address := %u", root_hub_addr);
		return 0;

	case 0x0009:				/* SET_CONFIGURATION */
	case 0x000b:				/* SET_INTERFACE */
	case 0x0001:				/* CLEAR_FEATURE, device */
	case 0x0003:				/* SET_FEATURE, device */
		return 0;

	case 0x8008:				/* GET_CONFIGURATION */
		buf[0] = 1;
		return 1;

	case 0x8000:				/* GET_STATUS, device */
	case 0x8100:				/* GET_STATUS, interface */
	case 0x8200:				/* GET_STATUS, endpoint */
		buf[0] = 0;
		buf[1] = 0;
		return max < 2 ? max : 2;

	case 0xa000:				/* GET_STATUS, hub */
		buf[0] = buf[1] = buf[2] = buf[3] = 0;
		return max < 4 ? max : 4;

	case 0xa300:				/* GET_STATUS, port */
		if (index != 1)
			return -1;
		buf[0] = (uint8)port_status;
		buf[1] = (uint8)(port_status >> 8);
		buf[2] = (uint8)port_change;
		buf[3] = (uint8)(port_change >> 8);
		return max < 4 ? max : 4;

	case 0x2303:				/* SET_FEATURE, port */
		if (index != 1)
			return -1;
		switch (value) {
		case PORT_POWER:
			/* Powering the port is what makes the device on it visible: current
			   connect status (bit 0), powered (bit 8) and low speed (bit 9), with
			   the connect-change bit set, which is what the hub driver's
			   status-change pipe is waiting for. */
			port_status |= 0x0301;
			port_change |= 1 << (C_PORT_CONNECTION - 16);
			break;
		case PORT_RESET:
			/* A reset completes at once here and, per the hub class, leaves
			   the port enabled with the reset change bit set. */
			port_status |= 0x0002;
			port_change |= 1 << (C_PORT_RESET - 16);
			break;
		case PORT_SUSPEND:
			port_status |= 0x0004;
			break;
		default:
			return -1;
		}
		USBHIDLog("  port SET_FEATURE %u -> status=%04x change=%04x",
			value, port_status, port_change);
		return 0;

	case 0x2301:				/* CLEAR_FEATURE, port */
		if (index != 1)
			return -1;
		switch (value) {
		case PORT_ENABLE:
			port_status &= ~0x0002;
			break;
		case PORT_SUSPEND:
			port_status &= ~0x0004;
			break;
		case PORT_POWER:
			port_status &= ~0x0100;
			break;
		default:
			if (value >= C_PORT_CONNECTION && value <= C_PORT_RESET)
				port_change &= ~(1 << (value - 16));
			else
				return -1;
			break;
		}
		USBHIDLog("  port CLEAR_FEATURE %u -> status=%04x change=%04x",
			value, port_status, port_change);
		return 0;

	default:
		USBHIDLog("  unhandled device request %02x %02x value=%04x index=%04x",
			type, request, value, index);
		return -1;
	}

	if (len > max)
		len = max;
	memcpy(buf, src, len);
	return len;
}

/* kUSBPending: what an endpoint with nothing to report leaves a transfer as.
   The hub's status-change pipe stays outstanding until a port actually
   changes, which is the whole point of it. */
enum { kUSBEndpointStalled = -6601 };

/*
 *  Which device answers. Only one device is ever at address 0 at a time, and the
 *  hub gets there first: it is addressed before its port is even powered, so
 *  until root_hub_addr is set, address 0 is the hub. After that, address 0 is
 *  whatever has just been plugged into the port. Same rule as on a wire.
 */
static int USBHubAddressed(uint32 fa)
{
	return root_hub_addr == 0 || fa == root_hub_addr;
}

static int USBRequestFor(uint32 fa, const uint8 *req, uint8 *buf, int max)
{
	int len = 0;

	if (USBHubAddressed(fa))
		return USBRootHubRequest(req, buf, max);
	/* The device on the port is the HID joystick usbhid.cpp already describes -
	   one device definition whichever transport presents it. Its interface is
	   class 3, subclass 0, protocol 0, which is exactly what the first driver
	   description in Apple's USB HID Driver extension matches. */
	if (USBHIDDeviceControl(req, buf, &len, 1) < 0)
		return -1;
	return len > max ? max : len;
}

static uint32 USBRootHubControl(uint32 refcon, uint32 completion, uint32 buffer,
	uint32 fa, uint32 length, uint32 dir)
{
	uint8 data[256];
	uint8 *setup;
	int n;

	if (fa >= USB_MAX_FUNCTIONS)
		return (uint32)kUIMUnimplemented;
	setup = usb_setup[fa];

	if (dir == USB_DIR_NONE) {
		/* Setup stage: latch the request and act on the ones with no data. */
		Mac2Host_memcpy(setup, buffer, 8);
		USBHIDLog("  setup fa=%u %02x %02x %02x%02x %02x%02x %02x%02x",
			fa, setup[0], setup[1], setup[3], setup[2], setup[5], setup[4],
			setup[7], setup[6]);
		if ((setup[6] | setup[7]) == 0) {
			n = USBRequestFor(fa, setup, data, 0);
			if (n < 0) {
				USBUIMComplete(completion, refcon, kUSBEndpointStalled, 0);
				return 0;
			}
		}
		USBUIMComplete(completion, refcon, 0, 8);
		return 0;
	}

	if (length == 0) {
		/* Status stage. */
		USBUIMComplete(completion, refcon, 0, 0);
		return 0;
	}

	if (dir == USB_DIR_IN) {
		n = USBRequestFor(fa, setup, data,
			length > sizeof(data) ? (int)sizeof(data) : (int)length);
		if (n < 0) {
			USBUIMComplete(completion, refcon, kUSBEndpointStalled, 0);
			return 0;
		}
		Host2Mac_memcpy(buffer, data, n);
		USBUIMComplete(completion, refcon, 0, (uint32)n);
		return 0;
	}

	/* OUT data stage - nothing the hub takes a payload for. */
	USBUIMComplete(completion, refcon, 0, length);
	return 0;
}

/*
 *  The hub's status-change endpoint. One outstanding transfer at a time, held
 *  until a port change appears; the hub driver re-arms it from its own
 *  completion, so this is the pipe the whole hub runs on.
 */
static uint32 hub_int_refcon;
static uint32 hub_int_completion;
static uint32 hub_int_buffer;
static uint32 hub_int_length;

/* And the gamepad's report pipe, held the same way until an input changes. */
static uint32 pad_int_refcon;
static uint32 pad_int_completion;
static uint32 pad_int_buffer;
static uint32 pad_int_length;

static uint32 USBInterruptTransfer(uint32 refcon, uint32 completion,
	uint32 buffer, uint32 fa, uint32 length)
{
	if (USBHubAddressed(fa)) {
		hub_int_refcon = refcon;
		hub_int_completion = completion;
		hub_int_buffer = buffer;
		hub_int_length = length;
		if (port_change)
			USBUIMPortChanged();
		return 0;
	}
	pad_int_refcon = refcon;
	pad_int_completion = completion;
	pad_int_buffer = buffer;
	pad_int_length = length;
	return 0;
}

/*
 *  Deliver a joystick report on the device's interrupt IN pipe.
 *
 *  A HID device with no idle rate set reports only when something changes, so
 *  the held transfer is completed only when the packed report differs from the
 *  one last delivered. Called from slot 21, like every other completion.
 */
static void USBPadReport(void)
{
	uint8 report[32];
	static uint8 last[32];
	static int last_len;
	uint32 completion = pad_int_completion;
	int n;

	if (completion == 0 || pad_int_length == 0)
		return;
	n = USBHIDDeviceInterruptIn(report, (int)sizeof(report));
	if (n <= 0)
		return;
	if (n == last_len && memcmp(report, last, n) == 0)
		return;
	memcpy(last, report, n);
	last_len = n;
	if (n > (int)pad_int_length)
		n = (int)pad_int_length;
	pad_int_completion = 0;
	Host2Mac_memcpy(pad_int_buffer, report, n);
	USBUIMComplete(completion, pad_int_refcon, 0, (uint32)n);
}

/*
 *  Report the one downstream port as changed. Bit 1 of the status-change byte
 *  is port 1 (bit 0 is the hub itself), so a single byte covers this hub.
 */
void USBUIMPortChanged(void)
{
	uint32 completion = hub_int_completion;
	uint32 refcon = hub_int_refcon;

	if (completion == 0 || hub_int_length == 0)
		return;
	hub_int_completion = 0;
	WriteMacInt8(hub_int_buffer, 0x02);
	USBHIDLog("  hub status change reported (status=%04x change=%04x)",
		port_status, port_change);
	USBUIMComplete(completion, refcon, 0, 1);
}

enum {
	UIM_INITIALIZE = 0,
	UIM_FINALIZE = 1,
	UIM_CREATE_CONTROL_ENDPOINT = 2,
	UIM_CREATE_CONTROL_TRANSFER = 3,
	UIM_CREATE_BULK_ENDPOINT = 6,
	UIM_CREATE_BULK_TRANSFER = 7,
	UIM_CREATE_INTERRUPT_ENDPOINT = 10,
	UIM_CREATE_INTERRUPT_TRANSFER = 11,
	UIM_CREATE_ISOCH_ENDPOINT = 14,
	UIM_CREATE_ISOCH_TRANSFER = 15,
	UIM_ABORT_ENDPOINT = 18,
	UIM_DELETE_ENDPOINT = 19,
	/* Named from the ROM UIM and from USL's use of them:
	     21  poll this bus - no arguments; USL calls it for all 16 buses in
	         USBIdleTask's pass (USL code 0x3c8c)
	     23  interrupt-status poll, from USLPolledProcessDoneQueue (0x3ce4)
	     24  UIMGetFrameNumber(UnsignedWide *) - ROM UIM code 0x32d0 reads
	         HcFmNumber, adds its 64-bit accumulator at globals+0x480 and stores
	         the pair; USL takes the low word as the current frame (0x3dc8) */
	UIM_POLL_BUS = 21,
	UIM_START_BUS = 22,
	UIM_POLL_INTERRUPT = 23,
	UIM_GET_FRAME_NUMBER = 24
};

/* What slot 0 reports. Setting it to kUIMUnimplemented was how the Expert was
   made to name our code in its own log:
     USBInitializeUIM: Error Returned From UIM Initialization Function. -6600
     LoadUIM: LoadOrReplacePlugin failed! -6600
   which is what proves this really is UIMInitialize being reached through the
   Expert's normal LoadUIM path. */
#define UIM_INIT_RESULT 0

uint32 USBUIMDispatch(uint32 slot, uint32 table, uint32 caller, uint32 r3,
	uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9,
	uint32 r10)
{
	/* Slots 21 and 24 are the polled heartbeat - USBIdleTask reaches them for
	   every bus on every pass - so only the opening few are worth a line. */
	static int heartbeat;

	if (slot != UIM_POLL_BUS && slot != UIM_GET_FRAME_NUMBER)
		USBHIDLog("UIM slot %u tbl=%08x from=%08x (%08x %08x %08x %08x %08x %08x %08x %08x)",
			slot, table, caller, r3, r4, r5, r6, r7, r8, r9, r10);
	else if (heartbeat < 4) {
		heartbeat++;
		USBHIDLog("UIM slot %u (heartbeat) from=%08x r3=%08x", slot, caller, r3);
	}

	switch (slot) {
	case UIM_INITIALIZE:
		uim_seen_init = 1;
		USBUIMFindExpertStatus();
		// UIMInitialize(busRef, flags, refcon). A UIM does not register
		// itself: the ROM's own OHCI UIM imports no USB library at all, so
		// USBAddBus is called by the Expert once this returns noErr. And
		// unlike that UIM there is no PCI configuration space to program and
		// no controller registers to set up - the controller is us.
		return UIM_INIT_RESULT;

	case UIM_FINALIZE:
		return 0;

	case UIM_CREATE_CONTROL_ENDPOINT:
		// (functionAddress, endpoint, maxPacketSize, speed). We keep no
		// endpoint state: every transfer carries the address and endpoint, so
		// accepting the endpoint is all that is required.
		USBHIDLog("  CreateControlEndpoint fa=%u ep=%u maxPkt=%u speed=%u",
			r3, r4, r5, r6);
		return 0;

	case UIM_CREATE_CONTROL_TRANSFER:
		// (refcon, completion, buffer, rounding, fa, endpoint, length, dir).
		return USBRootHubControl(r3, r4, r5, r7, r9, r10);

	case UIM_CREATE_INTERRUPT_ENDPOINT:
		// (fa, endpoint, ?, pollingInterval, maxPacketSize, direction). The ROM
		// UIM answers 0 straight away when fa is the root hub (code 0x1e58), and
		// there is no hardware here for any address, so accept them all.
		USBHIDLog("  CreateInterruptEndpoint fa=%u ep=%u interval=%u maxPkt=%u dir=%u",
			r3, r4, r6, r7, r8);
		return 0;

	case UIM_CREATE_INTERRUPT_TRANSFER:
		// NOT the same argument order as slot 3: the function address and endpoint
		// come first here, and refcon/completion/buffer shift along behind them.
		// The ROM UIM stores r6 at TD+0x10 and r5 at TD+0x14 and hands r7/r9 to
		// PrepareMemoryForIO (UIM code 0x2168, 0x21c0), and its root hub branch
		// compares r3 with the root hub address (0x2064).
		//   (fa, ep, refcon, completion, buffer, rounding, length, direction)
		// Leaving the transfer outstanding is what an endpoint with nothing to
		// report does; the hub's is completed from USBUIMPortChanged().
		return USBInterruptTransfer(r5, r6, r7, r3, r9);

	case UIM_ABORT_ENDPOINT:
	case UIM_DELETE_ENDPOINT:
		return 0;

	case UIM_GET_FRAME_NUMBER: {
		// UnsignedWide, so hi then lo. USB frames are 1 kHz; the host clock is
		// the only clock here, and USL only ever compares these against each
		// other. Returning without writing left it reading its own uninitialised
		// stack as the current frame.
		uint64 frame = GetTicks_usec() / 1000;
		WriteMacInt32(r3, (uint32)(frame >> 32));
		WriteMacInt32(r3 + 4, (uint32)frame);
		return 0;
	}

	case UIM_POLL_BUS:
		// Called for every bus on every pass of USL's own idle work, with no
		// arguments: a polled UIM's service entry. Transfer completions belong
		// here, and so does a port that has changed since the last look.
		if (port_change)
			USBUIMPortChanged();
		USBPadReport();
		USBUIMRunCompletions();
		return 0;

	case UIM_CREATE_BULK_ENDPOINT:
	case UIM_CREATE_BULK_TRANSFER:
	case UIM_CREATE_ISOCH_ENDPOINT:
	case UIM_CREATE_ISOCH_TRANSFER:
		// The joystick has neither, and refusing is honest: the family will
		// not offer bulk or isochronous pipes for a device that has none.
		return (uint32)kUIMUnimplemented;

	default:
		// Everything else - the transfer entries and the root hub group - is
		// still being characterised. Accept it so the family keeps walking its
		// bring-up sequence and the log shows the arguments of whatever it
		// reaches next, which is how the previous slots were pinned down.
		return 0;
	}
}

/*
 *  A guest reset takes the USB family's globals with it.
 *
 *  Every guest address above was resolved from the running system: the Expert's
 *  notification and idle TVectors, USBIdleTask, the status buffer, the routines
 *  of queued completions. After a reset those addresses still point into the
 *  ROM, so a call does not fail - it runs ROM code against globals that no
 *  longer exist, which is the host access violation seen inside the guest call
 *  from USBUIMPoll. Forget all of it, and the bus state with it; the node is
 *  republished from the idle loop after the restart and USBUIMRegisterBus
 *  resolves everything again.
 */
void USBUIMReset(void)
{
	uim_seen_init = 0;
	uim_status_buf = 0;
	memset(uim_status_seen, 0, sizeof(uim_status_seen));
	usb_done_in = usb_done_out = 0;
	usb_expert_notify = 0;
	usb_expert_pending = 0;
	usb_expert_idle = 0;
	usb_expert_drain_until = 0;
	usb_idle_task = 0;
	usb_next_by_class = 0;
	usb_probe_at = 0;
	usb_new_in = usb_new_out = 0;
	memset(usb_setup, 0, sizeof(usb_setup));
	root_hub_addr = 0;
	port_status = 0;
	port_change = 0;
	hub_int_refcon = hub_int_completion = hub_int_buffer = hub_int_length = 0;
	pad_int_refcon = pad_int_completion = pad_int_buffer = pad_int_length = 0;
}


#else /* !ENABLE_USB */

uint32 USBUIMDispatch(uint32 slot, uint32 table, uint32 caller, uint32 r3,
	uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9,
	uint32 r10)
{
	(void)slot; (void)table; (void)caller; (void)r3; (void)r4; (void)r5;
	(void)r6; (void)r7; (void)r8; (void)r9; (void)r10;
	return 0;
}
uint32 USBUIMExpertNotify(uint32 record) { (void)record; return 0; }
void USBUIMPortChanged(void) { }
void USBUIMPoll(void) { }
void USBUIMSampleGuest(void) { }
void USBUIMReset(void) { }
void USBUIMRegisterBus(uint32 node) { (void)node; }
const uint8 *USBUIMDriver(uint32 *size)
{
	if (size)
		*size = 0;
	return NULL;
}

#endif /* ENABLE_USB */