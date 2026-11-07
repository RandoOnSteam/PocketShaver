#include "sysdeps.h"
#include "adb.h"
#include "prefs.h"
#include "sflui.h"

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
	if (SflUiQueueInput(SFLUIINPUTBUTTON, button, down))
		return;
	if (down)
		ADBMouseDown(button);
	else
		ADBMouseUp(button);
}
