/*
 *  sbaltivec.c - SheepBench AltiVec loop, compiled with -maltivec
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

#include <Gestalt.h>
#include "bench.h"

#define BENCHGESTALTPPCFEATURES 'ppcf'
#define BENCHGESTALTHASVECTOR 4

typedef __vector float BenchVector;

Boolean BenchHasAltivec(void)
{
	long features;
	features = 0;
	return Gestalt(BENCHGESTALTPPCFEATURES, &features) == noErr && (features & (1L << BENCHGESTALTHASVECTOR)) != 0;
}

void BenchAltivec(float *x, float *y, uint32_t count, uint32_t iterations, const float *half)
{
	BenchVector halves;
	BenchVector source;
	BenchVector target;
	uint32_t iteration;
	uint32_t index;
	unsigned long vrsave;
	__asm__ volatile("lvx %0,0,%1" : "=v"(halves) : "r"(half), "m"(*(const float(*)[4])half));
	__asm__ volatile("mfspr %0,256" : "=r"(vrsave));
	__asm__ volatile("mtspr 256,%0" : : "r"(0xffffffffUL));
	for (iteration = 0; iteration < iterations; iteration++) {
		for (index = 0; index < count; index += 4) {
			target = *(BenchVector *)(y + index);
			source = *(BenchVector *)(x + index);
			*(BenchVector *)(y + index) = __builtin_altivec_vmaddfp(target, halves, source);
		}
	}
	__asm__ volatile("mtspr 256,%0" : : "r"(vrsave));
}
