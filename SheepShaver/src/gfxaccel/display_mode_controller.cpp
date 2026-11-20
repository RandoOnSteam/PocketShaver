/*
 *  display_mode_controller.cpp - Authoritative owner of display-mode state
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Sole writer of display-mode state. A state machine (Quiescent /
 *  QuickDrawOwner / ThreeDOwner / Transitioning / Blanking) accepts legal
 *  transitions and rejects the rest with a kDMCErr* code.
 *
 *  Each transition fires on_mode_exit in registration order before the
 *  snapshot swap, then on_mode_enter in reverse order after it. If an enter
 *  rejects, the outgoing snapshot is restored and kDMCErrSubscriberRejected
 *  returned. Late subscribers get a synthetic enter with the current snapshot.
 *
 *  Concurrency: writes are emul-thread only and serialized by s_write_mutex.
 *  Readers load s_current lock-free (acquire). Snapshots are immutable once
 *  published; a two-slot retirement ring keeps a reader's pointer valid for
 *  one more generation. A thread_local guard makes re-entrant calls from
 *  subscriber callbacks fail with kDMCErrReentrantRequest before locking.
 *
 *  Does not touch emulated Mac memory.
 */

#include "sysdeps.h"
#include "display_mode_controller.h"
#include "gfx_color_policy.h"

#include <cstring>
#include <cstdlib>
#include <vector>

// Concurrency primitives
#include "atomic.h"
#include <cassert>
#if defined(_WIN32)
#include <windows.h>
typedef uintptr_t dmc_thread_id_t;
static inline dmc_thread_id_t dmc_thread_self(void) { return (dmc_thread_id_t)GetCurrentThreadId(); }
// SRWLOCK and pthread_mutex_t both have a static initialiser, so the writer
// mutex needs no construction pass and no init-order guard.
typedef SRWLOCK dmc_mutex_t;
#define DMC_MUTEX_INIT      SRWLOCK_INIT
#define DMC_MUTEX_LOCK(m)   AcquireSRWLockExclusive(&(m))
#define DMC_MUTEX_UNLOCK(m) ReleaseSRWLockExclusive(&(m))
#define DMC_THREAD_LOCAL    __declspec(thread)
#else
#include <pthread.h>
typedef pthread_t dmc_thread_id_t;
static inline dmc_thread_id_t dmc_thread_self(void) { return pthread_self(); }
typedef pthread_mutex_t dmc_mutex_t;
#define DMC_MUTEX_INIT      PTHREAD_MUTEX_INITIALIZER
#define DMC_MUTEX_LOCK(m)   pthread_mutex_lock(&(m))
#define DMC_MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#define DMC_THREAD_LOCAL    __thread
#endif

#define DEBUG 0
#include "debug.h"

// Internal logging macros - promote to DMC_ERR for the rollback path
// so subscriber-rejected transitions are visible even in release builds.
#include "gfx_log.h"
#define DMC_LOG(fmt, ...) D(bug("[DMC] " fmt "\n", ##__VA_ARGS__))
#define DMC_ERR(...) GFX_DEBUG_EMIT("[DMC ERROR] ", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Module-local state (writer-mutex serialized; atomic snapshot
// publication; 2-generation retirement ring; re-entry guard; thread-identity
// assertion).
//
// s_current is ATOMIC (lock-free read path for the VBL-tick thread). All
// OTHER state (s_state, s_subscribers, s_next_generation, s_dmc_initialized,
// s_retired[]) is protected by s_write_mutex. Writers always hold the mutex
// when mutating any of the non-atomic state AND when publishing to s_current;
// readers ONLY load s_current and never touch the rest.
// ---------------------------------------------------------------------------
static dmc_mutex_t                            s_write_mutex = DMC_MUTEX_INIT;
static const DMCModeSnapshot * volatile       s_current = NULL;

