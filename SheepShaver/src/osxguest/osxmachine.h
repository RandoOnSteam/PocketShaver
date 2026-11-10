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
#define OSXPVRG4 0x000c0209
#define OSXCPUFREQUENCYG4 1000000000
#define OSXCPUFREQUENCY 266000000
#define OSXBUSFREQUENCY 66666666
#define OSXTIMEBASEFREQUENCY 16666666

#define OSXHEATHROWBASE 0xf3000000
#define OSXHEATHROWSIZE 0x00080000
#define OSXVRAMBASE 0x81000000
#define OSXVRAMSIZE 0x01000000
#define OSXDISPLAYTRAP 0x44d00002u
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
#define OSXIRQAUDIOOUTDMA 10
#define OSXIRQAUDIOINDMA 11
#define OSXIRQAUDIO 17

#define OSXDBDMACHANNELS 16
#define OSXDBDMAIDE0 11
#define OSXDBDMAIDE1 12
#define OSXDBDMABMACTX 2
#define OSXDBDMABMACRX 3
#define OSXDBDMAAUDIOOUT 8
#define OSXDBDMAAUDIOIN 9
#define OSXDBDMASTATUSACTIVE 0x0400
#define OSXAUDIOOFFSET 0x14000
#define OSXAUDIOSIZE 0x1000
#define OSXAUDIORING 65536
#define OSXAUDIOBLOCK 4096
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

typedef struct OsxAudio {
	uint32 control;
	uint32 codeccontrol;
	uint32 codec[8];
	uint32 readback;
	uint32 byteswap;
	uint32 rate;
	uint32 openrate;
	void *stream;
	int running;
	int disabled;
	int32 leftgain;
	int32 rightgain;
	uint32 head;
	uint32 tail;
	uint64 lasttime;
	uint8 ring[OSXAUDIORING];
	uint8 block[OSXAUDIOBLOCK];
} OsxAudio;

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
	void *cdimage;
	char *imagepath;
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

#define OSXDISKPATCHES 2048
#define OSXQUARTZSLOTS 760
#define OSXQUARTZSCREENRECT 0
#define OSXQUARTZSURFACEBOUNDS 1
#define OSXQUARTZWINDOWLIST 2
#define OSXQUARTZOPACITY 3
#define OSXQUARTZWINDOWCALLS 4

typedef struct OsxDiskPatch {
	uint64 offset;
	uint8 bytes[16];
	int length;
	int atapionly;
	const OsxAtaDrive *drive;
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
#if OSXG5
	int pmu;
	int pmustate;
	uint8 pmucommand;
	int pmucommandlength;
	int pmuresponselength;
	int pmucommandposition;
	int pmuresponseposition;
	int pmuresponsesize;
	uint8 pmucommandbuffer[OSXCUDABUFFER];
	uint8 pmuresponse[OSXCUDABUFFER];
	uint8 pmuintbits;
	uint8 pmuintmask;
	uint8 adbreply[OSXCUDABUFFER];
	int adbreplysize;
	uint64 tickdeadline;
#endif
} OsxCuda;

#if OSXG5
#define OSXG5MACIOBASE 0x80000000
#define OSXG5MACIOSIZE 0x00080000
#define OSXG5ATABASE 0x80080000
#define OSXG5ATASIZE 0x00004000
#define OSXG5HTCONFIG 0xf2000000
#define OSXG5HTCONFIGSIZE 0x02800000
#define OSXG5HTSELF 0xf8070000
#define OSXG5U3BASE 0xf8000000
#define OSXG5U3SIZE 0x01000000
#define OSXG5U3I2C 0xf8001000
#define OSXG5DART 0xf8033000
#define OSXG5DARTSIZE 0x7000
#define OSXG5HIGHRAM 0x100000000ULL
#define OSXG5MPICOFFSET 0x40000
#define OSXG5MPICSIZE 0x40000
#define OSXG5MPICSOURCES 68
#define OSXG5MPICIPI 64
#define OSXG5DEVICEHOST 0
#define OSXG5DEVICEMACIO 7
#define OSXG5DEVICEATA0 13
#define OSXG5DEVICEATA1 14
#define OSXG5DEVICEDISPLAY 16
#define OSXG5DEVICEGMAC 15
#define OSXG5FUNCTIONS 6
#define OSXG5GMACBASE 0x80200000
#define OSXG5GMACSIZE 0x00200000
#define OSXG5GMACFRAME 4096
#define OSXG5IRQGMAC 0x29
#define OSXG5IRQPMU 0x19
#define OSXG5IRQPMUGPIO 0x2f
#define OSXG5IRQU3I2C 0x00
#define OSXG5IRQK2I2C 0x1a
#define OSXG5IRQATA0 0x27
#define OSXG5IRQATA0DMA 0x0d
#define OSXG5IRQATA1 0x28
#define OSXG5IRQATA1DMA 0x0e
#define OSXG5I2CWIDTH 15
#define OSXG5IRQDIRECT 0x100
#define OSXG5IRQI2STX 0x01
#define OSXG5IRQI2SRX 0x02
#define OSXG5DBDMAI2SOUT 0
#define OSXG5DBDMAI2SIN 1
#define OSXG5GPIOBASE 0x50
#define OSXG5GPIOSIZE 0x60

