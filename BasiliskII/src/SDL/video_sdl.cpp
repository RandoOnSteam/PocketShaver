/*
 *  video_sdl.cpp - Video/graphics emulation, SDL 1.x specific stuff
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
 *  NOTES:
 *    The Ctrl key works like a qualifier for special actions:
 *      Ctrl-Tab = suspend DGA mode (TODO)
 *      Ctrl-Esc = emergency quit
 *      Ctrl-F1 = mount floppy
 *      Ctrl-F5 = grab mouse (in windowed mode)
 *
 *  FIXMEs and TODOs:
 *  - Windows requires an extra mouse event to update the actual cursor image?
 *  - Ctr-Tab for suspend/resume but how? SDL does not support that for non-Linux
 *  - Ctrl-Fn doesn't generate SDL_KEYDOWN events (SDL bug?)
 *  - Mouse acceleration, there is no API in SDL yet for that
 *  - Force relative mode in Grab mode even if SDL provides absolute coordinates?
 *  - Gamma tables support is likely to be broken here
 *  - Events processing is bound to the general emulation thread as SDL requires
 *    to PumpEvents() within the same thread as the one that called SetVideoMode().
 *    Besides, there can't seem to be a way to call SetVideoMode() from a child thread.
 *  - Backport hw cursor acceleration to Basilisk II?
 *  - Factor out code
 */

#include "sysdeps.h"

#include "my_sdl.h"
#if SDL_COMPILEDVERSION >= SDL_VERSIONNUM(1, 0, 0) && SDL_COMPILEDVERSION < SDL_VERSIONNUM(2, 0, 0)

#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <errno.h>
#include <vector>
#include <string>

#ifdef WIN32
#include <malloc.h> /* alloca() */
#endif

#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#ifdef ENABLE_EMULATOR_MONITOR
#include "emulator_monitor.h"
#endif
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "video_defs.h"
#include "video_blit.h"
#include "vm_alloc.h"
#if defined(ENABLE_GFXACCEL) && defined(SHEEPSHAVER)
#define SDL1_GFXACCEL 1
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"
#include "nqd_accel.h"
#endif
#include "SDL_syswm.h"

void video_get_host_window_size(int *width, int *height);

#define DEBUG 0
#include "debug.h"

// Supported video modes
using std::vector;
static vector<VIDEO_MODE> VideoModes;

// Display types
#ifdef SHEEPSHAVER
enum {
	DISPLAY_WINDOW = DIS_WINDOW,					// windowed display
	DISPLAY_SCREEN = DIS_SCREEN						// fullscreen display
};
extern int display_type;							// See enum above
#else
enum {
	DISPLAY_WINDOW,									// windowed display
	DISPLAY_SCREEN									// fullscreen display
};
static int display_type = DISPLAY_WINDOW;			// See enum above
#endif

// Constants
#if defined(WIN32) || __MACOSX__
const char KEYCODE_FILE_NAME[] = "BasiliskII_keycodes";
#else
const char KEYCODE_FILE_NAME[] = DATADIR "/keycodes";
#endif


// Global variables
static uint32 frame_skip;							// Prefs items
static int16 mouse_wheel_mode;
static int16 mouse_wheel_lines;

static uint8 *the_buffer = NULL;					// Mac frame buffer (where MacOS draws into)
static uint8 *the_buffer_copy = NULL;				// Copy of Mac frame buffer (for refreshed modes)
static uint32 the_buffer_size;						// Size of allocated the_buffer

static bool redraw_thread_active = false;			// Flag: Redraw thread installed
#ifndef USE_CPU_EMUL_SERVICES
static volatile bool redraw_thread_cancel;			// Flag: Cancel Redraw thread
static SDL_Thread *redraw_thread = NULL;			// Redraw thread
static volatile bool thread_stop_req = false;
static volatile bool thread_stop_ack = false;		// Acknowledge for thread_stop_req
#endif

#ifdef ENABLE_VOSF
static bool use_vosf = false;						// Flag: VOSF enabled
#else
static const bool use_vosf = false;					// VOSF not possible
#endif

static bool ctrl_down = false;						// Flag: Ctrl key pressed (for use with hotkeys)
static bool opt_down = false;						// Flag: Opt/Alt key pressed (for use with hotkeys)
static bool cmd_down = false;						// Flag: Cmd/Super/Win key pressed (for use with hotkeys)
static bool caps_on = false;						// Flag: Caps Lock on
static bool quit_full_screen = false;				// Flag: DGA close requested from redraw thread
static bool emerg_quit = false;						// Flag: Ctrl-Esc pressed, emergency quit requested from MacOS thread
static bool emul_suspended = false;					// Flag: Emulator suspended

static bool classic_mode = false;					// Flag: Classic Mac video mode

static bool use_keycodes = false;					// Flag: Use keycodes rather than keysyms
static int keycode_table[256];						// X keycode -> Mac keycode translation table

// SDL variables
static int screen_depth;							// Depth of current screen
static SDL_Cursor *sdl_cursor;						// Copy of Mac cursor
static SDL_Color sdl_palette[256];					// Color palette to be used as CLUT and gamma table
static bool sdl_palette_changed = false;			// Flag: Palette changed, redraw thread must set new colors
static bool toggle_fullscreen = false;
#ifdef ENABLE_EMULATOR_MONITOR
static const int sdl_eventmask = SDL_MOUSEEVENTMASK | SDL_KEYEVENTMASK | SDL_VIDEOEXPOSEMASK | SDL_QUITMASK | SDL_ACTIVEEVENTMASK | SDL_EVENTMASK(SDL_USEREVENT);
#else
static const int sdl_eventmask = SDL_MOUSEEVENTMASK | SDL_KEYEVENTMASK | SDL_VIDEOEXPOSEMASK | SDL_QUITMASK | SDL_ACTIVEEVENTMASK;
#endif

static bool mouse_grabbed = false;
static bool mouse_grabbed_window_name_status = false;

// Mutex to protect SDL events
static SDL_mutex *sdl_events_lock = NULL;
#define LOCK_EVENTS SDL_LockMutex(sdl_events_lock)
#define UNLOCK_EVENTS SDL_UnlockMutex(sdl_events_lock)

// Mutex to protect palette
static SDL_mutex *sdl_palette_lock = NULL;
#define LOCK_PALETTE SDL_LockMutex(sdl_palette_lock)
#define UNLOCK_PALETTE SDL_UnlockMutex(sdl_palette_lock)

// Mutex to protect frame buffer
static SDL_mutex *frame_buffer_lock = NULL;
#define LOCK_FRAME_BUFFER SDL_LockMutex(frame_buffer_lock)
#define UNLOCK_FRAME_BUFFER SDL_UnlockMutex(frame_buffer_lock)

// Video refresh function
static void VideoRefreshInit(void);
static void (*video_refresh)(void);


// Prototypes
static int redraw_func(void *arg);

// From sys_unix.cpp
extern void SysMountFirstFloppy(void);


/*
 *  SDL surface locking glue
 */

#ifdef ENABLE_VOSF
#define SDL_VIDEO_LOCK_VOSF_SURFACE(SURFACE) do {				\
	if ((SURFACE)->flags & (SDL_HWSURFACE | SDL_FULLSCREEN))	\
		the_host_buffer = (uint8 *)(SURFACE)->pixels;			\
} while (0)
#else
#define SDL_VIDEO_LOCK_VOSF_SURFACE(SURFACE)
#endif

#define SDL_VIDEO_LOCK_SURFACE(SURFACE) do {	\
	if (SDL_MUSTLOCK(SURFACE)) {				\
		SDL_LockSurface(SURFACE);				\
		SDL_VIDEO_LOCK_VOSF_SURFACE(SURFACE);	\
	}											\
} while (0)

#define SDL_VIDEO_UNLOCK_SURFACE(SURFACE) do {	\
	if (SDL_MUSTLOCK(SURFACE))					\
		SDL_UnlockSurface(SURFACE);				\
} while (0)


/*
 *  Framebuffer allocation routines
 */

#ifdef SDL1_GFXACCEL
#define FRAMEBUFFER_APERTURE_SIZE (80 * 1024 * 1024)

static void *FramebufferAperture(bool allocate)
{
	static void *aperture = VM_MAP_FAILED;
	if (allocate && aperture == VM_MAP_FAILED)
		aperture = vm_acquire(FRAMEBUFFER_APERTURE_SIZE, VM_MAP_DEFAULT | VM_MAP_32BIT);
	return aperture;
}
#endif

static bool HostPresentationSuspended(void)
{
#ifdef SDL1_GFXACCEL
	return MetalCompositorIsInitialized() != 0;
#else
	return false;
#endif
}

static void *vm_acquire_framebuffer(uint32 size)
{
#ifdef SDL1_GFXACCEL
	if (size <= FRAMEBUFFER_APERTURE_SIZE && FramebufferAperture(true) != VM_MAP_FAILED)
		return FramebufferAperture(false);
#endif
	// always try to reallocate framebuffer at the same address
	static void *fb = VM_MAP_FAILED;
	if (fb != VM_MAP_FAILED) {
		if (vm_acquire_fixed(fb, size) < 0) {
#ifndef SHEEPSHAVER
			printf("FATAL: Could not reallocate framebuffer at previous address\n");
#endif
			fb = VM_MAP_FAILED;
		}
	}
	if (fb == VM_MAP_FAILED)
		fb = vm_acquire(size, VM_MAP_DEFAULT | VM_MAP_32BIT);
	return fb;
}

static inline void vm_release_framebuffer(void *fb, uint32 size)
{
#ifdef SDL1_GFXACCEL
	if (fb == FramebufferAperture(false))
		return;
#endif
	vm_release(fb, size);
}

static inline int get_customized_color_depth(int default_depth)
{
	int display_color_depth = PrefsFindInt32("displaycolordepth");

	D(bug("Get displaycolordepth %d\n", display_color_depth));

	if(0 == display_color_depth)
		return default_depth;
	else{
		switch (display_color_depth) {
		case 8:
			return VIDEO_DEPTH_8BIT;
		case 15: case 16:
			return VIDEO_DEPTH_16BIT;
		case 24: case 32:
			return VIDEO_DEPTH_32BIT;
		default:
			return default_depth;
		}
	}
}

/*
 *  Windows message handler
 */

#ifdef WIN32
#include <dbt.h>
static WNDPROC sdl_window_proc = NULL;				// Window proc used by SDL

extern void SysMediaArrived(void);
extern void SysMediaRemoved(void);
extern HWND GetMainWindowHandle(void);

static LRESULT CALLBACK windows_message_handler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVICEREMOVECOMPLETE) {
			DEV_BROADCAST_HDR *p = (DEV_BROADCAST_HDR *)lParam;
			if (p->dbch_devicetype == DBT_DEVTYP_VOLUME)
				SysMediaRemoved();
		}
		else if (wParam == DBT_DEVICEARRIVAL) {
			DEV_BROADCAST_HDR *p = (DEV_BROADCAST_HDR *)lParam;
			if (p->dbch_devicetype == DBT_DEVTYP_VOLUME)
				SysMediaArrived();
		}
		return 0;

	case WM_ERASEBKGND:
		if (HostPresentationSuspended())
			return 1;
		break;

	case WM_PAINT:
		if (HostPresentationSuspended()) {
			ValidateRect(hwnd, NULL);
			return 0;
		}
		break;
	}

	if (sdl_window_proc)
		return CallWindowProc(sdl_window_proc, hwnd, msg, wParam, lParam);

	return DefWindowProc(hwnd, msg, wParam, lParam);
}
#endif


/*
 *  SheepShaver glue
 */

#ifdef SHEEPSHAVER
// Color depth modes type
typedef int video_depth;

// 1, 2, 4 and 8 bit depths use a color palette
static inline bool IsDirectMode(VIDEO_MODE const & mode)
{
	return IsDirectMode(mode.viAppleMode);
}

// Abstract base class representing one (possibly virtual) monitor
// ("monitor" = rectangular display with a contiguous frame buffer)
class monitor_desc {
public:
	monitor_desc(const vector<VIDEO_MODE> &available_modes, video_depth default_depth, uint32 default_id) {}
	virtual ~monitor_desc() {}

	// Get current Mac frame buffer base address
	uint32 get_mac_frame_base(void) const {return screen_base;}

	// Set Mac frame buffer base address (called from switch_to_mode())
	void set_mac_frame_base(uint32 base) {screen_base = base;}

	// Get current video mode
	const VIDEO_MODE &get_current_mode(void) const {return VModes[cur_mode];}

	// Called by the video driver to switch the video mode on this display
	// (must call set_mac_frame_base())
	virtual void switch_to_current_mode(void) = 0;

	// Called by the video driver to set the color palette (in indexed modes)
	// or the gamma table (in direct modes)
	virtual void set_palette(uint8 *pal, int num) = 0;
};

// Vector of pointers to available monitor descriptions, filled by VideoInit()
static vector<monitor_desc *> VideoMonitors;

// Find Apple mode matching best specified dimensions
static int find_apple_resolution(int xsize, int ysize)
{
	if (xsize == 640 && ysize == 480)
		return APPLE_640x480;
	if (xsize == 800 && ysize == 600)
		return APPLE_800x600;
	if (xsize == 1024 && ysize == 768)
		return APPLE_1024x768;
	if (xsize == 1152 && ysize == 768)
		return APPLE_1152x768;
	if (xsize == 1152 && ysize == 900)
		return APPLE_1152x900;
	if (xsize == 1280 && ysize == 1024)
		return APPLE_1280x1024;
	if (xsize == 1600 && ysize == 1200)
		return APPLE_1600x1200;
	return APPLE_CUSTOM;
}

