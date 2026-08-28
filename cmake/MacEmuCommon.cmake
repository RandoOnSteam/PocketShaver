# Shared CMake helpers for Basilisk II and SheepShaver.
# Included from the root CMakeLists.txt (or from a standalone subproject).

include(CheckIncludeFile)
include(CheckTypeSize)
include(GNUInstallDirs)

if(APPLE)
	include(MacEmuApple)
endif()

# ---------------------------------------------------------------------------
# Host / feature probes
# ---------------------------------------------------------------------------
function(macemu_detect_host)
	# iOS / Mac Catalyst use the vendored MacOSX/config headers; skip host
	# probes (try_compile against the iOS SDK is slow and needs signing).
	if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
		set(USE_SDL 1 PARENT_SCOPE)
		set(USE_SDL_VIDEO 1 PARENT_SCOPE)
		set(USE_SDL_AUDIO 1 PARENT_SCOPE)
		set(HAVE_SLIRP 1 PARENT_SCOPE)
		set(HAVE_PTHREADS 1 PARENT_SCOPE)
		if(ENABLE_BINCUE)
			set(BINCUE 1 PARENT_SCOPE)
		endif()
		if(ENABLE_VOSF)
			set(ENABLE_VOSF 1 PARENT_SCOPE)
		endif()
		return()
	endif()

	check_type_size("short" SIZEOF_SHORT)
	check_type_size("int" SIZEOF_INT)
	check_type_size("long" SIZEOF_LONG)
	check_type_size("long long" SIZEOF_LONG_LONG)
	check_type_size("void *" SIZEOF_VOID_P)
	check_type_size("float" SIZEOF_FLOAT)
	check_type_size("double" SIZEOF_DOUBLE)
	check_type_size("long double" SIZEOF_LONG_DOUBLE)

	if(NOT SIZEOF_SHORT)
		set(SIZEOF_SHORT 2)
	endif()
	if(NOT SIZEOF_INT)
		set(SIZEOF_INT 4)
	endif()
	if(NOT SIZEOF_LONG)
		if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
			set(SIZEOF_LONG 8)
		else()
			set(SIZEOF_LONG 4)
		endif()
	endif()
	if(NOT SIZEOF_LONG_LONG)
		set(SIZEOF_LONG_LONG 8)
	endif()
	if(NOT SIZEOF_VOID_P)
		set(SIZEOF_VOID_P ${CMAKE_SIZEOF_VOID_P})
	endif()
	if(NOT SIZEOF_FLOAT)
		set(SIZEOF_FLOAT 4)
	endif()
	if(NOT SIZEOF_DOUBLE)
		set(SIZEOF_DOUBLE 8)
	endif()
	if(NOT SIZEOF_LONG_DOUBLE)
		set(SIZEOF_LONG_DOUBLE 8)
	endif()

	check_include_file(unistd.h HAVE_UNISTD_H)
	check_include_file(strings.h HAVE_STRINGS_H)
	check_include_file(fenv.h HAVE_FENV_H)
	check_include_file(stdint.h HAVE_STDINT_H)
	check_include_file(stdlib.h HAVE_STDLIB_H)
	check_include_file(string.h HAVE_STRING_H)
	check_include_file(memory.h HAVE_MEMORY_H)
	check_include_file(sys/types.h HAVE_SYS_TYPES_H)
	check_include_file(sys/stat.h HAVE_SYS_STAT_H)
	check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
	check_include_file(fcntl.h HAVE_FCNTL_H)
	check_include_file(sys/time.h HAVE_SYS_TIME_H)
	check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
	check_include_file(sys/socket.h HAVE_SYS_SOCKET_H)
	check_include_file(sys/mman.h HAVE_SYS_MMAN_H)
	check_include_file(sys/select.h HAVE_SYS_SELECT_H)
	check_include_file(sys/poll.h HAVE_SYS_POLL_H)
	check_include_file(sys/wait.h HAVE_SYS_WAIT_H)
	check_include_file(sys/filio.h HAVE_SYS_FILIO_H)
	check_include_file(arpa/inet.h HAVE_ARPA_INET_H)
	check_include_file(stropts.h HAVE_STROPTS_H)
	check_include_file(sys/stropts.h HAVE_SYS_STROPTS_H)
	check_include_file(pty.h HAVE_PTY_H)
	check_include_file(util.h HAVE_UTIL_H)

	include(CheckFunctionExists)
	include(CheckSymbolExists)
	check_function_exists(strdup HAVE_STRDUP)
	check_function_exists(strerror HAVE_STRERROR)
	check_function_exists(cfmakeraw HAVE_CFMAKERAW)
	check_symbol_exists(nanosleep time.h HAVE_NANOSLEEP)
	check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
	check_symbol_exists(clock_nanosleep time.h HAVE_CLOCK_NANOSLEEP)
	check_symbol_exists(getpagesize unistd.h HAVE_GETPAGESIZE)
	check_symbol_exists(sigaction signal.h HAVE_SIGACTION)
	check_symbol_exists(mmap sys/mman.h HAVE_MMAP)
	check_symbol_exists(mprotect sys/mman.h HAVE_MPROTECT_FUNC)

	set(USE_SDL 1)
	set(USE_SDL_VIDEO 1)
	set(USE_SDL_AUDIO 1)
	set(HAVE_SLIRP 1)

	if(WIN32)
		set(HAVE_WIN32_VM 1)
		set(HAVE_WIN32_EXCEPTIONS 1)
		set(HAVE_SIGSEGV_SKIP_INSTRUCTION 1)
	else()
		if(CMAKE_SYSTEM_PROCESSOR MATCHES
			 "^(i[3-6]86|x86|x86_64|amd64|AMD64|arm|ARM|aarch64|AARCH64|ppc|powerpc|ppc64|p
pc64le|mips|mips64|sparc|sparc64|ia64)")
			set(HAVE_SIGSEGV_SKIP_INSTRUCTION 1)
		endif()
		# SIGSEGV recovery mechanism (mirrors BasiliskII/src/Unix/configure.ac).
		# sigsegv.cpp gates its fault-handler definitions on HAVE_SIGINFO_T /
		# HAVE_SIGCONTEXT_SUBTERFUGE; without one of them the whole handler block
		# is preprocessed out (undefined SIGSEGV_FAULT_HANDLER_ARGLIST/_ADDRESS).
		include(CheckCXXSourceCompiles)
		check_cxx_source_compiles("
			#include <signal.h>
			#include <sys/types.h>
			static void handler(int, siginfo_t *sip, void *) {
				void *addr = sip->si_addr;
				(void)addr;
			}
			int main() {
				struct sigaction sa;
				sa.sa_sigaction = handler;
				sa.sa_flags = SA_SIGINFO;
				return sigaction(SIGSEGV, &sa, 0);
			}" HAVE_SIGINFO_T)
		if(HAVE_SIGINFO_T)
			set(HAVE_SIGINFO_T 1)
		else()
			# Fallback: sigcontext subterfuge (older/other platforms).
			check_cxx_source_compiles("
				#include <signal.h>
				static void handler(int, struct sigcontext scs) {
					(void)scs.cr2;
				}
				int main() {
					signal(SIGSEGV, (void (*)(int))handler);
					return 0;
				}" HAVE_SIGCONTEXT_SUBTERFUGE)
			if(HAVE_SIGCONTEXT_SUBTERFUGE)
				set(HAVE_SIGCONTEXT_SUBTERFUGE 1)
			endif()
		endif()
		find_package(Threads QUIET)
		if(CMAKE_USE_PTHREADS_INIT)
			set(HAVE_PTHREADS 1)
			set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
			check_symbol_exists(pthread_cancel pthread.h HAVE_PTHREAD_CANCEL)
			check_symbol_exists(pthread_testcancel pthread.h HAVE_PTHREAD_TESTCANCEL)
			check_symbol_exists(pthread_cond_init pthread.h HAVE_PTHREAD_COND_INIT)
			check_symbol_exists(pthread_mutexattr_setprotocol pthread.h
				HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL)
			check_symbol_exists(pthread_mutexattr_settype pthread.h
				HAVE_PTHREAD_MUTEXATTR_SETTYPE)
			unset(CMAKE_REQUIRED_LIBRARIES)
		endif()
	endif()
	if(ENABLE_VOSF)
		set(ENABLE_VOSF 1)
	endif()
	if(ENABLE_BINCUE)
		set(BINCUE 1)
	endif()

	# Export to parent scope
	foreach(HOST_PROBE_VAR
			SIZEOF_SHORT SIZEOF_INT SIZEOF_LONG SIZEOF_LONG_LONG SIZEOF_VOID_P
			SIZEOF_FLOAT SIZEOF_DOUBLE SIZEOF_LONG_DOUBLE
			HAVE_UNISTD_H HAVE_STRINGS_H HAVE_FENV_H
			HAVE_STDINT_H HAVE_STDLIB_H HAVE_STRING_H HAVE_MEMORY_H
			HAVE_SYS_TYPES_H HAVE_SYS_STAT_H
			HAVE_FCNTL_H HAVE_SYS_TIME_H HAVE_SYS_IOCTL_H HAVE_SYS_SOCKET_H
			HAVE_SYS_MMAN_H HAVE_SYS_SELECT_H HAVE_SYS_POLL_H HAVE_SYS_WAIT_H
			HAVE_SYS_FILIO_H HAVE_ARPA_INET_H HAVE_STROPTS_H HAVE_SYS_STROPTS_H
			HAVE_PTY_H HAVE_UTIL_H
			HAVE_STRDUP HAVE_STRERROR
			HAVE_PTHREADS HAVE_PTHREAD_CANCEL HAVE_PTHREAD_TESTCANCEL
			HAVE_PTHREAD_COND_INIT HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL
			HAVE_PTHREAD_MUTEXATTR_SETTYPE HAVE_CFMAKERAW
			HAVE_NANOSLEEP HAVE_CLOCK_GETTIME HAVE_CLOCK_NANOSLEEP
			HAVE_GETPAGESIZE HAVE_SIGACTION
			USE_SDL USE_SDL_VIDEO USE_SDL_AUDIO HAVE_SLIRP
			HAVE_WIN32_VM HAVE_WIN32_EXCEPTIONS HAVE_SIGSEGV_SKIP_INSTRUCTION
			HAVE_SIGINFO_T HAVE_SIGCONTEXT_SUBTERFUGE
			ENABLE_VOSF BINCUE)
		if(DEFINED ${HOST_PROBE_VAR})
			set(${HOST_PROBE_VAR} "${${HOST_PROBE_VAR}}" PARENT_SCOPE)
		endif()
	endforeach()
endfunction()

# ---------------------------------------------------------------------------
# SDL
# ---------------------------------------------------------------------------
function(macemu_find_sdl)
	if(APPLE)
		macemu_apple_find_sdl()
		set(MACEMU_SDL_TARGET "${MACEMU_SDL_TARGET}" PARENT_SCOPE)
		set(MACEMU_SDL_INCLUDE_TARGET "${MACEMU_SDL_INCLUDE_TARGET}" PARENT_SCOPE)
		set(MACEMU_SDL_XCFRAMEWORK "${MACEMU_SDL_XCFRAMEWORK}" PARENT_SCOPE)
		set(MACEMU_SDL_SLICE "${MACEMU_SDL_SLICE}" PARENT_SCOPE)
		set(USE_SDL3 1 PARENT_SCOPE)
		return()
	endif()

	if(USE_SDL3)
		find_package(SDL3 QUIET CONFIG)
		if(NOT SDL3_FOUND)
			find_path(SDL3_INCLUDE_DIR SDL.h
				PATHS ENV SDL3DIR
				PATH_SUFFIXES include include/SDL3)
			find_library(SDL3_LIBRARY
				NAMES SDL3 SDL3-static
				PATHS ENV SDL3DIR
				PATH_SUFFIXES lib lib/x64 lib/x86)
			if(SDL3_INCLUDE_DIR AND SDL3_LIBRARY)
				set(SDL3_FOUND TRUE)
				if(NOT TARGET SDL3::SDL3)
					add_library(SDL3::SDL3 UNKNOWN IMPORTED)
					set_target_properties(SDL3::SDL3 PROPERTIES
						IMPORTED_LOCATION "${SDL3_LIBRARY}"
						INTERFACE_INCLUDE_DIRECTORIES "${SDL3_INCLUDE_DIR}")
				endif()
				message(STATUS "SDL3 find_path found")
				message(STATUS "SDL3_INCLUDE_DIR: ${SDL3_INCLUDE_DIR}")
				if(EXISTS "${SDL3_INCLUDE_DIR}/SDL3")
					set(SDL2_INCLUDE_DIR "${SDL3_INCLUDE_DIR}/SDL3" PARENT_SCOPE)
					message(STATUS "SDL3 new style include path")
				else()
					set(SDL2_INCLUDE_DIR "${SDL3_INCLUDE_DIR}" PARENT_SCOPE)
					message(STATUS "SDL3 old style include path")
				endif()
				set(SDL2_LIBRARY "${SDL3_LIBRARY}" PARENT_SCOPE)
			else()
				message(STATUS "--- SDL3 Detection Debug ---")
				message(STATUS "SDL3_INCLUDE_DIR: ${SDL3_INCLUDE_DIR}")
				message(STATUS "SDL3_LIBRARY:     ${SDL3_LIBRARY}")
				message(STATUS "ENV SDL3DIR:      $ENV{SDL3DIR}")
				message(STATUS "CMake SDL3_DIR:   ${SDL3_DIR}")
				message(FATAL_ERROR
					"SDL3 not found. Install SDL3 dev files or pass -DSDL3_DIR=... / set SDL3DIR")
			endif()
		else()
			if(TARGET SDL3::SDL3)
				set(MACEMU_SDL_TARGET SDL3::SDL3)
				message(STATUS "SDL3::SDL3 find_package found")
			elseif(TARGET SDL3::SDL3-static)
				set(MACEMU_SDL_TARGET SDL3::SDL3-static)
				message(STATUS "SDL3::SDL3-static find_package found")
			else()
				message(FATAL_ERROR "Bad SDL3 setup")
			endif()

			set(MACEMU_SDL_INCLUDE_TARGET SDL3::Headers PARENT_SCOPE)
			set(MACEMU_SDL_TARGET "${MACEMU_SDL_TARGET}" PARENT_SCOPE)

			get_target_property(SDL3_TARGETPROPERTY_INCLUDE ${MACEMU_SDL_TARGET}
				INTERFACE_INCLUDE_DIRECTORIES)
			get_target_property(SDL3_TARGETPROPERTY_LIBRARY ${MACEMU_SDL_TARGET}
				LOCATION)

			set(SDL3_INCLUDE_DIR "${SDL3_TARGETPROPERTY_INCLUDE}" PARENT_SCOPE)
			set(SDL3_LIBRARY "${SDL3_TARGETPROPERTY_LIBRARY}" PARENT_SCOPE)


			get_target_property(SDL3_TARGET_TYPE ${MACEMU_SDL_TARGET} TYPE)
			get_target_property(SDL3_TARGET_LINK_LIBRARIES ${MACEMU_SDL_TARGET} INTERFACE_LINK_LIBRARIES)
			get_target_property(SDL3_TARGET_INCLUDE_DIRECTORIES ${MACEMU_SDL_TARGET} INTERFACE_INCLUDE_DIRECTORIES)

			message(STATUS "SDL3 target: ${MACEMU_SDL_TARGET}")
			message(STATUS "SDL3 type:   ${SDL3_TARGET_TYPE}")
			message(STATUS "SDL3 links:  ${SDL3_TARGET_LINK_LIBRARIES}")
			message(STATUS "SDL3 incs:   ${SDL3_TARGET_INCLUDE_DIRECTORIES}")


			get_target_property(SDL3_IMPORTED_LOCATION SDL3::SDL3 IMPORTED_LOCATION)
			get_target_property(SDL3_IMPORTED_LOCATION_DEBUG SDL3::SDL3 IMPORTED_LOCATION_DEBUG)
			get_target_property(SDL3_IMPORTED_LOCATION_RELEASE SDL3::SDL3 IMPORTED_LOCATION_RELEASE)
			get_target_property(SDL3_IMPORTED_IMPLIB SDL3::SDL3 IMPORTED_IMPLIB)
			get_target_property(SDL3_IMPORTED_IMPLIB_DEBUG SDL3::SDL3 IMPORTED_IMPLIB_DEBUG)
			get_target_property(SDL3_IMPORTED_IMPLIB_RELEASE SDL3::SDL3 IMPORTED_IMPLIB_RELEASE)

			message(STATUS "location:        ${SDL3_IMPORTED_LOCATION}")
			message(STATUS "location debug:  ${SDL3_IMPORTED_LOCATION_DEBUG}")
			message(STATUS "location release:${SDL3_IMPORTED_LOCATION_RELEASE}")
			message(STATUS "implib:          ${SDL3_IMPORTED_IMPLIB}")
			message(STATUS "implib debug:    ${SDL3_IMPORTED_IMPLIB_DEBUG}")
			message(STATUS "implib release:  ${SDL3_IMPORTED_IMPLIB_RELEASE}")

		endif()
		message(STATUS "SDL3_INCLUDE_DIR: ${SDL3_INCLUDE_DIR}")
		message(STATUS "SDL3_LIBRARY:     ${SDL3_LIBRARY}")
		set(USE_SDL3 1 PARENT_SCOPE)
		return()
	endif()

	find_package(SDL2 CONFIG QUIET)
	if(NOT SDL2_FOUND AND DEFINED ENV{SDL2DIR})
		list(APPEND CMAKE_PREFIX_PATH "$ENV{SDL2DIR}")
		find_package(SDL2 CONFIG QUIET)
	endif()

	if(SDL2_FOUND)
		if(TARGET SDL2::SDL2)
			set(MACEMU_SDL_TARGET SDL2::SDL2 PARENT_SCOPE)
			set(MACEMU_SDL_INCLUDE_TARGET SDL2::SDL2 PARENT_SCOPE)
		elseif(TARGET SDL2::SDL2-static)
			set(MACEMU_SDL_TARGET SDL2::SDL2-static PARENT_SCOPE)
			set(MACEMU_SDL_INCLUDE_TARGET SDL2::SDL2-static PARENT_SCOPE)
		endif()
		if(TARGET SDL2::SDL2main)
			set(MACEMU_SDL_MAIN SDL2::SDL2main PARENT_SCOPE)
		endif()
	else()
		find_package(PkgConfig QUIET)
		if(PkgConfig_FOUND)
			pkg_check_modules(SDL2 REQUIRED sdl2)
			set(MACEMU_SDL_PKG 1 PARENT_SCOPE)
			set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}" PARENT_SCOPE)
			set(SDL2_LIBRARIES "${SDL2_LIBRARIES}" PARENT_SCOPE)
		else()
			find_path(SDL2_INCLUDE_DIR SDL.h
				PATHS ENV SDL2DIR
				PATH_SUFFIXES include include/SDL2)
			find_library(SDL2_LIBRARY NAMES SDL2
				PATHS ENV SDL2DIR
				PATH_SUFFIXES lib lib/x64 lib/x86)
			find_library(SDL2MAIN_LIBRARY NAMES SDL2main
				PATHS ENV SDL2DIR
						PATH_SUFFIXES lib lib/x64 lib/x86)
			if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
				message(FATAL_ERROR
					"SDL2 not found. Install SDL2 dev files or pass -DSDL2_DIR=... / set SDL2DIR")
			else()
				set(SDL2_INCLUDE_DIR "${SDL2_INCLUDE_DIR}" PARENT_SCOPE)
				set(SDL2_LIBRARY "${SDL2_LIBRARY}" PARENT_SCOPE)
				set(SDL2MAIN_LIBRARY "${SDL2MAIN_LIBRARY}" PARENT_SCOPE)
			endif()
		endif()
	endif()

	# <SDL2/SDL.h> (Linux) vs <SDL.h> (official VC zip)
	find_path(SDL2_NESTED_INCLUDE SDL2/SDL.h
		HINTS ${SDL2_INCLUDE_DIRS} ${SDL2_INCLUDE_DIR}
		PATHS ENV SDL2DIR
		PATH_SUFFIXES include)
	if(SDL2_NESTED_INCLUDE)
		set(USE_SDL2 1 PARENT_SCOPE)
	endif()
endfunction()

function(macemu_link_sdl EMULATOR_EXECUTABLE)
	if(APPLE)
		macemu_apple_link_sdl(${EMULATOR_EXECUTABLE})
		return()
	endif()
	if(MACEMU_SDL_MAIN)
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${MACEMU_SDL_MAIN})
	endif()
	if(MACEMU_SDL_TARGET)
		get_target_property(SDL_INCLUDE_INTERFACES ${MACEMU_SDL_INCLUDE_TARGET}
			INTERFACE_INCLUDE_DIRECTORIES)
		foreach(SDL_INCLUDE_INTERFACE IN LISTS SDL_INCLUDE_INTERFACES)
			if(EXISTS "${SDL_INCLUDE_INTERFACE}/SDL.h")
				target_include_directories(${EMULATOR_EXECUTABLE} PRIVATE
					"${SDL_INCLUDE_INTERFACE}")
				message(STATUS "SDL final include: SDL.h")
				break()
			elseif(EXISTS "${SDL_INCLUDE_INTERFACE}/SDL2/SDL.h")
				target_include_directories(${EMULATOR_EXECUTABLE} PRIVATE
					"${SDL_INCLUDE_INTERFACE}/SDL2")
				message(STATUS "SDL final include: SDL2/SDL.h")
				break()
			elseif(EXISTS "${SDL_INCLUDE_INTERFACE}/SDL3/SDL.h")
				target_include_directories(${EMULATOR_EXECUTABLE} PRIVATE
					"${SDL_INCLUDE_INTERFACE}/SDL3")
				message(STATUS "SDL final include: SDL3/SDL.h")
				break()
			endif()
		endforeach()
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${MACEMU_SDL_TARGET})
		if(WIN32)
			get_target_property(MACEMU_SDL_TARGET_TYPE ${MACEMU_SDL_TARGET} TYPE)
			if(MACEMU_SDL_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
				 MACEMU_SDL_TARGET_TYPE STREQUAL "MODULE_LIBRARY")
				add_custom_command(TARGET ${EMULATOR_EXECUTABLE} POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
									"$<TARGET_FILE:${MACEMU_SDL_TARGET}>"
									"$<TARGET_FILE_DIR:${EMULATOR_EXECUTABLE}>"
					COMMENT "Copy SDL runtime next to ${EMULATOR_EXECUTABLE}"
					VERBATIM)
			endif()
		endif()
	elseif(MACEMU_SDL_PKG)
		target_include_directories(${EMULATOR_EXECUTABLE} PRIVATE ${SDL2_INCLUDE_DIRS})
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${SDL2_LIBRARIES})
	else()
		target_include_directories(${EMULATOR_EXECUTABLE} PRIVATE ${SDL2_INCLUDE_DIR})
		if(SDL2MAIN_LIBRARY)
			target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${SDL2MAIN_LIBRARY})
		endif()
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${SDL2_LIBRARY})
	endif()
