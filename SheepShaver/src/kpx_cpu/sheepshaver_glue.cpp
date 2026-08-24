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
#include "ppc-report.h"

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
#include "usbhid.h"
#include "usbuim.h"
#include "mmio.h"
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

#ifndef PPC_DEBUG_TRACE
#define PPC_DEBUG_TRACE 0
#endif
#ifndef GUEST_STALL_TRACE
#define GUEST_STALL_TRACE 0
#endif
#if GUEST_STALL_TRACE && !PPC_DEBUG_TRACE
#error "GUEST_STALL_TRACE needs PPC_DEBUG_TRACE"
#endif

static void ppc_trace(const char *prefix, const char *format, ...)
{
#if PPC_DEBUG_TRACE
	va_list args;
	va_start(args, format);
	gfx_log_emitv(prefix, format, args);
	va_end(args);
#else
	(void)prefix;
	(void)format;
#endif
}
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
	void execute_kernel_entry(uint32 lr_slot);
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
	void acquire_fp_registers();
	bool external_interrupt();
#if PPC_DEBUG_TRACE
	void watch_event_queue(void);
#endif
	bool exception_step_trampoline_ready(void);
#if PPC_DEBUG_TRACE
	void exception_diagnostic_state(const char *reason, uint64 now);
	void exception_idle_diagnostic(void);
	void exception_stall_sample(void);
#if GUEST_STALL_TRACE
	void exec_frame_push(uint8 kind, uint32 entry, uint32 arg);
	void exec_frame_pop(void);
	void exec_frame_dump(const char *why);
	void guest_stall_watch(void);
#endif
	void record_rfi_site(uint32 return_pc, uint32 run_mode);
	void report_and_reset_rfi_sites(void);
#endif
	void preserve_system_ticks(const char *site);
#if PPC_DEBUG_TRACE
	void lowmem_watch(void);
	void lowmem_dump(const char *why);
#endif

	bool exception_step_active;
	bool exception_step_trap;
	uint32 exception_step_opcode;
	uint32 exception_step_trampoline;
	uint32 exception_step_pc;
	bool exception_step_pending;
	// Hardware-style entries which have not yet crossed an rfi boundary.
	// This is architectural nesting, not debugger-handler lifetime.
	uint32 exception_entry_depth;
#if PPC_DEBUG_TRACE
	uint32 exception_stall_pc;
	uint32 exception_stall_last_reported_pc;
	uint32 exception_stall_tick;
	enum { EXC_TRACK = 64 };
	uint32 exception_track_ppc_pc[EXC_TRACK];
	uint32 exception_track_68k_pc[EXC_TRACK];
	uint32 exception_track_level[EXC_TRACK];
	uint32 exception_track_mode[EXC_TRACK];
	uint64 exception_track_usec[EXC_TRACK];
	unsigned exception_track_next;
	unsigned exception_track_count;
	uint32 exception_last_seen_level;
	uint32 exception_last_seen_mode;
	bool exception_level_seen;
	bool exception_queue_reported;
	enum { EVQ_TRACK = 24 };
	uint32 evq_head[EVQ_TRACK];
	uint32 evq_tail[EVQ_TRACK];
	uint32 evq_link[EVQ_TRACK];
	uint32 evq_what[EVQ_TRACK];
	uint32 evq_pc[EVQ_TRACK];
	uint32 evq_68k_pc[EVQ_TRACK];
	uint32 evq_guard[EVQ_TRACK];
	uint32 evq_free[EVQ_TRACK];
	uint32 evq_msg[EVQ_TRACK];
	uint32 evq_when[EVQ_TRACK];
	uint32 evq_level[EVQ_TRACK];
	uint32 evq_mode[EVQ_TRACK];
	unsigned evq_next;
	unsigned evq_count;
	uint32 evq_last_head;
	uint32 evq_last_tail;
	uint32 evq_last_link;
	bool evq_reported;
	uint64 exception_stall_tick_moved_usec;
	uint64 exception_stall_via_services;
	uint64 exception_stall_moved_usec;
	uint64 exception_stall_sample_usec;
	uint64 exception_stall_samples;
#if GUEST_STALL_TRACE
	// Nested host-driven guest execution. execute_68k()/execute_macos_code()
	// run the guest one level deeper than the emulation loop, so a call that
	// never comes back pins the interrupt level of whatever invoked it. This
	// records what opened each level.
	enum { EXEC_FRAMES = 16 };
	enum { EXEC_FRAME_EMUL_OP = 0, EXEC_FRAME_68K = 1, EXEC_FRAME_MACOS = 2 };
	uint8  exec_frame_kind[EXEC_FRAMES];
	uint32 exec_frame_entry[EXEC_FRAMES];
	uint32 exec_frame_arg[EXEC_FRAMES];
	uint32 exec_frame_68k_pc[EXEC_FRAMES];
	uint32 exec_frame_r25[EXEC_FRAMES];
	uint32 exec_frame_sp[EXEC_FRAMES];
	uint64 exec_frame_usec[EXEC_FRAMES];
	unsigned exec_frame_depth;
	unsigned exec_frame_overflow;
	// Emul-thread stall watch: the register file is only coherent here.
	uint32 guest_stall_tick;
	uint64 guest_stall_tick_usec;
	uint64 guest_stall_report_usec;
	unsigned guest_stall_reports;
#endif
	uint64 exception_steps_taken;
	uint64 exception_vector_entries;
	enum { LOWMEM_BASE = 0x100, LOWMEM_WORDS = 192 };	// $100..$400
	enum { LOWMEM_TRACK = 128 };
	uint32 lowmem_snap[LOWMEM_WORDS];
	bool lowmem_snap_valid;
	uint32 lowmem_track_addr[LOWMEM_TRACK];
	uint32 lowmem_track_old[LOWMEM_TRACK];
	uint32 lowmem_track_new[LOWMEM_TRACK];
	uint32 lowmem_track_ppc[LOWMEM_TRACK];
	uint32 lowmem_track_lr[LOWMEM_TRACK];
	uint32 lowmem_track_68k[LOWMEM_TRACK];
	uint32 lowmem_track_level[LOWMEM_TRACK];
	uint32 lowmem_track_mode[LOWMEM_TRACK];
	uint32 lowmem_track_nest[LOWMEM_TRACK];
	uint32 lowmem_track_depth[LOWMEM_TRACK];
	uint32 lowmem_track_ticks[LOWMEM_TRACK];
	uint64 lowmem_track_usec[LOWMEM_TRACK];
	unsigned lowmem_track_next;
	unsigned lowmem_track_count;
	uint64 lowmem_changes;
	bool lowmem_armed;
	uint32 lowmem_prev_ticks;
	unsigned lowmem_reports;
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
	uint64 system_tick_correction_count;
	uint64 system_tick_recovered_total;
	uint32 system_tick_max_rollback;
	uint64 system_tick_correction_snapshot;
	uint64 system_tick_recovered_snapshot;
#endif /* PPC_DEBUG_TRACE */
	bool system_ticks_valid;
	uint32 system_ticks_high_water;
	uint64 system_queue_repairs;

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
	system_ticks_valid = false;
	system_ticks_high_water = 0;
	system_queue_repairs = 0;
#if PPC_DEBUG_TRACE
	exception_stall_pc = 0;
#if GUEST_STALL_TRACE
	memset(exec_frame_kind, 0, sizeof(exec_frame_kind));
	memset(exec_frame_entry, 0, sizeof(exec_frame_entry));
	memset(exec_frame_arg, 0, sizeof(exec_frame_arg));
	memset(exec_frame_68k_pc, 0, sizeof(exec_frame_68k_pc));
	memset(exec_frame_r25, 0, sizeof(exec_frame_r25));
	memset(exec_frame_sp, 0, sizeof(exec_frame_sp));
	memset(exec_frame_usec, 0, sizeof(exec_frame_usec));
	exec_frame_depth = 0;
	exec_frame_overflow = 0;
	guest_stall_tick = 0;
	guest_stall_tick_usec = 0;
	guest_stall_report_usec = 0;
	guest_stall_reports = 0;
#endif
	exception_stall_last_reported_pc = 0;
	exception_stall_tick = 0;
	memset(exception_track_ppc_pc, 0, sizeof(exception_track_ppc_pc));
	memset(exception_track_68k_pc, 0, sizeof(exception_track_68k_pc));
	memset(exception_track_level, 0, sizeof(exception_track_level));
	memset(exception_track_mode, 0, sizeof(exception_track_mode));
	memset(exception_track_usec, 0, sizeof(exception_track_usec));
	exception_track_next = 0;
	exception_track_count = 0;
	exception_last_seen_level = 0xffffffffu;
	exception_last_seen_mode = 0xffffffffu;
	exception_level_seen = false;
	exception_queue_reported = false;
	memset(evq_head, 0, sizeof(evq_head));
	memset(evq_tail, 0, sizeof(evq_tail));
	memset(evq_link, 0, sizeof(evq_link));
	memset(evq_what, 0, sizeof(evq_what));
	memset(evq_pc, 0, sizeof(evq_pc));
	memset(evq_68k_pc, 0, sizeof(evq_68k_pc));
	memset(evq_guard, 0, sizeof(evq_guard));
	memset(evq_free, 0, sizeof(evq_free));
	memset(evq_msg, 0, sizeof(evq_msg));
	memset(evq_when, 0, sizeof(evq_when));
	memset(evq_level, 0, sizeof(evq_level));
	memset(evq_mode, 0, sizeof(evq_mode));
	evq_next = 0;
	evq_count = 0;
	evq_last_head = 0xffffffffu;
	evq_last_tail = 0xffffffffu;
	evq_last_link = 0xffffffffu;
	evq_reported = false;
	exception_stall_tick_moved_usec = 0;
	exception_stall_via_services = 0;
	exception_stall_moved_usec = 0;
	exception_stall_sample_usec = 0;
	exception_stall_samples = 0;
	exception_steps_taken = 0;
	exception_vector_entries = 0;
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
	system_tick_correction_count = 0;
	system_tick_recovered_total = 0;
	memset(lowmem_snap, 0, sizeof(lowmem_snap));
	lowmem_snap_valid = false;
	memset(lowmem_track_addr, 0, sizeof(lowmem_track_addr));
	lowmem_track_next = 0;
	lowmem_track_count = 0;
	lowmem_changes = 0;
	lowmem_armed = false;
	lowmem_prev_ticks = 0;
	lowmem_reports = 0;
	system_tick_max_rollback = 0;
	system_tick_correction_snapshot = 0;
	system_tick_recovered_snapshot = 0;
#endif
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
	preserve_system_ticks("emul-op entry");
	WriteMacInt32(XLM_68K_R25, gpr(25));
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);
	for (int i = 0; i < 8; i++)
		r68.d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)
		r68.a[i] = gpr(16 + i);
	r68.a[7] = gpr(1);
	uint32 saved_cr = get_cr() & 0xff9fffff; // mask_operand::compute(11, 8)
	uint32 saved_xer = get_xer();
#if GUEST_STALL_TRACE
	exec_frame_push(EXEC_FRAME_EMUL_OP, emul_op, gpr(24));
#endif
	EmulOp(&r68, gpr(24), emul_op);
#if GUEST_STALL_TRACE
	exec_frame_pop();
