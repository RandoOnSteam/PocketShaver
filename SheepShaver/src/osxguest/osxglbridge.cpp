/*
 *  osxglbridge.cpp - host OpenGL bridge for Mac OS X guest Flurry and Quartz OpenGL contexts
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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
#include "sysdeps.h"
#include "osxcpu.h"
#include "osxmachine.h"
#include "osxglbridge.h"
#if OSXGLBRIDGE
#ifdef _WIN32
#include <windows.h>
#endif
#define GLCONTEXT_INCLUDEOPENGLHEADERS
#include "../gfxaccel/gl/glcontext.h"
#ifndef APIENTRY
#define APIENTRY
#endif

#define OSXGLFRAMEBUFFER 0x8d40
#define OSXGLCOLORATTACHMENT 0x8ce0
#define OSXGLFRAMEBUFFERCOMPLETE 0x8cd5

typedef void (APIENTRY *OsxGlGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *OsxGlBindFramebuffer)(GLenum, GLuint);
typedef GLenum (APIENTRY *OsxGlCheckFramebuffer)(GLenum);
typedef void (APIENTRY *OsxGlRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *OsxGlFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *OsxGlFramebufferTexture)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *OsxGlBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void *(APIENTRY *OsxGlMapBuffer)(GLenum, GLenum);
typedef GLboolean (APIENTRY *OsxGlUnmapBuffer)(GLenum);
typedef void (APIENTRY *OsxGlBlendSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *OsxGlSingleArgument)(GLenum);
typedef void (APIENTRY *OsxGlMultiCoordinate)(GLenum, GLfloat, GLfloat);
typedef void (APIENTRY *OsxGlMultiVector)(GLenum, const GLfloat *);
typedef void (APIENTRY *OsxGlBindProgram)(GLenum, GLuint);
typedef void (APIENTRY *OsxGlProgramNames)(GLsizei, GLuint *);
typedef GLboolean (APIENTRY *OsxGlIsProgram)(GLuint);
typedef void (APIENTRY *OsxGlProgramDoubles)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (APIENTRY *OsxGlProgramFloats)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *OsxGlProgramVector)(GLenum, GLuint, void *);
typedef void (APIENTRY *OsxGlProgramString)(GLenum, GLenum, GLsizei, const void *);
typedef void (APIENTRY *OsxGlProgramQuery)(GLenum, GLenum, void *);

#define OSXGLPROGRAMS 19

typedef struct OsxGlArray {
	uint32 address;
	uint32 size;
	uint32 type;
	uint32 stride;
	int enabled;
} OsxGlArray;

#define OSXGLCACHESIZE 32

typedef struct OsxGlCacheEntry {
	uint32 call;
	uint32 name;
	uint32 length;
	uint8 values[128];
} OsxGlCacheEntry;

struct OsxGlBridge {
	GLCONTEXT context;
	int contextready;
	uint8 *pixels;
	uint8 *upload;
	uint32 uploadsize;
	uint32 width;
	uint32 height;
	uint64 lastframe;
	unsigned int frames;
	unsigned int captured;
	unsigned int presented;
	uint64 firstpresent;
	OsxGlArray arrays[3];
	uint32 framebuffer;
	uint32 pbufferframebuffer;
	uint32 pbufferdepth;
	uint32 pbufferdepthwidth;
	uint32 pbufferdepthheight;
	uint32 readframebuffer;
	OsxGlGenFramebuffers genframebuffers;
	OsxGlBindFramebuffer bindframebuffer;
	OsxGlGenFramebuffers genrenderbuffers;
	OsxGlBindFramebuffer bindrenderbuffer;
	OsxGlRenderbufferStorage renderbufferstorage;
	OsxGlFramebufferRenderbuffer framebufferrenderbuffer;
	OsxGlFramebufferTexture framebuffertexture;
	GLuint packbuffers[2];
	int packindex;
	int packready;
	OsxGlGenFramebuffers genbuffers;
	OsxGlBindFramebuffer bindbuffer;
	OsxGlBufferData bufferdata;
	OsxGlMapBuffer mapbuffer;
	OsxGlUnmapBuffer unmapbuffer;
	uint32 unpackrowlength;
	uint32 unpackrowbytes;
	uint32 unpackclientstorage;
	uint32 unpackalignment;
	uint32 unpackskippixels;
	uint32 unpackskiprows;
	uint32 packrowlength;
	uint32 packalignment;
	OsxGlBlendSeparate blendseparate;
	OsxGlSingleArgument blendequation;
	OsxGlSingleArgument clientactivetexture;
	OsxGlSingleArgument activetexture;
	OsxGlMultiCoordinate multicoordinate;
	OsxGlMultiVector multicoordinatevector;
	GLContextEmptyProc programs[OSXGLPROGRAMS];
	OsxGlCacheEntry cache[OSXGLCACHESIZE];
	uint32 cachecount;
	int errorpending;
};

void DestroyGlBridge(OsxGlBridge *bridge)
{
	if (bridge->contextready)
		GLContext_Destroy(&bridge->context);
	free(bridge->pixels);
	free(bridge->upload);
	free(bridge);
}

static OsxGlBridge *CreateGlBridgeShared(uint32 width, uint32 height, OsxGlBridge *share, FILE *log)
{
	static const char *const programnames[OSXGLPROGRAMS] = {
		"glBindProgramARB", "glDeleteProgramsARB", "glGenProgramsARB", "glIsProgramARB",
		"glProgramEnvParameter4dARB", "glProgramEnvParameter4dvARB", "glProgramEnvParameter4fARB",
		"glProgramEnvParameter4fvARB", "glProgramLocalParameter4dARB", "glProgramLocalParameter4dvARB",
		"glProgramLocalParameter4fARB", "glProgramLocalParameter4fvARB", "glGetProgramEnvParameterdvARB",
		"glGetProgramEnvParameterfvARB", "glGetProgramLocalParameterdvARB", "glGetProgramLocalParameterfvARB",
		"glProgramStringARB", "glGetProgramStringARB", "glGetProgramivARB"
	};
	OsxGlBridge *bridge;
	GLPIXELATTRIBUTES pixelattributes;
	int program;
	OsxGlGenFramebuffers genframebuffers;
	OsxGlBindFramebuffer bindframebuffer;
	OsxGlCheckFramebuffer checkframebuffer;
	OsxGlGenFramebuffers genrenderbuffers;
	OsxGlBindFramebuffer bindrenderbuffer;
	OsxGlRenderbufferStorage renderbufferstorage;
	OsxGlFramebufferRenderbuffer framebufferrenderbuffer;
	GLuint framebuffer;
	GLuint color;
	GLuint depth;
	GLenum status;
	bridge = (OsxGlBridge *)calloc(1, sizeof(*bridge));
	if (!bridge)
		return NULL;
	bridge->width = width;
	bridge->height = height;
	bridge->unpackalignment = 4;
	bridge->packalignment = 4;
	GLPixelAttributes_Create(&pixelattributes);
	GLPixelAttributes_PlatformDefaults(&pixelattributes);
	GLPixelAttributes_RGBA(&pixelattributes);
	GLPixelAttributes_EndList(&pixelattributes);
	if (share)
		bridge->contextready = GLContext_CreateCloneWithWindow(&bridge->context,
			GLContext_GetHandle(&share->context), 0, NULL, &pixelattributes);
	else
		bridge->contextready = GLContext_CreateWithWindow(&bridge->context, 0, NULL, &pixelattributes);
	if (!bridge->contextready || !GLContext_SetCurrent(&bridge->context)) {
		if (log)
			fprintf(log, "quartz GL context creation failed ready %d\n", bridge->contextready);
		DestroyGlBridge(bridge);
		return NULL;
	}
	if (log)
		fprintf(log, "quartz GL vendor %s renderer %s version %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
	bridge->blendseparate = (OsxGlBlendSeparate)GLContext_GetProc(&bridge->context, "glBlendFuncSeparate");
	bridge->blendequation = (OsxGlSingleArgument)GLContext_GetProc(&bridge->context, "glBlendEquation");
	bridge->clientactivetexture = (OsxGlSingleArgument)GLContext_GetProc(&bridge->context, "glClientActiveTexture");
	bridge->activetexture = (OsxGlSingleArgument)GLContext_GetProc(&bridge->context, "glActiveTexture");
	bridge->multicoordinate = (OsxGlMultiCoordinate)GLContext_GetProc(&bridge->context, "glMultiTexCoord2f");
	bridge->multicoordinatevector = (OsxGlMultiVector)GLContext_GetProc(&bridge->context, "glMultiTexCoord4fv");
	for (program = 0; program < OSXGLPROGRAMS; program++)
		bridge->programs[program] = GLContext_GetProc(&bridge->context, programnames[program]);
	genframebuffers = (OsxGlGenFramebuffers)GLContext_GetProc(&bridge->context, "glGenFramebuffers");
	bindframebuffer = (OsxGlBindFramebuffer)GLContext_GetProc(&bridge->context, "glBindFramebuffer");
	checkframebuffer = (OsxGlCheckFramebuffer)GLContext_GetProc(&bridge->context, "glCheckFramebufferStatus");
	genrenderbuffers = (OsxGlGenFramebuffers)GLContext_GetProc(&bridge->context, "glGenRenderbuffers");
	bindrenderbuffer = (OsxGlBindFramebuffer)GLContext_GetProc(&bridge->context, "glBindRenderbuffer");
	renderbufferstorage = (OsxGlRenderbufferStorage)GLContext_GetProc(&bridge->context, "glRenderbufferStorage");
	framebufferrenderbuffer = (OsxGlFramebufferRenderbuffer)GLContext_GetProc(&bridge->context, "glFramebufferRenderbuffer");
	if (!genframebuffers || !bindframebuffer || !checkframebuffer ||
		!genrenderbuffers || !bindrenderbuffer || !renderbufferstorage || !framebufferrenderbuffer) {
		DestroyGlBridge(bridge);
		return NULL;
	}
	genframebuffers(1, &framebuffer);
	bridge->framebuffer = framebuffer;
	bindframebuffer(OSXGLFRAMEBUFFER, framebuffer);
	genrenderbuffers(1, &color);
	bindrenderbuffer(0x8d41, color);
	renderbufferstorage(0x8d41, GL_RGBA8, width, height);
	framebufferrenderbuffer(OSXGLFRAMEBUFFER, OSXGLCOLORATTACHMENT, 0x8d41, color);
	genrenderbuffers(1, &depth);
	bindrenderbuffer(0x8d41, depth);
	renderbufferstorage(0x8d41, 0x88f0, width, height);
	framebufferrenderbuffer(OSXGLFRAMEBUFFER, 0x821a, 0x8d41, depth);
	bridge->genframebuffers = genframebuffers;
	bridge->bindframebuffer = bindframebuffer;
	bridge->genrenderbuffers = genrenderbuffers;
	bridge->bindrenderbuffer = bindrenderbuffer;
	bridge->renderbufferstorage = renderbufferstorage;
	bridge->framebufferrenderbuffer = framebufferrenderbuffer;
	bridge->framebuffertexture = (OsxGlFramebufferTexture)GLContext_GetProc(&bridge->context, "glFramebufferTexture2D");
	bridge->genbuffers = (OsxGlGenFramebuffers)GLContext_GetProc(&bridge->context, "glGenBuffers");
	bridge->bindbuffer = (OsxGlBindFramebuffer)GLContext_GetProc(&bridge->context, "glBindBuffer");
	bridge->bufferdata = (OsxGlBufferData)GLContext_GetProc(&bridge->context, "glBufferData");
	bridge->mapbuffer = (OsxGlMapBuffer)GLContext_GetProc(&bridge->context, "glMapBuffer");
	bridge->unmapbuffer = (OsxGlUnmapBuffer)GLContext_GetProc(&bridge->context, "glUnmapBuffer");
	status = checkframebuffer(OSXGLFRAMEBUFFER);
	if (status != OSXGLFRAMEBUFFERCOMPLETE) {
		if (log)
			fprintf(log, "quartz GL framebuffer failed status %x error %x\n", status, glGetError());
		DestroyGlBridge(bridge);
		return NULL;
	}
	glViewport(0, 0, width, height);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	return bridge;
}

OsxGlBridge *CreateGlBridge(uint32 width, uint32 height)
{
	return CreateGlBridgeShared(width, height, NULL, NULL);
}

static double GlFloat(OsxCpu *cpu, int index)
{
	double value;
	memcpy(&value, &cpu->fpr[index], sizeof(value));
	return value;
}

static int GlGuestCopy(OsxCpu *cpu, uint8 *output, uint32 address, uint32 length)
{
	uint32 physical;
	uint32 count;
	while (length) {
		if (!OsxCpuTranslate(cpu, address, OSXTLBREAD, 1, &physical) || physical >= cpu->ramsize)
			return 0;
		count = 4096 - (address & 4095);
		if (count > length)
			count = length;
		if (count > cpu->ramsize - physical)
			return 0;
		memcpy(output, cpu->ram + physical, count);
		output += count;
		address += count;
		length -= count;
	}
	return 1;
}

static int GlGuestFloats(OsxCpu *cpu, float *output, uint32 address, uint32 length)
{
	uint32 index;
	uint32 value;
	if (!GlGuestCopy(cpu, (uint8 *)output, address, length * 4))
		return 0;
	for (index = 0; index < length; index++) {
		memcpy(&value, output + index, 4);
		value = bswap_32(value);
		memcpy(output + index, &value, 4);
	}
	return 1;
}

static uint8 *GlUpload(OsxGlBridge *bridge, uint32 length)
{
	uint8 *upload;
	if (length > bridge->uploadsize) {
		upload = (uint8 *)realloc(bridge->upload, length);
		if (!upload)
			return NULL;
		bridge->upload = upload;
		bridge->uploadsize = length;
	}
	return bridge->upload;
}

static void GlArray(OsxGlArray *array, uint32 size, uint32 type, uint32 stride, uint32 address)
{
	array->size = size;
	array->type = type;
	array->stride = stride;
	array->address = address;
}

static void GlDrawArrays(OsxGlBridge *bridge, OsxCpu *cpu, uint32 mode, uint32 first, uint32 count)
{
	OsxGlArray *array;
	float *values;
	uint32 index;
	uint32 vertex;
	uint32 stride;
	uint32 width;
	uint32 address;
	uint32 total;
	if (count > 16384 || !bridge->arrays[0].enabled)
		return;
	total = 0;
	for (index = 0; index < 3; index++) {
		array = &bridge->arrays[index];
		if (!array->enabled)
			continue;
		if (array->size < 2 || array->size > 4 || array->type != GL_FLOAT)
			return;
		total += count * array->size;
	}
	values = (float *)GlUpload(bridge, total * 4);
	if (!values)
		return;
	for (index = 0; index < 3; index++) {
		array = &bridge->arrays[index];
		if (!array->enabled)
			continue;
		width = array->size * 4;
		stride = array->stride;
		if (!stride)
			stride = width;
		address = array->address + first * stride;
		if (stride == width) {
			if (!GlGuestFloats(cpu, values, address, count * array->size))
				return;
		} else {
			for (vertex = 0; vertex < count; vertex++) {
				if (!GlGuestFloats(cpu, values + vertex * array->size, address + vertex * stride, array->size))
					return;
			}
		}
		if (index == 0)
			glVertexPointer(array->size, GL_FLOAT, 0, values);
		else if (index == 1)
			glColorPointer(array->size, GL_FLOAT, 0, values);
		else
			glTexCoordPointer(array->size, GL_FLOAT, 0, values);
		values += count * array->size;
	}
	glDrawArrays(mode, 0, count);
}

static int GlPixels(OsxGlBridge *bridge, OsxCpu *cpu, uint32 width, uint32 height, uint32 format, uint32 type, uint32 address)
{
	uint32 components;
	if (width > 1024 || height > 1024 || type != GL_UNSIGNED_BYTE)
		return 0;
	switch (format) {
	case GL_RGBA: components = 4; break;
	case GL_RGB: components = 3; break;
	case GL_LUMINANCE_ALPHA: components = 2; break;
	case GL_LUMINANCE:
	case GL_ALPHA: components = 1; break;
	default: return 0;
	}
	return GlUpload(bridge, width * height * components) &&
		GlGuestCopy(cpu, bridge->upload, address, width * height * components);
}

int GlBridgeCall(OsxGlBridge *bridge, OsxCpu *cpu, uint32 call)
{
	uint32 *a;
	GLuint texture;
	float color[4];
	if (!GLContext_IsCurrent(&bridge->context))
		GLContext_SetCurrent(&bridge->context);
	a = cpu->gpr + 3;
	switch (call) {
	case 45: glAlphaFunc(a[0], (float)GlFloat(cpu, 1)); break;
	case 46: glBegin(a[0]); break;
	case 47: glBindTexture(a[0], a[1]); break;
	case 48: glBlendFunc(a[0], a[1]); break;
	case 49: glClear(a[0]); break;
	case 50: glClearColor((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2), (float)GlFloat(cpu, 3), (float)GlFloat(cpu, 4)); break;
	case 51: glColor4f((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2), (float)GlFloat(cpu, 3), (float)GlFloat(cpu, 4)); break;
	case 52:
		if (GlGuestFloats(cpu, color, a[0], 4))
			glColor4fv(color);
		break;
	case 53: GlArray(&bridge->arrays[1], a[0], a[1], a[2], a[3]); break;
	case 54: glDisable(a[0]); break;
	case 55: GlDrawArrays(bridge, cpu, a[0], a[1], a[2]); break;
	case 56:
		if (GlPixels(bridge, cpu, a[0], a[1], a[2], a[3], a[4]))
			glDrawPixels(a[0], a[1], a[2], a[3], bridge->upload);
		break;
	case 57: glEnable(a[0]); break;
	case 58:
		if (a[0] == GL_VERTEX_ARRAY)
			bridge->arrays[0].enabled = 1;
		else if (a[0] == GL_COLOR_ARRAY)
			bridge->arrays[1].enabled = 1;
		else if (a[0] == GL_TEXTURE_COORD_ARRAY)
			bridge->arrays[2].enabled = 1;
		else
			break;
		glEnableClientState(a[0]);
		break;
	case 59: glEnd(); break;
	case OSXFLURRYFLUSH:
		glFlush();
		bridge->lastframe = OsxHostNanoseconds();
		bridge->frames++;
		break;
	case 61:
		if (a[0] == 1) {
			glGenTextures(1, &texture);
			OsxCpuWriteVirtual(cpu, a[1], texture);
		}
		break;
	case 62: a[0] = glGetError(); break;
	case 64: glLoadIdentity(); break;
	case 65: glMatrixMode(a[0]); break;
	case 66: break;
	case 67: glPixelZoom((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2)); break;
	case 68: glPopMatrix(); break;
	case 69: glPushMatrix(); break;
	case 70: glRasterPos2i(a[0], a[1]); break;
	case 71: glRectd(GlFloat(cpu, 1), GlFloat(cpu, 2), GlFloat(cpu, 3), GlFloat(cpu, 4)); break;
	case 72: glRotatef((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2), (float)GlFloat(cpu, 3), (float)GlFloat(cpu, 4)); break;
	case 73: glScalef((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2), (float)GlFloat(cpu, 3)); break;
	case 74: glScissor(a[0], a[1], a[2], a[3]); break;
	case 75: glShadeModel(a[0]); break;
	case 76: GlArray(&bridge->arrays[2], a[0], a[1], a[2], a[3]); break;
	case 77: glTexEnvf(a[0], a[1], (float)GlFloat(cpu, 1)); break;
	case 78: glTexParameteri(a[0], a[1], a[2]); break;
	case 79: glTranslatef((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2), (float)GlFloat(cpu, 3)); break;
	case 80: glVertex2f((float)GlFloat(cpu, 1), (float)GlFloat(cpu, 2)); break;
	case 81: GlArray(&bridge->arrays[0], a[0], a[1], a[2], a[3]); break;
	case 82: glViewport(a[0], a[1], a[2], a[3]); break;
	case 83:
		if (GlPixels(bridge, cpu, a[2], a[3], a[4], a[5], a[6])) {
			glTexParameteri(a[0], 0x8191, GL_TRUE);
			glTexImage2D(a[0], 0, a[1], a[2], a[3], 0, a[4], a[5], bridge->upload);
			a[0] = 0;
		} else
			a[0] = GL_INVALID_VALUE;
		break;
	case 85: glOrtho(GlFloat(cpu, 1), GlFloat(cpu, 2), GlFloat(cpu, 3), GlFloat(cpu, 4), -1.0, 1.0); break;
	default: return 0;
	}
	return 1;
}

static void GlBridgeCapture(OsxGlBridge *bridge)
{
	uint8 *mapped;
	uint32 size;
	int index;
	size = bridge->width * bridge->height * 4;
	if (!bridge->pixels)
		bridge->pixels = (uint8 *)malloc(size);
	if (!bridge->pixels)
		return;
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (!bridge->packbuffers[0] && bridge->genbuffers && bridge->bindbuffer && bridge->bufferdata &&
		bridge->mapbuffer && bridge->unmapbuffer) {
		bridge->genbuffers(2, bridge->packbuffers);
		for (index = 0; index < 2; index++) {
			bridge->bindbuffer(0x88eb, bridge->packbuffers[index]);
			bridge->bufferdata(0x88eb, size, NULL, 0x88e1);
		}
		bridge->bindbuffer(0x88eb, 0);
	}
	if (!bridge->packbuffers[0]) {
		glReadPixels(0, 0, bridge->width, bridge->height, 0x80e1, 0x8035, bridge->pixels);
		bridge->captured = bridge->frames;
	} else {
		bridge->bindbuffer(0x88eb, bridge->packbuffers[bridge->packindex]);
		glReadPixels(0, 0, bridge->width, bridge->height, 0x80e1, 0x8035, NULL);
		if (bridge->packready) {
			bridge->bindbuffer(0x88eb, bridge->packbuffers[bridge->packindex ^ 1]);
			mapped = (uint8 *)bridge->mapbuffer(0x88eb, 0x88b8);
			if (mapped) {
				memcpy(bridge->pixels, mapped, size);
				bridge->unmapbuffer(0x88eb);
				bridge->captured = bridge->frames;
			}
		}
		bridge->bindbuffer(0x88eb, 0);
		bridge->packready = 1;
		bridge->packindex ^= 1;
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, bridge->packrowlength);
	glPixelStorei(GL_PACK_ALIGNMENT, bridge->packalignment);
}

const uint8 *GlBridgeFrame(OsxGlBridge *bridge)
{
	if (!bridge->frames || OsxHostNanoseconds() - bridge->lastframe >= 500000000)
		return NULL;
	if (bridge->captured != bridge->frames) {
		if (!GLContext_IsCurrent(&bridge->context))
			GLContext_SetCurrent(&bridge->context);
		GlBridgeCapture(bridge);
	}
	if (bridge->captured != bridge->frames)
		return NULL;
	return bridge->pixels;
}

void GlBridgePresented(OsxGlBridge *bridge, FILE *log)
{
	uint64 now;
	now = OsxHostNanoseconds();
	if (!bridge->firstpresent)
		bridge->firstpresent = now;
	bridge->presented++;
	if (log && bridge->presented % 120 == 0)
		fprintf(log, "flurry presented %u distinct frames in %.3f seconds\n", bridge->presented,
			(now - bridge->firstpresent) / 1000000000.0);
}
typedef struct OsxQuartzPbuffer {
	struct OsxQuartzPbuffer *next;
	uint32 address;
	uint32 segment;
	uint32 width;
	uint32 height;
	GLuint texture;
} OsxQuartzPbuffer;

typedef struct OsxQuartzContext {
	struct OsxQuartzContext *next;
	OsxGlBridge *gl;
	uint32 strings;
	int extensions;
	uint32 address;
	uint32 engine;
	uint32 segment;
	OsxQuartzPbuffer *pbuffer;
	uint32 original[OSXQUARTZSLOTS];
	uint8 calls[95];
	uint32 viewport[4];
} OsxQuartzContext;

struct OsxQuartzBridge {
	OsxQuartzContext *contexts;
	OsxQuartzPbuffer *pbuffers;
	uint32 width;
	uint32 height;
	OsxQuartzContext *present;
	uint32 library;
	uint32 strings;
	uint32 processsegment;
	int processsaver;
	int extensions;
	int drawableflush;
	int held;
	uint64 heldsum;
	int fresh;
};

static uint32 GlArgument(OsxCpu *cpu, uint32 index)
{
	uint32 value;
	index++;
	if (index < 8)
		return cpu->gpr[3 + index];
	value = 0;
	OsxCpuReadVirtual(cpu, cpu->gpr[1] + 24 + index * 4, &value);
	return value;
}

static int GlGuestData(OsxCpu *cpu, uint8 *output, uint32 address, uint32 length, uint32 element)
{
	uint32 index;
	uint32 byte;
	uint8 value;
	if (!GlGuestCopy(cpu, output, address, length))
		return 0;
	for (index = 0; index < length; index += element) {
		for (byte = 0; byte < element / 2; byte++) {
			value = output[index + byte];
			output[index + byte] = output[index + element - byte - 1];
			output[index + element - byte - 1] = value;
		}
	}
	return 1;
}

static int GlGuestWrite(OsxCpu *cpu, uint32 address, const uint8 *input, uint32 length, uint32 element)
{
	uint32 index;
	uint32 byte;
	uint32 physical;
	for (index = 0; index < length; index += element) {
		for (byte = 0; byte < element; byte++) {
			if (!OsxCpuTranslate(cpu, address + index + byte, OSXTLBWRITE, 1, &physical) || physical >= cpu->ramsize)
				return 0;
			cpu->ram[physical] = input[index + element - byte - 1];
		}
	}
	return 1;
}

static uint32 GlStateCount(uint32 name)
{
	switch (name) {
	case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX:
		return 16;
	case GL_CURRENT_COLOR: case GL_CURRENT_TEXTURE_COORDS: case GL_CURRENT_RASTER_COLOR:
	case GL_CURRENT_RASTER_POSITION: case GL_CURRENT_RASTER_TEXTURE_COORDS:
	case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_CLEAR_VALUE: case GL_COLOR_WRITEMASK:
	case GL_FOG_COLOR: case GL_LIGHT_MODEL_AMBIENT: case GL_AMBIENT: case GL_DIFFUSE:
	case GL_SPECULAR: case GL_POSITION: case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
	case GL_TEXTURE_BORDER_COLOR: case GL_TEXTURE_ENV_COLOR:
		return 4;
	case GL_CURRENT_NORMAL: case GL_SPOT_DIRECTION:
		return 3;
	case GL_DEPTH_RANGE: case GL_MAX_VIEWPORT_DIMS: case GL_POLYGON_MODE:
	case GL_LINE_WIDTH_RANGE: case GL_POINT_SIZE_RANGE:
		return 2;
	}
	return 1;
}

static int GlPixelLayout(uint32 format, uint32 type, uint32 *components, uint32 *element)
{
	*components = 4;
	if (format == GL_RGB || format == 0x80e0)
		*components = 3;
	else if (format == GL_LUMINANCE_ALPHA)
		*components = 2;
	else if (format == GL_ALPHA || format == GL_LUMINANCE || format == GL_RED || format == GL_DEPTH_COMPONENT)
		*components = 1;
	else if (format != GL_RGBA && format != 0x80e1)
		return 0;
	*element = 1;
	if (type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT)
		*element = 4;
	else if (type == GL_SHORT || type == GL_UNSIGNED_SHORT || type == 0x140b)
		*element = 2;
	else if (type == 0x8035 || type == 0x8367 || type == 0x8036 || type == 0x8368) {
		*element = 4;
		*components = 1;
	} else if (type == 0x8033 || type == 0x8034 || (type >= 0x8363 && type <= 0x8366)) {
		*element = 2;
		*components = 1;
	} else if (type == 0x8032 || type == 0x8362)
		*components = 1;
	else if (type != GL_UNSIGNED_BYTE && type != GL_BYTE)
		return 0;
	return 1;
}

static uint32 GlPixelStride(uint32 width, uint32 rowlength, uint32 alignment, uint32 pixel)
{
	uint32 stride;
	if (rowlength)
		width = rowlength;
	stride = width * pixel;
	return (stride + alignment - 1) & ~(alignment - 1);
}

static int GlQuartzPixels(OsxGlBridge *bridge, OsxCpu *cpu, uint32 width, uint32 height,
	uint32 format, uint32 type, uint32 address)
{
	uint32 components;
	uint32 element;
	uint32 pixel;
	uint32 row;
	uint32 stride;
	uint32 length;
	if (!GlPixelLayout(format, type, &components, &element))
		return 0;
	if (bridge->unpackrowlength > 4096 || width > 4096 || height > 4096 || !height)
		return 0;
	pixel = components * element;
	stride = GlPixelStride(width, bridge->unpackrowlength, bridge->unpackalignment, pixel);
	if (bridge->unpackrowbytes)
		stride = bridge->unpackrowbytes;
	if (stride < width * pixel || stride > 65536)
		return 0;
	address += bridge->unpackskiprows * stride + bridge->unpackskippixels * pixel;
	length = stride * (height - 1) + width * pixel;
	if (length > 4 * 1024 * 1024 || !GlUpload(bridge, height * width * pixel))
		return 0;
	for (row = 0; row < height; row++) {
		if (!GlGuestData(cpu, bridge->upload + row * width * pixel, address + row * stride, width * pixel, element))
			return 0;
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	return 1;
}

static int GlCacheLoad(OsxGlBridge *bridge, uint32 call, uint32 name, uint8 *values, uint32 length)
{
	uint32 index;
	for (index = 0; index < bridge->cachecount; index++) {
		if (bridge->cache[index].call == call && bridge->cache[index].name == name &&
			bridge->cache[index].length == length) {
			memcpy(values, bridge->cache[index].values, length);
			return 1;
		}
	}
	return 0;
}

static void GlCacheStore(OsxGlBridge *bridge, uint32 call, uint32 name, const uint8 *values, uint32 length)
{
	OsxGlCacheEntry *entry;
	if (length > sizeof(entry->values))
		return;
	if (call != 144) {
		switch (name) {
		case 0x0ba0: case 0x0ba2: case 0x0ba3: case 0x0ba4: case 0x0ba5: case 0x0ba6: case 0x0ba7: case 0x0ba8:
		case 0x0b46: case 0x0be0: case 0x0be1: case 0x0c10: case 0x0c22: case 0x0d33: case 0x0d3a:
		case 0x0be2: case 0x8009: case 0x8069: case 0x84e0: case 0x84e2: case 0x84f6: case 0x84f8:
		case 0x8871: case 0x8872:
			break;
		default:
			return;
		}
	}
	if (bridge->cachecount == OSXGLCACHESIZE)
		bridge->cachecount = 0;
	entry = &bridge->cache[bridge->cachecount++];
	entry->call = call;
	entry->name = name;
	entry->length = length;
	memcpy(entry->values, values, length);
}

static void GlCacheForget(OsxGlBridge *bridge, uint32 first, uint32 last)
{
	uint32 index;
	index = 0;
	while (index < bridge->cachecount) {
		if (bridge->cache[index].name >= first && bridge->cache[index].name <= last)
			bridge->cache[index] = bridge->cache[--bridge->cachecount];
		else
			index++;
	}
}

static void GlCacheUpdate(OsxGlBridge *bridge, uint32 call, uint32 name)
{
	if (!bridge->cachecount)
		return;
	switch (call) {
	case 9: case 10: case 204: case 205:
		bridge->cachecount = 0;
		break;
	case 175:
		GlCacheForget(bridge, 0x0ba0, 0x0ba0);
		break;
	case 97: case 158: case 159: case 160: case 176: case 177: case 190: case 206: case 211:
	case 248: case 249: case 250: case 251: case 309: case 310:
		GlCacheForget(bridge, 0x0ba3, 0x0ba8);
		break;
	case 343:
		GlCacheForget(bridge, 0x84e0, 0x84e0);
		GlCacheForget(bridge, 0x0ba5, 0x0ba5);
		GlCacheForget(bridge, 0x0ba8, 0x0ba8);
		GlCacheForget(bridge, 0x0de0, 0x0de1);
		GlCacheForget(bridge, 0x8513, 0x8513);
		GlCacheForget(bridge, 0x84f5, 0x84f5);
		GlCacheForget(bridge, 0x8069, 0x8069);
		GlCacheForget(bridge, 0x84f6, 0x84f6);
		break;
	case 6: case 60:
		GlCacheForget(bridge, 0x8069, 0x8069);
		GlCacheForget(bridge, 0x84f6, 0x84f6);
		break;
	case 64: case 65: case 73: case 74:
		GlCacheForget(bridge, name, name);
		break;
	case 8: case 337:
		GlCacheForget(bridge, 0x0be0, 0x0be1);
		break;
	case 339:
		GlCacheForget(bridge, 0x8009, 0x8009);
		break;
	case 13:
		GlCacheForget(bridge, 0x0c22, 0x0c22);
		break;
	case 96:
		GlCacheForget(bridge, 0x0b46, 0x0b46);
		break;
	case 252:
		GlCacheForget(bridge, 0x0c10, 0x0c10);
		break;
	case 336:
		GlCacheForget(bridge, 0x0ba2, 0x0ba2);
		break;
	}
}

static int QuartzGlCall(OsxQuartzContext *context, OsxCpu *cpu, uint32 call)
{
	OsxGlBridge *bridge;
	uint32 arguments[10];
	uint32 index;
	uint32 count;
	uint32 element;
	uint32 state;
	int program;
	GLdouble statevalue;
	GLfloat statefloat;
	uint8 values[2048];
	bridge = context->gl;
	for (index = 0; index < 7; index++)
		arguments[index] = cpu->gpr[4 + index];
	if (call == 302 || call == 308) {
		arguments[7] = GlArgument(cpu, 7);
		arguments[8] = GlArgument(cpu, 8);
	}
	GlCacheUpdate(bridge, call, arguments[0]);
	program = -1;
	if (call >= 472 && call <= 475)
		program = call - 472;
	else if (call >= 519 && call <= 533)
		program = call - 515;
	if (program >= 0 && !bridge->programs[program])
		return 0;
	switch (call) {
	case 100: case 102: case 103: case 104: case 105: case 144:
		break;
	default:
		bridge->errorpending = 1;
		break;
	}
	switch (call) {
#include "osxquartzcalls.h"
	case 52: GlArray(&bridge->arrays[1], arguments[0], arguments[1], arguments[2], arguments[3]); break;
	case 290: GlArray(&bridge->arrays[2], arguments[0], arguments[1], arguments[2], arguments[3]); break;
	case 335: GlArray(&bridge->arrays[0], arguments[0], arguments[1], arguments[2], arguments[3]); break;
	case 65: case 74:
		index = 3;
		if (arguments[0] == GL_VERTEX_ARRAY)
			index = 0;
		else if (arguments[0] == GL_COLOR_ARRAY)
			index = 1;
		else if (arguments[0] == GL_TEXTURE_COORD_ARRAY)
			index = 2;
		if (index < 3)
			bridge->arrays[index].enabled = call == 74;
		if (call == 74)
			glEnableClientState(arguments[0]);
		else
			glDisableClientState(arguments[0]);
		break;
	case 66: GlDrawArrays(bridge, cpu, arguments[0], arguments[1], arguments[2]); break;
	case 67:
		if (arguments[0] == GL_FRONT || arguments[0] == GL_BACK || arguments[0] == GL_FRONT_AND_BACK)
			arguments[0] = OSXGLCOLORATTACHMENT;
		glDrawBuffer(arguments[0]);
		break;
	case 237:
		if (arguments[0] == GL_FRONT || arguments[0] == GL_BACK)
			arguments[0] = OSXGLCOLORATTACHMENT;
		glReadBuffer(arguments[0]);
		break;
	case 90: case 590: glFinish(); break;
	case 91: case 589:
		glFlush();
		break;
	case 591:
		break;
	case 60: case 99:
		count = arguments[0];
		if (!count)
			break;
		if (count > 1048576 || !GlUpload(bridge, count * 4))
			return 0;
		if (call == 99) {
			glGenTextures(count, (GLuint *)bridge->upload);
			GlGuestWrite(cpu, arguments[1], bridge->upload, count * 4, 4);
		} else if (GlGuestData(cpu, bridge->upload, arguments[1], count * 4, 4))
			glDeleteTextures(count, (const GLuint *)bridge->upload);
		break;
	case 100: case 102: case 104: case 105:
		memset(values, 0, sizeof(values));
		count = GlStateCount(arguments[0]);
		element = 4;
		if (call == 100)
			element = 1;
		else if (call == 102)
			element = 8;
		if (arguments[0] == 0x8a0f || arguments[0] == 0x8a16 || arguments[0] == 0x85b2) {
			state = bridge->unpackrowbytes;
			if (arguments[0] == 0x85b2)
				state = bridge->unpackclientstorage;
			else if (arguments[0] == 0x8a0f)
				state = 0;
			statevalue = state;
			statefloat = (GLfloat)state;
			if (call == 100)
				values[0] = (uint8)(state != 0);
			else if (call == 102)
				memcpy(values, &statevalue, sizeof(statevalue));
			else if (call == 104)
				memcpy(values, &statefloat, sizeof(statefloat));
			else
				memcpy(values, &state, sizeof(state));
		} else if (!GlCacheLoad(bridge, call, arguments[0], values, count * element)) {
			if (call == 100)
				glGetBooleanv(arguments[0], values);
			else if (call == 102)
				glGetDoublev(arguments[0], (GLdouble *)values);
			else if (call == 104)
				glGetFloatv(arguments[0], (GLfloat *)values);
			else
				glGetIntegerv(arguments[0], (GLint *)values);
			bridge->errorpending = 1;
			GlCacheStore(bridge, call, arguments[0], values, count * element);
		}
		GlGuestWrite(cpu, arguments[1], values, count * element, element);
		break;
	case 118:
		if (!context->strings)
			return 0;
		if (arguments[0] == GL_EXTENSIONS || arguments[0] == 0x8874) {
			if (!context->extensions)
				return 0;
		}
		if (arguments[0] == GL_EXTENSIONS)
			cpu->gpr[3] = context->strings;
		else if (arguments[0] == GL_RENDERER)
			cpu->gpr[3] = context->strings + OSXQUARTZRENDERERSTRING - OSXQUARTZSTRINGS;
		else if (arguments[0] == GL_VENDOR)
			cpu->gpr[3] = context->strings + OSXQUARTZVENDORSTRING - OSXQUARTZSTRINGS;
		else if (arguments[0] == 0x8874)
			cpu->gpr[3] = context->strings + sizeof(OSXQUARTZEXTENSIONS) - 1;
		else
			return 0;
		break;
	case 238:
		if (!GlPixelLayout(arguments[4], arguments[5], &count, &element) || arguments[2] > 4096 ||
			arguments[3] > 4096 || bridge->packrowlength > 4096)
			return 0;
		state = GlPixelStride(arguments[2], bridge->packrowlength, bridge->packalignment, count * element);
		if (!arguments[3] || !GlUpload(bridge, state * arguments[3]))
			return 0;
		glReadPixels(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], bridge->upload);
		for (index = 0; index < arguments[3]; index++) {
			if (!GlGuestWrite(cpu, arguments[6] + index * state, bridge->upload + index * state,
				arguments[2] * count * element, element))
				return 0;
		}
		break;
	case 371:
		if (!bridge->multicoordinatevector || !GlGuestData(cpu, values, arguments[1], 16, 4))
			return 0;
		bridge->multicoordinatevector(arguments[0], (const GLfloat *)values);
		break;
	case 470:
		cpu->gpr[3] = GL_TRUE;
		break;
	case 471:
		glFinish();
		break;
	case 472:
		((OsxGlBindProgram)bridge->programs[0])(arguments[0], arguments[1]);
		break;
	case 473: case 474:
		count = arguments[0];
		if (count > 4096 || !GlUpload(bridge, count * 4))
			return 0;
		if (call == 474) {
			((OsxGlProgramNames)bridge->programs[2])(count, (GLuint *)bridge->upload);
			GlGuestWrite(cpu, arguments[1], bridge->upload, count * 4, 4);
		} else if (GlGuestData(cpu, bridge->upload, arguments[1], count * 4, 4))
			((OsxGlProgramNames)bridge->programs[1])(count, (GLuint *)bridge->upload);
		break;
	case 475:
		cpu->gpr[3] = ((OsxGlIsProgram)bridge->programs[3])(arguments[0]);
		break;
	case 519: case 523:
		((OsxGlProgramDoubles)bridge->programs[program])(arguments[0], arguments[1],
			GlFloat(cpu, 1), GlFloat(cpu, 2), GlFloat(cpu, 3), GlFloat(cpu, 4));
		break;
	case 521: case 525:
		((OsxGlProgramFloats)bridge->programs[program])(arguments[0], arguments[1],
			(GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4));
		break;
	case 520: case 522: case 524: case 526:
		element = 4;
		if (call == 520 || call == 524)
			element = 8;
		if (!GlGuestData(cpu, values, arguments[2], element * 4, element))
			return 0;
		((OsxGlProgramVector)bridge->programs[program])(arguments[0], arguments[1], values);
		break;
	case 527: case 528: case 529: case 530:
		element = 4;
		if (call == 527 || call == 529)
			element = 8;
		((OsxGlProgramVector)bridge->programs[program])(arguments[0], arguments[1], values);
		GlGuestWrite(cpu, arguments[2], values, element * 4, element);
		break;
	case 531:
		if (!GlUpload(bridge, arguments[2]) || !GlGuestCopy(cpu, bridge->upload, arguments[3], arguments[2]))
			return 0;
		((OsxGlProgramString)bridge->programs[16])(arguments[0], arguments[1], arguments[2], bridge->upload);
		break;
	case 532:
		count = 0;
		((OsxGlProgramQuery)bridge->programs[18])(arguments[0], 0x8627, &count);
		if (!GlUpload(bridge, count + 1))
			return 0;
		((OsxGlProgramQuery)bridge->programs[17])(arguments[0], arguments[1], bridge->upload);
		GlGuestWrite(cpu, arguments[2], bridge->upload, count, 1);
		break;
	case 533:
		((OsxGlProgramQuery)bridge->programs[18])(arguments[0], arguments[1], values);
		GlGuestWrite(cpu, arguments[2], values, 4, 4);
		break;
	case 103:
		cpu->gpr[3] = GL_NO_ERROR;
		if (bridge->errorpending) {
			cpu->gpr[3] = glGetError();
			bridge->errorpending = cpu->gpr[3] != GL_NO_ERROR;
		}
		break;
	case 144:
		if (!GlCacheLoad(bridge, call, arguments[0], values, 1)) {
			values[0] = glIsEnabled(arguments[0]);
			bridge->errorpending = 1;
			GlCacheStore(bridge, call, arguments[0], values, 1);
		}
		cpu->gpr[3] = values[0];
		break;
	case 119: case 120: case 127: case 128:
		memset(values, 0, sizeof(values));
		if (call == 119)
			glGetTexEnvfv(arguments[0], arguments[1], (GLfloat *)values);
		else if (call == 120)
			glGetTexEnviv(arguments[0], arguments[1], (GLint *)values);
		else if (call == 127)
			glGetTexParameterfv(arguments[0], arguments[1], (GLfloat *)values);
		else
			glGetTexParameteriv(arguments[0], arguments[1], (GLint *)values);
		GlGuestWrite(cpu, arguments[2], values, GlStateCount(arguments[1]) * 4, 4);
		break;
	case 125: case 126:
		memset(values, 0, sizeof(values));
		if (call == 125)
			glGetTexLevelParameterfv(arguments[0], arguments[1], arguments[2], (GLfloat *)values);
		else
			glGetTexLevelParameteriv(arguments[0], arguments[1], arguments[2], (GLint *)values);
		GlGuestWrite(cpu, arguments[3], values, 4, 4);
		break;
	case 159: case 160: case 176: case 177:
		element = 4;
		if (call == 159 || call == 176)
			element = 8;
		if (!GlGuestData(cpu, values, arguments[0], element * 16, element))
			break;
		if (call == 159)
			glLoadMatrixd((const GLdouble *)values);
		else if (call == 160)
			glLoadMatrixf((const GLfloat *)values);
		else if (call == 176)
			glMultMatrixd((const GLdouble *)values);
		else
			glMultMatrixf((const GLfloat *)values);
		break;
	case 195: case 196:
		if (call == 195)
			arguments[1] = (uint32)GlFloat(cpu, 1);
		if (arguments[0] == 0x8a16) {
			bridge->unpackrowbytes = arguments[1];
			break;
		}
		if (arguments[0] == GL_UNPACK_SKIP_PIXELS) {
			bridge->unpackskippixels = arguments[1];
			break;
		}
		if (arguments[0] == GL_UNPACK_SKIP_ROWS) {
			bridge->unpackskiprows = arguments[1];
			break;
		}
		if (arguments[0] == GL_PACK_ROW_LENGTH)
			bridge->packrowlength = arguments[1];
		if (arguments[0] == GL_PACK_ALIGNMENT &&
			(arguments[1] == 1 || arguments[1] == 2 || arguments[1] == 4 || arguments[1] == 8))
			bridge->packalignment = arguments[1];
		if (arguments[0] == GL_UNPACK_ROW_LENGTH)
			bridge->unpackrowlength = arguments[1];
		if (arguments[0] == GL_UNPACK_ALIGNMENT &&
			(arguments[1] == 1 || arguments[1] == 2 || arguments[1] == 4 || arguments[1] == 8))
			bridge->unpackalignment = arguments[1];
		if (arguments[0] == 0x85b2)
			bridge->unpackclientstorage = arguments[1];
		else
			glPixelStorei(arguments[0], arguments[1]);
		break;
	case 303: case 305:
		if (arguments[1] == 0x85bc)
			break;
		if (call == 303)
			glTexParameterf(arguments[0], arguments[1], (GLfloat)GlFloat(cpu, 1));
		else
			glTexParameteri(arguments[0], arguments[1], arguments[2]);
		break;
	case 292: case 294: case 304: case 306:
		if ((call == 304 || call == 306) && arguments[1] == 0x85bc)
			break;
		if (!GlGuestData(cpu, values, arguments[2], GlStateCount(arguments[1]) * 4, 4))
			break;
		if (call == 292)
			glTexEnvfv(arguments[0], arguments[1], (const GLfloat *)values);
		else if (call == 294)
			glTexEnviv(arguments[0], arguments[1], (const GLint *)values);
		else if (call == 304)
			glTexParameterfv(arguments[0], arguments[1], (const GLfloat *)values);
		else
			glTexParameteriv(arguments[0], arguments[1], (const GLint *)values);
		break;
	case 69:
		if (!GlQuartzPixels(bridge, cpu, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]))
			return 0;
		glDrawPixels(arguments[0], arguments[1], arguments[2], arguments[3], bridge->upload);
		break;
	case 457:
		break;
	case 302:
		if (!arguments[8])
			glTexImage2D(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], arguments[6], arguments[7], NULL);
		else if (GlQuartzPixels(bridge, cpu, arguments[3], arguments[4], arguments[6], arguments[7], arguments[8]))
			glTexImage2D(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], arguments[6], arguments[7], bridge->upload);
		else
			return 0;
		break;
	case 308:
		if (GlQuartzPixels(bridge, cpu, arguments[4], arguments[5], arguments[6], arguments[7], arguments[8]))
			glTexSubImage2D(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], arguments[6], arguments[7], bridge->upload);
		else
			return 0;
		break;
	case 336:
		memcpy(context->viewport, arguments, sizeof(context->viewport));
		glViewport(arguments[0], arguments[1], arguments[2], arguments[3]);
		break;
	case 337:
		if (!bridge->blendseparate)
			return 0;
		bridge->blendseparate(arguments[0], arguments[1], arguments[2], arguments[3]);
		break;
	case 339:
		if (!bridge->blendequation)
			return 0;
		bridge->blendequation(arguments[0]);
		break;
	case 342:
		if (!bridge->clientactivetexture)
			return 0;
		bridge->clientactivetexture(arguments[0]);
		break;
	case 343:
		if (!bridge->activetexture)
			return 0;
		bridge->activetexture(arguments[0]);
		break;
	case 354:
		if (!bridge->multicoordinate)
			return 0;
		bridge->multicoordinate(arguments[0], (GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2));
		break;
	default: return 0;
	}
	return 1;
}

OsxQuartzBridge *CreateQuartzBridge(uint32 width, uint32 height, uint32 library, uint32 strings, int extensions)
{
	OsxQuartzBridge *bridge;
	bridge = (OsxQuartzBridge *)calloc(1, sizeof(*bridge));
	if (bridge) {
		bridge->width = width;
		bridge->height = height;
		bridge->library = library;
		bridge->strings = strings;
		bridge->extensions = extensions;
	}
	return bridge;
}

void DestroyQuartzBridge(OsxQuartzBridge *bridge)
{
	OsxQuartzContext *context;
	OsxQuartzContext *next;
	OsxQuartzPbuffer *pbuffer;
	OsxQuartzPbuffer *following;
	for (context = bridge->contexts; context; context = next) {
		next = context->next;
		DestroyGlBridge(context->gl);
		free(context);
	}
	for (pbuffer = bridge->pbuffers; pbuffer; pbuffer = following) {
		following = pbuffer->next;
		free(pbuffer);
	}
	free(bridge);
}

static int QuartzSaverProcess(OsxQuartzBridge *bridge, OsxCpu *cpu)
{
	static const char name[] = "ScreenSaverEngine";
	uint32 address;
	uint32 word;
	uint32 matched;
	uint32 position;
	if (bridge->processsegment == cpu->sr[0])
		return bridge->processsaver;
	bridge->processsegment = cpu->sr[0];
	bridge->processsaver = 0;
	matched = 0;
	for (address = 0xbfffe000u; address < 0xc0000000u; address += 4) {
		if (!OsxCpuReadVirtual(cpu, address, &word))
			continue;
		for (position = 0; position < 4; position++) {
			if (((word >> (24 - position * 8)) & 255) == (uint8)name[matched]) {
				matched++;
				if (matched == sizeof(name) - 1) {
					bridge->processsaver = 1;
					return 1;
				}
			} else
				matched = 0;
		}
	}
	return 0;
}

static void QuartzPresent(OsxQuartzBridge *bridge, OsxQuartzContext *context)
{
	if (context->pbuffer || context->viewport[2] != bridge->width || context->viewport[3] != bridge->height)
		return;
	glFlush();
	context->gl->lastframe = OsxHostNanoseconds();
	context->gl->frames++;
	bridge->present = context;
	bridge->fresh = 1;
	GlBridgeCapture(context->gl);
}

static OsxQuartzPbuffer *QuartzFindPbuffer(OsxQuartzBridge *bridge, uint32 address, uint32 segment)
{
	OsxQuartzPbuffer *pbuffer;
	for (pbuffer = bridge->pbuffers; pbuffer; pbuffer = pbuffer->next)
		if (pbuffer->address == address && pbuffer->segment == segment)
			return pbuffer;
	return NULL;
}

static int QuartzAttachPbuffer(OsxQuartzBridge *bridge, OsxQuartzContext *context, uint32 address,
	uint32 width, uint32 height)
{
	OsxQuartzPbuffer *pbuffer;
	OsxGlBridge *gl;
	GLuint name;
	GLint previous;
	gl = context->gl;
	if (!gl->framebuffertexture)
		return 0;
	GLContext_SetCurrent(&gl->context);
	pbuffer = QuartzFindPbuffer(bridge, address, context->segment);
	if (!pbuffer) {
		pbuffer = (OsxQuartzPbuffer *)calloc(1, sizeof(*pbuffer));
		if (!pbuffer)
			return 0;
		pbuffer->address = address;
		pbuffer->segment = context->segment;
		glGenTextures(1, &name);
		pbuffer->texture = name;
		pbuffer->next = bridge->pbuffers;
		bridge->pbuffers = pbuffer;
	}
	if (pbuffer->width != width || pbuffer->height != height) {
		glGetIntegerv(0x84f6, &previous);
		glBindTexture(0x84f5, pbuffer->texture);
		glTexImage2D(0x84f5, 0, 0x881a, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
		glBindTexture(0x84f5, previous);
		pbuffer->width = width;
		pbuffer->height = height;
	}
	if (!gl->pbufferframebuffer) {
		gl->genframebuffers(1, &name);
		gl->pbufferframebuffer = name;
		gl->genrenderbuffers(1, &name);
		gl->pbufferdepth = name;
	}
	gl->bindframebuffer(OSXGLFRAMEBUFFER, gl->pbufferframebuffer);
	if (width > gl->pbufferdepthwidth || height > gl->pbufferdepthheight) {
		if (width < gl->pbufferdepthwidth)
			width = gl->pbufferdepthwidth;
		if (height < gl->pbufferdepthheight)
			height = gl->pbufferdepthheight;
		gl->bindrenderbuffer(0x8d41, gl->pbufferdepth);
		gl->renderbufferstorage(0x8d41, 0x88f0, width, height);
		gl->framebufferrenderbuffer(OSXGLFRAMEBUFFER, 0x821a, 0x8d41, gl->pbufferdepth);
		gl->pbufferdepthwidth = width;
		gl->pbufferdepthheight = height;
	}
	gl->framebuffertexture(OSXGLFRAMEBUFFER, OSXGLCOLORATTACHMENT, 0x84f5, pbuffer->texture, 0);
	gl->cachecount = 0;
	glViewport(0, 0, pbuffer->width, pbuffer->height);
	glScissor(0, 0, pbuffer->width, pbuffer->height);
	context->viewport[0] = 0;
	context->viewport[1] = 0;
	context->viewport[2] = pbuffer->width;
	context->viewport[3] = pbuffer->height;
	context->pbuffer = pbuffer;
	return 1;
}

static int QuartzPbufferTexture(OsxQuartzBridge *bridge, OsxCpu *cpu)
{
	OsxQuartzContext *context;
	OsxQuartzContext *destination;
	OsxQuartzPbuffer *pbuffer;
	OsxGlBridge *gl;
	uint32 parameters[5];
	GLuint name;
	destination = NULL;
	if (!GlGuestData(cpu, (uint8 *)parameters, cpu->gpr[4] + 12, sizeof(parameters), 4))
		return 0;
	if (parameters[0] != GL_TEXTURE_2D && parameters[0] != 0x84f5)
		return 0;
	for (context = bridge->contexts; context; context = context->next)
		if (context->segment == cpu->sr[0] && context->address == cpu->gpr[3])
			destination = context;
	pbuffer = QuartzFindPbuffer(bridge, cpu->gpr[4], cpu->sr[0]);
	if (!destination || !pbuffer || !destination->gl->framebuffertexture)
		return 0;
	gl = destination->gl;
	GLContext_SetCurrent(&gl->context);
	if (!gl->readframebuffer) {
		gl->genframebuffers(1, &name);
		gl->readframebuffer = name;
	}
	gl->bindframebuffer(0x8ca8, gl->readframebuffer);
	gl->framebuffertexture(0x8ca8, OSXGLCOLORATTACHMENT, 0x84f5, pbuffer->texture, 0);
	glCopyTexImage2D(parameters[0], 0, parameters[1], 0, 0, pbuffer->width, pbuffer->height, 0);
	if (destination->pbuffer)
		gl->bindframebuffer(0x8ca8, gl->pbufferframebuffer);
	else
		gl->bindframebuffer(0x8ca8, gl->framebuffer);
	cpu->gpr[3] = 0;
	cpu->nextpc = cpu->lr & ~3u;
	return 1;
}

int QuartzBridgeCall(OsxQuartzBridge *bridge, OsxCpu *cpu, const uint32 *tails, uint32 call, FILE *log)
{
	OsxQuartzContext *context;
	OsxQuartzContext *candidate;
	OsxGlBridge *share;
	uint32 base;
	uint32 engine;
	uint32 address;
	uint32 value;
	uint32 slot;
	uint32 next;
	int result;
	int firstcall;
	context = NULL;
	share = NULL;
	if (call == 1017) {
		cpu->gpr[0] = cpu->lr;
		value = 0;
		if (cpu->gpr[5] == 73)
			value = 1;
		else if (cpu->gpr[5] == 120 || cpu->gpr[5] == 121)
			value = 0x8000000;
		if (value && QuartzSaverProcess(bridge, cpu) && OsxCpuWriteVirtual(cpu, cpu->gpr[6], value)) {
			cpu->gpr[3] = 0;
			cpu->nextpc = cpu->lr & ~3u;
		}
		return 1;
	}
	if (call == 1018) {
		bridge->drawableflush = 1;
		cpu->gpr[0] = cpu->lr;
		if (!OsxCpuReadVirtual(cpu, cpu->gpr[3], &engine))
			return 1;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->engine == engine) {
				if (!GLContext_IsCurrent(&candidate->gl->context))
					GLContext_SetCurrent(&candidate->gl->context);
				QuartzPresent(bridge, candidate);
				cpu->gpr[3] = 0;
				cpu->nextpc = cpu->lr & ~3u;
				break;
			}
		}
		return 1;
	}
	if (call == 1019) {
		QuartzBridgeCall(bridge, cpu, tails, 1023, log);
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment == cpu->sr[0] && candidate->address == cpu->gpr[3]) {
				if (!OsxCpuReadVirtual(cpu, cpu->gpr[4] + 24, &address) ||
					!OsxCpuReadVirtual(cpu, cpu->gpr[4] + 28, &value) ||
					!address || !value || address > 4096 || value > 4096)
					break;
				if (QuartzAttachPbuffer(bridge, candidate, cpu->gpr[4], address, value)) {
					cpu->gpr[3] = 0;
					cpu->nextpc = cpu->lr & ~3u;
				}
				break;
			}
		}
		return 1;
	}
	if (call == 1020) {
		if (!QuartzPbufferTexture(bridge, cpu))
			cpu->gpr[0] = cpu->lr;
		return 1;
	}
	if (call == 1022) {
		value = cpu->gpr[0];
		engine = cpu->gpr[3];
		cpu->gpr[3] -= 4;
		QuartzBridgeCall(bridge, cpu, tails, 1023, log);
		cpu->gpr[0] = value;
		cpu->gpr[3] = engine;
		cpu->nextpc = cpu->lr & ~3u;
		return 1;
	}
	if (!call || call == 1023) {
		cpu->gpr[0] = cpu->lr;
		address = cpu->gpr[3];
		base = bridge->library;
		if (!call)
			base = cpu->pc - tails[0];
		if (!address || !QuartzSaverProcess(bridge, cpu) || !OsxCpuReadVirtual(cpu, address, &engine) || !engine ||
			!OsxCpuReadVirtual(cpu, base + tails[0], &value) || value != OSXQUARTZTRAP)
			return 1;
		for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
			if (candidate->segment != cpu->sr[0])
				continue;
			share = candidate->gl;
			if (candidate->engine == engine)
				context = candidate;
		}
		if (!context) {
			context = (OsxQuartzContext *)calloc(1, sizeof(*context));
			if (!context)
				return 1;
			context->gl = CreateGlBridgeShared(bridge->width, bridge->height, share, log);
			if (!context->gl) {
				free(context);
				return 1;
			}
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			context->address = address;
			context->engine = engine;
			if (bridge->strings)
				context->strings = base + bridge->strings;
			context->extensions = bridge->extensions;
			context->segment = cpu->sr[0];
			context->next = bridge->contexts;
			bridge->contexts = context;
			if (log)
				fprintf(log, "quartz context %08x engine %08x segment %08x base %08x\n", address, engine, context->segment, base);
		}
		if (!context)
			return 1;
		context->address = address;
		for (slot = 1; slot < OSXQUARTZSLOTS; slot++) {
			if (!tails[slot] || !OsxCpuReadVirtual(cpu, address + slot * 4, &value) || !value)
				continue;
			if (value != base + tails[slot]) {
				context->original[slot] = value;
				OsxCpuWriteVirtual(cpu, address + slot * 4, base + tails[slot]);
			}
		}
		return 1;
	}
	if (call >= OSXQUARTZSLOTS || !tails[call])
		return 0;
	for (candidate = bridge->contexts; candidate; candidate = candidate->next) {
		if (candidate->segment == cpu->sr[0] && candidate->engine == cpu->gpr[3]) {
			context = candidate;
			break;
		}
	}
	if (!context) {
		if ((cpu->ctr & ~3u) == cpu->pc)
			return 0;
		cpu->nextpc = cpu->ctr & ~3u;
		return 1;
	}
	firstcall = !(context->calls[call / 8] & (1u << (call % 8)));
	context->calls[call / 8] |= 1u << (call % 8);
	if (firstcall && log)
		fprintf(log, "quartz call %u ctx %08x args %08x %08x %08x %08x %08x %08x %08x %08x %08x fp %.9g %.9g %.9g %.9g\n",
			call, context->engine, GlArgument(cpu, 0), GlArgument(cpu, 1), GlArgument(cpu, 2), GlArgument(cpu, 3),
			GlArgument(cpu, 4), GlArgument(cpu, 5), GlArgument(cpu, 6), GlArgument(cpu, 7), GlArgument(cpu, 8),
			GlFloat(cpu, 1), GlFloat(cpu, 2), GlFloat(cpu, 3), GlFloat(cpu, 4));
	if (!GLContext_IsCurrent(&context->gl->context))
		GLContext_SetCurrent(&context->gl->context);
	next = cpu->nextpc;
	result = QuartzGlCall(context, cpu, call);
	if (cpu->nextpc != next)
		return 1;
	if (result) {
		cpu->nextpc = cpu->lr & ~3u;
		if (call == 591 || (call == 91 && !bridge->drawableflush))
			QuartzPresent(bridge, context);
	} else {
		if (log && firstcall)
			fprintf(log, "quartz native slot %u ctx %08x target %08x\n", call, context->engine, context->original[call]);
		if (context->original[call])
			cpu->nextpc = context->original[call];
		else
			cpu->nextpc = cpu->ctr & ~3u;
	}
	return 1;
}

int QuartzHostStrings(char *renderer, char *vendor, uint32 size)
{
	OsxGlBridge *bridge;
	const char *text;
	bridge = CreateGlBridge(1, 1);
	if (!bridge)
		return 0;
	text = (const char *)glGetString(GL_RENDERER);
	if (text)
		strncpy(renderer, text, size - 1);
	text = (const char *)glGetString(GL_VENDOR);
	if (text)
		strncpy(vendor, text, size - 1);
	DestroyGlBridge(bridge);
	return 1;
}

int QuartzBridgeFresh(OsxQuartzBridge *bridge)
{
	int fresh;
	fresh = bridge->fresh;
	bridge->fresh = 0;
	return fresh;
}

const uint8 *QuartzBridgeFrame(OsxQuartzBridge *bridge, const uint8 *vram, uint32 size)
{
	OsxGlBridge *gl;
	uint64 sum;
	uint32 index;
	if (!bridge->present)
		return NULL;
	gl = bridge->present->gl;
	if (OsxHostNanoseconds() - gl->lastframe < 500000000) {
		bridge->held = 0;
		return GlBridgeFrame(gl);
	}
	sum = 0;
	for (index = 0; index + 8 <= size; index += 8)
		sum = (sum ^ *(const uint64 *)(vram + index)) * 1099511628211ULL;
	if (!bridge->held) {
		bridge->held = 1;
		bridge->heldsum = sum;
		if (!GLContext_IsCurrent(&gl->context))
			GLContext_SetCurrent(&gl->context);
		GlBridgeCapture(gl);
	} else if (bridge->heldsum != sum) {
		bridge->present = NULL;
		return NULL;
	}
	return gl->pixels;
}

#endif
