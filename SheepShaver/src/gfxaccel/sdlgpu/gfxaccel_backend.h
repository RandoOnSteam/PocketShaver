/*
 *  gfxaccel_backend.h - SDL-GPU backend identity and resource vocabulary
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef GFXACCEL_BACKEND_H
#define GFXACCEL_BACKEND_H

#if !defined(GFXACCEL_USE_SDLGPU)
#error "The SDL-GPU backend header requires GFXACCEL_USE_SDLGPU"
#endif

#define GFXACCEL_BACKEND_NAME "SDL-GPU"

typedef void *GfxGpuTexture;
typedef void *GfxGpuBuffer;
typedef void *GfxGpuDevice;
typedef void *GfxGpuCommandQueue;

typedef enum {
	kGfxPixelFormatBGRA8Unorm = 0,
	kGfxPixelFormatR8Uint     = 1,
	kGfxPixelFormatR16Uint    = 2,
	kGfxPixelFormatDepth32F   = 3,
	kGfxPixelFormatRGBA8Unorm = 4
} GfxPixelFormat;

#ifndef MTLPixelFormatBGRA8Unorm
#define MTLPixelFormatBGRA8Unorm 80u
#endif

#endif
