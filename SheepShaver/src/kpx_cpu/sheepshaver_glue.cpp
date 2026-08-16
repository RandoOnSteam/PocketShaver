/*
 *  sheepshaver_glue.cpp - Glue Kheperix CPU to SheepShaver CPU engine interface
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
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
#include "main.h"
#include "prefs.h"
#include "xlowmem.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_MACCATALYST
// Defined in PreferencesViewControllerObjC.mm - drains AppKit's NSEvent queue so
// UIKit input stays live while the emulator owns the main thread on Catalyst.
extern "C" void catalyst_pump_appkit_events(void);
#endif
#include "emul_op.h"
#include "rom_patches.h"
#include "macos_util.h"
#include "block-alloc.hpp"
#include "sigsegv.h"
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-operations.hpp"
#include "cpu/ppc/ppc-instructions.hpp"
#include "thunks.h"

// Used for NativeOp trampolines
#include "video.h"
#include "name_registry.h"
#include "serial.h"
#include "ether.h"
#include "timer.h"
#if (defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) || TARGET_OS_IPHONE
#include "rave_engine.h"
#include "gl_engine.h"
#include "dsp_engine.h"
#include "glide_engine.h"
#if defined(ENABLE_NATIVE_CINEPAK_PATCH) && ENABLE_NATIVE_CINEPAK_PATCH
#include "cinepak_hooks.h"
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef USE_SDL_VIDEO
#include "my_sdl.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#include "gfx_log.h"

extern "C" {
#include "dis-asm.h"
}

// Emulation time statistics
#ifndef EMUL_TIME_STATS
#define EMUL_TIME_STATS 0
#endif

#if EMUL_TIME_STATS
static clock_t emul_start_time;
static uint32 interrupt_count = 0;
static clock_t interrupt_time = 0;
static uint32 exec68k_count = 0;
static clock_t exec68k_time = 0;
static uint32 native_exec_count = 0;
static clock_t native_exec_time = 0;
static uint32 macos_exec_count = 0;
static clock_t macos_exec_time = 0;
#endif

static void enter_mon(void)
{
	// Start up mon in real-mode
#if ENABLE_MON
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
}

// From rsrc_patches.cpp
extern "C" void check_load_invoc(uint32 type, int16 id, uint32 h);
extern "C" void named_check_load_invoc(uint32 type, uint32 name, uint32 h);

// PowerPC EmulOp to exit from emulation loop
const uint32 POWERPC_EXEC_RETURN = POWERPC_EMUL_OP | 1;

// Enable Execute68k() safety checks?
#define SAFE_EXEC_68K 1

// Save FP state in Execute68k()?
#define SAVE_FP_EXEC_68K 1

// Interrupts in EMUL_OP mode?
#define INTERRUPTS_IN_EMUL_OP_MODE 1

// Pointer to Kernel Data
static KernelData * kernel_data;

// patch_nanokernel() replaces the ROM's SPRG3 writes with XLM_RUN_MODE writes.
// Mirror the two original table values when entering a hardware vector: the
// native-PPC and 68K emulator contexts have different low-level handlers.
static const uint32 PPC_NATIVE_EXCEPTION_TABLE_OFFSET = 0x420;
static const uint32 PPC_68K_EXCEPTION_TABLE_OFFSET = 0x360;

// Aggregate rather than logging each 60 Hz request. Deferred counts are CPU
// boundary retries of one coalesced level, not queued VBL events.
static uint64 external_interrupt_accepted_count;
static uint64 external_interrupt_nest_deferred_count;
static uint64 external_interrupt_native_vector_count;
static uint64 external_interrupt_68k_accepted_count;
static uint64 external_interrupt_emul_accepted_count;
static uint64 external_interrupt_68k_deferred_count;
static uint64 external_interrupt_mode_deferred_count;
static uint64 external_interrupt_accepted_snapshot;
static uint64 external_interrupt_nest_deferred_snapshot;
static uint64 external_interrupt_native_vector_snapshot;
static uint64 external_interrupt_68k_accepted_snapshot;
static uint64 external_interrupt_emul_accepted_snapshot;
static uint64 external_interrupt_68k_deferred_snapshot;
static uint64 external_interrupt_mode_deferred_snapshot;

// SIGSEGV handler
sigsegv_return_t sigsegv_handler(sigsegv_address_t, sigsegv_address_t);

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Special trampolines for EmulOp and NativeOp
static uint8 *emul_op_trampoline;
static uint8 *native_op_trampoline;
#endif


/**
 *		PowerPC emulator glue with special 'sheep' opcodes
 **/

enum {
	PPC_I(SHEEP) = PPC_I(MAX),
	PPC_I(SHEEP_MAX)
};

class sheepshaver_cpu
	: public powerpc_cpu
{
	void init_decoder();
	void execute_sheep(uint32 opcode);

public:

	// Constructor
	sheepshaver_cpu();

	// CR & XER accessors
	uint32 get_cr() const		{ return cr().get(); }
	void set_cr(uint32 v)		{ cr().set(v); }
	uint32 get_xer() const		{ return xer().get(); }
	void set_xer(uint32 v)		{ xer().set(v); }

	// Execute NATIVE_OP routine
	void execute_native_op(uint32 native_op);
	static void call_execute_native_op(powerpc_cpu * cpu, uint32 native_op);

	// Execute EMUL_OP routine
	void execute_emul_op(uint32 emul_op);
	static void call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op);

	// Execute 68k routine
	void execute_68k(uint32 entry, M68kRegisters *r);

	// Execute ppc routine
	void execute_ppc(uint32 entry);

	// Execute MacOS/PPC code
	uint32 execute_macos_code(uint32 tvect, int nargs, uint32 const *args);

	// Hand a taken trap to the MacOS exception handler chain
	const char *deliver_trap_exception(uint32 opcode);
	const char *enter_exception_vector(uint32 vector, uint32 handler_slot,
		uint32 saved_pc, uint32 saved_msr);
	void exception_step(void);
	void return_from_exception(uint32 saved_pc, uint32 saved_msr);
	bool decrementer_exception();
	bool external_interrupt();
	bool exception_step_trampoline_ready(void);
	void exception_diagnostic_state(const char *reason, uint64 now);
	void exception_idle_diagnostic(void);
	void record_rfi_site(uint32 return_pc, uint32 run_mode);
	void report_and_reset_rfi_sites(void);
	void preserve_system_ticks(void);

	bool exception_step_active;
	bool exception_step_trap;
	uint32 exception_step_opcode;
	uint32 exception_step_trampoline;
	uint32 exception_step_pc;
	bool exception_step_pending;
	// Hardware-style entries which have not yet crossed an rfi boundary.
	// This is architectural nesting, not debugger-handler lifetime.
	uint32 exception_entry_depth;
	uint64 exception_last_program_usec;
	uint64 exception_last_vector_usec;
	uint64 exception_idle_snapshot_usec;
	unsigned long exception_idle_snapshot_count;
	uint32 exception_tick_snapshot;
	uint32 exception_last_vector;
	uint32 exception_returns_since_program;
	uint32 exception_traces_since_program;
	uint32 exception_decrementers_since_program;
	uint64 exception_stall_last_usec;
	InterruptServiceDiagnostics exception_service_snapshot;
	enum { EXCEPTION_RFI_SITE_COUNT = 32 };
	struct exception_rfi_site_t {
		uint32 pc;
		uint32 mode;
		uint64 count;
	} exception_rfi_sites[EXCEPTION_RFI_SITE_COUNT];
	uint64 exception_rfi_site_overflow;
	bool system_ticks_valid;
	uint32 system_ticks_high_water;
	uint64 system_tick_correction_count;
	uint64 system_tick_recovered_total;
	uint32 system_tick_max_rollback;
	uint64 system_tick_correction_snapshot;
	uint64 system_tick_recovered_snapshot;

#if PPC_ENABLE_JIT
	// Compile one instruction
	virtual int compile1(codegen_context_t & cg_context);
#endif
	// Resource manager thunk
	void get_resource(uint32 old_get_resource);
	static void call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource);

	// Diagnostic accessors (crash-context dump, vm watch logging)
	uint32 cur_pc()			{ return pc(); }
	uint32 cur_lr()			{ return lr(); }
	uint32 cur_gpr(int i)	{ return gpr(i); }

	// Make sure the SIGSEGV handler can access CPU registers
	friend sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip);
};

sheepshaver_cpu::sheepshaver_cpu()
{
	init_decoder();
	sprg(0) = KernelDataAddr;
	sprg(3) = KernelDataAddr + PPC_68K_EXCEPTION_TABLE_OFFSET;
	exception_step_active = false;
	exception_step_trap = false;
	exception_step_opcode = 0;
	exception_step_trampoline = 0;
	exception_step_pc = 0;
	exception_step_pending = false;
	exception_entry_depth = 0;
	exception_last_program_usec = 0;
	exception_last_vector_usec = 0;
	exception_idle_snapshot_usec = 0;
	exception_idle_snapshot_count = 0;
	exception_tick_snapshot = 0;
	exception_last_vector = 0;
	exception_returns_since_program = 0;
	exception_traces_since_program = 0;
	exception_decrementers_since_program = 0;
	exception_stall_last_usec = 0;
	memset(&exception_service_snapshot, 0,
		sizeof(exception_service_snapshot));
	memset(exception_rfi_sites, 0, sizeof(exception_rfi_sites));
	exception_rfi_site_overflow = 0;
	system_ticks_valid = false;
	system_ticks_high_water = 0;
	system_tick_correction_count = 0;
	system_tick_recovered_total = 0;
	system_tick_max_rollback = 0;
	system_tick_correction_snapshot = 0;
	system_tick_recovered_snapshot = 0;

#if PPC_ENABLE_JIT
	if (PrefsFindBool("jit"))
		enable_jit();
#endif
}

void sheepshaver_cpu::init_decoder()
{
	static const instr_info_t sheep_ii_table[] = {
		{ "sheep",
		  (execute_pmf)&sheepshaver_cpu::execute_sheep,
		  PPC_I(SHEEP),
		  D_form, 6, 0, CFLOW_JUMP | CFLOW_TRAP
		}
	};

	const int ii_count = sizeof(sheep_ii_table)/sizeof(sheep_ii_table[0]);
	D(bug("SheepShaver extra decode table has %d entries\n", ii_count));

	for (int i = 0; i < ii_count; i++) {
		const instr_info_t * ii = &sheep_ii_table[i];
		init_decoder_entry(ii);
	}
}

/*		NativeOp instruction format:
		+------------+-------------------------+--+-----------+------------+
		|      6     |                         |FN|    OP     |      2     |
		+------------+-------------------------+--+-----------+------------+
		 0         5 |6                      18 19 20      25 26        31
*/

typedef bit_field< 19, 19 > FN_field;
typedef bit_field< 20, 25 > NATIVE_OP_field;
typedef bit_field< 26, 31 > EMUL_OP_field;

void sheepshaver_cpu::call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_emul_op(emul_op);
}

// Execute EMUL_OP routine
void sheepshaver_cpu::execute_emul_op(uint32 emul_op)
{
	M68kRegisters r68;
	// The 68K side services VBL and therefore owns the newest system Ticks
	// value.  Observe it before crossing into a host EmulOp; a later PowerPC
	// context return must not expose an older process-context copy.
	preserve_system_ticks();
	WriteMacInt32(XLM_68K_R25, gpr(25));
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);
	for (int i = 0; i < 8; i++)
		r68.d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)
		r68.a[i] = gpr(16 + i);
	r68.a[7] = gpr(1);
	uint32 saved_cr = get_cr() & 0xff9fffff; // mask_operand::compute(11, 8)
	uint32 saved_xer = get_xer();
	EmulOp(&r68, gpr(24), emul_op);
	set_cr(saved_cr);
	set_xer(saved_xer);
	for (int i = 0; i < 8; i++)
		gpr(8 + i) = r68.d[i];
	for (int i = 0; i < 7; i++)
		gpr(16 + i) = r68.a[i];
	gpr(1) = r68.a[7];
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);
}

