#include "sysdeps.h"
#include "sys.h"
#include "prefs.h"
#include "osxmachine.h"

#define HFSFOLDERRECORD 1
#define HFSFILERECORD 2
#define HFSROOTFOLDER 2
#define HFSMAXEXTENTS 512
#define HFSMAXNAME 255
#define BOOTPAGE 4096
#define BOOTMAXRANGES 64
#define BOOTARGSSIZE 0x200
#define TREENAMELENGTH 32

typedef struct OsxHfsExtent {
	uint32 start;
	uint32 count;
} OsxHfsExtent;

typedef struct OsxHfsFork {
	uint64 logical;
	uint32 totalblocks;
	uint32 fileid;
	uint8 forktype;
	int extentcount;
	OsxHfsExtent extents[HFSMAXEXTENTS];
} OsxHfsFork;

typedef struct OsxHfsVolume {
	void *file;
	loff_t base;
	int partition;
	uint32 blocksize;
	OsxHfsFork catalog;
	OsxHfsFork overflow;
	uint32 catalognodesize;
	uint32 catalogroot;
	uint32 overflownodesize;
	uint32 overflowfirstleaf;
	uint8 *node;
} OsxHfsVolume;

typedef struct OsxTreeProperty {
	char name[TREENAMELENGTH];
	uint32 length;
	uint8 *value;
	struct OsxTreeProperty *next;
} OsxTreeProperty;

typedef struct OsxTreeNode {
	OsxTreeProperty *firstproperty;
	OsxTreeProperty *lastproperty;
	int propertycount;
	struct OsxTreeNode *firstchild;
	struct OsxTreeNode *lastchild;
	struct OsxTreeNode *next;
	int childcount;
} OsxTreeNode;

typedef struct OsxMemoryRange {
	char name[TREENAMELENGTH];
	uint32 address;
	uint32 length;
} OsxMemoryRange;

typedef struct OsxBootState {
	OsxMachine *machine;
	OsxMemoryRange ranges[BOOTMAXRANGES];
	int rangecount;
	uint32 nextfree;
	uint32 phandle;
	char rootpath[128];
} OsxBootState;

static uint32 Big32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static uint32 Big16(const uint8 *data)
{
	return ((uint32)data[0] << 8) | data[1];
}

static void StoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static int ReadVolume(OsxHfsVolume *volume, loff_t offset, void *buffer, uint32 length)
{
	return Sys_read(volume->file, buffer, volume->base + offset, length) == length;
}

static void ParseFork(const uint8 *data, uint32 fileid, uint8 forktype, OsxHfsFork *fork)
{
	int index;
	fork->logical = ((uint64)Big32(data) << 32) | Big32(data + 4);
	fork->totalblocks = Big32(data + 12);
	fork->fileid = fileid;
	fork->forktype = forktype;
	fork->extentcount = 0;
	for (index = 0; index < 8; index++) {
		fork->extents[index].start = Big32(data + 16 + index * 8);
		fork->extents[index].count = Big32(data + 20 + index * 8);
		if (fork->extents[index].count)
			fork->extentcount = index + 1;
	}
}

static int ReadFork(OsxHfsVolume *volume, OsxHfsFork *fork, uint64 offset, uint8 *buffer, uint32 length)
{
	uint64 position;
	uint64 extentbytes;
	uint64 inner;
	uint32 take;
	int index;
	position = 0;
	for (index = 0; index < fork->extentcount && length > 0; index++) {
		extentbytes = (uint64)fork->extents[index].count * volume->blocksize;
		if (offset < position + extentbytes) {
			inner = offset - position;
			take = length;
			if ((uint64)take > extentbytes - inner)
				take = (uint32)(extentbytes - inner);
			if (!ReadVolume(volume, (loff_t)fork->extents[index].start * volume->blocksize + (loff_t)inner, buffer, take))
				return 0;
			buffer += take;
			offset += take;
			length -= take;
		}
		position += extentbytes;
	}
	return length == 0;
}

static uint32 BlocksInFork(OsxHfsFork *fork)
{
	uint32 total;
	int index;
	total = 0;
	for (index = 0; index < fork->extentcount; index++)
		total += fork->extents[index].count;
	return total;
}

