/*
 *  joymanager.cpp - SDL-backed replacement for the classic .JoyManager driver
 *
 *  The original driver returns pointers to driver-owned Mac memory.  In
 *  particular, clients read JoySimpleData and the live values in analogue
 *  JoyElement records directly, and consume JoyEventQueue without making a
 *  Device Manager call.
 * 
 *	(C) 2026 Ryan Norton (battlemageloveryt@gmail.com)
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
#include "macos_util.h"
#include "xlowmem.h"

#ifdef USE_SDL
#include "my_sdl.h"
#endif

#define DEBUG 0
#include "debug.h"

enum {
	JOY_MAX_DEVICES = 8,
	JOY_MAX_AXES = 8,
	JOY_MAX_BUTTONS = 64,
	JOY_MAX_HATS = 4,
	JOY_EVENT_COUNT = 64,
	JOY_AXIS_REST = 4000,
	JOY_GUEST_STORAGE_SIZE = 0x4000
};

enum {
	kJoyXAxisAvailable = 0x0001,
	kJoyYAxisAvailable = 0x0002,
	kJoyThrottleAvailable = 0x0008,
	kJoyRudderAvailable = 0x0010,
	kJoyGasAvailable = 0x0020,
	kJoyBrakeAvailable = 0x0040,
	kJoyXAndYAxisAvailable = 0x0100 /* Only checked by ISp and newer games */
};

enum {
	kJoyElemButton = 0,
	kJoyElemSelector = 1,
	kJoyElemUnpublished = 3,
	kJoyElemAxis = 10000,
	kJoyUnknownLabel = 0x7fff,
	kJoyLabelXAxis = 0,      /* 'xaxi' */
	kJoyLabelYAxis = 1,      /* 'yaxi' */
	kJoyLabelZAxis = 2,      /* 'zaxi' */
	kJoyLabelRxAxis = 3,     /* 'rxax' */
	kJoyLabelRyAxis = 4,     /* 'ryax' */
	kJoyLabelRzAxis = 5,     /* 'rzax' */
	kJoyLabelThrottle = 6,   /* 'thrt' */
	kJoyLabelRudder = 7,     /* 'rudd' */
	kJoyLabelGas = 8,        /* 'gasp' */
	kJoyLabelBrake = 9       /* 'brak' */
};

enum {
	kJoyEvtDown = 2,
	kJoyEvtUp = 3,
	kJoyEvtPosition = 4
};

enum {
	kJoyCsStart = 1002,
	kJoyCsStop = 1003,
	kJoyCsGetSimpleData = 1004,
	kJoyCsGetCount = 1005,
	kJoyCsGetInfo = 1006,
	kJoyCsEnableDevice = 1007,
	kJoyCsGetEventQueue = 1010,
	kJoyCsGetElementName = 1016
};

#if JOY_TRACE && defined(SHEEPSHAVER)
extern void USBHIDLog(const char *fmt, ...);
#define JoyTrace USBHIDLog
/* True once a second, for the periodic dumps. */
static bool JoyTraceTick(void)
{
	static uint64 next;
	uint64 now = GetTicks_usec();

	if (now < next)
		return false;
	next = now + 1000000;
	return true;
}
#else
static inline void JoyTrace(const char *, ...) { }
static inline bool JoyTraceTick(void) { return false; }
#endif

enum {
	joySimpleFeatures = 0x00,
	joySimpleAxis = 0x04,
	joySimpleHat = 0x14,
	joySimpleSize = 0x16,

	joyInfoName = 0x04,
	joyInfoFeatures = 0x28,
	joyInfoElementCount = 0x32,
	joyInfoElements = 0x34,
	joyInfoSize = 0x38,

	joyElementKind = 0x00,
	joyElementLabel = 0x02,
	joyElementMin = 0x08,
	joyElementMax = 0x0c,
	joyElementValue = 0x10,
	joyElementSize = 0x14,

	joyQueueBufStart = 0x00,
	joyQueueBufEnd = 0x04,
	joyQueueReadPtr = 0x0c,
	joyQueueWriteCount = 0x12,
	joyQueueReadCount = 0x14,
	joyQueueOverflow = 0x16,
	joyQueueSize = 0x18,

	joyEventWhen = 0x00,
	joyEventDevice = 0x04,
	joyEventElement = 0x06,
	joyEventWhat = 0x08,
	joyEventValue = 0x0a,
	joyEventSize = 0x0c
};

#ifdef USE_SDL
struct JoyHostDevice {
	SDL_Joystick *joystick;
	char name[36];
	uint32 simple_addr;
	uint32 info_addr;
	uint32 elements_addr;
	int axis_count;
	int simple_axis_count;
	int button_count;
	int hat_count;
	int button_element;
	int hat_element;
	int axis_element;
	int mirror_count; /* Rx/Ry mirror count */
	int mirror_element;  /* Rx/Ry mirror */
	bool rudder_throttle;
	bool enabled;
	uint8 buttons[JOY_MAX_BUTTONS];
	uint8 hats[JOY_MAX_HATS];
};
#endif

static uint32 joy_storage_addr;
static uint32 joy_simple_addr;
static uint32 joy_queue_addr;
static uint32 joy_event_buf_addr;
static int joy_device_count;
static int joy_start_count;
static bool joy_prepared;

#ifdef USE_SDL
static JoyHostDevice joy_devices[JOY_MAX_DEVICES];
#endif

uint32 JoyManagerAlignFour(uint32 addr)
{
	return (addr + 3) & ~3U;
}

uint32 JoyManagerGuestStorageSize(void)
{
	return JOY_GUEST_STORAGE_SIZE;
}

#ifdef USE_SDL
int JoyManagerSDLNumDevices(void)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int count;
	SDL_JoystickID *ids;

	count = 0;
	ids = SDL_GetJoysticks(&count);
	if (ids != NULL)
		SDL_free(ids);
	return count;
#else
	return SDL_NumJoysticks();
#endif
}

SDL_Joystick *JoyManagerSDLOpenDevice(int index)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int count;
	SDL_JoystickID *ids;
	SDL_Joystick *joystick;

	count = 0;
	joystick = NULL;
	ids = SDL_GetJoysticks(&count);
	if (ids != NULL && index >= 0 && index < count)
		joystick = SDL_OpenJoystick(ids[index]);
	if (ids != NULL)
		SDL_free(ids);
	return joystick;
