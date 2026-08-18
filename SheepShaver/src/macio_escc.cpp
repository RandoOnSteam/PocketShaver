/*
 *  macio_escc.cpp - Idle Z8530 ESCC in the mac-io window
 *
 *  AddrMap (rom_patches.cpp) publishes 0xF3012000 as SCC. Guest lbz/stb there
 *  must reach a chip. Writes are applied as Z8530 register cycles; readable
 *  status always reports an idle chip (Tx buffer empty, nothing received) so
 *  the ROM serial probe finishes instead of spinning on a dead FIFO.
 *
 *  The window traps (mmio.h) rather than being backed by RAM: a Z8530 is a
 *  two-register-per-channel state machine where the write pointer advances on
 *  every access, which only works if each access is seen as it happens.
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
#include "macio_escc.h"
#include "mmio.h"
#include "usbhid.h"

#include <string.h>

#define DEBUG 0
#include "debug.h"

/* Just the SCC page. Covering the whole 64 KB mac-io slice would swallow the
   VIA at 0xF3016000 as well, and answering 0 for its interrupt flags leaves
   the ROM's VIA-timer waits spinning forever. */
enum {
	ESCC_BASE = 0xF3012000u,
	ESCC_SIZE = 0x1000u,
	ESCC_RR0_IDLE = 0x44	/* TxEmpty | TxUnderrun */
};

/* Channel B command, channel B data, channel A command, channel A data. */
enum {
	ESCC_CHAN_B = 0,
	ESCC_CHAN_A = 1
};

struct escc_chan {
	uint8 wr[16];
	uint8 rr[16];
	uint8 ptr;
};

static escc_chan escc_ch[2];
static bool escc_installed;

static void escc_chan_reset(escc_chan *c)
{
	memset(c, 0, sizeof(*c));
	c->rr[0] = ESCC_RR0_IDLE;
}

static void escc_chip_reset(void)
{
	escc_chan_reset(&escc_ch[ESCC_CHAN_A]);
	escc_chan_reset(&escc_ch[ESCC_CHAN_B]);
}

static uint8 escc_cmd_read(escc_chan *c)
{
	uint8 v = c->rr[c->ptr & 15];

	c->ptr = 0;
	return v;
}

static void escc_cmd_write(escc_chan *c, uint8 val)
{
	if (c->ptr == 0) {
		/* WR0: low three bits select the next register, bits 3-5 are a
		   command (0x18 = Reset Tx/Rx, 0x38 = Reset Highest IUS). */
		c->ptr = (uint8)(val & 7);
		if ((val & 0x38) == 0x08)
			c->ptr = (uint8)(c->ptr + 8);
		return;
	}
	c->wr[c->ptr & 15] = val;
	if ((c->ptr & 15) == 9) {
		/* WR9 bits 7-6: 01 channel B reset, 10 channel A, 11 hardware. */
		if ((val & 0xc0) == 0xc0)
			escc_chip_reset();
		else if (val & 0x80)
			escc_chan_reset(&escc_ch[ESCC_CHAN_A]);
		else if (val & 0x40)
			escc_chan_reset(&escc_ch[ESCC_CHAN_B]);
	}
	c->ptr = 0;
}

/* Register layout from the window base, two bytes apart: +0 channel B command,
   +2 channel A command, +4 channel B data, +6 channel A data. The addresses
   the ROM serial probe touches (0xF3012000 / 0xF3012002) are the two command
   ports. Anything else in the mac-io slice reads back as zero, which is what
   an absent register reads as. */
static bool escc_port(uint32 off, escc_chan **c)
{
	if (off >= 8)
		return false;
	*c = &escc_ch[(off & 2) ? ESCC_CHAN_A : ESCC_CHAN_B];
	return (off & 4) == 0;	/* false: data port, Tx empty / Rx silent */
}

static uint32 escc_window_read(uint32 off, int size)
{
	escc_chan *c;

	(void)size;
	if (!escc_port(off, &c))
		return 0;
	return escc_cmd_read(c);
}

/* The ROM's serial debug monitor writes its reason for stopping out of the
   transmit port before it starts polling for commands, so anything sent here is
   worth reading rather than dropping - it is the ROM explaining itself. */
static void escc_transmit(uint8 c)
{
	static char line[160];
	static int n;

	if (c == '\r' || c == '\n' || n == (int)sizeof(line) - 1) {
		if (n) {
			line[n] = 0;
			USBHIDLog("serial: %s", line);
			n = 0;
		}
		return;
	}
	if (c >= 0x20 && c < 0x7f)
		line[n++] = (char)c;
}

static void escc_window_write(uint32 off, int size, uint32 val)
{
	escc_chan *c;

	(void)size;
	if (escc_port(off, &c))
		escc_cmd_write(c, (uint8)val);
	else if (off < 8)
		escc_transmit((uint8)val);
}

void MacIOESCCInstall(void)
{
	if (escc_installed)
		return;
	if (!MMIOMapWindow(ESCC_BASE, ESCC_SIZE,
			escc_window_read, escc_window_write)) {
		USBHIDLog("ESCC map FAILED at %08x", ESCC_BASE);
		return;
	}
	escc_chip_reset();
	escc_installed = true;
	USBHIDLog("ESCC window %08x..%08x", ESCC_BASE, ESCC_BASE + ESCC_SIZE);
}

void MacIOESCCReset(void)
{
	if (escc_installed)
		escc_chip_reset();
}
