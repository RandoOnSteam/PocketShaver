/*
 *  dsp_alt_buffer.h - DrawSprocket AltBuffer subsystem (sub-ops 700-705)
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Interface used by the present / underlay-restore path in
 *  dsp_draw_context.mm. The record table is private to dsp_alt_buffer.mm.
 */

#ifndef DSP_ALT_BUFFER_H
#define DSP_ALT_BUFFER_H

#include <cstdint>

/* Two per context (underlay + staging) across the 8-context table. */
#define DSP_MAX_ALT_BUFFERS 16

#ifdef __OBJC__
#import <Metal/Metal.h>
typedef id<MTLBuffer> DSpAltBacking;
typedef id<MTLTexture> DSpAltTexture;
#else
typedef void* DSpAltBacking;
typedef void* DSpAltTexture;
#endif /* __OBJC__ */

struct DSpAltBufferRecord {
	bool                  in_use;
	DSpAltBacking         backing;             /* DSp-heap MTLBuffer (BGRA8Unorm-viewable) */
	DSpAltTexture         texture;             /* BGRA8Unorm view for compositor routing */
	uint32_t              cgrafptr_mac_addr;   /* cached guest CGrafPort (0 until first GetCGrafPtr) */
	uint32_t              baseaddr_mac;        /* guest-RAM staging baseAddr backing the CGrafPort */
	uint32_t              baseaddr_size;       /* byte size of the pixel-staging block */
	bool                  baseaddr_owned_staging; /* baseaddr_mac is a DSpReserveGuestPixelStaging block to quarantine on teardown */
	uint32_t              width;
	uint32_t              height;
	uint32_t              depth;               /* bits/pixel (8/16/32) - the OWNING context's back-buffer depth at New time; drawable surface + backing both use it */
	uint32_t              seed_pixmap_mac;      /* owning screen PixMap metadata; required by indexed alt CGrafPorts (pmTable) */
	uint32_t              options;             /* DSpAltBufferOption bits */
	bool                  underlay_capable;    /* NULL inAttributes => true (PDF p.49) */
	/* Dirty-rect union - same fields/semantics as DSpContextPrivate's
	 * dirty_*; single-writer, no primitive. */
	int16_t               dirty_left, dirty_top, dirty_right, dirty_bottom;
	bool                  dirty_empty;
};


/* Resolve a 1-based alt-buffer handle to its record (NULL if invalid or not
 * in use). Mirrors DSpGetContext. */
DSpAltBufferRecord *DSpGetAltBuffer(uint32_t handle);

/* Copy the guest-RAM staging (drawn through the GetCGrafPtr port) into the
 * Metal backing for the underlay-restore copy. No-op on StorageModePrivate
 * (simulator) or without owned staging. */
void DSpSyncAltBufferStagingToBacking(DSpAltBufferRecord *rec);

/* Implementation specific backing (texture) creation */
bool DSpAllocAltBufferBacking(DSpAltBufferRecord *rec, uint32_t w, uint32_t h);

/* Free every in-use record on DrawSprocket shutdown. */
void DSpResetAltBufferTable(void);

/* Background drops each record's backing and texture (staging and dims
 * survive) so the DSp bump heap can reset; foreground re-allocates and
 * refills from guest staging. Called only from the DSp background/foreground
 * handlers. */
void DSpReleaseAltBufferBackingsForBackground(void);
void DSpRestoreAltBufferBackingsForForeground(void);
bool DSpAltBufferRestorePending(void);

/* Bytes per pixel / Metal format for an alt-buffer depth. Same depth->format
 * mapping as the back buffer (dsp_metal_renderer.mm DSpPixelFormatForDepthBits):
 * 8 -> R8Uint (indexed, CLUT unpack), 16 -> R16Uint (xRGB1555), 32 ->
 * BGRA8Unorm. Anything else is normalized to 32 at New time. */
uint32_t DSpAltBytesPerPixel(uint32_t depth);

void* DSpAltBufferRecordGetBackingContents(DSpAltBufferRecord* prec);

#endif /* DSP_ALT_BUFFER_H */