#endif
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

#if GUEST_STALL_TRACE
	exec_frame_push(EXEC_FRAME_68K, entry, r->a[1]);
#endif
	// Execute 68k opcode
	uint32 opcode = ReadMacInt16(gpr(24));
	gpr(27) = (int32)(int16)ReadMacInt16(gpr(24) += 2);
	gpr(29) += opcode * 8;
	execute(gpr(29));

#if GUEST_STALL_TRACE
	exec_frame_pop();
#endif
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
#if GUEST_STALL_TRACE
	exec_frame_push(EXEC_FRAME_MACOS, tvect, proc);
#endif
	execute(proc);
#if GUEST_STALL_TRACE
	exec_frame_pop();
#endif
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

static inline bool ppc_68k_sp_parked(uint32 a7)
{
	return a7 - (uint32)KernelDataAddr < 0x8000u;
}

static bool ppc_pc_in_68k_emulator(uint32 pc);
uint32 ppc_recover_68k_sp(uint32 a7);
extern uint64 IdleWaitUsec;
extern unsigned long IdleWaitCount;

#if PPC_DEBUG_TRACE
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
#endif

#if PPC_DEBUG_TRACE
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
		ppc_trace("[exception-rfi] ",
			"rank %u: return %08x, mode %u, %llu occurrence(s)\n",
			rank + 1, exception_rfi_sites[best].pc,
			exception_rfi_sites[best].mode,
			(unsigned long long)exception_rfi_sites[best].count);
	}
	if (exception_rfi_site_overflow != 0)
		ppc_trace("[exception-rfi] ",
			"%llu return(s) used sites beyond the %u-entry exact table\n",
			(unsigned long long)exception_rfi_site_overflow,
			(unsigned)EXCEPTION_RFI_SITE_COUNT);
	memset(exception_rfi_sites, 0, sizeof(exception_rfi_sites));
	exception_rfi_site_overflow = 0;
}
#endif

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
void sheepshaver_cpu::preserve_system_ticks(const char *site)
{
	if (!HasMacStarted()) {
		system_ticks_valid = false;
		return;
	}

	// The stale low-memory restore that moves Ticks backwards carries $14c and
	// $150 with it, and a stale qHead is what freezes the machine: it points
	// at an element the dequeue already released, so PostEvent's free scan
	// legitimately hands that element out again and Enqueue takes its
	// non-empty branch with qTail == elem, self-linking it. The Event Manager
	// then walks that link for ever at interrupt level 7 (ROM 0x10dc6) with
	// every interrupt masked.
	//
	// A queued element always carries a real event code, so a qHead pointing
	// at one marked free - what == 0xffff, the marker PostEvent's own scan
	// looks for at ROM 0x10d3e - is proof the header is the stale copy and
	// not a queue. Empty it: self-consistent, cannot self-link, and the
	// events it named were about to be mangled anyway.
	//
	// A repair rather than prevention, because preventing it means catching
	// the guest _BlockMoveData that does it, and that costs the store path on
	// every instruction the emulator runs. Here it is two guest reads at the
	// three points this function already runs at.
	{
		const uint32 qhead = ReadMacInt32(0x14c);
		if (qhead != 0 && guest_addr_ok(qhead, 8) &&
			ReadMacInt16(qhead + 6) == 0xffff) {
			const uint32 qtail = ReadMacInt32(0x150);
			WriteMacInt32(0x14c, 0);
			WriteMacInt32(0x150, 0);
			system_queue_repairs++;
			ppc_trace("[ticks] ",
				"EvtQHead %08x was a released element at %s; emptied the "
				"queue (repair %llu, qTail was %08x, Ticks %08x)\n",
				qhead, site, (unsigned long long)system_queue_repairs,
				qtail, ReadMacInt32(0x16a));
		}
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
#if PPC_DEBUG_TRACE
	system_tick_correction_count++;
	system_tick_recovered_total += rollback;
	if (rollback > system_tick_max_rollback)
		system_tick_max_rollback = rollback;
#endif

	WriteMacInt32(0x16a, system_ticks_high_water);
#if PPC_DEBUG_TRACE
	{
		const uint32 guard = ReadMacInt8(0x160);
		const uint32 qhead = ReadMacInt32(0x14c);
		const uint32 qtail = ReadMacInt32(0x150);
		ppc_trace("[ticks] ",
			"Ticks rolled back %u at %s (%08x -> %08x, high water %08x); "
			"guard $160=%02x bit6=%u; EvtQ head=%08x tail=%08x qLink=%08x "
			"what=%04x\n",
			rollback, site, system_ticks_high_water, current,
			system_ticks_high_water, guard, (guard >> 6) & 1,
			qhead, qtail,
			(qhead != 0 && guest_addr_ok(qhead, 8)) ? ReadMacInt32(qhead) : 0,
			(qhead != 0 && guest_addr_ok(qhead, 8))
				? ReadMacInt16(qhead + 6) : 0xffffu);
		ppc_trace("[ticks] ",
			"  ppc pc=%08x lr=%08x sp=%08x msr=%08x srr0=%08x srr1=%08x; "
			"68k pc=%08x level=%08x; mode=%u nest=%d depth=%u exec-depth=%d "
			"step %s\n",
			pc(), lr(), gpr(1), msr(), srr0(), srr1(), gpr(24), gpr(25),
			ReadMacInt32(XLM_RUN_MODE), (int32)ReadMacInt32(XLM_IRQ_NEST),
			exception_entry_depth, current_execute_depth(),
			exception_step_pending ? "armed" : "idle");
		ppc_trace("[ticks] ",
			"  corrections=%llu recovered=%llu max=%u; steps taken=%llu; "
			"vector entries=%llu; last vector %s\n",
			(unsigned long long)system_tick_correction_count,
			(unsigned long long)system_tick_recovered_total,
			system_tick_max_rollback,
			(unsigned long long)exception_steps_taken,
			(unsigned long long)exception_vector_entries,
			ppc_exception_vector_name(exception_last_vector));
		{
			const uint32 kd = KernelDataAddr;
			const uint32 ctx68 = ReadMacInt32(kd + 0x658);
			const uint32 cur = ReadMacInt32(kd + 0x65c);
			ppc_trace("[ticks] ",
				"  KD status=%08x savedDEC=%08x mask=%08x level@%08x=%04x; "
				"68kctx=%08x pc=%08x sp=%08x; current=%08x pc=%08x sp=%08x\n",
				ReadMacInt32(kd + 0x660), ReadMacInt32(kd + 0x668),
				ReadMacInt32(kd + 0x674), ReadMacInt32(kd + 0x67c),
				guest_addr_ok(ReadMacInt32(kd + 0x67c), 2)
					? ReadMacInt16(ReadMacInt32(kd + 0x67c)) : 0xffffu,
				ctx68,
				guest_addr_ok(ctx68, 0x118) ? ReadMacInt32(ctx68 + 0xfc) : 0,
				guest_addr_ok(ctx68, 0x118) ? ReadMacInt32(ctx68 + 0x10c) : 0,
				cur,
				guest_addr_ok(cur, 0x118) ? ReadMacInt32(cur + 0xfc) : 0,
				guest_addr_ok(cur, 0x118) ? ReadMacInt32(cur + 0x10c) : 0);
		}
	}
#endif
	// When the correction was applied the guest is back at the high water
	// mark; when it was skipped, follow the guest so the next genuine
	// advance is not reported as another rollback.
	if ((ReadMacInt8(0x160) & 0x40) != 0)
		system_ticks_high_water = current;
}

#if PPC_DEBUG_TRACE
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
	ppc_trace("[exception-state] ",
		"%s +%lu ms: pc=%08x lr=%08x sp=%08x msr=%08x "
		"srr0=%08x srr1=%08x sprg3=%08x\n",
		reason,
		exception_last_program_usec != 0
			? (unsigned long)((now - exception_last_program_usec) / 1000) : 0UL,
		pc(), lr(), gpr(1), msr(), srr0(), srr1(), sprg(3));
	ppc_trace("[exception-state] ",
		"mode=%u exec-depth=%d nest=%d r25=%08x flags=%08x spc=%08x "
		"tick=%08x canonical=%08x qhead=%08x; "
		"KD status=%08x savedDEC=%08x timebaseHz=%08x mask=%08x "
		"level@%08x=%08x\n",
		run_mode, current_execute_depth(), irq_nest, r25, (uint32)InterruptFlags,
		spcflags().get(),
		ReadMacInt32(0x16a), system_ticks_high_water, ReadMacInt32(0x14c), kernel_status,
		kernel_dec, timebase_frequency, irq_mask, level_address, level);
	ppc_trace("[exception-state] ",
		"68kctx=%08x valid=%u pc=%08x sp=%08x cr=%08x; "
		"current=%08x valid=%u pc=%08x sp=%08x cr=%08x\n",
		context_68k, context_68k_valid ? 1u : 0u, context_68k_pc,
		context_68k_sp, context_68k_cr, current_context,
		current_context_valid ? 1u : 0u, current_pc, current_sp, current_cr);
	ppc_trace("[exception-state] ",
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
#endif

#if PPC_DEBUG_TRACE
void sheepshaver_cpu::watch_event_queue(void)
{
#if GUEST_STALL_TRACE
	guest_stall_watch();
#endif
	lowmem_watch();
	const uint32 head = ReadMacInt32(0x14c);
	const uint32 link = (head != 0 && guest_addr_ok(head, 8))
		? ReadMacInt32(head) : 0;
	if (head == evq_last_head && link == evq_last_link)
		return;
	const uint32 tail = ReadMacInt32(0x150);
	const uint32 what = (head != 0 && guest_addr_ok(head, 8))
		? ReadMacInt16(head + 6) : 0xffffu;
	evq_head[evq_next] = head;
	evq_tail[evq_next] = tail;
	evq_link[evq_next] = link;
	evq_what[evq_next] = what;
	evq_pc[evq_next] = pc();
	evq_68k_pc[evq_next] = gpr(24);
	evq_guard[evq_next] = ReadMacInt8(0x160);
	evq_msg[evq_next] = (head != 0 && guest_addr_ok(head, 16))
		? ReadMacInt32(head + 8) : 0;
	evq_when[evq_next] = (head != 0 && guest_addr_ok(head, 16))
		? ReadMacInt32(head + 12) : 0;
	{
		const uint32 base = ReadMacInt32(0x146);
		const int count = (int)(int16)ReadMacInt16(0x154);
		int freeslots = 0;
		int q;
		for (q = 0; q <= count && q < 64; q++) {
			const uint32 el = base + (uint32)q * 0x16;
			if (guest_addr_ok(el, 16) && ReadMacInt16(el + 6) == 0xffff)
				freeslots++;
		}
		evq_free[evq_next] = (uint32)freeslots;
	}
	evq_level[evq_next] = gpr(25);
	evq_mode[evq_next] = ReadMacInt32(XLM_RUN_MODE);
	evq_next = (evq_next + 1) % EVQ_TRACK;
	if (evq_count < EVQ_TRACK)
		evq_count++;
	evq_last_head = head;
	evq_last_tail = tail;
	evq_last_link = link;

	if (head != 0 && link == head && !evq_reported) {
		unsigned n = evq_count;
		unsigned i = (evq_next + EVQ_TRACK - n) % EVQ_TRACK;
		unsigned k;
		evq_reported = true;
		ppc_trace("[evq] ",
			"EvtQHead %08x is self-linked (qTail=%08x what=%04x) at 68k pc "
			"%08x level %08x (ppc %08x), mode %u, nest %d, depth %u, step %s; history oldest first:\n",
			head, tail, what, gpr(24), gpr(25), pc(),
			ReadMacInt32(XLM_RUN_MODE),
			(int32)ReadMacInt32(XLM_IRQ_NEST), exception_entry_depth,
			exception_step_pending ? "armed" : "idle");
		for (k = 0; k < n; k++) {
			ppc_trace("[evq] ",
				"  head=%08x tail=%08x qLink=%08x what=%04x by 68k pc %08x "
				"level %08x (ppc %08x) mode %u guard=%02x free=%u msg=%08x when=%08x\n",
				evq_head[i], evq_tail[i], evq_link[i], evq_what[i],
				evq_68k_pc[i], evq_level[i], evq_pc[i], evq_mode[i],
				evq_guard[i], evq_free[i], evq_msg[i], evq_when[i]);
			i = (i + 1) % EVQ_TRACK;
		}
	}
}
#endif

#if PPC_DEBUG_TRACE
void sheepshaver_cpu::lowmem_watch(void)
{
	unsigned i;
	bool head_free = false;
	bool ticks_back = false;
	uint32 ticks;

	{
		const uint32 head = ReadMacInt32(0x14c);
		if (head != 0 && guest_addr_ok(head, 0x16) &&
			ReadMacInt16(head + 6) == 0xffff)
			head_free = true;
	}
	if (!lowmem_armed) {
		if (head_free && lowmem_reports < 4)
			lowmem_dump("EvtQHead points at an element already marked free");
		return;
	}

	ticks = ReadMacInt32(0x16a);
	for (i = 0; i < LOWMEM_WORDS; i++) {
		const uint32 addr = LOWMEM_BASE + i * 4;
		const uint32 now = ReadMacInt32(addr);
		if (lowmem_snap_valid && now == lowmem_snap[i])
			continue;
		if (lowmem_snap_valid) {
			const unsigned s = lowmem_track_next;
			lowmem_track_addr[s] = addr;
			lowmem_track_old[s] = lowmem_snap[i];
			lowmem_track_new[s] = now;
			lowmem_track_ppc[s] = pc();
			lowmem_track_lr[s] = lr();
			lowmem_track_68k[s] = gpr(24);
			lowmem_track_level[s] = gpr(25);
			lowmem_track_mode[s] = ReadMacInt32(XLM_RUN_MODE);
			lowmem_track_nest[s] = ReadMacInt32(XLM_IRQ_NEST);
			lowmem_track_depth[s] = exception_entry_depth;
			lowmem_track_ticks[s] = ticks;
			lowmem_track_usec[s] = GetTicks_usec();
			lowmem_track_next = (s + 1) % LOWMEM_TRACK;
			if (lowmem_track_count < LOWMEM_TRACK)
				lowmem_track_count++;
			lowmem_changes++;
		}
		lowmem_snap[i] = now;
	}
	if (lowmem_snap_valid && ticks < lowmem_prev_ticks)
		ticks_back = true;
	lowmem_prev_ticks = ticks;
	lowmem_snap_valid = true;

	if (lowmem_reports < 4 && (head_free || ticks_back))
		lowmem_dump(head_free
			? "EvtQHead points at an element already marked free"
			: "Ticks moved backwards");
}
#endif

#if PPC_DEBUG_TRACE
void sheepshaver_cpu::lowmem_dump(const char *why)
{
	unsigned i, k;

	lowmem_reports++;
	ppc_trace("[lowmem] ", "ROLLBACK CAUGHT: %s\n", why);
	ppc_trace("[lowmem] ",
		"pc=%08x lr=%08x ctr=%08x sp=%08x msr=%08x srr0=%08x srr1=%08x "
		"sprg0=%08x sprg1=%08x sprg2=%08x sprg3=%08x\n",
		pc(), lr(), ctr(), gpr(1), msr(), srr0(), srr1(),
		sprg(0), sprg(1), sprg(2), sprg(3));
	ppc_trace("[lowmem] ",
		"mode=%u nest=%d architectural depth=%u exec-depth=%d step %s; "
		"last vector %s %lu us ago; vector entries=%llu steps taken=%llu "
		"low-memory changes seen=%llu\n",
		ReadMacInt32(XLM_RUN_MODE), (int32)ReadMacInt32(XLM_IRQ_NEST),
		exception_entry_depth, current_execute_depth(),
		exception_step_pending ? "armed" : "idle",
		ppc_exception_vector_name(exception_last_vector),
		(unsigned long)(GetTicks_usec() - exception_last_vector_usec),
		(unsigned long long)exception_vector_entries,
		(unsigned long long)exception_steps_taken,
		(unsigned long long)lowmem_changes);
	for (i = 0; i < 32; i += 8)
		ppc_trace("[lowmem] ",
			"r%-2u %08x %08x %08x %08x %08x %08x %08x %08x\n", i,
			gpr(i), gpr(i + 1), gpr(i + 2), gpr(i + 3),
			gpr(i + 4), gpr(i + 5), gpr(i + 6), gpr(i + 7));
	ppc_trace("[lowmem] ",
		"deferrals: ext accepted=%llu nest=%llu mode=%llu; tick "
		"corrections=%llu recovered=%llu max=%u\n",
		(unsigned long long)external_interrupt_accepted_count,
		(unsigned long long)external_interrupt_nest_deferred_count,
		(unsigned long long)external_interrupt_mode_deferred_count,
		(unsigned long long)system_tick_correction_count,
		(unsigned long long)system_tick_recovered_total,
		system_tick_max_rollback);
	{
		const uint32 ctx68k = ReadMacInt32(KERNEL_DATA_BASE + 0x65c);
		const uint32 cur = ReadMacInt32(KERNEL_DATA_BASE + 0x658);
		ppc_trace("[lowmem] ",
			"KernelData: status=%08x savedDEC=%08x mask=%08x level=%08x; "
			"68kctx=%08x current=%08x\n",
			ReadMacInt32(KERNEL_DATA_BASE + 0x18),
			ReadMacInt32(KERNEL_DATA_BASE + 0x648),
			ReadMacInt32(KERNEL_DATA_BASE + 0x674),
			ReadMacInt32(KERNEL_DATA_BASE + 0x67c), ctx68k, cur);
	}
	ppc_trace("[lowmem] ",
		"EventQueue: qFlags=%04x qHead=%08x qTail=%08x buf=%08x count=%d "
		"Ticks=%08x guard $160=%02x mask $144=%04x\n",
		ReadMacInt16(0x14a), ReadMacInt32(0x14c), ReadMacInt32(0x150),
		ReadMacInt32(0x146), (int)(int16)ReadMacInt16(0x154),
		ReadMacInt32(0x16a), ReadMacInt8(0x160), ReadMacInt16(0x144));
	{
		const uint32 base = ReadMacInt32(0x146);
		const int count = (int)(int16)ReadMacInt16(0x154);
		int q;
		for (q = 0; q <= count && q < 64; q++) {
			const uint32 el = base + (uint32)q * 0x16;
			if (!guest_addr_ok(el, 0x16))
				continue;
			ppc_trace("[lowmem] ",
				"  elem[%2d] %08x qLink=%08x qType=%04x what=%04x msg=%08x "
				"when=%08x where=%08x mods=%04x%s%s\n",
				q, el, ReadMacInt32(el), ReadMacInt16(el + 4),
				ReadMacInt16(el + 6), ReadMacInt32(el + 8),
				ReadMacInt32(el + 12), ReadMacInt32(el + 16),
				ReadMacInt16(el + 20),
				el == ReadMacInt32(0x14c) ? " <-qHead" : "",
				el == ReadMacInt32(0x150) ? " <-qTail" : "");
		}
	}
	ppc_trace("[lowmem] ", "low-memory changes, oldest first:\n");
	k = lowmem_track_count;
	i = (lowmem_track_next + LOWMEM_TRACK - k) % LOWMEM_TRACK;
	while (k-- > 0) {
		ppc_trace("[lowmem] ",
			"  $%03x %08x -> %08x  ppc %08x lr %08x 68k pc %08x level %08x "
			"mode %u nest %d depth %u ticks %08x at %llu us\n",
			lowmem_track_addr[i], lowmem_track_old[i], lowmem_track_new[i],
			lowmem_track_ppc[i], lowmem_track_lr[i], lowmem_track_68k[i],
			lowmem_track_level[i], lowmem_track_mode[i],
			(int32)lowmem_track_nest[i], lowmem_track_depth[i],
			lowmem_track_ticks[i],
			(unsigned long long)lowmem_track_usec[i]);
		i = (i + 1) % LOWMEM_TRACK;
	}
	{
		// The code that did it, straight out of guest memory: the relocated
		// 68k emulator lives above the 4 MB ROM file, so it is not in the
		// image tools/rom_decode.py produces and has to be dumped to be
		// disassembled at all.
		static const char *const w[4] = {
			"ppc pc", "ppc lr", "68k pc", "ppc pc" };
		uint32 at[4];
		int b;
		at[0] = pc(); at[1] = lr(); at[2] = gpr(24);
		at[3] = pc();
		for (b = 0; b < 4; b++) {
			uint32 a;
			for (a = at[b] - 0x100; a < at[b] + 0x40; a += 16) {
				if (!guest_addr_ok(a, 16))
					continue;
				ppc_trace("[lowmem] ",
					"  code %s %08x: %08x %08x %08x %08x%s\n",
					w[b], a, ReadMacInt32(a), ReadMacInt32(a + 4),
					ReadMacInt32(a + 8), ReadMacInt32(a + 12),
					(at[b] >= a && at[b] < a + 16) ? "   <-- here" : "");
			}
		}
	}
	{
		// The whole watched region, so the full extent of a revert is visible
		// rather than only the words that happened to survive in a ring.
		uint32 a;
		for (a = LOWMEM_BASE; a < LOWMEM_BASE + LOWMEM_WORDS * 4; a += 16)
			ppc_trace("[lowmem] ",
				"  $%03x: %08x %08x %08x %08x\n", a, ReadMacInt32(a),
				ReadMacInt32(a + 4), ReadMacInt32(a + 8),
				ReadMacInt32(a + 12));
	}
	{
		// Both nanokernel context blocks: if the stale values are restored
		// from a saved context, they are in here.
		const uint32 c[2] = { ReadMacInt32(KERNEL_DATA_BASE + 0x65c),
			ReadMacInt32(KERNEL_DATA_BASE + 0x658) };
		int b;
		uint32 a;
		for (b = 0; b < 2; b++)
			for (a = c[b]; a < c[b] + 0x200; a += 16) {
				if (!guest_addr_ok(a, 16))
					continue;
				ppc_trace("[lowmem] ",
					"  ctx%d %08x: %08x %08x %08x %08x\n", b, a,
					ReadMacInt32(a), ReadMacInt32(a + 4),
					ReadMacInt32(a + 8), ReadMacInt32(a + 12));
			}
	}
	report_and_reset_rfi_sites();
	exception_diagnostic_state("lowmem-rollback", GetTicks_usec());
}
#endif

#if PPC_DEBUG_TRACE
#if GUEST_STALL_TRACE
// Where an address lives, so a pc printed in a log can be found again offline.
static const char *guest_region_name(uint32 addr)
{
	if (addr >= ROMBase && addr < ROMBase + ROM_SIZE)
		return "ROM-image";
	if (addr >= ROMBase && addr < ROMBase + ROM_AREA_SIZE)
		return "ROM-area-scratch";
	if (addr >= RAMBase && addr < RAMBase + RAMSize)
		return "RAM";
	if (SheepMem::Contains(addr))
		return "SheepMem";
	return "?";
}

void sheepshaver_cpu::exec_frame_push(uint8 kind, uint32 entry, uint32 arg)
{
	const unsigned d = exec_frame_depth;
	if (d >= EXEC_FRAMES) {
		exec_frame_overflow++;
		exec_frame_depth++;
		return;
	}
	exec_frame_kind[d] = kind;
	exec_frame_entry[d] = entry;
	exec_frame_arg[d] = arg;
	exec_frame_68k_pc[d] = gpr(24);
	exec_frame_r25[d] = gpr(25);
	exec_frame_sp[d] = gpr(1);
	exec_frame_usec[d] = GetTicks_usec();
	exec_frame_depth = d + 1;
}

void sheepshaver_cpu::exec_frame_pop(void)
{
	if (exec_frame_depth != 0)
		exec_frame_depth--;
}

void sheepshaver_cpu::exec_frame_dump(const char *why)
{
	static const char *const kind_name[3] = { "EMUL_OP", "Execute68k", "MacOS-PPC" };
	unsigned i;
	const uint64 now = GetTicks_usec();

	ppc_trace("[exec-frames] ",
		"%s: %u nested host-driven guest call(s), overflow %u, "
		"exec-depth %d\n",
		why, exec_frame_depth, exec_frame_overflow,
		current_execute_depth());
	for (i = 0; i < exec_frame_depth && i < EXEC_FRAMES; i++) {
		const uint8 k = exec_frame_kind[i];
		ppc_trace("[exec-frames] ",
			"  [%u] %s entry=%08x (%s) arg=%08x, opened %lu ms ago at "
			"68k pc %08x, r25 %08x, sp %08x\n",
			i, k < 3 ? kind_name[k] : "?", exec_frame_entry[i],
			guest_region_name(exec_frame_entry[i]), exec_frame_arg[i],
			(unsigned long)((now - exec_frame_usec[i]) / 1000),
			exec_frame_68k_pc[i], exec_frame_r25[i], exec_frame_sp[i]);
		// An Execute68k frame is a Time Manager task dispatch when its entry
		// is a Mixed Mode routine descriptor. Name the routine it will run so
		// a callback that never returns can be identified from the log alone.
		if (k == EXEC_FRAME_68K && guest_addr_ok(exec_frame_entry[i], 32) &&
				ReadMacInt16(exec_frame_entry[i]) == 0xAAFE) {
			const uint32 rd = exec_frame_entry[i];
			const uint32 tvect = ReadMacInt32(rd + 20);
			ppc_trace("[exec-frames] ",
				"      routine descriptor: version %u procInfo %08x ISA %u "
				"flags %04x procDescriptor %08x (%s)\n",
				ReadMacInt8(rd + 2), ReadMacInt32(rd + 12),
				ReadMacInt8(rd + 17), ReadMacInt16(rd + 18), tvect,
				guest_region_name(tvect));
			if (guest_addr_ok(tvect, 8))
				ppc_trace("[exec-frames] ",
					"      TVector: code %08x (%s) toc %08x\n",
					ReadMacInt32(tvect), guest_region_name(ReadMacInt32(tvect)),
					ReadMacInt32(tvect + 4));
		}
	}
}

// Runs on the emulation thread, so the register file is coherent and the 68k
// registers actually mean something. The host-thread sampler cannot say that:
// it catches the CPU mid-instruction and in whatever mode it happens to be.
void sheepshaver_cpu::guest_stall_watch(void)
{
	const uint32 tick = ReadMacInt32(0x16a);
	uint64 now;
	unsigned i;

	if (tick != guest_stall_tick) {
		guest_stall_tick = tick;
		guest_stall_tick_usec = GetTicks_usec();
		return;
	}
	if (guest_stall_tick_usec == 0) {
		guest_stall_tick_usec = GetTicks_usec();
		return;
	}
	now = GetTicks_usec();
	if (now - guest_stall_tick_usec < 2000000)
		return;
	// Only report while the 68k emulator itself is the code running. Its
	// register file is the only state in which gpr(8..25) are D0-D7/A0-A6,
	// the 68k pc and the interrupt level. XLM_RUN_MODE is too coarse: it
	// still reads MODE_68K while the nanokernel runs on its own registers.
	// patch_rom_ppc() copies the emulator to ROMBase+0x460000 and stores
	// that in LA_EmulatorCode, which is what execute_68k() loads into r30.
	{
		const uint32 emulator = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);
		if (emulator == 0 || pc() < emulator ||
				pc() >= emulator + 0xa0000)
			return;
		if (gpr(31) != KernelDataAddr + 0x1000)
			return;
	}
	if (guest_stall_report_usec != 0 && now - guest_stall_report_usec < 2000000)
		return;
	if (guest_stall_reports >= 6)
		return;
	guest_stall_report_usec = now;
	guest_stall_reports++;

	{
		const uint32 k68_pc = gpr(24);
		const uint32 a7 = gpr(1);
		ppc_trace("[guest-stall] ",
			"guest clock stopped %lu ms (tick=%08x). ppc pc=%08x (%s) "
			"lr=%08x msr=%08x; 68k pc=%08x (%s) r25=%08x IPL=%u\n",
			(unsigned long)((now - guest_stall_tick_usec) / 1000), tick,
			pc(), guest_region_name(pc()), lr(), msr(),
			k68_pc, guest_region_name(k68_pc), gpr(25), gpr(25) & 7);
		ppc_trace("[guest-stall] ",
			"  d0-d7 %08x %08x %08x %08x %08x %08x %08x %08x\n",
			gpr(8), gpr(9), gpr(10), gpr(11),
			gpr(12), gpr(13), gpr(14), gpr(15));
		ppc_trace("[guest-stall] ",
			"  a0-a6 %08x %08x %08x %08x %08x %08x %08x  a7=%08x\n",
			gpr(16), gpr(17), gpr(18), gpr(19),
			gpr(20), gpr(21), gpr(22), a7);
		if (guest_addr_ok(k68_pc - 16, 48)) {
			ppc_trace("[guest-stall] ", "  68k code at pc-16:");
			for (i = 0; i < 24; i++)
				ppc_trace("[guest-stall] ", "    +%-3d %04x%s", (int)i * 2 - 16,
					ReadMacInt16(k68_pc - 16 + i * 2),
					i == 8 ? "   <- pc" : "");
		}
		// The 68k stack: the return addresses on it name the caller chain that
		// reached the wait, which the pc alone cannot.
		if (guest_addr_ok(a7, 64)) {
			ppc_trace("[guest-stall] ", "  68k stack at a7:\n");
			for (i = 0; i < 16; i++) {
				const uint32 v = ReadMacInt32(a7 + i * 4);
				ppc_trace("[guest-stall] ", "    %08x: %08x (%s)\n",
					a7 + i * 4, v, guest_region_name(v));
			}
		}
		// The ROM's synchronous wait spins on a 16-bit result word going <= 0
		// (move.w (An),d0 / bgt.s .-2 at the emulator's 68k pc). Whichever
		// address register points at a Device Manager parameter block names
		// the call that never completed, so decode all of them.
		{
			for (i = 0; i < 7; i++) {
				const uint32 a = gpr(16 + i);
				if (!guest_addr_ok(a, 32))
					continue;
				ppc_trace("[guest-stall] ",
					"  a%u=%08x (%s): %08x %08x %08x %08x %08x %08x %08x %08x\n",
					i, a, guest_region_name(a),
					ReadMacInt32(a), ReadMacInt32(a + 4),
					ReadMacInt32(a + 8), ReadMacInt32(a + 12),
					ReadMacInt32(a + 16), ReadMacInt32(a + 20),
					ReadMacInt32(a + 24), ReadMacInt32(a + 28));
				// ParamBlockHeader: ioLink/ioType/ioTrap/ioCmdAddr/
				// ioCompletion/ioResult/ioNamePtr/ioVRefNum, then ioRefNum
				// and csCode for a control call.
				ppc_trace("[guest-stall] ",
					"     as pb: ioTrap=%04x ioCmdAddr=%08x ioCompletion=%08x "
					"ioResult=%d ioRefNum=%d csCode=%d\n",
					ReadMacInt16(a + 6), ReadMacInt32(a + 8),
					ReadMacInt32(a + 12), (int)(int16)ReadMacInt16(a + 16),
					(int)(int16)ReadMacInt16(a + 24),
					(int)(int16)ReadMacInt16(a + 26));
			}
		}
		// Execute68k() pushes the EXEC_RETURN opcode address as the 68k return
		// address. If it is no longer on that stack, the nested run can never
		// end even once the guest unblocks.
		if (exec_frame_depth >= 2 && guest_addr_ok(exec_frame_sp[1], 64)) {
			const uint32 fsp = exec_frame_sp[1];
			ppc_trace("[guest-stall] ",
				"  Execute68k frame sp=%08x, EXEC_RETURN opcode at %08x:\n",
				fsp, (uint32)XLM_EXEC_RETURN_OPCODE);
			for (i = 0; i < 16; i++)
				ppc_trace("[guest-stall] ",
					"    %08x: %08x%s\n", fsp + i * 4,
					ReadMacInt32(fsp + i * 4),
					ReadMacInt32(fsp + i * 4) == (uint32)XLM_EXEC_RETURN_OPCODE
						? "  <- EXEC_RETURN" : "");
		}
	}
	exec_frame_dump("guest clock stopped");
	exception_diagnostic_state("guest-stall", now);
}
#endif	/* GUEST_STALL_TRACE */

void sheepshaver_cpu::exception_stall_sample(void)
{
	// From the first time the debugger is entered onwards. For a freeze with
	// no program exception at all - a guest wedged in its own loop, or one
	// whose clock stopped - this never fires; GUEST_STALL_TRACE covers that
	// case instead.
	if (exception_last_program_usec == 0)
		return;

	const uint64 now = GetTicks_usec();
	const uint32 sampled_pc = pc();

	// Catch the corruption at the tick it happens rather than minutes later
	// once the Event Manager has wedged on it. EvtQHead ($14C) going
	// self-linked, or pointing at an element with a nonsense event code, is
	// the state the ROM's queue walk can never leave.
	{
		const uint32 qhead = ReadMacInt32(0x14c);
		bool bad = false;
		if (qhead != 0 && guest_addr_ok(qhead, 8)) {
			const uint32 link = ReadMacInt32(qhead);
			const uint32 what = ReadMacInt16(qhead + 6);
			bad = (link == qhead) || (what > 23);
		}
		if (bad && !exception_queue_reported) {
			unsigned n = exception_track_count;
			unsigned i = (exception_track_next + EXC_TRACK - n) % EXC_TRACK;
			unsigned k;
			exception_queue_reported = true;
			ppc_trace("[exception-corrupt] ",
				"EvtQHead %08x went bad: qLink=%08x what=%04x qTail=%08x; "
				"ppc pc=%08x, 68k pc=%08x, r25=%08x, mode=%u, nest=%d, "
				"depth=%u, step %s\n",
				qhead, ReadMacInt32(qhead), ReadMacInt16(qhead + 6),
				ReadMacInt32(0x150), sampled_pc, cur_gpr(24), cur_gpr(25),
				ReadMacInt32(XLM_RUN_MODE),
				(int32)ReadMacInt32(XLM_IRQ_NEST), exception_entry_depth,
				exception_step_pending ? "armed" : "idle");
			for (k = 0; k < n; k++) {
				ppc_trace("[exception-corrupt] ",
					"  -%lu ms: ppc %08x, 68k pc %08x, r25 %08x, mode %u\n",
					(unsigned long)((now - exception_track_usec[i]) / 1000),
					exception_track_ppc_pc[i], exception_track_68k_pc[i],
					exception_track_level[i], exception_track_mode[i]);
				i = (i + 1) % EXC_TRACK;
			}
		} else if (!bad) {
			exception_queue_reported = false;
		}
	}

	// Record every sample, and shout the moment the 68k interrupt level or the
	// run mode changes. Level 7 masks everything, so the transition into it is
	// the event that matters and it is invisible in a periodic dump.
	const uint32 level_now = cur_gpr(25) & 7;
	const uint32 mode_now = ReadMacInt32(XLM_RUN_MODE);
	exception_track_ppc_pc[exception_track_next] = sampled_pc;
	exception_track_68k_pc[exception_track_next] = cur_gpr(24);
	exception_track_level[exception_track_next] = cur_gpr(25);
	exception_track_mode[exception_track_next] = mode_now;
	exception_track_usec[exception_track_next] = now;
	exception_track_next = (exception_track_next + 1) % EXC_TRACK;
	if (exception_track_count < EXC_TRACK)
		exception_track_count++;
	/*if (!exception_level_seen || level_now != exception_last_seen_level ||
			mode_now != exception_last_seen_mode) {
		ppc_trace("[exception-level] ",
			"68k level %u -> %u, mode %u -> %u at ppc %08x, 68k pc %08x, "
			"r25=%08x, nest %d, depth %u, step %s\n",
			exception_level_seen ? exception_last_seen_level : 0xffffffffu,
			level_now,
			exception_level_seen ? exception_last_seen_mode : 0xffffffffu,
			mode_now, sampled_pc, cur_gpr(24), cur_gpr(25),
			(int32)ReadMacInt32(XLM_IRQ_NEST), exception_entry_depth,
			exception_step_pending ? "armed" : "idle");
		exception_last_seen_level = level_now;
		exception_last_seen_mode = mode_now;
		exception_level_seen = true;
	}*/

	if (sampled_pc != exception_stall_pc) {
		exception_stall_pc = sampled_pc;
		exception_stall_moved_usec = now;
	} else if (exception_stall_moved_usec == 0) {
		exception_stall_moved_usec = now;
	}

	// The guest clock, which is the symptom that actually matters. A guest can
	// be executing - writing DEC, taking decrementer vectors - while no VIA
	// interrupt is serviced and TickCount never advances. That is the freeze,
	// and a pc-only test misses it completely because the pc keeps moving.
	InterruptServiceDiagnostics service;
	GetInterruptServiceDiagnostics(service);
	const uint32 tick_now = ReadMacInt32(0x16a);
	if (tick_now != exception_stall_tick ||
			service.via_services != exception_stall_via_services) {
		exception_stall_tick = tick_now;
		exception_stall_via_services = service.via_services;
		exception_stall_tick_moved_usec = now;
	} else if (exception_stall_tick_moved_usec == 0) {
		exception_stall_tick_moved_usec = now;
	}

	const bool pc_stuck = (now - exception_stall_moved_usec) >= 2000000;
	const bool clock_stuck = (now - exception_stall_tick_moved_usec) >= 2000000;
	if (!pc_stuck && !clock_stuck)
		return;
	if (exception_stall_sample_usec != 0 &&
			now - exception_stall_sample_usec < 1000000)
		return;
	exception_stall_sample_usec = now;
	exception_stall_samples++;

	ppc_trace("[exception-stall] ",
		"guest clock stopped %lu ms (tick=%08x, %llu VIA service(s)); "
		"pc %s for %lu ms\n",
		(unsigned long)((now - exception_stall_tick_moved_usec) / 1000),
		tick_now, (unsigned long long)service.via_services,
		pc_stuck ? "unchanged" : "still moving",
		(unsigned long)((now - exception_stall_moved_usec) / 1000));
	ppc_trace("[exception-stall] ",
		"guest at pc=%08x (%08x) lr=%08x sp=%08x msr=%08x "
		"srr0=%08x srr1=%08x SE=%u\n",
		sampled_pc,
		guest_addr_ok(sampled_pc, 4) ? ReadMacInt32(sampled_pc) : 0xffffffffu,
		lr(), gpr(1), msr(), srr0(), srr1(),
		(msr() & PPC_MSR_SE) != 0 ? 1u : 0u);
	ppc_trace("[exception-stall] ",
		"last vector %s entered %lu ms ago, architectural depth %u, "
		"nest %d, mode %u, exec-depth %d, step %s at %08x, "
		"%llu vector entr%s and %llu step(s) so far\n",
		ppc_exception_vector_name(exception_last_vector),
		(unsigned long)((now - exception_last_vector_usec) / 1000),
		exception_entry_depth, (int32)ReadMacInt32(XLM_IRQ_NEST),
		ReadMacInt32(XLM_RUN_MODE), current_execute_depth(),
		exception_step_pending ? "armed" : "idle", exception_step_pc,
		(unsigned long long)exception_vector_entries,
		exception_vector_entries == 1 ? "y" : "ies",
		(unsigned long long)exception_steps_taken);
	ppc_trace("[exception-stall] ",
		"interrupts: flags=%08x spc=%08x accepted=%llu (%llu 68k, %llu emul-op, "
		"%llu native vector), deferred %llu nest / %llu mode / %llu 68k-IPL; "
		"OP_IRQ=%llu VIA=%llu; 68k pc=%08x level=%08x\n",
		(uint32)InterruptFlags, spcflags().get(),
		(unsigned long long)external_interrupt_accepted_count,
		(unsigned long long)external_interrupt_68k_accepted_count,
		(unsigned long long)external_interrupt_emul_accepted_count,
		(unsigned long long)external_interrupt_native_vector_count,
		(unsigned long long)external_interrupt_nest_deferred_count,
		(unsigned long long)external_interrupt_mode_deferred_count,
		(unsigned long long)external_interrupt_68k_deferred_count,
		(unsigned long long)service.op_irq_entries,
		(unsigned long long)service.via_services,
		cur_gpr(24), cur_gpr(25));
#if GUEST_STALL_TRACE
	exec_frame_dump("host sampler");
#endif
	// The 68k side in full: its emulator keeps D0-D7 in r8-r15, A0-A7 in
	// r16-r23, the 68k pc in r24 and the interrupt level in r25. If the level
	// is stuck at 7 nothing below it can ever be serviced, so the registers and
	// the instruction stream at that pc say which routine masked them.
	{
		const uint32 k68_pc = cur_gpr(24);
		ppc_trace("[exception-stall] ",
			"68k pc=%08x level=%08x  d0-d7 %08x %08x %08x %08x %08x %08x "
			"%08x %08x\n", k68_pc, cur_gpr(25),
			cur_gpr(8), cur_gpr(9), cur_gpr(10), cur_gpr(11),
			cur_gpr(12), cur_gpr(13), cur_gpr(14), cur_gpr(15));
		ppc_trace("[exception-stall] ",
			"   a0-a7 %08x %08x %08x %08x %08x %08x %08x %08x\n",
			cur_gpr(16), cur_gpr(17), cur_gpr(18), cur_gpr(19),
			cur_gpr(20), cur_gpr(21), cur_gpr(22), cur_gpr(23));
		if (guest_addr_ok(k68_pc, 16))
			ppc_trace("[exception-stall] ",
				"   68k code %04x %04x %04x %04x %04x %04x %04x %04x\n",
				ReadMacInt16(k68_pc), ReadMacInt16(k68_pc + 2),
				ReadMacInt16(k68_pc + 4), ReadMacInt16(k68_pc + 6),
				ReadMacInt16(k68_pc + 8), ReadMacInt16(k68_pc + 10),
				ReadMacInt16(k68_pc + 12), ReadMacInt16(k68_pc + 14));
		// Is an interrupt actually being presented to the 68k emulator?
		const uint32 level_addr = ReadMacInt32(KERNEL_DATA_BASE + 0x67c);
		ppc_trace("[exception-stall] ",
			"presented level@%08x=%04x mask=%08x cr=%08x lowmem ticks=%08x\n",
			level_addr, guest_addr_ok(level_addr, 2) ? ReadMacInt16(level_addr) : 0xffff,
			ReadMacInt32(KERNEL_DATA_BASE + 0x674), cr().get(),
			ReadMacInt32(0x16a));
	}

	// The event queue itself. The freeze is the ROM's Event Manager walking it
	// with the 68k interrupt level raised to 7 (ROM 0x10dbc ori.w #$700,sr,
	// walk at 0x10dc6..0x10dd2) and never reaching the sr restore, because the
	// chain is circular. Printing it names the element that closes the cycle,
	// and whether qTail already pointed at it - which is what Enqueue produces
	// when the same element is enqueued twice.
	{
		const uint32 qflags = ReadMacInt16(0x14a);
		const uint32 qhead = ReadMacInt32(0x14c);
		const uint32 qtail = ReadMacInt32(0x150);
		uint32 seen[48];
		uint32 e = qhead;
		int n = 0;
		ppc_trace("[exception-queue] ",
			"EventQueue qFlags=%04x qHead=%08x qTail=%08x\n",
			qflags, qhead, qtail);
		while (e != 0 && n < 48 && guest_addr_ok(e, 16)) {
			const uint32 next = ReadMacInt32(e);
			int k;
			int loop = -1;
			for (k = 0; k < n; k++)
				if (seen[k] == e) { loop = k; break; }
			ppc_trace("[exception-queue] ",
				"  [%d] %08x qLink=%08x what=%04x message=%08x when=%08x%s%s\n",
				n, e, next, ReadMacInt16(e + 6), ReadMacInt32(e + 8),
				ReadMacInt32(e + 12),
				e == qtail ? " <-qTail" : "",
				loop >= 0 ? " <-CYCLE" : "");
			if (loop >= 0)
				break;
			seen[n++] = e;
			e = next;
		}
		if (n >= 48)
			ppc_trace("[exception-queue] ", "  ... truncated at 48\n");
	}

	// The rolling history, oldest first: where the guest has been since the
	// debugger was entered, and when the level changed.
	{
		unsigned n = exception_track_count;
		unsigned i = (exception_track_next + EXC_TRACK - n) % EXC_TRACK;
		unsigned k;
		for (k = 0; k < n; k++) {
			ppc_trace("[exception-track] ",
				"-%lu ms: ppc %08x, 68k pc %08x, r25 %08x, mode %u\n",
				(unsigned long)((now - exception_track_usec[i]) / 1000),
				exception_track_ppc_pc[i], exception_track_68k_pc[i],
				exception_track_level[i], exception_track_mode[i]);
			i = (i + 1) % EXC_TRACK;
		}
		exception_track_count = 0;
	}

	// The full machine picture, but only the first few times so a long freeze
	// does not fill the log with identical dumps.
	if (exception_stall_samples <= 3 ||
			sampled_pc != exception_stall_last_reported_pc)
		exception_diagnostic_state("stall", now);
	exception_stall_last_reported_pc = sampled_pc;
}
#endif

#if PPC_DEBUG_TRACE
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
#endif

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
	sprg(3) = table;
	srr0() = saved_pc;
	srr1() = saved_msr;
	msr() = ppc_exception_msr(interrupted_msr);
	WriteMacInt32(XLM_IRQ_NEST, (uint32)nest + 1);
	exception_entry_depth++;
#if PPC_DEBUG_TRACE
	exception_last_vector = vector;
	exception_last_vector_usec = GetTicks_usec();
	exception_vector_entries++;
	// Every entry, not just program ones. The trace vector is the one that
	// stops coming back, and without this there is no record that it was even
	// entered, nor of which handler it dispatched to. External and decrementer
	// vectors fire constantly, so they are only logged while a debugger chain
	// or an armed step is in flight.
	if (vector != PPC_EXTERNAL_VECTOR || exception_step_pending ||
			exception_entry_depth > 1) {
		ppc_trace("[exception-entry] ",
			"%s vector at %08x: handler %08x, saved pc %08x, srr1 %08x, "
			"nest %d->%d, architectural depth %u, step %s, mode %u, "
			"68k pc %08x level %08x\n",
			ppc_exception_vector_name(vector), vector_pc, handler, saved_pc,
			saved_msr, nest, nest + 1, exception_entry_depth,
			exception_step_pending ? "armed" : "idle",
			ReadMacInt32(XLM_RUN_MODE), gpr(24), gpr(25));
	}
	// The full low-memory diff below costs 192 guest reads per block, which
	// would be slower than booting under a per-instruction watch. The rollback
	// only ever appears once the debugger is running, so arm it there and
	// leave the boot at the two-read trigger.
	if (vector == PPC_TRACE_VECTOR || vector == PPC_PROGRAM_VECTOR)
		lowmem_armed = true;
	if (vector == PPC_TRACE_VECTOR)
		exception_traces_since_program++;
	else if (vector == PPC_DECREMENTER_VECTOR)
		exception_decrementers_since_program++;
#endif
	pc() = vector_pc;
	return NULL;
}