#else
	return SDL_JoystickOpen(index);
#endif
}

void JoyManagerSDLCloseDevice(SDL_Joystick *joystick)
{
	if (joystick == NULL)
		return;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_CloseJoystick(joystick);
#else
	SDL_JoystickClose(joystick);
#endif
}

bool JoyManagerSDLDeviceAttached(SDL_Joystick *joystick)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return SDL_JoystickConnected(joystick);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_JoystickGetAttached(joystick) != SDL_FALSE;
#else
	return joystick != NULL;
#endif
}

bool JoyManagerSDLHasRudderThrottle(SDL_Joystick *joystick)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return SDL_GetJoystickType(joystick) == SDL_JOYSTICK_TYPE_FLIGHT_STICK;
#elif SDL_VERSION_ATLEAST(2, 0, 6)
	return SDL_JoystickGetType(joystick) == SDL_JOYSTICK_TYPE_FLIGHT_STICK;
#else
	(void)joystick;
	return true;
#endif
}

const char *JoyManagerSDLDeviceName(SDL_Joystick *joystick, int index)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	(void)index;
	return SDL_GetJoystickName(joystick);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	(void)index;
	return SDL_JoystickName(joystick);
#else
	(void)joystick;
	return SDL_JoystickName(index);
#endif
}

int JoyManagerSDLNumAxes(SDL_Joystick *joystick)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return SDL_GetNumJoystickAxes(joystick);
#else
	return SDL_JoystickNumAxes(joystick);
#endif
}

int JoyManagerSDLNumButtons(SDL_Joystick *joystick)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return SDL_GetNumJoystickButtons(joystick);
#else
	return SDL_JoystickNumButtons(joystick);
#endif
}

int JoyManagerSDLNumHats(SDL_Joystick *joystick)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return SDL_GetNumJoystickHats(joystick);
#else
	return SDL_JoystickNumHats(joystick);
#endif
}

int16 JoyManagerSDLAxis(SDL_Joystick *joystick, int axis)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return (int16)SDL_GetJoystickAxis(joystick, axis);
#else
	return (int16)SDL_JoystickGetAxis(joystick, axis);
#endif
}

uint8 JoyManagerSDLButton(SDL_Joystick *joystick, int button)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return (uint8)SDL_GetJoystickButton(joystick, button);
#else
	return SDL_JoystickGetButton(joystick, button);
#endif
}

uint8 JoyManagerSDLHat(SDL_Joystick *joystick, int hat)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	return (uint8)SDL_GetJoystickHat(joystick, hat);
#else
	return (uint8)SDL_JoystickGetHat(joystick, hat);
#endif
}

void JoyManagerSDLUpdate(void)
{
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_UpdateJoysticks();
#else
	SDL_JoystickUpdate();
#endif
}

bool JoyManagerSDLInit(void)
{
	bool initialized;

	initialized = (SDL_WasInit(SDL_INIT_JOYSTICK) & SDL_INIT_JOYSTICK) != 0;
	if (!initialized) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		initialized = SDL_InitSubSystem(SDL_INIT_JOYSTICK);
#else
		initialized = SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0;
#endif
	}
	if (!initialized)
		return false;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_SetJoystickEventsEnabled(false);
#else
	SDL_JoystickEventState(SDL_IGNORE);
#endif
	return true;
}
#endif

uint32 JoyManagerAxisFeature(int label)
{
	switch (label) {
		case kJoyLabelXAxis: return kJoyXAxisAvailable;
		case kJoyLabelYAxis: return kJoyYAxisAvailable;
		case kJoyLabelRudder: return kJoyRudderAvailable;
		case kJoyLabelThrottle: return kJoyThrottleAvailable;
		case kJoyLabelGas: return kJoyGasAvailable;
		case kJoyLabelBrake: return kJoyBrakeAvailable;
	}
	return 0;
}

uint32 JoyManagerFeaturesForAxes(int axes, bool rudder_throttle)
{
	uint32 features;
	int axis;

	features = 0;
	for (axis = 0; axis < axes && axis < JOY_MAX_AXES; axis++)
		features |= JoyManagerAxisFeature(
			JoyManagerAxisLabel(axis));
	return features;
}

/* Where JoySimpleData keeps each axis.  By label, not by position: the ISp
 * module that reads this block indexes it by role and does not read +0x08 at
 * all, so an axis stored at its ordinal would land in the wrong field or in
 * the hole. */
int JoyManagerSimpleOffset(int label)
{
	switch (label) {
		case kJoyLabelXAxis: return joySimpleAxis + 0x00;
		case kJoyLabelYAxis: return joySimpleAxis + 0x02;
		case kJoyLabelThrottle: return joySimpleAxis + 0x06;
		case kJoyLabelRudder: return joySimpleAxis + 0x08;
		case kJoyLabelGas: return joySimpleAxis + 0x0a;
		case kJoyLabelBrake: return joySimpleAxis + 0x0c;
	}
	return -1;
}

/* The layout is fixed, and it has to be: Descent II's JoyManager sampler does
 * not search by label, it reads elements 0..3 positionally and expects X, Y,
 * rudder, throttle there.  InputSprocket does search by label, but it accepts
 * this same set - a JoyManager 'rudd'/'thrt' pair is exactly what ISp expects
 * to see from a stick, and it maps them onto whatever needs a client asked
 * for.  So one layout serves both, and a twin-stick pad's right stick simply
 * arrives at ISp under the rudder and throttle labels.
 *
 * The two triggers are 'brak' and 'gasp', which Descent's four-label switch
 * skips and ISp knows as pedals.  They are declared 0..32767 and rest at 0,
 * their own min: a pedal that reads 0 when it is not pressed is correct, and
 * this is the one place resting at min is not the drift.
 *
 * Anything past the sixth axis has no label either mode looks for.  Published
 * as an axis it reached ISp as a 'none' element resting at exactly the
 * declared min, which CH scales to 0 - hard over, permanently, on whatever
 * need ISp bound it to, and that was the drift.  JoyManagerWriteDeviceInfo
 * publishes those with kJoyElemUnpublished instead, which CH's kind switch
 * drops, and appends separate 'rxax'/'ryax' mirror elements after the buttons
 * and hats so an ISp client that asks for the right stick by name still finds
 * it.  Descent never looks past element 3, so the mirrors are invisible to it.
 *
 * (rudder_throttle is the SDL probe result; it no longer selects a layout.
 * Labelling the right stick rxax/ryax here instead was tried and breaks
 * Descent, which then finds no rudder or throttle at all.) */
