#ifndef PRINTING_H
#define PRINTING_H

struct M68kRegisters;

#define PRINTSTYLEITEMOFFSET 0x40
#define PRINTJOBITEMOFFSET 0x46
#define PRINTFILTEROFFSET 0x4C
#define PRINTTHUNKSIZE 0x100

extern uint32 PrintThunkAddress(void);
extern void PrintWriteThunks(uint8 *host);
extern void PrintInstall(void);
extern void PrintGlue(M68kRegisters *r);

#endif
