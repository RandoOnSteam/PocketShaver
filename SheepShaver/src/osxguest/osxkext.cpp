/*
 *  osxkext.cpp - OSX KEXT
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
#include "sysdeps.h"
#include "osxmachine.h"

#define LZSSRING 4096
#define LZSSMAXMATCH 18
#define LZSSTHRESHOLD 2
#define KMODNAME 12
#define KMODADDRESS 148
#define KMODMAXCOUNT 512

static uint32 Big32(const uint8 *data)
{
	return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) | ((uint32)data[2] << 8) | data[3];
}

static uint8 *Decompress(const uint8 *source, uint32 sourcelength, uint32 length)
{
	uint8 ring[LZSSRING];
	uint8 *output;
	uint32 flags;
	uint32 position;
	uint32 used;
	uint32 match;
	uint32 count;
	uint32 index;
	uint32 ringposition;
	uint8 value;
	output = (uint8 *)malloc(length + 1);
	if (output == NULL)
		return NULL;
	memset(ring, ' ', sizeof(ring));
	ringposition = LZSSRING - LZSSMAXMATCH;
	flags = 0;
	position = 0;
	used = 0;
	while (position < sourcelength && used < length) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			flags = source[position++] | 0xff00;
			if (position >= sourcelength)
				break;
		}
		if (flags & 1) {
			value = source[position++];
			output[used++] = value;
			ring[ringposition] = value;
			ringposition = (ringposition + 1) & (LZSSRING - 1);
			continue;
		}
		if (position + 1 >= sourcelength)
			break;
		match = source[position] | ((uint32)(source[position + 1] & 0xf0) << 4);
		count = (source[position + 1] & 0x0f) + LZSSTHRESHOLD;
		position += 2;
		for (index = 0; index <= count && used < length; index++) {
			value = ring[(match + index) & (LZSSRING - 1)];
			output[used++] = value;
			ring[ringposition] = value;
			ringposition = (ringposition + 1) & (LZSSRING - 1);
		}
	}
	return output;
}

static uint8 *ExtractEntry(const uint8 *mkext, uint32 mkextsize, uint32 offset, uint32 compressed, uint32 real)
{
	uint8 *copy;
	if (compressed && offset + compressed > mkextsize)
		return NULL;
	if (compressed)
		return Decompress(mkext + offset, compressed, real);
	if (offset + real > mkextsize)
		return NULL;
	copy = (uint8 *)malloc(real + 1);
	memcpy(copy, mkext + offset, real);
	return copy;
}

static int PlistHasBundle(const uint8 *plist, uint32 length, const char *bundle)
{
	char pattern[256];
	const char *text;
	const char *found;
	sprintf(pattern, "<string>%s</string>", bundle);
	text = (const char *)plist;
	found = strstr(text, "CFBundleIdentifier");
	if (found == NULL || (uint32)(found - text) >= length)
		return 0;
	found = strstr(found, "<string>");
	return found != NULL && strncmp(found, pattern, strlen(pattern)) == 0;
}

static int ObjectSymbol(const uint8 *object, uint32 size, const char *symbol, uint32 *offset)
{
	uint32 commands;
	uint32 position;
	uint32 index;
	uint32 command;
	uint32 sections;
	uint32 section;
	uint32 sectionbase;
	uint32 textaddress;
	uint32 textindex;
	uint32 symoffset;
	uint32 symcount;
	uint32 stroffset;
	uint32 strsize;
	uint32 entry;
	uint32 stringindex;
	uint32 sectioncounter;
	if (size < 28 || Big32(object) != 0xfeedface)
		return 0;
	commands = Big32(object + 16);
	position = 28;
	textaddress = 0;
	textindex = 0;
	sectioncounter = 0;
	symcount = 0;
	symoffset = 0;
	stroffset = 0;
	strsize = 0;
	for (index = 0; index < commands && position + 8 <= size; index++) {
		command = Big32(object + position);
		if (command == 1) {
			sections = Big32(object + position + 48);
			sectionbase = position + 56;
			for (section = 0; section < sections; section++) {
				sectioncounter++;
				if (strncmp((const char *)object + sectionbase + section * 68, "__text", 16) == 0) {
					textaddress = Big32(object + sectionbase + section * 68 + 32);
					textindex = sectioncounter;
				}
			}
		} else if (command == 2) {
			symoffset = Big32(object + position + 8);
			symcount = Big32(object + position + 12);
			stroffset = Big32(object + position + 16);
			strsize = Big32(object + position + 20);
		}
		position += Big32(object + position + 4);
	}
	for (entry = 0; entry < symcount && symoffset + entry * 12 + 12 <= size; entry++) {
		stringindex = Big32(object + symoffset + entry * 12);
		if (stringindex >= strsize || object[symoffset + entry * 12 + 5] != textindex)
			continue;
		if (strcmp((const char *)object + stroffset + stringindex, symbol) == 0) {
			*offset = Big32(object + symoffset + entry * 12 + 8) - textaddress;
			return 1;
		}
	}
	return 0;
}

static void StoreBig32(uint8 *data, uint32 value)
{
	data[0] = (uint8)(value >> 24);
	data[1] = (uint8)(value >> 16);
	data[2] = (uint8)(value >> 8);
	data[3] = (uint8)value;
}

static int FindBundleEntry(const uint8 *mkext, uint32 size, const char *bundle)
{
	const uint8 *entry;
	uint8 *plist;
	uint32 count;
	uint32 index;
	int found;
	if (mkext == NULL || size < 32)
		return -1;
	count = Big32(mkext + 20);
	for (index = 0; index < count && 64 + index * 32 <= size; index++) {
		entry = mkext + 32 + index * 32;
		plist = ExtractEntry(mkext, size, Big32(entry), Big32(entry + 4), Big32(entry + 8));
		if (plist == NULL)
			continue;
		plist[Big32(entry + 8)] = 0;
		found = PlistHasBundle(plist, Big32(entry + 8), bundle);
		free(plist);
		if (found)
			return (int)index;
	}
	return -1;
}

static int SymbolString(const uint8 *object, uint32 size, uint32 symoff, uint32 stroff, uint32 strsize, uint32 index, const char *name)
{
	uint32 strx;
	strx = Big32(object + symoff + index * 12);
	if (strx >= strsize || stroff + strx >= size)
		return 0;
	return strncmp((const char *)object + stroff + strx, name, size - stroff - strx) == 0;
}

static void ApplySymbolAliases(uint8 *object, uint32 size, const char *const *aliases, int aliascount)
{
	uint32 commands;
	uint32 position;
	uint32 symoff;
	uint32 nsyms;
	uint32 stroff;
	uint32 strsize;
	uint32 from;
	uint32 to;
	int alias;
	if (size < 28 || Big32(object) != 0xfeedface)
		return;
	commands = Big32(object + 16);
	position = 28;
	nsyms = 0;
	while (commands-- && position + 24 <= size && Big32(object + position) != 2)
		position += Big32(object + position + 4);
	if (position + 24 > size || Big32(object + position) != 2)
		return;
	symoff = Big32(object + position + 8);
	nsyms = Big32(object + position + 12);
	stroff = Big32(object + position + 16);
	strsize = Big32(object + position + 20);
	if (symoff + nsyms * 12 > size || stroff + strsize > size)
		return;
	for (alias = 0; alias < aliascount; alias++) {
		for (from = 0; from < nsyms && !SymbolString(object, size, symoff, stroff, strsize, from, aliases[alias * 2]); from++)
			;
		for (to = 0; to < nsyms && !SymbolString(object, size, symoff, stroff, strsize, to, aliases[alias * 2 + 1]); to++)
			;
		if (from < nsyms && to < nsyms)
			StoreBig32(object + symoff + from * 12, Big32(object + symoff + to * 12));
	}
}

static uint32 StoredLength(const uint8 *field)
{
	if (Big32(field + 4))
		return Big32(field + 4);
	return Big32(field + 8);
}

uint32 OsxAdler32(const uint8 *data, uint32 length)
{
	uint32 low;
	uint32 high;
	uint32 index;
	low = 1;
	high = 0;
	for (index = 0; index < length; index++) {
		low = (low + data[index]) % 65521;
		high = (high + low) % 65521;
	}
	return (high << 16) | low;
}

int OsxMkextHasBundle(const uint8 *mkext, uint32 size, const char *bundle)
{
	return FindBundleEntry(mkext, size, bundle) >= 0;
}

uint8 *OsxMkextAddBundles(const uint8 *base, uint32 basesize, const uint8 *donor, uint32 donorsize, const char *const *bundles, int bundlecount, const char *const *aliases, int aliascount, uint32 *size)
{
	uint8 *module;
	const uint8 *source;
	uint8 *output;
	uint8 *target;
	int *picked;
	int pickedcount;
	int index;
	int entry;
	uint32 basecount;
	uint32 table;
	uint32 shift;
	uint32 total;
	uint32 position;
	uint32 length;
	uint32 field;
	basecount = Big32(base + 20);
	table = 32 + 32 * basecount;
	if (table > basesize)
		return NULL;
	picked = (int *)malloc(bundlecount * sizeof(int));
	pickedcount = 0;
	total = basesize;
	for (index = 0; index < bundlecount; index++) {
		if (FindBundleEntry(base, basesize, bundles[index]) >= 0)
			continue;
		entry = FindBundleEntry(donor, donorsize, bundles[index]);
		if (entry < 0)
			continue;
		source = donor + 32 + entry * 32;
		for (field = 0; field < 32; field += 16) {
			length = StoredLength(source + field);
			if (Big32(source + field + 8) && Big32(source + field) <= donorsize && length <= donorsize - Big32(source + field))
				total += Big32(source + field + 8);
		}
		picked[pickedcount++] = entry;
	}
	if (pickedcount == 0) {
		free(picked);
		return NULL;
	}
	shift = 32 * (uint32)pickedcount;
	total += shift;
	output = (uint8 *)calloc(total, 1);
	memcpy(output, base, table);
	memcpy(output + table + shift, base + table, basesize - table);
	for (index = 0; index < (int)basecount; index++) {
		target = output + 32 + index * 32;
		for (field = 0; field < 32; field += 16)
			if (Big32(target + field + 8))
				StoreBig32(target + field, Big32(target + field) + shift);
	}
	position = basesize + shift;
	for (index = 0; index < pickedcount; index++) {
		source = donor + 32 + picked[index] * 32;
		target = output + table + index * 32;
		memcpy(target, source, 32);
		for (field = 0; field < 32; field += 16) {
			length = StoredLength(source + field);
			if (!Big32(source + field + 8) || Big32(source + field) > donorsize || length > donorsize - Big32(source + field)) {
				memset(target + field, 0, 16);
				continue;
			}
			module = ExtractEntry(donor, donorsize, Big32(source + field), Big32(source + field + 4), Big32(source + field + 8));
			if (module == NULL) {
				memset(target + field, 0, 16);
				continue;
			}
			length = Big32(source + field + 8);
			if (field == 16)
				ApplySymbolAliases(module, length, aliases, aliascount);
			memcpy(output + position, module, length);
			free(module);
			StoreBig32(target + field, position);
			StoreBig32(target + field + 4, 0);
			position += length;
		}
	}
	StoreBig32(output + 8, total);
	StoreBig32(output + 20, basecount + (uint32)pickedcount);
	StoreBig32(output + 12, OsxAdler32(output + 16, total - 16));
	*size = total;
	free(picked);
	return output;
}

int OsxKextSymbolOffset(OsxMachine *machine, const char *bundle, const char *symbol, uint32 *offset)
{
	const uint8 *mkext;
	const uint8 *entry;
	uint8 *plist;
	uint8 *object;
	uint32 count;
	uint32 index;
	int found;
	mkext = machine->mkext;
	if (mkext == NULL || machine->mkextsize < 32)
		return 0;
	count = Big32(mkext + 20);
	found = 0;
	for (index = 0; index < count && !found; index++) {
		entry = mkext + 32 + index * 32;
		plist = ExtractEntry(mkext, machine->mkextsize, Big32(entry), Big32(entry + 4), Big32(entry + 8));
		if (plist == NULL)
			continue;
		plist[Big32(entry + 8)] = 0;
		if (PlistHasBundle(plist, Big32(entry + 8), bundle) && Big32(entry + 16)) {
			object = ExtractEntry(mkext, machine->mkextsize, Big32(entry + 16), Big32(entry + 20), Big32(entry + 24));
			if (object) {
				found = ObjectSymbol(object, Big32(entry + 24), symbol, offset);
				free(object);
			}
		}
		free(plist);
	}
	return found;
}

static int ReadVirtualBytes(OsxCpu *cpu, uint32 address, uint8 *output, uint32 length)
{
	uint32 word;
	uint32 index;
	for (index = 0; index < length; index++) {
		if (!OsxCpuReadVirtual(cpu, (address + index) & ~3u, &word))
			return 0;
		output[index] = (uint8)(word >> (24 - ((address + index) & 3) * 8));
	}
	return 1;
}

int OsxKextTextAddress(OsxMachine *machine, const char *bundle, uint32 *address)
{
	OsxCpu *cpu;
	uint8 header[4096];
	char name[65];
	uint32 kmod;
	uint32 base;
	uint32 commands;
	uint32 position;
	uint32 index;
	uint32 sections;
	uint32 section;
	int guard;
	cpu = machine->cpu;
	if (!OsxCpuReadVirtual(cpu, OsxFindKernelSymbol(machine, "_kmod"), &kmod))
		return 0;
	for (guard = 0; kmod != 0 && guard < KMODMAXCOUNT; guard++) {
		if (!ReadVirtualBytes(cpu, kmod + KMODNAME, (uint8 *)name, 64))
			return 0;
		name[64] = 0;
		if (strcmp(name, bundle) == 0) {
			if (!OsxCpuReadVirtual(cpu, kmod + KMODADDRESS, &base) || base == 0)
				return 0;
			if (!ReadVirtualBytes(cpu, base, header, sizeof(header)) || Big32(header) != 0xfeedface)
				return 0;
			commands = Big32(header + 16);
			position = 28;
			for (index = 0; index < commands && position + 56 <= sizeof(header); index++) {
				if (Big32(header + position) == 1) {
					sections = Big32(header + position + 48);
					for (section = 0; section < sections && position + 56 + section * 68 + 68 <= sizeof(header); section++) {
						if (strncmp((const char *)header + position + 56 + section * 68, "__text", 16) == 0) {
							*address = Big32(header + position + 56 + section * 68 + 32);
							return 1;
						}
					}
				}
				position += Big32(header + position + 4);
			}
			return 0;
		}
		if (!OsxCpuReadVirtual(cpu, kmod, &kmod))
			return 0;
	}
	return 0;
}