// Display error alert
static void ErrorAlert(int error)
{
	ErrorAlert(GetString(error));
}
#endif


/*
 *  monitor_desc subclass for SDL display
 */

class SDL_monitor_desc : public monitor_desc {
public:
	SDL_monitor_desc(const vector<VIDEO_MODE> &available_modes, video_depth default_depth, uint32 default_id) : monitor_desc(available_modes, default_depth, default_id), desktopwidth(0), desktopheight(0), initialgammavalid(false), lastgammavalid(false), hostfocused(true) {}
	~SDL_monitor_desc() {}

	virtual void switch_to_current_mode(void);
	virtual void set_palette(uint8 *pal, int num);
	virtual void set_gamma(uint8 *gamma, int num);

	bool video_open(void);
	void video_close(void);
	void ApplyGammaRamp(void);

	int desktopwidth;
	int desktopheight;
	uint16 initialgammared[256];
	uint16 initialgammagreen[256];
	uint16 initialgammablue[256];
	uint16 lastgammared[256];
	uint16 lastgammagreen[256];
	uint16 lastgammablue[256];
	bool initialgammavalid;
	bool lastgammavalid;
	bool hostfocused;
};


/*
 *  Utility functions
 */

// Find palette size for given color depth
static int palette_size(int mode)
{
	switch (mode) {
	case VIDEO_DEPTH_1BIT: return 2;
	case VIDEO_DEPTH_2BIT: return 4;
	case VIDEO_DEPTH_4BIT: return 16;
	case VIDEO_DEPTH_8BIT: return 256;
	case VIDEO_DEPTH_16BIT: return 32;
	case VIDEO_DEPTH_32BIT: return 256;
	default: return 0;
	}
}

// Map video_mode depth ID to numerical depth value
static int mac_depth_of_video_depth(int video_depth)
{
	int depth = -1;
	switch (video_depth) {
	case VIDEO_DEPTH_1BIT:
		depth = 1;
		break;
	case VIDEO_DEPTH_2BIT:
		depth = 2;
		break;
	case VIDEO_DEPTH_4BIT:
		depth = 4;
		break;
	case VIDEO_DEPTH_8BIT:
		depth = 8;
		break;
	case VIDEO_DEPTH_16BIT:
		depth = 16;
		break;
	case VIDEO_DEPTH_32BIT:
		depth = 32;
		break;
	default:
		abort();
	}
	return depth;
}

// Map video_mode depth ID to SDL screen depth
static int sdl_depth_of_video_depth(int video_depth)
{
	return (video_depth <= VIDEO_DEPTH_8BIT) ? 8 : mac_depth_of_video_depth(video_depth);
}

// Get screen dimensions
static void sdl_display_dimensions(int &width, int &height)
{
	static int max_width, max_height;
	if (max_width == 0 && max_height == 0) {
		max_width = 640 ; max_height = 480;
		SDL_Rect **modes = SDL_ListModes(NULL, SDL_FULLSCREEN | SDL_HWSURFACE);
		if (modes && modes != (SDL_Rect **)-1) {
			// It turns out that on some implementations, and contrary to the documentation,
			// the returned list is not sorted from largest to smallest (e.g. Windows)
			for (int i = 0; modes[i] != NULL; i++) {
				const int w = modes[i]->w;
				const int h = modes[i]->h;
				if (w > max_width && h > max_height) {
					max_width = w;
					max_height = h;
				}
			}
		}
	}
	width = max_width;
	height = max_height;
}

static inline int sdl_display_width(void)
{
	int width, height;
	sdl_display_dimensions(width, height);
	return width;
}

static inline int sdl_display_height(void)
{
	int width, height;
	sdl_display_dimensions(width, height);
	return height;
}

// Check whether specified mode is available
static bool has_mode(int type, int width, int height, int depth)
{
	// Filter out out-of-bounds resolutions
	if (width > sdl_display_width() || height > sdl_display_height())
		return false;

	// Rely on SDL capabilities
	return SDL_VideoModeOK(width, height,
						   sdl_depth_of_video_depth(depth),
						   SDL_HWSURFACE | (type == DISPLAY_SCREEN ? SDL_FULLSCREEN : 0)) != 0;
}

// Add mode to list of supported modes
static void add_mode(int type, int width, int height, int resolution_id, int bytes_per_row, int depth)
{
	// Filter out unsupported modes
	if (!has_mode(type, width, height, depth))
		return;

	// Fill in VideoMode entry
	VIDEO_MODE mode;
#ifdef SHEEPSHAVER
	resolution_id = find_apple_resolution(width, height);
	mode.viType = type;
#endif
	VIDEO_MODE_X = width;
	VIDEO_MODE_Y = height;
	VIDEO_MODE_RESOLUTION = resolution_id;
	VIDEO_MODE_ROW_BYTES = bytes_per_row;
	VIDEO_MODE_DEPTH = (video_depth)depth;
	VideoModes.push_back(mode);
}

// Set Mac frame layout and base address (uses the_buffer/MacFrameBaseMac)
static void set_mac_frame_buffer(SDL_monitor_desc &monitor, int depth)
{
#if !REAL_ADDRESSING && !DIRECT_ADDRESSING
	int layout = FLAYOUT_DIRECT;
	if (depth == VIDEO_DEPTH_16BIT)
		layout = (screen_depth == 15) ? FLAYOUT_HOST_555 : FLAYOUT_HOST_565;
	else if (depth == VIDEO_DEPTH_32BIT)
		layout = (screen_depth == 24) ? FLAYOUT_HOST_888 : FLAYOUT_DIRECT;
	MacFrameLayout = layout;
	monitor.set_mac_frame_base(MacFrameBaseMac);

	// Set variables used by UAE memory banking
	const VIDEO_MODE &mode = monitor.get_current_mode();
	MacFrameBaseHost = the_buffer;
	MacFrameSize = VIDEO_MODE_ROW_BYTES * VIDEO_MODE_Y;
	InitFrameBufferMapping();
#else
	monitor.set_mac_frame_base(Host2MacAddr(the_buffer));
#endif
	D(bug("monitor.mac_frame_base = %08x\n", monitor.get_mac_frame_base()));
}

// Set window name and class
static void set_window_name(bool mouse_grabbed)
{
	const char *title = PrefsFindString("title");
	std::string s = title ? title : GetString(STR_WINDOW_TITLE);
	int grabbed = 0;
	if (mouse_grabbed)
	{
        s += GetString(STR_WINDOW_TITLE_GRABBED_PRE);
		int hotkey = PrefsFindInt32("hotkey");
		hotkey = hotkey ? hotkey : 1;
		if (hotkey & 1) s += GetString(STR_WINDOW_TITLE_GRABBED1);
        if (hotkey & 2) s += GetString(STR_WINDOW_TITLE_GRABBED2);
        if (hotkey & 4) s += GetString(STR_WINDOW_TITLE_GRABBED4);
        s += GetString(STR_WINDOW_TITLE_GRABBED_POST);
	}
	const SDL_VideoInfo *vi = SDL_GetVideoInfo();
	if (vi && vi->wm_available)
	{
		//The icon name should stay the same
		SDL_WM_SetCaption(s.c_str(), GetString(STR_WINDOW_TITLE));
	}
}

// Set mouse grab mode
static SDL_GrabMode set_grab_mode(SDL_GrabMode mode)
{
	const SDL_VideoInfo *vi = SDL_GetVideoInfo();
	return (vi && vi->wm_available ? SDL_WM_GrabInput(mode) : SDL_GRAB_OFF);
}

// Migrate preferences items (XXX to be handled in MigratePrefs())
static void migrate_screen_prefs(void)
{
#ifdef SHEEPSHAVER
	// Look-up priorities are: "screen", "screenmodes", "windowmodes".
	if (PrefsFindString("screen"))
		return;

	uint32 window_modes = PrefsFindInt32("windowmodes");
	uint32 screen_modes = PrefsFindInt32("screenmodes");
	int width = 0, height = 0;
	if (screen_modes) {
		static const struct {
			int id;
			int width;
			int height;
		}
		modes[] = {
			{  1,	 640,	 480 },
			{  2,	 800,	 600 },
			{  4,	1024,	 768 },
			{ 64,	1152,	 768 },
			{  8,	1152,	 900 },
			{ 16,	1280,	1024 },
			{ 32,	1600,	1200 },
			{ 0, }
		};
		for (int i = 0; modes[i].id != 0; i++) {
			if (screen_modes & modes[i].id) {
				if (width < modes[i].width && height < modes[i].height) {
					width = modes[i].width;
					height = modes[i].height;
				}
			}
		}
	} else {
		if (window_modes & 1)
			width = 640, height = 480;
		if (window_modes & 2)
			width = 800, height = 600;
	}
	if (width && height) {
		char str[32];
		sprintf(str, "%s/%d/%d", screen_modes ? "dga" : "win", width, height);
		PrefsReplaceString("screen", str);
	}
#endif
}

static float GetMagnificationRate(void)
{
	const char *magnificationtext = PrefsFindString("mag_rate");
	float magnification = 1.0f;
	if (magnificationtext == NULL || sscanf(magnificationtext, "%f", &magnification) != 1 || magnification < 1.0f)
		return 1.0f;
	if (magnification > 4.0f)
		return 4.0f;
	return magnification;
}

static void BuildScaleMap(vector<int> &firstsources, vector<int> &secondsources, vector<int> &weights, int sourcesize, int destinationsize, bool nearest)
{
	const int64 lastposition = (int64)(sourcesize - 1) << 8;
	firstsources.resize(destinationsize);
	secondsources.resize(destinationsize);
	weights.resize(destinationsize);
	for (int index = 0; index < destinationsize; index++) {
		int64 position;
		if (nearest)
			position = ((int64)(2 * index + 1) * sourcesize / (2 * destinationsize)) << 8;
		else
			position = (int64)(2 * index + 1) * sourcesize * 256 / (2 * destinationsize) - 128;
		if (position < 0)
			position = 0;
		else if (position > lastposition)
			position = lastposition;
		firstsources[index] = (int)(position >> 8);
		weights[index] = (int)(position & 255);
		if (firstsources[index] < sourcesize - 1)
			secondsources[index] = firstsources[index] + 1;
		else
			secondsources[index] = firstsources[index];
	}
}

static inline uint32 BlendPixels(uint32 firstpixel, uint32 secondpixel, uint32 weight)
{
	const uint32 inverseweight = 256 - weight;
	const uint32 evenbytes = (((firstpixel & 0x00ff00ff) * inverseweight + (secondpixel & 0x00ff00ff) * weight) >> 8) & 0x00ff00ff;
	const uint32 oddbytes = (((firstpixel >> 8) & 0x00ff00ff) * inverseweight + ((secondpixel >> 8) & 0x00ff00ff) * weight) & 0xff00ff00;
	return evenbytes | oddbytes;
}

static void ScaleRowNearest(uint32 *destination, const uint32 *source, const int *columns, int count)
{
	for (int index = 0; index < count; index++)
		destination[index] = source[columns[index]];
}

static void ScaleRowLinear(uint32 *destination, const uint32 *source, const int *firstcolumns, const int *secondcolumns, const int *weights, int count)
{
	for (int index = 0; index < count; index++)
		destination[index] = BlendPixels(source[firstcolumns[index]], source[secondcolumns[index]], weights[index]);
}

static void ScaleRowBilinear(uint32 *destination, const uint32 *upper, const uint32 *lower, const int *firstcolumns, const int *secondcolumns, const int *weights, uint32 rowweight, int count)
{
	for (int index = 0; index < count; index++) {
		const uint32 top = BlendPixels(upper[firstcolumns[index]], upper[secondcolumns[index]], weights[index]);
		const uint32 bottom = BlendPixels(lower[firstcolumns[index]], lower[secondcolumns[index]], weights[index]);
		destination[index] = BlendPixels(top, bottom, rowweight);
	}
}


/*
 *  Display "driver" classes
 */

class driver_base {
public:
	driver_base(SDL_monitor_desc &m);
	~driver_base();

	void init(); // One-time init
	void set_video_mode(int flags);
	void adapt_to_video_mode();

	void update_palette(void);
	void suspend(void) {}
	void resume(void) {}
	void toggle_mouse_grab(void);
	void mouse_moved(int x, int y);

	void disable_mouse_accel(void);
	void restore_mouse_accel(void);

	void grab_mouse(void);
	void ungrab_mouse(void);

	void SetupScaling(void);
#ifdef SDL1_GFXACCEL
	void InitGfxAccel(void);
	void UploadStartupPalette(void);
#endif
	void PresentScaled(int left, int top, int width, int height, SDL_Rect *destination);
	void HostToGuest(int &x, int &y);
	void GuestToHost(int &x, int &y);
#ifdef SHEEPSHAVER
	SDL_Cursor *CreateMagnifiedCursor(bool usehotspot);
#endif

public:
	SDL_monitor_desc &monitor; // Associated video monitor
	const VIDEO_MODE &mode;    // Video mode handled by the driver

