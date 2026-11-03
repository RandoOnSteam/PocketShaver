/*
 *  sflui.c - Window, input and frame presentation of the SFL platform layer
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

#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "thread.h"
#include "display.h"
#include "sflui.h"
#if defined(SFLMONITORENABLED)
#include "sflmonitor.h"
#endif
#include "sflmain.h"

#define SFLUITICKMS 17 /* match Windows */
#define SFLUICLIPBOARDREPAINTS 15
#define SFLUIADBCOUNT 128
#define SFLUINOKEY -1
#define SFLUIPALETTESIZE 256
#define SFLUIPIXELBYTES 4
#define SFLUIADBCONTROL 0x36
#define SFLUIADBRETURN 0x24
#define SFLUIMONITORPORTVARIABLE "EMULATOR_MONITOR_PORT"

typedef struct SFLUIKEY
{
	size_t wskey;
	int adb;
} SFLUIKEY;

typedef struct SFLUIAPP
{
	WSAPP super;
	WSWINDOW window;
	WSWNDCLASS wc;
	WSTICKER ticker;
	WSOC oc;
	WSCRITICALSECTION lock;
	unsigned char *frame;
	unsigned char *shadow;
	size_t shadowbytes;
	int dirtytop;
	int dirtybottom;
	wsbool fullconvert;
	int frameskip;
	int frameticks;
	int framewidth;
	int frameheight;
	int framerowbytes;
	int framebits;
	int ocwidth;
	int ocheight;
	unsigned char palette[SFLUIPALETTESIZE * 3];
	unsigned char keys[SFLUIADBCOUNT];
	int buttondown;
	guipos_t mousex;
	guipos_t mousey;
	wsbool mousepending;
	int clientwidth;
	int clientheight;
	int clipboardrepaints;
	char *cliptext;
	size_t cliplength;
	char *clipset;
	size_t clipsetlength;
	wsbool clipsetpending;
	wsatomicint_t quit;
	wsatomicint_t resize;
	wsatomicint_t repaint;
	wsbool windowopen;
	wsbool fullscreen;
	wsbool displaychanged;
	WSDISPLAY display;
	WSRECT windowrect;
	wsbool occreated;
	wsbool tickercreated;
#if defined(SFLMONITORENABLED)
	SFLMONITOR monitor;
	wsbool monitorcreated;
#endif
#if defined(SFLMAINRUNEMULATORONMAINTHREAD)
	wsatomicint_t fullscreenpending;
#endif
} SFLUIAPP;

SFLUIAPP *SflUiApp;
static void SflUiApplySize(SFLUIAPP *pThis);
static void SflUiRefresh(SFLUIAPP *pThis);
static void SflUiUpdateClientSize(SFLUIAPP *pThis);
static void SflUiToggleFullScreen(SFLUIAPP *pThis);

void SflUiPumpEvents(void)
{ /* when SFLMAINRUNEMULATORONMAINTHREAD is off the UI thread handles
  all this. */
#if defined(SFLMAINRUNEMULATORONMAINTHREAD)
	SFLUIAPP *pThis = SflUiApp;
	WSEventLoop_Process(&pThis->super.mEventLoop);   /* handle_events() */

	if (wsatomicintload(&pThis->quit)) {             /* deferred quit  */
		SflMainStopEmulator();
		return;
	}
	if (wsatomicintload(&pThis->fullscreenpending)) {
		wsatomicintstore(&pThis->fullscreenpending, 0);
		SflUiToggleFullScreen(pThis);
	}
	if (wsatomicintload(&pThis->resize)) {
		wsatomicintstore(&pThis->resize, 0);
		SflUiApplySize(pThis);
		SflUiUpdateClientSize(pThis);
	}
	SflUiRefresh(pThis);                             /* video_refresh() */
#endif
}

