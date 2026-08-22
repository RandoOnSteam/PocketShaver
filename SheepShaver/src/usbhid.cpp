/*
 *  usbhid.cpp - OHCI host controller with one HID joystick
 *
 *  The Mac OS ROM's OHCIUIM matches pciclass,0c0310. We put that
 *  controller in the Name Registry and run the OHCI register file
 *  plus a single full-speed HID joystick on the root hub. Enumeration,
 *  HIDLib and InputSprocket HID are the guest's.
 *
 *  The register file is a trapping MMIO window (mmio.h), not a page of RAM:
 *  write-1-to-clear registers, doorbells and read-time state only work if the
 *  access is seen when it happens. Every entry point below therefore runs
 *  either in the faulting guest's own context or on the VBL, never on a
 *  helper thread - the endpoint and transfer descriptors we walk are guest
 *  memory that the guest owns.
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
#include "usbhid.h"
#include "mmio.h"
#include "joymanager.h"
#include "main.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#endif

#define DEBUG 0
#include "debug.h"

/* Per-access register and transfer tracing. Off by default: every line is
   flushed, and with the window trapping there is one call per guest load or
   store, so a driver that polls a register would spend all its time in
   fopen/fflush. Turn it on to watch a UIM bring the controller up. */
#define USBHID_TRACE 0
#if USBHID_TRACE
#define USBHIDTrace USBHIDLog
#else
static inline void USBHIDTrace(const char *, ...) { }
#endif

static FILE *usb_logf;

void USBHIDLog(const char *fmt, ...)
{
	va_list ap;
	uint64 t;

	if (usb_logf == NULL) {
#ifdef WIN32
		char path[MAX_PATH], *slash;
		if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0)
			strcpy(path, "usbhid.log");
		else {
			slash = strrchr(path, '\\');
			if (slash)
				strcpy(slash + 1, "usbhid.log");
			else
				strcpy(path, "usbhid.log");
		}
		usb_logf = fopen(path, "w");
#else
		usb_logf = fopen("usbhid.log", "w");
#endif
		if (usb_logf == NULL)
			return;
		fprintf(usb_logf, "usbhid log opened\n");
		fflush(usb_logf);
	}
	/* Self-contained clock on purpose: the first devices are installed before
	   timer_init() runs, and GetTicks_usec() divides by a frequency that is
	   still zero at that point. */
#ifdef WIN32
	{
		static LARGE_INTEGER freq, start;
		LARGE_INTEGER now;
		if (freq.QuadPart == 0) {
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&start);
			if (freq.QuadPart == 0)
				freq.QuadPart = 1;
		}
		QueryPerformanceCounter(&now);
		t = (uint64)((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart);
	}
#else
	t = 0;
#endif
	fprintf(usb_logf, "[%llu] ", (unsigned long long)t);
	va_start(ap, fmt);
	vfprintf(usb_logf, fmt, ap);
	va_end(ap);
	fputc('\n', usb_logf);
	fflush(usb_logf);
}

/* Everything below is the device itself. ENABLE_USB 0 replaces the lot with
   the no-op entry points at the end of the file. */
#ifdef ENABLE_USB

enum {
	OHCI_PORT0 = 0x54,
	OHCI_FRAMES_PER_VBL = 16,
	OHCI_ED_LIMIT = 32,
	OHCI_TD_LIMIT = 32
};

enum {
	OHCI_CTL_PLE = 1 << 2,
	OHCI_CTL_IE = 1 << 3,
	OHCI_CTL_CLE = 1 << 4,
	OHCI_CTL_BLE = 1 << 5,
	OHCI_CTL_HCFS = 0xc0,
	OHCI_USB_RESET = 0x00,
	OHCI_USB_RESUME = 0x40,
	OHCI_USB_OPERATIONAL = 0x80,
	OHCI_USB_SUSPEND = 0xc0
};

enum {
	OHCI_STATUS_HCR = 1 << 0,
	OHCI_STATUS_CLF = 1 << 1,
	OHCI_STATUS_BLF = 1 << 2
};

enum {
	OHCI_INTR_SO = 1u << 0,
	OHCI_INTR_WD = 1u << 1,
	OHCI_INTR_SF = 1u << 2,
	OHCI_INTR_RD = 1u << 3,
	OHCI_INTR_UE = 1u << 4,
	OHCI_INTR_FNO = 1u << 5,
	OHCI_INTR_RHSC = 1u << 6,
	OHCI_INTR_OC = 1u << 30,
	OHCI_INTR_MIE = 1u << 31
};

