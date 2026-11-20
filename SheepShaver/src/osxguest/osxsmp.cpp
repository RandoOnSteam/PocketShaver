/*
 *  osxsmp.cpp - one host thread per OSX guest CPU common routines
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
#include "osxhost.h"
#include "osxmachine.h"
#include "prefs.h"

#if OSXSMP

#define SMPSPINS 4000
#define SMPSLICE 20000
#define SMPSLICELIMIT 262144
#define SMPSLICEFLOOR 256
#define SMPQUANTUM (OSXTIMEBASEFREQUENCY / 4000)
#define SMPPARKMILLISECONDS 100
#define SMPMILLISECOND 1000
#define SMPCOARSEMICROSECONDS 2000
#define SMPPOLLMICROSECONDS 300
#define SMPSTALLBEATS 1526
#define SMPBEATNANOSECONDS 65536
#define SMPIDLESPINNANOSECONDS 100000

static void OsxGuestSmpRelax(uint32 *spins)
{
	(*spins)++;
	if (*spins < SMPSPINS) {
		OsxGuestAtomicPause();
		return;
	}
	OsxGuestHostSleep(0);
}

static uint32 OsxGuestSmpBeatNow(void)
{
	return (uint32)(OsxGuestHostNanoseconds() / SMPBEATNANOSECONDS);
}

void OsxGuestSmpLock(OsxCpu *cpu)
{
	OsxMachine *machine;
	int32 running;
#if OSXSMPTRACE
	uint64 started;
#endif
	machine = (OsxMachine *)cpu->owner;
#if OSXSMPTRACE
	cpu->tracelocks++;
#endif
	if (OsxGuestHostMutexTry(machine->lock))
		return;
#if OSXSMPTRACE
	started = OsxGuestHostNanoseconds();
#endif
	running = cpu->running;
	if (running)
		OsxGuestAtomicExchange(&cpu->running, 0);
	OsxGuestHostMutexEnter(machine->lock);
	if (running) {
		OsxGuestAtomicExchange(&cpu->running, 1);
		OsxGuestSmpDrain(cpu);
	}
#if OSXSMPTRACE
	cpu->tracelockwait += OsxGuestHostNanoseconds() - started;
#endif
}

void OsxGuestSmpUnlock(OsxCpu *cpu)
{
	OsxGuestHostMutexLeave(((OsxMachine *)cpu->owner)->lock);
}

uint32 OsxGuestSmpBusRead(void *context, uint32 address, int size)
{
	OsxCpu *cpu;
	uint32 value;
	cpu = (OsxCpu *)context;
	OsxGuestSmpLock(cpu);
	((OsxMachine *)cpu->owner)->accessor = cpu->cpuindex;
	value = OsxGuestBusRead(cpu->owner, address, size);
	OsxGuestSmpUnlock(cpu);
	return value;
}

void OsxGuestSmpBusWrite(void *context, uint32 address, int size, uint32 value)
{
	OsxCpu *cpu;
	cpu = (OsxCpu *)context;
	OsxGuestSmpLock(cpu);
	((OsxMachine *)cpu->owner)->accessor = cpu->cpuindex;
	OsxGuestBusWrite(cpu->owner, address, size, value);
	OsxGuestSmpUnlock(cpu);
}

void OsxGuestSmpWake(OsxCpu *cpu)
{
	OsxGuestAtomicExchange(&cpu->kick, 1);
#if OSXSMPTRACE
	cpu->tracekicks++;
#endif
	if (!cpu->sleeping)
		return;
	cpu->wakebeat = OsxGuestSmpBeatNow() | 1;
	OsxGuestHostSignalRaise((OsxSmpSignal *)cpu->wake);
}

void OsxGuestSmpSetLine(OsxMachine *machine, int index, int level)
{
	OsxCpu *cpu;
	cpu = machine->cpus[index];
	if (cpu->externalline == level)
		return;
	cpu->externalline = level;
	if (!level)
		return;
	if (machine->cpucount <= 1) {
		cpu->halted = 0;
		return;
	}
	OsxGuestAtomicExchange(&cpu->lineraised, 1);
	OsxGuestSmpWake(cpu);
}

void OsxGuestSmpKickOthers(OsxCpu *cpu)
{
	OsxMachine *machine;
	int index;
	machine = (OsxMachine *)cpu->owner;
	for (index = 0; index < machine->cpucount; index++)
		if (machine->cpus[index] != cpu && machine->cpus[index]->state != OSXSMPRESET)
			OsxGuestAtomicExchange(&machine->cpus[index]->kick, 1);
}

void OsxGuestSmpFlush(OsxCpu *cpu, uint32 page)
{
	OsxMachine *machine;
	OsxCpu *target;
	uint32 spins;
	int index;
#if OSXSMPTRACE
	uint64 started;
	started = OsxGuestHostNanoseconds();
	cpu->traceflushes++;
#endif
	machine = (OsxMachine *)cpu->owner;
	for (index = 0; index < machine->cpucount; index++) {
		target = machine->cpus[index];
		if (target == cpu || target->state == OSXSMPRESET)
			continue;
		while (OsxGuestAtomicCompare(&target->flushlock, 0, 1) != 0)
			OsxGuestAtomicPause();
		if (page == OSXSMPFLUSHEVERYTHING || target->flushcount >= OSXSMPFLUSHES)
			target->flushcount = OSXSMPFLUSHES + 1;
		else
			target->flushpages[target->flushcount++] = page;
		target->flushposted++;
		OsxGuestAtomicCompare(&target->flushlock, 1, 0);
		OsxGuestAtomicExchange(&target->kick, 1);
	}
	for (index = 0; index < machine->cpucount; index++) {
		target = machine->cpus[index];
		if (target == cpu || target->state == OSXSMPRESET)
			continue;
		spins = 0;
		while (target->running && target->flushdone != target->flushposted && !machine->quit) {
			OsxGuestSmpDrain(cpu);
			OsxGuestSmpRelax(&spins);
		}
	}
#if OSXSMPTRACE
	cpu->traceflushwait += OsxGuestHostNanoseconds() - started;
#endif
}

int OsxGuestSmpClaimHost(OsxCpu *cpu)
{
	OsxMachine *machine;
	if (cpu->cpucount <= 1 || cpu->hostthread == 0)
		return 1;
	machine = (OsxMachine *)cpu->owner;
	OsxGuestAtomicCompare(&machine->hostwanted, 0, (uint32)cpu->cpuindex + 1);
	cpu->restart = 1;
	OsxGuestSmpWake(machine->cpu);
	return 0;
}

static void OsxGuestSmpHost(OsxMachine *machine, int thread)
{
	OsxCpu *wanted;
	OsxCpu *mine;
	uint32 request;
	uint32 spins;
	int other;
	request = machine->hostwanted;
	if (!request)
		return;
	wanted = machine->cpus[request - 1];
	mine = machine->threadcpus[thread];
	spins = 0;
	if (thread != 0) {
		if (wanted != mine)
			return;
		OsxGuestAtomicExchange(&machine->hostparked, 1);
		while (machine->hostwanted == request && !machine->quit)
			OsxGuestSmpRelax(&spins);
		return;
	}
	if (wanted != mine) {
		while (!machine->hostparked && !machine->quit)
			OsxGuestSmpRelax(&spins);
		other = wanted->hostthread;
		machine->threadcpus[0] = wanted;
		machine->threadcpus[other] = mine;
		wanted->hostthread = 0;
		mine->hostthread = other;
		machine->cpu = wanted;
#if OSXSMPTRACE
		wanted->traceswaps++;
#endif
		OsxGuestAtomicExchange(&machine->hostparked, 0);
	}
	OsxGuestAtomicCompare(&machine->hostwanted, request, 0);
	OsxGuestAtomicFence();
}

static void OsxGuestSmpBeat(OsxMachine *machine, OsxCpu *cpu)
{
	OsxCpu *other;
	uint32 now;
	uint32 previous;
	uint32 gap;
	uint32 wakebeat;
	int stalled;
	int index;
	now = OsxGuestSmpBeatNow();
	previous = cpu->beat;
	wakebeat = cpu->wakebeat;
	if (wakebeat) {
		previous = wakebeat;
		cpu->wakebeat = 0;
	}
	gap = now - previous;
	cpu->beat = now;
	if (gap > 2 * SMPSTALLBEATS && gap < 0x80000000u)
		OsxGuestSmpRewindTime(cpu, ((uint64)(gap - 2 * SMPSTALLBEATS) * SMPBEATNANOSECONDS / 1000) * (OSXTIMEBASEFREQUENCY / 1000) / 1000);
	stalled = 0;
	for (index = 0; index < machine->cpucount; index++) {
		other = machine->cpus[index];
		if (other == cpu || other->state == OSXSMPRESET)
			continue;
		wakebeat = other->wakebeat;
		gap = now - other->beat;
		if (other->state == OSXSMPRUN && gap > SMPSTALLBEATS && gap < 0x80000000u)
			stalled = 1;
		gap = now - wakebeat;
		if (wakebeat && gap > SMPSTALLBEATS && gap < 0x80000000u)
			stalled = 1;
	}
	if (stalled != ((cpu->primary->timefrozen & OSXSMPFREEZESTALL) != 0))
		OsxGuestSmpFreezeTime(cpu, OSXSMPFREEZESTALL, stalled);
}

OsxCpu *OsxGuestSmpBegin(OsxMachine *machine, int thread)
{
	OsxCpu *cpu;
	int index;
	if (machine->inlinecpus) {
		cpu = machine->cpu;
		for (index = 1; index <= machine->cpucount; index++) {
			cpu = machine->cpus[(machine->cpu->cpuindex + index) % machine->cpucount];
			if (cpu->state != OSXSMPRESET)
				break;
		}
		machine->cpu = cpu;
		machine->threadcpus[0] = cpu;
		cpu->kick = 0;
		OsxGuestSmpDrain(cpu);
		return cpu;
	}
	OsxGuestSmpHost(machine, thread);
	cpu = machine->threadcpus[thread];
	OsxGuestAtomicExchange(&cpu->kick, 0);
	if (cpu->state != OSXSMPRESET)
		OsxGuestSmpBeat(machine, cpu);
	OsxGuestAtomicExchange(&cpu->running, 1);
	OsxGuestSmpDrain(cpu);
	return cpu;
}

void OsxGuestSmpEnd(OsxCpu *cpu)
{
	OsxGuestAtomicExchange(&cpu->running, 0);
}

void OsxGuestSmpInterrupts(OsxCpu *cpu, uint64 now)
{
	uint32 taken;
	if (cpu->lineraised) {
		OsxGuestAtomicExchange(&cpu->lineraised, 0);
		cpu->halted = 0;
	}
	if (!cpu->decpending && now >= cpu->capdeadline)
		cpu->decpending = 1;
	taken = cpu->decrementercount + cpu->externalcount;
#if OSXG5
	if (cpu->core970)
		OsxGuest970CheckInterrupts((Osx970 *)cpu->core970, now);
	else
#endif
	OsxGuestCpuCheckInterrupts(cpu, now);
	if (cpu->decrementercount + cpu->externalcount != taken)
		cpu->capdeadline = now + OSXSMPDECREMENTERCAP;
}

static void OsxGuestSmpSleep(OsxCpu *cpu, uint32 microseconds)
{
	uint64 deadline;
	uint64 now;
	uint32 remaining;
	deadline = OsxGuestHostNanoseconds() + (uint64)microseconds * SMPMILLISECOND;
	if (microseconds >= SMPCOARSEMICROSECONDS && OsxGuestHostSignalWait((OsxSmpSignal *)cpu->wake, microseconds / SMPMILLISECOND - 1))
		return;
	now = OsxGuestHostNanoseconds();
	while (!cpu->kick && now < deadline) {
		remaining = (uint32)((deadline - now) / SMPMILLISECOND);
		if (remaining > SMPPOLLMICROSECONDS)
			remaining = SMPPOLLMICROSECONDS;
		OsxGuestHostShortSleep(remaining);
		now = OsxGuestHostNanoseconds();
	}
	if (cpu->kick)
		OsxGuestHostSignalWait((OsxSmpSignal *)cpu->wake, 0);
}

void OsxGuestSmpIdle(OsxCpu *cpu, uint64 ticks)
{
	OsxMachine *machine;
	uint64 spinend;
	uint32 microseconds;
	int index;
	machine = (OsxMachine *)cpu->owner;
	microseconds = (uint32)(ticks * 1000000 / OSXTIMEBASEFREQUENCY);
	OsxGuestAtomicExchange(&cpu->running, 0);
	if (machine->inlinecpus) {
		for (index = 0; index < machine->cpucount; index++)
			if (machine->cpus[index]->state != OSXSMPRESET && !machine->cpus[index]->halted)
				return;
		OsxGuestHostShortSleep(microseconds);
		return;
	}
	OsxGuestAtomicExchange(&cpu->state, OSXSMPIDLE);
	spinend = OsxGuestHostNanoseconds() + SMPIDLESPINNANOSECONDS;
	while (!cpu->kick && microseconds > 0 && OsxGuestHostNanoseconds() < spinend)
		OsxGuestAtomicPause();
	OsxGuestAtomicExchange(&cpu->sleeping, 1);
	if (!cpu->kick && microseconds > 0)
		OsxGuestSmpSleep(cpu, microseconds);
	OsxGuestAtomicExchange(&cpu->sleeping, 0);
	if (!cpu->wakebeat)
		cpu->beat = OsxGuestSmpBeatNow();
	OsxGuestAtomicExchange(&cpu->state, OSXSMPRUN);
}

void OsxGuestSmpStart(OsxMachine *machine, int index)
{
	OsxCpu *cpu;
	cpu = machine->cpus[index];
	if (cpu->state != OSXSMPRESET)
		return;
	cpu->pc = OSXVECTORRESET;
	cpu->nextpc = cpu->pc;
	cpu->msr = 0;
	OsxGuestCpuUpdateModes(cpu);
#if OSXG5
	if (cpu->core970) {
		((Osx970 *)cpu->core970)->pc = OSXVECTORRESET;
		((Osx970 *)cpu->core970)->nextpc = OSXVECTORRESET + 4;
		((Osx970 *)cpu->core970)->msr = OSX970MSRSF | OSX970MSRHV;
		((Osx970 *)cpu->core970)->hior = 0;
	}
#endif
	cpu->halted = 0;
	cpu->beat = OsxGuestSmpBeatNow();
	cpu->capdeadline = OsxGuestCpuTimebase(cpu) + OSXSMPDECREMENTERCAP;
	if (machine->log)
		fprintf(machine->log, "smp cpu %d released from reset\n", index);
	OsxGuestAtomicExchange(&cpu->state, OSXSMPRUN);
	OsxGuestHostSignalRaise((OsxSmpSignal *)cpu->wake);
}

static void OsxGuestSmpThread(void *context)
{
	OsxSmpSlot *slot;
	OsxMachine *machine;
	OsxCpu *cpu;
	uint64 now;
	uint64 start;
	uint64 instructions;
	uint64 wait;
	uint64 sliceticks;
	int32 slicesize;
	slot = (OsxSmpSlot *)context;
	machine = slot->machine;
	slicesize = SMPSLICE;
	while (!machine->quit) {
		cpu = OsxGuestSmpBegin(machine, slot->index);
		if (cpu->state == OSXSMPRESET) {
			OsxGuestSmpEnd(cpu);
			OsxGuestHostSignalWait((OsxSmpSignal *)cpu->wake, SMPPARKMILLISECONDS);
			continue;
		}
		now = OsxGuestCpuTimebase(cpu);
		OsxGuestSmpInterrupts(cpu, now);
		if (cpu->halted) {
			wait = OSXSMPDECREMENTERCAP;
			if (cpu->capdeadline > now && cpu->capdeadline - now < wait)
				wait = cpu->capdeadline - now;
			if (cpu->decdeadline > now && cpu->decdeadline - now < wait)
				wait = cpu->decdeadline - now;
			OsxGuestSmpIdle(cpu, wait);
			continue;
		}
		start = now;
		instructions = cpu->instructions;
#if OSXG5
		if (cpu->core970)
			OsxGuest970Run((Osx970 *)cpu->core970, slicesize);
		else
#endif
		OsxGuestJitRun(cpu, slicesize);
		now = OsxGuestCpuTimebase(cpu);
		if (cpu->instructions - instructions >= (uint32)slicesize && now > start) {
			sliceticks = (cpu->instructions - instructions) * SMPQUANTUM / (now - start);
			if (sliceticks < SMPSLICEFLOOR)
				sliceticks = SMPSLICEFLOOR;
			if (sliceticks > SMPSLICELIMIT)
				sliceticks = SMPSLICELIMIT;
			slicesize = (int32)sliceticks;
		}
		OsxGuestSmpEnd(cpu);
	}
}

void OsxGuestSmpStatus(OsxMachine *machine, FILE *log)
{
	OsxCpu *cpu;
	int index;
#if OSXSMPTRACE
	uint32 frame;
	uint32 caller;
	int depth;
#endif
	if (machine->cpucount <= 1)
		return;
	for (index = 0; index < machine->cpucount; index++) {
		cpu = machine->cpus[index];
		fprintf(log, "smp cpu %d thread %d state %d pc %08x msr %08x instructions %llu halted %d dec %u ext %u exceptions %llu frozen %d\n", index, cpu->hostthread, (int)cpu->state, cpu->pc, cpu->msr, (unsigned long long)cpu->instructions, cpu->halted, cpu->decrementercount, cpu->externalcount, (unsigned long long)cpu->exceptions, (int)cpu->primary->timefrozen);
#if OSXSMPTRACE
		fprintf(log, "smp trace %d locks %llu lockwaitus %llu kicks %llu flushes %llu flushwaitus %llu stwcxfail %llu swaps %llu\n", index, (unsigned long long)cpu->tracelocks, (unsigned long long)(cpu->tracelockwait / 1000), (unsigned long long)cpu->tracekicks, (unsigned long long)cpu->traceflushes, (unsigned long long)(cpu->traceflushwait / 1000), (unsigned long long)cpu->tracefailures, (unsigned long long)cpu->traceswaps);
		fprintf(log, "smp frames %d lr %08x r1 %08x r3 %08x sr0 %08x line %d decpending %d chain", index, cpu->lr, cpu->gpr[1], cpu->gpr[3], cpu->sr[0], cpu->externalline, cpu->decpending);
		frame = cpu->gpr[1];
		for (depth = 0; depth < 10 && frame && OsxGuestCpuReadVirtual(cpu, frame, &frame) && frame && OsxGuestCpuReadVirtual(cpu, frame + 8, &caller); depth++)
			fprintf(log, " %08x", caller);
		fprintf(log, "\n");
#endif
	}
}

int OsxGuestSmpCreate(OsxMachine *machine, int count)
{
	OsxCpu *primary;
	OsxCpu *cpu;
	const char *model;
	int index;
	primary = machine->cpu;
	machine->cpus[0] = primary;
	machine->threadcpus[0] = primary;
	machine->cpucount = 1;
	model = PrefsFindString("osxmodel");
	if (model && strcmp(model, "PowerMac3,3") == 0)
		machine->core99 = 1;
	if (count > OSXSMPCPUS)
		count = OSXSMPCPUS;
	if (count > 1 && !primary->deterministic && machine->g5)
		machine->cpucount = count;
	if (count > 1 && !primary->deterministic && !machine->newworld && !PrefsFindBool("osxcpu970")) {
		machine->core99 = 1;
		machine->cpucount = count;
	}
	if (machine->core99)
		machine->newworld = 1;
	if (machine->cpucount == 1)
		return 1;
	machine->inlinecpus = PrefsFindBool("osxsmpinline");
	machine->lock = OsxGuestHostMutexCreate();
	if (machine->lock == NULL)
		return 0;
	for (index = 0; index < machine->cpucount; index++) {
		cpu = primary;
		if (index > 0) {
			cpu = (OsxCpu *)calloc(1, sizeof(OsxCpu));
			if (cpu == NULL)
				return 0;
			OsxGuestCpuReset(cpu, OSXPVRG4, OSXTIMEBASEFREQUENCY);
			cpu->ram = primary->ram;
			cpu->ramsize = primary->ramsize;
			cpu->vram = primary->vram;
			cpu->vrambase = primary->vrambase;
			cpu->vramsize = primary->vramsize;
			cpu->logfile = primary->logfile;
			cpu->halted = 1;
			cpu->state = OSXSMPRESET;
			if (!machine->inlinecpus)
				cpu->hostthread = index;
		} else
			cpu->state = OSXSMPRUN;
		machine->cpus[index] = cpu;
		machine->threadcpus[index] = cpu;
		machine->slots[index].machine = machine;
		machine->slots[index].index = index;
		cpu->primary = primary;
		cpu->owner = machine;
		cpu->cpuindex = index;
		cpu->cpucount = machine->cpucount;
		cpu->buscontext = cpu;
		cpu->busread = OsxGuestSmpBusRead;
		cpu->buswrite = OsxGuestSmpBusWrite;
		cpu->wake = OsxGuestHostSignalCreate();
		if (cpu->wake == NULL)
			return 0;
	}
	return 1;
}

#if OSXG5
static int OsxGuestSmpCore(OsxCpu *cpu, OsxCpu *primary)
{
	Osx970 *core;
	core = (Osx970 *)calloc(1, sizeof(Osx970));
	if (core == NULL)
		return 0;
	OsxGuest970Reset(core, cpu, primary->pvr);
	core->jithot = ((Osx970 *)primary->core970)->jithot;
	cpu->highram = primary->highram;
	cpu->core970 = core;
	if (((Osx970 *)primary->core970)->jit)
		OsxGuest970JitCreate(core);
	return 1;
}
#endif

int OsxGuestSmpLaunch(OsxMachine *machine)
{
	OsxCpu *primary;
	OsxCpu *cpu;
	int index;
	if (machine->cpucount <= 1)
		return 1;
	primary = machine->cpus[0];
	primary->beat = OsxGuestSmpBeatNow();
	primary->capdeadline = OsxGuestCpuTimebase(primary) + OSXSMPDECREMENTERCAP;
	for (index = 1; index < machine->cpucount; index++) {
		cpu = machine->cpus[index];
		OsxGuestCpuSetModel(cpu, primary->pvr);
		cpu->syscallobserver = primary->syscallobserver;
		cpu->syscallcontext = primary->syscallcontext;
		cpu->jithot = primary->jithot;
		if (primary->nativejit && !OsxGuestJitCreate(cpu))
			return 0;
#if OSXG5
		if (primary->core970 && !OsxGuestSmpCore(cpu, primary))
			return 0;
#endif
	}
	if (machine->log)
		fprintf(machine->log, "smp %d cpus inline %d\n", machine->cpucount, machine->inlinecpus);
	if (machine->inlinecpus)
		return 1;
	for (index = 1; index < machine->cpucount; index++) {
		machine->slots[index].thread = OsxGuestHostThreadStart(OsxGuestSmpThread, &machine->slots[index]);
		if (machine->slots[index].thread == NULL)
			return 0;
	}
	return 1;
}

void OsxGuestSmpJoin(OsxMachine *machine)
{
	int index;
	if (machine->cpucount <= 1 || machine->lock == NULL)
		return;
	if (!machine->quit)
		machine->quit = 1;
	for (index = 1; index < machine->cpucount; index++) {
		if (machine->slots[index].thread == NULL)
			continue;
		OsxGuestHostSignalRaise((OsxSmpSignal *)machine->threadcpus[index]->wake);
		OsxGuestHostThreadJoin(machine->slots[index].thread);
		machine->slots[index].thread = NULL;
	}
}

void OsxGuestSmpDestroy(OsxMachine *machine)
{
	OsxCpu *cpu;
	int index;
	if (machine->cpus[0] == NULL)
		return;
	OsxGuestSmpJoin(machine);
	machine->cpu = machine->cpus[0];
	if (machine->cpucount <= 1 || machine->lock == NULL)
		return;
	for (index = 0; index < machine->cpucount; index++) {
		cpu = machine->cpus[index];
		if (cpu == NULL)
			continue;
		if (cpu->wake)
			OsxGuestHostSignalDestroy((OsxSmpSignal *)cpu->wake);
#if OSXG5
		if (index > 0 && cpu->core970) {
			OsxGuest970JitDestroy((Osx970 *)cpu->core970);
			free(cpu->core970);
		}
#endif
		if (index > 0) {
			OsxGuestJitDestroy(cpu);
			free(cpu);
		}
	}
	OsxGuestHostMutexDestroy(machine->lock);
}

#endif
