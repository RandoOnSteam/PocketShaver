/*
 *  osxguest.h - Public interface for the OSX emulation side of SheepShaver
 * 
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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
#ifndef OSXGUEST_H
#define OSXGUEST_H

typedef struct OsxReferenceState {
	uint32 gpr[32];
	uint64 fpr[32];
	uint32 cr;
	uint32 xer;
	uint32 lr;
	uint32 ctr;
	uint32 fpscr;
	uint32 pc;
} OsxReferenceState;

void OsxReferenceExecute(uint32 entry, OsxReferenceState *state);
void OsxLockstepTest(uint8 *ramhost, uint32 ramsize, int iterations, const char *logpath);
#define OSXBOOTCLASSIC 1
#define OSXBOOTNATIVE 2
int OsxSelectBootImage(int *diskindex, int *partition);
int OsxGuestRequested(void);
int OsxGuestMain(void);

#endif