// Execute SheepShaver instruction
void sheepshaver_cpu::execute_sheep(uint32 opcode)
{
//	D(bug("Extended opcode %08x at %08x (68k pc %08x)\n", opcode, pc(), gpr(24)));
	assert((((opcode >> 26) & 0x3f) == 6) && OP_MAX <= 64 + 3);

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		QuitEmulator();
		break;

	case 1:		// EXEC_RETURN
		spcflags().set(SPCFLAG_CPU_EXEC_RETURN);
		break;

	case 2:		// EXEC_NATIVE
		{
			const uint32 selector = NATIVE_OP_field::extract(opcode);
			execute_native_op(selector);
			// The single-step trampoline performs a dynamic control transfer.
			if (selector == NATIVE_EXCEPTION_STEP)
				return;
		}
		if (FN_field::test(opcode))
			pc() = lr();
		else
			pc() += 4;
		break;

	default:	// EMUL_OP
		execute_emul_op(EMUL_OP_field::extract(opcode) - 3);
		pc() += 4;
		break;
	}
}

// Compile one instruction
#if PPC_ENABLE_JIT
int sheepshaver_cpu::compile1(codegen_context_t & cg_context)
{
#if defined(__aarch64__)
	// Threaded-code bring-up: SHEEP goes through the generic interpreter
	// invoke like everything else. The specialized emission below leans on
	// GPR/PC micro-ops (and, with PPC_REENTRANT_JIT, the EmulOp
	// trampolines) that the arm64 backend does not implement yet.
	return COMPILE_FAILURE;
#endif
	const instr_info_t *ii = cg_context.instr_info;
	if (ii->mnemo != PPC_I(SHEEP))
		return COMPILE_FAILURE;

	int status = COMPILE_FAILURE;
	powerpc_dyngen & dg = cg_context.codegen;
	uint32 opcode = cg_context.opcode;

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		dg.gen_invoke(QuitEmulator);
		status = COMPILE_CODE_OK;
		break;

	case 1:		// EXEC_RETURN
		dg.gen_spcflags_set(SPCFLAG_CPU_EXEC_RETURN);
		// Don't check for pending interrupts, we do know we have to
		// get out of this block ASAP
		dg.gen_exec_return();
		status = COMPILE_EPILOGUE_OK;
		break;

	case 2: {	// EXEC_NATIVE
		uint32 selector = NATIVE_OP_field::extract(opcode);
		// The step NativeOp changes PC dynamically. Compile it through the
		// generic execute_sheep() call so the block ends and interpreter/JIT
		// semantics stay identical.
		if (selector == NATIVE_EXCEPTION_STEP)
			return COMPILE_FAILURE;
		switch (selector) {
#if !PPC_REENTRANT_JIT
		// Filter out functions that may invoke Execute68k() or
		// CallMacOS(), this would break reentrancy as they could
		// invalidate the translation cache and even overwrite
		// continuation code when we are done with them.
		case NATIVE_PATCH_NAME_REGISTRY:
			dg.gen_invoke(DoPatchNameRegistry);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_INSTALL_ACCEL:
			dg.gen_invoke(VideoInstallAccel);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_VBL:
			dg.gen_invoke(VideoVBL);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_GET_RESOURCE:
		case NATIVE_GET_1_RESOURCE:
		case NATIVE_GET_IND_RESOURCE:
		case NATIVE_GET_1_IND_RESOURCE:
		case NATIVE_R_GET_RESOURCE: {
			static const uint32 get_resource_ptr[] = {
				XLM_GET_RESOURCE,
				XLM_GET_1_RESOURCE,
				XLM_GET_IND_RESOURCE,
				XLM_GET_1_IND_RESOURCE,
				XLM_R_GET_RESOURCE
			};
			uint32 old_get_resource = ReadMacInt32(get_resource_ptr[selector - NATIVE_GET_RESOURCE]);
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_get_resource;
			dg.gen_invoke_CPU_im(func, old_get_resource);
			status = COMPILE_CODE_OK;
			break;
		}
#endif
		case NATIVE_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_se_16_32_T1();
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NAMED_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))named_check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_SYNC_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_sync_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_bitblt_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_fillrect_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_UNKNOWN_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_unknown_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BLTMASK_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_bltmask_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLMASK_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_fillmask_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_bitblt);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_INVRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_invrect);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_fillrect);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BLTMASK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_bltmask);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLMASK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_fillmask);
			status = COMPILE_CODE_OK;
			break;
		}
		// Could we fully translate this NativeOp?
		if (status == COMPILE_CODE_OK) {
			if (!FN_field::test(opcode))
				cg_context.done_compile = false;
			else {
				dg.gen_load_T0_LR_aligned();
				dg.gen_set_PC_T0();
				cg_context.done_compile = true;
			}
			break;
		}
#if PPC_REENTRANT_JIT
		// Try to execute NativeOp trampoline
		if (!FN_field::test(opcode))
			dg.gen_set_PC_im(cg_context.pc + 4);
		else {
			dg.gen_load_T0_LR_aligned();
			dg.gen_set_PC_T0();
		}
		dg.gen_mov_32_T0_im(selector);
		dg.gen_jmp(native_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke NativeOp handler
		if (!FN_field::test(opcode)) {
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_execute_native_op;
			dg.gen_invoke_CPU_im(func, selector);
			cg_context.done_compile = false;
			status = COMPILE_CODE_OK;
		}
		// Otherwise, let it generate a call to execute_sheep() which
		// will cause necessary updates to the program counter
		break;
#endif
	}

	default: {	// EMUL_OP
		uint32 emul_op = EMUL_OP_field::extract(opcode) - 3;
#if PPC_REENTRANT_JIT
		// Try to execute EmulOp trampoline
		dg.gen_set_PC_im(cg_context.pc + 4);
		dg.gen_mov_32_T0_im(emul_op);
		dg.gen_jmp(emul_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke EmulOp handler
		typedef void (*func_t)(dyngen_cpu_base, uint32);
		func_t func = &sheepshaver_cpu::call_execute_emul_op;
		dg.gen_invoke_CPU_im(func, emul_op);
		cg_context.done_compile = false;
		status = COMPILE_CODE_OK;
		break;
#endif
	}
	}
	return status;
}
#endif

// Execute 68k routine
void sheepshaver_cpu::execute_68k(uint32 entry, M68kRegisters *r)
{
#if EMUL_TIME_STATS
	exec68k_count++;
	const clock_t exec68k_start = clock();
#endif

#if SAFE_EXEC_68K
	if (ReadMacInt32(XLM_RUN_MODE) != MODE_EMUL_OP)
		printf("FATAL: Execute68k() not called from EMUL_OP mode\n");
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_cr = get_cr();

	// Create MacOS stack frame
	// FIXME: make sure MacOS doesn't expect PPC registers to live on top
	uint32 sp = gpr(1);
	gpr(1) -= 56;
	WriteMacInt32(gpr(1), sp);

	// Save PowerPC registers
	uint32 saved_GPRs[19];
	memcpy(&saved_GPRs[0], &gpr(13), sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	double saved_FPRs[18];
	memcpy(&saved_FPRs[0], &fpr(14), sizeof(double)*(32-14));
#endif

	// Setup registers for 68k emulator
	cr().set(CR_SO_field<2>::mask());			// Supervisor mode
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  gpr(8 + i) = r->d[i];
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  gpr(16 + i) = r->a[i];
	gpr(23) = 0;
	gpr(24) = entry;
	gpr(25) = ReadMacInt32(XLM_68K_R25);		// MSB of SR
	gpr(26) = 0;
	gpr(28) = 0;								// VBR
	gpr(29) = ReadMacInt32(KERNEL_DATA_BASE + 0x1074);		// Pointer to opcode table
	gpr(30) = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);		// Address of emulator
	gpr(31) = KernelDataAddr + 0x1000;

	// Push return address (points to EXEC_RETURN opcode) on stack
	gpr(1) -= 4;
	WriteMacInt32(gpr(1), XLM_EXEC_RETURN_OPCODE);

	// Rentering 68k emulator
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

	// Set r0 to 0 for 68k emulator
	gpr(0) = 0;

	// Execute 68k opcode
	uint32 opcode = ReadMacInt16(gpr(24));
	gpr(27) = (int32)(int16)ReadMacInt16(gpr(24) += 2);
	gpr(29) += opcode * 8;
	execute(gpr(29));

	// Save r25 (contains current 68k interrupt level)
	WriteMacInt32(XLM_68K_R25, gpr(25));

	// Reentering EMUL_OP mode
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);

	// Save 68k registers
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  r->d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  r->a[i] = gpr(16 + i);

	// Restore PowerPC registers
	memcpy(&gpr(13), &saved_GPRs[0], sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	memcpy(&fpr(14), &saved_FPRs[0], sizeof(double)*(32-14));
#endif

	// Cleanup stack
	gpr(1) += 56;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	set_cr(saved_cr);

#if EMUL_TIME_STATS
	exec68k_time += (clock() - exec68k_start);
#endif
}

// Call MacOS PPC code
uint32 sheepshaver_cpu::execute_macos_code(uint32 tvect, int nargs, uint32 const *args)
{
#if EMUL_TIME_STATS
	macos_exec_count++;
	const clock_t macos_exec_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();

	// Build trampoline with EXEC_RETURN
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	lr() = trampoline.addr();

	gpr(1) -= 64;								// Create stack frame
	uint32 proc = ReadMacInt32(tvect);			// Get routine address
	uint32 toc = ReadMacInt32(tvect + 4);		// Get TOC pointer

	// Save PowerPC registers.
	uint32 regs[9];
	regs[0] = gpr(2);
	for (int i = 0; i < nargs; i++)
		regs[i + 1] = gpr(i + 3);

	// Prepare and call MacOS routine
	gpr(2) = toc;
	for (int i = 0; i < nargs; i++)
		gpr(i + 3) = args[i];
	execute(proc);
	uint32 retval = gpr(3);

	// Restore PowerPC registers
	for (int i = 0; i <= nargs; i++)
		gpr(i + 2) = regs[i];

	// Cleanup stack
	gpr(1) += 64;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;

#if EMUL_TIME_STATS
	macos_exec_time += (clock() - macos_exec_start);
#endif

	return retval;
}

/*
 *  Deliver PowerPC program and trace exceptions through the nanokernel.
 *
 *  SheepShaver already runs the ROM nanokernel for interrupts, but historically
 *  patched out SPRG3 and the final rfi because the CPU core did not model the
 *  corresponding architectural registers. A taken trap must enter that same
 *  path: LowLevelExceptionHandler and the nanokernel perform the context handoff
 *  which allows a debugger process to run while its target is stopped.
 *
 *  The CPU core now executes the nanokernel's original SRR0/SRR1/rfi return
 *  sequence. If rfi restores MSR[SE], return_from_exception redirects it to a
 *  generic one-instruction trampoline; that instruction is followed by the
 *  real trace vector. No guest clock, application, or System resource is
 *  modified.
 */

static const uint32 PPC_MSR_SE = 1u << (31 - 21);
static const uint32 PPC_SRR1_PROGRAM_TRAP = 0x00020000;

// Exception entry and rfi semantics for a 32-bit PowerPC implementation.
// SRR1 carries MSR bits 0, 5-9 and 16-31; bits 1-4 and 10-15 contain
// exception-specific information. The new exception context clears POW, EE,
// PR, FP, FE0, SE, BE, FE1, IR, DR, RI and LE, then copies ILE to LE.
static const uint32 PPC_RFI_MSR_MASK = 0x87c0ffff;
static const uint32 PPC_MSR_ILE = 1u << (31 - 15);
static const uint32 PPC_MSR_LE = 1u << (31 - 31);
static const uint32 PPC_EXCEPTION_MSR_CLEAR_MASK = 0x0004ef33;

