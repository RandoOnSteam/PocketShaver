#include "sysdeps.h"
#include "adb.h"
#include "sflui.h"

void VideoPumpEvents(void)
{
	SflUiPumpEvents();
}

void SflEmulatorKey(int code, int down)
{
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
