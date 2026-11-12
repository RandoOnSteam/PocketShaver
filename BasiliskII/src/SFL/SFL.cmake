set(SFLDIRECTORY "${BASILISKIISOURCE}/SFL")
if(APPLE)
	enable_language(OBJC)
endif()
set(SFLLIBRARIES "${BASILISKIISOURCE}/../../thirdparty/sfl")
option(SFL_USE_QT "Build the SFL frontend on Qt 6 instead of the native toolkit" OFF)
set(SFL_GTK_VERSION "3" CACHE STRING "GTK major version for the SFL frontend on Unix (2, 3 or 4)")
set(SFLGTKMODULE "gtk+-${SFL_GTK_VERSION}.0")
if(SFL_GTK_VERSION STREQUAL "4")
	set(SFLGTKMODULE "gtk4")
endif()

set(SFLPLATFORMSOURCES
	"${SFLDIRECTORY}/user_strings_sfl.cpp"
	"${SFLDIRECTORY}/main_sfl.cpp"
	"${SFLDIRECTORY}/audio_sfl.cpp"
	"${SFLDIRECTORY}/joy_sfl.cpp"
	"${SFLDIRECTORY}/timer_sfl.cpp"
	"${SFLDIRECTORY}/sys_sfl.cpp"
	"${SFLDIRECTORY}/prefs_sfl.cpp"
	"${SFLDIRECTORY}/xpram_sfl.cpp"
	"${SFLDIRECTORY}/clip_sfl.cpp"
	"${SFLDIRECTORY}/extfs_sfl.cpp"
	"${SFLDIRECTORY}/posix_sfl.cpp"
	"${SFLDIRECTORY}/sflroman.cpp"
	"${SFLDIRECTORY}/input_sfl.cpp"
	"${SFLDIRECTORY}/ether_sfl.cpp"
	"${BASILISKIISOURCE}/dummy/serial_dummy.cpp"
	"${BASILISKIISOURCE}/dummy/scsi_dummy.cpp"
	"${SFLDIRECTORY}/sflbootprefs.cpp"
)

function(sfl_configure_core APPLICATIONTARGET)
	target_compile_definitions(${APPLICATIONTARGET} PRIVATE HAVE_CONFIG_H)
	if(MSVC)
		target_compile_options(${APPLICATIONTARGET} PRIVATE /W0 "/FI${SFLDIRECTORY}/sysdeps.h")
	else()
		target_compile_options(${APPLICATIONTARGET} PRIVATE -w -fno-strict-aliasing -include "${SFLDIRECTORY}/sysdeps.h")
	endif()
endfunction()