static uint32 ppc_exception_msr(uint32 old_msr)
{
	uint32 new_msr = old_msr & ~PPC_EXCEPTION_MSR_CLEAR_MASK;
	if ((old_msr & PPC_MSR_ILE) != 0)
		new_msr |= PPC_MSR_LE;
	return new_msr;
}

static uint32 ppc_exception_srr1(uint32 old_msr, uint32 cause)
{
	return (old_msr & PPC_RFI_MSR_MASK) | cause;
}

static const uint32 PPC_EXTERNAL_VECTOR = 0x00300500;
static const uint32 PPC_PROGRAM_VECTOR = 0x00300700;
static const uint32 PPC_DECREMENTER_VECTOR = 0x00300900;
static const uint32 PPC_TRACE_VECTOR = 0x00300d00;
static const uint32 PPC_EXTERNAL_HANDLER_SLOT = 0x14;
static const uint32 PPC_PROGRAM_HANDLER_SLOT = 0x1c;
static const uint32 PPC_DECREMENTER_HANDLER_SLOT = 0x24;
static const uint32 PPC_TRACE_HANDLER_SLOT = 0x34;
static const uint32 PPC_VECTOR_TAG_OFFSET = 0x24;

static const char *ppc_exception_vector_name(uint32 vector)
{
	if (vector == PPC_EXTERNAL_VECTOR)
		return "external";
	if (vector == PPC_TRACE_VECTOR)
		return "trace";
	if (vector == PPC_DECREMENTER_VECTOR)
		return "decrementer";
	return "program";
}

static bool guest_addr_ok(uint32 a, uint32 len);
extern uint64 IdleWaitUsec;
extern unsigned long IdleWaitCount;

void sheepshaver_cpu::record_rfi_site(uint32 return_pc, uint32 run_mode)
{
	for (unsigned i = 0; i < EXCEPTION_RFI_SITE_COUNT; i++) {
		if (exception_rfi_sites[i].count != 0 &&
			exception_rfi_sites[i].pc == return_pc &&
			exception_rfi_sites[i].mode == run_mode) {
			exception_rfi_sites[i].count++;
			return;
		}
	}
	for (unsigned i = 0; i < EXCEPTION_RFI_SITE_COUNT; i++) {
		if (exception_rfi_sites[i].count == 0) {
			exception_rfi_sites[i].pc = return_pc;
			exception_rfi_sites[i].mode = run_mode;
			exception_rfi_sites[i].count = 1;
			return;
		}
	}
	exception_rfi_site_overflow++;
}

void sheepshaver_cpu::report_and_reset_rfi_sites(void)
{
	bool reported[EXCEPTION_RFI_SITE_COUNT];
	memset(reported, 0, sizeof(reported));
	for (unsigned rank = 0; rank < 12; rank++) {
		unsigned best = EXCEPTION_RFI_SITE_COUNT;
		for (unsigned i = 0; i < EXCEPTION_RFI_SITE_COUNT; i++) {
			if (!reported[i] && exception_rfi_sites[i].count != 0 &&
				(best == EXCEPTION_RFI_SITE_COUNT ||
				exception_rfi_sites[i].count > exception_rfi_sites[best].count))
				best = i;
		}
		if (best == EXCEPTION_RFI_SITE_COUNT)
			break;
		reported[best] = true;
		gfx_log_emit("[exception-rfi] ",
			"rank %u: return %08x, mode %u, %llu occurrence(s)\n",
			rank + 1, exception_rfi_sites[best].pc,
			exception_rfi_sites[best].mode,
			(unsigned long long)exception_rfi_sites[best].count);
	}
	if (exception_rfi_site_overflow != 0)
		gfx_log_emit("[exception-rfi] ",
			"%llu return(s) used sites beyond the %u-entry exact table\n",
			(unsigned long long)exception_rfi_site_overflow,
			(unsigned)EXCEPTION_RFI_SITE_COUNT);
	memset(exception_rfi_sites, 0, sizeof(exception_rfi_sites));
	exception_rfi_site_overflow = 0;
}

/*
 * Low-memory Ticks is system time, not part of a process's private context.
 * The real nanokernel can change address spaces on rfi. SheepShaver has a flat
 * guest address space instead, and Mac OS's software context handoff can expose
 * an older low-memory image after the VBL was serviced in the 68K context.
 *
 * Never manufacture time here: accept only values the guest VBL handler has
 * already reached, and prevent a later context restore from moving that value
 * backwards. The signed subtraction is the standard wrap-safe comparison for
 * the 32-bit counter (valid for deltas shorter than half its wrap period).
 */
void sheepshaver_cpu::preserve_system_ticks(void)
{
	if (!HasMacStarted()) {
		system_ticks_valid = false;
		return;
	}

	const uint32 current = ReadMacInt32(0x16a);
	if (!system_ticks_valid) {
		system_ticks_high_water = current;
		system_ticks_valid = true;
		return;
	}

	const int32 advance = (int32)(current - system_ticks_high_water);
	if (advance >= 0) {
		system_ticks_high_water = current;
		return;
	}

	const uint32 rollback = system_ticks_high_water - current;
	WriteMacInt32(0x16a, system_ticks_high_water);
	system_tick_correction_count++;
	system_tick_recovered_total += rollback;
	if (rollback > system_tick_max_rollback)
		system_tick_max_rollback = rollback;
}

void sheepshaver_cpu::exception_diagnostic_state(const char *reason, uint64 now)
{
	decrementer_diagnostics_t dec;
	get_decrementer_diagnostics(dec);
	InterruptServiceDiagnostics service;
	GetInterruptServiceDiagnostics(service);

	const uint32 run_mode = ReadMacInt32(XLM_RUN_MODE);
	const int32 irq_nest = (int32)ReadMacInt32(XLM_IRQ_NEST);
	const uint32 r25 = ReadMacInt32(XLM_68K_R25);
	const uint32 context_68k = ReadMacInt32(KernelDataAddr + 0x658);
	const uint32 current_context = ReadMacInt32(KernelDataAddr + 0x65c);
	const uint32 kernel_status = ReadMacInt32(KernelDataAddr + 0x660);
	const uint32 kernel_dec = ReadMacInt32(KernelDataAddr + 0x668);
	const uint32 irq_mask = ReadMacInt32(KernelDataAddr + 0x674);
	const uint32 level_address = ReadMacInt32(KernelDataAddr + 0x67c);
	const uint32 timebase_frequency = ReadMacInt32(KernelDataAddr + 0xf6c);
	const bool level_valid = guest_addr_ok(level_address, 2);
	const uint32 level = level_valid ? ReadMacInt16(level_address) : 0xffffffffu;

	const bool context_68k_valid = guest_addr_ok(context_68k, 0x118);
	const bool current_context_valid = guest_addr_ok(current_context, 0x118);
	const uint32 context_68k_cr = context_68k_valid
		? ReadMacInt32(context_68k + 0xdc) : 0xdead0001u;
	const uint32 context_68k_pc = context_68k_valid
		? ReadMacInt32(context_68k + 0xfc) : 0xdead0001u;
	const uint32 context_68k_sp = context_68k_valid
		? ReadMacInt32(context_68k + 0x10c) : 0xdead0001u;
	const uint32 current_cr = current_context_valid
		? ReadMacInt32(current_context + 0xdc) : 0xdead0002u;
	const uint32 current_pc = current_context_valid
		? ReadMacInt32(current_context + 0xfc) : 0xdead0002u;
	const uint32 current_sp = current_context_valid
		? ReadMacInt32(current_context + 0x10c) : 0xdead0002u;
	gfx_log_emit("[exception-state] ",
		"%s +%lu ms: pc=%08x lr=%08x sp=%08x msr=%08x "
		"srr0=%08x srr1=%08x sprg3=%08x\n",
		reason,
		exception_last_program_usec != 0
			? (unsigned long)((now - exception_last_program_usec) / 1000) : 0UL,
		pc(), lr(), gpr(1), msr(), srr0(), srr1(), sprg(3));
	gfx_log_emit("[exception-state] ",
		"mode=%u exec-depth=%d nest=%d r25=%08x flags=%08x spc=%08x "
		"tick=%08x canonical=%08x qhead=%08x; "
		"KD status=%08x savedDEC=%08x timebaseHz=%08x mask=%08x "
		"level@%08x=%08x\n",
		run_mode, current_execute_depth(), irq_nest, r25, (uint32)InterruptFlags,
		spcflags().get(),
		ReadMacInt32(0x16a), system_ticks_high_water, ReadMacInt32(0x14c), kernel_status,
		kernel_dec, timebase_frequency, irq_mask, level_address, level);
	gfx_log_emit("[exception-state] ",
		"68kctx=%08x valid=%u pc=%08x sp=%08x cr=%08x; "
		"current=%08x valid=%u pc=%08x sp=%08x cr=%08x\n",
		context_68k, context_68k_valid ? 1u : 0u, context_68k_pc,
		context_68k_sp, context_68k_cr, current_context,
		current_context_valid ? 1u : 0u, current_pc, current_sp, current_cr);
	gfx_log_emit("[exception-state] ",
		"DEC=%08x pending=%u writes=%llu delivered=%llu last=%08x "
		"range=%08x..%08x; OP_IRQ=%llu VIA-serviced=%llu; "
		"tick corrections=%llu recovered=%llu max-rollback=%u\n",
		dec.current, dec.pending ? 1u : 0u,
		(unsigned long long)dec.write_count,
		(unsigned long long)dec.delivery_count, dec.last_write,
		dec.minimum_write, dec.maximum_write,
		(unsigned long long)service.op_irq_entries,
		(unsigned long long)service.via_services,
		(unsigned long long)system_tick_correction_count,
		(unsigned long long)system_tick_recovered_total,
		system_tick_max_rollback);
}

void sheepshaver_cpu::exception_idle_diagnostic(void)
{
	if (exception_last_program_usec == 0)
		return;
	const uint64 now = GetTicks_usec();
	if (now - exception_last_program_usec > 20000000 ||
		now - exception_stall_last_usec < 1000000)
		return;
	exception_stall_last_usec = now;
	exception_diagnostic_state("idle-stall", now);
}

bool sheepshaver_cpu::exception_step_trampoline_ready(void)
{
	if (exception_step_trampoline != 0)
		return true;
	exception_step_trampoline = NativeFunction(NATIVE_EXCEPTION_STEP);
	return exception_step_trampoline != 0;
}

