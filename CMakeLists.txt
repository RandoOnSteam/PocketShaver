# macemu - master CMake build for Basilisk II and/or SheepShaver
#
#	 cmake -S . -B build -DSDL2_DIR=.../SDL2/cmake
#	 cmake --build build --target BasiliskII
#	 cmake --build build --target SheepShaver
#
# Or build only one:
#	 cmake -S . -B build -DBUILD_SHEEPSHAVER=OFF
#	 cmake -S . -B build -DBUILD_BASILISKII=OFF
#
cmake_minimum_required(VERSION 3.16)
project(macemu VERSION 1.0 LANGUAGES C CXX)

# The C++ in this tree is written to C++98: no <atomic>/<thread>/<chrono>/
# <unordered_map>, no auto/lambdas/nullptr/constexpr/range-for/move.  See
# SheepShaver/src/include/atomic.h for the per-platform stand-in for C11
# <stdatomic.h>.
#
# The C stays at C99. Every .c file in the build is vendored or generated -
# slirp (BSD), ppc-dis.c (binutils), sshpty/strlcpy (OpenSSH) - and each uses
# // comments, `inline` or <stdbool.h>, which -std=c90 rejects outright.
set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_BASILISKII	"Build Basilisk II (68k Mac emulator)" ON)
option(BUILD_SHEEPSHAVER "Build SheepShaver (PowerPC Mac emulator)" ON)
option(ENABLE_JIT				"Enable the Basilisk II UAE JIT" OFF)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_macemu_sheepshaver_jit_default OFF)
else()
	set(_macemu_sheepshaver_jit_default "${ENABLE_JIT}")
endif()
option(ENABLE_SHEEPSHAVER_JIT
	"Enable the SheepShaver dyngen JIT (unsafe with 64-bit MSVC)"
	${_macemu_sheepshaver_jit_default})
unset(_macemu_sheepshaver_jit_default)
option(ENABLE_GTK				"GTK preferences UI on Unix" OFF)
option(ENABLE_BINCUE		 "BIN/CUE CD image support" ON)
option(ENABLE_VOSF			 "Video on SEGV signals" OFF)
option(USE_SDL3					"Use SDL 3 instead of SDL 2" ON)
option(USE_SDLGPU				"Use SDL-GPU for SheepShaver gfxaccel" ON)
option(USE_SDLGPU_SHADERCROSS "Use SDL_shadercross as an SDL-GPU shader fallback" OFF)
option(ENABLE_SDLGPU_TRANSITION_LOGGING
	"Compile SDL-GPU transition diagnostics"
	OFF)
option(ENABLE_GFXACCEL	 "SheepShaver graphics accel (NQD/RAVE/GL/DSp)" ON)
option(ENABLE_QD3D_INIT_LOGGING
	"Compile focused SheepShaver QuickDraw 3D/RAVE initialization diagnostics"
	OFF)
option(ENABLE_QD3D_GRAPHICS_LOGGING
	"Compile high-volume QD3D graphics diagnostics"
	OFF)
option(ENABLE_QD3D_AUDIO_LOGGING
	"Compile sound component and SDL audio diagnostics"
	OFF)
option(ENABLE_QD3D_MEDIA_LOGGING
	"Compile CD/media I/O diagnostics"
	OFF)
option(ENABLE_QD3D_WAIT_LOGGING
	"Compile high-volume guest timing and wait diagnostics"
	OFF)
option(ENABLE_NATIVE_CINEPAK_PATCH
	"Patch QuickTime to use native Cinepak decoder"
	OFF)
# GFXACCEL_BACKEND: auto | opengl | metal | sdlgpu
# auto = Metal on Apple, OpenGL elsewhere
set(GFXACCEL_BACKEND "auto" CACHE STRING "gfxaccel GPU backend (auto|opengl|metal|sdlgpu)")
set_property(CACHE GFXACCEL_BACKEND PROPERTY STRINGS auto opengl metal sdlgpu)
if(USE_SDLGPU)
	set(USE_SDL3 ON CACHE BOOL "Use SDL 3 instead of SDL 2" FORCE)
endif()
if(WIN32)
	if(NOT USE_SDL3 AND NOT SDL2_DIR)
		set(SDL2_DIR "thirdparty/SDL2/cmake" CACHE PATH
			"Directory containing SDL2Config.cmake")
	elseif(USE_SDL3 AND NOT SDL3_DIR)
		set(SDL3_DIR "thirdparty/SDL3/cmake" CACHE PATH
			"Directory containing SDL3 cmake files")
		message(STATUS "SDL3Dir set")
	endif()
endif()
if(NOT BUILD_BASILISKII AND NOT BUILD_SHEEPSHAVER)
	message(FATAL_ERROR "Enable at least one of BUILD_BASILISKII or BUILD_SHEEPSHAVER")
endif()

set(MACEMU_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${MACEMU_CMAKE_DIR}")
include(MacEmuCommon)

macemu_detect_host()
macemu_find_sdl()

set(MACEMU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(B2_SRC "${MACEMU_ROOT}/BasiliskII/src")
set(SS_SRC "${MACEMU_ROOT}/SheepShaver/src")

if(BUILD_BASILISKII)
	add_subdirectory(BasiliskII)
endif()

if(BUILD_SHEEPSHAVER)
	add_subdirectory(SheepShaver)
endif()

message(STATUS "")
message(STATUS "macemu configuration")
message(STATUS "	Basilisk II	: ${BUILD_BASILISKII}")
message(STATUS "	SheepShaver	: ${BUILD_SHEEPSHAVER}")
message(STATUS "	Basilisk JIT : ${ENABLE_JIT}")
message(STATUS "	Sheep JIT		: ${ENABLE_SHEEPSHAVER_JIT}")
message(STATUS "	SDL3				 : ${USE_SDL3}")
message(STATUS "	SDL-GPU			 : ${USE_SDLGPU}")
message(STATUS "	SDL-GPU shadercross: ${USE_SDLGPU_SHADERCROSS}")
message(STATUS "	SDL-GPU transition log: ${ENABLE_SDLGPU_TRANSITION_LOGGING}")
message(STATUS "	gfxaccel		 : ${ENABLE_GFXACCEL} (${GFXACCEL_BACKEND})")
message(STATUS "	QD3D init log: ${ENABLE_QD3D_INIT_LOGGING}")
message(STATUS "	QD3D graphics log: ${ENABLE_QD3D_GRAPHICS_LOGGING}")
message(STATUS "	QD3D audio log: ${ENABLE_QD3D_AUDIO_LOGGING}")
message(STATUS "	QD3D media log: ${ENABLE_QD3D_MEDIA_LOGGING}")
message(STATUS "	QD3D wait log: ${ENABLE_QD3D_WAIT_LOGGING}")
message(STATUS "	Platform		 : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "")