#if defined(WSMACCLASSICORCARBON)
static int SflUiAdbFromKey(size_t wskey)
{
	static const SFLUIKEY keys[] = {
		{WSKEY_LCONTROL, 0x36}, {WSKEY_RCONTROL, 0x36}, {WSKEY_RSHIFT, 0x38}, {WSKEY_RALT, 0x3a}
	};
	size_t index;
	for (index = 0; index < countof(keys); index++)
		if (keys[index].wskey == wskey)
			return keys[index].adb;
	if (wskey < SFLUIADBCOUNT)
		return (int)wskey;
	return SFLUINOKEY;
}
#else
static int SflUiAdbFromKey(size_t wskey)
{
	static const SFLUIKEY keys[] = {
		{WSKEY_A, 0x00}, {WSKEY_S, 0x01}, {WSKEY_D, 0x02}, {WSKEY_F, 0x03},
		{WSKEY_H, 0x04}, {WSKEY_G, 0x05}, {WSKEY_Z, 0x06}, {WSKEY_X, 0x07},
		{WSKEY_C, 0x08}, {WSKEY_V, 0x09}, {WSKEY_B, 0x0b}, {WSKEY_Q, 0x0c},
		{WSKEY_W, 0x0d}, {WSKEY_E, 0x0e}, {WSKEY_R, 0x0f}, {WSKEY_Y, 0x10},
		{WSKEY_T, 0x11}, {WSKEY_1, 0x12}, {WSKEY_2, 0x13}, {WSKEY_3, 0x14},
		{WSKEY_4, 0x15}, {WSKEY_6, 0x16}, {WSKEY_5, 0x17}, {WSKEY_EQUAL, 0x18},
		{WSKEY_9, 0x19}, {WSKEY_7, 0x1a}, {WSKEY_MINUS, 0x1b}, {WSKEY_8, 0x1c},
		{WSKEY_0, 0x1d}, {WSKEY_BRACKETRIGHT, 0x1e}, {WSKEY_O, 0x1f}, {WSKEY_U, 0x20},
		{WSKEY_BRACKETLEFT, 0x21}, {WSKEY_I, 0x22}, {WSKEY_P, 0x23}, {WSKEY_RETURN, 0x24},
		{WSKEY_L, 0x25}, {WSKEY_J, 0x26}, {WSKEY_APOSTROPHE, 0x27}, {WSKEY_K, 0x28},
		{WSKEY_SEMICOLON, 0x29}, {WSKEY_BACKSLASH, 0x2a}, {WSKEY_COMMA, 0x2b}, {WSKEY_SLASH, 0x2c},
		{WSKEY_N, 0x2d}, {WSKEY_M, 0x2e}, {WSKEY_PERIOD, 0x2f}, {WSKEY_TAB, 0x30},
		{WSKEY_SPACE, 0x31}, {WSKEY_GRAVE, 0x32}, {WSKEY_BACKSPACE, 0x33}, {WSKEY_ESCAPE, 0x35},
		{WSKEY_CONTROL, 0x36}, {WSKEY_LCONTROL, 0x36}, {WSKEY_RCONTROL, 0x36},
		{WSKEY_LWIN, 0x37}, {WSKEY_RWIN, 0x37},
		{WSKEY_SHIFT, 0x38}, {WSKEY_LSHIFT, 0x38}, {WSKEY_RSHIFT, 0x38},
		{WSKEY_CAPITAL, 0x39},
		{WSKEY_ALT, 0x3a}, {WSKEY_LALT, 0x3a}, {WSKEY_RALT, 0x3a},
		{WSKEY_DECIMAL, 0x41}, {WSKEY_MULTIPLY, 0x43}, {WSKEY_ADD, 0x45}, {WSKEY_NUMLOCK, 0x47},
		{WSKEY_DIVIDE, 0x4b}, {WSKEY_NUMPADENTER, 0x4c}, {WSKEY_SUBTRACT, 0x4e},
		{WSKEY_NUMPAD0, 0x52}, {WSKEY_NUMPAD1, 0x53}, {WSKEY_NUMPAD2, 0x54}, {WSKEY_NUMPAD3, 0x55},
		{WSKEY_NUMPAD4, 0x56}, {WSKEY_NUMPAD5, 0x57}, {WSKEY_NUMPAD6, 0x58}, {WSKEY_NUMPAD7, 0x59},
		{WSKEY_NUMPAD8, 0x5b}, {WSKEY_NUMPAD9, 0x5c},
		{WSKEY_F5, 0x60}, {WSKEY_F6, 0x61}, {WSKEY_F7, 0x62}, {WSKEY_F3, 0x63},
		{WSKEY_F8, 0x64}, {WSKEY_F9, 0x65}, {WSKEY_F11, 0x67}, {WSKEY_F13, 0x69},
		{WSKEY_F14, 0x6b}, {WSKEY_F10, 0x6d}, {WSKEY_F12, 0x6f}, {WSKEY_F15, 0x71},
		{WSKEY_INSERT, 0x72}, {WSKEY_HOME, 0x73}, {WSKEY_PRIOR, 0x74}, {WSKEY_DELETE, 0x75},
		{WSKEY_F4, 0x76}, {WSKEY_END, 0x77}, {WSKEY_F2, 0x78}, {WSKEY_NEXT, 0x79},
		{WSKEY_F1, 0x7a}, {WSKEY_LEFT, 0x7b}, {WSKEY_RIGHT, 0x7c}, {WSKEY_DOWN, 0x7d},
		{WSKEY_UP, 0x7e}
	};
	size_t index;
	for (index = 0; index < countof(keys); index++)
		if (keys[index].wskey == wskey)
			return keys[index].adb;
	return SFLUINOKEY;
}
#endif

static void SflUiPixelOffsets(int format, int *ro, int *go, int *bo, int *ao)
{
	*ro = 0;
	*go = 1;
	*bo = 2;
	*ao = 3;
	if (format == WSOCOBJECT_PIXELFORMAT_BGRA) {
		*bo = 0;
		*go = 1;
		*ro = 2;
		*ao = 3;
	} else if (format == WSOCOBJECT_PIXELFORMAT_ARGB) {
		*ao = 0;
		*ro = 1;
		*go = 2;
		*bo = 3;
	} else if (format == WSOCOBJECT_PIXELFORMAT_ABGR) {
		*ao = 0;
		*bo = 1;
		*go = 2;
		*ro = 3;
	}
}

static int SflUiIndex(const unsigned char *row, int x, int bits)
{
	int perbyte;
	int shift;
	if (bits == 8)
		return row[x];
	perbyte = 8 / bits;
	shift = 8 - bits - (x % perbyte) * bits;
	return (row[x / perbyte] >> shift) & ((1 << bits) - 1);
}

