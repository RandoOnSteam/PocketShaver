cmake_minimum_required(VERSION 3.10)
project(SheepBench C CXX)
set(HOSTCC "cc" CACHE FILEPATH "build machine C compiler for sbexpected")
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sbexpected.h
	COMMAND ${HOSTCC} -O2 -ffp-contract=off -o ${CMAKE_CURRENT_BINARY_DIR}/sbexpected ${CMAKE_CURRENT_SOURCE_DIR}/sbexpected.c ${CMAKE_CURRENT_SOURCE_DIR}/sbkernels.c
	COMMAND ${CMAKE_CURRENT_BINARY_DIR}/sbexpected > ${CMAKE_CURRENT_BINARY_DIR}/sbexpected.h
	DEPENDS sbexpected.c sbkernels.c sbkernels.h sbcases.h
	VERBATIM)
get_filename_component(TOOLBIN "${CMAKE_C_COMPILER}" DIRECTORY)
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/k68code.h
	COMMAND ${TOOLBIN}/m68k-apple-macos-as -m68020 -o ${CMAKE_CURRENT_BINARY_DIR}/kernels68k.o ${CMAKE_CURRENT_SOURCE_DIR}/kernels68k.s
	COMMAND ${TOOLBIN}/m68k-apple-macos-objcopy -O binary -j .text ${CMAKE_CURRENT_BINARY_DIR}/kernels68k.o ${CMAKE_CURRENT_BINARY_DIR}/k68code
	COMMAND xxd -i k68code > k68code.h
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	DEPENDS kernels68k.s
	VERBATIM)
set(SHEEPBENCHSOURCES main.c benchcore.c benchui.c suitecpu.c suite68k.c suitememory.c suitemmu.c suitequickdraw.c suite3d.c
	suiteqd3d.c suitedisk.c sbaltivec.c sbkernels.c ${CMAKE_CURRENT_BINARY_DIR}/sbexpected.h ${CMAKE_CURRENT_BINARY_DIR}/k68code.h)
set(PDCURSESDIR ${CMAKE_CURRENT_SOURCE_DIR}/../pdcurses)
file(GLOB PDCURSESSOURCES ${PDCURSESDIR}/pdcurses/*.c ${PDCURSESDIR}/mac/*.c)
add_application(SheepBenchCurses ${SHEEPBENCHSOURCES} ${PDCURSESSOURCES} sheepbenchcurses.r CREATOR "ShBc")
target_include_directories(SheepBenchCurses PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${PDCURSESDIR}
	${PDCURSESDIR}/mac ${PDCURSESDIR}/common)
target_compile_definitions(SheepBenchCurses PRIVATE SHEEPBENCHCURSES)
target_compile_options(SheepBenchCurses PRIVATE -O2 -Wno-multichar -ffp-contract=off -Wno-pointer-sign -Wno-unused-function)
add_application(SheepBench main.c benchcore.c benchui.c suitecpu.c suite68k.c suitememory.c suitemmu.c suitequickdraw.c suite3d.c
	suiteqd3d.c suitedisk.c sbaltivec.c sbkernels.c ${CMAKE_CURRENT_BINARY_DIR}/sbexpected.h ${CMAKE_CURRENT_BINARY_DIR}/k68code.h
	CONSOLE CREATOR "ShBn")
target_include_directories(SheepBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(SheepBench PRIVATE -O2 -Wall -Wno-multichar -ffp-contract=off -Wno-pointer-sign -Wno-unused-function)
set_source_files_properties(sbaltivec.c PROPERTIES COMPILE_FLAGS "-maltivec -Wa,-maltivec")
