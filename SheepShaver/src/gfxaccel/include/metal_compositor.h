/*
 *  metal_compositor.h - shared compositor interface
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Metal compositor that replaces SDL rendering on iOS. Plain C/C++ header:
 *  no ObjC types.
 */

#ifndef METAL_COMPOSITOR_H
#define METAL_COMPOSITOR_H

#include <stdint.h>

#include "display_mode_controller.h"
#include "gfx_frame_pacing_policy.h"

/*
 * Engines publish through MetalCompositorSubmitFrame, which validates the
 * descriptor, rejects stale DMC generations, and caches the last overlay
 * layer. MetalCompositorPresent is the only path that acquires a drawable,
 * encodes and presents. Underlay and framebuffer slots are accepted but
 * ignored. The compositor sees only layers with opaque texture handles, never
 * engine identities. `source` is void* so .cpp files can hold an id<MTLTexture>.
 */

/* Strict z-order slots. */
typedef enum {
	kLayerSlotUnderlay    = 0,
	kLayerSlotFramebuffer = 1,
	kLayerSlotOverlay     = 2,
	kLayerSlotCount       = 3
} DMCLayerSlot;

enum {
	kGfxAccelMaxLayers = 16
};

/*
 * CompositeBlendMode - per-layer blend mode.
 * Covers every composite mode classic 2D and 3D engines can emit today;
 * additional modes stay deferred until an app-level need surfaces.
 */
typedef enum {
	kBlendOpaque        = 0,
	kBlendPremultiplied = 1,
	kBlendStraight      = 2
} CompositeBlendMode;

/*
 * `source` is a BGRA8Unorm id<MTLTexture> cast to void*. Destination fields
 * are floats to allow fractional placement when the drawable is not an
 * integer multiple of the framebuffer.
 */
struct CompositeLayer {
	void              *source;          /* native 32-bit-color texture handle */
	uint32_t           src_origin_x;
	uint32_t           src_origin_y;
	uint32_t           src_size_w;
	uint32_t           src_size_h;
	float              dst_origin_x;
	float              dst_origin_y;
	float              dst_size_w;
	float              dst_size_h;
	DMCLayerSlot       slot;
	CompositeBlendMode blend;
	float              alpha;           /* 0.0..1.0; consulted only for non-Opaque blend modes */
};

/*
 * `layers` is borrowed for the SubmitFrame call only. `generation` must match
 * dmc_current_snapshot()->generation at entry, else SubmitFrame returns
 * kGfxAccelErrStaleGeneration and the caller rebuilds; this keeps rollback
 * from leaving a descriptor pointing at freed resources.
 * `vbl_tick_target_usec` is kept for ABI shape only and is unused.
 */
struct FrameDescriptor {
	const struct CompositeLayer *layers;
	uint32_t                     layer_count;           /* 0..kGfxAccelMaxLayers */
	uint32_t                     generation;            /* DMC snapshot generation */
	uint64_t                     vbl_tick_target_usec;  /* advisory */
};

