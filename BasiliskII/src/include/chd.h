/*
 *  chd.h - MAME CHD hard disk image support
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

#ifndef CHD_H
#define CHD_H

#define CHDNOTCHD 0
#define CHDOPENED 1
#define CHDFAILED 2

typedef struct ChdImage ChdImage;

int ChdOpen(const char *path, int readonly, ChdImage **image);
void ChdClose(ChdImage *image);
size_t ChdRead(ChdImage *image, void *buffer, loff_t offset, size_t length);
size_t ChdWrite(ChdImage *image, const void *buffer, loff_t offset, size_t length);
loff_t ChdSize(ChdImage *image);
int ChdReadOnly(ChdImage *image);

#endif
