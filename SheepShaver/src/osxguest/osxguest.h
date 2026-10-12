#ifndef OSXGUEST_H
#define OSXGUEST_H

typedef struct OsxReferenceState {
	uint32 gpr[32];
	uint64 fpr[32];
	uint32 cr;
	uint32 xer;
	uint32 lr;
	uint32 ctr;
	uint32 fpscr;
	uint32 pc;
} OsxReferenceState;

void OsxReferenceExecute(uint32 entry, OsxReferenceState *state);
void OsxLockstepTest(uint8 *ramhost, uint32 ramsize, int iterations, const char *logpath);
int OsxGuestRequested(void);
int OsxGuestMain(void);

#endif