	bool init_ok;	// Initialization succeeded (we can't use exceptions because of -fomit-frame-pointer)
	SDL_Surface *s;	// The surface we draw into
	SDL_Surface *hostsurface;
	SDL_Surface *convertsurface;
	vector<int> columnfirst;
	vector<int> columnsecond;
	vector<int> columnweight;
	vector<int> rowfirst;
	vector<int> rowsecond;
	vector<int> rowweight;
	int viewleft;
	int viewtop;
	int viewwidth;
	int viewheight;
	bool scaling;
	bool nearestscaling;
	bool borderless;
#ifdef ENABLE_EMULATOR_MONITOR
	EmulatorMonitor *emulatormonitor;
#endif
};

#ifdef ENABLE_VOSF
static void update_display_window_vosf(driver_base *drv);
#endif
static void update_display_static(driver_base *drv);

static driver_base *drv = NULL;	// Pointer to currently used driver object

void update_sdl_video(SDL_Surface *screen, Sint32 x, Sint32 y, Sint32 w, Sint32 h)
{
	SDL_Rect destination;
	if (HostPresentationSuspended())
		return;
	if (!drv->scaling) {
		SDL_UpdateRect(screen, x, y, w, h);
		return;
	}
	if (drv->hostsurface == NULL)
		return;
	drv->PresentScaled(x, y, w, h, &destination);
	SDL_UpdateRect(drv->hostsurface, destination.x, destination.y, destination.w, destination.h);
}

void update_sdl_video(SDL_Surface *screen, int numrects, SDL_Rect *rects)
{
	SDL_Rect *destinations;
	if (HostPresentationSuspended())
		return;
	if (!drv->scaling) {
		SDL_UpdateRects(screen, numrects, rects);
		return;
	}
	if (drv->hostsurface == NULL)
		return;
	destinations = (SDL_Rect *)alloca(sizeof(SDL_Rect) * numrects);
	for (int index = 0; index < numrects; index++)
		drv->PresentScaled(rects[index].x, rects[index].y, rects[index].w, rects[index].h, &destinations[index]);
	SDL_UpdateRects(drv->hostsurface, numrects, destinations);
}

#ifdef ENABLE_VOSF
# include "video_vosf.h"
#endif

driver_base::driver_base(SDL_monitor_desc &m)
	: monitor(m), mode(m.get_current_mode()), init_ok(false), s(NULL),
	  hostsurface(NULL), convertsurface(NULL), viewleft(0), viewtop(0),
	  viewwidth(0), viewheight(0), scaling(false), nearestscaling(false), borderless(false)
#ifdef ENABLE_EMULATOR_MONITOR
	, emulatormonitor(NULL)
#endif
{
	the_buffer = NULL;
	the_buffer_copy = NULL;
}

void driver_base::set_video_mode(int flags)
{
	const float magnification = GetMagnificationRate();
	const int depth = sdl_depth_of_video_depth(VIDEO_MODE_DEPTH);
	int hostwidth;
	int hostheight;
	int hostflags = flags;
	SDL_PixelFormat *hostformat;
	scaling = magnification != 1.0f;
	if (!scaling) {
		hostsurface = s = SDL_SetVideoMode(VIDEO_MODE_X, VIDEO_MODE_Y, depth, SDL_HWSURFACE | flags);
		if (s == NULL)
			return;
		viewleft = 0;
		viewtop = 0;
		viewwidth = VIDEO_MODE_X;
		viewheight = VIDEO_MODE_Y;
#ifdef ENABLE_VOSF
		the_host_buffer = (uint8 *)s->pixels;
#endif
		return;
	}
	if ((flags & SDL_FULLSCREEN) && monitor.desktopwidth > 0 && monitor.desktopheight > 0) {
		hostwidth = monitor.desktopwidth;
		hostheight = monitor.desktopheight;
	}
	else if (flags & SDL_FULLSCREEN)
		sdl_display_dimensions(hostwidth, hostheight);
	else {
		hostwidth = (int)(VIDEO_MODE_X * magnification + 0.5f);
		hostheight = (int)(VIDEO_MODE_Y * magnification + 0.5f);
	}
#ifdef WIN32
	if (flags & SDL_FULLSCREEN)
		hostflags = (flags & ~SDL_FULLSCREEN) | SDL_NOFRAME;
#endif
	hostsurface = SDL_SetVideoMode(hostwidth, hostheight, 32, SDL_SWSURFACE | hostflags);
	if (hostsurface == NULL)
		return;
#ifdef WIN32
	if (flags & SDL_FULLSCREEN)
		SetWindowPos(GetMainWindowHandle(), HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE);
	else if (borderless) {
		RECT windowrect;
		GetWindowRect(GetMainWindowHandle(), &windowrect);
		SetWindowPos(GetMainWindowHandle(), HWND_TOP,
			(GetSystemMetrics(SM_CXSCREEN) - (windowrect.right - windowrect.left)) / 2,
			(GetSystemMetrics(SM_CYSCREEN) - (windowrect.bottom - windowrect.top)) / 2,
			0, 0, SWP_NOSIZE);
	}
	borderless = (flags & SDL_FULLSCREEN) != 0;
#endif
	hostformat = hostsurface->format;
	if (s == NULL) {
		if (depth == 8)
			s = SDL_CreateRGBSurface(SDL_SWSURFACE, VIDEO_MODE_X, VIDEO_MODE_Y, 8, 0, 0, 0, 0);
		else if (depth == 16 && screen_depth == 15)
			s = SDL_CreateRGBSurface(SDL_SWSURFACE, VIDEO_MODE_X, VIDEO_MODE_Y, 16, 0x7c00, 0x03e0, 0x001f, 0);
		else if (depth == 16)
			s = SDL_CreateRGBSurface(SDL_SWSURFACE, VIDEO_MODE_X, VIDEO_MODE_Y, 16, 0xf800, 0x07e0, 0x001f, 0);
		else
			s = SDL_CreateRGBSurface(SDL_SWSURFACE, VIDEO_MODE_X, VIDEO_MODE_Y, 32, 0xff0000, 0x00ff00, 0x0000ff, 0);
		if (s == NULL)
			return;
	}
	if (convertsurface != NULL && convertsurface != s)
		SDL_FreeSurface(convertsurface);
	if (s->format->BitsPerPixel == 32 && s->format->Rmask == hostformat->Rmask &&
		s->format->Gmask == hostformat->Gmask && s->format->Bmask == hostformat->Bmask)
		convertsurface = s;
	else
		convertsurface = SDL_CreateRGBSurface(SDL_SWSURFACE, VIDEO_MODE_X, VIDEO_MODE_Y, 32,
			hostformat->Rmask, hostformat->Gmask, hostformat->Bmask, 0);
	if (convertsurface == NULL) {
		hostsurface = NULL;
		return;
	}
	SetupScaling();
#ifdef ENABLE_VOSF
	the_host_buffer = (uint8 *)s->pixels;
#endif
}

void driver_base::SetupScaling(void)
{
	const int guestwidth = VIDEO_MODE_X;
	const int guestheight = VIDEO_MODE_Y;
	double scale = (double)hostsurface->w / guestwidth;
	if ((double)hostsurface->h / guestheight < scale)
		scale = (double)hostsurface->h / guestheight;
	if (PrefsFindBool("scale_integer")) {
		scale = (double)(int)scale;
		if (scale < 1.0)
			scale = 1.0;
	}
	viewwidth = (int)(guestwidth * scale + 0.5);
	viewheight = (int)(guestheight * scale + 0.5);
	if (viewwidth > hostsurface->w)
		viewwidth = hostsurface->w;
	if (viewheight > hostsurface->h)
		viewheight = hostsurface->h;
	viewleft = (hostsurface->w - viewwidth) / 2;
	viewtop = (hostsurface->h - viewheight) / 2;
	nearestscaling = PrefsFindBool("scale_nearest");
	BuildScaleMap(columnfirst, columnsecond, columnweight, guestwidth, viewwidth, nearestscaling);
	BuildScaleMap(rowfirst, rowsecond, rowweight, guestheight, viewheight, nearestscaling);
	SDL_FillRect(hostsurface, NULL, SDL_MapRGB(hostsurface->format, 0, 0, 0));
	if (!HostPresentationSuspended())
		SDL_UpdateRect(hostsurface, 0, 0, 0, 0);
}

void driver_base::PresentScaled(int left, int top, int width, int height, SDL_Rect *destination)
{
	const int guestwidth = VIDEO_MODE_X;
	const int guestheight = VIDEO_MODE_Y;
	const int marginx = viewwidth / guestwidth + 2;
	const int marginy = viewheight / guestheight + 2;
	int firstcolumn = (int)((int64)left * viewwidth / guestwidth) - marginx;
	int lastcolumn = (int)(((int64)(left + width) * viewwidth + guestwidth - 1) / guestwidth) + marginx;
	int firstrow = (int)((int64)top * viewheight / guestheight) - marginy;
	int lastrow = (int)(((int64)(top + height) * viewheight + guestheight - 1) / guestheight) + marginy;
	const uint8 *sourcepixels;
	uint8 *destinationrow;
	int sourcepitch;
	int count;
	if (firstcolumn < 0)
		firstcolumn = 0;
	if (lastcolumn > viewwidth)
		lastcolumn = viewwidth;
	if (firstrow < 0)
		firstrow = 0;
	if (lastrow > viewheight)
		lastrow = viewheight;
	if (convertsurface != s) {
		SDL_Rect sourcerect;
		SDL_Rect convertrect;
		sourcerect.x = left;
		sourcerect.y = top;
		sourcerect.w = width;
		sourcerect.h = height;
		convertrect = sourcerect;
		SDL_BlitSurface(s, &sourcerect, convertsurface, &convertrect);
	}
	if (SDL_MUSTLOCK(hostsurface))
		SDL_LockSurface(hostsurface);
	sourcepixels = (const uint8 *)convertsurface->pixels;
	sourcepitch = convertsurface->pitch;
	destinationrow = (uint8 *)hostsurface->pixels + (viewtop + firstrow) * hostsurface->pitch + (viewleft + firstcolumn) * 4;
	count = lastcolumn - firstcolumn;
	for (int row = firstrow; row < lastrow; row++) {
		const uint32 *upper = (const uint32 *)(sourcepixels + rowfirst[row] * sourcepitch);
		if (nearestscaling)
			ScaleRowNearest((uint32 *)destinationrow, upper, &columnfirst[firstcolumn], count);
		else if (rowweight[row] == 0)
			ScaleRowLinear((uint32 *)destinationrow, upper, &columnfirst[firstcolumn], &columnsecond[firstcolumn], &columnweight[firstcolumn], count);
		else
			ScaleRowBilinear((uint32 *)destinationrow, upper, (const uint32 *)(sourcepixels + rowsecond[row] * sourcepitch),
				&columnfirst[firstcolumn], &columnsecond[firstcolumn], &columnweight[firstcolumn], rowweight[row], count);
		destinationrow += hostsurface->pitch;
	}
	if (SDL_MUSTLOCK(hostsurface))
		SDL_UnlockSurface(hostsurface);
	destination->x = viewleft + firstcolumn;
	destination->y = viewtop + firstrow;
	destination->w = count;
	destination->h = lastrow - firstrow;
}

void driver_base::HostToGuest(int &x, int &y)
{
	x = (int)((int64)(x - viewleft) * VIDEO_MODE_X / viewwidth);
	y = (int)((int64)(y - viewtop) * VIDEO_MODE_Y / viewheight);
	if (x < 0)
		x = 0;
	else if (x >= (int)VIDEO_MODE_X)
		x = VIDEO_MODE_X - 1;
	if (y < 0)
		y = 0;
	else if (y >= (int)VIDEO_MODE_Y)
		y = VIDEO_MODE_Y - 1;
}

void driver_base::GuestToHost(int &x, int &y)
{
	x = viewleft + (int)((int64)x * viewwidth / VIDEO_MODE_X);
	y = viewtop + (int)((int64)y * viewheight / VIDEO_MODE_Y);
}

void driver_base::mouse_moved(int x, int y)
{
	if (scaling)
		HostToGuest(x, y);
	ADBMouseMoved(x, y);
}

#ifdef SHEEPSHAVER
SDL_Cursor *driver_base::CreateMagnifiedCursor(bool usehotspot)
{
	int size = (16 * viewwidth + VIDEO_MODE_X - 1) / VIDEO_MODE_X;
	int rowbytes;
	int hotx = 0;
	int hoty = 0;
	if (size > 32)
		size = 32;
	else if (size < 16)
		size = 16;
	rowbytes = (size + 7) >> 3;
	vector<uint8> cursordata(rowbytes * size, 0);
	vector<uint8> cursormask(rowbytes * size, 0);
	for (int row = 0; row < size; row++) {
		const int sourcerow = (row * 16 / size) * 16;
		for (int column = 0; column < size; column++) {
			const int sourcebit = sourcerow + column * 16 / size;
			const uint8 sourcemask = 0x80 >> (sourcebit & 7);
			const uint8 destinationmask = 0x80 >> (column & 7);
			const int destinationindex = row * rowbytes + (column >> 3);
			if (MacCursor[4 + (sourcebit >> 3)] & sourcemask)
				cursordata[destinationindex] |= destinationmask;
			if (MacCursor[36 + (sourcebit >> 3)] & sourcemask)
				cursormask[destinationindex] |= destinationmask;
		}
	}
	if (usehotspot) {
		hotx = MacCursor[2] * size / 16;
		hoty = MacCursor[3] * size / 16;
	}
	return SDL_CreateCursor(&cursordata[0], &cursormask[0], rowbytes * 8, size, hotx, hoty);
}
#endif

