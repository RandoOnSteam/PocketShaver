#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#define _FILE_OFFSET_BITS 64
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
typedef SOCKET MonitorSocket;
typedef __int64 MonitorOffset;
#define MONITOR_INVALID_SOCKET INVALID_SOCKET
#define MONITOR_PATH_SEPARATOR "\\"
#define MakeDirectory(PATH) _mkdir(PATH)
#define SeekFile(FILE, OFFSET) _fseeki64(FILE, OFFSET, SEEK_SET)
#define MONITOR_TCP_TABLE_OWNER_PID_LISTENER 3
#define MONITOR_TCP_STATE_LISTEN 2
typedef struct MonitorTcpRow {
	DWORD state;
	DWORD localaddress;
	DWORD localport;
	DWORD remoteaddress;
	DWORD remoteport;
	DWORD owningpid;
} MonitorTcpRow;
typedef struct MonitorTcpTable {
	DWORD entrycount;
	MonitorTcpRow rows[1];
} MonitorTcpTable;
typedef int (WSAAPI *GetAddrInfoProc)(const char *hostname,
	const char *service, const struct addrinfo *hints,
	struct addrinfo **result);
typedef void (WSAAPI *FreeAddrInfoProc)(struct addrinfo *addresses);
typedef DWORD (WINAPI *GetExtendedTcpTableProc)(PVOID table, PDWORD size,
	BOOL order, ULONG family, int tableclass, ULONG reserved);
typedef DWORD (WINAPI *AllocateAndGetTcpExTableFromStackProc)(PVOID *table,
	BOOL order, HANDLE heap, DWORD flags, DWORD family);
#else
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int MonitorSocket;
typedef off_t MonitorOffset;
#define MONITOR_INVALID_SOCKET (-1)
#define MONITOR_PATH_SEPARATOR "/"
#define MakeDirectory(PATH) mkdir(PATH, 0777)
#define SeekFile(FILE, OFFSET) fseeko(FILE, OFFSET, SEEK_SET)
#endif

#ifndef S_ISREG
#define S_ISREG(MODE) (((MODE) & S_IFMT) == S_IFREG)
#endif

#define MONITOR_PATH_SIZE 1024
#define MACBINARY_HEADER_SIZE 128
#define MONITOR_MAX_VOLUMES 16
#define MONITOR_MAX_DEPTH 64
#define MONITOR_NAME_SIZE 256
#define MONITOR_MACPATH_SIZE 2048
#define MONITOR_FORK_DATA 0x00
#define MONITOR_FORK_RESOURCE 0xFF
#define MONITOR_MAX_EXTENTS 8
#define MONITOR_MAC_EPOCH_OFFSET 2082844800UL
#define HFS_SIGNATURE 0x4244
#define HFSPLUS_SIGNATURE 0x482B
#define HFSX_SIGNATURE 0x4858
#define PARTITION_DRIVER_SIGNATURE 0x4552
#define PARTITION_MAP_SIGNATURE 0x504D
#define DISKCOPY_HEADER_SIZE 84
#define MONITOR_QUIT_TIMEOUT_MS 5000
#define MONITOR_KILL_TIMEOUT_MS 5000
#define MONITOR_PING_TIMEOUT_MS 2000

typedef struct CatalogEntry {
	unsigned long parentid;
	unsigned long id;
	unsigned long nameoffset;
	unsigned long recordoffset;
	int namelength;
	int isfolder;
} CatalogEntry;

typedef struct FolderIndex {
	unsigned long id;
	unsigned long entry;
} FolderIndex;

typedef struct HfsVolume {
	FILE *file;
	MonitorOffset allocationstart;
	unsigned long blocksize;
	int isplus;
	unsigned char *overflow;
	unsigned long overflowsize;
	unsigned char *catalog;
	unsigned long catalogsize;
	unsigned char *names;
	unsigned long namesused;
	unsigned long namescapacity;
	CatalogEntry *entries;
	unsigned long entrycount;
	unsigned long entrycapacity;
	FolderIndex *folders;
	unsigned long foldercount;
	unsigned long rootentry;
} HfsVolume;

typedef struct GuestVolumes {
	HfsVolume volumes[MONITOR_MAX_VOLUMES];
	int volumecount;
	char extfsroot[MONITOR_PATH_SIZE];
	unsigned char extfsname[MONITOR_NAME_SIZE];
	int extfsnamelength;
} GuestVolumes;

typedef struct GuestFile {
	unsigned char name[MONITOR_NAME_SIZE];
	int namelength;
	unsigned char finderinfo[32];
	unsigned char *data;
	unsigned long datalength;
	unsigned char *rsrc;
	unsigned long rsrclength;
	unsigned long created;
	unsigned long modified;
} GuestFile;

typedef struct OverflowSearch {
	int isplus;
	unsigned long fileid;
	unsigned int forktype;
	unsigned long startblock;
	unsigned long *extents;
	int count;
} OverflowSearch;

typedef struct HostSearch {
	const unsigned char *pattern;
	int patternlength;
	const unsigned char *scope;
	int scopelength;
	char hostpath[MONITOR_PATH_SIZE];
	unsigned char macpath[MONITOR_MACPATH_SIZE];
	int macpathlength;
	int depth;
	unsigned long matches;
} HostSearch;

typedef int (*LeafVisitor)(void *context, const unsigned char *record,
	unsigned long available);
typedef void (*HostVisitor)(void *context, const char *name, int isfolder);

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

static void SetReceiveTimeout(MonitorSocket monitorsocket, int timeoutms)
{
#ifdef _WIN32
	DWORD timeout;

	timeout = (DWORD)timeoutms;
	setsockopt(monitorsocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
		sizeof(timeout));
#else
	struct timeval timeout;

	timeout.tv_sec = timeoutms / 1000;
	timeout.tv_usec = (timeoutms % 1000) * 1000;
	setsockopt(monitorsocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
		sizeof(timeout));
#endif
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

static MonitorSocket ConnectAddressList(const struct addrinfo *addresses)
{
	const struct addrinfo *current;
	MonitorSocket monitorsocket;

	for (current = addresses; current != NULL; current = current->ai_next) {
		monitorsocket = socket(current->ai_family, current->ai_socktype,
			current->ai_protocol);
		if (monitorsocket != MONITOR_INVALID_SOCKET) {
			if (connect(monitorsocket, current->ai_addr,
				(int)current->ai_addrlen) == 0)
				return monitorsocket;
			CloseMonitorSocket(monitorsocket);
		}
	}
	return MONITOR_INVALID_SOCKET;
}

#ifdef _WIN32
static MonitorSocket ConnectIpv4(const char *hostname, const char *port)
{
	struct sockaddr_in address;
	struct hostent *host;
	MonitorSocket monitorsocket;
	unsigned long numeric;

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)atoi(port));
	numeric = inet_addr(hostname);
	if (numeric != INADDR_NONE) {
		address.sin_addr.s_addr = numeric;
	} else {
		host = gethostbyname(hostname);
		if (host == NULL || host->h_addrtype != AF_INET ||
			host->h_addr_list[0] == NULL)
			return MONITOR_INVALID_SOCKET;
		memcpy(&address.sin_addr, host->h_addr_list[0], sizeof(address.sin_addr));
	}
	monitorsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (monitorsocket == MONITOR_INVALID_SOCKET)
		return MONITOR_INVALID_SOCKET;
	if (connect(monitorsocket, (struct sockaddr *)&address,
		sizeof(address)) != 0) {
		CloseMonitorSocket(monitorsocket);
		return MONITOR_INVALID_SOCKET;
	}
	return monitorsocket;
}
#endif

static MonitorSocket ConnectMonitor(const char *hostname, const char *port)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	MonitorSocket monitorsocket;
#ifdef _WIN32
	GetAddrInfoProc getaddrinfoproc;
	FreeAddrInfoProc freeaddrinfoproc;
	HMODULE winsock;

	getaddrinfoproc = NULL;
	freeaddrinfoproc = NULL;
	winsock = GetModuleHandleA("ws2_32.dll");
	if (winsock != NULL) {
		getaddrinfoproc = (GetAddrInfoProc)GetProcAddress(winsock, "getaddrinfo");
		freeaddrinfoproc = (FreeAddrInfoProc)GetProcAddress(winsock,
			"freeaddrinfo");
	}
	if (getaddrinfoproc == NULL || freeaddrinfoproc == NULL)
		return ConnectIpv4(hostname, port);
#endif
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addresses = NULL;
#ifdef _WIN32
	if (getaddrinfoproc(hostname, port, &hints, &addresses) != 0)
		return MONITOR_INVALID_SOCKET;
	monitorsocket = ConnectAddressList(addresses);
	freeaddrinfoproc(addresses);