bool sheepshaver_cpu::decrementer_exception()
{
	return enter_exception_vector(PPC_DECREMENTER_VECTOR,
		PPC_DECREMENTER_HANDLER_SLOT, pc(),
		ppc_exception_srr1(msr(), 0)) == NULL;
}

void sheepshaver_cpu::acquire_fp_registers()
{
	msr() |= MSR_FP;
	if (KernelDataAddr == 0)
		return;
	{
		const uint32 kd = (uint32)KernelDataAddr;
		const uint32 ctx = ReadMacInt32(kd + 0x65c);
		int i;

		if (!guest_addr_ok(ctx + 0x200, 32 * 8))
			return;
		WriteMacInt32(kd + 0xf28, ReadMacInt32(kd + 0xf28) + 1);
		fpscr() = ReadMacInt32(ctx + 0xe4);
		for (i = 0; i < 32; i++) {
			const uint32 a = ctx + 0x200 + i * 8;
			fpr_dw(i) = ((uint64)ReadMacInt32(a) << 32) | ReadMacInt32(a + 4);
		}
	}
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
		PPC_EXTERNAL_HANDLER_SLOT, pc(),
		ppc_exception_srr1(msr(), 0));
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
	preserve_system_ticks("trap delivery");

	const uint32 trap_pc = pc();
