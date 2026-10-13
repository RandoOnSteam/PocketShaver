/*
 *  xpram.cpp - XPRAM handling
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

/*
 *  SEE ALSO
 *    Inside Macintosh: Operating System Utilities, chapter 7 "Parameter RAM Utilities"
 */

#include <string.h>

#include "sysdeps.h"
#include "xpram.h"


// Extended parameter RAM
uint8 XPRAM[XPRAM_SIZE];


/*
 *  Initialize XPRAM
 */

void XPRAMInit(const char *vmdir)
{
	// Clear XPRAM
	memset(XPRAM, 0, XPRAM_SIZE);

	// Load XPRAM from settings file
	LoadXPRAM(vmdir);
#if POWERPC_ROM
	if (XPRAM[0x130c] != 0x4e || XPRAM[0x130d] != 0x75
	 || XPRAM[0x130e] != 0x4d || XPRAM[0x130f] != 0x63) {
		memset(XPRAM + 0x1300, 0, 0x100);
		XPRAM[0x130c] = 0x4e;
		XPRAM[0x130d] = 0x75;
		XPRAM[0x130e] = 0x4d;
		XPRAM[0x130f] = 0x63;
		XPRAM[0x1301] = 0x80;
		XPRAM[0x1310] = 0xa8;
		XPRAM[0x1311] = 0x00;
		XPRAM[0x1312] = 0x00;
		XPRAM[0x1313] = 0x22;
		XPRAM[0x1314] = 0xcc;
		XPRAM[0x1315] = 0x0a;
		XPRAM[0x1316] = 0xcc;
		XPRAM[0x1317] = 0x0a;
		XPRAM[0x131c] = 0x00;
		XPRAM[0x131d] = 0x02;
		XPRAM[0x131e] = 0x63;
		XPRAM[0x131f] = 0x00;
		XPRAM[0x1308] = 0x13;
		XPRAM[0x1309] = 0x88;
		XPRAM[0x130a] = 0x00;
		XPRAM[0x130b] = 0xcc;
		XPRAM[0x1376] = 0x00;
		XPRAM[0x1377] = 0x01;
		XPRAM[0x138a] = 0x25;
	}
	uint32 pramindex;
	uint32 pramchecksum = 0;
	for (pramindex = 0x1800; pramindex < 0x2000; pramindex += 2)
		pramchecksum += ((uint32)XPRAM[pramindex] << 8) | XPRAM[pramindex + 1];
	if (pramchecksum % 0xffff != 0 || pramchecksum == 0) {
		memset(XPRAM + 0x1800, 0, 0x800);
		XPRAM[0x1800] = 0x12;
		XPRAM[0x1801] = 0x75;
		XPRAM[0x1802] = 1;
		XPRAM[0x1803] = 8;
		XPRAM[0x1806] = 0x18;
		XPRAM[0x1807] = 0x5c;
		XPRAM[0x1808] = 0x20;
		XPRAM[0x180c] = 0x20;
		for (pramindex = 0x1834; pramindex < 0x185c; pramindex += 4)
			XPRAM[pramindex] = 0x20;
		pramchecksum = 0;
		for (pramindex = 0x1800; pramindex < 0x2000; pramindex += 2)
			pramchecksum += ((uint32)XPRAM[pramindex] << 8) | XPRAM[pramindex + 1];
		pramchecksum = 0xffff - pramchecksum % 0xffff;
		XPRAM[0x1804] = pramchecksum >> 8;
		XPRAM[0x1805] = pramchecksum & 0xff;
	}

#endif
}


/*
 *  Deinitialize XPRAM
 */

void XPRAMExit(void)
{
	// Save XPRAM to settings file
	SaveXPRAM();
}
