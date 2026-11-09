cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	project(SheepShaverSFL C CXX)
else()
	unset(CMAKE_C_STANDARD)
endif()
option(SFL_MONITOR "Build the emulator monitor server" OFF)

set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(CMAKE_HOST_WIN32 AND CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	add_compile_definitions(__STRINGS__)
	add_compile_options("SHELL:-include bsdstrings.h")
endif()

get_filename_component(SSROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SSSRC "${SSROOT}/src")
set(B2SRC "${SSROOT}/../BasiliskII/src")
set(KPX "${SSSRC}/kpx_cpu")
include("${B2SRC}/SFL/SFL.cmake")

set(SFLCORE
	"${B2SRC}/adb.cpp"
	"${B2SRC}/audio.cpp"
	"${B2SRC}/cdrom.cpp"
	"${B2SRC}/bincue.cpp"
	"${B2SRC}/disk.cpp"
	"${B2SRC}/qcow.cpp"
	"${B2SRC}/chd.cpp"
	"${B2SRC}/imageio.cpp"
	"${B2SRC}/prefs.cpp"
	"${B2SRC}/scsi.cpp"
	"${B2SRC}/sony.cpp"
	"${B2SRC}/timer.cpp"
	"${B2SRC}/xpram.cpp"
	"${B2SRC}/joymanager.cpp"
	"${B2SRC}/extfs.cpp"
	"${SSSRC}/main.cpp"
	"${SSSRC}/prefs_items.cpp"
	"${SSSRC}/rom_patches.cpp"
	"${SSSRC}/rsrc_patches.cpp"
	"${SSSRC}/emul_op.cpp"
	"${SSSRC}/soundinput.cpp"
	"${SSSRC}/shears.cpp"
	"${SSROOT}/../guest/sheepbench/sbkernels.c"
	"${SSSRC}/name_registry.cpp"
	"${SSSRC}/usbhid.cpp"
	"${SSSRC}/usbuim.cpp"
	"${SSSRC}/macio_escc.cpp"
	"${SSSRC}/mmio.cpp"
	"${SSSRC}/pcibridge.cpp"
	"${SSSRC}/macos_util.cpp"
	"${SSSRC}/video.cpp"
	"${SSSRC}/ether.cpp"
	"${SSSRC}/thunks.cpp"
	"${SSSRC}/serial.cpp"
	"${SSSRC}/user_strings.cpp"
	"${SSSRC}/gfxaccel.cpp"
	"${B2SRC}/printing.cpp"
	"${B2SRC}/pictpdf.c"
	"${B2SRC}/pdfgen.c"
	"${KPX}/src/mathlib/ieeefp.cpp"
	"${KPX}/src/mathlib/mathlib.cpp"
	"${KPX}/src/cpu/ppc/ppc-cpu.cpp"
	"${KPX}/src/cpu/ppc/ppc-decode.cpp"
	"${KPX}/src/cpu/ppc/ppc-execute.cpp"
	"${KPX}/src/cpu/ppc/ppc-translate.cpp"
	"${KPX}/src/utils/utils-cpuinfo.cpp"
	"${KPX}/sheepshaver_glue.cpp"
	"${KPX}/ppc-dis.c"
)

set(SFLINCLUDES
	"${SFLDIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${SSSRC}/include"
	"${SSSRC}/osxguest"
	"${SSSRC}/Unix/dyngen_precompiled"
	"${B2SRC}/include"
	"${B2SRC}/CrossPlatform"
	"${KPX}/include"
	"${KPX}/src"
	"${SSSRC}/gfxaccel/include"
	"${SFLLIBS}"
)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	list(REMOVE_ITEM SFLCORE "${SSSRC}/gfxaccel.cpp")
endif()

add_library(SheepShaverSFL_sflcore OBJECT ${SFLCORE} ${SFLPLATFORM}
	"${CMAKE_CURRENT_SOURCE_DIR}/sflmem.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/video_sfl.cpp"
	"${SSSRC}/dummy/prefs_dummy.cpp")
if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	target_sources(SheepShaverSFL_sflcore PRIVATE "${SSSRC}/MacOSX/Carbon/osxguest_carbon.cpp")
else()
	include("${SSSRC}/osxguest/osxguest.cmake")
	target_sources(SheepShaverSFL_sflcore PRIVATE ${OSXGUEST_SOURCES}
		"${CMAKE_CURRENT_SOURCE_DIR}/osxhost_sfl.cpp"
		"${SSSRC}/CrossPlatform/sigsegv.cpp")
	target_compile_definitions(SheepShaverSFL_sflcore PRIVATE SFLFAULTHANDLER=1)
	if(WIN32)
		set_source_files_properties("${SSSRC}/CrossPlatform/sigsegv.cpp" PROPERTIES COMPILE_DEFINITIONS HAVE_WIN32_VM=1)
	endif()
	if(NOT APPLE)
		set(SFLGFX "${SSSRC}/gfxaccel")
		add_library(SheepShaverSFL_gfxaccel OBJECT
			"${SSSRC}/gfxaccel.cpp"
			"${SFLGFX}/display_mode_controller.cpp"
			"${SFLGFX}/dsp_alt_buffer.cpp"
			"${SFLGFX}/dsp_clut_gamma.cpp"
			"${SFLGFX}/dsp_dispatch.cpp"
			"${SFLGFX}/dsp_draw_context.cpp"
			"${SFLGFX}/dsp_engine.cpp"
			"${SFLGFX}/dsp_flatten_restore.cpp"
			"${SFLGFX}/dsp_install_hooks.cpp"
			"${SFLGFX}/dsp_mode_enumerate.cpp"
			"${SFLGFX}/dsp_thunks.cpp"
			"${SFLGFX}/gfxaccel_resources.cpp"
			"${SFLGFX}/gfxaccel_resources_heap.cpp"
			"${SFLGFX}/gl_defer.cpp"
			"${SFLGFX}/gl_dispatch.cpp"
			"${SFLGFX}/gl_engine.cpp"
			"${SFLGFX}/gl_state.cpp"
			"${SFLGFX}/gl_thunks.cpp"
			"${SFLGFX}/rave_dispatch.cpp"
			"${SFLGFX}/rave_draw_context.cpp"
			"${SFLGFX}/rave_engine.cpp"
			"${SFLGFX}/rave_thunks.cpp"
			"${SFLGFX}/glide_dispatch.cpp"
			"${SFLGFX}/glide_engine.cpp"
			"${SFLGFX}/glide_state.cpp"
			"${SFLGFX}/glide_pef_register.cpp"
			"${SFLGFX}/gl/gl_compositor.cpp"
			"${SFLGFX}/gl/vbl_source_sdl.cpp"
			"${SFLGFX}/gl/gfxaccel_resources_gl.cpp"
			"${SFLGFX}/gl/gfxaccel_resources_heap_gl.cpp"
			"${SFLGFX}/gl/gfxaccel_arc_shim.cpp"
			"${SFLGFX}/gl/pso_archive_gl.cpp"
			"${SFLGFX}/gl/nqd_gl_renderer.cpp"
			"${SFLGFX}/gl/rave_gl_renderer.cpp"
			"${SFLGFX}/gl/gl_ffp_renderer.cpp"
			"${SFLGFX}/gl/dsp_gl_renderer.cpp"
			"${SFLGFX}/gl/glide_gl_renderer.cpp"
			"${SSSRC}/osxguest/osxglengine.cpp")
		set_target_properties(SheepShaverSFL_gfxaccel PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
		target_include_directories(SheepShaverSFL_gfxaccel PRIVATE
			"${SFLGFX}" "${SFLGFX}/gl" ${SFLINCLUDES} "${SSROOT}/../thirdparty/SDL-1.2.15/include")
		target_compile_definitions(SheepShaverSFL_gfxaccel PRIVATE
			SHEEPSHAVER=1 ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1 USE_SDL1=1)
		sfl_configure_core(SheepShaverSFL_gfxaccel)
		target_compile_definitions(SheepShaverSFL_sflcore PRIVATE OSXGLBRIDGE=1 ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1)
		target_include_directories(SheepShaverSFL_sflcore PRIVATE "${SFLGFX}")
		target_sources(SheepShaverSFL_sflcore PRIVATE "${SSSRC}/gfxaccel/gl/glcontext.c")
		if(WIN32)
			target_compile_definitions(SheepShaverSFL_sflcore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
			target_compile_definitions(SheepShaverSFL_gfxaccel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES=1 __WIN32__)
		else()
			find_package(PkgConfig REQUIRED)
			pkg_check_modules(GDK3 REQUIRED gdk-3.0)
			target_include_directories(SheepShaverSFL_sflcore PRIVATE ${GDK3_INCLUDE_DIRS})
			target_include_directories(SheepShaverSFL_gfxaccel PRIVATE ${GDK3_INCLUDE_DIRS})
		endif()
	endif()
endif()
if(NOT MSVC)
	set_source_files_properties("${SSROOT}/../guest/sheepbench/sbkernels.c" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_include_directories(SheepShaverSFL_sflcore PRIVATE ${SFLINCLUDES})
target_compile_definitions(SheepShaverSFL_sflcore PRIVATE SHEEPSHAVER=1)
sfl_configure_core(SheepShaverSFL_sflcore)
sfl_add_application(SheepShaverSFL SHEP)
if(TARGET SheepShaverSFL_gfxaccel)
	target_sources(SheepShaverSFL PRIVATE $<TARGET_OBJECTS:SheepShaverSFL_gfxaccel>)
endif()

if(WIN32)
	target_link_libraries(SheepShaverSFL PRIVATE opengl32)
elseif(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	find_package(OpenGL REQUIRED)
	target_link_libraries(SheepShaverSFL PRIVATE OpenGL::GL ${GDK3_LIBRARIES})
endif()
