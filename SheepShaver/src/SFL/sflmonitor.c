/*
 *  sflmonitor.c - Emulator monitor server of the SFL platform layer
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

#include "gui.h"
#include "file.h"
#include "sflmonitor.h"
#include "sflui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SFLMONITORCOMMANDSIZE 1024
#define SFLMONITORRESPONSESIZE 4096
#define SFLMONITORESCAPESIZE 1024
#define SFLMONITORPRESSMS 60
#define SFLMONITORSETTLEMS 50
#define SFLMONITORTYPEMS 30
#define SFLMONITORDRAGSTEPS 10
#define SFLMONITORDRAGSTEPMS 20
#define SFLMONITORADBSHIFT 0x38
#define SFLMONITORADBPOWER 0x7f
#define SFLMONITORADBMAX 0x7f
#define SFLMONITORMAXBUTTON 2
#define SFLMONITORDEFLATEBLOCK 65535
#define SFLMONITORBMPHEADER 54
#define SFLMONITORPIXELBYTES 4

typedef struct SFLMONITORTEXTKEY
{
	char plain;
	char shifted;
	unsigned char code;
} SFLMONITORTEXTKEY;

typedef struct SFLMONITORPNG
{
	WSFILE file;
	unsigned long crctable[256];
	unsigned long crc;
	unsigned long adlerlow;
	unsigned long adlerhigh;
	size_t rawsize;
	size_t written;
	wsbool failed;
} SFLMONITORPNG;

static void SflMonitorEscape(const char *source, char *target, size_t targetsize)
{
	size_t sourceindex;
	size_t targetindex;
	targetindex = 0;
	for (sourceindex = 0; source[sourceindex] != 0 && targetindex + 2 < targetsize; sourceindex++) {
		if (source[sourceindex] == '"' || source[sourceindex] == '\\')
			target[targetindex++] = '\\';
		target[targetindex++] = source[sourceindex];
	}
	target[targetindex] = 0;
}

static void SflMonitorOk(char *response, size_t responsesize)
{
	sprintf(response, "{\"ok\":true}");
}

static wsbool SflMonitorError(char *response, size_t responsesize, const char *message)
{
	char escaped[SFLMONITORESCAPESIZE];
	SflMonitorEscape(message, escaped, sizeof(escaped));
	sprintf(response, "{\"ok\":false,\"error\":\"%s\"}", escaped);
	return FALSE;
}

static void SflMonitorPath(char *response, size_t responsesize, const char *path)
{
	char escaped[SFLMONITORESCAPESIZE];
	SflMonitorEscape(path, escaped, sizeof(escaped));
	sprintf(response, "{\"ok\":true,\"path\":\"%s\"}", escaped);
}

static void SflMonitorTrim(char *text)
{
	size_t length;
	length = strlen(text);
	while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n'
		|| text[length - 1] == ' ' || text[length - 1] == '\t')) {
		text[length - 1] = 0;
		length--;
	}
}

static wsbool SflMonitorCheckButton(int button, char *response, size_t responsesize)
{
	if (button >= 0 && button <= SFLMONITORMAXBUTTON)
		return TRUE;
	return SflMonitorError(response, responsesize, "button must be 0, 1, or 2");
}

static void SflMonitorPutBig(unsigned char *target, unsigned long value)
{
	target[0] = (unsigned char)(value >> 24);
	target[1] = (unsigned char)(value >> 16);
	target[2] = (unsigned char)(value >> 8);
	target[3] = (unsigned char)value;
}

static void SflMonitorPutLittle(unsigned char *target, unsigned long value, int bytes)
{
	int index;
	for (index = 0; index < bytes; index++)
		target[index] = (unsigned char)(value >> (index * 8));
}

static void SflMonitorPngWrite(SFLMONITORPNG *pThis, const unsigned char *data, size_t size, wsbool crc)
{
	size_t index;
	if (crc)
		for (index = 0; index < size; index++)
			pThis->crc = pThis->crctable[(pThis->crc ^ data[index]) & 0xff] ^ (pThis->crc >> 8);
	if (WSFile_Write(&pThis->file, data, size) != size)
		pThis->failed = TRUE;
}

static void SflMonitorPngBegin(SFLMONITORPNG *pThis, const char *type, unsigned long size)
{
	unsigned char header[8];
	SflMonitorPutBig(header, size);
	WSMemoryCopy(header + 4, type, 4);
	SflMonitorPngWrite(pThis, header, 4, FALSE);
	pThis->crc = 0xffffffffUL;
	SflMonitorPngWrite(pThis, header + 4, 4, TRUE);
}

static void SflMonitorPngEnd(SFLMONITORPNG *pThis)
{
	unsigned char trailer[4];
	SflMonitorPutBig(trailer, pThis->crc ^ 0xffffffffUL);
	SflMonitorPngWrite(pThis, trailer, 4, FALSE);
}

static void SflMonitorPngRaw(SFLMONITORPNG *pThis, const unsigned char *data, size_t size)
{
	unsigned char blockheader[5];
	size_t blocksize;
	size_t chunk;
	size_t index;
	while (size > 0) {
		if (pThis->written % SFLMONITORDEFLATEBLOCK == 0) {
			blocksize = pThis->rawsize - pThis->written;
			if (blocksize > SFLMONITORDEFLATEBLOCK)
				blocksize = SFLMONITORDEFLATEBLOCK;
			blockheader[0] = 0;
			if (pThis->written + blocksize == pThis->rawsize)
				blockheader[0] = 1;
			blockheader[1] = (unsigned char)blocksize;
			blockheader[2] = (unsigned char)(blocksize >> 8);
			blockheader[3] = (unsigned char)~blocksize;
			blockheader[4] = (unsigned char)(~blocksize >> 8);
			SflMonitorPngWrite(pThis, blockheader, 5, TRUE);
		}
		chunk = SFLMONITORDEFLATEBLOCK - pThis->written % SFLMONITORDEFLATEBLOCK;
		if (chunk > size)
			chunk = size;
		for (index = 0; index < chunk; index++) {
			pThis->adlerlow = (pThis->adlerlow + data[index]) % 65521;
			pThis->adlerhigh = (pThis->adlerhigh + pThis->adlerlow) % 65521;
		}
		SflMonitorPngWrite(pThis, data, chunk, TRUE);
		pThis->written += chunk;
		data += chunk;
		size -= chunk;
	}
}

static wsbool SflMonitorSavePng(const char *path, const unsigned char *pixels, int width, int height)
{
	static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	SFLMONITORPNG png;
	unsigned char header[13];
	unsigned char adler[4];
	unsigned char *row;
	size_t rowbytes;
	size_t blocks;
	unsigned long entry;
	unsigned long value;
	int bit;
	int y;
	int x;
	WSFile_Construct(&png.file);
	if (!WSFile_Create(&png.file, path, FILE_WRITE | FILE_CREATEALWAYS | FILE_BINARY))
		return FALSE;
	rowbytes = (size_t)width * 3 + 1;
	row = WSALLOC(unsigned char, rowbytes);
	if (row == NULL) {
		WSFile_Destroy(&png.file);
		return FALSE;
	}
	for (entry = 0; entry < 256; entry++) {
		value = entry;
		for (bit = 0; bit < 8; bit++) {
			if (value & 1)
				value = 0xedb88320UL ^ (value >> 1);
			else
				value >>= 1;
		}
		png.crctable[entry] = value;
	}
	png.failed = FALSE;
	png.adlerlow = 1;
	png.adlerhigh = 0;
	png.written = 0;
	png.rawsize = rowbytes * (size_t)height;
	blocks = (png.rawsize + SFLMONITORDEFLATEBLOCK - 1) / SFLMONITORDEFLATEBLOCK;
	SflMonitorPngWrite(&png, signature, 8, FALSE);
	SflMonitorPutBig(header, (unsigned long)width);
	SflMonitorPutBig(header + 4, (unsigned long)height);
	header[8] = 8;
	header[9] = 2;
	header[10] = 0;
	header[11] = 0;
	header[12] = 0;
	SflMonitorPngBegin(&png, "IHDR", 13);
	SflMonitorPngWrite(&png, header, 13, TRUE);
	SflMonitorPngEnd(&png);
	SflMonitorPngBegin(&png, "IDAT", (unsigned long)(2 + png.rawsize + blocks * 5 + 4));
	header[0] = 0x78;
	header[1] = 0x01;
	SflMonitorPngWrite(&png, header, 2, TRUE);
	row[0] = 0;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			row[1 + x * 3] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES];
			row[2 + x * 3] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES + 1];
			row[3 + x * 3] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES + 2];
		}
		SflMonitorPngRaw(&png, row, rowbytes);
	}
	SflMonitorPutBig(adler, (png.adlerhigh << 16) | png.adlerlow);
	SflMonitorPngWrite(&png, adler, 4, TRUE);
	SflMonitorPngEnd(&png);
	SflMonitorPngBegin(&png, "IEND", 0);
	SflMonitorPngEnd(&png);
	WSFREE(row);
	WSFile_Destroy(&png.file);
	return !png.failed;
}

static wsbool SflMonitorSaveBmp(const char *path, const unsigned char *pixels, int width, int height)
{
	WSFILE file;
	unsigned char header[SFLMONITORBMPHEADER];
	unsigned char *row;
	size_t rowbytes;
	wsbool saved;
	int y;
	int x;
	rowbytes = ((size_t)width * 3 + 3) & ~(size_t)3;
	WSFile_Construct(&file);
	if (!WSFile_Create(&file, path, FILE_WRITE | FILE_CREATEALWAYS | FILE_BINARY))
		return FALSE;
	row = WSALLOC(unsigned char, rowbytes);
	if (row == NULL) {
		WSFile_Destroy(&file);
		return FALSE;
	}
	WSMemoryClear(header, sizeof(header));
	WSMemoryClear(row, rowbytes);
	header[0] = 'B';
	header[1] = 'M';
	SflMonitorPutLittle(header + 2, (unsigned long)(SFLMONITORBMPHEADER + rowbytes * height), 4);
	SflMonitorPutLittle(header + 10, SFLMONITORBMPHEADER, 4);
	SflMonitorPutLittle(header + 14, 40, 4);
	SflMonitorPutLittle(header + 18, (unsigned long)width, 4);
	SflMonitorPutLittle(header + 22, (unsigned long)height, 4);
	SflMonitorPutLittle(header + 26, 1, 2);
	SflMonitorPutLittle(header + 28, 24, 2);
	SflMonitorPutLittle(header + 34, (unsigned long)(rowbytes * height), 4);
	saved = WSFile_Write(&file, header, sizeof(header)) == sizeof(header);
	for (y = height - 1; y >= 0 && saved; y--) {
		for (x = 0; x < width; x++) {
			row[x * 3] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES + 2];
			row[x * 3 + 1] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES + 1];
			row[x * 3 + 2] = pixels[((size_t)y * width + x) * SFLMONITORPIXELBYTES];
		}
		saved = WSFile_Write(&file, row, rowbytes) == rowbytes;
	}
	WSFREE(row);
	WSFile_Destroy(&file);
	return saved;
}

static wsbool SflMonitorIsPng(const char *path)
{
	size_t length;
	length = strlen(path);
	if (length < 4 || path[length - 4] != '.')
		return FALSE;
	return (path[length - 3] == 'p' || path[length - 3] == 'P')
		&& (path[length - 2] == 'n' || path[length - 2] == 'N')
		&& (path[length - 1] == 'g' || path[length - 1] == 'G');
}

static void SflMonitorShot(int host, const char *path, char *response, size_t responsesize)
{
	unsigned char *pixels;
	int width;
	int height;
	wsbool saved;
	while (*path == ' ')
		path++;
	if (*path == 0) {
		SflMonitorError(response, responsesize, "missing path");
		return;
	}
	pixels = SflUiCopyFrame(host, &width, &height);
	if (pixels == NULL) {
		SflMonitorError(response, responsesize, "framebuffer unavailable");
		return;
	}
	if (SflMonitorIsPng(path))
		saved = SflMonitorSavePng(path, pixels, width, height);
	else
		saved = SflMonitorSaveBmp(path, pixels, width, height);
	WSFREE(pixels);
	if (saved)
		SflMonitorPath(response, responsesize, path);
	else
		SflMonitorError(response, responsesize, "unable to write file");
}

static void SflMonitorKey(int code, int down)
{
	SflEmulatorKey(code, down);
}

static void SflMonitorKeyTap(int code)
{
	SflMonitorKey(code, 1);
	WSThreadSleep(SFLMONITORPRESSMS);
	SflMonitorKey(code, 0);
}

static void SflMonitorClick(int button)
{
	SflEmulatorMouseButton(button, 1);
	WSThreadSleep(SFLMONITORPRESSMS);
	SflEmulatorMouseButton(button, 0);
}

static void SflMonitorMoveSettle(int x, int y)
{
	SflEmulatorMouse(x, y);
	WSThreadSleep(SFLMONITORSETTLEMS);
}

static void SflMonitorDrag(int startx, int starty, int endx, int endy, int button, int holdtime)
{
	int step;
	SflMonitorMoveSettle(startx, starty);
	SflEmulatorMouseButton(button, 1);
	WSThreadSleep(SFLMONITORPRESSMS);
	for (step = 1; step <= SFLMONITORDRAGSTEPS; step++) {
		SflEmulatorMouse(startx + (endx - startx) * step / SFLMONITORDRAGSTEPS,
			starty + (endy - starty) * step / SFLMONITORDRAGSTEPS);
		WSThreadSleep(SFLMONITORDRAGSTEPMS);
	}
	WSThreadSleep(SFLMONITORSETTLEMS);
	WSThreadSleep((size_t)holdtime);
	SflEmulatorMouseButton(button, 0);
}

static wsbool SflMonitorTextKey(char character, int *code, wsbool *shifted)
{
	static const SFLMONITORTEXTKEY keys[] = {
		{'a', 'A', 0x00}, {'s', 'S', 0x01}, {'d', 'D', 0x02}, {'f', 'F', 0x03},
		{'h', 'H', 0x04}, {'g', 'G', 0x05}, {'z', 'Z', 0x06}, {'x', 'X', 0x07},
		{'c', 'C', 0x08}, {'v', 'V', 0x09}, {'`', '~', 0x0a}, {'b', 'B', 0x0b},
		{'q', 'Q', 0x0c}, {'w', 'W', 0x0d}, {'e', 'E', 0x0e}, {'r', 'R', 0x0f},
		{'y', 'Y', 0x10}, {'t', 'T', 0x11}, {'1', '!', 0x12}, {'2', '@', 0x13},
		{'3', '#', 0x14}, {'4', '$', 0x15}, {'6', '^', 0x16}, {'5', '%', 0x17},
		{'=', '+', 0x18}, {'9', '(', 0x19}, {'7', '&', 0x1a}, {'-', '_', 0x1b},
		{'8', '*', 0x1c}, {'0', ')', 0x1d}, {']', '}', 0x1e}, {'o', 'O', 0x1f},
		{'u', 'U', 0x20}, {'[', '{', 0x21}, {'i', 'I', 0x22}, {'p', 'P', 0x23},
		{'\n', 0, 0x24}, {'l', 'L', 0x25}, {'j', 'J', 0x26}, {'\'', '"', 0x27},
		{'k', 'K', 0x28}, {';', ':', 0x29}, {'\\', '|', 0x2a}, {',', '<', 0x2b},
		{'/', '?', 0x2c}, {'n', 'N', 0x2d}, {'m', 'M', 0x2e}, {'.', '>', 0x2f},
		{'\t', 0, 0x30}, {' ', 0, 0x31}
	};
	size_t index;
	for (index = 0; index < countof(keys); index++) {
		if (keys[index].plain == character) {
			*code = keys[index].code;
			*shifted = FALSE;
			return TRUE;
		}
		if (keys[index].shifted != 0 && keys[index].shifted == character) {
			*code = keys[index].code;
			*shifted = TRUE;
			return TRUE;
		}
	}
	return FALSE;
}

static void SflMonitorType(const char *text, char *response, size_t responsesize)
{
	size_t index;
	char character;
	int code;
	wsbool shifted;
	SflMonitorOk(response, responsesize);
	for (index = 0; text[index] != 0; index++) {
		character = text[index];
		if (character == '\\' && text[index + 1] == 'n') {
			character = '\n';
			index++;
		} else if (character == '\\' && text[index + 1] == 't') {
			character = '\t';
			index++;
		} else if (character == '\\' && text[index + 1] == '\\')
			index++;
		if (!SflMonitorTextKey(character, &code, &shifted)) {
			SflMonitorError(response, responsesize, "unsupported character");
			return;
		}
		if (shifted)
			SflMonitorKey(SFLMONITORADBSHIFT, 1);
		SflMonitorKey(code, 1);
		WSThreadSleep(SFLMONITORTYPEMS);
		SflMonitorKey(code, 0);
		if (shifted)
			SflMonitorKey(SFLMONITORADBSHIFT, 0);
		WSThreadSleep(SFLMONITORTYPEMS);
	}
}

static void SflMonitorExtFS(char *response, size_t responsesize)
{
	const char *path;
	path = SflEmulatorPrefString("extfs", 0);
	if (!SflEmulatorPrefBool("enableextfs") || path == NULL || path[0] == 0) {
		SflMonitorError(response, responsesize, "extfs is not enabled");
		return;
	}
	SflMonitorPath(response, responsesize, path);
}

static wsbool SflMonitorIsAbsolute(const char *path)
{
	if (path[0] == '/' || path[0] == '\\')
		return TRUE;
	return path[0] != 0 && path[1] == ':';
}

static void SflMonitorDisks(char *response, size_t responsesize)
{
	char escaped[SFLMONITORESCAPESIZE];
	char full[SFLMONITORESCAPESIZE];
	const char *path;
	char *cwd;
	size_t used;
	int index;
	cwd = NULL;
	SflMonitorEscape(SflEmulatorExtFSName(), escaped, sizeof(escaped));
	used = (size_t)sprintf(response, "{\"ok\":true,\"extfsname\":\"%s\",\"disks\":[", escaped);
	for (index = 0; (path = SflEmulatorPrefString("disk", index)) != NULL; index++) {
		if (path[0] == '*')
			path++;
		if (!SflMonitorIsAbsolute(path) && cwd == NULL)
			WSPathGetCWD(&cwd);
		if (!SflMonitorIsAbsolute(path) && cwd != NULL && strlen(cwd) + strlen(path) + 2 < sizeof(full))
			sprintf(full, "%s%c%s", cwd, WSPATHSEPARATORCHAR, path);
		else if (strlen(path) < sizeof(full))
			strcpy(full, path);
		else
			continue;
		SflMonitorEscape(full, escaped, sizeof(escaped));
		if (used + strlen(escaped) + 8 >= responsesize)
			break;
		if (index > 0)
			response[used++] = ',';
		used += (size_t)sprintf(response + used, "\"%s\"", escaped);
	}
	strcpy(response + used, "]}");
	if (cwd != NULL)
		WSFREE(cwd);
}

static void SflMonitorStatus(char *response, size_t responsesize)
{
	int width;
	int height;
	int depth;
	int hostwidth;
	int hostheight;
	if (!SflUiGetStatus(&width, &height, &depth, &hostwidth, &hostheight)) {
		SflMonitorError(response, responsesize, "framebuffer unavailable");
		return;
	}
	sprintf(response, "{\"ok\":true,\"width\":%d,\"height\":%d,\"depth\":%d,\"hostwidth\":%d,\"hostheight\":%d}",
		width, height, depth, hostwidth, hostheight);
}

static void SflMonitorHostMouse(SFLMONITOR *pThis, int x, int y)
{
	pThis->hostmousex = x;
	pThis->hostmousey = y;
	SflUiHostMouse(x, y);
}

static void SflMonitorExecute(SFLMONITOR *pThis, const char *command, char *response, size_t responsesize)
{
	int first;
	int second;
	int third;
	int fourth;
	int button;
	int holdtime;
	int fields;
	char direction[16];
	if (strcmp(command, "ping") == 0) {
		sprintf(response, "{\"ok\":true,\"monitor\":\"PocketShaver\",\"port\":%d,\"pid\":%lu}", pThis->port, (unsigned long)WSTaskGetId());
		return;
	}
	if (strcmp(command, "help") == 0) {
		sprintf(response, "{\"ok\":true,\"commands\":[\"ping\",\"status\",\"extfs\",\"disks\",\"shot PATH\",\"mouse X Y\",\"mousedown BUTTON\",\"mouseup BUTTON\",\"click X Y [BUTTON]\",\"dblclick X Y [BUTTON]\",\"drag X1 Y1 X2 Y2 [BUTTON [HOLDMS]]\",\"hold X Y MS [BUTTON]\",\"key down CODE\",\"key up CODE\",\"key tap CODE\",\"type TEXT\",\"power\",\"host shot PATH\",\"host mouse X Y\",\"host mousedown BUTTON\",\"host mouseup BUTTON\",\"host click X Y [BUTTON]\",\"quit\"]}");
		return;
	}
	if (strcmp(command, "extfs") == 0) {
		SflMonitorExtFS(response, responsesize);
		return;
	}
	if (strcmp(command, "disks") == 0) {
		SflMonitorDisks(response, responsesize);
		return;
	}
	if (strcmp(command, "status") == 0) {
		SflMonitorStatus(response, responsesize);
		return;
	}
	if (strncmp(command, "shot ", 5) == 0) {
		SflMonitorShot(0, command + 5, response, responsesize);
		return;
	}
	if (strncmp(command, "host shot ", 10) == 0) {
		SflMonitorShot(1, command + 10, response, responsesize);
		return;
	}
	if (sscanf(command, "host mouse %d %d", &first, &second) == 2) {
		SflMonitorHostMouse(pThis, first, second);
		SflMonitorOk(response, responsesize);
		return;
	}
	if (sscanf(command, "host mousedown %d", &first) == 1) {
		if (SflMonitorCheckButton(first, response, responsesize)) {
			SflUiHostButton(1);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	if (sscanf(command, "host mouseup %d", &first) == 1) {
		if (SflMonitorCheckButton(first, response, responsesize)) {
			SflUiHostButton(0);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	fields = sscanf(command, "host click %d %d %d", &first, &second, &button);
	if (fields >= 2) {
		if (fields == 2)
			button = 0;
		if (SflMonitorCheckButton(button, response, responsesize)) {
			SflMonitorHostMouse(pThis, first, second);
			WSThreadSleep(SFLMONITORSETTLEMS);
			SflUiHostButton(1);
			WSThreadSleep(SFLMONITORPRESSMS);
			SflUiHostButton(0);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	if (strcmp(command, "quit") == 0) {
		SflUiRequestQuit();
		SflMonitorOk(response, responsesize);
		return;
	}
	if (sscanf(command, "mouse %d %d", &first, &second) == 2) {
		SflEmulatorMouse(first, second);
		SflMonitorOk(response, responsesize);
		return;
	}
	if (sscanf(command, "mousedown %d", &first) == 1) {
		if (SflMonitorCheckButton(first, response, responsesize)) {
			SflEmulatorMouseButton(first, 1);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	if (sscanf(command, "mouseup %d", &first) == 1) {
		if (SflMonitorCheckButton(first, response, responsesize)) {
			SflEmulatorMouseButton(first, 0);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	fields = sscanf(command, "click %d %d %d", &first, &second, &button);
	if (fields >= 2) {
		if (fields == 2)
			button = 0;
		if (SflMonitorCheckButton(button, response, responsesize)) {
			SflMonitorMoveSettle(first, second);
			SflMonitorClick(button);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	fields = sscanf(command, "dblclick %d %d %d", &first, &second, &button);
	if (fields >= 2) {
		if (fields == 2)
			button = 0;
		if (SflMonitorCheckButton(button, response, responsesize)) {
			SflMonitorMoveSettle(first, second);
			SflMonitorClick(button);
			WSThreadSleep(SFLMONITORPRESSMS);
			SflMonitorClick(button);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	fields = sscanf(command, "drag %d %d %d %d %d %d", &first, &second, &third, &fourth, &button, &holdtime);
	if (fields >= 4) {
		if (fields < 5)
			button = 0;
		if (fields < 6)
			holdtime = SFLMONITORPRESSMS;
		if (SflMonitorCheckButton(button, response, responsesize)) {
			SflMonitorDrag(first, second, third, fourth, button, holdtime);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	fields = sscanf(command, "hold %d %d %d %d", &first, &second, &holdtime, &button);
	if (fields >= 3) {
		if (fields == 3)
			button = 0;
		if (SflMonitorCheckButton(button, response, responsesize)) {
			SflMonitorMoveSettle(first, second);
			SflEmulatorMouseButton(button, 1);
			WSThreadSleep((size_t)holdtime);
			SflEmulatorMouseButton(button, 0);
			SflMonitorOk(response, responsesize);
		}
		return;
	}
	if (strncmp(command, "type ", 5) == 0) {
		SflMonitorType(command + 5, response, responsesize);
		return;
	}
	if (sscanf(command, "key %15s %i", direction, &first) == 2) {
		if (first < 0 || first > SFLMONITORADBMAX) {
			SflMonitorError(response, responsesize, "key code must be 0 through 127");
			return;
		}
		if (strcmp(direction, "down") == 0) {
			SflMonitorKey(first, 1);
			SflMonitorOk(response, responsesize);
			return;
		}
		if (strcmp(direction, "up") == 0) {
			SflMonitorKey(first, 0);
			SflMonitorOk(response, responsesize);
			return;
		}
		if (strcmp(direction, "tap") == 0) {
			SflMonitorKeyTap(first);
			SflMonitorOk(response, responsesize);
			return;
		}
	}
	if (strcmp(command, "power") == 0) {
		SflMonitorKeyTap(SFLMONITORADBPOWER);
		SflMonitorOk(response, responsesize);
		return;
	}
	SflMonitorError(response, responsesize, "unknown command");
}

static void SflMonitorServe(SFLMONITOR *pThis, WSSOCKET *client)
{
	char command[SFLMONITORCOMMANDSIZE];
	char response[SFLMONITORRESPONSESIZE];
	size_t used;
	size_t received;
	used = 0;
	command[0] = 0;
	while (used < sizeof(command) - 1) {
		received = WSSocket_Read(client, command + used, sizeof(command) - used - 1, NULL);
		if (received == 0 || received == (size_t)-1)
			break;
		used += received;
		command[used] = 0;
		if (strchr(command, '\n') != NULL)
			break;
	}
	command[used] = 0;
	SflMonitorTrim(command);
	response[0] = 0;
	SflMonitorExecute(pThis, command, response, sizeof(response) - 2);
	strcat(response, "\n");
	WSSocket_Write(client, response, strlen(response), NULL);
}

static WSTHREADENTRYCALLBACK(SflMonitorThread)
{
	SFLMONITOR *pThis;
	WSSOCKET client;
	pThis = (SFLMONITOR *)cbparam;
	while (!wsatomicintload(&pThis->cancel)) {
		WSSocket_Construct(&client);
		if (!WSSocket_CreateAccepted(&client, &pThis->listener, NULL, &pThis->library))
			break;
		SflMonitorServe(pThis, &client);
		WSSocket_Shutdown(&client);
		WSSocket_Destroy(&client);
	}
	WSSignal_Raise(&pThis->exited);
	return WSTHREADENTRYPROCRETCODE(0);
}

wsbool SflMonitor_Create(SFLMONITOR *pThis, int port)
{
	WSSOCKADDRv4 address;
	WSASSERT(pThis != NULL);
	pThis->port = port;
	pThis->hostmousex = 0;
	pThis->hostmousey = 0;
	pThis->librarycreated = FALSE;
	pThis->listening = FALSE;
	pThis->threadcreated = FALSE;
	wsatomicintstore(&pThis->cancel, 0);
	WSSocket_Construct(&pThis->listener);
	if (!WSSocketLibrary_Create(&pThis->library))
		return FALSE;
	pThis->librarycreated = TRUE;
	WSSockAddrv4_Construct(&address, WSSOCKADDRv4_LOOPBACK, (uint16_t)port);
	if (!WSSocket_CreateServerWithAddr(&pThis->listener, &address, &pThis->library)) {
		SflMonitor_Destroy(pThis);
		return FALSE;
	}
	pThis->listening = TRUE;
	WSSignal_Create(&pThis->exited);
	if (!WSThread_Create(&pThis->thread, SflMonitorThread, pThis)) {
		WSSignal_Destroy(&pThis->exited);
		SflMonitor_Destroy(pThis);
		return FALSE;
	}
	pThis->threadcreated = TRUE;
	return TRUE;
}

void SflMonitor_Destroy(SFLMONITOR *pThis)
{
	wsatomicintstore(&pThis->cancel, 1);
	if (pThis->listening) {
		WSSocket_Shutdown(&pThis->listener);
		WSSocket_Destroy(&pThis->listener);
		pThis->listening = FALSE;
	}
	if (pThis->threadcreated) {
		WSSignal_Wait(&pThis->exited, WSSIGNAL_WAITINFINITE);
		WSSignal_Destroy(&pThis->exited);
		WSThread_Destroy(&pThis->thread);
		pThis->threadcreated = FALSE;
	}
	if (pThis->librarycreated) {
		WSSocketLibrary_Destroy(&pThis->library);
		pThis->librarycreated = FALSE;
	}
}
