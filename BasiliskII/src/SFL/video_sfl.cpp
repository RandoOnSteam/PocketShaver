#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "clip.h"
#include "adb.h"
#include "prefs.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "video.h"
#include "sflui.h"

class SflMonitor : public monitor_desc {
public:
	SflMonitor(const vector<video_mode> &modes, video_depth depth, bool classic)
		: monitor_desc(modes, depth, 0x80), classicmode(classic)
	{
		uint32 limit = 0x00400000;
		if (ROMVersion == ROM_VERSION_II)
			limit = 0x00a00000;
		if (RAMSize < limit)
			limit = RAMSize;
		classicbase = limit - 0x5900;
	}

	void switch_to_current_mode(void)
	{
		const video_mode &mode = get_current_mode();
		int bits = 8 * mode.bytes_per_row / mode.x;
		memset(MacFrameBaseHost, 0, MacFrameSize);
		if (classicmode)
			set_mac_frame_base(classicbase);
		else
			set_mac_frame_base(MacFrameBaseMac);
		SflUiSetFrame(MacFrameBaseHost, mode.x, mode.y, mode.bytes_per_row, bits);
	}

	void set_palette(uint8 *palette, int count)
	{
		SflUiSetPalette(palette, count);
	}

	void set_gamma(uint8 *gamma, int count) {}

private:
	bool classicmode;
	uint32 classicbase;
};

bool VideoInit(bool classic)
{
	vector<video_mode> modes;
	video_mode mode;
	video_depth depth;
	const char *setting = PrefsFindString("screen");
	int width = 640;
	int height = 480;
	int fullscreen = 0;
	if (classic || TwentyFourBitAddressing) {
		width = 512;
		height = 342;
		depth = VDEPTH_1BIT;
	} else {
		depth = VDEPTH_32BIT;
		if (setting && sscanf(setting, "dga/%d/%d", &width, &height) == 2) {
			fullscreen = 1;
			if (width <= 0 || height <= 0)
				SflUiGetDesktopSize(&width, &height);
		} else if (setting) {
			sscanf(setting, "win/%d/%d", &width, &height);
		}
		if (width < 512 || height < 384 || width > 4096 || height > 4096) {
			width = 640;
			height = 480;
		}
		width = (width + 7) & ~7;
	}
	mode.x = width;
	mode.y = height;
	mode.resolution_id = 0x80;
	mode.user_data = 0;
	for (int index = VDEPTH_1BIT; index <= depth; index++) {
		mode.depth = (video_depth)index;
		mode.bytes_per_row = TrivialBytesPerRow(width, mode.depth);
		modes.push_back(mode);
	}
	MacFrameSize = mode.bytes_per_row * height;
	MacFrameBaseHost = (uint8 *)calloc(1, MacFrameSize + 0x10000);
	if (!MacFrameBaseHost)
		return false;
	if (!SflUiOpen(GetString(STR_WINDOW_TITLE), width, height, fullscreen, PrefsFindInt32("frameskip"))) {
		free(MacFrameBaseHost);
		MacFrameBaseHost = NULL;
		ErrorAlert(GetString(STR_OPEN_WINDOW_ERR));
		return false;
	}
	MacFrameLayout = FLAYOUT_DIRECT;
	SflMonitor *monitor = new SflMonitor(modes, depth, classic || TwentyFourBitAddressing);
	VideoMonitors.push_back(monitor);
	monitor->switch_to_current_mode();
	ADBSetRelMouseMode(false);
	return true;
}

void VideoExit(void)
{
	SflUiSetFrame(NULL, 0, 0, 0, 32);
	for (size_t index = 0; index < VideoMonitors.size(); index++)
		delete VideoMonitors[index];
	VideoMonitors.clear();
	free(MacFrameBaseHost);
	MacFrameBaseHost = NULL;
}

void VideoQuitFullScreen(void)
{
	SflUiQuitFullScreen();
}

void VideoInterrupt(void)
{
	ClipInterrupt();
}

void VideoRefresh(void)
{
	VideoPumpEvents();
}

void SflEmulatorClose(void)
{
	QuitEmulator();
}