int JoyManagerAxisLabel(int axis)
{
	switch (axis) {
		case 0: return kJoyLabelXAxis;
		case 1: return kJoyLabelYAxis;
		case 2: return kJoyLabelRudder; /* Rx on twin sticks */
		case 3: return kJoyLabelThrottle; /* Ry on twin sticks */
		case 4: return kJoyLabelBrake; /* left trigger */
		case 5: return kJoyLabelGas; /* right trigger */
	}
	return kJoyUnknownLabel;
}

int32 JoyManagerAxisNeutralValue(int axis)
{ /* what the element reads when the device is gone: a stick centres at 0 and
   * a pedal rests at 0, the bottom of its own declared range; only the
   * throttle, declared 0..16384, sits mid-travel */
	if (JoyManagerAxisLabel(axis) == kJoyLabelThrottle)
		return 8192;
	return 0;
}

void JoyManagerWriteElement(uint32 addr, int kind, int label,
	int32 min_value, int32 max_value, int32 value)
{
	WriteMacInt16(addr + joyElementKind, kind);
	WriteMacInt16(addr + joyElementLabel, label);
	WriteMacInt32(addr + joyElementMin, min_value);
	WriteMacInt32(addr + joyElementMax, max_value);
	WriteMacInt32(addr + joyElementValue, value);
}

#ifdef USE_SDL
void JoyManagerWriteDeviceInfo(JoyHostDevice *device)
{
	uint32 element;
	uint32 features;
	int name_len;
	int i;

	features = JoyManagerFeaturesForAxes(device->simple_axis_count,
		device->rudder_throttle);
	name_len = (int)strlen(device->name);
	if (name_len > 35)
		name_len = 35;

	WriteMacInt8(device->info_addr + joyInfoName, name_len);
	if (name_len != 0)
		Host2Mac_memcpy(device->info_addr + joyInfoName + 1,
			device->name, name_len);
	WriteMacInt32(device->info_addr + joyInfoFeatures, features);
	WriteMacInt16(device->info_addr + joyInfoElementCount,
		device->axis_count + device->button_count + device->hat_count +
		device->mirror_count);
	WriteMacInt32(device->info_addr + joyInfoElements, device->elements_addr);

	element = device->elements_addr;
	for (i = 0; i < device->button_count; i++) {
		JoyManagerWriteElement(element, kJoyElemButton,
			kJoyUnknownLabel, 0, 1, 0);
		element += joyElementSize;
	}
	for (i = 0; i < device->hat_count; i++) {
		JoyManagerWriteElement(element, kJoyElemSelector,
			kJoyUnknownLabel, 0, 8, 0);
		element += joyElementSize;
	}	
	/* Descent II's JoyManager enumeration (Descent code 0x11c74..0x11dbc)
	 * walks this table and switches on JoyElement.label, taking exactly four:
	 *
	 *     label 0 -> slot 0 X        label 7 -> slot 2 Rudder
	 *     label 1 -> slot 1 Y        label 6 -> slot 3 Throttle
	 *
	 * and for each one it reads min (+0x08) and max (+0x0c) and keeps
	 * centre = (min + max) >> 1.  It does *not* ignore them, so min/max have
	 * to bound the values actually published and a self-centring axis has to
	 * rest at exactly that midpoint.  InputSprocket CH wants the same thing
	 * from the same fields - it scales (raw - min) * 0xffffffff / (max - min)
	 * - which is why one set of numbers satisfies both.
	 *
	 * Axes 4 and 5 are the triggers, published as 'brak' and 'gasp' over
	 * 0..32767.  Descent's switch takes only 0/1/7/6 so they stay invisible to
	 * it, while ISp gets two pedals resting at 0.
	 *
	 * Anything past that has no label either side looks for and is published
	 * with kJoyElemUnpublished, which CH's kind switch drops.  Left as an axis
	 * it reached ISp as a 'none' element resting at exactly min, which CH
	 * turns into 0 - hard over, permanently, on whatever need ISp bound it
	 * to. */
	for (i = 0; i < device->axis_count; i++) {
		int kind;
		int label;
		int32 min_value;
		int32 max_value;
		int32 neutral_value;

		label = JoyManagerAxisLabel(i);
		min_value = -32768;
		max_value = 32767;
		if (label == kJoyLabelThrottle) {
			min_value = 0;
			max_value = 16384;
		} else if (label == kJoyLabelGas || label == kJoyLabelBrake) {
			min_value = 0;
			max_value = 32767;
		}
		neutral_value = JoyManagerAxisNeutralValue(i);
		if (label == kJoyUnknownLabel)
			kind = kJoyElemUnpublished;
		else
			kind = kJoyElemAxis;
		JoyManagerWriteElement(element, kind, label,
			min_value, max_value, neutral_value);
		element += joyElementSize;
	}

	/* The right stick, a second time, as 'rxax' and 'ryax'.
	 *
	 * Isp and Descent II cannot be satisfied by one pair of labels.  Descent's
	 * JoyManager enumeration only recognises 0/1/7/6 (its code 0x11c74), so
	 * axes 2 and 3 have to be Rudder and Throttle or it never finds them.
	 * Descent's InputSprocket needs are 'zaxi' 'yaxi' 'xaxi' 'rzax' 'ryax'
	 * 'rxax' (its ISpInit table at 0xf74c), which those two match not at all,
	 * leaving ISp to fill them from whatever is spare.
	 *
	 * CH builds one row per element and takes the role from the label alone -
	 * there is no fixed axis order or count on that side - and it creates an
	 * ISpElement for every axis row whatever the label is (its 0xb14/0xb18).
	 * So publishing the same two SDL axes again under the labels ISp asks for
	 * gives each client an exact match and costs two elements.  They go after
	 * everything else, so no existing element index moves.
	 *
	 * It also sidesteps the one asymmetry in the pair we already publish: CH
	 * inverts an element labelled 'rudd' and nothing else (its 0x3860), so
	 * that axis necessarily reads opposite ways to the two clients.  'rxax'
	 * is passed through untouched. */
	for (i = 0; i < device->mirror_count; i++) {
		int label;

		if (i == 0)
			label = kJoyLabelRxAxis;
		else
			label = kJoyLabelRyAxis;
		JoyManagerWriteElement(element, kJoyElemAxis, label,
			-32768, 32767, 0);
		element += joyElementSize;
	}
}
#endif

