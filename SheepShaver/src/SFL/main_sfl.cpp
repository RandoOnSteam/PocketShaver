/*
 *  main_sfl.cpp - Emulation core of the SFL platform layer
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
#include "main.h"
#include "version.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "cpu_emulation.h"
#include "emul_op.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "video.h"
#include "sys.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "usbuim.h"
#include "sflmem.h"
#include "sflmain.h"
#include "sflui.h"
#include "gui.h"
#include "file.h"
#include "thread.h"

#define SFLMAINROMFILENAME "ROM"
#define SFLMAINROMFILENAME2 "Mac OS ROM"
#define SFLMAINROMBASE 0x40800000
#define SFLMAINHIGHPAGE 0xffff0000
#define SFLMAINKERNELPAGE 0x68ff0000
#define SFLMAINKERNEL2PAGE 0x5fff0000
#define SFLMAINSIGSTACKSIZE 0x10000
#define SFLMAINMINRAM (16 * 1024 * 1024)
#define SFLMAINNVRAMTICKS (60 * 60)
#define SFLMAINSECONDTICKS 60
#define SFLMAINNATIVEPAGE 4096
#define SFLMAINNANOKERNELSIZE 0x200000
#define SFLMAINSTOPWAITMS 5000

typedef struct SFLMAIN
{
#if !defined(SFLMAINRUNEMULATORONMAINTHREAD)
	WSTHREAD emulator;
#endif /* !SFLMAINRUNEMULATORONMAINTHREAD */
	WSSIGNAL parked;
	WSSIGNAL stopped;
	wsatomicint_t emulatorparked;
	WSCRITICALSECTION intflags;
	const char *prefspath;
	size_t emulatorid;
	uintptr sigstack;
	uint8 lastxpram[XPRAM_SIZE];
	wsatomicint_t pendingticks;
	int secondticks;
	int nvramticks;
	bool emulatorrunning;
	bool initialized;
} SFLMAIN;

static SFLMAIN SflMain;

uint32 RAMBase;
uint32 RAMSize;
uint32 ROMBase;
uint32 KernelDataAddr;
uint32 BootGlobsAddr;
uint32 DRCacheAddr;
uint32 PVR;
int64 CPUClockSpeed;
int64 BusClockSpeed;
int64 TimebaseSpeed;
uint8 *RAMBaseHost;
uint8 *ROMBaseHost;
volatile uint32 InterruptFlags = 0;
bool tick_inhibit;

uint32 SheepMem::page_size;
uintptr SheepMem::zero_page = 0;
uintptr SheepMem::base = 0x51000000;
uintptr SheepMem::proc;
uintptr SheepMem::data;

extern void emul_ppc(uint32 start);
extern void init_emul_ppc(void);
extern void exit_emul_ppc(void);
void jump_to_rom(uint32 entry);
#if defined(__cplusplus)
extern "C" {
#endif
const char *SflMainPrefsPath(void)
{
	return SflMain.prefspath;
}

void SflMainStopEmulator(void)
{
	StopEmulation();
}
#if defined(__cplusplus)
}
#endif

uintptr SignalStackBase(void)
{
	return SflMain.sigstack + SFLMAINSIGSTACKSIZE;
}

void SflMainParseArguments(int argc, char **argv)
{
	int index;
	SflMain.prefspath = NULL;
	for (index = 1; index + 1 < argc; index++) {
		if (strcmp(argv[index], "--config") == 0) {
			SflMain.prefspath = argv[index + 1];
			argv[index] = NULL;
			argv[index + 1] = NULL;
			index++;
		}
	}
}

int SflMainCompactArguments(int argc, char **argv)
{
	int source;
	int target;
	target = 1;
	for (source = 1; source < argc; source++) {
		if (argv[source] == NULL)
			continue;
		argv[target] = argv[source];
		target++;
	}
	return target;
}

bool SflMainMapMemory(void)
{
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000)
		RAMSize *= 1024 * 1024;
	if (RAMSize < SFLMAINMINRAM) {
		WarningAlert(GetString(STR_SMALL_RAM_WARN));
		RAMSize = SFLMAINMINRAM;
	}
	RAMBase = 0;
	RAMBaseHost = SflMemMap(RAMBase, RAMSize);
	if (RAMBaseHost == NULL) {
		ErrorAlert(GetString(STR_RAM_MMAP_ERR));
		return false;
	}
	ROMBase = SFLMAINROMBASE;
	ROMBaseHost = SflMemMap(ROMBase, ROM_AREA_SIZE);
	if (ROMBaseHost == NULL || SflMemMap(SFLMAINHIGHPAGE, SFLMEMPAGESIZE) == NULL) {
		ErrorAlert(GetString(STR_ROM_MMAP_ERR));
		return false;
	}
	if (SflMemMap(SFLMAINKERNELPAGE, SFLMEMPAGESIZE) == NULL
		|| !SflMemAlias(SFLMAINKERNEL2PAGE, SFLMAINKERNELPAGE, SFLMEMPAGESIZE)) {
		ErrorAlert(GetString(STR_KD_SHMAT_ERR));
		return false;
	}
	KernelDataAddr = KERNEL_DATA_BASE;
	if (SflMemMap(((RAMSize + 0x0fffffff) & 0xf0000000) + 0x10000000, SFLMAINNANOKERNELSIZE) == NULL) {
		ErrorAlert(GetString(STR_KD_SHMAT_ERR));
		return false;
	}
	if (!SheepMem::Init()) {
		ErrorAlert(GetString(STR_SHEEP_MEM_MMAP_ERR));
		return false;
	}
	return true;
}

