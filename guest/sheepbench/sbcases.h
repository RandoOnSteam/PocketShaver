/*
 *  sbcases.h - SheepBench kernel cases whose checksums are computed at build time
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

#ifndef SBCASES_H
#define SBCASES_H

#include "sbkernels.h"

typedef struct SbCase {
	uint32_t kernel;
	uint32_t iterations;
	uint32_t param;
} SbCase;

static const SbCase sbcases[] = {
	{SBKERNELINT, 100000, 0},
	{SBKERNELBYTECOPY, 4, 4096},
	{SBKERNELCOMPARE, 4, 2048},
	{SBKERNELDIRECT, 100000, 0},
	{SBKERNELCALL, 100000, 0},
	{SBKERNELATOMIC, 100000, 0},
	{SBKERNELDOUBLE, 100000, 0},
	{SBKERNELDOUBLETHRU, 4, 1024},
	{SBKERNELFLOAT, 100000, 0},
	{SBKERNELVEC, 16, 1024},
	{SBKERNELCOPY, 1, 4096},
	{SBKERNELCOPY, 1, 65536},
	{SBKERNELCOPY, 1, 1048576},
	{SBKERNELCOPY, 1, 16777216},
	{SBKERNELWORDCOPY, 1, 4096},
	{SBKERNELWORDCOPY, 1, 65536},
	{SBKERNELWORDCOPY, 1, 1048576},
	{SBKERNELWORDCOPY, 1, 16777216},
	{SBKERNELFILL, 3, 4096},
	{SBKERNELFILL, 3, 65536},
	{SBKERNELFILL, 3, 1048576},
	{SBKERNELFILL, 3, 16777216},
	{SBKERNELCHASE, 100000, 1024},
	{SBKERNELCHASE, 100000, 16384},
	{SBKERNELCHASE, 100000, 262144},
	{SBKERNELCHASE, 100000, 4194304}
};

#define SBCASECOUNT ((int)(sizeof(sbcases) / sizeof(sbcases[0])))

#endif
