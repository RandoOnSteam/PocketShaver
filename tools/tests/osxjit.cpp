#include "sysdeps.h"
#include "osxcpu.h"

#define TESTRAMSIZE 0x100000
#define TESTENTRY 0x20000
#define TESTDATA 0x40000
#define TESTBYTES 8192

typedef struct OsxJitTest {
	OsxCpu *reference;
	OsxCpu *native;
	uint32 random;
	int checks;
	int failures;
} OsxJitTest;

typedef struct OsxJitTemplate {
	uint32 opcode;
	uint32 mask;
} OsxJitTemplate;

static uint32 Random(OsxJitTest *test)
{
	test->random ^= test->random << 13;
	test->random ^= test->random >> 17;
	test->random ^= test->random << 5;
	return test->random;
}

static void Put(OsxCpu *cpu, uint32 address, uint32 value)
{
	value = tswap32(value);
	memcpy(cpu->ram + address, &value, 4);
}

static uint32 BusRead(void *context, uint32 address, int size)
{
	return address ^ (uint32)size;
}

static void BusWrite(void *context, uint32 address, int size, uint32 value)
{
	OsxCpu *cpu;
	cpu = (OsxCpu *)context;
	cpu->sia = address ^ value ^ (uint32)size;
}

static void Reset(OsxJitTest *test)
{
	OsxCpu *cpu;
	void *jit;
	int index;
	int reg;
	uint32 value;
	jit = test->native->nativejit;
	for (index = 0; index < 2; index++) {
		if (index == 0)
			cpu = test->reference;
		else
			cpu = test->native;
		OsxCpuReset(cpu, 0x00080202, 16666666);
		cpu->ramsize = TESTRAMSIZE;
		cpu->pc = TESTENTRY;
		cpu->msr = OSXMSRFP;
		cpu->buscontext = cpu;
		cpu->busread = BusRead;
		cpu->buswrite = BusWrite;
		OsxCpuUpdateModes(cpu);
	}
	test->native->nativejit = jit;
	for (reg = 0; reg < 32; reg++) {
		value = Random(test);
		test->reference->gpr[reg] = value;
		test->native->gpr[reg] = value;
	}
	test->reference->cr = Random(test);
	test->native->cr = test->reference->cr;
	test->reference->xer = Random(test) & 0xe000007f;
	test->native->xer = test->reference->xer;
	test->reference->lr = Random(test);
	test->native->lr = test->reference->lr;
	test->reference->ctr = Random(test);
	test->native->ctr = test->reference->ctr;
	test->reference->vrsave = Random(test);
	test->native->vrsave = test->reference->vrsave;
}

static void Check(OsxJitTest *test, int matches, const char *name, uint32 instruction)
{
	test->checks++;
	if (!matches) {
		test->failures++;
		if (test->failures <= 16) {
			printf("FAIL %s instruction %08x\n", name, instruction);
			OsxCpuDump(test->reference, stdout);
			OsxCpuDump(test->native, stdout);
		}
	}
}

static void Compare(OsxJitTest *test, const char *name, uint32 instruction)
{
	OsxCpu *reference;
	OsxCpu *native;
	int matches;
	reference = test->reference;
	native = test->native;
	matches = memcmp(reference->gpr, native->gpr, sizeof(reference->gpr)) == 0;
	matches &= memcmp(reference->fpr, native->fpr, sizeof(reference->fpr)) == 0;
	matches &= memcmp(reference->vr, native->vr, sizeof(reference->vr)) == 0;
	matches &= reference->vscr == native->vscr && reference->vrsave == native->vrsave;
	matches &= memcmp(reference->sr, native->sr, sizeof(reference->sr)) == 0;
	matches &= reference->pc == native->pc && reference->nextpc == native->nextpc;
	matches &= reference->cr == native->cr && reference->xer == native->xer;
	matches &= reference->lr == native->lr && reference->ctr == native->ctr;
	matches &= reference->msr == native->msr && reference->fpscr == native->fpscr;
	matches &= reference->srr0 == native->srr0 && reference->srr1 == native->srr1;
	matches &= reference->dar == native->dar && reference->dsisr == native->dsisr;
	matches &= reference->halted == native->halted && reference->slice == native->slice;
	matches &= reference->instructions == native->instructions && reference->exceptions == native->exceptions;
	matches &= reference->sia == native->sia;
	matches &= reference->reservation == native->reservation && reference->reservationvalid == native->reservationvalid;
	matches &= memcmp(reference->ram + TESTDATA, native->ram + TESTDATA, TESTBYTES) == 0;
	Check(test, matches, name, instruction);
}

static void Run(OsxJitTest *test, uint32 instruction, int count, const char *name)
{
	Put(test->reference, TESTENTRY, instruction);
	Put(test->native, TESTENTRY, instruction);
	Put(test->reference, TESTENTRY + 4, 0);
	Put(test->native, TESTENTRY + 4, 0);
	OsxJitInvalidate(test->native, TESTENTRY, 4096);
	OsxCpuRun(test->reference, count);
	OsxJitRun(test->native, count);
	Compare(test, name, instruction);
}

static void RandomInstructions(OsxJitTest *test)
{
	static const OsxJitTemplate templates[] = {
		{7u << 26, 0x03ffffff}, {10u << 26, 0x039fffff}, {11u << 26, 0x039fffff},
		{14u << 26, 0x03ffffff}, {15u << 26, 0x03ffffff},
		{20u << 26, 0x03ffffff}, {21u << 26, 0x03ffffff}, {23u << 26, 0x03ffffff},
		{24u << 26, 0x03ffffff}, {25u << 26, 0x03ffffff}, {26u << 26, 0x03ffffff},
		{27u << 26, 0x03ffffff}, {28u << 26, 0x03ffffff}, {29u << 26, 0x03ffffff},
		{(31u << 26) | (266u << 1), 0x03fff001}, {(31u << 26) | (40u << 1), 0x03fff001},
		{(31u << 26) | (235u << 1), 0x03fff001}, {(31u << 26) | (28u << 1), 0x03fff801},
		{(31u << 26) | (60u << 1), 0x03fff801}, {(31u << 26) | (124u << 1), 0x03fff801},
		{(31u << 26) | (284u << 1), 0x03fff801}, {(31u << 26) | (316u << 1), 0x03fff801},
		{(31u << 26) | (412u << 1), 0x03fff801}, {(31u << 26) | (444u << 1), 0x03fff801},
		{(31u << 26) | (476u << 1), 0x03fff801}, {(31u << 26) | (922u << 1), 0x03ff0001},
		{(31u << 26) | (954u << 1), 0x03ff0001}, {(31u << 26), 0x039ff800},
		{(31u << 26) | (32u << 1), 0x039ff800}, {(31u << 26) | (19u << 1), 0x03e00000},
		{16u << 26, 0x03ffffff}, {18u << 26, 0x03ffffff},
		{(19u << 26) | (16u << 1), 0x03ff0001}, {(19u << 26) | (528u << 1), 0x03ff0001},
		{(31u << 26) | (8u << 1), 0x03fff401}, {(31u << 26) | (138u << 1), 0x03fff401},
		{(31u << 26) | (10u << 1), 0x03fff801}, {(31u << 26) | (136u << 1), 0x03fff801},
		{(31u << 26) | (202u << 1), 0x03fff801}, {(31u << 26) | (200u << 1), 0x03fff801},
		{(31u << 26) | (234u << 1), 0x03fff801}, {(31u << 26) | (232u << 1), 0x03fff801},
		{(31u << 26) | (104u << 1), 0x03fff801}, {(31u << 26) | (11u << 1), 0x03fff801},
		{(31u << 26) | (75u << 1), 0x03fff801}, {(31u << 26) | (459u << 1), 0x03fff801},
		{(31u << 26) | (491u << 1), 0x03fff801}, {(31u << 26) | (24u << 1), 0x03fff801},
		{(31u << 26) | (536u << 1), 0x03fff801}, {(31u << 26) | (26u << 1), 0x03fff801},
		{(31u << 26) | (824u << 1), 0x03fff801}, {(31u << 26) | (144u << 1), 0x03fff000},
		{(31u << 26) | (598u << 1), 0}, {(31u << 26) | (854u << 1), 0},
		{(31u << 26) | (278u << 1), 0x03fff800}, {(19u << 26) | (150u << 1), 0},
		{8u << 26, 0x03ffffff}, {12u << 26, 0x03ffffff}, {13u << 26, 0x03ffffff},
		{(31u << 26) | (83u << 1), 0x03e00000}, {(31u << 26) | (146u << 1), 0x03e00000},
		{(19u << 26) | (0u << 1), 0x039c0000}, {(19u << 26) | (33u << 1), 0x03fff800},
		{(19u << 26) | (129u << 1), 0x03fff800}, {(19u << 26) | (193u << 1), 0x03fff800},
		{(19u << 26) | (225u << 1), 0x03fff800}, {(19u << 26) | (257u << 1), 0x03fff800},
		{(19u << 26) | (289u << 1), 0x03fff800}, {(19u << 26) | (417u << 1), 0x03fff800},
		{(19u << 26) | (449u << 1), 0x03fff800}, {(31u << 26) | (595u << 1), 0x03ef0000},
		{(31u << 26) | (792u << 1), 0x03fff801}, {(31u << 26) | (210u << 1), 0x03ef0000},
		{(31u << 26) | (242u << 1), 0x03e0f800}
	};
	uint32 instruction;
	int iteration;
	int index;
	for (iteration = 0; iteration < 100000; iteration++) {
		Reset(test);
		index = (int)(Random(test) % (sizeof(templates) / sizeof(templates[0])));
		instruction = templates[index].opcode | (Random(test) & templates[index].mask);
		Run(test, instruction, 1, "integer/branch");
	}
}