#else
	if (getaddrinfo(hostname, port, &hints, &addresses) != 0)
		return MONITOR_INVALID_SOCKET;
	monitorsocket = ConnectAddressList(addresses);
	freeaddrinfo(addresses);
#endif
	return monitorsocket;
}

static int ExchangeMonitor(const char *hostname, const char *port,
	const char *command, int commandlength, char *response, int responsesize,
	int timeoutms)
{
	MonitorSocket monitorsocket;
	int received;
	int used;
#ifdef _WIN32
	WSADATA winsockdata;
	if (WSAStartup(MAKEWORD(2, 2), &winsockdata) != 0) {
		fprintf(stderr, "Unable to initialize Winsock\n");
		return 2;
	}
#endif
	monitorsocket = ConnectMonitor(hostname, port);
	if (monitorsocket == MONITOR_INVALID_SOCKET) {
		fprintf(stderr, "Unable to connect to %s:%s\n", hostname, port);
#ifdef _WIN32
		WSACleanup();
#endif
		return 2;
	}
	if (timeoutms > 0)
		SetReceiveTimeout(monitorsocket, timeoutms);
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
		response, (int)sizeof(response), 0);
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
		(int)sizeof(response), 0) != 0) {
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

static unsigned int ReadBigEndian16(const unsigned char *bytes)
{
	return ((unsigned int)bytes[0] << 8) | (unsigned int)bytes[1];
}

static void PutBigEndian32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

static int ReadAt(FILE *file, MonitorOffset offset, void *buffer,
	unsigned long size)
{
	if (SeekFile(file, offset) != 0)
		return 0;
	return fread(buffer, 1, (size_t)size, file) == (size_t)size;
}

static unsigned long MacRomanToUnicode(unsigned char character)
{
	static const unsigned short high[128] = {
		0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
		0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
		0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
		0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
		0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
		0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
		0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
		0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
		0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
		0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
		0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
		0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
		0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
		0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
		0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
		0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
	};

	if (character < 0x80)
		return character;
	return high[character - 0x80];
}

static int UnicodeToMacRoman(unsigned long code)
{
	int index;

	if (code < 0x80)
		return (int)code;
	for (index = 0; index < 128; index++) {
		if (MacRomanToUnicode((unsigned char)(index + 0x80)) == code)
			return index + 0x80;
	}
	return -1;
}

static unsigned long ComposeUnicode(unsigned long base, unsigned long mark)
{
	static const char bases[] = "AAAAAACEEEEIIIINOOOOOUUUU";
	static const unsigned char marks[] = {
		0x00, 0x01, 0x02, 0x03, 0x08, 0x0A, 0x27, 0x00, 0x01, 0x02, 0x08, 0x00,
		0x01, 0x02, 0x08, 0x03, 0x00, 0x01, 0x02, 0x03, 0x08, 0x00, 0x01, 0x02,
		0x08
	};
	static const unsigned char composed[] = {
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC,
		0xCD, 0xCE, 0xCF, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD9, 0xDA, 0xDB,
		0xDC
	};
	unsigned long upper;
	unsigned long lowercase;
	int index;

	if (mark < 0x300 || mark > 0x327)
		return 0;
	if (mark == 0x308 && base == 'Y')
		return 0x178;
	if (mark == 0x308 && base == 'y')
		return 0xFF;
	upper = base;
	lowercase = 0;
	if (base >= 'a' && base <= 'z') {
		upper = base - 0x20;
		lowercase = 0x20;
	}
	for (index = 0; bases[index] != 0; index++) {
		if ((unsigned long)bases[index] == upper && marks[index] == mark - 0x300)
			return composed[index] + lowercase;
	}
	return 0;
}

static int CodesToMacRoman(const unsigned long *codes, int count,
	unsigned char *text, int textsize)
{
	unsigned long code;
	unsigned long composedcode;
	int index;
	int used;
	int mapped;

	used = 0;
	index = 0;
	while (index < count && used < textsize - 1) {
		code = codes[index++];
		if (index < count) {
			composedcode = ComposeUnicode(code, codes[index]);
			if (composedcode != 0) {
				code = composedcode;
				index++;
			}
		}
		mapped = UnicodeToMacRoman(code);
		if (mapped < 0)
			mapped = '?';
		text[used++] = (unsigned char)mapped;
	}
	return used;
}

static int ConvertUtf16Name(const unsigned char *units, int count,
	unsigned char *text, int textsize)
{
	unsigned long codes[MONITOR_NAME_SIZE];
	int index;

	if (count > MONITOR_NAME_SIZE)
		count = MONITOR_NAME_SIZE;
	for (index = 0; index < count; index++)
		codes[index] = ReadBigEndian16(units + index * 2);
	return CodesToMacRoman(codes, count, text, textsize);
}

static int ArgumentToMacRoman(const char *argument, unsigned char *text,
	int textsize)
{
	unsigned long codes[MONITOR_MACPATH_SIZE];
	int count;
#ifdef _WIN32
	wchar_t wide[MONITOR_MACPATH_SIZE];
	int index;

	count = MultiByteToWideChar(CP_ACP, 0, argument, -1, wide,
		MONITOR_MACPATH_SIZE);
	if (count <= 0)
		return 0;
	count--;
	for (index = 0; index < count; index++)
		codes[index] = (unsigned long)wide[index];
#else
	const unsigned char *cursor;
	unsigned long code;
	int extra;

	cursor = (const unsigned char *)argument;
	count = 0;
	while (*cursor != 0 && count < MONITOR_MACPATH_SIZE) {
		code = *cursor++;
		extra = 0;
		if (code >= 0xF0) {
			code &= 0x07;
			extra = 3;
		} else if (code >= 0xE0) {
			code &= 0x0F;
			extra = 2;
		} else if (code >= 0xC0) {
			code &= 0x1F;
			extra = 1;
		}
		while (extra > 0 && (*cursor & 0xC0) == 0x80) {
			code = (code << 6) | (unsigned long)(*cursor++ & 0x3F);
			extra--;
		}
		codes[count++] = code;
	}
#endif
	return CodesToMacRoman(codes, count, text, textsize);
}

static void PutUtf8(FILE *output, unsigned long code)
{
	if (code < 0x80) {
		fputc((int)code, output);
	} else if (code < 0x800) {
		fputc(0xC0 | (int)(code >> 6), output);
		fputc(0x80 | (int)(code & 0x3F), output);
	} else {
		fputc(0xE0 | (int)(code >> 12), output);
		fputc(0x80 | (int)((code >> 6) & 0x3F), output);
		fputc(0x80 | (int)(code & 0x3F), output);
	}
}

static void WriteMacRomanText(FILE *output, const unsigned char *text,
	unsigned long length, int astext)
{
	unsigned long index;

	for (index = 0; index < length; index++) {
		if (astext && text[index] == '\r') {
			fputc('\n', output);
			if (index + 1 < length && text[index + 1] == '\n')
				index++;
		} else if (!astext && text[index] < 0x20) {
			fputc('?', output);
		} else {
			PutUtf8(output, MacRomanToUnicode(text[index]));
		}
	}
}

static unsigned char FoldCase(unsigned char character)
{
	if (character >= 'A' && character <= 'Z')
		return (unsigned char)(character + 0x20);
	return character;
}

static int NamesEqual(const unsigned char *first, int firstlength,
	const unsigned char *second, int secondlength)
{
	int index;

	if (firstlength != secondlength)
		return 0;
	for (index = 0; index < firstlength; index++) {
		if (FoldCase(first[index]) != FoldCase(second[index]))
			return 0;
	}
	return 1;
}

static int PrefixesMatch(const unsigned char *first, int firstlength,
	const unsigned char *second, int secondlength)
{
	int length;

	length = firstlength;
	if (secondlength < length)
		length = secondlength;
	return NamesEqual(first, length, second, length);
}

static int MatchPattern(const unsigned char *pattern, int patternlength,
	const unsigned char *name, int namelength)
{
	int patternindex;
	int nameindex;
	int star;
	int resume;

	patternindex = 0;
	nameindex = 0;
	star = -1;
	resume = 0;
	while (nameindex < namelength) {
		if (patternindex < patternlength && pattern[patternindex] == '*') {
			star = patternindex++;
			resume = nameindex;
		} else if (patternindex < patternlength && (pattern[patternindex] == '?' ||
			FoldCase(pattern[patternindex]) == FoldCase(name[nameindex]))) {
			patternindex++;
			nameindex++;
		} else if (star >= 0) {
			patternindex = star + 1;
			nameindex = ++resume;
		} else {
			return 0;
		}
	}
	while (patternindex < patternlength && pattern[patternindex] == '*')
		patternindex++;
	return patternindex == patternlength;
}

static int ParseExtents(int isplus, const unsigned char *source,
	unsigned long *extents)
{
	int index;

	if (isplus) {
		for (index = 0; index < 8; index++) {
			extents[index * 2] = ReadBigEndian32(source + index * 8);
			extents[index * 2 + 1] = ReadBigEndian32(source + index * 8 + 4);
		}
		return 8;
	}
	for (index = 0; index < 3; index++) {
		extents[index * 2] = ReadBigEndian16(source + index * 4);
		extents[index * 2 + 1] = ReadBigEndian16(source + index * 4 + 2);
	}
	return 3;
}

static int WalkLeaves(const unsigned char *tree, unsigned long treesize,
	LeafVisitor visitor, void *context)
{
	const unsigned char *nodedata;
	unsigned long nodesize;
	unsigned long totalnodes;
	unsigned long node;
	unsigned long visited;
	unsigned int recordcount;
	unsigned int record;
	unsigned int recordoffset;
	unsigned int nextoffset;

	if (tree == NULL || treesize < 512)
		return 0;
	nodesize = ReadBigEndian16(tree + 32);
	if (nodesize < 512 || nodesize > treesize)
		return 0;
	totalnodes = treesize / nodesize;
	node = ReadBigEndian32(tree + 24);
	visited = 0;
	while (node != 0 && node < totalnodes && visited < totalnodes) {
		nodedata = tree + node * nodesize;
		if (nodedata[8] != 0xFF)
			return 0;
		recordcount = ReadBigEndian16(nodedata + 10);
		for (record = 0; record < recordcount &&
			2 * (record + 2) <= nodesize; record++) {
			recordoffset = ReadBigEndian16(nodedata + nodesize - 2 * (record + 1));
			nextoffset = ReadBigEndian16(nodedata + nodesize - 2 * (record + 2));
			if (recordoffset >= 14 && nextoffset > recordoffset &&
				nextoffset <= nodesize &&
				visitor(context, nodedata + recordoffset, nextoffset - recordoffset))
				return 1;
		}
		node = ReadBigEndian32(nodedata);
		visited++;
	}
	return 1;
}

static int VisitOverflowRecord(void *context, const unsigned char *record,
	unsigned long available)
{
	OverflowSearch *search;

	search = (OverflowSearch *)context;
	if (search->isplus) {
		if (available < 12 + 64 || ReadBigEndian16(record) != 10)
			return 0;
		if (record[2] != search->forktype ||
			ReadBigEndian32(record + 4) != search->fileid ||
			ReadBigEndian32(record + 8) != search->startblock)
			return 0;
		search->count = ParseExtents(1, record + 12, search->extents);
		return 1;
	}
	if (available < 8 + 12 || record[0] != 7)
		return 0;
	if (record[1] != search->forktype ||
		ReadBigEndian32(record + 2) != search->fileid ||
		ReadBigEndian16(record + 6) != search->startblock)
		return 0;
	search->count = ParseExtents(0, record + 8, search->extents);
	return 1;
}

static unsigned char *ReadFork(HfsVolume *volume,
	const unsigned char *extentrecord, unsigned long logicalsize,
	unsigned long fileid, unsigned int forktype)
{
	unsigned long extents[MONITOR_MAX_EXTENTS * 2];
	unsigned char *buffer;
	unsigned long copied;
	unsigned long before;
	unsigned long blocks;
	unsigned long bytes;
	unsigned long take;
	OverflowSearch search;
	int extentcount;
	int index;

	if (logicalsize == 0xFFFFFFFFUL)
		return NULL;
	buffer = (unsigned char *)malloc((size_t)logicalsize + 1);
	if (buffer == NULL)
		return NULL;
	extentcount = ParseExtents(volume->isplus, extentrecord, extents);
	copied = 0;
	blocks = 0;
	while (copied < logicalsize) {
		before = copied;
		for (index = 0; index < extentcount && copied < logicalsize; index++) {
			bytes = extents[index * 2 + 1] * volume->blocksize;
			take = logicalsize - copied;
			if (take > bytes)
				take = bytes;
			if (!ReadAt(volume->file, volume->allocationstart +
				(MonitorOffset)extents[index * 2] * volume->blocksize,
				buffer + copied, take)) {
				free(buffer);
				return NULL;
			}
			copied += take;
			blocks += extents[index * 2 + 1];
		}
		if (copied < logicalsize) {
			search.isplus = volume->isplus;
			search.fileid = fileid;
			search.forktype = forktype;
			search.startblock = blocks;
			search.extents = extents;
			search.count = 0;
			if (copied == before || volume->overflow == NULL) {
				free(buffer);
				return NULL;
			}
			WalkLeaves(volume->overflow, volume->overflowsize,
				VisitOverflowRecord, &search);
			if (search.count == 0) {
				free(buffer);
				return NULL;
			}
			extentcount = search.count;
		}
	}
	return buffer;
}

static int AddCatalogEntry(HfsVolume *volume, const CatalogEntry *entry,
	const unsigned char *name)
{
	unsigned char *names;
	CatalogEntry *entries;
	unsigned long capacity;

	if (volume->namesused + (unsigned long)entry->namelength >
		volume->namescapacity) {
		capacity = volume->namescapacity * 2 + 4096;
		names = (unsigned char *)realloc(volume->names, (size_t)capacity);
		if (names == NULL)
			return 0;
		volume->names = names;
		volume->namescapacity = capacity;
	}
	if (volume->entrycount == volume->entrycapacity) {
		capacity = volume->entrycapacity * 2 + 256;
		entries = (CatalogEntry *)realloc(volume->entries,
			(size_t)capacity * sizeof(CatalogEntry));
		if (entries == NULL)
			return 0;
		volume->entries = entries;
		volume->entrycapacity = capacity;
	}
	volume->entries[volume->entrycount] = *entry;
	volume->entries[volume->entrycount].nameoffset = volume->namesused;
	memcpy(volume->names + volume->namesused, name, (size_t)entry->namelength);
	volume->namesused += (unsigned long)entry->namelength;
	volume->entrycount++;
	return 1;
}

static int VisitCatalogRecord(void *context, const unsigned char *record,
	unsigned long available)
{
	HfsVolume *volume;
	CatalogEntry entry;
	const unsigned char *data;
	unsigned char name[MONITOR_NAME_SIZE];
	unsigned long keylength;
	unsigned long dataoffset;
	unsigned long required;
	unsigned int recordtype;

	volume = (HfsVolume *)context;
	if (available < 8)
		return 0;
	if (volume->isplus) {
		keylength = ReadBigEndian16(record);
		dataoffset = keylength + 2;
		if (dataoffset + 12 > available ||
			ReadBigEndian16(record + 6) * 2UL + 6 > keylength)
			return 0;
		entry.namelength = ConvertUtf16Name(record + 8,
			(int)ReadBigEndian16(record + 6), name, (int)sizeof(name));
		data = record + dataoffset;
		recordtype = ReadBigEndian16(data);
		required = 248;
		if (recordtype == 1)
			required = 88;
	} else {
		keylength = record[0];
		dataoffset = (keylength + 2) & ~1UL;
		if (dataoffset + 10 > available || record[6] + 6UL > keylength)
			return 0;
		memcpy(name, record + 7, record[6]);
		entry.namelength = record[6];
		data = record + dataoffset;
		recordtype = data[0];
		required = 102;
		if (recordtype == 1)
			required = 70;
	}
	if ((recordtype != 1 && recordtype != 2) || dataoffset + required > available)
		return 0;
	if (volume->isplus)
		entry.id = ReadBigEndian32(data + 8);
	else if (recordtype == 1)
		entry.id = ReadBigEndian32(data + 6);
	else
		entry.id = ReadBigEndian32(data + 20);
	entry.parentid = ReadBigEndian32(record + 2);
	entry.isfolder = recordtype == 1;
	entry.recordoffset = (unsigned long)(data - volume->catalog);
	entry.nameoffset = 0;
	return !AddCatalogEntry(volume, &entry, name);
}

static int CompareFolders(const void *first, const void *second)
{
	const FolderIndex *firstfolder;
	const FolderIndex *secondfolder;

	firstfolder = (const FolderIndex *)first;
	secondfolder = (const FolderIndex *)second;
	if (firstfolder->id < secondfolder->id)
		return -1;
	if (firstfolder->id > secondfolder->id)
		return 1;
	return 0;
}

static int BuildFolderIndex(HfsVolume *volume)
{
	unsigned long index;
	unsigned long count;
	int foundroot;

	count = 0;
	for (index = 0; index < volume->entrycount; index++) {
		if (volume->entries[index].isfolder)
			count++;
	}
	volume->folders = (FolderIndex *)malloc((size_t)(count + 1) *
		sizeof(FolderIndex));
	if (volume->folders == NULL)
		return 0;
	foundroot = 0;
	for (index = 0; index < volume->entrycount; index++) {
		if (volume->entries[index].isfolder) {
			volume->folders[volume->foldercount].id = volume->entries[index].id;
			volume->folders[volume->foldercount].entry = index;
			volume->foldercount++;
			if (volume->entries[index].parentid == 1) {
				volume->rootentry = index;
				foundroot = 1;
			}
		}
	}
	qsort(volume->folders, (size_t)volume->foldercount, sizeof(FolderIndex),
		CompareFolders);
	return foundroot;
}

static long LookupFolder(const HfsVolume *volume, unsigned long id)
{
	unsigned long low;
	unsigned long high;
	unsigned long middle;

	low = 0;
	high = volume->foldercount;
	while (low < high) {
		middle = (low + high) / 2;
		if (volume->folders[middle].id < id)
			low = middle + 1;
		else
			high = middle;
	}
	if (low < volume->foldercount && volume->folders[low].id == id)
		return (long)volume->folders[low].entry;
	return -1;
}

static int OpenVolumeAt(HfsVolume *volume, FILE *file, MonitorOffset offset)
{
	unsigned char header[512];
	unsigned int signature;

	memset(volume, 0, sizeof(*volume));
	volume->file = file;
	if (!ReadAt(file, offset + 1024, header, sizeof(header)))
		return 0;
	signature = ReadBigEndian16(header);
	if (signature == HFS_SIGNATURE) {
		if (ReadBigEndian16(header + 124) == HFSPLUS_SIGNATURE)
			return OpenVolumeAt(volume, file, offset +
				(MonitorOffset)ReadBigEndian16(header + 28) * 512 +
				(MonitorOffset)ReadBigEndian16(header + 126) *
				ReadBigEndian32(header + 20));
		volume->allocationstart = offset +
			(MonitorOffset)ReadBigEndian16(header + 28) * 512;
		volume->blocksize = ReadBigEndian32(header + 20);
		if (volume->blocksize == 0)
			return 0;
		volume->overflowsize = ReadBigEndian32(header + 130);
		volume->overflow = ReadFork(volume, header + 134, volume->overflowsize,
			3, MONITOR_FORK_DATA);
		volume->catalogsize = ReadBigEndian32(header + 146);
		volume->catalog = ReadFork(volume, header + 150, volume->catalogsize,
			4, MONITOR_FORK_DATA);
	} else if (signature == HFSPLUS_SIGNATURE || signature == HFSX_SIGNATURE) {
		volume->isplus = 1;
		volume->allocationstart = offset;
		volume->blocksize = ReadBigEndian32(header + 40);
		if (volume->blocksize == 0 || ReadBigEndian32(header + 192) != 0 ||
			ReadBigEndian32(header + 272) != 0)
			return 0;
		volume->overflowsize = ReadBigEndian32(header + 196);
		volume->overflow = ReadFork(volume, header + 208, volume->overflowsize,
			3, MONITOR_FORK_DATA);
		volume->catalogsize = ReadBigEndian32(header + 276);
		volume->catalog = ReadFork(volume, header + 288, volume->catalogsize,
			4, MONITOR_FORK_DATA);
	} else {
		return 0;
	}
	if (volume->overflow == NULL || volume->catalog == NULL ||
		!WalkLeaves(volume->catalog, volume->catalogsize, VisitCatalogRecord,
		volume))
		return 0;
	return BuildFolderIndex(volume);
}

static void CloseVolume(HfsVolume *volume)
{
	free(volume->overflow);
	free(volume->catalog);
	free(volume->names);
	free(volume->entries);
	free(volume->folders);
	if (volume->file != NULL)
		fclose(volume->file);
}

static int HasVolumeSignature(FILE *file, MonitorOffset offset)
{
	unsigned char signature[2];
	unsigned int value;

	if (!ReadAt(file, offset + 1024, signature, sizeof(signature)))
		return 0;
	value = ReadBigEndian16(signature);
	return value == HFS_SIGNATURE || value == HFSPLUS_SIGNATURE ||
		value == HFSX_SIGNATURE;
}

static int FindVolumeOffsets(FILE *file, MonitorOffset *offsets, int maximum)
{
	unsigned char block[512];
	unsigned long mapcount;
	unsigned long index;
	int count;

	if (!ReadAt(file, 0, block, sizeof(block)))
		return 0;
	count = 0;
	if (ReadBigEndian16(block) == PARTITION_DRIVER_SIGNATURE) {
		mapcount = 1;
		for (index = 1; index <= mapcount && index < 256 && count < maximum;
			index++) {
			if (!ReadAt(file, (MonitorOffset)index * 512, block, sizeof(block)) ||
				ReadBigEndian16(block) != PARTITION_MAP_SIGNATURE)
				break;
			mapcount = ReadBigEndian32(block + 4);
			if (strncmp((const char *)block + 48, "Apple_HFS", 9) == 0)
				offsets[count++] = (MonitorOffset)ReadBigEndian32(block + 8) * 512;
		}
		return count;
	}
	if (HasVolumeSignature(file, 0)) {
		offsets[0] = 0;
		return 1;
	}
	if (HasVolumeSignature(file, DISKCOPY_HEADER_SIZE)) {
		offsets[0] = DISKCOPY_HEADER_SIZE;
		return 1;
	}
	return 0;
}

static int OpenDiskImage(const char *path, HfsVolume *volumes, int maximum)
{
	MonitorOffset offsets[MONITOR_MAX_VOLUMES];
	struct stat filestat;
	FILE *file;
	int offsetcount;
	int index;
	int count;

	if (stat(path, &filestat) != 0)
		return 0;
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "Unable to open %s\n", path);
		return 0;
	}
	if (maximum > MONITOR_MAX_VOLUMES)
		maximum = MONITOR_MAX_VOLUMES;
	offsetcount = FindVolumeOffsets(file, offsets, maximum);
	fclose(file);
	count = 0;
	for (index = 0; index < offsetcount; index++) {
		file = fopen(path, "rb");
		if (file != NULL) {
			if (OpenVolumeAt(volumes + count, file, offsets[index]))
				count++;
			else
				CloseVolume(volumes + count);
		}
	}
	return count;
}

