cmake_minimum_required(VERSION 3.16)
project(SflHostTools C CXX)
set(B2SRC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include(../SFL.cmake)
set(UAE_DIR "${B2SRC}/uae_cpu")
set(B2_UAE_GEN_SRCS
	"${B2_GEN}/cpuemu.cpp" "${B2_GEN}/cpustbl.cpp" "${B2_GEN}/cputbl.h"
	"${B2_GEN}/cpuemu_nf.cpp" "${B2_GEN}/cpustbl_nf.cpp")
add_executable(sfl_build68k "${UAE_DIR}/build68k.c")
target_include_directories(sfl_build68k PRIVATE
	"${SFLDIR}" "${B2SRC}/include" "${UAE_DIR}")
target_compile_definitions(sfl_build68k PRIVATE HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_build68k PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(
	OUTPUT "${B2_GEN}/cpudefs.cpp"
	COMMAND ${CMAKE_COMMAND}
	-DMACEMU_REDIRECT_TOOL=$<TARGET_FILE:sfl_build68k>
	"-DMACEMU_REDIRECT_ARGS=\"${UAE_DIR}/table68k\""
	-DMACEMU_REDIRECT_OUTPUT=${B2_GEN}/cpudefs.cpp
	-P "${B2SRC}/../../cmake/run_redirect.cmake"
	DEPENDS sfl_build68k "${UAE_DIR}/table68k"
	COMMENT "Generating cpudefs.cpp"
	VERBATIM
)

add_library(sfl_uae_readcpu STATIC "${UAE_DIR}/readcpu.cpp" "${B2_GEN}/cpudefs.cpp")
target_include_directories(sfl_uae_readcpu PUBLIC
	"${SFLDIR}" "${B2SRC}/include" "${UAE_DIR}" "${B2_GEN}")
target_compile_definitions(sfl_uae_readcpu PUBLIC HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_uae_readcpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(sfl_gencpu "${UAE_DIR}/gencpu.c")
target_link_libraries(sfl_gencpu PRIVATE sfl_uae_readcpu)
target_compile_definitions(sfl_gencpu PRIVATE HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_gencpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(
	OUTPUT ${B2_UAE_GEN_SRCS}
	COMMAND $<TARGET_FILE:sfl_gencpu>
	DEPENDS sfl_gencpu "${B2_GEN}/cpudefs.cpp"
	WORKING_DIRECTORY "${B2_GEN}"
	COMMENT "Running gencpu"
	VERBATIM
)

sfl_configure_core(sfl_build68k)
sfl_configure_core(sfl_uae_readcpu)
sfl_configure_core(sfl_gencpu)

add_custom_target(sfl_uae_tables DEPENDS ${B2_UAE_GEN_SRCS})