static void MemoryInstructions(OsxJitTest *test)
{
	static const uint32 indexed[] = {23,55,87,119,151,183,215,247,279,311,343,375,407,439};
	OsxCpu *cpu;
	uint32 instruction;
	uint32 address;
	uint32 opcode;
	uint32 base;
	uint32 target;
	int iteration;
	int index;
	int kind;
	for (iteration = 0; iteration < 40000; iteration++) {
		Reset(test);
		opcode = 32 + Random(test) % 14;
		base = Random(test) % 32;
		target = Random(test) % 32;
		address = TESTDATA + Random(test) % TESTBYTES;
		if ((iteration & 7) == 0)
			address = TESTDATA + 4095;
		instruction = (opcode << 26) | (target << 21) | (base << 16);
		if (iteration & 1)
			instruction = (31u << 26) | (indexed[opcode - 32] << 1) | (target << 21) | (base << 16) | (31u << 11);
		if ((iteration & 3) == 3 && opcode == 32)
			instruction = (31u << 26) | (534u << 1) | (target << 21) | (base << 16) | (31u << 11);
		if ((iteration & 3) == 3 && opcode == 36)
			instruction = (31u << 26) | (662u << 1) | (target << 21) | (base << 16) | (31u << 11);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->gpr[base] = address;
			cpu->gpr[31] = 0;
			if ((iteration & 3) != 0) {
				for (kind = 0; kind < 2; kind++) {
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].page = address & 0xfffff000;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].segment = 0;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].host = cpu->ram + (address & 0xfffff000);
				}
			}
		}
		Run(test, instruction, 1, "memory");
	}
}

static uint64 FloatBits(OsxJitTest *test)
{
	static const uint64 specials[] = {
		0, 0x8000000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL,
		0x7ff4000000000001ULL, 0x0000000000000001ULL, 0x000fffffffffffffULL, 0x3ff0000000000000ULL,
		0x3810000000000000ULL, 0x3800000000000000ULL, 0x36a0000000000000ULL, 0x47efffffe0000000ULL,
		0x47f0000000000000ULL, 0xc7efffffffffffffULL, 0x3fd5555555555555ULL
	};
	if (Random(test) & 1)
		return specials[Random(test) % (sizeof(specials) / sizeof(specials[0]))];
	return ((uint64)Random(test) << 32) | Random(test);
}

static void FloatVectorMemory(OsxJitTest *test)
{
	static const uint32 indexed[] = {535, 567, 599, 631, 663, 695, 727, 759, 103, 231, 359, 487, 6, 38, 71, 199};
	OsxCpu *cpu;
	uint32 instruction;
	uint32 address;
	uint32 opcode;
	uint32 base;
	uint32 target;
	uint32 extended;
	uint32 word;
	uint64 bits;
	int iteration;
	int index;
	int kind;
	int reg;
	for (iteration = 0; iteration < 60000; iteration++) {
		Reset(test);
		base = 1 + Random(test) % 31;
		target = Random(test) % 32;
		address = TESTDATA + Random(test) % TESTBYTES;
		if ((iteration & 7) == 0)
			address = TESTDATA + 4096 - 4 * (Random(test) % 4);
		if (iteration % 3 == 0) {
			extended = indexed[Random(test) % 16];
			instruction = (31u << 26) | (extended << 1) | (target << 21) | (base << 16) | (31u << 11);
		} else {
			opcode = 48 + Random(test) % 8;
			instruction = (opcode << 26) | (target << 21) | (base << 16) | (Random(test) & 0x3c);
		}
		for (reg = 0; reg < 32; reg++) {
			bits = FloatBits(test);
			test->reference->fpr[reg].bits = bits;
			test->native->fpr[reg].bits = bits;
			for (word = 0; word < 4; word++) {
				test->reference->vr[reg][word] = Random(test);
				test->native->vr[reg][word] = test->reference->vr[reg][word];
			}
		}
		for (word = 0; word < TESTBYTES; word += 4) {
			bits = FloatBits(test);
			test->reference->ram[TESTDATA + word] = (uint8)(bits >> 56);
			test->reference->ram[TESTDATA + word + 1] = (uint8)(bits >> 48);
			test->reference->ram[TESTDATA + word + 2] = (uint8)(bits >> 40);
			test->reference->ram[TESTDATA + word + 3] = (uint8)(bits >> 32);
		}
		memcpy(test->native->ram + TESTDATA, test->reference->ram + TESTDATA, TESTBYTES);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->altivec = 1;
			cpu->msr = OSXMSRFP | OSXMSRVEC;
			if ((iteration % 17) == 0)
				cpu->msr = OSXMSRVEC;
			if ((iteration % 19) == 0)
				cpu->msr = OSXMSRFP;
			cpu->gpr[base] = address;
			cpu->gpr[31] = 0;
			if ((iteration & 3) != 0) {
				for (kind = 0; kind < 2; kind++) {
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].page = address & 0xfffff000;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].segment = 0;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].host = cpu->ram + (address & 0xfffff000);
				}
			}
			OsxCpuUpdateModes(cpu);
		}
		Run(test, instruction, 1, "float/vector memory");
	}
}

static void FloatArithmetic(OsxJitTest *test)
{
	static const uint32 forms[] = {18, 20, 21, 25, 0, 32, 12, 15, 40, 72, 136, 264, 28, 29, 30, 31};
	OsxCpu *cpu;
	uint32 instruction;
	uint32 extended;
	uint32 fpscr;
	uint64 bits;
	int iteration;
	int index;
	int reg;
	for (iteration = 0; iteration < 200000; iteration++) {
		Reset(test);
		extended = forms[Random(test) % 16];
		instruction = ((Random(test) & 31) << 21) | ((Random(test) & 31) << 16) | ((Random(test) & 31) << 11) | (Random(test) & 1);
		if (extended == 25)
			instruction = (instruction & ~0xf800u) | ((Random(test) & 31) << 6);
		if (extended >= 28)
			instruction |= (Random(test) & 31) << 6;
		if ((extended >= 18 && extended <= 25) || extended >= 28) {
			instruction |= extended << 1;
			if (Random(test) & 1)
				instruction |= 59u << 26;
			else
				instruction |= 63u << 26;
		} else
			instruction |= (63u << 26) | (extended << 1);
		fpscr = Random(test) & 0xfff7f0f8u;
		if (Random(test) & 3)
			fpscr &= ~3u;
		else
			fpscr |= Random(test) & 3;
		for (reg = 0; reg < 32; reg++) {
			bits = FloatBits(test);
			if (Random(test) & 1)
				bits &= 0xffffffffe0000000ULL;
			test->reference->fpr[reg].bits = bits;
			test->native->fpr[reg].bits = bits;
		}
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->fpscr = fpscr;
			if ((iteration % 23) == 0)
				cpu->msr = 0;
			OsxCpuUpdateModes(cpu);
		}
		Run(test, instruction, 1, "float arithmetic");
	}
}

static void VectorInstructions(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	uint32 kind;
	uint32 word;
	uint32 vscr;
	uint64 bits;
	int iteration;
	int index;
	int reg;
	for (iteration = 0; iteration < 120000; iteration++) {
		Reset(test);
		kind = Random(test) & 0x7ff;
		if (iteration & 1)
			kind = 32 + Random(test) % 16;
		instruction = (4u << 26) | ((Random(test) & 31) << 21) | ((Random(test) & 31) << 16) | ((Random(test) & 31) << 11) | kind;
		if (kind < 64 && kind >= 32)
			instruction |= (Random(test) & 31) << 6;
		vscr = Random(test) & 0x00010001u;
		for (reg = 0; reg < 32; reg++) {
			for (word = 0; word < 4; word += 2) {
				bits = FloatBits(test);
				test->reference->vr[reg][word] = (uint32)(bits >> 32);
				test->reference->vr[reg][word + 1] = Random(test);
				test->native->vr[reg][word] = test->reference->vr[reg][word];
				test->native->vr[reg][word + 1] = test->reference->vr[reg][word + 1];
			}
		}
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->altivec = 1;
			cpu->vscr = vscr;
			cpu->msr = OSXMSRFP | OSXMSRVEC;
			if ((iteration % 23) == 0)
				cpu->msr = OSXMSRFP;
			OsxCpuUpdateModes(cpu);
		}
		Run(test, instruction, 1, "vector");
	}
}