static const char *ParseJsonString(const char *source, char *text, int textsize)
{
	int used;

	used = 0;
	while (*source != 0 && *source != '"') {
		if (*source == '\\' && source[1] != 0)
			source++;
		if (used < textsize - 1)
			text[used++] = *source;
		source++;
	}
	text[used] = 0;
	if (*source != '"')
		return NULL;
	return source + 1;
}

static int LoadGuestVolumes(const char *hostname, const char *port,
	GuestVolumes *guest)
{
	char response[4096];
	char path[MONITOR_PATH_SIZE];
	char name[MONITOR_NAME_SIZE];
	const char *cursor;

	guest->volumecount = 0;
	guest->extfsroot[0] = 0;
	guest->extfsnamelength = 0;
	if (ExchangeMonitor(hostname, port, "disks\n", 6, response,
		(int)sizeof(response), 0) != 0) {
		fprintf(stderr, "Unable to list guest disks: %s", response);
		return 0;
	}
	cursor = strstr(response, "\"extfsname\":\"");
	if (cursor != NULL && ParseJsonString(cursor + 13, name,
		(int)sizeof(name)) != NULL)
		guest->extfsnamelength = ArgumentToMacRoman(name, guest->extfsname,
			(int)sizeof(guest->extfsname));
	cursor = strstr(response, "\"disks\":[");
	if (cursor != NULL) {
		cursor += 9;
		while (cursor != NULL && *cursor == '"' &&
			guest->volumecount < MONITOR_MAX_VOLUMES) {
			cursor = ParseJsonString(cursor + 1, path, (int)sizeof(path));
			if (cursor != NULL) {
				guest->volumecount += OpenDiskImage(path,
					guest->volumes + guest->volumecount,
					MONITOR_MAX_VOLUMES - guest->volumecount);
				if (*cursor == ',')
					cursor++;
			}
		}
	}
	if (guest->extfsnamelength > 0 && !QueryExtFSRoot(hostname, port,
		guest->extfsroot, (int)sizeof(guest->extfsroot)))
		guest->extfsroot[0] = 0;
	return 1;
}