enum {
	OHCI_PORT_CCS = 1 << 0,
	OHCI_PORT_PES = 1 << 1,
	OHCI_PORT_PSS = 1 << 2,
	OHCI_PORT_PRS = 1 << 4,
	OHCI_PORT_PPS = 1 << 8,
	OHCI_PORT_LSDA = 1 << 9,
	OHCI_PORT_CSC = 1 << 16,
	OHCI_PORT_PESC = 1 << 17,
	OHCI_PORT_PSSC = 1 << 18,
	OHCI_PORT_OCIC = 1 << 19,
	OHCI_PORT_PRSC = 1 << 20,
	OHCI_PORT_WTC = OHCI_PORT_CSC | OHCI_PORT_PESC |
		OHCI_PORT_PSSC | OHCI_PORT_OCIC | OHCI_PORT_PRSC
};

enum {
	OHCI_RHS_LPS = 1u << 0,
	OHCI_RHS_DRWE = 1u << 15,
	OHCI_RHS_LPSC = 1u << 16,
	OHCI_RHS_OCIC = 1u << 17,
	OHCI_RHS_CRWE = 1u << 31
};

enum {
	OHCI_ED_FA_MASK = 0x7f,
	OHCI_ED_EN_SHIFT = 7,
	OHCI_ED_D_SHIFT = 11,
	OHCI_ED_D_MASK = 3 << 11,
	OHCI_ED_K = 1 << 14,
	OHCI_ED_F = 1 << 15,
	OHCI_ED_H = 1,
	OHCI_ED_C = 2,
	OHCI_DPTR_MASK = 0xfffffff0u
};

enum {
	OHCI_TD_R = 1 << 18,
	OHCI_TD_DP_SHIFT = 19,
	OHCI_TD_DI_SHIFT = 21,
	OHCI_TD_T_SHIFT = 24,
	OHCI_TD_T_CARRY = 1 << 25,	/* T[1] == 0: take the toggle from ED.C */
	OHCI_TD_T_VALUE = 1 << 24,
	OHCI_TD_CC_SHIFT = 28,
	OHCI_TD_DIR_SETUP = 0,
	OHCI_TD_DIR_OUT = 1,
	OHCI_TD_DIR_IN = 2,
	OHCI_CC_NOERROR = 0,
	OHCI_CC_STALL = 4,
	OHCI_CC_DEVICENOTRESPONDING = 5,
	OHCI_CC_DATAUNDERRUN = 9
};

enum {
	/* X and Y at 16 bits, the hat in a nibble with a nibble of padding, then one
	   bit per button: 2*16 + 4 + 4 + 16 = 56 bits. Must match
	   kJoystickReportDesc below exactly - the guest lays the report out from
	   that descriptor and reads whatever the bit offsets land on. */
	kHIDReportBytes = 7,
	kHIDMaxButtons = 16
};

static const uint8 kJoystickReportDesc[] = {
	0x05, 0x01,
	0x09, 0x04,
	0xa1, 0x01,
	0x09, 0x30, 0x09, 0x31,
	0x15, 0x00,			/* Logical Minimum 0 */
	0x27, 0xff, 0xff, 0x00, 0x00,	/* Logical Maximum 65535 */
	0x75, 0x10,
	0x95, 0x02,
	0x81, 0x02,
	0x09, 0x39,
	0x15, 0x01,			/* Logical Minimum 1 */
	0x25, 0x08,			/* Logical Maximum 8 */
	0x75, 0x04,
	0x95, 0x01,
	0x81, 0x42,			/* Input (Data, Var, Abs, Null state) */
	0x75, 0x04,			/* pad the hat out to a byte */
	0x95, 0x01,
	0x81, 0x01,
	0x05, 0x09,
	0x19, 0x01,
	0x29, 0x10,
	0x15, 0x00,
	0x25, 0x01,
	0x75, 0x01,
	0x95, 0x10,
	0x81, 0x02,
	0x75, 0x04,			/* pad to a byte boundary */
	0x95, 0x01,
	0x81, 0x01,
	0xc0
};

/* idVendor 0x106b is Apple's, which is what lets Apple System Profiler name the
   vendor at all. idProduct is 'SH' rather than 1, so it cannot be read as a
   shipping Apple product and matches none of the vendor-specific drivers inside
   USB HID Driver - those target Logitech, ThrustMaster, Guillemot, ACT Labs and
   AVB IDs. The generic class 3/0/0 USBHIDDriver is the one that must claim it. */
static const uint8 kDevDesc[] = {
	18, 1, 0x10, 0x01, 0, 0, 0, 8,
	0x6b, 0x10, 0x48, 0x53, 0x00, 0x01, 1, 2, 0, 1
};

static const uint8 kCfgDesc[] = {
	9, 2, 34, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 1, 3, 0, 0, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22,
	(uint8)sizeof(kJoystickReportDesc),
	(uint8)(sizeof(kJoystickReportDesc) >> 8),
	7, 5, 0x81, 3, 16, 0, 8
};

