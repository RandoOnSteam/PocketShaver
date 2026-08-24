/*
 *  sdlgpu_gl_compat.h - OpenGL 1.x compatibility entry points for SDL-GPU
 *
 *	(C) 2026 Ryan Norton (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef GFXACCEL_SDLGPU_GL_COMPAT_H
#define GFXACCEL_SDLGPU_GL_COMPAT_H
#include <SDL.h>
#include <SDL_opengl.h>
#include "sdlgpu_transition_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

SDL_GLContext SDLGPUCreateContext(SDL_Window *window);
void SDLGPUDestroyContext(SDL_GLContext context);
SDL_GLContext SDLGPUGetCurrentContext(void);
bool SDLGPUMakeCurrent(SDL_Window *window, SDL_GLContext context);
bool SDLGPUSetAttribute(SDL_GLAttr attr, int value);
bool SDLGPUSetSwapInterval(int interval);
bool SDLGPUSwapWindow(SDL_Window *window);
void SDLGPUReleaseVideoWindow(SDL_Window *window);
SDL_FunctionPointer SDLGPUGetProcAddress(const char *name);
void SDLGPUSetTexturePresentationYFlip(GLuint texture, bool enabled);
void SDLGPUglAccum(GLenum op, GLfloat value);
void SDLGPUglActiveTexture(GLenum texture);
void SDLGPUglAlphaFunc(GLenum func, GLclampf ref);
void SDLGPUglBegin(GLenum mode);
void SDLGPUglBindFramebuffer(GLenum target, GLuint framebuffer);
void SDLGPUglBindRenderbuffer(GLenum target, GLuint renderbuffer);
void SDLGPUglBindTexture(GLenum target, GLuint texture);
void SDLGPUglBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig,
                     GLfloat xmove, GLfloat ymove, const GLubyte *bitmap);
void SDLGPUglBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void SDLGPUglBlendEquation(GLenum mode);
void SDLGPUglBlendFunc(GLenum sfactor, GLenum dfactor);
void SDLGPUglBlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb,
                                GLenum src_alpha, GLenum dst_alpha);
GLenum SDLGPUglCheckFramebufferStatus(GLenum target);
void SDLGPUglClear(GLbitfield mask);
void SDLGPUglClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void SDLGPUglClearDepth(GLclampd depth);
void SDLGPUglClearStencil(GLint stencil);
void SDLGPUglClientActiveTexture(GLenum texture);
void SDLGPUglClipPlane(GLenum plane, const GLdouble *equation);
void SDLGPUglColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void SDLGPUglColor4fv(const GLfloat *v);
void SDLGPUglColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void SDLGPUglColorMaterial(GLenum face, GLenum mode);
void SDLGPUglCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                GLint x, GLint y, GLsizei width, GLsizei height);
void SDLGPUglCullFace(GLenum mode);
void SDLGPUglDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void SDLGPUglDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
void SDLGPUglDeleteTextures(GLsizei n, const GLuint *textures);
void SDLGPUglDepthFunc(GLenum func);
void SDLGPUglDepthMask(GLboolean flag);
void SDLGPUglDepthRange(GLclampd near_val, GLclampd far_val);
void SDLGPUglDisable(GLenum cap);
void SDLGPUglDrawBuffer(GLenum mode);
void SDLGPUglDrawPixels(GLsizei width, GLsizei height, GLenum format,
                         GLenum type, const void *pixels);
void SDLGPUglEnable(GLenum cap);
void SDLGPUglEnd(void);
void SDLGPUglFinish(void);
void SDLGPUglFlush(void);
void SDLGPUglFogCoordf(GLfloat coord);
void SDLGPUglFogf(GLenum pname, GLfloat param);
void SDLGPUglFogfv(GLenum pname, const GLfloat *params);
void SDLGPUglFogi(GLenum pname, GLint param);
void SDLGPUglFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                      GLenum renderbuffer_target, GLuint renderbuffer);
void SDLGPUglFramebufferTexture2D(GLenum target, GLenum attachment,
                                   GLenum textarget, GLuint texture, GLint level);
void SDLGPUglFrontFace(GLenum mode);
void SDLGPUglGenerateMipmap(GLenum target);
void SDLGPUglGenFramebuffers(GLsizei n, GLuint *framebuffers);
void SDLGPUglGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void SDLGPUglGenTextures(GLsizei n, GLuint *textures);
GLenum SDLGPUglGetError(void);
void SDLGPUglGetFloatv(GLenum pname, GLfloat *params);
void SDLGPUglGetIntegerv(GLenum pname, GLint *params);
const GLubyte *SDLGPUglGetString(GLenum name);
void SDLGPUglGetTexEnviv(GLenum target, GLenum pname, GLint *params);
void SDLGPUglGetTexLevelParameteriv(GLenum target, GLint level,
                                     GLenum pname, GLint *params);
void SDLGPUglGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
GLboolean SDLGPUglIsEnabled(GLenum cap);
void SDLGPUglLightf(GLenum light, GLenum pname, GLfloat param);
void SDLGPUglLightfv(GLenum light, GLenum pname, const GLfloat *params);
void SDLGPUglLightModelfv(GLenum pname, const GLfloat *params);
void SDLGPUglLightModeli(GLenum pname, GLint param);
void SDLGPUglLineWidth(GLfloat width);
void SDLGPUglLoadIdentity(void);
void SDLGPUglLoadMatrixf(const GLfloat *m);
void SDLGPUglLogicOp(GLenum opcode);
void SDLGPUglMaterialf(GLenum face, GLenum pname, GLfloat param);
void SDLGPUglMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void SDLGPUglMatrixMode(GLenum mode);
void SDLGPUglMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
void SDLGPUglMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void SDLGPUglNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void SDLGPUglNormal3fv(const GLfloat *v);
void SDLGPUglOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
                    GLdouble near_val, GLdouble far_val);
void SDLGPUglPixelStorei(GLenum pname, GLint param);
void SDLGPUglPointSize(GLfloat size);
void SDLGPUglPolygonMode(GLenum face, GLenum mode);
void SDLGPUglPolygonOffset(GLfloat factor, GLfloat units);
void SDLGPUglPopAttrib(void);
void SDLGPUglPopMatrix(void);
void SDLGPUglPushAttrib(GLbitfield mask);
void SDLGPUglPushMatrix(void);
void SDLGPUglRasterPos2i(GLint x, GLint y);
void SDLGPUglReadBuffer(GLenum mode);
void SDLGPUglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels);
void SDLGPUglRenderbufferStorage(GLenum target, GLenum internalformat,
                                  GLsizei width, GLsizei height);
void SDLGPUglScalef(GLfloat x, GLfloat y, GLfloat z);
void SDLGPUglScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void SDLGPUglSecondaryColor3f(GLfloat red, GLfloat green, GLfloat blue);
void SDLGPUglShadeModel(GLenum mode);
void SDLGPUglStencilFunc(GLenum func, GLint ref, GLuint mask);
void SDLGPUglStencilMask(GLuint mask);
void SDLGPUglStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void SDLGPUglTexCoord2f(GLfloat s, GLfloat t);
void SDLGPUglTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void SDLGPUglTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void SDLGPUglTexEnvf(GLenum target, GLenum pname, GLfloat param);
void SDLGPUglTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void SDLGPUglTexEnvi(GLenum target, GLenum pname, GLint param);
void SDLGPUglTexGeni(GLenum coord, GLenum pname, GLint param);
void SDLGPUglTexImage2D(GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *pixels);
void SDLGPUglTexImage3D(GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLsizei depth, GLint border,
                         GLenum format, GLenum type, const void *pixels);
void SDLGPUglTexParameterf(GLenum target, GLenum pname, GLfloat param);
void SDLGPUglTexParameteri(GLenum target, GLenum pname, GLint param);
void SDLGPUglTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                            GLsizei width, GLsizei height, GLenum format,
                            GLenum type, const void *pixels);
void SDLGPUglTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                            GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                            GLenum format, GLenum type, const void *pixels);
void SDLGPUglVertex2f(GLfloat x, GLfloat y);
void SDLGPUglVertex3f(GLfloat x, GLfloat y, GLfloat z);
void SDLGPUglVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void SDLGPUglVertex4fv(const GLfloat *v);
void SDLGPUglVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
void SDLGPUglViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void SDLGPUglWindowPos2i(GLint x, GLint y);

#ifdef __cplusplus
}
#endif

typedef void (APIENTRY *SDLGPU_PFN_GEN_FRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *SDLGPU_PFN_DELETE_FRAMEBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *SDLGPU_PFN_BIND_FRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *SDLGPU_PFN_FRAMEBUFFER_TEXTURE_2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *SDLGPU_PFN_GEN_RENDERBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *SDLGPU_PFN_DELETE_RENDERBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *SDLGPU_PFN_BIND_RENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *SDLGPU_PFN_RENDERBUFFER_STORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *SDLGPU_PFN_FRAMEBUFFER_RENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *SDLGPU_PFN_CHECK_FRAMEBUFFER_STATUS)(GLenum);
typedef void (APIENTRY *SDLGPU_PFN_GENERATE_MIPMAP)(GLenum);
typedef void (APIENTRY *SDLGPU_PFN_ACTIVE_TEXTURE)(GLenum);
typedef void (APIENTRY *SDLGPU_PFN_CLIENT_ACTIVE_TEXTURE)(GLenum);
typedef void (APIENTRY *SDLGPU_PFN_MULTI_TEX_COORD_2F)(GLenum, GLfloat, GLfloat);
typedef void (APIENTRY *SDLGPU_PFN_MULTI_TEX_COORD_4F)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *SDLGPU_PFN_SECONDARY_COLOR_3F)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *SDLGPU_PFN_BLEND_FUNC_SEPARATE)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *SDLGPU_PFN_BLEND_COLOR)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *SDLGPU_PFN_BLEND_EQUATION)(GLenum);
typedef void (APIENTRY *SDLGPU_PFN_FOG_COORD_F)(GLfloat);

struct GfxGLExt {
	SDLGPU_PFN_GEN_FRAMEBUFFERS GenFramebuffers;
	SDLGPU_PFN_DELETE_FRAMEBUFFERS DeleteFramebuffers;
	SDLGPU_PFN_BIND_FRAMEBUFFER BindFramebuffer;
	SDLGPU_PFN_FRAMEBUFFER_TEXTURE_2D FramebufferTexture2D;
	SDLGPU_PFN_GEN_RENDERBUFFERS GenRenderbuffers;
	SDLGPU_PFN_DELETE_RENDERBUFFERS DeleteRenderbuffers;
	SDLGPU_PFN_BIND_RENDERBUFFER BindRenderbuffer;
	SDLGPU_PFN_RENDERBUFFER_STORAGE RenderbufferStorage;
	SDLGPU_PFN_FRAMEBUFFER_RENDERBUFFER FramebufferRenderbuffer;
	SDLGPU_PFN_CHECK_FRAMEBUFFER_STATUS CheckFramebufferStatus;
	SDLGPU_PFN_GENERATE_MIPMAP GenerateMipmap;
	SDLGPU_PFN_ACTIVE_TEXTURE ActiveTexture;
	SDLGPU_PFN_CLIENT_ACTIVE_TEXTURE ClientActiveTexture;
	SDLGPU_PFN_MULTI_TEX_COORD_2F MultiTexCoord2f;
	SDLGPU_PFN_MULTI_TEX_COORD_4F MultiTexCoord4f;
	SDLGPU_PFN_SECONDARY_COLOR_3F SecondaryColor3f;
	SDLGPU_PFN_BLEND_FUNC_SEPARATE BlendFuncSeparate;
	SDLGPU_PFN_BLEND_COLOR BlendColor;
	SDLGPU_PFN_BLEND_EQUATION BlendEquation;
	SDLGPU_PFN_FOG_COORD_F FogCoordf;
	bool fbo;
	bool multitex;
};

inline GfxGLExt &gfx_gl_ext()
{
	static GfxGLExt ext = {
		SDLGPUglGenFramebuffers, SDLGPUglDeleteFramebuffers,
		SDLGPUglBindFramebuffer, SDLGPUglFramebufferTexture2D,
		SDLGPUglGenRenderbuffers, SDLGPUglDeleteRenderbuffers,
		SDLGPUglBindRenderbuffer, SDLGPUglRenderbufferStorage,
		SDLGPUglFramebufferRenderbuffer, SDLGPUglCheckFramebufferStatus,
		SDLGPUglGenerateMipmap, SDLGPUglActiveTexture,
		SDLGPUglClientActiveTexture, SDLGPUglMultiTexCoord2f,
		SDLGPUglMultiTexCoord4f, SDLGPUglSecondaryColor3f,
		SDLGPUglBlendFuncSeparate, SDLGPUglBlendColor,
		SDLGPUglBlendEquation, SDLGPUglFogCoordf, true, true
	};
	return ext;
}

#define GFXACCEL_GL_EXT_H 1

#define SDL_GL_CreateContext SDLGPUCreateContext
#define SDL_GL_DestroyContext SDLGPUDestroyContext
#define SDL_GL_GetCurrentContext SDLGPUGetCurrentContext
#define SDL_GL_MakeCurrent SDLGPUMakeCurrent
#define SDL_GL_SetAttribute SDLGPUSetAttribute
#define SDL_GL_SetSwapInterval SDLGPUSetSwapInterval
#define SDL_GL_SwapWindow SDLGPUSwapWindow
#define SDL_GL_GetProcAddress SDLGPUGetProcAddress

#define glAccum SDLGPUglAccum
#define glActiveTexture SDLGPUglActiveTexture
#define glAlphaFunc SDLGPUglAlphaFunc
#define glBegin SDLGPUglBegin
#define glBindFramebuffer SDLGPUglBindFramebuffer
#define glBindRenderbuffer SDLGPUglBindRenderbuffer
#define glBindTexture SDLGPUglBindTexture
#define glBitmap SDLGPUglBitmap
#define glBlendColor SDLGPUglBlendColor
#define glBlendEquation SDLGPUglBlendEquation
#define glBlendFunc SDLGPUglBlendFunc
#define glBlendFuncSeparate SDLGPUglBlendFuncSeparate
#define glCheckFramebufferStatus SDLGPUglCheckFramebufferStatus
#define glClear SDLGPUglClear
#define glClearColor SDLGPUglClearColor
#define glClearDepth SDLGPUglClearDepth
#define glClearStencil SDLGPUglClearStencil
#define glClientActiveTexture SDLGPUglClientActiveTexture
#define glClipPlane SDLGPUglClipPlane
#define glColor4f SDLGPUglColor4f
#define glColor4fv SDLGPUglColor4fv
#define glColorMask SDLGPUglColorMask
#define glColorMaterial SDLGPUglColorMaterial
#define glCopyTexSubImage2D SDLGPUglCopyTexSubImage2D
#define glCullFace SDLGPUglCullFace
#define glDeleteFramebuffers SDLGPUglDeleteFramebuffers
#define glDeleteRenderbuffers SDLGPUglDeleteRenderbuffers
#define glDeleteTextures SDLGPUglDeleteTextures
#define glDepthFunc SDLGPUglDepthFunc
#define glDepthMask SDLGPUglDepthMask
#define glDepthRange SDLGPUglDepthRange
#define glDisable SDLGPUglDisable
#define glDrawBuffer SDLGPUglDrawBuffer
#define glDrawPixels SDLGPUglDrawPixels
#define glEnable SDLGPUglEnable
#define glEnd SDLGPUglEnd
#define glFinish SDLGPUglFinish
#define glFlush SDLGPUglFlush
#define glFogCoordf SDLGPUglFogCoordf
#define glFogf SDLGPUglFogf
#define glFogfv SDLGPUglFogfv
#define glFogi SDLGPUglFogi
#define glFramebufferRenderbuffer SDLGPUglFramebufferRenderbuffer
#define glFramebufferTexture2D SDLGPUglFramebufferTexture2D
#define glFrontFace SDLGPUglFrontFace
#define glGenerateMipmap SDLGPUglGenerateMipmap
#define glGenFramebuffers SDLGPUglGenFramebuffers
#define glGenRenderbuffers SDLGPUglGenRenderbuffers
#define glGenTextures SDLGPUglGenTextures
#define glGetError SDLGPUglGetError
#define glGetFloatv SDLGPUglGetFloatv
#define glGetIntegerv SDLGPUglGetIntegerv
#define glGetString SDLGPUglGetString
#define glGetTexEnviv SDLGPUglGetTexEnviv
#define glGetTexLevelParameteriv SDLGPUglGetTexLevelParameteriv
#define glGetTexParameteriv SDLGPUglGetTexParameteriv
#define glIsEnabled SDLGPUglIsEnabled
#define glLightf SDLGPUglLightf
#define glLightfv SDLGPUglLightfv
#define glLightModelfv SDLGPUglLightModelfv
#define glLightModeli SDLGPUglLightModeli
#define glLineWidth SDLGPUglLineWidth
#define glLoadIdentity SDLGPUglLoadIdentity
#define glLoadMatrixf SDLGPUglLoadMatrixf
#define glLogicOp SDLGPUglLogicOp
#define glMaterialf SDLGPUglMaterialf
#define glMaterialfv SDLGPUglMaterialfv
#define glMatrixMode SDLGPUglMatrixMode
#define glMultiTexCoord2f SDLGPUglMultiTexCoord2f
#define glMultiTexCoord4f SDLGPUglMultiTexCoord4f
#define glNormal3f SDLGPUglNormal3f
#define glNormal3fv SDLGPUglNormal3fv
#define glOrtho SDLGPUglOrtho
#define glPixelStorei SDLGPUglPixelStorei
#define glPointSize SDLGPUglPointSize
#define glPolygonMode SDLGPUglPolygonMode
#define glPolygonOffset SDLGPUglPolygonOffset
#define glPopAttrib SDLGPUglPopAttrib
#define glPopMatrix SDLGPUglPopMatrix
#define glPushAttrib SDLGPUglPushAttrib
#define glPushMatrix SDLGPUglPushMatrix
#define glRasterPos2i SDLGPUglRasterPos2i
#define glReadBuffer SDLGPUglReadBuffer
#define glReadPixels SDLGPUglReadPixels
#define glRenderbufferStorage SDLGPUglRenderbufferStorage
#define glScalef SDLGPUglScalef
#define glScissor SDLGPUglScissor
#define glSecondaryColor3f SDLGPUglSecondaryColor3f
#define glShadeModel SDLGPUglShadeModel
#define glStencilFunc SDLGPUglStencilFunc
#define glStencilMask SDLGPUglStencilMask
#define glStencilOp SDLGPUglStencilOp
#define glTexCoord2f SDLGPUglTexCoord2f
#define glTexCoord3f SDLGPUglTexCoord3f
#define glTexCoord4f SDLGPUglTexCoord4f
#define glTexEnvf SDLGPUglTexEnvf
#define glTexEnvfv SDLGPUglTexEnvfv
#define glTexEnvi SDLGPUglTexEnvi
#define glTexGeni SDLGPUglTexGeni
#define glTexImage2D SDLGPUglTexImage2D
#define glTexImage3D SDLGPUglTexImage3D
#define glTexParameterf SDLGPUglTexParameterf
#define glTexParameteri SDLGPUglTexParameteri
#define glTexSubImage2D SDLGPUglTexSubImage2D
#define glTexSubImage3D SDLGPUglTexSubImage3D
#define glVertex2f SDLGPUglVertex2f
#define glVertex3f SDLGPUglVertex3f
#define glVertex4f SDLGPUglVertex4f
#define glVertex4fv SDLGPUglVertex4fv
#define glVertexPointer SDLGPUglVertexPointer
#define glViewport SDLGPUglViewport
#define glWindowPos2i SDLGPUglWindowPos2i

#endif
