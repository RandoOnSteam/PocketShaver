/*
 *  sflmem.cpp - Paged guest address space for the SFL platform layer
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
#include "sflmem.h"
#include "vm_alloc.h"
#include "gui.h"

typedef struct SFLMEMREGION
{
	uint32 guest;
	uint32 size;
	uint8 *host;
	uint8 *block;
} SFLMEMREGION;

typedef struct SFLMEMSTATE
{
	SFLMEMREGION regions[SFLMEMMAXREGIONS];
	size_t numregions;
	uint8 *scratch;
} SFLMEMSTATE;

uintptr SflPageDelta[SFLMEMPAGECOUNT];
static SFLMEMSTATE SflMemState;

static uint32 SflMemPageCount(uint32 size)
{
	return (uint32)(((uint64)size + SFLMEMPAGESIZE - 1) >> SFLMEMPAGESHIFT);
}

static void SflMemSetPages(uint32 guest, uint32 numpages, uint8 *host)
{
	uint32 page;
	uint32 index;
	page = guest >> SFLMEMPAGESHIFT;
	for (index = 0; index < numpages; index++)
		SflPageDelta[page + index] = (uintptr)host - (uintptr)guest;
}

static void SflMemSetScratch(uint32 guest, uint32 numpages)
{
	uint32 page;
	uint32 index;
	page = guest >> SFLMEMPAGESHIFT;
	for (index = 0; index < numpages; index++)
		SflPageDelta[page + index] = (uintptr)SflMemState.scratch - ((uintptr)(page + index) << SFLMEMPAGESHIFT);
}

bool SflMemInit(void)
{
	SflMemState.numregions = 0;
	SflMemState.scratch = WSALLOC(uint8, SFLMEMPAGESIZE + SFLMEMSLACK);
	if (SflMemState.scratch == NULL)
		return false;
	WSMemoryClear(SflMemState.scratch, SFLMEMPAGESIZE + SFLMEMSLACK);
	SflMemSetScratch(0, SFLMEMPAGECOUNT);
	return true;
}

void SflMemExit(void)
{
	size_t index;
	for (index = 0; index < SflMemState.numregions; index++)
		if (SflMemState.regions[index].block != NULL)
			WSFREE(SflMemState.regions[index].block);
	SflMemState.numregions = 0;
	if (SflMemState.scratch != NULL)
		WSFREE(SflMemState.scratch);
	SflMemState.scratch = NULL;
}

uint8 *SflMemMap(uint32 guest, uint32 size)
{
	uint32 numpages;
	uint8 *block;
	SFLMEMREGION *region;
	WSASSERT((guest & (SFLMEMPAGESIZE - 1)) == 0);
	WSASSERT(size != 0);
	if (SflMemState.numregions == SFLMEMMAXREGIONS)
		return NULL;
	numpages = SflMemPageCount(size);
	block = WSALLOC(uint8, ((size_t)numpages << SFLMEMPAGESHIFT) + SFLMEMSLACK);
	if (block == NULL)
		return NULL;
	WSMemoryClear(block, ((size_t)numpages << SFLMEMPAGESHIFT) + SFLMEMSLACK);
	region = &SflMemState.regions[SflMemState.numregions];
	region->guest = guest;
	region->size = numpages << SFLMEMPAGESHIFT;
	region->host = block;
	region->block = block;
	SflMemState.numregions++;
	SflMemSetPages(guest, numpages, block);
	return block;
}

bool SflMemAlias(uint32 guest, uint32 existingguest, uint32 size)
{
	uint32 numpages;
	SFLMEMREGION *region;
	WSASSERT((guest & (SFLMEMPAGESIZE - 1)) == 0);
	WSASSERT((existingguest & (SFLMEMPAGESIZE - 1)) == 0);
	if (SflMemState.numregions == SFLMEMMAXREGIONS)
		return false;
	numpages = SflMemPageCount(size);
	region = &SflMemState.regions[SflMemState.numregions];
	region->guest = guest;
	region->size = numpages << SFLMEMPAGESHIFT;
	region->host = (uint8 *)(SflPageDelta[existingguest >> SFLMEMPAGESHIFT] + existingguest);
	region->block = NULL;
	SflMemState.numregions++;
	SflMemSetPages(guest, numpages, region->host);
	return true;
}

void SflMemUnmap(uint32 guest, uint32 size)
{
	size_t index;
	SFLMEMREGION *region;
	for (index = 0; index < SflMemState.numregions; index++) {
		region = &SflMemState.regions[index];
		if (region->guest != guest)
			continue;
		SflMemSetScratch(guest, SflMemPageCount(size));
		if (region->block != NULL)
			WSFREE(region->block);
		SflMemState.numregions--;
		*region = SflMemState.regions[SflMemState.numregions];
		return;
	}
}

uint32 SflMemHostToGuest(uint8 *addr)
{
	size_t index;
	SFLMEMREGION *region;
	for (index = 0; index < SflMemState.numregions; index++) {
		region = &SflMemState.regions[index];
		if (addr >= region->host && addr < region->host + region->size)
			return region->guest + (uint32)(addr - region->host);
	}
	return 0;
}

bool SflMemIsMapped(uint8 *addr)
{
	size_t index;
	SFLMEMREGION *region;
	for (index = 0; index < SflMemState.numregions; index++) {
		region = &SflMemState.regions[index];
		if (addr >= region->host && addr < region->host + region->size)
			return true;
	}
	return false;
}

void *vm_acquire(size_t size, int options)
{
	void *block;
	block = WSALLOC(uint8, size);
	if (block == NULL)
		return VM_MAP_FAILED;
	WSMemoryClear(block, size);
	return block;
}

int vm_acquire_fixed(void *addr, size_t size, int options)
{
	if (!SflMemIsMapped((uint8 *)addr))
		return -1;
	WSMemoryClear(addr, size);
	return 0;
}

int vm_release(void *addr, size_t size)
{
	if (SflMemIsMapped((uint8 *)addr))
		return 0;
	WSFREE(addr);
	return 0;
}

int vm_protect(void *addr, size_t size, int prot)
{
	return -1;
}