static uint32 usb_ctl, usb_status, usb_intr, usb_intr_en;
static uint32 usb_hcca, usb_ctrl_head, usb_ctrl_cur;
static uint32 usb_bulk_head, usb_bulk_cur, usb_period_cur, usb_done;
static uint32 usb_fm_interval, usb_periodic_start, usb_ls_thresh;
static uint32 usb_rhdesc_a, usb_rhdesc_b, usb_rhstatus, usb_port0;
static uint16 usb_frame;
static int usb_done_count;
static uint8 usb_dev_addr;
static uint8 usb_dev_cfg;
static uint8 usb_hid_report[kHIDReportBytes];
static bool usb_installed;

static uint32 ReadMacInt32LE(uint32 addr)
{
	return (uint32)ReadMacInt8(addr)
		| ((uint32)ReadMacInt8(addr + 1) << 8)
		| ((uint32)ReadMacInt8(addr + 2) << 16)
		| ((uint32)ReadMacInt8(addr + 3) << 24);
}

static void WriteMacInt32LE(uint32 addr, uint32 v)
{
	WriteMacInt8(addr, (uint8)v);
	WriteMacInt8(addr + 1, (uint8)(v >> 8));
	WriteMacInt8(addr + 2, (uint8)(v >> 16));
	WriteMacInt8(addr + 3, (uint8)(v >> 24));
}

static void WriteMacInt16LE(uint32 addr, uint16 v)
{
	WriteMacInt8(addr, (uint8)v);
	WriteMacInt8(addr + 1, (uint8)(v >> 8));
}

static void hid_pack_report(void)
{
	JoyManagerDevice *js;
	int16 x = 0, y = 0;
	uint16 ux, uy;
	uint8 hat = 0;
	uint16 buttons = 0;
	int i, nb;

	memset(usb_hid_report, 0, sizeof(usb_hid_report));
	if (!JoyManagerInit())
		goto store;
	js = JoyManagerOpenDevice(0);
	if (js == NULL)
		goto store;
	x = JoyManagerAxis(js, 0);
	y = JoyManagerAxis(js, 1);
	nb = JoyManagerNumButtons(js);
	if (nb > kHIDMaxButtons)
		nb = kHIDMaxButtons;
	for (i = 0; i < nb; i++) {
		if (JoyManagerButton(js, i))
			buttons |= (uint16)(1u << i);
	}
	/* 0 centred, 1..8 for the directions - the rose InputSprocket uses. */
	if (JoyManagerNumHats(js) > 0)
		hat = (uint8)JoyManagerHatPosition(JoyManagerHat(js, 0));
	{
		static uint64 next_log;
		if (GetTicks_usec() >= next_log) {
			next_log = GetTicks_usec() + 1000000;
			USBHIDLog("joy raw: axes=%d hats=%d buttons=%d"
				" x=%d y=%d hat=%02x btn=%04x",
				JoyManagerNumAxes(js), JoyManagerNumHats(js),
				JoyManagerNumButtons(js), x, y,
				JoyManagerNumHats(js) > 0 ? JoyManagerHat(js, 0) : 0xff,
				buttons);
		}
	}
	JoyManagerCloseDevice(js);
store:
	/* Little endian, as every HID report is, and biased into the unsigned
	   0..65535 the descriptor declares: rest is 0x8000. */
	ux = (uint16)((int32)x + 32768);
	uy = (uint16)((int32)y + 32768);
	usb_hid_report[0] = (uint8)ux;
	usb_hid_report[1] = (uint8)(ux >> 8);
	usb_hid_report[2] = (uint8)uy;
	usb_hid_report[3] = (uint8)(uy >> 8);
	usb_hid_report[4] = (uint8)(hat & 0x0f);
	usb_hid_report[5] = (uint8)buttons;
	usb_hid_report[6] = (uint8)(buttons >> 8);
}

static int hid_copy_desc(uint8 type, uint8 index, uint8 *dst, int max)
{
	static const uint8 str0[] = { 4, 3, 0x09, 0x04 };
	static const uint8 str1[] = {
		24, 3,
		'S', 0, 'h', 0, 'e', 0, 'e', 0, 'p', 0, 'S', 0,
		'h', 0, 'a', 0, 'v', 0, 'e', 0, 'r', 0
	};
	static const uint8 str2[] = {
		26, 3,
		'H', 0, 'I', 0, 'D', 0, ' ', 0,
		'J', 0, 'o', 0, 'y', 0, 's', 0, 't', 0, 'i', 0, 'c', 0, 'k', 0
	};
	const uint8 *src = NULL;
	int n = 0;

	if (type == 1) {
		src = kDevDesc;
		n = sizeof(kDevDesc);
	} else if (type == 2) {
		src = kCfgDesc;
		n = sizeof(kCfgDesc);
	} else if (type == 3) {
		if (index == 0) {
			src = str0;
			n = sizeof(str0);
		} else if (index == 1) {
			src = str1;
			n = sizeof(str1);
		} else if (index == 2) {
			src = str2;
			n = sizeof(str2);
		}
	} else if (type == 0x22) {
		src = kJoystickReportDesc;
		n = sizeof(kJoystickReportDesc);
	}
	if (src == NULL)
		return -1;
	if (n > max)
		n = max;
	memcpy(dst, src, n);
	return n;
}

