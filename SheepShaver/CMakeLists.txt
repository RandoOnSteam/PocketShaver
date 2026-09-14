# SheepShaver target (included from root CMakeLists.txt, or standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.16)
	if(CMAKE_HOST_APPLE)
		project(SheepShaver VERSION 2.3 LANGUAGES C CXX OBJC OBJCXX)
	else()
		project(SheepShaver VERSION 2.3 LANGUAGES C CXX)
	endif()
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_C_STANDARD 99)
	option(ENABLE_JIT "Legacy default for the PPC JIT" OFF)
	option(ENABLE_SHEEPSHAVER_JIT "Enable PPC JIT (precompiled dyngen)" ${ENABLE_JIT})
	option(ENABLE_GTK "GTK prefs UI (Unix)" OFF)
	option(ENABLE_BINCUE "BIN/CUE support" ON)
	option(ENABLE_VOSF "Video on SEGV" OFF)
	option(ENABLE_QD3D_INIT_LOGGING
	"Compile focused QuickDraw 3D/RAVE initialization diagnostics" OFF)
	option(ENABLE_QD3D_GRAPHICS_LOGGING
	"Compile high-volume QD3D graphics diagnostics" OFF)
	option(ENABLE_QD3D_AUDIO_LOGGING
	"Compile sound component and SDL audio diagnostics" OFF)
	option(ENABLE_QD3D_MEDIA_LOGGING
	"Compile CD/media I/O diagnostics" OFF)
	option(ENABLE_QD3D_WAIT_LOGGING
	"Compile high-volume timing/wait diagnostics" OFF)
	option(USE_SDL3 "Use SDL3" OFF)
	option(USE_SDLGPU "Use SDL-GPU for gfxaccel" OFF)
	option(ENABLE_SDLGPU_TRANSITION_LOGGING
		"Compile SDL-GPU transition diagnostics" ON)
	option(USE_SDLGPU_SHADERCROSS "Use SDL_shadercross as an SDL-GPU shader fallback" OFF)
	if(USE_SDLGPU)
		set(USE_SDL3 ON CACHE BOOL "Use SDL3" FORCE)
	endif()
	set(MACEMU_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
	list(APPEND CMAKE_MODULE_PATH "${MACEMU_CMAKE_DIR}")
	include(MacEmuCommon)
	set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../BasiliskII/src")
	set(SS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
	if(APPLE)
		macemu_apple_init()
		macemu_apple_enable_languages()
	endif()
	macemu_detect_host()
	macemu_find_sdl()
endif()

if(APPLE AND NOT DEFINED MACEMU_BUILD_DESKTOP_SHEEPSHAVER)
	macemu_apple_init()
	macemu_apple_enable_languages()
endif()

if(NOT DEFINED SS_SRC)
	set(SS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(NOT DEFINED B2_SRC)
	set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../BasiliskII/src")
endif()

if(NOT EXISTS "${B2_SRC}/main.cpp")
	message(FATAL_ERROR "Basilisk II sources not found at ${B2_SRC}")
endif()

set(SS_KPX "${SS_SRC}/kpx_cpu")

if(ENABLE_SHEEPSHAVER_JIT)
	set(ENABLE_DYNGEN 1)
endif()


if(WIN32)
	set(SS_SYSDEPS "${SS_SRC}/Windows")
elseif(APPLE)
	# Vendored per-arch headers in MacOSX/config (iOS vs macOS, Catalyst JIT).
	set(SS_SYSDEPS "${SS_SRC}/MacOSX/config")
else()
	set(SS_SYSDEPS "${SS_SRC}/Unix")
endif()
if(NOT APPLE)
	macemu_write_config(
		"${SS_SYSDEPS}"
		"SheepShaver" "SheepShaver" "2.3" "")
endif()




# ---------------------------------------------------------------------------
# Sources shared with Basilisk II are named by their real path in the
# BasiliskII tree, never through the matching symlink in SheepShaver/src.
# "make links" puts those symlinks there for the autoconf build, which
# compiles bare file names, but a symlink carries its own timestamp and Ninja
# reads that rather than the file it points at, so a build driven through one
# never notices an edit. Include precedence still comes from the order of
# target_include_directories below, SS paths before B2 paths, so SheepShaver
# headers win over their Basilisk II twins.
# ---------------------------------------------------------------------------
set(SS_SHARED
	"${B2_SRC}/adb.cpp"
	"${B2_SRC}/audio.cpp"
	"${B2_SRC}/cdrom.cpp"
	"${B2_SRC}/disk.cpp"
	"${B2_SRC}/extfs.cpp"
	"${B2_SRC}/prefs.cpp"
	"${B2_SRC}/scsi.cpp"
	"${B2_SRC}/sony.cpp"
	"${B2_SRC}/timer.cpp"
	"${B2_SRC}/xpram.cpp"
)
if(ENABLE_BINCUE)
	list(APPEND SS_SHARED "${B2_SRC}/bincue.cpp")
endif()

macemu_slirp_sources("${B2_SRC}" SS_SLIRP)
macemu_sdl_sources("${B2_SRC}" SS_SDL)
set(SS_XPLAT
	"${B2_SRC}/CrossPlatform/vm_alloc.cpp"
	"${B2_SRC}/CrossPlatform/sigsegv.cpp"
	"${B2_SRC}/CrossPlatform/video_blit.cpp"
)

set(SS_CORE
	"${SS_SRC}/main.cpp"
	"${SS_SRC}/prefs_items.cpp"
	"${SS_SRC}/rom_patches.cpp"
	"${SS_SRC}/rsrc_patches.cpp"
	"${SS_SRC}/emul_op.cpp"
	"${B2_SRC}/joymanager.cpp"
	"${SS_SRC}/name_registry.cpp"
	"${SS_SRC}/usbhid.cpp"
	"${SS_SRC}/macio_escc.cpp"
	"${SS_SRC}/mmio.cpp"
	"${SS_SRC}/pcibridge.cpp"
	"${SS_SRC}/usbuim.cpp"
	"${SS_SRC}/macos_util.cpp"
	"${SS_SRC}/gfxaccel.cpp"
	"${SS_SRC}/video.cpp"
	"${SS_SRC}/ether.cpp"
	"${SS_SRC}/thunks.cpp"
	"${SS_SRC}/serial.cpp"
	"${SS_SRC}/user_strings.cpp"
	${SS_SHARED}
	${SS_XPLAT}
	${SS_SDL}
	${SS_SLIRP}
)

# PowerPC emulator
set(SS_CPU
	"${SS_KPX}/src/mathlib/ieeefp.cpp"
	"${SS_KPX}/src/mathlib/mathlib.cpp"
	"${SS_KPX}/src/cpu/ppc/ppc-cpu.cpp"
	"${SS_KPX}/src/cpu/ppc/ppc-decode.cpp"
	"${SS_KPX}/src/cpu/ppc/ppc-execute.cpp"
	"${SS_KPX}/src/cpu/ppc/ppc-translate.cpp"
	"${SS_KPX}/src/utils/utils-cpuinfo.cpp"
	"${SS_KPX}/sheepshaver_glue.cpp"
	"${SS_KPX}/ppc-dis.c"
)

# ppc-execute.cpp does #include "ppc-execute-impl.cpp" and the JIT does
# #include "*-dyngen-ops.hpp". These live in the dyngen_precompiled dir; we add
# that dir to the include paths (no staging) so the #include resolves in place.
# OBJECT_DEPENDS on the including TUs makes an edit to those files recompile.
set(SS_DYNGEN_INC)
set(SS_PPC_EXECUTE_IMPL "${SS_SRC}/Unix/dyngen_precompiled/ppc-execute-impl.cpp")
if(NOT EXISTS "${SS_PPC_EXECUTE_IMPL}")
	message(FATAL_ERROR "Missing ${SS_PPC_EXECUTE_IMPL}")
endif()
list(APPEND SS_DYNGEN_INC "${SS_SRC}/Unix/dyngen_precompiled")
set_source_files_properties(
	"${SS_KPX}/src/cpu/ppc/ppc-execute.cpp"
	PROPERTIES OBJECT_DEPENDS "${SS_PPC_EXECUTE_IMPL}")

# Xcode always compiles the dyngen TUs; ENABLE_DYNGEN in config.h (Catalyst
# only on Apple) decides whether the JIT actually runs.
if(ENABLE_SHEEPSHAVER_JIT OR APPLE)
	list(APPEND SS_CPU
		"${SS_KPX}/src/cpu/jit/jit-cache.cpp"
		"${SS_KPX}/src/cpu/jit/basic-dyngen.cpp"
		"${SS_KPX}/src/cpu/ppc/ppc-dyngen.cpp"
		"${SS_KPX}/src/cpu/ppc/ppc-jit.cpp")
	if(WIN32 AND EXISTS "${SS_SRC}/Windows/cygwin_precompiled_dyngen/basic-dyngen-ops.hpp")
		set(SS_DYNGEN_OPS_DIR "${SS_SRC}/Windows/cygwin_precompiled_dyngen")
	else()
		set(SS_DYNGEN_OPS_DIR "${SS_SRC}/Unix/dyngen_precompiled")
	endif()
	list(APPEND SS_DYNGEN_INC "${SS_DYNGEN_OPS_DIR}")
	set_source_files_properties(
		"${SS_KPX}/src/cpu/jit/basic-dyngen.cpp"
		"${SS_KPX}/src/cpu/ppc/ppc-dyngen.cpp"
		PROPERTIES OBJECT_DEPENDS
			"${SS_DYNGEN_OPS_DIR}/basic-dyngen-ops.hpp;${SS_DYNGEN_OPS_DIR}/ppc-dyngen-ops.hpp")
endif()

if(WIN32)
	macemu_windows_net_sources("${B2_SRC}" SS_NET)

	# Windows sources SheepShaver owns outright.
	set(SS_PLATFORM
		"${SS_SRC}/Windows/main_windows.cpp"
		"${SS_SRC}/Windows/prefs_windows.cpp"
		"${SS_SRC}/Windows/user_strings_windows.cpp"
		"${SS_SRC}/Windows/about_window_windows.cpp"
		"${SS_SRC}/dummy/prefs_dummy.cpp"
	)

	# Windows sources shared with Basilisk II, compiled from the BasiliskII
	# tree. Each one includes "sysdeps.h" and MSVC looks in the file's own
	# directory first, which there holds Basilisk II's copy, so force-include
	# SheepShaver's ahead of it. Both use the SYSDEPS_H guard, so the include
	# in the file itself then does nothing.
	set(SS_SHARED_PLATFORM
		"${B2_SRC}/Windows/sys_windows.cpp"
		"${B2_SRC}/Windows/xpram_windows.cpp"
		"${B2_SRC}/Windows/timer_windows.cpp"
		"${B2_SRC}/Windows/serial_windows.cpp"
		"${B2_SRC}/Windows/ether_windows.cpp"
		"${B2_SRC}/Windows/clip_windows.cpp"
		"${B2_SRC}/Windows/extfs_windows.cpp"
		"${B2_SRC}/Windows/util_windows.cpp"
		"${B2_SRC}/Windows/posix_emu.cpp"
		"${B2_SRC}/dummy/scsi_dummy.cpp"
		"${B2_SRC}/dummy/prefs_editor_dummy.cpp"
	)
	set(SS_SYSDEPS_FORCE_INCLUDE "${SS_SRC}/Windows/sysdeps.h")
	if(MSVC)
		set_source_files_properties(${SS_SHARED_PLATFORM} PROPERTIES
			COMPILE_OPTIONS "/FI${SS_SYSDEPS_FORCE_INCLUDE}")
	else()
		set_source_files_properties(${SS_SHARED_PLATFORM} PROPERTIES
			COMPILE_OPTIONS "-include;${SS_SYSDEPS_FORCE_INCLUDE}")
	endif()
	set_source_files_properties(${SS_SHARED_PLATFORM} PROPERTIES
		OBJECT_DEPENDS "${SS_SYSDEPS_FORCE_INCLUDE}")
	list(APPEND SS_PLATFORM ${SS_SHARED_PLATFORM})
	list(APPEND SS_PLATFORM ${SS_NET})

	if(EXISTS "${SS_SRC}/Windows/SheepShaver.rc")
		file(SIZE "${SS_SRC}/Windows/SheepShaver.rc" SS_RC_SIZE)
		if(SS_RC_SIZE GREATER 100)
			list(APPEND SS_PLATFORM "${SS_SRC}/Windows/SheepShaver.rc")
		endif()
	endif()

	set(SS_PLATFORM_INC
	"${SS_SRC}/Windows"
	"${B2_SRC}/Windows"
	"${B2_SRC}/Windows/router"
	"${B2_SRC}/Windows/b2ether")
elseif(APPLE)
	macemu_apple_common_sources("${SS_SRC}" "${B2_SRC}" SS_PLATFORM)
	macemu_apple_macos_sources("${SS_SRC}" "${B2_SRC}" SS_APPLE_MACOS_EXTRA)
	macemu_apple_pocketshaver_sources("${SS_SRC}" SS_APPLE_IOS_EXTRA SS_APPLE_IOS_RES)
	macemu_apple_include_dirs("${SS_SRC}" "${B2_SRC}" SS_PLATFORM_INC)
	list(APPEND SS_PLATFORM_INC "${SS_SYSDEPS}")
else()
	# Unix sources SheepShaver owns outright, then the ones shared with
	# Basilisk II named by their real path there. clip_dummy.cpp used to be
	# added inside the loop below, so it went in once per shared source.
	set(SS_PLATFORM
	"${SS_SRC}/Unix/main_unix.cpp"
	"${SS_SRC}/Unix/about_window_unix.cpp"
	"${SS_SRC}/Unix/prefs_unix.cpp"
	"${SS_SRC}/Unix/user_strings_unix.cpp"
	"${B2_SRC}/Unix/sys_unix.cpp"
	"${B2_SRC}/Unix/xpram_unix.cpp"
	"${B2_SRC}/Unix/timer_unix.cpp"
	"${B2_SRC}/Unix/serial_unix.cpp"
	"${B2_SRC}/Unix/ether_unix.cpp"
	"${B2_SRC}/Unix/extfs_unix.cpp"
	"${B2_SRC}/Unix/rpc_unix.cpp"
	"${B2_SRC}/Unix/sshpty.c"
	"${B2_SRC}/Unix/strlcpy.c"
	"${B2_SRC}/Unix/tinyxml2.cpp"
	"${B2_SRC}/Unix/disk_sparsebundle.cpp"
	"${B2_SRC}/Unix/posix_sem.cpp"
	"${B2_SRC}/dummy/clip_dummy.cpp"
	"${B2_SRC}/dummy/prefs_dummy.cpp"
	)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		list(APPEND SS_PLATFORM "${B2_SRC}/Unix/Linux/scsi_linux.cpp")
	else()
		list(APPEND SS_PLATFORM "${B2_SRC}/dummy/scsi_dummy.cpp")
	endif()
	if(ENABLE_GTK)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
	list(APPEND SS_PLATFORM "${B2_SRC}/Unix/prefs_editor_gtk3.cpp")
	else()
	list(APPEND SS_PLATFORM "${B2_SRC}/dummy/prefs_editor_dummy.cpp")
	endif()
	set(SS_PLATFORM_INC "${SS_SRC}/Unix")
endif()

# ---------------------------------------------------------------------------
# Graphics acceleration (PocketShaver engines: NQD / RAVE / GL / DSp)
# ---------------------------------------------------------------------------
if(NOT DEFINED ENABLE_GFXACCEL)
	option(ENABLE_GFXACCEL "SheepShaver graphics accel (NQD/RAVE/GL/DSp)" ON)
endif()
if(NOT DEFINED GFXACCEL_BACKEND)
	set(GFXACCEL_BACKEND "auto" CACHE STRING "gfxaccel GPU backend (auto|opengl|metal|sdlgpu)")
endif()
set_property(CACHE GFXACCEL_BACKEND PROPERTY STRINGS auto opengl metal sdlgpu)

set(SS_GFXACCEL)
set(SS_GFXACCEL_INC)
set(SS_GFXACCEL_LIBS)
set(SS_GFXACCEL_DEFS)

if(ENABLE_GFXACCEL)
	set(SS_GFXACCEL_DIR "${SS_SRC}/gfxaccel")
	set(SS_GFXACCEL_INC "${SS_GFXACCEL_DIR}" "${SS_GFXACCEL_DIR}/include" "${SS_GFXACCEL_DIR}/gl")

	# Portable engine / policy / dispatch sources
	list(APPEND SS_GFXACCEL
	"${SS_GFXACCEL_DIR}/display_mode_controller.cpp"
	"${SS_GFXACCEL_DIR}/dsp_alt_buffer.cpp"
	"${SS_GFXACCEL_DIR}/dsp_clut_gamma.cpp"
	"${SS_GFXACCEL_DIR}/dsp_dispatch.cpp"
	"${SS_GFXACCEL_DIR}/dsp_draw_context.cpp"
	"${SS_GFXACCEL_DIR}/dsp_engine.cpp"
	"${SS_GFXACCEL_DIR}/dsp_flatten_restore.cpp"
	"${SS_GFXACCEL_DIR}/dsp_install_hooks.cpp"
	"${SS_GFXACCEL_DIR}/dsp_mode_enumerate.cpp"
	"${SS_GFXACCEL_DIR}/dsp_thunks.cpp"
	"${SS_GFXACCEL_DIR}/gfxaccel_resources.cpp"
	"${SS_GFXACCEL_DIR}/gfxaccel_resources_heap.cpp"
	"${SS_GFXACCEL_DIR}/gl_defer.cpp"
	"${SS_GFXACCEL_DIR}/gl_dispatch.cpp"
	"${SS_GFXACCEL_DIR}/gl_engine.cpp"
	"${SS_GFXACCEL_DIR}/gl_state.cpp"
	"${SS_GFXACCEL_DIR}/gl_thunks.cpp"
	"${SS_GFXACCEL_DIR}/rave_dispatch.cpp"
	"${SS_GFXACCEL_DIR}/rave_draw_context.cpp"
	"${SS_GFXACCEL_DIR}/rave_engine.cpp"
	"${SS_GFXACCEL_DIR}/rave_thunks.cpp"
	"${SS_GFXACCEL_DIR}/glide_dispatch.cpp"
	"${SS_GFXACCEL_DIR}/glide_engine.cpp"
	"${SS_GFXACCEL_DIR}/glide_state.cpp"
	"${SS_GFXACCEL_DIR}/glide_pef_register.cpp"
	)

	if(ENABLE_NATIVE_CINEPAK_PATCH)
		list(APPEND SS_GFXACCEL
		"${SS_GFXACCEL_DIR}/cinepak_decoder.cpp"
		"${SS_GFXACCEL_DIR}/cinepak_hooks.cpp"
		)
	endif()
	# Resolve backend. Explicit metal matches PocketShaver.xcodeproj
	# (SDL2 + video_sdl2.cpp) and must not be overwritten by USE_SDLGPU.
	# SDL-GPU remains SDL3 + video_sdl3.cpp.
	if(GFXACCEL_BACKEND STREQUAL "metal")
		set(USE_SDLGPU OFF)
		if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
			set(USE_SDL3 OFF)
		endif()
	elseif(USE_SDLGPU AND (GFXACCEL_BACKEND STREQUAL "auto" OR GFXACCEL_BACKEND STREQUAL "sdlgpu"))
		set(GFXACCEL_BACKEND "sdlgpu")
	endif()
	if(GFXACCEL_BACKEND STREQUAL "auto")
	if(APPLE)
		set(GFXACCEL_BACKEND "metal")
	else()
		set(GFXACCEL_BACKEND "opengl")
	endif()
	endif()

	if(GFXACCEL_BACKEND STREQUAL "metal")
	if(NOT APPLE)
		message(FATAL_ERROR "GFXACCEL_BACKEND=metal requires Apple platforms")
	endif()
	list(APPEND SS_GFXACCEL_DEFS ENABLE_GFXACCEL=1 GFXACCEL_USE_METAL=1)
	enable_language(OBJCXX)
	list(APPEND SS_GFXACCEL
		"${SS_GFXACCEL_DIR}/metal_device_shared.mm"
		"${SS_GFXACCEL_DIR}/metal_compositor.mm"
		"${SS_GFXACCEL_DIR}/metal_compositor_submitframe.mm"
		"${SS_GFXACCEL_DIR}/nqd_metal_renderer.mm"
		"${SS_GFXACCEL_DIR}/rave_metal_renderer.mm"
		"${SS_GFXACCEL_DIR}/gl_metal_renderer.mm"
		"${SS_GFXACCEL_DIR}/dsp_metal_renderer.mm"
		"${SS_GFXACCEL_DIR}/dsp_host_bridge.mm"
		"${SS_GFXACCEL_DIR}/gfxaccel_resources.mm"
		"${SS_GFXACCEL_DIR}/gfxaccel_resources_heap.mm"
		"${SS_GFXACCEL_DIR}/gfxaccel_arc_shim.mm"
		"${SS_GFXACCEL_DIR}/vbl_source.mm"
		"${SS_GFXACCEL_DIR}/pso_archive.mm"
		"${SS_GFXACCEL_DIR}/compositor_shaders.metal"
		"${SS_GFXACCEL_DIR}/nqd_shaders.metal"
		"${SS_GFXACCEL_DIR}/gl_shaders.metal"
		"${SS_GFXACCEL_DIR}/rave_shaders.metal"
	)
	set_source_files_properties(
		"${SS_GFXACCEL_DIR}/compositor_shaders.metal"
		"${SS_GFXACCEL_DIR}/nqd_shaders.metal"
		"${SS_GFXACCEL_DIR}/gl_shaders.metal"
		"${SS_GFXACCEL_DIR}/rave_shaders.metal"
		PROPERTIES
			LANGUAGE METAL
			XCODE_EXPLICIT_FILE_TYPE sourcecode.metal)
	# -framework, not find_library: find_library embeds the configure-time
	# iPhoneOS SDK path, which then shadows MacOSX.sdk when building
	# Catalyst (Foundation/NSURLHandle.h).
	list(APPEND SS_GFXACCEL_LIBS
		"-framework Metal" "-framework QuartzCore" "-framework Foundation")
	elseif(GFXACCEL_BACKEND STREQUAL "sdlgpu")
	if(NOT USE_SDL3)
		message(FATAL_ERROR "SDL-GPU requires SDL3")
	endif()
	set(SDLGPU_SHADERCROSS_TARGET)
	if(USE_SDLGPU_SHADERCROSS)
		find_package(SDL3_shadercross CONFIG REQUIRED)
		if(TARGET SDL3_shadercross::SDL3_shadercross)
			set(SDLGPU_SHADERCROSS_TARGET SDL3_shadercross::SDL3_shadercross)
		elseif(TARGET SDL3_shadercross::SDL3_shadercross-static)
			set(SDLGPU_SHADERCROSS_TARGET SDL3_shadercross::SDL3_shadercross-static)
		else()
			message(FATAL_ERROR "SDL3_shadercross package has no supported CMake target")
		endif()
	endif()
	list(PREPEND SS_GFXACCEL_INC "${SS_GFXACCEL_DIR}/sdlgpu")
	list(REMOVE_ITEM SS_GFXACCEL
		"${SS_GFXACCEL_DIR}/dsp_dispatch.cpp"
		"${SS_GFXACCEL_DIR}/dsp_draw_context.cpp"
		"${SS_GFXACCEL_DIR}/gl_dispatch.cpp"
		"${SS_GFXACCEL_DIR}/gl_engine.cpp"
		"${SS_GFXACCEL_DIR}/glide_dispatch.cpp"
		"${SS_GFXACCEL_DIR}/rave_draw_context.cpp"
		"${SS_GFXACCEL_DIR}/rave_dispatch.cpp"
		"${SS_GFXACCEL_DIR}/glide_state.cpp")
	list(REMOVE_ITEM SS_CORE "${B2_SRC}/SDL/video_sdl3.cpp")
	list(APPEND SS_CORE "${SS_GFXACCEL_DIR}/sdlgpu/video_sdlgpu.cpp")
	list(APPEND SS_GFXACCEL_DEFS
		ENABLE_GFXACCEL=1
		GFXACCEL_USE_OPENGL=1
		GFXACCEL_USE_SDLGPU=1)
	if(USE_SDLGPU_SHADERCROSS)
		list(APPEND SS_GFXACCEL_DEFS GFXACCEL_USE_SHADERCROSS=1)
	endif()
	list(APPEND SS_GFXACCEL
		"${SS_GFXACCEL_DIR}/sdlgpu/sdlgpu_gl_compat.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/dsp_dispatch_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/dsp_draw_context_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gl_dispatch_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gl_engine_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/glide_dispatch_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/rave_draw_context_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/rave_dispatch_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/glide_state_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gl_compositor_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/vbl_source_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gfxaccel_resources_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gfxaccel_resources_heap_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gfxaccel_arc_shim.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/pso_archive_sdlgpu.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/nqd_sdlgpu_renderer.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/rave_sdlgpu_renderer.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/gl_ffp_sdlgpu_renderer.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/dsp_sdlgpu_renderer.cpp"
		"${SS_GFXACCEL_DIR}/sdlgpu/glide_sdlgpu_renderer.cpp"
	)
	if(SDLGPU_SHADERCROSS_TARGET)
		list(APPEND SS_GFXACCEL_LIBS ${SDLGPU_SHADERCROSS_TARGET})
	endif()
	elseif(GFXACCEL_BACKEND STREQUAL "opengl")
	# OpenGL + SDL backend (default on Windows/Linux)
	list(APPEND SS_GFXACCEL_DEFS ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1)
	list(APPEND SS_GFXACCEL
		"${SS_GFXACCEL_DIR}/gl/gl_compositor.cpp"
		"${SS_GFXACCEL_DIR}/gl/vbl_source_sdl.cpp"
		"${SS_GFXACCEL_DIR}/gl/gfxaccel_resources_gl.cpp"
		"${SS_GFXACCEL_DIR}/gl/gfxaccel_resources_heap_gl.cpp"
		"${SS_GFXACCEL_DIR}/gl/gfxaccel_arc_shim.cpp"
		"${SS_GFXACCEL_DIR}/gl/pso_archive_gl.cpp"
		"${SS_GFXACCEL_DIR}/gl/nqd_gl_renderer.cpp"
		"${SS_GFXACCEL_DIR}/gl/rave_gl_renderer.cpp"
		"${SS_GFXACCEL_DIR}/gl/gl_ffp_renderer.cpp"
		"${SS_GFXACCEL_DIR}/gl/dsp_gl_renderer.cpp"
		"${SS_GFXACCEL_DIR}/gl/glide_gl_renderer.cpp"
	)
	if(WIN32)
		list(APPEND SS_GFXACCEL_LIBS opengl32)
	elseif(APPLE)
		find_library(OPENGL_LIB OpenGL)
		list(APPEND SS_GFXACCEL_LIBS ${OPENGL_LIB})
	else()
		find_package(OpenGL REQUIRED)
		list(APPEND SS_GFXACCEL_LIBS OpenGL::GL)
	endif()
	else()
	message(FATAL_ERROR "Unknown GFXACCEL_BACKEND=${GFXACCEL_BACKEND}")
	endif()

	message(STATUS "SheepShaver gfxaccel: backend=${GFXACCEL_BACKEND}")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
# ppc-execute-impl.cpp is #included by ppc-execute.cpp (not a separate TU)
function(sheepshaver_configure_target SHEEPSHAVER_EXECUTABLE)
	target_include_directories(${SHEEPSHAVER_EXECUTABLE} PRIVATE
		# Platform sysdeps (Windows/ Unix/ MacOSX/config) must come first
		${SS_PLATFORM_INC}
		"${SS_SRC}/include"
		${SS_DYNGEN_INC}
		"${B2_SRC}/include"
		"${B2_SRC}/CrossPlatform"
		"${B2_SRC}/slirp"
		"${SS_KPX}/include"
		"${SS_KPX}/src"
		${SS_GFXACCEL_INC}
	)

	target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE
		SHEEPSHAVER=1
		POWERPC_ROM=1
		EMULATED_PPC=1
		${SS_GFXACCEL_DEFS}
	)
	if(NOT WIN32 AND NOT APPLE)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE
			DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/SheepShaver")
	endif()
	if(WIN32)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE CONFIG_WIN32=1 NATMEM_OFFSET=0x11000000)
		option(ENABLE_MEM_BULK_LAZY "Lazily back entire guest address space on fault (MEM_BULK parity)" ON)
		if(ENABLE_MEM_BULK_LAZY)
			target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE MEM_BULK_LAZY=1)
		endif()
	endif()
	if(MSVC AND ENABLE_GFXACCEL)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE _USE_MATH_DEFINES=1)
	endif()
	if(ENABLE_SHEEPSHAVER_JIT)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE USE_JIT ENABLE_DYNGEN)
	endif()
	if(ENABLE_QD3D_INIT_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE QD3D_INIT_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_QD3D_GRAPHICS_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE QD3D_GRAPHICS_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_QD3D_AUDIO_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE QD3D_AUDIO_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_QD3D_MEDIA_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE QD3D_MEDIA_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_QD3D_WAIT_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE QD3D_WAIT_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_SDLGPU_TRANSITION_LOGGING)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE
			SDLGPU_TRANSITION_LOGGING_ENABLED=1)
	endif()
	if(ENABLE_NATIVE_CINEPAK_PATCH)
		target_compile_definitions(${SHEEPSHAVER_EXECUTABLE} PRIVATE
			ENABLE_NATIVE_CINEPAK_PATCH=1)
	endif()

	macemu_apply_common(${SHEEPSHAVER_EXECUTABLE})

	if(SS_GFXACCEL_LIBS)
		target_link_libraries(${SHEEPSHAVER_EXECUTABLE} PRIVATE ${SS_GFXACCEL_LIBS})
	endif()

	if(ENABLE_GTK AND NOT WIN32 AND NOT APPLE)
		target_include_directories(${SHEEPSHAVER_EXECUTABLE} PRIVATE ${GTK3_INCLUDE_DIRS})
		target_link_libraries(${SHEEPSHAVER_EXECUTABLE} PRIVATE ${GTK3_LIBRARIES})
	endif()
