/*
 *  shears.h - Sheep Shears host backend
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

#ifndef SHEARS_H
#define SHEARS_H

#define SHEARSMAGIC 0x53485253u
#define SHEARSMAILBOX 4096
#define SHEARSHEADER 32
#define SHEARSMAXPAYLOAD (SHEARSMAILBOX - SHEARSHEADER)

#define XLM_SHEARS_SIG 0x2860
#define XLM_SHEARS_PROC 0x2864
#define XLM_SHEARS_FLAGS 0x2868
#define XLM_SHEARS_CONTACT 0x286c

#define SHEARSCMDHELLO 1
#define SHEARSCMDPOLL 2
#define SHEARSCMDLOG 3
#define SHEARSCMDRESULT 5
#define SHEARSCMDINFO 7
#define SHEARSCMDNATIVE 8
#define SHEARSCMDSETJIT 9
#define SHEARSCMDREPORT 10

#define SHEARSOK 0
#define SHEARSUNSUPPORTED 1
#define SHEARSBADREQUEST 2
#define SHEARSDISABLED 3

#define SHEARSFLAGSHUTDOWN 2u

void ShearsInit(void);
uint32 ShearsCall(uint32 mailbox);
bool ShearsRequestShutdown(void);

#endif