static int hid_control(const uint8 setup[8], uint8 *data, int *len, int dir_in)
{
	uint8 type = setup[0], req = setup[1];
	uint16 value = (uint16)setup[2] | ((uint16)setup[3] << 8);
	uint16 index = (uint16)setup[4] | ((uint16)setup[5] << 8);
	uint16 wlen = (uint16)setup[6] | ((uint16)setup[7] << 8);

	USBHIDTrace("hid_control type=%02x req=%u value=%04x index=%04x wlen=%u dir_in=%d",
		type, req, value, index, wlen, dir_in);
	if (req == 5 && type == 0x00) {
		usb_dev_addr = (uint8)(value & 0x7f);
		*len = 0;
		return 0;
	}
	if (req == 9 && type == 0x00) {
		usb_dev_cfg = (uint8)value;
		*len = 0;
		return 0;
	}
	if (req == 8 && type == 0x80) {
		if (wlen < 1)
			return -1;
		data[0] = usb_dev_cfg;
		*len = 1;
		return 0;
	}
	if (req == 0 && (type & 0x7f) == 0x00) {
		data[0] = data[1] = 0;
		*len = (wlen < 2) ? (int)wlen : 2;
		return 0;
	}
	if (req == 6 && (type & 0x80)) {
		int n = hid_copy_desc((uint8)(value >> 8), (uint8)value,
			data, wlen);
		if (n < 0)
			return -1;
		*len = n;
		return 0;
	}
	if (req == 10 && type == 0x21) {
		*len = 0;
		return 0;
	}
	if (req == 1 && type == 0xa1) {
		hid_pack_report();
		if (wlen > kHIDReportBytes)
			wlen = kHIDReportBytes;
		memcpy(data, usb_hid_report, wlen);
		*len = wlen;
		return 0;
	}
	*len = 0;
	return 0;
}

static int hid_interrupt_in(uint8 *data, int max)
{
	int n = kHIDReportBytes;

	hid_pack_report();
	if (n > max)
		n = max;
	memcpy(data, usb_hid_report, n);
	return n;
}

/*
 *  The device itself, shared with the UIM in usbuim.cpp. There is one HID
 *  joystick, described once: its descriptors, its class requests and its report
 *  packing live here, and whichever transport presents it - the OHCI register
 *  file below or the software UIM - asks these two entries.
 */
int USBHIDDeviceControl(const uint8 setup[8], uint8 *data, int *len, int dir_in)
{
	return hid_control(setup, data, len, dir_in);
}

int USBHIDDeviceInterruptIn(uint8 *data, int max)
{
	return hid_interrupt_in(data, max);
}

static void ohci_soft_reset(void)
{
	USBHIDLog("ohci_soft_reset");
	usb_ctl = (usb_ctl & 0x100) | OHCI_USB_SUSPEND;
	usb_status = 0;
	usb_intr = 0;
	usb_intr_en = 0;
	usb_hcca = 0;
	usb_ctrl_head = usb_ctrl_cur = 0;
	usb_bulk_head = usb_bulk_cur = 0;
	usb_period_cur = 0;
	usb_done = 0;
	usb_done_count = 7;
	usb_fm_interval = 0x27782edf;
	usb_periodic_start = 0;
	usb_ls_thresh = 0x628;
	usb_frame = 0;
}

static void ohci_hard_reset(void)
{
	USBHIDLog("ohci_hard_reset");
	ohci_soft_reset();
	usb_ctl = 0;
	/* One downstream port, ports always powered (NPS), POTPGT 1. */
	usb_rhdesc_a = 0x01000201;
	usb_rhdesc_b = 0;
	usb_rhstatus = 0;
	/* The joystick is soldered on: the port comes up connected, and the
	   connect-status change is latched so the first root hub poll sees it. */
	usb_port0 = OHCI_PORT_CCS | OHCI_PORT_CSC | OHCI_PORT_PPS;
	usb_dev_addr = 0;
	usb_dev_cfg = 0;
}

