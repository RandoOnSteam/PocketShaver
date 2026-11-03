/*
 *  sflimpl.c - Instantiates the single-file libraries for the SFL platform layer
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

#define GUI_IMPLEMENTATION
#include "gui.h"
#define FILE_IMPLEMENTATION
#include "file.h"
#define THREAD_IMPLEMENTATION
#include "thread.h"
#define WSAUDIO_IMPLEMENTATION
#include "audio.h"
#define WSSOCKET_IMPLEMENTATION
#include "server.h"
#define WSJOYSTICK_IMPLEMENTATION
#include "device.h"
#define WSDISPLAY_IMPLEMENTATION
#include "display.h"
#define DRIVE_IMPLEMENTATION
#include "drive.h"