// Scoped writer lock; the sole replacement for std::lock_guard<std::mutex>.
class DMCLock {
	dmc_mutex_t &m;
	DMCLock(const DMCLock &);
	DMCLock &operator=(const DMCLock &);
public:
	DMCLock(dmc_mutex_t &mutex) : m(mutex) { DMC_MUTEX_LOCK(m); }
	~DMCLock() { DMC_MUTEX_UNLOCK(m); }
};
static DMCState                               s_state = kDMCStateQuiescent;
static uint32_t                               s_next_generation = 1;
static bool                                   s_dmc_initialized = false;
/* A platform mode switch is a two-phase transaction. prepare fires resource
 * detach while the outgoing GL context is still alive; the video driver then
 * replaces its surface/context; request_mode_switch commits the bound surface
 * and reattaches resources. The current snapshot remains the outgoing stable
 * snapshot between the two calls so lock-free readers never see half-bound
 * geometry. */
static bool                                   s_mode_switch_prepared = false;
static DMCState                               s_mode_switch_source_state =
												kDMCStateQuiescent;
static DMCModeDesc                            s_mode_switch_requested = {};

// Registration order. Exit walks forward, enter walks backward.
static std::vector<DMCSubscriber>             s_subscribers;

// Retirement ring, indexed by (generation - 1) % 2. Publishing generation N
// frees N-2, which no reader can still hold.
static const DMCModeSnapshot *                s_retired[2] = { NULL, NULL };

// Re-entry guard (subscriber callback re-enters a dmc_*
// write API). Thread-local so a cross-thread writer doesn't see the flag
// set by a different thread's outer call (defense-in-depth; all writers are
// on the emul thread today).
static DMC_THREAD_LOCAL bool                  s_in_dmc_call = false;


// Thread-identity baseline (assertion gate). Captured on the first
// dmc_create call; subsequent writes from any other thread trigger
// assert(0).
static dmc_thread_id_t                        s_emul_thread = 0;

// Create at the top of every write API, before locking. If `installed` is
// false the call is re-entrant: return kDMCErrReentrantRequest without
// taking the (non-recursive) mutex.
struct DMCReentryScope {
	bool installed;
	DMCReentryScope() {
		if (s_in_dmc_call) {
			installed = false;
		} else {
			s_in_dmc_call = true;
			installed = true;
		}
	}
	~DMCReentryScope() {
		if (installed) {
			s_in_dmc_call = false;
		}
	}
};

#define DMC_ASSERT_EMUL_THREAD() \
	do { \
		if (s_emul_thread != 0 && dmc_thread_self() != s_emul_thread) { \
			DMC_ERR("dmc_* called from non-emul thread (got %p, expected %p)", \
					(void *)(uintptr_t)dmc_thread_self(), (void *)(uintptr_t)s_emul_thread); \
			assert(0 && "DMC write from wrong thread"); \
		} \
	} while (0)

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int32_t dmc_validate_mode_desc(const DMCModeDesc *m) {
	if (m == NULL) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->width == 0 || m->width > 4096) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->height == 0 || m->height > 4096) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->depth != 1 && m->depth != 2 && m->depth != 4 &&
		m->depth != 8 && m->depth != 16 && m->depth != 32) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->row_bytes == 0) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->pitch < m->row_bytes) {
		return kDMCErrInvalidModeDesc;
	}
	return kDMCNoErr;
}

static bool dmc_same_mode_geometry(const DMCModeDesc *a,
								   const DMCModeDesc *b)
{
	return a != NULL && b != NULL &&
		a->width == b->width &&
		a->height == b->height &&
		a->depth == b->depth &&
		a->row_bytes == b->row_bytes &&
		a->pitch == b->pitch;
}

// Internal allocation wrappers that funnel the 4 direct calloc/malloc sites
// in this file through a single pair of helpers. Callers MUST hold
// s_write_mutex when invoking these (every call site is already inside the
// writer mutex by construction).
static DMCModeSnapshot *dmc_alloc_zeroed_snapshot(void) {
	return (DMCModeSnapshot *)calloc(1, sizeof(DMCModeSnapshot));
}

static DMCModeSnapshot *dmc_alloc_raw_snapshot(void) {
	return (DMCModeSnapshot *)malloc(sizeof(DMCModeSnapshot));
}

// Initialize gamma_lut[768] to the Mac-side identity ramp (input == output).
// The compositor composes the display-space 1.8->2.2 policy when it uploads
// this snapshot into its GPU-visible LUT.
static void dmc_init_identity_gamma(DMCModeSnapshot *snap) {
	GfxColorFillIdentityGammaLUT(snap->gamma_lut);
	GfxColorFillIdentityGammaLUT(snap->driver_gamma_lut);
}