void JoyManagerReset(void)
{
#ifdef USE_SDL
	int i;
#endif

#ifdef USE_SDL
	joy_prepared = false;
	for (i = 0; i < joy_device_count; i++) {
		JoyManagerSDLCloseDevice(joy_devices[i].joystick);
		joy_devices[i].joystick = NULL;
	}
	memset(joy_devices, 0, sizeof(joy_devices));
#else
	joy_prepared = false;
#endif
	joy_storage_addr = 0;
	joy_simple_addr = 0;
	joy_queue_addr = 0;
	joy_event_buf_addr = 0;
	joy_device_count = 0;
	joy_start_count = 0;
}

bool JoyManagerPrepare(void)
{
#ifdef USE_SDL
	int available;
	int i;

	JoyManagerReset();
	if (!JoyManagerSDLInit()) {
		D(bug("JoyManager: SDL joystick init failed: %s\n", SDL_GetError()));
		return false;
	}

	available = JoyManagerSDLNumDevices();
	if (available < 0)
		available = 0;
	if (available > JOY_MAX_DEVICES)
		available = JOY_MAX_DEVICES;

	for (i = 0; i < available; i++) {
		JoyHostDevice *device;
		SDL_Joystick *joystick;
		const char *name;
		int count;

		joystick = JoyManagerSDLOpenDevice(i);
		if (joystick == NULL)
			continue;

		device = &joy_devices[joy_device_count];
		memset(device, 0, sizeof(*device));
		device->joystick = joystick;
		device->rudder_throttle =
			JoyManagerSDLHasRudderThrottle(joystick);
		name = JoyManagerSDLDeviceName(joystick, i);
		if (name == NULL || name[0] == 0)
			name = "SDL Joystick";
		strncpy(device->name, name, sizeof(device->name) - 1);
		device->name[sizeof(device->name) - 1] = 0;

		count = JoyManagerSDLNumAxes(joystick);
		if (count < 0)
			count = 0;
		if (count > JOY_MAX_AXES) {
			device->axis_count = JOY_MAX_AXES;
		} else { 
			device->axis_count = count;
		}
		device->simple_axis_count = device->axis_count;
		if (device->simple_axis_count > 6)
			device->simple_axis_count = 6;

		count = JoyManagerSDLNumButtons(joystick);
		if (count < 0)
			count = 0;
		if (count > JOY_MAX_BUTTONS) {
			device->button_count = JOY_MAX_BUTTONS;
		} else {
			device->button_count = count;
		}

		count = JoyManagerSDLNumHats(joystick);
		if (count < 0)
			count = 0;
		if (count > JOY_MAX_HATS) {
			device->hat_count = JOY_MAX_HATS;
		} else { 
			device->hat_count = count;
		}
		device->button_element = 0;
		device->hat_element = device->button_count;
		device->axis_element = device->button_count + device->hat_count;
		if (device->axis_count > 3)
			device->mirror_count = 2;
		else if (device->axis_count > 2)
			device->mirror_count = 1;
		else
			device->mirror_count = 0;
		device->mirror_element = device->axis_element + device->axis_count;
		joy_device_count++;
	}

	joy_prepared = true;
	D(bug("JoyManager: found %d SDL joystick(s)\n", joy_device_count));
	return true;
#else
	JoyManagerReset();
	return false;
#endif
}

void JoyManagerResetQueue(void)
{
	if (joy_queue_addr == 0)
		return;

	Mac_memset(joy_event_buf_addr, 0, JOY_EVENT_COUNT * joyEventSize);
	WriteMacInt32(joy_queue_addr + joyQueueBufStart, joy_event_buf_addr);
	WriteMacInt32(joy_queue_addr + joyQueueBufEnd,
		joy_event_buf_addr + JOY_EVENT_COUNT * joyEventSize);
	WriteMacInt32(joy_queue_addr + joyQueueReadPtr, joy_event_buf_addr);
	WriteMacInt16(joy_queue_addr + joyQueueWriteCount, 0);
	WriteMacInt16(joy_queue_addr + joyQueueReadCount, 0);
	WriteMacInt8(joy_queue_addr + joyQueueOverflow, 0);
}

bool JoyManagerSetGuestStorage(uint32 addr, uint32 size)
{
#ifdef USE_SDL
	uint32 cursor;
	uint32 end;
	int i;

	if (!joy_prepared || addr == 0 || size < JOY_GUEST_STORAGE_SIZE)
		return false;

	joy_storage_addr = addr;
	Mac_memset(addr, 0, size);
	cursor = addr;
	end = addr + size;

	joy_simple_addr = cursor;
	cursor = JoyManagerAlignFour(cursor + joySimpleSize);
	joy_queue_addr = cursor;
	cursor = JoyManagerAlignFour(cursor + joyQueueSize);
	joy_event_buf_addr = cursor;
	cursor = JoyManagerAlignFour(cursor + JOY_EVENT_COUNT * joyEventSize);

	for (i = 0; i < joy_device_count; i++) {
		JoyHostDevice *device;
		uint32 element_bytes;
		uint32 simple_addr;
		uint32 info_addr;
		uint32 elements_addr;
		uint32 next_cursor;

		device = &joy_devices[i];
		element_bytes = (device->axis_count + device->button_count +
			device->hat_count + device->mirror_count) * joyElementSize;
		simple_addr = cursor;
		info_addr = JoyManagerAlignFour(simple_addr + joySimpleSize);
		elements_addr = JoyManagerAlignFour(info_addr + joyInfoSize);
		next_cursor = JoyManagerAlignFour(elements_addr + element_bytes);
		if (next_cursor > end) {
			JoyManagerReset();
			return false;
		}
		device->simple_addr = simple_addr;
		device->info_addr = info_addr;
		device->elements_addr = elements_addr;
		cursor = next_cursor;
		JoyManagerWriteDeviceInfo(device);
	}

	JoyManagerResetQueue();
	return true;
#else
	(void)addr;
	(void)size;
	return false;
#endif
}

