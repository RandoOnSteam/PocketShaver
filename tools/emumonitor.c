#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
typedef SOCKET MonitorSocket;
#define MONITOR_INVALID_SOCKET INVALID_SOCKET
#define MONITOR_PATH_SEPARATOR "\\"
#define MakeDirectory(PATH) _mkdir(PATH)
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int MonitorSocket;
#define MONITOR_INVALID_SOCKET (-1)
#define MONITOR_PATH_SEPARATOR "/"
#define MakeDirectory(PATH) mkdir(PATH, 0777)
#endif

#define MONITOR_PATH_SIZE 1024
#define MACBINARY_HEADER_SIZE 128

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

static int ExchangeMonitor(const char *hostname, const char *port,
	const char *command, int commandlength, char *response, int responsesize)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *current;
	MonitorSocket monitorsocket;
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
	while (used < responsesize - 1) {
		received = recv(monitorsocket, response + used,
			responsesize - used - 1, 0);
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
	if (strstr(response, "\"ok\":false") != NULL)
		return 1;
	return 0;
}

static int RunMonitor(const char *hostname, const char *port,
	const char *command, int commandlength)
{
	char response[4096];
	size_t length;
	int result;

	result = ExchangeMonitor(hostname, port, command, commandlength,
		response, (int)sizeof(response));
	length = strlen(response);
	if (length == 0)
		return result;
	fputs(response, stdout);
	if (response[length - 1] != '\n')
		fputc('\n', stdout);
	return result;
}

static int QueryExtFSRoot(const char *hostname, const char *port,
	char *root, int rootsize)
{
	char response[4096];
	const char *source;
	int length;

	if (ExchangeMonitor(hostname, port, "extfs\n", 6, response,
		(int)sizeof(response)) != 0) {
		fprintf(stderr, "Unable to get extfs folder: %s", response);
		return 0;
	}
	source = strstr(response, "\"path\":\"");
	if (source == NULL)
		return 0;
	source += 8;
	length = 0;
	while (*source != 0 && *source != '"' && length < rootsize - 1) {
		if (*source == '\\' && source[1] != 0)
			source++;
		root[length++] = *source++;
	}
	root[length] = 0;
	return length > 0;
}

static unsigned long ReadBigEndian32(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | (unsigned long)bytes[3];
}

static void ConvertToHostName(const unsigned char *macname, int length,
	char *hostname)
{
	static const char hexdigits[] = "0123456789ABCDEF";
	int index;
	int used;

	used = 0;
	for (index = 0; index < length; index++) {
		if (macname[index] < 0x20 || macname[index] >= 0x80 ||
			strchr("/\\:*?\"<>|%", macname[index]) != NULL) {
			hostname[used++] = '%';
			hostname[used++] = hexdigits[macname[index] >> 4];
			hostname[used++] = hexdigits[macname[index] & 15];
		} else {
			hostname[used++] = (char)macname[index];
		}
	}
	hostname[used] = 0;
}

static unsigned char *ReadWholeFile(const char *path, long *size)
{
	FILE *input;
	unsigned char *contents;

	input = fopen(path, "rb");
	if (input == NULL)
		return NULL;
	contents = NULL;
	if (fseek(input, 0, SEEK_END) == 0) {
		*size = ftell(input);
		if (*size > 0 && fseek(input, 0, SEEK_SET) == 0) {
			contents = (unsigned char *)malloc((size_t)*size);
			if (contents != NULL &&
				fread(contents, 1, (size_t)*size, input) != (size_t)*size) {
				free(contents);
				contents = NULL;
			}
		}
	}
	fclose(input);
	return contents;
}

static int WriteWholeFile(const char *directory, const char *subdirectory,
	const char *name, const unsigned char *data, unsigned long size)
{
	char path[MONITOR_PATH_SIZE];
	FILE *output;
	int written;

	if (subdirectory[0] != 0) {
		sprintf(path, "%s" MONITOR_PATH_SEPARATOR "%s", directory, subdirectory);
		MakeDirectory(path);
		sprintf(path, "%s" MONITOR_PATH_SEPARATOR "%s" MONITOR_PATH_SEPARATOR "%s",
			directory, subdirectory, name);
	} else {
		sprintf(path, "%s" MONITOR_PATH_SEPARATOR "%s", directory, name);
	}
	output = fopen(path, "wb");
	if (output == NULL) {
		fprintf(stderr, "Unable to write %s\n", path);
		return 0;
	}
	written = fwrite(data, 1, (size_t)size, output) == (size_t)size;
	if (fclose(output) != 0)
		written = 0;
	if (!written)
		fprintf(stderr, "Unable to write %s\n", path);
	return written;
}