typedef struct OsxG5Function {
	uint32 device;
	uint8 config[256];
	uint32 barmask[6];
} OsxG5Function;

typedef struct OsxMpic {
	uint32 ivpr[OSXG5MPICSOURCES];
	uint32 idr[OSXG5MPICSOURCES];
	uint8 line[OSXG5MPICSOURCES];
	uint8 pending[OSXG5MPICSOURCES];
	uint8 inservice[16];
	int inservicecount;
	uint32 gcr;
	uint32 vid;
	uint32 spvr;
	uint32 tfrr;
	uint32 ctpr;
	uint32 timers[16];
} OsxMpic;

typedef struct OsxKeyWest {
	uint8 mode;
	uint8 control;
	uint8 status;
	uint8 isr;
	uint8 ier;
	uint8 addr;
	uint8 subaddr;
	uint8 data;
	int active;
	int readpending;
	int lastnak;
	int pointerset;
	uint8 pointer;
	uint8 device;
	int width;
	int position;
	int volumewritten;
	uint8 secondary;
	int selected;
	uint8 map;
	uint8 receiver[0x80];
	uint8 memory[0x100 * OSXG5I2CWIDTH];
} OsxKeyWest;

typedef struct OsxGmac {
	uint32 gregs[0x2000 >> 2];
	uint32 txdma[0x1000 >> 2];
	uint32 rxdma[0x2000 >> 2];
	uint32 mac[0x200 >> 2];
	uint32 mif[0x20 >> 2];
	int rxkicked;
	uint32 txsize;
	uint32 txfirst;
	uint8 txdata[OSXG5GMACFRAME];
} OsxGmac;

typedef struct OsxG5 {
	OsxG5Function functions[OSXG5FUNCTIONS];
	OsxGmac gmac;
	uint32 dartcontrol;
	uint32 dartexception;
	uint32 highsize;
	OsxMpic mpic;
	OsxKeyWest i2c;
	OsxKeyWest k2i2c;
	uint32 i2sinterrupt;
	uint32 i2sformat;
	uint32 i2swordsizes;
	uint32 i2sframebias;
	uint32 i2sframes;
	uint32 fcr[20];
	uint8 gpio[OSXG5GPIOSIZE];
	uint32 atafcr[2];
	uint32 atairq[2];
} OsxG5;
#endif