// palette_gen and gamma_gen start at 0 unless the caller copies them from
// the outgoing snapshot. Published snapshots always have transitioning == 0;
// the transition itself is tracked in s_state.
static DMCModeSnapshot *dmc_alloc_snapshot_from_desc(const DMCModeDesc *m,
													 uint32_t generation,
													 uint32_t active_owner,
													 const uint8_t blanking_rgba[4]) {
	DMCModeSnapshot *s = dmc_alloc_zeroed_snapshot();
	if (s == NULL) {
		return NULL;
	}
	s->generation       = generation;
	s->layout_gen       = generation;
	s->transitioning    = 0;
	s->width            = m->width;
	s->height           = m->height;
	s->depth            = m->depth;
	s->row_bytes        = m->row_bytes;
	s->pitch            = m->pitch;
	s->palette_gen      = 0;
	s->gamma_gen        = 0;
	s->fade_active      = 0;
	s->vbl_usec         = m->vbl_usec;
	s->active_owner     = active_owner;
	if (blanking_rgba != NULL) {
		s->blanking_rgba[0] = blanking_rgba[0];
		s->blanking_rgba[1] = blanking_rgba[1];
		s->blanking_rgba[2] = blanking_rgba[2];
		s->blanking_rgba[3] = blanking_rgba[3];
	} else {
		s->blanking_rgba[0] = 0;
		s->blanking_rgba[1] = 0;
		s->blanking_rgba[2] = 0;
		s->blanking_rgba[3] = 0xFF;
	}
	s->screen_base_mac  = m->screen_base_mac;
	s->screen_base_host = m->screen_base_host;
	dmc_init_identity_gamma(s);
	return s;
}

// Clone with a new owner and optionally new blanking_rgba. No revalidation:
// the fields were validated when first committed.
static DMCModeSnapshot *dmc_clone_snapshot_with_owner(const DMCModeSnapshot *src,
													  uint32_t generation,
													  uint32_t active_owner,
													  const uint8_t blanking_rgba[4]) {
	DMCModeSnapshot *s = dmc_alloc_zeroed_snapshot();
	if (s == NULL) {
		return NULL;
	}
	*s = *src;
	s->generation       = generation;
	s->layout_gen       = generation;
	s->transitioning    = 0;
	s->active_owner     = active_owner;
	if (blanking_rgba != NULL) {
		s->blanking_rgba[0] = blanking_rgba[0];
		s->blanking_rgba[1] = blanking_rgba[1];
		s->blanking_rgba[2] = blanking_rgba[2];
		s->blanking_rgba[3] = blanking_rgba[3];
	}
	return s;
}

// Blanking / Quiescent owners aren't valid here; callers handle them.
static DMCState dmc_target_state_for_owner(uint32_t owner) {
	if (owner == kDMCOwnerRAVE || owner == kDMCOwnerGL ||
		owner == kDMCOwnerDSp || owner == kDMCOwnerGlide) {
		return kDMCStateThreeDOwner;
	}
	return kDMCStateQuickDrawOwner;
}

// `this_gen` is the generation just published. Frees the snapshot retired
// two publishes ago. Caller holds s_write_mutex.
static void dmc_retire_snapshot(const DMCModeSnapshot *old_snap, uint32_t this_gen) {
	if (old_snap == NULL) {
		return;
	}
	int slot = (int)((this_gen - 1) % 2);
	if (s_retired[slot] != NULL) {
		free((void *)s_retired[slot]);
		s_retired[slot] = NULL;
	}
	s_retired[slot] = old_snap;
}

// Subscriber dispatch, called with s_write_mutex held. Exit runs in
// registration order and can't veto. Enter runs in reverse order; a non-zero
// return stops dispatch and the caller rolls back.

static void dmc_internal_fire_exit_events(const DMCModeSnapshot *outgoing) {
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].on_mode_exit != NULL) {
			int32_t r = s_subscribers[i].on_mode_exit(outgoing, s_subscribers[i].ctx);
			if (r != kDMCNoErr) {
				DMC_LOG("subscriber %s on_mode_exit returned %d (advisory; not vetoable)",
						s_subscribers[i].name != NULL ? s_subscribers[i].name : "?", (int)r);
			}
		}
	}
}

