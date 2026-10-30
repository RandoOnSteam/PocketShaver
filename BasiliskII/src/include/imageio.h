/*
 *  imageio.h - shared file and inflate helpers for disk image formats
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

#ifndef IMAGEIO_H
#define IMAGEIO_H

#if defined(_MSC_VER)
#define IMAGESEEK _fseeki64
#define IMAGETELL _ftelli64
#else
#define IMAGESEEK fseeko
#define IMAGETELL ftello
#endif

uint64 ImageLoadBig64(const uint8 *data);
uint32 ImageLoadBig32(const uint8 *data);
void ImageStoreBig64(uint8 *data, uint64 value);
void ImageStoreBig32(uint8 *data, uint32 value);
size_t ImageReadSome(FILE *file, uint64 offset, void *buffer, size_t length);
int ImageReadAt(FILE *file, uint64 offset, void *buffer, size_t length);
int ImageWriteAt(FILE *file, uint64 offset, const void *buffer, size_t length);
uint64 ImageFileSize(FILE *file);
int ImageInflate(const uint8 *source, size_t sourcelength, uint8 *target, size_t targetlength);

#endif
