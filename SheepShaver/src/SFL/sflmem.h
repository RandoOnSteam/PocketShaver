/*
 *  sflmem.h - Paged guest address space for the SFL platform layer
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

#ifndef SFLMEM_H
#define SFLMEM_H

#define SFLMEMPAGESHIFT 16
#define SFLMEMPAGESIZE 0x10000
#define SFLMEMPAGECOUNT 0x10000
#define SFLMEMMAXREGIONS 32
#define SFLMEMSLACK 16

bool SflMemInit(void);
void SflMemExit(void);
uint8 *SflMemMap(uint32 guest, uint32 size);
bool SflMemAlias(uint32 guest, uint32 existingguest, uint32 size);
void SflMemUnmap(uint32 guest, uint32 size);
uint32 SflMemHostToGuest(uint8 *addr);
bool SflMemIsMapped(uint8 *addr);

#endif
