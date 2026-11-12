#include "gfxaccelhost.h"

#define GFXACCELHOSTPROGRAMS 19

typedef void (APIENTRY *GfxAccelHostBindProgramProc)(GLenum, GLuint);
typedef void (APIENTRY *GfxAccelHostProgramNamesProc)(GLsizei, GLuint *);
typedef GLboolean (APIENTRY *GfxAccelHostIsProgramProc)(GLuint);
typedef void (APIENTRY *GfxAccelHostProgramVectorProc)(GLenum, GLuint, void *);
typedef void (APIENTRY *GfxAccelHostProgramStringProc)(GLenum, GLenum, GLsizei, const void *);
typedef void (APIENTRY *GfxAccelHostProgramQueryProc)(GLenum, GLenum, void *);

typedef struct GfxAccelHostTarget {
	GLuint overlays[2];
	GLuint current;
	GLuint framebuffer;
	GLuint depth;
	GLuint pending;
	uint32_t width;
	uint32_t height;
	uint32_t write;
	int32_t left;
	int32_t top;
	int32_t drawwidth;
	int32_t drawheight;
	bool frameactive;
	bool committed;
	bool attached;
	GLuint readframebuffer;
	GLuint programs[2];
	bool programenabled[2];
	bool envset[2][96];
	float env[2][96][4];
	void *procs[GFXACCELHOSTPROGRAMS];
	bool procsloaded;
	int matrixslot;
	int textureslot;
	bool matrixloaded[6];
	float matrices[6][16];
	bool viewportloaded;
	int viewport[4];
} GfxAccelHostTarget;

static void GfxAccelHostLoadPrograms(GfxAccelHostTarget *target)
{
	static const char *const names[GFXACCELHOSTPROGRAMS] = {
		"glBindProgramARB", "glDeleteProgramsARB", "glGenProgramsARB", "glIsProgramARB",
		"glProgramEnvParameter4dARB", "glProgramEnvParameter4dvARB", "glProgramEnvParameter4fARB",
		"glProgramEnvParameter4fvARB", "glProgramLocalParameter4dARB", "glProgramLocalParameter4dvARB",
		"glProgramLocalParameter4fARB", "glProgramLocalParameter4fvARB", "glGetProgramEnvParameterdvARB",
		"glGetProgramEnvParameterfvARB", "glGetProgramLocalParameterdvARB", "glGetProgramLocalParameterfvARB",
		"glProgramStringARB", "glGetProgramStringARB", "glGetProgramivARB"
	};
	int index;
	if (target->procsloaded)
		return;
	for (index = 0; index < GFXACCELHOSTPROGRAMS; index++)
#if defined(GFXACCEL_USE_SDLGPU)
		target->procs[index] = SDLGPUGetProgramEntry(index);
#else
		target->procs[index] = GfxGLGetProcAddress(names[index]);
#endif
	target->procsloaded = true;
}

static GLenum GfxAccelHostProgramTarget(int index)
{
	if (index == 0)
		return GFXACCELHOSTVERTEXPROGRAM;
	return GFXACCELHOSTFRAGMENTPROGRAM;
}

static int GfxAccelHostProgramIndex(uint32_t programtarget)
{
	if (programtarget == GFXACCELHOSTVERTEXPROGRAM)
		return 0;
	if (programtarget == GFXACCELHOSTFRAGMENTPROGRAM)
		return 1;
	return -1;
}

static void GfxAccelHostProgramsApply(GfxAccelHostTarget *target)
{
	int type;
	int slot;
	GfxAccelHostLoadPrograms(target);
	if (!target->procs[0] || !target->procs[7])
		return;
	for (type = 0; type < 2; type++) {
		((GfxAccelHostBindProgramProc)target->procs[0])(GfxAccelHostProgramTarget(type), target->programs[type]);
		if (target->programenabled[type])
			glEnable(GfxAccelHostProgramTarget(type));
		else
			glDisable(GfxAccelHostProgramTarget(type));
		for (slot = 0; slot < 96; slot++) {
			if (target->envset[type][slot])
				((GfxAccelHostProgramVectorProc)target->procs[7])(GfxAccelHostProgramTarget(type), slot, target->env[type][slot]);
		}
	}
}

static void GfxAccelHostSelectTextureUnit(int unit)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	if (extensions->multitex && extensions->ActiveTexture)
		extensions->ActiveTexture(GL_TEXTURE0 + unit);
}