static void CloseGuestVolumes(GuestVolumes *guest)
{
	int index;

	for (index = 0; index < guest->volumecount; index++)
		CloseVolume(guest->volumes + index);
}

static int BuildEntryPath(const HfsVolume *volume, unsigned long entryindex,
	unsigned char *path, int pathsize)
{
	unsigned long chain[MONITOR_MAX_DEPTH];
	const CatalogEntry *entry;
	long parent;
	int depth;
	int used;
	int index;

	depth = 0;
	chain[depth++] = entryindex;
	while (volume->entries[chain[depth - 1]].parentid != 1) {
		parent = LookupFolder(volume, volume->entries[chain[depth - 1]].parentid);
		if (parent < 0 || depth >= MONITOR_MAX_DEPTH)
			return 0;
		chain[depth++] = (unsigned long)parent;
	}
	used = 0;
	for (index = depth - 1; index >= 0; index--) {
		entry = volume->entries + chain[index];
		if (used + entry->namelength + 2 >= pathsize)
			return 0;
		memcpy(path + used, volume->names + entry->nameoffset,
			(size_t)entry->namelength);
		used += entry->namelength;
		if (index > 0 || entry->isfolder)
			path[used++] = ':';
	}
	return used;
}

static const unsigned char *GetForkRecord(const HfsVolume *volume,
	const CatalogEntry *entry, unsigned int forktype, unsigned long *size)
{
	const unsigned char *data;

	data = volume->catalog + entry->recordoffset;
	if (volume->isplus) {
		data += 88;
		if (forktype == MONITOR_FORK_RESOURCE)
			data += 80;
		*size = ReadBigEndian32(data + 4);
		if (ReadBigEndian32(data) != 0)
			*size = 0xFFFFFFFFUL;
		return data + 16;
	}
	if (forktype == MONITOR_FORK_RESOURCE) {
		*size = ReadBigEndian32(data + 36);
		return data + 86;
	}
	*size = ReadBigEndian32(data + 26);
	return data + 74;
}