#ifdef USE_SDL
int JoyManagerSDLHatPosition(uint8 hat)
{
	bool up;
	bool down;
	bool right;
	bool left;

	up = (hat & SDL_HAT_UP) != 0;
	down = (hat & SDL_HAT_DOWN) != 0;
	right = (hat & SDL_HAT_RIGHT) != 0;
	left = (hat & SDL_HAT_LEFT) != 0;
	if (up && left) return 5;
	if (up && right) return 6;
	if (down && left) return 7;
	if (down && right) return 8;
	if (up) return 1;
	if (down) return 2;
	if (left) return 3;
	if (right) return 4;
	return 0;
}

void JoyManagerPutEvent(int device_index, int element_index, int what, int value)
{
	uint16 write_count;
	uint16 read_count;
	uint16 pending;
	uint32 event_addr;

	if (joy_queue_addr == 0)
		return;
	write_count = ReadMacInt16(joy_queue_addr + joyQueueWriteCount);
	read_count = ReadMacInt16(joy_queue_addr + joyQueueReadCount);
	pending = (uint16)(write_count - read_count);
	if (pending >= JOY_EVENT_COUNT) {
		/* WriteMacInt8(joy_queue_addr + joyQueueOverflow, 1);
		 * Do not raise joyQueueOverflow.  ISp CH's dequeue
		 * clears the flag and returns -12345 without
		 * ever writing the caller's 12-byte event record, and the poll
		 * at code 0x375c reads `what` out of that still-uninitialised
		 * stack buffer and dispatches on it - so signalling overflow
		 * makes CH act on a garbage event.  Drop it instead. */
		return;
	}

	event_addr = joy_event_buf_addr + (write_count % JOY_EVENT_COUNT) * joyEventSize;
	WriteMacInt32(event_addr + joyEventWhen, ReadMacInt32(0x016a));
	WriteMacInt16(event_addr + joyEventDevice, device_index + 1);
	WriteMacInt16(event_addr + joyEventElement, element_index);
	WriteMacInt16(event_addr + joyEventWhat, what);
	WriteMacInt16(event_addr + joyEventValue, value);
	WriteMacInt16(joy_queue_addr + joyQueueWriteCount, write_count + 1);
	JoyTrace("joymgr event dev=%d elem=%d what=%d value=%d (pending %d)",
		device_index + 1, element_index, what, value, (int)pending + 1);
}

void JoyManagerApplyButtonState(int device_index, int button,
	uint8 value)
{
	JoyHostDevice *device;

	if (device_index < 0 || device_index >= joy_device_count)
		return;
	device = &joy_devices[device_index];
	if (!device->enabled || button < 0 || button >= device->button_count)
		return;
	value = value != 0;
	if (value == device->buttons[button])
		return;
	JoyManagerPutEvent(device_index, device->button_element + button,
		value ? kJoyEvtDown : kJoyEvtUp, 0);
	device->buttons[button] = value;
}

void JoyManagerApplyHatState(int device_index, int hat, uint8 value)
{
	JoyHostDevice *device;

	if (device_index < 0 || device_index >= joy_device_count)
		return;
	device = &joy_devices[device_index];
	if (!device->enabled || hat < 0 || hat >= device->hat_count)
		return;
	if (value == device->hats[hat])
		return;
	JoyManagerPutEvent(device_index, device->hat_element + hat,
		kJoyEvtPosition, JoyManagerSDLHatPosition(value));
	device->hats[hat] = value;
}

void JoyManagerSnapshotDevice(JoyHostDevice *device)
{
	int i;

	if (!JoyManagerSDLDeviceAttached(device->joystick)) {
		memset(device->buttons, 0, sizeof(device->buttons));
		memset(device->hats, 0, sizeof(device->hats));
		return;
	}
	for (i = 0; i < device->button_count; i++)
		device->buttons[i] = JoyManagerSDLButton(device->joystick, i);
	for (i = 0; i < device->hat_count; i++)
		device->hats[i] = JoyManagerSDLHat(device->joystick, i);
}

/* Treat a stick resting slightly off centre as centred, then stretch what is
   left so full deflection still reaches the ends. */
int32 JoyManagerAxisRest(int32 v)
{
	if (v > -JOY_AXIS_REST && v < JOY_AXIS_REST)
		return 0;
	if (v > 0)
		v -= JOY_AXIS_REST;
	else
		v += JOY_AXIS_REST;
	return v * 32768 / (32768 - JOY_AXIS_REST);
}

int32 JoyManagerAxisValue(JoyHostDevice *device, int axis)
{
	int32 value;
	int label;

	label = JoyManagerAxisLabel(axis);
	if (label == kJoyLabelGas || label == kJoyLabelBrake) {
		/* A trigger is not self-centring, so the deadzone in
		 * JoyManagerAxisRest() - which pulls values back toward 0 - does not
		 * apply to it.  SDL reports -32768 released to 32767 fully pressed;
		 * both the element (0..32767) and JoySimpleData, which the ISp module
		 * reads as min(v, 0x7fff) << 17, want that as 0..32767. */
		value = JoyManagerSDLAxis(device->joystick, axis);
		return (value + 32768) >> 1;
	}

	value = JoyManagerAxisRest(JoyManagerSDLAxis(device->joystick, axis));
	switch (label) {
		case kJoyLabelThrottle:
			value = (-value + 32770) >> 2;
			break;
		case kJoyLabelRudder: /* (0xffff - (v + 0x8000)). */
			value = -value;
			break;
		case kJoyLabelYAxis:
			value = -value; /* API reports up as positive */
			break;
	}
	return value;
}