static int32_t dmc_internal_fire_enter_events(const DMCModeSnapshot *incoming) {
	// Use a raw reverse index
	// (size - 1 - idx) instead of a filtered dispatch_pos counter so
	// enter's dispatch_index semantics mirror exit's raw forward index.
	// Before: one-sided-callback subscribers produced ambiguous enter
	// indices (filtered counter skipped NULL on_mode_enter). After: every
	// dispatch_index reflects the subscriber's position in the registration
	// table regardless of callback presence. Exit's `(uint32_t)i` stays
	// untouched - both paths now converge on raw subscriber index.
	for (size_t i = s_subscribers.size(); i > 0; --i) {
		size_t idx = i - 1;  // walk in reverse without size_t underflow
		if (s_subscribers[idx].on_mode_enter != NULL) {
			int32_t r = s_subscribers[idx].on_mode_enter(incoming, s_subscribers[idx].ctx);
			if (r != kDMCNoErr) {
				DMC_ERR("subscriber %s on_mode_enter returned %d - initiating rollback",
						s_subscribers[idx].name != NULL ? s_subscribers[idx].name : "?", (int)r);
				return kDMCErrSubscriberRejected;
			}
		}
	}
	return kDMCNoErr;
}

static void dmc_internal_fire_enter_events_advisory(
	const DMCModeSnapshot *incoming,
	const char *reason)
{
	if (incoming == NULL) return;
	for (size_t i = s_subscribers.size(); i > 0; --i) {
		size_t idx = i - 1;
		if (s_subscribers[idx].on_mode_enter != NULL) {
			int32_t r = s_subscribers[idx].on_mode_enter(incoming,
														 s_subscribers[idx].ctx);
			if (r != kDMCNoErr) {
				DMC_ERR("subscriber %s on_mode_enter returned %d during "
						"%s - rollback compensation continues",
						s_subscribers[idx].name != NULL ? s_subscribers[idx].name : "?",
						(int)r, reason != NULL ? reason : "rollback compensation");
			}
		}
	}
}

static void dmc_internal_compensate_rejected_transition(
	const DMCModeSnapshot *rejected,
	const DMCModeSnapshot *restored,
	DMCState restored_state,
	const char *reason)
{
	if (rejected != NULL) {
		dmc_internal_fire_exit_events(rejected);
	}
	atomic_store_explicit(&s_current, restored, memory_order_release);
	s_state = restored_state;
	dmc_internal_fire_enter_events_advisory(restored, reason);
}

// ---------------------------------------------------------------------------
// Public API
//
// Every public write entry point follows the same preamble:
//   1. DMCReentryScope reentry;
//      if (!reentry.installed) return kDMCErrReentrantRequest;
//   2. DMCLock guard(s_write_mutex);
//   3. DMC_ASSERT_EMUL_THREAD();
//   4. FSM / validation checks.
//   5. Allocate incoming snapshot, fire exits, publish (atomic release),
//      fire enters, retire outgoing (or compensate + rollback on enter veto).
//
// dmc_current_snapshot is the ONLY read path; it is lock-free and callable
// from any thread (acquire-load on s_current).
// ---------------------------------------------------------------------------