static void GetFinderInfo(const HfsVolume *volume, const CatalogEntry *entry,
	unsigned char *finderinfo)
{
	const unsigned char *data;

	data = volume->catalog + entry->recordoffset;
	if (volume->isplus) {
		memcpy(finderinfo, data + 48, 32);
	} else if (entry->isfolder) {
		memcpy(finderinfo, data + 22, 16);
		memcpy(finderinfo + 16, data + 38, 16);
	} else {
		memcpy(finderinfo, data + 4, 16);
		memcpy(finderinfo + 16, data + 56, 16);
	}
}

static void PrintFileDetails(const unsigned char *finderinfo,
	unsigned long datalength, unsigned long rsrclength)
{
	fputc('\t', stdout);
	WriteMacRomanText(stdout, finderinfo, 4, 0);
	fputc('\t', stdout);
	WriteMacRomanText(stdout, finderinfo + 4, 4, 0);
	printf("\t%lu\t%lu\n", datalength, rsrclength);
}

static unsigned long FindInVolume(const HfsVolume *volume,
	const unsigned char *pattern, int patternlength, const unsigned char *scope,
	int scopelength)
{
	unsigned char path[MONITOR_MACPATH_SIZE];
	unsigned char finderinfo[32];
	const CatalogEntry *entry;
	unsigned long index;
	unsigned long matches;
	unsigned long datalength;
	unsigned long rsrclength;
	int pathlength;

	matches = 0;
	for (index = 0; index < volume->entrycount; index++) {
		entry = volume->entries + index;
		if (MatchPattern(pattern, patternlength, volume->names + entry->nameoffset,
			entry->namelength)) {
			pathlength = BuildEntryPath(volume, index, path, (int)sizeof(path));
			if (pathlength > 0 && pathlength >= scopelength &&
				PrefixesMatch(path, pathlength, scope, scopelength)) {
				WriteMacRomanText(stdout, path, (unsigned long)pathlength, 0);
				if (entry->isfolder) {
					fputc('\n', stdout);
				} else {
					GetFinderInfo(volume, entry, finderinfo);
					GetForkRecord(volume, entry, MONITOR_FORK_DATA, &datalength);
					GetForkRecord(volume, entry, MONITOR_FORK_RESOURCE, &rsrclength);
					PrintFileDetails(finderinfo, datalength, rsrclength);
				}
				matches++;
			}
		}
	}
	return matches;
}

