/*
 *  video_sfl.cpp - Video driver of the SFL platform layer
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

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "prefs.h"
#include "macos_util.h"
#include "user_strings.h"
#include "video.h"
#include "video_defs.h"
#include "shears.h"
#include "sflmem.h"
#include "sflmain.h"
#include "sflui.h"
#if defined(ENABLE_GFXACCEL)
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"
#include "nqd_accel.h"
#endif

#define SFLVIDEOSIZES 6
#define SFLVIDEOPOWERKEY 0x7f
#define SFLVIDEODEFAULTWIDTH 640
#define SFLVIDEODEFAULTHEIGHT 480
#define SFLVIDEOMINWIDTH 512
#define SFLVIDEOMINHEIGHT 384
#define SFLVIDEOMAXRESOLUTIONS 10

typedef struct SFLVIDEOSIZE {
	int width;
	int height;
	int id;
} SFLVIDEOSIZE;

typedef struct SFLVIDEO {
	uint8 *buffer;
	uint32 buffersize;
} SFLVIDEO;

static SFLVIDEO SflVideo;

static const SFLVIDEOSIZE SflVideoSizes[SFLVIDEOSIZES] = {
	{640, 480, APPLE_640x480},
	{800, 600, APPLE_800x600},
	{1024, 768, APPLE_1024x768},
	{1152, 870, APPLE_1152x900},
	{1280, 1024, APPLE_1280x1024},
	{1600, 1200, APPLE_1600x1200}
};

static int SflVideoBits(uint32 mode)
{
	if (mode == APPLE_1_BIT)
		return 1;
	if (mode == APPLE_2_BIT)
		return 2;
	if (mode == APPLE_4_BIT)
		return 4;
	if (mode == APPLE_8_BIT)
		return 8;
	if (mode == APPLE_16_BIT)
		return 16;
	return 32;
}

static int SflVideoAppleId(int width, int height, int *custom)
{
	int index;
	for (index = 0; index < SFLVIDEOSIZES; index++) {
		if (SflVideoSizes[index].width == width && SflVideoSizes[index].height == height)
			return SflVideoSizes[index].id;
	}
	return APPLE_CUSTOM + (*custom)++;
}

static void SflVideoAddModes(VideoInfo **cursor, int width, int height, int *custom)
{
	VideoInfo *mode;
	uint32 depth;
	int appleid;
	appleid = SflVideoAppleId(width, height, custom);
	for (depth = APPLE_1_BIT; depth <= APPLE_32_BIT; depth++) {
		mode = *cursor;
		mode->viType = DIS_WINDOW;
		mode->viRowBytes = TrivialBytesPerRow(width, depth);
		mode->viXsize = width;
		mode->viYsize = height;
		mode->viAppleMode = depth;
		mode->viAppleID = appleid;
		(*cursor)++;
	}
}

static bool SflVideoMapBuffer(void)
{
	uint32 size;
	uint32 modesize;
	int index;
	size = 0;
	for (index = 0; VModes[index].viType != DIS_INVALID; index++) {
		modesize = VModes[index].viRowBytes * VModes[index].viYsize;
		if (modesize > size)
			size = modesize;
	}
	SflVideo.buffer = SflMemMap(SFLMAINVIDEOBASE, size);
	if (SflVideo.buffer == NULL)
		return false;
	SflVideo.buffersize = size;
	return true;
}

static void SflVideoOpenMode(int index)
{
	VideoInfo *mode;
	uint8 *frame;
	mode = &VModes[index];
	memset(SflVideo.buffer, 0, mode->viRowBytes * mode->viYsize);
	cur_mode = index;
	screen_base = SFLMAINVIDEOBASE;
	frame = SflVideo.buffer;
#if defined(ENABLE_GFXACCEL)
	if (MetalCompositorIsInitialized())
		frame = NULL;
#endif
	SflUiSetFrame(frame, mode->viXsize, mode->viYsize, mode->viRowBytes, SflVideoBits(mode->viAppleMode));
}

#if defined(ENABLE_GFXACCEL)
static void SflVideoModeDesc(int index, DMCModeDesc *description)
{
	description->width = (uint32_t)VModes[index].viXsize;
	description->height = (uint32_t)VModes[index].viYsize;
	description->depth = (uint32_t)SflVideoBits(VModes[index].viAppleMode);
	description->row_bytes = (uint32_t)VModes[index].viRowBytes;
	description->pitch = description->row_bytes;
	description->vbl_usec = 0;
	description->screen_base_mac = 0;
	description->screen_base_host = NULL;
}

static void SflVideoBoundMode(int index, DMCModeDesc *description)
{
	SflVideoModeDesc(index, description);
	description->screen_base_mac = screen_base;
	description->screen_base_host = Mac2HostAddr(screen_base);
}

static void SflVideoInitGfxAccel(void)
{
	static const uint8 blackandwhite[6] = {255, 255, 255, 0, 0, 0};
	DMCModeDesc initial;
	VideoInfo *mode;
	int pitch;
	mode = &VModes[cur_mode];
	pitch = mode->viXsize;
	if (mode->viAppleMode == APPLE_16_BIT)
		pitch <<= 1;
	else if (mode->viAppleMode == APPLE_32_BIT)
		pitch <<= 2;
	SflVideoBoundMode(cur_mode, &initial);
	if (dmc_create(&initial) != kDMCNoErr)
		return;
	if (MetalCompositorInit(mode->viXsize, mode->viYsize, mode->viAppleMode, mode->viRowBytes, pitch,
		SflVideo.buffer, SflVideo.buffersize) != 0) {
		MetalCompositorShutdown();
		return;
	}
	gfxaccel_resources_init();
	SflUiSetFrame(NULL, mode->viXsize, mode->viYsize, mode->viRowBytes, SflVideoBits(mode->viAppleMode));
	MetalCompositorUpdatePalette(blackandwhite, 2);
	dmc_record_palette_change();
}
#endif

static bool SflVideoSwitchMode(int index)
{
#if defined(ENABLE_GFXACCEL)
	DMCModeDesc requested;
	DMCModeDesc bound;
	int previous;
	previous = cur_mode;
	SflVideoModeDesc(index, &requested);
	if (dmc_prepare_mode_switch(&requested) != kDMCNoErr)
		return false;
	SflVideoOpenMode(index);
	SflVideoBoundMode(cur_mode, &bound);
	if (dmc_request_mode_switch(&bound) != kDMCNoErr) {
		SflVideoOpenMode(previous);
		return false;
	}
	return true;
#else
	SflVideoOpenMode(index);
	return true;
#endif
}

bool VideoInit(void)
{
	VideoInfo *cursor;
	const char *entry;
	int custom;
	int count;
	int listwidth;
	int listheight;
	const char *mode;
	int width;
	int height;
	int index;
	private_data = NULL;
	video_activated = true;
	display_type = DIS_WINDOW;
	width = SFLVIDEODEFAULTWIDTH;
	height = SFLVIDEODEFAULTHEIGHT;
	mode = PrefsFindString("screen");
	if (mode && sscanf(mode, "dga/%d/%d", &width, &height) == 2) {
		display_type = DIS_SCREEN;
		if (width <= 0 || height <= 0)
			SflUiGetDesktopSize(&width, &height);
	} else if (mode && sscanf(mode, "win/%d/%d", &width, &height) != 2) {
		width = SFLVIDEODEFAULTWIDTH;
		height = SFLVIDEODEFAULTHEIGHT;
	}
	if (width < SFLVIDEOMINWIDTH || height < SFLVIDEOMINHEIGHT) {
		width = SFLVIDEODEFAULTWIDTH;
		height = SFLVIDEODEFAULTHEIGHT;
	}
	cursor = VModes;
	custom = 0;
	SflVideoAddModes(&cursor, width, height, &custom);
	if (PrefsFindString("resolution") != NULL) {
		count = 1;
		for (index = 0; count < SFLVIDEOMAXRESOLUTIONS && (entry = PrefsFindString("resolution", index)) != NULL; index++) {
			if (sscanf(entry, "%dx%d", &listwidth, &listheight) != 2)
				continue;
			if (listwidth < SFLVIDEOMINWIDTH || listheight < SFLVIDEOMINHEIGHT || (listwidth == width && listheight == height))
				continue;
			SflVideoAddModes(&cursor, listwidth, listheight, &custom);
			count++;
		}
	} else {
		for (index = 0; index < SFLVIDEOSIZES; index++) {
			if (SflVideoSizes[index].width < width && SflVideoSizes[index].height < height)
				SflVideoAddModes(&cursor, SflVideoSizes[index].width, SflVideoSizes[index].height, &custom);
		}
	}
	cursor->viType = DIS_INVALID;
	cursor->viRowBytes = 0;
	cursor->viXsize = 0;
	cursor->viYsize = 0;
	cursor->viAppleMode = 0;
	cursor->viAppleID = 0;
	if (!SflVideoMapBuffer())
		return false;
	if (!SflUiOpen(GetString(STR_WINDOW_TITLE), width, height, display_type == DIS_SCREEN, PrefsFindInt32("frameskip"))) {
		ErrorAlert(GetString(STR_OPEN_WINDOW_ERR));
		return false;
	}
	ADBSetRelMouseMode(false);
	SflVideoOpenMode(APPLE_32_BIT - APPLE_1_BIT);
#if defined(ENABLE_GFXACCEL)
	SflVideoInitGfxAccel();
#endif
	return true;
}

void VideoExit(void)
{
#if defined(ENABLE_GFXACCEL)
	if (nqd_metal_available)
		NQDMetalCleanup();
	gfxaccel_resources_shutdown();
	MetalCompositorShutdown();
	dmc_shutdown();
#endif
	SflUiSetFrame(NULL, 0, 0, 0, 32);
}

void VideoQuitFullScreen(void)
{
	SflUiQuitFullScreen();
}

void VideoVBL(void)
{
#if defined(ENABLE_GFXACCEL)
	if (nqd_metal_available)
		NQDMetalFlush();
	if (MetalCompositorIsInitialized())
		MetalCompositorPresent();
#endif
	if (private_data != NULL && private_data->interruptsEnabled)
		VSLDoInterruptService(private_data->vslServiceID);
}

int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr)
{
	uint16 requested;
	uint32 absolute;
	int index;
	bool switched;
	requested = ReadMacInt16(ParamPtr + csMode);
	absolute = video_abs_depth_from_rel(requested);
	if (absolute != 0)
		requested = (uint16)absolute;
	if (csSave->saveData == ReadMacInt32(ParamPtr + csData) && csSave->saveMode == requested)
		return noErr;
	for (index = 0; VModes[index].viType != DIS_INVALID; index++) {
		if (requested != VModes[index].viAppleMode || ReadMacInt32(ParamPtr + csData) != VModes[index].viAppleID)
			continue;
		if (index != cur_mode) {
			video_screen_publish_cm_suspend();
			DisableInterrupt();
			switched = SflVideoSwitchMode(index);
			EnableInterrupt();
			video_screen_publish_cm_resume();
			if (!switched)
				return paramErr;
		}
		WriteMacInt32(ParamPtr + csBaseAddr, screen_base);
		csSave->saveBaseAddr = screen_base;
		csSave->saveData = VModes[index].viAppleID;
		csSave->saveMode = VModes[index].viAppleMode;
		csSave->savePage = ReadMacInt16(ParamPtr + csPage);
		return noErr;
	}
	return paramErr;
}

void video_set_palette(void)
{
	uint8 palette[256 * 3];
	int index;
	for (index = 0; index < 256; index++) {
		palette[index * 3] = mac_pal[index].red;
		palette[index * 3 + 1] = mac_pal[index].green;
		palette[index * 3 + 2] = mac_pal[index].blue;
	}
	SflUiSetPalette(palette, 256);
#if defined(ENABLE_GFXACCEL)
	MetalCompositorUpdatePalette(palette, 256);
	dmc_record_palette_change();
#endif
}

void video_set_gamma(int n_colors)
{
}

bool video_can_change_cursor(void)
{
	return false;
}

void video_set_cursor(void)
{
}

bool video_get_framebuffer_drawable_rect(int *outx, int *outy, int *outwidth, int *outheight)
{
	if (VModes[cur_mode].viXsize == 0)
		return false;
	*outx = 0;
	*outy = 0;
	*outwidth = VModes[cur_mode].viXsize;
	*outheight = VModes[cur_mode].viYsize;
	return true;
}

void video_set_dirty_area(int x, int y, int w, int h)
{
}

#if defined(ENABLE_GFXACCEL)
void *video_get_native_window(void)
{
	return SflUiGetWindowHandle();
}

void *video_get_gl_handle(void)
{
	return SflUiGetGLHandle();
}

void video_get_host_window_size(int *width, int *height)
{
	int framewidth;
	int frameheight;
	int framedepth;
	*width = 0;
	*height = 0;
	SflUiGetStatus(&framewidth, &frameheight, &framedepth, width, height);
}
#endif

void SflEmulatorClose(void)
{
	if (SflUiQueueInput(SFLUIINPUTQUIT, 0, 0))
		return;
	if (ShearsRequestShutdown())
		return;
	ADBKeyDown(SFLVIDEOPOWERKEY);
	ADBKeyUp(SFLVIDEOPOWERKEY);
}
