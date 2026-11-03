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
set(SFLDIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SFLLIBS "${SSROOT}/../thirdparty/sfl")

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

set(SFLPLATFORM
	"${SFLDIR}/sflmem.cpp"
	"${SFLDIR}/user_strings_sfl.cpp"
	"${SFLDIR}/main_sfl.cpp"
	"${SFLDIR}/video_sfl.cpp"
	"${SFLDIR}/audio_sfl.cpp"
	"${SFLDIR}/joy_sfl.cpp"
	"${SFLDIR}/timer_sfl.cpp"
	"${SFLDIR}/sys_sfl.cpp"
	"${SFLDIR}/prefs_sfl.cpp"
	"${SFLDIR}/xpram_sfl.cpp"
	"${SSSRC}/MacOSX/Carbon/osxguest_carbon.cpp"
	"${SFLDIR}/clip_sfl.cpp"
	"${SFLDIR}/extfs_sfl.cpp"
	"${SFLDIR}/posix_sfl.cpp"
	"${SFLDIR}/sflroman.cpp"
	"${SSSRC}/dummy/serial_dummy.cpp"
	"${SSSRC}/dummy/scsi_dummy.cpp"
	"${SSSRC}/dummy/prefs_dummy.cpp"
	"${SSSRC}/dummy/prefs_editor_dummy.cpp"
	"${SFLDIR}/ether_sfl.cpp"
)

set(SFLINCLUDES
	"${SFLDIR}"
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

set(SFLDEFINES HAVE_CONFIG_H)

add_library(sflcore OBJECT ${SFLCORE} ${SFLPLATFORM})
if(NOT WIN32)
	set_source_files_properties("${B2SRC}/extfs.cpp" PROPERTIES COMPILE_DEFINITIONS WIN32)
endif()
target_include_directories(sflcore PRIVATE ${SFLINCLUDES})
target_compile_definitions(sflcore PRIVATE ${SFLDEFINES})
if(MSVC)
	target_compile_options(sflcore PRIVATE /W0 "/FI${SFLDIR}/sysdeps.h")
else()
	target_compile_options(sflcore PRIVATE -w -fno-strict-aliasing -include "${SFLDIR}/sysdeps.h")
	set_source_files_properties("${SSROOT}/../guest/sheepbench/sbkernels.c" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

set(SFLLIBSOURCES
	"${SFLDIR}/sflimpl.c"
	"${SFLDIR}/sflui.c"
	"${SFLDIR}/sflaudio.c"
	"${SFLDIR}/sflnet.c"
	"${SFLDIR}/sflsocket.c"
)
if(SFL_MONITOR)
	list(APPEND SFLLIBSOURCES "${SFLDIR}/sflmonitor.c")
endif()

add_library(sfllibs OBJECT ${SFLLIBSOURCES})
target_include_directories(sfllibs PRIVATE "${SFLLIBS}" "${SFLDIR}")
if(WIN32)
	target_compile_definitions(sfllibs PRIVATE _CONSOLE)
endif()
if(SFL_MONITOR)
	target_compile_definitions(sfllibs PRIVATE SFLMONITORENABLED)
endif()
if(MSVC)
	target_compile_options(sfllibs PRIVATE /W0)
else()
	target_compile_options(sfllibs PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SFLGTK REQUIRED gtk+-3.0 x11 xfixes xrandr xinerama xxf86vm wayland-client wayland-cursor fontconfig)
	target_include_directories(sfllibs PRIVATE ${SFLGTK_INCLUDE_DIRS})
	target_compile_options(sfllibs PRIVATE ${SFLGTK_CFLAGS_OTHER})
endif()

set(SFLSLIRPSOURCES
	"${B2SRC}/slirp/bootp.c"
	"${B2SRC}/slirp/cksum.c"
	"${B2SRC}/slirp/debug.c"
	"${B2SRC}/slirp/if.c"
	"${B2SRC}/slirp/ip_icmp.c"
	"${B2SRC}/slirp/ip_input.c"
	"${B2SRC}/slirp/ip_output.c"
	"${B2SRC}/slirp/mbuf.c"
	"${B2SRC}/slirp/misc.c"
	"${B2SRC}/slirp/sbuf.c"
	"${B2SRC}/slirp/slirp.c"
	"${B2SRC}/slirp/socket.c"
	"${B2SRC}/slirp/tcp_input.c"
	"${B2SRC}/slirp/tcp_output.c"
	"${B2SRC}/slirp/tcp_subr.c"
	"${B2SRC}/slirp/tcp_timer.c"
	"${B2SRC}/slirp/udp.c"
	"${SFLDIR}/sflslirp.c"
)

add_library(sflslirp OBJECT ${SFLSLIRPSOURCES})
target_include_directories(sflslirp PRIVATE "${SFLDIR}" "${B2SRC}/slirp")
if(MSVC)
	target_compile_options(sflslirp PRIVATE /W0)
else()
	target_compile_options(sflslirp PRIVATE -w)
endif()

if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	add_application(SheepShaverSFL "${SFLDIR}/sflcarbon.r" CREATOR "SHEP")
	target_sources(SheepShaverSFL PRIVATE $<TARGET_OBJECTS:sflcore> $<TARGET_OBJECTS:sfllibs> $<TARGET_OBJECTS:sflslirp>)
	set_target_properties(SheepShaverSFL PROPERTIES LINKER_LANGUAGE CXX)
else()
	add_executable(SheepShaverSFL $<TARGET_OBJECTS:sflcore> $<TARGET_OBJECTS:sfllibs> $<TARGET_OBJECTS:sflslirp>)
endif()
if(WIN32)
	target_link_libraries(SheepShaverSFL PRIVATE user32 gdi32 winmm ole32 shell32 advapi32 comdlg32 comctl32 uuid ws2_32)
endif()
if(UNIX AND NOT APPLE)
	target_link_libraries(SheepShaverSFL PRIVATE ${SFLGTK_LIBRARIES} dl pthread m)
endif()
