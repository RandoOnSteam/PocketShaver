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
	static const uint32 forms[] = {18, 20, 21, 25, 0, 32, 12, 15, 40, 72, 136, 264};
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
		extended = forms[Random(test) % 12];
		instruction = ((Random(test) & 31) << 21) | ((Random(test) & 31) << 16) | ((Random(test) & 31) << 11) | (Random(test) & 1);
		if (extended == 25)
			instruction = (instruction & ~0xf800u) | ((Random(test) & 31) << 6);
		if (extended >= 18 && extended <= 25) {
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
