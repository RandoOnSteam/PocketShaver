/*
 *  ppc-execute.cpp - PowerPC semantics
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
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

#include <stdio.h>
#include <math.h>
#include <time.h>
#if defined(__MINGW64__) || defined(_MSC_VER) || defined(__GLIBC__) || defined(__APPLE__)
#include <fenv.h>
#endif
/* Set to 0 to drop the out-of-range address report in execute_loadstore. */
#define PPC_REPORT_BAD_EA 0

#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-bitfields.hpp"
#include "cpu/ppc/ppc-operands.hpp"
#include "cpu/ppc/ppc-operations.hpp"
#include "cpu/ppc/ppc-execute.hpp"
#include "cpu/ppc/ppc-stfiwx.hpp"

static inline uint64 get_tb_ticks(void);

#ifndef SHEEPSHAVER
#include "basic-kernel.hpp"
#endif

#ifdef SHEEPSHAVER
#include "main.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "timer.h"
#endif

#ifdef TARGET_OS_IPHONE
#import "FatalErrorAlertViewControllerObjCCppHeader.h"
#import "MiscellaneousSettingsObjCCppHeader.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#include "gfx_log.h"

/**
 *	Illegal & NOP instructions
 **/

void powerpc_cpu::execute_illegal(uint32 opcode)
{
#ifdef SHEEPSHAVER
	// The guest can dispose component code resources behind prepared CFM
	// TVectors, leaving apps executing freed memory inside a container tracked
	// by the rsrc_patches monitor.  The freed space may be zero-filled OR
	// already re-populated by a new owner (any garbage opcode), so try the
	// repair on EVERY illegal opcode -- TryRepair itself verifies the word at
	// pc actually differs from the lock-time snapshot before restoring.
	// Retry semantics: every invalid opcode decodes as CFLOW_TRAP (block-
	// final), so returning without increment_pc() re-decodes from the
	// repaired memory and retries this PC.
	{
		extern int RsrcLocksTryRepair(uint32 pc, uint32 *out_start, uint32 *out_end);
		uint32 repair_start, repair_end;
		int repaired = RsrcLocksTryRepair(pc(), &repair_start, &repair_end);
		if (repaired) {
			if (repaired == 2) {
				// Disposed-import graceful return: the stale cross-TOC call
				// targeted a freed container whose code is gone.  Resume the
				// caller as if the import returned noErr -- the bctr glue left
				// LR = caller's return address (no link), and r3 carries the
				// result.  This breaks the runaway CFM-REPAIR storm without
				// restoring (and thus resurrecting) the freed code.
				gpr(3) = 0;
				pc() = lr();
			}
			invalidate_cache_range(repair_start, repair_end);
			return;
		}
	}
#endif

	gfx_log_emit("[crash] ", "Illegal instruction at %08x, opcode = %08x\n", pc(), opcode);
	execute_fault_report(opcode);
}

// Context dump + "ignore or abort" policy, shared by execute_illegal() and by
// execute_trap_taken() (a taken trap is a fault we cannot deliver, but it is
// not an illegal opcode, so it prints its own headline).
#ifdef SHEEPSHAVER
extern bool PPCGuestAddressValid(uint32 addr, uint32 len);
#endif

static inline bool fault_guest_addr_ok(uint32 addr, uint32 len)
{
#ifdef SHEEPSHAVER
	return PPCGuestAddressValid(addr, len);
#else
	return addr <= 0xffffffffu - len;
#endif
}

void powerpc_cpu::execute_fault_report(uint32 opcode)
{
#ifdef SHEEPSHAVER
	// Dump the locked-'nift' monitor table on the first fault -- host-side
	// reads only, context-safe.
	extern void RsrcLocksDumpOnCrash(void);
	RsrcLocksDumpOnCrash();
#endif

	// Backtrace: walk PPC stack frames to show call chain
	gfx_log_emit("[crash] ", "  PPC Backtrace (stack frame walk):\n");
	{
		uint32 sp = gpr(1);
		uint32 ret_lr = lr();
		gfx_log_emit("[crash] ", "    frame 0: PC=0x%08x LR=0x%08x SP=0x%08x\n", pc(), ret_lr, sp);
		for (int frame = 1; frame < 12 && sp != 0 && sp < 0x50000000 &&
			 fault_guest_addr_ok(sp, 4); frame++) {
			uint32 prev_sp = vm_read_memory_4(sp);  // backchain pointer
			if (prev_sp == 0 || prev_sp <= sp || prev_sp >= 0x50000000) break;
			if (!fault_guest_addr_ok(prev_sp + 8, 4)) break;
			uint32 saved_lr = vm_read_memory_4(prev_sp + 8);  // saved LR in caller's frame
			uint32 call_instr = 0;
			if (saved_lr >= 4 && saved_lr < 0x50000000 &&
				fault_guest_addr_ok(saved_lr - 4, 4))
				call_instr = vm_read_memory_4(saved_lr - 4);
			gfx_log_emit("[crash] ", "    frame %d: saved_LR=0x%08x SP=0x%08x call_instr=0x%08x\n",
					frame, saved_lr, prev_sp, call_instr);
			sp = prev_sp;
		}
	}

	// Dump PPC register state for crash analysis
	gfx_log_emit("[crash] ", "  LR=0x%08x CTR=0x%08x CR=0x%08x XER=0x%08x\n",
			lr(), ctr(), cr().get(), xer().get());
	gfx_log_emit("[crash] ", "  R0=0x%08x R1(SP)=0x%08x R2(TOC)=0x%08x R3=0x%08x\n",
			gpr(0), gpr(1), gpr(2), gpr(3));
	gfx_log_emit("[crash] ", "  R4=0x%08x R5=0x%08x R6=0x%08x R7=0x%08x\n",
			gpr(4), gpr(5), gpr(6), gpr(7));
	gfx_log_emit("[crash] ", "  R8=0x%08x R9=0x%08x R10=0x%08x R11=0x%08x\n",
			gpr(8), gpr(9), gpr(10), gpr(11));
	gfx_log_emit("[crash] ", "  R12=0x%08x R13=0x%08x\n", gpr(12), gpr(13));
	// Dump instructions around the crash address
	gfx_log_emit("[crash] ", "  Instructions around PC:\n");
	for (int di = -4; di <= 4; di++) {
		uint32 addr = pc() + di * 4;
		if (fault_guest_addr_ok(addr, 4))
			gfx_log_emit("[crash] ", "    [0x%08x] %08x%s\n", addr,
				vm_read_memory_4(addr), di == 0 ? " <-- CRASH" : "");
		else
			gfx_log_emit("[crash] ", "    [0x%08x] <unmapped>%s\n", addr,
				di == 0 ? " <-- CRASH" : "");
	}
	// Dump a few words at LR to help understand call chain
	gfx_log_emit("[crash] ", "  Instructions at LR 0x%08x:\n", lr());
	for (int di = -2; di <= 2; di++) {
		uint32 addr = lr() + di * 4;
		if (fault_guest_addr_ok(addr, 4))
			gfx_log_emit("[crash] ", "    [0x%08x] %08x\n", addr,
				vm_read_memory_4(addr));
		else
			gfx_log_emit("[crash] ", "    [0x%08x] <unmapped>\n", addr);
	}

	// Cross-TOC import calls reach here through a TVector held in r12
	// (glue: lwz r12,off(r2); lwz r0,0(r12); mtctr r0; lwz r2,4(r12); bctr).
	// Dump its neighborhood, and scan back from pc for the page-aligned PEF
	// container header ('Joy!') so the dead fragment can be identified.
	{
		uint32 tv = gpr(12);
		if (tv >= 0x1000 && tv < 0x50000000 &&
			fault_guest_addr_ok(tv - 8, 8 * 4)) {
			gfx_log_emit("[crash] ", "  TVector neighborhood (r12=0x%08x):\n", tv);
			for (int di = -2; di <= 5; di++) {
				uint32 addr = tv + di * 4;
				gfx_log_emit("[crash] ", "    [0x%08x] %08x%s\n", addr, vm_read_memory_4(addr),
						di == 0 ? " <-- code ptr" : (di == 1 ? " <-- TOC" : ""));
			}
		}
		if (pc() >= 0x1000 && pc() < 0x50000000) {
			uint32 base = pc() & ~0xfffu;
			bool found = false;
			for (int pages = 0; pages < 8192 && base >= 0x1000; pages++, base -= 0x1000) {
				if (!fault_guest_addr_ok(base, 4))
					break;
				if (vm_read_memory_4(base) == 0x4a6f7921) {	// 'Joy!'
					gfx_log_emit("[crash] ", "  PEF container candidate at 0x%08x (pc offset +0x%x):\n",
							base, pc() - base);
					for (int di = 0; di < 8; di++) {
						const uint32 addr = base + di * 4;
						if (fault_guest_addr_ok(addr, 4))
							gfx_log_emit("[crash] ", "    [0x%08x] %08x\n", addr,
								vm_read_memory_4(addr));
					}
					found = true;
					break;
				}
			}
			if (!found)
				gfx_log_emit("[crash] ", "  no 'Joy!' PEF header within 32 MiB below pc\n");
		}
	}

#ifdef TARGET_OS_IPHONE
	if (objc_getIgnoreIllegalInstructions()) {
		increment_pc(4);
		return;
	} else {
		objc_displayEncounteredIllegalInstructionAlert();
	}
#else
#ifdef SHEEPSHAVER
	if (PrefsFindBool("ignoreillegal")) {
		increment_pc(4);
		return;
	}
#endif // SHEEPSHAVER
#endif // TARGET_OS_IPHONE

#if ENABLE_MON
	disass_ppc(stdout, pc(), opcode);

	// Start up mon in real-mode
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
	abort();
}

void powerpc_cpu::execute_nop(uint32 opcode)
{
	increment_pc(4);
}

/**
 *  Floating-point rounding modes conversion
 **/

static inline int ppc_to_native_rounding_mode(int round)
{
	switch (round) {
	case 0: return FE_TONEAREST;
	case 1: return FE_TOWARDZERO;
	case 2: return FE_UPWARD;
	case 3: return FE_DOWNWARD;
	}
	return FE_TONEAREST;
}