static void ExtendedMemory(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	uint32 address;
	uint32 base;
	uint32 first;
	int iteration;
	int index;
	int kind;
	int choice;
	for (iteration = 0; iteration < 30000; iteration++) {
		Reset(test);
		choice = (int)(Random(test) % 5);
		base = 1 + Random(test) % 30;
		first = 1 + Random(test) % 31;
		address = (TESTDATA + Random(test) % (TESTBYTES - 160)) & ~3u;
		if ((iteration % 7) == 0)
			address += 1 + Random(test) % 3;
		if ((iteration % 11) == 0)
			address = TESTDATA + 4096 - 8;
		if (choice == 0)
			instruction = (46u << 26) | (first << 21) | (base << 16);
		else if (choice == 1)
			instruction = (47u << 26) | (first << 21) | (base << 16);
		else if (choice == 2)
			instruction = (31u << 26) | (first << 21) | (base << 16) | (31u << 11) | (20u << 1);
		else if (choice == 3)
			instruction = (31u << 26) | (first << 21) | (base << 16) | (31u << 11) | (150u << 1) | 1;
		else
			instruction = (31u << 26) | (base << 16) | (31u << 11) | (1014u << 1);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->gpr[base] = address;
			cpu->gpr[31] = 0;
			cpu->reservationvalid = (iteration >> 1) & 1;
			cpu->reservation = address;
			if (iteration & 4)
				cpu->reservation = address + 4;
			if ((iteration & 3) != 0) {
				for (kind = 0; kind < 2; kind++) {
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].page = address & 0xfffff000;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].segment = 0;
					cpu->tlb[OSXMODEREAL][kind][(address >> 12) & OSXTLBMASK].host = cpu->ram + (address & 0xfffff000);
				}
			}
		}
		Run(test, instruction, 1, "extended memory");
	}
}

static void MachineState(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 value;
	uint32 instruction;
	int iteration;
	int index;
	for (iteration = 0; iteration < 20000; iteration++) {
		Reset(test);
		value = OSXMSRFP;
		if (iteration & 1)
			value |= OSXMSREE;
		if (iteration & 2)
			value ^= OSXMSREE;
		if ((iteration % 13) == 0)
			value |= OSXMSRPOW;
		if ((iteration % 7) == 0)
			value |= OSXMSRDR;
		instruction = (31u << 26) | (5u << 21) | (146u << 1);
		if (iteration & 4)
			instruction = (31u << 26) | (5u << 21) | (83u << 1);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->gpr[5] = value;
			if (iteration & 8)
				cpu->msr |= OSXMSREE;
			if ((iteration % 17) == 0)
				cpu->msr |= OSXMSRPR;
		}
		Put(test->reference, TESTENTRY + 4, (14u << 26) | (6u << 21) | (6u << 16) | 1);
		Put(test->native, TESTENTRY + 4, (14u << 26) | (6u << 21) | (6u << 16) | 1);
		Put(test->reference, TESTENTRY + 8, 0);
		Put(test->native, TESTENTRY + 8, 0);
		Put(test->reference, TESTENTRY, instruction);
		Put(test->native, TESTENTRY, instruction);
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, 2);
		OsxJitRun(test->native, 2);
		Compare(test, "machine state", instruction);
	}
}

static void InlineStops(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	int scenario;
	int prefix;
	int index;
	int step;
	instruction = (31u << 26) | (5u << 21) | (146u << 1);
	for (scenario = 0; scenario < 3; scenario++) {
		for (prefix = 0; prefix < 8; prefix++) {
			Reset(test);
			for (index = 0; index < 2; index++) {
				if (index == 0)
					cpu = test->reference;
				else
					cpu = test->native;
				cpu->gpr[5] = OSXMSREE;
				if (scenario == 0) {
					cpu->gpr[5] = OSXMSRPOW;
					cpu->hid0 = OSXHID0NAP;
				} else {
					cpu->gpr[5] |= OSXMSRDR;
					if (scenario == 1)
						cpu->externalline = 1;
					else
						cpu->decpending = 1;
				}
				for (step = 0; step < prefix; step++)
					Put(cpu, TESTENTRY + (uint32)step * 4, 0x60000000);
				Put(cpu, TESTENTRY + (uint32)prefix * 4, instruction);
				Put(cpu, TESTENTRY + (uint32)(prefix + 1) * 4, 0x38c60001);
				Put(cpu, TESTENTRY + (uint32)(prefix + 2) * 4, 0);
			}
			OsxJitInvalidate(test->native, TESTENTRY, 4096);
			OsxCpuRun(test->reference, prefix + 3);
			OsxJitRun(test->native, prefix + 3);
			Compare(test, "inline CPU stop", instruction);
		}
	}
}

static void SprInstructions(OsxJitTest *test)
{
	uint32 number;
	uint32 instruction;
	int iteration;
	for (iteration = 0; iteration < 10000; iteration++) {
		Reset(test);
		number = Random(test) % 12;
		if (number == 0)
			number = 1;
		else if (number == 11)
			number = 256;
		else if (number < 3)
			number += 7;
		else if (number < 5)
			number += 15;
		else if (number < 7)
			number += 21;
		else
			number += 265;
		if (iteration & 2)
			test->native->msr |= OSXMSRPR;
		if (iteration & 2)
			test->reference->msr |= OSXMSRPR;
		instruction = (31u << 26) | ((Random(test) & 31) << 21) | ((number & 31) << 16) | ((number >> 5) << 11);
		if (iteration & 1)
			instruction |= 467u << 1;
		else
			instruction |= 339u << 1;
		Run(test, instruction, 1, "SPR");
	}
}

static void Sequences(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	int iteration;
	int index;
	int step;
	int count;
	for (iteration = 0; iteration < 10000; iteration++) {
		Reset(test);
		count = 1 + (int)(Random(test) % 65);
		for (step = 0; step < 64; step++) {
			instruction = ((14 + Random(test) % 2) << 26) | (Random(test) & 0x03ffffff);
			if ((step & 7) == 0)
				instruction = (21u << 26) | (Random(test) & 0x03ffffff);
			Put(test->reference, TESTENTRY + (uint32)step * 4, instruction);
			Put(test->native, TESTENTRY + (uint32)step * 4, instruction);
		}
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			Put(cpu, TESTENTRY + 256, 0x4bffff00);
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, count);
		OsxJitRun(test->native, count);
		Compare(test, "sequence/slice", 0);
	}
}

static uint32 MapPage(OsxCpu *cpu, uint32 address, uint32 physical, uint32 flags)
{
	uint32 vsid;
	uint32 page;
	uint32 pteg;
	vsid = cpu->sr[address >> 28] & 0x00ffffff;
	page = (address >> 12) & 0xffff;
	pteg = 0x80000 | (((vsid ^ page) & 0x3ff) << 6);
	Put(cpu, pteg, 0x80000000 | (vsid << 7) | (page >> 10));
	Put(cpu, pteg + 4, physical | flags);
	return pteg + 4;
}