static void CompleteFork(OsxHfsVolume *volume, OsxHfsFork *fork)
{
	uint32 node;
	uint32 records;
	uint32 record;
	uint32 offset;
	uint32 have;
	uint32 keylength;
	uint32 keystart;
	uint32 index;
	uint8 *data;
	int progress;
	have = BlocksInFork(fork);
	while (have < fork->totalblocks && volume->overflowfirstleaf) {
		progress = 0;
		node = volume->overflowfirstleaf;
		while (node && !progress) {
			if (!ReadFork(volume, &volume->overflow, (uint64)node * volume->overflownodesize, volume->node, volume->overflownodesize))
				return;
			data = volume->node;
			records = Big16(data + 10);
			for (record = 0; record < records && !progress; record++) {
				offset = Big16(data + volume->overflownodesize - 2 * (record + 1));
				keylength = Big16(data + offset);
				if (data[offset + 2] != fork->forktype || Big32(data + offset + 4) != fork->fileid)
					continue;
				keystart = Big32(data + offset + 8);
				if (keystart != have)
					continue;
				for (index = 0; index < 8 && fork->extentcount < HFSMAXEXTENTS; index++) {
					fork->extents[fork->extentcount].start = Big32(data + offset + 2 + keylength + index * 8);
					fork->extents[fork->extentcount].count = Big32(data + offset + 2 + keylength + index * 8 + 4);
					if (fork->extents[fork->extentcount].count)
						fork->extentcount++;
				}
				progress = 1;
			}
			node = Big32(data);
		}
		if (!progress)
			return;
		have = BlocksInFork(fork);
	}
}

static uint32 FoldCharacter(uint32 character)
{
	if (character >= 'A' && character <= 'Z')
		return character + 32;
	return character;
}

static int CompareCatalogKey(const uint8 *key, uint32 parent, const uint16 *name, uint32 namelength)
{
	uint32 keyparent;
	uint32 keynamelength;
	uint32 index;
	uint32 left;
	uint32 right;
	keyparent = Big32(key + 2);
	if (keyparent < parent)
		return -1;
	if (keyparent > parent)
		return 1;
	keynamelength = Big16(key + 6);
	for (index = 0; index < keynamelength && index < namelength; index++) {
		left = FoldCharacter(Big16(key + 8 + index * 2));
		right = FoldCharacter(name[index]);
		if (left < right)
			return -1;
		if (left > right)
			return 1;
	}
	if (keynamelength < namelength)
		return -1;
	if (keynamelength > namelength)
		return 1;
	return 0;
}

static int FindCatalogRecord(OsxHfsVolume *volume, uint32 parent, const char *text, uint8 *record)
{
	uint16 name[HFSMAXNAME];
	uint32 namelength;
	uint32 node;
	uint32 records;
	uint32 index;
	uint32 offset;
	uint32 child;
	uint32 keylength;
	int depth;
	int kind;
	int found;
	uint8 *data;
	namelength = 0;
	while (text[namelength] && namelength < HFSMAXNAME) {
		name[namelength] = (uint8)text[namelength];
		namelength++;
	}
	node = volume->catalogroot;
	for (depth = 0; depth < 16; depth++) {
		if (!ReadFork(volume, &volume->catalog, (uint64)node * volume->catalognodesize, volume->node, volume->catalognodesize))
			return 0;
		data = volume->node;
		kind = (int)(signed char)data[8];
		records = Big16(data + 10);
		if (kind == -1) {
			for (index = 0; index < records; index++) {
				offset = Big16(data + volume->catalognodesize - 2 * (index + 1));
				if (CompareCatalogKey(data + offset, parent, name, namelength) == 0) {
					keylength = Big16(data + offset);
					memcpy(record, data + offset + 2 + keylength, 248);
					return 1;
				}
			}
			return 0;
		}
		if (kind != 0)
			return 0;
		child = 0;
		found = 0;
		for (index = 0; index < records; index++) {
			offset = Big16(data + volume->catalognodesize - 2 * (index + 1));
			if (CompareCatalogKey(data + offset, parent, name, namelength) > 0)
				break;
			keylength = Big16(data + offset);
			child = Big32(data + offset + 2 + keylength);
			found = 1;
		}
		if (!found)
			return 0;
		node = child;
	}
	return 0;
}