static void SflUiConvertRow(SFLUIAPP *pThis, const unsigned char *source, unsigned char *target,
	int ro, int go, int bo, int ao)
{
	int x;
	int value;
	const unsigned char *rgb;
	unsigned char *pixel;
	for (x = 0; x < pThis->framewidth; x++) {
		pixel = target + x * 4;
		pixel[ao] = 0xff;
		if (pThis->framebits == 32) {
			pixel[ro] = source[x * 4 + 1];
			pixel[go] = source[x * 4 + 2];
			pixel[bo] = source[x * 4 + 3];
		} else if (pThis->framebits == 16) {
			value = (source[x * 2] << 8) | source[x * 2 + 1];
			pixel[ro] = (unsigned char)((((value >> 10) & 0x1f) << 3) | (((value >> 10) & 0x1f) >> 2));
			pixel[go] = (unsigned char)((((value >> 5) & 0x1f) << 3) | (((value >> 5) & 0x1f) >> 2));
			pixel[bo] = (unsigned char)(((value & 0x1f) << 3) | ((value & 0x1f) >> 2));
		} else {
			rgb = &pThis->palette[SflUiIndex(source, x, pThis->framebits) * 3];
			pixel[ro] = rgb[0];
			pixel[go] = rgb[1];
			pixel[bo] = rgb[2];
		}
	}
}

static void SflUiDraw(SFLUIAPP *pThis, WSDC *dc)
{
	WSOCOBJECT object;
	WSRECT source;
	WSRECT target;
	int ro;
	int go;
	int bo;
	int ao;
	int y;
	int row;
	WSCriticalSection_Lock(&pThis->lock);
	if (pThis->frame == NULL) {
		WSCriticalSection_Unlock(&pThis->lock);
		return;
	}
	if (pThis->occreated && (pThis->ocwidth != pThis->framewidth || pThis->ocheight != pThis->frameheight)) {
		WSOC_Destroy(&pThis->oc);
		pThis->occreated = FALSE;
	}
	if (!pThis->occreated) {
		WSOC_CreateDIWithBPP(&pThis->oc, dc, pThis->framewidth, pThis->frameheight, 32);
		pThis->ocwidth = pThis->framewidth;
		pThis->ocheight = pThis->frameheight;
		pThis->occreated = TRUE;
		pThis->fullconvert = TRUE;
	}
	if (pThis->fullconvert) {
		pThis->dirtytop = 0;
		pThis->dirtybottom = pThis->frameheight - 1;
		pThis->fullconvert = FALSE;
	}
	WSOC_LockPixels(&pThis->oc);
	WSOC_GetObject(&pThis->oc, &object);
	if (object.bits != NULL && object.bpp == 32 && pThis->shadow != NULL) {
		SflUiPixelOffsets(object.memorypixelformat, &ro, &go, &bo, &ao);
		for (y = pThis->dirtytop; y <= pThis->dirtybottom; y++) {
			row = y;
			if (!WSOCIsDataTopDown())
				row = pThis->frameheight - 1 - y;
			SflUiConvertRow(pThis, pThis->shadow + (size_t)y * pThis->framerowbytes,
				(unsigned char *)object.bits + (size_t)row * object.stride, ro, go, bo, ao);
		}
	}
	WSOC_UnlockPixels(&pThis->oc);
	pThis->dirtytop = pThis->frameheight;
	pThis->dirtybottom = -1;
	WSRect_Construct(&source, 0, 0, pThis->framewidth, pThis->frameheight);
	WSWindow_GetClientRect(&pThis->window, &target);
	WSOC_Blt(&pThis->oc, dc, &target, &source);
	WSCriticalSection_Unlock(&pThis->lock);
}



static void SflUiKey(SFLUIAPP *pThis, size_t wskey, int down)
{
	int adb;
	adb = SflUiAdbFromKey(wskey);
	if (adb == SFLUINOKEY)
		return;
	if (adb == SFLUIADBRETURN && down && pThis->keys[SFLUIADBCONTROL]) {
		#if defined(SFLMAINRUNEMULATORONMAINTHREAD)
			wsatomicintstore(&pThis->fullscreenpending, 1);
		#else
			SflUiToggleFullScreen(pThis);
		#endif
		return;
	}
	if (down && pThis->keys[adb])
		return;
	if (!down && !pThis->keys[adb])
		return;
	pThis->keys[adb] = (unsigned char)down;
	SflEmulatorKey(adb, down);
}

static void SflUiMouseDeliver(SFLUIAPP *pThis, guipos_t x, guipos_t y)
{
	int width;
	int height;
	WSCriticalSection_Lock(&pThis->lock);
	width = pThis->clientwidth;
	height = pThis->clientheight;
	WSCriticalSection_Unlock(&pThis->lock);
	if (width <= 0 || height <= 0 || pThis->framewidth <= 0)
		return;
	SflEmulatorMouse((int)(x * pThis->framewidth / width), (int)(y * pThis->frameheight / height));
}

static void SflUiMouse(SFLUIAPP *pThis, guipos_t x, guipos_t y)
{
	pThis->mousex = x;
	pThis->mousey = y;
	pThis->mousepending = TRUE;
}