static void Mmu(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 address;
	uint32 flags;
	uint32 instruction;
	uint32 codeaddress;
	uint32 pte;
	int scenario;
	int index;
	int user;
	int store;
	for (scenario = 0; scenario < 96; scenario++) {
		Reset(test);
		user = scenario & 1;
		store = scenario & 2;
		address = 0x20000000 + TESTDATA;
		if (scenario & 4)
			address += 4095;
		codeaddress = 0x10000000 + TESTENTRY;
		instruction = (33u << 26) | (5u << 21) | (4u << 16);
		if (store)
			instruction = (37u << 26) | (5u << 21) | (4u << 16);
		flags = 2;
		if (scenario & 8)
			flags = 1;
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			memset(cpu->ram + 0x80000, 0, 65536);
			cpu->sdr1 = 0x80000;
			cpu->msr = OSXMSRIR | OSXMSRDR | OSXMSRFP;
			if (user)
				cpu->msr |= OSXMSRPR;
			cpu->sr[1] = 3;
			cpu->sr[2] = 0x60000007;
			if (scenario & 16)
				cpu->sr[2] |= 0x80000000;
			pte = MapPage(cpu, codeaddress, TESTENTRY, 2);
			if (scenario & 32)
				Put(cpu, pte, TESTENTRY | 0x0a);
			MapPage(cpu, address, TESTDATA, flags);
			if (!(scenario & 64))
				MapPage(cpu, (address & 0xfffff000) + 4096, TESTDATA + 4096, flags);
			cpu->gpr[4] = address;
			cpu->pc = codeaddress;
			Put(cpu, TESTENTRY, (14u << 26) | (3u << 21) | (3u << 16) | 1);
			Put(cpu, TESTENTRY + 4, instruction);
			Put(cpu, TESTENTRY + 8, 0);
			OsxCpuUpdateModes(cpu);
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, 2);
		OsxJitRun(test->native, 2);
		Compare(test, "hashed MMU/fault", instruction);
		Check(test, memcmp(test->reference->ram + 0x80000, test->native->ram + 0x80000, 65536) == 0, "MMU R/C", instruction);
	}
	for (scenario = 0; scenario < 16; scenario++) {
		Reset(test);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->msr = OSXMSRIR | OSXMSRDR;
			if (scenario & 1)
				cpu->msr |= OSXMSRPR;
			cpu->ibat[0] = 0x8000001f;
			cpu->ibat[1] = 2;
			cpu->dbat[0] = 0x8000001f;
			cpu->dbat[1] = 2;
			if (scenario & 2)
				cpu->dbat[1] = 1;
			if (scenario & 4)
				cpu->dbat[0] &= ~1u;
			if (scenario & 8)
				cpu->dbat[0] &= ~2u;
			cpu->pc = 0x80000000 + TESTENTRY;
			cpu->gpr[4] = 0x80000000 + TESTDATA;
			Put(cpu, TESTENTRY, (37u << 26) | (5u << 21) | (4u << 16));
			Put(cpu, TESTENTRY + 4, 0);
			OsxCpuUpdateModes(cpu);
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		Compare(test, "BAT/protection", 0);
	}
}

static void Invalidation(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	int iteration;
	int index;
	for (iteration = 0; iteration < 1000; iteration++) {
		Reset(test);
		instruction = (14u << 26) | (3u << 21) | (3u << 16) | (Random(test) & 0xffff);
		Run(test, instruction, 1, "invalidate initial");
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->pc = TESTENTRY;
			Put(cpu, TESTENTRY, instruction ^ 0xffff);
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4);
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		Compare(test, "DMA invalidate", instruction);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			Put(cpu, TESTENTRY + 16, (31u << 26) | (4u << 11) | (982u << 1));
			Put(cpu, TESTENTRY + 20, 0);
			Put(cpu, TESTENTRY, instruction);
			cpu->gpr[4] = TESTENTRY;
			cpu->pc = TESTENTRY + 16;
		}
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		test->reference->pc = TESTENTRY;
		test->native->pc = TESTENTRY;
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		Compare(test, "icbi invalidate", instruction);
	}
}

static void Contexts(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint32 instruction;
	uint32 virtualbase;
	uint32 physical;
	int scenario;
	int index;
	Reset(test);
	instruction = 0x48000010;
	Put(test->reference, TESTENTRY, instruction);
	Put(test->native, TESTENTRY, instruction);
	OsxJitInvalidate(test->native, TESTENTRY, 4096);
	for (scenario = 0; scenario < 64; scenario++) {
		virtualbase = (uint32)(scenario % 16) << 28;
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->msr = OSXMSRIR | OSXMSRDR;
			if (scenario & 1)
				cpu->msr |= OSXMSRPR;
			cpu->ibat[0] = virtualbase | 0x1f;
			cpu->ibat[1] = 2;
			cpu->pc = virtualbase + TESTENTRY;
			OsxCpuFlushTlb(cpu);
			OsxCpuUpdateModes(cpu);
		}
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		Compare(test, "physical code aliases", instruction);
	}
	Reset(test);
	instruction = (32u << 26) | (5u << 21) | (4u << 16);
	Put(test->reference, TESTENTRY, instruction);
	Put(test->native, TESTENTRY, instruction);
	Put(test->reference, TESTENTRY + 4, 0);
	Put(test->native, TESTENTRY + 4, 0);
	OsxJitInvalidate(test->native, TESTENTRY, 4096);
	for (scenario = 0; scenario < 32; scenario++) {
		physical = TESTDATA;
		if (scenario & 1)
			physical += 4096;
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->msr = OSXMSRDR;
			cpu->pc = TESTENTRY;
			cpu->gpr[4] = 0x20000000 + TESTDATA;
			cpu->sr[2] = 7 + (uint32)scenario;
			cpu->sdr1 = 0x80000;
			memset(cpu->ram + 0x80000, 0, 65536);
			MapPage(cpu, cpu->gpr[4], physical, 2);
			Put(cpu, physical, (uint32)scenario);
			OsxCpuUpdateModes(cpu);
		}
		OsxCpuRun(test->reference, 1);
		OsxJitRun(test->native, 1);
		Compare(test, "segment TLB tags", instruction);
		Check(test, test->native->gpr[5] == (uint32)scenario, "remapped data", instruction);
	}
	for (scenario = 0; scenario < 8; scenario++) {
		Reset(test);
		instruction = 17u << 26;
		if (scenario & 2)
			instruction = (31u << 26) | (4u << 21) | (146u << 1);
		if (scenario & 4)
			instruction = (19u << 26) | (50u << 1);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->gpr[4] = OSXMSREE;
			cpu->srr0 = TESTENTRY + 256;
			cpu->srr1 = OSXMSREE;
			if (scenario & 1)
				cpu->msr |= OSXMSRPR;
			OsxCpuUpdateModes(cpu);
		}
		Run(test, instruction, 4, "privilege/exception exits");
	}
}

static void Floating(OsxJitTest *test)
{
	static const uint64 values[] = {
		0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL,
		0x0010000000000000ULL, 0x8010000000000000ULL,
		0x3ff0000000000000ULL, 0xbff0000000000000ULL,
		0x3fe0000000000000ULL, 0x4000000000000000ULL,
		0x7fefffffffffffffULL, 0xffefffffffffffffULL,
		0x7ff0000000000000ULL, 0xfff0000000000000ULL,
		0x7ff8000000000042ULL, 0x7ff0000000000042ULL
	};
	static const uint32 operations[] = {20, 21, 25, 40, 72, 136, 264};
	OsxCpu *cpu;
	uint32 instruction;
	uint32 address;
	uint32 opcode;
	uint64 left;
	uint64 right;
	int iteration;
	int index;
	for (iteration = 0; iteration < 20000; iteration++) {
		Reset(test);
		left = ((uint64)Random(test) << 32) | Random(test);
		right = ((uint64)Random(test) << 32) | Random(test);
		if (iteration < 4096) {
			left = values[iteration & 15];
			right = values[(iteration >> 4) & 15];
		}
		opcode = operations[Random(test) % 7];
		instruction = (63u << 26) | (3u << 21) | (4u << 16) | (opcode << 1) | (Random(test) & 1);
		if (opcode == 25)
			instruction |= 5u << 6;
		else
			instruction |= 5u << 11;
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->fpr[4].bits = left;
			cpu->fpr[5].bits = right;
			cpu->fpscr = (uint32)iteration;
			if ((iteration & 15) != 15)
				cpu->msr |= OSXMSRFP;
		}
		Run(test, instruction, 1, "floating arithmetic and moves");
	}
	for (iteration = 0; iteration < 10000; iteration++) {
		Reset(test);
		opcode = 50 + (Random(test) & 1);
		if (iteration & 1)
			opcode += 4;
		address = TESTDATA + (Random(test) & 4095);
		if ((iteration & 7) == 0)
			address = TESTDATA + 4095;
		instruction = (opcode << 26) | (3u << 21) | (4u << 16);
		left = ((uint64)Random(test) << 32) | Random(test);
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->gpr[4] = address;
			cpu->fpr[3].bits = left;
			if ((iteration & 15) != 15)
				cpu->msr |= OSXMSRFP;
			if ((iteration & 3) != 0) {
				cpu->tlb[OSXMODEREAL][OSXTLBREAD][(address >> 12) & OSXTLBMASK].page = address & 0xfffff000;
				cpu->tlb[OSXMODEREAL][OSXTLBREAD][(address >> 12) & OSXTLBMASK].host = cpu->ram + (address & 0xfffff000);
				cpu->tlb[OSXMODEREAL][OSXTLBWRITE][(address >> 12) & OSXTLBMASK] = cpu->tlb[OSXMODEREAL][OSXTLBREAD][(address >> 12) & OSXTLBMASK];
			}
		}
		Run(test, instruction, 1, "double memory and page boundary");
	}
	for (iteration = 0; iteration < 96; iteration++) {
		Reset(test);
		for (index = 0; index < 64; index++) {
			instruction = (63u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (21u << 1);
			Put(test->reference, TESTENTRY + (uint32)index * 4, instruction);
			Put(test->native, TESTENTRY + (uint32)index * 4, instruction);
		}
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->fpr[3].value = 1;
			cpu->fpr[4].value = 2;
			if (iteration & 1)
				cpu->msr |= OSXMSRFP;
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, iteration % 64 + 1);
		OsxJitRun(test->native, iteration % 64 + 1);
		Compare(test, "floating slices and disabled FPU", instruction);
	}
}

