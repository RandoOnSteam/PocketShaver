#include "../../SheepShaver/src/osxguest/osxcuda.cpp"
#include "../../SheepShaver/src/osxguest/osxflash.cpp"

typedef struct OsxInputTest {
	int checks;
	int failures;
} OsxInputTest;

void OsxPicSetLine(OsxMachine *machine, int source, int level)
{
	(void)machine;
	(void)source;
	(void)level;
}

static void Check(OsxInputTest *test, int result, const char *name)
{
	test->checks++;
	if (!result) {
		test->failures++;
		printf("FAIL %s\n", name);
	}
}

static void ReadMouse(OsxMachine *machine)
{
	uint8 request[2];
	request[0] = CUDAPACKETADB;
	request[1] = 0x3c;
	ReceivePacket(machine, request, sizeof(request));
}

static void TestPowerCommands(OsxInputTest *test, OsxMachine *machine)
{
	uint8 request[2] = {CUDAPACKETCUDA, CUDARESETSYSTEM};
	ReceivePacket(machine, request, sizeof(request));
	Check(test, machine->quit == 2, "restart requests startup selection");
	request[1] = CUDAPOWERDOWN;
	ReceivePacket(machine, request, sizeof(request));
	Check(test, machine->quit == 1, "power down exits without restart");
	machine->quit = 0;
}

static void TestBootPreferences(OsxInputTest *test, OsxMachine *machine)
{
	char value[256];
	uint8 startup[4] = {0, 1, 0xff, 0xc1};
	uint32 checksum = 0;
	int index;
	memset(machine->nvram, 0, OSXNVRAMSIZE);
	machine->nvram[0x1800] = 0x12;
	machine->nvram[0x1801] = 0x75;
	machine->nvram[0x1802] = 1;
	machine->nvram[0x1803] = 8;
	machine->nvram[0x1806] = 0x18;
	machine->nvram[0x1807] = 0x5c;
	machine->nvram[0x1808] = 0x20;
	for (index = 0x1834; index < 0x185c; index += 4)
		machine->nvram[index] = 0x20;
	machine->nvram[0x1838] = 0x1f;
	machine->nvram[0x1839] = 0xf0;
	machine->nvram[0x183b] = 3;
	memcpy(machine->nvram + 0x1ff0, "abc", 3);
	machine->nvram[0x1376] = 1;
	machine->nvram[0x1377] = 2;
	machine->nvram[0x1379] = 1;
	machine->nvram[0x137a] = 0xff;
	machine->nvram[0x137b] = 0xc1;
	Check(test, OsxNvramPrepareBoot(machine, 0, 0, 2), "prepare PRAM-selected native boot");
	OsxNvramReadString(machine->nvram, 0, value, sizeof(value));
	Check(test, strcmp(value, "pci/mac-io@10/ata@20000/@0:2") == 0, "canonical native disk selection");
	OsxNvramReadString(machine->nvram, 9, value, sizeof(value));
	Check(test, strcmp(value, "0 bootr") == 0, "canonical native boot command");
	OsxNvramReadString(machine->nvram, 1, value, sizeof(value));
	Check(test, strcmp(value, "abc") == 0, "preserve unrelated firmware string");
	for (index = 0x1800; index < 0x2000; index += 2)
		checksum += ((uint32)machine->nvram[index] << 8) | machine->nvram[index + 1];
	Check(test, checksum % 0xffff == 0, "canonical firmware checksum");
	OsxNvramFinishBoot(machine, startup);
	Check(test, machine->nvram[0x1379] == 1 && machine->nvram[0x137b] == 0xc1,
		"normal native restart preserves PRAM selection");
	Check(test, SetFirmwareBoot(machine->nvram, "/AAPL,ROM", "boot"), "guest selects classic firmware boot");
	OsxNvramFinishBoot(machine, startup);
	Check(test, machine->nvram[0x1378] == 0 && machine->nvram[0x1379] == 0 &&
		machine->nvram[0x137a] == 0 && machine->nvram[0x137b] == 0,
		"classic firmware selection clears stale native PRAM drive");
	Check(test, machine->nvram[0x1376] == 1 && machine->nvram[0x1377] == 2,
		"synchronization preserves adjacent PRAM");
	machine->nvram[0x1379] = 2;
	OsxNvramFinishBoot(machine, startup);
	Check(test, machine->nvram[0x1379] == 2, "preserve newly selected classic drive");
}

