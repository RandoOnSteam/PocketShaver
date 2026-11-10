cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	project(SheepShaverSFL C CXX)
else()
	unset(CMAKE_C_STANDARD)
endif()
option(SFLMONITOR "Build the emulator monitor server" OFF)

set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(CMAKE_HOST_WIN32 AND CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	add_compile_definitions(__STRINGS__)
	add_compile_options("SHELL:-include bsdstrings.h")
endif()

get_filename_component(SHEEPSHAVERROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SHEEPSHAVERSOURCE "${SHEEPSHAVERROOT}/src")
set(BASILISKIISOURCE "${SHEEPSHAVERROOT}/../BasiliskII/src")
set(KPXCPUSOURCE "${SHEEPSHAVERSOURCE}/kpx_cpu")
include("${BASILISKIISOURCE}/SFL/SFL.cmake")

set(SFLCORESOURCES
	"${BASILISKIISOURCE}/adb.cpp"
	"${BASILISKIISOURCE}/audio.cpp"
	"${BASILISKIISOURCE}/cdrom.cpp"
	"${BASILISKIISOURCE}/bincue.cpp"
	"${BASILISKIISOURCE}/disk.cpp"
	"${BASILISKIISOURCE}/qcow.cpp"
	"${BASILISKIISOURCE}/chd.cpp"
	"${BASILISKIISOURCE}/imageio.cpp"
	"${BASILISKIISOURCE}/prefs.cpp"
	"${BASILISKIISOURCE}/scsi.cpp"
	"${BASILISKIISOURCE}/sony.cpp"
	"${BASILISKIISOURCE}/timer.cpp"
	"${BASILISKIISOURCE}/xpram.cpp"
	"${BASILISKIISOURCE}/joymanager.cpp"
	"${BASILISKIISOURCE}/extfs.cpp"
	"${SHEEPSHAVERSOURCE}/main.cpp"
	"${SHEEPSHAVERSOURCE}/prefs_items.cpp"
	"${SHEEPSHAVERSOURCE}/rom_patches.cpp"
	"${SHEEPSHAVERSOURCE}/rsrc_patches.cpp"
	"${SHEEPSHAVERSOURCE}/emul_op.cpp"
	"${SHEEPSHAVERSOURCE}/soundinput.cpp"
	"${SHEEPSHAVERSOURCE}/shears.cpp"
	"${SHEEPSHAVERROOT}/../guest/sheepbench/sbkernels.c"
	"${SHEEPSHAVERSOURCE}/name_registry.cpp"
	"${SHEEPSHAVERSOURCE}/usbhid.cpp"
	"${SHEEPSHAVERSOURCE}/usbuim.cpp"
	"${SHEEPSHAVERSOURCE}/macio_escc.cpp"
	"${SHEEPSHAVERSOURCE}/mmio.cpp"
	"${SHEEPSHAVERSOURCE}/pcibridge.cpp"
	"${SHEEPSHAVERSOURCE}/macos_util.cpp"
	"${SHEEPSHAVERSOURCE}/video.cpp"
	"${SHEEPSHAVERSOURCE}/ether.cpp"
	"${SHEEPSHAVERSOURCE}/thunks.cpp"
	"${SHEEPSHAVERSOURCE}/serial.cpp"
	"${SHEEPSHAVERSOURCE}/user_strings.cpp"
	"${SHEEPSHAVERSOURCE}/gfxaccel.cpp"
	"${BASILISKIISOURCE}/printing.cpp"
	"${BASILISKIISOURCE}/pictpdf.c"
	"${BASILISKIISOURCE}/pdfgen.c"
	"${KPXCPUSOURCE}/src/mathlib/ieeefp.cpp"
	"${KPXCPUSOURCE}/src/mathlib/mathlib.cpp"
	"${KPXCPUSOURCE}/src/cpu/ppc/ppc-cpu.cpp"
	"${KPXCPUSOURCE}/src/cpu/ppc/ppc-decode.cpp"
	"${KPXCPUSOURCE}/src/cpu/ppc/ppc-execute.cpp"
	"${KPXCPUSOURCE}/src/cpu/ppc/ppc-translate.cpp"
	"${KPXCPUSOURCE}/src/utils/utils-cpuinfo.cpp"
	"${KPXCPUSOURCE}/sheepshaver_glue.cpp"
	"${KPXCPUSOURCE}/ppc-dis.c"
)

set(SFLINCLUDEDIRECTORIES
	"${SFLDIRECTORY}"
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${SHEEPSHAVERSOURCE}/include"
	"${SHEEPSHAVERSOURCE}/osxguest"
	"${SHEEPSHAVERSOURCE}/Unix/dyngen_precompiled"
	"${BASILISKIISOURCE}/include"
	"${BASILISKIISOURCE}/CrossPlatform"
	"${KPXCPUSOURCE}/include"
	"${KPXCPUSOURCE}/src"
	"${SHEEPSHAVERSOURCE}/gfxaccel/include"
	"${SFLLIBRARIES}"
)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	list(REMOVE_ITEM SFLCORESOURCES "${SHEEPSHAVERSOURCE}/gfxaccel.cpp")
endif()

add_library(SHEEPSHAVERSFLSFLCORE OBJECT ${SFLCORESOURCES} ${SFLPLATFORMSOURCES}
	"${CMAKE_CURRENT_SOURCE_DIR}/sflmem.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/video_sfl.cpp"
	"${SHEEPSHAVERSOURCE}/dummy/prefs_dummy.cpp")
if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	target_sources(SHEEPSHAVERSFLSFLCORE PRIVATE "${SHEEPSHAVERSOURCE}/MacOSX/Carbon/osxguest_carbon.cpp")
else()
	include("${SHEEPSHAVERSOURCE}/osxguest/osxguest.cmake")
	target_sources(SHEEPSHAVERSFLSFLCORE PRIVATE ${OSXGUESTSOURCES}
		"${CMAKE_CURRENT_SOURCE_DIR}/osxhost_sfl.cpp"
		"${SHEEPSHAVERSOURCE}/CrossPlatform/sigsegv.cpp")
	target_compile_definitions(SHEEPSHAVERSFLSFLCORE PRIVATE SFLFAULTHANDLER=1)
	if(WIN32)
		set_source_files_properties("${SHEEPSHAVERSOURCE}/CrossPlatform/sigsegv.cpp" PROPERTIES COMPILE_DEFINITIONS HAVE_WIN32_VM=1)
	endif()
	if(NOT APPLE)
		set(GRAPHICSACCELERATIONSOURCE "${SHEEPSHAVERSOURCE}/gfxaccel")
		add_library(SHEEPSHAVERSFLGFXACCEL OBJECT
			"${SHEEPSHAVERSOURCE}/gfxaccel.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/display_mode_controller.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_alt_buffer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_clut_gamma.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_dispatch.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_draw_context.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_engine.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_flatten_restore.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_install_hooks.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_mode_enumerate.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/dsp_thunks.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gfxaccel_resources.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gfxaccel_resources_heap.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl_defer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl_dispatch.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl_engine.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl_state.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gfxaccelhost.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl_thunks.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/rave_dispatch.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/rave_draw_context.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/rave_engine.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/rave_thunks.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/glide_dispatch.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/glide_engine.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/glide_state.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/glide_pef_register.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/gl_compositor.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/vbl_source_sdl.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/gfxaccel_resources_gl.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/gfxaccel_resources_heap_gl.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/gfxaccel_arc_shim.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/pso_archive_gl.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/nqd_gl_renderer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/rave_gl_renderer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/gl_ffp_renderer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/dsp_gl_renderer.cpp"
			"${GRAPHICSACCELERATIONSOURCE}/gl/glide_gl_renderer.cpp"
			"${SHEEPSHAVERSOURCE}/osxguest/osxglengine.cpp")
		set_target_properties(SHEEPSHAVERSFLGFXACCEL PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
		target_include_directories(SHEEPSHAVERSFLGFXACCEL PRIVATE
			"${GRAPHICSACCELERATIONSOURCE}" "${GRAPHICSACCELERATIONSOURCE}/gl" ${SFLINCLUDEDIRECTORIES} "${SHEEPSHAVERROOT}/../thirdparty/SDL-1.2.15/include")
		target_compile_definitions(SHEEPSHAVERSFLGFXACCEL PRIVATE
			SHEEPSHAVER=1 ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1 USE_SDL1=1)
		sfl_configure_core(SHEEPSHAVERSFLGFXACCEL)
		target_compile_definitions(SHEEPSHAVERSFLSFLCORE PRIVATE OSXGLBRIDGE=1 ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1)
		target_include_directories(SHEEPSHAVERSFLSFLCORE PRIVATE "${GRAPHICSACCELERATIONSOURCE}")
		target_sources(SHEEPSHAVERSFLSFLCORE PRIVATE "${SHEEPSHAVERSOURCE}/gfxaccel/gl/glcontext.c")
		if(WIN32)
			target_compile_definitions(SHEEPSHAVERSFLSFLCORE PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
			target_compile_definitions(SHEEPSHAVERSFLGFXACCEL PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES=1 __WIN32__)
		else()
			find_package(PkgConfig REQUIRED)
			pkg_check_modules(GDK3 REQUIRED gdk-3.0)
			target_include_directories(SHEEPSHAVERSFLSFLCORE PRIVATE ${GDK3_INCLUDE_DIRS})
			target_include_directories(SHEEPSHAVERSFLGFXACCEL PRIVATE ${GDK3_INCLUDE_DIRS})
		endif()
	endif()
endif()
if(NOT MSVC)
	set_source_files_properties("${SHEEPSHAVERROOT}/../guest/sheepbench/sbkernels.c" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_include_directories(SHEEPSHAVERSFLSFLCORE PRIVATE ${SFLINCLUDEDIRECTORIES})
target_compile_definitions(SHEEPSHAVERSFLSFLCORE PRIVATE SHEEPSHAVER=1)
sfl_configure_core(SHEEPSHAVERSFLSFLCORE)
sfl_add_application(SheepShaverSFL SHEP SHEEPSHAVERSFLSFLCORE)
if(TARGET SHEEPSHAVERSFLGFXACCEL)
	target_sources(SheepShaverSFL PRIVATE $<TARGET_OBJECTS:SHEEPSHAVERSFLGFXACCEL>)
endif()

if(WIN32)
	target_link_libraries(SheepShaverSFL PRIVATE opengl32)
elseif(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	find_package(OpenGL REQUIRED)
	target_link_libraries(SheepShaverSFL PRIVATE OpenGL::GL ${GDK3_LIBRARIES})
endif()