const char *sheepshaver_cpu::enter_exception_vector(
	uint32 vector, uint32 handler_slot, uint32 saved_pc, uint32 saved_msr)
{
	const uint32 table = KernelDataAddr +
		(ReadMacInt32(XLM_RUN_MODE) == MODE_NATIVE
			? PPC_NATIVE_EXCEPTION_TABLE_OFFSET
			: PPC_68K_EXCEPTION_TABLE_OFFSET);
	sprg(3) = table;
	const uint32 vector_pc = ROMBase + vector;
	if (!guest_addr_ok(table + handler_slot, 4))
		return "the nanokernel exception table is not mapped";
	const uint32 handler = ReadMacInt32(table + handler_slot);
	if (handler == 0 || !guest_addr_ok(handler, 4))
		return "the nanokernel exception handler is not installed";
	if (!guest_addr_ok(vector_pc, PPC_VECTOR_TAG_OFFSET + 4))
		return "the nanokernel exception vector is not mapped";
	const uint32 first = ReadMacInt32(vector_pc);
	if ((first & 0xfc000003) != 0x48000000 ||
		ReadMacInt32(vector_pc + PPC_VECTOR_TAG_OFFSET) != (vector & 0xffff))
		return "the nanokernel exception vector has an unsupported layout";
	const int32 nest = (int32)ReadMacInt32(XLM_IRQ_NEST);
	if (nest < 0 || nest == 0x7fffffff)
		return "the nanokernel interrupt nesting state is invalid";
	if (exception_entry_depth == 0xffffffffu)
		return "PowerPC exception nesting overflowed";

	// These are precisely the state changes made by PowerPC exception entry.
	// The vector itself saves r1/LR in SPRG1/SPRG2 and dispatches through SPRG3.
	// Pair the hardware exception with the same bookkeeping increment used by
	// SheepShaver's other nanokernel entry paths. The architectural rfi handler
	// consumes it on return.
	const uint32 interrupted_msr = msr();
	srr0() = saved_pc;
	srr1() = saved_msr;
	msr() = ppc_exception_msr(interrupted_msr);
	WriteMacInt32(XLM_IRQ_NEST, (uint32)nest + 1);
	exception_entry_depth++;
	exception_last_vector = vector;
	exception_last_vector_usec = GetTicks_usec();
	if (vector == PPC_TRACE_VECTOR)
		exception_traces_since_program++;
	else if (vector == PPC_DECREMENTER_VECTOR)
		exception_decrementers_since_program++;
	pc() = vector_pc;
	return NULL;
}

bool sheepshaver_cpu::decrementer_exception()
{
	return enter_exception_vector(PPC_DECREMENTER_VECTOR,
		PPC_DECREMENTER_HANDLER_SLOT, pc(), ppc_exception_srr1(msr(), 0)) == NULL;
}

bool sheepshaver_cpu::external_interrupt()
{
	// The 68K and EMUL_OP modes deliberately retain their existing callbacks:
	// they are host-created nested execution frames, not a native PowerPC
	// context which can take a hardware vector. Native mode must not use that
	// callback. It used to fabricate a nanokernel frame, execute it recursively,
	// and then restore SRR/MSR, preventing an interrupt-driven context switch
	// from becoming the CPU's actual continuation.
	const uint32 run_mode = ReadMacInt32(XLM_RUN_MODE);
	if (run_mode != MODE_NATIVE)
		return powerpc_cpu::external_interrupt();

	// A native exception may context-switch and make its rfi continuation the
	// CPU's new architectural state. That is valid only for the outer execution
	// frame; returning through a nested host helper would restore stale state.
	if (current_execute_depth() != 1) {
		external_interrupt_mode_deferred_count++;
		return false;
	}

	if (int32(ReadMacInt32(XLM_IRQ_NEST)) > 0) {
		external_interrupt_nest_deferred_count++;
		return false;
	}

#ifdef USE_SDL_VIDEO
	// SDL requires event pumping on the thread which established video mode.
	SDL_PumpEvents();
#endif
#if TARGET_OS_MACCATALYST
	catalyst_pump_appkit_events();
#endif

	// Present the VIA level and its nanokernel pending bit before entering the
	// hardware vector. Both stores are level/idempotent: if early boot has not
	// installed the vector yet, the asserted CPU request remains pending and a
	// later boundary retries it.
	const uint32 interrupt_level =
		ReadMacInt32(KERNEL_DATA_BASE + 0x67c);
	const uint32 context = ReadMacInt32(KERNEL_DATA_BASE + 0x658);
	if (!guest_addr_ok(interrupt_level, 2) ||
		!guest_addr_ok(context + 0xdc, 4)) {
		external_interrupt_mode_deferred_count++;
		return false;
	}
	WriteMacInt16(interrupt_level, 1);
	WriteMacInt32(context + 0xdc,
		ReadMacInt32(context + 0xdc) |
		ReadMacInt32(KERNEL_DATA_BASE + 0x674));

	const char *why = enter_exception_vector(PPC_EXTERNAL_VECTOR,
		PPC_EXTERNAL_HANDLER_SLOT, pc(), ppc_exception_srr1(msr(), 0));
	if (why != NULL) {
		external_interrupt_mode_deferred_count++;
		return false;
	}

#if EMUL_TIME_STATS
	interrupt_count++;
#endif
	external_interrupt_accepted_count++;
	external_interrupt_native_vector_count++;
	return true;
}

const char *sheepshaver_cpu::deliver_trap_exception(uint32 opcode)
{
	if (exception_step_active) {
		exception_step_trap = true;
		exception_step_opcode = opcode;
		return NULL;
	}
	if (!PrefsFindBool("ppcexceptions"))
		return "PowerPC exception delivery is disabled";
	if (!exception_step_trampoline_ready())
		return "the single-step return trampoline could not be allocated";
	preserve_system_ticks();

	const uint32 trap_pc = pc();
	const uint64 now = GetTicks_usec();
	const uint64 idle_now = IdleWaitUsec;
	const unsigned long idle_count_now = IdleWaitCount;
	const uint32 tick_now = ReadMacInt32(0x16a);
	const int32 nest_before = (int32)ReadMacInt32(XLM_IRQ_NEST);
	const uint64 previous_program = exception_last_program_usec;
	const uint32 previous_returns = exception_returns_since_program;
	const uint32 previous_traces = exception_traces_since_program;
	const uint32 previous_decrementers = exception_decrementers_since_program;
	InterruptServiceDiagnostics service_now;
	GetInterruptServiceDiagnostics(service_now);
	const char *why = enter_exception_vector(PPC_PROGRAM_VECTOR,
		PPC_PROGRAM_HANDLER_SLOT, trap_pc,
		ppc_exception_srr1(msr(), PPC_SRR1_PROGRAM_TRAP));
	if (why != NULL)
		return why;

	if (previous_program != 0) {
		gfx_log_emit("[exception] ",
			"program exception at %08x: entering nanokernel handler "
			"(since previous: %lu ms, %lu ms idle in %lu waits, "
			"%u guest ticks, %u rfi returns, %u trace and %u decrementer "
			"exceptions; "
			"nest %d->%d)\n",
			trap_pc,
			(unsigned long)((now - previous_program) / 1000),
			(unsigned long)((idle_now - exception_idle_snapshot_usec) / 1000),
			idle_count_now - exception_idle_snapshot_count,
			tick_now - exception_tick_snapshot,
			previous_returns, previous_traces, previous_decrementers,
			nest_before,
			(int32)ReadMacInt32(XLM_IRQ_NEST));
		gfx_log_emit("[exception] ",
			"external interrupt arbitration since previous: %llu accepted "
			"(%llu native vectors, %llu 68k, %llu emul-op); "
			"%llu nest, %llu 68k-IPL and %llu "
			"mode boundary retries\n",
			(unsigned long long)(external_interrupt_accepted_count -
				external_interrupt_accepted_snapshot),
			(unsigned long long)(external_interrupt_native_vector_count -
				external_interrupt_native_vector_snapshot),
			(unsigned long long)(external_interrupt_68k_accepted_count -
				external_interrupt_68k_accepted_snapshot),
			(unsigned long long)(external_interrupt_emul_accepted_count -
				external_interrupt_emul_accepted_snapshot),
			(unsigned long long)(external_interrupt_nest_deferred_count -
				external_interrupt_nest_deferred_snapshot),
			(unsigned long long)(external_interrupt_68k_deferred_count -
				external_interrupt_68k_deferred_snapshot),
			(unsigned long long)(external_interrupt_mode_deferred_count -
				external_interrupt_mode_deferred_snapshot));
		const uint64 service_delta = service_now.via_services -
			exception_service_snapshot.via_services;
		const uint64 op_irq_delta = service_now.op_irq_entries -
			exception_service_snapshot.op_irq_entries;
		gfx_log_emit("[exception] ",
			"interrupt service since previous: %llu OP_IRQ entr%s, "
			"%llu VIA service(s)\n",
			(unsigned long long)op_irq_delta,
			op_irq_delta == 1 ? "y" : "ies",
			(unsigned long long)service_delta);
		gfx_log_emit("[exception] ",
			"system TickCount preservation since previous: %llu stale "
			"context restore(s), %llu tick(s) recovered; canonical=%08x, "
			"lifetime maximum rollback=%u tick(s)\n",
			(unsigned long long)(system_tick_correction_count -
				system_tick_correction_snapshot),
			(unsigned long long)(system_tick_recovered_total -
				system_tick_recovered_snapshot),
			system_ticks_high_water, system_tick_max_rollback);
		report_and_reset_rfi_sites();
	} else {
		gfx_log_emit("[exception] ",
			"program exception at %08x: entering nanokernel handler "
			"(nest %d->%d)\n", trap_pc, nest_before,
			(int32)ReadMacInt32(XLM_IRQ_NEST));
		memset(exception_rfi_sites, 0, sizeof(exception_rfi_sites));
		exception_rfi_site_overflow = 0;
	}
	exception_last_program_usec = now;
	exception_idle_snapshot_usec = idle_now;
	exception_idle_snapshot_count = idle_count_now;
	exception_tick_snapshot = tick_now;
	exception_returns_since_program = 0;
	exception_traces_since_program = 0;
	exception_decrementers_since_program = 0;
	exception_stall_last_usec = now;
	exception_service_snapshot = service_now;
	external_interrupt_accepted_snapshot = external_interrupt_accepted_count;
	external_interrupt_nest_deferred_snapshot =
		external_interrupt_nest_deferred_count;
	external_interrupt_native_vector_snapshot =
		external_interrupt_native_vector_count;
	external_interrupt_68k_accepted_snapshot =
		external_interrupt_68k_accepted_count;
	external_interrupt_emul_accepted_snapshot =
		external_interrupt_emul_accepted_count;
	external_interrupt_68k_deferred_snapshot =
		external_interrupt_68k_deferred_count;
	external_interrupt_mode_deferred_snapshot =
		external_interrupt_mode_deferred_count;
	system_tick_correction_snapshot = system_tick_correction_count;
	system_tick_recovered_snapshot = system_tick_recovered_total;
	exception_diagnostic_state("program-entry", now);
	return NULL;
}