static void VectorSequences(OsxJitTest *test)
{
	static const uint32 kinds[] = {
		0, 64, 128, 1024, 1088, 1152, 1028, 1092, 1156, 1220, 1284,
		12, 76, 140, 268, 332, 396, 524, 588, 652, 32, 33, 42, 43, 44, 270
	};
	OsxCpu *cpu;
	uint32 instruction;
	uint32 kind;
	uint32 value;
	int scenario;
	int step;
	int index;
	int reg;
	int word;
	for (scenario = 0; scenario < 192; scenario++) {
		Reset(test);
		for (reg = 0; reg < 32; reg++) {
			for (word = 0; word < 4; word++) {
				value = Random(test);
				test->reference->vr[reg][word] = value;
				test->native->vr[reg][word] = value;
			}
		}
		for (step = 0; step < 32; step++) {
			kind = kinds[Random(test) % (sizeof(kinds) / sizeof(kinds[0]))];
			instruction = (4u << 26) | ((Random(test) & 31) << 21) | ((Random(test) & 31) << 16) | ((Random(test) & 31) << 11) | kind;
			if (kind >= 32 && kind <= 44)
				instruction |= (Random(test) & 31) << 6;
			if (step == 16 && (scenario & 1))
				instruction = (31u << 26) | (5u << 21) | (146u << 1);
			Put(test->reference, TESTENTRY + (uint32)step * 4, instruction);
			Put(test->native, TESTENTRY + (uint32)step * 4, instruction);
		}
		for (index = 0; index < 2; index++) {
			if (index == 0)
				cpu = test->reference;
			else
				cpu = test->native;
			cpu->altivec = 1;
			cpu->msr = OSXMSRVEC | OSXMSRFP;
			cpu->gpr[5] = OSXMSRFP;
		}
		OsxJitInvalidate(test->native, TESTENTRY, 4096);
		OsxCpuRun(test->reference, 32);
		OsxJitRun(test->native, 32);
		Compare(test, "vector sequences and enable changes", 0);
	}
}

static void Benchmark(OsxJitTest *test)
{
	OsxCpu *cpu;
	uint64 start;
	uint64 elapsed;
	int mode;
	int slice;
	int step;
	Reset(test);
	for (mode = 0; mode < 2; mode++) {
		if (mode == 0)
			cpu = test->reference;
		else
			cpu = test->native;
		for (step = 0; step < 31; step++)
			Put(cpu, TESTENTRY + (uint32)step * 4, (14u << 26) | (3u << 21) | (3u << 16) | 1);
		Put(cpu, TESTENTRY + 124, 0x4bffff84);
		OsxJitInvalidate(cpu, TESTENTRY, 4096);
		start = OsxHostNanoseconds();
		for (slice = 0; slice < 10000; slice++) {
			if (mode == 0)
				OsxCpuRun(cpu, 20000);
			else
				OsxJitRun(cpu, 20000);
		}
		elapsed = OsxHostNanoseconds() - start;
		printf("benchmark mode %d instructions 200000000 ns %llu\n", mode, (unsigned long long)elapsed);
	}
	Compare(test, "benchmark", 0);
}

#if OSXG5
static void Put970(Osx970 *core, uint64 address, int size, uint64 value)
{
	int index;
	for (index = size - 1; index >= 0; index--) {
		core->cpu->ram[address + (uint64)index] = (uint8)value;
		value >>= 8;
	}
}

static void Reset970(Osx970 *core, OsxCpu *cpu)
{
	cpu->ramsize = TESTRAMSIZE;
	cpu->buscontext = cpu;
	cpu->busread = BusRead;
	cpu->buswrite = BusWrite;
	Osx970Reset(core, cpu, OSX970PVR);
	cpu->xer = 0;
	cpu->cr = 0;
	core->msr = OSX970MSRHV | OSXMSRFP;
	core->pc = TESTENTRY;
}

static uint64 Step970(Osx970 *core, uint32 instruction)
{
	Put970(core, core->pc, 4, instruction);
	Osx970Run(core, 1);
	return core->gpr[3];
}

static void Expect970(OsxJitTest *test, const char *name, uint64 actual, uint64 expected)
{
	test->checks++;
	if (actual != expected) {
		test->failures++;
		if (test->failures <= 64)
			printf("FAIL 970 %s got %016llx want %016llx\n", name, (unsigned long long)actual, (unsigned long long)expected);
	}
}

static void Differential970(OsxJitTest *test, Osx970 *core)
{
	static const OsxJitTemplate templates[] = {
		{7u << 26, 0x03ffffff}, {10u << 26, 0x039fffff}, {11u << 26, 0x039fffff},
		{14u << 26, 0x03ffffff}, {15u << 26, 0x03ffffff},
		{20u << 26, 0x03ffffff}, {21u << 26, 0x03ffffff}, {23u << 26, 0x03ffffff},
		{24u << 26, 0x03ffffff}, {25u << 26, 0x03ffffff}, {26u << 26, 0x03ffffff},
		{27u << 26, 0x03ffffff}, {28u << 26, 0x03ffffff}, {29u << 26, 0x03ffffff},
		{(31u << 26) | (266u << 1), 0x03fff401}, {(31u << 26) | (40u << 1), 0x03fff401},
		{(31u << 26) | (235u << 1), 0x03fff401}, {(31u << 26) | (28u << 1), 0x03fff801},
		{(31u << 26) | (60u << 1), 0x03fff801}, {(31u << 26) | (124u << 1), 0x03fff801},
		{(31u << 26) | (284u << 1), 0x03fff801}, {(31u << 26) | (316u << 1), 0x03fff801},
		{(31u << 26) | (412u << 1), 0x03fff801}, {(31u << 26) | (444u << 1), 0x03fff801},
		{(31u << 26) | (476u << 1), 0x03fff801}, {(31u << 26) | (922u << 1), 0x03ff0001},
		{(31u << 26) | (954u << 1), 0x03ff0001}, {(31u << 26), 0x039ff800},
		{(31u << 26) | (32u << 1), 0x039ff800}, {(31u << 26) | (19u << 1), 0x03e00000},
		{16u << 26, 0x03ffffff}, {18u << 26, 0x03ffffff},
		{(19u << 26) | (16u << 1), 0x03ff0001}, {(19u << 26) | (528u << 1), 0x03ff0001},
		{(31u << 26) | (8u << 1), 0x03fff401}, {(31u << 26) | (138u << 1), 0x03fff401},
		{(31u << 26) | (10u << 1), 0x03fff401}, {(31u << 26) | (136u << 1), 0x03fff401},
		{(31u << 26) | (202u << 1), 0x03fff401}, {(31u << 26) | (200u << 1), 0x03fff401},
		{(31u << 26) | (234u << 1), 0x03fff401}, {(31u << 26) | (232u << 1), 0x03fff401},
		{(31u << 26) | (104u << 1), 0x03fff401}, {(31u << 26) | (11u << 1), 0x03fff801},
		{(31u << 26) | (75u << 1), 0x03fff801}, {(31u << 26) | (459u << 1), 0x03fff401},
		{(31u << 26) | (491u << 1), 0x03fff401}, {(31u << 26) | (24u << 1), 0x03fff801},
		{(31u << 26) | (536u << 1), 0x03fff801}, {(31u << 26) | (26u << 1), 0x03fff801},
		{(31u << 26) | (824u << 1), 0x03fff801}, {(31u << 26) | (144u << 1), 0x03fff000},
		{(31u << 26) | (792u << 1), 0x03fff801}, {8u << 26, 0x03ffffff}, {12u << 26, 0x03ffffff},
		{13u << 26, 0x03ffffff}, {(19u << 26) | (0u << 1), 0x039c0000},
		{(19u << 26) | (33u << 1), 0x03fff800}, {(19u << 26) | (129u << 1), 0x03fff800},
		{(19u << 26) | (193u << 1), 0x03fff800}, {(19u << 26) | (225u << 1), 0x03fff800},
		{(19u << 26) | (257u << 1), 0x03fff800}, {(19u << 26) | (289u << 1), 0x03fff800},
		{(19u << 26) | (417u << 1), 0x03fff800}, {(19u << 26) | (449u << 1), 0x03fff800},
		{3u << 26, 0x03ffffff}, {(31u << 26) | (4u << 1), 0x03fff800}
	};
	OsxCpu *reference;
	uint32 instruction;
	int iteration;
	int index;
	int reg;
	int matches;
	reference = test->reference;
	for (iteration = 0; iteration < 100000; iteration++) {
		Reset(test);
		Reset970(core, core->cpu);
		for (reg = 0; reg < 32; reg++)
			core->gpr[reg] = ((uint64)Random(test) << 32) | reference->gpr[reg];
		core->cpu->cr = reference->cr;
		core->cpu->xer = reference->xer;
		core->lr = ((uint64)Random(test) << 32) | reference->lr;
		core->ctr = ((uint64)Random(test) << 32) | reference->ctr;
		index = (int)(Random(test) % (sizeof(templates) / sizeof(templates[0])));
		instruction = templates[index].opcode | (Random(test) & templates[index].mask);
		Put(reference, TESTENTRY, instruction);
		OsxCpuRun(reference, 1);
		Put970(core, TESTENTRY, 4, instruction);
		Osx970Run(core, 1);
		matches = 1;
		for (reg = 0; reg < 32; reg++)
			matches &= (uint32)core->gpr[reg] == reference->gpr[reg];
		matches &= core->cpu->cr == reference->cr && core->cpu->xer == reference->xer;
		matches &= (uint32)core->lr == reference->lr && (uint32)core->ctr == reference->ctr;
		matches &= (uint32)core->pc == reference->pc && core->pc <= 0xffffffffULL;
		test->checks++;
		if (!matches) {
			test->failures++;
			if (test->failures <= 16) {
				printf("FAIL 970 differential instruction %08x\n", instruction);
				OsxCpuDump(reference, stdout);
				for (reg = 0; reg < 32; reg += 4)
					printf("970 r%-2d %016llx %016llx %016llx %016llx\n", reg, (unsigned long long)core->gpr[reg], (unsigned long long)core->gpr[reg + 1], (unsigned long long)core->gpr[reg + 2], (unsigned long long)core->gpr[reg + 3]);
				printf("970 pc %016llx cr %08x xer %08x lr %016llx ctr %016llx\n", (unsigned long long)core->pc, core->cpu->cr, core->cpu->xer, (unsigned long long)core->lr, (unsigned long long)core->ctr);
			}
		}
	}
}

