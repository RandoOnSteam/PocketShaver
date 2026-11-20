/*
 *  dsp_event_record.h - classic Mac EventRecord kinds and DSp reason codes.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pure C. Used by DSpProcessEventHandler (sub-opcode 750) to decode guest
 *  EventRecords and by the background/foreground lifecycle path.
 */

#ifndef DSP_EVENT_RECORD_H
#define DSP_EVENT_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Event kinds - DSp 1.7 PDF + Inside Macintosh: Toolbox Essentials.
 *  The classic Mac EventRecord.what enumeration; DSp uses a subset
 *  (mouseDown/Up + keyDown/Up + autoKey + osEvt). Listed in full to
 *  document the ABI; the sub-op-750 decode path examines osEvt.
 */
enum {
	kDSpEvent_NullEvent      = 0,
	kDSpEvent_MouseDown      = 1,
	kDSpEvent_MouseUp        = 2,
	kDSpEvent_KeyDown        = 3,
	kDSpEvent_KeyUp          = 4,
	kDSpEvent_AutoKey        = 5,
	kDSpEvent_UpdateEvt      = 6,
	kDSpEvent_DiskEvt        = 7,
	kDSpEvent_ActivateEvt    = 8,
	kDSpEvent_OSEvt          = 15
};

/*
 *  osEvt subtype constants - encoded into the high byte of the
 *  EventRecord.message field per Inside Macintosh: Toolbox Essentials.
 *  Used when decoding guest-supplied suspend/resume osEvt records.
 */
enum {
	kDSpOSEvt_SuspendResumeMessage = 0x01,  /* osEvt subtype 1 */
	kDSpOSEvt_MouseMovedMessage    = 0xFA   /* osEvt subtype 250 */
};

/* Suspend/resume osEvt message bit 0: 1 = resume, 0 = suspend. */
enum {
	kDSpOSEvtMsg_ResumeFlag        = 0x01u
};

/*
 *  Context-loss reason codes - positive values, NOT error codes.
 *  Used as the EventRecord.message field of an osEvt enqueued ahead of
 *  the suspend osEvt when a context-loss event fires.
 *
 *  Default: 1 (per DSp 1.7 PDF p.~92 placeholder); confirmable via
 *  DrawSprocketLib decompile.
 *
 *  Guard avoids double-definition when dsp_engine.h's mirror enum has
 *  already been seen in the same translation unit. Either header may
 *  be included first; whichever wins defines the enum and sets
 *  DSP_CONTEXT_REASON_LOST_DEFINED.
 */
#ifndef DSP_CONTEXT_REASON_LOST_DEFINED
#define DSP_CONTEXT_REASON_LOST_DEFINED 1
enum {
	kDSpContextReason_Lost         = 1
};
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* DSP_EVENT_RECORD_H */