static void ohci_port_write(uint32 val)
{
	if (val & OHCI_PORT_WTC)
		usb_port0 &= ~(val & OHCI_PORT_WTC);
	if (val & OHCI_PORT_CCS)		/* ClearPortEnable */
		usb_port0 &= ~OHCI_PORT_PES;
	if ((val & OHCI_PORT_PES) && (usb_port0 & OHCI_PORT_CCS))
		usb_port0 |= OHCI_PORT_PES;
	if ((val & OHCI_PORT_PRS) && (usb_port0 & OHCI_PORT_CCS)) {
		/* Reset completes within the fault, as a real controller would
		   within 10 ms: the device drops back to address 0. */
		usb_dev_addr = 0;
		usb_dev_cfg = 0;
		usb_port0 &= ~OHCI_PORT_PRS;
		usb_port0 |= OHCI_PORT_PES | OHCI_PORT_PRSC;
		usb_intr |= OHCI_INTR_RHSC;
	}
	if (val & OHCI_PORT_PPS)
		usb_port0 |= OHCI_PORT_PPS;
}

static uint32 ohci_reg_read(uint32 off)
{
	switch (off) {
	case 0x00: return 0x10;			/* HcRevision: OHCI 1.0 */
	case 0x04: return usb_ctl;
	case 0x08: return usb_status;
	case 0x0c: return usb_intr;
	case 0x10:
	case 0x14: return usb_intr_en;
	case 0x18: return usb_hcca;
	case 0x1c: return usb_period_cur;
	case 0x20: return usb_ctrl_head;
	case 0x24: return usb_ctrl_cur;
	case 0x28: return usb_bulk_head;
	case 0x2c: return usb_bulk_cur;
	case 0x30: return usb_done;
	case 0x34: return usb_fm_interval;
	case 0x38: {
		/* HcFmRemaining counts down across the 1 ms frame. A driver may spin
		   on it, so take it from the host clock rather than reporting a
		   constant that would never move. FrameRemainingToggle (bit 31)
		   follows FrameIntervalToggle, as it does after each SOF. */
		uint32 interval = usb_fm_interval & 0x3fff;
		uint32 usec = (uint32)(GetTicks_usec() % 1000);
		uint32 remaining = interval - interval * usec / 1000;
		return remaining | (usb_fm_interval & 0x80000000u);
	}
	case 0x3c: return usb_frame;
	case 0x40: return usb_periodic_start;
	case 0x44: return usb_ls_thresh;
	case 0x48: return usb_rhdesc_a;
	case 0x4c: return usb_rhdesc_b;
	case 0x50: return usb_rhstatus;
	case OHCI_PORT0: return usb_port0;
	default: return 0;
	}
}

static void ohci_reg_write(uint32 off, uint32 val)
{
	uint32 old_state, new_state;

	switch (off) {
	case 0x04:
		old_state = usb_ctl & OHCI_CTL_HCFS;
		usb_ctl = val;
		new_state = usb_ctl & OHCI_CTL_HCFS;
		USBHIDLog("HcControl %08x hcfs %02x -> %02x", val, old_state, new_state);
		if (old_state != new_state && new_state == OHCI_USB_RESET)
			ohci_hard_reset();
		break;
	case 0x08:
		/* HCR self-clears; the list-filled bits are doorbells serviced on
		   the next frame, not inside the guest's faulting instruction. */
		if (val & OHCI_STATUS_HCR) {
			ohci_soft_reset();
			break;
		}
		usb_status |= val & (OHCI_STATUS_CLF | OHCI_STATUS_BLF);
		break;
	case 0x0c:
		usb_intr &= ~val;		/* write 1 to clear */
		break;
	case 0x10:
		usb_intr_en |= val;
		break;
	case 0x14:
		usb_intr_en &= ~val;
		break;
	case 0x18:
		usb_hcca = val & 0xffffff00u;
		/* The first HCCA write is the milestone: reaching it means the UIM
		   got past ExpMgrConfigWriteWord and is really initialising us. */
		USBHIDLog("HcHCCA = %08x", usb_hcca);
		break;
	case 0x20:
		usb_ctrl_head = val & OHCI_DPTR_MASK;
		break;
	case 0x24:
		usb_ctrl_cur = val & OHCI_DPTR_MASK;
		break;
	case 0x28:
		usb_bulk_head = val & OHCI_DPTR_MASK;
		break;
	case 0x2c:
		usb_bulk_cur = val & OHCI_DPTR_MASK;
		break;
	case 0x34:
		usb_fm_interval = val;
		break;
	case 0x40:
		usb_periodic_start = val & 0x3fff;
		break;
	case 0x44:
		usb_ls_thresh = val & 0xfff;
		break;
	case 0x48:
		/* NumberDownstreamPorts and PowerSwitchingMode are read-only. */
		usb_rhdesc_a = (usb_rhdesc_a & 0x000002ff) | (val & ~0x000002ffu);
		break;
	case 0x4c:
		usb_rhdesc_b = val;
		break;
	case 0x50:
		if (val & OHCI_RHS_LPS)			/* ClearGlobalPower */
			usb_port0 &= ~(OHCI_PORT_PPS | OHCI_PORT_PSS | OHCI_PORT_PRS);
		if (val & OHCI_RHS_LPSC)		/* SetGlobalPower */
			usb_port0 |= OHCI_PORT_PPS;
		if (val & OHCI_RHS_DRWE)
			usb_rhstatus |= OHCI_RHS_DRWE;
		if (val & OHCI_RHS_CRWE)
			usb_rhstatus &= ~OHCI_RHS_DRWE;
		if (val & OHCI_RHS_OCIC)
			usb_rhstatus &= ~OHCI_RHS_OCIC;
		break;
	case OHCI_PORT0:
		ohci_port_write(val);
		break;
	default:
		break;
	}
}