static void SflUiMouseFlush(SFLUIAPP *pThis)
{
	if (!pThis->mousepending)
		return;
	pThis->mousepending = FALSE;
	SflUiMouseDeliver(pThis, pThis->mousex, pThis->mousey);
}

WSPAINTEVENT_CALLBACK(SflUiOnPaint)
{
	WSPAINTEVENT ev;
	WSPAINTDC dc;
	WSPaintEvent_Construct(&ev);
	WSPaintDC_Create(&dc, &ev);
	SflUiDraw(SflUiApp, WSPaintDC_GetDC(&dc));
	WSPaintDC_Destroy(&dc, &ev);
	return WSPAINTEVENT_UNHANDLED;
}

WSKEYEVENT_CALLBACK(SflUiOnKeyDown)
{
	WSKEYEVENT ev;
	WSKeyEvent_Construct(&ev);
	SflUiKey(SflUiApp, WSKeyEvent_GetKey(&ev), 1);
	return WSKEYEVENT_HANDLED;
}

WSKEYEVENT_CALLBACK(SflUiOnKeyUp)
{
	WSKEYEVENT ev;
	WSKeyEvent_Construct(&ev);
	SflUiKey(SflUiApp, WSKeyEvent_GetKey(&ev), 0);
	return WSKEYEVENT_HANDLED;
}

static void SflUiUpdateClientSize(SFLUIAPP *pThis)
{
	WSRECT client;
	WSWindow_GetClientRect(&pThis->window, &client);
	WSCriticalSection_Lock(&pThis->lock);
	pThis->clientwidth = (int)WSRect_GetWidth(&client);
	pThis->clientheight = (int)WSRect_GetHeight(&client);
	WSCriticalSection_Unlock(&pThis->lock);
}

static void SflUiClipboardSync(SFLUIAPP *pThis)
{
	char *text;
	char *copy;
	size_t length;
	size_t size;
	WSCriticalSection_Lock(&pThis->lock);
	text = pThis->clipset;
	length = pThis->clipsetlength;
	pThis->clipset = NULL;
	pThis->clipsetpending = FALSE;
	WSCriticalSection_Unlock(&pThis->lock);
	if (text != NULL) {
		WSClipboardSetText(text, length);
		WSFREE(text);
	}
	text = NULL;
	length = 0;
	size = 0;
	if (WSClipboardHasText())
		WSClipboardGetText(&text, &length, &size);
	copy = NULL;
	if (text != NULL) {
		copy = WSALLOC(char, length + 1);
		if (copy != NULL) {
			WSMemoryCopy(copy, text, length);
			copy[length] = 0;
		}
		if (size != 0)
			WSFREE(text);
	}
	WSCriticalSection_Lock(&pThis->lock);
	if (pThis->cliptext != NULL)
		WSFREE(pThis->cliptext);
	pThis->cliptext = copy;
	pThis->cliplength = 0;
	if (copy != NULL)
		pThis->cliplength = length;
	WSCriticalSection_Unlock(&pThis->lock);
}

static wsbool SflUiScanFrame(SFLUIAPP *pThis)
{
	size_t bytes;
	size_t offset;
	int y;
	wsbool changed;
	WSCriticalSection_Lock(&pThis->lock);
	if (pThis->frame == NULL) {
		WSCriticalSection_Unlock(&pThis->lock);
		return FALSE;
	}
	bytes = (size_t)pThis->framerowbytes * pThis->frameheight;
	if (bytes != pThis->shadowbytes) {
		WSFREE(pThis->shadow);
		pThis->shadow = WSALLOC(unsigned char, bytes);
		pThis->shadowbytes = bytes;
		if (pThis->shadow != NULL)
			WSMemoryCopy(pThis->shadow, pThis->frame, bytes);
		pThis->fullconvert = TRUE;
	}
	for (y = 0; y < pThis->frameheight && pThis->shadow != NULL; y++) {
		offset = (size_t)y * pThis->framerowbytes;
		if (memcmp(pThis->shadow + offset, pThis->frame + offset, pThis->framerowbytes) == 0)
			continue;
		WSMemoryCopy(pThis->shadow + offset, pThis->frame + offset, pThis->framerowbytes);
		if (y < pThis->dirtytop)
			pThis->dirtytop = y;
		if (y > pThis->dirtybottom)
			pThis->dirtybottom = y;
	}
	changed = pThis->fullconvert || pThis->dirtytop <= pThis->dirtybottom;
	WSCriticalSection_Unlock(&pThis->lock);
	return changed;
}

static void SflUiRefresh(SFLUIAPP *pThis)
{
	if (!wsatomicintload(&pThis->repaint) || !pThis->windowopen)
		return;
	wsatomicintstore(&pThis->repaint, 0);
	SflUiMouseFlush(pThis);
	pThis->frameticks++;
	if (pThis->frameticks >= pThis->frameskip) {
		pThis->frameticks = 0;
		if (SflUiScanFrame(pThis))
			WSWindow_Invalidate(&pThis->window);
	}
	pThis->clipboardrepaints++;
	if (pThis->clipboardrepaints >= SFLUICLIPBOARDREPAINTS || pThis->clipsetpending) {
		pThis->clipboardrepaints = 0;
		SflUiClipboardSync(pThis);
		SflUiUpdateClientSize(pThis);
	}
}

