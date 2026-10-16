/*
 *  osxmachine.h - low level public interface for OSX emulation
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
#ifndef OSXMACHINE_H
#define OSXMACHINE_H

#include "osxcpu.h"

#define OSXPVR 0x00080202
#define OSXCPUFREQUENCY 266000000
#define OSXBUSFREQUENCY 66666666
#define OSXTIMEBASEFREQUENCY 16666666

#define OSXHEATHROWBASE 0xf3000000
#define OSXHEATHROWSIZE 0x00080000
#define OSXVRAMBASE 0x81000000
#define OSXVRAMSIZE 0x01000000
#define OSXCONFIGADDRESSBASE 0xfec00000
#define OSXCONFIGDATABASE 0xfee00000
#define OSXMACHINEIDADDRESS 0xff000004
#define OSXFLASHBASE 0xfff80000
#define OSXFLASHSIZE 0x4000
#define OSXFLASHBANK 0x2000

#define OSXPCIDEVICEGRACKLE 0
#define OSXPCIDEVICEHEATHROW 16
#define OSXPCIDEVICEDISPLAY 18
#define OSXPCIDEVICES 3

#define OSXIRQIDE0DMA 2
#define OSXIRQIDE1DMA 3
#define OSXIRQIDE0 13
#define OSXIRQIDE1 14
#define OSXIRQCUDA 18
#define OSXIRQLEVELMASK 0x1ff00000u
#define OSXIRQBMACTXDMA 8
#define OSXIRQBMACRXDMA 9
#define OSXIRQBMAC 42

#define OSXDBDMACHANNELS 16
#define OSXDBDMAIDE0 11
#define OSXDBDMAIDE1 12
#define OSXDBDMABMACTX 2
#define OSXDBDMABMACRX 3
#define OSXBMACOFFSET 0x11000
#define OSXBMACSIZE 0x1000

#define OSXATABUSES 2
#define OSXATASECTOR 512
#define OSXATAPISECTOR 2048
#define OSXATAPICDSECTORS 405000
#define OSXATABUFFER 65536
#define OSXATADELAY 1500

#define OSXNVRAMSIZE 8192
#define OSXPRAMSIZE 256
#define OSXPRAMOFFSET 0x1300
#define OSXCUDABUFFER 64
#define OSXADBQUEUE 64
#define OSXMOUSEQUEUE 256

#define OSXINPUTMOUSEMOVE 1
#define OSXINPUTMOUSEDOWN 2
#define OSXINPUTMOUSEUP 3
#define OSXINPUTKEYDOWN 4
#define OSXINPUTKEYUP 5

typedef struct OsxMachine OsxMachine;

typedef struct OsxPciFunction {
	uint32 vendordevice;
	uint32 classrevision;
	uint32 command;
	uint32 bar0;
	uint32 bar0mask;
	uint32 latency;
	uint32 interrupt;
} OsxPciFunction;

typedef struct OsxPic {
	uint32 events[2];
	uint32 mask[2];
	uint32 levels[2];
} OsxPic;

typedef struct OsxDbdma {
	uint32 status;
	uint32 commandpointer;
	uint32 interruptselect;
	uint32 branchselect;
	uint32 waitselect;
	int irq;
} OsxDbdma;

typedef struct OsxBmac {
	uint16 registers[0x100];
	uint16 status;
	uint16 srom[64];
	uint16 sromword;
	uint32 sromcommand;
	int srombits;
	int sromoutput;
	int sromclock;
	int sromdata;
	int attached;
	int ready;
	void *slirpthread;
	int pendinglength;
	uint64 polldeadline;
	uint64 busydeadline;
	uint8 pending[1600];
	uint8 frame[1600];
} OsxBmac;

typedef struct OsxAtaDrive {
	void *file;
	uint64 sectors;
	int present;
	int atapi;
	int readonly;
	int multiple;
	int unitattention;
	int mediapresent;
	int mediachanged;
	int mediaindex;
	int dvd;
	uint64 insertdeadline;
	uint8 sensekey;
	uint8 asc;
	uint8 ascq;
	char model[41];
} OsxAtaDrive;

typedef struct OsxDiskPatch {
	uint64 offset;
	uint8 bytes[16];
	int length;
	int atapionly;
} OsxDiskPatch;

typedef struct OsxAtaBus {
	OsxAtaDrive drives[2];
	int irq;
	int dmachannel;
	int irqline;
	uint8 feature;
	uint8 error;
	uint8 count;
	uint8 sector;
	uint8 cylinderlow;
	uint8 cylinderhigh;
	uint8 select;
	uint8 status;
	uint8 control;
	uint8 command;
	uint32 timing;
	uint8 *buffer;
	uint32 bufferlength;
	uint32 bufferposition;
	uint32 drqblock;
	uint32 drqremaining;
	int transferdirection;
	int dmapending;
	int packetphase;
	int atapitransfer;
	uint64 lba;
	uint32 remaining;
	uint32 sectorsize;
	uint8 packet[12];
	OsxDiskPatch *patches;
	int patchcount;
	uint8 pendingstatus;
	uint64 irqdeadline;
} OsxAtaBus;

typedef struct OsxAdbDevice {
	uint8 address;
	uint8 defaultaddress;
	uint8 handler;
	uint8 defaulthandler;
} OsxAdbDevice;

typedef struct OsxMouseEvent {
	int x;
	int y;
	int button;
	int motion;
	int wheel;
} OsxMouseEvent;

typedef struct OsxCuda {
	uint8 b;
	uint8 a;
	uint8 dirb;
	uint8 dira;
	uint8 sr;
	uint8 acr;
	uint8 pcr;
	uint8 ifr;
	uint8 ier;
	uint8 lastb;
	uint16 t1latch;
	uint16 t2latch;
	uint64 t1start;
	uint64 t2start;
	uint64 t1deadline;
	uint64 t2deadline;
	uint64 srdeadline;
	uint64 polldeadline;
	uint8 outbuffer[OSXCUDABUFFER];
	int outlength;
	uint8 inbuffer[OSXCUDABUFFER];
	int inlength;
	int inposition;
	int autopoll;
	uint32 pollrate;
	uint16 pollmask;
	int irqline;
	int64 timeoffset;
	OsxAdbDevice keyboard;
	OsxAdbDevice mouse;
	uint8 keyqueue[OSXADBQUEUE];
	int keyread;
	int keywrite;
	int mousedx;
	int mousedy;
	int mousebutton;
	int mousereported;
	uint32 mousepending;
	int mouseabsolute;
	int mousex;
	int mousey;
	OsxMouseEvent mousequeue[OSXMOUSEQUEUE];
	int mouseread;
	int mousewrite;
	OsxMouseEvent pointerqueue[OSXMOUSEQUEUE];
	int pointerread;
	int pointerwrite;
} OsxCuda;

typedef struct OsxPointer {
	OsxMachine *machine;
	uint32 absoluteaddress;
	uint32 wheeladdress;
	uint32 callstack;
	int sender;
	int pointerx;
	int pointery;
	int pointervalid;
	int pointerbuttons;
	uint32 registers[13];
	uint32 lr;
	uint32 ctr;
	uint32 cr;
	uint32 xer;
} OsxPointer;

typedef struct OsxInputEvent {
	int type;
	int first;
	int second;
} OsxInputEvent;

struct OsxMachine {
	OsxCpu *cpu;
	uint8 *ram;
	uint32 ramsize;
	uint8 *vram;
	uint32 width;
	uint32 height;
	uint32 depth;
	uint32 rowbytes;
	uint32 configaddress;
	OsxPciFunction pci[OSXPCIDEVICES];
	uint32 heathrowfcr[2];
	OsxPic pic;
	OsxDbdma dbdma[OSXDBDMACHANNELS];
	OsxAtaBus ata[OSXATABUSES];
	OsxCuda cuda;
	OsxBmac bmac;
	uint8 nvram[OSXNVRAMSIZE];
	char nvrampath[512];
	uint8 flash[OSXFLASHSIZE];
	int flashmode;
	int flashdirty;
	char cdrompaths[8][512];
	int cdromcount;
	int dmatrace;
	FILE *log;
	FILE *console;
	int quit;
	int verbose;
	uint32 kernelentry;
	uint32 bootargs;
	int bootpartition;
	uint32 kernelend;
	uint32 scratch;
	uint8 *kernelimage;
	uint32 kernelsize;
	uint8 *mkext;
	uint32 mkextsize;
	int pointerx;
	int pointery;
	int pointerbuttons;
	int pointermoved;
	uint64 unknownreads;
	uint64 unknownwrites;
};

uint32 OsxBusRead(void *context, uint32 address, int size);
void OsxBusWrite(void *context, uint32 address, int size, uint32 value);
void OsxPciReset(OsxMachine *machine);
uint32 OsxPciConfigRead(OsxMachine *machine, uint32 offset, int size);
void OsxPciConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);

void OsxPicSetLine(OsxMachine *machine, int source, int level);
uint32 OsxHeathrowRead(OsxMachine *machine, uint32 offset, int size);
void OsxHeathrowWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);
void OsxDbdmaKick(OsxMachine *machine, int channel);

void OsxAtaReset(OsxMachine *machine);
int OsxAtaAttach(OsxMachine *machine, int bus, int unit, const char *path, int atapi);
uint32 OsxAtaRead(OsxMachine *machine, int bus, uint32 offset, int size);
void OsxAtaWrite(OsxMachine *machine, int bus, uint32 offset, int size, uint32 value);
void OsxAtaDmaReady(OsxMachine *machine, int bus);
void OsxAtaPoll(OsxMachine *machine, uint64 now);
int OsxDbdmaTransfer(OsxMachine *machine, int channel, uint8 *data, uint32 length, int towardmemory);
void OsxDbdmaFinish(OsxMachine *machine, int channel);
void OsxDbdmaDump(OsxMachine *machine, int channel);
int OsxDbdmaOutputFrame(OsxMachine *machine, int channel, uint8 *data, uint32 limit);
int OsxDbdmaInputFrame(OsxMachine *machine, int channel, const uint8 *data, uint32 length);

void OsxBmacHardwareAddress(uint8 *address);
void OsxBmacReset(OsxMachine *machine);
uint32 OsxBmacRead(OsxMachine *machine, uint32 offset);
void OsxBmacWrite(OsxMachine *machine, uint32 offset, uint32 value);
void OsxBmacTransmit(OsxMachine *machine);
void OsxBmacReceive(OsxMachine *machine);
void OsxBmacPoll(OsxMachine *machine, uint64 now);
uint64 OsxBmacNextDeadline(OsxMachine *machine);

void *EtherSlirpOpen(void);
int EtherSlirpReady(void *thread);
void EtherSlirpPoll(void);
int EtherSlirpReceive(uint8 *packet);
void EtherSlirpSend(const uint8 *packet, int length);

uint32 OsxFlashRead(OsxMachine *machine, uint32 offset, int size);
void OsxFlashWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);
void OsxFlashLoad(OsxMachine *machine, const char *path);
void OsxNvramReadString(const uint8 *nvram, int index, char *output, int size);
int OsxNvramWantsClassic(const char *device, const char *command);
int OsxNvramPrepareBoot(OsxMachine *machine, int bus, int unit, int partition);
void OsxNvramFinishBoot(OsxMachine *machine, const uint8 *startup);
void OsxFlashSave(OsxMachine *machine);

void OsxCudaReset(OsxMachine *machine);
uint32 OsxCudaRead(OsxMachine *machine, uint32 offset);
void OsxCudaWrite(OsxMachine *machine, uint32 offset, uint32 value);
void OsxCudaPoll(OsxMachine *machine, uint64 now);
uint64 OsxCudaNextDeadline(OsxMachine *machine);
void OsxCudaKey(OsxMachine *machine, int code, int down);
void OsxCudaMouse(OsxMachine *machine, int dx, int dy, int button);
void OsxCudaWheel(OsxMachine *machine, int delta);
void OsxPointerHook(OsxCpu *cpu, void *context, uint32 address);

int OsxBootLoad(OsxMachine *machine, const char *imagepath, int bootbus, int bootunit, const char *commandline);
uint32 OsxFindKernelSymbol(OsxMachine *machine, const char *name);
int OsxKextSymbolOffset(OsxMachine *machine, const char *bundle, const char *symbol, uint32 *offset);
uint32 OsxAdler32(const uint8 *data, uint32 length);
int OsxMkextHasBundle(const uint8 *mkext, uint32 size, const char *bundle);
uint8 *OsxMkextAddBundles(const uint8 *base, uint32 basesize, const uint8 *donor, uint32 donorsize, const char *const *bundles, int bundlecount, const char *const *aliases, int aliascount, uint32 *size);
int OsxKextTextAddress(OsxMachine *machine, const char *bundle, uint32 *address);

#endif