static int OpenVolume(OsxHfsVolume *volume, void *file)
{
	uint8 block[512];
	uint32 index;
	uint32 allocationblock;
	uint32 firstblock;
	uint32 embedstart;
	volume->file = file;
	volume->base = 0;
	volume->partition = 0;
	volume->node = NULL;
	if (Sys_read(file, block, 0, 512) != 512)
		return 0;
	if (block[0] == 'E' && block[1] == 'R') {
		for (index = 1; index < 64; index++) {
			if (Sys_read(file, block, (loff_t)index * 512, 512) != 512 || block[0] != 'P' || block[1] != 'M')
				return 0;
			if (strncmp((const char *)block + 48, "Apple_HFS", 32) == 0) {
				volume->base = (loff_t)Big32(block + 8) * 512;
				volume->partition = (int)index;
				break;
			}
		}
	}
	if (!ReadVolume(volume, 1024, block, 512))
		return 0;
	if (block[0] == 'B' && block[1] == 'D') {
		allocationblock = Big32(block + 20);
		firstblock = Big16(block + 28);
		embedstart = Big16(block + 126);
		volume->base += (loff_t)firstblock * 512 + (loff_t)embedstart * allocationblock;
		if (!ReadVolume(volume, 1024, block, 512))
			return 0;
	}
	if (!(block[0] == 'H' && (block[1] == '+' || block[1] == 'X')))
		return 0;
	volume->blocksize = Big32(block + 40);
	ParseFork(block + 0xc0, 3, 0, &volume->overflow);
	ParseFork(block + 0x110, 4, 0, &volume->catalog);
	volume->node = (uint8 *)malloc(65536);
	if (!ReadFork(volume, &volume->overflow, 0, volume->node, 512))
		return 0;
	volume->overflownodesize = Big16(volume->node + 32);
	volume->overflowfirstleaf = Big32(volume->node + 24);
	if (volume->overflow.extentcount == 0)
		volume->overflowfirstleaf = 0;
	if (!ReadFork(volume, &volume->catalog, 0, volume->node, 512))
		return 0;
	volume->catalognodesize = Big16(volume->node + 32);
	volume->catalogroot = Big32(volume->node + 16);
	CompleteFork(volume, &volume->catalog);
	return 1;
}

static void CloseVolume(OsxHfsVolume *volume)
{
	free(volume->node);
}

static uint8 *ReadFile(OsxHfsVolume *volume, const char *path, uint32 *size)
{
	uint8 record[248];
	char component[HFSMAXNAME + 1];
	OsxHfsFork *fork;
	uint8 *data;
	uint32 parent;
	uint32 length;
	const char *cursor;
	parent = HFSROOTFOLDER;
	cursor = path;
	memset(record, 0, sizeof(record));
	while (*cursor) {
		while (*cursor == '/')
			cursor++;
		length = 0;
		while (cursor[length] && cursor[length] != '/' && length < HFSMAXNAME) {
			component[length] = cursor[length];
			length++;
		}
		component[length] = 0;
		cursor += length;
		if (length == 0)
			break;
		if (!FindCatalogRecord(volume, parent, component, record))
			return NULL;
		parent = Big32(record + 8);
	}
	if (Big16(record) != HFSFILERECORD)
		return NULL;
	fork = (OsxHfsFork *)malloc(sizeof(OsxHfsFork));
	ParseFork(record + 88, Big32(record + 8), 0, fork);
	CompleteFork(volume, fork);
	data = (uint8 *)malloc((size_t)fork->logical + 1);
	if (data == NULL || !ReadFork(volume, fork, 0, data, (uint32)fork->logical)) {
		free(data);
		free(fork);
		return NULL;
	}
	*size = (uint32)fork->logical;
	free(fork);
	return data;
}

static void AddRange(OsxBootState *state, const char *name, uint32 address, uint32 length)
{
	OsxMemoryRange *range;
	if (state->rangecount >= BOOTMAXRANGES)
		return;
	range = &state->ranges[state->rangecount++];
	memset(range->name, 0, sizeof(range->name));
	strncpy(range->name, name, TREENAMELENGTH - 1);
	range->address = address;
	range->length = length;
}

