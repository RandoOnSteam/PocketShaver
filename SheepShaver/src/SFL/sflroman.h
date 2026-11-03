/*
 *  sflroman.h - Mac Roman and UTF-8 conversion for the SFL platform layer
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

#ifndef SFLROMAN_H
#define SFLROMAN_H

#define SFLROMANUNKNOWN '?'

size_t SflRomanToUtf8(const uint8 *source, size_t length, char *target, size_t targetsize, bool crtolf);
size_t SflUtf8ToRoman(const char *source, size_t length, uint8 *target, size_t targetsize, bool lftocr);

#endif
