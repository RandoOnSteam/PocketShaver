/*
 *  qcow.h - QEMU qcow2 disk images
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

#ifndef QCOW_H
#define QCOW_H

#define QCOWNOTQCOW 0
#define QCOWOPENED 1
#define QCOWFAILED 2

typedef struct QcowImage QcowImage;

int QcowOpen(const char *path, int readonly, QcowImage **image);
void QcowClose(QcowImage *image);
size_t QcowRead(QcowImage *image, void *buffer, loff_t offset, size_t length);
size_t QcowWrite(QcowImage *image, const void *buffer, loff_t offset, size_t length);
loff_t QcowSize(QcowImage *image);
int QcowReadOnly(QcowImage *image);

#endif