endfunction()

# ---------------------------------------------------------------------------
# config.h generation
# ---------------------------------------------------------------------------
function(macemu_write_config CONFIG_HEADER_DIR MACEMU_PACKAGE MACEMU_TARNAME MACEMU_VERSION MACEMU_BUGREPORT)
	configure_file(
		"${MACEMU_CMAKE_DIR}/config.h.in"
		"${CONFIG_HEADER_DIR}/config.h"
		@ONLY
	)
endfunction()

# ---------------------------------------------------------------------------
# Shared source lists (live under BasiliskII/src, reused by SheepShaver)
# ---------------------------------------------------------------------------
function(macemu_slirp_sources BASILISKII_SRC SLIRP_SOURCES_OUT)
	set(SLIRP_SOURCES
		"${BASILISKII_SRC}/slirp/bootp.c"
		"${BASILISKII_SRC}/slirp/cksum.c"
		"${BASILISKII_SRC}/slirp/debug.c"
		"${BASILISKII_SRC}/slirp/if.c"
		"${BASILISKII_SRC}/slirp/ip_icmp.c"
		"${BASILISKII_SRC}/slirp/ip_input.c"
		"${BASILISKII_SRC}/slirp/ip_output.c"
		"${BASILISKII_SRC}/slirp/mbuf.c"
		"${BASILISKII_SRC}/slirp/misc.c"
		"${BASILISKII_SRC}/slirp/sbuf.c"
		"${BASILISKII_SRC}/slirp/slirp.c"
		"${BASILISKII_SRC}/slirp/socket.c"
		"${BASILISKII_SRC}/slirp/tcp_input.c"
		"${BASILISKII_SRC}/slirp/tcp_output.c"
		"${BASILISKII_SRC}/slirp/tcp_subr.c"
		"${BASILISKII_SRC}/slirp/tcp_timer.c"
		"${BASILISKII_SRC}/slirp/tftp.c"
		"${BASILISKII_SRC}/slirp/udp.c"
	)
	set(${SLIRP_SOURCES_OUT} "${SLIRP_SOURCES}" PARENT_SCOPE)