#if PPC_DEBUG_TRACE
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
#endif
	const char *why = enter_exception_vector(PPC_PROGRAM_VECTOR,
		PPC_PROGRAM_HANDLER_SLOT, trap_pc,
		ppc_exception_srr1(msr(), PPC_SRR1_PROGRAM_TRAP));
	if (why != NULL)
		return why;

#if PPC_DEBUG_TRACE
	if (previous_program != 0) {
		ppc_trace("[exception] ",
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
		ppc_trace("[exception] ",
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
		ppc_trace("[exception] ",
			"interrupt service since previous: %llu OP_IRQ entr%s, "
			"%llu VIA service(s)\n",
			(unsigned long long)op_irq_delta,
			op_irq_delta == 1 ? "y" : "ies",
			(unsigned long long)service_delta);
		ppc_trace("[exception] ",
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
		ppc_trace("[exception] ",
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
#endif
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
	preserve_system_ticks("rfi return");
	uint32 return_pc = saved_pc & ~3u;
#if PPC_DEBUG_TRACE
	if (exception_last_program_usec != 0)
		record_rfi_site(return_pc, ReadMacInt32(XLM_RUN_MODE));
#endif
	const int32 nest = (int32)ReadMacInt32(XLM_IRQ_NEST);
	if (nest > 0)
		WriteMacInt32(XLM_IRQ_NEST, (uint32)nest - 1);
	else if (exception_entry_depth != 0)
		gfx_log_emit("[crash] ",
			"PowerPC rfi has invalid nanokernel nesting state %d\n", nest);

#if PPC_DEBUG_TRACE
	exception_returns_since_program++;
#endif
	if (exception_entry_depth != 0) {
#if PPC_DEBUG_TRACE
		const uint64 now = GetTicks_usec();
		const uint64 vector_usec = now - exception_last_vector_usec;
		if (exception_last_vector == PPC_PROGRAM_VECTOR ||
			exception_last_vector == PPC_TRACE_VECTOR ||
			exception_step_pending || exception_entry_depth > 1 ||
			(msr() & PPC_MSR_SE) != 0 || vector_usec >= 100000) {
			ppc_trace("[exception] ",
				"%s vector returned in %lu us to %08x "
				"(SE=%u, nest=%d, architectural depth=%u, mode=%u, "
				"68k pc=%08x level=%08x)\n",
				ppc_exception_vector_name(exception_last_vector),
				(unsigned long)vector_usec, return_pc,
				(msr() & PPC_MSR_SE) != 0 ? 1u : 0u,
				(int32)ReadMacInt32(XLM_IRQ_NEST), exception_entry_depth,
				ReadMacInt32(XLM_RUN_MODE), gpr(24), gpr(25));
		}
#endif
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
#if PPC_DEBUG_TRACE
			const uint64 now = GetTicks_usec();
			ppc_trace("[exception] ",
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
#endif
		} else {
			gfx_log_emit("[crash] ",
				"could not allocate the PowerPC trace trampoline\n");
			msr() &= ~PPC_MSR_SE;
		}
	}
	/* A return into the 68k emulator that skipped the lwz r1,4(r1). */
	if (ppc_68k_sp_parked(gpr(1)) && ppc_pc_in_68k_emulator(return_pc))
		gpr(1) = ppc_recover_68k_sp(gpr(1));
#if PPC_REPORT_BAD_EA
	/* Instruction fetch has no range check, so an rfi to a wild SRR0 takes the
	   process down with nothing logged. Say so while the registers that
	   produced it are still readable. */
	if (!guest_addr_ok(return_pc, 4)) {
		/* Powers of two: a wild rfi inside a loop must not become the reason
		   the emulator is slow. */
		static uint32 wild;

		wild++;
		if ((wild & (wild - 1)) == 0)
			bug("[bad-ea] rfi to %08x x%u"
				" (srr0 %08x srr1 %08x lr %08x r1 %08x)\n",
				return_pc, wild, saved_pc, saved_msr,
				get_register(powerpc_registers::LR).i, gpr(1));
	}
#endif
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
#if PPC_DEBUG_TRACE
	const uint32 stepped_pc = pc();
	const uint32 stepped_opcode = ReadMacInt32(stepped_pc);
#endif
	exception_step_pending = false;
	exception_step_trap = false;
	exception_step_opcode = 0;
	exception_step_active = true;
#if PPC_DEBUG_TRACE
	const uint64 step_started = GetTicks_usec();
	exception_steps_taken++;
#endif
	execute_one_instruction();
#if PPC_DEBUG_TRACE
	const uint64 step_usec = GetTicks_usec() - step_started;
#endif
	exception_step_active = false;

	const bool trapped = exception_step_trap;
	const uint32 vector = trapped ? PPC_PROGRAM_VECTOR : PPC_TRACE_VECTOR;
	const uint32 slot = trapped
		? PPC_PROGRAM_HANDLER_SLOT : PPC_TRACE_HANDLER_SLOT;
	const uint32 saved_msr = ppc_exception_srr1(msr(),
		trapped ? PPC_SRR1_PROGRAM_TRAP : 0);
	const uint32 saved_pc = pc();
#if PPC_DEBUG_TRACE
	ppc_trace("[exception] ",
		"single instruction at %08x (%08x) took %lu us; raising %s "
		"exception at %08x (mode %u, 68k pc %08x, level %08x, nest %d)\n",
		stepped_pc, stepped_opcode,
		(unsigned long)step_usec, trapped ? "program" : "trace", saved_pc,
		ReadMacInt32(XLM_RUN_MODE), gpr(24), gpr(25),
		(int32)ReadMacInt32(XLM_IRQ_NEST));
#endif
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


// Guest PC, for sampling from the host tick thread. A racy read of one word:
// the point is to find where the guest is spinning when it stops taking
// interrupts, and a sampling profiler is the only thing that still works
// then - anything driven from the VBL has stopped running by definition.
uint32 PPCSampleGuestPC(void)
{
	return ppc_cpu ? ppc_cpu->cur_pc() : 0;
}

// The ROM's 68k emulator keeps the 68k instruction pointer in r24 (its fetch
// is lhau r27,2(r24)) and the interrupt level in r25, so sampling those says
// which 68k code is looping and whether it has interrupts masked.
uint32 PPCSampleGuestGPR(int i)
{
	return ppc_cpu ? ppc_cpu->cur_gpr(i) : 0;
}

void PPCExceptionIdleDiagnostic(void)
{
#if PPC_DEBUG_TRACE
	if (ppc_cpu != NULL)
		ppc_cpu->exception_idle_diagnostic();
#endif
}

// Called from the host tick thread, which keeps running when the guest does
// not. exception_idle_diagnostic() is driven from SynchIdleTime, so it reports
// nothing in precisely the case that matters: a guest stuck inside an exception
// handler never reaches the idle loop again. Sampling from outside is the only
// view left once that happens.
void PPCExceptionStallSample(void)
{
#if PPC_DEBUG_TRACE
	if (ppc_cpu != NULL)
		ppc_cpu->exception_stall_sample();
#endif
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
		bug("%s0x%8llx:  ", addr == pc ? " >" : "  ", addr);
		print_insn_ppc(addr, &info);
		bug("\n");
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

/* Is this a 68k stack pointer? */
static bool ppc_68k_sp_ok(uint32 sp)
{
	return sp != 0 && (sp & 1) == 0 && !ppc_68k_sp_parked(sp)
		&& guest_addr_ok(sp, 4);
}

static bool ppc_pc_in_68k_emulator(uint32 pc)
{
	const uint32 emulator = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);

	return emulator != 0 && pc - emulator < 0xa0000u;
}

/* Returns a7 unchanged when nothing usable is parked. */
uint32 ppc_recover_68k_sp(uint32 a7)
{
	uint32 sp = ReadMacInt32(KERNEL_DATA_BASE + 0x004);
	uint32 ctx;

	if (ppc_68k_sp_ok(sp))
		return sp;
	ctx = ReadMacInt32(KERNEL_DATA_BASE + 0x658);
	if (!guest_addr_ok(ctx, 0x110))
		return a7;
	sp = ReadMacInt32(ctx + 0x10c);
	return ppc_68k_sp_ok(sp) ? sp : a7;
}

#if PPC_REPORT_BAD_EA

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

struct ppc_68k_branch ppc_68k_branches[PPC_68K_BRANCHES];
uint16 ppc_68k_branch_map[PPC_68K_BRANCH_HASH];
int ppc_68k_branch_pos = -1;
static const uint32 ppc_68k_r31 = (uint32)(KERNEL_DATA_BASE + 0x1000);
bool ppc_68k_a7_flagged = false;

uint32 ppc_68k_last_pc = 0;

static void ppc_dump_68k_branches(void)
{
	static int dumps = 0;
	/* winbug() formats into a 1K record, so keep a batch under that. */
	char out[900], line[128];
	int i, first, o = 0;

	if (dumps >= 3 || ppc_68k_branch_pos < 0)
		return;
	dumps++;
	first = PPC_68K_BRANCHES - PPC_68K_BRANCH_DUMP;
	if (first < 1)
		first = 1;
	/* Batched to what one record holds. Each bug() is a synchronous debugger
	   write, so a line at a time is 256 round trips and a visible stall. */
	for (i = first; i <= PPC_68K_BRANCHES; i++) {
		int k = (ppc_68k_branch_pos + i) & (PPC_68K_BRANCHES - 1);
		int n;

		if (ppc_68k_branches[k].hits == 0)
			continue;
		n = snprintf(line, sizeof(line),
			"[bad-ea] 68k branch %08x -> %08x op=%04x x%u\n",
			ppc_68k_branches[k].from, ppc_68k_branches[k].to,
			ppc_68k_branches[k].op & 0xffff, ppc_68k_branches[k].hits);
		if (n <= 0)
			continue;
		if (o + n >= (int)sizeof(out)) {
			out[o] = 0;
			bug("%s", out);
			o = 0;
		}
		memcpy(out + o, line, n);
		o += n;
	}
	if (o != 0) {
		out[o] = 0;
		bug("%s", out);
	}
}

void ppc_log_68k_op(uint32 from, uint32 npc, uint32 op, uint32 a7)
{
	static uint32 seen[8];
	static int seen_count = 0;
	char msg[320];
	uint32 ksp, ctx, ctx_sp = 0;
	int i, n;

	if (!ppc_report_is_new(from ^ (npc << 1) ^ op, seen, 8, &seen_count))
		return;
	ksp = ReadMacInt32(KERNEL_DATA_BASE + 0x004);
	ctx = ReadMacInt32(KERNEL_DATA_BASE + 0x658);
	if (guest_addr_ok(ctx, 0x110))
		ctx_sp = ReadMacInt32(ctx + 0x10c);
	n = snprintf(msg, sizeof(msg),
		"[bad-ea] 68k op=%04x %08x -> %08x a7=%08x ksp=%08x ctx_sp=%08x vec5c=%08x",
		op, from, npc, a7, ksp, ctx_sp, ReadMacInt32(0x5c));
	if (n < 0)
		n = 0;
	if (guest_addr_ok(npc, 8) && n < (int)sizeof(msg) - 28) {
		n += snprintf(msg + n, sizeof(msg) - n, " dest");
		for (i = 0; i < 4 && n > 0 && n < (int)sizeof(msg) - 8; i++)
			n += snprintf(msg + n, sizeof(msg) - n, " %04x",
				ReadMacInt16(npc + i * 2));
	}
	bug("%s\n", msg);
}

static void ppc_report_context(const char *what, uint32 pc, uint32 ea)
{
	sheepshaver_cpu *cpu = ppc_cpu;
	int i;
	char msg[500];

	bug(
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
	/* +0x004 is the sp the nanokernel saved, +0x658/+0x65c the contexts. */
	{
		uint32 ctx68k = ReadMacInt32(KERNEL_DATA_BASE + 0x658);
		uint32 ctx_pc = 0, ctx_sp = 0;

		if (guest_addr_ok(ctx68k, 0x110)) {
			ctx_pc = ReadMacInt32(ctx68k + 0xfc);
			ctx_sp = ReadMacInt32(ctx68k + 0x10c);
		}
		bug("  kdata sp=%08x ctx68k=%08x cur=%08x status=%08x level=%08x\n"
			"  ctx_pc=%08x ctx_sp=%08x vec5c=%08x tramp=%08x %08x\n",
			ReadMacInt32(KERNEL_DATA_BASE + 0x004),
			ctx68k,
			ReadMacInt32(KERNEL_DATA_BASE + 0x65c),
			ReadMacInt32(KERNEL_DATA_BASE + 0x660),
			ReadMacInt32(KERNEL_DATA_BASE + 0x67c),
			ctx_pc, ctx_sp, ReadMacInt32(0x5c),
			ReadMacInt32(KERNEL_DATA_BASE + 0x5f0),
			ReadMacInt32(KERNEL_DATA_BASE + 0x5f4));
	}
	/* The same registers under their 68k names. */
	{
		if (ppc_pc_in_68k_emulator(pc)) {
			bug("  68k d0-d7 %08x %08x %08x %08x %08x %08x %08x %08x\n"
				"  68k a0-a7 %08x %08x %08x %08x %08x %08x %08x %08x\n",
				cpu->gpr(8), cpu->gpr(9), cpu->gpr(10), cpu->gpr(11),
				cpu->gpr(12), cpu->gpr(13), cpu->gpr(14), cpu->gpr(15),
				cpu->gpr(16), cpu->gpr(17), cpu->gpr(18), cpu->gpr(19),
				cpu->gpr(20), cpu->gpr(21), cpu->gpr(22), cpu->gpr(1));
		}
	}

	bug("  ticks=%u runmode=%u irqnest=%d r25=%08x\n",
		ReadMacInt32(0x16a), ReadMacInt32(XLM_RUN_MODE),
		(int)ReadMacInt32(XLM_IRQ_NEST), cpu->gpr(25));
	/* A dispatch table is usually still addressed by one of these. */
	{
		int reg;

		for (reg = 0; reg < 7; reg++) {
			uint32 base = cpu->gpr(16 + reg);
			int o, k;
			char msg[500];
			if (!guest_addr_ok(base, 0x20))
				continue;
			o = snprintf(msg, sizeof(msg),
				"[bad-ea] a%d %08x:", reg, base);
			for (k = 0; k < 0x20; k += 4)
				o += snprintf(msg + o, sizeof(msg) - o,
					" %08x", ReadMacInt32(base + k));
			bug("%s\n", msg);
		}
	}
	/* The 68k stack. */
	{
		uint32 sp = cpu->gpr(1);
		int o = 0, k;

		if (guest_addr_ok(sp, 0x60)) {
			for (k = 0; k < 0x60; k += 16) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] a7+%03x:", k);
				for (i = 0; i < 16; i += 2)
					o += snprintf(msg + o, sizeof(msg) - o, " %04x",
						ReadMacInt16(sp + k + i));
				bug("%s\n", msg);
			}
		}
	}
	/* The structure a3 points at. */
	{
		uint32 rec = cpu->gpr(19);
		int o = 0, k;

		if (guest_addr_ok(rec, 0x90)) {
			for (k = 0; k < 0x90; k += 16) {
				o = snprintf(msg, sizeof(msg), "[bad-ea] a3+%03x:", k);
				for (i = 0; i < 16; i += 2)
					o += snprintf(msg + o, sizeof(msg) - o, " %04x",
						ReadMacInt16(rec + k + i));
				bug("%s\n", msg);
			}
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
				bug("%s\n", msg);
			}
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
			bug("[bad-ea] frame %2d sp=%08x back=%08x lr=%08x\n",
				depth, sp, back, saved_lr);
			if (back <= sp || back - sp > 0x100000)
				break;
			sp = back;
		}
	}
	/* The low memory globals this class of bug tramples or reads: the 68k
	   exception vectors live below 0x100, the unit table pointer is at 0x11c
	   and the SCC register base addresses are at 0x1d8 and 0x1dc. */
	{
		int o = 0, k, j;
		char msg[500];

		for (k = 0; k < 0x200; k += 32) {
			o = snprintf(msg, sizeof(msg), "[bad-ea] lomem %03x:", k);
			for (j = 0; j < 32; j += 4)
				o += snprintf(msg + o, sizeof(msg) - o, " %08x",
					ReadMacInt32(k + j));
			bug("%s\n", msg);
		}
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
				bug("%s\n", msg);
			}
		}
	}
	/* The 68k instruction stream around the faulting instruction, so it can
	   be disassembled afterwards and matched against the program's CODE. */
	{
		uint32 p68 = cpu->gpr(24);
		uint32 base = p68 - 32;
		int o = 0, k;
		char msg[500];

		if (p68 >= 32 && guest_addr_ok(base, 80)) {
			o = snprintf(msg, sizeof(msg), "[bad-ea] 68k code %08x:", base);
			for (k = 0; k < 80 && o < (int)sizeof(msg) - 4; k += 2)
				o += snprintf(msg + o, sizeof(msg) - o, " %04x",
					ReadMacInt16(base + k));
			bug("%s\n", msg);
		}
	}
}

void ppc_report_bad_ea(uint32 pc, uint32 ea, int is_load)
{
	static uint32 seen[64];
	static int seen_count = 0;
	const char *what = "store";
	uint32 key;

	if (ppc_cpu == NULL || guest_addr_ok(ea, 1))
		return;
	/* One PowerPC pc is one 68k opcode shape, so key on the 68k pc. */
	key = pc;
	if (ppc_cpu->gpr(31) == ppc_68k_r31)
		key = ppc_cpu->gpr(24);
	if (!ppc_report_is_new(key, seen, 64, &seen_count))
		return;
	if (is_load)
		what = "load";
	ppc_report_context(what, pc, ea);
	ppc_dump_68k_branches();
}

void ppc_report_bad_jump(uint32 pc, uint32 from, uint32 to);
void ppc_report_bad_a7(uint32 pc, uint32 a7);

/* a7 is post-recovery here, so parked means the recovery itself failed. */
void ppc_report_68k_transfer(uint32 pc, uint32 from, uint32 to, uint32 op,
	uint32 a7)
{
	const bool parked = ppc_68k_sp_parked(a7);

	if (op == 0xfe07
			|| (op == 0xfe02
				&& (to >= 0x10000000u || (to & 1) || to < 0x10000u || parked)))
		ppc_log_68k_op(from, to, op, a7);
	if ((to < 0x10000u && (to - 0x2800u) >= 0x100u && parked) || (to & 1)
			|| ((0xff88u >> (to >> 28)) & 1)
			|| (to - 0x41000000u) < 0x0f000000u)
		ppc_report_bad_jump(pc, from, to);
	else if (!ppc_68k_a7_flagged && parked)
		ppc_report_bad_a7(pc, a7);
}

void ppc_report_bad_jump(uint32 pc, uint32 from, uint32 to)
{
	static uint32 seen[64];
	static int seen_count = 0;
	char msg[512];
	int o, k;

	if (ppc_cpu == NULL)
		return;
	/* Keyed on the pair: one site, two wrong targets, is two findings. */
	if (!ppc_report_is_new(from ^ (to << 1), seen, 64, &seen_count)) {
		static uint32 spent;

		spent++;
		if ((spent & (spent - 1)) == 0)
			bug("[bad-ea] 68k jump %08x -> %08x (context budget spent, x%u)\n", from, to, spent);
		return;
	}
	ppc_report_context("68k jump", pc, to);
	/* The instruction that handed control over: jmp, rts or a table. */
	if (guest_addr_ok(from - 0x20, 0x60)) {
		o = snprintf(msg, sizeof(msg), "[bad-ea] 68k from %08x:", from - 0x20);
		for (k = 0; k < 0x60 && o < (int)sizeof(msg) - 8; k += 2)
			o += snprintf(msg + o, sizeof(msg) - o, " %04x",
				ReadMacInt16(from - 0x20 + k));
		bug("%s\n", msg);
	}
	ppc_dump_68k_branches();
}

void ppc_report_bad_a7(uint32 pc, uint32 a7)
{
	if (ppc_cpu == NULL || ppc_68k_a7_flagged)
		return;
	ppc_68k_a7_flagged = true;
	ppc_report_context("68k a7 in kernel data", pc, a7);
	ppc_dump_68k_branches();
}

void ppc_report_fault_trail(void)
{ /* The trail into a fault */
	ppc_dump_68k_branches();
}
/* A store into the 68k exception vectors. */
void ppc_report_vector_store(uint32 pc, uint32 ea)
{
	static uint32 seen[64];
	static int seen_count = 0;
	uint32 emulator;
	uint32 key;

	if (ppc_cpu == NULL)
		return;
	key = pc;
	emulator = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);
	if (emulator != 0 && pc >= emulator && pc < emulator + 0xa0000)
		key = ppc_cpu->gpr(24);
	if (!ppc_report_is_new(key, seen, 64, &seen_count))
		return;
	if (ea != 0x28 && ea != 0x2c) {
		bug("[bad-ea] vector store ea=%08x pc=%08x 68k-pc=%08x\n",
			ea, pc, ppc_cpu->gpr(24));
		return;
	}
	ppc_report_context("vector store", pc, ea);
}

