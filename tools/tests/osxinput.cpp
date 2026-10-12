#include "../../SheepShaver/src/osxguest/osxcuda.cpp"

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
	TestAbsolute(&test, machine);
	free(machine->cpu);
	free(machine);
	printf("checks=%d failures=%d\n", test.checks, test.failures);
	return test.failures != 0;
}