WSMOUSEMOVEEVENT_CALLBACK(SflUiOnMouseMove)
{
	WSMOUSEMOVEEVENT ev;
	WSMouseMoveEvent_Construct(&ev);
	SflUiMouse(SflUiApp, WSMouseMoveEvent_GetX(&ev), WSMouseMoveEvent_GetY(&ev));
	SflUiRefresh(SflUiApp);
	return WSMOUSEMOVEEVENT_UNHANDLED;
}

WSMOUSEEVENT_CALLBACK(SflUiOnMouseDown)
{
	WSMOUSEEVENT ev;
	WSMouseEvent_Construct(&ev);
	SflUiMouse(SflUiApp, WSMouseEvent_GetX(&ev), WSMouseEvent_GetY(&ev));
	SflUiMouseFlush(SflUiApp);
	if (!SflUiApp->buttondown) {
		SflUiApp->buttondown = 1;
		SflEmulatorButton(1);
	}
	return WSMOUSEEVENT_UNHANDLED;
}

WSMOUSEEVENT_CALLBACK(SflUiOnMouseUp)
{
	WSMOUSEEVENT ev;
	WSMouseEvent_Construct(&ev);
	SflUiMouse(SflUiApp, WSMouseEvent_GetX(&ev), WSMouseEvent_GetY(&ev));
	SflUiMouseFlush(SflUiApp);
	if (SflUiApp->buttondown) {
		SflUiApp->buttondown = 0;
		SflEmulatorButton(0);
	}
	return WSMOUSEEVENT_UNHANDLED;
}

WSCURSOREVENT_CALLBACK(SflUiOnSetCursor)
{
	WSCURSOREVENT ev;
	WSCursorEvent_Construct(&ev);
	if (!WSCursorEvent_IsClient(&ev))
		return WSCURSOREVENT_UNHANDLED;
	WSCursorEvent_SetCursor(&ev, WSCURSORTYPE_NONE);
	return WSCURSOREVENT_HANDLED;
}

WSCLOSEEVENT_CALLBACK(SflUiOnClose)
{
	SflEmulatorClose();
	return WSCLOSEEVENT_HANDLED;
}

WSTICKEREVENT_CALLBACK(SflUiOnTicker)
{
	WSTICKEREVENT event;
	WSTickerEvent_Construct(&event);
	if (!wsatomicintload(&SflUiApp->quit)) {
		SflEmulatorTick();
		wsatomicintstore(&SflUiApp->repaint, 1);
	}
	return WSTICKEREVENT_UNHANDLED;
}

BEGIN_WINDOW_MESSAGES(SflUiWindowMessages)
	IMPLEMENT_PAINT_MESSAGE(SflUiOnPaint)
	IMPLEMENT_KEYDOWN_MESSAGE(SflUiOnKeyDown)
	IMPLEMENT_KEYUP_MESSAGE(SflUiOnKeyUp)
	IMPLEMENT_MOUSEMOVE_MESSAGE(SflUiOnMouseMove)
	IMPLEMENT_MOUSEDOWN_MESSAGE(SflUiOnMouseDown)
	IMPLEMENT_MOUSEUP_MESSAGE(SflUiOnMouseUp)
	IMPLEMENT_CLOSE_MESSAGE(SflUiOnClose)
	IMPLEMENT_SETCURSOR_MESSAGE(SflUiOnSetCursor)
END_WINDOW_MESSAGES(SflUiWindowMessages)

static void SflUiResetDisplay(SFLUIAPP *pThis)
{
	if (pThis->displaychanged)
		WSDisplay_ResetMode(&pThis->display);
	pThis->displaychanged = FALSE;
}

static void SflUiSetDisplayMode(SFLUIAPP *pThis)
{
	WSVIDEOMODE current;
	WSVIDEOMODE requested;
	SflUiResetDisplay(pThis);
	WSDisplay_GetCurrentMode(&pThis->display, &current);
	if (current.width == pThis->framewidth && current.height == pThis->frameheight)
		return;
	requested = current;
	requested.width = pThis->framewidth;
	requested.height = pThis->frameheight;
	pThis->displaychanged = WSDisplay_ChangeMode(&pThis->display, &requested);
}

static void SflUiApplySize(SFLUIAPP *pThis)
{
	WSRECT outer;
	WSRECT client;
	guipos_t extrawidth;
	guipos_t extraheight;
	if (pThis->fullscreen) {
		SflUiSetDisplayMode(pThis);
		return;
	}
	WSWindow_GetRect(&pThis->window, &outer);
	WSWindow_GetClientRect(&pThis->window, &client);
	extrawidth = WSRect_GetWidth(&outer) - WSRect_GetWidth(&client);
	extraheight = WSRect_GetHeight(&outer) - WSRect_GetHeight(&client);
	WSWindow_SetSize(&pThis->window, pThis->framewidth + extrawidth, pThis->frameheight + extraheight);
}

static void SflUiEnterFullScreen(SFLUIAPP *pThis)
{
	WSWindow_GetRect(&pThis->window, &pThis->windowrect);
	WSDisplayGetPrimary(&pThis->display);
	pThis->fullscreen = TRUE;
	SflUiSetDisplayMode(pThis);
	WSWindow_FullScreen(&pThis->window);
}