#endif

static void dump_crash_context(sheepshaver_cpu *cpu)
{
	// Guest stack crawl. PowerOpen ABI: back chain at [sp], saved LR at [sp+8].
	uint32 sp = cpu->cur_gpr(1);
	bug("guest stack crawl (r1=%08x):\n", sp);
	for (int depth = 0; depth < 32; depth++) {
		if (!guest_addr_ok(sp, 12)) {
			bug("  [%2d] sp %08x (unmapped; stop)\n", depth, sp);
			break;
		}
		uint32 back = ReadMacInt32(sp);
		uint32 saved_lr = ReadMacInt32(sp + 8);
		bug("  [%2d] sp %08x back %08x lr %08x\n", depth, sp, back, saved_lr);
		if (back <= sp || back - sp > 0x100000)
			break;
		sp = back;
	}

	// Window around the kernel-data interrupt stack. The StarCraft dump showed
	// this region holding little-endian copies of its own guest addresses;
	// flag any word still matching that signature.
	const uint32 lo = (uint32)KERNEL_DATA_BASE - 0x200;
	const uint32 hi = (uint32)KERNEL_DATA_BASE + 0x40;
	bug("kernel-area dump [%08x..%08x) ('<' = LE self-address):\n", lo, hi);
	for (uint32 a = lo; a < hi; a += 16) {
		if (!guest_addr_ok(a, 16))
			continue;
		bug("  %08x:", a);
		for (int i = 0; i < 4; i++) {
			uint32 w = ReadMacInt32(a + i * 4);
			uint32 le = ((w & 0xff) << 24) | ((w & 0xff00) << 8) | ((w >> 8) & 0xff00) | (w >> 24);
			bug(" %08x%c", w, (le - (a + i * 4)) <= 0x100 ? '<' : ' ');
		}
		bug("\n");
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

	// Emulated device windows (see mmio.h). Their pages are deliberately kept
	// inaccessible so that every guest load and store lands here and is served
	// with real register semantics instead of being polled out of RAM.
	if (MMIOIsWindow((void *)addr))
		return SIGSEGV_RETURN_DEVICE_ACCESS;

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

#if PPC_REPORT_BAD_EA
	ppc_report_fault_trail();
#endif
	/* Through bug(), like every other report here: a GUI build has no stderr
	   and this dump was being written into nothing. */
	bug("SIGSEGV\n");
	bug("  pc %p\n", sigsegv_get_fault_instruction_address(sip));
	bug("  ea %p\n", sigsegv_get_fault_address(sip));
	dump_registers();
	dump_log();
	dump_crash_context(cpu);
	if (guest_addr_ok(pc - 8 * 4, (8 + 8 + 1) * 4))
		dump_disassembly(pc, 8, 8);
	else
		bug("  (pc %08x outside mapped guest areas; disassembly skipped)\n", pc);

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
#ifdef ENABLE_USB
				USBHIDVBL();
#endif /* ENABLE_USB */
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


// Combines 27 instructions, twice per Mixed Mode excursion.
void sheepshaver_cpu::execute_kernel_entry(uint32 lr_slot)
{
	const uint32 saved_sp = gpr(1);
	uint32 kd, ctx, v;

	ctr() = saved_sp;								// mtctr r1
	WriteMacInt32(XLM_IRQ_NEST, ReadMacInt32(XLM_IRQ_NEST) + 1);
	kd = ReadMacInt32(XLM_KERNEL_DATA);
	gpr(1) = kd;
	WriteMacInt32(kd + 0x18, gpr(6));
	gpr(6) = saved_sp;								// mfctr r6
	WriteMacInt32(kd + 0x04, gpr(6));
	gpr(6) = ReadMacInt32(kd + 0x65c);
	ctx = gpr(6);
	WriteMacInt32(ctx + 0x13c, gpr(7));
	WriteMacInt32(ctx + 0x144, gpr(8));
	WriteMacInt32(ctx + 0x14c, gpr(9));
	WriteMacInt32(ctx + 0x154, gpr(10));
	WriteMacInt32(ctx + 0x15c, gpr(11));
	WriteMacInt32(ctx + 0x164, gpr(12));
	WriteMacInt32(ctx + 0x16c, gpr(13));
	gpr(13) = get_cr();								// mfcr r13
	gpr(7) = ReadMacInt32(kd + 0x660);
	gpr(12) = lr();									// mflr r12
	// rlwimi. r7,r7,8,0,0
	v = gpr(7);
	gpr(7) = (v & 0x7fffffffu) | (((v << 8) | (v >> 24)) & 0x80000000u);
	record_cr0((int32)gpr(7));
	gpr(10) = ReadMacInt32(kd + lr_slot);
	lr() = gpr(10);									// mtlr r10
	gpr(10) = gpr(12);								// mr r10,r12
	gpr(11) = 0x0002f072u;
	// rlwimi r7,r7,27,26,26
	v = gpr(7);
	gpr(7) = (v & ~0x20u) | (((v << 27) | (v >> 5)) & 0x20u);
}

// Execute NATIVE_OP routine
void sheepshaver_cpu::execute_native_op(uint32 selector)
{
#if EMUL_TIME_STATS
	native_exec_count++;
	const clock_t native_exec_start = clock();
#endif

	switch (selector) {
	case NATIVE_KERNEL_ENTRY_68K:	execute_kernel_entry(0x5f0);	break;
	case NATIVE_KERNEL_ENTRY_MIXED:	execute_kernel_entry(0x5f4);	break;
	case NATIVE_KERNEL_ENTRY_RESET:	execute_kernel_entry(0x5f8);	break;
	case NATIVE_KERNEL_ENTRY_FE0A:	execute_kernel_entry(0x5fc);	break;
	case NATIVE_KERNEL_ENTRY_FE0F:	execute_kernel_entry(0x604);	break;
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
#ifdef ENABLE_USB
	case NATIVE_USB_PUBLISH_NODE:
		DoPublishUSBNode();
		break;
	case NATIVE_USB_UIM_POLL:
		USBUIMPoll();
		break;
	case NATIVE_USB_UIM_COMPLETE:
		USBUIMDeliverCompletions();
		break;
	case NATIVE_USB_EXPORT_HOOK:
		gpr(3) = USBUIMExportHook(pc(), gpr(3), gpr(4), gpr(5), gpr(6),
			gpr(7), gpr(8));
		break;
	case NATIVE_USB_EXPERT_NOTIFY: {
		// Chains into the Expert, so the same non-volatile registers have to
		// survive as for NATIVE_USB_UIM_DISPATCH above.
		uint32 saved[20];
		int i;
		saved[0] = gpr(1);
		for (i = 13; i < 32; i++)
			saved[i - 12] = gpr(i);
		gpr(3) = USBUIMExpertNotify(gpr(3));
		gpr(1) = saved[0];
		for (i = 13; i < 32; i++)
			gpr(i) = saved[i - 12];
		break;
	}
	case NATIVE_USB_UIM_DISPATCH: {
		// One entry for all 25 plugin slots; the stub put the slot in r11.
		// r11 = slot, r12 = the fragment's own TOC, which for this PEF is the
		// address of ThePluginDispatchTable itself.
		//
		// A UIM entry completes root hub transfers by calling straight back
		// into the guest, and execute_ppc() runs a nested call on this same
		// register file - it saves LR and nothing else. A plugin entry is an
		// ordinary PowerPC function call, so the non-volatile registers have to
		// come back untouched: USBServicesLib holds the function address in r28
		// across all three stages of a control transfer (USL code 0x4a7c and
		// 0x4b10 both pass it from there), and losing it sent the data stage of
		// every control transfer to address 0.
		uint32 saved[20];
		int i;
		saved[0] = gpr(1);
		for (i = 13; i < 32; i++)
			saved[i - 12] = gpr(i);
		gpr(3) = USBUIMDispatch(gpr(11), gpr(12), lr(), gpr(3), gpr(4), gpr(5),
			gpr(6), gpr(7), gpr(8), gpr(9), gpr(10));
		gpr(1) = saved[0];
		for (i = 13; i < 32; i++)
			gpr(i) = saved[i - 12];
		break;
	}
#endif /* #ifdef ENABLE_USB */
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

uint32 GuestNestedCalls = 0;

void Execute68k(uint32 pc, M68kRegisters *r)
{
	GuestNestedCalls++;
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