static int usb_first_accesses = 64;

/* True while the opening burst of accesses should be logged. */
static bool usb_note_access(void)
{
	if (usb_first_accesses <= 0)
		return false;
	if (usb_first_accesses == 64) {
		uint32 expandmem = ReadMacInt32(0x2b6);
		uint32 g = expandmem ? ReadMacInt32(expandmem + 0x234) : 0;
		USBHIDLog("ExpandMem=%08x ExpMgrGlobals=%08x", expandmem, g);
		if (g) {
			/* +0x04 heads the bus record list (0x58 bytes each, linked at
			   +0x54); +0x0c is the bus-number table built from "bus-range"
			   nodes. A config cycle is CallUniversalProc on record+0x34, so
			   an empty list is the whole reason the UIM's write fails. */
			uint32 rec = ReadMacInt32(g + 4);
			int n;
			USBHIDLog("  ExpMgr list=%08x bustable=%08x", rec, ReadMacInt32(g + 0xc));
			for (n = 0; rec && n < 8; n++) {
				USBHIDLog("  bus[%d] @%08x key=%08x cfgwr=%08x next=%08x", n, rec,
					ReadMacInt32(rec), ReadMacInt32(rec + 0x34),
					ReadMacInt32(rec + 0x54));
				rec = ReadMacInt32(rec + 0x54);
			}
		}
	}
	usb_first_accesses--;
	return true;
}

static uint32 ohci_window_read(uint32 off, int size)
{
	uint32 v = ohci_reg_read(off & ~3u);

	v >>= (off & 3) * 8;
	if (size < 4)
		v &= (1u << (size * 8)) - 1;
	if (usb_note_access()) {
		USBHIDLog("rd  %02x.%d = %08x", off, size, v);
	} else
		USBHIDTrace("rd  %02x.%d = %08x", off, size, v);
	return v;
}

static void ohci_window_write(uint32 off, int size, uint32 val)
{
	uint32 word = off & ~3u;
	int shift;

	if (usb_note_access()) {
		USBHIDLog("wr  %02x.%d = %08x ctl=%08x st=%08x ie=%08x is=%08x",
			off, size, val, usb_ctl, usb_status, usb_intr_en, usb_intr);
	} else
		USBHIDTrace("wr  %02x.%d = %08x ctl=%08x st=%08x ie=%08x is=%08x",
			off, size, val, usb_ctl, usb_status, usb_intr_en, usb_intr);
	if (size < 4) {
		/* Merge a sub-word store into the register the guest is aiming at.
		   Read-modify-write is right here: the UIM only ever does full-word
		   accesses, so this exists to keep a stray narrow store honest. */
		uint32 mask;
		shift = (int)(off & 3) * 8;
		mask = ((1u << (size * 8)) - 1) << shift;
		val = (ohci_reg_read(word) & ~mask) | ((val << shift) & mask);
	}
	ohci_reg_write(word, val);
}

static int ohci_copy_td(uint32 cbp, uint32 be, uint8 *buf, int len, int to_mem)
{
	int n, left = len;
	uint32 ptr = cbp;

	if (cbp == 0 || be == 0 || len <= 0)
		return 0;
	n = 0x1000 - (int)(ptr & 0xfff);
	if (n > left)
		n = left;
	if (to_mem)
		Host2Mac_memcpy(ptr, buf, n);
	else
		Mac2Host_memcpy(buf, ptr, n);
	left -= n;
	buf += n;
	if (left == 0)
		return len;
	ptr = be & ~0xfffu;
	if (to_mem)
		Host2Mac_memcpy(ptr, buf, left);
	else
		Mac2Host_memcpy(buf, ptr, left);
	return len;
}