bool JoyManagerWriteDeviceState(JoyHostDevice *device)
{ /* for JoyGetSimpleDataPtr() */
	bool active;
	uint32 features;
	int i;

	active = device->enabled &&
		JoyManagerSDLDeviceAttached(device->joystick);
	Mac_memset(device->simple_addr, 0, joySimpleSize);
	if(active) {
		features = JoyManagerFeaturesForAxes(device->simple_axis_count,
			device->rudder_throttle);
	} else {
		features = 0;
	}
	WriteMacInt32(device->simple_addr + joySimpleFeatures, features);

	for (i = 0; i < device->axis_count; i++) {
		int32 value;
		int off;

		if(active) {
			value = JoyManagerAxisValue(device, i);
		} else {
			value = JoyManagerAxisNeutralValue(i);
		}
		off = JoyManagerSimpleOffset(
			JoyManagerAxisLabel(i));
		if (active && off >= 0)
			WriteMacInt16(device->simple_addr + off, value);
		WriteMacInt32(device->elements_addr +
			(device->axis_element + i) * joyElementSize +
			joyElementValue, value);
	}
	for (i = 0; i < device->mirror_count; i++) {
		int32 value;

		/* Not JoyManagerAxisValue(): that applies the rudder negation and the
		   throttle rescale, and this element is declared -32768..32767 with
		   its centre at 0.  The mirror publishes the stick as it is. */
		if (active) {
			value = JoyManagerAxisRest(
				JoyManagerSDLAxis(device->joystick, 2 + i));
		} else {
			value = 0;
		}
		WriteMacInt32(device->elements_addr +
			(device->mirror_element + i) * joyElementSize +
			joyElementValue, value);
	}
	if (active && device->hat_count > 0)
		WriteMacInt16(device->simple_addr + joySimpleHat,
			JoyManagerSDLHatPosition(JoyManagerSDLHat(device->joystick, 0)));
	return active;
}

bool JoyManagerWriteElementName(uint32 addr, int device_index,
	int element_index)
{
	JoyHostDevice *device;
	char name[32];
	int axis;
	int button;
	int hat;

	if (addr == 0 || device_index < 1 || device_index > joy_device_count)
		return false;
	device = &joy_devices[device_index - 1];
	if (element_index < 0 || element_index >= device->axis_count +
		device->button_count + device->hat_count + device->mirror_count)
		return false;

	button = element_index - device->button_element;
	hat = element_index - device->hat_element;
	axis = element_index - device->axis_element;
	if (button >= 0 && button < device->button_count) {
		sprintf(name, "Button %d", button + 1);
	} else if (hat >= 0 && hat < device->hat_count) {
		sprintf(name, "Hat %d", hat + 1);
	} else if (axis >= 0 && axis < device->axis_count) {
		switch (JoyManagerAxisLabel(axis)) {
			case kJoyLabelXAxis: strcpy(name, "X Axis"); break;
			case kJoyLabelYAxis: strcpy(name, "Y Axis"); break;
			case kJoyLabelRxAxis: strcpy(name, "Rx Axis"); break;
			case kJoyLabelRyAxis: strcpy(name, "Ry Axis"); break;
			case kJoyLabelRudder: strcpy(name, "Rudder"); break;
			case kJoyLabelThrottle: strcpy(name, "Throttle"); break;
			case kJoyLabelGas: strcpy(name, "Gas"); break;
			case kJoyLabelBrake: strcpy(name, "Brake"); break;
			default: sprintf(name, "Axis %d", axis + 1); break;
		}
	} else if (element_index - device->mirror_element >= 0 &&
		element_index - device->mirror_element < device->mirror_count) {
		if (element_index == device->mirror_element)
			strcpy(name, "Rx Axis");
		else
			strcpy(name, "Ry Axis");
	} else {
		sprintf(name, "Element %d", element_index);
	}
	Host2Mac_memcpy(addr, name, strlen(name) + 1);
	return true;
}