/**
 *	Helper class to compute the overflow/carry condition
 *
 *		OP		Operation to perform
 */

template< class OP >
struct op_carry {
	static inline bool apply(uint32, uint32, uint32) {
		return false;
	}
};

template<>
struct op_carry<op_add> {
	static inline bool apply(uint32 a, uint32 b, uint32 c) {
		// TODO: use 32-bit arithmetic
		uint64 carry = (uint64)a + (uint64)b + (uint64)c;
		return (carry >> 32) != 0;
	}
};

template< class OP >
struct op_overflow {
	static inline bool apply(uint32, uint32, uint32) {
		return false;
	}
};

template<>
struct op_overflow<op_neg> {
	static inline bool apply(uint32 a, uint32, uint32) {
		return a == 0x80000000;
	};
};

template<>
struct op_overflow<op_add> {
	static inline bool apply(uint32 a, uint32 b, uint32 c) {
		// TODO: use 32-bit arithmetic
		int64 overflow = (int64)(int32)a + (int64)(int32)b + (int64)(int32)c;
		return (((uint64)overflow) >> 63) ^ (((uint32)overflow) >> 31);
	}
};

/**
 *	Perform an addition/subtraction
 *
 *		RA		Input operand register, possibly 0
 *		RB		Input operand either register or immediate
 *		RC		Input carry
 *		CA		Predicate to compute the carry out of the operation
 *		OE		Predicate to compute the overflow flag
 *		Rc		Predicate to record CR0
 **/

template< class RA, class RB, class RC, class CA, class OE, class Rc >
void powerpc_cpu::execute_addition(uint32 opcode)
{
	const uint32 a = RA::get(this, opcode);
	const uint32 b = RB::get(this, opcode);
	const uint32 c = RC::get(this, opcode);
	uint32 d = a + b + c;

	// Set XER (CA) if instruction affects carry bit
	if (CA::test(opcode))
		xer().set_ca(op_carry<op_add>::apply(a, b, c));

	// Set XER (OV, SO) if instruction has OE set
	if (OE::test(opcode))
		xer().set_ov(op_overflow<op_add>::apply(a, b, c));

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((int32)d);

	// Commit result to output operand
	operand_RD::set(this, opcode, d);

	increment_pc(4);
}

/**
 *	Generic arithmetic instruction
 *
 *		OP		Operation to perform
 *		RD		Output register
 *		RA		Input operand register
 *		RB		Input operand register or immediate (optional: operand_NONE)
 *		RC		Input operand register or immediate (optional: operand_NONE)
 *		OE		Predicate to compute overflow flag
 *		Rc		Predicate to record CR0
 **/

template< class OP, class RD, class RA, class RB, class RC, class OE, class Rc >
void powerpc_cpu::execute_generic_arith(uint32 opcode)
{
	const uint32 a = RA::get(this, opcode);
	const uint32 b = RB::get(this, opcode);
	const uint32 c = RC::get(this, opcode);

	uint32 d = op_apply<uint32, OP, RA, RB, RC>::apply(a, b, c);

	// Set XER (OV, SO) if instruction has OE set
	if (OE::test(opcode))
		xer().set_ov(op_overflow<OP>::apply(a, b, c));

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((int32)d);

	// commit result to output operand
	RD::set(this, opcode, d);

	increment_pc(4);
}

/**
 *	Rotate Left Word Immediate then Mask Insert
 *
 *		SH		Shift count
 *		MA		Mask value
 *		Rc		Predicate to record CR0
 **/

template< class SH, class MA, class Rc >
void powerpc_cpu::execute_rlwimi(uint32 opcode)
{
	const uint32 n = SH::get(this, opcode);
	const uint32 m = MA::get(this, opcode);
	const uint32 rs = operand_RS::get(this, opcode);
	const uint32 ra = operand_RA::get(this, opcode);
	uint32 d = op_ppc_rlwimi::apply(rs, n, m, ra);

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((int32)d);

	// Commit result to output operand
	operand_RA::set(this, opcode, d);

	increment_pc(4);
}

/**
 *	Shift instructions
 *
 *		OP		Operation to perform
 *		RD		Output operand
 *		RA		Source operand
 *		SH		Shift count
 *		SO		Shift operation
 *		CA		Predicate to compute carry bit
 *		Rc		Predicate to record CR0
 **/

template< class OP >
struct invalid_shift {
	static inline uint32 value(uint32) {
		return 0;
	}
};

template<>
struct invalid_shift<op_shra> {
	static inline uint32 value(uint32 r) {
		return 0 - (r >> 31);
	}
};

template< class OP, class RD, class RA, class SH, class SO, class CA, class Rc >
void powerpc_cpu::execute_shift(uint32 opcode)
{
	const uint32 n = SO::apply(SH::get(this, opcode));
	const uint32 r = RA::get(this, opcode);
	uint32 d;

	// Shift operation is valid only if rB[26] = 0
	if (n & 0x20) {
		d = invalid_shift<OP>::value(r);
		if (CA::test(opcode))
			xer().set_ca(d >> 31);
	}
	else {
		d = OP::apply(r, n);
		if (CA::test(opcode)) {
			const uint32 ca = (r & 0x80000000) && (r & ~(0xffffffff << n));
			xer().set_ca(ca);
		}
	}

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((int32)d);

	// Commit result to output operand
	RD::set(this, opcode, d);

	increment_pc(4);
}

/**
 *	Branch conditional instructions
 *
 *		PC		Input program counter (PC, LR, CTR)
 *		BO		BO operand
 *		DP		Displacement operand
 *		AA		Predicate for absolute address
 *		LK		Predicate to record NPC into link register
 **/

template< class PC, class BO, class DP, class AA, class LK >
void powerpc_cpu::execute_branch(uint32 opcode)
{
	const int bo = BO::get(this, opcode);
	bool ctr_ok = true;
	bool cond_ok = true;

	if (BO_CONDITIONAL_BRANCH(bo)) {
		cond_ok = cr().test(BI_field::extract(opcode));
		if (!BO_BRANCH_IF_TRUE(bo))
			cond_ok = !cond_ok;
	}

	if (BO_DECREMENT_CTR(bo)) {
		ctr_ok = (ctr() -= 1) == 0;
		if (!BO_BRANCH_IF_CTR_ZERO(bo))
			ctr_ok = !ctr_ok;
	}

	const uint32 npc = pc() + 4;
	if (ctr_ok && cond_ok)
		pc() = ((AA::test(opcode) ? 0 : PC::get(this, opcode)) + DP::get(this, opcode)) & -4;
	else
		pc() = npc;

	if (LK::test(opcode))
		lr() = npc;
}

/**
 *	Compare instructions
 *
 *		RB		Second operand (GPR, SIMM, UIMM)
 *		CT		Type of variables to be compared (uint32, int32)
 **/

template< class RB, typename CT >
void powerpc_cpu::execute_compare(uint32 opcode)
{
	const uint32 a = operand_RA::get(this, opcode);
	const uint32 b = RB::get(this, opcode);
	const uint32 crfd = crfD_field::extract(opcode);
	record_cr(crfd, (CT)a < (CT)b ? -1 : ((CT)a > (CT)b ? +1 : 0));
	increment_pc(4);
}

/**
 *	Operations on condition register
 *
 *		OP		Operation to perform
 **/

template< class OP >
void powerpc_cpu::execute_cr_op(uint32 opcode)
{
	const uint32 crbA = crbA_field::extract(opcode);
	uint32 a = (cr().get() >> (31 - crbA)) & 1;
	const uint32 crbB = crbB_field::extract(opcode);
	uint32 b = (cr().get() >> (31 - crbB)) & 1;
	const uint32 crbD = crbD_field::extract(opcode);
	uint32 d = OP::apply(a, b) & 1;
	cr().set((cr().get() & ~(1 << (31 - crbD))) | (d << (31 - crbD)));
	increment_pc(4);
}

/**
 *	Divide instructions
 *
 *		SB		Signed division
 *		OE		Predicate to compute overflow
 *		Rc		Predicate to record CR0
 **/

template< bool SB, class OE, class Rc >
void powerpc_cpu::execute_divide(uint32 opcode)
{
	const uint32 a = operand_RA::get(this, opcode);
	const uint32 b = operand_RB::get(this, opcode);
	uint32 d;

	// Specialize divide semantic action
	if (OE::test(opcode))
		d = do_execute_divide<SB, true>(a, b);
	else
		d = do_execute_divide<SB, false>(a, b);

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((int32)d);

	// Commit result to output operand
	operand_RD::set(this, opcode, d);

	increment_pc(4);
}

/**
 *	Multiply instructions
 *
 *		HI		Predicate for multiply high word
 *		SB		Predicate for signed operation
 *		OE		Predicate to compute overflow
 *		Rc		Predicate to record CR0
 **/

template< bool HI, bool SB, class OE, class Rc >
void powerpc_cpu::execute_multiply(uint32 opcode)
{
	const uint32 a = operand_RA::get(this, opcode);
	const uint32 b = operand_RB::get(this, opcode);
	uint64 d = SB ? (int64)(int32)a * (int64)(int32)b : (uint64)a * (uint64)b;

	// Overflow if the product cannot be represented in 32 bits
	if (OE::test(opcode)) {
		xer().set_ov((d & UVAL64(0xffffffff80000000)) != 0 &&
					 (d & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000));
	}

	// Only keep high word if multiply high instruction
	if (HI)
		d >>= 32;

	// Set CR0 (LT, GT, EQ, SO) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr0((uint32)d);

	// Commit result to output operand
	operand_RD::set(this, opcode, (uint32)d);

	increment_pc(4);
}

/**
 *  Record FPSCR
 *
 *		Update FP exception bits
 **/