// Complete the nanokernel's real rfi. Besides restoring architectural state,
// this consumes the host-visible interrupt nesting level which SheepShaver
// increments before entering the nanokernel. Keeping that bookkeeping here
// removes a native callback from every context return.
void sheepshaver_cpu::return_from_exception(uint32 saved_pc, uint32 saved_msr)
{
	msr() = (msr() & ~PPC_RFI_MSR_MASK) |
		(saved_msr & PPC_RFI_MSR_MASK);
	// The guest has completed its context restore before executing rfi. Keep
	// global system time from being replaced by that context's stale snapshot.
	preserve_system_ticks();
	uint32 return_pc = saved_pc & ~3u;
	const uint32 run_mode = ReadMacInt32(XLM_RUN_MODE);
	if (exception_last_program_usec != 0)
		record_rfi_site(return_pc, run_mode);
	const int32 nest = (int32)ReadMacInt32(XLM_IRQ_NEST);
	if (nest > 0)
		WriteMacInt32(XLM_IRQ_NEST, (uint32)nest - 1);
	else if (exception_entry_depth != 0)
		gfx_log_emit("[crash] ",
			"PowerPC rfi has invalid nanokernel nesting state %d\n", nest);

	exception_returns_since_program++;
	if (exception_entry_depth != 0) {
		const uint64 now = GetTicks_usec();
		const uint64 vector_usec = now - exception_last_vector_usec;
		if (exception_last_vector == PPC_PROGRAM_VECTOR ||
			(msr() & PPC_MSR_SE) != 0 || vector_usec >= 100000) {
			gfx_log_emit("[exception] ",
				"%s vector returned in %lu us to %08x "
				"(SE=%u, nest=%d, architectural depth=%u)\n",
				ppc_exception_vector_name(exception_last_vector),
				(unsigned long)vector_usec, return_pc,
				(msr() & PPC_MSR_SE) != 0 ? 1u : 0u,
				(int32)ReadMacInt32(XLM_IRQ_NEST), exception_entry_depth);
		}
		exception_entry_depth--;
	}

	if ((msr() & PPC_MSR_SE) != 0) {
		// An asynchronous exception may be recognized immediately after the
		// rfi which armed single-step, before the software trampoline executes.
		// Its own rfi returns to that trampoline with SE still set; preserve the
		// original instruction address instead of replacing it with the
		// trampoline's address.
		if (exception_step_pending &&
			return_pc == exception_step_trampoline) {
			// The already-armed step continues below at the same trampoline.
		} else if (exception_step_trampoline_ready()) {
			exception_step_pc = return_pc;
			exception_step_pending = true;
			return_pc = exception_step_trampoline;
			const uint64 now = GetTicks_usec();
			gfx_log_emit("[exception] ",
				"single-step return redirected to %08x (%lu ms since last "
				"program exception, %lu ms idle, "
				"%u guest ticks, nest=%d)\n",
				exception_step_pc,
				exception_last_program_usec != 0
					? (unsigned long)((now - exception_last_program_usec) / 1000)
					: 0UL,
				(unsigned long)((IdleWaitUsec - exception_idle_snapshot_usec) / 1000),
				ReadMacInt32(0x16a) - exception_tick_snapshot,
				(int32)ReadMacInt32(XLM_IRQ_NEST));
			exception_diagnostic_state("step-ready", now);
		} else {
			gfx_log_emit("[crash] ",
				"could not allocate the PowerPC trace trampoline\n");
			msr() &= ~PPC_MSR_SE;
		}
	}
	pc() = return_pc;
}

// Reached only when the handler returned with MSR[SE]. At this point the
// nanokernel has restored the interrupted register file and its nesting count,
// so execute exactly one instruction and enter the appropriate hardware vector.
void sheepshaver_cpu::exception_step(void)
{
	if (!exception_step_pending) {
		increment_pc(4);
		return;
	}

	pc() = exception_step_pc;
	const uint32 stepped_pc = pc();
	const uint32 stepped_opcode = ReadMacInt32(stepped_pc);
	exception_step_pending = false;
	exception_step_trap = false;
	exception_step_opcode = 0;
	exception_step_active = true;
	const uint64 step_started = GetTicks_usec();
	execute_one_instruction();
	const uint64 step_usec = GetTicks_usec() - step_started;
	exception_step_active = false;

	const bool trapped = exception_step_trap;
	const uint32 vector = trapped ? PPC_PROGRAM_VECTOR : PPC_TRACE_VECTOR;
	const uint32 slot = trapped
		? PPC_PROGRAM_HANDLER_SLOT : PPC_TRACE_HANDLER_SLOT;
	const uint32 saved_msr = ppc_exception_srr1(msr(),
		trapped ? PPC_SRR1_PROGRAM_TRAP : 0);
	const uint32 saved_pc = pc();
	gfx_log_emit("[exception] ",
		"single instruction at %08x (%08x) took %lu us; raising %s "
		"exception at %08x\n", stepped_pc, stepped_opcode,
		(unsigned long)step_usec, trapped ? "program" : "trace", saved_pc);
	const char *why =
		enter_exception_vector(vector, slot, saved_pc, saved_msr);
	if (why == NULL)
		return;

	gfx_log_emit("[crash] ",
		"%s exception after stepping at %08x could not enter the "
		"nanokernel: %s\n", trapped ? "program" : "trace", saved_pc, why);
	report_fault(trapped ? exception_step_opcode : 0);
	pc() = trapped ? saved_pc + 4 : saved_pc;
}
// Execute ppc routine
inline void sheepshaver_cpu::execute_ppc(uint32 entry)
{
	// Save branch registers
	uint32 saved_lr = lr();

	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	WriteMacInt32(trampoline.addr(), POWERPC_EXEC_RETURN);
	lr() = trampoline.addr();

	execute(entry);

	// Restore branch registers
	lr() = saved_lr;
}

void sheepshaver_cpu::call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource) {
	static_cast<sheepshaver_cpu *>(cpu)->get_resource(old_get_resource);
}

// Resource Manager thunk
inline void sheepshaver_cpu::get_resource(uint32 old_get_resource)
{
	uint32 type = gpr(3);
	int16 id = gpr(4);

	// Create stack frame
	gpr(1) -= 56;

	// Call old routine
	execute_ppc(old_get_resource);

	// Call CheckLoad()
	uint32 handle = gpr(3);
	check_load_invoc(type, id, handle);
	gpr(3) = handle;

	// Cleanup stack
	gpr(1) += 56;
}


/**
 *		SheepShaver CPU engine interface
 **/

// PowerPC CPU emulator
static sheepshaver_cpu *ppc_cpu = NULL;

void PPCExceptionIdleDiagnostic(void)
{
	if (ppc_cpu != NULL)
		ppc_cpu->exception_idle_diagnostic();
}

void FlushCodeCache(uintptr start, uintptr end)
{
	D(bug("FlushCodeCache(%08x, %08x)\n", start, end));
	ppc_cpu->invalidate_cache_range(start, end);
}

// Dump PPC registers
static void dump_registers(void)
{
	ppc_cpu->dump_registers();
}

// Dump log
static void dump_log(void)
{
	ppc_cpu->dump_log();
}

static int read_mem(bfd_vma memaddr, bfd_byte *myaddr, int length, struct disassemble_info *info)
{
	Mac2Host_memcpy(myaddr, memaddr, length);
	return 0;
}

static void dump_disassembly(const uint32 pc, const int prefix_count, const int suffix_count)
{
	struct disassemble_info info;
	INIT_DISASSEMBLE_INFO(info, stderr, fprintf);
	info.read_memory_func = read_mem;

	const int count = prefix_count + suffix_count + 1;
	const uint32 base_addr = pc - prefix_count * 4;
	for (int i = 0; i < count; i++) {
		const bfd_vma addr = base_addr + i * 4;
		fprintf(stderr, "%s0x%8llx:  ", addr == pc ? " >" : "  ", addr);
		print_insn_ppc(addr, &info);
		fprintf(stderr, "\n");
	}
}

// Crash-context diagnostics (StarCraft post-splash SIGSEGV investigation):
// validated guest reads only -- a wild pc/sp must not re-fault inside the
// signal handler (dump_disassembly previously died reading unmapped memory).
static bool guest_addr_ok(uint32 a, uint32 len)
{
	if (a >= RAMBase && a - RAMBase < RAMSize && RAMSize - (a - RAMBase) >= len)
		return true;
	if (a >= ROMBase && a - ROMBase < ROM_AREA_SIZE && ROM_AREA_SIZE - (a - ROMBase) >= len)
		return true;
	const uint32 kbase = (uint32)(KERNEL_DATA_BASE & ~0x3fffu);
	const uint32 kend = (uint32)(KERNEL_DATA_BASE + KERNEL_AREA_SIZE);
	if (a >= kbase && a < kend && kend - a >= len)
		return true;
	// Native-op TVECTs / thunks live in SheepMem (guest 0x51xxxxxx on Windows).
	if (SheepMem::Contains(a) && SheepMem::Size() - (a - SheepMem::Base()) >= len)
		return true;
	return false;
}

bool PPCGuestAddressValid(uint32 addr, uint32 len)
{
	return guest_addr_ok(addr, len);
}

/*
 *  Report an access execute_loadstore() flagged, before it performs it.  That
 *  is the only place the state can be recorded when the access itself takes
 *  the process down and no fault handler ever runs.
 */

/* True the first time this pc is reported, so a routine looping over a bad
   pointer cannot bury the log and stall the emulation. */

static bool ppc_report_is_new(uint32 pc, uint32 *seen, int seen_max,
	int *seen_count)
{
	int i;

	for (i = 0; i < *seen_count; i++)
		if (seen[i] == pc)
			return false;
	if (*seen_count >= seen_max)
		return false;
	seen[(*seen_count)++] = pc;
	return true;
}

static void ppc_report_context(const char *what, uint32 pc, uint32 ea)
{
	sheepshaver_cpu *cpu = ppc_cpu;
	char msg[512];
	int i;

	snprintf(msg, sizeof(msg),
		"[bad-ea] %s ea=%08x pc=%08x 68k-pc=%08x lr=%08x ctr=%08x\n"
		"  r0-r7   %08x %08x %08x %08x %08x %08x %08x %08x\n"
		"  r8-r15  %08x %08x %08x %08x %08x %08x %08x %08x\n"
		"  r16-r23 %08x %08x %08x %08x %08x %08x %08x %08x\n"
		"  r24-r31 %08x %08x %08x %08x %08x %08x %08x %08x\n",
		what, ea, pc, cpu->gpr(24),
		cpu->get_register(powerpc_registers::LR).i,
		cpu->get_register(powerpc_registers::CTR).i,
		cpu->gpr(0), cpu->gpr(1), cpu->gpr(2), cpu->gpr(3),
		cpu->gpr(4), cpu->gpr(5), cpu->gpr(6), cpu->gpr(7),
		cpu->gpr(8), cpu->gpr(9), cpu->gpr(10), cpu->gpr(11),
		cpu->gpr(12), cpu->gpr(13), cpu->gpr(14), cpu->gpr(15),
		cpu->gpr(16), cpu->gpr(17), cpu->gpr(18), cpu->gpr(19),
		cpu->gpr(20), cpu->gpr(21), cpu->gpr(22), cpu->gpr(23),
		cpu->gpr(24), cpu->gpr(25), cpu->gpr(26), cpu->gpr(27),
		cpu->gpr(28), cpu->gpr(29), cpu->gpr(30), cpu->gpr(31));
	fputs(msg, stderr);
	fflush(stderr);
#if defined(_WIN32)
	OutputDebugStringA(msg);
#endif
	/* The 68k stack.  r1 is the emulator's a7, so when the emulator itself
	   faults this holds the exception frame it just pushed -- status word,
	   then the 68k pc that took the exception, then the vector offset --
	   followed by the return addresses of whatever called into that code. */
	{
		uint32 sp = cpu->gpr(1);
		int o = 0, k;

		if (guest_addr_ok(sp, 0x60)) {
			for (k = 0; k < 0x60; k += 16) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] a7+%03x:", k);
				for (i = 0; i < 16; i += 2)
					o += snprintf(msg + o, sizeof(msg) - o, " %04x",
						ReadMacInt16(sp + k + i));
				snprintf(msg + o, sizeof(msg) - o, "\n");
				fputs(msg, stderr);
#if defined(_WIN32)
				OutputDebugStringA(msg);
#endif
			}
			fflush(stderr);
		}
	}
	/* The structure a3 points at.  If its first 108 bytes look like a
	   GrafPort but the window fields past them do not, the block is not the
	   WindowRecord the program thinks it is. */
	{
		uint32 rec = cpu->gpr(19);
		int o = 0, k;

		if (guest_addr_ok(rec, 0x90)) {
			for (k = 0; k < 0x90; k += 16) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] a3+%03x:", k);
				for (i = 0; i < 16; i += 2)
					o += snprintf(msg + o, sizeof(msg) - o, " %04x",
						ReadMacInt16(rec + k + i));
				snprintf(msg + o, sizeof(msg) - o, "\n");
				fputs(msg, stderr);
#if defined(_WIN32)
				OutputDebugStringA(msg);
#endif
			}
			fflush(stderr);
		}
	}
	/* The PowerPC instructions around the fault.  A fault in native code has
	   no meaningful 68k program counter, so this is what names the routine.
	   The window reaches well back so the call that produced the bad pointer
	   is in it, not just the instruction that used it. */
	{
		uint32 base = pc - 128;
		int o = 0, k, j;

		if (pc >= 128 && guest_addr_ok(base, 192)) {
			for (k = 0; k < 192; k += 32) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] ppc code %08x:",
					base + k);
				for (j = 0; j < 32; j += 4)
					o += snprintf(msg + o, sizeof(msg) - o, " %08x",
						ReadMacInt32(base + k + j));
				snprintf(msg + o, sizeof(msg) - o, "\n");
				fputs(msg, stderr);