endfunction()

function(macemu_sdl_sources BASILISKII_SRC SDL_SOURCES_OUT)
	set(SDL_SOURCES
		"${BASILISKII_SRC}/SDL/video_sdl.cpp"
		"${BASILISKII_SRC}/SDL/video_sdl2.cpp"
		"${BASILISKII_SRC}/SDL/video_sdl3.cpp"
		"${BASILISKII_SRC}/SDL/audio_sdl.cpp"
		"${BASILISKII_SRC}/SDL/audio_sdl3.cpp"
	)
	set(${SDL_SOURCES_OUT} "${SDL_SOURCES}" PARENT_SCOPE)
endfunction()

function(macemu_xplat_sources BASILISKII_SRC XPLAT_SOURCES_OUT)
	# Prefer paths under a sibling tree when callers pass them via
	# macemu_resolve_path; this helper alone always uses BasiliskII.
	set(XPLAT_SOURCES
		"${BASILISKII_SRC}/CrossPlatform/vm_alloc.cpp"
		"${BASILISKII_SRC}/CrossPlatform/sigsegv.cpp"
		"${BASILISKII_SRC}/CrossPlatform/video_blit.cpp"
	)
	set(${XPLAT_SOURCES_OUT} "${XPLAT_SOURCES}" PARENT_SCOPE)