static void SflUiLeaveFullScreen(SFLUIAPP *pThis)
{
	SflUiResetDisplay(pThis);
	WSWindow_UnFullScreen(&pThis->window, &pThis->windowrect);
	WSDisplay_Destroy(&pThis->display);
	pThis->fullscreen = FALSE;
	SflUiApplySize(pThis);
}

static void SflUiToggleFullScreen(SFLUIAPP *pThis)
{
	if (pThis->fullscreen)
		SflUiLeaveFullScreen(pThis);
	else
		SflUiEnterFullScreen(pThis);
}

int SflUiOpen(const char *title, int width, int height, int fullscreen, int frameskip)
{
	WSRECT bounds;
	SFLUIAPP *pThis;
	pThis = SflUiApp;
	WSASSERT(pThis != NULL);
	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, width, height);
	if (!WSWndClass_Create(&pThis->wc, &pThis->super, NULL, SflUiWindowMessages, "SheepShaverSFL"))
		return 0;
	if (!WSWindow_Create(&pThis->window, &pThis->super, &bounds, title, NULL, &pThis->wc))
		return 0;
	WSWindow_SetCon(&pThis->window, pThis);
	pThis->windowopen = TRUE;
	pThis->framewidth = width;
	pThis->frameheight = height;
	pThis->fullscreen = FALSE;
	pThis->displaychanged = FALSE;
	pThis->frameskip = frameskip;
	if (pThis->frameskip < 1)
		pThis->frameskip = 1;
	SflUiApplySize(pThis);
	WSWindow_Show(&pThis->window);
	if (fullscreen)
		SflUiEnterFullScreen(pThis);
	SflUiUpdateClientSize(pThis);
	return 1;
}

void SflUiGetDesktopSize(int *width, int *height)
{
	WSRECT geometry;
	WSDisplayGetGeometry(&geometry);
	*width = WSRect_GetWidth(&geometry);
	*height = WSRect_GetHeight(&geometry);
}

void SflUiQuitFullScreen(void)
{
	SflUiResetDisplay(SflUiApp);
}

void SflUiClose(void)
{
	SFLUIAPP *pThis;
	pThis = SflUiApp;
	if (pThis == NULL || !pThis->windowopen)
		return;
	if (pThis->occreated) {
		WSOC_Destroy(&pThis->oc);
		pThis->occreated = FALSE;
	}
	if (pThis->fullscreen)
		SflUiLeaveFullScreen(pThis);
	WSWindow_Destroy(&pThis->window);
	WSWndClass_Destroy(&pThis->wc, &pThis->super);
	pThis->windowopen = FALSE;
}

void SflUiSetFrame(unsigned char *buffer, int width, int height, int rowbytes, int bits)
{
	SFLUIAPP *pThis;
	int sizechanged;
	pThis = SflUiApp;
	WSCriticalSection_Lock(&pThis->lock);
	sizechanged = width != pThis->framewidth || height != pThis->frameheight;
	pThis->frame = buffer;
	pThis->framewidth = width;
	pThis->frameheight = height;
	pThis->framerowbytes = rowbytes;
	pThis->framebits = bits;
	pThis->fullconvert = TRUE;
	WSCriticalSection_Unlock(&pThis->lock);
	if (sizechanged)
		wsatomicintstore(&pThis->resize, 1);
}

void SflUiSetPalette(const unsigned char *rgb, int count)
{
	SFLUIAPP *pThis;
	pThis = SflUiApp;
	if (count > SFLUIPALETTESIZE)
		count = SFLUIPALETTESIZE;
	WSCriticalSection_Lock(&pThis->lock);
	WSMemoryCopy(pThis->palette, rgb, (size_t)count * 3);
	pThis->fullconvert = TRUE;
	WSCriticalSection_Unlock(&pThis->lock);
}

int SflUiGetStatus(int *width, int *height, int *depth, int *hostwidth, int *hostheight)
{
	SFLUIAPP *pThis;
	pThis = SflUiApp;
	if (pThis == NULL || !pThis->windowopen)
		return 0;
	WSCriticalSection_Lock(&pThis->lock);
	*width = pThis->framewidth;
	*height = pThis->frameheight;
	*depth = pThis->framebits;
	*hostwidth = pThis->clientwidth;
	*hostheight = pThis->clientheight;
	WSCriticalSection_Unlock(&pThis->lock);
	return 1;
}

static void SflUiCopyHostRow(const unsigned char *source, unsigned char *target, int width,
	int ro, int go, int bo)
{
	int x;
	for (x = 0; x < width; x++) {
		target[x * SFLUIPIXELBYTES] = source[x * SFLUIPIXELBYTES + ro];
		target[x * SFLUIPIXELBYTES + 1] = source[x * SFLUIPIXELBYTES + go];
		target[x * SFLUIPIXELBYTES + 2] = source[x * SFLUIPIXELBYTES + bo];
		target[x * SFLUIPIXELBYTES + 3] = 0xff;
	}
}