void JoyManagerUpdateState(void)
{
	JoyHostDevice *shared_device;
	JoyHostDevice *fallback_device;
	int i;

	if (joy_simple_addr == 0)
		return;

	shared_device = NULL;
	fallback_device = NULL;
	for (i = 0; i < joy_device_count; i++) {
		JoyHostDevice *device;

		device = &joy_devices[i];
		if (!JoyManagerWriteDeviceState(device))
			continue;
		if (fallback_device == NULL)
			fallback_device = device;
		if (shared_device == NULL && device->simple_axis_count > 0)
			shared_device = device;
	}

	if (shared_device == NULL)
		shared_device = fallback_device;
	Mac_memset(joy_simple_addr, 0, joySimpleSize);
	if (shared_device != NULL)
		Mac2Mac_memcpy(joy_simple_addr, shared_device->simple_addr,
			joySimpleSize);

	if (JoyTraceTick()) { /* Everything InputSprocket can see */
		int i;

		JoyTrace("joymgr global simple @%08x feat=%08x x=%d y=%d thr=%d rud=%d"
			" gas=%d brk=%d hat=%d (shared=%s)", joy_simple_addr,
			ReadMacInt32(joy_simple_addr + 0x00),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x04),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x06),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x0a),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x0c),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x0e),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x10),
			(int)(int16)ReadMacInt16(joy_simple_addr + 0x14),
			shared_device != NULL ? shared_device->name : "none");
		if (joy_queue_addr != 0)
			JoyTrace("joymgr queue write=%d read=%d overflow=%d start_count=%d",
				(int)ReadMacInt16(joy_queue_addr + joyQueueWriteCount),
				(int)ReadMacInt16(joy_queue_addr + joyQueueReadCount),
				(int)ReadMacInt8(joy_queue_addr + joyQueueOverflow),
				joy_start_count);
		for (i = 0; i < joy_device_count; i++) {
			JoyHostDevice *device = &joy_devices[i];
			char raw[320];
			int len = 0;
			int n;

			JoyTrace("joymgr dev %d '%s' enabled=%d attached=%d axes=%d"
				" (simple %d) btns=%d hats=%d rt=%d info=%08x elems=%08x"
				" base(btn %d hat %d axis %d)", i + 1, device->name,
				(int)device->enabled,
				(int)JoyManagerSDLDeviceAttached(device->joystick),
				device->axis_count, device->simple_axis_count,
				device->button_count, device->hat_count,
				(int)device->rudder_throttle, device->info_addr,
				device->elements_addr, device->button_element,
				device->hat_element, device->axis_element);

			raw[0] = 0;
			for (n = 0; n < device->axis_count && len < 250; n++)
				len += sprintf(raw + len, " a%d=%d", n,
					(int)JoyManagerSDLAxis(device->joystick, n));
			for (n = 0; n < device->hat_count && len < 280; n++)
				len += sprintf(raw + len, " h%d=%02x", n,
					JoyManagerSDLHat(device->joystick, n));
			JoyTrace("  raw sdl%s", raw);
			len = 0;
			raw[0] = 0;
			for (n = 0; n < device->button_count && len < 250; n++)
				len += sprintf(raw + len, "%d",
					JoyManagerSDLButton(device->joystick, n) ? 1 : 0);
			JoyTrace("  raw btn %s", raw);
			JoyTrace("  info feat=%08x elemcount=%d",
				ReadMacInt32(device->info_addr + joyInfoFeatures),
				(int)(int16)ReadMacInt16(device->info_addr +
					joyInfoElementCount));
			JoyTrace("  dev simple feat=%08x x=%d y=%d thr=%d rud=%d gas=%d"
				" brk=%d hat=%d", ReadMacInt32(device->simple_addr + 0x00),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x04),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x06),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x0a),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x0c),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x0e),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x10),
				(int)(int16)ReadMacInt16(device->simple_addr + 0x14));
			for (n = 0; n < device->axis_count + device->button_count +
					device->hat_count; n++) {
				uint32 e = device->elements_addr + n * joyElementSize;
				int kind = (int16)ReadMacInt16(e + joyElementKind);
				/* Idle buttons say nothing; axes and hats always do. */
				if (kind == kJoyElemButton &&
						ReadMacInt32(e + joyElementValue) == 0)
					continue;
				JoyTrace("  elem %2d kind=%d label=%d min=%d max=%d value=%d",
					n, kind, (int)(int16)ReadMacInt16(e + joyElementLabel),
					(int)(int32)ReadMacInt32(e + joyElementMin),
					(int)(int32)ReadMacInt32(e + joyElementMax),
					(int)(int32)ReadMacInt32(e + joyElementValue));
			}
		}
	}
}
#endif

void JoyManagerVBL(void)
{
#ifdef USE_SDL
	int i;

	if (!joy_prepared || joy_storage_addr == 0)
		return;
	JoyManagerSDLUpdate();
	JoyManagerUpdateState();
	if (joy_start_count == 0)
		return;

	for (i = 0; i < joy_device_count; i++) {
		JoyHostDevice *device;
		bool attached;
		int j;

		device = &joy_devices[i];
		if (!device->enabled)
			continue;
		attached = JoyManagerSDLDeviceAttached(device->joystick);
		for (j = 0; j < device->button_count; j++) {
			uint8 value;
			if(attached) {
				value = JoyManagerSDLButton(device->joystick, j);
			} else {
				value = 0;
			}
			JoyManagerApplyButtonState(i, j, value);
		}
		for (j = 0; j < device->hat_count; j++) {
			uint8 value;

			value = attached ? JoyManagerSDLHat(device->joystick, j) : 0;
			JoyManagerApplyHatState(i, j, value);
		}
	}
#endif
}

bool JoyManagerWriteOutWord(uint32 pb, int value)
{
	uint32 result;

	result = ReadMacInt32(pb + csParam);
	if (result == 0)
		return false;
	WriteMacInt16(result, value);
	return true;
}

bool JoyManagerWriteOutPtr(uint32 pb, uint32 value)
{
	uint32 result;

	result = ReadMacInt32(pb + csParam);
	if (result == 0)
		return false;
	WriteMacInt32(result, value);
	return true;
}

int16 JoyManagerOpen(uint32 pb, uint32 dce)
{
	(void)pb;
	if (!joy_prepared || joy_storage_addr == 0)
		return openErr;
	WriteMacInt32(dce + dCtlPosition, 0);
	return noErr;
}

