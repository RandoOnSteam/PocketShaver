#include <stdio.h>
#include <string.h>
#include <Sound.h>
#include <Gestalt.h>
#include <Memory.h>
#include <Events.h>
#include <Files.h>
#include <Script.h>

#define MICTESTSYNCBYTES 88200
#define MICTESTASYNCBYTES 22254
#define MICTESTCAPTURE 65536
#define MICTESTCHUNK 4096

typedef struct MicTestCapture {
	Ptr data;
	long length;
	long interrupts;
	long completions;
} MicTestCapture;

static MicTestCapture *Shared(void)
{
	static MicTestCapture capture;
	return &capture;
}

static pascal void CaptureInterrupt(SPBPtr spb, Ptr buffer, short peak, long size)
{
	MicTestCapture *capture;
	capture = Shared();
	if (capture->length + size <= MICTESTCAPTURE)
		BlockMoveData(buffer, capture->data + capture->length, size);
	capture->length += size;
	capture->interrupts++;
}

static pascal void CaptureCompletion(SPBPtr spb)
{
	Shared()->completions++;
}

static void SaveRecording(const char *name, Ptr data, long length)
{
	FSSpec spec;
	Str255 pascalname;
	short file;
	pascalname[0] = (unsigned char)strlen(name);
	memcpy(pascalname + 1, name, pascalname[0]);
	FSMakeFSSpec(0, 0, pascalname, &spec);
	FSpDelete(&spec);
	FSpCreate(&spec, 'ttxt', 'TEXT', 0);
	if (FSpOpenDF(&spec, fsRdWrPerm, &file) != noErr)
		return;
	FSWrite(file, &length, data);
	FSClose(file);
}

int main(void)
{
	char log[1024];
	char *line;
	EventRecord event;
	Str255 name;
	Handle icon;
	SPB spb;
	Ptr buffer;
	OSType quality;
	long response;
	long ref;
	short meter[2];
	short on;
	OSErr err;
	FSSpec spec;
	short file;
	line = log;
	line += sprintf(line, "start\n");
	SaveRecording("mictest.log", log, line - log);
	err = Gestalt(gestaltSoundAttr, &response);
	line += sprintf(line, "gestalt %d %08lx\n", err, response);
	SaveRecording("mictest.log", log, line - log);
	name[0] = 0;
	err = SPBGetIndexedDevice(1, name, &icon);
	line += sprintf(line, "indexed %d %.*s\n", err, name[0], (char *)name + 1);
	SaveRecording("mictest.log", log, line - log);
	err = SPBOpenDevice(NULL, siWritePermission, &ref);
	line += sprintf(line, "open %d\n", err);
	SaveRecording("mictest.log", log, line - log);
	quality = siBetterQuality;
	err = SPBSetDeviceInfo(ref, siRecordingQuality, &quality);
	buffer = NewPtr(MICTESTSYNCBYTES);
	memset(&spb, 0, sizeof(spb));
	spb.inRefNum = ref;
	spb.count = MICTESTSYNCBYTES;
	spb.bufferLength = MICTESTSYNCBYTES;
	spb.bufferPtr = buffer;
	err = SPBRecord(&spb, false);
	line += sprintf(line, "sync %d count %ld error %d\n", err, spb.count, spb.error);
	SaveRecording("mictest.log", log, line - log);
	SaveRecording("sync16.raw", buffer, spb.count);
	quality = siGoodQuality;
	SPBSetDeviceInfo(ref, siRecordingQuality, &quality);
	memset(&spb, 0, sizeof(spb));
	spb.inRefNum = ref;
	spb.count = MICTESTASYNCBYTES;
	spb.bufferLength = MICTESTASYNCBYTES;
	spb.bufferPtr = buffer;
	err = SPBRecord(&spb, true);
	while (err == noErr && spb.error > 0)
		WaitNextEvent(0, &event, 1, NULL);
	line += sprintf(line, "async %d count %ld error %d\n", err, spb.count, spb.error);
	SaveRecording("mictest.log", log, line - log);
	SaveRecording("async8.raw", buffer, spb.count);
	on = 1;
	SPBSetDeviceInfo(ref, siLevelMeterOnOff, &on);
	SPBGetDeviceInfo(ref, siLevelMeterOnOff, meter);
	response = TickCount() + 60;
	while (TickCount() < response)
		WaitNextEvent(0, &event, 1, NULL);
	err = SPBGetDeviceInfo(ref, siLevelMeterOnOff, meter);
	line += sprintf(line, "meter %d on %d level %d\n", err, meter[0], meter[1]);
	SaveRecording("mictest.log", log, line - log);
	memset(&spb, 0, sizeof(spb));
	spb.inRefNum = ref;
	spb.count = MICTESTASYNCBYTES;
	FSMakeFSSpec(0, 0, "\ptofile.raw", &spec);
	FSpDelete(&spec);
	FSpCreate(&spec, 'ttxt', 'TEXT', 0);
	FSpOpenDF(&spec, fsRdWrPerm, &file);
	err = SPBRecordToFile(file, &spb, false);
	FSClose(file);
	line += sprintf(line, "tofile %d count %ld error %d\n", err, spb.count, spb.error);
	SaveRecording("mictest.log", log, line - log);
	Shared()->data = NewPtr(MICTESTCAPTURE);
	memset(&spb, 0, sizeof(spb));
	spb.inRefNum = ref;
	spb.bufferLength = MICTESTCHUNK;
	spb.bufferPtr = buffer;
	spb.interruptRoutine = NewSIInterruptUPP(CaptureInterrupt);
	spb.completionRoutine = NewSICompletionUPP(CaptureCompletion);
	err = SPBRecord(&spb, true);
	while (err == noErr && Shared()->length < MICTESTCAPTURE)
		WaitNextEvent(0, &event, 1, NULL);
	err = SPBStopRecording(ref);
	line += sprintf(line, "interrupt %d calls %ld bytes %ld completions %ld error %d\n", err, Shared()->interrupts, Shared()->length, Shared()->completions, spb.error);
	SaveRecording("mictest.log", log, line - log);
	SaveRecording("interrupt.raw", Shared()->data, MICTESTCAPTURE);
	err = SPBCloseDevice(ref);
	line += sprintf(line, "close %d\n", err);
	SaveRecording("mictest.log", log, line - log);
	return 0;
}