#ifdef SDL1_GFXACCEL
static void DMCModeDescFromVModesIndex(int index, DMCModeDesc *description)
{
	description->width = (uint32_t)VModes[index].viXsize;
	description->height = (uint32_t)VModes[index].viYsize;
	switch ((int)VModes[index].viAppleMode) {
	case VIDEO_DEPTH_1BIT:
		description->depth = 1;
		break;
	case VIDEO_DEPTH_2BIT:
		description->depth = 2;
		break;
	case VIDEO_DEPTH_4BIT:
		description->depth = 4;
		break;
	case VIDEO_DEPTH_16BIT:
		description->depth = 16;
		break;
	case VIDEO_DEPTH_32BIT:
		description->depth = 32;
		break;
	default:
		description->depth = 8;
		break;
	}
	description->row_bytes = (uint32_t)VModes[index].viRowBytes;
	description->pitch = description->row_bytes;
	description->vbl_usec = 0;
	description->screen_base_mac = 0;
	description->screen_base_host = NULL;
}

void driver_base::InitGfxAccel(void)
{
	int pitch = VIDEO_MODE_X;
	int result;
	if (VIDEO_MODE_DEPTH == VIDEO_DEPTH_16BIT)
		pitch <<= 1;
	else if (VIDEO_MODE_DEPTH == VIDEO_DEPTH_32BIT)
		pitch <<= 2;
	if (dmc_current_snapshot() == NULL) {
		DMCModeDesc initial;
		DMCModeDescFromVModesIndex(cur_mode, &initial);
		initial.screen_base_mac = screen_base;
		if (screen_base != 0)
			initial.screen_base_host = Mac2HostAddr(screen_base);
		result = dmc_create(&initial);
		if (result != kDMCNoErr)
			fprintf(stderr, "[DMC] dmc_create FAILED (err=%d)\n", result);
	}
	if (MetalCompositorIsInitialized()) {
		result = MetalCompositorResize(VIDEO_MODE_X, VIDEO_MODE_Y, VIDEO_MODE_DEPTH,
			VIDEO_MODE_ROW_BYTES, pitch, the_buffer, the_buffer_size);
		if (result != 0)
			fprintf(stderr, "[metal_compositor] resize FAILED (err=%d)\n", result);
		return;
	}
	result = MetalCompositorInit(VIDEO_MODE_X, VIDEO_MODE_Y, VIDEO_MODE_DEPTH,
		VIDEO_MODE_ROW_BYTES, pitch, the_buffer, the_buffer_size);
	if (result != 0) {
		fprintf(stderr, "[metal_compositor] init FAILED (err=%d)\n", result);
		MetalCompositorShutdown();
		return;
	}
	result = gfxaccel_resources_init();
	if (result != 0)
		fprintf(stderr, "[gfxaccel_resources] init FAILED (err=%d)\n", result);
}

void driver_base::UploadStartupPalette(void)
{
	uint8 blackandwhite[6] = {255, 255, 255, 0, 0, 0};
	MetalCompositorUpdatePalette(blackandwhite, 2);
	dmc_record_palette_change();
}
#endif

void driver_base::init()
{
	set_video_mode(display_type == DISPLAY_SCREEN ? SDL_FULLSCREEN : 0);
	if (s == NULL || hostsurface == NULL)
		return;
	int aligned_height = (VIDEO_MODE_Y + 15) & ~15;

#ifdef ENABLE_VOSF
	use_vosf = true;
	// Allocate memory for frame buffer (SIZE is extended to page-boundary)
	the_buffer_size = page_extend((aligned_height + 2) * s->pitch);
	the_buffer = (uint8 *)vm_acquire_framebuffer(the_buffer_size);
	the_buffer_copy = (uint8 *)malloc(the_buffer_size);
	D(bug("the_buffer = %p, the_buffer_copy = %p, the_host_buffer = %p\n", the_buffer, the_buffer_copy, the_host_buffer));

	// Check whether we can initialize the VOSF subsystem and it's profitable
	if (!video_vosf_init(monitor)) {
		WarningAlert(GetString(STR_VOSF_INIT_ERR));
		use_vosf = false;
	}
	else if (!video_vosf_profitable()) {
		video_vosf_exit();
		printf("VOSF acceleration is not profitable on this platform, disabling it\n");
		use_vosf = false;
	}
	if (!use_vosf) {
		free(the_buffer_copy);
		vm_release(the_buffer, the_buffer_size);
		the_host_buffer = NULL;
	}
#endif
	if (!use_vosf) {
		// Allocate memory for frame buffer
		the_buffer_size = (aligned_height + 2) * s->pitch;
		the_buffer_copy = (uint8 *)calloc(1, the_buffer_size);
		the_buffer = (uint8 *)vm_acquire_framebuffer(the_buffer_size);
		D(bug("the_buffer = %p, the_buffer_copy = %p\n", the_buffer, the_buffer_copy));
	}

	// Set frame buffer base
	set_mac_frame_buffer(monitor, VIDEO_MODE_DEPTH);
#ifdef SDL1_GFXACCEL
	memset(the_buffer, 0, the_buffer_size);
	InitGfxAccel();
#endif

	adapt_to_video_mode();
#ifdef SDL1_GFXACCEL
	UploadStartupPalette();
#endif
#ifdef SHEEPSHAVER
	if (PrefsFindBool("init_grab") && !video_can_change_cursor())
		grab_mouse();
#else
	if (PrefsFindBool("init_grab"))
		grab_mouse();
#endif
}

void driver_base::adapt_to_video_mode() {
	ADBSetRelMouseMode(false);

	// Init blitting routines
	if (!s) return;
	SDL_PixelFormat *f = s->format;
	VisualFormat visualFormat;
	visualFormat.depth = sdl_depth_of_video_depth(VIDEO_MODE_DEPTH);
	visualFormat.Rmask = f->Rmask;
	visualFormat.Gmask = f->Gmask;
	visualFormat.Bmask = f->Bmask;
	Screen_blitter_init(visualFormat, true, mac_depth_of_video_depth(VIDEO_MODE_DEPTH));

	// Load gray ramp to 8->16/32 expand map
	if (!IsDirectMode(mode))
		for (int i=0; i<256; i++)
			ExpandMap[i] = SDL_MapRGB(f, i, i, i);


	bool hardware_cursor = false;
#ifdef SHEEPSHAVER
	hardware_cursor = video_can_change_cursor();
	if (hardware_cursor) {
		if (sdl_cursor)
			SDL_FreeCursor(sdl_cursor);
		if ((sdl_cursor = CreateMagnifiedCursor(false)) != NULL) {
			SDL_SetCursor(sdl_cursor);
		}
	}
	// Tell the video driver there's a change in cursor type
	if (private_data)
		private_data->cursorHardware = hardware_cursor;
#endif
	// Hide cursor
	SDL_ShowCursor(hardware_cursor);

	// Set window name/class
	set_window_name(false);

	// Everything went well
	init_ok = true;
#ifdef ENABLE_EMULATOR_MONITOR
	if (emulatormonitor == NULL) {
		emulatormonitor = new EmulatorMonitor();
		if (!emulatormonitor->Start()) {
			delete emulatormonitor;
			emulatormonitor = NULL;
		}
	}
#endif
}

driver_base::~driver_base()
{
#ifdef ENABLE_EMULATOR_MONITOR
	delete emulatormonitor;
	emulatormonitor = NULL;
#endif
	ungrab_mouse();
	restore_mouse_accel();

	if (convertsurface != NULL && convertsurface != s)
		SDL_FreeSurface(convertsurface);
	if (s)
		SDL_FreeSurface(s);

	// the_buffer shall always be mapped through vm_acquire_framebuffer()
	if (the_buffer != VM_MAP_FAILED) {
		D(bug(" releasing the_buffer at %p (%d bytes)\n", the_buffer, the_buffer_size));
		vm_release_framebuffer(the_buffer, the_buffer_size);
		the_buffer = NULL;
	}

	// Free frame buffer(s)
	if (!use_vosf) {
		if (the_buffer_copy) {
			free(the_buffer_copy);
			the_buffer_copy = NULL;
		}
	}
#ifdef ENABLE_VOSF
	else {
		if (the_buffer_copy) {
			D(bug(" freeing the_buffer_copy at %p\n", the_buffer_copy));
			free(the_buffer_copy);
			the_buffer_copy = NULL;
		}

		// Deinitialize VOSF
		video_vosf_exit();
	}
#endif

	SDL_ShowCursor(1);
}

// Palette has changed
void driver_base::update_palette(void)
{
	const VIDEO_MODE &mode = monitor.get_current_mode();

	if ((int)VIDEO_MODE_DEPTH > VIDEO_DEPTH_8BIT)
		return;
	if (!scaling) {
		SDL_SetPalette(s, SDL_PHYSPAL, sdl_palette, 0, 256);
		return;
	}
	SDL_SetColors(s, sdl_palette, 0, 256);
	update_sdl_video(s, 0, 0, VIDEO_MODE_X, VIDEO_MODE_Y);
}

// Disable mouse acceleration
void driver_base::disable_mouse_accel(void)
{
}

// Restore mouse acceleration to original value
void driver_base::restore_mouse_accel(void)
{
}

// Toggle mouse grab
void driver_base::toggle_mouse_grab(void)
{
	if (mouse_grabbed)
		ungrab_mouse();
	else
		grab_mouse();
}

// Grab mouse, switch to relative mouse mode
void driver_base::grab_mouse(void)
{
	if (!mouse_grabbed) {
		SDL_GrabMode new_mode = set_grab_mode(SDL_GRAB_ON);
		if (new_mode == SDL_GRAB_ON) {
			disable_mouse_accel();
			mouse_grabbed = true;
		}
	}
}

// Ungrab mouse, switch to absolute mouse mode
void driver_base::ungrab_mouse(void)
{
	if (mouse_grabbed) {
		SDL_GrabMode new_mode = set_grab_mode(SDL_GRAB_OFF);
		if (new_mode == SDL_GRAB_OFF) {
			restore_mouse_accel();
			mouse_grabbed = false;
		}
	}
}

/*
 *  Initialization
 */

// Init keycode translation table
static void keycode_init(void)
{
	bool use_kc = PrefsFindBool("keycodes");
	if (use_kc) {

		// Get keycode file path from preferences
		const char *kc_path = PrefsFindString("keycodefile");

		// Open keycode table
		FILE *f = fopen(kc_path ? kc_path : KEYCODE_FILE_NAME, "r");
		if (f == NULL) {
			char str[256];
			snprintf(str, sizeof(str), GetString(STR_KEYCODE_FILE_WARN), kc_path ? kc_path : KEYCODE_FILE_NAME, strerror(errno));
			WarningAlert(str);
			return;
		}

		// Default translation table
		for (int i=0; i<256; i++)
			keycode_table[i] = -1;

		// Search for server vendor string, then read keycodes
		char video_driver[256];
		SDL_VideoDriverName(video_driver, sizeof(video_driver));
		bool video_driver_found = false;
		char line[256];
		int n_keys = 0;
		while (fgets(line, sizeof(line) - 1, f)) {
			// Read line
			int len = strlen(line);
			if (len == 0)
				continue;
			line[len-1] = 0;

			// Comments begin with "#" or ";"
			if (line[0] == '#' || line[0] == ';' || line[0] == 0)
				continue;

			if (video_driver_found) {
				// Skip aliases as long as we have read keycodes yet
				// Otherwise, it's another mapping and we have to stop
				static const char sdl_str[] = "sdl";
				if (strncmp(line, sdl_str, sizeof(sdl_str) - 1) == 0 && n_keys == 0)
					continue;

				// Read keycode
				int x_code, mac_code;
				if (sscanf(line, "%d %d", &x_code, &mac_code) == 2)
					keycode_table[x_code & 0xff] = mac_code, n_keys++;
				else
					break;
			} else {
				// Search for SDL video driver string
				static const char sdl_str[] = "sdl";
				if (strncmp(line, sdl_str, sizeof(sdl_str) - 1) == 0) {
					char *p = line + sizeof(sdl_str);
					if (strstr(video_driver, p) == video_driver)
						video_driver_found = true;
				}
			}
		}

		// Keycode file completely read
		fclose(f);
		use_keycodes = video_driver_found;

		// Vendor not found? Then display warning
		if (!video_driver_found) {
			char str[256];
			snprintf(str, sizeof(str), GetString(STR_KEYCODE_VENDOR_WARN), video_driver, kc_path ? kc_path : KEYCODE_FILE_NAME);
			WarningAlert(str);
			return;
		}

		D(bug("Using SDL/%s keycodes table, %d key mappings\n", video_driver, n_keys));
	}
}

// Open display for current mode
bool SDL_monitor_desc::video_open(void)
{
	D(bug("video_open()\n"));
#if DEBUG
	const VIDEO_MODE &mode = get_current_mode();
	D(bug("Current video mode:\n"));
	D(bug(" %dx%d (ID %02x), %d bpp\n", VIDEO_MODE_X, VIDEO_MODE_Y, VIDEO_MODE_RESOLUTION, 1 << (VIDEO_MODE_DEPTH & 0x0f)));
#endif

	// Create display driver object of requested type
	drv = new(std::nothrow) driver_base(*this);
	if (drv == NULL)
		return false;
	drv->init();
	if (!drv->init_ok) {
		delete drv;
		drv = NULL;
		return false;
	}

#ifdef WIN32
	// Chain in a new message handler for WM_DEVICECHANGE
	HWND the_window = GetMainWindowHandle();
	sdl_window_proc = (WNDPROC)GetWindowLongPtr(the_window, GWLP_WNDPROC);
	SetWindowLongPtr(the_window, GWLP_WNDPROC, (LONG_PTR)windows_message_handler);
#endif

	// Initialize VideoRefresh function
	VideoRefreshInit();

	// Lock down frame buffer
	LOCK_FRAME_BUFFER;

	// Start redraw/input thread
#ifndef USE_CPU_EMUL_SERVICES
	redraw_thread_cancel = false;
	redraw_thread_active = ((redraw_thread = SDL_CreateThread(redraw_func, NULL)) != NULL);
	if (!redraw_thread_active) {
		printf("FATAL: cannot create redraw thread\n");
		return false;
	}
#else
	redraw_thread_active = true;
#endif
	return true;
}

