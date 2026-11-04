/*
 *  sflmonitor.h - Emulator monitor server of the SFL platform layer
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

#ifndef SFLMONITOR_H
#define SFLMONITOR_H

#include "server.h"
#include "thread.h"

#define SFLMONITORPORT 19840

typedef struct SFLMONITOR
{
	WSSOCKETLIBRARY library;
	WSSOCKET listener;
	WSTHREAD thread;
	WSSIGNAL exited;
	wsatomicint_t cancel;
	int port;
	int hostmousex;
	int hostmousey;
	wsbool librarycreated;
	wsbool listening;
	wsbool threadcreated;
} SFLMONITOR;

wsbool SflMonitor_Create(SFLMONITOR *pThis, int port);
void SflMonitor_Destroy(SFLMONITOR *pThis);

#endif
