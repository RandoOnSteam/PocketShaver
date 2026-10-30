/*
 *  imageio.cpp - shared file and inflate helpers for disk image formats
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imageio.h"

#define IMAGEMAXCODES 288

typedef struct ImageInflateState {
	const uint8 *source;
	size_t sourcelength;
	size_t sourceposition;
	uint8 *target;
	size_t targetlength;
	size_t targetposition;
	uint32 bitbuffer;
	int bitcount;
	int failed;
} ImageInflateState;

typedef struct ImageHuffman {
	short count[16];
	short symbol[IMAGEMAXCODES];
} ImageHuffman;

uint64 ImageLoadBig64(const uint8 *data)
{
	return ((uint64)data[0] << 56) | ((uint64)data[1] << 48) | ((uint64)data[2] << 40) | ((uint64)data[3] << 32) |
		((uint64)data[4] << 24) | ((uint64)data[5] << 16) | ((uint64)data[6] << 8) | data[7];
}

uint32 ImageLoadBig32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

void ImageStoreBig64(uint8 *data, uint64 value)
{
	int index;
	for (index = 0; index < 8; index++)
		data[index] = (uint8)(value >> (56 - index * 8));
}

void ImageStoreBig32(uint8 *data, uint32 value)
{
	int index;
	for (index = 0; index < 4; index++)
		data[index] = (uint8)(value >> (24 - index * 8));
}

size_t ImageReadSome(FILE *file, uint64 offset, void *buffer, size_t length)
{
	if (IMAGESEEK(file, (int64)offset, SEEK_SET) != 0)
		return 0;
	return fread(buffer, 1, length, file);
}

int ImageReadAt(FILE *file, uint64 offset, void *buffer, size_t length)
{
	return ImageReadSome(file, offset, buffer, length) == length;
}

int ImageWriteAt(FILE *file, uint64 offset, const void *buffer, size_t length)
{
	if (IMAGESEEK(file, (int64)offset, SEEK_SET) != 0)
		return 0;
	return fwrite(buffer, 1, length, file) == length;
}

uint64 ImageFileSize(FILE *file)
{
	IMAGESEEK(file, 0, SEEK_END);
	return (uint64)IMAGETELL(file);
}

static int InflateBits(ImageInflateState *state, int need)
{
	uint32 value;
	value = state->bitbuffer;
	while (state->bitcount < need) {
		if (state->sourceposition >= state->sourcelength) {
			state->failed = 1;
			return 0;
		}
		value |= (uint32)state->source[state->sourceposition++] << state->bitcount;
		state->bitcount += 8;
	}
	state->bitbuffer = value >> need;
	state->bitcount -= need;
	return (int)(value & ((1u << need) - 1));
}

static int InflateStored(ImageInflateState *state)
{
	const uint8 *source;
	size_t length;
	state->bitbuffer = 0;
	state->bitcount = 0;
	if (state->sourceposition + 4 > state->sourcelength)
		return 0;
	source = state->source + state->sourceposition;
	length = (size_t)source[0] | ((size_t)source[1] << 8);
	if (source[2] != (uint8)~source[0] || source[3] != (uint8)~source[1])
		return 0;
	state->sourceposition += 4;
	if (state->sourceposition + length > state->sourcelength || state->targetposition + length > state->targetlength)
		return 0;
	memcpy(state->target + state->targetposition, state->source + state->sourceposition, length);
	state->sourceposition += length;
	state->targetposition += length;
	return 1;
}

static int InflateBuild(ImageHuffman *huffman, const short *lengths, int count)
{
	short offsets[16];
	int symbol;
	int length;
	int left;
	for (length = 0; length < 16; length++)
		huffman->count[length] = 0;
	for (symbol = 0; symbol < count; symbol++)
		huffman->count[lengths[symbol]]++;
	if (huffman->count[0] == count)
		return 1;
	left = 1;
	for (length = 1; length < 16; length++) {
		left <<= 1;
		left -= huffman->count[length];
		if (left < 0)
			return 0;
	}
	offsets[1] = 0;
	for (length = 1; length < 15; length++)
		offsets[length + 1] = (short)(offsets[length] + huffman->count[length]);
	for (symbol = 0; symbol < count; symbol++)
		if (lengths[symbol] != 0)
			huffman->symbol[offsets[lengths[symbol]]++] = (short)symbol;
	return 1;
}

static int InflateDecode(ImageInflateState *state, const ImageHuffman *huffman)
{
	int code;
	int first;
	int index;
	int length;
	int count;
	code = 0;
	first = 0;
	index = 0;
	for (length = 1; length < 16; length++) {
		code |= InflateBits(state, 1);
		if (state->failed)
			return -1;
		count = huffman->count[length];
		if (code - count < first)
			return huffman->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

static int InflateCodes(ImageInflateState *state, const ImageHuffman *lengthcodes, const ImageHuffman *distancecodes)
{
	static const short lengthbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const short lengthextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const short distancebase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const short distanceextra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	int symbol;
	size_t length;
	size_t distance;
	for (;;) {
		symbol = InflateDecode(state, lengthcodes);
		if (symbol < 0)
			return 0;
		if (symbol == 256)
			return 1;
		if (symbol < 256) {
			if (state->targetposition >= state->targetlength)
				return 0;
			state->target[state->targetposition++] = (uint8)symbol;
			continue;
		}
		symbol -= 257;
		if (symbol >= 29)
			return 0;
		length = (size_t)(lengthbase[symbol] + InflateBits(state, lengthextra[symbol]));
		symbol = InflateDecode(state, distancecodes);
		if (symbol < 0 || symbol >= 30)
			return 0;
		distance = (size_t)(distancebase[symbol] + InflateBits(state, distanceextra[symbol]));
		if (state->failed || distance > state->targetposition || state->targetposition + length > state->targetlength)
			return 0;
		while (length--) {
			state->target[state->targetposition] = state->target[state->targetposition - distance];
			state->targetposition++;
		}
	}
}

static int InflateFixed(ImageInflateState *state)
{
	ImageHuffman lengthcodes;
	ImageHuffman distancecodes;
	short lengths[IMAGEMAXCODES];
	int symbol;
	for (symbol = 0; symbol < 144; symbol++)
		lengths[symbol] = 8;
	for (; symbol < 256; symbol++)
		lengths[symbol] = 9;
	for (; symbol < 280; symbol++)
		lengths[symbol] = 7;
	for (; symbol < IMAGEMAXCODES; symbol++)
		lengths[symbol] = 8;
	InflateBuild(&lengthcodes, lengths, IMAGEMAXCODES);
	for (symbol = 0; symbol < 30; symbol++)
		lengths[symbol] = 5;
	InflateBuild(&distancecodes, lengths, 30);
	return InflateCodes(state, &lengthcodes, &distancecodes);
}

static int InflateDynamic(ImageInflateState *state)
{
	static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	ImageHuffman lengthcodes;
	ImageHuffman distancecodes;
	short lengths[320];
	int literalcount;
	int distancecount;
	int codecount;
	int index;
	int symbol;
	int length;
	int repeat;
	literalcount = InflateBits(state, 5) + 257;
	distancecount = InflateBits(state, 5) + 1;
	codecount = InflateBits(state, 4) + 4;
	if (literalcount > 286 || distancecount > 30)
		return 0;
	for (index = 0; index < codecount; index++)
		lengths[order[index]] = (short)InflateBits(state, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (state->failed || !InflateBuild(&lengthcodes, lengths, 19))
		return 0;
	index = 0;
	while (index < literalcount + distancecount) {
		symbol = InflateDecode(state, &lengthcodes);
		if (symbol < 0)
			return 0;
		if (symbol < 16) {
			lengths[index++] = (short)symbol;
			continue;
		}
		length = 0;
		if (symbol == 16) {
			if (index == 0)
				return 0;
			length = lengths[index - 1];
			repeat = 3 + InflateBits(state, 2);
		} else if (symbol == 17)
			repeat = 3 + InflateBits(state, 3);
		else
			repeat = 11 + InflateBits(state, 7);
		if (state->failed || index + repeat > literalcount + distancecount)
			return 0;
		while (repeat--)
			lengths[index++] = (short)length;
	}
	if (lengths[256] == 0)
		return 0;
	if (!InflateBuild(&lengthcodes, lengths, literalcount) || !InflateBuild(&distancecodes, lengths + literalcount, distancecount))
		return 0;
	return InflateCodes(state, &lengthcodes, &distancecodes);
}

int ImageInflate(const uint8 *source, size_t sourcelength, uint8 *target, size_t targetlength)
{
	ImageInflateState state;
	int last;
	int type;
	int completed;
	state.source = source;
	state.sourcelength = sourcelength;
	state.sourceposition = 0;
	state.target = target;
	state.targetlength = targetlength;
	state.targetposition = 0;
	state.bitbuffer = 0;
	state.bitcount = 0;
	state.failed = 0;
	do {
		last = InflateBits(&state, 1);
		type = InflateBits(&state, 2);
		completed = 0;
		if (state.failed)
			return state.targetposition == targetlength;
		if (type == 0)
			completed = InflateStored(&state);
		else if (type == 1)
			completed = InflateFixed(&state);
		else if (type == 2)
			completed = InflateDynamic(&state);
		if (!completed)
			return state.targetposition == targetlength;
	} while (!last);
	memset(target + state.targetposition, 0, targetlength - state.targetposition);
	return 1;
}
