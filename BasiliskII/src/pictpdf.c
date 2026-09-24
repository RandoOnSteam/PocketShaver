/*
 *  pictpdf.c - Converts a macintosh PICT to a PDF
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pdfgen.h"
#include "pictpdf.h"

#define PICTPDFSHAPERECT 0
#define PICTPDFSHAPERRECT 1
#define PICTPDFSHAPEOVAL 2
#define PICTPDFSHAPEARC 3
#define PICTPDFSHAPEPOLY 4
#define PICTPDFSHAPEREGION 5

#define PICTPDFVERBFRAME 0
#define PICTPDFVERBPAINT 1
#define PICTPDFVERBERASE 2
#define PICTPDFVERBINVERT 3
#define PICTPDFVERBFILL 4

#define PICTPDFMODESKIP -1
#define PICTPDFMODECOPY 0
#define PICTPDFMODEOR 1
#define PICTPDFMODEXOR 2
#define PICTPDFMODEBIC 3
#define PICTPDFMODENOTCOPY 4
#define PICTPDFMODENOTOR 5
#define PICTPDFMODENOTXOR 6
#define PICTPDFMODENOTBIC 7

#define PICTPDFREGIONFILL 0
#define PICTPDFREGIONFRAME 1

#define PICTPDFFAMILYHELVETICA 0
#define PICTPDFFAMILYTIMES 1
#define PICTPDFFAMILYCOURIER 2
#define PICTPDFFAMILYSYMBOL 3
#define PICTPDFFAMILYDINGBATS 4

#define PICTPDFMAXFONTS 64
#define PICTPDFBEZIERCIRCLE 0.5522847498
#define PICTPDFPI 3.14159265358979323846

typedef struct PICTPDFFONT
{
	long fontid;
	char fontname[64];
} PICTPDFFONT;

typedef struct PICTPDFPATTERN
{
	unsigned char bits[8];
	int ispixpat;
	unsigned long pixcolor;
} PICTPDFPATTERN;

typedef struct PICTPDFPIXMAP
{
	long rowbytes;
	long bounds[4];
	int ispixmap;
	long packtype;
	long pixelsize;
	long cmpcount;
	unsigned long palette[256];
} PICTPDFPIXMAP;

typedef struct PICTPDFSTATE
{
	struct pdf_doc* pdf;
	const unsigned char* picture;
	long picturesize;
	long position;
	int failed;
	int version;
	PICTPDFMAP map;
	long originh;
	long originv;
	long penh;
	long penv;
	long pensizeh;
	long pensizev;
	long penmode;
	PICTPDFPATTERN penpattern;
	PICTPDFPATTERN fillpattern;
	PICTPDFPATTERN backpattern;
	unsigned long foreground;
	unsigned long background;
	long textfont;
	long textsize;
	long textface;
	long textmode;
	long texth;
	long textv;
	long ovalh;
	long ovalv;
	long lastrect[4];
	long arcstart;
	long arcangle;
	long lastpoly;
	long lastregion;
	PICTPDFFONT fonts[PICTPDFMAXFONTS];
	int fontcount;
	const char* pdffont;
	char* content;
	long contentlength;
	long contentcapacity;
} PICTPDFSTATE;

static int PictPdfReadByte(PICTPDFSTATE* state)
{
	if (state->position >= state->picturesize)
	{
		state->failed = 1;
		return 0;
	}
	state->position++;
	return state->picture[state->position - 1];
}

static long PictPdfReadSignedByte(PICTPDFSTATE* state)
{
	long value;

	value = PictPdfReadByte(state);
	if (value >= 0x80)
		value -= 0x100;
	return value;
}

static long PictPdfReadWord(PICTPDFSTATE* state)
{
	long high;

	high = PictPdfReadByte(state);
	return (high << 8) | PictPdfReadByte(state);
}

static long PictPdfReadSignedWord(PICTPDFSTATE* state)
{
	long value;

	value = PictPdfReadWord(state);
	if (value >= 0x8000)
		value -= 0x10000;
	return value;
}

static unsigned long PictPdfReadLong(PICTPDFSTATE* state)
{
	unsigned long high;

	high = (unsigned long)PictPdfReadWord(state);
	return (high << 16) | (unsigned long)PictPdfReadWord(state);
}

static void PictPdfSkip(PICTPDFSTATE* state, unsigned long count)
{
	if (count > (unsigned long)(state->picturesize - state->position))
	{
		state->failed = 1;
		state->position = state->picturesize;
	}
	else
		state->position += (long)count;
}

static void PictPdfSkipWordLength(PICTPDFSTATE* state)
{
	PictPdfSkip(state, (unsigned long)PictPdfReadWord(state));
}

static void PictPdfSkipSizedRecord(PICTPDFSTATE* state)
{
	long size;

	size = PictPdfReadWord(state);
	if (size < 2)
		state->failed = 1;
	else
		PictPdfSkip(state, (unsigned long)(size - 2));
}

static void PictPdfReadRect(PICTPDFSTATE* state, long* rect)
{
	rect[0] = PictPdfReadSignedWord(state);
	rect[1] = PictPdfReadSignedWord(state);
	rect[2] = PictPdfReadSignedWord(state);
	rect[3] = PictPdfReadSignedWord(state);
}

static unsigned long PictPdfReadRGB(PICTPDFSTATE* state)
{
	unsigned long red;
	unsigned long green;

	red = (unsigned long)PictPdfReadWord(state) >> 8;
	green = (unsigned long)PictPdfReadWord(state) >> 8;
	return (red << 16) | (green << 8) | ((unsigned long)PictPdfReadWord(state) >> 8);
}

static void PictPdfAppend(PICTPDFSTATE* state, const char* text)
{
	long length;
	long capacity;
	char* grown;

	length = (long)strlen(text);
	if (state->contentlength + length + 1 > state->contentcapacity)
	{
		capacity = state->contentcapacity;
		if (capacity < 4096)
			capacity = 4096;
		while (capacity < state->contentlength + length + 1)
			capacity *= 2;
		grown = (char*)realloc(state->content, (size_t)capacity);
		if (!grown)
		{
			state->failed = 1;
			return;
		}
		state->content = grown;
		state->contentcapacity = capacity;
	}
	memcpy(state->content + state->contentlength, text, (size_t)(length + 1));
	state->contentlength += length;
}

static void PictPdfAppendNumber(PICTPDFSTATE* state, double value)
{
	char number[64];
	char* cursor;

	if (value > -0.0005 && value < 0.0005)
		value = 0;
	sprintf(number, "%.3f", value);
	for (cursor = number; *cursor; cursor++)
	{
		if (*cursor == ',')
			*cursor = '.';
	}
	cursor--;
	while (*cursor == '0')
	{
		*cursor = 0;
		cursor--;
	}
	if (*cursor == '.')
		*cursor = 0;
	PictPdfAppend(state, number);
	PictPdfAppend(state, " ");
}

static void PictPdfFlush(PICTPDFSTATE* state)
{
	if (state->contentlength > 0)
	{
		pdf_add_raw_stream(state->pdf, NULL, state->content);
		state->contentlength = 0;
		state->content[0] = 0;
	}
}

static double PictPdfMapX(const PICTPDFSTATE* state, double h)
{
	return (h - state->originh - state->map.left) * state->map.scalex;
}

static double PictPdfMapY(const PICTPDFSTATE* state, double v)
{
	return state->map.pageheight - (v - state->originv - state->map.top) * state->map.scaley;
}

static void PictPdfPoint(PICTPDFSTATE* state, double h, double v, const char* operation)
{
	PictPdfAppendNumber(state, PictPdfMapX(state, h));
	PictPdfAppendNumber(state, PictPdfMapY(state, v));
	PictPdfAppend(state, operation);
}

static void PictPdfCurve(PICTPDFSTATE* state, double h1, double v1, double h2, double v2, double h3, double v3)
{
	PictPdfAppendNumber(state, PictPdfMapX(state, h1));
	PictPdfAppendNumber(state, PictPdfMapY(state, v1));
	PictPdfAppendNumber(state, PictPdfMapX(state, h2));
	PictPdfAppendNumber(state, PictPdfMapY(state, v2));
	PictPdfPoint(state, h3, v3, "c\n");
}

static void PictPdfColor(PICTPDFSTATE* state, unsigned long color, const char* operation)
{
	PictPdfAppendNumber(state, ((color >> 16) & 0xFF) / 255.0);
	PictPdfAppendNumber(state, ((color >> 8) & 0xFF) / 255.0);
	PictPdfAppendNumber(state, (color & 0xFF) / 255.0);
	PictPdfAppend(state, operation);
}

static void PictPdfRectPath(PICTPDFSTATE* state, double top, double left, double bottom, double right)
{
	PictPdfPoint(state, left, top, "m\n");
	PictPdfPoint(state, right, top, "l\n");
	PictPdfPoint(state, right, bottom, "l\n");
	PictPdfPoint(state, left, bottom, "l\n");
	PictPdfAppend(state, "h\n");
}

static void PictPdfOvalPath(PICTPDFSTATE* state, double top, double left, double bottom, double right)
{
	double centerh;
	double centerv;
	double offseth;
	double offsetv;

	centerh = (left + right) / 2;
	centerv = (top + bottom) / 2;
	offseth = (right - left) / 2 * PICTPDFBEZIERCIRCLE;
	offsetv = (bottom - top) / 2 * PICTPDFBEZIERCIRCLE;
	PictPdfPoint(state, centerh, top, "m\n");
	PictPdfCurve(state, centerh + offseth, top, right, centerv - offsetv, right, centerv);
	PictPdfCurve(state, right, centerv + offsetv, centerh + offseth, bottom, centerh, bottom);
	PictPdfCurve(state, centerh - offseth, bottom, left, centerv + offsetv, left, centerv);
	PictPdfCurve(state, left, centerv - offsetv, centerh - offseth, top, centerh, top);
	PictPdfAppend(state, "h\n");
}

static void PictPdfRoundRectPath(PICTPDFSTATE* state, double top, double left, double bottom, double right, double ovalwidth, double ovalheight)
{
	double radiush;
	double radiusv;
	double offseth;
	double offsetv;

	radiush = ovalwidth / 2;
	radiusv = ovalheight / 2;
	if (radiush > (right - left) / 2)
		radiush = (right - left) / 2;
	if (radiusv > (bottom - top) / 2)
		radiusv = (bottom - top) / 2;
	if (radiush <= 0 || radiusv <= 0)
	{
		PictPdfRectPath(state, top, left, bottom, right);
		return;
	}
	offseth = radiush * (1 - PICTPDFBEZIERCIRCLE);
	offsetv = radiusv * (1 - PICTPDFBEZIERCIRCLE);
	PictPdfPoint(state, left + radiush, top, "m\n");
	PictPdfPoint(state, right - radiush, top, "l\n");
	PictPdfCurve(state, right - offseth, top, right, top + offsetv, right, top + radiusv);
	PictPdfPoint(state, right, bottom - radiusv, "l\n");
	PictPdfCurve(state, right, bottom - offsetv, right - offseth, bottom, right - radiush, bottom);
	PictPdfPoint(state, left + radiush, bottom, "l\n");
	PictPdfCurve(state, left + offseth, bottom, left, bottom - offsetv, left, bottom - radiusv);
	PictPdfPoint(state, left, top + radiusv, "l\n");
	PictPdfCurve(state, left, top + offsetv, left + offseth, top, left + radiush, top);
	PictPdfAppend(state, "h\n");
}

static void PictPdfArcPath(PICTPDFSTATE* state, double top, double left, double bottom, double right, long startangle, long arcangle, int wedge)
{
	double centerh;
	double centerv;
	double radiush;
	double radiusv;
	double from;
	double to;
	double step;
	double handle;
	int segments;
	int segment;

	if (arcangle < 0)
	{
		startangle += arcangle;
		arcangle = -arcangle;
	}
	if (arcangle > 360)
		arcangle = 360;
	segments = (int)((arcangle + 89) / 90);
	if (segments == 0)
		return;
	centerh = (left + right) / 2;
	centerv = (top + bottom) / 2;
	radiush = (right - left) / 2;
	radiusv = (bottom - top) / 2;
	step = arcangle * PICTPDFPI / 180 / segments;
	from = startangle * PICTPDFPI / 180;
	handle = 4.0 / 3.0 * tan(step / 4);
	if (wedge)
	{
		PictPdfPoint(state, centerh, centerv, "m\n");
		PictPdfPoint(state, centerh + radiush * sin(from), centerv - radiusv * cos(from), "l\n");
	}
	else
		PictPdfPoint(state, centerh + radiush * sin(from), centerv - radiusv * cos(from), "m\n");
	for (segment = 0; segment < segments; segment++)
	{
		to = from + step;
		PictPdfCurve(state,
			centerh + radiush * sin(from) + handle * radiush * cos(from),
			centerv - radiusv * cos(from) + handle * radiusv * sin(from),
			centerh + radiush * sin(to) - handle * radiush * cos(to),
			centerv - radiusv * cos(to) - handle * radiusv * sin(to),
			centerh + radiush * sin(to),
			centerv - radiusv * cos(to));
		from = to;
	}
	if (wedge)
		PictPdfAppend(state, "h\n");
}

static void PictPdfShapePath(PICTPDFSTATE* state, int shape, double top, double left, double bottom, double right, double ovalwidth, double ovalheight)
{
	if (shape == PICTPDFSHAPERRECT)
		PictPdfRoundRectPath(state, top, left, bottom, right, ovalwidth, ovalheight);
	else if (shape == PICTPDFSHAPEOVAL)
		PictPdfOvalPath(state, top, left, bottom, right);
	else
		PictPdfRectPath(state, top, left, bottom, right);
}

static int PictPdfBaseMode(long mode)
{
	if (mode == 50)
		return PICTPDFMODESKIP;
	if (mode == 36 || mode == 49)
		return PICTPDFMODEOR;
	if (mode >= 0 && mode <= 15)
		return (int)(mode & 7);
	return PICTPDFMODECOPY;
}

static unsigned long PictPdfBlend(unsigned long foreground, unsigned long background, double coverage)
{
	unsigned long result;
	double channel;
	int shift;

	result = 0;
	for (shift = 0; shift <= 16; shift += 8)
	{
		channel = ((foreground >> shift) & 0xFF) * coverage + ((background >> shift) & 0xFF) * (1 - coverage);
		result |= ((unsigned long)(channel + 0.5) & 0xFF) << shift;
	}
	return result;
}

static int PictPdfPatternColor(const PICTPDFSTATE* state, const PICTPDFPATTERN* pattern, long mode, unsigned long* color)
{
	unsigned long foreground;
	double coverage;
	int basemode;
	int bits;
	int index;
	int byte;

	basemode = PictPdfBaseMode(mode);
	if (basemode == PICTPDFMODESKIP)
		return 0;
	foreground = state->foreground;
	coverage = 1;
	if (pattern->ispixpat)
		foreground = pattern->pixcolor;
	else
	{
		bits = 0;
		for (index = 0; index < 8; index++)
		{
			for (byte = pattern->bits[index]; byte; byte &= byte - 1)
				bits++;
		}
		coverage = bits / 64.0;
	}
	if (basemode >= PICTPDFMODENOTCOPY)
	{
		coverage = 1 - coverage;
		basemode -= PICTPDFMODENOTCOPY;
	}
	if (basemode == PICTPDFMODEXOR)
		return 0;
	if (basemode != PICTPDFMODECOPY && coverage <= 0)
		return 0;
	if (basemode == PICTPDFMODEBIC)
		*color = state->background;
	else
		*color = PictPdfBlend(foreground, state->background, coverage);
	return 1;
}

static int PictPdfVerbColor(const PICTPDFSTATE* state, int verb, unsigned long* color)
{
	if (verb == PICTPDFVERBFRAME || verb == PICTPDFVERBPAINT)
		return PictPdfPatternColor(state, &state->penpattern, state->penmode, color);
	if (verb == PICTPDFVERBFILL)
		return PictPdfPatternColor(state, &state->fillpattern, 8, color);
	if (verb == PICTPDFVERBERASE)
		return PictPdfPatternColor(state, &state->backpattern, 8, color);
	return 0;
}

static void PictPdfToggleRegionPoint(long* points, long* pointcount, long capacity, long x)
{
	long index;

	for (index = 0; index < *pointcount && points[index] < x; index++)
		;
	if (index < *pointcount && points[index] == x)
	{
		memmove(points + index, points + index + 1, (size_t)(*pointcount - index - 1) * sizeof(long));
		(*pointcount)--;
	}
	else if (*pointcount < capacity)
	{
		memmove(points + index + 1, points + index, (size_t)(*pointcount - index) * sizeof(long));
		points[index] = x;
		(*pointcount)++;
	}
}

static void PictPdfRegionBand(PICTPDFSTATE* state, const long* points, long pointcount, long top, long bottom, int action)
{
	long index;

	for (index = 0; index + 1 < pointcount; index += 2)
	{
		if (action == PICTPDFREGIONFILL)
			PictPdfRectPath(state, top, points[index], bottom, points[index + 1]);
		else
		{
			PictPdfPoint(state, points[index], top, "m\n");
			PictPdfPoint(state, points[index], bottom, "l\n");
			PictPdfPoint(state, points[index + 1], top, "m\n");
			PictPdfPoint(state, points[index + 1], bottom, "l\n");
		}
	}
}

static void PictPdfRegionPath(PICTPDFSTATE* state, long regionposition, int action)
{
	long saved;
	long size;
	long end;
	long bounds[4];
	long* points;
	long pointcount;
	long capacity;
	long y;
	long previousy;
	long x;
	long pendingx;
	int haspending;
	int first;

	saved = state->position;
	state->position = regionposition;
	size = PictPdfReadWord(state);
	PictPdfReadRect(state, bounds);
	end = regionposition + size;
	if (size <= 10 || end > state->picturesize)
	{
		if (bounds[2] > bounds[0] && bounds[3] > bounds[1])
			PictPdfRectPath(state, bounds[0], bounds[1], bounds[2], bounds[3]);
		state->position = saved;
		return;
	}
	capacity = (size - 10) / 2 + 1;
	points = (long*)malloc((size_t)capacity * sizeof(long));
	if (!points)
	{
		state->failed = 1;
		state->position = saved;
		return;
	}
	pointcount = 0;
	previousy = 0;
	first = 1;
	while (state->position + 2 <= end && !state->failed)
	{
		y = PictPdfReadSignedWord(state);
		if (y == 0x7FFF)
			break;
		if (!first)
			PictPdfRegionBand(state, points, pointcount, previousy, y, action);
		first = 0;
		previousy = y;
		haspending = 0;
		pendingx = 0;
		while (state->position + 2 <= end && !state->failed)
		{
			x = PictPdfReadSignedWord(state);
			if (x == 0x7FFF)
				break;
			PictPdfToggleRegionPoint(points, &pointcount, capacity, x);
			if (action == PICTPDFREGIONFRAME)
			{
				if (haspending)
				{
					PictPdfPoint(state, pendingx, y, "m\n");
					PictPdfPoint(state, x, y, "l\n");
					haspending = 0;
				}
				else
				{
					pendingx = x;
					haspending = 1;
				}
			}
		}
	}
	free(points);
	state->position = saved;
}

static void PictPdfSetClip(PICTPDFSTATE* state, long regionposition)
{
	long saved;
	long bounds[4];

	PictPdfAppend(state, "Q q\n");
	saved = state->position;
	state->position = regionposition + 2;
	PictPdfReadRect(state, bounds);
	state->position = saved;
	if (bounds[0] <= -32000 && bounds[1] <= -32000 && bounds[2] >= 32000 && bounds[3] >= 32000)
		return;
	PictPdfAppend(state, "0 0 0 0 re\n");
	PictPdfRegionPath(state, regionposition, PICTPDFREGIONFILL);
	PictPdfAppend(state, "W n\n");
}

static void PictPdfLineHull(PICTPDFSTATE* state, double fromh, double fromv, double toh, double tov)
{
	double width;
	double height;
	double h[6];
	double v[6];
	int index;

	width = state->pensizeh;
	height = state->pensizev;
	if (toh >= fromh && tov >= fromv)
	{
		h[0] = fromh; v[0] = fromv;
		h[1] = fromh + width; v[1] = fromv;
		h[2] = toh + width; v[2] = tov;
		h[3] = toh + width; v[3] = tov + height;
		h[4] = toh; v[4] = tov + height;
		h[5] = fromh; v[5] = fromv + height;
	}
	else if (toh < fromh && tov >= fromv)
	{
		h[0] = fromh; v[0] = fromv;
		h[1] = fromh + width; v[1] = fromv;
		h[2] = fromh + width; v[2] = fromv + height;
		h[3] = toh + width; v[3] = tov + height;
		h[4] = toh; v[4] = tov + height;
		h[5] = toh; v[5] = tov;
	}
	else if (toh >= fromh)
	{
		h[0] = fromh; v[0] = fromv;
		h[1] = toh; v[1] = tov;
		h[2] = toh + width; v[2] = tov;
		h[3] = toh + width; v[3] = tov + height;
		h[4] = fromh + width; v[4] = fromv + height;
		h[5] = fromh; v[5] = fromv + height;
	}
	else
	{
		h[0] = toh; v[0] = tov;
		h[1] = toh + width; v[1] = tov;
		h[2] = fromh + width; v[2] = fromv;
		h[3] = fromh + width; v[3] = fromv + height;
		h[4] = fromh; v[4] = fromv + height;
		h[5] = toh; v[5] = tov + height;
	}
	PictPdfPoint(state, h[0], v[0], "m\n");
	for (index = 1; index < 6; index++)
		PictPdfPoint(state, h[index], v[index], "l\n");
	PictPdfAppend(state, "h\n");
}

static void PictPdfDrawLine(PICTPDFSTATE* state, long fromh, long fromv, long toh, long tov)
{
	unsigned long color;

	if (state->pensizeh <= 0 || state->pensizev <= 0)
		return;
	if (!PictPdfPatternColor(state, &state->penpattern, state->penmode, &color))
		return;
	PictPdfColor(state, color, "rg\n");
	PictPdfLineHull(state, fromh, fromv, toh, tov);
	PictPdfAppend(state, "f\n");
}

static void PictPdfDrawShape(PICTPDFSTATE* state, int shape, int verb, const long* rect)
{
	unsigned long color;
	double inseth;
	double insetv;

	if (rect[2] <= rect[0] || rect[3] <= rect[1])
		return;
	if (verb == PICTPDFVERBFRAME && (state->pensizeh <= 0 || state->pensizev <= 0))
		return;
	if (!PictPdfVerbColor(state, verb, &color))
		return;
	if (shape == PICTPDFSHAPEARC && verb == PICTPDFVERBFRAME)
	{
		inseth = state->pensizeh / 2.0;
		insetv = state->pensizev / 2.0;
		PictPdfColor(state, color, "RG\n");
		PictPdfAppendNumber(state, (state->pensizeh * state->map.scalex + state->pensizev * state->map.scaley) / 2);
		PictPdfAppend(state, "w 0 J\n");
		PictPdfArcPath(state, rect[0] + insetv, rect[1] + inseth, rect[2] - insetv, rect[3] - inseth, state->arcstart, state->arcangle, 0);
		PictPdfAppend(state, "S\n");
		return;
	}
	PictPdfColor(state, color, "rg\n");
	if (shape == PICTPDFSHAPEARC)
	{
		PictPdfArcPath(state, rect[0], rect[1], rect[2], rect[3], state->arcstart, state->arcangle, 1);
		PictPdfAppend(state, "f\n");
		return;
	}
	PictPdfShapePath(state, shape, rect[0], rect[1], rect[2], rect[3], state->ovalh, state->ovalv);
	if (verb == PICTPDFVERBFRAME)
	{
		if (rect[2] - rect[0] > 2 * state->pensizev && rect[3] - rect[1] > 2 * state->pensizeh)
			PictPdfShapePath(state, shape, rect[0] + state->pensizev, rect[1] + state->pensizeh, rect[2] - state->pensizev, rect[3] - state->pensizeh, state->ovalh - 2 * state->pensizeh, state->ovalv - 2 * state->pensizev);
		PictPdfAppend(state, "f*\n");
	}
	else
		PictPdfAppend(state, "f\n");
}

static void PictPdfDrawPoly(PICTPDFSTATE* state, int verb, long polyposition)
{
	unsigned long color;
	long saved;
	long size;
	long count;
	long index;
	long bounds[4];
	long v;
	long h;
	long previoush;
	long previousv;

	if (verb == PICTPDFVERBFRAME && (state->pensizeh <= 0 || state->pensizev <= 0))
		return;
	if (!PictPdfVerbColor(state, verb, &color))
		return;
	saved = state->position;
	state->position = polyposition;
	size = PictPdfReadWord(state);
	PictPdfReadRect(state, bounds);
	count = (size - 10) / 4;
	if (count < 1)
	{
		state->position = saved;
		return;
	}
	PictPdfColor(state, color, "rg\n");
	previoush = 0;
	previousv = 0;
	for (index = 0; index < count && !state->failed; index++)
	{
		v = PictPdfReadSignedWord(state);
		h = PictPdfReadSignedWord(state);
		if (verb == PICTPDFVERBFRAME)
		{
			if (index > 0)
				PictPdfLineHull(state, previoush, previousv, h, v);
		}
		else if (index == 0)
			PictPdfPoint(state, h, v, "m\n");
		else
			PictPdfPoint(state, h, v, "l\n");
		previoush = h;
		previousv = v;
	}
	if (verb == PICTPDFVERBFRAME)
		PictPdfAppend(state, "f\n");
	else
		PictPdfAppend(state, "h f*\n");
	state->position = saved;
}

static void PictPdfDrawRegion(PICTPDFSTATE* state, int verb, long regionposition)
{
	unsigned long color;

	if (!PictPdfVerbColor(state, verb, &color))
		return;
	if (verb == PICTPDFVERBFRAME)
	{
		if (state->pensizeh <= 0 || state->pensizev <= 0)
			return;
		PictPdfColor(state, color, "RG\n");
		PictPdfAppendNumber(state, (state->pensizeh * state->map.scalex + state->pensizev * state->map.scaley) / 2);
		PictPdfAppend(state, "w 0 J\n");
		PictPdfRegionPath(state, regionposition, PICTPDFREGIONFRAME);
		PictPdfAppend(state, "S\n");
		return;
	}
	PictPdfColor(state, color, "rg\n");
	PictPdfRegionPath(state, regionposition, PICTPDFREGIONFILL);
	PictPdfAppend(state, "f\n");
}

static void PictPdfShapeOpcode(PICTPDFSTATE* state, long opcode)
{
	int shape;
	int verb;
	int same;
	long size;

	shape = (int)((opcode - 0x30) >> 4);
	verb = (int)(opcode & 7);
	same = (opcode & 8) != 0;
	if (shape <= PICTPDFSHAPEARC)
	{
		if (!same)
			PictPdfReadRect(state, state->lastrect);
		if (shape == PICTPDFSHAPEARC)
		{
			state->arcstart = PictPdfReadSignedWord(state);
			state->arcangle = PictPdfReadSignedWord(state);
		}
		if (verb <= PICTPDFVERBFILL && !state->failed)
			PictPdfDrawShape(state, shape, verb, state->lastrect);
		return;
	}
	if (!same)
	{
		if (shape == PICTPDFSHAPEPOLY)
			state->lastpoly = state->position;
		else
			state->lastregion = state->position;
		size = PictPdfReadWord(state);
		if (size < 10)
		{
			state->failed = 1;
			return;
		}
		PictPdfSkip(state, (unsigned long)(size - 2));
	}
	if (verb > PICTPDFVERBFILL || state->failed)
		return;
	if (shape == PICTPDFSHAPEPOLY && state->lastpoly)
		PictPdfDrawPoly(state, verb, state->lastpoly);
	else if (shape == PICTPDFSHAPEREGION && state->lastregion)
		PictPdfDrawRegion(state, verb, state->lastregion);
}

static const char* PictPdfFontName(const PICTPDFSTATE* state, int* family)
{
	static const char* const fontnames[5][4] =
	{
		{ "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique" },
		{ "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic" },
		{ "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique" },
		{ "Symbol", "Symbol", "Symbol", "Symbol" },
		{ "ZapfDingbats", "ZapfDingbats", "ZapfDingbats", "ZapfDingbats" }
	};
	char lowered[64];
	const char* name;
	int index;

	name = "";
	for (index = 0; index < state->fontcount; index++)
	{
		if (state->fonts[index].fontid == state->textfont)
			name = state->fonts[index].fontname;
	}
	for (index = 0; name[index] && index < 63; index++)
	{
		lowered[index] = name[index];
		if (lowered[index] >= 'A' && lowered[index] <= 'Z')
			lowered[index] = (char)(lowered[index] - 'A' + 'a');
	}
	lowered[index] = 0;
	*family = PICTPDFFAMILYHELVETICA;
	if (lowered[0] == 0)
	{
		if (state->textfont == 2 || state->textfont == 20)
			*family = PICTPDFFAMILYTIMES;
		else if (state->textfont == 4 || state->textfont == 22)
			*family = PICTPDFFAMILYCOURIER;
		else if (state->textfont == 23)
			*family = PICTPDFFAMILYSYMBOL;
	}
	else if (strstr(lowered, "sans"))
		*family = PICTPDFFAMILYHELVETICA;
	else if (strstr(lowered, "symbol"))
		*family = PICTPDFFAMILYSYMBOL;
	else if (strstr(lowered, "dingbat"))
		*family = PICTPDFFAMILYDINGBATS;
	else if (strstr(lowered, "courier") || strstr(lowered, "monaco") || strstr(lowered, "mono") || strstr(lowered, "typewriter"))
		*family = PICTPDFFAMILYCOURIER;
	else if (strstr(lowered, "times") || strstr(lowered, "new york") || strstr(lowered, "palatino") || strstr(lowered, "book") ||
		strstr(lowered, "century") || strstr(lowered, "garamond") || strstr(lowered, "georgia") || strstr(lowered, "roman") ||
		strstr(lowered, "serif") || strstr(lowered, "schoolbook"))
		*family = PICTPDFFAMILYTIMES;
	return fontnames[*family][state->textface & 3];
}

static void PictPdfTextToUtf8(const unsigned char* text, long count, int rawencoding, char* output)
{
	static const unsigned short macroman[128] =
	{
		0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
		0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
		0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x003D, 0x00C6, 0x00D8,
		0x003F, 0x00B1, 0x003C, 0x003E, 0x00A5, 0x00B5, 0x003F, 0x003F, 0x003F, 0x003F, 0x003F, 0x00AA, 0x00BA, 0x003F, 0x00E6, 0x00F8,
		0x00BF, 0x00A1, 0x00AC, 0x003F, 0x0192, 0x007E, 0x003F, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
		0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x003F, 0x00FF, 0x0178, 0x002F, 0x20AC, 0x2039, 0x203A, 0x0000, 0x0000,
		0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
		0x003F, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0069, 0x02C6, 0x02DC, 0x00AF, 0x003F, 0x003F, 0x00B0, 0x00B8, 0x0022, 0x003F, 0x003F
	};
	unsigned long codepoint;
	long index;
	char* cursor;

	cursor = output;
	for (index = 0; index < count; index++)
	{
		codepoint = text[index];
		if (codepoint < 0x20)
			continue;
		if (codepoint >= 0x80 && !rawencoding)
		{
			codepoint = macroman[codepoint - 0x80];
			if (codepoint == 0)
			{
				*cursor++ = 'f';
				if (text[index] == 0xDE)
					*cursor++ = 'i';
				else
					*cursor++ = 'l';
				continue;
			}
		}
		if (codepoint < 0x80)
			*cursor++ = (char)codepoint;
		else if (codepoint < 0x800)
		{
			*cursor++ = (char)(0xC0 | (codepoint >> 6));
			*cursor++ = (char)(0x80 | (codepoint & 0x3F));
		}
		else
		{
			*cursor++ = (char)(0xE0 | (codepoint >> 12));
			*cursor++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
			*cursor++ = (char)(0x80 | (codepoint & 0x3F));
		}
	}
	*cursor = 0;
}

static void PictPdfDrawText(PICTPDFSTATE* state, long count)
{
	unsigned char text[256];
	char utf8[1024];
	const char* fontname;
	unsigned long color;
	double size;
	double x;
	double y;
	float width;
	long index;
	int basemode;
	int family;

	for (index = 0; index < count; index++)
		text[index] = (unsigned char)PictPdfReadByte(state);
	if (state->failed)
		return;
	basemode = PictPdfBaseMode(state->textmode);
	if (basemode == PICTPDFMODESKIP || basemode == PICTPDFMODEXOR || basemode == PICTPDFMODENOTXOR)
		return;
	color = state->foreground;
	if (basemode == PICTPDFMODEBIC || basemode == PICTPDFMODENOTBIC)
		color = state->background;
	if (state->textmode == 49)
		color = PictPdfBlend(state->foreground, state->background, 0.5);
	fontname = PictPdfFontName(state, &family);
	PictPdfTextToUtf8(text, count, family == PICTPDFFAMILYSYMBOL || family == PICTPDFFAMILYDINGBATS, utf8);
	if (!utf8[0])
		return;
	size = state->textsize;
	if (size <= 0)
		size = 12;
	size *= state->map.scaley;
	x = PictPdfMapX(state, state->texth);
	y = PictPdfMapY(state, state->textv);
	PictPdfFlush(state);
	if (state->pdffont != fontname)
	{
		pdf_set_font(state->pdf, fontname);
		state->pdffont = fontname;
	}
	pdf_add_text(state->pdf, NULL, utf8, (float)size, (float)x, (float)y, (uint32_t)color);
	if ((state->textface & 4) && pdf_get_font_text_width(state->pdf, fontname, utf8, (float)size, &width) >= 0)
	{
		PictPdfColor(state, color, "rg\n");
		PictPdfAppendNumber(state, x);
		PictPdfAppendNumber(state, y - size * 0.15);
		PictPdfAppendNumber(state, width);
		PictPdfAppendNumber(state, size * 0.05);
		PictPdfAppend(state, "re f\n");
	}
}

static void PictPdfReadPattern(PICTPDFSTATE* state, PICTPDFPATTERN* pattern)
{
	int index;

	for (index = 0; index < 8; index++)
		pattern->bits[index] = (unsigned char)PictPdfReadByte(state);
	pattern->ispixpat = 0;
}

static unsigned long PictPdfOldColor(unsigned long value)
{
	switch (value)
	{
	case 30:
		return 0xFFFFFF;
	case 69:
		return 0xFFFF00;
	case 137:
		return 0xFF00FF;
	case 205:
		return 0xFF0000;
	case 273:
		return 0x00FFFF;
	case 341:
		return 0x00FF00;
	case 409:
		return 0x0000FF;
	}
	return 0;
}

static void PictPdfReadPixMap(PICTPDFSTATE* state, PICTPDFPIXMAP* pixmap, int direct)
{
	long flags;
	long size;
	long index;
	long value;
	long entry;
	unsigned long color;

	if (direct)
		PictPdfSkip(state, 4);
	pixmap->rowbytes = PictPdfReadWord(state);
	pixmap->ispixmap = direct || (pixmap->rowbytes & 0x8000) != 0;
	pixmap->rowbytes &= 0x3FFF;
	PictPdfReadRect(state, pixmap->bounds);
	pixmap->packtype = 0;
	pixmap->pixelsize = 1;
	pixmap->cmpcount = 1;
	pixmap->palette[0] = state->background;
	pixmap->palette[1] = state->foreground;
	if (!pixmap->ispixmap)
		return;
	PictPdfSkip(state, 2);
	pixmap->packtype = PictPdfReadWord(state);
	PictPdfSkip(state, 14);
	pixmap->pixelsize = PictPdfReadWord(state);
	pixmap->cmpcount = PictPdfReadWord(state);
	PictPdfSkip(state, 14);
	if (direct)
		return;
	PictPdfSkip(state, 4);
	flags = PictPdfReadWord(state);
	size = PictPdfReadWord(state);
	for (index = 0; index <= size && !state->failed; index++)
	{
		value = PictPdfReadWord(state);
		color = PictPdfReadRGB(state);
		entry = index;
		if (!(flags & 0x8000))
			entry = value & 0xFF;
		if (entry < 256)
			pixmap->palette[entry] = color;
	}
}

static void PictPdfUnpackBits(PICTPDFSTATE* state, long packedlength, unsigned char* output, long outputlength, int unitsize)
{
	long end;
	long written;
	long count;
	long unit;
	int flag;
	unsigned char value[2];

	end = state->position + packedlength;
	if (packedlength < 0 || end > state->picturesize)
	{
		state->failed = 1;
		return;
	}
	written = 0;
	while (state->position < end && !state->failed)
	{
		flag = PictPdfReadByte(state);
		if (flag < 128)
		{
			for (count = flag + 1; count > 0; count--)
			{
				for (unit = 0; unit < unitsize; unit++)
				{
					value[0] = (unsigned char)PictPdfReadByte(state);
					if (written < outputlength)
						output[written++] = value[0];
				}
			}
		}
		else if (flag > 128)
		{
			for (unit = 0; unit < unitsize; unit++)
				value[unit] = (unsigned char)PictPdfReadByte(state);
			for (count = 257 - flag; count > 0; count--)
			{
				for (unit = 0; unit < unitsize; unit++)
				{
					if (written < outputlength)
						output[written++] = value[unit];
				}
			}
		}
	}
	state->position = end;
}

static unsigned char* PictPdfReadPixels(PICTPDFSTATE* state, const PICTPDFPIXMAP* pixmap, int packed, long* rowlength)
{
	unsigned char* pixels;
	unsigned char* row;
	unsigned char* planes;
	long width;
	long height;
	long packtype;
	long y;
	long x;
	long packedlength;
	long planeoffset;

	width = pixmap->bounds[3] - pixmap->bounds[1];
	height = pixmap->bounds[2] - pixmap->bounds[0];
	if (width <= 0 || height <= 0 || pixmap->rowbytes <= 0)
	{
		state->failed = 1;
		return NULL;
	}
	if (pixmap->pixelsize != 1 && pixmap->pixelsize != 2 && pixmap->pixelsize != 4 && pixmap->pixelsize != 8 &&
		pixmap->pixelsize != 16 && pixmap->pixelsize != 32)
	{
		state->failed = 1;
		return NULL;
	}
	packtype = pixmap->packtype;
	if (pixmap->pixelsize == 32 && packtype == 0)
		packtype = 4;
	if (pixmap->pixelsize == 16 && packtype == 0)
		packtype = 3;
	if (pixmap->pixelsize <= 8)
		packtype = 0;
	if (pixmap->rowbytes < 8 || !packed)
		packtype = 1;
	*rowlength = pixmap->rowbytes;
	if (pixmap->pixelsize == 32)
		*rowlength = width * 3;
	pixels = (unsigned char*)calloc((size_t)height, (size_t)*rowlength);
	planes = (unsigned char*)calloc(1, (size_t)(pixmap->rowbytes + width * 4));
	if (!pixels || !planes)
	{
		free(pixels);
		free(planes);
		state->failed = 1;
		return NULL;
	}
	for (y = 0; y < height && !state->failed; y++)
	{
		row = pixels + y * *rowlength;
		if (packtype == 2)
		{
			for (x = 0; x < width * 3; x++)
				row[x] = (unsigned char)PictPdfReadByte(state);
			continue;
		}
		if (packtype == 1)
		{
			for (x = 0; x < pixmap->rowbytes; x++)
				planes[x] = (unsigned char)PictPdfReadByte(state);
		}
		else
		{
			if (pixmap->rowbytes > 250)
				packedlength = PictPdfReadWord(state);
			else
				packedlength = PictPdfReadByte(state);
			if (packtype == 3)
				PictPdfUnpackBits(state, packedlength, planes, pixmap->rowbytes, 2);
			else if (packtype == 4)
				PictPdfUnpackBits(state, packedlength, planes, pixmap->cmpcount * width, 1);
			else
				PictPdfUnpackBits(state, packedlength, planes, pixmap->rowbytes, 1);
		}
		if (pixmap->pixelsize != 32)
			memcpy(row, planes, (size_t)pixmap->rowbytes);
		else if (packtype == 1)
		{
			for (x = 0; x < width && x * 4 + 3 < pixmap->rowbytes; x++)
			{
				row[x * 3] = planes[x * 4 + 1];
				row[x * 3 + 1] = planes[x * 4 + 2];
				row[x * 3 + 2] = planes[x * 4 + 3];
			}
		}
		else
		{
			planeoffset = 0;
			if (pixmap->cmpcount >= 4)
				planeoffset = width;
			for (x = 0; x < width; x++)
			{
				row[x * 3] = planes[planeoffset + x];
				row[x * 3 + 1] = planes[planeoffset + width + x];
				row[x * 3 + 2] = planes[planeoffset + width * 2 + x];
			}
		}
	}
	free(planes);
	if (state->failed)
	{
		free(pixels);
		return NULL;
	}
	return pixels;
}

static unsigned long PictPdfPixelColor(const PICTPDFPIXMAP* pixmap, const unsigned char* row, long rowlength, long x)
{
	long value;
	long bitoffset;
	unsigned long red;
	unsigned long green;
	unsigned long blue;

	if (pixmap->pixelsize == 32)
		return ((unsigned long)row[x * 3] << 16) | ((unsigned long)row[x * 3 + 1] << 8) | row[x * 3 + 2];
	if (pixmap->pixelsize == 16)
	{
		if (x * 2 + 1 >= rowlength)
			return 0;
		value = ((long)row[x * 2] << 8) | row[x * 2 + 1];
		red = (value >> 10) & 31;
		green = (value >> 5) & 31;
		blue = value & 31;
		return (((red << 3) | (red >> 2)) << 16) | (((green << 3) | (green >> 2)) << 8) | ((blue << 3) | (blue >> 2));
	}
	bitoffset = x * pixmap->pixelsize;
	if ((bitoffset >> 3) >= rowlength)
		return 0;
	value = row[bitoffset >> 3];
	value = (value >> (8 - pixmap->pixelsize - (bitoffset & 7))) & ((1L << pixmap->pixelsize) - 1);
	return pixmap->palette[value];
}

static long PictPdfRunLengthEncode(const unsigned char* input, long length, unsigned char* output)
{
	long position;
	long written;
	long run;
	long literal;

	position = 0;
	written = 0;
	while (position < length)
	{
		run = 1;
		while (position + run < length && run < 128 && input[position + run] == input[position])
			run++;
		if (run >= 2)
		{
			output[written++] = (unsigned char)(257 - run);
			output[written++] = input[position];
			position += run;
			continue;
		}
		literal = 1;
		while (position + literal < length && literal < 128 &&
			!(position + literal + 1 < length && input[position + literal] == input[position + literal + 1]))
			literal++;
		output[written++] = (unsigned char)(literal - 1);
		memcpy(output + written, input + position, (size_t)literal);
		written += literal;
		position += literal;
	}
	output[written++] = 128;
	return written;
}

static void PictPdfDestinationMatrix(PICTPDFSTATE* state, const long* destination)
{
	PictPdfAppendNumber(state, (destination[3] - destination[1]) * state->map.scalex);
	PictPdfAppend(state, "0 0 ");
	PictPdfAppendNumber(state, (destination[2] - destination[0]) * state->map.scaley);
	PictPdfAppendNumber(state, PictPdfMapX(state, destination[1]));
	PictPdfAppendNumber(state, PictPdfMapY(state, destination[2]));
	PictPdfAppend(state, "cm\n");
}

static void PictPdfDrawStencil(PICTPDFSTATE* state, const unsigned char* pixels, long rowlength, const long* crop, const long* destination, int invert, unsigned long color)
{
	static const char hexdigits[] = "0123456789ABCDEF";
	unsigned char* bits;
	unsigned char* encoded;
	char* hex;
	char header[128];
	long outrowbytes;
	long encodedlength;
	long x;
	long y;
	long sourcex;
	long index;
	long cursor;

	outrowbytes = (crop[2] + 7) / 8;
	bits = (unsigned char*)calloc((size_t)crop[3], (size_t)outrowbytes);
	encoded = (unsigned char*)malloc((size_t)(outrowbytes * crop[3] + (outrowbytes * crop[3]) / 128 + 2));
	if (!bits || !encoded)
	{
		free(bits);
		free(encoded);
		state->failed = 1;
		return;
	}
	for (y = 0; y < crop[3]; y++)
	{
		for (x = 0; x < crop[2]; x++)
		{
			sourcex = crop[0] + x;
			if ((pixels[(crop[1] + y) * rowlength + (sourcex >> 3)] >> (7 - (sourcex & 7))) & 1)
				bits[y * outrowbytes + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
		}
	}
	encodedlength = PictPdfRunLengthEncode(bits, outrowbytes * crop[3], encoded);
	free(bits);
	hex = (char*)malloc((size_t)(encodedlength * 2 + encodedlength / 32 + 2));
	if (!hex)
	{
		free(encoded);
		state->failed = 1;
		return;
	}
	cursor = 0;
	for (index = 0; index < encodedlength; index++)
	{
		hex[cursor++] = hexdigits[encoded[index] >> 4];
		hex[cursor++] = hexdigits[encoded[index] & 15];
		if ((index & 31) == 31)
			hex[cursor++] = '\n';
	}
	hex[cursor] = 0;
	free(encoded);
	PictPdfAppend(state, "q\n");
	PictPdfColor(state, color, "rg\n");
	PictPdfDestinationMatrix(state, destination);
	if (invert)
		sprintf(header, "BI /W %ld /H %ld /IM true /BPC 1 /D [0 1] /F [/AHx /RL] ID\n", crop[2], crop[3]);
	else
		sprintf(header, "BI /W %ld /H %ld /IM true /BPC 1 /D [1 0] /F [/AHx /RL] ID\n", crop[2], crop[3]);
	PictPdfAppend(state, header);
	PictPdfAppend(state, hex);
	PictPdfAppend(state, ">\nEI Q\n");
	free(hex);
}

static void PictPdfDrawColorImage(PICTPDFSTATE* state, const PICTPDFPIXMAP* pixmap, const unsigned char* pixels, long rowlength, const long* crop, const long* destination)
{
	unsigned char* rgb;
	unsigned char* cursor;
	unsigned long color;
	long x;
	long y;

	rgb = (unsigned char*)malloc((size_t)(crop[2] * crop[3] * 3));
	if (!rgb)
	{
		state->failed = 1;
		return;
	}
	cursor = rgb;
	for (y = 0; y < crop[3]; y++)
	{
		for (x = 0; x < crop[2]; x++)
		{
			color = PictPdfPixelColor(pixmap, pixels + (crop[1] + y) * rowlength, rowlength, crop[0] + x);
			*cursor++ = (unsigned char)(color >> 16);
			*cursor++ = (unsigned char)(color >> 8);
			*cursor++ = (unsigned char)color;
		}
	}
	PictPdfFlush(state);
	pdf_add_rgb24(state->pdf, NULL, (float)PictPdfMapX(state, destination[1]), (float)PictPdfMapY(state, destination[2]),
		(float)((destination[3] - destination[1]) * state->map.scalex), (float)((destination[2] - destination[0]) * state->map.scaley),
		rgb, (uint32_t)crop[2], (uint32_t)crop[3]);
	free(rgb);
}

static void PictPdfDrawBits(PICTPDFSTATE* state, long opcode)
{
	PICTPDFPIXMAP pixmap;
	unsigned char* pixels;
	unsigned long stencilcolor;
	long source[4];
	long destination[4];
	long crop[4];
	long rowlength;
	long mode;
	long regionposition;
	int direct;
	int region;
	int basemode;
	int invert;

	direct = opcode == 0x9A || opcode == 0x9B;
	region = (opcode & 1) != 0;
	PictPdfReadPixMap(state, &pixmap, direct);
	PictPdfReadRect(state, source);
	PictPdfReadRect(state, destination);
	mode = PictPdfReadWord(state);
	regionposition = state->position;
	if (region)
		PictPdfSkipSizedRecord(state);
	if (state->failed)
		return;
	pixels = PictPdfReadPixels(state, &pixmap, opcode >= 0x98, &rowlength);
	if (!pixels)
		return;
	crop[0] = source[1] - pixmap.bounds[1];
	crop[1] = source[0] - pixmap.bounds[0];
	crop[2] = source[3] - source[1];
	crop[3] = source[2] - source[0];
	if (crop[0] < 0)
	{
		crop[2] += crop[0];
		crop[0] = 0;
	}
	if (crop[1] < 0)
	{
		crop[3] += crop[1];
		crop[1] = 0;
	}
	if (crop[0] + crop[2] > pixmap.bounds[3] - pixmap.bounds[1])
		crop[2] = pixmap.bounds[3] - pixmap.bounds[1] - crop[0];
	if (crop[1] + crop[3] > pixmap.bounds[2] - pixmap.bounds[0])
		crop[3] = pixmap.bounds[2] - pixmap.bounds[0] - crop[1];
	basemode = PictPdfBaseMode(mode);
	if (crop[2] > 0 && crop[3] > 0 && destination[2] > destination[0] && destination[3] > destination[1] &&
		basemode != PICTPDFMODESKIP && basemode != PICTPDFMODEXOR && basemode != PICTPDFMODENOTXOR)
	{
		if (region)
		{
			PictPdfAppend(state, "q\n");
			PictPdfRegionPath(state, regionposition, PICTPDFREGIONFILL);
			PictPdfAppend(state, "W n\n");
		}
		if (pixmap.pixelsize == 1)
		{
			invert = basemode >= PICTPDFMODENOTCOPY;
			if (invert)
				basemode -= PICTPDFMODENOTCOPY;
			stencilcolor = state->foreground;
			if (basemode == PICTPDFMODEBIC)
				stencilcolor = state->background;
			if (basemode == PICTPDFMODECOPY)
			{
				PictPdfColor(state, state->background, "rg\n");
				PictPdfRectPath(state, destination[0], destination[1], destination[2], destination[3]);
				PictPdfAppend(state, "f\n");
			}
			PictPdfDrawStencil(state, pixels, rowlength, crop, destination, invert, stencilcolor);
		}
		else
			PictPdfDrawColorImage(state, &pixmap, pixels, rowlength, crop, destination);
		if (region)
			PictPdfAppend(state, "Q\n");
	}
	free(pixels);
}

static void PictPdfReadPixPat(PICTPDFSTATE* state, PICTPDFPATTERN* pattern)
{
	PICTPDFPIXMAP pixmap;
	unsigned char* pixels;
	unsigned long color;
	unsigned long red;
	unsigned long green;
	unsigned long blue;
	long type;
	long rowlength;
	long x;
	long y;
	long count;

	type = PictPdfReadWord(state);
	PictPdfReadPattern(state, pattern);
	if (type == 2)
	{
		pattern->pixcolor = PictPdfReadRGB(state);
		pattern->ispixpat = 1;
		return;
	}
	if (type != 1)
		return;
	PictPdfReadPixMap(state, &pixmap, 0);
	pixels = PictPdfReadPixels(state, &pixmap, 1, &rowlength);
	if (!pixels)
		return;
	red = 0;
	green = 0;
	blue = 0;
	count = 0;
	for (y = 0; y < pixmap.bounds[2] - pixmap.bounds[0]; y++)
	{
		for (x = 0; x < pixmap.bounds[3] - pixmap.bounds[1]; x++)
		{
			color = PictPdfPixelColor(&pixmap, pixels + y * rowlength, rowlength, x);
			red += (color >> 16) & 0xFF;
			green += (color >> 8) & 0xFF;
			blue += color & 0xFF;
			count++;
		}
	}
	free(pixels);
	if (count > 0)
	{
		pattern->pixcolor = ((red / count) << 16) | ((green / count) << 8) | (blue / count);
		pattern->ispixpat = 1;
	}
}

static void PictPdfReadFontName(PICTPDFSTATE* state)
{
	long length;
	long start;
	long fontid;
	long namelength;
	long index;
	PICTPDFFONT* font;

	length = PictPdfReadWord(state);
	start = state->position;
	fontid = PictPdfReadWord(state);
	namelength = PictPdfReadByte(state);
	font = NULL;
	for (index = 0; index < state->fontcount; index++)
	{
		if (state->fonts[index].fontid == fontid)
			font = &state->fonts[index];
	}
	if (!font && state->fontcount < PICTPDFMAXFONTS)
	{
		font = &state->fonts[state->fontcount];
		state->fontcount++;
	}
	for (index = 0; index < namelength; index++)
	{
		if (font && index < 63)
			font->fontname[index] = (char)PictPdfReadByte(state);
	}
	if (font)
	{
		font->fontid = fontid;
		if (namelength > 63)
			namelength = 63;
		font->fontname[namelength] = 0;
	}
	state->position = start;
	PictPdfSkip(state, (unsigned long)length);
}

static void PictPdfSkipOpcode(PICTPDFSTATE* state, long opcode)
{
	if ((opcode >= 0x24 && opcode <= 0x27) || (opcode >= 0x2D && opcode <= 0x2F) || (opcode >= 0x92 && opcode <= 0x97) ||
		(opcode >= 0x9C && opcode <= 0x9F) || (opcode >= 0xA2 && opcode <= 0xAF))
		PictPdfSkipWordLength(state);
	else if (opcode >= 0xD0 && opcode <= 0xFE)
		PictPdfSkip(state, PictPdfReadLong(state));
	else if (opcode >= 0x100 && opcode <= 0x7FFF)
		PictPdfSkip(state, (unsigned long)((opcode >> 8) * 2));
	else if (opcode >= 0x8100)
		PictPdfSkip(state, PictPdfReadLong(state));
}

static void PictPdfLineTo(PICTPDFSTATE* state, long toh, long tov)
{
	PictPdfDrawLine(state, state->penh, state->penv, toh, tov);
	state->penh = toh;
	state->penv = tov;
}

static void PictPdfOpcode(PICTPDFSTATE* state, long opcode)
{
	long v;
	long h;

	if (opcode >= 0x30 && opcode <= 0x8F)
	{
		PictPdfShapeOpcode(state, opcode);
		return;
	}
	switch (opcode)
	{
	case 0x01:
		PictPdfSetClip(state, state->position);
		PictPdfSkipSizedRecord(state);
		break;
	case 0x02:
		PictPdfReadPattern(state, &state->backpattern);
		break;
	case 0x03:
		state->textfont = PictPdfReadWord(state);
		break;
	case 0x04:
		state->textface = PictPdfReadByte(state);
		break;
	case 0x05:
		state->textmode = PictPdfReadWord(state);
		break;
	case 0x06:
		PictPdfSkip(state, 4);
		break;
	case 0x07:
		state->pensizev = PictPdfReadSignedWord(state);
		state->pensizeh = PictPdfReadSignedWord(state);
		break;
	case 0x08:
		state->penmode = PictPdfReadWord(state);
		break;
	case 0x09:
		PictPdfReadPattern(state, &state->penpattern);
		break;
	case 0x0A:
		PictPdfReadPattern(state, &state->fillpattern);
		break;
	case 0x0B:
		state->ovalv = PictPdfReadSignedWord(state);
		state->ovalh = PictPdfReadSignedWord(state);
		break;
	case 0x0C:
		state->originh += PictPdfReadSignedWord(state);
		state->originv += PictPdfReadSignedWord(state);
		break;
	case 0x0D:
		state->textsize = PictPdfReadWord(state);
		break;
	case 0x0E:
		state->foreground = PictPdfOldColor(PictPdfReadLong(state));
		break;
	case 0x0F:
		state->background = PictPdfOldColor(PictPdfReadLong(state));
		break;
	case 0x10:
		PictPdfSkip(state, 8);
		break;
	case 0x11:
		PictPdfSkip(state, (unsigned long)state->version);
		break;
	case 0x12:
		PictPdfReadPixPat(state, &state->backpattern);
		break;
	case 0x13:
		PictPdfReadPixPat(state, &state->penpattern);
		break;
	case 0x14:
		PictPdfReadPixPat(state, &state->fillpattern);
		break;
	case 0x15:
	case 0x16:
		PictPdfSkip(state, 2);
		break;
	case 0x1A:
		state->foreground = PictPdfReadRGB(state);
		break;
	case 0x1B:
		state->background = PictPdfReadRGB(state);
		break;
	case 0x1D:
	case 0x1F:
		PictPdfSkip(state, 6);
		break;
	case 0x20:
		state->penv = PictPdfReadSignedWord(state);
		state->penh = PictPdfReadSignedWord(state);
		v = PictPdfReadSignedWord(state);
		h = PictPdfReadSignedWord(state);
		PictPdfLineTo(state, h, v);
		break;
	case 0x21:
		state->penv = PictPdfReadSignedWord(state);
		state->penh = PictPdfReadSignedWord(state);
		h = state->penh + PictPdfReadSignedByte(state);
		v = state->penv + PictPdfReadSignedByte(state);
		PictPdfLineTo(state, h, v);
		break;
	case 0x22:
		v = PictPdfReadSignedWord(state);
		h = PictPdfReadSignedWord(state);
		PictPdfLineTo(state, h, v);
		break;
	case 0x23:
		h = state->penh + PictPdfReadSignedByte(state);
		v = state->penv + PictPdfReadSignedByte(state);
		PictPdfLineTo(state, h, v);
		break;
	case 0x28:
		state->textv = PictPdfReadSignedWord(state);
		state->texth = PictPdfReadSignedWord(state);
		PictPdfDrawText(state, PictPdfReadByte(state));
		break;
	case 0x29:
		state->texth += PictPdfReadByte(state);
		PictPdfDrawText(state, PictPdfReadByte(state));
		break;
	case 0x2A:
		state->textv += PictPdfReadByte(state);
		PictPdfDrawText(state, PictPdfReadByte(state));
		break;
	case 0x2B:
		state->texth += PictPdfReadByte(state);
		state->textv += PictPdfReadByte(state);
		PictPdfDrawText(state, PictPdfReadByte(state));
		break;
	case 0x2C:
		PictPdfReadFontName(state);
		break;
	case 0x90:
	case 0x91:
	case 0x98:
	case 0x99:
	case 0x9A:
	case 0x9B:
		PictPdfDrawBits(state, opcode);
		break;
	case 0xA0:
		PictPdfSkip(state, 2);
		break;
	case 0xA1:
		PictPdfSkip(state, 2);
		PictPdfSkipWordLength(state);
		break;
	default:
		PictPdfSkipOpcode(state, opcode);
		break;
	}
}

int PictPdfRenderPage(struct pdf_doc* pdf, const unsigned char* picture, long picturesize, const PICTPDFMAP* map)
{
	PICTPDFSTATE* state;
	long opcode;
	int result;

	if (picturesize < 12)
		return -1;
	state = (PICTPDFSTATE*)calloc(1, sizeof(PICTPDFSTATE));
	if (!state)
		return -1;
	state->pdf = pdf;
	state->picture = picture;
	state->picturesize = picturesize;
	state->map = *map;
	state->pensizeh = 1;
	state->pensizev = 1;
	state->penmode = 8;
	state->textmode = 1;
	state->background = 0xFFFFFF;
	memset(state->penpattern.bits, 0xFF, 8);
	memset(state->fillpattern.bits, 0xFF, 8);
	if (picture[10] == 0x11 && picture[11] == 0x01)
	{
		state->version = 1;
		state->position = 12;
	}
	else if (picturesize >= 14 && picture[10] == 0x00 && picture[11] == 0x11 && picture[12] == 0x02 && picture[13] == 0xFF)
	{
		state->version = 2;
		state->position = 14;
	}
	else
	{
		free(state);
		return -1;
	}
	PictPdfAppend(state, "q\n");
	while (!state->failed && state->position < state->picturesize)
	{
		if (state->version == 2)
		{
			state->position = (state->position + 1) & ~1L;
			opcode = PictPdfReadWord(state);
		}
		else
			opcode = PictPdfReadByte(state);
		if (opcode == 0xFF || state->failed)
			break;
		PictPdfOpcode(state, opcode);
	}
	PictPdfAppend(state, "Q\n");
	result = 0;
	if (state->failed)
		result = -1;
	state->failed = 0;
	PictPdfFlush(state);
	free(state->content);
	free(state);
	return result;
}
