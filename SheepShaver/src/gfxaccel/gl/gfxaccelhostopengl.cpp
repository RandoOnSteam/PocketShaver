#include "gfxaccelhost.h"

#define GFXACCELHOSTPROGRAMS 22
#define GFXACCELHOSTTEXTUREUNITS 4

typedef void (APIENTRY *GfxAccelHostBindProgramProc)(GLenum, GLuint);
typedef void (APIENTRY *GfxAccelHostProgramNamesProc)(GLsizei, GLuint *);
typedef GLboolean (APIENTRY *GfxAccelHostIsProgramProc)(GLuint);
typedef void (APIENTRY *GfxAccelHostProgramVectorProc)(GLenum, GLuint, void *);
typedef void (APIENTRY *GfxAccelHostProgramStringProc)(GLenum, GLenum, GLsizei, const void *);
typedef void (APIENTRY *GfxAccelHostProgramQueryProc)(GLenum, GLenum, void *);
typedef void (APIENTRY *GfxAccelHostAttributeProc)(GLuint, const GLfloat *);
typedef void (APIENTRY *GfxAccelHostVolumeProc)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *GfxAccelHostVolumeUpdateProc)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);

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
		"glProgramStringARB", "glGetProgramStringARB", "glGetProgramivARB", "glVertexAttrib4fvARB", "glTexImage3D", "glTexSubImage3D"
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

static GLuint GfxAccelHostTextureHostName(GLTextureObject *object)
{
	GLuint hostname;
	if (!object->metal_texture) {
		glGenTextures(1, &hostname);
		object->metal_texture = (void *)(uintptr_t)hostname;
	}
	return (GLuint)(uintptr_t)object->metal_texture;
}

static GLenum GfxAccelHostBindingTarget(uint32_t target)
{
	if (target >= GFXACCELHOSTCUBEMAPFIRSTFACE && target <= GFXACCELHOSTCUBEMAPLASTFACE)
		return GFXACCELHOSTCUBEMAP;
	return target;
}

