/*
 *	utils_ios.mm - iOS utility functions.
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

#include <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "sysdeps.h"
#include "my_sdl.h"
#include "utils_ios.h"
#include "atomic.h"

#if USE_SDL2
#include <SDL2/SDL_syswm.h>
#endif

#include <sys/sysctl.h>
#include <Metal/Metal.h>

// This is used from video_sdl.cpp.
void NSAutoReleasePool_wrap(void (*fn)(void))
{
//	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	fn();
//	[pool release];
}

#if SDL_VERSION_ATLEAST(2,0,0)

void disable_SDL2_macosx_menu_bar_keyboard_shortcuts() {
#if 0
	for (NSMenuItem * menu_item in [NSApp mainMenu].itemArray) {
		if (menu_item.hasSubmenu) {
			for (NSMenuItem * sub_item in menu_item.submenu.itemArray) {
				sub_item.keyEquivalent = @"";
				sub_item.keyEquivalentModifierMask = 0;
			}
		}
		if ([menu_item.title isEqualToString:@"View"]) {
			[[NSApp mainMenu] removeItem:menu_item];
			break;
		}
	}
#endif
	
}

bool is_fullscreen_osx(SDL_Window * window)
{
	return false;
#if 0
	if (!window) {
		return false;
	}
	
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
		return false;
	}

	const NSWindowStyleMask styleMask = [wmInfo.info.cocoa.window styleMask];
	return (styleMask & NSWindowStyleMaskFullScreen) != 0;
#endif
}
#endif

void set_menu_bar_visible_osx(bool visible)
{
//	[NSMenu setMenuBarVisible:(visible ? YES : NO)];
}

void set_current_directory()
{
//	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	chdir([[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] UTF8String]);
//	[pool release];
}

#if TARGET_OS_MACCATALYST
// The app's bundle identifier, with a defensive fallback if -bundleIdentifier
// is ever nil. Must match the Swift side (Bundle.main.bundleIdentifier) so both
// resolve to byte-identical container paths.
static NSString *pocketshaver_bundle_identifier()
{
	NSString *bid = [[NSBundle mainBundle] bundleIdentifier];
	return bid.length ? bid : @"com.carbjo.pocketshaver";
}

// <real home>/Library/Containers/<bundle-id>/Data — the container Data dir the
// OS would manage if we were sandboxed. We are not, so NSHomeDirectory() is the
// real user home; we store here anyway to keep data out of the visible home.
static NSString *pocketshaver_container_data_path()
{
	NSString *containers = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Containers"];
	NSString *container  = [containers stringByAppendingPathComponent:pocketshaver_bundle_identifier()];
	return [container stringByAppendingPathComponent:@"Data"];
}

void pocketshaver_migrate_home_if_needed()
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSFileManager *fm = [NSFileManager defaultManager];
		NSString *newHome = pocketshaver_container_data_path();
		NSString *oldHome = [NSHomeDirectory() stringByAppendingPathComponent:@"PocketShaver Home"];

		// Treat an absent OR empty container as "not yet migrated": a prior
		// interrupted attempt (or a first-use access that created an empty dir)
		// must not defeat migration and strand the intact legacy home.
		NSArray *newContents = [fm contentsOfDirectoryAtPath:newHome error:nil];
		BOOL newHasContent = (newContents.count > 0);
		BOOL oldExists = [fm fileExistsAtPath:oldHome];

		if (!newHasContent && oldExists) {
			// Ensure ~/Library/Containers/<bundle-id>/ exists, and clear any
			// empty/leftover container so move/copy has a clean destination
			// (both fail if the destination already exists).
			[fm createDirectoryAtPath:[newHome stringByDeletingLastPathComponent]
				  withIntermediateDirectories:YES attributes:nil error:nil];
			[fm removeItemAtPath:newHome error:nil];

			NSError *err = nil;
			if ([fm moveItemAtPath:oldHome toPath:newHome error:&err]) {
				printf("Migrated app home %s -> %s\n",
					   [oldHome fileSystemRepresentation], [newHome fileSystemRepresentation]);
			} else {
				// Move can fail across volumes; fall back to a copy, keeping the
				// legacy home intact. Clear any partial destination the failed
				// move may have left so the copy starts clean.
				[fm removeItemAtPath:newHome error:nil];
				NSError *cErr = nil;
				if ([fm copyItemAtPath:oldHome toPath:newHome error:&cErr]) {
					printf("Copied app home %s -> %s (move failed: %s)\n",
						   [oldHome fileSystemRepresentation], [newHome fileSystemRepresentation],
						   [[err localizedDescription] UTF8String]);
				} else {
					// Discard any partial copy so the next launch retries cleanly
					// from the still-intact legacy home instead of adopting a
					// truncated container. (The empty dir the ensure-create below
					// leaves is treated as "not migrated" on the next run.)
					[fm removeItemAtPath:newHome error:nil];
					printf("WARNING: could not migrate app home %s -> %s (%s)\n",
						   [oldHome fileSystemRepresentation], [newHome fileSystemRepresentation],
						   [[cErr localizedDescription] UTF8String]);
				}
			}
		} else if (newHasContent && oldExists) {
			// Interim-build overlap: a populated container already exists. Leave
			// the stale legacy dir untouched rather than risk clobbering live
			// data (cleanup of the legacy dir is deliberately left to the user).
			printf("Note: both container home %s and legacy %s exist; leaving legacy dir in place.\n",
				   [newHome fileSystemRepresentation], [oldHome fileSystemRepresentation]);
		}

		// Ensure the container Data dir exists for first use.
		[fm createDirectoryAtPath:newHome withIntermediateDirectories:YES attributes:nil error:nil];
	});
}

const char* pocketshaver_home_directory()
{
	// Idempotent one-time relocation; calling here guarantees the move even if
	// some path resolves before main()'s explicit call.
	pocketshaver_migrate_home_if_needed();
	NSString *home = pocketshaver_container_data_path();
	// Recreate defensively each call (matches prior behavior if the dir is
	// removed at runtime); cheap and returns immediately if it already exists.
	[[NSFileManager defaultManager] createDirectoryAtPath:home
							  withIntermediateDirectories:YES attributes:nil error:nil];
	static char buf[1024];
	strlcpy(buf, [home fileSystemRepresentation], sizeof(buf));
	return buf;
}
#endif

const char* document_directory()
{
#if TARGET_OS_MACCATALYST
	// On Catalyst, "Documents" (ROM, prefs, disk images, extfs root) lives
	// under the PocketShaver home rather than the user's real ~/Documents.
	static char buf[1024];
	NSString *docs = [[NSString stringWithUTF8String:pocketshaver_home_directory()]
					  stringByAppendingPathComponent:@"Documents"];
	[[NSFileManager defaultManager] createDirectoryAtPath:docs
							  withIntermediateDirectories:YES
											   attributes:nil
													error:nil];
	strlcpy(buf, [docs fileSystemRepresentation], sizeof(buf));
	return buf;
#else
	NSArray* aDirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
	//	NSLog (@"%s Found dirs: %@", __PRETTY_FUNCTION__, aDirs);
	if ([aDirs count]) {
		return [[aDirs firstObject] UTF8String];
	}
	return "";
#endif
}

const char* home_directory()
{
	return [NSHomeDirectory() UTF8String];
}

bool MetalIsAvailable() {
	return true;
#if 0
	const int EL_CAPITAN = 15; // Darwin major version of El Capitan
	char s[16];
	size_t size = sizeof(s);
	int v;
	if (sysctlbyname("kern.osrelease", s, &size, NULL, 0) || sscanf(s, "%d", &v) != 1 || v < EL_CAPITAN) return false;
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	bool r = dev != nil;
	[dev release];
	return r;
#endif
}

extern SDL_Window *sdl_window;
#if TARGET_OS_MACCATALYST
extern "C" bool catalyst_is_window_fullscreen(void);
#endif

static UIView *s_window_content_view = nil;
#if TARGET_OS_MACCATALYST
static NSArray<NSLayoutConstraint *> *s_window_pin_constraints = nil;
#endif
static atomic_uint64 s_present_rect_origin = 0;
static atomic_uint64 s_present_rect_size = 0;

void *PocketShaverGetSDLUIWindow(void)
{
	if (!sdl_window)
		return NULL;
#if USE_SDL3
	SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
	return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
#elif USE_SDL2
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(sdl_window, &wmInfo))
		return NULL;
	if (wmInfo.subsystem != SDL_SYSWM_UIKIT)
		return NULL;
	return (__bridge void *)wmInfo.info.uikit.window;
#else
	return NULL;
#endif
}

#if TARGET_OS_MACCATALYST
static void PocketShaverPinContentView(void)
{
	UIView *view = s_window_content_view;
	if (!view || !view.superview)
		return;
	if (s_window_pin_constraints) {
		[NSLayoutConstraint deactivateConstraints:s_window_pin_constraints];
		s_window_pin_constraints = nil;
	}
	view.translatesAutoresizingMaskIntoConstraints = NO;
	UIView *superview = view.superview;
	BOOL fullscreen = catalyst_is_window_fullscreen();
	NSLayoutYAxisAnchor *topAnchor = fullscreen ? superview.topAnchor
	                                             : superview.safeAreaLayoutGuide.topAnchor;
	NSArray<NSLayoutConstraint *> *pins = @[
		[view.topAnchor constraintEqualToAnchor:topAnchor],
		[view.leadingAnchor constraintEqualToAnchor:superview.leadingAnchor],
		[view.trailingAnchor constraintEqualToAnchor:superview.trailingAnchor],
		[view.bottomAnchor constraintEqualToAnchor:superview.bottomAnchor],
	];
	[NSLayoutConstraint activateConstraints:pins];
	s_window_pin_constraints = pins;
}

static void PocketShaverApplyLetterboxColor(void)
{
	UIView *view = s_window_content_view;
	if (!view)
		return;
	if (catalyst_is_window_fullscreen()) {
		view.backgroundColor = [UIColor blackColor];
	} else {
		view.backgroundColor = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
			return traits.userInterfaceStyle == UIUserInterfaceStyleDark
				? [UIColor colorWithWhite:0.16 alpha:1.0]
				: [UIColor colorWithWhite:0.93 alpha:1.0];
		}];
	}
}
#endif

static UIView *PocketShaverFindMetalView(UIView *root)
{
	if (!root)
		return nil;
	if ([root.layer isKindOfClass:[CAMetalLayer class]])
		return root;
	for (UIView *child in root.subviews) {
		UIView *found = PocketShaverFindMetalView(child);
		if (found)
			return found;
	}
	return nil;
}

static UIView *PocketShaverSDLMetalView(void)
{
	UIWindow *uiWindow = (__bridge UIWindow *)PocketShaverGetSDLUIWindow();
	if (!uiWindow)
		return nil;
	UIView *root = uiWindow.rootViewController.view;
#if USE_SDL3
	if (sdl_window) {
		SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
		NSInteger tag = (NSInteger)SDL_GetNumberProperty(props,
			SDL_PROP_WINDOW_UIKIT_METAL_VIEW_TAG_NUMBER, 0);
		if (tag != 0 && root) {
			UIView *tagged = [root viewWithTag:tag];
			if (tagged)
				return tagged;
		}
	}
#endif
	return PocketShaverFindMetalView(root);
}

static void PocketShaverInsertContentView(UIView *view)
{
	if (!view)
		return;
	UIWindow *uiWindow = (__bridge UIWindow *)PocketShaverGetSDLUIWindow();
	UIView *sdlContainer = uiWindow.rootViewController.view;
	if (sdlContainer && view != sdlContainer && view.superview != sdlContainer)
		[sdlContainer insertSubview:view atIndex:0];
	else if (!view.superview && uiWindow)
		[uiWindow insertSubview:view atIndex:0];
}

void PocketShaverInstallWindowContentView(void *uiview)
{
	UIView *view = (__bridge UIView *)uiview;
#if TARGET_OS_MACCATALYST
	if (s_window_pin_constraints) {
		[NSLayoutConstraint deactivateConstraints:s_window_pin_constraints];
		s_window_pin_constraints = nil;
	}
#endif
	s_window_content_view = view;
	if (!view)
		return;
	PocketShaverInsertContentView(view);
#if TARGET_OS_MACCATALYST
	PocketShaverPinContentView();
	PocketShaverApplyLetterboxColor();
#else
	view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	if (view.superview)
		view.frame = view.superview.bounds;
#endif
	MetalCompositorRefreshPresentRect();
}

void PocketShaverRehomeWindowContentView(void)
{
	if (!s_window_content_view)
		return;
	PocketShaverInsertContentView(s_window_content_view);
#if TARGET_OS_MACCATALYST
	PocketShaverPinContentView();
#endif
	MetalCompositorRefreshPresentRect();
}

void PocketShaverAdoptSDLWindowContentView(void)
{
	UIView *metal = PocketShaverSDLMetalView();
	if (metal) {
		fprintf(stderr, "[pocketshaver] pinning SDL Metal view %p bounds=%.0fx%.0f\n",
			metal, metal.bounds.size.width, metal.bounds.size.height);
		PocketShaverInstallWindowContentView((__bridge void *)metal);
		return;
	}
	/* Do not Auto Layout-pin SDL's container: that collapses the Metal
	 * drawable on Catalyst. Size the container with the autoresizing mask. */
	UIWindow *uiWindow = (__bridge UIWindow *)PocketShaverGetSDLUIWindow();
	UIView *root = uiWindow.rootViewController.view;
	if (!root)
		root = uiWindow;
	if (!root)
		return;
	root.translatesAutoresizingMaskIntoConstraints = YES;
	root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	if (root.superview)
		root.frame = root.superview.bounds;
	fprintf(stderr, "[pocketshaver] no Metal view yet; sized SDL container %p to %.0fx%.0f\n",
		root, root.bounds.size.width, root.bounds.size.height);
}