endfunction()

# Resolve CrossPlatform TUs the same way as other shared SheepShaver sources:
# SheepShaver/src/CrossPlatform/<file> may be a text stub ("../../../BasiliskII/...")
# or a real override. Never compile a stale full copy the IDE shows while the
# build silently uses BasiliskII - resolve_path picks the real file.
function(macemu_xplat_sources_resolved SHEEPSHAVER_SRC BASILISKII_SRC XPLAT_SOURCES_OUT)
	set(XPLAT_SOURCES)
	foreach(XPLAT_FILENAME vm_alloc.cpp sigsegv.cpp video_blit.cpp)
		macemu_resolve_path(
			"${SHEEPSHAVER_SRC}/CrossPlatform/${XPLAT_FILENAME}"
			"${BASILISKII_SRC}/CrossPlatform/${XPLAT_FILENAME}"
			RESOLVED_XPLAT_PATH)
		list(APPEND XPLAT_SOURCES "${RESOLVED_XPLAT_PATH}")
	endforeach()
	set(${XPLAT_SOURCES_OUT} "${XPLAT_SOURCES}" PARENT_SCOPE)
endfunction()

# Warn (or fail) when a SheepShaver path looks like a full source file while the
# build actually compiles the BasiliskII twin - classic "MSVC didn't pick up my
# edit" trap (text stubs are OK; large non-stub files are not).
function(macemu_check_shared_source_traps SHEEPSHAVER_SRC BASILISKII_SRC)
	set(SHARED_SOURCE_TRAP_RELPATHS
		CrossPlatform/sigsegv.cpp
		CrossPlatform/vm_alloc.cpp
		CrossPlatform/video_blit.cpp
		bincue.cpp
		cdrom.cpp
		disk.cpp
		prefs.cpp
		Windows/sys_windows.cpp
		Windows/clip_windows.cpp
		Windows/posix_emu.cpp
	)
	set(SHARED_SOURCE_TRAPS)
	foreach(SHARED_SOURCE_RELPATH IN LISTS SHARED_SOURCE_TRAP_RELPATHS)
		set(SHEEPSHAVER_SHARED_PATH "${SHEEPSHAVER_SRC}/${SHARED_SOURCE_RELPATH}")
		set(BASILISKII_SHARED_PATH "${BASILISKII_SRC}/${SHARED_SOURCE_RELPATH}")
		if(EXISTS "${SHEEPSHAVER_SHARED_PATH}" AND EXISTS "${BASILISKII_SHARED_PATH}"
			AND NOT IS_DIRECTORY "${SHEEPSHAVER_SHARED_PATH}")
			file(SIZE "${SHEEPSHAVER_SHARED_PATH}" SHEEPSHAVER_SHARED_FILE_SIZE)
			if(SHEEPSHAVER_SHARED_FILE_SIZE GREATER_EQUAL 200)
				file(READ "${SHEEPSHAVER_SHARED_PATH}" SHEEPSHAVER_SHARED_FILE_HEAD LIMIT 200)
				string(STRIP "${SHEEPSHAVER_SHARED_FILE_HEAD}" SHEEPSHAVER_SHARED_FILE_HEAD)
				if(NOT SHEEPSHAVER_SHARED_FILE_HEAD MATCHES "\\.\\./")
					# Full local copy. If it differs from B2, edits to either side confuse.
					file(SHA256 "${SHEEPSHAVER_SHARED_PATH}" SHEEPSHAVER_SHARED_SHA256)
					file(SHA256 "${BASILISKII_SHARED_PATH}" BASILISKII_SHARED_SHA256)
					if(NOT SHEEPSHAVER_SHARED_SHA256 STREQUAL BASILISKII_SHARED_SHA256)
						list(APPEND SHARED_SOURCE_TRAPS
							"${SHEEPSHAVER_SHARED_PATH} (differs from ${BASILISKII_SHARED_PATH})")
					endif()
				endif()
			endif()
		endif()
	endforeach()
	if(SHARED_SOURCE_TRAPS)
		message(WARNING
			"Shared sources under SheepShaver differ from BasiliskII twins.\n"
			"CMake may compile one path while you edit the other in the IDE:\n"
			"    ${SHARED_SOURCE_TRAPS}\n"
			"Prefer a text stub (relative path to BasiliskII) unless this is an intentional SS-only override "
			"listed explicitly in SheepShaver/CMakeLists.txt.")
	endif()