#if defined(_WIN32)
				OutputDebugStringA(msg);
#endif
			}
			fflush(stderr);
		}
	}
	/* The call chain.  PowerOpen keeps the caller's stack pointer at 0(sp)
	   and the return address at 8(sp), so walking that names every routine
	   between here and whatever asked for the work. */
	{
		uint32 sp = cpu->gpr(1);
		int depth;

		for (depth = 0; depth < 16; depth++) {
			uint32 back, saved_lr;

			if (!guest_addr_ok(sp, 12))
				break;
			back = ReadMacInt32(sp);
			saved_lr = ReadMacInt32(sp + 8);
			snprintf(msg, sizeof(msg),
				"[bad-ea] frame %2d sp=%08x back=%08x lr=%08x\n",
				depth, sp, back, saved_lr);
			fputs(msg, stderr);
#if defined(_WIN32)
			OutputDebugStringA(msg);
#endif
			if (back <= sp || back - sp > 0x100000)
				break;
			sp = back;
		}
		fflush(stderr);
	}
	/* The low memory globals this class of bug tramples or reads: the 68k
	   exception vectors live below 0x100, the unit table pointer is at 0x11c
	   and the SCC register base addresses are at 0x1d8 and 0x1dc. */
	{
		int o = 0, k, j;

		for (k = 0; k < 0x200; k += 32) {
			o = snprintf(msg, sizeof(msg), "[bad-ea] lomem %03x:", k);
			for (j = 0; j < 32; j += 4)
				o += snprintf(msg + o, sizeof(msg) - o, " %08x",
					ReadMacInt32(k + j));
			snprintf(msg + o, sizeof(msg) - o, "\n");
			fputs(msg, stderr);
#if defined(_WIN32)
			OutputDebugStringA(msg);
#endif
		}
		fflush(stderr);
	}
	/* The table of contents the faulting fragment runs with.  Its first
	   entries name the fragment's own data, which is what tells one native
	   code fragment apart from another. */
	{
		uint32 toc = cpu->gpr(2);
		int o = 0, k, j;

		if (toc >= 64 && guest_addr_ok(toc - 64, 128)) {
			for (k = 0; k < 128; k += 32) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] toc %08x:",
					toc - 64 + k);
				for (j = 0; j < 32; j += 4)
					o += snprintf(msg + o, sizeof(msg) - o, " %08x",
						ReadMacInt32(toc - 64 + k + j));
				snprintf(msg + o, sizeof(msg) - o, "\n");
				fputs(msg, stderr);
#if defined(_WIN32)
				OutputDebugStringA(msg);
#endif
			}
			fflush(stderr);
		}
	}
	/* The 68k instruction stream around the faulting instruction, so it can
	   be disassembled afterwards and matched against the program's CODE. */
	{
		uint32 p68 = cpu->gpr(24);
		uint32 base = p68 - 32;
		int o = 0, k;

		if (p68 >= 32 && guest_addr_ok(base, 80)) {
			o = snprintf(msg, sizeof(msg), "[bad-ea] 68k code %08x:", base);
			for (k = 0; k < 80 && o < (int)sizeof(msg) - 4; k += 2)
				o += snprintf(msg + o, sizeof(msg) - o, " %04x",
					ReadMacInt16(base + k));
			snprintf(msg + o, sizeof(msg) - o, "\n");
			fputs(msg, stderr);
			fflush(stderr);
#if defined(_WIN32)
			OutputDebugStringA(msg);
#endif
		}
	}
}

void ppc_report_bad_ea(uint32 pc, uint32 ea, int is_load)
{
	static uint32 seen[64];
	static int seen_count = 0;
	const char *what = "store";

	if (ppc_cpu == NULL || guest_addr_ok(ea, 1))
		return;
	if (!ppc_report_is_new(pc, seen, 64, &seen_count))
		return;
	if (is_load)
		what = "load";
	ppc_report_context(what, pc, ea);
}

/* A store into the 68k exception vectors.  The address is mapped, so the
   access itself is harmless; what matters is that the vector it lands on is
   used by every A-trap the guest executes from then on. */

void ppc_report_vector_store(uint32 pc, uint32 ea)
{
	static uint32 seen[64];
	static int seen_count = 0;

	if (ppc_cpu == NULL)
		return;
	if (!ppc_report_is_new(pc, seen, 64, &seen_count))
		return;
	ppc_report_context("vector store", pc, ea);
}

static void dump_crash_context(sheepshaver_cpu *cpu)
{
	// Guest stack crawl. PowerOpen ABI: back chain at [sp], saved LR at [sp+8].
	uint32 sp = cpu->cur_gpr(1);
	fprintf(stderr, "guest stack crawl (r1=%08x):\n", sp);
	for (int depth = 0; depth < 32; depth++) {
		if (!guest_addr_ok(sp, 12)) {
			fprintf(stderr, "  [%2d] sp %08x (unmapped; stop)\n", depth, sp);
			break;
		}
		uint32 back = ReadMacInt32(sp);
		uint32 saved_lr = ReadMacInt32(sp + 8);
		fprintf(stderr, "  [%2d] sp %08x back %08x lr %08x\n", depth, sp, back, saved_lr);
		if (back <= sp || back - sp > 0x100000)
			break;
		sp = back;
	}

	// Window around the kernel-data interrupt stack. The StarCraft dump showed
	// this region holding little-endian copies of its own guest addresses;
	// flag any word still matching that signature.
	const uint32 lo = (uint32)KERNEL_DATA_BASE - 0x200;
	const uint32 hi = (uint32)KERNEL_DATA_BASE + 0x40;
	fprintf(stderr, "kernel-area dump [%08x..%08x) ('<' = LE self-address):\n", lo, hi);
	for (uint32 a = lo; a < hi; a += 16) {
		if (!guest_addr_ok(a, 16))
			continue;
		fprintf(stderr, "  %08x:", a);
		for (int i = 0; i < 4; i++) {
			uint32 w = ReadMacInt32(a + i * 4);
			uint32 le = ((w & 0xff) << 24) | ((w & 0xff00) << 8) | ((w >> 8) & 0xff00) | (w >> 24);
			fprintf(stderr, " %08x%c", w, (le - (a + i * 4)) <= 0x100 ? '<' : ' ');
		}
		fprintf(stderr, "\n");
	}

	extern void RsrcLocksDumpOnCrash(void);
	RsrcLocksDumpOnCrash();
}

sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
{
#if ENABLE_VOSF
	// Handle screen fault
	extern bool Screen_fault_handler(sigsegv_info_t *sip);
	if (Screen_fault_handler(sip))
		return SIGSEGV_RETURN_SUCCESS;
#endif

	const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);
