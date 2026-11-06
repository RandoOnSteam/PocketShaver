/*
 *  osxglbridge.h - host OpenGL bridge for Mac OS X guest Flurry and Quartz OpenGL contexts
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
#ifndef OSXGLBRIDGE_H
#define OSXGLBRIDGE_H

#define OSXFLURRYTRAP 0x44f00002u
#define OSXFLURRYSTUB 0x9608u
#define OSXFLURRYSTUBSIZE 32u
#define OSXFLURRYSTUBCOUNT 66u
#define OSXFLURRYFLUSH 60u
#define OSXQUARTZTRAP 0x44c00002u
#define OSXQUARTZSTRINGS 0x400u
#define OSXQUARTZRENDERERSTRING 0x800u
#define OSXQUARTZVENDORSTRING 0x880u
#define OSXQUARTZSTRINGEND 0x900u
#define OSXQUARTZEXTENSIONS "GL_ARB_multitexture GL_ARB_texture_rectangle GL_EXT_texture_rectangle " \
	"GL_ARB_texture_non_power_of_two GL_APPLE_client_storage GL_APPLE_packed_pixels GL_EXT_bgra GL_EXT_abgr " \
	"GL_EXT_blend_func_separate GL_EXT_blend_minmax GL_EXT_blend_subtract GL_ARB_texture_env_combine " \
	"GL_ARB_texture_env_add GL_ARB_texture_env_crossbar GL_ARB_texture_env_dot3 GL_ARB_texture_border_clamp " \
	"GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_ARB_texture_mirrored_repeat GL_EXT_texture_lod_bias " \
	"GL_SGIS_generate_mipmap GL_EXT_texture_filter_anisotropic GL_ARB_texture_float GL_APPLE_float_pixels " \
	"GL_ARB_fragment_program GL_APPLE_flush_render"

#ifndef OSXGLBRIDGE
#define OSXGLBRIDGE 0
#endif
#if OSXGLBRIDGE

typedef struct OsxGlBridge OsxGlBridge;
typedef struct OsxQuartzBridge OsxQuartzBridge;
OsxQuartzBridge *CreateQuartzBridge(uint32 width, uint32 height);
void DestroyQuartzBridge(OsxQuartzBridge *bridge);
int QuartzBridgeCall(OsxQuartzBridge *bridge, OsxCpu *cpu, const uint32 *tails, uint32 call, FILE *log);
const uint8 *QuartzBridgeFrame(OsxQuartzBridge *bridge);
int QuartzHostStrings(char *renderer, char *vendor, uint32 size);
int QuartzBridgeFresh(OsxQuartzBridge *bridge);
OsxGlBridge *CreateGlBridge(uint32 width, uint32 height);
void DestroyGlBridge(OsxGlBridge *bridge);
int GlBridgeCall(OsxGlBridge *bridge, OsxCpu *cpu, uint32 call);
const uint8 *GlBridgeFrame(OsxGlBridge *bridge);
void GlBridgePresented(OsxGlBridge *bridge, FILE *log);

#endif
#endif
