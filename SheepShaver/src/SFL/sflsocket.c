/*
 *  sflsocket.c - BSD socket calls used by slirp, implemented over server.h
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "gui.h"
#include "server.h"
#include "thread.h"
#define SFLSOCKET_IMPLEMENTATION
#include "sflsocket.h"

#define SFLSOCKFIRSTFD 3
#define SFLSOCKSELECTSLEEPMS 1
#define SFLSOCKADDRLIST 2

typedef struct SFLSOCKENTRY
{
	WSSOCKET socket;
	WSSOCKADDRv4 bound;
	int type;
	wsbool used;
	wsbool created;
	wsbool hasbound;
	wsbool listening;
	wsbool connecting;
	wsbool nonblocking;
	wsbool failed;
	wssocketerrorkind_t errkind;
} SFLSOCKENTRY;

typedef struct SFLSOCKSTATE
{
	WSSOCKETLIBRARY library;
	SFLSOCKENTRY entries[SFLSOCKMAXFDS];
	struct SflSockHostEnt host;
	struct SflSockInAddr hostaddress;
	char *hostlist[SFLSOCKADDRLIST];
	char hostname[SFLSOCKNAMESIZE];
	char ntoa[16];
	wsbool librarycreated;
} SFLSOCKSTATE;

static SFLSOCKSTATE SflSockState;

unsigned short SflSockSwap16(unsigned short value)
{
	return (unsigned short)(((value & 0xff) << 8) | ((value >> 8) & 0xff));
}

unsigned int SflSockSwap32(unsigned int value)
{
	return ((value & 0xff) << 24) | ((value & 0xff00) << 8) | ((value >> 8) & 0xff00) | ((value >> 24) & 0xff);
}

static unsigned int SflSockNetToHost32(unsigned int value)
{
	const unsigned char *bytes;
	bytes = (const unsigned char *)&value;
	return ((unsigned int)bytes[0] << 24) | ((unsigned int)bytes[1] << 16) | ((unsigned int)bytes[2] << 8) | bytes[3];
}

static unsigned int SflSockHostToNet32(unsigned int value)
{
	unsigned int result;
	unsigned char *bytes;
	bytes = (unsigned char *)&result;
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
	return result;
}

static unsigned short SflSockNetToHost16(unsigned short value)
{
	const unsigned char *bytes;
	bytes = (const unsigned char *)&value;
	return (unsigned short)((bytes[0] << 8) | bytes[1]);
}

static unsigned short SflSockHostToNet16(unsigned short value)
{
	unsigned short result;
	unsigned char *bytes;
	bytes = (unsigned char *)&result;
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
	return result;
}

static int SflSockFail(int error)
{
	errno = error;
	return -1;
}

static int SflSockErrno(wssocketerrorkind_t errkind)
{
	switch (errkind) {
	case WSSOCKET_ERRORKIND_WOULDBLOCK:
		return EWOULDBLOCK;
	case WSSOCKET_ERRORKIND_INPROGRESS:
		return EINPROGRESS;
	case WSSOCKET_ERRORKIND_CONNREFUSED:
		return ECONNREFUSED;
	case WSSOCKET_ERRORKIND_NOTCONN:
		return ENOTCONN;
	case WSSOCKET_ERRORKIND_HOSTUNREACH:
		return EHOSTUNREACH;
	case WSSOCKET_ERRORKIND_NETUNREACH:
		return ENETUNREACH;
	case WSSOCKET_ERRORKIND_CONNRESET:
		return ECONNRESET;
	default:
		return EIO;
	}
}

static int SflSockEntryErrno(const SFLSOCKENTRY *entry)
{
	return SflSockErrno(entry->errkind);
}

static wsbool SflSockLibrary(void)
{
	if (!SflSockState.librarycreated)
		SflSockState.librarycreated = WSSocketLibrary_Create(&SflSockState.library);
	return SflSockState.librarycreated;
}

static SFLSOCKENTRY *SflSockEntry(int fd)
{
	if (fd < 0 || fd >= SFLSOCKMAXFDS || !SflSockState.entries[fd].used)
		return NULL;
	return &SflSockState.entries[fd];
}

static void SflSockToAddr(const struct SflSockAddr *addr, WSSOCKADDRv4 *result)
{
	const struct SflSockAddrIn *in;
	in = (const struct SflSockAddrIn *)addr;
	WSSockAddrv4_Construct(result, SflSockNetToHost32(in->sin_addr.s_addr), SflSockNetToHost16(in->sin_port));
}

static void SflSockFromAddr(const WSSOCKADDRv4 *addr, struct SflSockAddr *result, int *length)
{
	struct SflSockAddrIn *in;
	in = (struct SflSockAddrIn *)result;
	memset(in, 0, sizeof(*in));
	in->sin_family = SFLSOCKAFINET;
	in->sin_port = SflSockHostToNet16(WSSockAddrv4_GetPort(addr));
	in->sin_addr.s_addr = SflSockHostToNet32(WSSockAddrv4_GetIP(addr));
	if (length != NULL)
		*length = (int)sizeof(*in);
}

static void SflSockApplyMode(SFLSOCKENTRY *entry)
{
	WSSocket_SetNonBlockingWithVal(&entry->socket, entry->nonblocking);
}

static wsbool SflSockCreateDatagram(SFLSOCKENTRY *entry)
{
	WSSOCKADDRv4 any;
	if (entry->created)
		return TRUE;
	if (!entry->hasbound) {
		WSSockAddrv4_Construct(&any, WSSOCKADDRv4_ANY, 0);
		entry->bound = any;
	}
	if (!WSSocket_CreateEndpointWithAddr(&entry->socket, &entry->bound, 0, &SflSockState.library, &entry->errkind))
		return FALSE;
	entry->created = TRUE;
	SflSockApplyMode(entry);
	return TRUE;
}

int SflSockSocket(int family, int type, int protocol)
{
	int fd;
	SFLSOCKENTRY *entry;
	if (family != SFLSOCKAFINET || (type != SFLSOCKSTREAM && type != SFLSOCKDGRAM))
		return SflSockFail(EINVAL);
	if (!SflSockLibrary())
		return SflSockFail(EIO);
	for (fd = SFLSOCKFIRSTFD; fd < SFLSOCKMAXFDS; fd++) {
		entry = &SflSockState.entries[fd];
		if (entry->used)
			continue;
		memset(entry, 0, sizeof(*entry));
		WSSocket_Construct(&entry->socket);
		entry->used = TRUE;
		entry->type = type;
		return fd;
	}
	return SflSockFail(EMFILE);
}

int SflSockBind(int fd, const struct SflSockAddr *addr, int length)
{
	SFLSOCKENTRY *entry;
	entry = SflSockEntry(fd);
	if (entry == NULL || entry->created)
		return SflSockFail(EBADF);
	SflSockToAddr(addr, &entry->bound);
	entry->hasbound = TRUE;
	if (entry->type == SFLSOCKDGRAM && !SflSockCreateDatagram(entry))
		return SflSockFail(SflSockEntryErrno(entry));
	return 0;
}

int SflSockListen(int fd, int backlog)
{
	SFLSOCKENTRY *entry;
	entry = SflSockEntry(fd);
	if (entry == NULL || entry->type != SFLSOCKSTREAM || entry->created)
		return SflSockFail(EBADF);
	if (!WSSocket_CreateServerWithAddr(&entry->socket, &entry->bound, &SflSockState.library))
		return SflSockFail(SflSockEntryErrno(entry));
	entry->created = TRUE;
	entry->listening = TRUE;
	SflSockApplyMode(entry);
	return 0;
}

int SflSockAccept(int fd, struct SflSockAddr *addr, int *length)
{
	SFLSOCKENTRY *entry;
	SFLSOCKENTRY *accepted;
	WSSOCKADDRv4 peer;
	wsbool block;
	int newfd;
	entry = SflSockEntry(fd);
	if (entry == NULL || !entry->listening)
		return SflSockFail(EBADF);
	newfd = SflSockSocket(SFLSOCKAFINET, SFLSOCKSTREAM, 0);
	if (newfd < 0)
		return -1;
	accepted = &SflSockState.entries[newfd];
	block = WSSOCKET_NONBLOCKING;
	if (!WSSocket_CreateAccepted(&accepted->socket, &entry->socket, &block, &SflSockState.library)) {
		accepted->used = FALSE;
		if (block)
			return SflSockFail(EWOULDBLOCK);
		return SflSockFail(SflSockEntryErrno(accepted));
	}
	accepted->created = TRUE;
	if (addr != NULL && WSSocket_GetPeerAddr(&accepted->socket, &peer))
		SflSockFromAddr(&peer, addr, length);
	return newfd;
}

int SflSockConnect(int fd, const struct SflSockAddr *addr, int length)
{
	SFLSOCKENTRY *entry;
	WSSOCKADDRv4 target;
	wsbool block;
	entry = SflSockEntry(fd);
	if (entry == NULL || entry->type != SFLSOCKSTREAM || entry->created)
		return SflSockFail(EBADF);
	SflSockToAddr(addr, &target);
	block = WSSOCKET_BLOCKING;
	if (entry->nonblocking)
		block = WSSOCKET_NONBLOCKING;
	if (!WSSocket_CreateClientWithAddr(&entry->socket, &target, &block, &SflSockState.library, &entry->errkind))
		return SflSockFail(SflSockEntryErrno(entry));
	entry->created = TRUE;
	if (block) {
		entry->connecting = TRUE;
		return SflSockFail(EINPROGRESS);
	}
	return 0;
}

static wsbool SflSockConnected(SFLSOCKENTRY *entry)
{
	int state;
	if (!entry->connecting)
		return TRUE;
	state = WSSocket_PollConnect(&entry->socket, &entry->errkind);
	if (state > 0) {
		entry->connecting = FALSE;
		return TRUE;
	}
	if (state < 0) {
		entry->connecting = FALSE;
		entry->failed = TRUE;
	}
	return FALSE;
}

int SflSockSend(int fd, const void *data, int length, int flags)
{
	SFLSOCKENTRY *entry;
	wsbool block;
	size_t sent;
	entry = SflSockEntry(fd);
	if (entry == NULL || !entry->created)
		return SflSockFail(ENOTCONN);
	if (entry->failed)
		return SflSockFail(ECONNREFUSED);
	if (!SflSockConnected(entry)) {
		if (entry->failed)
			return SflSockFail(ECONNREFUSED);
		return SflSockFail(EWOULDBLOCK);
	}
	if (length == 0)
		return 0;
	block = WSSOCKET_BLOCKING;
	if (entry->nonblocking)
		block = WSSOCKET_NONBLOCKING;
	sent = WSSocket_Write(&entry->socket, data, (size_t)length, &block);
	if (sent == (size_t)-1 || sent == 0) {
		if (block)
			return SflSockFail(EWOULDBLOCK);
		return SflSockFail(SflSockEntryErrno(entry));
	}
	return (int)sent;
}

int SflSockRecv(int fd, void *data, int length, int flags)
{
	SFLSOCKENTRY *entry;
	wsbool block;
	size_t received;
	entry = SflSockEntry(fd);
	if (entry == NULL || !entry->created)
		return SflSockFail(ENOTCONN);
	if (entry->failed)
		return SflSockFail(ECONNREFUSED);
	if (length == 0)
		return 0;
	block = WSSOCKET_BLOCKING;
	if (entry->nonblocking)
		block = WSSOCKET_NONBLOCKING;
	if (flags & SFLSOCKMSGPEEK)
		received = WSSocket_Peek(&entry->socket, data, (size_t)length, &block);
	else
		received = WSSocket_Read(&entry->socket, data, (size_t)length, &block);
	if (received == (size_t)-1) {
		if (block)
			return SflSockFail(EWOULDBLOCK);
		return SflSockFail(SflSockEntryErrno(entry));
	}
	return (int)received;
}

int SflSockSendTo(int fd, const void *data, int length, int flags, const struct SflSockAddr *addr, int addrlength)
{
	SFLSOCKENTRY *entry;
	WSSOCKADDRv4 target;
	size_t sent;
	entry = SflSockEntry(fd);
	if (entry == NULL || entry->type != SFLSOCKDGRAM)
		return SflSockFail(EBADF);
	if (!SflSockCreateDatagram(entry))
		return SflSockFail(SflSockEntryErrno(entry));
	SflSockToAddr(addr, &target);
	sent = WSSocket_WriteEndpointWithAddr(&entry->socket, data, (size_t)length, &target, NULL, &entry->errkind);
	if (sent == (size_t)-1)
		return SflSockFail(SflSockEntryErrno(entry));
	return (int)sent;
}

int SflSockRecvFrom(int fd, void *data, int length, int flags, struct SflSockAddr *addr, int *addrlength)
{
	SFLSOCKENTRY *entry;
	WSSOCKADDRv4 from;
	size_t received;
	entry = SflSockEntry(fd);
	if (entry == NULL || !entry->created || entry->type != SFLSOCKDGRAM)
		return SflSockFail(EBADF);
	received = WSSocket_ReadEndpointWithAddr(&entry->socket, data, (size_t)length, &from, NULL, &entry->errkind);
	if (received == (size_t)-1)
		return SflSockFail(SflSockEntryErrno(entry));
	if (addr != NULL)
		SflSockFromAddr(&from, addr, addrlength);
	return (int)received;
}

int SflSockSetSockOpt(int fd, int level, int name, const void *value, int length)
{
	if (SflSockEntry(fd) == NULL)
		return SflSockFail(EBADF);
	return 0;
}

int SflSockGetSockName(int fd, struct SflSockAddr *addr, int *length)
{
	SFLSOCKENTRY *entry;
	WSSOCKADDRv4 local;
	wssocketerrorkind_t errkind;
	entry = SflSockEntry(fd);
	if (entry == NULL)
		return SflSockFail(EBADF);
	if (!entry->created) {
		SflSockFromAddr(&entry->bound, addr, length);
		return 0;
	}

	errkind = WSSocket_GetLocalAddr(&entry->socket, &local);
	if (errkind != WSSOCKET_ERRORKIND_NONE)
		return SflSockFail(SflSockErrno(errkind));
	SflSockFromAddr(&local, addr, length);
	return 0;
}

int SflSockShutdown(int fd, int how)
{
	SFLSOCKENTRY *entry;
	entry = SflSockEntry(fd);
	if (entry == NULL || !entry->created)
		return SflSockFail(ENOTCONN);
	if (how == 0)
		WSSocket_ShutdownWithHow(&entry->socket, WSSOCKETSHUTDOWN_READ);
	else
		WSSocket_ShutdownWithHow(&entry->socket, WSSOCKETSHUTDOWN_WRITE);
	return 0;
}

int SflSockClose(int fd)
{
	SFLSOCKENTRY *entry;
	entry = SflSockEntry(fd);
	if (entry == NULL)
		return SflSockFail(EBADF);
	if (entry->created)
		WSSocket_Destroy(&entry->socket);
	entry->used = FALSE;
	entry->created = FALSE;
	return 0;
}

int SflSockIoctl(int fd, long request, void *arg)
{
	SFLSOCKENTRY *entry;
	entry = SflSockEntry(fd);
	if (entry == NULL)
		return SflSockFail(EBADF);
	if (request == SFLSOCKFIONBIO) {
		entry->nonblocking = *(int *)arg != 0;
		if (entry->created)
			SflSockApplyMode(entry);
		return 0;
	}
	if (request == SFLSOCKFIONREAD) {
		*(int *)arg = 0;
		if (entry->created)
			*(int *)arg = (int)WSSocket_GetReadSize(&entry->socket);
		return 0;
	}
	return SflSockFail(EINVAL);
}

static wsbool SflSockReadable(SFLSOCKENTRY *entry)
{
	if (!entry->created || entry->connecting)
		return FALSE;
	if (entry->failed)
		return TRUE;
	return WSSocket_WaitForRead(&entry->socket, 0, NULL);
}

static wsbool SflSockWritable(SFLSOCKENTRY *entry)
{
	if (!entry->created)
		return FALSE;
	if (entry->connecting)
		return SflSockConnected(entry) || entry->failed;
	if (entry->failed)
		return TRUE;
	return WSSocket_WaitForWrite(&entry->socket, 0, NULL);
}

static int SflSockScan(int count, SflSockFdSet *readset, SflSockFdSet *writeset, SflSockFdSet *exceptset,
	SflSockFdSet *readyread, SflSockFdSet *readywrite)
{
	int fd;
	int ready;
	SFLSOCKENTRY *entry;
	ready = 0;
	memset(readyread, 0, sizeof(*readyread));
	memset(readywrite, 0, sizeof(*readywrite));
	for (fd = 0; fd < count && fd < SFLSOCKMAXFDS; fd++) {
		entry = SflSockEntry(fd);
		if (entry == NULL)
			continue;
		if (readset != NULL && ((readset->bits[fd >> 3] >> (fd & 7)) & 1) && SflSockReadable(entry)) {
			readyread->bits[fd >> 3] |= (unsigned char)(1 << (fd & 7));
			ready++;
		}
		if (writeset != NULL && ((writeset->bits[fd >> 3] >> (fd & 7)) & 1) && SflSockWritable(entry)) {
			readywrite->bits[fd >> 3] |= (unsigned char)(1 << (fd & 7));
			ready++;
		}
	}
	return ready;
}

int SflSockSelect(int count, SflSockFdSet *readset, SflSockFdSet *writeset, SflSockFdSet *exceptset, long timeoutms)
{
	SflSockFdSet readyread;
	SflSockFdSet readywrite;
	long waitms;
	long waited;
	int ready;
	waitms = timeoutms;
	waited = 0;
	for (;;) {
		ready = SflSockScan(count, readset, writeset, exceptset, &readyread, &readywrite);
		if (ready != 0 || (waitms >= 0 && waited >= waitms))
			break;
		WSThreadSleep(SFLSOCKSELECTSLEEPMS);
		waited += SFLSOCKSELECTSLEEPMS;
	}
	if (readset != NULL)
		*readset = readyread;
	if (writeset != NULL)
		*writeset = readywrite;
	if (exceptset != NULL)
		memset(exceptset, 0, sizeof(*exceptset));
	return ready;
}

int SflSockGetHostName(char *name, int size)
{
	if (!SflSockLibrary() || !WSSocketGetHostName(name, (size_t)size))
		return SflSockFail(EIO);
	return 0;
}

struct SflSockHostEnt *SflSockGetHostByName(const char *name)
{
	uint32_t ip;
	if (!SflSockLibrary() || !WSSocketResolveHostv4(name, &ip))
		return NULL;
	strncpy(SflSockState.hostname, name, sizeof(SflSockState.hostname) - 1);
	SflSockState.hostname[sizeof(SflSockState.hostname) - 1] = 0;
	SflSockState.hostaddress.s_addr = SflSockHostToNet32(ip);
	SflSockState.hostlist[0] = (char *)&SflSockState.hostaddress;
	SflSockState.hostlist[1] = NULL;
	SflSockState.host.h_name = SflSockState.hostname;
	SflSockState.host.h_aliases = NULL;
	SflSockState.host.h_addrtype = SFLSOCKAFINET;
	SflSockState.host.h_length = (short)sizeof(SflSockState.hostaddress);
	SflSockState.host.h_addr_list = SflSockState.hostlist;
	return &SflSockState.host;
}

int SflSockInetAton(const char *text, struct SflSockInAddr *addr)
{
	unsigned int parts[4];
	unsigned int value;
	int count;
	char extra;
	count = sscanf(text, "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &extra);
	if (count != 4 || parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255)
		return 0;
	value = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
	addr->s_addr = SflSockHostToNet32(value);
	return 1;
}

unsigned int SflSockInetAddr(const char *text)
{
	struct SflSockInAddr addr;
	if (!SflSockInetAton(text, &addr))
		return SFLSOCKNONE;
	return addr.s_addr;
}

char *SflSockInetNtoa(struct SflSockInAddr addr)
{
	unsigned int value;
	value = SflSockNetToHost32(addr.s_addr);
	sprintf(SflSockState.ntoa, "%u.%u.%u.%u", (value >> 24) & 0xff, (value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
	return SflSockState.ntoa;
}

int SflSockGetDnsServer(struct SflSockInAddr *addr)
{
	char *dns;
	int found;
	dns = NULL;
	if (!SflSockLibrary() || !WSSocketAllocDNSServer(&dns) || dns == NULL)
		return -1;
	found = SflSockInetAton(dns, addr);
	WSFREE(dns);
	if (!found)
		return -1;
	return 0;
}