static void TestPram(OsxInputTest *test, OsxMachine *machine)
{
	uint8 request[6] = {CUDAPACKETCUDA, CUDASETPRAM, 0, 0x78, 0x12, 0x34};
	OsxCudaReset(machine);
	ReceivePacket(machine, request, sizeof(request));
	Check(test, machine->nvram[OSXPRAMOFFSET + 0x78] == 0x12 &&
		machine->nvram[OSXPRAMOFFSET + 0x79] == 0x34, "CUDA writes shared startup PRAM");
	machine->nvram[OSXPRAMOFFSET + 0x78] = 0x56;
	request[1] = CUDAGETPRAM;
	ReceivePacket(machine, request, 4);
	Check(test, machine->cuda.inlength == 4 && machine->cuda.inbuffer[3] == 0x56,
		"CUDA reads memory-mapped PRAM");
	OsxCudaReset(machine);
	ReceivePacket(machine, request, 4);
	Check(test, machine->cuda.inbuffer[3] == 0x56, "CUDA reset preserves PRAM");
	request[1] = CUDASETPRAM;
	request[3] = 0xff;
	machine->nvram[OSXPRAMOFFSET + OSXPRAMSIZE] = 0x78;
	ReceivePacket(machine, request, sizeof(request));
	Check(test, machine->nvram[OSXPRAMOFFSET + 0xff] == 0x12 &&
		machine->nvram[OSXPRAMOFFSET + OSXPRAMSIZE] == 0x78, "CUDA PRAM write boundary");
}