static GLuint GfxAccelHostRectangleHostName(GLContext *context, uint32_t guestname)
{
	GLTextureObject *object;
	GLuint hostname;
	if (!guestname)
		return 0;
	object = &context->texture_objects[guestname];
	if (!object->metal_texture) {
		glGenTextures(1, &hostname);
		object->metal_texture = (void *)(uintptr_t)hostname;
	}
	return (GLuint)(uintptr_t)object->metal_texture;
}

void *GfxAccelHostTargetCreate(void)
{
	return calloc(1, sizeof(GfxAccelHostTarget));
}

void GfxAccelHostTargetDestroy(void *target)
{
	free(target);
}

void GfxAccelHostTargetLeave(void *handle)
{
	GfxAccelHostTarget *target;
	GfxGLExt *extensions;
	target = (GfxAccelHostTarget *)handle;
	extensions = &gfx_gl_ext();
	target->overlays[0] = s_ov[0];
	target->overlays[1] = s_ov[1];
	target->current = s_cur;
	target->framebuffer = s_fbo;
	target->depth = s_depth;
	target->width = s_ow;
	target->height = s_oh;
	target->write = s_wr;
	target->left = s_dl;
	target->top = s_dt;
	target->drawwidth = s_dw;
	target->drawheight = s_dh;
	target->frameactive = s_frame_active;
	target->committed = s_frame_committed;
	if (s_frame_active && extensions->fbo)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GfxAccelHostTargetEnter(void *handle)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	s_ov[0] = target->overlays[0];
	s_ov[1] = target->overlays[1];
	s_cur = target->current;
	s_fbo = target->framebuffer;
	s_depth = target->depth;
	s_ow = target->width;
	s_oh = target->height;
	s_wr = target->write;
	s_dl = target->left;
	s_dt = target->top;
	s_dw = target->drawwidth;
	s_dh = target->drawheight;
	s_frame_active = target->frameactive;
	s_frame_committed = target->committed;
}

void GfxAccelHostTargetResume(GLContext *context, void *handle)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	if (s_frame_active && extensions->fbo)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	GLMetalInvalidateStateCache(context);
	GfxAccelHostProgramsApply((GfxAccelHostTarget *)handle);
}

int GfxAccelHostTargetFrameActive(void)
{
	if (s_frame_active)
		return 1;
	return 0;
}

void GfxAccelHostTargetFlush(void)
{
	if (s_frame_active)
		glFlush();
}

void GfxAccelHostTargetReadPixels(uint32_t width, uint32_t height, void *pixels)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	if (!s_fbo || !extensions->fbo) {
		memset(pixels, 0, (size_t)width * height * 4);
		return;
	}
	if (!s_frame_active)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GFXACCELHOSTUNSIGNEDINT8888, pixels);
	if (!s_frame_active)
		extensions->BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GfxAccelHostTargetReleaseOverlays(void *handle)
{
	if (((GfxAccelHostTarget *)handle)->attached) {
		s_ov[0] = 0;
		s_ov[1] = 0;
		s_cur = 0;
	}
	GLMetalReleaseOverlayTextures(true);
}

void GfxAccelHostTargetAttachTexture(void *handle, unsigned texture, uint32_t width, uint32_t height)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	if (!target->attached && s_ov[0])
		GLMetalReleaseOverlayTextures(false);
	target->attached = true;
	s_ov[0] = texture;
	s_ov[1] = texture;
	s_cur = texture;
	s_ow = width;
	s_oh = height;
	s_wr = 0;
	s_dl = 0;
	s_dt = 0;
	s_dw = (int32_t)width;
	s_dh = (int32_t)height;
	s_frame_committed = false;
}

void *GfxAccelHostOverlayVend(void *handle, int index, uint32_t width, uint32_t height)
{
	GfxAccelHostTarget *target;
	GLuint name;
	target = (GfxAccelHostTarget *)handle;
	if (index != 0)
		return (void *)(uintptr_t)target->pending;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	target->pending = name;
	return (void *)(uintptr_t)name;
}

void GfxAccelHostOverlayRelease(void *texture)
{
	GLuint name;
	name = (GLuint)(uintptr_t)texture;
	glDeleteTextures(1, &name);
}

void GfxAccelHostOverlayBind(int32_t width, int32_t height)
{
	gl_overlay_bind(0, 0, width, height);
}

int GfxAccelHostDeviceReady(void)
{
#if defined(GFXACCEL_USE_SDLGPU)
	if (!SDLGPUInitializeWindowless())
		return 0;
#endif
	if (SharedMetalDevice())
		return 1;
	return 0;
}

void GfxAccelHostInvalidateState(GLContext *context)
{
	GLMetalInvalidateStateCache(context);
}

int GfxAccelHostTextureUnitCount(GLContext *context)
{
	return GLMetalGetTextureUnitCount(context);
}

