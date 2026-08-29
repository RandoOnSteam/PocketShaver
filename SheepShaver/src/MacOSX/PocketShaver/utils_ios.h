/*
 *	utils_ios.h - iOS utility functions.
 *
 *  Copyright (C) 2011 Alexei Svitkine
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
 
 Additional code by Tom Padula 2022.

 */

#ifndef UTILS_IOS_H
#define UTILS_IOS_H

// Invokes the specified function with an NSAutoReleasePool in place.
void NSAutoReleasePool_wrap(void (*fn)(void));

#if USE_SDL2
#include <SDL2/SDL.h>
#include "SDL2/SDL_version.h"
#elif USE_SDL3
#include <SDL3/SDL.h>
#include "SDL3/SDL_version.h"
#endif
#if USE_SDL2 || USE_SDL3
#if SDL_VERSION_ATLEAST(2,0,0)
void disable_SDL2_macosx_menu_bar_keyboard_shortcuts();
bool is_fullscreen_osx(SDL_Window * window);
#endif
#endif

void set_menu_bar_visible_osx(bool visible);

void set_current_directory();
const char* home_directory();
const char* document_directory();
// On the (unsandboxed) Mac Catalyst build we deliberately store all app-owned
// data under the app's container Data directory
// (~/Library/Containers/<bundle-id>/Data) rather than the user's visible home,
// to keep it out of casual sight and reduce accidental corruption. Returns that
// path, creating it if needed. Only defined on Catalyst.
const char* pocketshaver_home_directory();

// Moves a pre-existing ~/PocketShaver Home (from builds that stored data in the
// visible home) into the container Data directory exactly once. Idempotent and
// safe to call from multiple entry points; call as early as possible at launch,
// before any file access. Only defined on Catalyst.
void pocketshaver_migrate_home_if_needed();

bool MetalIsAvailable();

#ifdef __cplusplus
extern "C" {
#endif
void PocketShaverRunOnMainSync(void (*fn)(void *), void *arg);
void PocketShaverPresentCPUSurface(void *sdl_surface);
void PocketShaverPresentCPUFrame(const void *pixels, int w, int h, int pitch);
void VideoPresentFallback(void);
/* Native UIWindow backing the SDL window (UIWindow *), or NULL. */
void *PocketShaverGetSDLUIWindow(void);
/* Register the UIView that fills the host window (Metal overlay or SDL's
 * own content view). Pinning, letterbox colour, and present-rect tracking
 * all operate on this view. Pass NULL to clear. */
void PocketShaverInstallWindowContentView(void *uiview);
/* Re-insert and re-pin the registered content view after SDL swaps its
 * container (mode switch). Idempotent. */
void PocketShaverRehomeWindowContentView(void);
/* Use SDL's UIKit content view as the window-filling view (SDL-GPU path). */
void PocketShaverAdoptSDLWindowContentView(void);
/* Present-rect cache in window coordinates (same ABI video_sdl* already
 * calls through metal_compositor.h). */
void MetalCompositorRefreshPresentRect(void);
void MetalCompositorGetPresentRect(int *out_x, int *out_y, int *out_w, int *out_h);
void MetalCompositorReapplyWindowPinning(void);
double MetalCompositorWindowedContentInsetTop(void);
#ifdef __cplusplus
}
#endif

extern void set_relative_mouse_enabled();
extern void set_relative_mouse_disabled();
extern void toggle_relative_mouse();
extern void set_relative_mouse_automatic();
extern void report_relative_mouse_capability();
extern void setup_frame_rate();
extern void set_input_disabled(bool is_disabled);
extern void SyncHostPasteboardToPending(void);
extern void WritePendingGuestScrapToHostPasteboard(void);

#endif
