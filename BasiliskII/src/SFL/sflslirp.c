/*
 *  sflslirp.c - slirp driver of the SFL platform layer
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

#include "slirp.h"
#include "libslirp.h"
#include "sflslirp.h"

#define SFLSLIRPUSECPERMS 1000

typedef struct SFLSLIRP
{
	SFLSLIRPOUTPUT output;
	void *context;
} SFLSLIRP;

static SFLSLIRP SflSlirp;

int SflSlirpInit(SFLSLIRPOUTPUT output, void *context)
{
	SflSlirp.output = output;
	SflSlirp.context = context;
	return slirp_init() >= 0;
}

void SflSlirpInput(const unsigned char *frame, int length)
{
	slirp_input(frame, length);
}

void SflSlirpPoll(int timeoutms)
{
	fd_set readset;
	fd_set writeset;
	fd_set exceptset;
	struct timeval timeout;
	int count;
	count = -1;
	FD_ZERO(&readset);
	FD_ZERO(&writeset);
	FD_ZERO(&exceptset);
	slirp_select_fill(&count, &readset, &writeset, &exceptset);
	timeout.tv_sec = 0;
	timeout.tv_usec = timeoutms * SFLSLIRPUSECPERMS;
	select(count + 1, &readset, &writeset, &exceptset, &timeout);
	slirp_select_poll(&readset, &writeset, &exceptset);
}

void tftp_input(struct mbuf *m)
{
}

int slirp_can_output(void)
{
	return 1;
}

void slirp_output(const uint8 *pkt, int pkt_len)
{
	if (SflSlirp.output != NULL)
		SflSlirp.output(SflSlirp.context, pkt, pkt_len);
}