static unsigned char *SflUiCopyHost(SFLUIAPP *pThis, int *width, int *height)
{
	WSOCOBJECT object;
	unsigned char *pixels;
	int ro;
	int go;
	int bo;
	int ao;
	int y;
	int row;
	if (!pThis->occreated)
		return NULL;
	WSOC_LockPixels(&pThis->oc);
	WSOC_GetObject(&pThis->oc, &object);
	pixels = NULL;
	if (object.bits != NULL && object.bpp == 32)
		pixels = WSALLOC(unsigned char, (size_t)pThis->ocwidth * pThis->ocheight * SFLUIPIXELBYTES);
	if (pixels != NULL) {
		SflUiPixelOffsets(object.memorypixelformat, &ro, &go, &bo, &ao);
		for (y = 0; y < pThis->ocheight; y++) {
			row = y;
			if (!WSOCIsDataTopDown())
				row = pThis->ocheight - 1 - y;
			SflUiCopyHostRow((const unsigned char *)object.bits + (size_t)row * object.stride,
				pixels + (size_t)y * pThis->ocwidth * SFLUIPIXELBYTES, pThis->ocwidth, ro, go, bo);
		}
		*width = pThis->ocwidth;
		*height = pThis->ocheight;
	}
	WSOC_UnlockPixels(&pThis->oc);
	return pixels;
}

static unsigned char *SflUiCopyGuest(SFLUIAPP *pThis, int *width, int *height)
{
	unsigned char *pixels;
	int y;
	if (pThis->frame == NULL || pThis->framewidth <= 0 || pThis->frameheight <= 0)
		return NULL;
	pixels = WSALLOC(unsigned char, (size_t)pThis->framewidth * pThis->frameheight * SFLUIPIXELBYTES);
	if (pixels == NULL)
		return NULL;
	for (y = 0; y < pThis->frameheight; y++)
		SflUiConvertRow(pThis, pThis->frame + (size_t)y * pThis->framerowbytes,
			pixels + (size_t)y * pThis->framewidth * SFLUIPIXELBYTES, 0, 1, 2, 3);
	*width = pThis->framewidth;
	*height = pThis->frameheight;
	return pixels;
}

unsigned char *SflUiCopyFrame(int host, int *width, int *height)
{
	SFLUIAPP *pThis;
	unsigned char *pixels;
	pThis = SflUiApp;
	if (pThis == NULL)
		return NULL;
	WSCriticalSection_Lock(&pThis->lock);
	if (host)
		pixels = SflUiCopyHost(pThis, width, height);
	else
		pixels = SflUiCopyGuest(pThis, width, height);
	WSCriticalSection_Unlock(&pThis->lock);
	return pixels;
}

void SflUiHostMouse(int x, int y)
{
	if (SflUiApp != NULL && SflUiApp->windowopen)
		SflUiMouseDeliver(SflUiApp, (guipos_t)x, (guipos_t)y);
}

void SflUiHostButton(int down)
{
	if (SflUiApp == NULL || SflUiApp->buttondown == down)
		return;
	SflUiApp->buttondown = down;
	SflEmulatorButton(down);
}

int SflUiClipboardGet(char **text, size_t *length)
{
	SFLUIAPP *pThis;
	pThis = SflUiApp;
	*text = NULL;
	*length = 0;
	if (pThis == NULL)
		return 0;
	WSCriticalSection_Lock(&pThis->lock);
	if (pThis->cliptext != NULL) {
		*text = WSALLOC(char, pThis->cliplength + 1);
		if (*text != NULL) {
			WSMemoryCopy(*text, pThis->cliptext, pThis->cliplength);
			(*text)[pThis->cliplength] = 0;
			*length = pThis->cliplength;
		}
	}
	WSCriticalSection_Unlock(&pThis->lock);
	return *text != NULL;
}

void SflUiClipboardSet(const char *text, size_t length)
{
	SFLUIAPP *pThis;
	char *copy;
	pThis = SflUiApp;
	if (pThis == NULL)
		return;
	copy = WSALLOC(char, length + 1);
	if (copy == NULL)
		return;
	WSMemoryCopy(copy, text, length);
	copy[length] = 0;
	WSCriticalSection_Lock(&pThis->lock);
	if (pThis->clipset != NULL)
		WSFREE(pThis->clipset);
	pThis->clipset = copy;
	pThis->clipsetlength = length;
	pThis->clipsetpending = TRUE;
	WSCriticalSection_Unlock(&pThis->lock);
}

void SflUiAlert(const char *title, const char *text)
{
	WSWINDOW *parent;
	parent = NULL;
	if (SflUiApp != NULL && SflUiApp->windowopen)
		parent = &SflUiApp->window;
	WSMessageBox(parent, title, text, WSMESSAGEBOX_OK);
}

void SflUiRequestQuit(void)
{
	if (SflUiApp != NULL)
		wsatomicintstore(&SflUiApp->quit, 1);
}