/* Error codes start at -4000 to stay clear of DMC's -3000..-3099. */
enum GfxAccelError {
	kGfxAccelNoErr                       =     0,
	kGfxAccelErrInvalidDescriptor        = -4001,   /* malformed descriptor or layer */
	kGfxAccelErrStaleGeneration          = -4002,   /* desc->generation != current snapshot gen */
	kGfxAccelErrInvalidSlot              = -4003,   /* slot enum out of range */
	kGfxAccelErrDrawableUnavailable      = -4004,   /* nextDrawable returned nil */
	kGfxAccelErrPipelineUnavailable      = -4005,   /* PSO build failed */
	kGfxAccelErrEngineNotRegistered      = -4006,   /* vend called for unregistered engine */
	kGfxAccelErrTextureFormatUnsupported = -4007    /* non-BGRA8Unorm source texture */
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `depth` arguments are VIDEO_DEPTH_* (video_blit.h): 1/2/4/8-bit indexed,
 * 16-bit xRGB1555 big-endian, or 32-bit BGRA8.
 */

/*
 * Creates the CompositorMetalView (UIView + CAMetalLayer) over the window,
 * wraps the framebuffer as a zero-copy shared MTLBuffer with a texture view,
 * and builds the pipeline for `depth`. row_bytes is the data stride; pitch is
 * the page-aligned allocation stride (>= row_bytes); buffer and buffer_size
 * (>= height * pitch) are page-aligned. Returns 0 or -1.
 */
int MetalCompositorInit(int width, int height, int depth, int row_bytes,
                        int pitch, void *buffer, uint32_t buffer_size);

/*
 * Expands 3-byte RGB entries (up to 256) to RGBA into the shared palette
 * buffer. Warns and does nothing outside indexed modes.
 */
void MetalCompositorUpdatePalette(const uint8_t *pal, int num_colors);

/*
 * Presents one frame: the framebuffer plus any cached overlay. Safe to call
 * before init.
 */
void MetalCompositorPresent(void);

/*
 * Desktop OpenGL only: present the cached frame without advancing the
 * VBL callback chain. Guest 3D entry points use this form so a synchronous
 * frame presentation cannot re-enter PPC code before the native thunk returns.
 */
#if defined(GFXACCEL_USE_OPENGL)
void MetalCompositorPresentWithoutVBL(void);
void MetalCompositorPresentAdopted(void);
int MetalCompositorAdoptFrame(unsigned int readframebuffer, int width, int height);
int MetalCompositorReadFrame(uint8_t *pixels, int rowbytes);
int MetalCompositorSetGamma(const uint8_t *table);
#endif

/*
 * Tear down all Metal compositor resources.
 *
 * Removes the CompositorMetalView from the UIWindow and releases
 * all Metal objects. Safe to call when not initialized.
 */
void MetalCompositorShutdown(void);

/*
 * Rebuilds only the depth-dependent resources (buffer, texture, pipeline,
 * palette, sampler) and keeps the view, layer, device and queue, avoiding a
 * flash. Engines re-vend their overlay textures in on_mode_enter/exit.
 *
 * Called on the emul thread with interrupts disabled, so no Present can run.
 * No UIView work may happen here, which is why the view is kept. Requires
 * prior Init; returns -1 otherwise. Same arguments and return as Init.
 */
int MetalCompositorResize(int width, int height, int depth, int row_bytes,
                          int pitch, void *buffer, uint32_t buffer_size);

/* 1 between a successful Init and Shutdown. Picks Init vs Resize. */
int MetalCompositorIsInitialized(void);

/*
 * MetalCompositorCurrentMode.
 *
 * Report the width/height/depth the compositor is currently configured for
 * (what is on screen right now). Any of the out-pointers may be NULL. Returns 1
 * if the compositor is initialized (values written), 0 otherwise (untouched).
 *
 * Used by video_sdl2.cpp to distinguish a depth-only switch (same resolution ->
 * lightweight MetalCompositorResize) from a resolution switch (full reopen).
 */
int MetalCompositorCurrentMode(int *out_width, int *out_height, int *out_depth);
/*

 * Validates the descriptor (null, layer_count, slot, non-null source), rejects
 * a stale generation with kGfxAccelErrStaleGeneration (engine drops the frame
 * and rebuilds), and caches the last overlay layer with its texture retained.
 * Acquires no drawable and encodes nothing; Present composites the cached
 * overlay over the framebuffer each VBL.
 *
 * Because a handle is cached rather than a snapshot, the engine must not
 * modify a submitted overlay texture until its next SubmitFrame.
 */
int32_t MetalCompositorSubmitFrame(const struct FrameDescriptor *desc);

/*
 * Blocks until the engine's next 3D frame deadline, derived from display-link
 * cadence (one interval if ticks stop). Returns kGfxAccelNoErr,
 * kGfxAccelErrVBLTimeout if mach_wait_until fails, or
 * kGfxAccelErrVBLNotInitialized if VBLSource is not running.
 */
int32_t MetalCompositorSync3DFramePacingForEngine(int32_t engine_id);

/* Called from the VBL callback. The stored value is currently unused. */
void MetalCompositorSubmitFrame_SetTargetTimestamp(double target_ts);

/*
 * Overlay cache. Present keeps composing the last overlay between submits,
 * e.g. across Nanosaur's QADrawContextDelete -> New gap, so full-3D apps don't
 * flicker to black.
 *
 * AcquireCachedOverlay copies the layer and returns a retained texture; the
 * caller must call ReleaseCachedOverlay after the frame commits. Returns 1 if
 * an overlay is cached. EncodeCachedOverlay draws a layer into an existing
 * render encoder with the display-gamma premultiplied PSO. ClearCachedOverlay
 * runs on mode exit and shutdown.
 */
int  MetalCompositorSubmitFrame_AcquireCachedOverlay(struct CompositeLayer *out_layer,
                                                     void **out_tex_retained);
void MetalCompositorSubmitFrame_ReleaseCachedOverlay(void *tex_retained);
void MetalCompositorSubmitFrame_EncodeCachedOverlay(void *render_encoder,
                                                    const struct CompositeLayer *layer,
                                                    void *display_gamma_lut);
void MetalCompositorSubmitFrame_ClearCachedOverlay(void);
/* Clears the cache only if it still holds `source`, so a newer producer's
 * overlay survives. NULL is a no-op. */
void MetalCompositorSubmitFrame_ClearCachedOverlayForSource(void *source);

/* Legacy cache reset retained for the Metal implementation. Desktop GL has no
 * framebuffer-layer cache and implements this as a no-op. */
void MetalCompositorSubmitFrame_ClearCachedFramebuffer(void);

/* GL only: flush a Present deferred while a RAVE/Glide FBO pass owned the
 * shared GL context. Metal implements this as a no-op. */
void MetalCompositorFlushDeferredPresent(void);

#if defined(GFXACCEL_USE_OPENGL)
/* Description of the currently bound guest-visible screen. The guest address,
 * host pointer, geometry, and compositor texture always describe one logical
 * surface. */
struct GfxAccelScreenSurface {
	uint32_t mac_base;
	void    *host_base;
	uint32_t byte_size;
	int      width;
	int      height;
	int      depth;
	int      row_bytes;
};

/* Query the canonical guest-visible screen selected by DSp/video mode state. */
int  MetalCompositorGetScreenSurface(struct GfxAccelScreenSurface *out_surface);
#endif


/*
 * The CAMetalLayer as void*, or NULL before init. Background/foreground
 * handlers zero and restore its drawableSize.
 */
void *MetalCompositorGetLayer(void);

/*
 * MetalCompositorPaletteLatch.
 *
 * VBL-latched dirty swap: atomically promotes the back palette buffer to
 * front only when MetalCompositorUpdatePalette has published new data. Must
 * be called from the VBL callback BEFORE any command encoding for the frame
 * begins. The compositor reads s_palette_front_idx once at encode start and
 * binds that buffer.
 */
void MetalCompositorPaletteLatch(void);

/*
 * Uploads a 768-byte planar Mac gamma LUT (256 R, G, B), composed with the
 * display policy. No-op if the buffer or lut is NULL.
 */
void MetalCompositorUpdateGammaLUT(const uint8_t *lut);

/*
 * Present-rect cache for the absolute-cursor letterbox map.
 *
 * RefreshPresentRect caches the view rect in window coordinates (the space
 * SDL mouse events use); it is a no-op off the main thread. GetPresentRect
 * returns the cached rect from any thread; size is 0 before the first
 * refresh, so fall back to an identity map.
 */
void MetalCompositorRefreshPresentRect(void);
void MetalCompositorGetPresentRect(int *out_x, int *out_y, int *out_w, int *out_h);


#ifdef __cplusplus
}
#endif

#endif /* METAL_COMPOSITOR_H */