static void Wide970(OsxJitTest *test, Osx970 *core)
{
	uint64 value;
	Reset970(core, core->cpu);
	core->msr |= OSX970MSRSF;
	core->gpr[4] = 0x123456789abcdef0ULL;
	core->gpr[5] = 8;
	Expect970(test, "rldicl", Step970(core, 0x78834200), 0x0056789abcdef012ULL);
	Expect970(test, "rldicr", Step970(core, 0x788327c4), 0x2345678900000000ULL);
	Expect970(test, "sld", Step970(core, 0x7c000036 | (4u << 21) | (3u << 16) | (5u << 11)), 0x3456789abcdef000ULL);
	Expect970(test, "srd", Step970(core, 0x7c000436 | (4u << 21) | (3u << 16) | (5u << 11)), 0x00123456789abcdeULL);
	core->gpr[4] = 0xf000000000000010ULL;
	Expect970(test, "srad", Step970(core, 0x7c000634 | (4u << 21) | (3u << 16) | (5u << 11)), 0xfff0000000000000ULL);
	Expect970(test, "srad carry", (core->cpu->xer >> 29) & 1, 1);
	Expect970(test, "sradi", Step970(core, 0x7c000674 | (4u << 21) | (3u << 16) | (4u << 11)), 0xff00000000000001ULL);
	Expect970(test, "sradi carry", (core->cpu->xer >> 29) & 1, 0);
	core->gpr[4] = 0x0000000080000000ULL;
	Expect970(test, "extsw", Step970(core, 0x7c0007b4 | (4u << 21) | (3u << 16)), 0xffffffff80000000ULL);
	Expect970(test, "cntlzd", Step970(core, 0x7c000074 | (4u << 21) | (3u << 16)), 32);
	core->gpr[4] = 0x0000000100000001ULL;
	core->gpr[5] = 0x0000000300000003ULL;
	Expect970(test, "mulld", Step970(core, 0x7c0001d2 | (3u << 21) | (4u << 16) | (5u << 11)), 0x0000000600000003ULL);
	core->gpr[4] = 0xffffffffffffffffULL;
	core->gpr[5] = 0xffffffffffffffffULL;
	Expect970(test, "mulhdu", Step970(core, 0x7c000012 | (3u << 21) | (4u << 16) | (5u << 11)), 0xfffffffffffffffeULL);
	Expect970(test, "mulhd", Step970(core, 0x7c000092 | (3u << 21) | (4u << 16) | (5u << 11)), 0);
	core->gpr[4] = 0x8000000000000000ULL;
	core->gpr[5] = 2;
	Expect970(test, "mulhd negative", Step970(core, 0x7c000092 | (3u << 21) | (4u << 16) | (5u << 11)), 0xffffffffffffffffULL);
	core->gpr[4] = (uint64)(int64)-100;
	core->gpr[5] = 7;
	Expect970(test, "divd", Step970(core, 0x7c0003d2 | (3u << 21) | (4u << 16) | (5u << 11)), (uint64)(int64)-14);
	Expect970(test, "divdu", Step970(core, 0x7c000392 | (3u << 21) | (4u << 16) | (5u << 11)), 0x2492492492492484ULL);
	core->gpr[4] = 0x0000000100000000ULL;
	core->gpr[5] = 1;
	Step970(core, 0x7c200000 | (4u << 16) | (5u << 11));
	Expect970(test, "cmpd", core->cpu->cr >> 28, 4);
	Step970(core, 0x7c000000 | (4u << 16) | (5u << 11));
	Expect970(test, "cmpw", core->cpu->cr >> 28, 8);
	core->gpr[4] = 0xffffffffffffffffULL;
	core->gpr[5] = 1;
	Expect970(test, "add carry 64", Step970(core, 0x7c000014 | (3u << 21) | (4u << 16) | (5u << 11)), 0);
	Expect970(test, "add carry 64 ca", (core->cpu->xer >> 29) & 1, 1);
	core->gpr[4] = 0x00000000ffffffffULL;
	Expect970(test, "add carry 64 no ca", Step970(core, 0x7c000014 | (3u << 21) | (4u << 16) | (5u << 11)), 0x100000000ULL);
	Expect970(test, "add carry 64 no ca flag", (core->cpu->xer >> 29) & 1, 0);
	core->msr &= ~OSX970MSRSF;
	Step970(core, 0x7c000014 | (3u << 21) | (4u << 16) | (5u << 11));
	Expect970(test, "add carry 32 ca flag", (core->cpu->xer >> 29) & 1, 1);
	Expect970(test, "add carry 32 result", core->gpr[3], 0x100000000ULL);
	core->msr |= OSX970MSRSF;
	core->gpr[4] = TESTDATA;
	core->gpr[5] = 0x0102030405060708ULL;
	Step970(core, 0xf8000000 | (5u << 21) | (4u << 16) | 8);
	Expect970(test, "std/ld", Step970(core, 0xe8000000 | (3u << 21) | (4u << 16) | 8), 0x0102030405060708ULL);
	Put970(core, TESTDATA + 16, 4, 0x80000000);
	Expect970(test, "lwa", Step970(core, 0xe8000002 | (3u << 21) | (4u << 16) | 16), 0xffffffff80000000ULL);
	Step970(core, 0xf8000001 | (5u << 21) | (4u << 16) | 32);
	Expect970(test, "stdu update", core->gpr[4], TESTDATA + 32);
	core->gpr[4] = TESTDATA;
	Step970(core, 0x7c0000a8 | (3u << 21) | (4u << 11));
	Expect970(test, "ldarx", core->gpr[3], 0);
	core->gpr[6] = 0x1111222233334444ULL;
	core->cpu->xer = 0;
	Step970(core, 0x7c0001ad | (6u << 21) | (4u << 11));
	Expect970(test, "stdcx success", core->cpu->cr >> 28, 2);
	Step970(core, 0x7c0001ad | (6u << 21) | (4u << 11));
	Expect970(test, "stdcx fail", core->cpu->cr >> 28, 0);
	core->gpr[3] = 0;
	core->gpr[4] = 0xffffffff00000000ULL;
	core->gpr[5] = 0x0000000012345678ULL;
	Expect970(test, "rlwinm wrap", Step970(core, 0x54000000 | (5u << 21) | (3u << 16) | (0u << 11) | (28u << 6) | (3u << 1)), 0x1234567810000008ULL);
	core->ctr = 0x100000001ULL;
	core->msr &= ~OSX970MSRSF;
	core->pc = TESTENTRY;
	Step970(core, 0x42000010);
	Expect970(test, "bdnz 32-bit low zero", core->pc, TESTENTRY + 4);
	core->ctr = 0x100000001ULL;
	core->msr |= OSX970MSRSF;
	core->pc = TESTENTRY;
	Step970(core, 0x42000010);
	Expect970(test, "bdnz 64-bit", core->pc, TESTENTRY + 16);
	core->pc = TESTENTRY;
	core->srr0 = 0x0000000100000104ULL;
	core->srr1 = OSX970MSRSF | OSXMSRFP;
	Step970(core, 0x4c000024);
	Expect970(test, "rfid pc", core->pc, 0x0000000100000104ULL);
	core->pc = TESTENTRY;
	core->msr = OSX970MSRHV | OSXMSRFP | OSX970MSRSF;
	value = core->cpu->exceptions;
	Step970(core, 0x44000002);
	Expect970(test, "sc vector", core->pc, 0xc00);
	Expect970(test, "sc srr0", core->srr0, TESTENTRY + 4);
	Expect970(test, "sc msr", core->msr, OSX970MSRHV | OSX970MSRSF);
	Expect970(test, "sc count", core->cpu->exceptions, value + 1);
}