int32_t dmc_create(const struct DMCModeDesc *initial_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	// (No DMC_ASSERT_EMUL_THREAD here: production captures s_emul_thread
	// on the FIRST dmc_create call. Subsequent writes assert.)

	if (s_dmc_initialized) {
		return kDMCErrAlreadyInitialized;
	}
	int32_t verr = dmc_validate_mode_desc(initial_mode);
	if (verr != kDMCNoErr) {
		return verr;
	}

	if (s_emul_thread == 0) {
		s_emul_thread = dmc_thread_self();
	}

	const uint8_t default_blanking[4] = { 0x00, 0x00, 0x00, 0xFF };
	DMCModeSnapshot *snap = dmc_alloc_snapshot_from_desc(initial_mode,
														 s_next_generation,
														 (uint32_t)kDMCOwnerQuickDraw,
														 default_blanking);
	if (snap == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;

	// T1 has no outgoing snapshot (source state is Quiescent), so there is
	// nothing to exit.
	atomic_store_explicit(&s_current, snap, memory_order_release);
	s_state = kDMCStateQuickDrawOwner;
	s_dmc_initialized = true;

	// No subscribers can exist yet; this is for safety.
	int32_t enter_err = dmc_internal_fire_enter_events(snap);
	if (enter_err != kDMCNoErr) {
		// No prior snapshot, so roll back to Quiescent.
		dmc_internal_compensate_rejected_transition(
			snap, NULL, kDMCStateQuiescent, "dmc_create enter veto");
		free(snap);
		s_dmc_initialized = false;
		s_next_generation--;
		return kDMCErrSubscriberRejected;
	}
	return kDMCNoErr;
}

int32_t dmc_shutdown(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	// Fire exit on the current snapshot for every subscriber in
	// registration order. There is NO incoming snapshot in Quiescent, so
	// we do not fire enter (reverse enter is conditional on an incoming
	// snapshot).
	const DMCModeSnapshot *cur = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (cur != NULL) {
		dmc_internal_fire_exit_events(cur);
	}
	atomic_store_explicit(&s_current, (const DMCModeSnapshot *)NULL,
	                      memory_order_release);
	s_state = kDMCStateQuiescent;
	s_dmc_initialized = false;
	s_mode_switch_prepared = false;
	s_mode_switch_source_state = kDMCStateQuiescent;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));

	// No readers remain after shutdown, so free everything.
	if (cur != NULL) {
		free((void *)cur);
	}
	for (int i = 0; i < 2; ++i) {
		if (s_retired[i] != NULL) {
			free((void *)s_retired[i]);
			s_retired[i] = NULL;
		}
	}
	s_subscribers.clear();
	return kDMCNoErr;
}

int32_t dmc_subscribe(const struct DMCSubscriber *sub) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (sub == NULL || sub->name == NULL) {
		return kDMCErrSubscriberRejected;
	}
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].name != NULL &&
			strcmp(s_subscribers[i].name, sub->name) == 0) {
			return kDMCErrSubscriberAlreadyRegistered;
		}
	}
	DMCSubscriber copy = *sub;
	s_subscribers.push_back(copy);

	// Catchup synthesized on_mode_enter for mid-transition subscriber
	// ("subscribe always leaves you in a consistent state"). The new
	// subscriber observes the
	// current snapshot as its enter event. Return value is IGNORED here:
	// a mid-init subscriber cannot veto an already-committed state - it
	// must deal with whatever mode is currently published or fail in its
	// own init path.
	const DMCModeSnapshot *cur = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (cur != NULL && copy.on_mode_enter != NULL) {
		(void)copy.on_mode_enter(cur, copy.ctx);
	}
	return kDMCNoErr;
}

int32_t dmc_unsubscribe(const char *name) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (name == NULL) {
		return kDMCErrSubscriberNotFound;
	}
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].name != NULL &&
			strcmp(s_subscribers[i].name, name) == 0) {
			s_subscribers.erase(s_subscribers.begin() + i);
			return kDMCNoErr;
		}
	}
	return kDMCErrSubscriberNotFound;
}

// Lock-free, any thread. The pointer stays valid for at least one more
// publish (retirement ring).
const struct DMCModeSnapshot *dmc_current_snapshot(void) {
	return atomic_load_explicit(&s_current, memory_order_acquire);
}

// True while a 3D or DrawSprocket context owns the screen. SynchIdleTime
// (emul_op.cpp) caps its host sleep then, so per-frame and per-read guest
// yields do not each wait for the next 60Hz tick.
extern "C" int gfx_idle_latency_sensitive(void) {
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	if (!snap) return 0;
	return snap->active_owner == kDMCOwnerRAVE ||
	       snap->active_owner == kDMCOwnerGL ||
	       snap->active_owner == kDMCOwnerDSp;
}