static int hid_handle_packet(int addr, int endp, int dir, uint8 *buf, int len)
{
	static uint8 setup[8];
	static int have_setup;
	int out_len = 0;

	USBHIDTrace("hid_pkt addr=%d (dev=%u) endp=%d dir=%d len=%d",
		addr, usb_dev_addr, endp, dir, len);
	if (addr != 0 && addr != usb_dev_addr)
		return -OHCI_CC_DEVICENOTRESPONDING;
	if (endp == 0) {
		if (dir == OHCI_TD_DIR_SETUP) {
			if (len < 8)
				return -OHCI_CC_STALL;
			memcpy(setup, buf, 8);
			have_setup = 1;
			return 0;
		}
		if (!have_setup)
			return 0;
		if (len == 0) {
			int dummy = 0;
			if (setup[1] == 5 || setup[1] == 9)
				hid_control(setup, buf, &dummy, 0);
			have_setup = 0;
			return 0;
		}
		if (hid_control(setup, buf, &out_len, dir == OHCI_TD_DIR_IN) < 0)
			return -OHCI_CC_STALL;
		if (dir != OHCI_TD_DIR_IN)
			out_len = len;
		return out_len;
	}
	if (endp == 1 && dir == OHCI_TD_DIR_IN)
		return hid_interrupt_in(buf, len);
	return -OHCI_CC_STALL;
}

static int ohci_service_td(uint32 *ed_head, uint32 ed_flags)
{
	uint32 td_addr, td_flags, cbp, next, be, dir;
	uint8 buf[256];
	int len = 0, ret, di, toggle;

	td_addr = *ed_head & OHCI_DPTR_MASK;
	if (td_addr == 0)
		return 1;
	td_flags = ReadMacInt32LE(td_addr);
	cbp = ReadMacInt32LE(td_addr + 4);
	next = ReadMacInt32LE(td_addr + 8);
	be = ReadMacInt32LE(td_addr + 12);

	dir = (ed_flags & OHCI_ED_D_MASK) >> OHCI_ED_D_SHIFT;
	if (dir != OHCI_TD_DIR_OUT && dir != OHCI_TD_DIR_IN)
		dir = (td_flags >> OHCI_TD_DP_SHIFT) & 3;

	if (cbp && be) {
		if ((cbp & 0xfffff000u) != (be & 0xfffff000u))
			len = (int)(be & 0xfff) + 0x1001 - (int)(cbp & 0xfff);
		else
			len = (int)(be - cbp) + 1;
		if (len < 0)
			len = 0;
		if (len > (int)sizeof(buf))
			len = sizeof(buf);
		if (len && dir != OHCI_TD_DIR_IN)
			ohci_copy_td(cbp, be, buf, len, 0);
	}

	ret = hid_handle_packet(ed_flags & OHCI_ED_FA_MASK,
		(ed_flags >> OHCI_ED_EN_SHIFT) & 0xf, (int)dir, buf, len);
	USBHIDTrace("TD %08x ret=%d dir=%u len=%d flags=%08x", td_addr, ret, dir, len, td_flags);
	if (ret == -OHCI_CC_DEVICENOTRESPONDING)
		return 1;

	/* The data toggle the transfer used: TD.T[1] selects between the TD's own
	   T[0] and the carry in ED.C, and the successor toggle goes back to ED.C. */
	if (td_flags & OHCI_TD_T_CARRY)
		toggle = (td_flags & OHCI_TD_T_VALUE) != 0;
	else
		toggle = (*ed_head & OHCI_ED_C) != 0;

	if (ret < 0) {
		td_flags = (td_flags & 0x0fffffffu) | ((uint32)(-ret) << 28);
		*ed_head |= OHCI_ED_H;
	} else {
		if (dir == OHCI_TD_DIR_IN && ret > 0)
			ohci_copy_td(cbp, be, buf, ret, 1);
		if (dir == OHCI_TD_DIR_IN && ret < len && !(td_flags & OHCI_TD_R)) {
			td_flags = (td_flags & 0x0fffffffu)
				| ((uint32)OHCI_CC_DATAUNDERRUN << 28);
		} else {
			td_flags &= 0x0fffffffu;
			toggle = !toggle;
			if (ret == len || dir != OHCI_TD_DIR_IN)
				WriteMacInt32LE(td_addr + 4, 0);
			else {
				uint32 ncbp = cbp + (uint32)ret;
				if ((cbp & 0xfff) + (uint32)ret > 0xfff)
					ncbp = (be & ~0xfffu) + ((cbp + (uint32)ret) & 0xfff);
				WriteMacInt32LE(td_addr + 4, ncbp);
			}
		}
		*ed_head = toggle ? (*ed_head | OHCI_ED_C) : (*ed_head & ~OHCI_ED_C);
	}

	/* Retire the TD onto the done queue, newest first, and remember the
	   shortest completion delay any queued TD asked for. */
	*ed_head = (*ed_head & ~OHCI_DPTR_MASK) | (next & OHCI_DPTR_MASK);
	WriteMacInt32LE(td_addr + 8, usb_done);
	usb_done = td_addr;
	di = (int)((td_flags >> OHCI_TD_DI_SHIFT) & 7);
	if (di < usb_done_count)
		usb_done_count = di;
	WriteMacInt32LE(td_addr, td_flags);
	return ((td_flags >> OHCI_TD_CC_SHIFT) & 0xf) != OHCI_CC_NOERROR;
}