typedef struct OsxPointer {
	OsxMachine *machine;
	uint32 absoluteaddress;
	uint32 wheeladdress;
	uint32 callstack;
	int sender;
	int compactwheel;
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

#define OSXCLIPMALLOC 0
#define OSXCLIPFREE 1
#define OSXCLIPBZERO 2
#define OSXCLIPGETCURRENT 3
#define OSXCLIPCLEARCURRENT 4
#define OSXCLIPPUTFLAVOR 5
#define OSXCLIPFLAVORSIZE 6
#define OSXCLIPFLAVORDATA 7
#define OSXCLIPDOCK 8
#define OSXCLIPDOCKCOMMANDS 9
#define OSXCLIPFUNCTIONS 10

typedef struct OsxClip {
	uint32 functions[OSXCLIPFUNCTIONS];
	uint32 textlow;
	uint32 texthigh;
	int ready;
	int busy;
	int task;
	int step;
	uint32 saved[32];
	uint32 savedlr;
	uint32 savedctr;
	uint32 pc;
	uint32 stack;
	uint32 stackphysical;
	uint32 block;
	uint32 data;
	uint32 size;
	uint32 scrap;
	uint32 flavor;
	uint32 lastscrap;
	int primed;
	uint32 lasthash;
	uint8 *pending;
	uint32 pendinglength;
	uint64 started;
	uint64 lastput;
	uint64 lastget;
} OsxClip;

struct OsxMachine {
	OsxCpu *cpu;
	uint8 *ram;
	uint32 ramsize;
	uint32 pvr;
	uint8 *vram;
	uint32 width;
	uint32 height;
	uint32 depth;
	uint32 rowbytes;
	uint32 maxwidth;
	uint32 maxheight;
	uint32 displaymode;
	uint32 displaypreferred;
	int displaycustom;
	uint32 configaddress;
	OsxPciFunction pci[OSXPCIDEVICES];
	uint32 heathrowfcr[2];
	OsxPic pic;
	OsxDbdma dbdma[OSXDBDMACHANNELS];
	OsxAtaBus ata[OSXATABUSES];
	OsxCuda cuda;
	OsxBmac bmac;
	OsxAudio audio;
	uint8 nvram[OSXNVRAMSIZE];
	uint8 nvramclassic[OSXNVRAMSIZE];
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
	int flurry;
	int quartz;
	uint32 quartztails[OSXQUARTZSLOTS];
	uint32 quartzbase;
	uint32 quartzwindow[OSXQUARTZWINDOWCALLS];
	uint32 quartzstrings;
	int quartzextensions;
	int quartzallocatedextensions;
	uint32 flurrystubs[86];
	uint32 flurryinit;
	uint32 kernelend;
	uint32 scratch;
	uint8 *kernelimage;
	uint32 kernelsize;
	uint8 *mkext;
	uint32 mkextsize;
	OsxClip clip;
	int pointerx;
	int pointery;
	int pointerbuttons;
	int pointermoved;
	uint64 unknownreads;
	uint64 unknownwrites;
#if OSXG5
	int g5;
	OsxG5 g5state;
#endif
};

uint32 OsxGuestBusRead(void *context, uint32 address, int size);
void OsxGuestBusWrite(void *context, uint32 address, int size, uint32 value);
void OsxGuestPciReset(OsxMachine *machine);
uint32 OsxGuestPciConfigRead(OsxMachine *machine, uint32 offset, int size);
void OsxGuestPciConfigWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);

void OsxGuestPicSetLine(OsxMachine *machine, int source, int level);
#if OSXG5
void OsxGuestG5Reset(OsxMachine *machine);
uint32 OsxGuestG5BusRead(OsxMachine *machine, uint32 address, int size);
void OsxGuestG5BusWrite(OsxMachine *machine, uint32 address, int size, uint32 value);
void OsxGuestG5SetLine(OsxMachine *machine, int source, int level);
void OsxGuestG5SetPmuRequest(OsxMachine *machine, int level);
void OsxGuestG5PrepareNvram(OsxMachine *machine);
void OsxGuestG5GmacReceive(OsxMachine *machine);
uint8 *OsxGuestG5DmaHost(OsxMachine *machine, uint32 bus, uint32 length);
int OsxGuestG5DmaCopy(OsxMachine *machine, uint32 bus, uint8 *data, uint32 length, int towardmemory);
#endif
uint32 OsxGuestHeathrowRead(OsxMachine *machine, uint32 offset, int size);
void OsxGuestHeathrowWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);
void OsxGuestDbdmaKick(OsxMachine *machine, int channel);

void OsxGuestAtaReset(OsxMachine *machine);
int OsxGuestAtaAttach(OsxMachine *machine, int bus, int unit, const char *path, int atapi);
void OsxGuestAtaCloseMedia(OsxAtaDrive *drive);
uint32 OsxGuestAtaRead(OsxMachine *machine, int bus, uint32 offset, int size);
void OsxGuestAtaWrite(OsxMachine *machine, int bus, uint32 offset, int size, uint32 value);
void OsxGuestAtaDmaReady(OsxMachine *machine, int bus);
void OsxGuestAtaPoll(OsxMachine *machine, uint64 now);
int OsxGuestDbdmaTransfer(OsxMachine *machine, int channel, uint8 *data, uint32 length, int towardmemory);
void OsxGuestDbdmaFinish(OsxMachine *machine, int channel);
void OsxGuestDbdmaDump(OsxMachine *machine, int channel);
int OsxGuestDbdmaOutputFrame(OsxMachine *machine, int channel, uint8 *data, uint32 limit);
int OsxGuestDbdmaInputFrame(OsxMachine *machine, int channel, const uint8 *data, uint32 length);