static uint32 Allocate(OsxBootState *state, uint32 length)
{
	uint32 address;
	address = state->nextfree;
	state->nextfree = (address + length + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	return address;
}

static int LoadKernel(OsxBootState *state, const uint8 *image, uint32 size)
{
	OsxMachine *machine;
	char rangename[TREENAMELENGTH];
	char segment[17];
	uint32 commands;
	uint32 offset;
	uint32 command;
	uint32 commandsize;
	uint32 vmaddr;
	uint32 vmsize;
	uint32 fileoffset;
	uint32 filesize;
	uint32 index;
	uint32 symoffset;
	uint32 symcount;
	uint32 stroffset;
	uint32 strsize;
	uint32 address;
	uint32 total;
	uint32 kernelend;
	uint8 *target;
	machine = state->machine;
	if (size < 28 || Big32(image) != 0xfeedface)
		return 0;
	commands = Big32(image + 16);
	offset = 28;
	kernelend = 0;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		command = Big32(image + offset);
		commandsize = Big32(image + offset + 4);
		if (command == 1) {
			memset(segment, 0, sizeof(segment));
			memcpy(segment, image + offset + 8, 16);
			vmaddr = Big32(image + offset + 24);
			vmsize = Big32(image + offset + 28);
			fileoffset = Big32(image + offset + 32);
			filesize = Big32(image + offset + 36);
			if (vmsize && vmaddr + vmsize <= machine->ramsize) {
				target = machine->ram + vmaddr;
				if (filesize && fileoffset + filesize <= size)
					memcpy(target, image + fileoffset, filesize);
				if (filesize < vmsize)
					memset(target + filesize, 0, vmsize - filesize);
				sprintf(rangename, "Kernel-%s", segment);
				AddRange(state, rangename, vmaddr, vmsize);
				if (vmaddr + vmsize > kernelend)
					kernelend = vmaddr + vmsize;
			}
		} else if (command == 5) {
			machine->kernelentry = Big32(image + offset + 16);
		} else if (command == 2) {
			symoffset = Big32(image + offset + 8);
			symcount = Big32(image + offset + 12);
			stroffset = Big32(image + offset + 16);
			strsize = Big32(image + offset + 20);
		}
		offset += commandsize;
	}
	state->nextfree = (kernelend + BOOTPAGE - 1) & ~(uint32)(BOOTPAGE - 1);
	total = 28 + Big32(image + 20);
	address = Allocate(state, total);
	memcpy(machine->ram + address, image, total);
	AddRange(state, "Kernel-__HEADER", address, total);
	if (symcount && symoffset + symcount * 12 <= size && stroffset + strsize <= size) {
		total = 24 + symcount * 12 + strsize;
		address = Allocate(state, total);
		target = machine->ram + address;
		StoreBig32(target, 2);
		StoreBig32(target + 4, 24);
		StoreBig32(target + 8, address + 24);
		StoreBig32(target + 12, symcount);
		StoreBig32(target + 16, address + 24 + symcount * 12);
		StoreBig32(target + 20, strsize);
		memcpy(target + 24, image + symoffset, symcount * 12);
		memcpy(target + 24 + symcount * 12, image + stroffset, strsize);
		AddRange(state, "Kernel-__SYMTAB", address, total);
	}
	return machine->kernelentry != 0;
}

uint32 OsxFindKernelSymbol(OsxMachine *machine, const char *name)
{
	const uint8 *image;
	uint32 size;
	uint32 commands;
	uint32 offset;
	uint32 index;
	uint32 symoffset;
	uint32 symcount;
	uint32 stroffset;
	uint32 strsize;
	uint32 symbol;
	uint32 stringindex;
	image = machine->kernelimage;
	size = machine->kernelsize;
	if (image == NULL || size < 28)
		return 0;
	commands = Big32(image + 16);
	offset = 28;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && offset + 8 <= size; index++) {
		if (Big32(image + offset) == 2) {
			symoffset = Big32(image + offset + 8);
			symcount = Big32(image + offset + 12);
			stroffset = Big32(image + offset + 16);
			strsize = Big32(image + offset + 20);
		}
		offset += Big32(image + offset + 4);
	}
	for (symbol = 0; symbol < symcount; symbol++) {
		stringindex = Big32(image + symoffset + symbol * 12);
		if (stringindex < strsize && strcmp((const char *)image + stroffset + stringindex, name) == 0)
			return Big32(image + symoffset + symbol * 12 + 8);
	}
	return 0;
}

static OsxTreeNode *NewNode(OsxTreeNode *parent, const char *name)
{
	OsxTreeNode *node;
	OsxTreeProperty *property;
	node = (OsxTreeNode *)calloc(1, sizeof(OsxTreeNode));
	property = (OsxTreeProperty *)calloc(1, sizeof(OsxTreeProperty));
	strcpy(property->name, "name");
	property->length = (uint32)strlen(name) + 1;
	property->value = (uint8 *)malloc(property->length);
	memcpy(property->value, name, property->length);
	node->firstproperty = property;
	node->lastproperty = property;
	node->propertycount = 1;
	if (parent) {
		if (parent->lastchild)
			parent->lastchild->next = node;
		else
			parent->firstchild = node;
		parent->lastchild = node;
		parent->childcount++;
	}
	return node;
}

