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
