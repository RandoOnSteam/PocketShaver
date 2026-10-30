/*
 *  sheepshears.c - Sheep Shears background tool for Mac OS 9 under SheepShaver
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

#include <MacTypes.h>
#include <Events.h>
#include <AppleEvents.h>
#include <Processes.h>
#include "../shearsclient.h"

#define SHEEPSHEARSRETRYTICKS 600
#define SHEEPSHEARSACTIONSHUTDOWN 1UL

typedef struct SheepShears {
	ShearsMailbox *mailbox;
	Boolean connected;
	Boolean quit;
	unsigned long sleepticks;
	unsigned long retryat;
} SheepShears;

static OSErr AskFinderToShutDown(void)
{
	AEAddressDesc target;
	AppleEvent event;
	AppleEvent reply;
	OSType finder;
	OSErr err;
	finder = 'MACS';
	err = AECreateDesc(typeApplSignature, &finder, sizeof(finder), &target);
	if (err != noErr)
		return err;
	err = AECreateAppleEvent('FNDR', 'shut', &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
	AEDisposeDesc(&target);
	if (err != noErr)
		return err;
	reply.descriptorType = typeNull;
	reply.dataHandle = NULL;
	err = AESend(&event, &reply, kAENoReply, kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
	AEDisposeDesc(&event);
	return err;
}

static pascal OSErr HandleQuit(const AppleEvent *event, AppleEvent *reply, long refcon)
{
	((SheepShears *)refcon)->quit = true;
	return noErr;
}

static pascal OSErr HandleIgnore(const AppleEvent *event, AppleEvent *reply, long refcon)
{
	return noErr;
}

static void Connect(SheepShears *shears)
{
	unsigned long hello[2];
	hello[0] = SHEARSVERSION;
	hello[1] = 0;
	shears->retryat = TickCount() + SHEEPSHEARSRETRYTICKS;
	if (ShearsCall(shears->mailbox, SHEARSCMDHELLO, hello, sizeof(hello)) != 0)
		return;
	shears->connected = true;
	shears->sleepticks = ShearsWord(shears->mailbox, 0);
	if (shears->sleepticks < 1 || shears->sleepticks > 120)
		shears->sleepticks = 30;
	ShearsLog(shears->mailbox, "Sheep Shears tool started");
}

static void Poll(SheepShears *shears)
{
	unsigned long result[2];
	if (ShearsCall(shears->mailbox, SHEARSCMDPOLL, NULL, 0) != 0) {
		shears->connected = false;
		shears->retryat = TickCount() + SHEEPSHEARSRETRYTICKS;
		return;
	}
	if (!(ShearsWord(shears->mailbox, 1) & SHEARSFLAGSHUTDOWN))
		return;
	result[0] = SHEEPSHEARSACTIONSHUTDOWN;
	result[1] = (unsigned long)(long)AskFinderToShutDown();
	ShearsCall(shears->mailbox, SHEARSCMDRESULT, result, sizeof(result));
}

int main(void)
{
	SheepShears shears;
	EventRecord event;
	memset(&shears, 0, sizeof(shears));
	shears.mailbox = ShearsOpen();
	if (shears.mailbox == NULL)
		return 0;
	shears.sleepticks = 30;
	AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(HandleQuit), (long)&shears, false);
	AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(HandleIgnore), 0, false);
	AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(HandleIgnore), 0, false);
	AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(HandleIgnore), 0, false);
	while (!shears.quit) {
		if (shears.connected)
			Poll(&shears);
		else if (TickCount() >= shears.retryat)
			Connect(&shears);
		if (WaitNextEvent(everyEvent, &event, shears.sleepticks, NULL) && event.what == kHighLevelEvent)
			AEProcessAppleEvent(&event);
	}
	return 0;
}