uint8 *SflMainReadRomFile(const char *path, uint32 *size)
{
	WSFILE file;
	uint8 *data;
	WSFile_Construct(&file);
	if (!WSFile_Create(&file, path, FILE_READ | FILE_BINARY))
		return NULL;
	*size = (uint32)WSFile_Size(&file);
	data = (uint8 *)malloc(*size);
	if (data != NULL)
		*size = (uint32)WSFile_Read(&file, data, *size);
	WSFile_Destroy(&file);
	return data;
}

bool SflMainLoadRom(void)
{
	const char *path;
	uint8 *data;
	uint32 size;
	bool decoded;
	path = PrefsFindString("rom");
	data = NULL;
	size = 0;
	if (path != NULL && *path != 0)
		data = SflMainReadRomFile(path, &size);
	if (data == NULL)
		data = SflMainReadRomFile(SFLMAINROMFILENAME, &size);
	if (data == NULL)
		data = SflMainReadRomFile(SFLMAINROMFILENAME2, &size);
	if (data == NULL) {
		ErrorAlert(GetString(STR_NO_ROM_FILE_ERR));
		return false;
	}
	decoded = DecodeROM(data, size);
	free(data);
	if (!decoded) {
		if (size != ROM_SIZE)
			ErrorAlert(GetString(STR_ROM_SIZE_ERR));
		else
			ErrorAlert(GetString(STR_ROM_FILE_READ_ERR));
		return false;
	}
	return true;
}

void SflMainPark(void)
{
	wsatomicintstore(&SflMain.emulatorparked, 1);
	WSSignal_Raise(&SflMain.stopped);
	WSSignal_Wait(&SflMain.parked, WSSIGNAL_WAITINFINITE);
}

void SflEmulatorStart()
{
	jump_to_rom(ROMBase + 0x310000);
}
#if !defined(SFLMAINRUNEMULATORONMAINTHREAD)
WSTHREADENTRYCALLBACK(SflMainEmulatorThread)
{
	SflMain.emulatorid = WSThreadGetCurrentID();
	SflEmulatorStart();
	SflUiRequestQuit();
	SflMainPark();
	return WSTHREADENTRYPROCRETCODE(0);
}
#endif /* #if !defined(SFLMAINRUNEMULATORONMAINTHREAD) */
int SflEmulatorSetup(int argc, char **argv)
{
	WSCriticalSection_Construct(&SflMain.intflags);
	WSSignal_Create(&SflMain.parked);
	WSSignal_Create(&SflMain.stopped);
	wsatomicintstore(&SflMain.emulatorparked, 0);
	SflMain.emulatorrunning = false;
	wsatomicintstore(&SflMain.pendingticks, 0);
	SflMain.secondticks = 0;
	SflMain.nvramticks = 0;
	SflMainParseArguments(argc, argv);
	argc = SflMainCompactArguments(argc, argv);
	PrefsInit(NULL, argc, argv);
	PVR = 0x000c0000;
	CPUClockSpeed = 100000000;
	BusClockSpeed = 100000000;
	TimebaseSpeed = 25000000;
	if (PrefsFindInt32("cpuclock") != 0)
		CPUClockSpeed = (int64)1000000 * PrefsFindInt32("cpuclock");
	if (!SflMemInit()) {
		ErrorAlert(GetString(STR_SFL_ADDRESS_SPACE_ERR));
		return 0;
	}
	if (!SflMainMapMemory())
		return 0;
	timer_init();
	SysInit();
	if (!SflMainLoadRom())
		return 0;
	if (!InitAll(NULL))
		return 0;
	SflMain.initialized = true;
	memcpy(SflMain.lastxpram, XPRAM, XPRAM_SIZE);
#if !defined(SFLMAINRUNEMULATORONMAINTHREAD)
	if (!WSThread_Create(&SflMain.emulator, SflMainEmulatorThread, NULL))
		return 0;
#else
	SflMain.emulatorid = WSThreadGetCurrentID();
#endif /* !SFLMAINRUNEMULATORONMAINTHREAD */
	SflMain.emulatorrunning = true;
	return 1;
}