static void Mmu970(OsxJitTest *test, Osx970 *core)
{
	uint64 physical;
	uint64 vsid;
	uint64 address;
	uint64 hash;
	uint64 pteg;
	Reset970(core, core->cpu);
	core->msr |= OSX970MSRSF;
	core->sdr1 = 0x80000;
	vsid = 0x123456;
	address = 0x0000000120005000ULL;
	core->gpr[4] = (address & 0xfffffffff0000000ULL) | OSX970SLBVALID | 5;
	core->gpr[3] = (vsid << 12) | OSX970SLBKP;
	Step970(core, 0x7c000324 | (3u << 21) | (4u << 11));
	hash = vsid ^ ((address & 0x0fffffffULL) >> 12);
	pteg = 0x80000 | ((hash & 0x7ff) << 7);
	Put970(core, pteg + 16 * 3, 8, (vsid << 12) | ((address >> 16) & 0xf80) | 1);
	Put970(core, pteg + 16 * 3 + 8, 8, 0x60000 | 2);
	core->msr |= OSXMSRDR;
	Expect970(test, "translate hit", (uint64)Osx970Translate(core, address + 0x123, OSXTLBREAD, 0, &physical), 1);
	Expect970(test, "translate address", physical, 0x60123);
	Expect970(test, "translate referenced", core->cpu->ram[pteg + 16 * 3 + 14] & 1, 1);
	Expect970(test, "translate segment miss", (uint64)Osx970Translate(core, 0x0000000220005000ULL, OSXTLBREAD, 0, &physical), 0);
	core->pc = TESTENTRY;
	core->msr &= ~(uint64)OSXMSRDR;
	core->gpr[5] = 0xdeadbeefcafef00dULL;
	core->gpr[4] = address + 0x40;
	core->msr |= OSXMSRDR;
	Put970(core, TESTENTRY, 4, 0xf8000000 | (5u << 21) | (4u << 16));
	Osx970Run(core, 1);
	Expect970(test, "store through mmu", ((uint64)core->cpu->ram[0x60040] << 24) | ((uint64)core->cpu->ram[0x60041] << 16) | ((uint64)core->cpu->ram[0x60042] << 8) | core->cpu->ram[0x60043], 0xdeadbeef);
	Expect970(test, "store changed bit", (core->cpu->ram[pteg + 16 * 3 + 15] >> 7) & 1, 1);
	core->gpr[4] = 0x0000000220005000ULL;
	core->pc = TESTENTRY;
	Osx970Run(core, 1);
	Expect970(test, "data segment vector", core->pc, 0x380);
	Expect970(test, "data segment dar", core->dar, 0x0000000220005000ULL);
}

static uint32 RandomMemory970(OsxJitTest *test)
{
	static const uint32 dform[] = {32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45};
	static const uint32 xform[] = {23, 55, 87, 119, 279, 311, 343, 375, 21, 53, 151, 183, 215, 247, 407, 439, 149, 181};
	uint32 kind;
	uint32 base;
	uint32 target;
	kind = Random(test) % 9;
	base = 20 + Random(test) % 4;
	target = Random(test) % 16;
	if (kind == 4)
		return (31u << 26) | (target << 21) | (base << 16) | (25u << 11) | (20u << 1);
	if (kind == 5)
		return (31u << 26) | (target << 21) | (base << 16) | (25u << 11) | (150u << 1) | 1;
	if (kind == 6)
		return ((46u + Random(test) % 2) << 26) | ((24u + Random(test) % 8) << 21) | (base << 16) | ((Random(test) % 128) & ~3u);
	if (kind == 7)
		return (31u << 26) | (target << 21) | (base << 16) | (25u << 11) | ((103u + (Random(test) % 2) * 128u) << 1);
	if (kind == 8)
		return (7u << 26) | (target << 21) | (base << 16) | (Random(test) & 0xffff);
	kind &= 3;
	if (kind == 0)
		return (dform[Random(test) % 14] << 26) | (target << 21) | (base << 16) | ((Random(test) % 512) & ~1u);
	if (kind == 1)
		return (31u << 26) | (xform[Random(test) % 18] << 1) | (target << 21) | (base << 16) | (24u << 11);
	if (kind == 2)
		return (58u << 26) | (target << 21) | (base << 16) | ((Random(test) % 256) & ~3u) | (Random(test) % 3);
	return (62u << 26) | (target << 21) | (base << 16) | ((Random(test) % 256) & ~3u) | (Random(test) % 2);
}