void powerpc_cpu::record_fpscr(int exceptions)
{
#if PPC_ENABLE_FPU_EXCEPTIONS
	// Reset non-sticky bits
	fpscr() &= ~(FPSCR_VX_field::mask() | FPSCR_FEX_field::mask());

	// Always update FX if any exception bit was set
	if (exceptions)
		fpscr() |= FPSCR_FX_field::mask() | exceptions;

	// Always update VX
	if (fpscr() & (FPSCR_VXSNAN_field::mask() | FPSCR_VXISI_field::mask() |
				   FPSCR_VXISI_field::mask() | FPSCR_VXIDI_field::mask() |
				   FPSCR_VXZDZ_field::mask() | FPSCR_VXIMZ_field::mask() |
				   FPSCR_VXVC_field::mask() | FPSCR_VXSOFT_field::mask() |
				   FPSCR_VXSQRT_field::mask() | FPSCR_VXCVI_field::mask()))
		fpscr() |= FPSCR_VX_field::mask();

	// Always update FEX
	if (((fpscr() & FPSCR_VX_field::mask()) && (fpscr() & FPSCR_VE_field::mask())) ||
		((fpscr() & FPSCR_OX_field::mask()) && (fpscr() & FPSCR_OE_field::mask())) ||
		((fpscr() & FPSCR_UX_field::mask()) && (fpscr() & FPSCR_UE_field::mask())) ||
		((fpscr() & FPSCR_ZX_field::mask()) && (fpscr() & FPSCR_ZE_field::mask())) ||
		((fpscr() & FPSCR_XX_field::mask()) && (fpscr() & FPSCR_XE_field::mask())))
		fpscr() |= FPSCR_FEX_field::mask();
#endif
}

/**
 *	Floating-point arithmetic
 *
 *		FP		Floating Point type
 *		OP		Operation to perform
 *		RD		Output register
 *		RA		Input operand
 *		RB		Input operand (optional)
 *		RC		Input operand (optional)
 *		Rc		Predicate to record CR1
 *		FPSCR	Predicate to compute FPSCR bits
 **/

template< class FP, class OP, class RD, class RA, class RB, class RC, class Rc, bool FPSCR >
void powerpc_cpu::execute_fp_arith(uint32 opcode)
{
	const double a = RA::get(this, opcode);
	const double b = RB::get(this, opcode);
	const double c = RC::get(this, opcode);

#if PPC_ENABLE_FPU_EXCEPTIONS
	int exceptions;
	if (FPSCR) {
		exceptions = op_apply<uint32, fp_exception_condition<OP>, RA, RB, RC>::apply(a, b, c);
		feclearexcept(FE_ALL_EXCEPT);
		febarrier();
	}
#endif

	FP d = op_apply<double, OP, RA, RB, RC>::apply(a, b, c);

	if (FPSCR) {

		// Update FPSCR exception bits
#if PPC_ENABLE_FPU_EXCEPTIONS
		febarrier();
		int raised = fetestexcept(FE_ALL_EXCEPT);
		if (raised & FE_INEXACT)
			exceptions |= FPSCR_XX_field::mask();
		if (raised & FE_DIVBYZERO)
			exceptions |= FPSCR_ZX_field::mask();
		if (raised & FE_UNDERFLOW)
			exceptions |= FPSCR_UX_field::mask();
		if (raised & FE_OVERFLOW)
			exceptions |= FPSCR_OX_field::mask();
		record_fpscr(exceptions);
#endif

		// FPSCR[FPRF] is set to the class and sign of the result
		if (!FPSCR_VE_field::test(fpscr()))
			fp_classify(d);
	}

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	// Commit result to output operand
	RD::set(this, opcode, d);
	increment_pc(4);
}

/**
 *	Load/store instructions
 *
 *		OP		Operation to perform on loaded value
 *		RA		Base operand
 *		RB		Displacement (GPR(RB), EXTS(d))
 *		LD		Load operation?
 *		SZ		Size of load/store operation
 *		UP		Update RA with EA
 *		RX		Reverse operand
 **/

template< int SZ, bool RX >
struct memory_helper;

#define DEFINE_MEMORY_HELPER(SIZE)																\
template< bool RX >																				\
struct memory_helper<SIZE, RX>																	\
{																								\
	static inline uint32 load(uint32 ea) {														\
		return RX ? vm_read_memory_##SIZE##_reversed(ea) : vm_read_memory_##SIZE(ea);			\
	}																							\
	static inline void store(uint32 ea, uint32 value) {											\
		RX ? vm_write_memory_##SIZE##_reversed(ea, value) : vm_write_memory_##SIZE(ea, value);	\
	}																							\
}

DEFINE_MEMORY_HELPER(1);
DEFINE_MEMORY_HELPER(2);
DEFINE_MEMORY_HELPER(4);

template< class OP, class RA, class RB, bool LD, int SZ, bool UP, bool RX >
void powerpc_cpu::execute_loadstore(uint32 opcode)
{
	const uint32 a = RA::get(this, opcode);
	const uint32 b = RB::get(this, opcode);
	const uint32 ea = a + b;

#if PPC_REPORT_BAD_EA
	/* The guest can only reach RAM and the frame buffer (top nibble 0, 1, 2),
	   the ROM just above 0x40800000, SheepMem at 5 and the kernel area at 6.
	   Flag the nibbles none of those use, plus the gap between the end of the
	   ROM and SheepMem, which is where an address built out of text lands.
	   Reporting here happens before the access faults, which is the only
	   chance to record anything when the fault takes the process down. */
	if (((0xff88u >> (ea >> 28)) & 1)
			|| (ea >= 0x41000000u && ea < 0x50000000u)) {
		extern void ppc_report_bad_ea(uint32 pc, uint32 ea, int is_load);
		ppc_report_bad_ea(pc(), ea, LD);
	}
	/* The first 256 bytes of guest memory are the 68k exception vectors.
	   The ROM fills them in during boot and nothing writes them afterwards,
	   so name whoever stores there: a bad vector kills the next A-trap. */
	if (!LD && ea < 0x100) {
		extern void ppc_report_vector_store(uint32 pc, uint32 ea);
		ppc_report_vector_store(pc(), ea);
	}
#endif


	if (LD)
		operand_RD::set(this, opcode, OP::apply(memory_helper<SZ, RX>::load(ea)));
	else
		memory_helper<SZ, RX>::store(ea, operand_RS::get(this, opcode));

	if (UP)
		RA::set(this, opcode, ea);

	increment_pc(4);
}

template< class RA, class DP, bool LD >
void powerpc_cpu::execute_loadstore_multiple(uint32 opcode)
{
	const uint32 a = RA::get(this, opcode);
	const uint32 d = DP::get(this, opcode);
	uint32 ea = a + d;
/*
	// FIXME: generate exception if ea is not word-aligned
	if ((ea & 3) != 0) {
#ifdef SHEEPSHAVER
		D(bug("unaligned load/store multiple to %08x\n", ea));
		increment_pc(4);
		return;
#else
		abort();
#endif
	}
*/
	int r = LD ? rD_field::extract(opcode) : rS_field::extract(opcode);
	while (r <= 31) {
		if (LD)
			gpr(r) = vm_read_memory_4(ea);
		else
			vm_write_memory_4(ea, gpr(r));
		r++;
		ea += 4;
	}

	increment_pc(4);
}

/**
 *	Floating-point load/store instructions
 *
 *		RA		Base operand
 *		RB		Displacement (GPR(RB), EXTS(d))
 *		LD		Load operation?
 *		DB		Predicate for double value
 *		UP		Predicate to update RA with EA
 **/

template< class RA, class RB, bool LD, bool DB, bool UP >
void powerpc_cpu::execute_fp_loadstore(uint32 opcode)
{
	const uint32 a = RA::get(this, opcode);
	const uint32 b = RB::get(this, opcode);
	const uint32 ea = a + b;
	uint64 v;

	if (LD) {
		if (DB)
			v = vm_read_memory_8(ea);
		else
			v = fp_load_single_convert(vm_read_memory_4(ea));
		operand_fp_dw_RD::set(this, opcode, v);
	}
	else {
		v = operand_fp_dw_RS::get(this, opcode);
		if (DB)
			vm_write_memory_8(ea, v);
		else
			vm_write_memory_4(ea, fp_store_single_convert(v));
	}

	if (UP)
		RA::set(this, opcode, ea);

	increment_pc(4);
}

// Store Floating-Point as Integer Word Indexed (stfiwx): store the low 32 bits
// of FPR(RS) to EA = (RA|0) + RB, with no conversion.  Not a template like the
// other FP load/stores because it stores the raw low word, not a converted
// single/double.
void powerpc_cpu::execute_stfiwx(uint32 opcode)
{
	const uint32 a = operand_RA_or_0::get(this, opcode);
	const uint32 b = operand_RB::get(this, opcode);
	const uint32 ea = PPCStfiwxEffectiveAddress(a, b);
	const uint32 store_value = PPCStfiwxStoreWord(operand_fp_dw_RS::get(this, opcode));

	vm_write_memory_4(ea, store_value);
	increment_pc(4);
}

/**
 *	Load/Store String Word instruction
 *
 *		RA		Input operand as base EA
 *		IM		lswi mode?
 *		NB		Number of bytes to transfer
 **/

template< class RA, bool IM, class NB >
void powerpc_cpu::execute_load_string(uint32 opcode)
{
	uint32 ea = RA::get(this, opcode);
	if (!IM)
		ea += operand_RB::get(this, opcode);

	int nb = NB::get(this, opcode);
	if (IM && nb == 0)
		nb = 32;

	int rd = rD_field::extract(opcode);
#if 1
	int i;
	for (i = 0; nb - i >= 4; i += 4, rd = (rd + 1) & 0x1f)
		gpr(rd) = vm_read_memory_4(ea + i);
	switch (nb - i) {
	case 1:
		gpr(rd) = vm_read_memory_1(ea + i) << 24;
		break;
	case 2:
		gpr(rd) = vm_read_memory_2(ea + i) << 16;
		break;
	case 3:
		gpr(rd) = (vm_read_memory_2(ea + i) << 16) + (vm_read_memory_1(ea + i + 2) << 8);
		break;
	}
#else
	for (int i = 0; i < nb; i++) {
		switch (i & 3) {
		case 0:
			gpr(rd) = vm_read_memory_1(ea + i) << 24;
			break;
		case 1:
			gpr(rd) = (gpr(rd) & 0xff00ffff) | (vm_read_memory_1(ea + i) << 16);
			break;
		case 2:
			gpr(rd) = (gpr(rd) & 0xffff00ff) | (vm_read_memory_1(ea + i) << 8);
			break;
		case 3:
			gpr(rd) = (gpr(rd) & 0xffffff00) | vm_read_memory_1(ea + i);
			rd = (rd + 1) & 0x1f;
			break;
		}
	}
#endif

	increment_pc(4);
}