function(sfl_add_application APPLICATIONTARGET APPLICATIONCREATOR APPLICATIONCORE)
	string(TOUPPER "${APPLICATIONTARGET}" APPLICATIONUPPER)
	set(SFLLIBRARYSOURCES
		"${SFLDIRECTORY}/sflimpl.c"
		"${SFLDIRECTORY}/sflui.c"
		"${SFLDIRECTORY}/sflbootgui.c"
		"${SFLDIRECTORY}/sflaudio.c"
		"${SFLDIRECTORY}/sflnet.c"
		"${SFLDIRECTORY}/sflsocket.c"
	)
	if(ENABLE_EMULATOR_MONITOR)
		list(APPEND SFLLIBRARYSOURCES "${SFLDIRECTORY}/sflmonitor.c")
	endif()

	if(SFL_USE_QT)
		set_source_files_properties(${SFLLIBRARYSOURCES} PROPERTIES LANGUAGE CXX)
	elseif(APPLE)
		set_source_files_properties(${SFLLIBRARYSOURCES} PROPERTIES LANGUAGE OBJC)
	endif()
	add_library(${APPLICATIONUPPER}SFLLIBRARIES OBJECT ${SFLLIBRARYSOURCES})
	target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE SFL_APP_NAME="${APPLICATIONTARGET}")
	target_include_directories(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE "${SFLLIBRARIES}" "${SFLDIRECTORY}")
	if(WIN32)
		target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE _CONSOLE)
	endif()
	if(APPLE)
		target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE OBJC_OLD_DISPATCH_PROTOTYPES=1)
	endif()
	if(ENABLE_EMULATOR_MONITOR)
		target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE ENABLE_EMULATOR_MONITOR=1)
	endif()
	if(MSVC)
		target_compile_options(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE /W0)
	else()
		target_compile_options(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE -w)
	endif()
	set(SFLUNIXMODULES x11 xfixes xrandr xinerama xxf86vm wayland-client wayland-cursor fontconfig dbus-1)
	if(SFL_USE_QT)
		find_package(Qt6 REQUIRED COMPONENTS Widgets OPTIONAL_COMPONENTS GuiPrivate)
		target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE WSQTOPT)
		set(SFLQTLIBRARIES Qt6::Widgets Qt6::GuiPrivate)
		if(UNIX AND NOT APPLE)
			foreach(SFLKDECOMPONENT CoreAddons I18n TextWidgets XmlGui ConfigWidgets StatusNotifierItem WidgetsAddons)
				find_package(KF6${SFLKDECOMPONENT} REQUIRED)
				list(APPEND SFLQTLIBRARIES KF6::${SFLKDECOMPONENT})
			endforeach()
		endif()
		target_link_libraries(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE ${SFLQTLIBRARIES})
	else()
		list(APPEND SFLUNIXMODULES ${SFLGTKMODULE})
		if(NOT SFL_GTK_VERSION STREQUAL "3")
			target_compile_definitions(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE WSGTK${SFL_GTK_VERSION}OPT)
		endif()
	endif()
	if(UNIX AND NOT APPLE)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(SFLGTK REQUIRED ${SFLUNIXMODULES})
		target_include_directories(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE ${SFLGTK_INCLUDE_DIRS})
		target_compile_options(${APPLICATIONUPPER}SFLLIBRARIES PRIVATE ${SFLGTK_CFLAGS_OTHER})
	endif()

	set(SFLSLIRPSOURCES
		"${BASILISKIISOURCE}/slirp/bootp.c"
		"${BASILISKIISOURCE}/slirp/cksum.c"
		"${BASILISKIISOURCE}/slirp/debug.c"
		"${BASILISKIISOURCE}/slirp/if.c"
		"${BASILISKIISOURCE}/slirp/ip_icmp.c"
		"${BASILISKIISOURCE}/slirp/ip_input.c"
		"${BASILISKIISOURCE}/slirp/ip_output.c"
		"${BASILISKIISOURCE}/slirp/mbuf.c"
		"${BASILISKIISOURCE}/slirp/misc.c"
		"${BASILISKIISOURCE}/slirp/sbuf.c"
		"${BASILISKIISOURCE}/slirp/slirp.c"
		"${BASILISKIISOURCE}/slirp/socket.c"
		"${BASILISKIISOURCE}/slirp/tcp_input.c"
		"${BASILISKIISOURCE}/slirp/tcp_output.c"
		"${BASILISKIISOURCE}/slirp/tcp_subr.c"
		"${BASILISKIISOURCE}/slirp/tcp_timer.c"
		"${BASILISKIISOURCE}/slirp/udp.c"
		"${SFLDIRECTORY}/sflslirp.c"
	)

	add_library(${APPLICATIONUPPER}SFLSLIRP OBJECT ${SFLSLIRPSOURCES})
	target_include_directories(${APPLICATIONUPPER}SFLSLIRP PRIVATE "${SFLDIRECTORY}" "${BASILISKIISOURCE}/slirp")
	if(MSVC)
		target_compile_options(${APPLICATIONUPPER}SFLSLIRP PRIVATE /W0)
	else()
		target_compile_options(${APPLICATIONUPPER}SFLSLIRP PRIVATE -w)
	endif()

	if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
		add_application(${APPLICATIONTARGET} "${SFLDIRECTORY}/sflcarbon.r" CREATOR "${APPLICATIONCREATOR}")
		target_sources(${APPLICATIONTARGET} PRIVATE $<TARGET_OBJECTS:${APPLICATIONCORE}> $<TARGET_OBJECTS:${APPLICATIONUPPER}SFLLIBRARIES> $<TARGET_OBJECTS:${APPLICATIONUPPER}SFLSLIRP>)
		set_target_properties(${APPLICATIONTARGET} PROPERTIES LINKER_LANGUAGE CXX)
	else()
		add_executable(${APPLICATIONTARGET} $<TARGET_OBJECTS:${APPLICATIONCORE}> $<TARGET_OBJECTS:${APPLICATIONUPPER}SFLLIBRARIES> $<TARGET_OBJECTS:${APPLICATIONUPPER}SFLSLIRP>)
	endif()
	if(WIN32)
		target_link_libraries(${APPLICATIONTARGET} PRIVATE user32 gdi32 winmm ole32 shell32 advapi32 comdlg32 comctl32 uuid ws2_32)
	endif()
	if(SFL_USE_QT)
		target_link_libraries(${APPLICATIONTARGET} PRIVATE ${SFLQTLIBRARIES})
	endif()
	if(UNIX AND NOT APPLE)
		target_link_libraries(${APPLICATIONTARGET} PRIVATE ${SFLGTK_LIBRARIES} dl pthread m)
	endif()
	if(APPLE)
		target_link_libraries(${APPLICATIONTARGET} PRIVATE "-framework Cocoa" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio" "-framework AVFoundation" "-framework IOKit" "-framework ForceFeedback")
		find_library(SFLMEDIAPLAYER MediaPlayer)
		if(SFLMEDIAPLAYER)
			target_link_libraries(${APPLICATIONTARGET} PRIVATE "-weak_framework MediaPlayer")
		endif()
	endif()
endfunction()
