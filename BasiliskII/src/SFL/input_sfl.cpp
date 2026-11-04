#include "sysdeps.h"
#include "adb.h"
#include "sflui.h"

void VideoPumpEvents(void)
{
	SflUiPumpEvents();
}

void SflEmulatorKey(int code, int down)
{
	if (down)
		ADBKeyDown(code);
	else
		ADBKeyUp(code);
}

void SflEmulatorMouse(int x, int y)
{
	ADBMouseMoved(x, y);
}

void SflEmulatorButton(int down)
{
	SflEmulatorMouseButton(0, down);
}

void SflEmulatorMouseButton(int button, int down)
{
	if (down)
		ADBMouseDown(button);
	else
		ADBMouseUp(button);
}
