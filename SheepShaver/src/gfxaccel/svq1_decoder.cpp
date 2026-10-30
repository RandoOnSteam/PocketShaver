/*
 *  svq1_decoder.cpp - Sorenson Video 1 (SVQ1) decoder
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
#include <stdlib.h>
#include <string.h>
#include "svq1_decoder.h"
#include "svq1_tables.h"

#define SVQ1BLOCKSKIP 0
#define SVQ1BLOCKINTER 1
#define SVQ1BLOCKINTER4V 2
#define SVQ1BLOCKINTRA 3
#define SVQ1MAXSTAGES 6
#define SVQ1MAXDIMENSION 4096

typedef struct Svq1Bits {
	const uint8 *data;
	uint32 length;
	uint32 position;
	uint32 buffer;
	int count;
} Svq1Bits;

typedef struct Svq1Vector {
	int x;
	int y;
} Svq1Vector;

struct Svq1Decoder {
	int width;
	int height;
	int allocatedwidth;
	int allocatedheight;
	int framecode;
	int lasttempref;
	int nonreference;
	int planewidth[3];
	int planeheight[3];
	uint8 *planes[2][3];
	int reference;
	int output;
	int havereference;
	int rgbvalid;
	Svq1Vector *motion;
	uint8 *swapped;
	uint32 swappedsize;
	uint32 *rgb;
	int32 lumatable[256];
	int32 redtable[256];
	int32 greenutable[256];
	int32 greenvtable[256];
	int32 bluetable[256];
};

static const int svq1codebookbase[4] = {0, 768, 2304, 5376};
static const uint16 svq1framesizes[7][2] = {{160, 120}, {128, 96}, {176, 144}, {352, 288}, {704, 576}, {240, 180}, {320, 240}};

static void BitsInit(Svq1Bits *bits, const uint8 *data, uint32 length)
{
	bits->data = data;
	bits->length = length;
	bits->position = 0;
	bits->buffer = 0;
	bits->count = 0;
}

static uint32 BitsPeek(Svq1Bits *bits, int count)
{
	while (bits->count <= 24) {
		if (bits->position < bits->length)
			bits->buffer |= (uint32)bits->data[bits->position] << (24 - bits->count);
		bits->position++;
		bits->count += 8;
	}
	return bits->buffer >> (32 - count);
}

static void BitsSkip(Svq1Bits *bits, int count)
{
	bits->buffer <<= count;
	bits->count -= count;
}

static uint32 BitsRead(Svq1Bits *bits, int count)
{
	uint32 value;
	if (count == 0)
		return 0;
	value = BitsPeek(bits, count);
	BitsSkip(bits, count);
	return value;
}

static int BitsLeft(Svq1Bits *bits)
{
	return (int)((int64)bits->length * 8 - ((int64)bits->position * 8 - bits->count));
}

static int VlcDecode(Svq1Bits *bits, const uint32 *table, int rootbits)
{
	uint32 entry;
	uint32 base;
	int levelbits;
	base = 0;
	levelbits = rootbits;
	for (;;) {
		entry = table[base + BitsPeek(bits, levelbits)];
		if (!(entry & 0x80)) {
			BitsSkip(bits, (int)(entry & 0x7f));
			return (int)(entry >> 8);
		}
		BitsSkip(bits, levelbits);
		levelbits = (int)(entry & 0x7f);
		base = entry >> 8;
	}
}

static int Clamp(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

static int Median(int a, int b, int c)
{
	int low;
	int high;
	low = a;
	high = b;
	if (a > b) {
		low = b;
		high = a;
	}
	if (c > high)
		return high;
	if (c < low)
		return low;
	return c;
}

static int Clip(int value, int low, int high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

static int Split(Svq1Bits *bits, uint8 **list, int *index, int *mark, int *count, int *level, int pitch)
{
	int step;
	for (; *level > 0; (*index)++) {
		if (*index == *mark) {
			*mark = *count;
			if (--(*level) == 0)
				break;
		}
		if (!BitsRead(bits, 1))
			break;
		if (*count > 61)
			return 0;
		step = 1;
		if (*level & 1)
			step = pitch;
		list[(*count)++] = list[*index];
		list[(*count)++] = list[*index] + (step << ((*level >> 1) + 1));
	}
	return 1;
}

static void Vectors(const int8 *codebooks, Svq1Bits *bits, int level, int stages, const int8 **vectors)
{
	uint32 cache;
	int size;
	int stage;
	size = 8 << level;
	cache = BitsRead(bits, 4 * stages);
	for (stage = 0; stage < stages; stage++)
		vectors[stage] = codebooks + svq1codebookbase[level] + ((((cache >> (4 * (stages - stage - 1))) & 15) + 16 * stage) * size);
}

static int DecodeIntraBlock(Svq1Bits *bits, uint8 *pixels, int pitch)
{
	uint8 *list[64];
	const int8 *vectors[SVQ1MAXSTAGES];
	int index;
	int mark;
	int count;
	int level;
	int width;
	int height;
	int stages;
	int mean;
	int x;
	int y;
	int stage;
	int value;
	uint8 *row;
	list[0] = pixels;
	for (index = 0, mark = 1, count = 1, level = 5; index < count; index++) {
		if (!Split(bits, list, &index, &mark, &count, &level, pitch))
			return -1;
		width = 1 << ((4 + level) / 2);
		height = 1 << ((3 + level) / 2);
		stages = VlcDecode(bits, svq1intrastagetable + svq1intrastageoffsets[level], 7) - 1;
		if (stages == -1) {
			for (y = 0; y < height; y++)
				memset(list[index] + y * pitch, 0, (size_t)width);
			continue;
		}
		if (stages > SVQ1MAXSTAGES || (stages > 0 && level >= 4))
			return -1;
		mean = VlcDecode(bits, svq1intrameantable, 8);
		if (mean > 255)
			return -1;
		if (stages == 0) {
			for (y = 0; y < height; y++)
				memset(list[index] + y * pitch, mean, (size_t)width);
			continue;
		}
		Vectors(svq1intracodebooks, bits, level, stages, vectors);
		for (y = 0; y < height; y++) {
			row = list[index] + y * pitch;
			for (x = 0; x < width; x++) {
				value = mean;
				for (stage = 0; stage < stages; stage++)
					value += vectors[stage][y * width + x];
				row[x] = (uint8)Clamp(value);
			}
		}
	}
	return 0;
}

static int DecodeInterBlock(Svq1Bits *bits, uint8 *pixels, int pitch, int buggy)
{
	uint8 *list[64];
	const int8 *vectors[SVQ1MAXSTAGES];
	int index;
	int mark;
	int count;
	int level;
	int width;
	int height;
	int stages;
	int mean;
	int x;
	int y;
	int stage;
	int value;
	uint8 *row;
	list[0] = pixels;
	for (index = 0, mark = 1, count = 1, level = 5; index < count; index++) {
		if (!Split(bits, list, &index, &mark, &count, &level, pitch))
			return -1;
		width = 1 << ((4 + level) / 2);
		height = 1 << ((3 + level) / 2);
		stages = VlcDecode(bits, svq1interstagetable + svq1interstageoffsets[level], 6) - 1;
		if (stages == -1)
			continue;
		if (stages > SVQ1MAXSTAGES || (stages > 0 && level >= 4))
			return -1;
		mean = VlcDecode(bits, svq1intermeantable, 9);
		if (mean > 511)
			return -1;
		mean -= 256;
		if (buggy && mean == -128)
			mean = 128;
		else if (buggy && mean == 128)
			mean = -128;
		if (stages > 0)
			Vectors(svq1intercodebooks, bits, level, stages, vectors);
		for (y = 0; y < height; y++) {
			row = list[index] + y * pitch;
			for (x = 0; x < width; x++) {
				value = row[x] + mean;
				for (stage = 0; stage < stages; stage++)
					value += vectors[stage][y * width + x];
				row[x] = (uint8)Clamp(value);
			}
		}
	}
	return 0;
}

static int DecodeMotionVector(Svq1Bits *bits, Svq1Vector *vector, Svq1Vector **predictors)
{
	int component;
	int difference;
	int predicted;
	for (component = 0; component < 2; component++) {
		difference = VlcDecode(bits, svq1motiontable, 7);
		if (difference > 32)
			return -1;
		if (difference && BitsRead(bits, 1))
			difference = -difference;
		if (component == 1) {
			predicted = difference + Median(predictors[0]->y, predictors[1]->y, predictors[2]->y);
			vector->y = (int)((uint32)predicted << 26) >> 26;
		} else {
			predicted = difference + Median(predictors[0]->x, predictors[1]->x, predictors[2]->x);
			vector->x = (int)((uint32)predicted << 26) >> 26;
		}
	}
	return 0;
}

static void PutPixels(uint8 *target, const uint8 *source, int pitch, int size, int vectorx, int vectory)
{
	int x;
	int y;
	const uint8 *below;
	for (y = 0; y < size; y++) {
		below = source + pitch;
		if (!(vectorx & 1) && !(vectory & 1))
			memcpy(target, source, (size_t)size);
		else if (!(vectory & 1))
			for (x = 0; x < size; x++)
				target[x] = (uint8)((source[x] + source[x + 1] + 1) >> 1);
		else if (!(vectorx & 1))
			for (x = 0; x < size; x++)
				target[x] = (uint8)((source[x] + below[x] + 1) >> 1);
		else
			for (x = 0; x < size; x++)
				target[x] = (uint8)((source[x] + source[x + 1] + below[x] + below[x + 1] + 2) >> 2);
		target += pitch;
		source += pitch;
	}
}

static int MotionBlock(Svq1Bits *bits, uint8 *current, const uint8 *previous, int pitch, Svq1Vector *motion, int x, int y, int width, int height)
{
	Svq1Vector vector;
	Svq1Vector *predictors[3];
	predictors[0] = &motion[0];
	predictors[1] = &motion[0];
	predictors[2] = &motion[0];
	if (y != 0) {
		predictors[1] = &motion[x / 8 + 2];
		predictors[2] = &motion[x / 8 + 4];
	}
	if (DecodeMotionVector(bits, &vector, predictors))
		return -1;
	motion[0] = vector;
	motion[x / 8 + 2] = vector;
	motion[x / 8 + 3] = vector;
	vector.x = Clip(vector.x, -2 * x, 2 * (width - x - 16));
	vector.y = Clip(vector.y, -2 * y, 2 * (height - y - 16));
	PutPixels(current, previous + (x + (vector.x >> 1)) + (y + (vector.y >> 1)) * pitch, pitch, 16, vector.x, vector.y);
	return 0;
}

static int MotionBlock4(Svq1Bits *bits, uint8 *current, const uint8 *previous, int pitch, Svq1Vector *motion, int x, int y, int width, int height)
{
	Svq1Vector vector;
	Svq1Vector *predictors[4];
	int block;
	int vectorx;
	int vectory;
	predictors[0] = &motion[0];
	predictors[1] = &motion[0];
	predictors[2] = &motion[0];
	if (y != 0) {
		predictors[1] = &motion[x / 8 + 2];
		predictors[2] = &motion[x / 8 + 4];
	}
	if (DecodeMotionVector(bits, &vector, predictors))
		return -1;
	predictors[0] = &vector;
	if (y == 0) {
		predictors[1] = &vector;
		predictors[2] = &vector;
	} else
		predictors[1] = &motion[x / 8 + 3];
	if (DecodeMotionVector(bits, &motion[0], predictors))
		return -1;
	predictors[1] = &motion[0];
	predictors[2] = &motion[x / 8 + 1];
	if (DecodeMotionVector(bits, &motion[x / 8 + 2], predictors))
		return -1;
	predictors[2] = &motion[x / 8 + 2];
	predictors[3] = &motion[x / 8 + 3];
	if (DecodeMotionVector(bits, predictors[3], predictors))
		return -1;
	for (block = 0; block < 4; block++) {
		vectorx = Clip(predictors[block]->x + (block & 1) * 16, -2 * x, 2 * (width - x - 8));
		vectory = Clip(predictors[block]->y + (block >> 1) * 16, -2 * y, 2 * (height - y - 8));
		PutPixels(current, previous + (x + (vectorx >> 1)) + (y + (vectory >> 1)) * pitch, pitch, 8, vectorx, vectory);
		if (block & 1)
			current += 8 * (pitch - 1);
		else
			current += 8;
	}
	return 0;
}

static int DecodeDeltaBlock(Svq1Bits *bits, uint8 *current, const uint8 *previous, int pitch, Svq1Vector *motion, int x, int y, int width, int height, int buggy)
{
	const uint8 *source;
	int type;
	int row;
	type = VlcDecode(bits, svq1blocktypetable, 3);
	if (type == SVQ1BLOCKSKIP || type == SVQ1BLOCKINTRA) {
		motion[0].x = 0;
		motion[0].y = 0;
		motion[x / 8 + 2] = motion[0];
		motion[x / 8 + 3] = motion[0];
	}
	if (type == SVQ1BLOCKSKIP) {
		source = previous + x + y * pitch;
		for (row = 0; row < 16; row++)
			memcpy(current + row * pitch, source + row * pitch, 16);
		return 0;
	}
	if (type == SVQ1BLOCKINTER) {
		if (MotionBlock(bits, current, previous, pitch, motion, x, y, width, height))
			return -1;
		return DecodeInterBlock(bits, current, pitch, buggy);
	}
	if (type == SVQ1BLOCKINTER4V) {
		if (MotionBlock4(bits, current, previous, pitch, motion, x, y, width, height))
			return -1;
		return DecodeInterBlock(bits, current, pitch, buggy);
	}
	if (type == SVQ1BLOCKINTRA)
		return DecodeIntraBlock(bits, current, pitch);
	return -1;
}

static void FreePlanes(Svq1Decoder *decoder)
{
	int frame;
	int plane;
	for (frame = 0; frame < 2; frame++)
		for (plane = 0; plane < 3; plane++) {
			free(decoder->planes[frame][plane]);
			decoder->planes[frame][plane] = NULL;
		}
	free(decoder->motion);
	free(decoder->rgb);
	decoder->motion = NULL;
	decoder->rgb = NULL;
	decoder->allocatedwidth = 0;
	decoder->allocatedheight = 0;
	decoder->havereference = 0;
}

static int AllocatePlanes(Svq1Decoder *decoder, int width, int height)
{
	int frame;
	int plane;
	if (decoder->allocatedwidth == width && decoder->allocatedheight == height)
		return 1;
	FreePlanes(decoder);
	decoder->planewidth[0] = (width + 15) & ~15;
	decoder->planeheight[0] = (height + 15) & ~15;
	decoder->planewidth[1] = ((width / 4) + 15) & ~15;
	decoder->planeheight[1] = ((height / 4) + 15) & ~15;
	decoder->planewidth[2] = decoder->planewidth[1];
	decoder->planeheight[2] = decoder->planeheight[1];
	for (frame = 0; frame < 2; frame++)
		for (plane = 0; plane < 3; plane++) {
			decoder->planes[frame][plane] = (uint8 *)calloc((size_t)decoder->planewidth[plane], (size_t)decoder->planeheight[plane]);
			if (decoder->planes[frame][plane] == NULL)
				return 0;
		}
	decoder->motion = (Svq1Vector *)calloc((size_t)(decoder->planewidth[0] / 8 + 3), sizeof(Svq1Vector));
	decoder->rgb = (uint32 *)malloc((size_t)width * (size_t)height * sizeof(uint32));
	if (decoder->motion == NULL || decoder->rgb == NULL)
		return 0;
	decoder->allocatedwidth = width;
	decoder->allocatedheight = height;
	return 1;
}

static void SkipString(Svq1Bits *bits)
{
	uint32 length;
	uint32 index;
	length = BitsRead(bits, 8);
	for (index = 0; index < length; index++)
		BitsRead(bits, 8);
}

static int DecodeHeader(Svq1Decoder *decoder, Svq1Bits *bits, int *intra, int *buggy)
{
	int tempref;
	int type;
	int sizecode;
	tempref = (int)BitsRead(bits, 8);
	*buggy = tempref == 0 && decoder->lasttempref == 0;
	decoder->lasttempref = tempref;
	type = (int)BitsRead(bits, 2);
	if (type == 3)
		return -1;
	*intra = type == 0;
	decoder->nonreference = type == 2;
	if (*intra) {
		if (decoder->framecode == 0x50 || decoder->framecode == 0x60)
			BitsRead(bits, 16);
		if ((decoder->framecode ^ 0x10) >= 0x50)
			SkipString(bits);
		BitsRead(bits, 5);
		sizecode = (int)BitsRead(bits, 3);
		if (sizecode == 7) {
			decoder->width = (int)BitsRead(bits, 12);
			decoder->height = (int)BitsRead(bits, 12);
		} else {
			decoder->width = svq1framesizes[sizecode][0];
			decoder->height = svq1framesizes[sizecode][1];
		}
		if (decoder->width == 0 || decoder->height == 0 || decoder->width > SVQ1MAXDIMENSION || decoder->height > SVQ1MAXDIMENSION)
			return -1;
	}
	if (BitsRead(bits, 1)) {
		BitsRead(bits, 2);
		if (BitsRead(bits, 2) != 0)
			return -1;
	}
	if (BitsRead(bits, 1)) {
		BitsRead(bits, 8);
		while (BitsLeft(bits) > 0 && BitsRead(bits, 1))
			BitsRead(bits, 8);
	}
	if (BitsLeft(bits) <= 0)
		return -1;
	return 0;
}

static int DecodePlane(Svq1Decoder *decoder, Svq1Bits *bits, int plane, int target, int intra, int buggy)
{
	uint8 *current;
	const uint8 *previous;
	int width;
	int height;
	int pitch;
	int x;
	int y;
	width = decoder->planewidth[plane];
	height = decoder->planeheight[plane];
	pitch = width;
	current = decoder->planes[target][plane];
	previous = decoder->planes[decoder->reference][plane];
	if (!intra)
		memset(decoder->motion, 0, (size_t)(width / 8 + 3) * sizeof(Svq1Vector));
	for (y = 0; y < height; y += 16) {
		for (x = 0; x < width; x += 16) {
			if (intra && DecodeIntraBlock(bits, current + x, pitch))
				return -1;
			if (!intra && DecodeDeltaBlock(bits, current + x, previous, pitch, decoder->motion, x, y, width, height, buggy))
				return -1;
		}
		if (!intra) {
			decoder->motion[0].x = 0;
			decoder->motion[0].y = 0;
		}
		current += 16 * pitch;
	}
	return 0;
}

static const uint8 *Unscramble(Svq1Decoder *decoder, const uint8 *data, uint32 length)
{
	uint8 *swapped;
	int word;
	int byte;
	if (length < 36)
		return NULL;
	if (decoder->swappedsize < length) {
		free(decoder->swapped);
		decoder->swapped = (uint8 *)malloc(length);
		decoder->swappedsize = 0;
		if (decoder->swapped == NULL)
			return NULL;
		decoder->swappedsize = length;
	}
	swapped = decoder->swapped;
	memcpy(swapped, data, length);
	for (word = 0; word < 4; word++)
		for (byte = 0; byte < 4; byte++)
			swapped[4 + word * 4 + byte] = (uint8)(data[4 + word * 4 + ((byte + 2) & 3)] ^ data[4 + (7 - word) * 4 + byte]);
	return swapped;
}

int Svq1Decode(Svq1Decoder *decoder, const uint8 *data, uint32 length)
{
	Svq1Bits bits;
	int intra;
	int buggy;
	int target;
	int plane;
	BitsInit(&bits, data, length);
	decoder->framecode = (int)BitsRead(&bits, 22);
	if ((decoder->framecode & ~0x70) || !(decoder->framecode & 0x60))
		return -1;
	if (decoder->framecode != 0x20) {
		data = Unscramble(decoder, data, length);
		if (data == NULL)
			return -1;
		BitsInit(&bits, data, length);
		BitsRead(&bits, 22);
	}
	if (DecodeHeader(decoder, &bits, &intra, &buggy))
		return -1;
	if (!AllocatePlanes(decoder, decoder->width, decoder->height))
		return -1;
	if (!intra && !decoder->havereference)
		return -1;
	if (BitsLeft(&bits) < decoder->planewidth[0] * decoder->planeheight[0] / 256)
		return -1;
	target = 1 - decoder->reference;
	for (plane = 0; plane < 3; plane++)
		if (DecodePlane(decoder, &bits, plane, target, intra, buggy))
			return -1;
	decoder->output = target;
	decoder->rgbvalid = 0;
	if (!decoder->nonreference) {
		decoder->reference = target;
		decoder->havereference = 1;
	}
	return 0;
}

Svq1Decoder *Svq1Create(int width, int height)
{
	Svq1Decoder *decoder;
	int index;
	decoder = (Svq1Decoder *)calloc(1, sizeof(Svq1Decoder));
	if (decoder == NULL)
		return NULL;
	decoder->width = (width + 3) & ~3;
	decoder->height = (height + 3) & ~3;
	decoder->lasttempref = 0xff;
	for (index = 0; index < 256; index++) {
		decoder->lumatable[index] = 298 * (index - 16) + 128;
		decoder->redtable[index] = 409 * (index - 128);
		decoder->greenutable[index] = -100 * (index - 128);
		decoder->greenvtable[index] = -208 * (index - 128);
		decoder->bluetable[index] = 516 * (index - 128);
	}
	return decoder;
}

void Svq1Destroy(Svq1Decoder *decoder)
{
	FreePlanes(decoder);
	free(decoder->swapped);
	free(decoder);
}

int Svq1Width(Svq1Decoder *decoder)
{
	return decoder->width;
}

int Svq1Height(Svq1Decoder *decoder)
{
	return decoder->height;
}

const uint8 *Svq1Plane(Svq1Decoder *decoder, int plane, int *pitch)
{
	*pitch = decoder->planewidth[plane];
	return decoder->planes[decoder->output][plane];
}

const uint32 *Svq1Frame(Svq1Decoder *decoder)
{
	const uint8 *luma;
	const uint8 *blue;
	const uint8 *red;
	uint32 *target;
	int32 lumavalue;
	int32 redoffset;
	int32 greenoffset;
	int32 blueoffset;
	int x;
	int y;
	int chroma;
	if (decoder->rgb == NULL)
		return NULL;
	if (decoder->rgbvalid)
		return decoder->rgb;
	target = decoder->rgb;
	for (y = 0; y < decoder->height; y++) {
		luma = decoder->planes[decoder->output][0] + y * decoder->planewidth[0];
		blue = decoder->planes[decoder->output][1] + (y >> 2) * decoder->planewidth[1];
		red = decoder->planes[decoder->output][2] + (y >> 2) * decoder->planewidth[2];
		for (x = 0; x < decoder->width; x++) {
			chroma = x >> 2;
			lumavalue = decoder->lumatable[luma[x]];
			redoffset = decoder->redtable[red[chroma]];
			greenoffset = decoder->greenutable[blue[chroma]] + decoder->greenvtable[red[chroma]];
			blueoffset = decoder->bluetable[blue[chroma]];
			*target++ = ((uint32)Clamp((lumavalue + redoffset) >> 8) << 16) | ((uint32)Clamp((lumavalue + greenoffset) >> 8) << 8) |
				(uint32)Clamp((lumavalue + blueoffset) >> 8);
		}
	}
	decoder->rgbvalid = 1;
	return decoder->rgb;
}