int32_t dmc_prepare_mode_switch(const struct DMCModeDesc *new_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state == kDMCStateTransitioning || s_mode_switch_prepared) {
		return kDMCErrTransitionInProgress;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	int32_t verr = dmc_validate_mode_desc(new_mode);
	if (verr != kDMCNoErr) {
		return verr;
	}

	const DMCModeSnapshot *outgoing =
		atomic_load_explicit(&s_current, memory_order_relaxed);
	s_mode_switch_source_state = s_state;
	s_mode_switch_requested = *new_mode;
	s_mode_switch_prepared = true;
	s_state = kDMCStateTransitioning;

	if (outgoing != NULL) {
		dmc_internal_fire_exit_events(outgoing);
	}
	return kDMCNoErr;
}

int32_t dmc_cancel_prepared_mode_switch(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (!s_mode_switch_prepared ||
		s_state != kDMCStateTransitioning) {
		return kDMCErrIllegalTransition;
	}

	const DMCModeSnapshot *outgoing =
		atomic_load_explicit(&s_current, memory_order_relaxed);
	s_state = s_mode_switch_source_state;
	s_mode_switch_prepared = false;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));
	dmc_internal_fire_enter_events_advisory(
		outgoing, "dmc_cancel_prepared_mode_switch");
	return kDMCNoErr;
}

