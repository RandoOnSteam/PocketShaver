/*
 *  SDLGPUgl_compat.cpp - OpenGL 1.x compatibility rendering on SDL-GPU
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
#include "sdlgpu_gl_compat.h"
#include "sdlgpu_ffp_msl.h"
#include "sdlgpu_ffp_ps_dxil.h"
#include "sdlgpu_ffp_ps_spirv.h"
#include "sdlgpu_ffp_vs_dxil.h"
#include "sdlgpu_ffp_vs_spirv.h"
#include "gfx_log.h"
#if SDLGPU_TRANSITION_LOGGING_ENABLED
#include "atomic.h"
#endif

#include <SDL3/SDL_gpu.h>
#if defined(GFXACCEL_USE_SHADERCROSS)
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_3D
#define GL_TEXTURE_3D 0x806F
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R 0x8072
#endif
#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_SOURCE2_ALPHA 0x858A
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_OPERAND2_ALPHA 0x859A
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_RGB_SCALE 0x8573
#endif
#ifndef GL_TEXTURE_ENV_COLOR
#define GL_TEXTURE_ENV_COLOR 0x2201
#endif
#ifndef GL_ALPHA_SCALE
#define GL_ALPHA_SCALE 0x0D1C
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_FOG_COORDINATE_SOURCE
#define GL_FOG_COORDINATE_SOURCE 0x8450
#define GL_FOG_COORDINATE 0x8451
#define GL_FRAGMENT_DEPTH 0x8452
#endif
#ifndef GL_COLOR_SUM
#define GL_COLOR_SUM 0x8458
#endif
#ifndef GL_SECONDARY_COLOR_ARRAY
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#endif
#ifndef GL_MAX_TEXTURE_UNITS_ARB
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_TEXTURE_BINDING_3D
#define GL_TEXTURE_BINDING_3D 0x806A
#endif

namespace {

struct Matrix4 {
	float m[16];
};

struct CompatVertex {
	float position[4];
	float color[4];
	float secondary[4];
	float texcoord0[4];
	float texcoord1[4];
	float fog_coord;
	float padding[3];
	float eye_position[4];
};

struct TextureLevel {
	int width;
	int height;
	int depth;
	bool defined;

	TextureLevel() : width(0), height(0), depth(0), defined(false) {}
};

struct TextureObject {
	GLuint name;
	GLenum target;
	int width;
	int height;
	int depth;
	int levels;
	GLint internal_format;
	GLint min_filter;
	GLint mag_filter;
	GLint wrap_s;
	GLint wrap_t;
	GLint wrap_r;
	GLint max_level;
	float lod_bias;
	SDL_GPUTexture *texture;
	SDL_GPUSampler *sampler;
	bool sampler_dirty;
	bool presentation_y_flip;
	std::vector<TextureLevel> level_data;

	TextureObject()
		: name(0), target(GL_TEXTURE_2D), width(0), height(0), depth(1), levels(1),
		  internal_format(GL_RGBA8), min_filter(GL_NEAREST), mag_filter(GL_NEAREST),
		  wrap_s(GL_REPEAT), wrap_t(GL_REPEAT), wrap_r(GL_REPEAT), max_level(1000),
		  lod_bias(0.0f), texture(NULL), sampler(NULL), sampler_dirty(true),
		  presentation_y_flip(false)
	{
	}
};

struct RenderbufferObject {
	GLuint name;
	int width;
	int height;
	SDL_GPUTexture *texture;

	RenderbufferObject() : name(0), width(0), height(0), texture(NULL) {}
};

struct FramebufferObject {
	GLuint name;
	GLuint color_texture;
	GLuint depth_renderbuffer;
	GLint color_level;

	FramebufferObject()
		: name(0), color_texture(0), depth_renderbuffer(0), color_level(0) {}
};

struct TextureEnvironment {
	GLint mode;
	GLint combine_rgb;
	GLint combine_alpha;
	GLint source_rgb[3];
	GLint source_alpha[3];
	GLint operand_rgb[3];
	GLint operand_alpha[3];
	GLfloat color[4];
	GLfloat rgb_scale;
	GLfloat alpha_scale;

	TextureEnvironment()
		: mode(GL_MODULATE), combine_rgb(GL_MODULATE), combine_alpha(GL_MODULATE),
		  rgb_scale(1.0f), alpha_scale(1.0f)
	{
		source_rgb[0] = GL_TEXTURE;
		source_rgb[1] = GL_PREVIOUS;
		source_rgb[2] = GL_CONSTANT;
		source_alpha[0] = GL_TEXTURE;
		source_alpha[1] = GL_PREVIOUS;
		source_alpha[2] = GL_CONSTANT;
		operand_rgb[0] = GL_SRC_COLOR;
		operand_rgb[1] = GL_SRC_COLOR;
		operand_rgb[2] = GL_SRC_ALPHA;
		operand_alpha[0] = GL_SRC_ALPHA;
		operand_alpha[1] = GL_SRC_ALPHA;
		operand_alpha[2] = GL_SRC_ALPHA;
		color[0] = 0.0f;
		color[1] = 0.0f;
		color[2] = 0.0f;
		color[3] = 0.0f;
	}
};

struct LightState {
	bool enabled;
	float ambient[4];
	float diffuse[4];
	float specular[4];
	float position[4];
	float spot_direction[3];
	float spot_exponent;
	float spot_cutoff;
	float constant_attenuation;
	float linear_attenuation;
	float quadratic_attenuation;

	LightState()
		: enabled(false), spot_exponent(0.0f), spot_cutoff(180.0f),
		  constant_attenuation(1.0f), linear_attenuation(0.0f),
		  quadratic_attenuation(0.0f)
	{
		std::memset(ambient, 0, sizeof(ambient));
		std::memset(diffuse, 0, sizeof(diffuse));
		std::memset(specular, 0, sizeof(specular));
		position[0] = 0.0f;
		position[1] = 0.0f;
		position[2] = 1.0f;
		position[3] = 0.0f;
		spot_direction[0] = 0.0f;
		spot_direction[1] = 0.0f;
		spot_direction[2] = -1.0f;
	}
};

struct MaterialState {
	float ambient[4];
	float diffuse[4];
	float specular[4];
	float emission[4];
	float shininess;

	MaterialState() : shininess(0.0f)
	{
		ambient[0] = 0.2f;
		ambient[1] = 0.2f;
		ambient[2] = 0.2f;
		ambient[3] = 1.0f;
		diffuse[0] = 0.8f;
		diffuse[1] = 0.8f;
		diffuse[2] = 0.8f;
		diffuse[3] = 1.0f;
		std::memset(specular, 0, sizeof(specular));
		std::memset(emission, 0, sizeof(emission));
	}
};

struct FixedState {
	Matrix4 modelview;
	Matrix4 projection;
	Matrix4 texture_matrix[2];
	GLenum matrix_mode;
	int active_texture;
	int client_active_texture;
	GLuint bound_texture_2d[2];
	GLuint bound_texture_3d[2];
	bool texture_2d_enabled[2];
	bool texture_3d_enabled[2];
	TextureEnvironment texenv[2];
	float current_color[4];
	float current_secondary[4];
	float current_normal[3];
	float current_texcoord[2][4];
	float current_fog_coord;
	bool blend_enabled;
	GLenum blend_src_rgb;
	GLenum blend_dst_rgb;
	GLenum blend_src_alpha;
	GLenum blend_dst_alpha;
	GLenum blend_equation;
	float blend_color[4];
	bool alpha_test_enabled;
	GLenum alpha_func;
	float alpha_ref;
	bool depth_test_enabled;
	bool depth_write;
	GLenum depth_func;
	double depth_near;
	double depth_far;
	bool stencil_test_enabled;
	GLenum stencil_func;
	GLint stencil_ref;
	GLuint stencil_value_mask;
	GLuint stencil_write_mask;
	GLenum stencil_fail;
	GLenum stencil_depth_fail;
	GLenum stencil_pass;
	bool cull_enabled;
	GLenum cull_face;
	GLenum front_face;
	GLenum polygon_mode;
	bool polygon_offset_enabled;
	float polygon_offset_factor;
	float polygon_offset_units;
	bool scissor_enabled;
	GLint viewport[4];
	GLint scissor[4];
	bool color_mask[4];
	bool fog_enabled;
	GLenum fog_mode;
	GLenum fog_source;
	float fog_color[4];
	float fog_density;
	float fog_start;
	float fog_end;
	bool lighting_enabled;
	bool normalize_enabled;
	bool color_material_enabled;
	GLenum color_material_mode;
	bool color_sum_enabled;
	LightState lights[8];
	MaterialState material;
	float light_model_ambient[4];
	bool light_model_local_viewer;
	bool light_model_two_side;
	bool clip_enabled[6];
	double clip_plane[6][4];
	float clear_color[4];
	double clear_depth;
	GLint clear_stencil;
	float point_size;
	float line_width;
	GLenum shade_model;
	GLint unpack_alignment;
	GLint pack_alignment;
	float raster_x;
	float raster_y;
	bool raster_valid;

	FixedState();
};

struct PipelineKey {
	Uint32 primitive;
	Uint32 blend_enabled;
	Uint32 blend_src_rgb;
	Uint32 blend_dst_rgb;
	Uint32 blend_src_alpha;
	Uint32 blend_dst_alpha;
	Uint32 blend_equation;
	Uint32 color_mask;
	Uint32 depth_test;
	Uint32 depth_write;
	Uint32 depth_func;
	Uint32 stencil_test;
	Uint32 stencil_func;
	Uint32 stencil_fail;
	Uint32 stencil_depth_fail;
	Uint32 stencil_pass;
	Uint32 stencil_value_mask;
	Uint32 stencil_write_mask;
	Uint32 cull_mode;
	Uint32 front_face;
	Uint32 fill_mode;
	Uint32 depth_bias;
	Uint32 has_depth;

	bool operator<(const PipelineKey &other) const
	{
		return std::memcmp(this, &other, sizeof(PipelineKey)) < 0;
	}
};

struct FragmentUniforms {
	float data[24][4];
};

struct PendingDraw {
	std::vector<CompatVertex> vertices;
	FixedState state;
	FragmentUniforms uniforms;
	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUTexture *color;
	SDL_GPUTexture *depth;
	TextureObject *textures[3];
	SDL_GPUSampler *samplers[3];
	int target_width;
	int target_height;
	int color_level;
	Uint32 vertex_offset;
};

static SDL_GPUDevice *s_device = NULL;
static SDL_Window *s_window = NULL;
#if defined(GFXACCEL_USE_SHADERCROSS)
static bool s_shadercross_initialized = false;
#endif
static SDL_GPUShader *s_vertex_shader = NULL;
static SDL_GPUShader *s_fragment_shader = NULL;
static SDL_GPUBuffer *s_vertex_buffer = NULL;
static SDL_GPUTransferBuffer *s_vertex_transfer = NULL;
static SDL_GPUTransferBuffer *s_texture_transfer = NULL;
static Uint32 s_vertex_capacity = 4 * 1024 * 1024;
static Uint32 s_texture_transfer_capacity = 4 * 1024 * 1024;
static SDL_GPUCommandBuffer *s_deferred_command = NULL;
static SDL_GPUTexture *s_default_color = NULL;
static SDL_GPUTexture *s_default_depth = NULL;
static int s_default_width = 0;
static int s_default_height = 0;
static TextureObject *s_white_2d = NULL;
static TextureObject *s_white_3d = NULL;
static std::map<GLuint, TextureObject *> s_textures;
static std::map<GLuint, RenderbufferObject *> s_renderbuffers;
static std::map<GLuint, FramebufferObject *> s_framebuffers;
static std::map<PipelineKey, SDL_GPUGraphicsPipeline *> s_pipelines;
static GLuint s_next_texture = 1;
static GLuint s_next_framebuffer = 1;
static GLuint s_next_renderbuffer = 1;
static GLuint s_bound_framebuffer = 0;
static GLuint s_bound_renderbuffer = 0;
static FixedState s_state;
static std::vector<FixedState> s_attrib_stack;
static std::vector<Matrix4> s_modelview_stack;
static std::vector<Matrix4> s_projection_stack;
static std::vector<Matrix4> s_texture_stack[2];
static GLenum s_begin_mode = 0;
static std::vector<CompatVertex> s_vertices;
static std::deque<PendingDraw> s_pending_draws;
static Uint32 s_pending_vertex_bytes = 0;
static GLenum s_error = GL_NO_ERROR;

#if SDLGPU_TRANSITION_LOGGING_ENABLED
enum {
	kSDLGPUTransitionEngineCount = 5,
	kSDLGPUTransitionDispatchOpcodeCount = 1024,
	kSDLGPUTransitionGuestSampleCount = 512
};

struct SDLGPUTransitionProfile {
	Uint64 device_create_ticks;
	Uint64 shader_create_ticks;
	Uint64 buffer_create_ticks;
	Uint64 window_claim_ticks;
	Uint64 window_release_ticks;
	Uint64 texture_create_ticks;
	Uint64 texture_convert_ticks;
	Uint64 texture_upload_ticks;
	Uint64 texture_release_ticks;
	Uint64 pipeline_create_ticks;
	Uint64 draw_flush_ticks;
	Uint64 readback_ticks;
	Uint64 copy_texture_ticks;
	Uint64 present_ticks;
	Uint64 swapchain_config_ticks;
	Uint64 swapchain_acquire_ticks;
	Uint64 swapchain_submit_ticks;
	Uint64 finish_ticks;
	Uint64 clear_ticks;
	Uint64 pacing_ticks;
	Uint64 video_vbl_ticks;
	Uint64 video_vbl_nqd_ticks;
	Uint64 video_vbl_compositor_ticks;
	Uint64 video_vbl_lock_ticks;
	Uint64 video_vbl_service_ticks;
	Uint64 video_vbl_interval_max_ticks;
	Uint64 vbl_callback_ticks;
	Uint64 upload_bytes;
	Uint64 draw_vertices;
	Uint32 device_create_count;
	Uint32 shader_create_count;
	Uint32 buffer_create_count;
	Uint32 window_claim_count;
	Uint32 window_release_count;
	Uint32 texture_create_count;
	Uint32 texture_convert_count;
	Uint32 texture_upload_count;
	Uint32 texture_release_count;
	Uint32 pipeline_create_count;
	Uint32 draw_flush_count;
	Uint32 draw_count;
	Uint32 readback_count;
	Uint32 copy_texture_count;
	Uint32 present_count;
	Uint32 swapchain_config_count;
	Uint32 swapchain_acquire_count;
	Uint32 swapchain_submit_count;
	Uint32 swapchain_interval;
	Uint32 swapchain_mode;
	Uint32 swapchain_immediate_supported;
	Uint32 swapchain_mailbox_supported;
	Uint32 swapchain_config_succeeded;
	Uint32 finish_count;
	Uint32 clear_count;
	Uint32 pacing_count;
	Uint32 video_vbl_count;
	Uint32 vbl_callback_count;
	Uint64 dispatch_ticks[kSDLGPUTransitionEngineCount]
		[kSDLGPUTransitionDispatchOpcodeCount];
	Uint64 dispatch_max_ticks[kSDLGPUTransitionEngineCount]
		[kSDLGPUTransitionDispatchOpcodeCount];
	Uint64 dispatch_count[kSDLGPUTransitionEngineCount]
		[kSDLGPUTransitionDispatchOpcodeCount];
};

static SDLGPUTransitionProfile s_transition_profile;
static Uint32 s_transition_trace_depth = 0;
static Uint64 s_transition_profile_start_tick = 0;
static Uint64 s_transition_last_event_tick = 0;
static Uint64 s_transition_gap_ticks[16];
static Uint64 s_transition_last_video_vbl_tick = 0;
static atomic_uint64 s_transition_redraw_ticks = 0;
static atomic_uint64 s_transition_redraw_count = 0;
static atomic_uint64 s_transition_active_dispatch =
	(((Uint64)0xffffffffu << 32) | (Uint64)0xffffffffu);
static atomic_uint64 s_transition_guest_sample_sequence = 0;
static Uint64 s_transition_profile_sample_sequence = 0;

struct SDLGPUTransitionGuestProfile {
	atomic_uint64 sample_count;
	atomic_uint64 irq_pending_count;
	atomic_uint64 irq_nested_count;
	atomic_uint64 tick_inhibited_count;
	atomic_uint64 last_pc;
	atomic_uint64 last_pc68k;
	atomic_uint64 last_level68k;
	atomic_uint64 last_run_mode;
	atomic_uint64 last_irq_nest;
	atomic_uint64 last_irq_flags;
	atomic_uint64 last_tick_inhibited;
	atomic_uint64 stable_pc_count;
	atomic_uint64 stable_pc;
	atomic_uint64 stable_pc68k_count;
	atomic_uint64 stable_pc68k;
};

static SDLGPUTransitionGuestProfile s_transition_guest_profile;

struct SDLGPUTransitionGuestSample {
	atomic_uint64 sequence;
	atomic_uint64 tick;
	atomic_uint64 pc;
	atomic_uint64 pc68k;
	atomic_uint64 level68k;
	atomic_uint64 run_mode;
	atomic_uint64 irq_nest;
	atomic_uint64 irq_flags;
	atomic_uint64 tick_inhibited;
	atomic_uint64 dispatch_engine;
	atomic_uint64 dispatch_opcode;
};

static SDLGPUTransitionGuestSample
	s_transition_guest_samples[kSDLGPUTransitionGuestSampleCount];
#endif

static bool SDLGPUFlushPendingDraws();
static SDL_GPUCommandBuffer *SDLGPUAcquireDeferredCommand();
static bool SDLGPUSubmitDeferredCommand();
static void SDLGPUCancelDeferredCommand();

#if SDLGPU_TRANSITION_LOGGING_ENABLED
static Uint64 SDLGPUProfileNow()
{
	return SDL_GetPerformanceCounter();
}

static double SDLGPUProfileMilliseconds(Uint64 ticks)
{
	static Uint64 frequency = 0;
	if (frequency == 0)
		frequency = SDL_GetPerformanceFrequency();
	if (frequency == 0)
		return 0.0;
	return (double)ticks * 1000.0 / (double)frequency;
}

static void SDLGPUProfileReset()
{
	std::memset(&s_transition_profile, 0, sizeof(s_transition_profile));
	s_transition_profile_start_tick = SDLGPUProfileNow();
	s_transition_profile_sample_sequence = atomic_load_explicit(
		&s_transition_guest_sample_sequence, memory_order_acquire);
}

static void SDLGPUProfileRuntimeCheckpoint()
{
	if (s_transition_trace_depth != 0)
		return;
	if (s_transition_profile.texture_create_count == 0 &&
		s_transition_profile.texture_upload_count == 0 &&
		s_transition_profile.texture_release_count == 0 &&
		s_transition_profile.pipeline_create_count == 0)
		return;
	SDLGPUTransitionTraceBegin();
	SDLGPUTransitionTraceEnd("resource-flush", 0xffffffffu, 0, 0,
		s_transition_profile_start_tick);
}

static SDL_GPUTexture *SDLGPUCreateTextureResource(
	const SDL_GPUTextureCreateInfo *info)
{
	Uint64 start_tick = SDLGPUProfileNow();
	SDL_GPUTexture *texture = SDL_CreateGPUTexture(s_device, info);
	s_transition_profile.texture_create_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.texture_create_count++;
	return texture;
}
#else
#define SDLGPUCreateTextureResource(info) SDL_CreateGPUTexture(s_device, info)
#endif

static SDL_GPUCommandBuffer *SDLGPUAcquireDeferredCommand()
{
	if (!s_device)
		return NULL;
	if (!s_deferred_command)
		s_deferred_command = SDL_AcquireGPUCommandBuffer(s_device);
	return s_deferred_command;
}

static bool SDLGPUSubmitDeferredCommand()
{
	if (!s_deferred_command)
		return true;
	SDL_GPUCommandBuffer *command = s_deferred_command;
	s_deferred_command = NULL;
	return SDL_SubmitGPUCommandBuffer(command);
}

static void SDLGPUCancelDeferredCommand()
{
	if (!s_deferred_command)
		return;
	SDL_GPUCommandBuffer *command = s_deferred_command;
	s_deferred_command = NULL;
	SDL_CancelGPUCommandBuffer(command);
}

static void SDLGPUReleaseTextureResource(SDL_GPUTexture *texture)
{
	if (!texture)
		return;
	SDLGPUSubmitDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 start_tick = SDLGPUProfileNow();
#endif
	SDL_ReleaseGPUTexture(s_device, texture);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.texture_release_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.texture_release_count++;
#endif
}

#if SDLGPU_TRANSITION_LOGGING_ENABLED
static bool SDLGPUProfileTextureUploadResult(bool result, Uint64 start_tick,
	Uint32 size)
{
	s_transition_profile.texture_upload_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.texture_upload_count++;
	s_transition_profile.upload_bytes += size;
	return result;
}

static bool SDLGPUProfileReadbackResult(bool result, Uint64 start_tick)
{
	s_transition_profile.readback_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.readback_count++;
	return result;
}

static bool SDLGPUProfilePresentResult(bool result, Uint64 start_tick)
{
	s_transition_profile.present_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.present_count++;
	return result;
}
#endif

static void SDLGPUSetIdentityMatrix(Matrix4 &matrix)
{
	std::memset(matrix.m, 0, sizeof(matrix.m));
	matrix.m[0] = 1.0f;
	matrix.m[5] = 1.0f;
	matrix.m[10] = 1.0f;
	matrix.m[15] = 1.0f;
}

static Matrix4 SDLGPUMultiplyMatrices(const Matrix4 &left, const Matrix4 &right)
{
	Matrix4 result;
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			result.m[column * 4 + row] =
				left.m[0 * 4 + row] * right.m[column * 4 + 0] +
				left.m[1 * 4 + row] * right.m[column * 4 + 1] +
				left.m[2 * 4 + row] * right.m[column * 4 + 2] +
				left.m[3 * 4 + row] * right.m[column * 4 + 3];
		}
	}
	return result;
}

static void SDLGPUTransformVector(const Matrix4 &matrix, const float input[4], float output[4])
{
	for (int row = 0; row < 4; row++) {
		output[row] = matrix.m[row] * input[0] + matrix.m[4 + row] * input[1] +
			matrix.m[8 + row] * input[2] + matrix.m[12 + row] * input[3];
	}
}

static Matrix4 &SDLGPUGetCurrentMatrix()
{
	if (s_state.matrix_mode == GL_PROJECTION)
		return s_state.projection;
	if (s_state.matrix_mode == GL_TEXTURE)
		return s_state.texture_matrix[s_state.active_texture];
	return s_state.modelview;
}

static float SDLGPUClampFloat(float value, float low, float high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

static void SDLGPUNormalizeVector3(float value[3])
{
	float length = std::sqrt(value[0] * value[0] + value[1] * value[1] +
	                         value[2] * value[2]);
	if (length > 0.000001f) {
		value[0] /= length;
		value[1] /= length;
		value[2] /= length;
	}
}

FixedState::FixedState()
	: matrix_mode(GL_MODELVIEW), active_texture(0), client_active_texture(0),
	  current_fog_coord(0.0f), blend_enabled(false), blend_src_rgb(GL_ONE),
	  blend_dst_rgb(GL_ZERO), blend_src_alpha(GL_ONE), blend_dst_alpha(GL_ZERO),
	  blend_equation(GL_FUNC_ADD), alpha_test_enabled(false), alpha_func(GL_ALWAYS),
	  alpha_ref(0.0f), depth_test_enabled(false), depth_write(true),
	  depth_func(GL_LESS), depth_near(0.0), depth_far(1.0),
	  stencil_test_enabled(false), stencil_func(GL_ALWAYS), stencil_ref(0),
	  stencil_value_mask(~0u), stencil_write_mask(~0u), stencil_fail(GL_KEEP),
	  stencil_depth_fail(GL_KEEP), stencil_pass(GL_KEEP), cull_enabled(false),
	  cull_face(GL_BACK), front_face(GL_CCW), polygon_mode(GL_FILL),
	  polygon_offset_enabled(false), polygon_offset_factor(0.0f),
	  polygon_offset_units(0.0f), scissor_enabled(false), fog_enabled(false),
	  fog_mode(GL_EXP), fog_source(GL_FRAGMENT_DEPTH), fog_density(1.0f),
	  fog_start(0.0f), fog_end(1.0f), lighting_enabled(false),
	  normalize_enabled(false), color_material_enabled(false),
	  color_material_mode(GL_AMBIENT_AND_DIFFUSE), color_sum_enabled(false),
	  light_model_local_viewer(false), light_model_two_side(false),
	  clear_depth(1.0), clear_stencil(0), point_size(1.0f), line_width(1.0f),
	  shade_model(GL_SMOOTH), unpack_alignment(4), pack_alignment(4),
	  raster_x(0.0f), raster_y(0.0f), raster_valid(false)
{
	SDLGPUSetIdentityMatrix(modelview);
	SDLGPUSetIdentityMatrix(projection);
	for (int unit = 0; unit < 2; unit++) {
		SDLGPUSetIdentityMatrix(texture_matrix[unit]);
		bound_texture_2d[unit] = 0;
		bound_texture_3d[unit] = 0;
		texture_2d_enabled[unit] = false;
		texture_3d_enabled[unit] = false;
		current_texcoord[unit][0] = 0.0f;
		current_texcoord[unit][1] = 0.0f;
		current_texcoord[unit][2] = 0.0f;
		current_texcoord[unit][3] = 1.0f;
	}
	current_color[0] = 1.0f;
	current_color[1] = 1.0f;
	current_color[2] = 1.0f;
	current_color[3] = 1.0f;
	current_secondary[0] = 0.0f;
	current_secondary[1] = 0.0f;
	current_secondary[2] = 0.0f;
	current_secondary[3] = 1.0f;
	current_normal[0] = 0.0f;
	current_normal[1] = 0.0f;
	current_normal[2] = 1.0f;
	blend_color[0] = 0.0f;
	blend_color[1] = 0.0f;
	blend_color[2] = 0.0f;
	blend_color[3] = 0.0f;
	viewport[0] = 0;
	viewport[1] = 0;
	viewport[2] = 640;
	viewport[3] = 480;
	scissor[0] = 0;
	scissor[1] = 0;
	scissor[2] = 640;
	scissor[3] = 480;
	for (int channel = 0; channel < 4; channel++)
		color_mask[channel] = true;
	fog_color[0] = 0.0f;
	fog_color[1] = 0.0f;
	fog_color[2] = 0.0f;
	fog_color[3] = 0.0f;
	light_model_ambient[0] = 0.2f;
	light_model_ambient[1] = 0.2f;
	light_model_ambient[2] = 0.2f;
	light_model_ambient[3] = 1.0f;
	for (int light = 0; light < 8; light++) {
		if (light == 0) {
			lights[light].diffuse[0] = 1.0f;
			lights[light].diffuse[1] = 1.0f;
			lights[light].diffuse[2] = 1.0f;
			lights[light].diffuse[3] = 1.0f;
			lights[light].specular[0] = 1.0f;
			lights[light].specular[1] = 1.0f;
			lights[light].specular[2] = 1.0f;
			lights[light].specular[3] = 1.0f;
		}
	}
	for (int plane = 0; plane < 6; plane++) {
		clip_enabled[plane] = false;
		std::memset(clip_plane[plane], 0, sizeof(clip_plane[plane]));
	}
	clear_color[0] = 0.0f;
	clear_color[1] = 0.0f;
	clear_color[2] = 0.0f;
	clear_color[3] = 0.0f;
}

#if defined(GFXACCEL_USE_SHADERCROSS)
static const char *s_ffp_shader =
	"struct VSInput { float4 position : TEXCOORD0; float4 color : TEXCOORD1; "
	"float4 secondary : TEXCOORD2; float4 tex0 : TEXCOORD3; "
	"float4 tex1 : TEXCOORD4; float fogCoord : TEXCOORD5; float4 eye : TEXCOORD6; };\n"
	"struct VSOutput { float4 position : SV_Position; float4 color : TEXCOORD0; "
	"float4 secondary : TEXCOORD1; float4 tex0 : TEXCOORD2; float4 tex1 : TEXCOORD3; "
	"float fogCoord : TEXCOORD4; float4 eye : TEXCOORD5;\n"
	"#if defined(SPIRV)\n"
	"[[vk::builtin(\"PointSize\")]]\n"
	"#endif\n"
	"float pointSize : PSIZE; };\n"
	"VSOutput VSMain(VSInput input) { VSOutput output; output.position = input.position; "
	"output.color = input.color; output.secondary = input.secondary; output.tex0 = input.tex0; "
	"output.tex1 = input.tex1; output.fogCoord = input.fogCoord; output.eye = input.eye; "
	"output.pointSize = 1.0; return output; }\n"
	"Texture2D Texture0 : register(t0, space2); SamplerState Sampler0 : register(s0, space2);\n"
	"Texture2D Texture1 : register(t1, space2); SamplerState Sampler1 : register(s1, space2);\n"
	"Texture3D Texture3 : register(t2, space2); SamplerState Sampler3 : register(s2, space2);\n"
	"cbuffer FragmentData : register(b0, space3) { float4 U[24]; };\n"
	"float4 SourceValue(int source, float4 texel, float4 primary, float4 previous, float4 constantColor) { "
	"if (source == 5890) return texel; if (source == 34167) return primary; "
	"if (source == 34168) return previous; return constantColor; }\n"
	"float3 OperandRGB(float4 value, int operand) { if (operand == 769) return 1.0 - value.rgb; "
	"if (operand == 770) return value.aaa; if (operand == 771) return 1.0 - value.aaa; return value.rgb; }\n"
	"float OperandAlpha(float4 value, int operand) { if (operand == 771) return 1.0 - value.a; return value.a; }\n"
	"float4 CombineUnit(float4 previous, float4 primary, float4 texel, float4 constantColor, "
	"float4 flags, float4 combine, float4 source0, float4 source1, float4 source2) { "
	"int mode = (int)flags.z; if (mode == 7681) return texel; if (mode == 8448) return previous * texel; "
	"if (mode == 260) return saturate(previous + texel); if (mode == 8449) { "
	"return float4(lerp(previous.rgb, texel.rgb, texel.a), previous.a); } if (mode != 34160) return previous * texel; "
	"float4 a = SourceValue((int)source0.x, texel, primary, previous, constantColor); "
	"float4 b = SourceValue((int)source0.y, texel, primary, previous, constantColor); "
	"float4 c = SourceValue((int)source0.z, texel, primary, previous, constantColor); "
	"float3 ar = OperandRGB(a, (int)source1.y); float3 br = OperandRGB(b, (int)source1.z); "
	"float3 cr = OperandRGB(c, (int)source1.w); float3 rgb; int rgbMode = (int)flags.w; "
	"if (rgbMode == 7681) rgb = ar; else if (rgbMode == 260) rgb = ar + br; "
	"else if (rgbMode == 34165) rgb = ar * cr + br * (1.0 - cr); else rgb = ar * br; "
	"float4 aa = SourceValue((int)source0.w, texel, primary, previous, constantColor); "
	"float4 ab = SourceValue((int)source1.x, texel, primary, previous, constantColor); "
	"float alphaA = OperandAlpha(aa, (int)source2.x); float alphaB = OperandAlpha(ab, (int)source2.y); "
	"float alpha; int alphaMode = (int)combine.x; if (alphaMode == 7681) alpha = alphaA; "
	"else if (alphaMode == 260) alpha = alphaA + alphaB; else alpha = alphaA * alphaB; "
	"return saturate(float4(rgb * combine.y, alpha * combine.z)); }\n"
	"bool AlphaPass(float alpha, int func, float refValue) { if (func == 512) return false; "
	"if (func == 513) return alpha < refValue; if (func == 514) return alpha == refValue; "
	"if (func == 515) return alpha <= refValue; if (func == 516) return alpha > refValue; "
	"if (func == 517) return alpha != refValue; if (func == 518) return alpha >= refValue; return true; }\n"
	"float4 PSMain(VSOutput input) : SV_Target0 { uint clipMask = (uint)U[4].z; "
	"for (int plane = 0; plane < 6; plane++) { if ((clipMask & (1u << plane)) != 0 && dot(input.eye, U[16 + plane]) < 0.0) discard; } "
	"float4 primary = input.color; float4 color = primary; float4 texel0 = float4(1,1,1,1); "
	"if (U[5].x != 0.0) texel0 = Texture0.Sample(Sampler0, input.tex0.xy / input.tex0.w); "
	"else if (U[5].y != 0.0) texel0 = Texture3.Sample(Sampler3, input.tex0.xyz / input.tex0.w); "
	"if (U[5].x != 0.0 || U[5].y != 0.0) color = CombineUnit(color, primary, texel0, U[1], U[5], U[7], U[9], U[10], U[11]); "
	"if (U[6].x != 0.0) { float4 texel1 = Texture1.Sample(Sampler1, input.tex1.xy / input.tex1.w); "
	"color = CombineUnit(color, primary, texel1, U[2], U[6], U[8], U[12], U[13], U[14]); } "
	"if (U[4].w != 0.0) color.rgb = saturate(color.rgb + input.secondary.rgb); "
	"if (U[15].x != 0.0 && !AlphaPass(color.a, (int)U[4].y, U[3].x)) discard; "
	"if (U[15].y != 0.0) { float fogFactor; int fogMode = (int)U[4].x; float fogValue = input.fogCoord; "
	"if (fogMode == 9729) fogFactor = (U[3].w - fogValue) / max(U[3].w - U[3].z, 0.00001); "
	"else if (fogMode == 2049) { float d = U[3].y * fogValue; fogFactor = exp(-(d * d)); } "
	"else fogFactor = exp(-(U[3].y * fogValue)); color.rgb = lerp(U[0].rgb, color.rgb, saturate(fogFactor)); } "
	"return saturate(color); }\n";
#endif

static void SDLGPUSetError(GLenum error)
{
	if (s_error == GL_NO_ERROR)
		s_error = error;
}

static int SDLGPUCalculateMipLevelCount(int width, int height, int depth)
{
	int count = 1;
	int size = width;
	if (height > size)
		size = height;
	if (depth > size)
		size = depth;
	while (size > 1) {
		size >>= 1;
		count++;
	}
	return count;
}

static int SDLGPUAlignInteger(int value, int alignment)
{
	if (alignment <= 1)
		return value;
	return (value + alignment - 1) & ~(alignment - 1);
}

static TextureObject *SDLGPUFindTexture(GLuint name)
{
	std::map<GLuint, TextureObject *>::iterator found = s_textures.find(name);
	if (found == s_textures.end())
		return NULL;
	return found->second;
}

static TextureObject *SDLGPUGetBoundTexture(GLenum target)
{
	GLuint name = 0;
	if (target == GL_TEXTURE_3D)
		name = s_state.bound_texture_3d[s_state.active_texture];
	else
		name = s_state.bound_texture_2d[s_state.active_texture];
	if (name == 0)
		return NULL;
	return SDLGPUFindTexture(name);
}

static void SDLGPUReleaseTextureResources(TextureObject *object)
{
	if (!object || !s_device)
		return;
	if (object->sampler || object->texture)
		SDLGPUSubmitDeferredCommand();
	if (object->sampler) {
		SDL_ReleaseGPUSampler(s_device, object->sampler);
		object->sampler = NULL;
	}
	if (object->texture) {
		SDLGPUReleaseTextureResource(object->texture);
		object->texture = NULL;
	}
}

static SDL_GPUTexture *SDLGPUCreateTexture(GLenum target, int width, int height,
	                                      int depth, int levels)
{
	SDL_GPUTextureCreateInfo info;
	std::memset(&info, 0, sizeof(info));
	if (target == GL_TEXTURE_3D)
		info.type = SDL_GPU_TEXTURETYPE_3D;
	else
		info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	if (target != GL_TEXTURE_3D)
		info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = (Uint32)depth;
	info.num_levels = (Uint32)levels;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDLGPUCreateTextureResource(&info);
}

static bool SDLGPUAllocateTexture(TextureObject *object, GLenum target,
	                         int base_width, int base_height, int base_depth,
	                         GLint internal_format)
{
	if (!object || !s_device || base_width <= 0 || base_height <= 0 || base_depth <= 0)
		return false;
	int levels = SDLGPUCalculateMipLevelCount(base_width, base_height, base_depth);
	if (object->texture && object->target == target && object->width == base_width &&
		object->height == base_height && object->depth == base_depth) {
		object->internal_format = internal_format;
		return true;
	}
	if (!SDLGPUFlushPendingDraws())
		return false;
	SDLGPUReleaseTextureResources(object);
	object->target = target;
	object->width = base_width;
	object->height = base_height;
	object->depth = base_depth;
	object->levels = levels;
	object->internal_format = internal_format;
	object->texture = SDLGPUCreateTexture(target, base_width, base_height, base_depth, levels);
	object->sampler_dirty = true;
	object->level_data.clear();
	object->level_data.resize((size_t)levels);
	if (!object->texture) {
		bug("[gfxaccel-sdlgpu] texture creation failed: %s\n", SDL_GetError());
		return false;
	}
	return true;
}

static bool SDLGPUEnsureTextureTransferCapacity(Uint32 size)
{
	if (s_texture_transfer && size <= s_texture_transfer_capacity)
		return true;
	if (!SDLGPUSubmitDeferredCommand())
		return false;
	if (s_texture_transfer) {
		SDL_ReleaseGPUTransferBuffer(s_device, s_texture_transfer);
		s_texture_transfer = NULL;
	}
	Uint32 capacity = s_texture_transfer_capacity;
	if (capacity == 0)
		capacity = 4 * 1024 * 1024;
	while (capacity < size && capacity <= UINT32_MAX / 2)
		capacity *= 2;
	if (capacity < size)
		capacity = size;
	SDL_GPUTransferBufferCreateInfo transfer_info;
	std::memset(&transfer_info, 0, sizeof(transfer_info));
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = capacity;
	s_texture_transfer = SDL_CreateGPUTransferBuffer(s_device, &transfer_info);
	if (!s_texture_transfer)
		return false;
	s_texture_transfer_capacity = capacity;
	return true;
}

static void SDLGPUCopyTexturePixels(unsigned char *destination, const void *pixels,
	int width, int height, int depth, GLenum format, GLenum type, int alignment,
	GLint internal_format, bool flip_rows)
{
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
	s_transition_profile.texture_convert_count++;
#endif
	size_t size = (size_t)width * (size_t)height * (size_t)depth * 4;
	if (!pixels || type != GL_UNSIGNED_BYTE) {
		std::memset(destination, 0, size);
		if (pixels)
			SDLGPUSetError(GL_INVALID_ENUM);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		s_transition_profile.texture_convert_ticks +=
			SDLGPUProfileNow() - profile_start_tick;
#endif
		return;
	}
	int source_bpp = 4;
	if (format == GL_RGB)
		source_bpp = 3;
	int source_pitch = SDLGPUAlignInteger(width * source_bpp, alignment);
	const unsigned char *source = static_cast<const unsigned char *>(pixels);
	bool force_opaque = internal_format == GL_RGB || internal_format == GL_RGB8;
	for (int z = 0; z < depth; z++) {
		for (int y = 0; y < height; y++) {
			int destination_y = y;
			if (flip_rows)
				destination_y = height - 1 - y;
			const unsigned char *source_row = source +
				(size_t)(z * height + y) * (size_t)source_pitch;
			unsigned char *destination_row = destination +
				(size_t)(z * height + destination_y) * (size_t)width * 4;
			if (format == GL_BGRA && !force_opaque) {
				std::memcpy(destination_row, source_row, (size_t)width * 4);
				continue;
			}
			if (format == GL_BGRA) {
				for (int x = 0; x < width; x++) {
					destination_row[x * 4 + 0] = source_row[x * 4 + 0];
					destination_row[x * 4 + 1] = source_row[x * 4 + 1];
					destination_row[x * 4 + 2] = source_row[x * 4 + 2];
					destination_row[x * 4 + 3] = 255;
				}
				continue;
			}
			for (int x = 0; x < width; x++) {
				const unsigned char *pixel = source_row + x * source_bpp;
				destination_row[x * 4 + 0] = pixel[2];
				destination_row[x * 4 + 1] = pixel[1];
				destination_row[x * 4 + 2] = pixel[0];
				if (source_bpp == 4 && !force_opaque)
					destination_row[x * 4 + 3] = pixel[3];
				else
					destination_row[x * 4 + 3] = 255;
			}
		}
	}
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.texture_convert_ticks +=
		SDLGPUProfileNow() - profile_start_tick;
#endif
}

static bool SDLGPUUploadTextureRegion(TextureObject *object, int level, int x, int y,
	int z, int width, int height, int depth, const void *pixels, GLenum format,
	GLenum type, int alignment, GLint internal_format)
{
	if (!object || !object->texture || !pixels || width <= 0 || height <= 0 || depth <= 0)
		return false;
	if (!SDLGPUFlushPendingDraws())
		return false;
	Uint32 size = (Uint32)((size_t)width * (size_t)height * (size_t)depth * 4);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	if (!SDLGPUEnsureTextureTransferCapacity(size)) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileTextureUploadResult(false, profile_start_tick, size);
#else
		return false;
#endif
	}
	void *mapped = SDL_MapGPUTransferBuffer(s_device, s_texture_transfer, true);
	if (!mapped) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileTextureUploadResult(false, profile_start_tick, size);
#else
		return false;
#endif
	}
	SDLGPUCopyTexturePixels(static_cast<unsigned char *>(mapped), pixels, width,
		height, depth, format, type, alignment, internal_format,
		object->presentation_y_flip);
	SDL_UnmapGPUTransferBuffer(s_device, s_texture_transfer);
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileTextureUploadResult(false, profile_start_tick, size);
#else
		return false;
#endif
	}
	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
	if (!copy) {
		SDLGPUCancelDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileTextureUploadResult(false, profile_start_tick, size);
#else
		return false;
#endif
	}
	SDL_GPUTextureTransferInfo source;
	SDL_GPUTextureRegion destination;
	std::memset(&source, 0, sizeof(source));
	std::memset(&destination, 0, sizeof(destination));
	source.transfer_buffer = s_texture_transfer;
	source.pixels_per_row = (Uint32)width;
	source.rows_per_layer = (Uint32)height;
	int destination_y = y;
	if (object->presentation_y_flip) {
		destination_y = std::max(1, object->height >> level) - y - height;
		if (destination_y < 0)
			destination_y = 0;
	}
	destination.texture = object->texture;
	destination.mip_level = (Uint32)level;
	destination.x = (Uint32)x;
	destination.y = (Uint32)destination_y;
	destination.z = (Uint32)z;
	destination.w = (Uint32)width;
	destination.h = (Uint32)height;
	destination.d = (Uint32)depth;
	SDL_UploadToGPUTexture(copy, &source, &destination, false);
	SDL_EndGPUCopyPass(copy);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	return SDLGPUProfileTextureUploadResult(true, profile_start_tick, size);
#else
	return true;
#endif
}

static SDL_GPUSamplerAddressMode SDLGPUMapAddressMode(GLint mode)
{
	if (mode == GL_CLAMP || mode == GL_CLAMP_TO_EDGE)
		return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

static bool SDLGPUMinFilterUsesMipmaps(GLint filter)
{
	return filter == GL_NEAREST_MIPMAP_NEAREST ||
		filter == GL_LINEAR_MIPMAP_NEAREST ||
		filter == GL_NEAREST_MIPMAP_LINEAR ||
		filter == GL_LINEAR_MIPMAP_LINEAR;
}

static int SDLGPUHighestCompleteMipLevel(TextureObject *object)
{
	if (!object)
		return 0;
	int highest = -1;
	for (int level = 0; level < (int)object->level_data.size(); level++) {
		if (!object->level_data[(size_t)level].defined)
			break;
		highest = level;
	}
	if (highest < 0)
		highest = 0;
	return highest;
}

static SDL_GPUSampler *SDLGPUGetTextureSampler(TextureObject *object)
{
	if (!object)
		return NULL;
	if (object->sampler && !object->sampler_dirty)
		return object->sampler;
	if (object->sampler) {
		SDLGPUSubmitDeferredCommand();
		SDL_ReleaseGPUSampler(s_device, object->sampler);
		object->sampler = NULL;
	}
	SDL_GPUSamplerCreateInfo info;
	std::memset(&info, 0, sizeof(info));
	info.min_filter = SDL_GPU_FILTER_NEAREST;
	if (object->min_filter == GL_LINEAR || object->min_filter == GL_LINEAR_MIPMAP_LINEAR ||
		object->min_filter == GL_LINEAR_MIPMAP_NEAREST)
		info.min_filter = SDL_GPU_FILTER_LINEAR;
	info.mag_filter = SDL_GPU_FILTER_NEAREST;
	if (object->mag_filter == GL_LINEAR)
		info.mag_filter = SDL_GPU_FILTER_LINEAR;
	info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	if (object->min_filter == GL_LINEAR_MIPMAP_LINEAR ||
		object->min_filter == GL_NEAREST_MIPMAP_LINEAR)
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	info.address_mode_u = SDLGPUMapAddressMode(object->wrap_s);
	info.address_mode_v = SDLGPUMapAddressMode(object->wrap_t);
	info.address_mode_w = SDLGPUMapAddressMode(object->wrap_r);
	info.mip_lod_bias = object->lod_bias;
	info.min_lod = 0.0f;
	info.max_lod = 0.0f;
	if (SDLGPUMinFilterUsesMipmaps(object->min_filter)) {
		int max_level = SDLGPUHighestCompleteMipLevel(object);
		if (object->max_level < max_level)
			max_level = object->max_level;
		if (max_level < 0)
			max_level = 0;
		info.max_lod = (float)max_level;
	}
	object->sampler = SDL_CreateGPUSampler(s_device, &info);
	object->sampler_dirty = false;
	return object->sampler;
}

static SDL_GPUShader *SDLGPUCreateRawShader(SDL_GPUShaderStage stage,
	                                   const char *entrypoint,
	                                   SDL_GPUShaderFormat format,
	                                   const unsigned char *code,
	                                   size_t code_size,
	                                   Uint32 num_samplers,
	                                   Uint32 num_uniform_buffers)
{
	SDL_GPUShaderCreateInfo info;
	std::memset(&info, 0, sizeof(info));
	info.code_size = code_size;
	info.code = code;
	info.entrypoint = entrypoint;
	info.format = format;
	info.stage = stage;
	info.num_samplers = num_samplers;
	info.num_uniform_buffers = num_uniform_buffers;
	return SDL_CreateGPUShader(s_device, &info);
}

static SDL_GPUShader *SDLGPUCompileRawShader(SDL_GPUShaderStage stage,
	                                    const char *entrypoint,
	                                    Uint32 num_samplers,
	                                    Uint32 num_uniform_buffers)
{
	SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(s_device);
	SDL_GPUShader *shader = NULL;
	if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
		if (stage == SDL_GPU_SHADERSTAGE_VERTEX)
			shader = SDLGPUCreateRawShader(stage, entrypoint, SDL_GPU_SHADERFORMAT_SPIRV,
				s_ffp_vs_spirv, sizeof(s_ffp_vs_spirv), num_samplers,
				num_uniform_buffers);
		else
			shader = SDLGPUCreateRawShader(stage, entrypoint, SDL_GPU_SHADERFORMAT_SPIRV,
				s_ffp_ps_spirv, sizeof(s_ffp_ps_spirv), num_samplers,
				num_uniform_buffers);
		if (shader)
			return shader;
	}
	if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
		if (stage == SDL_GPU_SHADERSTAGE_VERTEX)
			shader = SDLGPUCreateRawShader(stage, entrypoint, SDL_GPU_SHADERFORMAT_DXIL,
				s_ffp_vs_dxil, sizeof(s_ffp_vs_dxil), num_samplers,
				num_uniform_buffers);
		else
			shader = SDLGPUCreateRawShader(stage, entrypoint, SDL_GPU_SHADERFORMAT_DXIL,
				s_ffp_ps_dxil, sizeof(s_ffp_ps_dxil), num_samplers,
				num_uniform_buffers);
		if (shader)
			return shader;
	}
	if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		shader = SDLGPUCreateRawShader(stage, entrypoint, SDL_GPU_SHADERFORMAT_MSL,
			reinterpret_cast<const unsigned char *>(s_ffp_msl_shader),
			sizeof(s_ffp_msl_shader) - 1, num_samplers, num_uniform_buffers);
		if (shader)
			return shader;
	}
	return NULL;
}

#if defined(GFXACCEL_USE_SHADERCROSS)
static SDL_GPUShader *SDLGPUCompileShaderCrossShader(SDL_GPUShaderStage stage,
	                                            const char *entrypoint,
	                                            Uint32 num_samplers,
	                                            Uint32 num_uniform_buffers)
{
	SDL_ShaderCross_ShaderStage cross_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
	if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT)
		cross_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
	SDL_ShaderCross_HLSL_Info hlsl;
	std::memset(&hlsl, 0, sizeof(hlsl));
	SDL_ShaderCross_HLSL_Define defines[2];
	std::memset(defines, 0, sizeof(defines));
	char spirv_define[] = "SPIRV";
	char spirv_value[] = "1";
	hlsl.source = s_ffp_shader;
	hlsl.entrypoint = entrypoint;
	hlsl.shader_stage = cross_stage;
	if (stage == SDL_GPU_SHADERSTAGE_VERTEX) {
		defines[0].name = spirv_define;
		defines[0].value = spirv_value;
		hlsl.defines = defines;
	}
	size_t spirv_size = 0;
	void *spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &spirv_size);
	if (!spirv) {
		bug("[gfxaccel-sdlgpu] shader compile failed: %s\n", SDL_GetError());
		return NULL;
	}
	SDL_ShaderCross_SPIRV_Info info;
	std::memset(&info, 0, sizeof(info));
	info.bytecode = static_cast<const Uint8 *>(spirv);
	info.bytecode_size = spirv_size;
	info.entrypoint = entrypoint;
	info.shader_stage = cross_stage;
	SDL_ShaderCross_GraphicsShaderResourceInfo resources;
	std::memset(&resources, 0, sizeof(resources));
	resources.num_samplers = num_samplers;
	resources.num_uniform_buffers = num_uniform_buffers;
	SDL_GPUShader *shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
		s_device, &info, &resources, 0);
	SDL_free(spirv);
	if (!shader)
		bug("[gfxaccel-sdlgpu] shader translation failed: %s\n", SDL_GetError());
	return shader;
}
#endif

static SDL_GPUShader *SDLGPUCompileShader(SDL_GPUShaderStage stage,
	                                const char *entrypoint,
	                                Uint32 num_samplers,
	                                Uint32 num_uniform_buffers)
{
	SDL_GPUShader *shader = SDLGPUCompileRawShader(stage, entrypoint, num_samplers,
		num_uniform_buffers);
	if (shader)
		return shader;
#if defined(GFXACCEL_USE_SHADERCROSS)
	if (s_shadercross_initialized)
		return SDLGPUCompileShaderCrossShader(stage, entrypoint, num_samplers,
			num_uniform_buffers);
#endif
	bug("[gfxaccel-sdlgpu] raw shader creation failed: %s\n",
		SDL_GetError());
	return NULL;
}

static SDL_GPUTexture *SDLGPUCreateDepthTarget(int width, int height)
{
	SDL_GPUTextureCreateInfo info;
	std::memset(&info, 0, sizeof(info));
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
	info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	info.width = (Uint32)width;
	info.height = (Uint32)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDLGPUCreateTextureResource(&info);
}

static bool SDLGPUEnsureDefaultTargets()
{
	if (!s_device || !s_window)
		return false;
	int width = 0;
	int height = 0;
	if (!SDL_GetWindowSizeInPixels(s_window, &width, &height) || width <= 0 || height <= 0)
		return false;
	if (s_default_color && s_default_depth && width == s_default_width &&
		height == s_default_height)
		return true;
	if (!SDLGPUFlushPendingDraws())
		return false;
	if (s_default_color)
		SDLGPUReleaseTextureResource(s_default_color);
	if (s_default_depth)
		SDLGPUReleaseTextureResource(s_default_depth);
	s_default_color = SDLGPUCreateTexture(GL_TEXTURE_2D, width, height, 1, 1);
	s_default_depth = SDLGPUCreateDepthTarget(width, height);
	s_default_width = width;
	s_default_height = height;
	if (!s_default_color || !s_default_depth)
		return false;
	return true;
}

static void SDLGPUReleaseAllResources()
{
	if (!s_device)
		return;
	SDLGPUFlushPendingDraws();
	SDLGPUSubmitDeferredCommand();
	s_pending_draws.clear();
	s_pending_vertex_bytes = 0;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 finish_start_tick = SDLGPUProfileNow();
#endif
	SDL_WaitForGPUIdle(s_device);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.finish_ticks += SDLGPUProfileNow() - finish_start_tick;
	s_transition_profile.finish_count++;
#endif
	std::map<PipelineKey, SDL_GPUGraphicsPipeline *>::iterator pipeline;
	for (pipeline = s_pipelines.begin(); pipeline != s_pipelines.end(); ++pipeline)
		SDL_ReleaseGPUGraphicsPipeline(s_device, pipeline->second);
	s_pipelines.clear();
	std::map<GLuint, TextureObject *>::iterator texture;
	for (texture = s_textures.begin(); texture != s_textures.end(); ++texture) {
		SDLGPUReleaseTextureResources(texture->second);
		delete texture->second;
	}
	s_textures.clear();
	std::map<GLuint, RenderbufferObject *>::iterator renderbuffer;
	for (renderbuffer = s_renderbuffers.begin(); renderbuffer != s_renderbuffers.end(); ++renderbuffer) {
		if (renderbuffer->second->texture)
			SDLGPUReleaseTextureResource(renderbuffer->second->texture);
		delete renderbuffer->second;
	}
	s_renderbuffers.clear();
	std::map<GLuint, FramebufferObject *>::iterator framebuffer;
	for (framebuffer = s_framebuffers.begin(); framebuffer != s_framebuffers.end(); ++framebuffer)
		delete framebuffer->second;
	s_framebuffers.clear();
	if (s_default_color)
		SDLGPUReleaseTextureResource(s_default_color);
	if (s_default_depth)
		SDLGPUReleaseTextureResource(s_default_depth);
	if (s_vertex_buffer)
		SDL_ReleaseGPUBuffer(s_device, s_vertex_buffer);
	if (s_vertex_transfer)
		SDL_ReleaseGPUTransferBuffer(s_device, s_vertex_transfer);
	if (s_texture_transfer)
		SDL_ReleaseGPUTransferBuffer(s_device, s_texture_transfer);
	if (s_vertex_shader)
		SDL_ReleaseGPUShader(s_device, s_vertex_shader);
	if (s_fragment_shader)
		SDL_ReleaseGPUShader(s_device, s_fragment_shader);
	s_default_color = NULL;
	s_default_depth = NULL;
	s_vertex_buffer = NULL;
	s_vertex_transfer = NULL;
	s_texture_transfer = NULL;
	s_texture_transfer_capacity = s_vertex_capacity;
	s_vertex_shader = NULL;
	s_fragment_shader = NULL;
	s_white_2d = NULL;
	s_white_3d = NULL;
}

static bool SDLGPUCreateWhiteTexture(GLenum target, TextureObject **output)
{
	TextureObject *object = new TextureObject;
	object->target = target;
	if (!SDLGPUAllocateTexture(object, target, 1, 1, 1, GL_RGBA8)) {
		delete object;
		return false;
	}
	unsigned char white[4] = {255, 255, 255, 255};
	if (!SDLGPUUploadTextureRegion(object, 0, 0, 0, 0, 1, 1, 1, white,
		GL_BGRA, GL_UNSIGNED_BYTE, 4, GL_RGBA8)) {
		SDLGPUReleaseTextureResources(object);
		delete object;
		return false;
	}
	*output = object;
	return true;
}

static bool SDLGPUAttachWindow(SDL_Window *window)
{
	if (!s_device || !window)
		return false;
	if (s_window == window)
		return true;
	if (s_window) {
		SDLGPUFlushPendingDraws();
		SDLGPUSubmitDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		Uint64 release_start_tick = SDLGPUProfileNow();
#endif
		SDL_ReleaseWindowFromGPUDevice(s_device, s_window);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		s_transition_profile.window_release_ticks +=
			SDLGPUProfileNow() - release_start_tick;
		s_transition_profile.window_release_count++;
#endif
		s_window = NULL;
	}
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 claim_start_tick = SDLGPUProfileNow();
#endif
	bool claimed = SDL_ClaimWindowForGPUDevice(s_device, window);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.window_claim_ticks += SDLGPUProfileNow() - claim_start_tick;
	s_transition_profile.window_claim_count++;
#endif
	if (!claimed) {
		bug("[gfxaccel-sdlgpu] window claim failed: %s\n", SDL_GetError());
		return false;
	}
	s_window = window;
	return true;
}

static bool SDLGPUInitializeDevice(SDL_Window *window)
{
	if (s_device) {
		if (!SDLGPUAttachWindow(window))
			return false;
		return SDLGPUEnsureDefaultTargets();
	}
	if (!window)
		return false;
	SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV |
		SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
#if defined(GFXACCEL_USE_SHADERCROSS)
	s_shadercross_initialized = SDL_ShaderCross_Init();
	if (s_shadercross_initialized)
		formats |= SDL_ShaderCross_GetSPIRVShaderFormats();
	else
		bug("[gfxaccel-sdlgpu] optional shadercross unavailable: %s\n",
			SDL_GetError());

#endif
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 device_start_tick = SDLGPUProfileNow();
#endif
	s_device = SDL_CreateGPUDevice(formats, false, NULL);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.device_create_ticks += SDLGPUProfileNow() - device_start_tick;
	s_transition_profile.device_create_count++;
#endif
	if (!s_device) {
		bug("[gfxaccel-sdlgpu] device creation failed: %s\n", SDL_GetError());
#if defined(GFXACCEL_USE_SHADERCROSS)
		if (s_shadercross_initialized) {
			SDL_ShaderCross_Quit();
			s_shadercross_initialized = false;
		}
#endif
		return false;
	}
	if (!SDLGPUAttachWindow(window)) {
		SDL_DestroyGPUDevice(s_device);
		s_device = NULL;
#if defined(GFXACCEL_USE_SHADERCROSS)
		if (s_shadercross_initialized) {
			SDL_ShaderCross_Quit();
			s_shadercross_initialized = false;
		}
#endif
		return false;
	}
	SDL_SetGPUAllowedFramesInFlight(s_device, 2);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 shader_start_tick = SDLGPUProfileNow();
#endif
	s_vertex_shader = SDLGPUCompileShader(SDL_GPU_SHADERSTAGE_VERTEX, "VSMain", 0, 0);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.shader_create_ticks += SDLGPUProfileNow() - shader_start_tick;
	s_transition_profile.shader_create_count++;
	shader_start_tick = SDLGPUProfileNow();
#endif
	s_fragment_shader = SDLGPUCompileShader(SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", 3, 1);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.shader_create_ticks += SDLGPUProfileNow() - shader_start_tick;
	s_transition_profile.shader_create_count++;
#endif
	SDL_GPUBufferCreateInfo buffer_info;
	std::memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = s_vertex_capacity;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 buffer_start_tick = SDLGPUProfileNow();
#endif
	s_vertex_buffer = SDL_CreateGPUBuffer(s_device, &buffer_info);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.buffer_create_ticks += SDLGPUProfileNow() - buffer_start_tick;
	s_transition_profile.buffer_create_count++;
#endif
	SDL_GPUTransferBufferCreateInfo transfer_info;
	std::memset(&transfer_info, 0, sizeof(transfer_info));
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = s_vertex_capacity;
	s_texture_transfer_capacity = s_vertex_capacity;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	buffer_start_tick = SDLGPUProfileNow();
#endif
	s_vertex_transfer = SDL_CreateGPUTransferBuffer(s_device, &transfer_info);
	s_texture_transfer = SDL_CreateGPUTransferBuffer(s_device, &transfer_info);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.buffer_create_ticks += SDLGPUProfileNow() - buffer_start_tick;
	s_transition_profile.buffer_create_count += 2;
#endif
	if (!s_vertex_shader || !s_fragment_shader || !s_vertex_buffer || !s_vertex_transfer ||
		!s_texture_transfer ||
		!SDLGPUEnsureDefaultTargets() || !SDLGPUCreateWhiteTexture(GL_TEXTURE_2D, &s_white_2d) ||
		!SDLGPUCreateWhiteTexture(GL_TEXTURE_3D, &s_white_3d)) {
		SDLGPUReleaseAllResources();
		SDL_ReleaseWindowFromGPUDevice(s_device, window);
		SDL_DestroyGPUDevice(s_device);
		s_device = NULL;
		s_window = NULL;
#if defined(GFXACCEL_USE_SHADERCROSS)
		if (s_shadercross_initialized) {
			SDL_ShaderCross_Quit();
			s_shadercross_initialized = false;
		}
#endif
		return false;
	}
	const char *driver = SDL_GetGPUDeviceDriver(s_device);
	if (!driver)
		driver = "unknown";
	bug("[gfxaccel-sdlgpu] SDL-GPU ready: %s\n", driver);
	return true;
}

static SDL_GPUCompareOp SDLGPUMapCompareOp(GLenum function)
{
	switch (function) {
	case GL_NEVER: return SDL_GPU_COMPAREOP_NEVER;
	case GL_LESS: return SDL_GPU_COMPAREOP_LESS;
	case GL_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
	case GL_LEQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	case GL_GREATER: return SDL_GPU_COMPAREOP_GREATER;
	case GL_NOTEQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
	case GL_GEQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
	default: return SDL_GPU_COMPAREOP_ALWAYS;
	}
}

static SDL_GPUBlendFactor SDLGPUMapBlendFactor(GLenum factor)
{
	switch (factor) {
	case GL_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
	case GL_ONE: return SDL_GPU_BLENDFACTOR_ONE;
	case GL_SRC_COLOR: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
	case GL_SRC_ALPHA_SATURATE: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
	default: return SDL_GPU_BLENDFACTOR_ONE;
	}
}

static SDL_GPUBlendOp SDLGPUMapBlendOp(GLenum operation)
{
#ifdef GL_FUNC_SUBTRACT
	if (operation == GL_FUNC_SUBTRACT)
		return SDL_GPU_BLENDOP_SUBTRACT;
#endif
#ifdef GL_FUNC_REVERSE_SUBTRACT
	if (operation == GL_FUNC_REVERSE_SUBTRACT)
		return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
#endif
#ifdef GL_MIN
	if (operation == GL_MIN)
		return SDL_GPU_BLENDOP_MIN;
#endif
#ifdef GL_MAX
	if (operation == GL_MAX)
		return SDL_GPU_BLENDOP_MAX;
#endif
	return SDL_GPU_BLENDOP_ADD;
}

static SDL_GPUStencilOp SDLGPUMapStencilOp(GLenum operation)
{
	switch (operation) {
	case GL_ZERO: return SDL_GPU_STENCILOP_ZERO;
	case GL_REPLACE: return SDL_GPU_STENCILOP_REPLACE;
	case GL_INCR: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
	case GL_DECR: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
	case GL_INVERT: return SDL_GPU_STENCILOP_INVERT;
#ifdef GL_INCR_WRAP
	case GL_INCR_WRAP: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
#endif
#ifdef GL_DECR_WRAP
	case GL_DECR_WRAP: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
#endif
	default: return SDL_GPU_STENCILOP_KEEP;
	}
}

static SDL_GPUPrimitiveType SDLGPUMapPrimitive(GLenum mode)
{
	if (mode == GL_LINES)
		return SDL_GPU_PRIMITIVETYPE_LINELIST;
	if (mode == GL_LINE_STRIP)
		return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
	if (mode == GL_POINTS)
		return SDL_GPU_PRIMITIVETYPE_POINTLIST;
	if (mode == GL_TRIANGLE_STRIP)
		return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
	return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

static void SDLGPUBuildPipelineKey(PipelineKey &key, GLenum mode, bool has_depth)
{
	std::memset(&key, 0, sizeof(key));
	key.primitive = (Uint32)SDLGPUMapPrimitive(mode);
	key.blend_enabled = s_state.blend_enabled;
	key.blend_src_rgb = (Uint32)s_state.blend_src_rgb;
	key.blend_dst_rgb = (Uint32)s_state.blend_dst_rgb;
	key.blend_src_alpha = (Uint32)s_state.blend_src_alpha;
	key.blend_dst_alpha = (Uint32)s_state.blend_dst_alpha;
	key.blend_equation = (Uint32)s_state.blend_equation;
	for (int channel = 0; channel < 4; channel++) {
		if (s_state.color_mask[channel])
			key.color_mask |= 1u << channel;
	}
	key.depth_test = s_state.depth_test_enabled;
	key.depth_write = s_state.depth_write;
	key.depth_func = (Uint32)s_state.depth_func;
	key.stencil_test = s_state.stencil_test_enabled;
	key.stencil_func = (Uint32)s_state.stencil_func;
	key.stencil_fail = (Uint32)s_state.stencil_fail;
	key.stencil_depth_fail = (Uint32)s_state.stencil_depth_fail;
	key.stencil_pass = (Uint32)s_state.stencil_pass;
	key.stencil_value_mask = s_state.stencil_value_mask & 0xffu;
	key.stencil_write_mask = s_state.stencil_write_mask & 0xffu;
	if (s_state.cull_enabled)
		key.cull_mode = (Uint32)s_state.cull_face;
	key.front_face = (Uint32)s_state.front_face;
	key.fill_mode = (Uint32)s_state.polygon_mode;
	key.depth_bias = s_state.polygon_offset_enabled;
	key.has_depth = has_depth;
}

static SDL_GPUGraphicsPipeline *SDLGPUGetPipeline(GLenum mode, bool has_depth)
{
	PipelineKey key;
	SDLGPUBuildPipelineKey(key, mode, has_depth);
	std::map<PipelineKey, SDL_GPUGraphicsPipeline *>::iterator found = s_pipelines.find(key);
	if (found != s_pipelines.end())
		return found->second;
	SDL_GPUVertexBufferDescription buffer_description;
	std::memset(&buffer_description, 0, sizeof(buffer_description));
	buffer_description.slot = 0;
	buffer_description.pitch = sizeof(CompatVertex);
	buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	SDL_GPUVertexAttribute attributes[7];
	std::memset(attributes, 0, sizeof(attributes));
	for (Uint32 index = 0; index < 7; index++) {
		attributes[index].location = index;
		attributes[index].buffer_slot = 0;
		attributes[index].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	}
	attributes[0].offset = (Uint32)offsetof(CompatVertex, position);
	attributes[1].offset = (Uint32)offsetof(CompatVertex, color);
	attributes[2].offset = (Uint32)offsetof(CompatVertex, secondary);
	attributes[3].offset = (Uint32)offsetof(CompatVertex, texcoord0);
	attributes[4].offset = (Uint32)offsetof(CompatVertex, texcoord1);
	attributes[5].offset = (Uint32)offsetof(CompatVertex, fog_coord);
	attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
	attributes[6].offset = (Uint32)offsetof(CompatVertex, eye_position);
	SDL_GPUColorTargetDescription color_description;
	std::memset(&color_description, 0, sizeof(color_description));
	color_description.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	color_description.blend_state.enable_blend = s_state.blend_enabled;
	color_description.blend_state.src_color_blendfactor = SDLGPUMapBlendFactor(s_state.blend_src_rgb);
	color_description.blend_state.dst_color_blendfactor = SDLGPUMapBlendFactor(s_state.blend_dst_rgb);
	color_description.blend_state.src_alpha_blendfactor = SDLGPUMapBlendFactor(s_state.blend_src_alpha);
	color_description.blend_state.dst_alpha_blendfactor = SDLGPUMapBlendFactor(s_state.blend_dst_alpha);
	color_description.blend_state.color_blend_op = SDLGPUMapBlendOp(s_state.blend_equation);
	color_description.blend_state.alpha_blend_op = SDLGPUMapBlendOp(s_state.blend_equation);
	color_description.blend_state.enable_color_write_mask = true;
	color_description.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)key.color_mask;
	SDL_GPUGraphicsPipelineCreateInfo info;
	std::memset(&info, 0, sizeof(info));
	info.vertex_shader = s_vertex_shader;
	info.fragment_shader = s_fragment_shader;
	info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
	info.vertex_input_state.num_vertex_buffers = 1;
	info.vertex_input_state.vertex_attributes = attributes;
	info.vertex_input_state.num_vertex_attributes = 7;
	info.primitive_type = SDLGPUMapPrimitive(mode);
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	if (s_state.polygon_mode == GL_LINE)
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	if (s_state.cull_enabled) {
		if (s_state.cull_face == GL_FRONT)
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
		else
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	}
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	if (s_state.front_face == GL_CW)
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
	info.rasterizer_state.enable_depth_clip = true;
	info.rasterizer_state.enable_depth_bias = s_state.polygon_offset_enabled;
	info.rasterizer_state.depth_bias_constant_factor = s_state.polygon_offset_units;
	info.rasterizer_state.depth_bias_slope_factor = s_state.polygon_offset_factor;
	info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.depth_stencil_state.enable_depth_test = has_depth && s_state.depth_test_enabled;
	info.depth_stencil_state.enable_depth_write = has_depth && s_state.depth_write;
	info.depth_stencil_state.compare_op = SDLGPUMapCompareOp(s_state.depth_func);
	info.depth_stencil_state.enable_stencil_test = has_depth && s_state.stencil_test_enabled;
	info.depth_stencil_state.compare_mask = (Uint8)s_state.stencil_value_mask;
	info.depth_stencil_state.write_mask = (Uint8)s_state.stencil_write_mask;
	info.depth_stencil_state.front_stencil_state.compare_op = SDLGPUMapCompareOp(s_state.stencil_func);
	info.depth_stencil_state.front_stencil_state.fail_op = SDLGPUMapStencilOp(s_state.stencil_fail);
	info.depth_stencil_state.front_stencil_state.depth_fail_op = SDLGPUMapStencilOp(s_state.stencil_depth_fail);
	info.depth_stencil_state.front_stencil_state.pass_op = SDLGPUMapStencilOp(s_state.stencil_pass);
	info.depth_stencil_state.back_stencil_state = info.depth_stencil_state.front_stencil_state;
	info.target_info.color_target_descriptions = &color_description;
	info.target_info.num_color_targets = 1;
	info.target_info.has_depth_stencil_target = has_depth;
	info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(s_device, &info);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.pipeline_create_ticks += SDLGPUProfileNow() - profile_start_tick;
	s_transition_profile.pipeline_create_count++;
#endif
	if (!pipeline) {
		bug("[gfxaccel-sdlgpu] pipeline creation failed: %s\n", SDL_GetError());
		return NULL;
	}
	s_pipelines[key] = pipeline;
	return pipeline;
}

static bool SDLGPUGetCurrentTargets(SDL_GPUTexture **color, SDL_GPUTexture **depth,
	                        int *width, int *height, int *color_level)
{
	*color = NULL;
	*depth = NULL;
	*width = 0;
	*height = 0;
	*color_level = 0;
	if (s_bound_framebuffer == 0) {
		if (!SDLGPUEnsureDefaultTargets())
			return false;
		*color = s_default_color;
		*depth = s_default_depth;
		*width = s_default_width;
		*height = s_default_height;
		return true;
	}
	std::map<GLuint, FramebufferObject *>::iterator found =
		s_framebuffers.find(s_bound_framebuffer);
	if (found == s_framebuffers.end())
		return false;
	FramebufferObject *framebuffer = found->second;
	TextureObject *texture = SDLGPUFindTexture(framebuffer->color_texture);
	if (!texture || !texture->texture)
		return false;
	*color = texture->texture;
	int level = framebuffer->color_level;
	*color_level = level;
	*width = std::max(1, texture->width >> level);
	*height = std::max(1, texture->height >> level);
	std::map<GLuint, RenderbufferObject *>::iterator depth_found =
		s_renderbuffers.find(framebuffer->depth_renderbuffer);
	if (depth_found != s_renderbuffers.end())
		*depth = depth_found->second->texture;
	return true;
}

static TextureObject *SDLGPUGetSelectedTexture(int unit, bool texture_3d);

static void SDLGPUFillFragmentUniforms(FragmentUniforms &uniforms)
{
	std::memset(&uniforms, 0, sizeof(uniforms));
	for (int channel = 0; channel < 4; channel++) {
		uniforms.data[0][channel] = s_state.fog_color[channel];
		uniforms.data[1][channel] = s_state.texenv[0].color[channel];
		uniforms.data[2][channel] = s_state.texenv[1].color[channel];
	}
	uniforms.data[3][0] = s_state.alpha_ref;
	uniforms.data[3][1] = s_state.fog_density;
	uniforms.data[3][2] = s_state.fog_start;
	uniforms.data[3][3] = s_state.fog_end;
	uniforms.data[4][0] = (float)s_state.fog_mode;
	uniforms.data[4][1] = (float)s_state.alpha_func;
	int clip_mask = 0;
	for (int plane = 0; plane < 6; plane++) {
		if (s_state.clip_enabled[plane])
			clip_mask |= 1 << plane;
	}
	uniforms.data[4][2] = (float)clip_mask;
	if (s_state.color_sum_enabled)
		uniforms.data[4][3] = 1.0f;
	for (int unit = 0; unit < 2; unit++) {
		TextureEnvironment environment = s_state.texenv[unit];
		if (environment.mode == GL_BLEND) {
			environment.mode = GL_COMBINE;
			environment.combine_rgb = GL_INTERPOLATE;
			environment.combine_alpha = GL_MODULATE;
			environment.source_rgb[0] = GL_CONSTANT;
			environment.source_rgb[1] = GL_PREVIOUS;
			environment.source_rgb[2] = GL_TEXTURE;
			environment.operand_rgb[0] = GL_SRC_COLOR;
			environment.operand_rgb[1] = GL_SRC_COLOR;
			environment.operand_rgb[2] = GL_SRC_COLOR;
			environment.source_alpha[0] = GL_PREVIOUS;
			environment.source_alpha[1] = GL_TEXTURE;
		} else if (environment.mode == GL_ADD) {
			environment.mode = GL_COMBINE;
			environment.combine_rgb = GL_ADD;
			environment.combine_alpha = GL_MODULATE;
			environment.source_rgb[0] = GL_PREVIOUS;
			environment.source_rgb[1] = GL_TEXTURE;
			environment.operand_rgb[0] = GL_SRC_COLOR;
			environment.operand_rgb[1] = GL_SRC_COLOR;
			environment.source_alpha[0] = GL_PREVIOUS;
			environment.source_alpha[1] = GL_TEXTURE;
		}
		int flags = 5 + unit;
		if (s_state.texture_2d_enabled[unit])
			uniforms.data[flags][0] = 1.0f;
		if (s_state.texture_3d_enabled[unit])
			uniforms.data[flags][1] = 1.0f;
		uniforms.data[flags][2] = (float)environment.mode;
		uniforms.data[flags][3] = (float)environment.combine_rgb;
		int combine = 7 + unit;
		uniforms.data[combine][0] = (float)environment.combine_alpha;
		uniforms.data[combine][1] = environment.rgb_scale;
		uniforms.data[combine][2] = environment.alpha_scale;
		int source = 9;
		if (unit == 1)
			source = 12;
		uniforms.data[source][0] = (float)environment.source_rgb[0];
		uniforms.data[source][1] = (float)environment.source_rgb[1];
		uniforms.data[source][2] = (float)environment.source_rgb[2];
		uniforms.data[source][3] = (float)environment.source_alpha[0];
		uniforms.data[source + 1][0] = (float)environment.source_alpha[1];
		uniforms.data[source + 1][1] = (float)environment.operand_rgb[0];
		uniforms.data[source + 1][2] = (float)environment.operand_rgb[1];
		uniforms.data[source + 1][3] = (float)environment.operand_rgb[2];
		uniforms.data[source + 2][0] = (float)environment.operand_alpha[0];
		uniforms.data[source + 2][1] = (float)environment.operand_alpha[1];
	}
	if (s_state.alpha_test_enabled)
		uniforms.data[15][0] = 1.0f;
	if (s_state.fog_enabled)
		uniforms.data[15][1] = 1.0f;
	TextureObject *bias_texture0 = SDLGPUGetSelectedTexture(0, false);
	TextureObject *bias_texture1 = SDLGPUGetSelectedTexture(1, false);
	if (!s_state.texture_2d_enabled[0] && s_state.texture_3d_enabled[0])
		bias_texture0 = SDLGPUGetSelectedTexture(0, true);
	if (bias_texture0)
		uniforms.data[15][2] = bias_texture0->lod_bias;
	if (bias_texture1)
		uniforms.data[15][3] = bias_texture1->lod_bias;
	for (int plane = 0; plane < 6; plane++) {
		for (int component = 0; component < 4; component++)
			uniforms.data[16 + plane][component] = (float)s_state.clip_plane[plane][component];
	}
}

static TextureObject *SDLGPUGetSelectedTexture(int unit, bool texture_3d)
{
	GLuint name = s_state.bound_texture_2d[unit];
	if (texture_3d)
		name = s_state.bound_texture_3d[unit];
	TextureObject *object = SDLGPUFindTexture(name);
	if (!object || !object->texture) {
		if (texture_3d)
			return s_white_3d;
		return s_white_2d;
	}
	return object;
}

static bool SDLGPUUploadPendingVertices(SDL_GPUCommandBuffer *command)
{
	if (s_pending_vertex_bytes == 0 || s_pending_vertex_bytes > s_vertex_capacity)
		return false;
	void *mapped = SDL_MapGPUTransferBuffer(s_device, s_vertex_transfer, true);
	if (!mapped)
		return false;
	unsigned char *destination_bytes = static_cast<unsigned char *>(mapped);
	for (size_t index = 0; index < s_pending_draws.size(); index++) {
		const PendingDraw &draw = s_pending_draws[index];
		Uint32 size = (Uint32)(draw.vertices.size() * sizeof(CompatVertex));
		std::memcpy(destination_bytes + draw.vertex_offset, &draw.vertices[0], size);
	}
	SDL_UnmapGPUTransferBuffer(s_device, s_vertex_transfer);
	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
	if (!copy)
		return false;
	SDL_GPUTransferBufferLocation source;
	SDL_GPUBufferRegion destination;
	std::memset(&source, 0, sizeof(source));
	std::memset(&destination, 0, sizeof(destination));
	source.transfer_buffer = s_vertex_transfer;
	destination.buffer = s_vertex_buffer;
	destination.size = s_pending_vertex_bytes;
	SDL_UploadToGPUBuffer(copy, &source, &destination, true);
	SDL_EndGPUCopyPass(copy);
	return true;
}

static bool SDLGPUFlushPendingDraws()
{
	if (s_pending_draws.empty())
		return true;
	if (!s_device)
		return false;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
	s_transition_profile.draw_flush_count++;
	s_transition_profile.draw_count += (Uint32)s_pending_draws.size();
	for (size_t profile_index = 0; profile_index < s_pending_draws.size();
		profile_index++)
		s_transition_profile.draw_vertices +=
			(Uint64)s_pending_draws[profile_index].vertices.size();
#endif
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		s_transition_profile.draw_flush_ticks +=
			SDLGPUProfileNow() - profile_start_tick;
#endif
		return false;
	}
	if (!SDLGPUUploadPendingVertices(command)) {
		SDLGPUCancelDeferredCommand();
		s_pending_draws.clear();
		s_pending_vertex_bytes = 0;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		s_transition_profile.draw_flush_ticks +=
			SDLGPUProfileNow() - profile_start_tick;
#endif
		return false;
	}
	size_t first = 0;
	while (first < s_pending_draws.size()) {
		const PendingDraw &group = s_pending_draws[first];
		size_t last = first + 1;
		while (last < s_pending_draws.size()) {
			const PendingDraw &next = s_pending_draws[last];
			if (next.color != group.color || next.depth != group.depth ||
				next.color_level != group.color_level)
				break;
			last++;
		}
		SDL_GPUColorTargetInfo color_info;
		std::memset(&color_info, 0, sizeof(color_info));
		color_info.texture = group.color;
		color_info.mip_level = (Uint32)group.color_level;
		color_info.load_op = SDL_GPU_LOADOP_LOAD;
		color_info.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo depth_info;
		std::memset(&depth_info, 0, sizeof(depth_info));
		depth_info.texture = group.depth;
		depth_info.load_op = SDL_GPU_LOADOP_LOAD;
		depth_info.store_op = SDL_GPU_STOREOP_STORE;
		depth_info.stencil_load_op = SDL_GPU_LOADOP_LOAD;
		depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
		const SDL_GPUDepthStencilTargetInfo *depth_pointer = NULL;
		if (group.depth)
			depth_pointer = &depth_info;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color_info, 1,
		                                                depth_pointer);
		if (!pass) {
			SDLGPUCancelDeferredCommand();
			s_pending_draws.clear();
			s_pending_vertex_bytes = 0;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
			s_transition_profile.draw_flush_ticks +=
				SDLGPUProfileNow() - profile_start_tick;
#endif
			return false;
		}
		for (size_t index = first; index < last; index++) {
			const PendingDraw &draw = s_pending_draws[index];
			SDL_PushGPUFragmentUniformData(command, 0, &draw.uniforms,
			                               sizeof(draw.uniforms));
			SDL_BindGPUGraphicsPipeline(pass, draw.pipeline);
			SDL_GPUBufferBinding vertex_binding;
			vertex_binding.buffer = s_vertex_buffer;
			vertex_binding.offset = draw.vertex_offset;
			SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
			SDL_GPUTextureSamplerBinding bindings[3];
			for (int unit = 0; unit < 3; unit++) {
				bindings[unit].texture = draw.textures[unit]->texture;
				bindings[unit].sampler = draw.samplers[unit];
			}
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
			SDL_GPUViewport viewport;
			viewport.x = (float)draw.state.viewport[0];
			viewport.y = (float)(draw.target_height - draw.state.viewport[1] -
				draw.state.viewport[3]);
			viewport.w = (float)draw.state.viewport[2];
			viewport.h = (float)draw.state.viewport[3];
			viewport.min_depth = (float)draw.state.depth_near;
			viewport.max_depth = (float)draw.state.depth_far;
			SDL_SetGPUViewport(pass, &viewport);
			SDL_Rect scissor;
			if (draw.state.scissor_enabled) {
				scissor.x = draw.state.scissor[0];
				scissor.y = draw.target_height - draw.state.scissor[1] -
					draw.state.scissor[3];
				scissor.w = draw.state.scissor[2];
				scissor.h = draw.state.scissor[3];
			} else {
				scissor.x = 0;
				scissor.y = 0;
				scissor.w = draw.target_width;
				scissor.h = draw.target_height;
			}
			SDL_SetGPUScissor(pass, &scissor);
			SDL_FColor blend_color;
			blend_color.r = draw.state.blend_color[0];
			blend_color.g = draw.state.blend_color[1];
			blend_color.b = draw.state.blend_color[2];
			blend_color.a = draw.state.blend_color[3];
			SDL_SetGPUBlendConstants(pass, blend_color);
			SDL_SetGPUStencilReference(pass, (Uint8)draw.state.stencil_ref);
			SDL_DrawGPUPrimitives(pass, (Uint32)draw.vertices.size(), 1, 0, 0);
		}
		SDL_EndGPURenderPass(pass);
		first = last;
	}
	s_pending_draws.clear();
	s_pending_vertex_bytes = 0;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.draw_flush_ticks += SDLGPUProfileNow() - profile_start_tick;
#endif
	return true;
}

static bool SDLGPUQueueVertices(GLenum mode, std::vector<CompatVertex> &vertices)
{
	if (!s_device || vertices.empty())
		return false;
	size_t byte_count = vertices.size() * sizeof(CompatVertex);
	if (byte_count > s_vertex_capacity)
		return false;
	if ((size_t)s_pending_vertex_bytes + byte_count > s_vertex_capacity) {
		if (!SDLGPUFlushPendingDraws())
			return false;
	}
	SDL_GPUTexture *color = NULL;
	SDL_GPUTexture *depth = NULL;
	int target_width = 0;
	int target_height = 0;
	int color_level = 0;
	if (!SDLGPUGetCurrentTargets(&color, &depth, &target_width, &target_height,
		&color_level))
		return false;
	if (!s_state.depth_test_enabled && !s_state.stencil_test_enabled)
		depth = NULL;
	SDL_GPUGraphicsPipeline *pipeline = SDLGPUGetPipeline(mode, depth != NULL);
	if (!pipeline)
		return false;
	s_pending_draws.push_back(PendingDraw());
	PendingDraw &draw = s_pending_draws.back();
	draw.vertices.swap(vertices);
	draw.state = s_state;
	draw.pipeline = pipeline;
	draw.color = color;
	draw.depth = depth;
	draw.target_width = target_width;
	draw.target_height = target_height;
	draw.color_level = color_level;
	SDLGPUFillFragmentUniforms(draw.uniforms);
	draw.textures[0] = SDLGPUGetSelectedTexture(0, false);
	draw.textures[1] = SDLGPUGetSelectedTexture(1, false);
	draw.textures[2] = SDLGPUGetSelectedTexture(0, true);
	for (int unit = 0; unit < 3; unit++) {
		draw.samplers[unit] = SDLGPUGetTextureSampler(draw.textures[unit]);
		if (!draw.samplers[unit]) {
			s_pending_draws.pop_back();
			return false;
		}
	}
	for (int unit = 0; unit < 2; unit++) {
		if (!draw.textures[unit]->presentation_y_flip)
			continue;
		for (size_t index = 0; index < draw.vertices.size(); index++) {
			float *coordinate = draw.vertices[index].texcoord0;
			if (unit == 1)
				coordinate = draw.vertices[index].texcoord1;
			coordinate[1] = coordinate[3] - coordinate[1];
		}
	}
	draw.vertex_offset = s_pending_vertex_bytes;
	s_pending_vertex_bytes += (Uint32)byte_count;
	return true;
}

static void SDLGPUExpandPrimitive(GLenum mode, std::vector<CompatVertex> &input,
	                         GLenum &output_mode, std::vector<CompatVertex> &output)
{
	output.clear();
	output_mode = mode;
	if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON) {
		output_mode = GL_TRIANGLES;
		for (size_t index = 2; index < input.size(); index++) {
			output.push_back(input[0]);
			output.push_back(input[index - 1]);
			output.push_back(input[index]);
		}
		return;
	}
	if (mode == GL_QUADS) {
		output_mode = GL_TRIANGLES;
		for (size_t index = 0; index + 3 < input.size(); index += 4) {
			output.push_back(input[index]);
			output.push_back(input[index + 1]);
			output.push_back(input[index + 2]);
			output.push_back(input[index]);
			output.push_back(input[index + 2]);
			output.push_back(input[index + 3]);
		}
		return;
	}
	if (mode == GL_QUAD_STRIP) {
		output_mode = GL_TRIANGLES;
		for (size_t index = 0; index + 3 < input.size(); index += 2) {
			output.push_back(input[index]);
			output.push_back(input[index + 1]);
			output.push_back(input[index + 3]);
			output.push_back(input[index]);
			output.push_back(input[index + 3]);
			output.push_back(input[index + 2]);
		}
		return;
	}
	if (mode == GL_LINE_LOOP) {
		output_mode = GL_LINES;
		for (size_t index = 1; index < input.size(); index++) {
			output.push_back(input[index - 1]);
			output.push_back(input[index]);
		}
		if (input.size() > 1) {
			output.push_back(input[input.size() - 1]);
			output.push_back(input[0]);
		}
		return;
	}
	output.swap(input);
}

static void SDLGPUCopyVector4(float destination[4], const float source[4])
{
	for (int component = 0; component < 4; component++)
		destination[component] = source[component];
}

static void SDLGPUComputeVertexColor(const float eye_position[4], float color[4],
	                             float secondary[4])
{
	SDLGPUCopyVector4(color, s_state.current_color);
	SDLGPUCopyVector4(secondary, s_state.current_secondary);
	if (!s_state.lighting_enabled)
		return;
	float normal[3];
	normal[0] = s_state.modelview.m[0] * s_state.current_normal[0] +
	            s_state.modelview.m[4] * s_state.current_normal[1] +
	            s_state.modelview.m[8] * s_state.current_normal[2];
	normal[1] = s_state.modelview.m[1] * s_state.current_normal[0] +
	            s_state.modelview.m[5] * s_state.current_normal[1] +
	            s_state.modelview.m[9] * s_state.current_normal[2];
	normal[2] = s_state.modelview.m[2] * s_state.current_normal[0] +
	            s_state.modelview.m[6] * s_state.current_normal[1] +
	            s_state.modelview.m[10] * s_state.current_normal[2];
	if (s_state.normalize_enabled)
		SDLGPUNormalizeVector3(normal);
	float material_ambient[4];
	float material_diffuse[4];
	SDLGPUCopyVector4(material_ambient, s_state.material.ambient);
	SDLGPUCopyVector4(material_diffuse, s_state.material.diffuse);
	if (s_state.color_material_enabled) {
		if (s_state.color_material_mode == GL_AMBIENT ||
			s_state.color_material_mode == GL_AMBIENT_AND_DIFFUSE)
			SDLGPUCopyVector4(material_ambient, s_state.current_color);
		if (s_state.color_material_mode == GL_DIFFUSE ||
			s_state.color_material_mode == GL_AMBIENT_AND_DIFFUSE)
			SDLGPUCopyVector4(material_diffuse, s_state.current_color);
	}
	for (int channel = 0; channel < 3; channel++) {
		color[channel] = s_state.material.emission[channel] +
			s_state.light_model_ambient[channel] * material_ambient[channel];
		secondary[channel] = s_state.current_secondary[channel];
	}
	color[3] = material_diffuse[3];
	secondary[3] = 1.0f;
	for (int light_index = 0; light_index < 8; light_index++) {
		const LightState &light = s_state.lights[light_index];
		if (!light.enabled)
			continue;
		float light_vector[3];
		float attenuation = 1.0f;
		if (light.position[3] == 0.0f) {
			light_vector[0] = light.position[0];
			light_vector[1] = light.position[1];
			light_vector[2] = light.position[2];
			SDLGPUNormalizeVector3(light_vector);
		} else {
			light_vector[0] = light.position[0] / light.position[3] - eye_position[0];
			light_vector[1] = light.position[1] / light.position[3] - eye_position[1];
			light_vector[2] = light.position[2] / light.position[3] - eye_position[2];
			float distance = std::sqrt(light_vector[0] * light_vector[0] +
				light_vector[1] * light_vector[1] + light_vector[2] * light_vector[2]);
			if (distance > 0.000001f) {
				light_vector[0] /= distance;
				light_vector[1] /= distance;
				light_vector[2] /= distance;
			}
			attenuation = 1.0f / std::max(0.000001f,
				light.constant_attenuation + light.linear_attenuation * distance +
				light.quadratic_attenuation * distance * distance);
		}
		float diffuse = std::max(0.0f, normal[0] * light_vector[0] +
			normal[1] * light_vector[1] + normal[2] * light_vector[2]);
		float view[3] = {-eye_position[0], -eye_position[1], -eye_position[2]};
		if (!s_state.light_model_local_viewer) {
			view[0] = 0.0f;
			view[1] = 0.0f;
			view[2] = 1.0f;
		}
		SDLGPUNormalizeVector3(view);
		float half_vector[3] = {light_vector[0] + view[0],
			light_vector[1] + view[1], light_vector[2] + view[2]};
		SDLGPUNormalizeVector3(half_vector);
		float specular = 0.0f;
		if (diffuse > 0.0f) {
			float dot_value = std::max(0.0f, normal[0] * half_vector[0] +
				normal[1] * half_vector[1] + normal[2] * half_vector[2]);
			specular = std::pow(dot_value, s_state.material.shininess);
		}
		for (int channel = 0; channel < 3; channel++) {
			color[channel] += attenuation * (light.ambient[channel] * material_ambient[channel] +
				light.diffuse[channel] * material_diffuse[channel] * diffuse);
			secondary[channel] += attenuation * light.specular[channel] *
				s_state.material.specular[channel] * specular;
		}
	}
	for (int channel = 0; channel < 4; channel++) {
		color[channel] = SDLGPUClampFloat(color[channel], 0.0f, 1.0f);
		secondary[channel] = SDLGPUClampFloat(secondary[channel], 0.0f, 1.0f);
	}
}

static void SDLGPUAppendVertex(float x, float y, float z, float w)
{
	if (s_begin_mode == 0) {
		SDLGPUSetError(GL_INVALID_OPERATION);
		return;
	}
	CompatVertex vertex;
	std::memset(&vertex, 0, sizeof(vertex));
	float object_position[4] = {x, y, z, w};
	SDLGPUTransformVector(s_state.modelview, object_position, vertex.eye_position);
	SDLGPUTransformVector(s_state.projection, vertex.eye_position, vertex.position);
	vertex.position[2] = (vertex.position[2] + vertex.position[3]) * 0.5f;
	SDLGPUComputeVertexColor(vertex.eye_position, vertex.color, vertex.secondary);
	SDLGPUTransformVector(s_state.texture_matrix[0], s_state.current_texcoord[0], vertex.texcoord0);
	SDLGPUTransformVector(s_state.texture_matrix[1], s_state.current_texcoord[1], vertex.texcoord1);
	if (s_state.fog_source == GL_FOG_COORDINATE)
		vertex.fog_coord = s_state.current_fog_coord;
	else
		vertex.fog_coord = std::fabs(vertex.eye_position[2]);
	s_vertices.push_back(vertex);
}

static bool SDLGPUIsCapabilityEnabled(GLenum capability)
{
	if (capability == GL_BLEND) return s_state.blend_enabled;
	if (capability == GL_ALPHA_TEST) return s_state.alpha_test_enabled;
	if (capability == GL_DEPTH_TEST) return s_state.depth_test_enabled;
	if (capability == GL_STENCIL_TEST) return s_state.stencil_test_enabled;
	if (capability == GL_CULL_FACE) return s_state.cull_enabled;
	if (capability == GL_SCISSOR_TEST) return s_state.scissor_enabled;
	if (capability == GL_FOG) return s_state.fog_enabled;
	if (capability == GL_LIGHTING) return s_state.lighting_enabled;
	if (capability == GL_NORMALIZE) return s_state.normalize_enabled;
	if (capability == GL_COLOR_MATERIAL) return s_state.color_material_enabled;
	if (capability == GL_COLOR_SUM) return s_state.color_sum_enabled;
	if (capability == GL_POLYGON_OFFSET_FILL || capability == GL_POLYGON_OFFSET_LINE ||
		capability == GL_POLYGON_OFFSET_POINT) return s_state.polygon_offset_enabled;
	if (capability == GL_TEXTURE_2D) return s_state.texture_2d_enabled[s_state.active_texture];
	if (capability == GL_TEXTURE_3D) return s_state.texture_3d_enabled[s_state.active_texture];
	if (capability >= GL_LIGHT0 && capability < GL_LIGHT0 + 8)
		return s_state.lights[capability - GL_LIGHT0].enabled;
	if (capability >= GL_CLIP_PLANE0 && capability < GL_CLIP_PLANE0 + 6)
		return s_state.clip_enabled[capability - GL_CLIP_PLANE0];
	return false;
}

static void SDLGPUSetCapability(GLenum capability, bool enabled)
{
	if (capability == GL_BLEND) s_state.blend_enabled = enabled;
	else if (capability == GL_ALPHA_TEST) s_state.alpha_test_enabled = enabled;
	else if (capability == GL_DEPTH_TEST) s_state.depth_test_enabled = enabled;
	else if (capability == GL_STENCIL_TEST) s_state.stencil_test_enabled = enabled;
	else if (capability == GL_CULL_FACE) s_state.cull_enabled = enabled;
	else if (capability == GL_SCISSOR_TEST) s_state.scissor_enabled = enabled;
	else if (capability == GL_FOG) s_state.fog_enabled = enabled;
	else if (capability == GL_LIGHTING) s_state.lighting_enabled = enabled;
	else if (capability == GL_NORMALIZE) s_state.normalize_enabled = enabled;
	else if (capability == GL_COLOR_MATERIAL) s_state.color_material_enabled = enabled;
	else if (capability == GL_COLOR_SUM) s_state.color_sum_enabled = enabled;
	else if (capability == GL_POLYGON_OFFSET_FILL || capability == GL_POLYGON_OFFSET_LINE ||
		capability == GL_POLYGON_OFFSET_POINT) s_state.polygon_offset_enabled = enabled;
	else if (capability == GL_TEXTURE_2D) s_state.texture_2d_enabled[s_state.active_texture] = enabled;
	else if (capability == GL_TEXTURE_3D) s_state.texture_3d_enabled[s_state.active_texture] = enabled;
	else if (capability >= GL_LIGHT0 && capability < GL_LIGHT0 + 8)
		s_state.lights[capability - GL_LIGHT0].enabled = enabled;
	else if (capability >= GL_CLIP_PLANE0 && capability < GL_CLIP_PLANE0 + 6)
		s_state.clip_enabled[capability - GL_CLIP_PLANE0] = enabled;
}

static std::vector<Matrix4> &SDLGPUGetCurrentMatrixStack()
{
	if (s_state.matrix_mode == GL_PROJECTION)
		return s_projection_stack;
	if (s_state.matrix_mode == GL_TEXTURE)
		return s_texture_stack[s_state.active_texture];
	return s_modelview_stack;
}

} // namespace

#if SDLGPU_TRANSITION_LOGGING_ENABLED
extern "C" Uint64 SDLGPUTransitionTraceBegin(void)
{
	Uint64 now = SDLGPUProfileNow();
	Uint32 depth = s_transition_trace_depth;
	if (depth == 0 && s_transition_profile_start_tick == 0) {
		s_transition_profile_start_tick = now;
		s_transition_profile_sample_sequence = atomic_load_explicit(
			&s_transition_guest_sample_sequence, memory_order_acquire);
	}
	if (depth < 16) {
		s_transition_gap_ticks[depth] = 0;
		if (depth == 0 && s_transition_last_event_tick != 0 &&
			now > s_transition_last_event_tick)
			s_transition_gap_ticks[depth] = now - s_transition_last_event_tick;
	}
	s_transition_trace_depth++;
	return now;
}

extern "C" void SDLGPUTransitionTraceEnd(const char *event_name, Uint32 engine_id,
	int width, int height, Uint64 start_tick)
{
	Uint64 end_tick = SDLGPUProfileNow();
	Uint64 elapsed_ticks = end_tick - start_tick;
	Uint64 gap_ticks = 0;
	Uint64 dispatch_count = 0;
	Uint64 dispatch_ticks = 0;
	Uint64 dispatch_max_ticks = 0;
	for (int dispatch_engine = 0;
		dispatch_engine < kSDLGPUTransitionEngineCount; dispatch_engine++) {
		for (int dispatch_opcode = 0;
			dispatch_opcode < kSDLGPUTransitionDispatchOpcodeCount;
			dispatch_opcode++) {
			dispatch_count += s_transition_profile.dispatch_count
				[dispatch_engine][dispatch_opcode];
			dispatch_ticks += s_transition_profile.dispatch_ticks
				[dispatch_engine][dispatch_opcode];
			if (s_transition_profile.dispatch_max_ticks
				[dispatch_engine][dispatch_opcode] > dispatch_max_ticks) {
				dispatch_max_ticks = s_transition_profile.dispatch_max_ticks
					[dispatch_engine][dispatch_opcode];
			}
		}
	}
	if (s_transition_trace_depth > 0 && s_transition_trace_depth <= 16)
		gap_ticks = s_transition_gap_ticks[s_transition_trace_depth - 1];
	bug("[gfxaccel-sdlgpu-timing] stamp=%llu event=%s engine=%u depth=%u size=%dx%d total=%.3fms preceding=%.3fms "
		"device=%u/%.3f shader=%u/%.3f buffer=%u/%.3f "
		"window=%u/%.3f+%u/%.3f texture=%u/%.3f+%u/%.3f+%u/%llu/%.3f+%u/%.3f "
		"pipeline=%u/%.3f draw=%u/%u/%llu/%.3f readback=%u/%.3f "
		"copy=%u/%.3f present=%u/%.3f swap=%u/%.3f/%u/%u/%u/%u/%u acquire=%u/%.3f submit=%u/%.3f "
		"finish=%u/%.3f clear=%u/%.3f pacing=%u/%.3f "
		"videovbl=%u/%.3f nqd=%.3f compositor=%.3f lock=%.3f service=%.3f intervalmax=%.3f "
		"vblcallback=%u/%.3f redraw=%llu/%.3f dispatch=%llu/%.3f/%.3f "
		"guest=%llu pc=%08x/%08x mode=%u level=%u nest=%u irq=%08x inhibit=%u "
		"counts=%llu/%llu/%llu stable=%llu@%08x/%llu@%08x\n",
		(unsigned long long)SDL_GetTicks(), event_name, (unsigned)engine_id,
		(unsigned)s_transition_trace_depth, width, height,
		SDLGPUProfileMilliseconds(elapsed_ticks),
		SDLGPUProfileMilliseconds(gap_ticks),
		(unsigned)s_transition_profile.device_create_count,
		SDLGPUProfileMilliseconds(s_transition_profile.device_create_ticks),
		(unsigned)s_transition_profile.shader_create_count,
		SDLGPUProfileMilliseconds(s_transition_profile.shader_create_ticks),
		(unsigned)s_transition_profile.buffer_create_count,
		SDLGPUProfileMilliseconds(s_transition_profile.buffer_create_ticks),
		(unsigned)s_transition_profile.window_claim_count,
		SDLGPUProfileMilliseconds(s_transition_profile.window_claim_ticks),
		(unsigned)s_transition_profile.window_release_count,
		SDLGPUProfileMilliseconds(s_transition_profile.window_release_ticks),
		(unsigned)s_transition_profile.texture_create_count,
		SDLGPUProfileMilliseconds(s_transition_profile.texture_create_ticks),
		(unsigned)s_transition_profile.texture_convert_count,
		SDLGPUProfileMilliseconds(s_transition_profile.texture_convert_ticks),
		(unsigned)s_transition_profile.texture_upload_count,
		(unsigned long long)s_transition_profile.upload_bytes,
		SDLGPUProfileMilliseconds(s_transition_profile.texture_upload_ticks),
		(unsigned)s_transition_profile.texture_release_count,
		SDLGPUProfileMilliseconds(s_transition_profile.texture_release_ticks),
		(unsigned)s_transition_profile.pipeline_create_count,
		SDLGPUProfileMilliseconds(s_transition_profile.pipeline_create_ticks),
		(unsigned)s_transition_profile.draw_flush_count,
		(unsigned)s_transition_profile.draw_count,
		(unsigned long long)s_transition_profile.draw_vertices,
		SDLGPUProfileMilliseconds(s_transition_profile.draw_flush_ticks),
		(unsigned)s_transition_profile.readback_count,
		SDLGPUProfileMilliseconds(s_transition_profile.readback_ticks),
		(unsigned)s_transition_profile.copy_texture_count,
		SDLGPUProfileMilliseconds(s_transition_profile.copy_texture_ticks),
		(unsigned)s_transition_profile.present_count,
		SDLGPUProfileMilliseconds(s_transition_profile.present_ticks),
		(unsigned)s_transition_profile.swapchain_config_count,
		SDLGPUProfileMilliseconds(s_transition_profile.swapchain_config_ticks),
		(unsigned)s_transition_profile.swapchain_interval,
		(unsigned)s_transition_profile.swapchain_mode,
		(unsigned)s_transition_profile.swapchain_immediate_supported,
		(unsigned)s_transition_profile.swapchain_mailbox_supported,
		(unsigned)s_transition_profile.swapchain_config_succeeded,
		(unsigned)s_transition_profile.swapchain_acquire_count,
		SDLGPUProfileMilliseconds(s_transition_profile.swapchain_acquire_ticks),
		(unsigned)s_transition_profile.swapchain_submit_count,
		SDLGPUProfileMilliseconds(s_transition_profile.swapchain_submit_ticks),
		(unsigned)s_transition_profile.finish_count,
		SDLGPUProfileMilliseconds(s_transition_profile.finish_ticks),
		(unsigned)s_transition_profile.clear_count,
		SDLGPUProfileMilliseconds(s_transition_profile.clear_ticks),
		(unsigned)s_transition_profile.pacing_count,
		SDLGPUProfileMilliseconds(s_transition_profile.pacing_ticks),
		(unsigned)s_transition_profile.video_vbl_count,
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_ticks),
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_nqd_ticks),
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_compositor_ticks),
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_lock_ticks),
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_service_ticks),
		SDLGPUProfileMilliseconds(s_transition_profile.video_vbl_interval_max_ticks),
		(unsigned)s_transition_profile.vbl_callback_count,
		SDLGPUProfileMilliseconds(s_transition_profile.vbl_callback_ticks),
		(unsigned long long)atomic_exchange_explicit(&s_transition_redraw_count,
			0, memory_order_relaxed),
		SDLGPUProfileMilliseconds(atomic_exchange_explicit(
			&s_transition_redraw_ticks, 0, memory_order_relaxed)),
		(unsigned long long)dispatch_count,
		SDLGPUProfileMilliseconds(dispatch_ticks),
		SDLGPUProfileMilliseconds(dispatch_max_ticks),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.sample_count, 0, memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_pc,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_pc68k,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_run_mode,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_level68k,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_irq_nest,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.last_irq_flags,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(
			&s_transition_guest_profile.last_tick_inhibited,
			memory_order_relaxed),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.irq_pending_count, 0,
			memory_order_relaxed),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.irq_nested_count, 0,
			memory_order_relaxed),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.tick_inhibited_count, 0,
			memory_order_relaxed),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.stable_pc_count, 0,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.stable_pc,
			memory_order_relaxed),
		(unsigned long long)atomic_exchange_explicit(
			&s_transition_guest_profile.stable_pc68k_count, 0,
			memory_order_relaxed),
		(unsigned)atomic_load_explicit(&s_transition_guest_profile.stable_pc68k,
			memory_order_relaxed));
	bool dump_detail = false;
	Uint64 detail_threshold = SDL_GetPerformanceFrequency() / 20;
	if (elapsed_ticks >= detail_threshold || gap_ticks >= detail_threshold)
		dump_detail = true;
	if (dump_detail) {
		for (int dispatch_engine = 0;
			dispatch_engine < kSDLGPUTransitionEngineCount; dispatch_engine++) {
			for (int dispatch_opcode = 0;
				dispatch_opcode < kSDLGPUTransitionDispatchOpcodeCount;
				dispatch_opcode++) {
				Uint64 opcode_count = s_transition_profile.dispatch_count
					[dispatch_engine][dispatch_opcode];
				if (opcode_count == 0)
					continue;
				bug("[gfxaccel-sdlgpu-dispatch] event=%s engine=%u opcode=%u count=%llu total=%.3fms max=%.3fms\n",
					event_name, (unsigned)dispatch_engine,
					(unsigned)dispatch_opcode,
					(unsigned long long)opcode_count,
					SDLGPUProfileMilliseconds(s_transition_profile.dispatch_ticks
						[dispatch_engine][dispatch_opcode]),
					SDLGPUProfileMilliseconds(s_transition_profile.dispatch_max_ticks
						[dispatch_engine][dispatch_opcode]));
			}
		}
		Uint64 sample_end = atomic_load_explicit(
			&s_transition_guest_sample_sequence, memory_order_acquire);
		Uint64 sample_begin = s_transition_profile_sample_sequence + 1;
		if (sample_end >= kSDLGPUTransitionGuestSampleCount &&
			sample_begin <= sample_end - kSDLGPUTransitionGuestSampleCount) {
			sample_begin = sample_end - kSDLGPUTransitionGuestSampleCount + 1;
		}
		for (Uint64 sample_sequence = sample_begin;
			sample_sequence <= sample_end; sample_sequence++) {
			Uint32 sample_index = (Uint32)((sample_sequence - 1) %
				kSDLGPUTransitionGuestSampleCount);
			SDLGPUTransitionGuestSample &sample =
				s_transition_guest_samples[sample_index];
			Uint64 stored_sequence = atomic_load_explicit(&sample.sequence,
				memory_order_acquire);
			if (stored_sequence != sample_sequence)
				continue;
			Uint64 sample_tick = atomic_load_explicit(&sample.tick,
				memory_order_relaxed);
			Uint64 sample_offset = 0;
			if (sample_tick > s_transition_profile_start_tick)
				sample_offset = sample_tick - s_transition_profile_start_tick;
			bug("[gfxaccel-sdlgpu-sample] event=%s offset=%.3fms pc=%08x pc68k=%08x level=%u mode=%u nest=%u irq=%08x inhibit=%u dispatch=%u/%u\n",
				event_name, SDLGPUProfileMilliseconds(sample_offset),
				(unsigned)atomic_load_explicit(&sample.pc, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.pc68k, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.level68k, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.run_mode, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.irq_nest, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.irq_flags, memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.tick_inhibited,
					memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.dispatch_engine,
					memory_order_relaxed),
				(unsigned)atomic_load_explicit(&sample.dispatch_opcode,
					memory_order_relaxed));
		}
	}
	s_transition_last_event_tick = end_tick;
	if (s_transition_trace_depth > 0)
		s_transition_trace_depth--;
	if (s_transition_trace_depth == 0)
		SDLGPUProfileReset();
}

extern "C" void SDLGPUTransitionTraceRecordPacing(Uint64 start_tick)
{
	s_transition_profile.pacing_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.pacing_count++;
}

extern "C" void SDLGPUTransitionTraceRecordVBLCallback(Uint64 start_tick)
{
	s_transition_profile.vbl_callback_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.vbl_callback_count++;
}

extern "C" void SDLGPUTransitionTraceRecordVideoVBL(Uint64 start_tick,
	Uint64 nqd_ticks, Uint64 compositor_ticks, Uint64 lock_ticks,
	Uint64 service_ticks)
{
	Uint64 now = SDLGPUProfileNow();
	s_transition_profile.video_vbl_ticks += now - start_tick;
	s_transition_profile.video_vbl_nqd_ticks += nqd_ticks;
	s_transition_profile.video_vbl_compositor_ticks += compositor_ticks;
	s_transition_profile.video_vbl_lock_ticks += lock_ticks;
	s_transition_profile.video_vbl_service_ticks += service_ticks;
	if (s_transition_last_video_vbl_tick != 0 &&
		start_tick > s_transition_last_video_vbl_tick) {
		Uint64 interval = start_tick - s_transition_last_video_vbl_tick;
		if (interval > s_transition_profile.video_vbl_interval_max_ticks)
			s_transition_profile.video_vbl_interval_max_ticks = interval;
	}
	s_transition_last_video_vbl_tick = start_tick;
	s_transition_profile.video_vbl_count++;
}

extern "C" void SDLGPUTransitionTraceRecordRedraw(Uint64 start_tick)
{
	atomic_fetch_add_explicit(&s_transition_redraw_ticks,
		SDLGPUProfileNow() - start_tick, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_transition_redraw_count, 1,
		memory_order_relaxed);
}

extern "C" void SDLGPUTransitionTraceRecordGuestSample(Uint32 pc,
	Uint32 pc68k, Uint32 level68k, Uint32 run_mode, Uint32 irq_nest,
	Uint32 irq_flags, int tick_inhibited)
{
	Uint64 sample_sequence = atomic_fetch_add_explicit(
		&s_transition_guest_sample_sequence, 1, memory_order_acq_rel) + 1;
	Uint32 sample_index = (Uint32)((sample_sequence - 1) %
		kSDLGPUTransitionGuestSampleCount);
	SDLGPUTransitionGuestSample &sample =
		s_transition_guest_samples[sample_index];
	atomic_store_explicit(&sample.sequence, 0, memory_order_release);
	atomic_store_explicit(&sample.tick, SDLGPUProfileNow(),
		memory_order_relaxed);
	atomic_store_explicit(&sample.pc, pc, memory_order_relaxed);
	atomic_store_explicit(&sample.pc68k, pc68k, memory_order_relaxed);
	atomic_store_explicit(&sample.level68k, level68k, memory_order_relaxed);
	atomic_store_explicit(&sample.run_mode, run_mode, memory_order_relaxed);
	atomic_store_explicit(&sample.irq_nest, irq_nest, memory_order_relaxed);
	atomic_store_explicit(&sample.irq_flags, irq_flags, memory_order_relaxed);
	atomic_store_explicit(&sample.tick_inhibited, tick_inhibited != 0,
		memory_order_relaxed);
	Uint64 active_dispatch = atomic_load_explicit(
		&s_transition_active_dispatch, memory_order_acquire);
	atomic_store_explicit(&sample.dispatch_engine, active_dispatch >> 32,
		memory_order_relaxed);
	atomic_store_explicit(&sample.dispatch_opcode,
		active_dispatch & 0xffffffffu, memory_order_relaxed);
	atomic_store_explicit(&sample.sequence, sample_sequence,
		memory_order_release);
	static Uint32 previous_pc = 0;
	static Uint32 previous_pc68k = 0;
	static Uint64 pc_streak = 0;
	static Uint64 pc68k_streak = 0;
	if (pc == previous_pc)
		pc_streak++;
	else
		pc_streak = 1;
	if (pc68k == previous_pc68k)
		pc68k_streak++;
	else
		pc68k_streak = 1;
	previous_pc = pc;
	previous_pc68k = pc68k;
	if (pc_streak > atomic_load_explicit(
		&s_transition_guest_profile.stable_pc_count, memory_order_relaxed)) {
		atomic_store_explicit(&s_transition_guest_profile.stable_pc_count,
			pc_streak, memory_order_relaxed);
		atomic_store_explicit(&s_transition_guest_profile.stable_pc, pc,
			memory_order_relaxed);
	}
	if (pc68k_streak > atomic_load_explicit(
		&s_transition_guest_profile.stable_pc68k_count,
		memory_order_relaxed)) {
		atomic_store_explicit(&s_transition_guest_profile.stable_pc68k_count,
			pc68k_streak, memory_order_relaxed);
		atomic_store_explicit(&s_transition_guest_profile.stable_pc68k,
			pc68k, memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&s_transition_guest_profile.sample_count, 1,
		memory_order_relaxed);
	if (irq_flags != 0)
		atomic_fetch_add_explicit(
			&s_transition_guest_profile.irq_pending_count, 1,
			memory_order_relaxed);
	if (irq_nest != 0)
		atomic_fetch_add_explicit(&s_transition_guest_profile.irq_nested_count,
			1, memory_order_relaxed);
	if (tick_inhibited)
		atomic_fetch_add_explicit(
			&s_transition_guest_profile.tick_inhibited_count, 1,
			memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_pc, pc,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_pc68k, pc68k,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_level68k, level68k,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_run_mode, run_mode,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_irq_nest, irq_nest,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_irq_flags, irq_flags,
		memory_order_relaxed);
	atomic_store_explicit(&s_transition_guest_profile.last_tick_inhibited,
		tick_inhibited != 0, memory_order_relaxed);
}

extern "C" Uint64 SDLGPUTransitionTraceDispatchBegin(Uint32 engine_id,
	Uint32 opcode)
{
	Uint64 active_dispatch = ((Uint64)engine_id << 32) | (Uint64)opcode;
	atomic_store_explicit(&s_transition_active_dispatch, active_dispatch,
		memory_order_release);
	return SDLGPUProfileNow();
}

extern "C" void SDLGPUTransitionTraceRecordDispatch(Uint32 engine_id,
	Uint32 opcode, Uint64 start_tick)
{
	Uint64 elapsed_ticks = SDLGPUProfileNow() - start_tick;
	atomic_store_explicit(&s_transition_active_dispatch,
		((Uint64)0xffffffffu << 32) | (Uint64)0xffffffffu,
		memory_order_release);
	if (engine_id >= kSDLGPUTransitionEngineCount)
		return;
	if (opcode >= kSDLGPUTransitionDispatchOpcodeCount)
		opcode = kSDLGPUTransitionDispatchOpcodeCount - 1;
	s_transition_profile.dispatch_count[engine_id][opcode]++;
	s_transition_profile.dispatch_ticks[engine_id][opcode] += elapsed_ticks;
	if (elapsed_ticks >
		s_transition_profile.dispatch_max_ticks[engine_id][opcode]) {
		s_transition_profile.dispatch_max_ticks[engine_id][opcode] =
			elapsed_ticks;
	}
}
#endif

extern "C" SDL_GLContext SDLGPUCreateContext(SDL_Window *window)
{
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
#endif
	if (!SDLGPUInitializeDevice(window)) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		SDLGPUTransitionTraceEnd("context-create-failed", 0xffffffffu, 0, 0,
			trace_start_tick);
#endif
		return NULL;
	}
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	SDLGPUTransitionTraceEnd("context-create", 0xffffffffu, s_default_width,
		s_default_height, trace_start_tick);
#endif
	return reinterpret_cast<SDL_GLContext>(s_device);
}

extern "C" void SDLGPUDestroyContext(SDL_GLContext)
{
	if (!s_device || !s_window)
		return;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 trace_start_tick = SDLGPUTransitionTraceBegin();
#endif
	SDLGPUReleaseVideoWindow(s_window);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	SDLGPUTransitionTraceEnd("context-destroy", 0xffffffffu, s_default_width,
		s_default_height, trace_start_tick);
#endif
}

extern "C" void SDLGPUReleaseVideoWindow(SDL_Window *window)
{
	if (!s_device || !window || s_window != window)
		return;
	SDLGPUFlushPendingDraws();
	SDLGPUSubmitDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 release_start_tick = SDLGPUProfileNow();
#endif
	SDL_ReleaseWindowFromGPUDevice(s_device, window);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.window_release_ticks += SDLGPUProfileNow() - release_start_tick;
	s_transition_profile.window_release_count++;
#endif
	s_window = NULL;
}

extern "C" SDL_GLContext SDLGPUGetCurrentContext(void)
{
	if (!s_device || !s_window)
		return NULL;
	return reinterpret_cast<SDL_GLContext>(s_device);
}

extern "C" bool SDLGPUMakeCurrent(SDL_Window *window, SDL_GLContext context)
{
	if (!context)
		return true;
	if (!s_device || !window || context != reinterpret_cast<SDL_GLContext>(s_device))
		return false;
	if (!SDLGPUAttachWindow(window))
		return false;
	return SDLGPUEnsureDefaultTargets();
}

extern "C" bool SDLGPUSetAttribute(SDL_GLAttr, int)
{
	return true;
}

extern "C" bool SDLGPUSetSwapInterval(int interval)
{
	if (!s_device || !s_window)
		return false;
	bool immediate_supported = SDL_WindowSupportsGPUPresentMode(s_device,
		s_window, SDL_GPU_PRESENTMODE_IMMEDIATE);
	bool mailbox_supported = SDL_WindowSupportsGPUPresentMode(s_device,
		s_window, SDL_GPU_PRESENTMODE_MAILBOX);
	SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
	if (interval == 0 && immediate_supported)
		mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
	else if (interval < 0 && mailbox_supported)
		mode = SDL_GPU_PRESENTMODE_MAILBOX;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 start_tick = SDLGPUProfileNow();
#endif
	bool result = SDL_SetGPUSwapchainParameters(s_device, s_window,
		SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.swapchain_config_ticks += SDLGPUProfileNow() - start_tick;
	s_transition_profile.swapchain_config_count++;
	s_transition_profile.swapchain_interval = (Uint32)interval;
	s_transition_profile.swapchain_mode = (Uint32)mode;
	s_transition_profile.swapchain_immediate_supported = 0;
	if (immediate_supported)
		s_transition_profile.swapchain_immediate_supported = 1;
	s_transition_profile.swapchain_mailbox_supported = 0;
	if (mailbox_supported)
		s_transition_profile.swapchain_mailbox_supported = 1;
	s_transition_profile.swapchain_config_succeeded = 0;
	if (result)
		s_transition_profile.swapchain_config_succeeded = 1;
#endif
	return result;
}

extern "C" bool SDLGPUSwapWindow(SDL_Window *window)
{
	if (!s_device || !window)
		return false;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	if (!SDLGPUEnsureDefaultTargets()) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfilePresentResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	if (!SDLGPUFlushPendingDraws()) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfilePresentResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfilePresentResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUTexture *swapchain = NULL;
	Uint32 width = 0;
	Uint32 height = 0;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 acquire_start_tick = SDLGPUProfileNow();
#endif
	bool acquired = SDL_WaitAndAcquireGPUSwapchainTexture(command, window,
		&swapchain, &width, &height);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.swapchain_acquire_ticks +=
		SDLGPUProfileNow() - acquire_start_tick;
	s_transition_profile.swapchain_acquire_count++;
#endif
	if (!acquired) {
		SDLGPUCancelDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfilePresentResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	if (swapchain) {
		SDL_GPUBlitInfo info;
		std::memset(&info, 0, sizeof(info));
		info.source.texture = s_default_color;
		info.source.w = (Uint32)s_default_width;
		info.source.h = (Uint32)s_default_height;
		info.destination.texture = swapchain;
		info.destination.w = width;
		info.destination.h = height;
		info.load_op = SDL_GPU_LOADOP_DONT_CARE;
		info.filter = SDL_GPU_FILTER_LINEAR;
		info.flip_mode = SDL_FLIP_NONE;
		SDL_BlitGPUTexture(command, &info);
	}
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 submit_start_tick = SDLGPUProfileNow();
#endif
	bool submitted = SDLGPUSubmitDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.swapchain_submit_ticks +=
		SDLGPUProfileNow() - submit_start_tick;
	s_transition_profile.swapchain_submit_count++;
#endif
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	return SDLGPUProfilePresentResult(submitted, profile_start_tick);
#else
	return submitted;
#endif
}

extern "C" SDL_FunctionPointer SDLGPUGetProcAddress(const char *name)
{
	if (!name)
		return NULL;
	if (std::strcmp(name, "glTexImage3D") == 0 || std::strcmp(name, "glTexImage3DEXT") == 0)
		return reinterpret_cast<SDL_FunctionPointer>(SDLGPUglTexImage3D);
	if (std::strcmp(name, "glTexSubImage3D") == 0 || std::strcmp(name, "glTexSubImage3DEXT") == 0)
		return reinterpret_cast<SDL_FunctionPointer>(SDLGPUglTexSubImage3D);
	if (std::strcmp(name, "glWindowPos2i") == 0 || std::strcmp(name, "glWindowPos2iARB") == 0)
		return reinterpret_cast<SDL_FunctionPointer>(SDLGPUglWindowPos2i);
	return NULL;
}

extern "C" void SDLGPUSetTexturePresentationYFlip(GLuint texture, bool enabled)
{
	TextureObject *object = SDLGPUFindTexture(texture);
	if (!object || object->presentation_y_flip == enabled)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	object->presentation_y_flip = enabled;
}

extern "C" void SDLGPUglBegin(GLenum mode)
{
	if (s_begin_mode != 0) {
		SDLGPUSetError(GL_INVALID_OPERATION);
		return;
	}
	s_begin_mode = mode;
	s_vertices.clear();
}

extern "C" void SDLGPUglEnd(void)
{
	if (s_begin_mode == 0) {
		SDLGPUSetError(GL_INVALID_OPERATION);
		return;
	}
	GLenum output_mode = s_begin_mode;
	std::vector<CompatVertex> output;
	SDLGPUExpandPrimitive(s_begin_mode, s_vertices, output_mode, output);
	SDLGPUQueueVertices(output_mode, output);
	s_vertices.clear();
	s_begin_mode = 0;
}

extern "C" void SDLGPUglVertex2f(GLfloat x, GLfloat y)
{
	SDLGPUAppendVertex(x, y, 0.0f, 1.0f);
}

extern "C" void SDLGPUglVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
	SDLGPUAppendVertex(x, y, z, 1.0f);
}

extern "C" void SDLGPUglVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	SDLGPUAppendVertex(x, y, z, w);
}

extern "C" void SDLGPUglVertex4fv(const GLfloat *value)
{
	if (value)
		SDLGPUAppendVertex(value[0], value[1], value[2], value[3]);
}

extern "C" void SDLGPUglColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	s_state.current_color[0] = red;
	s_state.current_color[1] = green;
	s_state.current_color[2] = blue;
	s_state.current_color[3] = alpha;
}

extern "C" void SDLGPUglColor4fv(const GLfloat *value)
{
	if (value)
		SDLGPUCopyVector4(s_state.current_color, value);
}

extern "C" void SDLGPUglSecondaryColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
	s_state.current_secondary[0] = red;
	s_state.current_secondary[1] = green;
	s_state.current_secondary[2] = blue;
	s_state.current_secondary[3] = 1.0f;
}

extern "C" void SDLGPUglNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
	s_state.current_normal[0] = x;
	s_state.current_normal[1] = y;
	s_state.current_normal[2] = z;
}

extern "C" void SDLGPUglNormal3fv(const GLfloat *value)
{
	if (value)
		SDLGPUglNormal3f(value[0], value[1], value[2]);
}

extern "C" void SDLGPUglTexCoord2f(GLfloat s, GLfloat t)
{
	SDLGPUglTexCoord4f(s, t, 0.0f, 1.0f);
}

extern "C" void SDLGPUglTexCoord3f(GLfloat s, GLfloat t, GLfloat r)
{
	SDLGPUglTexCoord4f(s, t, r, 1.0f);
}

extern "C" void SDLGPUglTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
	float *coordinate = s_state.current_texcoord[s_state.active_texture];
	coordinate[0] = s;
	coordinate[1] = t;
	coordinate[2] = r;
	coordinate[3] = q;
}

extern "C" void SDLGPUglMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t)
{
	SDLGPUglMultiTexCoord4f(target, s, t, 0.0f, 1.0f);
}

extern "C" void SDLGPUglMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t,
	                                      GLfloat r, GLfloat q)
{
	int unit = (int)(target - GL_TEXTURE0);
	if (unit < 0 || unit > 1)
		return;
	s_state.current_texcoord[unit][0] = s;
	s_state.current_texcoord[unit][1] = t;
	s_state.current_texcoord[unit][2] = r;
	s_state.current_texcoord[unit][3] = q;
}

extern "C" void SDLGPUglFogCoordf(GLfloat coordinate)
{
	s_state.current_fog_coord = coordinate;
}

extern "C" void SDLGPUglMatrixMode(GLenum mode)
{
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	s_state.matrix_mode = mode;
}

extern "C" void SDLGPUglLoadIdentity(void)
{
	SDLGPUSetIdentityMatrix(SDLGPUGetCurrentMatrix());
}

extern "C" void SDLGPUglLoadMatrixf(const GLfloat *matrix)
{
	if (matrix)
		std::memcpy(SDLGPUGetCurrentMatrix().m, matrix, sizeof(SDLGPUGetCurrentMatrix().m));
}

extern "C" void SDLGPUglPushMatrix(void)
{
	SDLGPUGetCurrentMatrixStack().push_back(SDLGPUGetCurrentMatrix());
}

extern "C" void SDLGPUglPopMatrix(void)
{
	std::vector<Matrix4> &stack = SDLGPUGetCurrentMatrixStack();
	if (stack.empty()) {
		SDLGPUSetError(GL_STACK_UNDERFLOW);
		return;
	}
	SDLGPUGetCurrentMatrix() = stack.back();
	stack.pop_back();
}

extern "C" void SDLGPUglOrtho(GLdouble left, GLdouble right, GLdouble bottom,
	                            GLdouble top, GLdouble near_value, GLdouble far_value)
{
	if (right == left || top == bottom || far_value == near_value) {
		SDLGPUSetError(GL_INVALID_VALUE);
		return;
	}
	Matrix4 matrix;
	SDLGPUSetIdentityMatrix(matrix);
	matrix.m[0] = (float)(2.0 / (right - left));
	matrix.m[5] = (float)(2.0 / (top - bottom));
	matrix.m[10] = (float)(-2.0 / (far_value - near_value));
	matrix.m[12] = (float)(-(right + left) / (right - left));
	matrix.m[13] = (float)(-(top + bottom) / (top - bottom));
	matrix.m[14] = (float)(-(far_value + near_value) / (far_value - near_value));
	SDLGPUGetCurrentMatrix() = SDLGPUMultiplyMatrices(SDLGPUGetCurrentMatrix(), matrix);
}

extern "C" void SDLGPUglScalef(GLfloat x, GLfloat y, GLfloat z)
{
	Matrix4 matrix;
	SDLGPUSetIdentityMatrix(matrix);
	matrix.m[0] = x;
	matrix.m[5] = y;
	matrix.m[10] = z;
	SDLGPUGetCurrentMatrix() = SDLGPUMultiplyMatrices(SDLGPUGetCurrentMatrix(), matrix);
}

extern "C" void SDLGPUglEnable(GLenum capability)
{
	SDLGPUSetCapability(capability, true);
}

extern "C" void SDLGPUglDisable(GLenum capability)
{
	SDLGPUSetCapability(capability, false);
}

extern "C" GLboolean SDLGPUglIsEnabled(GLenum capability)
{
	if (SDLGPUIsCapabilityEnabled(capability))
		return GL_TRUE;
	return GL_FALSE;
}

extern "C" void SDLGPUglActiveTexture(GLenum texture)
{
	int unit = (int)(texture - GL_TEXTURE0);
	if (unit < 0 || unit > 1) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	s_state.active_texture = unit;
}

extern "C" void SDLGPUglClientActiveTexture(GLenum texture)
{
	int unit = (int)(texture - GL_TEXTURE0);
	if (unit < 0 || unit > 1) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	s_state.client_active_texture = unit;
}

extern "C" void SDLGPUglBlendFunc(GLenum source, GLenum destination)
{
	s_state.blend_src_rgb = source;
	s_state.blend_dst_rgb = destination;
	s_state.blend_src_alpha = source;
	s_state.blend_dst_alpha = destination;
}

extern "C" void SDLGPUglBlendFuncSeparate(GLenum source_rgb, GLenum destination_rgb,
	                                        GLenum source_alpha, GLenum destination_alpha)
{
	s_state.blend_src_rgb = source_rgb;
	s_state.blend_dst_rgb = destination_rgb;
	s_state.blend_src_alpha = source_alpha;
	s_state.blend_dst_alpha = destination_alpha;
}

extern "C" void SDLGPUglBlendEquation(GLenum mode)
{
	s_state.blend_equation = mode;
}

extern "C" void SDLGPUglBlendColor(GLclampf red, GLclampf green, GLclampf blue,
	                                 GLclampf alpha)
{
	s_state.blend_color[0] = red;
	s_state.blend_color[1] = green;
	s_state.blend_color[2] = blue;
	s_state.blend_color[3] = alpha;
}

extern "C" void SDLGPUglAlphaFunc(GLenum function, GLclampf reference)
{
	s_state.alpha_func = function;
	s_state.alpha_ref = SDLGPUClampFloat(reference, 0.0f, 1.0f);
}

extern "C" void SDLGPUglDepthFunc(GLenum function)
{
	s_state.depth_func = function;
}

extern "C" void SDLGPUglDepthMask(GLboolean flag)
{
	s_state.depth_write = flag != GL_FALSE;
}

extern "C" void SDLGPUglDepthRange(GLclampd near_value, GLclampd far_value)
{
	s_state.depth_near = std::max(0.0, std::min(1.0, near_value));
	s_state.depth_far = std::max(0.0, std::min(1.0, far_value));
}

extern "C" void SDLGPUglStencilFunc(GLenum function, GLint reference, GLuint mask)
{
	s_state.stencil_func = function;
	s_state.stencil_ref = reference;
	s_state.stencil_value_mask = mask;
}

extern "C" void SDLGPUglStencilMask(GLuint mask)
{
	s_state.stencil_write_mask = mask;
}

extern "C" void SDLGPUglStencilOp(GLenum fail, GLenum depth_fail, GLenum pass)
{
	s_state.stencil_fail = fail;
	s_state.stencil_depth_fail = depth_fail;
	s_state.stencil_pass = pass;
}

extern "C" void SDLGPUglCullFace(GLenum mode)
{
	s_state.cull_face = mode;
}

extern "C" void SDLGPUglFrontFace(GLenum mode)
{
	s_state.front_face = mode;
}

extern "C" void SDLGPUglPolygonMode(GLenum, GLenum mode)
{
	s_state.polygon_mode = mode;
}

extern "C" void SDLGPUglPolygonOffset(GLfloat factor, GLfloat units)
{
	s_state.polygon_offset_factor = factor;
	s_state.polygon_offset_units = units;
}

extern "C" void SDLGPUglColorMask(GLboolean red, GLboolean green,
	                                GLboolean blue, GLboolean alpha)
{
	s_state.color_mask[0] = red != GL_FALSE;
	s_state.color_mask[1] = green != GL_FALSE;
	s_state.color_mask[2] = blue != GL_FALSE;
	s_state.color_mask[3] = alpha != GL_FALSE;
}

extern "C" void SDLGPUglViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	s_state.viewport[0] = x;
	s_state.viewport[1] = y;
	s_state.viewport[2] = width;
	s_state.viewport[3] = height;
}

extern "C" void SDLGPUglScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	s_state.scissor[0] = x;
	s_state.scissor[1] = y;
	s_state.scissor[2] = width;
	s_state.scissor[3] = height;
}

extern "C" void SDLGPUglPointSize(GLfloat size)
{
	s_state.point_size = size;
}

extern "C" void SDLGPUglLineWidth(GLfloat width)
{
	s_state.line_width = width;
}

extern "C" void SDLGPUglShadeModel(GLenum mode)
{
	s_state.shade_model = mode;
}

extern "C" void SDLGPUglLogicOp(GLenum)
{
}

extern "C" void SDLGPUglClipPlane(GLenum plane, const GLdouble *equation)
{
	int index = (int)(plane - GL_CLIP_PLANE0);
	if (index < 0 || index >= 6 || !equation) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	std::memcpy(s_state.clip_plane[index], equation, sizeof(s_state.clip_plane[index]));
}

extern "C" void SDLGPUglColorMaterial(GLenum, GLenum mode)
{
	s_state.color_material_mode = mode;
}

extern "C" void SDLGPUglFogf(GLenum name, GLfloat parameter)
{
	if (name == GL_FOG_DENSITY) s_state.fog_density = parameter;
	else if (name == GL_FOG_START) s_state.fog_start = parameter;
	else if (name == GL_FOG_END) s_state.fog_end = parameter;
	else if (name == GL_FOG_MODE) s_state.fog_mode = (GLenum)(GLint)parameter;
	else if (name == GL_FOG_COORDINATE_SOURCE) s_state.fog_source = (GLenum)(GLint)parameter;
}

extern "C" void SDLGPUglFogi(GLenum name, GLint parameter)
{
	SDLGPUglFogf(name, (GLfloat)parameter);
}

extern "C" void SDLGPUglFogfv(GLenum name, const GLfloat *parameters)
{
	if (!parameters)
		return;
	if (name == GL_FOG_COLOR)
		SDLGPUCopyVector4(s_state.fog_color, parameters);
	else
		SDLGPUglFogf(name, parameters[0]);
}

extern "C" void SDLGPUglPushAttrib(GLbitfield)
{
	s_attrib_stack.push_back(s_state);
}

extern "C" void SDLGPUglPopAttrib(void)
{
	if (s_attrib_stack.empty()) {
		SDLGPUSetError(GL_STACK_UNDERFLOW);
		return;
	}
	s_state = s_attrib_stack.back();
	s_attrib_stack.pop_back();
}

extern "C" void SDLGPUglPixelStorei(GLenum name, GLint parameter)
{
	if (name == GL_UNPACK_ALIGNMENT)
		s_state.unpack_alignment = parameter;
	else if (name == GL_PACK_ALIGNMENT)
		s_state.pack_alignment = parameter;
}

extern "C" void SDLGPUglDrawBuffer(GLenum)
{
}

extern "C" void SDLGPUglReadBuffer(GLenum)
{
}

extern "C" void SDLGPUglVertexPointer(GLint, GLenum, GLsizei, const void *)
{
}

extern "C" void SDLGPUglTexGeni(GLenum, GLenum, GLint)
{
}

extern "C" void SDLGPUglLightfv(GLenum light_name, GLenum name, const GLfloat *parameters)
{
	int index = (int)(light_name - GL_LIGHT0);
	if (index < 0 || index >= 8 || !parameters)
		return;
	LightState &light = s_state.lights[index];
	if (name == GL_AMBIENT) SDLGPUCopyVector4(light.ambient, parameters);
	else if (name == GL_DIFFUSE) SDLGPUCopyVector4(light.diffuse, parameters);
	else if (name == GL_SPECULAR) SDLGPUCopyVector4(light.specular, parameters);
	else if (name == GL_POSITION) SDLGPUTransformVector(s_state.modelview, parameters, light.position);
	else if (name == GL_SPOT_DIRECTION) {
		light.spot_direction[0] = s_state.modelview.m[0] * parameters[0] +
			s_state.modelview.m[4] * parameters[1] + s_state.modelview.m[8] * parameters[2];
		light.spot_direction[1] = s_state.modelview.m[1] * parameters[0] +
			s_state.modelview.m[5] * parameters[1] + s_state.modelview.m[9] * parameters[2];
		light.spot_direction[2] = s_state.modelview.m[2] * parameters[0] +
			s_state.modelview.m[6] * parameters[1] + s_state.modelview.m[10] * parameters[2];
		SDLGPUNormalizeVector3(light.spot_direction);
	}
}

extern "C" void SDLGPUglLightf(GLenum light_name, GLenum name, GLfloat parameter)
{
	int index = (int)(light_name - GL_LIGHT0);
	if (index < 0 || index >= 8)
		return;
	LightState &light = s_state.lights[index];
	if (name == GL_SPOT_EXPONENT) light.spot_exponent = parameter;
	else if (name == GL_SPOT_CUTOFF) light.spot_cutoff = parameter;
	else if (name == GL_CONSTANT_ATTENUATION) light.constant_attenuation = parameter;
	else if (name == GL_LINEAR_ATTENUATION) light.linear_attenuation = parameter;
	else if (name == GL_QUADRATIC_ATTENUATION) light.quadratic_attenuation = parameter;
}

extern "C" void SDLGPUglLightModelfv(GLenum name, const GLfloat *parameters)
{
	if (name == GL_LIGHT_MODEL_AMBIENT && parameters)
		SDLGPUCopyVector4(s_state.light_model_ambient, parameters);
}

extern "C" void SDLGPUglLightModeli(GLenum name, GLint parameter)
{
	if (name == GL_LIGHT_MODEL_LOCAL_VIEWER)
		s_state.light_model_local_viewer = parameter != 0;
	else if (name == GL_LIGHT_MODEL_TWO_SIDE)
		s_state.light_model_two_side = parameter != 0;
}

extern "C" void SDLGPUglMaterialfv(GLenum, GLenum name, const GLfloat *parameters)
{
	if (!parameters)
		return;
	if (name == GL_AMBIENT) SDLGPUCopyVector4(s_state.material.ambient, parameters);
	else if (name == GL_DIFFUSE) SDLGPUCopyVector4(s_state.material.diffuse, parameters);
	else if (name == GL_SPECULAR) SDLGPUCopyVector4(s_state.material.specular, parameters);
	else if (name == GL_EMISSION) SDLGPUCopyVector4(s_state.material.emission, parameters);
	else if (name == GL_AMBIENT_AND_DIFFUSE) {
		SDLGPUCopyVector4(s_state.material.ambient, parameters);
		SDLGPUCopyVector4(s_state.material.diffuse, parameters);
	} else if (name == GL_SHININESS)
		s_state.material.shininess = parameters[0];
}

extern "C" void SDLGPUglMaterialf(GLenum face, GLenum name, GLfloat parameter)
{
	SDLGPUglMaterialfv(face, name, &parameter);
}

extern "C" void SDLGPUglTexEnvi(GLenum, GLenum name, GLint parameter)
{
	TextureEnvironment &environment = s_state.texenv[s_state.active_texture];
	if (name == GL_TEXTURE_ENV_MODE) environment.mode = parameter;
	else if (name == GL_COMBINE_RGB) environment.combine_rgb = parameter;
	else if (name == GL_COMBINE_ALPHA) environment.combine_alpha = parameter;
	else if (name >= GL_SOURCE0_RGB && name <= GL_SOURCE2_RGB)
		environment.source_rgb[name - GL_SOURCE0_RGB] = parameter;
	else if (name >= GL_SOURCE0_ALPHA && name <= GL_SOURCE2_ALPHA)
		environment.source_alpha[name - GL_SOURCE0_ALPHA] = parameter;
	else if (name >= GL_OPERAND0_RGB && name <= GL_OPERAND2_RGB)
		environment.operand_rgb[name - GL_OPERAND0_RGB] = parameter;
	else if (name >= GL_OPERAND0_ALPHA && name <= GL_OPERAND2_ALPHA)
		environment.operand_alpha[name - GL_OPERAND0_ALPHA] = parameter;
	else if (name == GL_RGB_SCALE) environment.rgb_scale = (GLfloat)parameter;
	else if (name == GL_ALPHA_SCALE) environment.alpha_scale = (GLfloat)parameter;
}

extern "C" void SDLGPUglTexEnvf(GLenum target, GLenum name, GLfloat parameter)
{
	if (name == GL_RGB_SCALE)
		s_state.texenv[s_state.active_texture].rgb_scale = parameter;
	else if (name == GL_ALPHA_SCALE)
		s_state.texenv[s_state.active_texture].alpha_scale = parameter;
	else
		SDLGPUglTexEnvi(target, name, (GLint)parameter);
}

extern "C" void SDLGPUglTexEnvfv(GLenum target, GLenum name, const GLfloat *parameters)
{
	if (!parameters)
		return;
	if (name == GL_TEXTURE_ENV_COLOR)
		SDLGPUCopyVector4(s_state.texenv[s_state.active_texture].color, parameters);
	else
		SDLGPUglTexEnvf(target, name, parameters[0]);
}

extern "C" void SDLGPUglGenTextures(GLsizei count, GLuint *textures)
{
	if (!textures || count < 0)
		return;
	for (GLsizei index = 0; index < count; index++) {
		while (s_textures.find(s_next_texture) != s_textures.end())
			s_next_texture++;
		TextureObject *object = new TextureObject;
		object->name = s_next_texture;
		s_textures[object->name] = object;
		textures[index] = object->name;
		s_next_texture++;
	}
}

extern "C" void SDLGPUglDeleteTextures(GLsizei count, const GLuint *textures)
{
	if (!textures || count < 0)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	for (GLsizei index = 0; index < count; index++) {
		std::map<GLuint, TextureObject *>::iterator found = s_textures.find(textures[index]);
		if (found == s_textures.end())
			continue;
		for (int unit = 0; unit < 2; unit++) {
			if (s_state.bound_texture_2d[unit] == textures[index])
				s_state.bound_texture_2d[unit] = 0;
			if (s_state.bound_texture_3d[unit] == textures[index])
				s_state.bound_texture_3d[unit] = 0;
		}
		SDLGPUReleaseTextureResources(found->second);
		delete found->second;
		s_textures.erase(found);
	}
}

extern "C" void SDLGPUglBindTexture(GLenum target, GLuint texture)
{
	if (texture != 0 && s_textures.find(texture) == s_textures.end()) {
		TextureObject *object = new TextureObject;
		object->name = texture;
		object->target = target;
		s_textures[texture] = object;
	}
	if (target == GL_TEXTURE_3D)
		s_state.bound_texture_3d[s_state.active_texture] = texture;
	else if (target == GL_TEXTURE_2D)
		s_state.bound_texture_2d[s_state.active_texture] = texture;
	else
		SDLGPUSetError(GL_INVALID_ENUM);
}

extern "C" void SDLGPUglTexParameteri(GLenum target, GLenum name, GLint parameter)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object)
		return;
	GLint *value = NULL;
	if (name == GL_TEXTURE_MIN_FILTER) value = &object->min_filter;
	else if (name == GL_TEXTURE_MAG_FILTER) value = &object->mag_filter;
	else if (name == GL_TEXTURE_WRAP_S) value = &object->wrap_s;
	else if (name == GL_TEXTURE_WRAP_T) value = &object->wrap_t;
	else if (name == GL_TEXTURE_WRAP_R) value = &object->wrap_r;
	else if (name == GL_TEXTURE_MAX_LEVEL) value = &object->max_level;
	if (!value || *value == parameter)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	*value = parameter;
	object->sampler_dirty = true;
}

extern "C" void SDLGPUglTexParameterf(GLenum target, GLenum name, GLfloat parameter)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object)
		return;
#ifdef GL_TEXTURE_LOD_BIAS
	if (name == GL_TEXTURE_LOD_BIAS) {
		if (object->lod_bias == parameter)
			return;
		if (!SDLGPUFlushPendingDraws())
			return;
		object->lod_bias = parameter;
		object->sampler_dirty = true;
		return;
	}
#endif
	SDLGPUglTexParameteri(target, name, (GLint)parameter);
}

static void SDLGPUDefineTextureLevel(TextureObject *object, int level, int width,
	                            int height, int depth)
{
	if (!object || level < 0 || level >= object->levels)
		return;
	TextureLevel &level_data = object->level_data[(size_t)level];
	bool completeness_changed = !level_data.defined || level_data.width != width ||
		level_data.height != height || level_data.depth != depth;
	level_data.width = width;
	level_data.height = height;
	level_data.depth = depth;
	level_data.defined = true;
	if (completeness_changed)
		object->sampler_dirty = true;
}

extern "C" void SDLGPUglTexImage2D(GLenum target, GLint level, GLint internal_format,
	                                 GLsizei width, GLsizei height, GLint,
	                                 GLenum format, GLenum type, const void *pixels)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || level < 0 || width <= 0 || height <= 0)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	int base_width = width << level;
	int base_height = height << level;
	if (!SDLGPUAllocateTexture(object, target, base_width, base_height, 1, internal_format))
		return;
	if (!pixels) {
		SDLGPUDefineTextureLevel(object, level, width, height, 1);
		return;
	}
	if (SDLGPUUploadTextureRegion(object, level, 0, 0, 0, width, height, 1,
		pixels, format, type, s_state.unpack_alignment, internal_format))
		SDLGPUDefineTextureLevel(object, level, width, height, 1);
}

extern "C" void SDLGPUglTexImage3D(GLenum target, GLint level, GLint internal_format,
	                                 GLsizei width, GLsizei height, GLsizei depth, GLint,
	                                 GLenum format, GLenum type, const void *pixels)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || level < 0 || width <= 0 || height <= 0 || depth <= 0)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	int base_width = width << level;
	int base_height = height << level;
	int base_depth = depth << level;
	if (!SDLGPUAllocateTexture(object, target, base_width, base_height, base_depth, internal_format))
		return;
	if (!pixels) {
		SDLGPUDefineTextureLevel(object, level, width, height, depth);
		return;
	}
	if (SDLGPUUploadTextureRegion(object, level, 0, 0, 0, width, height, depth,
		pixels, format, type, s_state.unpack_alignment, internal_format))
		SDLGPUDefineTextureLevel(object, level, width, height, depth);
}

static void SDLGPUDefineUpdatedTextureLevel(TextureObject *object, int level)
{
	if (!object || level < 0 || level >= (int)object->level_data.size())
		return;
	TextureLevel &destination = object->level_data[(size_t)level];
	int level_width = std::max(1, object->width >> level);
	int level_height = std::max(1, object->height >> level);
	int level_depth = std::max(1, object->depth >> level);
	bool completeness_changed = !destination.defined ||
		destination.width != level_width || destination.height != level_height ||
		destination.depth != level_depth;
	destination.width = level_width;
	destination.height = level_height;
	destination.depth = level_depth;
	destination.defined = true;
	if (completeness_changed)
		object->sampler_dirty = true;
}

extern "C" void SDLGPUglTexSubImage2D(GLenum target, GLint level, GLint xoffset,
	                                    GLint yoffset, GLsizei width, GLsizei height,
	                                    GLenum format, GLenum type, const void *pixels)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || !object->texture || !pixels)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	if (SDLGPUUploadTextureRegion(object, level, xoffset, yoffset, 0, width, height, 1,
		pixels, format, type, s_state.unpack_alignment, object->internal_format))
		SDLGPUDefineUpdatedTextureLevel(object, level);
}

extern "C" void SDLGPUglTexSubImage3D(GLenum target, GLint level, GLint xoffset,
	                                    GLint yoffset, GLint zoffset, GLsizei width,
	                                    GLsizei height, GLsizei depth, GLenum format,
	                                    GLenum type, const void *pixels)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || !object->texture || !pixels)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	if (SDLGPUUploadTextureRegion(object, level, xoffset, yoffset, zoffset, width,
		height, depth, pixels, format, type, s_state.unpack_alignment,
		object->internal_format))
		SDLGPUDefineUpdatedTextureLevel(object, level);
}

extern "C" void SDLGPUglGenerateMipmap(GLenum target)
{
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || !object->texture)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command)
		return;
	SDL_GenerateMipmapsForGPUTexture(command, object->texture);
	for (int level = 0; level < object->levels; level++)
		object->level_data[(size_t)level].defined = true;
	object->sampler_dirty = true;
}

extern "C" void SDLGPUglGenFramebuffers(GLsizei count, GLuint *framebuffers)
{
	if (!framebuffers || count < 0)
		return;
	for (GLsizei index = 0; index < count; index++) {
		while (s_framebuffers.find(s_next_framebuffer) != s_framebuffers.end())
			s_next_framebuffer++;
		FramebufferObject *object = new FramebufferObject;
		object->name = s_next_framebuffer;
		s_framebuffers[object->name] = object;
		framebuffers[index] = object->name;
		s_next_framebuffer++;
	}
}

extern "C" void SDLGPUglDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
	if (!framebuffers || count < 0)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	for (GLsizei index = 0; index < count; index++) {
		std::map<GLuint, FramebufferObject *>::iterator found =
			s_framebuffers.find(framebuffers[index]);
		if (found == s_framebuffers.end())
			continue;
		if (s_bound_framebuffer == framebuffers[index])
			s_bound_framebuffer = 0;
		delete found->second;
		s_framebuffers.erase(found);
	}
}

extern "C" void SDLGPUglBindFramebuffer(GLenum target, GLuint framebuffer)
{
	if (target != GL_FRAMEBUFFER) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	if (framebuffer != 0 && s_framebuffers.find(framebuffer) == s_framebuffers.end()) {
		FramebufferObject *object = new FramebufferObject;
		object->name = framebuffer;
		s_framebuffers[framebuffer] = object;
	}
	s_bound_framebuffer = framebuffer;
}

extern "C" void SDLGPUglFramebufferTexture2D(GLenum target, GLenum attachment,
	                                           GLenum, GLuint texture, GLint level)
{
	if (target != GL_FRAMEBUFFER || attachment != GL_COLOR_ATTACHMENT0 ||
		s_bound_framebuffer == 0)
		return;
	FramebufferObject *framebuffer = s_framebuffers[s_bound_framebuffer];
	framebuffer->color_texture = texture;
	framebuffer->color_level = level;
}

extern "C" void SDLGPUglGenRenderbuffers(GLsizei count, GLuint *renderbuffers)
{
	if (!renderbuffers || count < 0)
		return;
	for (GLsizei index = 0; index < count; index++) {
		while (s_renderbuffers.find(s_next_renderbuffer) != s_renderbuffers.end())
			s_next_renderbuffer++;
		RenderbufferObject *object = new RenderbufferObject;
		object->name = s_next_renderbuffer;
		s_renderbuffers[object->name] = object;
		renderbuffers[index] = object->name;
		s_next_renderbuffer++;
	}
}

extern "C" void SDLGPUglDeleteRenderbuffers(GLsizei count, const GLuint *renderbuffers)
{
	if (!renderbuffers || count < 0)
		return;
	if (!SDLGPUFlushPendingDraws())
		return;
	for (GLsizei index = 0; index < count; index++) {
		std::map<GLuint, RenderbufferObject *>::iterator found =
			s_renderbuffers.find(renderbuffers[index]);
		if (found == s_renderbuffers.end())
			continue;
		if (s_bound_renderbuffer == renderbuffers[index])
			s_bound_renderbuffer = 0;
		if (found->second->texture)
			SDLGPUReleaseTextureResource(found->second->texture);
		delete found->second;
		s_renderbuffers.erase(found);
	}
}

extern "C" void SDLGPUglBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
	if (target != GL_RENDERBUFFER) {
		SDLGPUSetError(GL_INVALID_ENUM);
		return;
	}
	if (renderbuffer != 0 && s_renderbuffers.find(renderbuffer) == s_renderbuffers.end()) {
		RenderbufferObject *object = new RenderbufferObject;
		object->name = renderbuffer;
		s_renderbuffers[renderbuffer] = object;
	}
	s_bound_renderbuffer = renderbuffer;
}

extern "C" void SDLGPUglRenderbufferStorage(GLenum target, GLenum,
	                                            GLsizei width, GLsizei height)
{
	if (target != GL_RENDERBUFFER || s_bound_renderbuffer == 0)
		return;
	RenderbufferObject *object = s_renderbuffers[s_bound_renderbuffer];
	if (!SDLGPUFlushPendingDraws())
		return;
	if (object->texture)
		SDLGPUReleaseTextureResource(object->texture);
	object->width = width;
	object->height = height;
	object->texture = SDLGPUCreateDepthTarget(width, height);
}

extern "C" void SDLGPUglFramebufferRenderbuffer(GLenum target, GLenum attachment,
	                                              GLenum, GLuint renderbuffer)
{
	if (target != GL_FRAMEBUFFER || attachment != GL_DEPTH_ATTACHMENT ||
		s_bound_framebuffer == 0)
		return;
	s_framebuffers[s_bound_framebuffer]->depth_renderbuffer = renderbuffer;
}

extern "C" GLenum SDLGPUglCheckFramebufferStatus(GLenum target)
{
	if (target != GL_FRAMEBUFFER)
		return GL_INVALID_ENUM;
	if (s_bound_framebuffer == 0)
		return GL_FRAMEBUFFER_COMPLETE;
	std::map<GLuint, FramebufferObject *>::iterator found =
		s_framebuffers.find(s_bound_framebuffer);
	if (found == s_framebuffers.end())
		return GL_INVALID_OPERATION;
	TextureObject *color = SDLGPUFindTexture(found->second->color_texture);
	if (!color || !color->texture)
		return GL_INVALID_OPERATION;
	return GL_FRAMEBUFFER_COMPLETE;
}

extern "C" void SDLGPUglClearColor(GLclampf red, GLclampf green,
	                                 GLclampf blue, GLclampf alpha)
{
	s_state.clear_color[0] = red;
	s_state.clear_color[1] = green;
	s_state.clear_color[2] = blue;
	s_state.clear_color[3] = alpha;
}

extern "C" void SDLGPUglClearDepth(GLclampd depth)
{
	s_state.clear_depth = depth;
}

extern "C" void SDLGPUglClearStencil(GLint stencil)
{
	s_state.clear_stencil = stencil;
}

extern "C" void SDLGPUglClear(GLbitfield mask)
{
	if (!SDLGPUFlushPendingDraws())
		return;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	SDL_GPUTexture *color = NULL;
	SDL_GPUTexture *depth = NULL;
	int width = 0;
	int height = 0;
	int color_level = 0;
	if (!SDLGPUGetCurrentTargets(&color, &depth, &width, &height, &color_level))
		return;
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command)
		return;
	SDL_GPUColorTargetInfo color_info;
	std::memset(&color_info, 0, sizeof(color_info));
	color_info.texture = color;
	color_info.mip_level = (Uint32)color_level;
	color_info.load_op = SDL_GPU_LOADOP_LOAD;
	if ((mask & GL_COLOR_BUFFER_BIT) != 0)
		color_info.load_op = SDL_GPU_LOADOP_CLEAR;
	color_info.store_op = SDL_GPU_STOREOP_STORE;
	color_info.clear_color.r = s_state.clear_color[0];
	color_info.clear_color.g = s_state.clear_color[1];
	color_info.clear_color.b = s_state.clear_color[2];
	color_info.clear_color.a = s_state.clear_color[3];
	SDL_GPUDepthStencilTargetInfo depth_info;
	std::memset(&depth_info, 0, sizeof(depth_info));
	depth_info.texture = depth;
	depth_info.load_op = SDL_GPU_LOADOP_LOAD;
	depth_info.store_op = SDL_GPU_STOREOP_STORE;
	depth_info.stencil_load_op = SDL_GPU_LOADOP_LOAD;
	depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
	depth_info.clear_depth = (float)s_state.clear_depth;
	depth_info.clear_stencil = (Uint8)s_state.clear_stencil;
	if ((mask & GL_DEPTH_BUFFER_BIT) != 0)
		depth_info.load_op = SDL_GPU_LOADOP_CLEAR;
	if ((mask & GL_STENCIL_BUFFER_BIT) != 0)
		depth_info.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
	const SDL_GPUDepthStencilTargetInfo *depth_pointer = NULL;
	if (depth && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0)
		depth_pointer = &depth_info;
	SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &color_info, 1,
	                                                depth_pointer);
	if (!pass) {
		SDLGPUCancelDeferredCommand();
		return;
	}
	SDL_EndGPURenderPass(pass);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.clear_ticks += SDLGPUProfileNow() - profile_start_tick;
	s_transition_profile.clear_count++;
#endif
}

static bool SDLGPUDownloadTexture(SDL_GPUTexture *texture, SDL_GPUTextureFormat format,
	                         int mip_level, int x, int y, int width, int height,
	                         std::vector<unsigned char> &output)
{
	if (!texture || width <= 0 || height <= 0)
		return false;
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	if (!SDLGPUFlushPendingDraws()) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	Uint32 size = SDL_CalculateGPUTextureFormatSize(format, (Uint32)width,
	                                              (Uint32)height, 1);
	if (size == 0) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUTransferBufferCreateInfo transfer_info;
	std::memset(&transfer_info, 0, sizeof(transfer_info));
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size = size;
	SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(s_device, &transfer_info);
	if (!transfer) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUCommandBuffer *command = SDLGPUAcquireDeferredCommand();
	if (!command) {
		SDL_ReleaseGPUTransferBuffer(s_device, transfer);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
	if (!copy) {
		SDLGPUCancelDeferredCommand();
		SDL_ReleaseGPUTransferBuffer(s_device, transfer);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUTextureRegion source;
	SDL_GPUTextureTransferInfo destination;
	std::memset(&source, 0, sizeof(source));
	std::memset(&destination, 0, sizeof(destination));
	source.texture = texture;
	source.mip_level = (Uint32)mip_level;
	source.x = (Uint32)x;
	source.y = (Uint32)y;
	source.w = (Uint32)width;
	source.h = (Uint32)height;
	source.d = 1;
	destination.transfer_buffer = transfer;
	destination.pixels_per_row = (Uint32)width;
	destination.rows_per_layer = (Uint32)height;
	SDL_DownloadFromGPUTexture(copy, &source, &destination);
	SDL_EndGPUCopyPass(copy);
	s_deferred_command = NULL;
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
	if (!fence) {
		SDL_ReleaseGPUTransferBuffer(s_device, transfer);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		return SDLGPUProfileReadbackResult(false, profile_start_tick);
#else
		return false;
#endif
	}
	SDL_GPUFence *fences[1] = {fence};
	SDL_WaitForGPUFences(s_device, true, fences, 1);
	void *mapped = SDL_MapGPUTransferBuffer(s_device, transfer, false);
	if (mapped) {
		output.resize(size);
		std::memcpy(&output[0], mapped, size);
		SDL_UnmapGPUTransferBuffer(s_device, transfer);
	}
	SDL_ReleaseGPUFence(s_device, fence);
	SDL_ReleaseGPUTransferBuffer(s_device, transfer);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	return SDLGPUProfileReadbackResult(mapped != NULL, profile_start_tick);
#else
	return mapped != NULL;
#endif
}

extern "C" void SDLGPUglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
	                                 GLenum format, GLenum type, void *pixels)
{
	if (!pixels || width <= 0 || height <= 0)
		return;
	SDL_GPUTexture *color = NULL;
	SDL_GPUTexture *depth = NULL;
	int target_width = 0;
	int target_height = 0;
	int color_level = 0;
	if (!SDLGPUGetCurrentTargets(&color, &depth, &target_width, &target_height,
		&color_level))
		return;
	int source_y = target_height - y - height;
	if (source_y < 0)
		source_y = 0;
	std::vector<unsigned char> downloaded;
	if (format == GL_DEPTH_COMPONENT) {
		if (type != GL_FLOAT || !SDLGPUDownloadTexture(depth,
			SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, 0, x, source_y, width, height,
			downloaded))
			return;
		int source_stride = 8;
		if ((int)downloaded.size() < width * height * source_stride)
			source_stride = 4;
		int destination_pitch = SDLGPUAlignInteger(width * (int)sizeof(float),
			s_state.pack_alignment);
		unsigned char *destination = static_cast<unsigned char *>(pixels);
		for (int row = 0; row < height; row++) {
			float *destination_row = reinterpret_cast<float *>(destination +
				(size_t)row * (size_t)destination_pitch);
			int source_row = height - row - 1;
			for (int column = 0; column < width; column++) {
				float value = 1.0f;
				std::memcpy(&value, &downloaded[((size_t)source_row * (size_t)width +
					(size_t)column) * (size_t)source_stride], sizeof(value));
				destination_row[column] = value;
			}
		}
		return;
	}
	if (type != GL_UNSIGNED_BYTE || !SDLGPUDownloadTexture(color,
		SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, color_level, x, source_y, width, height,
		downloaded))
		return;
	int destination_components = 4;
	if (format == GL_RGB)
		destination_components = 3;
	int destination_pitch = SDLGPUAlignInteger(width * destination_components,
		s_state.pack_alignment);
	unsigned char *destination = static_cast<unsigned char *>(pixels);
	for (int row = 0; row < height; row++) {
		const unsigned char *source = &downloaded[(size_t)(height - row - 1) *
			(size_t)width * 4];
		unsigned char *destination_row = destination + (size_t)row *
			(size_t)destination_pitch;
		for (int column = 0; column < width; column++) {
			if (format == GL_BGRA) {
				std::memcpy(destination_row + column * 4, source + column * 4, 4);
			} else if (format == GL_RGB) {
				destination_row[column * 3 + 0] = source[column * 4 + 2];
				destination_row[column * 3 + 1] = source[column * 4 + 1];
				destination_row[column * 3 + 2] = source[column * 4 + 0];
			} else {
				destination_row[column * 4 + 0] = source[column * 4 + 2];
				destination_row[column * 4 + 1] = source[column * 4 + 1];
				destination_row[column * 4 + 2] = source[column * 4 + 0];
				destination_row[column * 4 + 3] = source[column * 4 + 3];
			}
		}
	}
}

extern "C" void SDLGPUglRasterPos2i(GLint x, GLint y)
{
	float object[4] = {(float)x, (float)y, 0.0f, 1.0f};
	float eye[4];
	float clip[4];
	SDLGPUTransformVector(s_state.modelview, object, eye);
	SDLGPUTransformVector(s_state.projection, eye, clip);
	if (std::fabs(clip[3]) < 0.000001f) {
		s_state.raster_valid = false;
		return;
	}
	float ndc_x = clip[0] / clip[3];
	float ndc_y = clip[1] / clip[3];
	s_state.raster_x = s_state.viewport[0] + (ndc_x + 1.0f) *
		(float)s_state.viewport[2] * 0.5f;
	s_state.raster_y = s_state.viewport[1] + (ndc_y + 1.0f) *
		(float)s_state.viewport[3] * 0.5f;
	s_state.raster_valid = true;
}

extern "C" void SDLGPUglWindowPos2i(GLint x, GLint y)
{
	s_state.raster_x = (float)x;
	s_state.raster_y = (float)y;
	s_state.raster_valid = true;
}

static CompatVertex SDLGPUCreateScreenVertex(float x, float y, float s, float t,
	                              int target_width, int target_height)
{
	CompatVertex vertex;
	std::memset(&vertex, 0, sizeof(vertex));
	vertex.position[0] = x / (float)target_width * 2.0f - 1.0f;
	vertex.position[1] = y / (float)target_height * 2.0f - 1.0f;
	vertex.position[2] = 0.0f;
	vertex.position[3] = 1.0f;
	vertex.color[0] = 1.0f;
	vertex.color[1] = 1.0f;
	vertex.color[2] = 1.0f;
	vertex.color[3] = 1.0f;
	vertex.secondary[3] = 1.0f;
	vertex.texcoord0[0] = s;
	vertex.texcoord0[1] = t;
	vertex.texcoord0[3] = 1.0f;
	vertex.texcoord1[3] = 1.0f;
	vertex.eye_position[3] = 1.0f;
	return vertex;
}

static void SDLGPUDrawPixelBlock(int width, int height, GLenum format, GLenum type,
	                         const void *pixels)
{
	if (!s_state.raster_valid || !pixels || width <= 0 || height <= 0)
		return;
	GLuint texture_name = 0;
	SDLGPUglGenTextures(1, &texture_name);
	FixedState saved = s_state;
	SDLGPUglActiveTexture(GL_TEXTURE0);
	SDLGPUglBindTexture(GL_TEXTURE_2D, texture_name);
	SDLGPUglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	SDLGPUglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	SDLGPUglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, type, pixels);
	s_state.texture_2d_enabled[0] = true;
	s_state.texture_3d_enabled[0] = false;
	s_state.texture_2d_enabled[1] = false;
	s_state.texture_3d_enabled[1] = false;
	s_state.texenv[0].mode = GL_REPLACE;
	s_state.lighting_enabled = false;
	s_state.fog_enabled = false;
	s_state.alpha_test_enabled = false;
	SDL_GPUTexture *color = NULL;
	SDL_GPUTexture *depth = NULL;
	int target_width = 0;
	int target_height = 0;
	int color_level = 0;
	if (SDLGPUGetCurrentTargets(&color, &depth, &target_width, &target_height,
		&color_level)) {
		float left = saved.raster_x;
		float bottom = saved.raster_y;
		float right = left + width;
		float top = bottom + height;
		std::vector<CompatVertex> quad;
		quad.push_back(SDLGPUCreateScreenVertex(left, bottom, 0.0f, 1.0f, target_width, target_height));
		quad.push_back(SDLGPUCreateScreenVertex(right, bottom, 1.0f, 1.0f, target_width, target_height));
		quad.push_back(SDLGPUCreateScreenVertex(right, top, 1.0f, 0.0f, target_width, target_height));
		quad.push_back(SDLGPUCreateScreenVertex(left, bottom, 0.0f, 1.0f, target_width, target_height));
		quad.push_back(SDLGPUCreateScreenVertex(right, top, 1.0f, 0.0f, target_width, target_height));
		quad.push_back(SDLGPUCreateScreenVertex(left, top, 0.0f, 0.0f, target_width, target_height));
		SDLGPUQueueVertices(GL_TRIANGLES, quad);
	}
	s_state = saved;
	SDLGPUglDeleteTextures(1, &texture_name);
}

extern "C" void SDLGPUglDrawPixels(GLsizei width, GLsizei height,
	                                 GLenum format, GLenum type, const void *pixels)
{
	SDLGPUDrawPixelBlock(width, height, format, type, pixels);
}

extern "C" void SDLGPUglBitmap(GLsizei width, GLsizei height, GLfloat xorigin,
	                             GLfloat yorigin, GLfloat xmove, GLfloat ymove,
	                             const GLubyte *bitmap)
{
	if (bitmap && width > 0 && height > 0) {
		std::vector<unsigned char> rgba((size_t)width * (size_t)height * 4, 0);
		int source_pitch = SDLGPUAlignInteger((width + 7) / 8, s_state.unpack_alignment);
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				if ((bitmap[y * source_pitch + x / 8] & (0x80 >> (x & 7))) != 0) {
					size_t offset = ((size_t)y * (size_t)width + (size_t)x) * 4;
					rgba[offset + 0] = (unsigned char)(s_state.current_color[2] * 255.0f);
					rgba[offset + 1] = (unsigned char)(s_state.current_color[1] * 255.0f);
					rgba[offset + 2] = (unsigned char)(s_state.current_color[0] * 255.0f);
					rgba[offset + 3] = (unsigned char)(s_state.current_color[3] * 255.0f);
				}
			}
		}
		s_state.raster_x -= xorigin;
		s_state.raster_y -= yorigin;
		SDLGPUDrawPixelBlock(width, height, GL_BGRA, GL_UNSIGNED_BYTE, &rgba[0]);
		s_state.raster_x += xorigin;
		s_state.raster_y += yorigin;
	}
	s_state.raster_x += xmove;
	s_state.raster_y += ymove;
}

extern "C" void SDLGPUglCopyTexSubImage2D(GLenum target, GLint level,
	                                        GLint xoffset, GLint yoffset,
	                                        GLint x, GLint y, GLsizei width,
	                                        GLsizei height)
{
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
	std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4);
	SDLGPUglReadPixels(x, y, width, height, GL_BGRA, GL_UNSIGNED_BYTE, &pixels[0]);
	SDLGPUglTexSubImage2D(target, level, xoffset, yoffset, width, height,
	                     GL_BGRA, GL_UNSIGNED_BYTE, &pixels[0]);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	s_transition_profile.copy_texture_ticks += SDLGPUProfileNow() - profile_start_tick;
	s_transition_profile.copy_texture_count++;
#endif
}

extern "C" void SDLGPUglAccum(GLenum, GLfloat)
{
}

extern "C" GLenum SDLGPUglGetError(void)
{
	GLenum error = s_error;
	s_error = GL_NO_ERROR;
	return error;
}

extern "C" const GLubyte *SDLGPUglGetString(GLenum name)
{
	static const GLubyte vendor[] = "SDL";
	static const GLubyte version[] = "SDL-GPU fixed-function translator 1.0";
	static GLubyte renderer[128];
	if (name == GL_VENDOR)
		return vendor;
	if (name == GL_VERSION)
		return version;
	if (name == GL_RENDERER) {
		const char *driver = "uninitialized";
		if (s_device) {
			const char *selected = SDL_GetGPUDeviceDriver(s_device);
			if (selected)
				driver = selected;
		}
		SDL_snprintf(reinterpret_cast<char *>(renderer), sizeof(renderer),
			"SDL-GPU (%s)", driver);
		return renderer;
	}
	return reinterpret_cast<const GLubyte *>("");
}

extern "C" void SDLGPUglGetIntegerv(GLenum name, GLint *parameters)
{
	if (!parameters)
		return;
	if (name == GL_MATRIX_MODE) *parameters = (GLint)s_state.matrix_mode;
	else if (name == GL_ACTIVE_TEXTURE) *parameters = GL_TEXTURE0 + s_state.active_texture;
	else if (name == GL_MAX_TEXTURE_UNITS_ARB) *parameters = 2;
	else if (name == GL_TEXTURE_BINDING_2D)
		*parameters = (GLint)s_state.bound_texture_2d[s_state.active_texture];
	else if (name == GL_TEXTURE_BINDING_3D)
		*parameters = (GLint)s_state.bound_texture_3d[s_state.active_texture];
	else if (name == GL_BLEND_SRC) *parameters = (GLint)s_state.blend_src_rgb;
	else if (name == GL_BLEND_DST) *parameters = (GLint)s_state.blend_dst_rgb;
	else if (name == GL_ALPHA_TEST_FUNC) *parameters = (GLint)s_state.alpha_func;
	else if (name == GL_PACK_ALIGNMENT) *parameters = s_state.pack_alignment;
	else if (name == GL_UNPACK_ALIGNMENT) *parameters = s_state.unpack_alignment;
	else if (name == GL_VIEWPORT) std::memcpy(parameters, s_state.viewport, sizeof(s_state.viewport));
	else *parameters = 0;
}

extern "C" void SDLGPUglGetFloatv(GLenum name, GLfloat *parameters)
{
	if (!parameters)
		return;
	if (name == GL_ALPHA_TEST_REF)
		*parameters = s_state.alpha_ref;
	else if (name == GL_CURRENT_COLOR)
		SDLGPUCopyVector4(parameters, s_state.current_color);
	else
		*parameters = 0.0f;
}

extern "C" void SDLGPUglGetTexEnviv(GLenum, GLenum name, GLint *parameters)
{
	if (!parameters)
		return;
	TextureEnvironment &environment = s_state.texenv[s_state.active_texture];
	if (name == GL_TEXTURE_ENV_MODE) *parameters = environment.mode;
	else if (name == GL_COMBINE_RGB) *parameters = environment.combine_rgb;
	else if (name == GL_COMBINE_ALPHA) *parameters = environment.combine_alpha;
	else *parameters = 0;
}

extern "C" void SDLGPUglGetTexLevelParameteriv(GLenum target, GLint level,
	                                             GLenum name, GLint *parameters)
{
	if (!parameters)
		return;
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object || level < 0 || level >= object->levels) {
		*parameters = 0;
		return;
	}
	if (name == GL_TEXTURE_WIDTH) *parameters = std::max(1, object->width >> level);
	else if (name == GL_TEXTURE_HEIGHT) *parameters = std::max(1, object->height >> level);
	else if (name == GL_TEXTURE_INTERNAL_FORMAT) *parameters = object->internal_format;
	else if (name == GL_TEXTURE_ALPHA_SIZE) {
		if (object->internal_format == GL_RGB || object->internal_format == GL_RGB8)
			*parameters = 0;
		else
			*parameters = 8;
	} else *parameters = 0;
}

extern "C" void SDLGPUglGetTexParameteriv(GLenum target, GLenum name,
	                                        GLint *parameters)
{
	if (!parameters)
		return;
	TextureObject *object = SDLGPUGetBoundTexture(target);
	if (!object) {
		*parameters = 0;
		return;
	}
	if (name == GL_TEXTURE_MIN_FILTER) *parameters = object->min_filter;
	else if (name == GL_TEXTURE_MAG_FILTER) *parameters = object->mag_filter;
	else if (name == GL_TEXTURE_WRAP_S) *parameters = object->wrap_s;
	else if (name == GL_TEXTURE_WRAP_T) *parameters = object->wrap_t;
	else if (name == GL_TEXTURE_WRAP_R) *parameters = object->wrap_r;
	else if (name == GL_TEXTURE_MAX_LEVEL) *parameters = object->max_level;
	else *parameters = 0;
}

extern "C" void SDLGPUglFinish(void)
{
	if (s_device) {
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		Uint64 profile_start_tick = SDLGPUProfileNow();
#endif
		SDLGPUFlushPendingDraws();
		SDLGPUSubmitDeferredCommand();
		SDL_WaitForGPUIdle(s_device);
#if SDLGPU_TRANSITION_LOGGING_ENABLED
		Uint64 elapsed_ticks = SDLGPUProfileNow() - profile_start_tick;
		s_transition_profile.finish_ticks += elapsed_ticks;
		s_transition_profile.finish_count++;
		double elapsed_ms = SDLGPUProfileMilliseconds(elapsed_ticks);
		if (elapsed_ms >= 5.0)
			bug("[gfxaccel-sdlgpu-timing] stamp=%llu slow-op=glFinish total=%.3fms\n",
				(unsigned long long)SDL_GetTicks(), elapsed_ms);
		SDLGPUProfileRuntimeCheckpoint();
#endif
	}
}

extern "C" void SDLGPUglFlush(void)
{
	SDLGPUFlushPendingDraws();
	SDLGPUSubmitDeferredCommand();
#if SDLGPU_TRANSITION_LOGGING_ENABLED
	SDLGPUProfileRuntimeCheckpoint();
#endif
}