extern "C" void MetalCompositorRefreshPresentRect(void)
{
	if (![NSThread isMainThread])
		return;
	UIView *view = s_window_content_view;
	if (!view || !view.superview)
		return;
	CGRect vb = view.bounds;
	if (vb.size.width <= 0.0 || vb.size.height <= 0.0)
		return;
	CGRect inWindow = [view convertRect:vb toView:nil];
	int x = (int)(inWindow.origin.x + 0.5);
	int y = (int)(inWindow.origin.y + 0.5);
	int w = (int)(inWindow.size.width + 0.5);
	int h = (int)(inWindow.size.height + 0.5);
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	atomic_store_explicit(&s_present_rect_origin,
		((uint64_t)(uint32_t)x << 32) | (uint32_t)y, memory_order_relaxed);
	atomic_store_explicit(&s_present_rect_size,
		((uint64_t)(uint32_t)w << 32) | (uint32_t)h, memory_order_relaxed);
}

extern "C" void MetalCompositorGetPresentRect(int *out_x, int *out_y,
                                              int *out_w, int *out_h)
{
	uint64_t o = atomic_load_explicit(&s_present_rect_origin, memory_order_relaxed);
	uint64_t s = atomic_load_explicit(&s_present_rect_size, memory_order_relaxed);
	if (out_x) *out_x = (int)(uint32_t)(o >> 32);
	if (out_y) *out_y = (int)(uint32_t)(o & 0xffffffffu);
	if (out_w) *out_w = (int)(uint32_t)(s >> 32);
	if (out_h) *out_h = (int)(uint32_t)(s & 0xffffffffu);
}

extern "C" double MetalCompositorWindowedContentInsetTop(void)
{
#if TARGET_OS_MACCATALYST
	if (!s_window_content_view || !s_window_content_view.superview)
		return 0.0;
	return (double)s_window_content_view.superview.safeAreaInsets.top;
#else
	return 0.0;
#endif
}

extern "C" void MetalCompositorReapplyWindowPinning(void)
{
#if TARGET_OS_MACCATALYST
	void (^apply)(void) = ^{
		PocketShaverPinContentView();
		PocketShaverApplyLetterboxColor();
		MetalCompositorRefreshPresentRect();
	};
	if ([NSThread isMainThread])
		apply();
	else
		dispatch_async(dispatch_get_main_queue(), apply);
#endif
}