int32_t dmc_request_mode_switch(const struct DMCModeDesc *new_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const bool prepared = s_mode_switch_prepared &&
		s_state == kDMCStateTransitioning;
	if (s_state == kDMCStateTransitioning && !prepared) {
		return kDMCErrTransitionInProgress;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	int32_t verr = dmc_validate_mode_desc(new_mode);
	if (verr == kDMCNoErr && prepared &&
		!dmc_same_mode_geometry(&s_mode_switch_requested, new_mode)) {
		verr = kDMCErrInvalidModeDesc;
	}
	if (verr != kDMCNoErr) {
		if (prepared) {
			const DMCModeSnapshot *outgoing =
				atomic_load_explicit(&s_current, memory_order_relaxed);
			s_state = s_mode_switch_source_state;
			s_mode_switch_prepared = false;
			memset(&s_mode_switch_requested, 0,
				   sizeof(s_mode_switch_requested));
			dmc_internal_fire_enter_events_advisory(
				outgoing, "dmc_request_mode_switch invalid prepared commit");
		}
		return verr;
	}

	// Read current snapshot via relaxed (mutex is held; readers only see
	// release-published values).
	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);
	DMCState         src_state = prepared
		? s_mode_switch_source_state : s_state;
	uint32_t prior_owner = (outgoing != NULL) ? outgoing->active_owner : (uint32_t)kDMCOwnerQuickDraw;
	uint32_t new_owner;
	if (prior_owner == kDMCOwnerRAVE ||
		prior_owner == kDMCOwnerGL ||
		prior_owner == kDMCOwnerDSp ||
		prior_owner == kDMCOwnerGlide) {
		new_owner = prior_owner;
	} else {
		new_owner = (uint32_t)kDMCOwnerQuickDraw;
	}
	// Carry these across the switch. fade_active matters because DSp games
	// can FadeGammaOut across a resolution change.
	uint32_t carry_palette_gen = (outgoing != NULL) ? outgoing->palette_gen : 0;
	uint32_t carry_gamma_gen   = (outgoing != NULL) ? outgoing->gamma_gen   : 0;
	// A mid-fade mode switch must NOT drop the fade flag - a
	// DSp game can FadeGammaOut across a resolution change.
	uint32_t carry_fade_active = (outgoing != NULL) ? outgoing->fade_active : 0;
	const uint8_t carry_blanking[4] = {
		(outgoing != NULL) ? outgoing->blanking_rgba[0] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[1] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[2] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[3] : (uint8_t)0xFF
	};

	// Enter Transitioning state. We no longer mutate outgoing in
	// place (snapshots are immutable once published); the transition is
	// observable via s_state. Readers that cared about the former
	// snapshot.transitioning sentinel should read s_state or simply accept
	// a sub-microsecond window during which s_current still points at the
	// stable `outgoing`.
	if (!prepared) {
		s_state = kDMCStateTransitioning;

		// Legacy single-phase observer: detach immediately before publishing.
		// The canonical video driver uses prepare/commit so this callback runs
		// while the old platform context is still alive.
		if (outgoing != NULL) {
			dmc_internal_fire_exit_events(outgoing);
		}
	}

	DMCModeSnapshot *incoming = dmc_alloc_snapshot_from_desc(new_mode,
															 s_next_generation,
															 new_owner,
															 carry_blanking);
	if (incoming == NULL) {
		// Exits already ran, so compensate with an advisory enter.
		s_state = src_state;
		s_mode_switch_prepared = false;
		memset(&s_mode_switch_requested, 0,
			   sizeof(s_mode_switch_requested));
		dmc_internal_fire_enter_events_advisory(
			outgoing, "dmc_request_mode_switch allocation rollback");
		return kDMCErrOutOfMemory;
	}
	incoming->palette_gen = carry_palette_gen;
	incoming->gamma_gen   = carry_gamma_gen;
	incoming->fade_active = carry_fade_active;
	// Replace the default gamma with the outgoing LUTs.
	if (outgoing != NULL) {
		memcpy(incoming->gamma_lut, outgoing->gamma_lut, 768);
		memcpy(incoming->driver_gamma_lut, outgoing->driver_gamma_lut, 768);
	}
	s_next_generation++;

	// Publish (atomic release-store).
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	DMCState target = dmc_target_state_for_owner(new_owner);
	s_state = target;
	s_mode_switch_prepared = false;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));

	int32_t enter_err = dmc_internal_fire_enter_events(incoming);
	if (enter_err != kDMCNoErr) {
		dmc_internal_compensate_rejected_transition(
			incoming, outgoing, src_state,
			"dmc_request_mode_switch enter veto");
		// Route the rejected snapshot through
		// the retirement ring so concurrent readers get one more publish of
		// grace before it is freed. incoming->generation + 1 is the ring slot
		// the NEXT successful publish would have used; concurrent readers
		// holding `incoming` finish their frame before this slot is
		// overwritten (at gen + 3 publish). The rejected generation stays
		// consumed - we do NOT decrement s_next_generation because
		// the ring-slot calculation relies on the bumped counter.
		dmc_retire_snapshot(incoming, incoming->generation + 1);
		return kDMCErrSubscriberRejected;
	}

	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_set_active_owner(uint32_t owner) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	// Out-of-enum-range check. kDMCOwnerGlide is the highest legal owner
	// that set_active_owner may request. Blanking / Quiescent are reached
	// only via dmc_request_blanking / dmc_shutdown.
	if (owner > (uint32_t)kDMCOwnerGlide) {
		return kDMCErrInvalidOwner;
	}
	if (owner == (uint32_t)kDMCOwnerBlanking ||
		owner == (uint32_t)kDMCOwnerQuiescent) {
		return kDMCErrInvalidOwner;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	if (s_state == kDMCStateTransitioning) {
		return kDMCErrTransitionInProgress;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);
	// Idempotent early-return when the requested
	// owner + the current FSM state already match. Once engines
	// start vending overlays per frame (which drives a dmc_set_active_owner call
	// per frame), this prevents 60 Hz calloc + subscriber-dispatch churn. Both
	// owner AND state must match - a state transition (e.g. back from Blanking)
	// still requires a real transition.
	if (outgoing != NULL &&
		outgoing->active_owner == owner &&
		s_state == dmc_target_state_for_owner(owner)) {
		return kDMCNoErr;
	}
	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  owner,
															  NULL);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	DMCState target = dmc_target_state_for_owner(owner);
	s_state = target;

	/* active_owner identifies the most recent screen producer, not a resource
	 * lifetime. No display surface or GL/Metal context changed here, so firing
	 * mode callbacks would destroy the texture the producer is about to
	 * submit and then re-vend an unrelated replacement under the same frame. */
	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_request_blanking(const uint8_t rgba[4]) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrBlankingAlreadyActive;
	}
	if (s_state == kDMCStateTransitioning) {
		return kDMCErrTransitionInProgress;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);

	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  (uint32_t)kDMCOwnerBlanking,
															  rgba);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	s_state = kDMCStateBlanking;

	/* Blanking changes visible policy, not framebuffer identity. Preserve all
	 * engine allocations so a fade cannot invalidate the frame it is fading. */
	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_end_blanking(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state != kDMCStateBlanking) {
		return kDMCErrNotBlanking;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);

	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  (uint32_t)kDMCOwnerQuickDraw,
															  NULL);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	s_state = kDMCStateQuickDrawOwner;

	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