static void AddProperty(OsxTreeNode *node, const char *name, const void *value, uint32 length)
{
	OsxTreeProperty *property;
	property = (OsxTreeProperty *)calloc(1, sizeof(OsxTreeProperty));
	strncpy(property->name, name, TREENAMELENGTH - 1);
	property->length = length;
	property->value = (uint8 *)malloc(length + 1);
	if (length)
		memcpy(property->value, value, length);
	node->lastproperty->next = property;
	node->lastproperty = property;
	node->propertycount++;
}

static void AddString(OsxTreeNode *node, const char *name, const char *value)
{
	AddProperty(node, name, value, (uint32)strlen(value) + 1);
}

static void AddStrings(OsxTreeNode *node, const char *name, const char *first, const char *second)
{
	char buffer[256];
	uint32 length;
	length = (uint32)strlen(first) + 1;
	memcpy(buffer, first, length);
	memcpy(buffer + length, second, strlen(second) + 1);
	length += (uint32)strlen(second) + 1;
	AddProperty(node, name, buffer, length);
}

static void AddCells(OsxTreeNode *node, const char *name, const uint32 *cells, int count)
{
	uint8 buffer[256];
	int index;
	for (index = 0; index < count; index++)
		StoreBig32(buffer + index * 4, cells[index]);
	AddProperty(node, name, buffer, (uint32)count * 4);
}

static void AddCell(OsxTreeNode *node, const char *name, uint32 value)
{
	AddCells(node, name, &value, 1);
}

static OsxTreeProperty *FindProperty(OsxTreeNode *node, const char *name)
{
	OsxTreeProperty *property;
	for (property = node->firstproperty; property; property = property->next)
		if (strcmp(property->name, name) == 0)
			return property;
	return NULL;
}

static void AddPhandles(OsxTreeNode *node, uint32 *phandle)
{
	OsxTreeNode *child;
	AddCell(node, "AAPL,phandle", (*phandle)++);
	for (child = node->firstchild; child; child = child->next)
		AddPhandles(child, phandle);
}

static uint32 TreeSize(OsxTreeNode *node)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 size;
	size = 8;
	for (property = node->firstproperty; property; property = property->next)
		size += TREENAMELENGTH + 4 + ((property->length + 3) & ~3u);
	for (child = node->firstchild; child; child = child->next)
		size += TreeSize(child);
	return size;
}

static uint8 *FlattenTree(OsxTreeNode *node, uint8 *output)
{
	OsxTreeProperty *property;
	OsxTreeNode *child;
	uint32 padded;
	StoreBig32(output, (uint32)node->propertycount);
	StoreBig32(output + 4, (uint32)node->childcount);
	output += 8;
	for (property = node->firstproperty; property; property = property->next) {
		memset(output, 0, TREENAMELENGTH);
		memcpy(output, property->name, strlen(property->name));
		StoreBig32(output + TREENAMELENGTH, property->length);
		output += TREENAMELENGTH + 4;
		padded = (property->length + 3) & ~3u;
		memset(output, 0, padded);
		if (property->length)
			memcpy(output, property->value, property->length);
		output += padded;
	}
	for (child = node->firstchild; child; child = child->next)
		output = FlattenTree(child, output);
	return output;
}

static void DestroyTree(OsxTreeNode *node)
{
	OsxTreeProperty *property;
	OsxTreeProperty *nextproperty;
	OsxTreeNode *child;
	OsxTreeNode *nextchild;
	for (property = node->firstproperty; property; property = nextproperty) {
		nextproperty = property->next;
		free(property->value);
		free(property);
	}
	for (child = node->firstchild; child; child = nextchild) {
		nextchild = child->next;
		DestroyTree(child);
	}
	free(node);
}

static void AddAtaNode(OsxTreeNode *macio, const char *name, uint32 offset, uint32 dmaoffset, uint32 irq, uint32 dmairq)
{
	OsxTreeNode *node;
	uint32 cells[4];
	node = NewNode(macio, name);
	AddString(node, "device_type", "ata");
	AddString(node, "compatible", "heathrow-ata");
	AddString(node, "model", "ata-3");
	cells[0] = offset;
	cells[1] = 0x1000;
	cells[2] = dmaoffset;
	cells[3] = 0x100;
	AddCells(node, "reg", cells, 4);
	cells[0] = irq;
	cells[1] = dmairq;
	AddCells(node, "AAPL,interrupts", cells, 2);
	AddCells(node, "interrupts", cells, 2);
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
}