void SflEmulatorStop(void)
{
#if !defined(SFLMAINRUNEMULATORONMAINTHREAD)
	if (SflMain.emulatorrunning && !wsatomicintload(&SflMain.emulatorparked)) {
		StopEmulation();
		WSSignal_Wait(&SflMain.stopped, SFLMAINSTOPWAITMS);
		if (!wsatomicintload(&SflMain.emulatorparked))
			return;
	}
#endif /* #if !defined(SFLMAINRUNEMULATORONMAINTHREAD) */
	if (SflMain.initialized) {
		exit_emul_ppc();
		ExitAll();
		SysExit();
		SflMain.initialized = false;
	}
	SheepMem::Exit();
	SflMemExit();
	PrefsExit();
}

void SflEmulatorTick(void)
{
	if (!SflMain.emulatorrunning || tick_inhibit)
		return;
	wsatomicintinc(&SflMain.pendingticks);
	TriggerInterrupt();
	USBUIMSampleGuest();
	PPCExceptionStallSample();
}

void SflEmulatorPump(void)
{
	int ticks;
	ticks = wsatomicintswap(&SflMain.pendingticks, 0);
	if (ticks == 0)
		return;
	SetInterruptFlag(INTFLAG_VIA);
	SflMain.secondticks += ticks;
	if (SflMain.secondticks > SFLMAINSECONDTICKS) {
		SflMain.secondticks = 0;
		WriteMacInt32(0x20c, TimerDateTime());
	}
	SflMain.nvramticks += ticks;
	if (SflMain.nvramticks > SFLMAINNVRAMTICKS) {
		SflMain.nvramticks = 0;
		if (memcmp(SflMain.lastxpram, XPRAM, XPRAM_SIZE) != 0) {
			memcpy(SflMain.lastxpram, XPRAM, XPRAM_SIZE);
			SaveXPRAM();
		}
	}
}

const char *SflEmulatorExtFSName(void)
{
	return GetString(STR_EXTFS_VOLUME_NAME);
}

const char *SflEmulatorPrefString(const char *name, int index)
{
	return PrefsFindString(name, index);
}

int SflEmulatorPrefBool(const char *name)
{
	if (PrefsFindBool(name))
		return 1;
	return 0;
}

void jump_to_rom(uint32 entry)
{
	init_emul_ppc();
	emul_ppc(entry);
}

void QuitEmulator(void)
{
	SflUiRequestQuit();
#if defined(SFLMAINRUNEMULATORONMAINTHREAD)
	StopEmulation(); /* SPCFLAG_CPU_STOP; see 642-645 in ppc-cpu.cpp */
#else
	if (SflMain.emulatorrunning && WSThreadGetCurrentID() == SflMain.emulatorid)
		SflMainPark();
#endif /* #if !defined(SFLMAINRUNEMULATORONMAINTHREAD) */
}

void Dump68kRegs(M68kRegisters *r)
{
	int index;
	for (index = 0; index < 8; index++)
		printf("d%d: %08x a%d: %08x\n", index, r->d[index], index, r->a[index]);
}

void MakeExecutable(int dummy, uint32 start, uint32 length)
{
	if (start >= ROMBase && start < ROMBase + ROM_SIZE)
		return;
	FlushCodeCache(start, start + length);
}

struct B2_mutex {
	WSCRITICALSECTION lock;
};

B2_mutex *B2_create_mutex(void)
{
	B2_mutex *mutex;
	mutex = new B2_mutex;
	WSCriticalSection_Construct(&mutex->lock);
	return mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
	WSCriticalSection_Lock(&mutex->lock);
}

void B2_unlock_mutex(B2_mutex *mutex)
{
	WSCriticalSection_Unlock(&mutex->lock);
}

void B2_delete_mutex(B2_mutex *mutex)
{
	WSCriticalSection_Destruct(&mutex->lock);
	delete mutex;
}

void SetInterruptFlag(uint32 flag)
{
	WSCriticalSection_Lock(&SflMain.intflags);
	InterruptFlags |= flag;
	WSCriticalSection_Unlock(&SflMain.intflags);
}

void ClearInterruptFlag(uint32 flag)
{
	WSCriticalSection_Lock(&SflMain.intflags);
	InterruptFlags &= ~flag;
	WSCriticalSection_Unlock(&SflMain.intflags);
}

void DisableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) + 1);
}

void EnableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) - 1);
}

bool SheepMem::Init(void)
{
	page_size = SFLMAINNATIVEPAGE;
	proc = base;
	if (SflMemMap(base, size + SFLMAINSIGSTACKSIZE) == NULL)
		return false;
	zero_page = proc + (size / 2);
	Mac_memset(zero_page, 0, page_size);
	SflMain.sigstack = base + size;
	data = base + size;
	return true;
}

void SheepMem::Exit(void)
{
	data = 0;
}

void ErrorAlert(const char *text)
{
	printf("ERROR: %s\n", text);
	if (PrefsFindBool("nogui"))
		return;
	SflUiAlert(GetString(STR_ERROR_ALERT_TITLE), text);
}

void WarningAlert(const char *text)
{
	printf("WARNING: %s\n", text);
	if (PrefsFindBool("nogui"))
		return;
	SflUiAlert(GetString(STR_WARNING_ALERT_TITLE), text);
}

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	printf(GetString(STR_SHELL_WARNING_PREFIX), text);
	return false;
}