template< class RA, bool IM, class NB >
void powerpc_cpu::execute_store_string(uint32 opcode)
{
	uint32 ea = RA::get(this, opcode);
	if (!IM)
		ea += operand_RB::get(this, opcode);

	int nb = NB::get(this, opcode);
	if (IM && nb == 0)
		nb = 32;

	int rs = rS_field::extract(opcode);
	int sh = 24;
	for (int i = 0; i < nb; i++) {
		vm_write_memory_1(ea + i, gpr(rs) >> sh);
		sh -= 8;
		if (sh < 0) {
			sh = 24;
			rs = (rs + 1) & 0x1f;
		}
	}

	increment_pc(4);
}

/**
 *	Load Word and Reserve Indexed / Store Word Conditional Indexed
 *
 *		RA		Input operand as base EA
 **/

template< class RA >
void powerpc_cpu::execute_lwarx(uint32 opcode)
{
	const uint32 ea = RA::get(this, opcode) + operand_RB::get(this, opcode);
	uint32 reserve_data = vm_read_memory_4(ea);
	regs().reserve_valid = 1;
	regs().reserve_addr = ea;
#if KPX_MAX_CPUS != 1
	regs().reserve_data = reserve_data;
#endif
	operand_RD::set(this, opcode, reserve_data);
	increment_pc(4);
}

template< class RA >
void powerpc_cpu::execute_stwcx(uint32 opcode)
{
	const uint32 ea = RA::get(this, opcode) + operand_RB::get(this, opcode);
	cr().clear(0);
	if (regs().reserve_valid) {
		if (regs().reserve_addr == ea /* physical_addr(EA) */
#if KPX_MAX_CPUS != 1
			/* HACK: if another processor wrote to the reserved block,
			   nothing happens, i.e. we should operate as if reserve == 0 */
			&& regs().reserve_data == vm_read_memory_4(ea)
#endif
			) {
			vm_write_memory_4(ea, operand_RS::get(this, opcode));
			cr().set(0, standalone_CR_EQ_field::mask());
		}
		regs().reserve_valid = 0;
	}
	cr().set_so(0, xer().get_so());
	increment_pc(4);
}

/**
 *	Floating-point compare instruction
 *
 *		OC		Predicate for ordered compare
 **/

template< bool OC >
void powerpc_cpu::execute_fp_compare(uint32 opcode)
{
	const double a = operand_fp_RA::get(this, opcode);
	const double b = operand_fp_RB::get(this, opcode);
	const int crfd = crfD_field::extract(opcode);
	int c;

	if (is_NaN(a) || is_NaN(b))
		c = 1;
	else if (isless(a, b))
		c = 8;
	else if (isgreater(a, b))
		c = 4;
	else
		c = 2;

	FPSCR_FPCC_field::insert(fpscr(), c);
	cr().set(crfd, c);

	// Update FPSCR exception bits
#if PPC_ENABLE_FPU_EXCEPTIONS
	int exceptions = 0;
	if (is_SNaN(a) || is_SNaN(b)) {
		exceptions |= FPSCR_VXSNAN_field::mask();
		if (OC && !FPSCR_VE_field::test(fpscr()))
			exceptions |= FPSCR_VXVC_field::mask();
	}
	else if (OC && (is_QNaN(a) || is_QNaN(b)))
		exceptions |= FPSCR_VXVC_field::mask();
	record_fpscr(exceptions);
#endif

	increment_pc(4);
}

/**
 *	Floating Convert to Integer Word instructions
 *
 *		RN		Rounding mode
 *		Rc		Predicate to record CR1
 **/

template< class RN, class Rc >
void powerpc_cpu::execute_fp_int_convert(uint32 opcode)
{
	const double b = operand_fp_RB::get(this, opcode);
	const uint32 r = RN::get(this, opcode);
	any_register d;

#if PPC_ENABLE_FPU_EXCEPTIONS
	int exceptions = 0;
	if (is_NaN(b)) {
		exceptions |= FPSCR_VXCVI_field::mask();
		if (is_SNaN(b))
			exceptions |= FPSCR_VXSNAN_field::mask();
	}
	if (isinf(b))
		exceptions |= FPSCR_VXCVI_field::mask();

	feclearexcept(FE_ALL_EXCEPT);
	febarrier();
#endif

	// Convert to integer word if operand fits bounds
	if (b >= -(double)0x80000000 && b <= (double)0x7fffffff) {
#if defined mathlib_lrint
		int old_round = fegetround();
		fesetround(ppc_to_native_rounding_mode(r));
		d.j = (int32)mathlib_lrint(b);
		fesetround(old_round);
#else
		switch (r) {
		case 0: d.j = (int32)op_frin::apply(b); break; // near
		case 1: d.j = (int32)op_friz::apply(b); break; // zero
		case 2: d.j = (int32)op_frip::apply(b); break; // +inf
		case 3: d.j = (int32)op_frim::apply(b); break; // -inf
		}
#endif
	}

	// NOTE: this catches infinity and NaN operands
	else if (b > 0)
		d.j = 0x7fffffff;
	else
		d.j = 0x80000000;

	// Update FPSCR exception bits
#if PPC_ENABLE_FPU_EXCEPTIONS
	febarrier();
	int raised = fetestexcept(FE_ALL_EXCEPT);
	if (raised & FE_UNDERFLOW)
		exceptions |= FPSCR_UX_field::mask();
	if (raised & FE_INEXACT)
		exceptions |= FPSCR_XX_field::mask();
	record_fpscr(exceptions);
#endif

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	// Commit result to output operand
	operand_fp_RD::set(this, opcode, d.d);
	increment_pc(4);
}

/**
 *	Floating-point Round to Single
 *
 *		Rc		Predicate to record CR1
 **/

#ifndef FPCLASSIFY_RETURN_T
#ifdef __MINGW32__
#define FPCLASSIFY_RETURN_T int
#else
#define FPCLASSIFY_RETURN_T uint8
#endif
#endif

template< class FP >
void powerpc_cpu::fp_classify(FP x)
{
	uint32 c = fpscr() & ~FPSCR_FPRF_field::mask();
	FPCLASSIFY_RETURN_T fc = fpclassify(x);
	switch (fc) {
	case FP_NAN:
		c |= FPSCR_FPRF_FU_field::mask() | FPSCR_FPRF_C_field::mask();
		break;
	case FP_ZERO:
		c |= FPSCR_FPRF_FE_field::mask();
		if (signbit(x))
			c |= FPSCR_FPRF_C_field::mask();
		break;
	case FP_INFINITE:
		c |= FPSCR_FPRF_FU_field::mask();
		goto FL_FG_field;
	case FP_SUBNORMAL:
		c |= FPSCR_FPRF_C_field::mask();
		// fall-through
	case FP_NORMAL:
	  FL_FG_field:
		if (x < 0)
			c |= FPSCR_FPRF_FL_field::mask();
		else
			c |= FPSCR_FPRF_FG_field::mask();
		break;
	}
	fpscr() = c;
}

template< class Rc >
void powerpc_cpu::execute_fp_round(uint32 opcode)
{
	const double b = operand_fp_RB::get(this, opcode);

#if PPC_ENABLE_FPU_EXCEPTIONS
	int exceptions =
		fp_invalid_operation_condition<double>::
		apply(FPSCR_VXSNAN_field::mask(), b);

	feclearexcept(FE_ALL_EXCEPT);
	febarrier();
#endif

	float d = (float)b;

	// Update FPSCR exception bits
#if PPC_ENABLE_FPU_EXCEPTIONS
	febarrier();
	int raised = fetestexcept(FE_ALL_EXCEPT);
	if (raised & FE_UNDERFLOW)
		exceptions |= FPSCR_UX_field::mask();
	if (raised & FE_OVERFLOW)
		exceptions |= FPSCR_OX_field::mask();
	if (raised & FE_INEXACT)
		exceptions |= FPSCR_XX_field::mask();
	record_fpscr(exceptions);
#endif

	// FPSCR[FPRF] is set to the class and sign of the result
	if (!FPSCR_VE_field::test(fpscr()))
		fp_classify(d);

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	// Commit result to output operand
	operand_fp_RD::set(this, opcode, (double)d);
	increment_pc(4);
}

/**
 *		System Call instruction
 **/

void powerpc_cpu::execute_syscall(uint32 opcode)
{
#ifdef SHEEPSHAVER
	execute_illegal(opcode);
#else
	cr().set_so(0, execute_do_syscall && !execute_do_syscall(this));
#endif
	increment_pc(4);
}

/**
 *		Trap instructions
 **/

// The TO field (bits 6..10, same position as rS) selects which comparison
// results raise the trap.  TO[0] is the most significant bit, so:
//   0x10 signed less than, 0x08 signed greater than, 0x04 equal,
//   0x02 unsigned less than, 0x01 unsigned greater than.
static inline bool trap_condition(uint32 to, uint32 a, uint32 b)
{
	return (((to & 0x10) && (int32)a <  (int32)b) ||
			((to & 0x08) && (int32)a >  (int32)b) ||
			((to & 0x04) && a == b) ||
			((to & 0x02) && a <  b) ||
			((to & 0x01) && a >  b));
}

void powerpc_cpu::execute_trap(uint32 opcode)
{
	if (trap_condition(rS_field::extract(opcode),
					   operand_RA::get(this, opcode),
					   operand_RB::get(this, opcode)))
		execute_trap_taken(opcode);
	else
		increment_pc(4);
}

void powerpc_cpu::execute_trap_imm(uint32 opcode)
{
	if (trap_condition(rS_field::extract(opcode),
					   operand_RA::get(this, opcode),
					   operand_SIMM::get(this, opcode)))
		execute_trap_taken(opcode);
	else
		increment_pc(4);
}