#ifdef SHEEPSHAVER
bool VideoInit(void)
{
	const bool classic = false;
#else
bool VideoInit(bool classic)
{
#endif
	classic_mode = classic;

#ifdef ENABLE_VOSF
	// Zero the mainBuffer structure
	mainBuffer.dirtyPages = NULL;
	mainBuffer.pageInfo = NULL;
#endif

	// Create Mutexes
	if ((sdl_events_lock = SDL_CreateMutex()) == NULL)
		return false;
	if ((sdl_palette_lock = SDL_CreateMutex()) == NULL)
		return false;
	if ((frame_buffer_lock = SDL_CreateMutex()) == NULL)
		return false;

	// Init keycode translation
	keycode_init();

	// Read prefs
	frame_skip = PrefsFindInt32("frameskip");
	mouse_wheel_mode = PrefsFindInt32("mousewheelmode");
	mouse_wheel_lines = PrefsFindInt32("mousewheellines");

	// Get screen mode from preferences
	migrate_screen_prefs();
	const char *mode_str = NULL;
	if (classic_mode)
		mode_str = "win/512/342";
	else
		mode_str = PrefsFindString("screen");

	// Determine display type and default dimensions
	int default_width, default_height;
	if (classic) {
		default_width = 512;
		default_height = 384;
	}
	else {
		default_width = 640;
		default_height = 480;
	}
	display_type = DISPLAY_WINDOW;
	if (mode_str) {
		if (sscanf(mode_str, "win/%d/%d", &default_width, &default_height) == 2)
			display_type = DISPLAY_WINDOW;
		else if (sscanf(mode_str, "dga/%d/%d", &default_width, &default_height) == 2)
			display_type = DISPLAY_SCREEN;
	}
	if (default_width <= 0)
		default_width = sdl_display_width();
	else if (default_width > sdl_display_width())
		default_width = sdl_display_width();
	if (default_height <= 0)
		default_height = sdl_display_height();
	else if (default_height > sdl_display_height())
		default_height = sdl_display_height();

	// Mac screen depth follows X depth
	screen_depth = SDL_GetVideoInfo()->vfmt->BitsPerPixel;
	int default_depth;
	switch (screen_depth) {
	case 8:
		default_depth = VIDEO_DEPTH_8BIT;
		break;
	case 15: case 16:
		default_depth = VIDEO_DEPTH_16BIT;
		break;
	case 24: case 32:
		default_depth = VIDEO_DEPTH_32BIT;
		break;
	default:
		default_depth =  VIDEO_DEPTH_1BIT;
		break;
	}

	// Initialize list of video modes to try
	struct {
		int w;
		int h;
		int resolution_id;
	}
#ifdef SHEEPSHAVER
	// Omit Classic resolutions
	video_modes[] = {
		{   -1,   -1, 0x80 },
		{  640,  480, 0x81 },
		{  800,  600, 0x82 },
		{ 1024,  768, 0x83 },
		{ 1152,  870, 0x84 },
		{ 1280, 1024, 0x85 },
		{ 1600, 1200, 0x86 },
		{ 0, }
	};
#else
	video_modes[] = {
		{   -1,   -1, 0x80 },
		{  512,  384, 0x80 },
		{  640,  480, 0x81 },
		{  800,  600, 0x82 },
		{ 1024,  768, 0x83 },
		{ 1152,  870, 0x84 },
		{ 1280, 1024, 0x85 },
		{ 1600, 1200, 0x86 },
		{ 0, }
	};
#endif
	video_modes[0].w = default_width;
	video_modes[0].h = default_height;

	// Construct list of supported modes
	if (display_type == DISPLAY_WINDOW) {
		if (classic)
			add_mode(display_type, 512, 342, 0x80, 64, VIDEO_DEPTH_1BIT);
		else {
			for (int i = 0; video_modes[i].w != 0; i++) {
				const int w = video_modes[i].w;
				const int h = video_modes[i].h;
#ifdef SDL1_GFXACCEL
				if (i > 0 && ((w == default_width && h == default_height) ||
				              w > sdl_display_width() || h > sdl_display_height()))
					continue;
#else
				if (i > 0 && (w >= default_width || h >= default_height))
					continue;
#endif
				for (int d = VIDEO_DEPTH_1BIT; d <= default_depth; d++)
					add_mode(display_type, w, h, video_modes[i].resolution_id, TrivialBytesPerRow(w, (video_depth)d), d);
			}
		}
	} else if (display_type == DISPLAY_SCREEN) {
		for (int i = 0; video_modes[i].w != 0; i++) {
			const int w = video_modes[i].w;
			const int h = video_modes[i].h;
			if (i > 0 && (w >= default_width || h >= default_height))
				continue;
			if (w == 512 && h == 384)
				continue;
			for (int d = VIDEO_DEPTH_1BIT; d <= default_depth; d++)
				add_mode(display_type, w, h, video_modes[i].resolution_id, TrivialBytesPerRow(w, (video_depth)d), d);
		}
	}

	if (VideoModes.empty()) {
		ErrorAlert(STR_NO_XVISUAL_ERR);
		return false;
	}

	// Find requested default mode with specified dimensions
	uint32 default_id;
	std::vector<VIDEO_MODE>::const_iterator i, end = VideoModes.end();
	for (i = VideoModes.begin(); i != end; ++i) {
		const VIDEO_MODE & mode = (*i);
		if (VIDEO_MODE_X == default_width && VIDEO_MODE_Y == default_height && VIDEO_MODE_DEPTH == default_depth) {
			default_id = VIDEO_MODE_RESOLUTION;
#ifdef SHEEPSHAVER
			std::vector<VIDEO_MODE>::const_iterator begin = VideoModes.begin();
			cur_mode = distance(begin, i);
#endif
			break;
		}
	}
	if (i == end) { // not found, use first available mode
		const VIDEO_MODE & mode = VideoModes[0];
		default_depth = VIDEO_MODE_DEPTH;
		default_id = VIDEO_MODE_RESOLUTION;
#ifdef SHEEPSHAVER
		cur_mode = 0;
#endif
	}

#ifdef SHEEPSHAVER
	for (int i = 0; i < VideoModes.size(); i++)
		VModes[i] = VideoModes[i];
	VideoInfo *p = &VModes[VideoModes.size()];
	p->viType = DIS_INVALID;        // End marker
	p->viRowBytes = 0;
	p->viXsize = p->viYsize = 0;
	p->viAppleMode = 0;
	p->viAppleID = 0;
#endif

#if DEBUG
	D(bug("Available video modes:\n"));
	for (i = VideoModes.begin(); i != end; ++i) {
		const VIDEO_MODE & mode = (*i);
		int bits = 1 << VIDEO_MODE_DEPTH;
		if (bits == 16)
			bits = 15;
		else if (bits == 32)
			bits = 24;
		D(bug(" %dx%d (ID %02x), %d colors\n", VIDEO_MODE_X, VIDEO_MODE_Y, VIDEO_MODE_RESOLUTION, 1 << bits));
	}
#endif

	int color_depth = get_customized_color_depth(default_depth);

	D(bug("Return get_customized_color_depth %d\n", color_depth));

	// Create SDL_monitor_desc for this (the only) display
	SDL_monitor_desc *monitor = new SDL_monitor_desc(VideoModes, (video_depth)color_depth, default_id);
#ifdef WIN32
	monitor->desktopwidth = GetSystemMetrics(SM_CXSCREEN);
	monitor->desktopheight = GetSystemMetrics(SM_CYSCREEN);
#else
	monitor->desktopwidth = SDL_GetVideoInfo()->current_w;
	monitor->desktopheight = SDL_GetVideoInfo()->current_h;
#endif
	VideoMonitors.push_back(monitor);

	// Open display
	return monitor->video_open();
}


/*
 *  Deinitialization
 */

// Close display
void SDL_monitor_desc::video_close(void)
{
	D(bug("video_close()\n"));

#ifdef WIN32
	// Remove message handler for WM_DEVICECHANGE
	HWND the_window = GetMainWindowHandle();
	SetWindowLongPtr(the_window, GWLP_WNDPROC, (LONG_PTR)sdl_window_proc);
#endif

	// Stop redraw thread
#ifndef USE_CPU_EMUL_SERVICES
	if (redraw_thread_active) {
		redraw_thread_cancel = true;
		SDL_WaitThread(redraw_thread, NULL);
	}
#endif
	redraw_thread_active = false;

	// Unlock frame buffer
	UNLOCK_FRAME_BUFFER;
	D(bug(" frame buffer unlocked\n"));

	// Close display
	delete drv;
	drv = NULL;
}

void VideoExit(void)
{
#ifdef SDL1_GFXACCEL
	if (nqd_metal_available)
		NQDMetalCleanup();
	gfxaccel_resources_shutdown();
	MetalCompositorShutdown();
#endif
	// Close displays
	vector<monitor_desc *>::iterator i, end = VideoMonitors.end();
	for (i = VideoMonitors.begin(); i != end; ++i) {
		SDL_monitor_desc *monitor = dynamic_cast<SDL_monitor_desc *>(*i);
		monitor->hostfocused = false;
		monitor->ApplyGammaRamp();
		monitor->video_close();
	}

	// Destroy locks
	if (frame_buffer_lock)
		SDL_DestroyMutex(frame_buffer_lock);
	if (sdl_palette_lock)
		SDL_DestroyMutex(sdl_palette_lock);
	if (sdl_events_lock)
		SDL_DestroyMutex(sdl_events_lock);
}


/*
 *  Close down full-screen mode (if bringing up error alerts is unsafe while in full-screen mode)
 */

void VideoQuitFullScreen(void)
{
	D(bug("VideoQuitFullScreen()\n"));
	quit_full_screen = true;
}

static void do_toggle_fullscreen(void)
{
#ifndef USE_CPU_EMUL_SERVICES
	// pause redraw thread
	thread_stop_ack = false;
	thread_stop_req = true;
	while (!thread_stop_ack) ;
#endif

	// save the mouse position
	int x, y;
	SDL_GetMouseState(&x, &y);
	drv->HostToGuest(x, y);

	// save the screen contents
	SDL_Surface *tmp_surface = NULL;
	if (!drv->scaling)
		tmp_surface = SDL_ConvertSurface(drv->s, drv->s->format, drv->s->flags);

	// switch modes
	display_type = (display_type == DISPLAY_SCREEN) ? DISPLAY_WINDOW
		: DISPLAY_SCREEN;
	drv->set_video_mode(display_type == DISPLAY_SCREEN ? SDL_FULLSCREEN : 0);
	drv->adapt_to_video_mode();

	// reset the palette
#ifdef SHEEPSHAVER
	video_set_palette();
#endif
	drv->monitor.ApplyGammaRamp();
	drv->update_palette();

	// restore the screen contents
	if (tmp_surface != NULL) {
		SDL_BlitSurface(tmp_surface, NULL, drv->s, NULL);
		SDL_FreeSurface(tmp_surface);
	}
	update_sdl_video(drv->s, 0, 0, drv->VIDEO_MODE_X, drv->VIDEO_MODE_Y);
	drv->GuestToHost(x, y);

	// reset the video refresh handler
	VideoRefreshInit();

	// while SetVideoMode is happening, control key up may be missed
	ADBKeyUp(0x36);

	// restore the mouse position
	SDL_WarpMouse(x, y);

	// resume redraw thread
	toggle_fullscreen = false;
#ifndef USE_CPU_EMUL_SERVICES
	thread_stop_req = false;
#endif
}

/*
 *  Mac VBL interrupt
 */

/*
 *  Execute video VBL routine
 */

#ifdef SHEEPSHAVER
void VideoVBL(void)
{
	// Emergency quit requested? Then quit
	if (emerg_quit)
		QuitEmulator();

	if (toggle_fullscreen)
		do_toggle_fullscreen();

	// Setting the window name must happen on the main thread, else it doesn't work on
	// some platforms - e.g. macOS Sierra.
	if (mouse_grabbed_window_name_status != mouse_grabbed) {
	    set_window_name(mouse_grabbed);
	    mouse_grabbed_window_name_status = mouse_grabbed;
	}

#ifdef SDL1_GFXACCEL
	if (nqd_metal_available)
		NQDMetalFlush();
	if (MetalCompositorIsInitialized())
		MetalCompositorPresent();
#endif

	// Temporarily give up frame buffer lock (this is the point where
	// we are suspended when the user presses Ctrl-Tab)
	UNLOCK_FRAME_BUFFER;
	LOCK_FRAME_BUFFER;

	// Execute video VBL
	if (private_data != NULL && private_data->interruptsEnabled)
		VSLDoInterruptService(private_data->vslServiceID);
}
#else
void VideoInterrupt(void)
{
	// We must fill in the events queue in the same thread that did call SDL_SetVideoMode()
	SDL_PumpEvents();

	// Emergency quit requested? Then quit
	if (emerg_quit)
		QuitEmulator();

	if (toggle_fullscreen)
		do_toggle_fullscreen();

	// Setting the window name must happen on the main thread, else it doesn't work on
	// some platforms - e.g. macOS Sierra.
	if (mouse_grabbed_window_name_status != mouse_grabbed) {
		set_window_name(mouse_grabbed);
		mouse_grabbed_window_name_status = mouse_grabbed;
	}

	// Temporarily give up frame buffer lock (this is the point where
	// we are suspended when the user presses Ctrl-Tab)
	UNLOCK_FRAME_BUFFER;
	LOCK_FRAME_BUFFER;
}
#endif


/*
 *  Set palette
 */

#ifdef SHEEPSHAVER
void video_set_palette(void)
{
	monitor_desc * monitor = VideoMonitors[0];
	int n_colors = palette_size(monitor->get_current_mode().viAppleMode);
	uint8 pal[256 * 3];
	for (int c = 0; c < n_colors; c++) {
		pal[c*3 + 0] = mac_pal[c].red;
		pal[c*3 + 1] = mac_pal[c].green;
		pal[c*3 + 2] = mac_pal[c].blue;
	}
	monitor->set_palette(pal, n_colors);
}
#endif

void SDL_monitor_desc::set_palette(uint8 *pal, int num_in)
{
	const VIDEO_MODE &mode = get_current_mode();

#ifdef SDL1_GFXACCEL
	MetalCompositorUpdatePalette(pal, num_in);
	dmc_record_palette_change();
#endif
	if ((int)VIDEO_MODE_DEPTH > VIDEO_DEPTH_8BIT) {
		set_gamma(pal, num_in);
		return;
	}

	LOCK_PALETTE;

	// Convert colors to XColor array
	int num_out = 256;
	bool stretch = false;
	SDL_Color *p = sdl_palette;
	for (int i=0; i<num_out; i++) {
		int c = (stretch ? (i * num_in) / num_out : i);
		p->r = pal[c*3 + 0] * 0x0101;
		p->g = pal[c*3 + 1] * 0x0101;
		p->b = pal[c*3 + 2] * 0x0101;
		p++;
	}

	// Recalculate pixel color expansion map
	if (!IsDirectMode(mode)) {
		for (int i=0; i<256; i++) {
			int c = i & (num_in-1); // If there are less than 256 colors, we repeat the first entries (this makes color expansion easier)
			ExpandMap[i] = SDL_MapRGB(drv->s->format, pal[c*3+0], pal[c*3+1], pal[c*3+2]);
		}

#ifdef ENABLE_VOSF
		if (use_vosf) {
			// We have to redraw everything because the interpretation of pixel values changed
			LOCK_VOSF;
			PFLAG_SET_ALL;
			UNLOCK_VOSF;
			memset(the_buffer_copy, 0, VIDEO_MODE_ROW_BYTES * VIDEO_MODE_Y);
		}
#endif
	}

	// Tell redraw thread to change palette
	sdl_palette_changed = true;

	UNLOCK_PALETTE;
}

void SDL_monitor_desc::set_gamma(uint8 *gamma, int num_in)
{
	uint16 red[256];
	uint16 green[256];
	uint16 blue[256];
	const int repeats = 256 / num_in;

	if (gamma[0] == 127 && gamma[num_in * 3 - 1] == 127)
		return;

	for (int entry = 0; entry < num_in; entry++) {
		for (int repeat = 0; repeat < repeats; repeat++) {
			red[entry * repeats + repeat] = gamma[entry * 3 + 0] << 8;
			green[entry * repeats + repeat] = gamma[entry * 3 + 1] << 8;
			blue[entry * repeats + repeat] = gamma[entry * 3 + 2] << 8;
		}
	}
	for (int entry = num_in * repeats; entry < 256; entry++) {
		red[entry] = gamma[(num_in - 1) * 3] << 8;
		green[entry] = gamma[(num_in - 1) * 3 + 1] << 8;
		blue[entry] = gamma[(num_in - 1) * 3 + 2] << 8;
	}

	if (lastgammavalid && memcmp(red, lastgammared, sizeof(red)) == 0 &&
		memcmp(green, lastgammagreen, sizeof(green)) == 0 &&
		memcmp(blue, lastgammablue, sizeof(blue)) == 0)
		return;

	memcpy(lastgammared, red, sizeof(red));
	memcpy(lastgammagreen, green, sizeof(green));
	memcpy(lastgammablue, blue, sizeof(blue));
	lastgammavalid = true;
	ApplyGammaRamp();
}

void SDL_monitor_desc::ApplyGammaRamp(void)
{
	const char *gammamode = PrefsFindString("gammaramp");
	bool useguestgamma = false;
	int result;

	if (!initialgammavalid)
		initialgammavalid = SDL_GetGammaRamp(initialgammared, initialgammagreen, initialgammablue) == 0;
	if (hostfocused && lastgammavalid && gammamode != NULL && strcmp(gammamode, "off") != 0 &&
		(strcmp(gammamode, "fullscreen") != 0 || display_type == DISPLAY_SCREEN))
		useguestgamma = true;

	if (useguestgamma)
		result = SDL_SetGammaRamp(lastgammared, lastgammagreen, lastgammablue);
	else if (initialgammavalid)
		result = SDL_SetGammaRamp(initialgammared, initialgammagreen, initialgammablue);
	else
		return;
	if (result < 0)
		fprintf(stderr, "SDL_SetGammaRamp returned %d, SDL error: %s\n", result, SDL_GetError());
}

/*
 *  Switch video mode
 */

#ifdef SHEEPSHAVER
static void ResumeAfterModeSwitch(void)
{
	thread_stop_req = false;
	EnableInterrupt();
	video_screen_publish_cm_resume();
}

static int16 SwitchToModeIndex(int modeindex)
{
	const int previousmode = cur_mode;
#ifdef SDL1_GFXACCEL
	DMCModeDesc requestedmode;
	DMCModeDesc boundmode;
#endif
	video_screen_publish_cm_suspend();
	DisableInterrupt();
	thread_stop_ack = false;
	thread_stop_req = true;
	while (!thread_stop_ack) ;
#ifdef SDL1_GFXACCEL
	DMCModeDescFromVModesIndex(modeindex, &requestedmode);
	if (dmc_prepare_mode_switch(&requestedmode) != kDMCNoErr) {
		ResumeAfterModeSwitch();
		return paramErr;
	}
#endif
	cur_mode = modeindex;
	VideoMonitors[0]->switch_to_current_mode();
#ifdef SDL1_GFXACCEL
	DMCModeDescFromVModesIndex(cur_mode, &boundmode);
	boundmode.screen_base_mac = screen_base;
	if (screen_base != 0)
		boundmode.screen_base_host = Mac2HostAddr(screen_base);
	if (boundmode.screen_base_host == NULL) {
		cur_mode = previousmode;
		VideoMonitors[0]->switch_to_current_mode();
		dmc_cancel_prepared_mode_switch();
		ResumeAfterModeSwitch();
		return paramErr;
	}
	if (dmc_request_mode_switch(&boundmode) != kDMCNoErr) {
		cur_mode = previousmode;
		VideoMonitors[0]->switch_to_current_mode();
		ResumeAfterModeSwitch();
		return paramErr;
	}
#endif
	(void)previousmode;
	ResumeAfterModeSwitch();
	return noErr;
}

int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr)
{
	uint16 requestedmode = ReadMacInt16(ParamPtr + csMode);
	uint32 absolutemode = video_abs_depth_from_rel(requestedmode);
	if (absolutemode != 0)
		requestedmode = (uint16)absolutemode;

	if ((csSave->saveData == ReadMacInt32(ParamPtr + csData)) &&
	    (csSave->saveMode == requestedmode))
		return noErr;

	for (int i = 0; VModes[i].viType != DIS_INVALID; i++) {
		if (requestedmode == VModes[i].viAppleMode &&
		    ReadMacInt32(ParamPtr + csData) == VModes[i].viAppleID) {
			if (i != cur_mode) {
				int16 result = SwitchToModeIndex(i);
				if (result != noErr)
					return result;
			}
			WriteMacInt32(ParamPtr + csBaseAddr, screen_base);
			csSave->saveBaseAddr = screen_base;
			csSave->saveData = VModes[i].viAppleID;
			csSave->saveMode = VModes[i].viAppleMode;
			csSave->savePage = ReadMacInt16(ParamPtr + csPage);
			return noErr;
		}
	}
	return paramErr;
}
#endif

