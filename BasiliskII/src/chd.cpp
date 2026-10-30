/*
 *  chd.cpp - MAME CHD hard disk image support
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
#include "chd.h"
#include "imageio.h"

#define CHDHEADER 124
#define CHDVERSION 5
#define CHDMAXHUNK (1024 * 1024)
#define CHDMAXSELF 16
#define CHDMAXPATH 1024
#define CHDMAXOFFSET (1ULL << 40)
#define CHDDIFFSUFFIX ".diff.chd"
#define CHDCODECZLIB 0x7a6c6962u
#define CHDCODECLZMA 0x6c7a6d61u
#define CHDCODECHUFF 0x68756666u
#define CHDCODECFLAC 0x666c6163u
#define CHDTYPENONE 4
#define CHDTYPESELF 5
#define CHDTYPEPARENT 6
#define CHDTYPERLESMALL 7
#define CHDTYPERLELARGE 8
#define CHDTYPESELF0 9
#define CHDTYPESELF1 10
#define CHDTYPEPARENTSELF 11
#define CHDTYPEPARENT0 12
#define CHDTYPEPARENT1 13
#define CHDTYPEUNALLOCATED 15
#define CHDNOHUNK 0xffffffffu
#define CHDLZMALITERALS (0x300 << 3)

typedef struct ChdBits {
	const uint8 *data;
	uint32 length;
	uint32 position;
	uint32 buffer;
	int count;
} ChdBits;

typedef struct ChdHuffman {
	int numcodes;
	int maxbits;
	uint8 lengths[256];
	uint16 *lookup;
} ChdHuffman;

typedef struct ChdLzmaLength {
	uint16 choice;
	uint16 choice2;
	uint16 low[16][8];
	uint16 mid[16][8];
	uint16 high[256];
} ChdLzmaLength;

typedef struct ChdLzmaProbs {
	uint16 literal[CHDLZMALITERALS];
	uint16 ismatch[12 << 4];
	uint16 isrep[12];
	uint16 isrepg0[12];
	uint16 isrepg1[12];
	uint16 isrepg2[12];
	uint16 isrep0long[12 << 4];
	uint16 posslot[4][64];
	uint16 posdecoders[115];
	uint16 align[16];
	ChdLzmaLength length;
	ChdLzmaLength replength;
} ChdLzmaProbs;

typedef struct ChdLzma {
	ChdLzmaProbs probs;
	const uint8 *source;
	uint32 sourcelength;
	uint32 sourceposition;
	uint32 range;
	uint32 code;
} ChdLzma;

struct ChdImage {
	FILE *file;
	ChdImage *parent;
	int readonly;
	uint32 hunkbytes;
	uint32 unitbytes;
	uint64 logical;
	uint32 hunkcount;
	uint32 compressors[4];
	uint64 mapoffset;
	uint64 fileend;
	uint8 sha1[20];
	uint8 parentsha1[20];
	uint64 *map;
	uint8 *hunk;
	uint32 cachedhunk;
	uint8 *packed;
	ChdLzma *lzma;
	uint16 *hufflookup;
	int32 *flac;
};

static uint64 Pack(uint32 type, uint32 length, uint64 offset)
{
	return (offset << 24) | ((uint64)length << 4) | type;
}

static uint16 Crc16(uint16 crc, const uint8 *data, uint32 length)
{
	uint32 index;
	int bit;
	for (index = 0; index < length; index++) {
		crc = (uint16)(crc ^ (data[index] << 8));
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x8000)
				crc = (uint16)((crc << 1) ^ 0x1021);
			else
				crc = (uint16)(crc << 1);
		}
	}
	return crc;
}

static void BitsInit(ChdBits *bits, const uint8 *data, uint32 length)
{
	bits->data = data;
	bits->length = length;
	bits->position = 0;
	bits->buffer = 0;
	bits->count = 0;
}

static uint32 BitsPeek(ChdBits *bits, int count)
{
	if (count == 0)
		return 0;
	while (bits->count <= 24) {
		if (bits->position < bits->length)
			bits->buffer |= (uint32)bits->data[bits->position] << (24 - bits->count);
		bits->position++;
		bits->count += 8;
	}
	return bits->buffer >> (32 - count);
}

static void BitsSkip(ChdBits *bits, int count)
{
	bits->buffer <<= count;
	bits->count -= count;
}

static uint32 BitsRead(ChdBits *bits, int count)
{
	uint32 value;
	if (count > 24) {
		value = BitsRead(bits, count - 16) << 16;
		return value | BitsRead(bits, 16);
	}
	value = BitsPeek(bits, count);
	BitsSkip(bits, count);
	return value;
}

static int32 BitsSigned(ChdBits *bits, int count)
{
	uint32 value;
	if (count == 0)
		return 0;
	value = BitsRead(bits, count);
	if (value & (1u << (count - 1)))
		return (int32)((int64)value - ((int64)1 << count));
	return (int32)value;
}

static int BitsOverflow(ChdBits *bits)
{
	return (uint64)bits->position * 8 - (uint64)bits->count > (uint64)bits->length * 8;
}

static int HuffmanBuild(ChdHuffman *huffman)
{
	uint32 histogram[33];
	uint32 start;
	uint32 next;
	uint32 code;
	uint32 entry;
	uint32 last;
	int length;
	int symbol;
	memset(histogram, 0, sizeof(histogram));
	for (symbol = 0; symbol < huffman->numcodes; symbol++) {
		if (huffman->lengths[symbol] > huffman->maxbits)
			return 0;
		histogram[huffman->lengths[symbol]]++;
	}
	start = 0;
	for (length = 32; length > 0; length--) {
		next = (start + histogram[length]) >> 1;
		if (length != 1 && next * 2 != start + histogram[length])
			return 0;
		histogram[length] = start;
		start = next;
	}
	memset(huffman->lookup, 0, sizeof(uint16) << huffman->maxbits);
	for (symbol = 0; symbol < huffman->numcodes; symbol++) {
		length = huffman->lengths[symbol];
		if (length == 0)
			continue;
		code = histogram[length]++;
		last = (code + 1) << (huffman->maxbits - length);
		if (last > (1u << huffman->maxbits))
			return 0;
		for (entry = code << (huffman->maxbits - length); entry < last; entry++)
			huffman->lookup[entry] = (uint16)((symbol << 5) | length);
	}
	return 1;
}

static int HuffmanDecode(ChdHuffman *huffman, ChdBits *bits)
{
	uint16 entry;
	entry = huffman->lookup[BitsPeek(bits, huffman->maxbits)];
	BitsSkip(bits, entry & 31);
	return entry >> 5;
}

static int HuffmanImportRle(ChdHuffman *huffman, ChdBits *bits)
{
	int numbits;
	int code;
	int value;
	int repeat;
	numbits = 3;
	if (huffman->maxbits >= 16)
		numbits = 5;
	else if (huffman->maxbits >= 8)
		numbits = 4;
	code = 0;
	while (code < huffman->numcodes) {
		value = (int)BitsRead(bits, numbits);
		if (value != 1) {
			huffman->lengths[code++] = (uint8)value;
			continue;
		}
		value = (int)BitsRead(bits, numbits);
		if (value == 1) {
			huffman->lengths[code++] = 1;
			continue;
		}
		repeat = (int)BitsRead(bits, numbits) + 3;
		if (code + repeat > huffman->numcodes)
			return 0;
		while (repeat--)
			huffman->lengths[code++] = (uint8)value;
	}
	return HuffmanBuild(huffman);
}

static int HuffmanImportTree(ChdHuffman *huffman, ChdBits *bits)
{
	ChdHuffman small;
	uint16 smalllookup[64];
	uint32 temp;
	int start;
	int count;
	int index;
	int fullbits;
	int last;
	int code;
	int value;
	int repeat;
	small.numcodes = 24;
	small.maxbits = 6;
	small.lookup = smalllookup;
	small.lengths[0] = (uint8)BitsRead(bits, 3);
	start = (int)BitsRead(bits, 3) + 1;
	count = 0;
	for (index = 1; index < 24; index++) {
		small.lengths[index] = 0;
		if (index >= start && count != 7) {
			count = (int)BitsRead(bits, 3);
			if (count != 7)
				small.lengths[index] = (uint8)count;
		}
	}
	if (!HuffmanBuild(&small))
		return 0;
	temp = (uint32)huffman->numcodes - 9;
	fullbits = 0;
	while (temp != 0) {
		temp >>= 1;
		fullbits++;
	}
	last = 0;
	code = 0;
	while (code < huffman->numcodes) {
		value = HuffmanDecode(&small, bits);
		if (value != 0) {
			last = value - 1;
			huffman->lengths[code++] = (uint8)last;
			continue;
		}
		repeat = (int)BitsRead(bits, 3) + 2;
		if (repeat == 9)
			repeat += (int)BitsRead(bits, fullbits);
		for (; repeat != 0 && code < huffman->numcodes; repeat--)
			huffman->lengths[code++] = (uint8)last;
	}
	return HuffmanBuild(huffman);
}

static uint32 LzmaByte(ChdLzma *lzma)
{
	if (lzma->sourceposition >= lzma->sourcelength)
		return 0;
	return lzma->source[lzma->sourceposition++];
}

static uint32 LzmaBit(ChdLzma *lzma, uint16 *probability)
{
	uint32 bound;
	uint32 bit;
	bound = (lzma->range >> 11) * *probability;
	if (lzma->code < bound) {
		*probability = (uint16)(*probability + ((2048 - *probability) >> 5));
		lzma->range = bound;
		bit = 0;
	} else {
		*probability = (uint16)(*probability - (*probability >> 5));
		lzma->code -= bound;
		lzma->range -= bound;
		bit = 1;
	}
	if (lzma->range < (1u << 24)) {
		lzma->range <<= 8;
		lzma->code = (lzma->code << 8) | LzmaByte(lzma);
	}
	return bit;
}

static uint32 LzmaDirect(ChdLzma *lzma, int count)
{
	uint32 result;
	uint32 mask;
	result = 0;
	while (count--) {
		lzma->range >>= 1;
		lzma->code -= lzma->range;
		mask = 0 - (lzma->code >> 31);
		lzma->code += lzma->range & mask;
		if (lzma->range < (1u << 24)) {
			lzma->range <<= 8;
			lzma->code = (lzma->code << 8) | LzmaByte(lzma);
		}
		result = (result << 1) + (mask + 1);
	}
	return result;
}

static uint32 LzmaTree(ChdLzma *lzma, uint16 *probabilities, int count)
{
	uint32 node;
	int index;
	node = 1;
	for (index = 0; index < count; index++)
		node = (node << 1) + LzmaBit(lzma, &probabilities[node]);
	return node - (1u << count);
}

static uint32 LzmaReverse(ChdLzma *lzma, uint16 *probabilities, int count)
{
	uint32 node;
	uint32 symbol;
	uint32 bit;
	int index;
	node = 1;
	symbol = 0;
	for (index = 0; index < count; index++) {
		bit = LzmaBit(lzma, &probabilities[node]);
		node = (node << 1) + bit;
		symbol |= bit << index;
	}
	return symbol;
}

static uint32 LzmaLength(ChdLzma *lzma, ChdLzmaLength *length, uint32 posstate)
{
	if (!LzmaBit(lzma, &length->choice))
		return LzmaTree(lzma, length->low[posstate], 3);
	if (!LzmaBit(lzma, &length->choice2))
		return 8 + LzmaTree(lzma, length->mid[posstate], 3);
	return 16 + LzmaTree(lzma, length->high, 8);
}

static uint32 LzmaDistance(ChdLzma *lzma, uint32 length)
{
	uint32 slot;
	uint32 distance;
	int direct;
	if (length > 3)
		length = 3;
	slot = LzmaTree(lzma, lzma->probs.posslot[length], 6);
	if (slot < 4)
		return slot;
	direct = (int)(slot >> 1) - 1;
	distance = (2 | (slot & 1)) << direct;
	if (slot < 14)
		return distance + LzmaReverse(lzma, lzma->probs.posdecoders + distance - slot, direct);
	distance += LzmaDirect(lzma, direct - 4) << 4;
	return distance + LzmaReverse(lzma, lzma->probs.align, 4);
}

static int LzmaDecode(ChdLzma *lzma, const uint8 *source, uint32 sourcelength, uint8 *target, uint32 targetlength)
{
	uint16 *probability;
	uint16 *literal;
	uint32 rep[4];
	uint32 index;
	uint32 state;
	uint32 position;
	uint32 posstate;
	uint32 previous;
	uint32 symbol;
	uint32 matchbyte;
	uint32 matchbit;
	uint32 bit;
	uint32 length;
	uint32 distance;
	probability = (uint16 *)&lzma->probs;
	for (index = 0; index < sizeof(ChdLzmaProbs) / sizeof(uint16); index++)
		probability[index] = 1024;
	lzma->source = source;
	lzma->sourcelength = sourcelength;
	lzma->sourceposition = 0;
	lzma->range = 0xffffffffu;
	lzma->code = 0;
	if (LzmaByte(lzma) != 0)
		return 0;
	for (index = 0; index < 4; index++)
		lzma->code = (lzma->code << 8) | LzmaByte(lzma);
	memset(rep, 0, sizeof(rep));
	state = 0;
	position = 0;
	while (position < targetlength) {
		posstate = position & 3;
		if (!LzmaBit(lzma, &lzma->probs.ismatch[(state << 4) + posstate])) {
			previous = 0;
			if (position > 0)
				previous = target[position - 1];
			literal = lzma->probs.literal + 0x300 * (previous >> 5);
			symbol = 1;
			if (state >= 7) {
				matchbyte = target[position - rep[0] - 1];
				while (symbol < 0x100) {
					matchbit = (matchbyte >> 7) & 1;
					matchbyte <<= 1;
					bit = LzmaBit(lzma, &literal[((1 + matchbit) << 8) + symbol]);
					symbol = (symbol << 1) | bit;
					if (matchbit != bit)
						break;
				}
			}
			while (symbol < 0x100)
				symbol = (symbol << 1) | LzmaBit(lzma, &literal[symbol]);
			target[position++] = (uint8)symbol;
			if (state < 4)
				state = 0;
			else if (state < 10)
				state -= 3;
			else
				state -= 6;
			continue;
		}
		if (LzmaBit(lzma, &lzma->probs.isrep[state])) {
			if (position == 0)
				return 0;
			if (!LzmaBit(lzma, &lzma->probs.isrepg0[state])) {
				if (!LzmaBit(lzma, &lzma->probs.isrep0long[(state << 4) + posstate])) {
					if (state < 7)
						state = 9;
					else
						state = 11;
					target[position] = target[position - rep[0] - 1];
					position++;
					continue;
				}
			} else {
				if (!LzmaBit(lzma, &lzma->probs.isrepg1[state]))
					distance = rep[1];
				else {
					if (!LzmaBit(lzma, &lzma->probs.isrepg2[state]))
						distance = rep[2];
					else {
						distance = rep[3];
						rep[3] = rep[2];
					}
					rep[2] = rep[1];
				}
				rep[1] = rep[0];
				rep[0] = distance;
			}
			length = LzmaLength(lzma, &lzma->probs.replength, posstate);
			if (state < 7)
				state = 8;
			else
				state = 11;
		} else {
			rep[3] = rep[2];
			rep[2] = rep[1];
			rep[1] = rep[0];
			length = LzmaLength(lzma, &lzma->probs.length, posstate);
			if (state < 7)
				state = 7;
			else
				state = 10;
			rep[0] = LzmaDistance(lzma, length);
			if (rep[0] == 0xffffffffu)
				break;
			if (rep[0] >= position)
				return 0;
		}
		length += 2;
		if (length > targetlength - position)
			length = targetlength - position;
		for (; length != 0; length--) {
			target[position] = target[position - rep[0] - 1];
			position++;
		}
	}
	return position == targetlength;
}

static int FlacResidual(ChdBits *bits, int32 *samples, uint32 blocksize, uint32 order)
{
	uint32 method;
	uint32 parambits;
	uint32 escape;
	uint32 partitionorder;
	uint32 partition;
	uint32 count;
	uint32 param;
	uint32 quotient;
	uint32 value;
	uint32 index;
	method = BitsRead(bits, 2);
	if (method > 1)
		return 0;
	parambits = 4 + method;
	escape = (1u << parambits) - 1;
	partitionorder = BitsRead(bits, 4);
	if ((blocksize >> partitionorder) < order || ((blocksize >> partitionorder) << partitionorder) != blocksize)
		return 0;
	index = order;
	for (partition = 0; partition < (1u << partitionorder); partition++) {
		count = blocksize >> partitionorder;
		if (partition == 0)
			count -= order;
		param = BitsRead(bits, (int)parambits);
		if (param == escape) {
			param = BitsRead(bits, 5);
			for (; count != 0; count--)
				samples[index++] = BitsSigned(bits, (int)param);
			continue;
		}
		for (; count != 0; count--) {
			quotient = 0;
			while (BitsRead(bits, 1) == 0) {
				quotient++;
				if (BitsOverflow(bits))
					return 0;
			}
			value = (quotient << param) | BitsRead(bits, (int)param);
			samples[index++] = (int32)(value >> 1) ^ -(int32)(value & 1);
		}
	}
	return !BitsOverflow(bits);
}

static int FlacSubframe(ChdBits *bits, int32 *samples, uint32 blocksize, int bps)
{
	int32 coefficients[32];
	int64 sum;
	uint32 type;
	uint32 order;
	uint32 index;
	uint32 tap;
	int wasted;
	int precision;
	int shift;
	if (BitsRead(bits, 1) != 0)
		return 0;
	type = BitsRead(bits, 6);
	wasted = 0;
	if (BitsRead(bits, 1)) {
		wasted = 1;
		while (BitsRead(bits, 1) == 0 && wasted < bps)
			wasted++;
	}
	bps -= wasted;
	if (bps <= 0)
		return 0;
	if (type == 0) {
		samples[0] = BitsSigned(bits, bps);
		for (index = 1; index < blocksize; index++)
			samples[index] = samples[0];
	} else if (type == 1) {
		for (index = 0; index < blocksize; index++)
			samples[index] = BitsSigned(bits, bps);
	} else if (type >= 8 && type <= 12) {
		order = type - 8;
		if (order > blocksize)
			return 0;
		for (index = 0; index < order; index++)
			samples[index] = BitsSigned(bits, bps);
		if (!FlacResidual(bits, samples, blocksize, order))
			return 0;
		for (index = order; index < blocksize; index++) {
			if (order == 1)
				samples[index] += samples[index - 1];
			else if (order == 2)
				samples[index] += 2 * samples[index - 1] - samples[index - 2];
			else if (order == 3)
				samples[index] += 3 * samples[index - 1] - 3 * samples[index - 2] + samples[index - 3];
			else if (order == 4)
				samples[index] += 4 * samples[index - 1] - 6 * samples[index - 2] + 4 * samples[index - 3] - samples[index - 4];
		}
	} else if (type >= 32) {
		order = type - 31;
		if (order > blocksize)
			return 0;
		for (index = 0; index < order; index++)
			samples[index] = BitsSigned(bits, bps);
		precision = (int)BitsRead(bits, 4) + 1;
		shift = BitsSigned(bits, 5);
		if (precision == 16 || shift < 0)
			return 0;
		for (index = 0; index < order; index++)
			coefficients[index] = BitsSigned(bits, precision);
		if (!FlacResidual(bits, samples, blocksize, order))
			return 0;
		for (index = order; index < blocksize; index++) {
			sum = 0;
			for (tap = 0; tap < order; tap++)
				sum += (int64)coefficients[tap] * samples[index - 1 - tap];
			samples[index] += (int32)(sum >> shift);
		}
	} else
		return 0;
	if (wasted)
		for (index = 0; index < blocksize; index++)
			samples[index] <<= wasted;
	return 1;
}

static uint32 FlacFrame(ChdBits *bits, int32 *left, int32 *right, uint32 room)
{
	uint32 blockcode;
	uint32 ratecode;
	uint32 assignment;
	uint32 samplecode;
	uint32 value;
	uint32 mask;
	uint32 blocksize;
	uint32 index;
	int32 mid;
	int32 side;
	int sidebits;
	if (BitsRead(bits, 14) != 0x3ffe)
		return 0;
	BitsRead(bits, 2);
	blockcode = BitsRead(bits, 4);
	ratecode = BitsRead(bits, 4);
	assignment = BitsRead(bits, 4);
	samplecode = BitsRead(bits, 3);
	BitsRead(bits, 1);
	value = BitsRead(bits, 8);
	for (mask = 0x40; (value & 0x80) && (value & mask); mask >>= 1)
		BitsRead(bits, 8);
	blocksize = 0;
	if (blockcode == 1)
		blocksize = 192;
	else if (blockcode >= 2 && blockcode <= 5)
		blocksize = 576u << (blockcode - 2);
	else if (blockcode == 6)
		blocksize = BitsRead(bits, 8) + 1;
	else if (blockcode == 7)
		blocksize = BitsRead(bits, 16) + 1;
	else if (blockcode >= 8)
		blocksize = 256u << (blockcode - 8);
	if (ratecode == 12)
		BitsRead(bits, 8);
	else if (ratecode == 13 || ratecode == 14)
		BitsRead(bits, 16);
	BitsRead(bits, 8);
	if (blocksize == 0 || blocksize > room || (samplecode != 0 && samplecode != 4) || (assignment != 1 && (assignment < 8 || assignment > 10)))
		return 0;
	sidebits = 0;
	if (assignment == 9)
		sidebits = 1;
	if (!FlacSubframe(bits, left, blocksize, 16 + sidebits))
		return 0;
	sidebits = 0;
	if (assignment == 8 || assignment == 10)
		sidebits = 1;
	if (!FlacSubframe(bits, right, blocksize, 16 + sidebits))
		return 0;
	for (index = 0; index < blocksize; index++) {
		if (assignment == 8)
			right[index] = left[index] - right[index];
		else if (assignment == 9)
			left[index] += right[index];
		else if (assignment == 10) {
			side = right[index];
			mid = (int32)(((uint32)left[index] << 1) | (uint32)(side & 1));
			left[index] = (mid + side) >> 1;
			right[index] = (mid - side) >> 1;
		}
	}
	BitsSkip(bits, bits->count & 7);
	BitsRead(bits, 16);
	return blocksize;
}

static int FlacDecode(ChdImage *image, const uint8 *source, uint32 length, uint8 *target)
{
	ChdBits bits;
	int32 *left;
	int32 *right;
	uint32 frames;
	uint32 done;
	uint32 blocksize;
	uint32 index;
	uint32 high;
	uint32 low;
	if (length < 1 || (source[0] != 'L' && source[0] != 'B'))
		return 0;
	high = 1;
	low = 0;
	if (source[0] == 'B') {
		high = 0;
		low = 1;
	}
	frames = image->hunkbytes / 4;
	left = image->flac;
	right = image->flac + frames;
	BitsInit(&bits, source + 1, length - 1);
	done = 0;
	while (done < frames) {
		blocksize = FlacFrame(&bits, left, right, frames - done);
		if (blocksize == 0 || BitsOverflow(&bits))
			return 0;
		for (index = 0; index < blocksize; index++) {
			target[(done + index) * 4 + high] = (uint8)(left[index] >> 8);
			target[(done + index) * 4 + low] = (uint8)left[index];
			target[(done + index) * 4 + 2 + high] = (uint8)(right[index] >> 8);
			target[(done + index) * 4 + 2 + low] = (uint8)right[index];
		}
		done += blocksize;
	}
	return 1;
}

static int HuffDecode(ChdImage *image, const uint8 *source, uint32 length, uint8 *target)
{
	ChdHuffman huffman;
	ChdBits bits;
	uint32 index;
	huffman.numcodes = 256;
	huffman.maxbits = 16;
	huffman.lookup = image->hufflookup;
	BitsInit(&bits, source, length);
	if (!HuffmanImportTree(&huffman, &bits))
		return 0;
	for (index = 0; index < image->hunkbytes; index++)
		target[index] = (uint8)HuffmanDecode(&huffman, &bits);
	return !BitsOverflow(&bits);
}

static int Decompress(ChdImage *image, uint32 codec, uint32 length, uint8 *target)
{
	if (codec == CHDCODECZLIB)
		return ImageInflate(image->packed, length, target, image->hunkbytes);
	if (codec == CHDCODECLZMA)
		return LzmaDecode(image->lzma, image->packed, length, target, image->hunkbytes);
	if (codec == CHDCODECHUFF)
		return HuffDecode(image, image->packed, length, target);
	if (codec == CHDCODECFLAC)
		return FlacDecode(image, image->packed, length, target);
	return 0;
}

static int ReadHunk(ChdImage *image, uint32 hunk, uint8 *target)
{
	uint64 entry;
	uint32 type;
	uint32 length;
	uint64 offset;
	int depth;
	for (depth = 0; depth < CHDMAXSELF && hunk < image->hunkcount; depth++) {
		entry = image->map[hunk];
		type = (uint32)(entry & 15);
		length = (uint32)((entry >> 4) & 0xfffff);
		offset = entry >> 24;
		if (type == CHDTYPESELF) {
			hunk = (uint32)offset;
			continue;
		}
		if (type == CHDTYPENONE)
			return ImageReadAt(image->file, offset, target, image->hunkbytes);
		if (type == CHDTYPEUNALLOCATED) {
			if (image->parent)
				return ReadHunk(image->parent, hunk, target);
			memset(target, 0, image->hunkbytes);
			return 1;
		}
		if (type > 3 || !ImageReadAt(image->file, offset, image->packed, length))
			return 0;
		return Decompress(image, image->compressors[type], length, target);
	}
	return 0;
}

static uint8 *CachedHunk(ChdImage *image, uint32 hunk)
{
	if (image->cachedhunk == hunk)
		return image->hunk;
	image->cachedhunk = CHDNOHUNK;
	if (!ReadHunk(image, hunk, image->hunk))
		return NULL;
	image->cachedhunk = hunk;
	return image->hunk;
}

static int LoadRawMap(ChdImage *image)
{
	uint8 *raw;
	uint32 hunk;
	uint32 value;
	raw = (uint8 *)malloc((size_t)image->hunkcount * 4);
	if (raw == NULL || !ImageReadAt(image->file, image->mapoffset, raw, (size_t)image->hunkcount * 4)) {
		free(raw);
		return 0;
	}
	for (hunk = 0; hunk < image->hunkcount; hunk++) {
		value = ImageLoadBig32(raw + hunk * 4);
		image->map[hunk] = CHDTYPEUNALLOCATED;
		if (value != 0)
			image->map[hunk] = Pack(CHDTYPENONE, 0, (uint64)value * image->hunkbytes);
	}
	free(raw);
	return 1;
}

static int DecodeMap(ChdImage *image, ChdBits *bits, uint64 offset, const uint8 *header)
{
	ChdHuffman huffman;
	uint16 lookup[256];
	uint8 raw[12];
	uint64 entryoffset;
	uint64 lastparent;
	uint32 lastself;
	uint32 hunk;
	uint32 type;
	uint32 length;
	uint32 crc;
	uint32 last;
	uint32 repeat;
	uint16 mapcrc;
	huffman.numcodes = 16;
	huffman.maxbits = 8;
	huffman.lookup = lookup;
	if (!HuffmanImportRle(&huffman, bits))
		return 0;
	last = 0;
	repeat = 0;
	for (hunk = 0; hunk < image->hunkcount; hunk++) {
		if (repeat > 0) {
			image->map[hunk] = last;
			repeat--;
			continue;
		}
		type = (uint32)HuffmanDecode(&huffman, bits);
		if (type == CHDTYPERLESMALL)
			repeat = 2 + (uint32)HuffmanDecode(&huffman, bits);
		else if (type == CHDTYPERLELARGE) {
			repeat = 2 + 16 + ((uint32)HuffmanDecode(&huffman, bits) << 4);
			repeat += (uint32)HuffmanDecode(&huffman, bits);
		} else
			last = type;
		image->map[hunk] = last;
	}
	lastself = 0;
	lastparent = 0;
	mapcrc = 0xffff;
	for (hunk = 0; hunk < image->hunkcount; hunk++) {
		type = (uint32)image->map[hunk];
		entryoffset = offset;
		length = 0;
		crc = 0;
		if (type <= 3) {
			length = BitsRead(bits, header[12]);
			offset += length;
			crc = BitsRead(bits, 16);
		} else if (type == CHDTYPENONE) {
			length = image->hunkbytes;
			offset += length;
			crc = BitsRead(bits, 16);
		} else if (type == CHDTYPESELF) {
			lastself = BitsRead(bits, header[13]);
			entryoffset = lastself;
		} else if (type == CHDTYPEPARENT) {
			lastparent = BitsRead(bits, header[14]);
			entryoffset = lastparent;
		} else if (type == CHDTYPESELF0 || type == CHDTYPESELF1) {
			if (type == CHDTYPESELF1)
				lastself++;
			type = CHDTYPESELF;
			entryoffset = lastself;
		} else if (type == CHDTYPEPARENTSELF) {
			type = CHDTYPEPARENT;
			lastparent = (uint64)hunk * image->hunkbytes / image->unitbytes;
			entryoffset = lastparent;
		} else if (type == CHDTYPEPARENT0 || type == CHDTYPEPARENT1) {
			if (type == CHDTYPEPARENT1)
				lastparent += image->hunkbytes / image->unitbytes;
			type = CHDTYPEPARENT;
			entryoffset = lastparent;
		} else
			return 0;
		raw[0] = (uint8)type;
		raw[1] = (uint8)(length >> 16);
		raw[2] = (uint8)(length >> 8);
		raw[3] = (uint8)length;
		raw[4] = (uint8)(entryoffset >> 40);
		raw[5] = (uint8)(entryoffset >> 32);
		ImageStoreBig32(raw + 6, (uint32)entryoffset);
		raw[10] = (uint8)(crc >> 8);
		raw[11] = (uint8)crc;
		mapcrc = Crc16(mapcrc, raw, 12);
		if (length >= CHDMAXHUNK || entryoffset >= CHDMAXOFFSET)
			return 0;
		if (type == CHDTYPENONE)
			length = 0;
		image->map[hunk] = Pack(type, length, entryoffset);
	}
	return mapcrc == ((header[10] << 8) | header[11]) && !BitsOverflow(bits);
}

static int LoadCompressedMap(ChdImage *image)
{
	uint8 header[16];
	uint8 *packed;
	ChdBits bits;
	uint32 length;
	uint64 offset;
	int decoded;
	if (!ImageReadAt(image->file, image->mapoffset, header, sizeof(header)))
		return 0;
	length = ImageLoadBig32(header);
	offset = ((uint64)ImageLoadBig32(header + 4) << 16) | ((uint32)header[8] << 8) | header[9];
	packed = (uint8 *)malloc((size_t)length + 1);
	if (packed == NULL || !ImageReadAt(image->file, image->mapoffset + 16, packed, length)) {
		free(packed);
		return 0;
	}
	BitsInit(&bits, packed, length);
	decoded = DecodeMap(image, &bits, offset, header);
	free(packed);
	return decoded;
}

static int UsesCodec(ChdImage *image, uint32 codec)
{
	int index;
	for (index = 0; index < 4; index++)
		if (image->compressors[index] == codec)
			return 1;
	return 0;
}

static ChdImage *OpenFile(const char *path, int readonly, int *status)
{
	FILE *file;
	ChdImage *image;
	uint8 header[CHDHEADER];
	uint64 hunkcount;
	int index;
	int loaded;
	*status = CHDNOTCHD;
	file = NULL;
	if (!readonly)
		file = fopen(path, "r+b");
	if (file == NULL) {
		readonly = 1;
		file = fopen(path, "rb");
	}
	if (file == NULL)
		return NULL;
	memset(header, 0, sizeof(header));
	ImageReadSome(file, 0, header, sizeof(header));
	if (memcmp(header, "MComprHD", 8) != 0) {
		fclose(file);
		return NULL;
	}
	*status = CHDFAILED;
	image = (ChdImage *)calloc(1, sizeof(ChdImage));
	if (image == NULL) {
		fclose(file);
		return NULL;
	}
	image->file = file;
	image->readonly = readonly;
	image->cachedhunk = CHDNOHUNK;
	for (index = 0; index < 4; index++)
		image->compressors[index] = ImageLoadBig32(header + 16 + index * 4);
	image->logical = ImageLoadBig64(header + 32);
	image->mapoffset = ImageLoadBig64(header + 40);
	image->hunkbytes = ImageLoadBig32(header + 56);
	image->unitbytes = ImageLoadBig32(header + 60);
	memcpy(image->sha1, header + 84, 20);
	memcpy(image->parentsha1, header + 104, 20);
	hunkcount = 0;
	if (image->hunkbytes != 0)
		hunkcount = (image->logical + image->hunkbytes - 1) / image->hunkbytes;
	if (ImageLoadBig32(header + 8) != CHDHEADER || ImageLoadBig32(header + 12) != CHDVERSION || image->hunkbytes == 0 ||
		image->hunkbytes > CHDMAXHUNK || image->unitbytes == 0 || image->hunkbytes % image->unitbytes != 0 || hunkcount == 0 ||
		hunkcount > 0xffffffffu) {
		ChdClose(image);
		return NULL;
	}
	for (index = 0; index < 4; index++) {
		if (image->compressors[index] != 0 && image->compressors[index] != CHDCODECZLIB && image->compressors[index] != CHDCODECLZMA &&
			image->compressors[index] != CHDCODECHUFF && image->compressors[index] != CHDCODECFLAC) {
			ChdClose(image);
			return NULL;
		}
	}
	image->hunkcount = (uint32)hunkcount;
	if (image->compressors[0] != 0)
		image->readonly = 1;
	image->map = (uint64 *)malloc((size_t)image->hunkcount * 8);
	image->hunk = (uint8 *)malloc(image->hunkbytes);
	image->packed = (uint8 *)malloc(image->hunkbytes);
	if (UsesCodec(image, CHDCODECLZMA))
		image->lzma = (ChdLzma *)malloc(sizeof(ChdLzma));
	if (UsesCodec(image, CHDCODECHUFF))
		image->hufflookup = (uint16 *)malloc(sizeof(uint16) << 16);
	if (UsesCodec(image, CHDCODECFLAC))
		image->flac = (int32 *)malloc((size_t)image->hunkbytes / 4 * 2 * sizeof(int32));
	loaded = 0;
	if (image->map && image->hunk && image->packed && (image->lzma || !UsesCodec(image, CHDCODECLZMA)) &&
		(image->hufflookup || !UsesCodec(image, CHDCODECHUFF)) && (image->flac || !UsesCodec(image, CHDCODECFLAC))) {
		if (image->compressors[0] == 0)
			loaded = LoadRawMap(image);
		else
			loaded = LoadCompressedMap(image);
	}
	if (!loaded) {
		ChdClose(image);
		return NULL;
	}
	image->fileend = (ImageFileSize(file) + image->hunkbytes - 1) / image->hunkbytes * image->hunkbytes;
	*status = CHDOPENED;
	return image;
}

static int HasParent(const uint8 *sha1)
{
	int index;
	for (index = 0; index < 20; index++)
		if (sha1[index] != 0)
			return 1;
	return 0;
}

static ChdImage *CreateDiff(ChdImage *base, const char *path)
{
	FILE *file;
	uint8 header[CHDHEADER];
	uint8 *map;
	int written;
	int status;
	file = fopen(path, "wb");
	if (file == NULL)
		return NULL;
	memset(header, 0, sizeof(header));
	memcpy(header, "MComprHD", 8);
	ImageStoreBig32(header + 8, CHDHEADER);
	ImageStoreBig32(header + 12, CHDVERSION);
	ImageStoreBig64(header + 32, base->logical);
	ImageStoreBig64(header + 40, CHDHEADER);
	ImageStoreBig32(header + 56, base->hunkbytes);
	ImageStoreBig32(header + 60, base->unitbytes);
	memcpy(header + 104, base->sha1, 20);
	map = (uint8 *)calloc(base->hunkcount, 4);
	written = map != NULL && fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
		fwrite(map, 4, base->hunkcount, file) == base->hunkcount;
	free(map);
	if (fclose(file) != 0 || !written) {
		remove(path);
		return NULL;
	}
	return OpenFile(path, 0, &status);
}

int ChdOpen(const char *path, int readonly, ChdImage **image)
{
	ChdImage *base;
	ChdImage *diff;
	FILE *existing;
	char diffpath[CHDMAXPATH];
	int status;
	*image = NULL;
	base = OpenFile(path, readonly, &status);
	if (base == NULL)
		return status;
	if (HasParent(base->parentsha1)) {
		ChdClose(base);
		return CHDFAILED;
	}
	*image = base;
	if (base->compressors[0] == 0 || strlen(path) + strlen(CHDDIFFSUFFIX) >= CHDMAXPATH)
		return CHDOPENED;
	strcpy(diffpath, path);
	strcat(diffpath, CHDDIFFSUFFIX);
	existing = fopen(diffpath, "rb");
	if (existing == NULL) {
		diff = NULL;
		if (!readonly)
			diff = CreateDiff(base, diffpath);
		if (diff == NULL)
			return CHDOPENED;
	} else {
		fclose(existing);
		diff = OpenFile(diffpath, readonly, &status);
		if (diff == NULL || diff->compressors[0] != 0 || memcmp(diff->parentsha1, base->sha1, 20) != 0 ||
			diff->hunkbytes != base->hunkbytes || diff->logical != base->logical) {
			if (diff)
				ChdClose(diff);
			ChdClose(base);
			*image = NULL;
			return CHDFAILED;
		}
	}
	diff->parent = base;
	*image = diff;
	return CHDOPENED;
}

void ChdClose(ChdImage *image)
{
	if (image->parent)
		ChdClose(image->parent);
	fclose(image->file);
	free(image->map);
	free(image->hunk);
	free(image->packed);
	free(image->lzma);
	free(image->hufflookup);
	free(image->flac);
	free(image);
}

size_t ChdRead(ChdImage *image, void *buffer, loff_t offset, size_t length)
{
	uint8 *bytes;
	uint8 *data;
	uint64 position;
	uint64 entry;
	uint32 hunk;
	uint32 inside;
	size_t done;
	size_t chunk;
	bytes = (uint8 *)buffer;
	done = 0;
	while (done < length) {
		position = (uint64)offset + done;
		if (position >= image->logical)
			break;
		hunk = (uint32)(position / image->hunkbytes);
		inside = (uint32)(position % image->hunkbytes);
		chunk = image->hunkbytes - inside;
		if (chunk > length - done)
			chunk = length - done;
		if (chunk > image->logical - position)
			chunk = (size_t)(image->logical - position);
		entry = image->map[hunk];
		if ((entry & 15) == CHDTYPENONE) {
			if (!ImageReadAt(image->file, (entry >> 24) + inside, bytes + done, chunk))
				break;
		} else {
			data = CachedHunk(image, hunk);
			if (data == NULL)
				break;
			memcpy(bytes + done, data + inside, chunk);
		}
		done += chunk;
	}
	return done;
}

size_t ChdWrite(ChdImage *image, const void *buffer, loff_t offset, size_t length)
{
	const uint8 *bytes;
	uint8 *data;
	uint8 stored[4];
	uint64 position;
	uint64 entry;
	uint64 host;
	uint32 hunk;
	uint32 inside;
	size_t done;
	size_t chunk;
	if (image->readonly)
		return 0;
	bytes = (const uint8 *)buffer;
	done = 0;
	while (done < length) {
		position = (uint64)offset + done;
		if (position >= image->logical)
			break;
		hunk = (uint32)(position / image->hunkbytes);
		inside = (uint32)(position % image->hunkbytes);
		chunk = image->hunkbytes - inside;
		if (chunk > length - done)
			chunk = length - done;
		if (chunk > image->logical - position)
			chunk = (size_t)(image->logical - position);
		entry = image->map[hunk];
		if ((entry & 15) == CHDTYPENONE) {
			if (!ImageWriteAt(image->file, (entry >> 24) + inside, bytes + done, chunk))
				break;
			if (image->cachedhunk == hunk)
				memcpy(image->hunk + inside, bytes + done, chunk);
		} else {
			data = CachedHunk(image, hunk);
			host = image->fileend;
			if (data == NULL || host / image->hunkbytes > 0xffffffffu)
				break;
			memcpy(data + inside, bytes + done, chunk);
			ImageStoreBig32(stored, (uint32)(host / image->hunkbytes));
			if (!ImageWriteAt(image->file, host, data, image->hunkbytes) ||
				!ImageWriteAt(image->file, image->mapoffset + (uint64)hunk * 4, stored, 4)) {
				image->cachedhunk = CHDNOHUNK;
				break;
			}
			image->fileend += image->hunkbytes;
			image->map[hunk] = Pack(CHDTYPENONE, 0, host);
		}
		done += chunk;
	}
	fflush(image->file);
	return done;
}

loff_t ChdSize(ChdImage *image)
{
	return (loff_t)image->logical;
}

int ChdReadOnly(ChdImage *image)
{
	return image->readonly;
}