static int HexValue(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

static int DecodeHostName(const char *hostname, unsigned char *name,
	int namesize)
{
	int used;

	used = 0;
	while (*hostname != 0 && used < namesize - 1) {
		if (hostname[0] == '%' && HexValue(hostname[1]) >= 0 &&
			HexValue(hostname[2]) >= 0) {
			name[used++] = (unsigned char)(HexValue(hostname[1]) * 16 +
				HexValue(hostname[2]));
			hostname += 3;
		} else {
			name[used++] = (unsigned char)*hostname++;
		}
	}
	return used;
}

static void ReadHostFinderInfo(const char *directory, const char *name,
	unsigned char *finderinfo)
{
	char path[MONITOR_PATH_SIZE * 2];
	FILE *input;
	size_t length;

	memset(finderinfo, 0, 32);
	memset(finderinfo, '?', 8);
	sprintf(path, "%s" MONITOR_PATH_SEPARATOR ".finf" MONITOR_PATH_SEPARATOR "%s",
		directory, name);
	input = fopen(path, "rb");
	if (input == NULL)
		return;
	length = fread(finderinfo, 1, 32, input);
	fclose(input);
	if (length < 8) {
		memset(finderinfo, 0, 32);
		memset(finderinfo, '?', 8);
	}
}

static unsigned long HostFileSize(const char *directory, const char *subdirectory,
	const char *name)
{
	char path[MONITOR_PATH_SIZE * 2];
	struct stat filestat;

	if (subdirectory[0] != 0)
		sprintf(path, "%s" MONITOR_PATH_SEPARATOR "%s" MONITOR_PATH_SEPARATOR "%s",
			directory, subdirectory, name);
	else
		sprintf(path, "%s" MONITOR_PATH_SEPARATOR "%s", directory, name);
	if (stat(path, &filestat) != 0)
		return 0;
	return (unsigned long)filestat.st_size;
}

static void ListHostFolder(const char *directory, HostVisitor visitor,
	void *context)
{
#ifdef _WIN32
	char pattern[MONITOR_PATH_SIZE + 4];
	WIN32_FIND_DATAA finddata;
	HANDLE find;

	if (strlen(directory) + 3 >= sizeof(pattern))
		return;
	sprintf(pattern, "%s\\*", directory);
	find = FindFirstFileA(pattern, &finddata);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do {
		visitor(context, finddata.cFileName,
			(finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
	} while (FindNextFileA(find, &finddata));
	FindClose(find);
#else
	char folderpath[MONITOR_PATH_SIZE];
	char path[MONITOR_PATH_SIZE * 2];
	struct dirent *item;
	struct stat itemstat;
	DIR *folder;

	if (strlen(directory) >= sizeof(folderpath))
		return;
	strcpy(folderpath, directory);
	folder = opendir(folderpath);
	if (folder == NULL)
		return;
	while ((item = readdir(folder)) != NULL) {
		sprintf(path, "%s/%s", folderpath, item->d_name);
		if (stat(path, &itemstat) == 0)
			visitor(context, item->d_name, S_ISDIR(itemstat.st_mode));
	}
	closedir(folder);
#endif
}

static void VisitHostItem(void *context, const char *name, int isfolder)
{
	HostSearch *search;
	unsigned char macname[MONITOR_NAME_SIZE];
	unsigned char finderinfo[32];
	size_t hostlength;
	int macnamelength;
	int macpathlength;

	search = (HostSearch *)context;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
		strcmp(name, ".finf") == 0 || strcmp(name, ".rsrc") == 0)
		return;
	macnamelength = DecodeHostName(name, macname, (int)sizeof(macname));
	hostlength = strlen(search->hostpath);
	macpathlength = search->macpathlength;
	if (hostlength + strlen(name) + 2 >= MONITOR_PATH_SIZE ||
		macpathlength + macnamelength + 2 >= MONITOR_MACPATH_SIZE)
		return;
	memcpy(search->macpath + macpathlength, macname, (size_t)macnamelength);
	search->macpathlength += macnamelength;
	if (isfolder)
		search->macpath[search->macpathlength++] = ':';
	if (search->macpathlength >= search->scopelength &&
		MatchPattern(search->pattern, search->patternlength, macname,
		macnamelength) && PrefixesMatch(search->macpath, search->macpathlength,
		search->scope, search->scopelength)) {
		WriteMacRomanText(stdout, search->macpath,
			(unsigned long)search->macpathlength, 0);
		if (isfolder) {
			fputc('\n', stdout);
		} else {
			ReadHostFinderInfo(search->hostpath, name, finderinfo);
			PrintFileDetails(finderinfo, HostFileSize(search->hostpath, "", name),
				HostFileSize(search->hostpath, ".rsrc", name));
		}
		search->matches++;
	}
	if (isfolder && search->depth < MONITOR_MAX_DEPTH &&
		PrefixesMatch(search->macpath, search->macpathlength, search->scope,
		search->scopelength)) {
		sprintf(search->hostpath + hostlength, MONITOR_PATH_SEPARATOR "%s", name);
		search->depth++;
		ListHostFolder(search->hostpath, VisitHostItem, search);
		search->depth--;
	}
	search->hostpath[hostlength] = 0;
	search->macpathlength = macpathlength;
}

static int Find(const char *hostname, const char *port, const char *pattern,
	const char *scope)
{
	GuestVolumes guest;
	HostSearch search;
	unsigned char patterntext[MONITOR_NAME_SIZE + 2];
	unsigned char scopetext[MONITOR_MACPATH_SIZE];
	const unsigned char *patternstart;
	unsigned long matches;
	int patternlength;
	int scopelength;
	int index;

	patternlength = ArgumentToMacRoman(pattern, patterntext + 1,
		MONITOR_NAME_SIZE);
	patternstart = patterntext + 1;
	if (memchr(patternstart, '*', (size_t)patternlength) == NULL &&
		memchr(patternstart, '?', (size_t)patternlength) == NULL) {
		patterntext[0] = '*';
		patterntext[patternlength + 1] = '*';
		patternlength += 2;
		patternstart = patterntext;
	}
	scopelength = 0;
	if (scope != NULL)
		scopelength = ArgumentToMacRoman(scope, scopetext, (int)sizeof(scopetext));
	if (!LoadGuestVolumes(hostname, port, &guest))
		return 2;
	matches = 0;
	for (index = 0; index < guest.volumecount; index++)
		matches += FindInVolume(guest.volumes + index, patternstart,
			patternlength, scopetext, scopelength);
	if (guest.extfsroot[0] != 0) {
		search.pattern = patternstart;
		search.patternlength = patternlength;
		search.scope = scopetext;
		search.scopelength = scopelength;
		strcpy(search.hostpath, guest.extfsroot);
		memcpy(search.macpath, guest.extfsname, (size_t)guest.extfsnamelength);
		search.macpath[guest.extfsnamelength] = ':';
		search.macpathlength = guest.extfsnamelength + 1;
		search.depth = 0;
		search.matches = 0;
		if (PrefixesMatch(search.macpath, search.macpathlength, scopetext,
			scopelength))
			ListHostFolder(search.hostpath, VisitHostItem, &search);
		matches += search.matches;
	}
	CloseGuestVolumes(&guest);
	if (matches == 0)
		return 1;
	return 0;
}

static long ResolveVolumePath(const HfsVolume *volume,
	const unsigned char *path, int pathlength)
{
	const CatalogEntry *root;
	const CatalogEntry *entry;
	unsigned long index;
	long current;
	long found;
	int start;
	int end;

	root = volume->entries + volume->rootentry;
	end = 0;
	while (end < pathlength && path[end] != ':')
		end++;
	if (!NamesEqual(path, end, volume->names + root->nameoffset,
		root->namelength))
		return -1;
	current = (long)volume->rootentry;
	while (end < pathlength) {
		start = end + 1;
		end = start;
		while (end < pathlength && path[end] != ':')
			end++;
		if (end > start) {
			found = -1;
			for (index = 0; index < volume->entrycount && found < 0; index++) {
				entry = volume->entries + index;
				if (entry->parentid == volume->entries[current].id &&
					NamesEqual(path + start, end - start,
					volume->names + entry->nameoffset, entry->namelength))
					found = (long)index;
			}
			if (found < 0)
				return -1;
			current = found;
		}
	}
	return current;
}

static int ReadVolumeFile(HfsVolume *volume, unsigned long entryindex,
	GuestFile *file, int wantresource)
{
	const CatalogEntry *entry;
	const unsigned char *data;
	const unsigned char *extents;
	unsigned long size;

	entry = volume->entries + entryindex;
	if (entry->isfolder)
		return 0;
	memcpy(file->name, volume->names + entry->nameoffset,
		(size_t)entry->namelength);
	file->namelength = entry->namelength;
	GetFinderInfo(volume, entry, file->finderinfo);
	data = volume->catalog + entry->recordoffset;
	if (volume->isplus) {
		file->created = ReadBigEndian32(data + 12);
		file->modified = ReadBigEndian32(data + 16);
	} else {
		file->created = ReadBigEndian32(data + 44);
		file->modified = ReadBigEndian32(data + 48);
	}
	extents = GetForkRecord(volume, entry, MONITOR_FORK_DATA, &size);
	file->data = ReadFork(volume, extents, size, entry->id, MONITOR_FORK_DATA);
	if (file->data == NULL)
		return 0;
	file->datalength = size;
	if (!wantresource)
		return 1;
	extents = GetForkRecord(volume, entry, MONITOR_FORK_RESOURCE, &size);
	file->rsrc = ReadFork(volume, extents, size, entry->id,
		MONITOR_FORK_RESOURCE);
	if (file->rsrc == NULL)
		return 0;
	file->rsrclength = size;
	return 1;
}

static unsigned char *ReadHostFork(const char *path, unsigned long *length)
{
	unsigned char *contents;
	struct stat filestat;
	long size;

	*length = 0;
	if (stat(path, &filestat) != 0 || filestat.st_size == 0)
		return (unsigned char *)malloc(1);
	size = 0;
	contents = ReadWholeFile(path, &size);
	if (contents != NULL)
		*length = (unsigned long)size;
	return contents;
}

static int ReadHostFile(const GuestVolumes *guest, const unsigned char *path,
	int pathlength, int start, GuestFile *file, int wantresource)
{
	char hostpath[MONITOR_PATH_SIZE];
	char directory[MONITOR_PATH_SIZE];
	char hostname[MONITOR_NAME_SIZE * 3 + 1];
	char sidecar[MONITOR_PATH_SIZE * 2];
	struct stat filestat;
	int end;

	strcpy(hostpath, guest->extfsroot);
	directory[0] = 0;
	hostname[0] = 0;
	while (start < pathlength) {
		end = start;
		while (end < pathlength && path[end] != ':')
			end++;
		if (end > start) {
			if (end - start >= MONITOR_NAME_SIZE ||
				strlen(hostpath) + (size_t)(end - start) * 3 + 2 >= MONITOR_PATH_SIZE)
				return 0;
			strcpy(directory, hostpath);
			ConvertToHostName(path + start, end - start, hostname);
			strcat(hostpath, MONITOR_PATH_SEPARATOR);
			strcat(hostpath, hostname);
			memcpy(file->name, path + start, (size_t)(end - start));
			file->namelength = end - start;
		}
		start = end + 1;
	}
	if (hostname[0] == 0 || stat(hostpath, &filestat) != 0 ||
		!S_ISREG(filestat.st_mode))
		return 0;
	file->created = (unsigned long)filestat.st_mtime + MONITOR_MAC_EPOCH_OFFSET;
	file->modified = file->created;
	ReadHostFinderInfo(directory, hostname, file->finderinfo);
	file->data = ReadHostFork(hostpath, &file->datalength);
	if (file->data == NULL)
		return 0;
	if (!wantresource)
		return 1;
	sprintf(sidecar, "%s" MONITOR_PATH_SEPARATOR ".rsrc" MONITOR_PATH_SEPARATOR "%s",
		directory, hostname);
	file->rsrc = ReadHostFork(sidecar, &file->rsrclength);
	return file->rsrc != NULL;
}

static int ReadGuestFile(GuestVolumes *guest, const char *path,
	GuestFile *file, int wantresource)
{
	unsigned char macpath[MONITOR_MACPATH_SIZE];
	long entryindex;
	int macpathlength;
	int index;
	int first;

	macpathlength = ArgumentToMacRoman(path, macpath, (int)sizeof(macpath));
	for (index = 0; index < guest->volumecount; index++) {
		entryindex = ResolveVolumePath(guest->volumes + index, macpath,
			macpathlength);
		if (entryindex >= 0)
			return ReadVolumeFile(guest->volumes + index,
				(unsigned long)entryindex, file, wantresource);
	}
	first = 0;
	while (first < macpathlength && macpath[first] != ':')
		first++;
	if (guest->extfsroot[0] != 0 && NamesEqual(macpath, first, guest->extfsname,
		guest->extfsnamelength))
		return ReadHostFile(guest, macpath, macpathlength, first + 1, file,
			wantresource);
	return 0;
}

static void FreeGuestFile(GuestFile *file)
{
	free(file->data);
	free(file->rsrc);
}

static int LoadGuestFile(const char *hostname, const char *port,
	const char *path, GuestFile *file, int wantresource)
{
	GuestVolumes guest;
	int found;

	memset(file, 0, sizeof(*file));
	if (!LoadGuestVolumes(hostname, port, &guest))
		return 0;
	found = ReadGuestFile(&guest, path, file, wantresource);
	CloseGuestVolumes(&guest);
	if (!found) {
		fprintf(stderr, "Unable to read %s\n", path);
		FreeGuestFile(file);
	}
	return found;
}

static int Cat(const char *hostname, const char *port, const char *path)
{
	GuestFile file;

	if (!LoadGuestFile(hostname, port, path, &file, 0))
		return 2;
	WriteMacRomanText(stdout, file.data, file.datalength, 1);
	FreeGuestFile(&file);
	return 0;
}

static unsigned int Crc16(const unsigned char *data, int length)
{
	unsigned int crc;
	int index;
	int bit;

	crc = 0;
	for (index = 0; index < length; index++) {
		crc ^= (unsigned int)data[index] << 8;
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x8000)
				crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
			else
				crc = (crc << 1) & 0xFFFF;
		}
	}
	return crc;
}

