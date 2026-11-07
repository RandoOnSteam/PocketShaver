cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	project(BasiliskIISFL C CXX)
else()
	unset(CMAKE_C_STANDARD)
endif()
option(SFL_MONITOR "Build the emulator monitor server" OFF)
set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(B2SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")
include(SFL.cmake)
set(UAE_DIR "${B2SRC}/uae_cpu")
set(B2_GEN "${CMAKE_CURRENT_BINARY_DIR}/uae_gen")
file(MAKE_DIRECTORY "${B2_GEN}")
if(CMAKE_HOST_WIN32 AND CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
	add_compile_definitions(__STRINGS__)
	add_compile_options("SHELL:-include bsdstrings.h")
endif()
set(B2_UAE_GEN_SRCS
	"${B2_GEN}/cpuemu.cpp" "${B2_GEN}/cpustbl.cpp" "${B2_GEN}/cputbl.h"
	"${B2_GEN}/cpuemu_nf.cpp" "${B2_GEN}/cpustbl_nf.cpp")
include(ExternalProject)
ExternalProject_Add(sfl_uae_tables
	SOURCE_DIR "${SFLDIR}/HostTools"
	BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/host-tools"
	CMAKE_ARGS "-DB2_GEN=${B2_GEN}" "-DCMAKE_BUILD_TYPE=Release"
		"-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
	BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target sfl_uae_tables
	BUILD_ALWAYS TRUE
	BUILD_BYPRODUCTS "${B2_GEN}/cpudefs.cpp" ${B2_UAE_GEN_SRCS}
	INSTALL_COMMAND "")
set_source_files_properties("${B2_GEN}/cpudefs.cpp" ${B2_UAE_GEN_SRCS} PROPERTIES GENERATED TRUE)

set(B2_CORE
	"${B2SRC}/main.cpp"
	"${B2SRC}/prefs.cpp"
	"${B2SRC}/prefs_items.cpp"
	"${B2SRC}/rom_patches.cpp"
	"${B2SRC}/slot_rom.cpp"
	"${B2SRC}/rsrc_patches.cpp"
	"${B2SRC}/emul_op.cpp"
	"${B2SRC}/macos_util.cpp"
	"${B2SRC}/xpram.cpp"
	"${B2SRC}/timer.cpp"
	"${B2SRC}/adb.cpp"
	"${B2SRC}/serial.cpp"
	"${B2SRC}/ether.cpp"
	"${B2SRC}/sony.cpp"
	"${B2SRC}/disk.cpp"
	"${B2SRC}/qcow.cpp"
	"${B2SRC}/chd.cpp"
	"${B2SRC}/imageio.cpp"
	"${B2SRC}/cdrom.cpp"
	"${B2SRC}/scsi.cpp"
	"${B2SRC}/video.cpp"
	"${B2SRC}/audio.cpp"
	"${B2SRC}/extfs.cpp"
	"${B2SRC}/user_strings.cpp"
	"${B2SRC}/printing.cpp"
	"${B2SRC}/pictpdf.c"
	"${B2SRC}/pdfgen.c"
	"${B2SRC}/joymanager.cpp"
	"${B2SRC}/bincue.cpp"
)


set(B2_UAE
	"${UAE_DIR}/basilisk_glue.cpp"
	"${UAE_DIR}/memory.cpp"
	"${UAE_DIR}/newcpu.cpp"
	"${UAE_DIR}/readcpu.cpp"
	"${UAE_DIR}/fpu/fpu_ieee.cpp"
	"${B2_GEN}/cpudefs.cpp"
	"${B2_GEN}/cpuemu.cpp"
	"${B2_GEN}/cpustbl.cpp"
	"${B2_GEN}/cpuemu_nf.cpp"
	"${B2_GEN}/cpustbl_nf.cpp"
)

add_library(BasiliskIISFL_sflcore OBJECT ${B2_CORE} ${B2_UAE} ${SFLPLATFORM}
	"${SFLDIR}/video_sfl.cpp")
add_dependencies(BasiliskIISFL_sflcore sfl_uae_tables)
target_include_directories(BasiliskIISFL_sflcore PRIVATE "${SFLDIR}" "${B2SRC}/include"
	"${UAE_DIR}" "${B2_GEN}" "${SFLLIBS}")
sfl_configure_core(BasiliskIISFL_sflcore)
sfl_add_application(BasiliskIISFL B2EM)
