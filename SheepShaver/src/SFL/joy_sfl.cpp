/*
 *  joy_sfl.cpp - Joystick access of the SFL platform layer
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
#include "joymanager.h"
#include "gui.h"
#include "device.h"

#define SFLJOYMAXDEVICES 16
#define SFLJOYMAXAXES 8
#define SFLJOYPOVSECTOR 4500
#define SFLJOYPOVFULL 36000

struct JoyManagerDevice {
	WSJOYSTICK joystick;
	WSJOYSTICKCAPS caps;
	WSJOYSTICKSTATE state;
	int axistypes[SFLJOYMAXAXES];
	int axiscount;
	bool attached;
};

typedef struct SFLJOY {
	JoyManagerDevice *devices[SFLJOYMAXDEVICES];
	int count;
} SFLJOY;

static SFLJOY SflJoy;

static const int SflJoyAxisOrder[SFLJOYMAXAXES] = {
	WSJOYSTICK_GETAXIS_X, WSJOYSTICK_GETAXIS_Y, WSJOYSTICK_GETAXIS_RX, WSJOYSTICK_GETAXIS_RY,
	WSJOYSTICK_GETAXIS_RZ, WSJOYSTICK_GETAXIS_Z, WSJOYSTICK_GETAXIS_SLIDER1, WSJOYSTICK_GETAXIS_SLIDER2
};

static const uint8 SflJoyPovHats[8] = {
	JOYMANAGER_HAT_UP, JOYMANAGER_HAT_UP | JOYMANAGER_HAT_RIGHT, JOYMANAGER_HAT_RIGHT,
	JOYMANAGER_HAT_DOWN | JOYMANAGER_HAT_RIGHT, JOYMANAGER_HAT_DOWN, JOYMANAGER_HAT_DOWN | JOYMANAGER_HAT_LEFT,
	JOYMANAGER_HAT_LEFT, JOYMANAGER_HAT_UP | JOYMANAGER_HAT_LEFT
};

bool JoyManagerInit(void)
{
	return true;
}

int JoyManagerNumDevices(void)
{
	WSJOYSTICKITERATOR iterator;
	int count;
	count = 0;
	WSJoystickBegin(&iterator);
	while (!WSJoystickIsEnd(&iterator)) {
		count++;
		WSJoystickIterator_Next(&iterator);
	}
	WSJoystickIterator_Destroy(&iterator);
	return count;
}

JoyManagerDevice *JoyManagerOpenDevice(int index)
{
	WSJOYSTICKITERATOR iterator;
	JoyManagerDevice *device;
	int position;
	int order;
	if (SflJoy.count >= SFLJOYMAXDEVICES)
		return NULL;
	WSJoystickBegin(&iterator);
	for (position = 0; position < index && !WSJoystickIsEnd(&iterator); position++)
		WSJoystickIterator_Next(&iterator);
	if (WSJoystickIsEnd(&iterator)) {
		WSJoystickIterator_Destroy(&iterator);
		return NULL;
	}
	device = new JoyManagerDevice;
	WSJoystickIterator_Value(&iterator, &device->joystick);
	WSJoystickIterator_Destroy(&iterator);
	if (!WSJoystick_IsCreated(&device->joystick)) {
		delete device;
		return NULL;
	}
	WSJoystick_GetCaps(&device->joystick, &device->caps);
	device->axiscount = 0;
	for (order = 0; order < SFLJOYMAXAXES; order++) {
		if (WSJoystickCaps_HasAxis(&device->caps, 1 << SflJoyAxisOrder[order]))
			device->axistypes[device->axiscount++] = SflJoyAxisOrder[order];
	}
	device->attached = WSJoystick_Poll(&device->joystick, &device->state) != FALSE;
	SflJoy.devices[SflJoy.count++] = device;
	return device;
}

void JoyManagerCloseDevice(JoyManagerDevice *joystick)
{
	int index;
	WSASSERT(joystick != NULL);
	for (index = 0; index < SflJoy.count; index++) {
		if (SflJoy.devices[index] == joystick) {
			SflJoy.devices[index] = SflJoy.devices[--SflJoy.count];
			break;
		}
	}
	WSJoystick_Destroy(&joystick->joystick);
	delete joystick;
}

int JoyManagerNumButtons(JoyManagerDevice *joystick)
{
	return (int)WSJoystickCaps_GetButtonCount(&joystick->caps);
}

int JoyManagerNumAxes(JoyManagerDevice *joystick)
{
	return joystick->axiscount;
}

int JoyManagerNumHats(JoyManagerDevice *joystick)
{
	return (int)WSJoystickCaps_GetPOVCount(&joystick->caps);
}

bool JoyManagerHasRudderThrottle(JoyManagerDevice *joystick)
{
	return WSJoystickCaps_HasAxis(&joystick->caps, WSJOYSTICK_HASAXIS_RZ) != FALSE;
}

bool JoyManagerDeviceAttached(JoyManagerDevice *joystick)
{
	return joystick->attached;
}

const char *JoyManagerDeviceName(JoyManagerDevice *joystick, int index)
{
	return WSJoystickCaps_GetProductName(&joystick->caps, &joystick->joystick);
}

int16 JoyManagerAxis(JoyManagerDevice *joystick, int axis)
{
	axis_t minimum;
	axis_t maximum;
	axis_t current;
	int type;
	if (axis >= joystick->axiscount || !joystick->attached)
		return 0;
	type = joystick->axistypes[axis];
	minimum = WSJoystickCaps_GetAxisMin(&joystick->caps, type);
	maximum = WSJoystickCaps_GetAxisMax(&joystick->caps, type);
	current = WSJoystickState_GetAxisPosition(&joystick->state, type);
	if (maximum <= minimum)
		return 0;
	if (current < minimum)
		current = minimum;
	if (current > maximum)
		current = maximum;
	return (int16)((int32)(((uint64)(current - minimum) * 65535) / (maximum - minimum)) - 32768);
}

uint8 JoyManagerButton(JoyManagerDevice *joystick, int button)
{
	if (!joystick->attached)
		return 0;
	return WSJoystickState_GetButtonState(&joystick->state, button) != FALSE;
}

uint8 JoyManagerHat(JoyManagerDevice *joystick, int hat)
{
	axis_t minimum;
	axis_t maximum;
	axis_t current;
	axis_t angle;
	if (!joystick->attached)
		return 0;
	minimum = WSJoystickCaps_GetPOVMin(&joystick->caps, hat);
	maximum = WSJoystickCaps_GetPOVMax(&joystick->caps, hat);
	current = WSJoystickState_GetPOVPosition(&joystick->state, hat);
	if (WSJoystickStateIsPOVNonNullCentered(current, minimum, maximum))
		return 0;
	angle = WSJoystickStateGetPOVNonNullAngle(current, minimum, maximum,
		WSJoystickStateGetPOVNonNullGranularity(minimum, maximum));
	return SflJoyPovHats[((angle + SFLJOYPOVSECTOR / 2) % SFLJOYPOVFULL) / SFLJOYPOVSECTOR];
}

void JoyManagerUpdate(void)
{
	int index;
	for (index = 0; index < SflJoy.count; index++)
		SflJoy.devices[index]->attached =
			WSJoystick_Poll(&SflJoy.devices[index]->joystick, &SflJoy.devices[index]->state) != FALSE;
}