static int WritePadded(FILE *output, const unsigned char *data,
	unsigned long length)
{
	unsigned char padding[MACBINARY_HEADER_SIZE];
	unsigned long remainder;

	memset(padding, 0, sizeof(padding));
	if (length > 0 && fwrite(data, 1, (size_t)length, output) != (size_t)length)
		return 0;
	remainder = length & (MACBINARY_HEADER_SIZE - 1);
	if (remainder == 0)
		return 1;
	remainder = MACBINARY_HEADER_SIZE - remainder;
	return fwrite(padding, 1, (size_t)remainder, output) == (size_t)remainder;
}

static int WriteMacBinary(const char *path, const GuestFile *file)
{
	unsigned char header[MACBINARY_HEADER_SIZE];
	unsigned int crc;
	FILE *output;
	int namelength;
	int written;

	memset(header, 0, sizeof(header));
	namelength = file->namelength;
	if (namelength > 63)
		namelength = 63;
	header[1] = (unsigned char)namelength;
	memcpy(header + 2, file->name, (size_t)namelength);
	memcpy(header + 65, file->finderinfo, 8);
	header[73] = file->finderinfo[8];
	memcpy(header + 75, file->finderinfo + 10, 6);
	PutBigEndian32(header + 83, file->datalength);
	PutBigEndian32(header + 87, file->rsrclength);
	PutBigEndian32(header + 91, file->created);
	PutBigEndian32(header + 95, file->modified);
	header[101] = file->finderinfo[9];
	header[122] = 129;
	header[123] = 129;
	crc = Crc16(header, 124);
	header[124] = (unsigned char)(crc >> 8);
	header[125] = (unsigned char)crc;
	output = fopen(path, "wb");
	if (output == NULL)
		return 0;
	written = fwrite(header, 1, sizeof(header), output) == sizeof(header) &&
		WritePadded(output, file->data, file->datalength) &&
		WritePadded(output, file->rsrc, file->rsrclength);
	if (fclose(output) != 0)
		written = 0;
	return written;
}

static void PrintJsonText(const char *text)
{
	while (*text != 0) {
		if (*text == '"' || *text == '\\')
			fputc('\\', stdout);
		fputc(*text++, stdout);
	}
}

static int Fetch(const char *hostname, const char *port, const char *path,
	const char *output)
{
	GuestFile file;
	char outputpath[MONITOR_PATH_SIZE];
	char defaultname[MONITOR_NAME_SIZE * 3 + 1];

	if (output != NULL && strlen(output) >= sizeof(outputpath)) {
		fprintf(stderr, "Path is too long\n");
		return 2;
	}
	if (!LoadGuestFile(hostname, port, path, &file, 1))
		return 2;
	if (output != NULL) {
		strcpy(outputpath, output);
	} else {
		ConvertToHostName(file.name, file.namelength, defaultname);
		sprintf(outputpath, "%s.bin", defaultname);
	}
	if (!WriteMacBinary(outputpath, &file)) {
		fprintf(stderr, "Unable to write %s\n", outputpath);
		FreeGuestFile(&file);
		return 2;
	}
	fputs("{\"ok\":true,\"path\":\"", stdout);
	PrintJsonText(outputpath);
	printf("\",\"data\":%lu,\"rsrc\":%lu}\n", file.datalength, file.rsrclength);
	FreeGuestFile(&file);
	return 0;
}

static int IsLocalHost(const char *hostname)
{
	return strcmp(hostname, "127.0.0.1") == 0 ||
		strcmp(hostname, "localhost") == 0 || strcmp(hostname, "::1") == 0;
}

static unsigned long QueryProcessId(const char *hostname, const char *port)
{
	char response[4096];
	const char *field;

	if (ExchangeMonitor(hostname, port, "ping\n", 5, response,
		(int)sizeof(response), MONITOR_PING_TIMEOUT_MS) != 0)
		return 0;
	field = strstr(response, "\"pid\":");
	if (field == NULL)
		return 0;
	return strtoul(field + 6, NULL, 10);
}

#ifdef __linux__
static unsigned long FindListeningInode(int port)
{
	char line[512];
	FILE *table;
	unsigned int localport;
	unsigned int state;
	unsigned long inode;
	unsigned long found;

	table = fopen("/proc/net/tcp", "r");
	if (table == NULL)
		return 0;
	found = 0;
	while (found == 0 && fgets(line, sizeof(line), table) != NULL) {
		if (sscanf(line,
			" %*d: %*x:%x %*x:%*x %x %*x:%*x %*x:%*x %*x %*u %*u %lu",
			&localport, &state, &inode) == 3 && (int)localport == port &&
			state == 0x0A)
			found = inode;
	}
	fclose(table);
	return found;
}

static unsigned long FindInodeOwner(unsigned long inode)
{
	char path[128];
	char target[64];
	char expected[64];
	struct dirent *process;
	struct dirent *descriptor;
	DIR *processes;
	DIR *descriptors;
	ssize_t length;
	unsigned long found;

	sprintf(expected, "socket:[%lu]", inode);
	processes = opendir("/proc");
	if (processes == NULL)
		return 0;
	found = 0;
	while (found == 0 && (process = readdir(processes)) != NULL) {
		if (process->d_name[0] >= '1' && process->d_name[0] <= '9') {
			sprintf(path, "/proc/%.32s/fd", process->d_name);
			descriptors = opendir(path);
			if (descriptors != NULL) {
				while (found == 0 && (descriptor = readdir(descriptors)) != NULL) {
					sprintf(path, "/proc/%.32s/fd/%.32s", process->d_name,
						descriptor->d_name);
					length = readlink(path, target, sizeof(target) - 1);
					if (length > 0) {
						target[length] = 0;
						if (strcmp(target, expected) == 0)
							found = strtoul(process->d_name, NULL, 10);
					}
				}
				closedir(descriptors);
			}
		}
	}
	closedir(processes);
	return found;
}
#endif

#ifdef __APPLE__
static unsigned long FindListeningDescriptor(pid_t processid, int port)
{
	struct proc_fdinfo *descriptors;
	struct socket_fdinfo socketinfo;
	unsigned long found;
	int descriptorcount;
	int index;
	int bytes;

	bytes = proc_pidinfo(processid, PROC_PIDLISTFDS, 0, NULL, 0);
	if (bytes <= 0)
		return 0;
	descriptors = (struct proc_fdinfo *)malloc((size_t)bytes);
	if (descriptors == NULL)
		return 0;
	bytes = proc_pidinfo(processid, PROC_PIDLISTFDS, 0, descriptors, bytes);
	descriptorcount = bytes / (int)PROC_PIDLISTFD_SIZE;
	found = 0;
	for (index = 0; index < descriptorcount && found == 0; index++) {
		if (descriptors[index].proc_fdtype == PROX_FDTYPE_SOCKET &&
			proc_pidfdinfo(processid, descriptors[index].proc_fd,
			PROC_PIDFDSOCKETINFO, &socketinfo, PROC_PIDFDSOCKETINFO_SIZE) ==
			PROC_PIDFDSOCKETINFO_SIZE &&
			socketinfo.psi.soi_kind == SOCKINFO_TCP &&
			socketinfo.psi.soi_proto.pri_tcp.tcpsi_state == TSI_S_LISTEN &&
			ntohs((unsigned short)socketinfo.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport) ==
			port)
			found = (unsigned long)processid;
	}
	free(descriptors);
	return found;
}
#endif