void powerpc_cpu::execute_trap_taken(uint32 opcode)
{
	// A taken trap raises a program exception, which the nanokernel hands to
	// the handler chain a process joined with InstallExceptionHandler() or
	// InstallSystemExceptionHandler().  That is how the CodeWarrior debugger
	// nub (MetroNub) implements PowerPC breakpoints: it writes tw 20,r0,r0
	// (0x7e800008) over the first instruction of a routine and waits for the
	// exception, and it is also how Debugger()/DebugStr() reach MacsBug.
	//
	// SheepShaver does not leave SPRG3 armed globally because its nanokernel
	// entry is adapted for the emulator. The glue supplies SRR0/SRR1/SPRG3 and
	// transfers control to the ROM's program-exception vector, which performs
	// the normal nanokernel context handoff and handler dispatch.
	const char *why = "not built for SheepShaver";
#ifdef SHEEPSHAVER
	{
		extern const char *DeliverTrapException(uint32 opcode);
		why = DeliverTrapException(opcode);
		if (why == NULL)
			return;		// handed to the guest; it resumes us when it is done
	}
#endif

	// Nobody claimed it.  Resuming past the trap is not an alternative: the
	// instruction a breakpoint overwrote (usually the prologue's mflr r0)
	// never runs, so the routine stores a stale r0 as its return address and
	// comes back to the same breakpoint forever.  Report and stop.
	gfx_log_emit("[crash] ", "Trap taken at %08x, opcode = %08x%s -- not delivered: %s\n",
			pc(), opcode,
			opcode == 0x7e800008 ? " (debugger breakpoint)" : "", why);
	execute_fault_report(opcode);
}

/**
 *		Instructions dealing with system registers
 **/

void powerpc_cpu::execute_mcrf(uint32 opcode)
{
	const int crfS = crfS_field::extract(opcode);
	const int crfD = crfD_field::extract(opcode);
	cr().set(crfD, cr().get(crfS));
	increment_pc(4);
}

void powerpc_cpu::execute_mcrfs(uint32 opcode)
{
	const int crfS = crfS_field::extract(opcode);
	const int crfD = crfD_field::extract(opcode);

	// The contents of FPSCR field crfS are copied to CR field crfD
	const uint32 m = 0xf << (28 - 4 * crfS);
	cr().set(crfD, (fpscr() & m) >> (28 - 4 * crfS));

	// All exception bits copied (except FEX and VX) are cleared in the FPSCR
	fpscr() &= ~(m & (FPSCR_FX_field::mask() | FPSCR_OX_field::mask() |
					  FPSCR_UX_field::mask() | FPSCR_ZX_field::mask() |
					  FPSCR_XX_field::mask() | FPSCR_VXSNAN_field::mask() |
					  FPSCR_VXISI_field::mask() | FPSCR_VXIDI_field::mask() |
					  FPSCR_VXZDZ_field::mask() | FPSCR_VXIMZ_field::mask() |
					  FPSCR_VXVC_field::mask() | FPSCR_VXSOFT_field::mask() |
					  FPSCR_VXSQRT_field::mask() | FPSCR_VXCVI_field::mask()));

	increment_pc(4);
}

void powerpc_cpu::execute_mcrxr(uint32 opcode)
{
	const int crfD = crfD_field::extract(opcode);
	const uint32 x = xer().get();
	cr().set(crfD, x >> 28);
	xer().set(x & 0x0fffffff);
	increment_pc(4);
}

void powerpc_cpu::execute_mtcrf(uint32 opcode)
{
	uint32 mask = field2mask[CRM_field::extract(opcode)];
	cr().set((operand_RS::get(this, opcode) & mask) | (cr().get() & ~mask));
	increment_pc(4);
}

template< class FM, class RB, class Rc >
void powerpc_cpu::execute_mtfsf(uint32 opcode)
{
	const uint64 fsf = RB::get(this, opcode);
	const uint32 f = FM::get(this, opcode);
	uint32 m = field2mask[f];

	// FPSCR[FX] is altered only if FM[0] = 1
	if ((f & 0x80) == 0)
		m &= ~FPSCR_FX_field::mask();

	// The mtfsf instruction cannot alter FPSCR[FEX] nor FPSCR[VX] explicitly
	int exceptions = fsf & m;
	exceptions &= ~(FPSCR_FEX_field::mask() | FPSCR_VX_field::mask());

	// Move frB bits to FPSCR according to field mask
	fpscr() = (fpscr() & ~m) | exceptions;

	// Update FPSCR exception bits (don't implicitly update FX)
	record_fpscr(0);

	// Update native FP control word
	if (m & FPSCR_RN_field::mask())
		fesetround(ppc_to_native_rounding_mode(FPSCR_RN_field::extract(fpscr())));

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	increment_pc(4);
}

template< class RB, class Rc >
void powerpc_cpu::execute_mtfsfi(uint32 opcode)
{
	const uint32 crfD = crfD_field::extract(opcode);
	uint32 m = 0xf << (4 * (7 - crfD));

	// FPSCR[FX] is altered only if crfD = 0
	if (crfD == 0)
		m &= ~FPSCR_FX_field::mask();

	// The mtfsfi instruction cannot alter FPSCR[FEX] nor FPSCR[VX] explicitly
	int exceptions = RB::get(this, opcode) & m;
	exceptions &= ~(FPSCR_FEX_field::mask() | FPSCR_VX_field::mask());

	// Move immediate to FPSCR according to field crfD
	fpscr() = (fpscr() & ~m) | exceptions;

	// Update native FP control word
	if (m & FPSCR_RN_field::mask())
		fesetround(ppc_to_native_rounding_mode(FPSCR_RN_field::extract(fpscr())));

	// Update FPSCR exception bits (don't implicitly update FX)
	record_fpscr(0);

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	increment_pc(4);
}

template< class RB, class Rc >
void powerpc_cpu::execute_mtfsb(uint32 opcode)
{
	const bool set_bit = RB::get(this, opcode);

	// The mtfsb0 and mtfsb1 instructions cannot alter FPSCR[FEX] nor FPSCR[VX] explicitly
	uint32 m = 1 << (31 - crbD_field::extract(opcode));
	m &= ~(FPSCR_FEX_field::mask() | FPSCR_VX_field::mask());

	// Bit crbD of the FPSCR is set or clear
	fpscr() &= ~m;

	// Update FPSCR exception bits
	record_fpscr(set_bit ? m : 0);

	// Update native FP control word if FPSCR[RN] changed
	if (m & FPSCR_RN_field::mask())
		fesetround(ppc_to_native_rounding_mode(FPSCR_RN_field::extract(fpscr())));

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	increment_pc(4);
}

template< class Rc >
void powerpc_cpu::execute_mffs(uint32 opcode)
{
	// Move FPSCR to FPR(FRD)
	operand_fp_dw_RD::set(this, opcode, fpscr());

	// Set CR1 (FX, FEX, VX, VOX) if instruction has Rc set
	if (Rc::test(opcode))
		record_cr1();

	increment_pc(4);
}

void powerpc_cpu::execute_mfmsr(uint32 opcode)
{
	operand_RD::set(this, opcode, msr());
	increment_pc(4);
}

void powerpc_cpu::execute_mtmsr(uint32 opcode)
{
	msr() = operand_RS::get(this, opcode);
	increment_pc(4);
	if (decrementer_pending && (msr() & 0x00008000) != 0)
		spcflags().set(SPCFLAG_CPU_DECREMENTER);
}

void powerpc_cpu::execute_mfsr(uint32 opcode)
{
	operand_RD::set(this, opcode, sr(SR_field::extract(opcode)));
	increment_pc(4);
}

void powerpc_cpu::execute_mfsrin(uint32 opcode)
{
	const uint32 index = operand_RB::get(this, opcode) >> 28;
	operand_RD::set(this, opcode, sr(index));
	increment_pc(4);
}

void powerpc_cpu::execute_mtsr(uint32 opcode)
{
	sr(SR_field::extract(opcode)) = operand_RS::get(this, opcode);
	increment_pc(4);
}

void powerpc_cpu::execute_mtsrin(uint32 opcode)
{
	const uint32 index = operand_RB::get(this, opcode) >> 28;
	sr(index) = operand_RS::get(this, opcode);
	increment_pc(4);
}

void powerpc_cpu::return_from_exception(uint32 saved_pc, uint32 saved_msr)
{
	// rfi restores only the architecturally defined SRR1 fields.
	static const uint32 rfi_msr_mask = 0x87c0ffff;
	msr() = (msr() & ~rfi_msr_mask) | (saved_msr & rfi_msr_mask);
	pc() = saved_pc & ~3u;
}

void powerpc_cpu::execute_rfi(uint32 opcode)
{
	return_from_exception(srr0(), srr1());
	service_decrementer();
}

static const uint32 PPC_MSR_EE = 0x00008000;

#ifdef SHEEPSHAVER

/*
 *  Decrementer deadline timer - host mutex, condition and thread
 *
 *  There is exactly one waiter (the timer thread) and one signaller (the CPU
 *  thread), which is what lets the Win32 side stand an auto-reset event in
 *  for a condition variable: a SetEvent issued between the waiter's unlock
 *  and its wait stays latched in the event, so no wake-up is lost. The
 *  POSIX side uses a real condition variable and keeps microsecond timeouts;
 *  Win32 waits in whole milliseconds. Either way the loop re-tests the
 *  deadline on every wake, so a coarse or spurious wake-up only costs one
 *  more trip around it.
 */

#if defined(_WIN32)

#include <windows.h>

struct powerpc_decrementer_timer {
	CRITICAL_SECTION mutex;
	HANDLE           wakeup;      // auto-reset event
	HANDLE           thread;
};

static DWORD WINAPI decrementer_timer_entry(LPVOID cpu)
{
	((powerpc_cpu *)cpu)->decrementer_timer_loop();
	return 0;
}

