cmake_minimum_required(VERSION 3.16)
project(SflHostTools C CXX)
set(BASILISKIISOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include(../SFL.cmake)
set(UAEDIRECTORY "${BASILISKIISOURCE}/uae_cpu")
set(BASILISKIIUAEGENERATEDSOURCES
	"${BASILISKIIGENERATED}/cpuemu.cpp" "${BASILISKIIGENERATED}/cpustbl.cpp" "${BASILISKIIGENERATED}/cputbl.h"
	"${BASILISKIIGENERATED}/cpuemu_nf.cpp" "${BASILISKIIGENERATED}/cpustbl_nf.cpp")
add_executable(sfl_build68k "${UAEDIRECTORY}/build68k.c")
target_include_directories(sfl_build68k PRIVATE
	"${SFLDIRECTORY}" "${BASILISKIISOURCE}/include" "${UAEDIRECTORY}")
target_compile_definitions(sfl_build68k PRIVATE HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_build68k PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(
	OUTPUT "${BASILISKIIGENERATED}/cpudefs.cpp"
	COMMAND ${CMAKE_COMMAND}
	-DMACEMU_REDIRECT_TOOL=$<TARGET_FILE:sfl_build68k>
	"-DMACEMU_REDIRECT_ARGS=\"${UAEDIRECTORY}/table68k\""
	-DMACEMU_REDIRECT_OUTPUT=${BASILISKIIGENERATED}/cpudefs.cpp
	-P "${BASILISKIISOURCE}/../../cmake/run_redirect.cmake"
	DEPENDS sfl_build68k "${UAEDIRECTORY}/table68k"
	COMMENT "Generating cpudefs.cpp"
	VERBATIM
)

add_library(sfl_uae_readcpu STATIC "${UAEDIRECTORY}/readcpu.cpp" "${BASILISKIIGENERATED}/cpudefs.cpp")
target_include_directories(sfl_uae_readcpu PUBLIC
	"${SFLDIRECTORY}" "${BASILISKIISOURCE}/include" "${UAEDIRECTORY}" "${BASILISKIIGENERATED}")
target_compile_definitions(sfl_uae_readcpu PUBLIC HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_uae_readcpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(sfl_gencpu "${UAEDIRECTORY}/gencpu.c")
target_link_libraries(sfl_gencpu PRIVATE sfl_uae_readcpu)
target_compile_definitions(sfl_gencpu PRIVATE HAVE_CONFIG_H)
if(MSVC)
	target_compile_definitions(sfl_gencpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(
	OUTPUT ${BASILISKIIUAEGENERATEDSOURCES}
	COMMAND $<TARGET_FILE:sfl_gencpu>
	DEPENDS sfl_gencpu "${BASILISKIIGENERATED}/cpudefs.cpp"
	WORKING_DIRECTORY "${BASILISKIIGENERATED}"
	COMMENT "Running gencpu"
	VERBATIM
)

sfl_configure_core(sfl_build68k)
sfl_configure_core(sfl_uae_readcpu)
sfl_configure_core(sfl_gencpu)

add_custom_target(sfl_uae_tables DEPENDS ${BASILISKIIUAEGENERATEDSOURCES})
