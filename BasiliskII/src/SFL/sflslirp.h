/*
 *  sflslirp.h - slirp driver of the SFL platform layer
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

#ifndef SFLSLIRP_H
#define SFLSLIRP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SFLSLIRPOUTPUT)(void *context, const unsigned char *frame, int length);

int SflSlirpInit(SFLSLIRPOUTPUT output, void *context);
void SflSlirpInput(const unsigned char *frame, int length);
void SflSlirpPoll(int timeoutms);

#ifdef __cplusplus
}
#endif

#endif