void SDL_monitor_desc::switch_to_current_mode(void)
{
	// Close and reopen display
	LOCK_EVENTS;
	video_close();
	video_open();
	UNLOCK_EVENTS;

	if (drv == NULL) {
		ErrorAlert(STR_OPEN_WINDOW_ERR);
		QuitEmulator();
	}
}


/*
 *  Can we set the MacOS cursor image into the window?
 */

#ifdef SHEEPSHAVER
bool video_can_change_cursor(void)
{
	if (display_type != DISPLAY_WINDOW || !PrefsFindBool("hardcursor"))
		return false;

#if defined(__APPLE__)
	static char driver[] = "Quartz?";
	static int quartzok = -1;

	if (quartzok < 0) {
		if (SDL_VideoDriverName(driver, sizeof driver) == NULL || strncmp(driver, "Quartz", sizeof driver))
			quartzok = true;
		else {
			// Quartz driver bug prevents cursor changing in SDL 1.2.11 to 1.2.14.
			const SDL_version *vp = SDL_Linked_Version();
			int version = SDL_VERSIONNUM(vp->major, vp->minor, vp->patch);
			quartzok = (version <= SDL_VERSIONNUM(1, 2, 10) || version >= SDL_VERSIONNUM(1, 2, 15));
		}
	}

	return quartzok;
#else
	return true;
#endif
}
#endif


/*
 *  Set cursor image for window
 */

#ifdef SHEEPSHAVER
void video_set_cursor(void)
{
	// Set new cursor image if it was changed
	if (sdl_cursor) {
		SDL_FreeCursor(sdl_cursor);
		sdl_cursor = SDL_CreateCursor(MacCursor + 4, MacCursor + 36, 16, 16, MacCursor[2], MacCursor[3]);
		if (sdl_cursor) {
			SDL_ShowCursor(private_data == NULL || private_data->cursorVisible);
			SDL_SetCursor(sdl_cursor);

			// XXX Windows apparently needs an extra mouse event to
			// make the new cursor image visible.
			// On Mac, if mouse is grabbed, SDL_ShowCursor() recenters the
			// mouse, we have to put it back.
			bool move = false;
#ifdef WIN32
			move = true;
#elif defined(__APPLE__)
			move = mouse_grabbed;
#endif
			if (move) {
				int visible = SDL_ShowCursor(-1);
				if (visible) {
					int x, y;
					SDL_GetMouseState(&x, &y);
					SDL_WarpMouse(x, y);
				}
			}
		}
	}
}
#endif


/*
 *  Keyboard-related utilify functions
 */

static bool is_modifier_key(SDL_KeyboardEvent const & e)
{
	switch (e.keysym.sym) {
	case SDLK_NUMLOCK:
	case SDLK_CAPSLOCK:
	case SDLK_SCROLLOCK:
	case SDLK_RSHIFT:
	case SDLK_LSHIFT:
	case SDLK_RCTRL:
	case SDLK_LCTRL:
	case SDLK_RALT:
	case SDLK_LALT:
	case SDLK_RMETA:
	case SDLK_LMETA:
	case SDLK_LSUPER:
	case SDLK_RSUPER:
	case SDLK_MODE:
	case SDLK_COMPOSE:
		return true;
	}
	return false;
}

static bool is_hotkey_down(SDL_keysym const & ks)
{
	int hotkey = PrefsFindInt32("hotkey");
	if (!hotkey) hotkey = 1;
	return (ctrl_down || (ks.mod & KMOD_CTRL) || !(hotkey & 1)) &&
			(opt_down || (ks.mod & KMOD_ALT) || !(hotkey & 2)) &&
			(cmd_down || (ks.mod & KMOD_META) || !(hotkey & 4));
}

static int modify_opt_cmd(int code) {
	static bool f, c;
	if (!f) {
		f = true;
		c = PrefsFindBool("swap_opt_cmd");
	}
	if (c) {
		switch (code) {
			case 0x37: return 0x3a;
			case 0x3a: return 0x37;
		}
	}
	return code;
}

/*
 *  Translate key event to Mac keycode, returns -1 if no keycode was found
 *  and -2 if the key was recognized as a hotkey
 */