static wsbool SflUiOnInit(SFLUIAPP *pThis)
{
	SflUiApp = pThis;
	pThis->frame = NULL;
	pThis->framewidth = 0;
	pThis->frameheight = 0;
	pThis->framerowbytes = 0;
	pThis->framebits = 32;
	pThis->ocwidth = 0;
	pThis->ocheight = 0;
	pThis->buttondown = 0;
	pThis->mousex = 0;
	pThis->mousey = 0;
	pThis->mousepending = FALSE;
	pThis->clientwidth = 0;
	pThis->clientheight = 0;
	pThis->clipboardrepaints = 0;
	pThis->shadow = NULL;
	pThis->shadowbytes = 0;
	pThis->dirtytop = 0;
	pThis->dirtybottom = -1;
	pThis->fullconvert = TRUE;
	pThis->frameskip = 1;
	pThis->frameticks = 0;
	pThis->cliptext = NULL;
	pThis->cliplength = 0;
	pThis->clipset = NULL;
	pThis->clipsetlength = 0;
	pThis->clipsetpending = FALSE;
	pThis->windowopen = FALSE;
	pThis->occreated = FALSE;
	pThis->tickercreated = FALSE;
#if defined(SFLMONITORENABLED)
	pThis->monitorcreated = FALSE;
#endif
#if defined(SFLMAINRUNEMULATORONMAINTHREAD)
	wsatomicintset(&pThis->fullscreenpending, 0);
#endif
	WSMemoryClear(pThis->palette, sizeof(pThis->palette));
	WSMemoryClear(pThis->keys, sizeof(pThis->keys));
	wsatomicintstore(&pThis->quit, 0);
	wsatomicintstore(&pThis->resize, 0);
	wsatomicintstore(&pThis->repaint, 0);
	WSCriticalSection_Construct(&pThis->lock);
	return WSApp_Create(&pThis->super);
}

#if defined(SFLMONITORENABLED)
static void SflUiStartMonitor(SFLUIAPP *pThis)
{
	const char *variable;
	int port;
	port = SFLMONITORPORT;
	variable = getenv(SFLUIMONITORPORTVARIABLE);
	if (variable != NULL && atoi(variable) > 0)
		port = atoi(variable);
	pThis->monitorcreated = SflMonitor_Create(&pThis->monitor, port);
}

static void SflUiStopMonitor(SFLUIAPP *pThis)
{
	if (!pThis->monitorcreated)
		return;
	SflMonitor_Destroy(&pThis->monitor);
	pThis->monitorcreated = FALSE;
}
#endif



static int SflUiOnExit(SFLUIAPP *pThis)
{
	if (pThis->cliptext != NULL)
		WSFREE(pThis->cliptext);
	if (pThis->clipset != NULL)
		WSFREE(pThis->clipset);
	WSApp_Destroy(&pThis->super);
	WSFREE(pThis->shadow);
	WSCriticalSection_Destruct(&pThis->lock);
	SflUiApp = NULL;
	return 0;
}

#if !defined(SFLMAINRUNEMULATORONMAINTHREAD)
static wsbool SflUiOnLoopBegin(SFLUIAPP *pThis)
{
	if (!SflEmulatorSetup(pThis->super.mArgc, pThis->super.mArgv))
		return FALSE;
	if (!pThis->windowopen)
		return FALSE;
#if defined(SFLMONITORENABLED)
	SflUiStartMonitor(pThis);
#endif
	pThis->tickercreated = WSTicker_Create(&pThis->ticker,
		&pThis->window, SFLUITICKMS, SflUiOnTicker);
	return TRUE;
}

static wsbool SflUiOnIdle(SFLUIAPP *pThis)
{
	if (wsatomicintload(&pThis->quit))
		return FALSE;
	if (wsatomicintload(&pThis->resize) && pThis->windowopen) {
		wsatomicintstore(&pThis->resize, 0);
		SflUiApplySize(pThis);
		SflUiUpdateClientSize(pThis);
	}
	SflUiRefresh(pThis);
	return TRUE;
}

static wsbool SflUiOnLoopEnd(SFLUIAPP *pThis)
{
	if (pThis->tickercreated) {
		WSTicker_Destroy(&pThis->ticker);
		pThis->tickercreated = FALSE;
	}
#if defined(SFLMONITORENABLED)
	SflUiStopMonitor(pThis);
#endif
	SflEmulatorStop();
	SflUiClose();
	return FALSE;
}
IMPLEMENT_WSAPP(SFLUIAPP, SflUiOnInit, SflUiOnIdle,
	SflUiOnLoopBegin, SflUiOnLoopEnd, SflUiOnExit)
#else
static void SflUiOnRun(SFLUIAPP *pThis)
{
	if (!SflEmulatorSetup(pThis->super.mArgc, pThis->super.mArgv))
		return;
	if (pThis->windowopen)
	{
		#if defined(SFLMONITORENABLED)
			SflUiStartMonitor(pThis);
		#endif
		WSTimerBeginPeriod(1);
		pThis->tickercreated = WSTicker_Create(&pThis->ticker,
			&pThis->window, SFLUITICKMS, SflUiOnTicker);

		if (pThis->tickercreated) {
			SflEmulatorStart();
			WSTicker_Destroy(&pThis->ticker);
			WSTimerEndPeriod(1);
			pThis->tickercreated = FALSE;
			SflEmulatorStop();
		}
		#if defined(SFLMONITORENABLED)
			SflUiStopMonitor(pThis);
		#endif
	}
	SflUiClose();
}
IMPLEMENT_WSAPP_CUSTOMRUN(SFLUIAPP, SflUiOnInit, SflUiOnExit, SflUiOnRun)
#endif