static void Jit970(OsxJitTest *test, Osx970 *interpreted)
{
	static const OsxJitTemplate templates[] = {
		{14u << 26, 0x01ffffff}, {15u << 26, 0x01ffffff}, {24u << 26, 0x01ffffff}, {25u << 26, 0x01ffffff},
		{26u << 26, 0x01ffffff}, {27u << 26, 0x01ffffff}, {28u << 26, 0x01ffffff}, {29u << 26, 0x01ffffff},
		{10u << 26, 0x03bf7fff}, {11u << 26, 0x03bf7fff}, {20u << 26, 0x01ffffff}, {21u << 26, 0x01ffffff},
		{(31u << 26) | (266u << 1), 0x01eff001}, {(31u << 26) | (40u << 1), 0x01eff001}, {(31u << 26) | (104u << 1), 0x01ef0001},
		{(31u << 26) | (235u << 1), 0x01eff001}, {(31u << 26) | (28u << 1), 0x01eff001}, {(31u << 26) | (60u << 1), 0x01eff001},
		{(31u << 26) | (124u << 1), 0x01eff001}, {(31u << 26) | (284u << 1), 0x01eff001}, {(31u << 26) | (316u << 1), 0x01eff001},
		{(31u << 26) | (412u << 1), 0x01eff001}, {(31u << 26) | (444u << 1), 0x01eff001}, {(31u << 26) | (476u << 1), 0x01eff001},
		{(31u << 26) | (922u << 1), 0x01ef0001}, {(31u << 26) | (954u << 1), 0x01ef0001}, {(31u << 26) | (986u << 1), 0x01ef0001},
		{(31u << 26), 0x03af7800}, {(31u << 26) | (32u << 1), 0x03af7800},
		{(31u << 26) | (339u << 1) | (8u << 16), 0x01e00000}, {(31u << 26) | (339u << 1) | (9u << 16), 0x01e00000},
		{(31u << 26) | (24u << 1), 0x01eff001}, {(31u << 26) | (824u << 1), 0x01eff001},
		{8u << 26, 0x01ffffff}, {12u << 26, 0x01ffffff}, {13u << 26, 0x01ffffff},
		{(31u << 26) | (10u << 1), 0x01eff001}, {(31u << 26) | (138u << 1), 0x01eff001}, {(31u << 26) | (202u << 1), 0x01ef0001},
		{(31u << 26) | (8u << 1), 0x01eff001}, {(31u << 26) | (536u << 1), 0x01eff001}, {(31u << 26) | (26u << 1), 0x01ef0001},
		{(19u << 26) | (33u << 1), 0x03fff800}, {(19u << 26) | (129u << 1), 0x03fff800}, {(19u << 26) | (193u << 1), 0x03fff800},
		{(19u << 26) | (225u << 1), 0x03fff800}, {(19u << 26) | (257u << 1), 0x03fff800}, {(19u << 26) | (289u << 1), 0x03fff800},
		{(19u << 26) | (417u << 1), 0x03fff800}, {(19u << 26) | (449u << 1), 0x03fff800},
		{30u << 26, 0x01fffffb}, {(31u << 26) | (83u << 1), 0x01e00000}
	};
	Osx970 *compiled;
	OsxCpu *cpu;
	uint32 program[24];
	uint32 index;
	int iteration;
	int reg;
	int matches;
	int side;
	Osx970 *core;
	int smallctr;
	cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	compiled = (Osx970 *)calloc(1, sizeof(Osx970));
	cpu->ram = (uint8 *)calloc(1, TESTRAMSIZE);
	Reset970(compiled, cpu);
	compiled->jithot = 1;
	if (!Osx970JitCreate(compiled)) {
		printf("970 jit unavailable\n");
		free(cpu->ram);
		free(compiled);
		free(cpu);
		return;
	}
	for (iteration = 0; iteration < 20000; iteration++) {
		smallctr = Random(test) % 4 == 0;
		for (index = 0; index < 24; index++) {
			reg = (int)(Random(test) % 12);
			if (reg == 0 && index < 20)
				program[index] = (16u << 26) | ((Random(test) % 32) << 21) | ((Random(test) % 32) << 16) | ((1 + Random(test) % (24 - index)) << 2);
			else if (reg == 1 && index > 20)
				program[index] = (19u << 26) | ((Random(test) % 32) << 21) | ((Random(test) % 32) << 16) | (16u << 1);
			else if (reg == 2 && index > 20 && !smallctr)
				program[index] = (19u << 26) | ((20u | (Random(test) % 2) * 8) << 21) | ((Random(test) % 32) << 16) | (528u << 1);
			else if (reg == 3)
				program[index] = (31u << 26) | ((Random(test) % 32) << 21) | (19u << 1);
			else if (reg == 4)
				program[index] = (31u << 26) | ((Random(test) % 32) << 21) | ((Random(test) & 0xff) << 12) | (144u << 1);
			else if (reg == 5)
				program[index] = (31u << 26) | ((Random(test) % 32) << 21) | ((272u + Random(test) % 4) & 31) << 16 | (((272u + Random(test) % 4) >> 5) << 11) | (339u << 1);
			else if (Random(test) % 3 == 0)
				program[index] = RandomMemory970(test);
			else {
				reg = (int)(Random(test) % (sizeof(templates) / sizeof(templates[0])));
				program[index] = templates[reg].opcode | (Random(test) & templates[reg].mask);
			}
		}
		for (side = 0; side < 2; side++) {
			if (side == 0)
				core = interpreted;
			else
				core = compiled;
			if (side == 0)
				Reset970(core, core->cpu);
			else {
				core->msr = OSX970MSRHV | OSXMSRFP;
				core->pc = TESTENTRY;
				core->cpu->cr = 0;
				core->cpu->xer = 0;
			}
			if (iteration & 1)
				core->msr |= OSX970MSRSF;
			if (iteration & 2)
				core->msr |= OSXMSRVEC;
		}
		for (reg = 0; reg < 32; reg++) {
			interpreted->gpr[reg] = ((uint64)Random(test) << 32) | Random(test);
			if (reg >= 20 && reg < 24)
				interpreted->gpr[reg] = TESTDATA + (uint64)(reg - 20) * 1024;
			if (reg == 24)
				interpreted->gpr[reg] = Random(test) % 512;
			if (reg == 25)
				interpreted->gpr[reg] = (Random(test) % 128) * 4 + (Random(test) % 8 == 0);
			compiled->gpr[reg] = interpreted->gpr[reg];
		}
		for (index = 0; index < 32; index++) {
			interpreted->cpu->vr[index][0] = Random(test);
			interpreted->cpu->vr[index][1] = Random(test);
			interpreted->cpu->vr[index][2] = Random(test);
			interpreted->cpu->vr[index][3] = Random(test);
		}
		memcpy(compiled->cpu->vr, interpreted->cpu->vr, sizeof(interpreted->cpu->vr));
		interpreted->reservationvalid = 0;
		compiled->reservationvalid = 0;
		interpreted->cpu->cr = Random(test);
		interpreted->cpu->xer = Random(test) & 0xe000007f;
		compiled->cpu->cr = interpreted->cpu->cr;
		compiled->cpu->xer = interpreted->cpu->xer;
		interpreted->lr = TESTENTRY + 96;
		interpreted->ctr = ((uint64)Random(test) << 32) | (TESTENTRY + 96);
		if (smallctr)
			interpreted->ctr = 1 + Random(test) % 3;
		for (reg = 0; reg < 4; reg++) {
			interpreted->sprg[reg] = ((uint64)Random(test) << 32) | Random(test);
			compiled->sprg[reg] = interpreted->sprg[reg];
		}
		compiled->lr = interpreted->lr;
		compiled->ctr = interpreted->ctr;
		for (index = 0; index < TESTBYTES; index++)
			interpreted->cpu->ram[TESTDATA + index] = (uint8)Random(test);
		memcpy(compiled->cpu->ram + TESTDATA, interpreted->cpu->ram + TESTDATA, TESTBYTES);
		for (index = 0; index < 24; index++) {
			Put970(interpreted, TESTENTRY + index * 4, 4, program[index]);
			Put970(compiled, TESTENTRY + index * 4, 4, program[index]);
		}
		Put970(interpreted, TESTENTRY + 96, 4, 0x48000000);
		Put970(compiled, TESTENTRY + 96, 4, 0x48000000);
		Osx970InvalidatePhysical(compiled, TESTENTRY, 4096);
		Osx970Run(interpreted, 24);
		Osx970Run(compiled, 24);
		matches = 1;
		for (reg = 0; reg < 32; reg++)
			matches &= interpreted->gpr[reg] == compiled->gpr[reg];
		matches &= interpreted->cpu->cr == compiled->cpu->cr && interpreted->cpu->xer == compiled->cpu->xer;
		matches &= interpreted->lr == compiled->lr && interpreted->ctr == compiled->ctr && interpreted->pc == compiled->pc;
		matches &= memcmp(interpreted->cpu->ram + TESTDATA, compiled->cpu->ram + TESTDATA, TESTBYTES) == 0;
		matches &= memcmp(interpreted->cpu->vr, compiled->cpu->vr, sizeof(interpreted->cpu->vr)) == 0;
		matches &= interpreted->reservationvalid == compiled->reservationvalid;
		test->checks++;
		if (!matches) {
			test->failures++;
			if (test->failures <= 8) {
				printf("FAIL 970 jit iteration %d sf %d\n", iteration, iteration & 1);
				for (index = 0; index < 24; index++)
					printf("  %08x\n", program[index]);
				for (reg = 0; reg < 32; reg++)
					if (interpreted->gpr[reg] != compiled->gpr[reg])
						printf("  r%d %016llx jit %016llx\n", reg, (unsigned long long)interpreted->gpr[reg], (unsigned long long)compiled->gpr[reg]);
				printf("  cr %08x jit %08x xer %08x jit %08x pc %llx jit %llx\n", interpreted->cpu->cr, compiled->cpu->cr, interpreted->cpu->xer, compiled->cpu->xer, (unsigned long long)interpreted->pc, (unsigned long long)compiled->pc);
			}
		}
	}
	Osx970JitDestroy(compiled);
	free(cpu->ram);
	free(compiled);
	free(cpu);
}

static void Test970(OsxJitTest *test)
{
	Osx970 *core;
	OsxCpu *cpu;
	cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	core = (Osx970 *)calloc(1, sizeof(Osx970));
	cpu->ram = (uint8 *)calloc(1, TESTRAMSIZE);
	Reset970(core, cpu);
	Differential970(test, core);
	Wide970(test, core);
	Mmu970(test, core);
	Reset970(core, cpu);
	Jit970(test, core);
	free(cpu->ram);
	free(core);
	free(cpu);
}
#endif

int main(void)
{
	OsxJitTest test;
	int result;
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&test, 0, sizeof(test));
	test.random = 0x9e3779b9;
	test.reference = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	test.native = (OsxCpu *)calloc(1, sizeof(OsxCpu));
	test.reference->ram = (uint8 *)calloc(1, TESTRAMSIZE);
	test.native->ram = (uint8 *)calloc(1, TESTRAMSIZE);
	test.native->ramsize = TESTRAMSIZE;
	test.native->jithot = 1;
	if (!OsxJitCreate(test.native))
		return 2;
	RandomInstructions(&test);
	MemoryInstructions(&test);
	FloatVectorMemory(&test);
	FloatArithmetic(&test);
	VectorInstructions(&test);
	ExtendedMemory(&test);
	MachineState(&test);
	InlineStops(&test);
	SprInstructions(&test);
	Sequences(&test);
	Mmu(&test);
	Invalidation(&test);
	Contexts(&test);
	Floating(&test);
	VectorSequences(&test);
	Benchmark(&test);
#if OSXG5
	Test970(&test);
#endif
	printf("checks %d failures %d\n", test.checks, test.failures);
	result = test.failures != 0;
	test.native->logfile = stdout;
	OsxJitDestroy(test.native);
	free(test.reference->ram);
	free(test.native->ram);
	free(test.reference);
	free(test.native);
	return result;
}