static void GfxAccelHostTextureSelect(int unit, uint32_t target, GLTextureObject *object)
{
	GfxAccelHostSelectTextureUnit(unit);
	glBindTexture(GfxAccelHostBindingTarget(target), GfxAccelHostTextureHostName(object));
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
	GLMetalBindSamples(context);
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

void GfxAccelHostTargetReadPixels(GLContext *context, uint32_t width, uint32_t height, void *pixels)
{
	GfxGLExt *extensions;
	extensions = &gfx_gl_ext();
	GLMetalResolveSamples(context);
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

int GfxAccelHostTargetAdopt(GLContext *context, uint32_t width, uint32_t height)
{
	GfxGLExt *extensions;
	int adopted;
	extensions = &gfx_gl_ext();
	if (!s_fbo || !extensions->fbo)
		return 0;
	GLMetalResolveSamples(context);
	adopted = MetalCompositorAdoptFrame(s_fbo, (int)width, (int)height);
	if (s_frame_active) {
		extensions->BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
		GLMetalBindSamples(context);
	}
	return adopted;
}

void GfxAccelHostTargetReleaseOverlays(void *handle)
{
	if (((GfxAccelHostTarget *)handle)->attached) {
		s_ov[0] = 0;
		s_ov[1] = 0;
		s_cur = 0;
	}
	((GfxAccelHostTarget *)handle)->attached = false;
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

void GfxAccelHostTextureApply(GLContext *context, int unit, uint32_t target, uint32_t guestname, int enabled)
{
	GfxAccelHostSelectTextureUnit(unit);
	if (guestname)
		glBindTexture(target, GfxAccelHostTextureHostName(&context->texture_objects[guestname]));
	else
		glBindTexture(target, 0);
	if (enabled)
		glEnable(target);
	else
		glDisable(target);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostTextureParameter(GLContext *context, int unit, uint32_t target, GLTextureObject *object, uint32_t pname, float value)
{
	GfxAccelHostTextureSelect(unit, target, object);
	if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GFXACCELHOSTTEXTUREWRAPR) && value == (float)GFXACCELHOSTCLAMP)
		value = (float)GL_CLAMP_TO_EDGE;
	if (target == GFXACCELHOSTTEXTURERECTANGLE && pname == GL_TEXTURE_MIN_FILTER)
		value = (float)GLMetalBaseMinFilter((uint32_t)value);
	glTexParameterf(target, pname, value);
	GLMetalSelectTextureUnitZero();
	GLMetalInvalidateTextureBindings(context);
}

void GfxAccelHostTextureImage(int unit, uint32_t target, GLTextureObject *object, int level, int xoffset, int yoffset, int width, int height, const uint8_t *pixels, int replacewholeimage)
{
	GLint internalformat;
	GfxAccelHostTextureSelect(unit, target, object);
	internalformat = GL_RGBA8;
	if (object->internal_format_opaque)
		internalformat = GL_RGB8;
	if (replacewholeimage)
		glTexImage2D(target, level, internalformat, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	else
		glTexSubImage2D(target, level, xoffset, yoffset, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostTextureVolume(void *handle, int unit, GLTextureObject *object, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, const uint8_t *pixels, int replacewholeimage)
{
	GfxAccelHostTarget *target;
	target = (GfxAccelHostTarget *)handle;
	GfxAccelHostLoadPrograms(target);
	if (!target->procs[GFXACCELHOSTVOLUMEENTRY] || !target->procs[GFXACCELHOSTVOLUMEENTRY + 1])
		return;
	GfxAccelHostTextureSelect(unit, GFXACCELHOSTTEXTURE3D, object);
	if (replacewholeimage)
		((GfxAccelHostVolumeProc)target->procs[GFXACCELHOSTVOLUMEENTRY])(GFXACCELHOSTTEXTURE3D, level, GL_RGBA8, width, height, depth, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	else
		((GfxAccelHostVolumeUpdateProc)target->procs[GFXACCELHOSTVOLUMEENTRY + 1])(GFXACCELHOSTTEXTURE3D, level, xoffset, yoffset, zoffset, width, height, depth, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	GLMetalSelectTextureUnitZero();
}

void GfxAccelHostTextureCopyFramebuffer(GLContext *context, int unit, uint32_t target, GLTextureObject *object, int level, uint32_t internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage)
{
	GLMetalResolveSamples(context);
	GfxAccelHostTextureSelect(unit, target, object);
	if (replacewholeimage)
		glCopyTexImage2D(target, level, internalformat, sourcex, sourcey, width, height, 0);
	else
		glCopyTexSubImage2D(target, level, xoffset, yoffset, sourcex, sourcey, width, height);
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

void GfxAccelHostSamples(GLContext *context, int samples)
{
	GLMetalInit(context);
	if (GLMetalGetState(context))
		GLMetalGetState(context)->samples = samples;
}

float *GfxAccelHostReadDepthRect(GLContext *context, int x, int y, int width, int height)
{
	std::vector<float> clipped;
	float *depths;
	int left;
	int bottom;
	int right;
	int top;
	int row;
	int index;
	GLMetalBeginFrame(context);
	if (!s_frame_active || width <= 0 || height <= 0)
		return NULL;
	GLMetalResolveSamples(context);
	depths = (float *)malloc((size_t)width * height * sizeof(float));
	if (!depths)
		return NULL;
	for (index = 0; index < width * height; index++)
		depths[index] = GFXACCELHOSTDEPTHOUTSIDE;
	left = x;
	bottom = y;
	right = x + width;
	top = y + height;
	if (left < 0)
		left = 0;
	if (bottom < 0)
		bottom = 0;
	if (right > (int)s_ow)
		right = (int)s_ow;
	if (top > (int)s_oh)
		top = (int)s_oh;
	if (left >= right || bottom >= top)
		return depths;
	clipped.resize((size_t)(right - left) * (top - bottom));
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(left, bottom, right - left, top - bottom, GL_DEPTH_COMPONENT, GL_FLOAT, vec_data(clipped));
	for (row = bottom; row < top; row++)
		memcpy(depths + (size_t)(row - y) * width + (left - x), vec_data(clipped) + (size_t)(row - bottom) * (right - left), (size_t)(right - left) * sizeof(float));
	return depths;
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

int GfxAccelHostProgramEnabled(void *handle, uint32_t capability)
{
	int type;
	type = GfxAccelHostProgramIndex(capability);
	if (type < 0)
		return 0;
	return ((GfxAccelHostTarget *)handle)->programenabled[type];
}

void GfxAccelHostDrawAttributes(GLContext *context, void *handle, uint32_t mode, uint32_t count, const uint32_t *attributes, uint32_t attributecount, const float *values)
{
	GfxAccelHostAttributeProc attribute;
	bool enabled[GFXACCELHOSTTEXTUREUNITS];
	uint32_t vertex;
	uint32_t slot;
	int unit;
	attribute = (GfxAccelHostAttributeProc)((GfxAccelHostTarget *)handle)->procs[GFXACCELHOSTATTRIBUTEENTRY];
	GLMetalBeginFrame(context);
	if (!s_frame_active)
		return;
	for (unit = 0; unit < GFXACCELHOSTTEXTUREUNITS; unit++) {
		enabled[unit] = context->tex_units[unit].enabled_2d;
		if (((GfxAccelHostTarget *)handle)->programenabled[1] && context->tex_units[unit].bound_object_2d)
			context->tex_units[unit].enabled_2d = true;
	}
	GLMetalApplyState(context);
	for (unit = 0; unit < GFXACCELHOSTTEXTUREUNITS; unit++)
		context->tex_units[unit].enabled_2d = enabled[unit];
	glBegin(mode);
	for (vertex = 0; vertex < count; vertex++) {
		for (slot = 0; slot < attributecount; slot++) {
			attribute(attributes[slot], values);
			values += 4;
		}
	}
	glEnd();
}