// Field-only updates (palette, gamma, blanking color) still publish a fresh
// snapshot with a new generation, keeping the retirement ring consistent.
// They fire no subscriber events; listeners compare palette_gen / gamma_gen.
int32_t dmc_record_palette_change(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation  = s_next_generation++;
	fresh->palette_gen = old->palette_gen + 1;

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}

// Companion to
// dmc_record_gamma_change_with_lut that ALSO publishes a fade_active flag in
// the SAME snapshot bump. Clone-mutate-publish under the EXISTING s_write_mutex
// (no NEW MTLFence / MTLSharedEvent / mutex / @synchronized / atomic - the
// flag rides the existing atomic-release publish). Publishing fade_active
// and the interpolated LUT together avoids a frame where the LUT is mid-fade
// but the flag is stale, which would warp one fade frame: a separate
// fade_active setter would produce exactly that torn frame.
int32_t dmc_record_gamma_change_with_lut_fade(const uint8_t *lut, int fade_active) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation  = s_next_generation++;
	fresh->gamma_gen   = old->gamma_gen + 1;
	fresh->fade_active = (uint32_t)(fade_active ? 1 : 0);

	if (lut != NULL) {
		memcpy(fresh->gamma_lut, lut, 768);
	}

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}

// lut is planar 768 bytes (256 R, G, B); NULL keeps the current LUT. Not a
// fade, so fade_active is cleared.
int32_t dmc_record_gamma_change_with_lut(const uint8_t *lut) {
	return dmc_record_gamma_change_with_lut_fade(lut, 0);
}

// Record a DRIVER (guest SetGamma) table: always store it in
// driver_gamma_lut - the "original intensity" the DSp fades blend toward -
// and apply it to the displayed gamma_lut only when no fade is in progress.
// During a fade the displayed LUT is left alone (an immediate apply would
// visibly pop the faded screen); the fade's end-state push delivers the
// table instead. Same clone-mutate-publish discipline as the other
// dmc_record_* writers (existing s_write_mutex; no new primitives).
int32_t dmc_record_driver_gamma_change(const uint8_t *lut) {
	if (lut == NULL) {
		return kDMCErrInvalidModeDesc;
	}
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation = s_next_generation++;
	memcpy(fresh->driver_gamma_lut, lut, 768);

	const bool apply_now = (old->fade_active == 0);
	if (apply_now) {
		fresh->gamma_gen = old->gamma_gen + 1;
		memcpy(fresh->gamma_lut, lut, 768);
	}

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return apply_now ? kDMCNoErr : kDMCDriverGammaDeferred;
}

// Bump gamma_gen without changing the LUT.
int32_t dmc_record_gamma_change(void) {
	return dmc_record_gamma_change_with_lut(NULL);
}

// Assign the snapshot's blanking color WITHOUT
// entering the Blanking FSM state. DSpSetBlankingColor (sub-op 760) only sets
// the color the library will use the next time the screen IS blanked - it does
// NOT blank now (DSp 1.7 PDF p.30). This is the no-state-transition twin of
// dmc_record_gamma_change_with_lut: clone the current snapshot, mutate the one
// field (blanking_rgba), bump generation, publish, retire old. It deliberately
// does NOT call any blanking-enter path (dmc_request_blanking transitions the
// FSM to Blanking - wrong for SetBlankingColor).
//
// Reuses the EXISTING DMC single-writer primitive (s_write_mutex +
// DMCReentryScope + DMC_ASSERT_EMUL_THREAD); adds ZERO new MTLFence /
// MTLSharedEvent / mutex / @synchronized / atomic. The DMC is the
// documented single-writer exception, so this is compliant.
//
// No subscriber events are fired - this is a pure snapshot field update, not an
// FSM transition (same posture as dmc_record_palette_change / gamma_change).
int32_t dmc_set_blanking_color(const uint8_t rgba[4]) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (rgba == NULL) {
		return kDMCErrInvalidModeDesc;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation = s_next_generation++;
	memcpy(fresh->blanking_rgba, rgba, 4);

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}
