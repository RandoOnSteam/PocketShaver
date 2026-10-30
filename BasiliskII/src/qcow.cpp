/*
 *  qcow.cpp - QEMU qcow2 disk images
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
#include "qcow.h"
#include "imageio.h"

#define QCOWMAGIC 0x514649fbu
#define QCOWHEADER 112
#define QCOWMAXDEPTH 64
#define QCOWMAXPATH 1024
#define QCOWOFFSETMASK 0x00fffffffffffe00ULL
#define QCOWREFCOUNTMASK 0xfffffffffffffe00ULL
#define QCOWCOPIED 0x8000000000000000ULL
#define QCOWCOMPRESSED 0x4000000000000000ULL
#define QCOWZERO 1ULL
#define QCOWBADENTRY 2ULL
#define QCOWINCOMPATDIRTY 1ULL

struct QcowImage {
	FILE *file;
	QcowImage *backing;
	int raw;
	int readonly;
	uint32 clusterbits;
	uint32 clustersize;
	uint32 l2bits;
	uint64 size;
	uint32 l1size;
	uint64 l1offset;
	uint64 *l1;
	uint64 refcounttableoffset;
	uint64 refcountentries;
	uint64 *refcounttable;
	uint64 fileend;
	uint64 l2offset;
	uint8 *l2;
	uint8 *cluster;
	uint8 *zeros;
	uint8 *packed;
	uint64 unpackedentry;
	uint8 *unpacked;
};

static int LoadL2(QcowImage *image, uint64 l2offset)
{
	if (image->l2offset == l2offset)
		return 1;
	image->l2offset = 0;
	if (!ImageReadAt(image->file, l2offset, image->l2, image->clustersize))
		return 0;
	image->l2offset = l2offset;
	return 1;
}

static uint64 ClusterEntry(QcowImage *image, uint64 position)
{
	uint64 l1index;
	uint64 l2offset;
	l1index = position >> (image->clusterbits + image->l2bits);
	if (l1index >= image->l1size)
		return 0;
	l2offset = image->l1[l1index] & QCOWOFFSETMASK;
	if (l2offset == 0)
		return 0;
	if (!LoadL2(image, l2offset))
		return QCOWBADENTRY;
	return ImageLoadBig64(image->l2 + ((position >> image->clusterbits) & ((1u << image->l2bits) - 1)) * 8);
}

static void CompressedSpan(QcowImage *image, uint64 entry, uint64 *host, size_t *length)
{
	uint32 shift;
	uint64 sectors;
	shift = 62 - (image->clusterbits - 8);
	*host = entry & ((1ULL << shift) - 1);
	sectors = ((entry >> shift) & ((1ULL << (image->clusterbits - 8)) - 1)) + 1;
	*length = (size_t)(sectors * 512 - (*host & 511));
	if (*length > (size_t)image->clustersize * 2)
		*length = (size_t)image->clustersize * 2;
}

static int Unpack(QcowImage *image, uint64 entry)
{
	uint64 host;
	size_t length;
	size_t got;
	if (image->unpackedentry == entry)
		return 1;
	image->unpackedentry = 0;
	CompressedSpan(image, entry, &host, &length);
	got = ImageReadSome(image->file, host, image->packed, length);
	if (!ImageInflate(image->packed, got, image->unpacked, image->clustersize))
		return 0;
	image->unpackedentry = entry;
	return 1;
}

static int ReadChunk(QcowImage *image, uint64 position, uint8 *target, size_t length)
{
	uint64 entry;
	uint32 inside;
	entry = ClusterEntry(image, position);
	inside = (uint32)(position & (image->clustersize - 1));
	if (entry == QCOWBADENTRY)
		return 0;
	if (entry & QCOWCOMPRESSED) {
		if (!Unpack(image, entry))
			return 0;
		memcpy(target, image->unpacked + inside, length);
		return 1;
	}
	if (!(entry & QCOWZERO) && (entry & QCOWOFFSETMASK) != 0)
		return ImageReadAt(image->file, (entry & QCOWOFFSETMASK) + inside, target, length);
	memset(target, 0, length);
	if (!(entry & QCOWZERO) && image->backing)
		QcowRead(image->backing, target, (loff_t)position, length);
	return 1;
}

static int AdjustRefcount(QcowImage *image, uint64 host, int delta)
{
	uint64 index;
	uint64 perblock;
	uint64 tableindex;
	uint64 block;
	uint8 stored[8];
	uint8 count[2];
	uint32 value;
	index = host >> image->clusterbits;
	perblock = image->clustersize / 2;
	tableindex = index / perblock;
	if (tableindex >= image->refcountentries)
		return 0;
	block = image->refcounttable[tableindex] & QCOWREFCOUNTMASK;
	if (block == 0) {
		block = image->fileend;
		image->fileend += image->clustersize;
		if (!ImageWriteAt(image->file, block, image->zeros, image->clustersize))
			return 0;
		image->refcounttable[tableindex] = block;
		ImageStoreBig64(stored, block);
		if (!ImageWriteAt(image->file, image->refcounttableoffset + tableindex * 8, stored, 8) || !AdjustRefcount(image, block, 1))
			return 0;
	}
	block += (index % perblock) * 2;
	if (!ImageReadAt(image->file, block, count, 2))
		return 0;
	value = ((uint32)count[0] << 8) | count[1];
	if (delta < 0 && value == 0)
		return 1;
	value = (uint32)((int)value + delta);
	count[0] = (uint8)(value >> 8);
	count[1] = (uint8)value;
	return ImageWriteAt(image->file, block, count, 2);
}

static uint64 Allocate(QcowImage *image)
{
	uint64 host;
	host = image->fileend;
	image->fileend += image->clustersize;
	if (!AdjustRefcount(image, host, 1))
		return 0;
	return host;
}

static void ReleaseCompressed(QcowImage *image, uint64 entry)
{
	uint64 host;
	uint64 cluster;
	uint64 last;
	size_t length;
	CompressedSpan(image, entry, &host, &length);
	last = (host + length - 1) >> image->clusterbits;
	for (cluster = host >> image->clusterbits; cluster <= last; cluster++)
		AdjustRefcount(image, cluster << image->clusterbits, -1);
}

static int FillCluster(QcowImage *image, uint64 position, uint64 entry)
{
	if (entry & QCOWCOMPRESSED) {
		if (!Unpack(image, entry))
			return 0;
		memcpy(image->cluster, image->unpacked, image->clustersize);
		return 1;
	}
	if (!(entry & QCOWZERO) && (entry & QCOWOFFSETMASK) != 0)
		return ImageReadAt(image->file, entry & QCOWOFFSETMASK, image->cluster, image->clustersize);
	memset(image->cluster, 0, image->clustersize);
	if (!(entry & QCOWZERO) && image->backing)
		QcowRead(image->backing, image->cluster, (loff_t)(position & ~(uint64)(image->clustersize - 1)), image->clustersize);
	return 1;
}

static uint64 WritableCluster(QcowImage *image, uint64 position)
{
	uint64 l1index;
	uint64 l2index;
	uint64 l2offset;
	uint64 entry;
	uint64 host;
	uint8 stored[8];
	l1index = position >> (image->clusterbits + image->l2bits);
	l2index = (position >> image->clusterbits) & ((1u << image->l2bits) - 1);
	if (l1index >= image->l1size)
		return 0;
	l2offset = image->l1[l1index] & QCOWOFFSETMASK;
	if (l2offset == 0) {
		l2offset = Allocate(image);
		if (l2offset == 0 || !ImageWriteAt(image->file, l2offset, image->zeros, image->clustersize))
			return 0;
		image->l1[l1index] = l2offset | QCOWCOPIED;
		ImageStoreBig64(stored, image->l1[l1index]);
		if (!ImageWriteAt(image->file, image->l1offset + l1index * 8, stored, 8))
			return 0;
	}
	if (!LoadL2(image, l2offset))
		return 0;
	entry = ImageLoadBig64(image->l2 + l2index * 8);
	if (!(entry & (QCOWCOMPRESSED | QCOWZERO)) && (entry & QCOWOFFSETMASK) != 0 && (entry & QCOWCOPIED))
		return entry & QCOWOFFSETMASK;
	if (!FillCluster(image, position, entry))
		return 0;
	host = Allocate(image);
	if (host == 0 || !ImageWriteAt(image->file, host, image->cluster, image->clustersize))
		return 0;
	if (entry & QCOWCOMPRESSED)
		ReleaseCompressed(image, entry);
	if (!LoadL2(image, l2offset))
		return 0;
	ImageStoreBig64(image->l2 + l2index * 8, host | QCOWCOPIED);
	if (!ImageWriteAt(image->file, l2offset + l2index * 8, image->l2 + l2index * 8, 8))
		return 0;
	return host;
}

static int ResolveBacking(const char *path, const char *name, char *resolved)
{
	const char *slash;
	const char *backslash;
	size_t directory;
	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	if (backslash && (slash == NULL || backslash > slash))
		slash = backslash;
	directory = 0;
	if (slash && name[0] != '/' && name[0] != '\\' && !(name[0] && name[1] == ':'))
		directory = (size_t)(slash - path) + 1;
	if (directory + strlen(name) >= QCOWMAXPATH)
		return 0;
	memcpy(resolved, path, directory);
	strcpy(resolved + directory, name);
	return 1;
}

static QcowImage *OpenChain(const char *path, int readonly, int depth, int *status)
{
	QcowImage *image;
	FILE *file;
	uint8 header[QCOWHEADER];
	char name[QCOWMAXPATH];
	char resolved[QCOWMAXPATH];
	uint64 backingoffset;
	uint32 backinglength;
	uint32 version;
	uint64 index;
	int backingstatus;
	*status = QCOWNOTQCOW;
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
	if (ImageLoadBig32(header) != QCOWMAGIC && depth == 0) {
		fclose(file);
		return NULL;
	}
	*status = QCOWFAILED;
	image = (QcowImage *)calloc(1, sizeof(QcowImage));
	if (image == NULL) {
		fclose(file);
		return NULL;
	}
	image->file = file;
	image->readonly = readonly;
	image->fileend = ImageFileSize(file);
	if (ImageLoadBig32(header) != QCOWMAGIC) {
		image->raw = 1;
		image->size = image->fileend;
		*status = QCOWOPENED;
		return image;
	}
	version = ImageLoadBig32(header + 4);
	image->clusterbits = ImageLoadBig32(header + 20);
	image->size = ImageLoadBig64(header + 24);
	if ((version != 2 && version != 3) || image->clusterbits < 9 || image->clusterbits > 21 || ImageLoadBig32(header + 32) != 0 ||
		(version == 3 && (ImageLoadBig64(header + 72) & ~QCOWINCOMPATDIRTY) != 0)) {
		QcowClose(image);
		return NULL;
	}
	if ((version == 3 && ImageLoadBig32(header + 96) != 4) || ImageLoadBig32(header + 60) != 0 || depth > 0)
		image->readonly = 1;
	image->clustersize = 1u << image->clusterbits;
	image->l2bits = image->clusterbits - 3;
	image->fileend = (image->fileend + image->clustersize - 1) & ~(uint64)(image->clustersize - 1);
	image->l1size = ImageLoadBig32(header + 36);
	image->l1offset = ImageLoadBig64(header + 40);
	image->refcounttableoffset = ImageLoadBig64(header + 48);
	image->refcountentries = (uint64)ImageLoadBig32(header + 56) * image->clustersize / 8;
	image->l1 = (uint64 *)malloc((size_t)image->l1size * 8 + 8);
	image->refcounttable = (uint64 *)malloc((size_t)image->refcountentries * 8 + 8);
	image->l2 = (uint8 *)malloc(image->clustersize);
	image->cluster = (uint8 *)malloc(image->clustersize);
	image->zeros = (uint8 *)calloc(1, image->clustersize);
	image->unpacked = (uint8 *)malloc(image->clustersize);
	image->packed = (uint8 *)malloc((size_t)image->clustersize * 2);
	if (!image->l1 || !image->refcounttable || !image->l2 || !image->cluster || !image->zeros || !image->unpacked || !image->packed ||
		!ImageReadAt(file, image->l1offset, image->l1, (size_t)image->l1size * 8) ||
		!ImageReadAt(file, image->refcounttableoffset, image->refcounttable, (size_t)image->refcountentries * 8)) {
		QcowClose(image);
		return NULL;
	}
	for (index = 0; index < image->l1size; index++)
		image->l1[index] = ImageLoadBig64((const uint8 *)&image->l1[index]);
	for (index = 0; index < image->refcountentries; index++)
		image->refcounttable[index] = ImageLoadBig64((const uint8 *)&image->refcounttable[index]);
	backingoffset = ImageLoadBig64(header + 8);
	backinglength = ImageLoadBig32(header + 16);
	if (backingoffset != 0) {
		if (depth >= QCOWMAXDEPTH || backinglength >= QCOWMAXPATH || !ImageReadAt(file, backingoffset, name, backinglength)) {
			QcowClose(image);
			return NULL;
		}
		name[backinglength] = 0;
		if (!ResolveBacking(path, name, resolved)) {
			QcowClose(image);
			return NULL;
		}
		image->backing = OpenChain(resolved, 1, depth + 1, &backingstatus);
		if (image->backing == NULL) {
			QcowClose(image);
			return NULL;
		}
	}
	if (!image->readonly && version == 3 && ImageLoadBig64(header + 88) != 0)
		ImageWriteAt(file, 88, image->zeros, 8);
	*status = QCOWOPENED;
	return image;
}

int QcowOpen(const char *path, int readonly, QcowImage **image)
{
	int status;
	*image = OpenChain(path, readonly, 0, &status);
	return status;
}

void QcowClose(QcowImage *image)
{
	if (image->backing)
		QcowClose(image->backing);
	fclose(image->file);
	free(image->l1);
	free(image->refcounttable);
	free(image->l2);
	free(image->cluster);
	free(image->zeros);
	free(image->unpacked);
	free(image->packed);
	free(image);
}

size_t QcowRead(QcowImage *image, void *buffer, loff_t offset, size_t length)
{
	uint8 *bytes;
	uint64 position;
	size_t done;
	size_t chunk;
	if (image->raw)
		return ImageReadSome(image->file, (uint64)offset, buffer, length);
	bytes = (uint8 *)buffer;
	done = 0;
	while (done < length) {
		position = (uint64)offset + done;
		if (position >= image->size)
			break;
		chunk = image->clustersize - (size_t)(position & (image->clustersize - 1));
		if (chunk > length - done)
			chunk = length - done;
		if (chunk > image->size - position)
			chunk = (size_t)(image->size - position);
		if (!ReadChunk(image, position, bytes + done, chunk))
			break;
		done += chunk;
	}
	return done;
}

size_t QcowWrite(QcowImage *image, const void *buffer, loff_t offset, size_t length)
{
	const uint8 *bytes;
	uint64 position;
	uint64 host;
	size_t done;
	size_t chunk;
	if (image->readonly)
		return 0;
	bytes = (const uint8 *)buffer;
	done = 0;
	while (done < length) {
		position = (uint64)offset + done;
		if (position >= image->size)
			break;
		chunk = image->clustersize - (size_t)(position & (image->clustersize - 1));
		if (chunk > length - done)
			chunk = length - done;
		if (chunk > image->size - position)
			chunk = (size_t)(image->size - position);
		host = WritableCluster(image, position);
		if (host == 0 || !ImageWriteAt(image->file, host + (position & (image->clustersize - 1)), bytes + done, chunk))
			break;
		done += chunk;
	}
	fflush(image->file);
	return done;
}

loff_t QcowSize(QcowImage *image)
{
	return (loff_t)image->size;
}

int QcowReadOnly(QcowImage *image)
{
	return image->readonly;
}