static powerpc_decrementer_timer *decrementer_timer_create(void)
{
	powerpc_decrementer_timer *t = new powerpc_decrementer_timer;
	InitializeCriticalSection(&t->mutex);
	t->wakeup = CreateEvent(NULL, FALSE, FALSE, NULL);
	t->thread = NULL;
	return t;
}

static void decrementer_timer_spawn(powerpc_decrementer_timer *t,
									powerpc_cpu *cpu)
{
	t->thread = CreateThread(NULL, 0, decrementer_timer_entry, cpu, 0, NULL);
}

static void decrementer_timer_join(powerpc_decrementer_timer *t)
{
	WaitForSingleObject(t->thread, INFINITE);
	CloseHandle(t->thread);
	CloseHandle(t->wakeup);
	DeleteCriticalSection(&t->mutex);
	delete t;
}

static void decrementer_timer_lock(powerpc_decrementer_timer *t)
	{ EnterCriticalSection(&t->mutex); }
static void decrementer_timer_unlock(powerpc_decrementer_timer *t)
	{ LeaveCriticalSection(&t->mutex); }
static void decrementer_timer_signal(powerpc_decrementer_timer *t)
	{ SetEvent(t->wakeup); }

// Called with the mutex held; returns with it held.
static void decrementer_timer_wait(powerpc_decrementer_timer *t, uint64 usec)
{
	DWORD msec = INFINITE;
	if (usec != ~(uint64)0) {
		uint64 ms = (usec + 999) / 1000;
		msec = ms > 0 ? (DWORD)ms : 1;
	}
	LeaveCriticalSection(&t->mutex);
	WaitForSingleObject(t->wakeup, msec);
	EnterCriticalSection(&t->mutex);
}

#else

#include <pthread.h>
#include <time.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

struct powerpc_decrementer_timer {
	pthread_mutex_t mutex;
	pthread_cond_t  cond;
	pthread_t       thread;
};

static void *decrementer_timer_entry(void *cpu)
{
	((powerpc_cpu *)cpu)->decrementer_timer_loop();
	return NULL;
}

static powerpc_decrementer_timer *decrementer_timer_create(void)
{
	powerpc_decrementer_timer *t = new powerpc_decrementer_timer;
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->cond, NULL);
	return t;
}

static void decrementer_timer_spawn(powerpc_decrementer_timer *t,
									powerpc_cpu *cpu)
{
	pthread_create(&t->thread, NULL, decrementer_timer_entry, cpu);
}

static void decrementer_timer_join(powerpc_decrementer_timer *t)
{
	pthread_join(t->thread, NULL);
	pthread_cond_destroy(&t->cond);
	pthread_mutex_destroy(&t->mutex);
	delete t;
}

static void decrementer_timer_lock(powerpc_decrementer_timer *t)
	{ pthread_mutex_lock(&t->mutex); }
static void decrementer_timer_unlock(powerpc_decrementer_timer *t)
	{ pthread_mutex_unlock(&t->mutex); }
static void decrementer_timer_signal(powerpc_decrementer_timer *t)
	{ pthread_cond_signal(&t->cond); }

// Called with the mutex held; returns with it held.
static void decrementer_timer_wait(powerpc_decrementer_timer *t, uint64 usec)
{
	if (usec == ~(uint64)0) {
		pthread_cond_wait(&t->cond, &t->mutex);
		return;
	}

	// pthread_cond_timedwait takes an absolute CLOCK_REALTIME point.
	struct timespec ts;
#if defined(CLOCK_REALTIME)
	clock_gettime(CLOCK_REALTIME, &ts);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = (long)tv.tv_usec * 1000;
#endif
	ts.tv_sec += (time_t)(usec / 1000000);
	ts.tv_nsec += (long)((usec % 1000000) * 1000);
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&t->cond, &t->mutex, &ts);
}

#endif /* per-platform decrementer timer primitives */

void powerpc_cpu::start_decrementer_timer()
{
	if (decrementer_timer)
		return;
	// Publish the primitives before the thread that waits on them exists.
	decrementer_timer = decrementer_timer_create();
	decrementer_timer_spawn(decrementer_timer, this);
}

void powerpc_cpu::stop_decrementer_timer()
{
	if (!decrementer_timer)
		return;
	decrementer_timer_lock(decrementer_timer);
	decrementer_timer_stop = true;
	decrementer_timer_unlock(decrementer_timer);
	decrementer_timer_signal(decrementer_timer);
	decrementer_timer_join(decrementer_timer);
	decrementer_timer = NULL;
}

void powerpc_cpu::schedule_decrementer_timer(uint64 deadline)
{
	start_decrementer_timer();
	decrementer_timer_lock(decrementer_timer);
	decrementer_timer_deadline = deadline;
	decrementer_timer_unlock(decrementer_timer);
	decrementer_timer_signal(decrementer_timer);
}

void powerpc_cpu::decrementer_timer_loop()
{
	const uint64 no_deadline = ~(uint64)0;
	powerpc_decrementer_timer *timer = decrementer_timer;
	decrementer_timer_lock(timer);
	while (!decrementer_timer_stop) {
		const uint64 deadline = decrementer_timer_deadline;
		if (deadline == no_deadline) {
			// Predicate loop around the bare wait: the wait may return
			// spuriously, which is what the predicate overload absorbed.
			while (!decrementer_timer_stop &&
				   decrementer_timer_deadline == no_deadline)
				decrementer_timer_wait(timer, no_deadline);
			continue;
		}

		const uint64 now = get_tb_ticks();
		if (now >= deadline) {
			// Consume this publication before waking the CPU. read_decrementer()
			// advances the wrapping counter and a subsequent DEC write or
			// successful delivery publishes the next edge.
			decrementer_timer_deadline = no_deadline;
			decrementer_timer_unlock(timer);
			spcflags().set(SPCFLAG_CPU_DECREMENTER);
			idle_resume();
			decrementer_timer_lock(timer);
			continue;
		}

		const uint64 frequency = TimebaseSpeed > 0
			? (uint64)TimebaseSpeed : 1;
		const uint64 delta = deadline - now;
		const uint64 whole_seconds = delta / frequency;
		const uint64 remainder = delta % frequency;
		uint64 usec = whole_seconds * 1000000;
		usec += (remainder * 1000000 + frequency - 1) / frequency;
		if (usec == 0)
			usec = 1;
		decrementer_timer_wait(timer, usec);
	}
	decrementer_timer_unlock(timer);
}
#endif

uint32 powerpc_cpu::read_decrementer()
{
	if (!decrementer_initialized)
		return decrementer_base;

	const uint64 now = get_tb_ticks();
	if (!decrementer_pending && now >= decrementer_next_underflow) {
		decrementer_pending = true;
		spcflags().set(SPCFLAG_CPU_DECREMENTER);
#ifdef SHEEPSHAVER
		schedule_decrementer_timer(~(uint64)0);
#endif

		// DEC is a wrapping 32-bit counter. Keep the following edge correct
		// even if the host was suspended for more than one complete period.
		const uint64 period = (uint64)1 << 32;
		const uint64 periods = (now - decrementer_next_underflow) / period + 1;
		if (periods <= (~(uint64)0 - decrementer_next_underflow) / period)
			decrementer_next_underflow += periods * period;
		else
			decrementer_next_underflow = ~(uint64)0;
	}
	return decrementer_base - (uint32)(now - decrementer_base_ticks);
}

void powerpc_cpu::write_decrementer(uint32 value)
{
	const uint32 old_value = read_decrementer();
	const uint64 now = get_tb_ticks();
	decrementer_last_write = value;
	if (value < decrementer_minimum_write)
		decrementer_minimum_write = value;
	if (value > decrementer_maximum_write)
		decrementer_maximum_write = value;
	decrementer_write_count++;

	decrementer_base = value;
	decrementer_base_ticks = now;
	decrementer_next_underflow = now + (uint64)value + 1;
	decrementer_initialized = true;

	// 6xx/7xx DEC is edge-triggered. A write which crosses from a
	// non-negative value to a negative one asserts the same exception
	// condition as natural completion of the countdown. The nanokernel uses
	// exactly this sequence when restoring an already-expired task quantum.
	if ((int32)old_value >= 0 && (int32)value < 0)
		decrementer_pending = true;
	if (decrementer_pending)
		spcflags().set(SPCFLAG_CPU_DECREMENTER);
#ifdef SHEEPSHAVER
	schedule_decrementer_timer(decrementer_pending
		? ~(uint64)0 : decrementer_next_underflow);
#endif
}

bool powerpc_cpu::decrementer_exception()
{
	return false;
}

bool powerpc_cpu::service_decrementer()
{
	(void)read_decrementer();
	if (execute_depth > 1)
		return false;
	if (!decrementer_pending || (msr() & PPC_MSR_EE) == 0)
		return false;

	// 6xx/7xx processors clear the edge-triggered request on delivery. If the
	// embedding cannot enter its vector yet (during early boot), retain it.
	decrementer_pending = false;
	spcflags().clear(SPCFLAG_CPU_DECREMENTER);
	if (decrementer_exception()) {
		decrementer_delivery_count++;
#ifdef SHEEPSHAVER
		schedule_decrementer_timer(decrementer_next_underflow);
#endif
		return true;
	}
	decrementer_pending = true;
	return false;
}

void powerpc_cpu::get_decrementer_diagnostics(decrementer_diagnostics_t &d)
{
	d.current = read_decrementer();
	d.last_write = decrementer_last_write;
	d.minimum_write = decrementer_write_count != 0
		? decrementer_minimum_write : 0;
	d.maximum_write = decrementer_write_count != 0
		? decrementer_maximum_write : 0;
	d.write_count = decrementer_write_count;
	d.delivery_count = decrementer_delivery_count;
	d.pending = decrementer_pending;
}

