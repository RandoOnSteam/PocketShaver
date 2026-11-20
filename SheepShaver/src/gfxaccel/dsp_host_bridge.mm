/*
 *  dsp_host_bridge.mm - DSp <-> iOS host bridge implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Tells DSpIdleTimerService.swift when a DSp context goes Active fullscreen
 *  so it can disable the idle timer. The setter runs on the emul thread; the
 *  Swift observer handles the notification on the main queue.
 *
 *  The flag is also re-read on willEnterForeground, because a transition
 *  made while backgrounded would otherwise be missed.
 */

#import <Foundation/Foundation.h>

#import "dsp_host_bridge.h"

#include "atomic.h"

/* Module-scope storage - single writer emul-thread; readers: main-thread
 * Swift observer on foreground re-apply, plus the emul-thread
 * conditional-set read via the getter. Initial state = false (no DSp
 * context is Active at DSp boot time; DSpContext_Reserve's default
 * state is Inactive). */
static atomic_uint32 s_dsp_active_fullscreen = 0;

extern "C" void DSpHostBridge_SetActiveFullscreen(bool active)
{
	/* Write the flag first so readers (including the observer
	 * closure that the notification post will trigger) see the new value
	 * atomically. atomic_uint32 with memory_order_relaxed is the minimum
	 * sanctioned primitive - the Swift observer's queue: .main hop
	 * provides the happens-before for the subsequent UIApplication write
	 * (main-runloop iteration boundary is a synchronization point). */
	atomic_store_explicit(&s_dsp_active_fullscreen, active ? 1u : 0u,
	                      memory_order_relaxed);

	/* Post notification to DSpIdleTimerService. Uses the
	 * same name string the Swift observer registered for:
	 * Notification.Name("DSpHostBridge.activeFullscreenChanged"). The
	 * Swift observer's queue: .main hops delivery to main thread before
	 * writing UIApplication.shared.isIdleTimerDisabled.
	 *
	 * @autoreleasepool guards against any autoreleased objects the post
	 * might create internally (defensive - the NATIVE_DSP_DISPATCH emul
	 * thread does not have an enclosing autorelease pool). Negligible
	 * cost on a transition-edge-only call path. */
	@autoreleasepool {
		[[NSNotificationCenter defaultCenter]
		    postNotificationName:@"DSpHostBridge.activeFullscreenChanged"
		                  object:nil];
	}
}

extern "C" bool DSpHostBridge_GetActiveFullscreen(void)
{
	return atomic_load_explicit(&s_dsp_active_fullscreen,
	                            memory_order_relaxed) != 0;
}
