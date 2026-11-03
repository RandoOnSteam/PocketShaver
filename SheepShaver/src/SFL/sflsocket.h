/*
 *  sflsocket.h - BSD socket calls used by slirp, implemented over server.h
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

#ifndef SFLSOCKET_H
#define SFLSOCKET_H

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#if defined(_WIN32)
#include <sys/timeb.h>
#else
#include <sys/types.h>
#include <sys/time.h>
#endif

#define SFLSOCKMAXFDS 256
#define SFLSOCKAFINET 2
#define SFLSOCKSTREAM 1
#define SFLSOCKDGRAM 2
#define SFLSOCKSOLSOCKET 0xffff
#define SFLSOCKREUSEADDR 0x0004
#define SFLSOCKKEEPALIVE 0x0008
#define SFLSOCKOOBINLINE 0x0100
#define SFLSOCKIPPROTOICMP 1
#define SFLSOCKIPPROTOTCP 6
#define SFLSOCKIPPROTOUDP 17
#define SFLSOCKTCPNODELAY 1
#define SFLSOCKMSGOOB 1
#define SFLSOCKMSGPEEK 2
#define SFLSOCKFIONREAD 1
#define SFLSOCKFIONBIO 2
#define SFLSOCKANY 0x00000000UL
#define SFLSOCKLOOPBACK 0x7f000001UL
#define SFLSOCKNONE 0xffffffffUL
#define SFLSOCKNAMESIZE 256

struct SflSockInAddr {
	unsigned int s_addr;
};

struct SflSockAddr {
	unsigned short sa_family;
	char sa_data[14];
};

struct SflSockAddrIn {
	unsigned short sin_family;
	unsigned short sin_port;
	struct SflSockInAddr sin_addr;
	char sin_zero[8];
};

struct SflSockHostEnt {
	char *h_name;
	char **h_aliases;
	short h_addrtype;
	short h_length;
	char **h_addr_list;
};

typedef struct SflSockFdSet {
	unsigned char bits[SFLSOCKMAXFDS / 8];
} SflSockFdSet;

#ifdef __cplusplus
extern "C" {
#endif

int SflSockSocket(int family, int type, int protocol);
int SflSockBind(int fd, const struct SflSockAddr *addr, int length);
int SflSockListen(int fd, int backlog);
int SflSockAccept(int fd, struct SflSockAddr *addr, int *length);
int SflSockConnect(int fd, const struct SflSockAddr *addr, int length);
int SflSockSend(int fd, const void *data, int length, int flags);
int SflSockRecv(int fd, void *data, int length, int flags);
int SflSockSendTo(int fd, const void *data, int length, int flags, const struct SflSockAddr *addr, int addrlength);
int SflSockRecvFrom(int fd, void *data, int length, int flags, struct SflSockAddr *addr, int *addrlength);
int SflSockSetSockOpt(int fd, int level, int name, const void *value, int length);
int SflSockGetSockName(int fd, struct SflSockAddr *addr, int *length);
int SflSockShutdown(int fd, int how);
int SflSockClose(int fd);
int SflSockIoctl(int fd, long request, void *arg);
int SflSockSelect(int count, SflSockFdSet *readset, SflSockFdSet *writeset, SflSockFdSet *exceptset, long timeoutms);
int SflSockGetHostName(char *name, int size);
struct SflSockHostEnt *SflSockGetHostByName(const char *name);
int SflSockInetAton(const char *text, struct SflSockInAddr *addr);
unsigned int SflSockInetAddr(const char *text);
char *SflSockInetNtoa(struct SflSockInAddr addr);
int SflSockGetDnsServer(struct SflSockInAddr *addr);
unsigned short SflSockSwap16(unsigned short value);
unsigned int SflSockSwap32(unsigned int value);

#ifdef __cplusplus
}
#endif

#if !defined(SFLSOCKET_IMPLEMENTATION)

#if defined(_WIN32)
#include <stdint.h>
struct timeval {
	long tv_sec;
	long tv_usec;
};
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef char *caddr_t;
#endif

typedef int ioctlsockopt_t;
#define WSAGetLastError() (int)(errno)
#define WSASetLastError(e) (void)(errno = (e))
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAENOTCONN ENOTCONN
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAENETUNREACH ENETUNREACH
#define WSAECONNREFUSED ECONNREFUSED
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define HAVE_INET_ATON 1
#define NO_UNIX_SOCKETS 1
#define USE_FIONBIO 1

#define in_addr SflSockInAddr
#define sockaddr SflSockAddr
#define sockaddr_in SflSockAddrIn
#define hostent SflSockHostEnt
#define h_addr h_addr_list[0]
#define socklen_t int
#undef fd_set
#define fd_set SflSockFdSet
#undef FD_SETSIZE
#define FD_SETSIZE SFLSOCKMAXFDS
#undef FD_ZERO
#undef FD_SET
#undef FD_CLR
#undef FD_ISSET
#define FD_ZERO(set) memset((set), 0, sizeof(*(set)))
#define FD_SET(fd, set) ((set)->bits[(fd) >> 3] |= (unsigned char)(1 << ((fd) & 7)))
#define FD_CLR(fd, set) ((set)->bits[(fd) >> 3] &= (unsigned char)~(1 << ((fd) & 7)))
#define FD_ISSET(fd, set) (((set)->bits[(fd) >> 3] >> ((fd) & 7)) & 1)

#undef AF_INET
#undef PF_INET
#undef SOCK_STREAM
#undef SOCK_DGRAM
#undef SOL_SOCKET
#undef SO_REUSEADDR
#undef SO_KEEPALIVE
#undef SO_OOBINLINE
#undef IPPROTO_ICMP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#undef TCP_NODELAY
#undef MSG_OOB
#undef MSG_PEEK
#undef FIONREAD
#undef FIONBIO
#undef INADDR_ANY
#undef INADDR_LOOPBACK
#undef INADDR_NONE
#define AF_INET SFLSOCKAFINET
#define PF_INET SFLSOCKAFINET
#define SOCK_STREAM SFLSOCKSTREAM
#define SOCK_DGRAM SFLSOCKDGRAM
#define SOL_SOCKET SFLSOCKSOLSOCKET
#define SO_REUSEADDR SFLSOCKREUSEADDR
#define SO_KEEPALIVE SFLSOCKKEEPALIVE
#define SO_OOBINLINE SFLSOCKOOBINLINE
#define IPPROTO_ICMP SFLSOCKIPPROTOICMP
#define IPPROTO_TCP SFLSOCKIPPROTOTCP
#define IPPROTO_UDP SFLSOCKIPPROTOUDP
#define TCP_NODELAY SFLSOCKTCPNODELAY
#define MSG_OOB SFLSOCKMSGOOB
#define MSG_PEEK SFLSOCKMSGPEEK
#define FIONREAD SFLSOCKFIONREAD
#define FIONBIO SFLSOCKFIONBIO
#define INADDR_ANY SFLSOCKANY
#define INADDR_LOOPBACK SFLSOCKLOOPBACK
#define INADDR_NONE SFLSOCKNONE

#undef htons
#undef ntohs
#undef htonl
#undef ntohl
#if defined(WORDS_BIGENDIAN)
#define htons(x) ((unsigned short)(x))
#define ntohs(x) ((unsigned short)(x))
#define htonl(x) ((unsigned int)(x))
#define ntohl(x) ((unsigned int)(x))
#else
#define htons(x) SflSockSwap16((unsigned short)(x))
#define ntohs(x) SflSockSwap16((unsigned short)(x))
#define htonl(x) SflSockSwap32((unsigned int)(x))
#define ntohl(x) SflSockSwap32((unsigned int)(x))
#endif

#undef ioctlsocket
#undef closesocket
#define socket SflSockSocket
#define bind SflSockBind
#define listen SflSockListen
#define accept SflSockAccept
#define connect SflSockConnect
#define send SflSockSend
#define recv SflSockRecv
#define sendto SflSockSendTo
#define recvfrom SflSockRecvFrom
#define setsockopt SflSockSetSockOpt
#define getsockname SflSockGetSockName
#define shutdown SflSockShutdown
#define closesocket SflSockClose
#define ioctlsocket SflSockIoctl
static long SflSockTimeoutMs(const struct timeval *timeout)
{
	if (timeout == NULL)
		return -1;
	return (long)timeout->tv_sec * 1000 + (long)timeout->tv_usec / 1000;
}
#define select(count, readset, writeset, exceptset, timeout) SflSockSelect((count), (readset), (writeset), (exceptset), SflSockTimeoutMs(timeout))
#define gethostname SflSockGetHostName
#define gethostbyname SflSockGetHostByName
#define inet_aton SflSockInetAton
#define inet_addr SflSockInetAddr
#define inet_ntoa SflSockInetNtoa

#endif

#endif