static void TestAbsolute(OsxInputTest *test, OsxMachine *machine)
{
	OsxCpu *cpu;
	OsxPointer pointer;
	uint32 registers[13];
	uint32 value;
	int index;
	OsxCudaReset(machine);
	machine->width = 1024;
	machine->height = 768;
	machine->cuda.mouseabsolute = 1;
	OsxCudaMouse(machine, 400, 300, 0);
	OsxCudaMouse(machine, 100, 50, 0);
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaMouse(machine, 150, 100, 1);
	OsxCudaMouse(machine, 0, 0, 0);
	for (index = 0; index < 4; index++)
		ReadMouse(machine);
	Check(test, machine->cuda.pointerwrite == 4, "ordered absolute packets");
	Check(test, machine->cuda.pointerqueue[0].x == 500 && machine->cuda.pointerqueue[0].y == 350 && !machine->cuda.pointerqueue[0].button, "combined motion endpoint");
	Check(test, machine->cuda.pointerqueue[1].x == 500 && machine->cuda.pointerqueue[1].y == 350 && machine->cuda.pointerqueue[1].button, "press coordinates");
	Check(test, machine->cuda.pointerqueue[2].x == 650 && machine->cuda.pointerqueue[2].y == 450 && machine->cuda.pointerqueue[2].button, "drag coordinates");
	Check(test, machine->cuda.pointerqueue[3].x == 650 && machine->cuda.pointerqueue[3].y == 450 && !machine->cuda.pointerqueue[3].button, "release coordinates");
	cpu = machine->cpu;
	cpu->ram = (uint8 *)calloc(1, 0x10000);
	cpu->ramsize = 0x10000;
	OsxCpuReset(cpu, OSXPVR, OSXTIMEBASEFREQUENCY);
	memset(&pointer, 0, sizeof(pointer));
	pointer.machine = machine;
	pointer.absoluteaddress = 0x3000;
	for (index = 0; index < 13; index++)
		cpu->gpr[index] = 0x12340000 + index;
	cpu->gpr[1] = 0x8000;
	cpu->gpr[4] = 0;
	cpu->lr = 0x4000;
	cpu->ctr = 0x5000;
	cpu->cr = 0x456789ab;
	cpu->xer = 0x80001234;
	cpu->pc = 0x2000;
	cpu->nextpc = 0x2004;
	memcpy(registers, cpu->gpr, sizeof(registers));
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->nextpc == 0x3000 && cpu->lr == 0x2000 && cpu->gpr[1] == 0x7fa0, "absolute call and stack");
	OsxCpuReadVirtual(cpu, cpu->gpr[5], &value);
	Check(test, value == (500u << 16 | 350), "absolute point ABI");
	OsxCpuReadVirtual(cpu, cpu->gpr[6], &value);
	Check(test, value == 1024, "absolute horizontal bounds");
	OsxCpuReadVirtual(cpu, cpu->gpr[6] + 4, &value);
	Check(test, value == 768, "absolute vertical bounds");
	OsxCpuReadVirtual(cpu, cpu->gpr[1] + 0x38, &value);
	Check(test, value == registers[8] && cpu->gpr[10] == registers[7], "absolute timestamp ABI");
	Check(test, cpu->gpr[4] == 0 && cpu->gpr[7] == 1 && cpu->gpr[8] == 0 && cpu->gpr[9] == 0, "absolute buttons and proximity");
	for (index = 0; index < 13; index++) {
		if (index != 1)
			cpu->gpr[index] = 0xdeadbeef;
	}
	cpu->cr = 0;
	cpu->ctr = 0;
	cpu->xer = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, memcmp(cpu->gpr, registers, sizeof(registers)) == 0 && cpu->nextpc == 0x4000 && cpu->lr == 0x4000 && cpu->ctr == 0x5000 && cpu->cr == 0x456789ab && cpu->xer == 0x80001234, "absolute return restores caller");
	cpu->gpr[4] = 1;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 4, "absolute primary button");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 1;
	OsxPointerHook(cpu, &pointer, 0x2000);
	OsxCpuReadVirtual(cpu, cpu->gpr[5], &value);
	Check(test, value == (650u << 16 | 450) && cpu->gpr[4] == 4, "absolute drag");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 0, "absolute release");
	OsxPointerHook(cpu, &pointer, 0x2000);
	OsxCudaMouse(machine, 10, 10, 0);
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaMouse(machine, 0, 0, 0);
	for (index = 0; index < 3; index++)
		ReadMouse(machine);
	cpu->gpr[4] = 1;
	OsxPointerHook(cpu, &pointer, 0x2000);
	OsxCpuReadVirtual(cpu, cpu->gpr[5], &value);
	Check(test, value == (660u << 16 | 460) && cpu->gpr[4] == 4, "press after dropped motion packet");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	OsxCpuReadVirtual(cpu, cpu->gpr[5], &value);
	Check(test, value == (660u << 16 | 460) && cpu->gpr[4] == 0 && machine->cuda.pointerread == machine->cuda.pointerwrite, "release after dropped motion packet");
	OsxPointerHook(cpu, &pointer, 0x2000);
	OsxCudaMouse(machine, 0, 0, 2);
	OsxCudaMouse(machine, 0, 0, 0);
	OsxCudaMouse(machine, 0, 0, 4);
	OsxCudaMouse(machine, 0, 0, 0);
	for (index = 0; index < 4; index++)
		ReadMouse(machine);
	Check(test, (machine->cuda.inbuffer[3] & 0x80) != 0, "secondary button keeps primary bit released");
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 1, "secondary button press");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 0, "secondary button release");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 2, "middle button press");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->gpr[4] == 0 && machine->cuda.pointerread == machine->cuda.pointerwrite, "middle button release");
	OsxPointerHook(cpu, &pointer, 0x2000);
	cpu->nextpc = 0x2004;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->nextpc == 0x4000 && pointer.callstack == 0, "consumed packet does not replay pointer event");
	pointer.wheeladdress = 0x3800;
	OsxCudaWheel(machine, 1);
	OsxCudaWheel(machine, -1);
	for (index = 0; index < 2; index++)
		ReadMouse(machine);
	cpu->gpr[4] = 0;
	cpu->gpr[7] = 0x11111111;
	cpu->gpr[8] = 0x22222222;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->nextpc == 0x3800 && cpu->gpr[4] == 1 && cpu->gpr[5] == 0 && cpu->gpr[6] == 0 && cpu->gpr[7] == 0x11111111 && cpu->gpr[8] == 0x22222222, "wheel up call");
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->nextpc == 0x4000, "wheel return restores caller");
	cpu->gpr[4] = 0;
	OsxPointerHook(cpu, &pointer, 0x2000);
	Check(test, cpu->nextpc == 0x3800 && cpu->gpr[4] == 0xffffffff && machine->cuda.pointerread == machine->cuda.pointerwrite, "wheel down call");
	OsxPointerHook(cpu, &pointer, 0x2000);
	free(cpu->ram);
}

