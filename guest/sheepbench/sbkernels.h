/*
 *  sbkernels.h - SheepBench kernels shared by the guest app and the host
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

#ifndef SBKERNELS_H
#define SBKERNELS_H

#include <stddef.h>
#include <stdint.h>

#define SBKERNELINT 1
#define SBKERNELBYTECOPY 2
#define SBKERNELCOPY 3
#define SBKERNELFILL 4
#define SBKERNELCHASE 5
#define SBKERNELDOUBLE 6
#define SBKERNELCALL 7
#define SBKERNELFLOAT 8
#define SBKERNELCOUNT 9

#ifdef __cplusplus
extern "C" {
#endif

const char *SbName(uint32_t kernel);
size_t SbScratchBytes(uint32_t kernel, uint32_t param);
void SbPrepare(uint32_t kernel, uint32_t param, void *scratch);
uint32_t SbRun(uint32_t kernel, uint32_t iterations, uint32_t param, void *scratch);

#ifdef __cplusplus
}
#endif

#endif