endfunction()

# Windows router / ether / cdenable (shared between both emulators)
function(macemu_windows_net_sources BASILISKII_SRC WINDOWS_NET_SOURCES_OUT)
	set(WINDOWS_NET_SOURCES
		"${BASILISKII_SRC}/Windows/cdenable/cache.cpp"
		"${BASILISKII_SRC}/Windows/cdenable/eject_nt.cpp"
		"${BASILISKII_SRC}/Windows/b2ether/packet32.cpp"
		"${BASILISKII_SRC}/Windows/router/arp.cpp"
		"${BASILISKII_SRC}/Windows/router/dump.cpp"
		"${BASILISKII_SRC}/Windows/router/dynsockets.cpp"
		"${BASILISKII_SRC}/Windows/router/ftp.cpp"
		"${BASILISKII_SRC}/Windows/router/icmp.cpp"
		"${BASILISKII_SRC}/Windows/router/iphelp.cpp"
		"${BASILISKII_SRC}/Windows/router/ipsocket.cpp"
		"${BASILISKII_SRC}/Windows/router/mib/interfaces.cpp"
		"${BASILISKII_SRC}/Windows/router/mib/mibaccess.cpp"
		"${BASILISKII_SRC}/Windows/router/router.cpp"
		"${BASILISKII_SRC}/Windows/router/tcp.cpp"
		"${BASILISKII_SRC}/Windows/router/udp.cpp"
	)
	if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		list(APPEND WINDOWS_NET_SOURCES "${BASILISKII_SRC}/Windows/cdenable/ntcd.cpp")
	endif()
	set(${WINDOWS_NET_SOURCES_OUT} "${WINDOWS_NET_SOURCES}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Common compile / link settings for an emulator executable
# ---------------------------------------------------------------------------
function(macemu_apply_common EMULATOR_EXECUTABLE)
	target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
		HAVE_CONFIG_H
		_REENTRANT
		DIRECT_ADDRESSING
	)
	if(NOT WIN32)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE _GNU_SOURCE)
	endif()

	if(WIN32)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
			WIN32
			_WINDOWS
			NOMINMAX
			_CRT_SECURE_NO_WARNINGS
			_CRT_NONSTDC_NO_WARNINGS
			_WIN32_WINNT=0x0601
			WINVER=0x0601
		)
	endif()

	# MSVC compatibility for POSIX-ish code (slirp, fcntl flags, strdup, alloca)
	if(MSVC)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
			__STDC__
			_CRT_DECLARE_NONSTDC_NAMES=1
		)
		# strdup / alloca live under different names in the UCRT
		target_compile_options(${EMULATOR_EXECUTABLE} PRIVATE
			"/FImalloc.h"
		)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
			"strdup=_strdup"
			"alloca=_alloca"
		)
	endif()

	if(ENABLE_BINCUE)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE BINCUE)
	endif()

	# GCC-style asm flags only on non-MSVC (MSVC uses MSVC_INTRINSICS instead)
	if(NOT MSVC)
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
			target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE X86_64_ASSEMBLY OPTIMIZED_FLAGS)
		elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86|X86")
			target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE X86_ASSEMBLY OPTIMIZED_FLAGS SAHF_SETO_PROFITABLE)
		endif()
	elseif(WIN32)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
			MSVC_INTRINSICS
			OPTIMIZED_FLAGS
			SAHF_SETO_PROFITABLE
			UNALIGNED_PROFITABLE
		)
	endif()

	if(USE_SDL3)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE USE_SDL3=1)
	elseif(DEFINED USE_SDL2 AND USE_SDL2)
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE USE_SDL2=1)
	endif()

	macemu_link_sdl(${EMULATOR_EXECUTABLE})

	if(WIN32)
		target_compile_options(${EMULATOR_EXECUTABLE} PRIVATE /D__WIN32__)
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ws2_32 iphlpapi winmm)
		if(MSVC)
			set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES WIN32_EXECUTABLE TRUE)
			target_compile_options(${EMULATOR_EXECUTABLE} PRIVATE /bigobj /wd4102 /wd4244 /wd4267 /wd4996 /Zc:__STDC__)
		endif()
	else()
		find_package(Threads REQUIRED)
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE Threads::Threads)
		if(UNIX AND NOT APPLE)
			target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE dl m)
		endif()
	endif()