static int kc_decode(SDL_keysym const & ks, bool key_down)
{
	switch (ks.sym) {
	case SDLK_a: return 0x00;
	case SDLK_b: return 0x0b;
	case SDLK_c: return 0x08;
	case SDLK_d: return 0x02;
	case SDLK_e: return 0x0e;
	case SDLK_f: return 0x03;
	case SDLK_g: return 0x05;
	case SDLK_h: return 0x04;
	case SDLK_i: return 0x22;
	case SDLK_j: return 0x26;
	case SDLK_k: return 0x28;
	case SDLK_l: return 0x25;
	case SDLK_m: return 0x2e;
	case SDLK_n: return 0x2d;
	case SDLK_o: return 0x1f;
	case SDLK_p: return 0x23;
	case SDLK_q: return 0x0c;
	case SDLK_r: return 0x0f;
	case SDLK_s: return 0x01;
	case SDLK_t: return 0x11;
	case SDLK_u: return 0x20;
	case SDLK_v: return 0x09;
	case SDLK_w: return 0x0d;
	case SDLK_x: return 0x07;
	case SDLK_y: return 0x10;
	case SDLK_z: return 0x06;

	case SDLK_1: case SDLK_EXCLAIM: return 0x12;
	case SDLK_2: case SDLK_AT: return 0x13;
	case SDLK_3: case SDLK_HASH: return 0x14;
	case SDLK_4: case SDLK_DOLLAR: return 0x15;
	case SDLK_5: return 0x17;
	case SDLK_6: return 0x16;
	case SDLK_7: return 0x1a;
	case SDLK_8: return 0x1c;
	case SDLK_9: return 0x19;
	case SDLK_0: return 0x1d;

	case SDLK_BACKQUOTE: return 0x0a;
	case SDLK_MINUS: case SDLK_UNDERSCORE: return 0x1b;
	case SDLK_EQUALS: case SDLK_PLUS: return 0x18;
	case SDLK_LEFTBRACKET: return 0x21;
	case SDLK_RIGHTBRACKET: return 0x1e;
	case SDLK_BACKSLASH: return 0x2a;
	case SDLK_SEMICOLON: case SDLK_COLON: return 0x29;
	case SDLK_QUOTE: case SDLK_QUOTEDBL: return 0x27;
	case SDLK_COMMA: case SDLK_LESS: return 0x2b;
	case SDLK_PERIOD: case SDLK_GREATER: return 0x2f;
	case SDLK_SLASH: case SDLK_QUESTION: return 0x2c;

	case SDLK_TAB: if (is_hotkey_down(ks)) {if (!key_down) drv->suspend(); return -2;} else return 0x30;
	case SDLK_RETURN: if (is_hotkey_down(ks)) {if (!key_down) toggle_fullscreen = true; return -2;} else return 0x24;
	case SDLK_SPACE: return 0x31;
	case SDLK_BACKSPACE: return 0x33;

	case SDLK_DELETE: return 0x75;
	case SDLK_INSERT: return 0x72;
	case SDLK_HOME: case SDLK_HELP: return 0x73;
	case SDLK_END: return 0x77;
	case SDLK_PAGEUP: return 0x74;
	case SDLK_PAGEDOWN: return 0x79;

	case SDLK_LCTRL: return 0x36;
	case SDLK_RCTRL: return 0x36;
	case SDLK_LSHIFT: return 0x38;
	case SDLK_RSHIFT: return 0x38;
	case SDLK_LALT: case SDLK_RALT: return 0x3a;
	case SDLK_LMETA: case SDLK_RMETA: return 0x37;
	case SDLK_LSUPER: case SDLK_RSUPER: return 0x37; // "Windows" key
	case SDLK_MENU: return 0x32;
	case SDLK_CAPSLOCK: return 0x39;
	case SDLK_NUMLOCK: return 0x47;

	case SDLK_UP: return 0x3e;
	case SDLK_DOWN: return 0x3d;
	case SDLK_LEFT: return 0x3b;
	case SDLK_RIGHT: return 0x3c;

	case SDLK_ESCAPE: if (is_hotkey_down(ks)) {if (!key_down) { quit_full_screen = true; emerg_quit = true; } return -2;} else return 0x35;

	case SDLK_F1: if (is_hotkey_down(ks)) {if (!key_down) SysMountFirstFloppy(); return -2;} else return 0x7a;
	case SDLK_F2: return 0x78;
	case SDLK_F3: return 0x63;
	case SDLK_F4: return 0x76;
	case SDLK_F5:
#ifdef SHEEPSHAVER
		if (is_hotkey_down(ks) && !video_can_change_cursor()) {if (!key_down) drv->toggle_mouse_grab(); return -2;} else return 0x60;
#else
		if (is_hotkey_down(ks)) {if (!key_down) drv->toggle_mouse_grab(); return -2;} else return 0x60;
#endif
	case SDLK_F6: return 0x61;
	case SDLK_F7: return 0x62;
	case SDLK_F8: return 0x64;
	case SDLK_F9: return 0x65;
	case SDLK_F10: return 0x6d;
	case SDLK_F11: return 0x67;
	case SDLK_F12: return 0x6f;

	case SDLK_PRINT: return 0x69;
	case SDLK_SCROLLOCK: return 0x6b;
	case SDLK_PAUSE: return 0x71;

	case SDLK_KP0: return 0x52;
	case SDLK_KP1: return 0x53;
	case SDLK_KP2: return 0x54;
	case SDLK_KP3: return 0x55;
	case SDLK_KP4: return 0x56;
	case SDLK_KP5: return 0x57;
	case SDLK_KP6: return 0x58;
	case SDLK_KP7: return 0x59;
	case SDLK_KP8: return 0x5b;
	case SDLK_KP9: return 0x5c;
	case SDLK_KP_PERIOD: return 0x41;
	case SDLK_KP_PLUS: return 0x45;
	case SDLK_KP_MINUS: return 0x4e;
	case SDLK_KP_MULTIPLY: return 0x43;
	case SDLK_KP_DIVIDE: return 0x4b;
	case SDLK_KP_ENTER: return 0x4c;
	case SDLK_KP_EQUALS: return 0x51;
	}
	D(bug("Unhandled SDL keysym: %d\n", ks.sym));
	return -1;
}

static int event2keycode(SDL_KeyboardEvent const &ev, bool key_down)
{
	return kc_decode(ev.keysym, key_down);
}

static void force_complete_window_refresh()
{
	if (display_type == DISPLAY_WINDOW) {
#ifdef ENABLE_VOSF
		if (use_vosf) {	// VOSF refresh
			LOCK_VOSF;
			PFLAG_SET_ALL;
			UNLOCK_VOSF;
		}
#endif
		// Ensure each byte of the_buffer_copy differs from the_buffer to force a full update.
		const VIDEO_MODE &mode = VideoMonitors[0]->get_current_mode();
		const int len = VIDEO_MODE_ROW_BYTES * VIDEO_MODE_Y;
		for (int i = 0; i < len; i++)
			the_buffer_copy[i] = !the_buffer[i];
	}
}

/*
 *  SDL event handling
 */

static void handle_events(void)
{
	SDL_Event events[10];
	const int n_max_events = sizeof(events) / sizeof(events[0]);
	int n_events;

	while ((n_events = SDL_PeepEvents(events, n_max_events, SDL_GETEVENT, sdl_eventmask)) > 0) {
		for (int i = 0; i < n_events; i++) {
			SDL_Event & event = events[i];
			switch (event.type) {
#ifdef ENABLE_EMULATOR_MONITOR
			default:
				if (drv != NULL && drv->emulatormonitor != NULL) {
					EmulatorMonitorView monitorview;
					monitorview.framebuffer = the_buffer;
					monitorview.palette = sdl_palette;
					monitorview.rowbytes = drv->VIDEO_MODE_ROW_BYTES;
					monitorview.hostsurface = drv->hostsurface;
					if (HostPresentationSuspended())
						monitorview.hostsurface = NULL;
					monitorview.window = NULL;
					video_get_host_window_size(&monitorview.hostwidth, &monitorview.hostheight);
					monitorview.width = drv->VIDEO_MODE_X;
					monitorview.height = drv->VIDEO_MODE_Y;
					monitorview.depth = 1 << (drv->VIDEO_MODE_DEPTH & 0x0f);
					drv->emulatormonitor->HandleEvent(event, monitorview);
				}
				break;
#endif

			// Mouse button
			case SDL_MOUSEBUTTONDOWN: {
				unsigned int button = event.button.button;
				if (button == SDL_BUTTON_LEFT)
					ADBMouseDown(0);
				else if (button == SDL_BUTTON_RIGHT)
					ADBMouseDown(1);
				else if (button == SDL_BUTTON_MIDDLE)
					ADBMouseDown(2);
				else if (button < 6) {	// Wheel mouse
					if (mouse_wheel_mode == 0) {
						int key = (button == 5) ? 0x79 : 0x74;	// Page up/down
						ADBKeyDown(key);
						ADBKeyUp(key);
					} else {
						int key = (button == 5) ? 0x3d : 0x3e;	// Cursor up/down
						for(int i=0; i<mouse_wheel_lines; i++) {
							ADBKeyDown(key);
							ADBKeyUp(key);
						}
					}
				}
				break;
			}
			case SDL_MOUSEBUTTONUP: {
				unsigned int button = event.button.button;
				if (button == SDL_BUTTON_LEFT)
					ADBMouseUp(0);
				else if (button == SDL_BUTTON_RIGHT)
					ADBMouseUp(1);
				else if (button == SDL_BUTTON_MIDDLE)
					ADBMouseUp(2);
				break;
			}

			// Mouse moved
			case SDL_MOUSEMOTION:
				drv->mouse_moved(event.motion.x, event.motion.y);
				break;

			// Keyboard
			case SDL_KEYDOWN: {
				int code = -1;
				if (use_keycodes && !is_modifier_key(event.key)) {
					if (event2keycode(event.key, true) != -2)	// This is called to process the hotkeys
						code = keycode_table[event.key.keysym.scancode & 0xff];
				} else
					code = event2keycode(event.key, true);
				if (code >= 0) {
					if (!emul_suspended) {
						if (code == 0x36) {
							ctrl_down = true;
						} else if (code == 0x3a) {
							opt_down = true;
						    code = modify_opt_cmd(code);
						} else if (code == 0x37) {
							cmd_down = true;
						    code = modify_opt_cmd(code);
						}
						if (code == 0x39) {	// Caps Lock pressed
							if (caps_on) {
								ADBKeyUp(code);
								caps_on = false;
							} else {
								ADBKeyDown(code);
								caps_on = true;
							}
						} else
							ADBKeyDown(code);
					} else {
						if (code == 0x31)
							drv->resume();	// Space wakes us up
					}
				}
				break;
			}
			case SDL_KEYUP: {
				int code = -1;
				if (use_keycodes && !is_modifier_key(event.key)) {
					if (event2keycode(event.key, false) != -2)	// This is called to process the hotkeys
						code = keycode_table[event.key.keysym.scancode & 0xff];
				} else
					code = event2keycode(event.key, false);
				if (code >= 0) {
					if (code == 0x36) {
						ctrl_down = false;
					} else if (code == 0x3a) {
						opt_down = false;
					    code = modify_opt_cmd(code);
					} else if (code == 0x37) {
						cmd_down = false;
					    code = modify_opt_cmd(code);
					}
					if (code == 0x39) {	// Caps Lock released
						if (caps_on) {
							ADBKeyUp(code);
							caps_on = false;
						} else {
							ADBKeyDown(code);
							caps_on = true;
						}
					} else
						ADBKeyUp(code);
				}
				break;
			}

			// Hidden parts exposed, force complete refresh of window
			case SDL_VIDEOEXPOSE:
				force_complete_window_refresh();
				break;

			// Window "close" widget clicked
			case SDL_QUIT:
				ADBKeyDown(0x7f);	// Power key
				ADBKeyUp(0x7f);
				break;

			// Application activate/deactivate
			case SDL_ACTIVEEVENT:
				// Force a complete window refresh when activating, to avoid redraw artifacts otherwise.
				if (event.active.gain)
					force_complete_window_refresh();
				if (event.active.state & SDL_APPINPUTFOCUS) {
					drv->monitor.hostfocused = event.active.gain != 0;
					drv->monitor.ApplyGammaRamp();
				}
				break;
			}
		}
	}
}


/*
 *  Window display update
 */