static int IsRemoteSource(const char *source)
{
	struct stat sourcestat;
	const char *colon;

	if (stat(source, &sourcestat) == 0)
		return 0;
	colon = strchr(source, ':');
	return colon != NULL && colon - source > 1;
}

static int FetchRemoteSource(const char *source, char *localpath)
{
	char command[MONITOR_PATH_SIZE * 2 + 32];
	const char *temporary;

	temporary = getenv("TEMP");
	if (temporary == NULL)
		temporary = getenv("TMPDIR");
	if (temporary == NULL)
		temporary = "/tmp";
	sprintf(localpath, "%s" MONITOR_PATH_SEPARATOR "emumonitor-deploy.bin",
		temporary);
	sprintf(command, "scp -q \"%s\" \"%s\"", source, localpath);
	if (system(command) != 0) {
		fprintf(stderr, "Unable to fetch %s\n", source);
		return 0;
	}
	return 1;
}

static int Deploy(const char *hostname, const char *port, const char *source,
	const char *destination)
{
	char root[MONITOR_PATH_SIZE];
	char localpath[MONITOR_PATH_SIZE];
	char name[64 * 3 + 1];
	unsigned char finderinfo[32];
	unsigned char *contents;
	unsigned long datalength;
	unsigned long resourcelength;
	unsigned long resourceoffset;
	unsigned int finderflags;
	long size;
	int deployed;

	if (strlen(source) >= MONITOR_PATH_SIZE - 64 ||
		(destination != NULL && strlen(destination) >= MONITOR_PATH_SIZE - 256)) {
		fprintf(stderr, "Path is too long\n");
		return 2;
	}
	if (destination == NULL) {
		if (!QueryExtFSRoot(hostname, port, root, MONITOR_PATH_SIZE - 256))
			return 2;
		destination = root;
	}
	strcpy(localpath, source);
	if (IsRemoteSource(source) && !FetchRemoteSource(source, localpath))
		return 2;
	size = 0;
	contents = ReadWholeFile(localpath, &size);
	if (contents == NULL) {
		fprintf(stderr, "Unable to read %s\n", localpath);
		return 2;
	}
	if (size < MACBINARY_HEADER_SIZE || contents[0] != 0 || contents[74] != 0 ||
		contents[1] < 1 || contents[1] > 63) {
		fprintf(stderr, "%s is not a MacBinary file\n", source);
		free(contents);
		return 2;
	}
	datalength = ReadBigEndian32(contents + 83);
	resourcelength = ReadBigEndian32(contents + 87);
	resourceoffset = MACBINARY_HEADER_SIZE + ((datalength + 127) & ~127UL);
	if (resourceoffset + resourcelength > (unsigned long)size) {
		fprintf(stderr, "%s is truncated\n", source);
		free(contents);
		return 2;
	}
	ConvertToHostName(contents + 2, contents[1], name);
	finderflags = (((unsigned int)contents[73] << 8) | contents[101]) & 0xfeff;
	memset(finderinfo, 0, sizeof(finderinfo));
	memcpy(finderinfo, contents + 65, 8);
	finderinfo[8] = (unsigned char)(finderflags >> 8);
	finderinfo[9] = (unsigned char)finderflags;
	memset(finderinfo + 10, 0xff, 4);
	deployed = WriteWholeFile(destination, "", name,
			contents + MACBINARY_HEADER_SIZE, datalength) &&
		WriteWholeFile(destination, ".rsrc", name, contents + resourceoffset,
			resourcelength) &&
		WriteWholeFile(destination, ".finf", name, finderinfo,
			sizeof(finderinfo));
	free(contents);
	if (!deployed)
		return 2;
	printf("{\"ok\":true,\"name\":\"%s\",\"data\":%lu,\"rsrc\":%lu}\n", name,
		datalength, resourcelength);
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
			"Usage: emumonitor [--host HOST] [--port PORT] COMMAND [ARG...]\n"
			"       emumonitor [--host HOST] [--port PORT] deploy FILE.bin [FOLDER]\n");
		return 2;
	}
	if (strcmp(argv[argument], "deploy") == 0) {
		if (argc - argument < 2 || argc - argument > 3) {
			fprintf(stderr, "Usage: emumonitor deploy FILE.bin [FOLDER]\n");
			return 2;
		}
		if (argc - argument == 3)
			return Deploy(hostname, port, argv[argument + 1], argv[argument + 2]);
		return Deploy(hostname, port, argv[argument + 1], NULL);
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