template< class SPR >
void powerpc_cpu::execute_mfspr(uint32 opcode)
{
	const uint32 spr = SPR::get(this, opcode);
	uint32 d;
	switch (spr) {
	case powerpc_registers::SPR_XER:	d = xer().get();break;
	case powerpc_registers::SPR_LR:		d = lr();		break;
	case powerpc_registers::SPR_CTR:	d = ctr();		break;
	case powerpc_registers::SPR_DEC:	d = read_decrementer(); break;
	case powerpc_registers::SPR_SRR0:	d = srr0();		break;
	case powerpc_registers::SPR_SRR1:	d = srr1();		break;
	case powerpc_registers::SPR_SPRG0:	d = sprg(0);		break;
	case powerpc_registers::SPR_SPRG1:	d = sprg(1);		break;
	case powerpc_registers::SPR_SPRG2:	d = sprg(2);		break;
	case powerpc_registers::SPR_SPRG3:	d = sprg(3);		break;
	case powerpc_registers::SPR_VRSAVE:	d = vrsave();	break;
#ifdef SHEEPSHAVER
	case powerpc_registers::SPR_SDR1:	d = 0xdead001f;	break;
	case powerpc_registers::SPR_PVR: {
		extern uint32 PVR;
		d = PVR;
		break;
	}
	default: d = 0;
#else
	default: execute_illegal(opcode);
#endif
	}
	operand_RD::set(this, opcode, d);
	increment_pc(4);
}

template< class SPR >
void powerpc_cpu::execute_mtspr(uint32 opcode)
{
	const uint32 spr = SPR::get(this, opcode);
	const uint32 s = operand_RS::get(this, opcode);

	switch (spr) {
	case powerpc_registers::SPR_XER:	xer().set(s);	break;
	case powerpc_registers::SPR_LR:		lr() = s;		break;
	case powerpc_registers::SPR_CTR:	ctr() = s;		break;
	case powerpc_registers::SPR_DEC:	write_decrementer(s); break;
	case powerpc_registers::SPR_SRR0:	srr0() = s;		break;
	case powerpc_registers::SPR_SRR1:	srr1() = s;		break;
	case powerpc_registers::SPR_SPRG0:	sprg(0) = s;	break;
	case powerpc_registers::SPR_SPRG1:	sprg(1) = s;	break;
	case powerpc_registers::SPR_SPRG2:	sprg(2) = s;	break;
	case powerpc_registers::SPR_SPRG3:	sprg(3) = s;	break;
	case powerpc_registers::SPR_VRSAVE:	vrsave() = s;	break;
#ifndef SHEEPSHAVER
	default: execute_illegal(opcode);
#endif
	}

	increment_pc(4);
}

// Compute with 96 bit intermediate result: (a * b) / c
static uint64 muldiv64(uint64 a, uint32 b, uint32 c)
{
	union {
		uint64 ll;
		struct {
#ifdef WORDS_BIGENDIAN
			uint32 high, low;
#else
			uint32 low, high;
#endif
		} l;
	} u, res;

	u.ll = a;
	uint64 rl = (uint64)u.l.low * (uint64)b;
	uint64 rh = (uint64)u.l.high * (uint64)b;
	rh += (rl >> 32);
	res.l.high = rh / c;
	res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
	return res.ll;
}

static inline uint64 get_tb_ticks(void)
{
	uint64 ticks;
#ifdef SHEEPSHAVER
	const uint32 TBFreq = TimebaseSpeed;
	ticks = muldiv64(GetTicks_usec(), TBFreq, 1000000);
#else
	const uint32 TBFreq = 25 * 1000 * 1000; // 25 MHz
	ticks = muldiv64((uint64)clock(), TBFreq, CLOCKS_PER_SEC);
#endif
	return ticks;
}

template< class TBR >
void powerpc_cpu::execute_mftbr(uint32 opcode)
{
	uint32 tbr = TBR::get(this, opcode);
	uint32 d = 0;
	switch (tbr) {
	case 268: d = (uint32)get_tb_ticks(); break;
	case 269: d = (get_tb_ticks() >> 32); break;
	default: execute_illegal(opcode);
	}
	operand_RD::set(this, opcode, d);
	increment_pc(4);
}

/**
 *		Instruction cache management
 **/

void powerpc_cpu::execute_invalidate_cache_range()
{
	if (cache_range.start != cache_range.end) {
		invalidate_cache_range(cache_range.start, cache_range.end);
		cache_range.start = cache_range.end = 0;
	}
}

template< class RA, class RB >
void powerpc_cpu::execute_icbi(uint32 opcode)
{
	const uint32 ea = RA::get(this, opcode) + RB::get(this, opcode);
	const uint32 block_start = ea - (ea % 32);

	if (block_start == cache_range.end) {
		// Extend region to invalidate
		cache_range.end += 32;
	}
	else {
		// New region to invalidate
		execute_invalidate_cache_range();
		cache_range.start = block_start;
		cache_range.end = cache_range.start + 32;
	}

	increment_pc(4);
}

void powerpc_cpu::execute_isync(uint32 opcode)
{
	execute_invalidate_cache_range();
	increment_pc(4);
}

/**
 *		(Fake) data cache management
 **/

template< class RA, class RB >
void powerpc_cpu::execute_dcbz(uint32 opcode)
{
	uint32 ea = RA::get(this, opcode) + RB::get(this, opcode);
	vm_memset(ea - (ea % 32), 0, 32);
	increment_pc(4);
}

/**
 *		Vector load/store instructions
 **/

template< bool SL >
void powerpc_cpu::execute_vector_load_for_shift(uint32 opcode)
{
	const uint32 ra = operand_RA_or_0::get(this, opcode);
	const uint32 rb = operand_RB::get(this, opcode);
	const uint32 ea = ra + rb;
	powerpc_vr & vD = vr(vD_field::extract(opcode));
	int j = SL ? (ea & 0xf) : (0x10 - (ea & 0xf));
	for (int i = 0; i < 16; i++)
		vD.b[ev_mixed::byte_element(i)] = j++;
	increment_pc(4);
}

