/*
 *  shearsclient.h - guest side of the Sheep Shears mailbox
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

#ifndef SHEARSCLIENT_H
#define SHEARSCLIENT_H

#include <MacTypes.h>
#include <Memory.h>
#include <string.h>

#define SHEARSSHEEPSHAVER 0x42616168UL
#define SHEARSMAGIC 0x53485253UL
#define SHEARSVERSION 1UL
#define SHEARSPAYLOAD 4064
#define SHEARSCMDHELLO 1UL
#define SHEARSCMDPOLL 2UL
#define SHEARSCMDLOG 3UL
#define SHEARSCMDRESULT 5UL
#define SHEARSCMDINFO 7UL
#define SHEARSCMDNATIVE 8UL
#define SHEARSCMDSETJIT 9UL
#define SHEARSCMDREPORT 10UL
#define SHEARSFLAGSHUTDOWN 2UL
#define SHEARSNOREPLY (-1)

typedef unsigned long (*ShearsProc)(void *mailbox);

typedef struct ShearsMailbox {
	unsigned long magic;
	unsigned long version;
	unsigned long seq;
	unsigned long command;
	unsigned long status;
	unsigned long length;
	unsigned long reserved0;
	unsigned long reserved1;
	unsigned char payload[SHEARSPAYLOAD];
} ShearsMailbox;

static ShearsMailbox *ShearsOpen(void)
{
	if (*(volatile unsigned long *)0x2800 != SHEARSSHEEPSHAVER || *(volatile unsigned long *)0x2860 != SHEARSMAGIC ||
		*(volatile unsigned long *)0x2864 == 0)
		return NULL;
	return (ShearsMailbox *)NewPtrSysClear(sizeof(ShearsMailbox));
}

static long ShearsCall(ShearsMailbox *mailbox, unsigned long command, const void *payload, unsigned long length)
{
	ShearsProc proc;
	if (mailbox == NULL || length > SHEARSPAYLOAD)
		return SHEARSNOREPLY;
	mailbox->magic = SHEARSMAGIC;
	mailbox->version = SHEARSVERSION;
	mailbox->seq++;
	mailbox->command = command;
	mailbox->status = 0xffffffffUL;
	mailbox->length = (length + 3) & ~3UL;
	memset(mailbox->payload, 0, mailbox->length);
	if (length)
		memcpy(mailbox->payload, payload, length);
	proc = (ShearsProc)(*(volatile unsigned long *)0x2864);
	if (proc(mailbox) != SHEARSMAGIC)
		return SHEARSNOREPLY;
	return (long)mailbox->status;
}

static unsigned long ShearsWord(ShearsMailbox *mailbox, int index)
{
	if (mailbox->length < (unsigned long)(index + 1) * 4)
		return 0;
	return ((unsigned long *)mailbox->payload)[index];
}

static void ShearsLog(ShearsMailbox *mailbox, const char *text)
{
	ShearsCall(mailbox, SHEARSCMDLOG, text, strlen(text));
}

#endif