#ifdef _WIN32
static unsigned long FindListeningRow(const MonitorTcpTable *table, int port)
{
	DWORD index;

	for (index = 0; index < table->entrycount; index++) {
		if (table->rows[index].state == MONITOR_TCP_STATE_LISTEN &&
			ntohs((u_short)table->rows[index].localport) == port)
			return table->rows[index].owningpid;
	}
	return 0;
}

static unsigned long FindListeningProcessExtended(
	GetExtendedTcpTableProc getextendedtcptable, int port)
{
	MonitorTcpTable *table;
	unsigned long found;
	DWORD size;

	size = 0;
	if (getextendedtcptable(NULL, &size, FALSE, AF_INET,
		MONITOR_TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	table = (MonitorTcpTable *)malloc(size);
	if (table == NULL)
		return 0;
	found = 0;
	if (getextendedtcptable(table, &size, FALSE, AF_INET,
		MONITOR_TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR)
		found = FindListeningRow(table, port);
	free(table);
	return found;
}

static unsigned long FindListeningProcessFromStack(
	AllocateAndGetTcpExTableFromStackProc allocateandgettable, int port)
{
	MonitorTcpTable *table;
	unsigned long found;

	table = NULL;
	if (allocateandgettable((PVOID *)&table, FALSE, GetProcessHeap(), 0,
		AF_INET) != NO_ERROR || table == NULL)
		return 0;
	found = FindListeningRow(table, port);
	HeapFree(GetProcessHeap(), 0, table);
	return found;
}
#endif

static unsigned long FindListeningProcess(int port)
{
#ifdef _WIN32
	GetExtendedTcpTableProc getextendedtcptable;
	AllocateAndGetTcpExTableFromStackProc allocateandgettable;
	HMODULE iphelper;
	unsigned long found;

	iphelper = LoadLibraryA("iphlpapi.dll");
	if (iphelper == NULL)
		return 0;
	found = 0;
	getextendedtcptable = (GetExtendedTcpTableProc)GetProcAddress(iphelper,
		"GetExtendedTcpTable");
	allocateandgettable = (AllocateAndGetTcpExTableFromStackProc)GetProcAddress(
		iphelper, "AllocateAndGetTcpExTableFromStack");
	if (getextendedtcptable != NULL)
		found = FindListeningProcessExtended(getextendedtcptable, port);
	else if (allocateandgettable != NULL)
		found = FindListeningProcessFromStack(allocateandgettable, port);
	FreeLibrary(iphelper);
	return found;
#elif defined(__APPLE__)
	pid_t *processids;
	unsigned long found;
	int processcount;
	int index;
	int bytes;

	bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
	if (bytes <= 0)
		return 0;
	processids = (pid_t *)malloc((size_t)bytes);
	if (processids == NULL)
		return 0;
	processcount = proc_listpids(PROC_ALL_PIDS, 0, processids, bytes) /
		(int)sizeof(pid_t);
	found = 0;
	for (index = 0; index < processcount && found == 0; index++) {
		if (processids[index] > 0)
			found = FindListeningDescriptor(processids[index], port);
	}
	free(processids);
	return found;
#elif defined(__linux__)
	unsigned long inode;

	inode = FindListeningInode(port);
	if (inode == 0)
		return 0;
	return FindInodeOwner(inode);
#else
	(void)port;
	return 0;
#endif
}

static int OpenMonitorProcess(unsigned long processid, void **process)
{
#ifdef _WIN32
	*process = (void *)OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE,
		(DWORD)processid);
	return *process != NULL;
#else
	*process = NULL;
	return kill((pid_t)processid, 0) == 0 || errno != ESRCH;
#endif
}

static void CloseMonitorProcess(void *process)
{
#ifdef _WIN32
	CloseHandle((HANDLE)process);
#else
	(void)process;
#endif
}

static int WaitForProcessExit(void *process, unsigned long processid,
	int timeoutms)
{
#ifdef _WIN32
	(void)processid;
	return WaitForSingleObject((HANDLE)process, (DWORD)timeoutms) ==
		WAIT_OBJECT_0;
#else
	struct timespec delay;
	int waited;

	(void)process;
	delay.tv_sec = 0;
	delay.tv_nsec = 50000000L;
	for (waited = 0; waited < timeoutms; waited += 50) {
		if (kill((pid_t)processid, 0) != 0 && errno == ESRCH)
			return 1;
		nanosleep(&delay, NULL);
	}
	return kill((pid_t)processid, 0) != 0 && errno == ESRCH;
#endif
}

static int KillMonitorProcess(void *process, unsigned long processid)
{
#ifdef _WIN32
	(void)processid;
	return TerminateProcess((HANDLE)process, 1) != 0;
#else
	(void)process;
	return kill((pid_t)processid, SIGKILL) == 0;
#endif
}

static int Quit(const char *hostname, const char *port, int timeoutms)
{
	char response[4096];
	const char *method;
	unsigned long processid;
	void *process;
	int exited;
	int hung;

	processid = QueryProcessId(hostname, port);
	hung = 0;
	if (processid == 0 && IsLocalHost(hostname)) {
		processid = FindListeningProcess(atoi(port));
		hung = processid != 0;
	}
	if (processid == 0) {
		fprintf(stderr, "Unable to get the emulator process id\n");
		return 2;
	}
	if (!IsLocalHost(hostname)) {
		ExchangeMonitor(hostname, port, "quit\n", 5, response,
			(int)sizeof(response), timeoutms);
		printf("{\"ok\":true,\"pid\":%lu,\"method\":\"quit\",\"verified\":false}\n",
			processid);
		return 0;
	}
	if (!OpenMonitorProcess(processid, &process)) {
		fprintf(stderr, "Unable to open process %lu\n", processid);
		return 2;
	}
	method = "quit";
	exited = 0;
	if (!hung) {
		ExchangeMonitor(hostname, port, "quit\n", 5, response,
			(int)sizeof(response), timeoutms);
		exited = WaitForProcessExit(process, processid, timeoutms);
	}
	if (!exited) {
		method = "kill";
		exited = KillMonitorProcess(process, processid) &&
			WaitForProcessExit(process, processid, MONITOR_KILL_TIMEOUT_MS);
	}
	CloseMonitorProcess(process);
	if (!exited) {
		fprintf(stderr, "Unable to end process %lu\n", processid);
		return 2;
	}
	printf("{\"ok\":true,\"pid\":%lu,\"method\":\"%s\"}\n", processid, method);
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
			"       emumonitor [--host HOST] [--port PORT] deploy FILE.bin [FOLDER]\n"
			"       emumonitor [--host HOST] [--port PORT] find PATTERN [VOLUME:FOLDER]\n"
			"       emumonitor [--host HOST] [--port PORT] cat VOLUME:PATH\n"
			"       emumonitor [--host HOST] [--port PORT] fetch VOLUME:PATH [FILE.bin]\n"
			"       emumonitor [--host HOST] [--port PORT] quit [TIMEOUTMS]\n");
		return 2;
	}
	if (strcmp(argv[argument], "quit") == 0) {
		if (argc - argument > 2) {
			fprintf(stderr, "Usage: emumonitor quit [TIMEOUTMS]\n");
			return 2;
		}
		if (argc - argument == 2)
			return Quit(hostname, port, atoi(argv[argument + 1]));
		return Quit(hostname, port, MONITOR_QUIT_TIMEOUT_MS);
	}
	if (strcmp(argv[argument], "find") == 0) {
		if (argc - argument < 2 || argc - argument > 3) {
			fprintf(stderr, "Usage: emumonitor find PATTERN [VOLUME:FOLDER]\n");
			return 2;
		}
		if (argc - argument == 3)
			return Find(hostname, port, argv[argument + 1], argv[argument + 2]);
		return Find(hostname, port, argv[argument + 1], NULL);
	}
	if (strcmp(argv[argument], "cat") == 0) {
		if (argc - argument != 2) {
			fprintf(stderr, "Usage: emumonitor cat VOLUME:PATH\n");
			return 2;
		}
		return Cat(hostname, port, argv[argument + 1]);
	}
	if (strcmp(argv[argument], "fetch") == 0) {
		if (argc - argument < 2 || argc - argument > 3) {
			fprintf(stderr, "Usage: emumonitor fetch VOLUME:PATH [FILE.bin]\n");
			return 2;
		}
		if (argc - argument == 3)
			return Fetch(hostname, port, argv[argument + 1], argv[argument + 2]);
		return Fetch(hostname, port, argv[argument + 1], NULL);
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