static void ohci_service_ed_list(uint32 head)
{
	uint32 cur, next, flags, tail, ed_head;
	int n, t;

	for (n = 0, cur = head; cur && n < OHCI_ED_LIMIT;
			cur = next, n++) {
		flags = ReadMacInt32LE(cur);
		tail = ReadMacInt32LE(cur + 4);
		ed_head = ReadMacInt32LE(cur + 8);
		next = ReadMacInt32LE(cur + 12) & OHCI_DPTR_MASK;
		if ((ed_head & OHCI_ED_H) || (flags & OHCI_ED_K) || (flags & OHCI_ED_F))
			continue;
		for (t = 0; t < OHCI_TD_LIMIT
				&& (ed_head & OHCI_DPTR_MASK) != (tail & OHCI_DPTR_MASK);
				t++) {
			if (ohci_service_td(&ed_head, flags))
				break;
		}
		WriteMacInt32LE(cur + 8, ed_head);
	}
}

static void ohci_frame(void)
{
	uint32 slot;

	usb_frame = (uint16)(usb_frame + 1);
	if ((usb_frame & 0x7fff) == 0)
		usb_intr |= OHCI_INTR_FNO;
	usb_intr |= OHCI_INTR_SF;

	if (usb_hcca == 0)
		return;

	if (usb_ctl & OHCI_CTL_PLE) {
		slot = ReadMacInt32LE(usb_hcca + 4 * (usb_frame & 0x1f));
		usb_period_cur = slot & OHCI_DPTR_MASK;
		ohci_service_ed_list(usb_period_cur);
		usb_period_cur = 0;
	}
	if ((usb_ctl & OHCI_CTL_CLE) && (usb_status & OHCI_STATUS_CLF)) {
		ohci_service_ed_list(usb_ctrl_head);
		usb_status &= ~OHCI_STATUS_CLF;
		usb_ctrl_cur = 0;
	}
	if ((usb_ctl & OHCI_CTL_BLE) && (usb_status & OHCI_STATUS_BLF)) {
		ohci_service_ed_list(usb_bulk_head);
		usb_status &= ~OHCI_STATUS_BLF;
		usb_bulk_cur = 0;
	}

	WriteMacInt16LE(usb_hcca + 0x80, usb_frame);
	WriteMacInt16LE(usb_hcca + 0x82, 0);

	if (usb_done && !(usb_intr & OHCI_INTR_WD)) {
		if (usb_done_count == 0) {
			/* Bit 0 of HccaDoneHead tells the driver that other unmasked
			   interrupt bits are pending besides WritebackDoneHead. */
			if (usb_intr & usb_intr_en & ~(OHCI_INTR_WD | OHCI_INTR_MIE))
				usb_done |= 1;
			WriteMacInt32LE(usb_hcca + 0x84, usb_done);
			usb_done = 0;
			usb_done_count = 7;
			usb_intr |= OHCI_INTR_WD;
		} else if (usb_done_count != 7)
			usb_done_count--;
	}
}

uint32 USBHIDRegBase(void)
{
	return USBHID_BAR;
}

bool USBHIDReady(void)
{
	return usb_installed;
}

void USBHIDInstall(void)
{
	if (usb_installed)
		return;
	if (!MMIOMapWindow(USBHID_BAR, USBHID_BAR_SIZE,
			ohci_window_read, ohci_window_write)) {
		USBHIDLog("BAR map FAILED at %08x - controller not published",
			USBHID_BAR);
		return;
	}
	ohci_hard_reset();
	usb_installed = true;
	USBHIDLog("OHCI window %08x..%08x", USBHID_BAR, USBHID_BAR + USBHID_BAR_SIZE);
}

void USBHIDReset(void)
{
	if (!usb_installed)
		return;
	ohci_hard_reset();
}

void USBHIDVBL(void)
{
	int i;

	if (!usb_installed)
		return;
	if ((usb_ctl & OHCI_CTL_HCFS) != OHCI_USB_OPERATIONAL)
		return;

	/* A real controller runs 1000 frames a second; at 60 Hz that is 16 or 17
	   per VBL. Frames are only generated while operational, so HcFmNumber
	   stays put across a reset the way the driver expects. */
	for (i = 0; i < OHCI_FRAMES_PER_VBL; i++)
		ohci_frame();
}

#else
void USBHIDInstall(void) { }
void USBHIDReset(void) { }
void USBHIDVBL(void) { }
bool USBHIDReady(void) { return false; }
uint32 USBHIDRegBase(void) { return 0; }
int USBHIDDeviceControl(const uint8 setup[8], uint8 *data, int *len,
	int dir_in)
{
	(void)setup; (void)data; (void)dir_in;
	if (len)
		*len = 0;
	return -1;
}
int USBHIDDeviceInterruptIn(uint8 *data, int max)
{
	(void)data; (void)max;
	return 0;
}
#endif /* ENABLE_USB */