int main(void)
{
	OsxInputTest test;
	OsxMachine *machine;
	int index;
	int button;
	memset(&test, 0, sizeof(test));
	machine = (OsxMachine *)calloc(1, sizeof(OsxMachine));
	if (!machine)
		return 1;
	machine->cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	if (!machine->cpu) {
		free(machine);
		return 1;
	}
	OsxCpuReset(machine->cpu, OSXPVR, OSXTIMEBASEFREQUENCY);
	OsxCudaReset(machine);
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaMouse(machine, 0, 0, 0);
	ReadMouse(machine);
	Check(&test, machine->cuda.inlength == 5 && machine->cuda.inbuffer[3] == 0 && machine->cuda.inbuffer[4] == 0x80, "press before poll");
	ReadMouse(machine);
	Check(&test, machine->cuda.inlength == 5 && machine->cuda.inbuffer[3] == 0x80 && machine->cuda.inbuffer[4] == 0x80, "release before poll");
	ReadMouse(machine);
	Check(&test, machine->cuda.inlength == 3, "idle after click");
	OsxCudaMouse(machine, 130, -130, 1);
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaMouse(machine, 0, 0, 0);
	ReadMouse(machine);
	Check(&test, machine->cuda.inbuffer[3] == 0x40 && machine->cuda.inbuffer[4] == 0xbf, "press with clamped motion");
	ReadMouse(machine);
	Check(&test, machine->cuda.inbuffer[3] == 0xc0 && machine->cuda.inbuffer[4] == 0xbf, "release with clamped motion");
	ReadMouse(machine);
	Check(&test, machine->cuda.inbuffer[3] == 0xfe && machine->cuda.inbuffer[4] == 0x84, "remaining motion");
	Check(&test, !MouseHasData(&machine->cuda), "motion exhausted");
	for (index = 0; index < 2048; index++)
		OsxCudaMouse(machine, 0, 0, (index + 1) & 1);
	for (index = 0; index < 2048; index++) {
		ReadMouse(machine);
		button = (index + 1) & 1;
		Check(&test, machine->cuda.inlength == 5 && ((machine->cuda.inbuffer[3] & 0x80) == 0) == button, "rapid alternating buttons");
	}
	Check(&test, !MouseHasData(&machine->cuda), "rapid clicks exhausted");
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaMouse(machine, 0, 0, 0);
	machine->cuda.autopoll = 1;
	machine->cuda.inlength = 0;
	AutoPoll(machine);
	Check(&test, machine->cuda.inlength == 5 && machine->cuda.inbuffer[1] == 0x40 && machine->cuda.inbuffer[3] == 0, "autopoll press");
	machine->cuda.inlength = 0;
	AutoPoll(machine);
	Check(&test, machine->cuda.inlength == 5 && machine->cuda.inbuffer[3] == 0x80, "autopoll release");
	OsxCudaMouse(machine, 0, 0, 1);
	OsxCudaReset(machine);
	Check(&test, !MouseHasData(&machine->cuda), "reset clears pending input");
	TestPowerCommands(&test, machine);
	TestBootPreferences(&test, machine);
	TestPram(&test, machine);
	TestAbsolute(&test, machine);
	free(machine->cpu);
	free(machine);
	printf("checks=%d failures=%d\n", test.checks, test.failures);
	return test.failures != 0;
}
