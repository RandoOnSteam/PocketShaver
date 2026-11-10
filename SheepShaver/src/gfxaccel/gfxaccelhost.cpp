#include "sysdeps.h"
#include "cpu_emulation.h"
#include "gl_engine.h"
#include "gfxaccelhost.h"

uint32_t GuestTickCount(void)
{
	if (!RAMBaseHost)
		return 0;
	return ReadMacInt32(0x016a);
}

void GfxAccelHostBeginFrame(GLContext *ctx)
{
	GLMetalBeginFrame(ctx);
}

void GfxAccelHostBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLMetalBitmap(ctx, width, height, bgra_data, data_len);
}

void GfxAccelHostClear(GLContext *ctx, uint32_t mask)
{
	GLMetalClear(ctx, mask);
}

void GfxAccelHostDestroyTexture(GLTextureObject *texObj)
{
	GLMetalDestroyTexture(texObj);
}

void GfxAccelHostDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLMetalDrawPixels(ctx, width, height, bgra_data, data_len);
}

void GfxAccelHostEndFrame(GLContext *ctx)
{
	GLMetalEndFrame(ctx);
}

void GfxAccelHostFlushImmediateMode(GLContext *ctx)
{
	GLMetalFlushImmediateMode(ctx);
}

void GfxAccelHostInit(GLContext *ctx)
{
	GLMetalInit(ctx);
}

uint8_t * GfxAccelHostReadFramebufferRect(GLContext *ctx, int x, int y, int width, int height, int *out_len)
{
	return GLMetalReadFramebufferRect(ctx, x, y, width, height, out_len);
}

void GfxAccelHostRelease(GLContext *ctx)
{
	GLMetalRelease(ctx);
}

void GfxAccelHostUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, int depth, const uint8_t *data, int dataLen)
{
	GLMetalUpload3DTexture(ctx, texObj, level, width, height, depth, data, dataLen);
}

void GfxAccelHostUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int w, int h, const uint8_t *data, int bytesPerRow)
{
	GLMetalUploadSubTexture(ctx, texObj, level, xoff, yoff, w, h, data, bytesPerRow);
}

void GfxAccelHostUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int zoff, int w, int h, int d, const uint8_t *data, int bytesPerRow, int bytesPerImage)
{
	GLMetalUploadSubTexture3D(ctx, texObj, level, xoff, yoff, zoff, w, h, d, data, bytesPerRow, bytesPerImage);
}

void GfxAccelHostUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, const uint8_t *data, int dataLen)
{
	GLMetalUploadTexture(ctx, texObj, level, width, height, data, dataLen);
}
