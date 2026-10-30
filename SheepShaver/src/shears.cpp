/*
 *  shears.cpp - Sheep Shears host backend
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "thunks.h"
#include "shears.h"
#include "../../guest/sheepbench/sbkernels.h"

#define SHEARSPOLLTICKS 30
#define SHEARSCONTACTMS 3000
#define SHEARSCAPSHUTDOWN 2
#define SHEARSNATIVEBUDGETUS 20000
#define SHEARSREPORTFILE "SheepBench.txt"
#if defined(_WIN32)
#define SHEARSHOSTNAME "windows"
#elif defined(__APPLE__)
#define SHEARSHOSTNAME "macos"
#else
#define SHEARSHOSTNAME "linux"
#endif

static uint32 NowMilliseconds(void)
{
	return (uint32)(GetTicks_usec() / 1000) | 1;
}

static void ReplyWords(uint32 mailbox, int16 status, const uint32 *words, uint32 count)
{
	uint32 index;
	WriteMacInt32(mailbox + 16, (uint32)status);
	WriteMacInt32(mailbox + 20, count * 4);
	for (index = 0; index < count; index++)
		WriteMacInt32(mailbox + SHEARSHEADER + index * 4, words[index]);
}

static void ReplyText(uint32 mailbox, const char *text, uint32 length)
{
	WriteMacInt32(mailbox + 16, SHEARSOK);
	WriteMacInt32(mailbox + 20, length);
	Host2Mac_memcpy(mailbox + SHEARSHEADER, text, length);
}

static void CopyText(uint32 mailbox, uint32 length, char *text, uint32 capacity)
{
	if (length >= capacity)
		length = capacity - 1;
	Mac2Host_memcpy(text, mailbox + SHEARSHEADER, length);
	text[length] = 0;
}

static void Report(const char *text)
{
	FILE *file;
	file = fopen(SHEARSREPORTFILE, "a");
	if (file) {
		fputs(text, file);
		fclose(file);
	}
	printf("%s", text);
	fflush(stdout);
}

static void Native(uint32 mailbox)
{
	uint32 words[3];
	uint32 kernel;
	uint32 iterations;
	uint32 param;
	uint32 repeats;
	uint32 index;
	uint64 start;
	uint64 elapsed;
	uint64 picoseconds;
	void *scratch;
	kernel = ReadMacInt32(mailbox + SHEARSHEADER);
	iterations = ReadMacInt32(mailbox + SHEARSHEADER + 4);
	param = ReadMacInt32(mailbox + SHEARSHEADER + 8);
	if (kernel == 0 || kernel >= SBKERNELCOUNT || iterations == 0) {
		ReplyWords(mailbox, SHEARSUNSUPPORTED, NULL, 0);
		return;
	}
	scratch = malloc(SbScratchBytes(kernel, param) + 16);
	if (scratch == NULL) {
		ReplyWords(mailbox, SHEARSUNSUPPORTED, NULL, 0);
		return;
	}
	SbPrepare(kernel, param, scratch);
	words[0] = SbRun(kernel, iterations, param, scratch);
	repeats = 1;
	for (;;) {
		start = GetTicks_usec();
		for (index = 0; index < repeats; index++)
			SbRun(kernel, iterations, param, scratch);
		elapsed = GetTicks_usec() - start;
		if (elapsed >= SHEARSNATIVEBUDGETUS || repeats >= 0x10000000u)
			break;
		repeats *= 2;
	}
	free(scratch);
	picoseconds = elapsed * 1000000 / ((uint64)repeats * iterations);
	words[1] = (uint32)(picoseconds >> 32);
	words[2] = (uint32)picoseconds;
	ReplyWords(mailbox, SHEARSOK, words, 3);
}

static void Info(uint32 mailbox)
{
	char text[512];
	int length;
	length = snprintf(text, sizeof(text), "version=1\npvr=%08x\ntimebase=%lld\nram=%u\njit=%d\nhost=%s\n", PVR,
		(long long)TimebaseSpeed, RAMSize, (int)PrefsFindBool("jit"), SHEARSHOSTNAME);
	ReplyText(mailbox, text, (uint32)length);
}

void ShearsInit(void)
{
	WriteMacInt32(XLM_SHEARS_SIG, SHEARSMAGIC);
	WriteMacInt32(XLM_SHEARS_PROC, NativeTVECT(NATIVE_SHEARS));
	WriteMacInt32(XLM_SHEARS_FLAGS, 0);
	WriteMacInt32(XLM_SHEARS_CONTACT, 0);
}

uint32 ShearsCall(uint32 mailbox)
{
	char text[SHEARSMAXPAYLOAD + 1];
	char line[SHEARSMAXPAYLOAD + 32];
	uint32 words[2];
	uint32 command;
	uint32 length;
	if ((mailbox & 3) != 0 || mailbox < RAMBase || mailbox - RAMBase > RAMSize - SHEARSMAILBOX)
		return 0;
	if (ReadMacInt32(mailbox) != SHEARSMAGIC)
		return 0;
	length = ReadMacInt32(mailbox + 20);
	if (length > SHEARSMAXPAYLOAD)
		return 0;
	command = ReadMacInt32(mailbox + 12);
	WriteMacInt32(XLM_SHEARS_CONTACT, NowMilliseconds());
	words[0] = SHEARSPOLLTICKS;
	words[1] = ReadMacInt32(XLM_SHEARS_FLAGS);
	if (command == SHEARSCMDHELLO) {
		words[1] = SHEARSCAPSHUTDOWN;
		ReplyWords(mailbox, SHEARSOK, words, 2);
		printf("Sheep Shears: guest tool connected\n");
		fflush(stdout);
	} else if (command == SHEARSCMDPOLL)
		ReplyWords(mailbox, SHEARSOK, words, 2);
	else if (command == SHEARSCMDLOG) {
		CopyText(mailbox, length, text, sizeof(text));
		snprintf(line, sizeof(line), "Sheep Shears: %s\n", text);
		printf("%s", line);
		fflush(stdout);
		ReplyWords(mailbox, SHEARSOK, NULL, 0);
	} else if (command == SHEARSCMDRESULT) {
		WriteMacInt32(XLM_SHEARS_FLAGS, ReadMacInt32(XLM_SHEARS_FLAGS) & ~SHEARSFLAGSHUTDOWN);
		printf("Sheep Shears: shutdown request answered, error %d\n", (int)(int32)ReadMacInt32(mailbox + SHEARSHEADER + 4));
		fflush(stdout);
		ReplyWords(mailbox, SHEARSOK, NULL, 0);
	} else if (command == SHEARSCMDINFO)
		Info(mailbox);
	else if (command == SHEARSCMDNATIVE)
		Native(mailbox);
	else if (command == SHEARSCMDREPORT) {
		CopyText(mailbox, length, text, sizeof(text));
		Report(text);
		ReplyWords(mailbox, SHEARSOK, NULL, 0);
	} else
		ReplyWords(mailbox, SHEARSUNSUPPORTED, NULL, 0);
	return SHEARSMAGIC;
}

bool ShearsRequestShutdown(void)
{
	uint32 contact;
	if (ReadMacInt32(XLM_SHEARS_SIG) != SHEARSMAGIC)
		return false;
	contact = ReadMacInt32(XLM_SHEARS_CONTACT);
	if (contact == 0 || NowMilliseconds() - contact > SHEARSCONTACTMS)
		return false;
	if (ReadMacInt32(XLM_SHEARS_FLAGS) & SHEARSFLAGSHUTDOWN)
		return false;
	WriteMacInt32(XLM_SHEARS_FLAGS, ReadMacInt32(XLM_SHEARS_FLAGS) | SHEARSFLAGSHUTDOWN);
	printf("Sheep Shears: asking the guest to shut down\n");
	fflush(stdout);
	return true;
}
