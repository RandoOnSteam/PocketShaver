/*
 *  installer.c - Sheep Shears installer: copies the tool into Startup Items, starts it, removes it again
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

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <Finder.h>
#include <Processes.h>
#include <AppleEvents.h>
#include "../shearsclient.h"

#define INSTALLERITEMINSTALL 1
#define INSTALLERITEMQUIT 2
#define INSTALLERITEMREMOVE 3
#define INSTALLERITEMSTATUS 4
#define INSTALLERTOOLNAME "\pSheepShears"
#define INSTALLERAUTONAME "\pSheep Shears Auto Install"
#define INSTALLERCOPYBYTES 16384
#define INSTALLERINITED 0x0100
#define INSTALLERLAUNCHNOFILEFLAGS 0x0800
#define INSTALLERLAUNCHDONTSWITCH 0x0200

typedef struct Installer {
	FSSpec self;
	char message[200];
	char buffer[INSTALLERCOPYBYTES];
} Installer;

static OSErr InstallerFindSelf(Installer *installer)
{
	ProcessSerialNumber process;
	ProcessInfoRec info;
	GetCurrentProcess(&process);
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(info);
	info.processAppSpec = &installer->self;
	return GetProcessInformation(&process, &info);
}

static OSErr InstallerFolderSpec(OSType kind, ConstStr255Param name, FSSpec *spec)
{
	short volume;
	long folder;
	OSErr err;
	err = FindFolder(kOnSystemDisk, kind, kCreateFolder, &volume, &folder);
	if (err != noErr)
		return err;
	err = FSMakeFSSpec(volume, folder, name, spec);
	if (err == fnfErr)
		return noErr;
	return err;
}

static OSErr InstallerOpenFork(const FSSpec *spec, Boolean resource, SInt8 permission, short *file)
{
	if (resource)
		return FSpOpenRF(spec, permission, file);
	return FSpOpenDF(spec, permission, file);
}

static OSErr InstallerCopyFork(Installer *installer, const FSSpec *from, const FSSpec *to, Boolean resource)
{
	short source;
	short target;
	long count;
	long written;
	OSErr err;
	OSErr writeerr;
	err = InstallerOpenFork(from, resource, fsRdPerm, &source);
	if (err != noErr)
		return err;
	err = InstallerOpenFork(to, resource, fsWrPerm, &target);
	if (err != noErr) {
		FSClose(source);
		return err;
	}
	for (;;) {
		count = INSTALLERCOPYBYTES;
		err = FSRead(source, &count, installer->buffer);
		if (count > 0) {
			written = count;
			writeerr = FSWrite(target, &written, installer->buffer);
			if (writeerr != noErr) {
				err = writeerr;
				break;
			}
		}
		if (err != noErr)
			break;
	}
	if (err == eofErr)
		err = noErr;
	FSClose(source);
	FSClose(target);
	return err;
}

static OSErr InstallerCopyFile(Installer *installer, const FSSpec *from, const FSSpec *to)
{
	FInfo info;
	OSErr err;
	err = FSpGetFInfo(from, &info);
	if (err != noErr)
		return err;
	err = FSpDelete(to);
	if (err != noErr && err != fnfErr)
		return err;
	err = FSpCreate(to, info.fdCreator, info.fdType, smSystemScript);
	if (err != noErr)
		return err;
	err = InstallerCopyFork(installer, from, to, false);
	if (err == noErr)
		err = InstallerCopyFork(installer, from, to, true);
	if (err == noErr) {
		info.fdFlags &= ~INSTALLERINITED;
		err = FSpSetFInfo(to, &info);
	}
	if (err != noErr)
		FSpDelete(to);
	return err;
}

static Boolean InstallerFindTool(ProcessSerialNumber *process)
{
	ProcessInfoRec info;
	process->highLongOfPSN = 0;
	process->lowLongOfPSN = kNoProcess;
	while (GetNextProcess(process) == noErr) {
		memset(&info, 0, sizeof(info));
		info.processInfoLength = sizeof(info);
		if (GetProcessInformation(process, &info) == noErr && info.processSignature == 'ShSh' &&
			info.processType == 'APPL')
			return true;
	}
	return false;
}

static void InstallerQuitTool(void)
{
	ProcessSerialNumber process;
	AEAddressDesc target;
	AppleEvent event;
	AppleEvent reply;
	EventRecord record;
	unsigned long until;
	if (!InstallerFindTool(&process))
		return;
	if (AECreateDesc(typeProcessSerialNumber, &process, sizeof(process), &target) != noErr)
		return;
	if (AECreateAppleEvent(kCoreEventClass, kAEQuitApplication, &target, kAutoGenerateReturnID, kAnyTransactionID, &event) ==
		noErr) {
		reply.descriptorType = typeNull;
		reply.dataHandle = NULL;
		AESend(&event, &reply, kAENoReply, kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
		AEDisposeDesc(&event);
	}
	AEDisposeDesc(&target);
	until = TickCount() + 240;
	while (TickCount() < until && InstallerFindTool(&process))
		WaitNextEvent(0, &record, 10, NULL);
}

static OSErr InstallerStartTool(const FSSpec *tool)
{
	LaunchParamBlockRec launch;
	memset(&launch, 0, sizeof(launch));
	launch.launchBlockID = extendedBlock;
	launch.launchEPBLength = extendedBlockLen;
	launch.launchControlFlags = launchContinue | INSTALLERLAUNCHNOFILEFLAGS | INSTALLERLAUNCHDONTSWITCH;
	launch.launchAppSpec = (FSSpecPtr)tool;
	return LaunchApplication(&launch);
}

static void InstallerDescribe(Installer *installer, OSErr err)
{
	if (err == fnfErr)
		strcpy(installer->message, "SheepShears was not found next to this installer.");
	else if (err == vLckdErr || err == wPrErr)
		strcpy(installer->message, "The disk is locked.");
	else if (err == fBsyErr)
		strcpy(installer->message, "A file is in use. Quit SheepShears and try again.");
	else if (err == dskFulErr)
		strcpy(installer->message, "The disk is full.");
	else
		snprintf(installer->message, sizeof(installer->message), "Mac OS error %d.", (int)err);
}

static OSErr InstallerInstall(Installer *installer)
{
	FSSpec source;
	FSSpec target;
	OSErr err;
	err = FSMakeFSSpec(installer->self.vRefNum, installer->self.parID, INSTALLERTOOLNAME, &source);
	if (err == noErr)
		err = InstallerFolderSpec(kStartupFolderType, INSTALLERTOOLNAME, &target);
	if (err == noErr) {
		InstallerQuitTool();
		err = InstallerCopyFile(installer, &source, &target);
	}
	if (err != noErr) {
		InstallerDescribe(installer, err);
		return err;
	}
	if (InstallerStartTool(&target) == noErr)
		strcpy(installer->message, "Installed. SheepShears is running now and starts every time Mac OS starts.");
	else
		strcpy(installer->message, "Installed. SheepShears will start at the next restart.");
	return noErr;
}

static OSErr InstallerRemove(Installer *installer)
{
	FSSpec target;
	OSErr err;
	err = InstallerFolderSpec(kStartupFolderType, INSTALLERTOOLNAME, &target);
	if (err == noErr) {
		InstallerQuitTool();
		err = FSpDelete(&target);
		if (err == fnfErr)
			err = noErr;
	}
	if (err != noErr) {
		InstallerDescribe(installer, err);
		return err;
	}
	strcpy(installer->message, "Removed.");
	return noErr;
}

static void InstallerSetStatus(DialogPtr dialog, const char *text)
{
	Str255 title;
	Handle item;
	Rect box;
	short type;
	size_t length;
	length = strlen(text);
	if (length > 255)
		length = 255;
	title[0] = (unsigned char)length;
	memcpy(title + 1, text, length);
	GetDialogItem(dialog, INSTALLERITEMSTATUS, &type, &item, &box);
	SetDialogItemText(item, title);
}

static Boolean InstallerAutomatic(Installer *installer)
{
	FSSpec flag;
	FInfo info;
	ShearsMailbox *mailbox;
	OSErr err;
	if (InstallerFolderSpec(kSystemFolderType, INSTALLERAUTONAME, &flag) != noErr || FSpGetFInfo(&flag, &info) != noErr)
		return false;
	err = InstallerInstall(installer);
	mailbox = ShearsOpen();
	if (mailbox) {
		if (err == noErr)
			ShearsLog(mailbox, "installer: done");
		else
			ShearsLog(mailbox, "installer: failed");
		ShearsLog(mailbox, installer->message);
	}
	return true;
}

static void InstallerToolbox(void)
{
	InitGraf(&qd.thePort);
	InitFonts();
	InitWindows();
	InitMenus();
	TEInit();
	InitDialogs(NULL);
	InitCursor();
	FlushEvents(everyEvent, 0);
}

int main(void)
{
	Installer *installer;
	DialogPtr dialog;
	short item;
	InstallerToolbox();
	installer = (Installer *)NewPtrClear(sizeof(Installer));
	if (installer == NULL || InstallerFindSelf(installer) != noErr)
		return 0;
	if (InstallerAutomatic(installer))
		return 0;
	dialog = GetNewDialog(128, NULL, (WindowPtr)-1L);
	if (dialog == NULL)
		return 0;
	InstallerSetStatus(dialog, "Installs SheepShears into Startup Items: clean shutdown when the SheepShaver window is closed.");
	ShowWindow(dialog);
	SelectWindow(dialog);
	SetPort(dialog);
	for (;;) {
		ModalDialog(NULL, &item);
		if (item == INSTALLERITEMQUIT)
			break;
		if (item == INSTALLERITEMINSTALL) {
			InstallerSetStatus(dialog, "Installing...");
			InstallerInstall(installer);
			InstallerSetStatus(dialog, installer->message);
		} else if (item == INSTALLERITEMREMOVE) {
			InstallerRemove(installer);
			InstallerSetStatus(dialog, installer->message);
		}
	}
	DisposeDialog(dialog);
	return 0;
}