template< class VD, class RA, class RB >
void powerpc_cpu::execute_vector_load(uint32 opcode)
{
	uint32 ea = RA::get(this, opcode) + RB::get(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	switch (VD::element_size) {
	case 1:
		VD::set_element(vD, (ea & 0x0f), vm_read_memory_1(ea));
		break;
	case 2:
		VD::set_element(vD, ((ea >> 1) & 0x07), vm_read_memory_2(ea & ~1));
		break;
	case 4:
		VD::set_element(vD, ((ea >> 2) & 0x03), vm_read_memory_4(ea & ~3));
		break;
	case 8:
		ea &= ~15;
		vD.w[0] = vm_read_memory_4(ea +  0);
		vD.w[1] = vm_read_memory_4(ea +  4);
		vD.w[2] = vm_read_memory_4(ea +  8);
		vD.w[3] = vm_read_memory_4(ea + 12);
		break;
	}
	increment_pc(4);
}

template< class VS, class RA, class RB >
void powerpc_cpu::execute_vector_store(uint32 opcode)
{
	uint32 ea = RA::get(this, opcode) + RB::get(this, opcode);
	typename VS::type & vS = VS::ref(this, opcode);
	switch (VS::element_size) {
	case 1:
		vm_write_memory_1(ea, VS::get_element(vS, (ea & 0x0f)));
		break;
	case 2:
		vm_write_memory_2(ea & ~1, VS::get_element(vS, ((ea >> 1) & 0x07)));
		break;
	case 4:
		vm_write_memory_4(ea & ~3, VS::get_element(vS, ((ea >> 2) & 0x03)));
		break;
	case 8:
		ea &= ~15;
		vm_write_memory_4(ea +  0, vS.w[0]);
		vm_write_memory_4(ea +  4, vS.w[1]);
		vm_write_memory_4(ea +  8, vS.w[2]);
		vm_write_memory_4(ea + 12, vS.w[3]);
		break;
	}
	increment_pc(4);
}

/**
 *	Vector arithmetic
 *
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 *		Rc		Predicate to record CR6
 *		C1		If recording CR6, do we check for '1' bits in vD?
 **/

template< class OP, class VD, class VA, class VB, class VC, class Rc, int C1 >
void powerpc_cpu::execute_vector_arith(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VC::type const & vC = VC::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;

	for (int i = 0; i < n_elements; i++) {
		const typename VA::element_type a = VA::get_element(vA, i);
		const typename VB::element_type b = VB::get_element(vB, i);
		const typename VC::element_type c = VC::get_element(vC, i);
		typename VD::element_type d = op_apply<typename VD::element_type, OP, VA, VB, VC>::apply(a, b, c);
		if (VD::saturate(d))
			vscr().set_sat(1);
		VD::set_element(vD, i, d);
	}

	// Propagate all conditions to CR6
	if (Rc::test(opcode))
		record_cr6(vD, C1);

	increment_pc(4);
}

/**
 *	Vector mixed arithmetic
 *
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 **/

template< class OP, class VD, class VA, class VB, class VC >
void powerpc_cpu::execute_vector_arith_mixed(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VC::type const & vC = VC::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;
	const int n_sub_elements = 4 / VA::element_size;

	for (int i = 0; i < n_elements; i++) {
		const typename VC::element_type c = VC::get_element(vC, i);
		typename VD::element_type d = c;
		for (int j = 0; j < n_sub_elements; j++) {
			const typename VA::element_type a = VA::get_element(vA, i * n_sub_elements + j);
			const typename VB::element_type b = VB::get_element(vB, i * n_sub_elements + j);
			d += op_apply<typename VD::element_type, OP, VA, VB, null_vector_operand>::apply(a, b, c);
		}
		if (VD::saturate(d))
			vscr().set_sat(1);
		VD::set_element(vD, i, d);
	}

	increment_pc(4);
}

/**
 *	Vector odd/even arithmetic
 *
 *		ODD		Flag: are we computing every odd element?
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 **/

template< int ODD, class OP, class VD, class VA, class VB, class VC >
void powerpc_cpu::execute_vector_arith_odd(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VC::type const & vC = VC::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;

	for (int i = 0; i < n_elements; i++) {
		const typename VA::element_type a = VA::get_element(vA, (i * 2) + ODD);
		const typename VB::element_type b = VB::get_element(vB, (i * 2) + ODD);
		const typename VC::element_type c = VC::get_element(vC, (i * 2) + ODD);
		typename VD::element_type d = op_apply<typename VD::element_type, OP, VA, VB, VC>::apply(a, b, c);
		if (VD::saturate(d))
			vscr().set_sat(1);
		VD::set_element(vD, i, d);
	}

	increment_pc(4);
}

/**
 *	Vector merge instructions
 *
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 *		LO		Flag: use lower part of element
 **/

template< class VD, class VA, class VB, int LO >
void powerpc_cpu::execute_vector_merge(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;

	// Compute into a temporary so that an in-place destination (vD aliasing
	// vA or vB, which compiled/hand-written AltiVec code does routinely to
	// save registers) reads original source bytes, not ones already
	// overwritten.  Real hardware reads all sources before writing vD.
	powerpc_vr tmp;
	for (int i = 0; i < n_elements; i += 2) {
		VD::set_element(tmp, i    , VA::get_element(vA, (i / 2) + LO * (n_elements / 2)));
		VD::set_element(tmp, i + 1, VB::get_element(vB, (i / 2) + LO * (n_elements / 2)));
	}
	vD = tmp;

	increment_pc(4);
}

/**
 *	Vector pack/unpack instructions
 *
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 *		LO		Flag: use lower part of element
 **/

template< class VD, class VA, class VB >
void powerpc_cpu::execute_vector_pack(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;
	const int n_pivot = n_elements / 2;

	// Temp dest: pack narrows vA/vB into vD; an in-place vD (==vA or ==vB)
	// would otherwise overwrite source bytes still needed by later elements.
	powerpc_vr tmp;
	for (int i = 0; i < n_elements; i++) {
		typename VD::element_type d;
		if (i < n_pivot)
			d = VA::get_element(vA, i);
		else
			d = VB::get_element(vB, i - n_pivot);
		if (VD::saturate(d))
			vscr().set_sat(1);
		VD::set_element(tmp, i, d);
	}
	vD = tmp;

	increment_pc(4);
}

template< int LO, class VD, class VA >
void powerpc_cpu::execute_vector_unpack(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;

	// Temp dest: unpack widens vA into vD; an in-place vD (==vA) would
	// overwrite source elements still needed by later iterations.
	powerpc_vr tmp;
	for (int i = 0; i < n_elements; i++)
		VD::set_element(tmp, i, VA::get_element(vA, i + LO * n_elements));
	vD = tmp;

	increment_pc(4);
}

void powerpc_cpu::execute_vector_pack_pixel(uint32 opcode)
{
	powerpc_vr const & vA = vr(vA_field::extract(opcode));
	powerpc_vr const & vB = vr(vB_field::extract(opcode));
	powerpc_vr & vD = vr(vD_field::extract(opcode));

	// Temp dest: guard against in-place vD (==vA or ==vB).
	powerpc_vr tmp;
	for (int i = 0; i < 4; i++) {
		const uint32 a = vA.w[i];
		tmp.h[ev_mixed::half_element(i)] = ((a >> 9) & 0xfc00) | ((a >> 6) & 0x03e0) | ((a >> 3) & 0x001f);
		const uint32 b = vB.w[i];
		tmp.h[ev_mixed::half_element(i + 4)] = ((b >> 9) & 0xfc00) | ((b >> 6) & 0x03e0) | ((b >> 3) & 0x001f);
	}
	vD = tmp;

	increment_pc(4);
}

template< int LO >
void powerpc_cpu::execute_vector_unpack_pixel(uint32 opcode)
{
	powerpc_vr const & vB = vr(vB_field::extract(opcode));
	powerpc_vr & vD = vr(vD_field::extract(opcode));

	// Temp dest: guard against in-place vD (==vB).
	powerpc_vr tmp;
	for (int i = 0; i < 4; i++) {
		const uint32 h = vB.h[ev_mixed::half_element(i + LO * 4)];
		tmp.w[i] = (((h & 0x8000) ? 0xff000000 : 0) |
				   ((h & 0x7c00) << 6) |
				   ((h & 0x03e0) << 3) |
				   (h & 0x001f));
	}

	vD = tmp;
	increment_pc(4);
}

/**
 *	Vector shift instructions
 *
 *		SD		Shift direction: left (-1), right (+1)
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		VC		Input operand vector (optional: operand_NONE)
 *		SH		Shift count operand
 **/

template< int SD >
void powerpc_cpu::execute_vector_shift(uint32 opcode)
{
	powerpc_vr const & vA = vr(vA_field::extract(opcode));
	powerpc_vr const & vB = vr(vB_field::extract(opcode));
	powerpc_vr & vD = vr(vD_field::extract(opcode));

	// The contents of the low-order three bits of all byte
	// elements in vB must be identical to vB[125-127]; otherwise
	// the value placed into vD is undefined.
	const int sh = vB.b[ev_mixed::byte_element(15)] & 7;
	if (sh == 0) {
		for (int i = 0; i < 4; i++)
			vD.w[i] = vA.w[i];
	}
	else {
		uint32 prev_bits = 0;
		if (SD < 0) {
			for (int i = 3; i >= 0; i--) {
				uint32 next_bits = vA.w[i] >> (32 - sh);
				vD.w[i] = ((vA.w[i] << sh) | prev_bits);
				prev_bits = next_bits;
			}
		}
		else if (SD > 0) {
			for (int i = 0; i < 4; i++) {
				uint32 next_bits = vA.w[i] << (32 - sh);
				vD.w[i] = ((vA.w[i] >> sh) | prev_bits);
				prev_bits = next_bits;
			}
		}
	}

	increment_pc(4);
}

template< int SD, class VD, class VA, class VB, class SH >
void powerpc_cpu::execute_vector_shift_octet(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);

	const int sh = SH::get(this, opcode);
	if (SD < 0) {
		for (int i = 0; i < 16; i++) {
			if (i + sh < 16)
				VD::set_element(vD, i, VA::get_element(vA, i + sh));
			else
				VD::set_element(vD, i, VB::get_element(vB, i - (16 - sh)));
		}
	}
	else if (SD > 0) {
		for (int i = 0; i < 16; i++) {
			if (i < sh)
				VD::set_element(vD, i, VB::get_element(vB, 16 - (i - sh)));
			else
				VD::set_element(vD, i, VA::get_element(vA, i - sh));
		}
	}

	increment_pc(4);
}

/**
 *	Vector splat instructions
 *
 *		OP		Operation to perform on element
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 *		IM		Immediate value to replicate
 **/

template< class OP, class VD, class VB, bool IM >
void powerpc_cpu::execute_vector_splat(uint32 opcode)
{
	typename VD::type & vD = VD::ref(this, opcode);
	const int n_elements = 16 / VD::element_size;

	uint32 value;
	if (IM)
		value = OP::apply(vUIMM_field::extract(opcode));
	else {
		typename VB::type const & vB = VB::const_ref(this, opcode);
		const int n = vUIMM_field::extract(opcode) & (n_elements - 1);
		value = OP::apply(VB::get_element(vB, n));
	}

	for (int i = 0; i < n_elements; i++)
		VD::set_element(vD, i, value);

	increment_pc(4);
}

/**
 *	Vector sum instructions
 *
 *		SZ		Size of destination vector elements
 *		VD		Output operand vector
 *		VA		Input operand vector
 *		VB		Input operand vector (optional: operand_NONE)
 **/

template< int SZ, class VD, class VA, class VB >
void powerpc_cpu::execute_vector_sum(uint32 opcode)
{
	typename VA::type const & vA = VA::const_ref(this, opcode);
	typename VB::type const & vB = VB::const_ref(this, opcode);
	typename VD::type & vD = VD::ref(this, opcode);
	typename VD::element_type d;

	switch (SZ) {
	case 1: // vsum
		d = VB::get_element(vB, 3);
		for (int j = 0; j < 4; j++)
			d += VA::get_element(vA, j);
		if (VD::saturate(d))
			vscr().set_sat(1);
		VD::set_element(vD, 0, 0);
		VD::set_element(vD, 1, 0);
		VD::set_element(vD, 2, 0);
		VD::set_element(vD, 3, d);
		break;

	case 2: // vsum2
		for (int i = 0; i < 4; i += 2) {
			d = VB::get_element(vB, i + 1);
			for (int j = 0; j < 2; j++)
				d += VA::get_element(vA, i + j);
			if (VD::saturate(d))
				vscr().set_sat(1);
			VD::set_element(vD, i + 0, 0);
			VD::set_element(vD, i + 1, d);
		}
		break;

	case 4: // vsum4
		for (int i = 0; i < 4; i += 1) {
			d = VB::get_element(vB, i);
			const int n_elements = 4 / VA::element_size;
			for (int j = 0; j < n_elements; j++)
				d += VA::get_element(vA, i * n_elements + j);
			if (VD::saturate(d))
				vscr().set_sat(1);
			VD::set_element(vD, i, d);
		}
		break;
	}

	increment_pc(4);
}

/**
 *		Misc vector instructions
 **/

void powerpc_cpu::execute_vector_permute(uint32 opcode)
{
	powerpc_vr const & vA = vr(vA_field::extract(opcode));
	powerpc_vr const & vB = vr(vB_field::extract(opcode));
	powerpc_vr const & vC = vr(vC_field::extract(opcode));
	powerpc_vr & vD = vr(vD_field::extract(opcode));

	for (int i = 0; i < 16; i++) {
		const int ei = ev_mixed::byte_element(i);
		const int n  = vC.b[ei] & 0x1f;
		const int en = ev_mixed::byte_element(n & 0xf);
		vD.b[ei] = (n & 0x10) ? vB.b[en] : vA.b[en];
	}

	increment_pc(4);
}

void powerpc_cpu::execute_mfvscr(uint32 opcode)
{
	const int vD = vD_field::extract(opcode);
	vr(vD).w[0] = 0;
	vr(vD).w[1] = 0;
	vr(vD).w[2] = 0;
	vr(vD).w[3] = vscr().get();
	increment_pc(4);
}

void powerpc_cpu::execute_mtvscr(uint32 opcode)
{
	const int vB = vB_field::extract(opcode);
	vscr().set(vr(vB).w[3]);
	increment_pc(4);
}

/**
 *		Explicit template instantiations
 **/

#include "ppc-execute-impl.cpp"