int16 JoyManagerControl(uint32 pb, uint32 dce)
{
	int16 code;

	(void)dce;
	code = (int16)ReadMacInt16(pb + csCode);
	D(bug("JoyManagerControl %d\n", code));
	/* Who is talking to the driver, and about which device. This is what says
	   whether ISp Joy is on the simple-data path (1004) or ISp CH is on the
	   element path (1006), and which index it enabled (1007). */
	JoyTrace("joymgr csCode %d params=%04x %04x %04x start_count=%d", code,
		ReadMacInt16(pb + csParam + 4), ReadMacInt16(pb + csParam + 6),
		ReadMacInt16(pb + csParam + 8), joy_start_count);
	switch (code) {
		case kJoyCsStart:
#ifdef USE_SDL
			JoyManagerSDLUpdate();
#endif
			if (joy_start_count == 0) {
				JoyManagerResetQueue();
#ifdef USE_SDL
				{
					int i;
					for (i = 0; i < joy_device_count; i++)
						JoyManagerSnapshotDevice(&joy_devices[i]);
				}
#endif
			}
			if (joy_start_count < 0x7fff)
				joy_start_count++;
#ifdef USE_SDL
			JoyManagerUpdateState();
#endif
			return noErr;

		case kJoyCsStop:
			if (joy_start_count > 0)
				joy_start_count--;
			return JoyManagerWriteOutWord(pb, joy_start_count) ? noErr : paramErr;

		case kJoyCsGetSimpleData:
			return JoyManagerWriteOutPtr(pb, joy_simple_addr) ? noErr : paramErr;

		case kJoyCsGetCount:
			return JoyManagerWriteOutWord(pb, joy_device_count) ? noErr : paramErr;

		case kJoyCsGetInfo: {
			int index;
			uint32 info;

			index = (int16)ReadMacInt16(pb + csParam + 4);
			info = 0;
#ifdef USE_SDL
			if (index >= 1 && index <= joy_device_count)
				info = joy_devices[index - 1].info_addr;
#endif
			return JoyManagerWriteOutPtr(pb, info) ? noErr : paramErr;
		}

		case kJoyCsEnableDevice: {
			int index;
			bool enable;
			int result;

			index = (int16)ReadMacInt16(pb + csParam + 4);
			enable = ReadMacInt8(pb + csParam + 6) != 0;
			result = noErr;
#ifdef USE_SDL
			JoyManagerSDLUpdate();
			if (index == 0) {
				int i;
				for (i = 0; i < joy_device_count; i++) {
					joy_devices[i].enabled = enable;
					JoyManagerSnapshotDevice(&joy_devices[i]);
				}
			} else if (index >= 1 && index <= joy_device_count) {
				joy_devices[index - 1].enabled = enable;
				JoyManagerSnapshotDevice(&joy_devices[index - 1]);
			} else {
				result = paramErr;
			}
#else
			if (index != 0)
				result = paramErr;
#endif
#ifdef USE_SDL
			JoyManagerUpdateState();
#endif
			return JoyManagerWriteOutWord(pb, result) ? noErr : paramErr;
		}

		case kJoyCsGetEventQueue:
			return JoyManagerWriteOutPtr(pb, joy_queue_addr) ? noErr : paramErr;

		case kJoyCsGetElementName: {
			int device_index;
			int element_index;
			uint32 name_addr;
			uint32 auxiliary_addr;
			int result;

			device_index = (int16)ReadMacInt16(pb + csParam + 4);
			element_index = (int16)ReadMacInt16(pb + csParam + 6);
			name_addr = ReadMacInt32(pb + csParam + 8);
			auxiliary_addr = ReadMacInt32(pb + csParam + 12);
			result = noErr;
#ifdef USE_SDL
			if (!JoyManagerWriteElementName(name_addr, device_index,
				element_index))
				result = paramErr;
#else
			result = paramErr;
#endif
			if (auxiliary_addr != 0)
				WriteMacInt32(auxiliary_addr, 0);
			return JoyManagerWriteOutWord(pb, result) ? noErr : paramErr;
		}
	}
	return controlErr;
}

int16 JoyManagerStatus(uint32 pb, uint32 dce)
{
	(void)pb;
	(void)dce;
	return statusErr;
}

int16 JoyManagerClose(uint32 pb, uint32 dce)
{
#ifdef USE_SDL
	int i;
#endif

	(void)pb;
	(void)dce;
	joy_start_count = 0;
#ifdef USE_SDL
	for (i = 0; i < joy_device_count; i++)
		joy_devices[i].enabled = false;
	JoyManagerUpdateState();
#else
	if (joy_simple_addr != 0)
		Mac_memset(joy_simple_addr, 0, joySimpleSize);
#endif
	JoyManagerResetQueue();
	return noErr;
}

#ifdef USE_SDL
JoyManagerDevice *JoyManagerOpenDevice(int index)
{
	return JoyManagerSDLOpenDevice(index);
}
void JoyManagerCloseDevice(JoyManagerDevice *joystick)
{
	JoyManagerSDLCloseDevice(joystick);
}
int JoyManagerNumDevices(void)
{
	return JoyManagerSDLNumDevices();
}
int JoyManagerNumButtons(JoyManagerDevice *joystick)
{
	return JoyManagerSDLNumButtons(joystick);
}
int JoyManagerNumAxes(JoyManagerDevice *joystick)
{
	return JoyManagerSDLNumAxes(joystick);
}
int JoyManagerNumHats(JoyManagerDevice *joystick)
{
	return JoyManagerSDLNumHats(joystick);
}
bool JoyManagerInit(void)
{
	return JoyManagerSDLInit();
}
bool JoyManagerHasRudderThrottle(JoyManagerDevice *joystick)
{
	return JoyManagerSDLHasRudderThrottle(joystick);
}
bool JoyManagerDeviceAttached(JoyManagerDevice *joystick)
{
	return JoyManagerSDLDeviceAttached(joystick);
}
const char *JoyManagerDeviceName(JoyManagerDevice *joystick,
	int index)
{
	return JoyManagerSDLDeviceName(joystick, index);
}
int16 JoyManagerAxis(JoyManagerDevice *joystick, int axis)
{
	return JoyManagerSDLAxis(joystick, axis);
}
uint8 JoyManagerButton(JoyManagerDevice *joystick, int button)
{
	return JoyManagerSDLButton(joystick, button);
}
uint8 JoyManagerHat(JoyManagerDevice *joystick, int hat)
{
	return JoyManagerSDLHat(joystick, hat);
}
void JoyManagerUpdate(void)
{
	JoyManagerSDLUpdate();
}
int JoyManagerHatPosition(uint8 hat)
{
	return JoyManagerSDLHatPosition(hat);
}
#else
JoyManagerDevice *JoyManagerOpenDevice(int index)
{
	return NULL;
}
void JoyManagerCloseDevice(JoyManagerDevice *joystick)
{
}
int JoyManagerNumDevices(void)
{
	return 0;
}
int JoyManagerNumButtons(JoyManagerDevice *joystick)
{
	return 0;
}
int JoyManagerNumAxes(JoyManagerDevice *joystick)
{
	return 0;
}
int JoyManagerNumHats(JoyManagerDevice *joystick)
{
	return 0;
}
bool JoyManagerInit(void)
{
	return false;
}
bool JoyManagerHasRudderThrottle(JoyManagerDevice *joystick)
{
	return false;
}
bool JoyManagerDeviceAttached(JoyManagerDevice *joystick)
{
	return false;
}
const char *JoyManagerDeviceName(JoyManagerDevice *joystick,
	int index)
{
	return NULL;
}
int16 JoyManagerAxis(JoyManagerDevice *joystick, int axis)
{
	return 0;
}
uint8 JoyManagerButton(JoyManagerDevice *joystick, int button)
{
	return 0;
}
uint8 JoyManagerHat(JoyManagerDevice *joystick, int hat)
{
	return 0;
}
void JoyManagerUpdate(void)
{
}
int JoyManagerHatPosition(uint8 hat)
{
	return 0;
}
#endif /* #ifdef USE_SDL */