static OsxTreeNode *BuildTree(OsxBootState *state, OsxTreeNode **memorymap)
{
	OsxMachine *machine;
	OsxTreeNode *root;
	OsxTreeNode *node;
	OsxTreeNode *pci;
	OsxTreeNode *macio;
	OsxTreeNode *cuda;
	OsxTreeNode *chosen;
	uint32 cells[16];
	machine = state->machine;
	root = NewNode(NULL, "device-tree");
	AddString(root, "model", "AAPL,Gossamer");
	AddStrings(root, "compatible", "AAPL,Gossamer", "MacRISC");
	AddCell(root, "#address-cells", 1);
	AddCell(root, "#size-cells", 1);
	AddCell(root, "clock-frequency", OSXBUSFREQUENCY);
	AddString(root, "copyright", "Copyright 1983-1997 Apple Computer, Inc. All Rights Reserved");

	node = NewNode(root, "cpus");
	AddCell(node, "#address-cells", 1);
	AddCell(node, "#size-cells", 0);
	node = NewNode(node, "PowerPC,750");
	AddString(node, "device_type", "cpu");
	AddCell(node, "reg", 0);
	AddCell(node, "cpu-version", OSXPVR);
	AddCell(node, "clock-frequency", OSXCPUFREQUENCY);
	AddCell(node, "bus-frequency", OSXBUSFREQUENCY);
	AddCell(node, "timebase-frequency", OSXTIMEBASEFREQUENCY);
	AddCell(node, "d-cache-size", 0x8000);
	AddCell(node, "i-cache-size", 0x8000);
	AddCell(node, "d-cache-sets", 128);
	AddCell(node, "i-cache-sets", 128);
	AddCell(node, "d-cache-block-size", 32);
	AddCell(node, "i-cache-block-size", 32);
	AddCell(node, "reservation-granule-size", 32);
	AddCell(node, "tlb-size", 128);
	AddCell(node, "tlb-sets", 64);
	AddProperty(node, "graphics", "", 0);
	AddProperty(node, "external-control", "", 0);
	AddProperty(node, "general-purpose", "", 0);

	node = NewNode(root, "memory");
	AddString(node, "device_type", "memory");
	cells[0] = 0;
	cells[1] = machine->ramsize;
	AddCells(node, "reg", cells, 2);

	chosen = NewNode(root, "chosen");
	AddString(chosen, "bootpath", state->rootpath);
	AddString(chosen, "rootpath", state->rootpath);
	*memorymap = NewNode(chosen, "memory-map");

	node = NewNode(root, "nvram");
	AddString(node, "device_type", "nvram");
	AddStrings(node, "compatible", "nvram", "flash");
	cells[0] = OSXFLASHBASE;
	cells[1] = OSXFLASHSIZE;
	AddCells(node, "reg", cells, 2);
	AddCell(node, "#bytes", OSXFLASHSIZE / 2);
	AddString(node, "AAPL,unit-string", "fff80000");

	node = NewNode(root, "options");
	AddString(node, "boot-args", "");
	AddString(node, "auto-boot?", "true");

	node = NewNode(root, "aliases");
	AddString(node, "pci", "/pci@80000000");

	pci = NewNode(root, "pci");
	AddString(pci, "device_type", "pci");
	AddStrings(pci, "compatible", "grackle", "MOT,PPC106");
	AddString(pci, "model", "MOT,PPC106");
	cells[0] = 0x80000000;
	cells[1] = 0x7f000000;
	AddCells(pci, "reg", cells, 2);
	AddCell(pci, "#address-cells", 3);
	AddCell(pci, "#size-cells", 2);
	cells[0] = 0;
	cells[1] = 0;
	AddCells(pci, "bus-range", cells, 2);
	AddCell(pci, "clock-frequency", 33333333);
	cells[0] = 0x01000000;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0xfe000000;
	cells[4] = 0;
	cells[5] = 0x00c00000;
	cells[6] = 0x02000000;
	cells[7] = 0;
	cells[8] = 0x80000000;
	cells[9] = 0x80000000;
	cells[10] = 0;
	cells[11] = 0x7d000000;
	AddCells(pci, "ranges", cells, 12);
	AddString(pci, "AAPL,unit-string", "80000000");

	macio = NewNode(pci, "mac-io");
	AddString(macio, "device_type", "mac-io");
	AddString(macio, "compatible", "heathrow");
	AddString(macio, "model", "AAPL,343S1201");
	cells[0] = OSXPCIDEVICEHEATHROW << 11;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0;
	cells[4] = 0;
	cells[5] = 0x02000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[6] = 0;
	cells[7] = 0;
	cells[8] = 0;
	cells[9] = OSXHEATHROWSIZE;
	AddCells(macio, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[1] = 0;
	cells[2] = OSXHEATHROWBASE;
	cells[3] = 0;
	cells[4] = OSXHEATHROWSIZE;
	AddCells(macio, "assigned-addresses", cells, 5);
	cells[0] = 0;
	cells[1] = 0x82000010 | (OSXPCIDEVICEHEATHROW << 11);
	cells[2] = 0;
	cells[3] = OSXHEATHROWBASE;
	cells[4] = OSXHEATHROWSIZE;
	AddCells(macio, "ranges", cells, 5);
	AddCell(macio, "AAPL,address", OSXHEATHROWBASE);
	AddCell(macio, "vendor-id", 0x106b);
	AddCell(macio, "device-id", 0x0010);
	AddCell(macio, "revision-id", 1);
	AddCell(macio, "class-code", 0xff0000);
	AddCell(macio, "#address-cells", 1);
	AddCell(macio, "#size-cells", 1);
	AddString(macio, "AAPL,unit-string", "10");

	cuda = NewNode(macio, "via-cuda");
	AddString(cuda, "device_type", "via-cuda");
	cells[0] = 0x16000;
	cells[1] = 0x2000;
	AddCells(cuda, "reg", cells, 2);
	AddCell(cuda, "AAPL,interrupts", OSXIRQCUDA);
	AddCell(cuda, "interrupts", OSXIRQCUDA);
	node = NewNode(cuda, "adb");
	AddString(node, "device_type", "adb");
	AddString(node, "compatible", "adb");
	node = NewNode(cuda, "pram");
	AddString(node, "device_type", "pram");
	node = NewNode(cuda, "rtc");
	AddString(node, "device_type", "rtc");
	AddString(node, "compatible", "rtc");

	AddAtaNode(macio, "ata", 0x20000, 0x8b00, OSXIRQIDE0, OSXIRQIDE0DMA);
	AddAtaNode(macio, "ata", 0x21000, 0x8c00, OSXIRQIDE1, OSXIRQIDE1DMA);

	node = NewNode(pci, "display");
	AddString(node, "device_type", "display");
	AddString(node, "compatible", "display");
	AddString(node, "model", "SheepShaver,Framebuffer");
	cells[0] = OSXPCIDEVICEDISPLAY << 11;
	cells[1] = 0;
	cells[2] = 0;
	cells[3] = 0;
	cells[4] = 0;
	cells[5] = 0x02000010 | (OSXPCIDEVICEDISPLAY << 11);
	cells[6] = 0;
	cells[7] = 0;
	cells[8] = 0;
	cells[9] = OSXVRAMSIZE;
	AddCells(node, "reg", cells, 10);
	cells[0] = 0x82000010 | (OSXPCIDEVICEDISPLAY << 11);
	cells[1] = 0;
	cells[2] = OSXVRAMBASE;
	cells[3] = 0;
	cells[4] = OSXVRAMSIZE;
	AddCells(node, "assigned-addresses", cells, 5);
	AddCell(node, "vendor-id", 0x1234);
	AddCell(node, "device-id", 0x1111);
	AddCell(node, "revision-id", 2);
	AddCell(node, "class-code", 0x030000);
	AddCell(node, "address", OSXVRAMBASE);
	AddCell(node, "width", machine->width);
	AddCell(node, "height", machine->height);
	AddCell(node, "linebytes", machine->rowbytes);
	AddCell(node, "depth", machine->depth);
	AddProperty(node, "AAPL,boot-display", "", 0);
	AddString(node, "AAPL,unit-string", "12");
	return root;
}

static void InstallBootArgs(OsxBootState *state, const char *commandline, uint32 tree, uint32 treelength)
{
	OsxMachine *machine;
	uint8 *args;
	machine = state->machine;
	args = machine->ram + machine->bootargs;
	memset(args, 0, BOOTARGSSIZE);
	args[0] = 0;
	args[1] = 1;
	args[2] = 0;
	args[3] = 1;
	strncpy((char *)args + 4, commandline, 255);
	StoreBig32(args + 0x104, 0);
	StoreBig32(args + 0x108, machine->ramsize);
	StoreBig32(args + 0x1d4, OSXVRAMBASE);
	StoreBig32(args + 0x1d8, 1);
	StoreBig32(args + 0x1dc, machine->rowbytes);
	StoreBig32(args + 0x1e0, machine->width);
	StoreBig32(args + 0x1e4, machine->height);
	StoreBig32(args + 0x1e8, machine->depth);
	StoreBig32(args + 0x1ec, 0);
	StoreBig32(args + 0x1f0, tree);
	StoreBig32(args + 0x1f4, treelength);
	StoreBig32(args + 0x1f8, state->nextfree);
}

int OsxBootLoad(OsxMachine *machine, const char *imagepath, int bootbus, int bootunit, const char *commandline)
{
	OsxBootState state;
	OsxHfsVolume volume;
	OsxTreeNode *root;
	OsxTreeNode *memorymap;
	OsxTreeProperty *property;
	void *file;
	uint8 *kernel;
	uint8 *mkext;
	uint32 kernelsize;
	uint32 mkextsize;
	uint32 address;
	uint32 treesize;
	uint32 treeaddress;
	uint32 cells[2];
	char rangename[TREENAMELENGTH];
	int index;
	memset(&state, 0, sizeof(state));
	state.machine = machine;
	file = machine->ata[bootbus].drives[bootunit].file;
	if (!OpenVolume(&volume, file)) {
		fprintf(stderr, "osx: no HFS+ volume in %s\n", imagepath);
		CloseVolume(&volume);
		return 0;
	}
	sprintf(state.rootpath, "/pci@80000000/mac-io@10/ata@%x/@%d:%d", 0x20000 + bootbus * 0x1000, bootunit, volume.partition);
	kernel = ReadFile(&volume, "/mach_kernel", &kernelsize);
	if (kernel == NULL || !LoadKernel(&state, kernel, kernelsize)) {
		fprintf(stderr, "osx: cannot load /mach_kernel\n");
		CloseVolume(&volume);
		return 0;
	}
	mkext = ReadFile(&volume, "/System/Library/Extensions.mkext", &mkextsize);
	if (mkext) {
		address = Allocate(&state, mkextsize);
		memcpy(machine->ram + address, mkext, mkextsize);
		sprintf(rangename, "DriversPackage-%x", address);
		AddRange(&state, rangename, address, mkextsize);
		machine->mkext = mkext;
		machine->mkextsize = mkextsize;
	} else {
		fprintf(stderr, "osx: no Extensions.mkext, booting without drivers\n");
	}
	CloseVolume(&volume);
	machine->bootargs = Allocate(&state, BOOTARGSSIZE);
	AddRange(&state, "BootArgs", machine->bootargs, BOOTARGSSIZE);
	root = BuildTree(&state, &memorymap);
	cells[0] = 0;
	cells[1] = 0;
	AddProperty(memorymap, "DeviceTree", cells, 8);
	for (index = 0; index < state.rangecount; index++)
		AddProperty(memorymap, state.ranges[index].name, cells, 8);
	state.phandle = 0xff000001;
	AddPhandles(root, &state.phandle);
	treesize = TreeSize(root);
	treeaddress = Allocate(&state, treesize);
	AddRange(&state, "DeviceTree", treeaddress, treesize);
	for (index = 0; index < state.rangecount; index++) {
		property = FindProperty(memorymap, state.ranges[index].name);
		if (property) {
			StoreBig32(property->value, state.ranges[index].address);
			StoreBig32(property->value + 4, state.ranges[index].length);
		}
	}
	FlattenTree(root, machine->ram + treeaddress);
	DestroyTree(root);
	machine->kernelend = state.nextfree;
	InstallBootArgs(&state, commandline, treeaddress, treesize);
	machine->scratch = state.nextfree + 0x10000;
	if (machine->log) {
		fprintf(machine->log, "kernel entry %08x bootargs %08x tree %08x+%x top %08x\n", machine->kernelentry, machine->bootargs, treeaddress, treesize, state.nextfree);
		for (index = 0; index < state.rangecount; index++)
			fprintf(machine->log, "  %-24s %08x %08x\n", state.ranges[index].name, state.ranges[index].address, state.ranges[index].length);
	}
	machine->kernelimage = kernel;
	machine->kernelsize = kernelsize;
	return 1;
}