void GfxAccelHostHostString(uint32_t name, char *text, uint32_t size)
{
	const char *value;
	memset(text, 0, size);
	value = (const char *)glGetString(name);
#if defined(GFXACCEL_USE_SDLGPU)
	if (name == GL_VERSION)
		value = "2.1";
	if (name == GL_VENDOR)
		value = "ATI Technologies Inc.";
	if (name == GL_RENDERER)
		value = "ATI Radeon X1600 OpenGL Engine";
#endif
	if (value)
		strncpy(text, value, size - 1);
}

unsigned GfxAccelHostTextureCreate(uint32_t width, uint32_t height, int floatpixels)
{
	GLuint name;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (floatpixels)
		glTexImage2D(GL_TEXTURE_2D, 0, GFXACCELHOSTRGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	return name;
}

void GfxAccelHostTextureDelete(unsigned texture)
{
	GLuint name;
	name = texture;
	glDeleteTextures(1, &name);
}

int GfxAccelHostFramebufferAvailable(void)
{
	if (gfx_gl_ext().fbo)
		return 1;
	return 0;
}

void GfxAccelHostReadFramebufferAttach(void *handle, unsigned texture)
{
	GfxAccelHostTarget *target;
	GfxGLExt *extensions;
	target = (GfxAccelHostTarget *)handle;
	extensions = &gfx_gl_ext();
	if (!target->readframebuffer)
		extensions->GenFramebuffers(1, &target->readframebuffer);
	extensions->BindFramebuffer(GFXACCELHOSTREADFRAMEBUFFER, target->readframebuffer);
	extensions->FramebufferTexture2D(GFXACCELHOSTREADFRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
}

void GfxAccelHostReadFramebufferRelease(void)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	if (s_frame_active)
		extensions->BindFramebuffer(GFXACCELHOSTREADFRAMEBUFFER, s_fbo);
	else
		extensions->BindFramebuffer(GFXACCELHOSTREADFRAMEBUFFER, 0);
}

void GfxAccelHostTextureCopyFromRead(GLTextureObject *object, uint32_t internalformat, int width, int height)
{
	glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)object->metal_texture);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, internalformat, 0, 0, (GLsizei)width, (GLsizei)height, 0);
}

void GfxAccelHostShadowTextureCreate(GLContext *context, GLTextureObject *object)
{
	static const uint8_t shadowpixel[4] = {255, 255, 255, 255};
	GLuint name;
	glGenTextures(1, &name);
	GLMetalSelectTextureUnitZero();
	glBindTexture(GL_TEXTURE_2D, name);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, shadowpixel);
	object->metal_texture = (void *)(uintptr_t)name;
	object->min_filter = GL_NEAREST;
	object->mag_filter = GL_NEAREST;
	object->wrap_s = GL_CLAMP_TO_EDGE;
	object->wrap_t = GL_CLAMP_TO_EDGE;
	GLMetalInvalidateTextureBindings(context);
}

void GfxAccelHostShadowTextureDestroy(GLTextureObject *object)
{
	GLuint name;
	name = (GLuint)(uintptr_t)object->metal_texture;
	glDeleteTextures(1, &name);
}

