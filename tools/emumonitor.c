#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET MonitorSocket;
#define MONITOR_INVALID_SOCKET INVALID_SOCKET
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int MonitorSocket;
#define MONITOR_INVALID_SOCKET (-1)
#endif

static void CloseMonitorSocket(MonitorSocket monitorsocket)
{
#ifdef _WIN32
	closesocket(monitorsocket);
#else
	close(monitorsocket);
#endif
}

static int SendAll(MonitorSocket monitorsocket, const char *data, int length)
{
	int sent;
	int sendflags;
	int totalsent;

	sendflags = 0;
#ifdef MSG_NOSIGNAL
	sendflags = MSG_NOSIGNAL;
#endif
	totalsent = 0;
	while (totalsent < length) {
		sent = send(monitorsocket, data + totalsent, length - totalsent,
			sendflags);
		if (sent <= 0)
			return 0;
		totalsent += sent;
	}
	return 1;
}

static int BuildCommand(int argc, char **argv, int firstargument,
	char *command, int commandsize)
{
	int argument;
	int length;
	int argumentlength;

	length = 0;
	command[0] = 0;
	for (argument = firstargument; argument < argc; argument++) {
		argumentlength = (int)strlen(argv[argument]);
		if (length + argumentlength + 2 >= commandsize)
			return 0;
		if (length > 0)
			command[length++] = ' ';
		memcpy(command + length, argv[argument], argumentlength);
		length += argumentlength;
	}
	command[length++] = '\n';
	command[length] = 0;
	return length;
}

static int RunMonitor(const char *hostname, const char *port,
	const char *command, int commandlength)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *current;
	MonitorSocket monitorsocket;
	char response[4096];
	int received;
	int used;
	int result;
#ifdef _WIN32
	WSADATA winsockdata;
	if (WSAStartup(MAKEWORD(2, 2), &winsockdata) != 0) {
		fprintf(stderr, "Unable to initialize Winsock\n");
		return 2;
	}
#endif
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addresses = NULL;
	result = getaddrinfo(hostname, port, &hints, &addresses);
	if (result != 0) {
		fprintf(stderr, "Unable to resolve monitor host\n");
#ifdef _WIN32
		WSACleanup();
#endif
		return 2;
	}
	monitorsocket = MONITOR_INVALID_SOCKET;
	current = addresses;
	while (current != NULL) {
		monitorsocket = socket(current->ai_family, current->ai_socktype,
			current->ai_protocol);
		if (monitorsocket != MONITOR_INVALID_SOCKET) {
			if (connect(monitorsocket, current->ai_addr,
				(int)current->ai_addrlen) == 0)
				break;
			CloseMonitorSocket(monitorsocket);
			monitorsocket = MONITOR_INVALID_SOCKET;
		}
		current = current->ai_next;
	}
	freeaddrinfo(addresses);
	if (monitorsocket == MONITOR_INVALID_SOCKET) {
		fprintf(stderr, "Unable to connect to %s:%s\n", hostname, port);
#ifdef _WIN32
		WSACleanup();
#endif
		return 2;
	}
	if (!SendAll(monitorsocket, command, commandlength)) {
		fprintf(stderr, "Unable to send monitor command\n");
		CloseMonitorSocket(monitorsocket);
#ifdef _WIN32
		WSACleanup();
#endif
		return 2;
	}
	used = 0;
	while (used < (int)sizeof(response) - 1) {
		received = recv(monitorsocket, response + used,
			(int)sizeof(response) - used - 1, 0);
		if (received <= 0)
			break;
		used += received;
		response[used] = 0;
		if (strchr(response, '\n') != NULL)
			break;
	}
	CloseMonitorSocket(monitorsocket);
#ifdef _WIN32
	WSACleanup();
#endif
	response[used] = 0;
	if (used == 0) {
		fprintf(stderr, "Monitor returned no response\n");
		return 2;
	}
	fputs(response, stdout);
	if (response[used - 1] != '\n')
		fputc('\n', stdout);
	if (strstr(response, "\"ok\":false") != NULL)
		return 1;
	return 0;
}

static int Main(int argc, char **argv)
{
	const char *hostname;
	const char *port;
	char command[4096];
	int argument;
	int commandlength;

	hostname = "127.0.0.1";
	port = "19840";
	argument = 1;
	while (argument < argc) {
		if (strcmp(argv[argument], "--host") == 0) {
			argument++;
			if (argument >= argc) {
				fprintf(stderr, "--host requires a value\n");
				return 2;
			}
			hostname = argv[argument++];
		} else if (strcmp(argv[argument], "--port") == 0) {
			argument++;
			if (argument >= argc) {
				fprintf(stderr, "--port requires a value\n");
				return 2;
			}
			port = argv[argument++];
		} else {
			break;
		}
	}
	if (argument >= argc) {
		fprintf(stderr,
			"Usage: emumonitor [--host HOST] [--port PORT] COMMAND [ARG...]\n");
		return 2;
	}
	commandlength = BuildCommand(argc, argv, argument, command,
		(int)sizeof(command));
	if (commandlength == 0) {
		fprintf(stderr, "Monitor command is too long\n");
		return 2;
	}
	return RunMonitor(hostname, port, command, commandlength);
}

int main(int argc, char **argv)
{
	return Main(argc, argv);
}
