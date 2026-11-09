#include "sysdeps.h"
#include "adb.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "sflui.h"

#define SFLCURSORSTATEADDRESS 0x8d0

void VideoPumpEvents(void)
{
	SflUiPumpEvents();
}

void SflEmulatorKey(int code, int down)
{
	if (PrefsFindBool("swap_opt_cmd")) {
		if (code == 0x37)
			code = 0x3a;
		else if (code == 0x3a)
			code = 0x37;
	}
	if (SflUiQueueInput(SFLUIINPUTKEY, code, down))
		return;
	if (down)
		ADBKeyDown(code);
	else
		ADBKeyUp(code);
}

void SflEmulatorMouse(int x, int y)
{
	if (SflUiQueueInput(SFLUIINPUTMOUSE, x, y))
		return;
	ADBMouseMoved(x, y);
}

void SflEmulatorButton(int down)
{
	SflEmulatorMouseButton(0, down);
}

void SflEmulatorMouseButton(int button, int down)
{
	const char *modifier;
	int key;
	if (button == 1) {
		modifier = PrefsFindString("rightclick");
		key = 0x36;
		if (modifier && !strcmp(modifier, "command")) key = 0x37;
		if (down) SflEmulatorKey(key, 1);
		SflEmulatorMouseButton(0, down);
		if (!down) SflEmulatorKey(key, 0);
		return;
	}
	if (SflUiQueueInput(SFLUIINPUTBUTTON, button, down))
		return;
	if (down)
		ADBMouseDown(button);
	else
		ADBMouseUp(button);
}

void SflEmulatorWheel(int notches)
{
	int32 lines;
	int scrolldown;
	int key;
	int index;
	if (!notches || SflUiQueueInput(SFLUIINPUTWHEEL, notches, 0))
		return;
	lines = PrefsFindInt32("mousewheellines");
	scrolldown = notches < 0;
	if (lines < 0) {
		scrolldown = !scrolldown;
		lines = -lines;
	}
	if (PrefsFindInt32("mousewheelmode")) {
		key = 0x3e;
		if (scrolldown)
			key = 0x3d;
	} else {
		lines = 1;
		key = 0x74;
		if (scrolldown)
			key = 0x79;
	}
	for (index = 0; index < lines; index++) {
		ADBKeyDown(key);
		ADBKeyUp(key);
	}
}

int SflEmulatorRelativeMouse(int operation)
{
	const char *mode;
	bool always;
	bool automatic;
	mode = PrefsFindString("relmouse");
	always = mode != NULL && strcmp(mode, "always") == 0;
	automatic = mode != NULL && strcmp(mode, "auto") == 0;
	if (operation == SFLRELATIVESTART)
		ADBSetRelMouseMode(always || PrefsFindBool("init_grab"));
	else if (operation == SFLRELATIVETOGGLE && !always && !automatic)
		ADBSetRelMouseMode(!ADBIsRelativeMouseMode());
	else if (operation == SFLRELATIVEUPDATE && automatic)
		ADBSetRelMouseMode((int16)ReadMacInt16(SFLCURSORSTATEADDRESS) < 0);
	return ADBIsRelativeMouseMode();
}