void GfxAccelHostRectangleBind(GLContext *context, int unit, uint32_t guestname)
{
	GfxAccelHostSelectTextureUnit(unit);
	glBindTexture(GFXACCELHOSTTEXTURERECTANGLE, GfxAccelHostRectangleHostName(context, guestname));
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostRectangleEnable(int unit, int enabled)
{
	GfxAccelHostSelectTextureUnit(unit);
	if (enabled)
		glEnable(GFXACCELHOSTTEXTURERECTANGLE);
	else
		glDisable(GFXACCELHOSTTEXTURERECTANGLE);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostRectangleSampler(int unit, GLTextureObject *object)
{
	GfxAccelHostSelectTextureUnit(unit);
	glTexParameteri(GFXACCELHOSTTEXTURERECTANGLE, GL_TEXTURE_MIN_FILTER, GLMetalBaseMinFilter(object->min_filter));
	glTexParameteri(GFXACCELHOSTTEXTURERECTANGLE, GL_TEXTURE_MAG_FILTER, GLMetalMapTextureMagFilter(object->mag_filter));
	glTexParameteri(GFXACCELHOSTTEXTURERECTANGLE, GL_TEXTURE_WRAP_S, GLMetalMapTextureWrap(object->wrap_s));
	glTexParameteri(GFXACCELHOSTTEXTURERECTANGLE, GL_TEXTURE_WRAP_T, GLMetalMapTextureWrap(object->wrap_t));
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostRectangleUpload(int unit, GLTextureObject *object, int level, int xoffset, int yoffset, int width, int height, const uint8_t *pixels, int replacewholeimage)
{
	GLint internalformat;
	GfxAccelHostSelectTextureUnit(unit);
	internalformat = GL_RGBA8;
	if (object->internal_format_opaque)
		internalformat = GL_RGB8;
	if (replacewholeimage)
		glTexImage2D(GFXACCELHOSTTEXTURERECTANGLE, level, internalformat, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	else
		glTexSubImage2D(GFXACCELHOSTTEXTURERECTANGLE, level, xoffset, yoffset, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	GLMetalSelectTextureUnitZero();
	GfxAccelHostRectangleSampler(unit, object);
}

void GfxAccelHostRectangleApply(GLContext *context, int unit, uint32_t guestname, int enabled)
{
	GfxAccelHostSelectTextureUnit(unit);
	glBindTexture(GFXACCELHOSTTEXTURERECTANGLE, GfxAccelHostRectangleHostName(context, guestname));
	if (enabled)
		glEnable(GFXACCELHOSTTEXTURERECTANGLE);
	else
		glDisable(GFXACCELHOSTTEXTURERECTANGLE);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostRectangleDisable(int unit)
{
	GfxAccelHostSelectTextureUnit(unit);
	glDisable(GFXACCELHOSTTEXTURERECTANGLE);
}

void GfxAccelHostRectangleFinish(void)
{
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostRectangleCopyFramebuffer(int unit, int level, uint32_t internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage)
{
	GfxAccelHostSelectTextureUnit(unit);
	if (replacewholeimage)
		glCopyTexImage2D(GFXACCELHOSTTEXTURERECTANGLE, level, internalformat, sourcex, sourcey, width, height, 0);
	else
		glCopyTexSubImage2D(GFXACCELHOSTTEXTURERECTANGLE, level, xoffset, yoffset, sourcex, sourcey, width, height);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostBlendFunc(uint32_t sourcefactor, uint32_t destinationfactor, int separate, uint32_t sourcealpha, uint32_t destinationalpha)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	if (separate && extensions->BlendFuncSeparate) {
		extensions->BlendFuncSeparate(sourcefactor, destinationfactor, sourcealpha, destinationalpha);
		return;
	}
	glBlendFunc(sourcefactor, destinationfactor);
}

void GfxAccelHostRasterPosition(GLContext *context)
{
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(context->viewport[0], context->viewport[0] + context->viewport[2], context->viewport[1], context->viewport[1] + context->viewport[3], -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	glRasterPos2i((GLint)context->raster_pos[0], (GLint)context->raster_pos[1]);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}

void GfxAccelHostMatrixMode(void *handle, uint32_t mode)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	if (mode == GL_TEXTURE) {
		target->matrixslot = target->textureslot;
		if (target->textureslot < 3)
			target->textureslot++;
	} else if (mode == GL_PROJECTION) {
		target->matrixslot = 4;
		target->textureslot = 0;
	} else {
		target->matrixslot = 5;
	}
	glMatrixMode(mode);
}

void GfxAccelHostLoadMatrix(GLContext *context, void *handle, const float *matrix)
{
	GfxAccelHostTarget *target;
	GLMetalStateCache *cache;
	int slot;
	target = (GfxAccelHostTarget *)handle;
	cache = GLMetalGetStateCache(context);
	slot = target->matrixslot;
	if (cache && cache->valid && target->matrixloaded[slot] && !memcmp(target->matrices[slot], matrix, sizeof(target->matrices[slot])))
		return;
	memcpy(target->matrices[slot], matrix, sizeof(target->matrices[slot]));
	target->matrixloaded[slot] = true;
	glLoadMatrixf(matrix);
}

void GfxAccelHostViewport(GLContext *context, void *handle, int x, int y, int width, int height)
{
	GfxAccelHostTarget *target;
	GLMetalStateCache *cache;
	target = (GfxAccelHostTarget *)handle;
	cache = GLMetalGetStateCache(context);
	if (cache && cache->valid && target->viewportloaded && target->viewport[0] == x && target->viewport[1] == y && target->viewport[2] == width && target->viewport[3] == height)
		return;
	target->viewport[0] = x;
	target->viewport[1] = y;
	target->viewport[2] = width;
	target->viewport[3] = height;
	target->viewportloaded = true;
	glViewport(x, y, width, height);
}

int GfxAccelHostProgramEntryAvailable(void *handle, int entry)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	GfxAccelHostLoadPrograms(target);
	if (target->procs[entry])
		return 1;
	return 0;
}

void GfxAccelHostProgramEnable(void *handle, uint32_t capability, int enable)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	target->programenabled[GfxAccelHostProgramIndex(capability)] = enable != 0;
	if (enable)
		glEnable(capability);
	else
		glDisable(capability);
}

void GfxAccelHostProgramBind(void *handle, uint32_t programtarget, uint32_t name)
{
	GfxAccelHostTarget *target;
	int type;
	target = (GfxAccelHostTarget *)handle;
	((GfxAccelHostBindProgramProc)target->procs[0])(programtarget, name);
	type = GfxAccelHostProgramIndex(programtarget);
	if (type >= 0)
		target->programs[type] = name;
}

void GfxAccelHostProgramsDelete(void *handle, uint32_t count, const uint32_t *names)
{
	((GfxAccelHostProgramNamesProc)((GfxAccelHostTarget *)handle)->procs[1])(count, (GLuint *)names);
}

void GfxAccelHostProgramsGenerate(void *handle, uint32_t count, uint32_t *names)
{
	((GfxAccelHostProgramNamesProc)((GfxAccelHostTarget *)handle)->procs[2])(count, (GLuint *)names);
}

uint32_t GfxAccelHostProgramIs(void *handle, uint32_t name)
{
	return ((GfxAccelHostIsProgramProc)((GfxAccelHostTarget *)handle)->procs[3])(name);
}

static void GfxAccelHostRememberEnv(GfxAccelHostTarget *target, uint32_t programtarget, uint32_t slot, const float *values)
{
	int type;
	type = GfxAccelHostProgramIndex(programtarget);
	if (type < 0 || slot >= 96)
		return;
	memcpy(target->env[type][slot], values, sizeof(target->env[type][slot]));
	target->envset[type][slot] = true;
}

void GfxAccelHostProgramParameterDoubles(void *handle, int local, uint32_t programtarget, uint32_t slot, double *values)
{
	GfxAccelHostTarget *target;
	float floats[4];
	int index;
	int entry;
	target = (GfxAccelHostTarget *)handle;
	entry = 5;
	if (local)
		entry = 9;
	((GfxAccelHostProgramVectorProc)target->procs[entry])(programtarget, slot, values);
	if (local)
		return;
	for (index = 0; index < 4; index++)
		floats[index] = (float)values[index];
	GfxAccelHostRememberEnv(target, programtarget, slot, floats);
}

void GfxAccelHostProgramParameterFloats(void *handle, int local, uint32_t programtarget, uint32_t slot, float *values)
{
	GfxAccelHostTarget *target;
	int entry;
	target = (GfxAccelHostTarget *)handle;
	entry = 7;
	if (local)
		entry = 11;
	((GfxAccelHostProgramVectorProc)target->procs[entry])(programtarget, slot, values);
	if (!local)
		GfxAccelHostRememberEnv(target, programtarget, slot, values);
}

void GfxAccelHostProgramGetDoubles(void *handle, int local, uint32_t programtarget, uint32_t slot, double *values)
{
	GfxAccelHostTarget *target;
	int entry;
	target = (GfxAccelHostTarget *)handle;
	entry = 12;
	if (local)
		entry = 14;
	((GfxAccelHostProgramVectorProc)target->procs[entry])(programtarget, slot, values);
}

void GfxAccelHostProgramGetFloats(void *handle, int local, uint32_t programtarget, uint32_t slot, float *values)
{
	GfxAccelHostTarget *target;
	int entry;
	target = (GfxAccelHostTarget *)handle;
	entry = 13;
	if (local)
		entry = 15;
	((GfxAccelHostProgramVectorProc)target->procs[entry])(programtarget, slot, values);
}

int GfxAccelHostProgramString(void *handle, uint32_t programtarget, uint32_t format, uint32_t length, const void *text)
{
	GLint position;
	((GfxAccelHostProgramStringProc)((GfxAccelHostTarget *)handle)->procs[16])(programtarget, format, length, text);
	position = -1;
	glGetIntegerv(GFXACCELHOSTPROGRAMERRORPOSITION, &position);
	return position;
}

void GfxAccelHostProgramGetString(void *handle, uint32_t programtarget, uint32_t name, void *text)
{
	((GfxAccelHostProgramQueryProc)((GfxAccelHostTarget *)handle)->procs[17])(programtarget, name, text);
}

int GfxAccelHostProgramQuery(void *handle, uint32_t programtarget, uint32_t name)
{
	GLint value;
	value = 0;
	((GfxAccelHostProgramQueryProc)((GfxAccelHostTarget *)handle)->procs[18])(programtarget, name, &value);
	return value;
}
