/*
 *  ppc-report.h - 68k runaway diagnostics
 *
 *  (C) 2026 Ryan Norton (battlemageloveryt@gmail.com)
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

#ifndef PPC_REPORT_H
#define PPC_REPORT_H

#define PPC_REPORT_BAD_EA 0
#define PPC_REPORT_EVERY_ACCESS 0

#if PPC_REPORT_BAD_EA

enum {
	PPC_68K_BRANCHES = 1024,	// Ring entries
	PPC_68K_BRANCH_DUMP = 256,	// Newest entries a dump prints
	PPC_68K_BRANCH_HASH = 512	// Buckets folding a repeated transfer
};

struct ppc_68k_branch {
	uint32 key;		// from ^ (to << 1), what confirms a bucket
	uint32 from;
	uint32 to;
	uint32 op;
	uint32 hits;
};
extern struct ppc_68k_branch ppc_68k_branches[PPC_68K_BRANCHES];
// Slot + 1, so the zero-initialised state is "empty".
extern uint16 ppc_68k_branch_map[PPC_68K_BRANCH_HASH];
extern int ppc_68k_branch_pos;
extern uint32 ppc_68k_last_pc;
extern bool ppc_68k_a7_flagged;

extern void ppc_report_bad_ea(uint32 pc, uint32 ea, int is_load);
extern void ppc_report_fault_trail(void);
extern void ppc_report_vector_store(uint32 pc, uint32 ea);
extern void ppc_report_68k_transfer(uint32 pc, uint32 from, uint32 to,
	uint32 op, uint32 a7);

#endif

#endif