void OsxGuestBmacHardwareAddress(uint8 *address);
void OsxGuestBmacReset(OsxMachine *machine);
uint32 OsxGuestBmacRead(OsxMachine *machine, uint32 offset);
void OsxGuestBmacWrite(OsxMachine *machine, uint32 offset, uint32 value);
void OsxGuestBmacTransmit(OsxMachine *machine);
void OsxGuestBmacReceive(OsxMachine *machine);
void OsxGuestBmacPoll(OsxMachine *machine, uint64 now);
uint64 OsxGuestBmacNextDeadline(OsxMachine *machine);

void OsxGuestAudioReset(OsxMachine *machine);
uint32 OsxGuestAudioRead(OsxMachine *machine, uint32 offset);
void OsxGuestAudioWrite(OsxMachine *machine, uint32 offset, uint32 value);
void OsxGuestAudioPoll(OsxMachine *machine, uint64 now);
void OsxGuestAudioExit(OsxMachine *machine);
void OsxGuestAudioOpenForCdrom(OsxMachine *machine);



uint32 OsxGuestFlashRead(OsxMachine *machine, uint32 offset, int size);
void OsxGuestFlashWrite(OsxMachine *machine, uint32 offset, int size, uint32 value);
void OsxGuestFlashLoad(OsxMachine *machine, const char *path);
void OsxGuestNvramReadString(const uint8 *nvram, int index, char *output, int size);
int OsxGuestNvramWantsClassic(const char *device, const char *command);
int OsxGuestNvramIsNewWorld(const uint8 *nvram);
void OsxGuestNvramMakeNewWorld(uint8 *nvram);
void OsxGuestNvramRestoreOldWorld(uint8 *nvram, const uint8 *classic);
int OsxGuestNvramPrepareBoot(OsxMachine *machine, int bus, int unit, int partition);
void OsxGuestNvramFinishBoot(OsxMachine *machine, const uint8 *startup);
void OsxGuestFlashSave(OsxMachine *machine);

uint8 *OsxGuestDisplayDriver(uint32 *length);
void OsxGuestDisplayReset(OsxMachine *machine);
int OsxGuestDisplayDriverCall(OsxMachine *machine, OsxCpu *cpu);

void OsxGuestCudaReset(OsxMachine *machine);
uint32 OsxGuestCudaRead(OsxMachine *machine, uint32 offset);
void OsxGuestCudaWrite(OsxMachine *machine, uint32 offset, uint32 value);
void OsxGuestCudaPoll(OsxMachine *machine, uint64 now);
uint64 OsxGuestCudaNextDeadline(OsxMachine *machine);
void OsxGuestCudaKey(OsxMachine *machine, int code, int down);
void OsxGuestCudaMouse(OsxMachine *machine, int dx, int dy, int button);
void OsxGuestCudaWheel(OsxMachine *machine, int delta);
void OsxGuestPointerHook(OsxCpu *cpu, void *context, uint32 address);

int OsxGuestBootLoad(OsxMachine *machine, const char *imagepath, int bootbus, int bootunit, const char *commandline);
uint32 OsxGuestFindKernelSymbol(OsxMachine *machine, const char *name);
int OsxGuestClipSyscall(OsxMachine *machine, OsxCpu *cpu);
int OsxGuestKextSymbolOffset(OsxMachine *machine, const char *bundle, const char *symbol, uint32 *offset);
uint32 OsxGuestAdler32(const uint8 *data, uint32 length);
int OsxGuestMkextHasBundle(const uint8 *mkext, uint32 size, const char *bundle);
uint8 *OsxGuestMkextAddBundles(const uint8 *base, uint32 basesize, const uint8 *donor, uint32 donorsize, const char *const *bundles, int bundlecount, const char *const *aliases, int aliascount, uint32 *size);
int OsxGuestKextTextAddress(OsxMachine *machine, const char *bundle, uint32 *address);

#endif
