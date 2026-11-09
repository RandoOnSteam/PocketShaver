/*
 *  osxclip_sdl.cpp - SDL host clipboard for the Mac OS X guest
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
#include "my_sdl.h"
#include "osxhost.h"

char *OsxGuestHostClipboardGet(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	char *text;
	char *copy;
	if (!SDL_HasClipboardText())
		return NULL;
	text = SDL_GetClipboardText();
	if (text == NULL)
		return NULL;
	copy = strdup(text);
	SDL_free(text);
	return copy;
#elif defined(_WIN32)
	char *data;
	HANDLE handle;
	const wchar_t *wide;
	char *text;
	int size;
	data = NULL;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
		return NULL;
	handle = GetClipboardData(CF_UNICODETEXT);
	wide = NULL;
	if (handle)
		wide = (const wchar_t *)GlobalLock(handle);
	if (wide) {
		size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
		text = (char *)malloc(size + 1);
		if (text && size > 0 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, size, NULL, NULL) > 0)
			data = text;
		else
			free(text);
		GlobalUnlock(handle);
	}
	CloseClipboard();
	return data;
#else
	return NULL;
#endif
}

void OsxGuestHostClipboardSet(const char *text)
{
#if !SDL_VERSION_ATLEAST(2, 0, 0) && defined(_WIN32)
	HGLOBAL handle;
	wchar_t *wide;
	int size;
	int index;
	int used;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_SetClipboardText(text);
#elif defined(_WIN32)
	size = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	handle = GlobalAlloc(GMEM_MOVEABLE, (size_t)size * 2 * sizeof(wchar_t));
	if (handle && OpenClipboard(NULL)) {
		wide = (wchar_t *)GlobalLock(handle);
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide + size, size);
		used = 0;
		for (index = 0; index < size; index++) {
			if (wide[size + index] == L'\n')
				wide[used++] = L'\r';
			wide[used++] = wide[size + index];
		}
		GlobalUnlock(handle);
		EmptyClipboard();
		if (!SetClipboardData(CF_UNICODETEXT, handle))
			GlobalFree(handle);
		CloseClipboard();
	} else if (handle)
		GlobalFree(handle);
#endif
}