#if HAVE_SIGSEGV_SKIP_INSTRUCTION
	// Ignore writes to ROM
	if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;

	if (!ppc_cpu) {
		// Haven't even gotten started yet?
		return SIGSEGV_RETURN_FAILURE;
	}

	// Get program counter of target CPU
	sheepshaver_cpu * const cpu = ppc_cpu;
	const uint32 pc = cpu->pc();

	// Fault while guest PC is in Mac ROM/RAM/DR cache, OR in SheepMem.
	// SheepMem holds native-op TVECTs (EMUL_OP trampolines). When a native
	// handler (e.g. VideoDoDriverIO) faults via WriteMacInt32, guest PC is
	// still the TVECT opcode address - without SheepMem here, ignoresegv
	// never applies and boot dies on the first unmapped guest store.
	bool mac_fault = (pc >= ROMBase && pc < (ROMBase + ROM_AREA_SIZE))
		|| (pc >= RAMBase && pc < (RAMBase + RAMSize))
		|| (pc >= DR_CACHE_BASE && pc < (DR_CACHE_BASE + DR_CACHE_SIZE))
		|| SheepMem::Contains(pc);
	if (mac_fault) {

		// "VM settings" during MacOS 8 installation
		if (pc == ROMBase + 0x488160 && cpu->gpr(20) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.5 installation
		else if (pc == ROMBase + 0x488140 && cpu->gpr(16) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8 serial drivers on startup
		else if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.1 serial drivers on startup
		else if (pc == ROMBase + 0x48c5e0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		else if (pc == ROMBase + 0x4a10a0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// MacOS 8.6 serial drivers on startup (with DR Cache and OldWorld ROM)
		else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(16) == 0xf3012002 || cpu->gpr(16) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore writes to the zero page (compare host addresses under NATMEM)
		else if ((uint32)(addr - (uintptr)Mac2HostAddr(SheepMem::ZeroPage())) < (uint32)SheepMem::PageSize())
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore all other faults, if requested
		if (PrefsFindBool("ignoresegv"))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	}
#else
#error "FIXME: You don't have the capability to skip instruction within signal handlers"
#endif

	fprintf(stderr, "SIGSEGV\n");
	fprintf(stderr, "  pc %p\n", sigsegv_get_fault_instruction_address(sip));
	fprintf(stderr, "  ea %p\n", sigsegv_get_fault_address(sip));
	dump_registers();
	dump_log();
	dump_crash_context(cpu);
	if (guest_addr_ok(pc - 8 * 4, (8 + 8 + 1) * 4))
		dump_disassembly(pc, 8, 8);
	else
		fprintf(stderr, "  (pc %08x outside mapped guest areas; disassembly skipped)\n", pc);

	enter_mon();
	QuitEmulator();

	return SIGSEGV_RETURN_FAILURE;
}

/*
 *  Initialize CPU emulation
 */

void init_emul_ppc(void)
{
	// Get pointer to KernelData in host address space
	kernel_data = (KernelData *)Mac2HostAddr(KERNEL_DATA_BASE);

	// Initialize main CPU emulator
	ppc_cpu = new sheepshaver_cpu();
	ppc_cpu->set_register(powerpc_registers::GPR(3), any_register((uint32)ROMBase + 0x30d000));
	ppc_cpu->set_register(powerpc_registers::GPR(4), any_register(KernelDataAddr + 0x1000));
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

#if ENABLE_MON
	// Install "regs" command in cxmon
	mon_add_command("regs", dump_registers, "regs                     Dump PowerPC registers\n");
	mon_add_command("log", dump_log, "log                      Dump PowerPC emulation log\n");
#endif

#if EMUL_TIME_STATS
	emul_start_time = clock();
#endif
}

/*
 *  Deinitialize emulation
 */

void exit_emul_ppc(void)
{
#if EMUL_TIME_STATS
	clock_t emul_end_time = clock();

	printf("### Statistics for SheepShaver emulation parts\n");
	const clock_t emul_time = emul_end_time - emul_start_time;
	printf("Total emulation time : %.1f sec\n", double(emul_time) / double(CLOCKS_PER_SEC));
	printf("Total interrupt count: %d (%2.1f Hz)\n", interrupt_count,
		   (double(interrupt_count) * CLOCKS_PER_SEC) / double(emul_time));
#define PRINT_STATS(LABEL, VAR_PREFIX) do {								\
		printf("Total " LABEL " count : %d\n", VAR_PREFIX##_count);		\
		printf("Total " LABEL " time  : %.1f sec (%.1f%%)\n",			\
			   double(VAR_PREFIX##_time) / double(CLOCKS_PER_SEC),		\
			   100.0 * double(VAR_PREFIX##_time) / double(emul_time));	\
	} while (0)

	PRINT_STATS("Execute68k[Trap] execution", exec68k);
	PRINT_STATS("NativeOp execution", native_exec);
	PRINT_STATS("MacOS routine execution", macos_exec);

#undef PRINT_STATS
	printf("\n");
#endif

	delete ppc_cpu;
	ppc_cpu = NULL;
}

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Initialize EmulOp trampolines
void init_emul_op_trampolines(basic_dyngen & dg)
{
	typedef void (*func_t)(dyngen_cpu_base, uint32);
	func_t func;

	// EmulOp
	emul_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_emul_op;
	dg.gen_invoke_CPU_T0(func);
	dg.gen_exec_return();
	dg.gen_end();

	// NativeOp
	native_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_native_op;
	dg.gen_invoke_CPU_T0(func);
	dg.gen_exec_return();
	dg.gen_end();

	D(bug("EmulOp trampoline:   %p\n", emul_op_trampoline));
	D(bug("NativeOp trampoline: %p\n", native_op_trampoline));
}
#endif

/*
 *  Emulation loop
 */

void emul_ppc(uint32 entry)
{
#if 0
	ppc_cpu->start_log();
#endif
	// start emulation loop and enable code translation or caching
	ppc_cpu->execute(entry);
}

/*
 *  Handle PowerPC interrupt
 */

void TriggerInterrupt(void)
{
	idle_resume();
	// Trigger interrupt to main cpu only
	if (ppc_cpu)
		ppc_cpu->trigger_interrupt();
}

bool HandleInterrupt(powerpc_registers *r)
{
	// The host-side interrupt request models the processor's level-sensitive
	// INT input. Do all eligibility checks before touching guest state. A false
	// return keeps the CPU request asserted until a later instruction boundary.
	if (int32(ReadMacInt32(XLM_IRQ_NEST)) > 0) {
		external_interrupt_nest_deferred_count++;
		return false;
	}

	const uint32 run_mode = ReadMacInt32(XLM_RUN_MODE);
	switch (run_mode) {
	case MODE_68K:
		break;
#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:
		if ((ReadMacInt32(XLM_68K_R25) & 7) != 0) {
			external_interrupt_68k_deferred_count++;
			return false;
		}
		break;
#endif
	default:
		external_interrupt_mode_deferred_count++;
		return false;
	}

#ifdef USE_SDL_VIDEO
	// We must fill in the events queue in the same thread that did call SDL_SetVideoMode()
	SDL_PumpEvents();
#endif
#if TARGET_OS_MACCATALYST
	// Keep UIKit responsive while the emulator owns the main thread on Catalyst.
	// Runs on the emul/main thread (~60Hz for interpreter and JIT), reentrancy-
	// guarded by processing_interrupt in check_spcflags().
	catalyst_pump_appkit_events();
#endif

	// Update interrupt count
#if EMUL_TIME_STATS
	interrupt_count++;
#endif
	external_interrupt_accepted_count++;

	// Interrupt action depends on current run mode
	switch (run_mode) {
	case MODE_68K:
		external_interrupt_68k_accepted_count++;
		// 68k emulator active, trigger 68k interrupt level 1
		WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
		r->cr.set(r->cr.get() | ReadMacInt32(KERNEL_DATA_BASE + 0x674));
		break;

#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:
		external_interrupt_emul_accepted_count++;
		// 68k emulator active, within EMUL_OP routine. Eligibility above
		// established that the emulated 68k interrupt level is zero.
#if EMUL_TIME_STATS
		const clock_t interrupt_start = clock();
#endif
#if 1
		// Execute full 68k interrupt routine
		M68kRegisters r;
		uint32 old_r25 = ReadMacInt32(XLM_68K_R25);	// Save interrupt level
		WriteMacInt32(XLM_68K_R25, 0x21);			// Execute with interrupt level 1
		static const uint8 proc_template[] = {
				0x3f, 0x3c, 0x00, 0x00,			// move.w	#$0000,-(sp)	(fake format word)
				0x48, 0x7a, 0x00, 0x0a,			// pea		@1(pc)			(return address)
				0x40, 0xe7,						// move		sr,-(sp)		(saved SR)
				0x20, 0x78, 0x00, 0x064,		// move.l	$64,a0
				0x4e, 0xd0,						// jmp		(a0)
				M68K_RTS >> 8, M68K_RTS & 0xff	// @1
		};
		BUILD_SHEEPSHAVER_PROCEDURE(proc);
		Execute68k(proc, &r);
		WriteMacInt32(XLM_68K_R25, old_r25);		// Restore interrupt level
#else
		// Only update cursor
		if (HasMacStarted()) {
			if (InterruptFlags & INTFLAG_VIA) {
				ClearInterruptFlag(INTFLAG_VIA);
				ADBInterrupt();
				ExecuteNative(NATIVE_VIDEO_VBL);
			}
		}
#endif
#if EMUL_TIME_STATS
		interrupt_time += (clock() - interrupt_start);
#endif
		break;
#endif
	}
	return true;
}

void sheepshaver_cpu::call_execute_native_op(powerpc_cpu * cpu, uint32 selector) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_native_op(selector);
}

// Execute NATIVE_OP routine
void sheepshaver_cpu::execute_native_op(uint32 selector)
{
#if EMUL_TIME_STATS
	native_exec_count++;
	const clock_t native_exec_start = clock();
#endif

	switch (selector) {
	case NATIVE_PATCH_NAME_REGISTRY:
		DoPatchNameRegistry();
		break;
	case NATIVE_VIDEO_INSTALL_ACCEL:
		VideoInstallAccel();
		break;
	case NATIVE_VIDEO_VBL:
		VideoVBL();
		break;
	case NATIVE_VIDEO_DO_DRIVER_IO:
		gpr(3) = (int32)(int16)VideoDoDriverIO(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7));
		break;
	case NATIVE_EXCEPTION_STEP:
		exception_step();
		break;
	case NATIVE_ETHER_AO_GET_HWADDR:
		AO_get_ethernet_address(gpr(3));
		break;
	case NATIVE_ETHER_AO_ADD_MULTI:
		AO_enable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_DEL_MULTI:
		AO_disable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_SEND_PACKET:
		AO_transmit_packet(gpr(3));
		break;
	case NATIVE_ETHER_IRQ:
		EtherIRQ();
		break;
	case NATIVE_ETHER_INIT:
		gpr(3) = InitStreamModule((void *)(size_t)gpr(3));
		break;
	case NATIVE_ETHER_TERM:
		TerminateStreamModule();
		break;
	case NATIVE_ETHER_OPEN:
		gpr(3) = ether_open((queue_t *)(size_t)gpr(3), (void *)(size_t)gpr(4), gpr(5), gpr(6), (void*)(size_t)gpr(7));
		break;
	case NATIVE_ETHER_CLOSE:
		gpr(3) = ether_close((queue_t *)(size_t)gpr(3), gpr(4), (void *)(size_t)gpr(5));
		break;
	case NATIVE_ETHER_WPUT:
		gpr(3) = ether_wput((queue_t *)(size_t)gpr(3), (mblk_t *)(size_t)gpr(4));
		break;
	case NATIVE_ETHER_RSRV:
		gpr(3) = ether_rsrv((queue_t *)(size_t)gpr(3));
		break;
	case NATIVE_NQD_SYNC_HOOK:
		gpr(3) = NQD_sync_hook(gpr(3));
		break;
	case NATIVE_NQD_UNKNOWN_HOOK:
		gpr(3) = NQD_unknown_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT_HOOK:
		gpr(3) = NQD_bitblt_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT:
		NQD_bitblt(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT_HOOK:
		gpr(3) = NQD_fillrect_hook(gpr(3));
		break;
	case NATIVE_NQD_INVRECT:
		NQD_invrect(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT:
		NQD_fillrect(gpr(3));
		break;
	case NATIVE_NQD_BLTMASK_HOOK:
		gpr(3) = NQD_bltmask_hook(gpr(3));
		break;
	case NATIVE_NQD_FILLMASK_HOOK:
		gpr(3) = NQD_fillmask_hook(gpr(3));
		break;
	case NATIVE_NQD_BLTMASK:
		NQD_bltmask(gpr(3));
		break;
	case NATIVE_NQD_FILLMASK:
		NQD_fillmask(gpr(3));
		break;
	case NATIVE_SERIAL_NOTHING:
	case NATIVE_SERIAL_OPEN:
	case NATIVE_SERIAL_PRIME_IN:
	case NATIVE_SERIAL_PRIME_OUT:
	case NATIVE_SERIAL_CONTROL:
	case NATIVE_SERIAL_STATUS:
	case NATIVE_SERIAL_CLOSE: {
		typedef int16 (*SerialCallback)(uint32, uint32);
		static const SerialCallback serial_callbacks[] = {
			SerialNothing,
			SerialOpen,
			SerialPrimeIn,
			SerialPrimeOut,
			SerialControl,
			SerialStatus,
			SerialClose
		};
		gpr(3) = serial_callbacks[selector - NATIVE_SERIAL_NOTHING](gpr(3), gpr(4));
		break;
	}
	case NATIVE_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_RESOURCE));
		break;
	case NATIVE_GET_1_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_RESOURCE));
		break;
	case NATIVE_GET_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_IND_RESOURCE));
		break;
	case NATIVE_GET_1_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_IND_RESOURCE));
		break;
	case NATIVE_R_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_R_GET_RESOURCE));
		break;
	case NATIVE_MAKE_EXECUTABLE:
		MakeExecutable(0, gpr(4), gpr(5));
		break;
	case NATIVE_CHECK_LOAD_INVOC:
		check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	case NATIVE_NAMED_CHECK_LOAD_INVOC:
		named_check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	case NATIVE_RAVE_DISPATCH: {
#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
		&& (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
		gpr(3) = (uint32)-1;
		break;
#else
		// Save critical PPC registers that re-entrant PPC code could corrupt.
		// Metal/SDL initialization during DrawContextNew can trigger event
		// processing or CFM callbacks that re-enter the PPC emulator.
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);
		uint32 saved_pc = pc();

		// Read sub-opcode BEFORE dispatch for targeted logging
		uint32 pre_subop = ReadMacInt32(rave_scratch_addr);

		// For hook sub-opcodes (200-207), log full PPC state
		if (pre_subop >= 200 && pre_subop <= 207) {
			D(bug("RAVE NATIVE_OP: subop=%d PC=0x%08x LR=0x%08x CTR=0x%08x SP=0x%08x R2=0x%08x\n",
				   pre_subop, saved_pc, saved_lr, saved_ctr, saved_sp, saved_r2));
			// Dump instructions at LR (return address) to see what caller expects
			D(bug("RAVE NATIVE_OP: instructions at LR 0x%08x:\n", saved_lr));
			for (int di = -2; di < 6; di++) {
				uint32 addr = saved_lr + di * 4;
				uint32 instr = ReadMacInt32(addr);
				D(bug("  [0x%08x] %08x%s\n", addr, instr, di == 0 ? " <-- LR" : ""));
			}
		}

		// PPC calling convention: float arguments go in FPR, not GPR.
		// SetFloat(drawContext, tag, value) has value as a float in fpr(1).
		// We must extract the float bits and place them in r5 so the
		// dispatch handler receives the correct value via gpr(5).
		if (pre_subop == 0) {  // kRaveDrawSetFloat
			float fval = (float)fpr(1);
			uint32 fbits;
			memcpy(&fbits, &fval, sizeof(uint32));
			gpr(5) = fbits;
		}

		uint32 rave_ret = RaveDispatchARC(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7), gpr(8));
		gpr(3) = rave_ret;

		// GetFloat returns float bits in gpr(3). PPC caller also expects
		// the float return value in fpr(1).
		if (pre_subop == 3) {  // kRaveDrawGetFloat
			float fval;
			memcpy(&fval, &rave_ret, sizeof(float));
			fpr(1) = (double)fval;
		}

		// Check all registers for corruption
		if (lr() != saved_lr) {
			printf("RAVE: LR CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_lr, lr());
			lr() = saved_lr;
		}
		if (ctr() != saved_ctr) {
			printf("RAVE: CTR CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_ctr, ctr());
			ctr() = saved_ctr;
		}
		if (gpr(1) != saved_sp) {
			printf("RAVE: SP CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_sp, gpr(1));
			gpr(1) = saved_sp;
		}
		if (gpr(2) != saved_r2) {
			printf("RAVE: R2(TOC) CORRUPTED during native op! was 0x%08x, now 0x%08x - restoring\n",
				   saved_r2, gpr(2));
			gpr(2) = saved_r2;
		}

		// For sub-opcode 204 (DrawContextNew), log the return value
		if (pre_subop == 204) {
			D(bug("RAVE NATIVE_OP: DrawContextNew returning %d, blr will go to LR=0x%08x\n",
				   (int32)rave_ret, lr()));
		}
		break;
#endif
	}
	case NATIVE_OPENGL_DISPATCH: {
#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
		&& (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
		gpr(3) = (uint32)-1;
		break;
#else
		// Save critical PPC registers (same pattern as RAVE -- Metal/SDL init
		// can trigger re-entrant PPC execution that corrupts these).
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);

		// Read sub-opcode from GL scratch word
		uint32 sub_opcode = ReadMacInt32(gl_scratch_addr);

		// Check dispatch-table flag: if set, the game called through the
		// context's internal dispatch table and passed the context index
		// in R3 with real GL args starting at R4.  Shift args left by one.
		extern uint32_t gl_dt_flag_addr;
		uint32 dt_flag = ReadMacInt32(gl_dt_flag_addr);
		WriteMacInt32(gl_dt_flag_addr, 0);  // clear for next call

		uint32 arg_r3, arg_r4, arg_r5, arg_r6, arg_r7, arg_r8, arg_r9, arg_r10;
		if (dt_flag) {
			// Dispatch-table path: skip context index in R3, shift args.
			// r4-r10 become args 0-6. Arg 7 (r10) is set to 0 for now;
			// functions with 9+ args (like glTexImage2D) read additional
			// args from the PPC stack via gl_ppc_stack_arg().
			arg_r3 = gpr(4);  arg_r4 = gpr(5);  arg_r5 = gpr(6);
			arg_r6 = gpr(7);  arg_r7 = gpr(8);  arg_r8 = gpr(9);
			arg_r9 = gpr(10); arg_r10 = 0;
		} else {
			// Stub-patching path: args in normal positions
			arg_r3 = gpr(3);  arg_r4 = gpr(4);  arg_r5 = gpr(5);
			arg_r6 = gpr(6);  arg_r7 = gpr(7);  arg_r8 = gpr(8);
			arg_r9 = gpr(9);  arg_r10 = gpr(10);
		}

		// Generic FPR extraction based on function signature table.
		// PPC ABI passes float/double args in FPR1-FPR13. We extract them
		// into a uint32 array so GLDispatch can reconstruct float values.
		// Dispatch-table calls shift GPR args only; FPR numbering is unchanged
		// because the context index is an integer argument.
		const GLFuncSignature& sig = gl_func_signatures[sub_opcode < GL_MAX_SUBOPCODE ? sub_opcode : 0];
		const int max_ppc_fpr_args = 13;
		const int max_float_mask_bits = (int)(sizeof(sig.float_mask) * 8);
		uint32 float_bits[max_ppc_fpr_args] = {0};  // Max 13 FPR args in PPC ABI
		int fpr_idx = 0;
		for (int i = 0; i < sig.num_args && i < max_float_mask_bits && fpr_idx < max_ppc_fpr_args; i++) {
			if (sig.float_mask & (1u << i)) {
				// This arg position is a float/double -- extract from next FPR.
				// PPC ABI: floats are promoted to double in FPR, cast back to float.
				float fval = (float)fpr(1 + fpr_idx);
				memcpy(&float_bits[fpr_idx], &fval, 4);
				fpr_idx++;
			}
		}

		// Save PPC stack pointer for functions with 9+ args (e.g., glTexImage2D)
		// When dt_flag is set, stack args are shifted by 1 position because
		// the context arg consumed one GPR slot, pushing all subsequent args.
		{
			extern uint32_t gl_ppc_sp;
			extern int gl_ppc_stack_arg_offset;
			gl_ppc_sp = saved_sp;
			gl_ppc_stack_arg_offset = dt_flag ? 1 : 0;
		}

		// Call dispatch with GPR args and extracted float bits
		gpr(3) = GLDispatchARC(arg_r3, arg_r4, arg_r5, arg_r6,
		                    arg_r7, arg_r8, arg_r9, arg_r10,
		                    float_bits, fpr_idx);

		// Restore registers that may have been corrupted
		lr() = saved_lr;
		ctr() = saved_ctr;
		if (gpr(1) != saved_sp) gpr(1) = saved_sp;
		if (gpr(2) != saved_r2) gpr(2) = saved_r2;

		break;
#endif
	}
	case NATIVE_DSP_DISPATCH: {
#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
		&& (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
		gpr(3) = (uint32)-1;
		break;
#else
		// No Metal resources, so no @autoreleasepool wrapper is needed
		// here; the context-lifecycle path wraps in @autoreleasepool once
		// GPU calls (GetBackBuffer vending a MTLTexture) land. Register-
		// preservation pattern mirrors RAVE/GL for re-entrant PPC safety.
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);

		/* Stash caller LR + r11
		 * for the one-shot DSpDispatch diagnostic on unresolved sub-opcodes
		 * 400/401/501/502/600. r11 carries the CFM TVECT address under the
		 * CFM ABI, so it identifies WHICH DSp symbol the caller jumped
		 * through. Globals are defined in dsp_dispatch.cpp; single-thread
		 * (emul) read+write so no synchronisation required. */
		{
			extern uint32_t dsp_caller_lr;
			extern uint32_t dsp_caller_r11;
			dsp_caller_lr = saved_lr;
			dsp_caller_r11 = gpr(11);
		}

		uint32 dsp_ret = DSpDispatch(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7), gpr(8));
		gpr(3) = dsp_ret;

		if (lr() != saved_lr) { lr() = saved_lr; }
		if (ctr() != saved_ctr) { ctr() = saved_ctr; }
		if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
		if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
		break;
#endif
	}
	case NATIVE_GLIDE_DISPATCH: {
#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
		|| (TARGET_OS_IPHONE)
		gpr(3) = (uint32)-1;
		break;
#else
		/* Pass r3-r10: grTexDownloadMipMapLevel needs evenOdd/data in r9/r10.
		 * r1=SP for 9th+ stack args (e.g. grLfbWriteRegion src_data).
		 * Glide 2 utilities and state setters also use the normal PPC f1-f4
		 * argument registers for float parameters. */
		uint32 saved_lr = lr();
		uint32 saved_ctr = ctr();
		uint32 saved_sp = gpr(1);
		uint32 saved_r2 = gpr(2);
		gpr(3) = GlideDispatch(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7), gpr(8),
		                       gpr(9), gpr(10), gpr(1),
		                       fpr(1), fpr(2), fpr(3), fpr(4));
		/* Float-returning Glide entry points (guFogTableIndexToW) hand their
		 * result back out-of-band; PPC returns floats in FPR1, not r3. */
		{
			float fret;
			if (GlideDispatchTakeFloatResult(&fret))
				fpr(1) = (double)fret;
		}
		if (lr() != saved_lr) lr() = saved_lr;
		if (ctr() != saved_ctr) ctr() = saved_ctr;
		if (gpr(1) != saved_sp) gpr(1) = saved_sp;
		if (gpr(2) != saved_r2) gpr(2) = saved_r2;
		break;
#endif
	}
  #if defined(ENABLE_NATIVE_CINEPAK_PATCH) \
			&& ENABLE_NATIVE_CINEPAK_PATCH
	case NATIVE_OPENDEFAULTCOMPONENT_CINEPAK_HOOK:
	case NATIVE_FINDNEXTCOMPONENT_CINEPAK_HOOK: {
	#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
			&& (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
			gpr(3) = 0;
			break;
	#else
			/* FN=1 first-instruction hooks: execute_sheep does pc() = lr() right
			 * after this returns, and the handlers run call_macos (nested guest
			 * execution) which clobbers LR/CTR - save/restore is mandatory or the
			 * return lands in the nested callee instead of the real caller. */
			uint32 saved_lr = lr();
			uint32 saved_ctr = ctr();
			uint32 saved_sp = gpr(1);
			uint32 saved_r2 = gpr(2);

			if (selector == NATIVE_OPENDEFAULTCOMPONENT_CINEPAK_HOOK)
				gpr(3) = CinepakOpenDefaultComponentHook(gpr(3), gpr(4));
			else
				gpr(3) = CinepakFindNextComponentHook(gpr(3), gpr(4));

			if (lr() != saved_lr) { lr() = saved_lr; }
			if (ctr() != saved_ctr) { ctr() = saved_ctr; }
			if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
			if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
			break;
	#endif
	}
	case NATIVE_CINEPAK_DISPATCH: {
	#if !(defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)) \
			&& (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
			gpr(3) = (uint32)-50; /* paramErr */
			break;
	#else
			/* Component entry (via routine descriptor): pure host work, but keep
			 * the same register-preservation pattern for safety. */
			uint32 saved_lr = lr();
			uint32 saved_ctr = ctr();
			uint32 saved_sp = gpr(1);
			uint32 saved_r2 = gpr(2);

			gpr(3) = CinepakDispatch(gpr(3));

			if (lr() != saved_lr) { lr() = saved_lr; }
			if (ctr() != saved_ctr) { ctr() = saved_ctr; }
			if (gpr(1) != saved_sp) { gpr(1) = saved_sp; }
			if (gpr(2) != saved_r2) { gpr(2) = saved_r2; }
			break;
	#endif
	}
#endif /* ENABLE_NATIVE_CINEPAK_PATCH */
	default:
		printf("FATAL: NATIVE_OP called with bogus selector %d\n", selector);
		QuitEmulator();
		break;
	}
#if EMUL_TIME_STATS
	native_exec_time += (clock() - native_exec_start);
#endif
}