endfunction()

if(APPLE)
	if(MACEMU_BUILD_DESKTOP_SHEEPSHAVER)
		add_executable(SheepShaver MACOSX_BUNDLE
			${SS_CORE} ${SS_CPU} ${SS_PLATFORM} ${SS_GFXACCEL}
			${SS_APPLE_MACOS_EXTRA})
		sheepshaver_configure_target(SheepShaver)
		target_include_directories(SheepShaver BEFORE PRIVATE
			"${SS_SRC}/MacOSX/config")
		macemu_apple_apply_app(SheepShaver macos)
		install(TARGETS SheepShaver BUNDLE DESTINATION .)
	endif()
	if(MACEMU_BUILD_POCKETSHAVER)
		add_executable(PocketShaver MACOSX_BUNDLE
			${SS_CORE} ${SS_CPU} ${SS_PLATFORM} ${SS_GFXACCEL}
			${SS_APPLE_IOS_EXTRA} ${SS_APPLE_IOS_RES})
		foreach(SS_APPLE_IOS_RESOURCE IN LISTS SS_APPLE_IOS_RES)
			set_source_files_properties("${SS_APPLE_IOS_RESOURCE}" PROPERTIES
				MACOSX_PACKAGE_LOCATION Resources)
		endforeach()
		sheepshaver_configure_target(PocketShaver)
		set(POCKETSHAVER_APP_KIND ios)
		if(MACEMU_APPLE_TARGET STREQUAL "catalyst")
			set(POCKETSHAVER_APP_KIND catalyst)
		endif()
		macemu_apple_apply_app(PocketShaver ${POCKETSHAVER_APP_KIND})
		target_include_directories(PocketShaver BEFORE PRIVATE
			"${SS_SRC}/MacOSX/config")
		target_include_directories(PocketShaver PRIVATE
			"${SS_SRC}/MacOSX/PocketShaver"
			"${SS_SRC}/MacOSX/impluse-hfs"
			"${SS_SRC}/MacOSX/Bonjour"
			"${B2_SRC}/SDL"
			${MACEMU_APPLE_POCKETSHAVER_HEADER_DIRS})
		install(TARGETS PocketShaver BUNDLE DESTINATION .)
	endif()
else()
	add_executable(SheepShaver
		${SS_CORE} ${SS_CPU} ${SS_PLATFORM} ${SS_GFXACCEL})
	if(NOT WIN32)
		find_package(X11 REQUIRED)
		target_link_libraries(SheepShaver PRIVATE X11::X11 X11::Xext)
	endif()
	sheepshaver_configure_target(SheepShaver)
	install(TARGETS SheepShaver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

message(STATUS "SheepShaver: JIT=${ENABLE_SHEEPSHAVER_JIT}")
message(STATUS "SheepShaver: QD3D init logging=${ENABLE_QD3D_INIT_LOGGING}")
message(STATUS "SheepShaver: QD3D graphics logging=${ENABLE_QD3D_GRAPHICS_LOGGING}")
message(STATUS "SheepShaver: QD3D audio logging=${ENABLE_QD3D_AUDIO_LOGGING}")
message(STATUS "SheepShaver: QD3D media logging=${ENABLE_QD3D_MEDIA_LOGGING}")
message(STATUS "SheepShaver: QD3D wait logging=${ENABLE_QD3D_WAIT_LOGGING}")
message(STATUS "SheepShaver: SDL-GPU transition logging=${ENABLE_SDLGPU_TRANSITION_LOGGING}")