// Static display update (fixed frame rate, but incremental)
static void update_display_static(driver_base *drv)
{
	if (HostPresentationSuspended())
		return;
	// Incremental update code
	int wide = 0, high = 0;
	uint32 x1, x2, y1, y2;
	const VIDEO_MODE &mode = drv->mode;
	int bytes_per_row = VIDEO_MODE_ROW_BYTES;
	uint8 *p, *p2;

	// Check for first line from top and first line from bottom that have changed
	y1 = 0;
	for (uint32 j = 0; j < VIDEO_MODE_Y; j++) {
		if (memcmp(&the_buffer[j * bytes_per_row], &the_buffer_copy[j * bytes_per_row], bytes_per_row)) {
			y1 = j;
			break;
		}
	}
	y2 = y1 - 1;
	for (uint32 j = VIDEO_MODE_Y; j-- > y1; ) {
		if (memcmp(&the_buffer[j * bytes_per_row], &the_buffer_copy[j * bytes_per_row], bytes_per_row)) {
			y2 = j;
			break;
		}
	}
	high = y2 - y1 + 1;

	// Check for first column from left and first column from right that have changed
	if (high) {
		if (VIDEO_MODE_DEPTH < VIDEO_DEPTH_8BIT) {
			const int src_bytes_per_row = bytes_per_row;
			const int dst_bytes_per_row = drv->s->pitch;
			const int pixels_per_byte = VIDEO_MODE_X / src_bytes_per_row;

			x1 = VIDEO_MODE_X / pixels_per_byte;
			for (uint32 j = y1; j <= y2; j++) {
				p = &the_buffer[j * bytes_per_row];
				p2 = &the_buffer_copy[j * bytes_per_row];
				for (uint32 i = 0; i < x1; i++) {
					if (*p != *p2) {
						x1 = i;
						break;
					}
					p++; p2++;
				}
			}
			x2 = x1;
			for (uint32 j = y1; j <= y2; j++) {
				p = &the_buffer[j * bytes_per_row];
				p2 = &the_buffer_copy[j * bytes_per_row];
				p += bytes_per_row;
				p2 += bytes_per_row;
				for (uint32 i = (VIDEO_MODE_X / pixels_per_byte); i > x2; i--) {
					p--; p2--;
					if (*p != *p2) {
						x2 = i;
						break;
					}
				}
			}
			x1 *= pixels_per_byte;
			x2 *= pixels_per_byte;
			wide = (x2 - x1 + pixels_per_byte - 1) & -pixels_per_byte;

			// Update copy of the_buffer
			if (high && wide) {

				// Lock surface, if required
				if (SDL_MUSTLOCK(drv->s))
					SDL_LockSurface(drv->s);

				// Blit to screen surface
				int si = y1 * src_bytes_per_row + (x1 / pixels_per_byte);
				int di = y1 * dst_bytes_per_row + x1;
				for (uint32 j = y1; j <= y2; j++) {
					memcpy(the_buffer_copy + si, the_buffer + si, wide / pixels_per_byte);
					Screen_blit((uint8 *)drv->s->pixels + di, the_buffer + si, wide / pixels_per_byte);
					si += src_bytes_per_row;
					di += dst_bytes_per_row;
				}

				// Unlock surface, if required
				if (SDL_MUSTLOCK(drv->s))
					SDL_UnlockSurface(drv->s);

				// Refresh display
				update_sdl_video(drv->s, x1, y1, wide, high);
			}

		} else {
			const int bytes_per_pixel = VIDEO_MODE_ROW_BYTES / VIDEO_MODE_X;
			const int dst_bytes_per_row = drv->s->pitch;

			x1 = VIDEO_MODE_X;
			for (uint32 j = y1; j <= y2; j++) {
				p = &the_buffer[j * bytes_per_row];
				p2 = &the_buffer_copy[j * bytes_per_row];
				for (uint32 i = 0; i < x1 * bytes_per_pixel; i++) {
					if (*p != *p2) {
						x1 = i / bytes_per_pixel;
						break;
					}
					p++; p2++;
				}
			}
			x2 = x1;
			for (uint32 j = y1; j <= y2; j++) {
				p = &the_buffer[j * bytes_per_row];
				p2 = &the_buffer_copy[j * bytes_per_row];
				p += bytes_per_row;
				p2 += bytes_per_row;
				for (uint32 i = VIDEO_MODE_X * bytes_per_pixel; i > x2 * bytes_per_pixel; i--) {
					p--;
					p2--;
					if (*p != *p2) {
						x2 = i / bytes_per_pixel;
						break;
					}
				}
			}
			wide = x2 - x1;

			// Update copy of the_buffer
			if (high && wide) {

				// Lock surface, if required
				if (SDL_MUSTLOCK(drv->s))
					SDL_LockSurface(drv->s);

				// Blit to screen surface
				for (uint32 j = y1; j <= y2; j++) {
					uint32 i = j * bytes_per_row + x1 * bytes_per_pixel;
					int dst_i = j * dst_bytes_per_row + x1 * bytes_per_pixel;
					memcpy(the_buffer_copy + i, the_buffer + i, bytes_per_pixel * wide);
					Screen_blit((uint8 *)drv->s->pixels + dst_i, the_buffer + i, bytes_per_pixel * wide);
				}

				// Unlock surface, if required
				if (SDL_MUSTLOCK(drv->s))
					SDL_UnlockSurface(drv->s);

				// Refresh display
				update_sdl_video(drv->s, x1, y1, wide, high);
			}
		}
	}
}

// Static display update (fixed frame rate, bounding boxes based)
// XXX use NQD bounding boxes to help detect dirty areas?
static void update_display_static_bbox(driver_base *drv)
{
	if (HostPresentationSuspended())
		return;
	const VIDEO_MODE &mode = drv->mode;

	// Allocate bounding boxes for SDL_UpdateRects()
	const uint32 N_PIXELS = 64;
	const uint32 n_x_boxes = (VIDEO_MODE_X + N_PIXELS - 1) / N_PIXELS;
	const uint32 n_y_boxes = (VIDEO_MODE_Y + N_PIXELS - 1) / N_PIXELS;
	SDL_Rect *boxes = (SDL_Rect *)alloca(sizeof(SDL_Rect) * n_x_boxes * n_y_boxes);
	uint32 nr_boxes = 0;

	// Lock surface, if required
	if (SDL_MUSTLOCK(drv->s))
		SDL_LockSurface(drv->s);

	// Update the surface from Mac screen
	const uint32 bytes_per_row = VIDEO_MODE_ROW_BYTES;
	const uint32 bytes_per_pixel = bytes_per_row / VIDEO_MODE_X;
	const uint32 dst_bytes_per_row = drv->s->pitch;
	for (uint32 y = 0; y < VIDEO_MODE_Y; y += N_PIXELS) {
		uint32 h = N_PIXELS;
		if (h > VIDEO_MODE_Y - y)
			h = VIDEO_MODE_Y - y;
		for (uint32 x = 0; x < VIDEO_MODE_X; x += N_PIXELS) {
			uint32 w = N_PIXELS;
			if (w > VIDEO_MODE_X - x)
				w = VIDEO_MODE_X - x;
			const int xs = w * bytes_per_pixel;
			const int xb = x * bytes_per_pixel;
			bool dirty = false;
			for (uint32 j = y; j < (y + h); j++) {
				const uint32 yb = j * bytes_per_row;
				const uint32 dst_yb = j * dst_bytes_per_row;
				if (memcmp(&the_buffer[yb + xb], &the_buffer_copy[yb + xb], xs) != 0) {
					memcpy(&the_buffer_copy[yb + xb], &the_buffer[yb + xb], xs);
					Screen_blit((uint8 *)drv->s->pixels + dst_yb + xb, the_buffer + yb + xb, xs);
					dirty = true;
				}
			}
			if (dirty) {
				boxes[nr_boxes].x = x;
				boxes[nr_boxes].y = y;
				boxes[nr_boxes].w = w;
				boxes[nr_boxes].h = h;
				nr_boxes++;
			}
		}
	}

	// Unlock surface, if required
	if (SDL_MUSTLOCK(drv->s))
		SDL_UnlockSurface(drv->s);

	// Refresh display
	if (nr_boxes)
		update_sdl_video(drv->s, nr_boxes, boxes);
}


// We suggest the compiler to inline the next two functions so that it
// may specialise the code according to the current screen depth and
// display type. A clever compiler would do that job by itself though...

// NOTE: update_display_vosf is inlined too

static inline void possibly_quit_dga_mode()
{
	// Quit DGA mode if requested (something terrible has happened and we
	// want to give control back to the user)
	if (quit_full_screen) {
		quit_full_screen = false;
		delete drv;
		drv = NULL;
	}
}

static inline void possibly_ungrab_mouse()
{
	// Ungrab mouse if requested (something terrible has happened and we
	// want to give control back to the user)
	if (quit_full_screen) {
		quit_full_screen = false;
		if (drv)
			drv->ungrab_mouse();
	}
}

static inline void handle_palette_changes(void)
{
	LOCK_PALETTE;

	if (sdl_palette_changed) {
		sdl_palette_changed = false;
		drv->update_palette();
	}

	UNLOCK_PALETTE;
}

static void video_refresh_window_static(void);

static void video_refresh_dga(void)
{
	// Quit DGA mode if requested
	possibly_quit_dga_mode();
	video_refresh_window_static();
}

#ifdef ENABLE_VOSF
#if REAL_ADDRESSING || DIRECT_ADDRESSING
static void video_refresh_dga_vosf(void)
{
	// Quit DGA mode if requested
	possibly_quit_dga_mode();
	
	// Update display (VOSF variant)
	static uint32 tick_counter = 0;
	if (++tick_counter >= frame_skip) {
		tick_counter = 0;
		if (mainBuffer.dirty) {
			LOCK_VOSF;
			update_display_dga_vosf(drv);
			UNLOCK_VOSF;
		}
	}
}
#endif

static void video_refresh_window_vosf(void)
{
	// Ungrab mouse if requested
	possibly_ungrab_mouse();
	
	// Update display (VOSF variant)
	static uint32 tick_counter = 0;
	if (++tick_counter >= frame_skip) {
		tick_counter = 0;
		if (mainBuffer.dirty) {
			LOCK_VOSF;
			update_display_window_vosf(drv);
			UNLOCK_VOSF;
		}
	}
}
#endif // def ENABLE_VOSF

static void video_refresh_window_static(void)
{
	// Ungrab mouse if requested
	possibly_ungrab_mouse();

	// Update display (static variant)
	static uint32 tick_counter = 0;
	if (++tick_counter >= frame_skip) {
		tick_counter = 0;
		const VIDEO_MODE &mode = drv->mode;
		if ((int)VIDEO_MODE_DEPTH >= VIDEO_DEPTH_8BIT)
			update_display_static_bbox(drv);
		else
			update_display_static(drv);
	}
}


/*
 *  Thread for screen refresh, input handling etc.
 */

static void VideoRefreshInit(void)
{
	// TODO: set up specialised 8bpp VideoRefresh handlers ?
	if (display_type == DISPLAY_SCREEN) {
#if ENABLE_VOSF && (REAL_ADDRESSING || DIRECT_ADDRESSING)
		if (use_vosf)
			video_refresh = video_refresh_dga_vosf;
		else
#endif
			video_refresh = video_refresh_dga;
	}
	else {
#ifdef ENABLE_VOSF
		if (use_vosf)
			video_refresh = video_refresh_window_vosf;
		else
#endif
			video_refresh = video_refresh_window_static;
	}
}

static inline void do_video_refresh(void)
{
	// Handle SDL events
	handle_events();

	// Update display
	video_refresh();


	// Set new palette if it was changed
	handle_palette_changes();
}

// This function is called on non-threaded platforms from a timer interrupt
void VideoRefresh(void)
{
	// We need to check redraw_thread_active to inhibit refreshed during
	// mode changes on non-threaded platforms
	if (!redraw_thread_active)
		return;

	// Process pending events and update display
	do_video_refresh();
}

const int VIDEO_REFRESH_HZ = 60;
const int VIDEO_REFRESH_DELAY = 1000000 / VIDEO_REFRESH_HZ;

#ifndef USE_CPU_EMUL_SERVICES
static int redraw_func(void *arg)
{
	uint64 start = GetTicks_usec();
	int64 ticks = 0;
	uint64 next = GetTicks_usec() + VIDEO_REFRESH_DELAY;

	while (!redraw_thread_cancel) {

		// Wait
		next += VIDEO_REFRESH_DELAY;
		int32 delay = int32(next - GetTicks_usec());
		if (delay > 0)
			Delay_usec(delay);
		else if (delay < -VIDEO_REFRESH_DELAY)
			next = GetTicks_usec();
		ticks++;

		// Pause if requested (during video mode switches)
		if (thread_stop_req) {
			thread_stop_ack = true;
			continue;
		}

		// Process pending events and update display
		do_video_refresh();
	}

	uint64 end = GetTicks_usec();
	D(bug("%lld refreshes in %lld usec = %f refreshes/sec\n", ticks, end - start, ticks * 1000000.0 / (end - start)));
	return 0;
}
#endif


/*
 *  Record dirty area from NQD
 */

#ifdef SHEEPSHAVER
void video_set_dirty_area(int x, int y, int w, int h)
{
#ifdef ENABLE_VOSF
	const VIDEO_MODE &mode = drv->mode;
	const unsigned screen_width = VIDEO_MODE_X;
	const unsigned screen_height = VIDEO_MODE_Y;
	const unsigned bytes_per_row = VIDEO_MODE_ROW_BYTES;

	if (use_vosf) {
		vosf_set_dirty_area(x, y, w, h, screen_width, screen_height, bytes_per_row);
		return;
	}
#endif

	// XXX handle dirty bounding boxes for non-VOSF modes
}
#endif

void *video_get_native_window(void)
{
	SDL_SysWMinfo windowinfo;
	SDL_VERSION(&windowinfo.version);
	if (SDL_GetWMInfo(&windowinfo) <= 0)
		return NULL;
#if defined(WIN32)
	return (void *)windowinfo.window;
#elif defined(SDL_VIDEO_DRIVER_X11)
	return (void *)(uintptr)windowinfo.info.x11.window;
#else
	return NULL;
#endif
}

void video_get_host_window_size(int *width, int *height)
{
	*width = 0;
	*height = 0;
	if (drv != NULL && drv->hostsurface != NULL) {
		*width = drv->hostsurface->w;
		*height = drv->hostsurface->h;
	}
}

#ifdef SHEEPSHAVER
void video_set_gamma(int n_colors)
{
	monitor_desc *monitor = VideoMonitors[0];
	uint8 gamma[256 * 3];
	for (int color = 0; color < n_colors; color++) {
		gamma[color * 3 + 0] = mac_gamma[color].red;
		gamma[color * 3 + 1] = mac_gamma[color].green;
		gamma[color * 3 + 2] = mac_gamma[color].blue;
	}
	((SDL_monitor_desc *)monitor)->set_gamma(gamma, n_colors);
}

bool video_get_framebuffer_drawable_rect(int *outx, int *outy, int *outwidth, int *outheight)
{
	if (drv == NULL || drv->hostsurface == NULL)
		return false;
	*outx = drv->viewleft;
	*outy = drv->viewtop;
	*outwidth = drv->viewwidth;
	*outheight = drv->viewheight;
	return true;
}
#endif

#endif	// ends: SDL version check
