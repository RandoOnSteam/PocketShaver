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

add_library(SheepShaverSFL_sflcore OBJECT ${SFLCORE} ${SFLPLATFORM}
	"${CMAKE_CURRENT_SOURCE_DIR}/sflmem.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/video_sfl.cpp"
	"${SSSRC}/dummy/prefs_dummy.cpp")
if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	target_sources(SheepShaverSFL_sflcore PRIVATE "${SSSRC}/MacOSX/Carbon/osxguest_carbon.cpp")
else()
	include("${SSSRC}/osxguest/osxguest.cmake")
	target_sources(SheepShaverSFL_sflcore PRIVATE ${OSXGUEST_SOURCES}
		"${CMAKE_CURRENT_SOURCE_DIR}/osxhost_sfl.cpp")
	if(WIN32)
		target_compile_definitions(SheepShaverSFL_sflcore PRIVATE WIN32_LEAN_AND_MEAN OSXGLBRIDGE=1)
		target_sources(SheepShaverSFL_sflcore PRIVATE "${SSSRC}/gfxaccel/gl/glcontext.c")
	elseif(NOT APPLE)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(GDK3 REQUIRED gdk-3.0)
		target_include_directories(SheepShaverSFL_sflcore PRIVATE ${GDK3_INCLUDE_DIRS})
		target_compile_definitions(SheepShaverSFL_sflcore PRIVATE OSXGLBRIDGE=1)
		target_sources(SheepShaverSFL_sflcore PRIVATE "${SSSRC}/gfxaccel/gl/glcontext.c")
	endif()
endif()
if(NOT MSVC)
	set_source_files_properties("${SSROOT}/../guest/sheepbench/sbkernels.c" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_include_directories(SheepShaverSFL_sflcore PRIVATE ${SFLINCLUDES})
target_compile_definitions(SheepShaverSFL_sflcore PRIVATE SHEEPSHAVER=1)
sfl_configure_core(SheepShaverSFL_sflcore)
sfl_add_application(SheepShaverSFL SHEP)

if(WIN32)
	target_link_libraries(SheepShaverSFL PRIVATE opengl32)
elseif(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	find_package(OpenGL REQUIRED)
	target_link_libraries(SheepShaverSFL PRIVATE OpenGL::GL ${GDK3_LIBRARIES})
endif()
