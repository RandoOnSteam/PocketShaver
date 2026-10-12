#include "sysdeps.h"
#include "osxmachine.h"

void OsxPointerHook(OsxCpu *cpu, void *context, uint32 address)
{
	OsxPointer *pointer;
	OsxCuda *cuda;
	OsxMouseEvent *event;
	uint32 stack;
	uint32 location;
	uint32 timehigh;
	uint32 timelow;
	int pressed;
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
	pressed = cpu->gpr[4] != 0;
	while (cuda->pointerread != cuda->pointerwrite) {
		event = &cuda->pointerqueue[cuda->pointerread];
		if (!event->motion && event->button != pressed)
			break;
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
	if (!OsxCpuWriteVirtual(cpu, stack, cpu->gpr[1]) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x38, timelow) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x40, location) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x44, pointer->machine->width) ||
		!OsxCpuWriteVirtual(cpu, stack + 0x48, pointer->machine->height))
		return;
	memcpy(pointer->registers, cpu->gpr, sizeof(pointer->registers));
	pointer->lr = cpu->lr;
	pointer->ctr = cpu->ctr;
	pointer->cr = cpu->cr;
	pointer->xer = cpu->xer;
	pointer->callstack = stack;
	cpu->gpr[1] = stack;
	cpu->gpr[4] = 0;
	if (pressed)
		cpu->gpr[4] = 4;
	cpu->gpr[5] = stack + 0x40;
	cpu->gpr[6] = stack + 0x44;
	cpu->gpr[7] = 1;
	cpu->gpr[8] = 0;
	cpu->gpr[9] = 0;
	cpu->gpr[10] = timehigh;
	cpu->lr = cpu->pc;
	cpu->nextpc = pointer->absoluteaddress;
	(void)address;
}
