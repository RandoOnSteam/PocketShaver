/*
 *  osxpointer.cpp - OSX-specific mouse routines
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
#include "osxmachine.h"

static void CallGuest(OsxCpu *cpu, OsxPointer *pointer, uint32 stack, uint32 target)
{
	memcpy(pointer->registers, cpu->gpr, sizeof(pointer->registers));
	pointer->lr = cpu->lr;
	pointer->ctr = cpu->ctr;
	pointer->cr = cpu->cr;
	pointer->xer = cpu->xer;
	pointer->callstack = stack;
	cpu->gpr[1] = stack;
	cpu->lr = cpu->pc;
	cpu->nextpc = target;
}

static void ScrollWheel(OsxCpu *cpu, OsxPointer *pointer, int delta)
{
	uint32 stack;
	uint32 timehigh;
	uint32 timelow;
	uint32 sender;
	uint32 offset;
	stack = (cpu->gpr[1] - 0x60) & ~15u;
	timehigh = cpu->gpr[7];
	timelow = cpu->gpr[8];
	sender = cpu->gpr[9];
	if (pointer->wheeladdress == 0 || !OsxCpuWriteVirtual(cpu, stack, cpu->gpr[1]))
		return;
	if (pointer->sender && pointer->compactwheel) {
		if (!OsxCpuWriteVirtual(cpu, stack + 0x38, timelow) ||
			!OsxCpuWriteVirtual(cpu, stack + 0x3c, sender))
			return;
	} else if (pointer->sender) {
		for (offset = 0x38; offset < 0x44; offset += 4)
			if (!OsxCpuWriteVirtual(cpu, stack + offset, 0))
				return;
		if (!OsxCpuWriteVirtual(cpu, stack + 0x44, timehigh) ||
			!OsxCpuWriteVirtual(cpu, stack + 0x48, timelow) ||
			!OsxCpuWriteVirtual(cpu, stack + 0x4c, sender))
			return;
	}
	CallGuest(cpu, pointer, stack, pointer->wheeladdress);
	cpu->gpr[4] = (uint32)(int32)(int16)delta;
	cpu->gpr[5] = 0;
	cpu->gpr[6] = 0;
	cpu->gpr[7] = timehigh;
	cpu->gpr[8] = timelow;
	if (pointer->sender) {
		cpu->gpr[7] = (uint32)((int32)(int16)delta << 16);
		cpu->gpr[8] = 0;
		cpu->gpr[9] = 0;
		cpu->gpr[10] = (uint32)(int32)(int16)delta;
		if (pointer->compactwheel)
			cpu->gpr[10] = timehigh;
	}
}

void OsxPointerHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxPointer *pointer;
	OsxCuda *cuda;
	OsxMouseEvent *event;
	uint32 stack;
	uint32 location;
	uint32 timehigh;
	uint32 timelow;
	uint32 sender;
	uint32 buttons;
	int pressed;
	int delta;
	pointer = (OsxPointer *)context;
	if (pointer->callstack == cpu->gpr[1] && pointer->callstack != 0) {
		memcpy(cpu->gpr, pointer->registers, sizeof(pointer->registers));
		cpu->lr = pointer->lr;
		cpu->ctr = pointer->ctr;
		cpu->cr = pointer->cr;
		cpu->xer = pointer->xer;
		cpu->nextpc = pointer->lr;
		pointer->callstack = 0;
		return;
	}
	if (pointer->callstack != 0)
		return;
	cuda = &pointer->machine->cuda;
	if (cuda->pointerread == cuda->pointerwrite) {
		cpu->nextpc = cpu->lr;
		return;
	}
	delta = 0;
	while (cuda->pointerread != cuda->pointerwrite && cuda->pointerqueue[cuda->pointerread].wheel &&
		(delta == 0 || (cuda->pointerqueue[cuda->pointerread].wheel > 0) == (delta > 0))) {
		delta += cuda->pointerqueue[cuda->pointerread].wheel;
		cuda->pointerread = (cuda->pointerread + 1) % OSXMOUSEQUEUE;
	}
	if (delta) {
		ScrollWheel(cpu, pointer, delta);
		return;
	}
	pressed = cpu->gpr[4] != 0;
	while (cuda->pointerread != cuda->pointerwrite) {
		event = &cuda->pointerqueue[cuda->pointerread];
		if (event->wheel || (!event->motion && (event->button & 1) != pressed))
			break;
		pointer->pointerbuttons = event->button;
		pointer->pointerx = event->x;
		pointer->pointery = event->y;
		pointer->pointervalid = 1;
		cuda->pointerread = (cuda->pointerread + 1) % OSXMOUSEQUEUE;
		if (!event->motion)
			break;
	}
	if (!pointer->pointervalid)
		return;
	stack = (cpu->gpr[1] - 0x60) & ~15u;
	location = ((uint32)pointer->pointerx << 16) | ((uint32)pointer->pointery & 0xffff);
	timehigh = cpu->gpr[7];
	timelow = cpu->gpr[8];
	sender = cpu->gpr[9];
	if (!OsxCpuWriteVirtual(cpu, stack, cpu->gpr[1]) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x38, timelow) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x3c, sender) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x40, location) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x44, pointer->machine->width) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x48, pointer->machine->height))
		return;
	buttons = 0;
	if (pressed)
		buttons |= 4;
	if (pointer->pointerbuttons & 2)
		buttons |= 1;
	if (pointer->pointerbuttons & 4)
		buttons |= 2;
	CallGuest(cpu, pointer, stack, pointer->absoluteaddress);
	cpu->gpr[4] = buttons;
	cpu->gpr[5] = stack + 0x40;
	cpu->gpr[6] = stack + 0x44;
	cpu->gpr[7] = 1;
	cpu->gpr[8] = 0;
	cpu->gpr[9] = 0;
	cpu->gpr[10] = timehigh;
	(void)address;
}