/*
 *  Offer a taken trap to the MacOS exception handler chain (see
 *  sheepshaver_cpu::deliver_trap_exception).  Called from the PowerPC core.
 */

const char *DeliverTrapException(uint32 opcode)
{
	if (ppc_cpu == NULL)
		return "no CPU";
	return ppc_cpu->deliver_trap_exception(opcode);
}


/*
 *  Execute 68k subroutine (must be ended with EXEC_RETURN)
 *  This must only be called by the emul_thread when in EMUL_OP mode
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68k(uint32 pc, M68kRegisters *r)
{
	ppc_cpu->execute_68k(pc, r);
}

/*
 *  Execute 68k A-Trap from EMUL_OP routine
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	SheepVar proc_var(4);
	uint32 proc = proc_var.addr();
	WriteMacInt16(proc, trap);
	WriteMacInt16(proc + 2, M68K_RTS);
	Execute68k(proc, r);
}

/*
 *  Call MacOS PPC code
 */

uint32 call_macos(uint32 tvect)
{
	return ppc_cpu->execute_macos_code(tvect, 0, NULL);
}

uint32 call_macos1(uint32 tvect, uint32 arg1)
{
	const uint32 args[] = { arg1 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2)
{
	const uint32 args[] = { arg1, arg2 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3)
{
	const uint32 args[] = { arg1, arg2, arg3 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos8(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7, uint32 arg8)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}