endfunction()

# ---------------------------------------------------------------------------
# Resolve "text symlink" paths used by SheepShaver checkouts on Windows
# ---------------------------------------------------------------------------
function(macemu_resolve_path PREFERRED_PATH FALLBACK_PATH RESOLVED_PATH_OUT)
	if(EXISTS "${PREFERRED_PATH}" AND NOT IS_DIRECTORY "${PREFERRED_PATH}")
		file(SIZE "${PREFERRED_PATH}" PREFERRED_FILE_SIZE)
		if(PREFERRED_FILE_SIZE LESS 200)
			file(READ "${PREFERRED_PATH}" PREFERRED_FILE_CONTENT)
			string(STRIP "${PREFERRED_FILE_CONTENT}" PREFERRED_FILE_CONTENT)
			if(PREFERRED_FILE_CONTENT MATCHES "\\.\\./")
				get_filename_component(PREFERRED_FILE_DIR "${PREFERRED_PATH}" DIRECTORY)
				get_filename_component(PREFERRED_RESOLVED_PATH
					"${PREFERRED_FILE_DIR}/${PREFERRED_FILE_CONTENT}" ABSOLUTE)
				if(EXISTS "${PREFERRED_RESOLVED_PATH}")
					set(${RESOLVED_PATH_OUT} "${PREFERRED_RESOLVED_PATH}" PARENT_SCOPE)
					return()
				endif()
			endif()
		endif()
	endif()
	if(EXISTS "${PREFERRED_PATH}")
		set(${RESOLVED_PATH_OUT} "${PREFERRED_PATH}" PARENT_SCOPE)
	elseif(EXISTS "${FALLBACK_PATH}")
		set(${RESOLVED_PATH_OUT} "${FALLBACK_PATH}" PARENT_SCOPE)
	else()
		message(FATAL_ERROR "Missing source: ${PREFERRED_PATH} (also tried ${FALLBACK_PATH})")
	endif()
endfunction()
