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
# Apple (CMake-only; does not use any .xcodeproj):
#	 cmake -S . -B build && cmake --build build --target SheepShaver
#	 cmake -S . -B build-ios -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#		 -DCMAKE_OSX_SYSROOT=iphonesimulator
#	 cmake --build build-ios --target PocketShaver
#	 cmake -S . -B build-catalyst -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.toolchain.cmake
#	 cmake --build build-catalyst --config Debug --target PocketShaver
#	 Metal compositor (PocketShaver.xcodeproj: SDL2 + video_sdl2.cpp):
#	   -DGFXACCEL_BACKEND=metal
#	 SDL-GPU compositor (PocketShaver SDLGPU.xcodeproj: SDL3 + video_sdl3.cpp):
#	   -DUSE_SDLGPU=ON   (default)
#
cmake_minimum_required(VERSION 3.16)
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
	if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
		set(CMAKE_OSX_DEPLOYMENT_TARGET "12.1" CACHE STRING
			"macOS deployment target")
	endif()
endif()
if(CMAKE_HOST_APPLE)
	project(macemu VERSION 1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
	project(macemu VERSION 1.0 LANGUAGES C CXX)
endif()

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
if(CMAKE_HOST_APPLE)
	set(BASILISKII_OPTION_DEFAULT OFF)
else()
	set(BASILISKII_OPTION_DEFAULT ON)
endif()
option(BUILD_BASILISKII	"Build Basilisk II (68k Mac emulator)" ${BASILISKII_OPTION_DEFAULT})
unset(BASILISKII_OPTION_DEFAULT)
option(BUILD_SHEEPSHAVER "Build SheepShaver (PowerPC Mac emulator)" ON)
option(ENABLE_JIT				"Enable the Basilisk II UAE JIT" OFF)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(SHEEPSHAVER_JIT_OPTION_DEFAULT OFF)
else()
	set(SHEEPSHAVER_JIT_OPTION_DEFAULT "${ENABLE_JIT}")
endif()
option(ENABLE_SHEEPSHAVER_JIT
	"Enable the SheepShaver dyngen JIT (unsafe with 64-bit MSVC)"
	${SHEEPSHAVER_JIT_OPTION_DEFAULT})
unset(SHEEPSHAVER_JIT_OPTION_DEFAULT)
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
#   metal  = PocketShaver.xcodeproj (SDL2 + video_sdl2.cpp + Metal compositor)
#   sdlgpu = "PocketShaver SDLGPU.xcodeproj" (SDL3 + video_sdl3.cpp)
# auto = Metal on Apple when USE_SDLGPU is OFF, OpenGL elsewhere.
# USE_SDLGPU=ON selects sdlgpu unless GFXACCEL_BACKEND is set to metal.
set(GFXACCEL_BACKEND "auto" CACHE STRING "gfxaccel GPU backend (auto|opengl|metal|sdlgpu)")
set_property(CACHE GFXACCEL_BACKEND PROPERTY STRINGS auto opengl metal sdlgpu)
if(GFXACCEL_BACKEND STREQUAL "metal")
	set(USE_SDLGPU OFF)
	# PocketShaver.xcodeproj links SDL2.xcframework; video_sdl2.cpp is
	# compiled only when SDL < 3. iOS/Catalyst Metal therefore uses SDL2.
	# Desktop SheepShaver keeps the configured SDL (typically SDL3).
	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		set(USE_SDL3 OFF CACHE BOOL "Use SDL 3 instead of SDL 2" FORCE)
	endif()
elseif(USE_SDLGPU AND (GFXACCEL_BACKEND STREQUAL "auto" OR GFXACCEL_BACKEND STREQUAL "sdlgpu"))
	set(GFXACCEL_BACKEND "sdlgpu")
endif()
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

if(APPLE)
	macemu_apple_init()
	macemu_apple_enable_languages()
endif()

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
if(APPLE)
	message(STATUS "	Apple target	 : ${MACEMU_APPLE_TARGET}")
	message(STATUS "	SheepShaver.app: ${MACEMU_BUILD_DESKTOP_SHEEPSHAVER}")
	message(STATUS "	PocketShaver.app: ${MACEMU_BUILD_POCKETSHAVER}")
endif()
message(STATUS "")
