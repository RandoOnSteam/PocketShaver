/*
 *  metal_compositor_submitframe.mm - SubmitFrame implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  SubmitFrame is a validate-and-cache shim. It sees only CompositeLayer
 *  PODs with opaque texture handles, rejects malformed or stale
 *  descriptors, and keeps the last kLayerSlotOverlay layer in a one-entry
 *  cache. Underlay/framebuffer layers are accepted and ignored.
 *
 *  MetalCompositorPresent is the only drawable owner. It composites the
 *  cached overlay over the 2D framebuffer every VBL, which covers gaps when
 *  an engine stops submitting (e.g. a full-3D app recreating its
 *  QADrawContext between scenes would otherwise flash black). A second
 *  presenter here would race Present for drawables and hide the 2D layer.
 *
 *  The cache holds a strong reference to the source texture, not a pixel
 *  snapshot: the engine must not redraw that texture until it submits a
 *  different one. SubmitFrame does no GPU work and never waits on
 *  s_inflight_semaphore.
 */

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdint.h>
#include "atomic.h"
#include <os/lock.h>

#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources_heap.h"
#include "vbl_source.h"

// Own log macros so this file builds without metal_compositor.mm's includes.

#define COMPSF_LOG(fmt, ...) \
	do { printf("[MetalCompositor/SubmitFrame] " fmt "\n", ##__VA_ARGS__); } while (0)

#define COMPSF_ERR(fmt, ...) \
	do { printf("[MetalCompositor/SubmitFrame ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)

static id<MTLDevice>                  s_device             = nil;
static id<MTLCommandQueue>            s_queue              = nil;
static id<MTLLibrary>                 s_library            = nil;
static CAMetalLayer                  *s_layer              = nil;    // may be nil
static dispatch_semaphore_t           s_inflight_semaphore = NULL;
static id<MTLRenderPipelineState>     s_pipe_for_blend[3]  = { nil, nil, nil };
static id<MTLRenderPipelineState>     s_pipe_display_premultiplied = nil;

/*
 * Target presentation timestamp set by the VBL callback via
 * MetalCompositorSubmitFrame_SetTargetTimestamp().
 *
 * atomic.h double -- written from VBL callback (main thread), read
 * from emul thread.  Same minimal-primitive rationale as vbl_source.mm's
 * atomic uint64 counters.
 *
 * Iteration #3 (rave-overlay-flicker-black): in production SubmitFrame
 * no longer calls presentAtTime: (Present owns presentation), so this
 * field is effectively unused.  Retained for the
 * MetalCompositorSubmitFrame_SetTargetTimestamp API shape which external
 * callers (vbl_source) still invoke.
 */
static atomic_double s_target_presentation_ts = 0;

// Overlay cache: the last kLayerSlotOverlay layer, composited by Present while
// the engine is between frames. Retains the source texture until replaced or
// cleared. Written by SubmitFrame and the clear helpers, read by Present.

static os_unfair_lock           s_overlay_cache_lock  = OS_UNFAIR_LOCK_INIT;
static bool                     s_overlay_cache_valid = false;
static struct CompositeLayer    s_overlay_cache_layer;
static id<MTLTexture>           s_overlay_cache_tex   = nil;


// One PSO per blend mode, plus the display-gamma premultiplied variant:
//   kBlendOpaque        - no blending
//   kBlendPremultiplied - src=One,      dst=OneMinusSrcAlpha
//   kBlendStraight      - src=SrcAlpha, dst=OneMinusSrcAlpha

static id<MTLLibrary> submitframe_resolve_library(id<MTLDevice> dev);

static int submitframe_build_pipelines(void)
{
	if (!s_device) {
		COMPSF_ERR("submitframe_build_pipelines: device nil");
		return -1;
	}
	if (!s_library) {
		s_library = submitframe_resolve_library(s_device);
	}
	id<MTLLibrary> lib = s_library;
	if (!lib) {
		COMPSF_ERR("submitframe_build_pipelines: could not resolve Metal library");
		return -1;
	}

	id<MTLFunction> vfunc = [lib newFunctionWithName:@"submitframe_vertex"];
	id<MTLFunction> ffunc = [lib newFunctionWithName:@"submitframe_fragment"];
	id<MTLFunction> display_ffunc =
		[lib newFunctionWithName:@"submitframe_fragment_display_premultiplied"];
	if (!vfunc || !ffunc || !display_ffunc) {
		COMPSF_ERR("submitframe_build_pipelines: shader function lookup "
				   "(vertex=%p fragment=%p display=%p)",
				   vfunc, ffunc, display_ffunc);
		return -1;
	}

	const CompositeBlendMode modes[3] = { kBlendOpaque, kBlendPremultiplied, kBlendStraight };
	for (int i = 0; i < 3; ++i) {
		CompositeBlendMode mode = modes[i];
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
		pd.vertexFunction   = vfunc;
		pd.fragmentFunction = ffunc;
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		switch (mode) {
			case kBlendOpaque:
				pd.colorAttachments[0].blendingEnabled = NO;
				break;
			case kBlendPremultiplied:
				pd.colorAttachments[0].blendingEnabled             = YES;
				pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
				pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
				pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
				pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
				pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
				break;
			case kBlendStraight:
				pd.colorAttachments[0].blendingEnabled             = YES;
				pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
				pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
				pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
				pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
				pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
				break;
		}

		NSError *err = nil;
		id<MTLRenderPipelineState> pso =
			[s_device newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!pso) {
			COMPSF_ERR("submitframe_build_pipelines: blend=%d PSO failed: %s",
					   (int)mode, [[err localizedDescription] UTF8String]);
			return -1;
		}
		s_pipe_for_blend[(int)mode] = pso;
	}

	{
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
		pd.vertexFunction   = vfunc;
		pd.fragmentFunction = display_ffunc;
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		pd.colorAttachments[0].blendingEnabled             = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
		pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
		pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
		pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
		pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;

		NSError *err = nil;
		s_pipe_display_premultiplied =
			[s_device newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!s_pipe_display_premultiplied) {
			COMPSF_ERR("submitframe_build_pipelines: display premult PSO failed: %s",
					   [[err localizedDescription] UTF8String]);
			return -1;
		}
	}

	COMPSF_LOG("submitframe_build_pipelines: built blend-mode + display PSOs");
	return 0;
}

static id<MTLRenderPipelineState> submitframe_pipeline_for_blend(CompositeBlendMode b)
{
	if ((int)b < 0 || (int)b >= 3) return nil;
	return s_pipe_for_blend[(int)b];
}

// The viewport is set to the layer's dst rect so the fullscreen triangle
// leaves the 2D underlay visible outside it (e.g. a 640x469 overlay must not
// cover an 11-pixel menu strip).

static void submitframe_encode_layer(id<MTLRenderCommandEncoder> enc,
									 const struct CompositeLayer *layer)
{
	if (!enc || !layer) return;

	id<MTLRenderPipelineState> pso = submitframe_pipeline_for_blend(layer->blend);
	if (!pso) {
		COMPSF_ERR("submitframe_encode_layer: no PSO for blend=%d - skipping layer",
				   (int)layer->blend);
		return;
	}
	[enc setRenderPipelineState:pso];

	/* Per-rect viewport clipping (Iteration #3).  dst_origin / dst_size are
	 * in pixel coords (same space Metal viewports expect).  MTLViewport
	 * takes doubles: origin_x, origin_y, width, height, znear, zfar.  Zero-
	 * sized rects are silently skipped by Metal; guard explicitly to avoid
	 * a validation warning under Metal debug. */
	if (layer->dst_size_w > 0.0f && layer->dst_size_h > 0.0f) {
		MTLViewport vp;
		vp.originX = (double)layer->dst_origin_x;
		vp.originY = (double)layer->dst_origin_y;
		vp.width   = (double)layer->dst_size_w;
		vp.height  = (double)layer->dst_size_h;
		vp.znear   = 0.0;
		vp.zfar    = 1.0;
		[enc setViewport:vp];
	}

	id<MTLTexture> src = (__bridge id<MTLTexture>)layer->source;
	[enc setFragmentTexture:src atIndex:0];

	typedef struct {
		float src_origin[2];
		float src_size[2];
		float dst_origin[2];
		float dst_size[2];
		float alpha;
		float _pad[3];
	} LayerUniform;

	LayerUniform u;
	u.src_origin[0] = (float)layer->src_origin_x;
	u.src_origin[1] = (float)layer->src_origin_y;
	u.src_size[0]   = (float)layer->src_size_w;
	u.src_size[1]   = (float)layer->src_size_h;
	u.dst_origin[0] = layer->dst_origin_x;
	u.dst_origin[1] = layer->dst_origin_y;
	u.dst_size[0]   = layer->dst_size_w;
	u.dst_size[1]   = layer->dst_size_h;
	u.alpha         = layer->alpha;
	u._pad[0] = u._pad[1] = u._pad[2] = 0.0f;

	[enc setVertexBytes:&u length:sizeof(u) atIndex:0];
	[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];

	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

static void submitframe_encode_layer_display_gamma(id<MTLRenderCommandEncoder> enc,
												   const struct CompositeLayer *layer,
												   id<MTLBuffer> display_gamma_lut)
{
	if (!enc || !layer) return;
	if (layer->blend != kBlendPremultiplied ||
		display_gamma_lut == nil ||
		s_pipe_display_premultiplied == nil) {
		submitframe_encode_layer(enc, layer);
		return;
	}

	[enc setRenderPipelineState:s_pipe_display_premultiplied];

	if (layer->dst_size_w > 0.0f && layer->dst_size_h > 0.0f) {
		MTLViewport vp;
		vp.originX = (double)layer->dst_origin_x;
		vp.originY = (double)layer->dst_origin_y;
		vp.width   = (double)layer->dst_size_w;
		vp.height  = (double)layer->dst_size_h;
		vp.znear   = 0.0;
		vp.zfar    = 1.0;
		[enc setViewport:vp];
	}

	id<MTLTexture> src = (__bridge id<MTLTexture>)layer->source;
	[enc setFragmentTexture:src atIndex:0];

	typedef struct {
		float src_origin[2];
		float src_size[2];
		float dst_origin[2];
		float dst_size[2];
		float alpha;
		float _pad[3];
	} LayerUniform;

	LayerUniform u;
	u.src_origin[0] = (float)layer->src_origin_x;
	u.src_origin[1] = (float)layer->src_origin_y;
	u.src_size[0]   = (float)layer->src_size_w;
	u.src_size[1]   = (float)layer->src_size_h;
	u.dst_origin[0] = layer->dst_origin_x;
	u.dst_origin[1] = layer->dst_origin_y;
	u.dst_size[0]   = layer->dst_size_w;
	u.dst_size[1]   = layer->dst_size_h;
	u.alpha         = layer->alpha;
	u._pad[0] = u._pad[1] = u._pad[2] = 0.0f;

	[enc setVertexBytes:&u length:sizeof(u) atIndex:0];
	[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
	[enc setFragmentBuffer:display_gamma_lut offset:0 atIndex:1];

	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// Overlay cache. Cleared on mode exit (the texture is sized for the old mode)
// and on Unbind (the texture belongs to the torn-down device).

static void submitframe_cache_store(const struct CompositeLayer *layer)
{
	if (!layer) return;
	id<MTLTexture> new_tex = (__bridge id<MTLTexture>)layer->source;
	os_unfair_lock_lock(&s_overlay_cache_lock);
	s_overlay_cache_layer = *layer;
	s_overlay_cache_tex   = new_tex;       /* strong retain under ARC */
	s_overlay_cache_valid = (new_tex != nil);
	os_unfair_lock_unlock(&s_overlay_cache_lock);
}

extern "C" int MetalCompositorSubmitFrame_AcquireCachedOverlay(
	struct CompositeLayer *out_layer, void **out_tex_retained)
{
	if (!out_layer || !out_tex_retained) return 0;
	*out_tex_retained = NULL;
	id<MTLTexture> tex = nil;
	int have = 0;
	os_unfair_lock_lock(&s_overlay_cache_lock);
	if (s_overlay_cache_valid && s_overlay_cache_tex != nil) {
		*out_layer = s_overlay_cache_layer;
		tex        = s_overlay_cache_tex;  /* retained by __bridge_retained below */
		have       = 1;
	}
	os_unfair_lock_unlock(&s_overlay_cache_lock);
	if (have && tex != nil) {
		*out_tex_retained = (__bridge_retained void *)tex;
		/* Rebind the source field to the retained handle: the caller's
		 * layer copy now owns +1 retain count on the texture which the
		 * caller must release via MetalCompositorSubmitFrame_ReleaseCachedOverlay. */
		out_layer->source = *out_tex_retained;
	}
	return have;
}

extern "C" void MetalCompositorSubmitFrame_ReleaseCachedOverlay(void *tex_retained)
{
	if (tex_retained == NULL) return;
	/* Balance the __bridge_retained in AcquireCachedOverlay. */
	(void)(__bridge_transfer id<MTLTexture>)tex_retained;
}

extern "C" void MetalCompositorSubmitFrame_ClearCachedOverlay(void)
{
	os_unfair_lock_lock(&s_overlay_cache_lock);
	s_overlay_cache_tex   = nil;
	s_overlay_cache_valid = false;
	os_unfair_lock_unlock(&s_overlay_cache_lock);
}

extern "C" void MetalCompositorFlushDeferredPresent(void)
{
	/* Metal encodes independently of engine passes; nothing to flush. */
}

extern "C" void MetalCompositorSubmitFrame_ClearCachedFramebuffer(void)
{
	/* Metal copies DSp directly into its framebuffer texture and therefore
	 * has no persistent framebuffer-slot cache to invalidate. */
}

extern "C" void MetalCompositorSubmitFrame_ClearCachedOverlayForSource(void *source)
{
    if (source == NULL) return;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)source;
    os_unfair_lock_lock(&s_overlay_cache_lock);
    if (s_overlay_cache_tex == texture) {
        s_overlay_cache_tex = nil;
        s_overlay_cache_valid = false;
    }
    os_unfair_lock_unlock(&s_overlay_cache_lock);
}

/* Called from the VBL callback; the stored value is currently unused. */
extern "C"
void MetalCompositorSubmitFrame_SetTargetTimestamp(double ts)
{
	atomic_store_explicit(&s_target_presentation_ts, ts, memory_order_release);
}

/* For RAVE/GL, which include metal_compositor.h but not vbl_source.h. */
extern "C"
int32_t MetalCompositorSync3DFramePacingForEngine(int32_t engine_id)
{
	return vbl_source_sync_3d_pacing_for_engine(engine_id);
}

// Present calls this after drawing the 2D underlay in the same encoder.

extern "C" void MetalCompositorSubmitFrame_EncodeCachedOverlay(
	void *render_encoder, const struct CompositeLayer *layer, void *display_gamma_lut)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)render_encoder;
	id<MTLBuffer> gamma = (__bridge id<MTLBuffer>)display_gamma_lut;
	submitframe_encode_layer_display_gamma(enc, layer, gamma);
}

/*
 * Find the library with submitframe_vertex: the default library first, then
 * each loaded bundle's default.metallib.
 */
static id<MTLLibrary> submitframe_resolve_library(id<MTLDevice> dev)
{
	id<MTLLibrary> lib = [dev newDefaultLibrary];
	if (lib != nil) {
		id<MTLFunction> probe = [lib newFunctionWithName:@"submitframe_vertex"];
		if (probe != nil) return lib;
	}
	for (NSBundle *b in [NSBundle allBundles]) {
		NSURL *url = [b URLForResource:@"default" withExtension:@"metallib"];
		if (url == nil) continue;
		NSError *err = nil;
		id<MTLLibrary> candidate = [dev newLibraryWithURL:url error:&err];
		if (candidate == nil) continue;
		id<MTLFunction> probe = [candidate newFunctionWithName:@"submitframe_vertex"];
		if (probe != nil) return candidate;
	}
	return lib;   /* return the best-we-got (may still be nil) */
}

extern "C" int MetalCompositorSubmitFrame_BindPresentationContext(
	void *device, void *queue, void *cametal_layer)
{
	if (device == NULL || queue == NULL) {
		COMPSF_ERR("Bind: NULL device/queue");
		return -1;
	}
	s_device = (__bridge id<MTLDevice>)device;
	s_queue  = (__bridge id<MTLCommandQueue>)queue;
	s_layer  = (__bridge CAMetalLayer *)cametal_layer;  // may be nil in some test modes

	/* Resolve shader library (see submitframe_resolve_library). */
	s_library = submitframe_resolve_library(s_device);
	if (!s_library) {
		COMPSF_ERR("Bind: no Metal library with submitframe shaders found");
		s_device = nil;
		s_queue  = nil;
		s_layer  = nil;
		return -1;
	}

	/* Release any prior PSOs. */
	for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
	s_pipe_display_premultiplied = nil;

	if (submitframe_build_pipelines() != 0) {
		for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
		s_pipe_display_premultiplied = nil;
		s_device  = nil;
		s_queue   = nil;
		s_layer   = nil;
		s_library = nil;
		return -1;
	}

	if (s_inflight_semaphore == NULL) {
		s_inflight_semaphore = dispatch_semaphore_create(3);
		if (s_inflight_semaphore == NULL) {
			COMPSF_ERR("Bind: dispatch_semaphore_create(3) failed");
			for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
			s_pipe_display_premultiplied = nil;
			s_device  = nil;
			s_queue   = nil;
			s_layer   = nil;
			s_library = nil;
			return -1;
		}
	}
	return 0;
}

extern "C" void MetalCompositorSubmitFrame_UnbindPresentationContext(void)
{
	/* Drain the semaphore (3 waits) to ensure any in-flight GPU completion
	 * handlers have fired before we release the semaphore object - then
	 * signal 3 times to restore the semaphore's count to its initial value
	 * (3). Releasing a dispatch_semaphore_t whose count is less than its
	 * initial value triggers libdispatch abort_cause_3 under ARC dealloc.
	 * Per Apple docs: "Calling dispatch_release on a semaphore with a
	 * count lower than the value passed to dispatch_semaphore_create is
	 * a bug." */
	if (s_inflight_semaphore != NULL) {
		dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_signal(s_inflight_semaphore);
		dispatch_semaphore_signal(s_inflight_semaphore);
		dispatch_semaphore_signal(s_inflight_semaphore);
		s_inflight_semaphore = nil;
	}
	for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
	s_pipe_display_premultiplied = nil;
	s_device  = nil;
	s_queue   = nil;
	s_layer   = nil;
	s_library = nil;
	/* Drop any cached overlay whose texture is tied to a now-torn-down
	 * device / drawable size (rave-overlay-flicker-black fix). */
	os_unfair_lock_lock(&s_overlay_cache_lock);
	s_overlay_cache_tex   = nil;
	s_overlay_cache_valid = false;
	os_unfair_lock_unlock(&s_overlay_cache_lock);
}


// Validates the descriptor and caches its overlay layer. Never touches a
// drawable; MetalCompositorPresent owns presentation.

extern "C" int32_t MetalCompositorSubmitFrame(const struct FrameDescriptor *desc)
{
	// 1. Cheap validation first - no semaphore consumed.
	if (desc == NULL) {
		return kGfxAccelErrInvalidDescriptor;
	}
	if (desc->layer_count > kGfxAccelMaxLayers) {
		return kGfxAccelErrInvalidDescriptor;
	}
	if (desc->layer_count > 0 && desc->layers == NULL) {
		return kGfxAccelErrInvalidDescriptor;
	}
	for (uint32_t i = 0; i < desc->layer_count; ++i) {
		const struct CompositeLayer *layer = &desc->layers[i];
		if ((int)layer->slot < 0 || (int)layer->slot >= kLayerSlotCount) {
			return kGfxAccelErrInvalidSlot;
		}
		if (layer->source == NULL) {
			return kGfxAccelErrInvalidDescriptor;
		}
	}


	// 2. Stale-generation rejection (cheap atomic load; closes the UAF
	//    under subscriber-reject rollback).
	const DMCModeSnapshot *cur = dmc_current_snapshot();
	if (cur != NULL && desc->generation != cur->generation) {
		return kGfxAccelErrStaleGeneration;
	}

	int32_t eviction_err = gfxaccel_heap_wait_for_eviction(0);
	if (eviction_err != kGfxAccelResNoErr) {
		return eviction_err;
	}

	// 3. Guard against un-bound compositor.
	if (s_inflight_semaphore == NULL) {
		COMPSF_ERR("SubmitFrame: inflight semaphore NULL - bind not called");
		return kGfxAccelErrPipelineUnavailable;
	}
	if (desc->layer_count > 0 && s_pipe_for_blend[kBlendOpaque] == nil) {
		COMPSF_ERR("SubmitFrame: blend PSOs not built");
		return kGfxAccelErrPipelineUnavailable;
	}

	// 4. Populate the overlay cache first.  In production (no test render
	//    target) this is the only work SubmitFrame does -- Present owns
	//    the drawable and composites the cached overlay atop the 2D
	//    framebuffer.  We cache the LAST overlay-slot layer in the
	//    descriptor; a typical RAVE/GL frame submits a single overlay
	//    layer, so "last" == "only" in practice.  Framebuffer / underlay
	//    layers are NOT cached and are ignored in production.
	for (int32_t i = (int32_t)desc->layer_count - 1; i >= 0; --i) {
		if (desc->layers[i].slot == kLayerSlotOverlay) {
			submitframe_cache_store(&desc->layers[i]);
			break;
		}
	}

	// 5. Production: Present is the sole drawable owner.  Cache is populated,
	//    nothing more to do.  No semaphore consumed -- we do no GPU work.
	return kGfxAccelNoErr;

}

