set(SFLDIR "${B2SRC}/SFL")
if(APPLE)
	enable_language(OBJC)
endif()
set(SFLLIBS "${B2SRC}/../../thirdparty/sfl")

set(SFLPLATFORM
	"${SFLDIR}/user_strings_sfl.cpp"
	"${SFLDIR}/main_sfl.cpp"
	"${SFLDIR}/audio_sfl.cpp"
	"${SFLDIR}/joy_sfl.cpp"
	"${SFLDIR}/timer_sfl.cpp"
	"${SFLDIR}/sys_sfl.cpp"
	"${SFLDIR}/prefs_sfl.cpp"
	"${SFLDIR}/xpram_sfl.cpp"
	"${SFLDIR}/clip_sfl.cpp"
	"${SFLDIR}/extfs_sfl.cpp"
	"${SFLDIR}/posix_sfl.cpp"
	"${SFLDIR}/sflroman.cpp"
	"${SFLDIR}/input_sfl.cpp"
	"${SFLDIR}/ether_sfl.cpp"
	"${B2SRC}/dummy/serial_dummy.cpp"
	"${B2SRC}/dummy/scsi_dummy.cpp"
	"${B2SRC}/dummy/prefs_editor_dummy.cpp"
)

function(sfl_configure_core target)
	target_compile_definitions(${target} PRIVATE HAVE_CONFIG_H)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W0 "/FI${SFLDIR}/sysdeps.h")
	else()
		target_compile_options(${target} PRIVATE -w -fno-strict-aliasing -include "${SFLDIR}/sysdeps.h")
	endif()
endfunction()

function(sfl_add_application target creator)
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

	if(APPLE)
		set_source_files_properties(${SFLLIBSOURCES} PROPERTIES LANGUAGE OBJC)
	endif()
	add_library(${target}_sfllibs OBJECT ${SFLLIBSOURCES})
	target_compile_definitions(${target}_sfllibs PRIVATE SFL_APP_NAME="${target}")
	target_include_directories(${target}_sfllibs PRIVATE "${SFLLIBS}" "${SFLDIR}")
	if(WIN32)
		target_compile_definitions(${target}_sfllibs PRIVATE _CONSOLE)
	endif()
	if(APPLE)
		target_compile_definitions(${target}_sfllibs PRIVATE OBJC_OLD_DISPATCH_PROTOTYPES=1)
	endif()
	if(SFL_MONITOR)
		target_compile_definitions(${target}_sfllibs PRIVATE SFLMONITORENABLED)
	endif()
	if(MSVC)
		target_compile_options(${target}_sfllibs PRIVATE /W0)
	else()
		target_compile_options(${target}_sfllibs PRIVATE -w)
	endif()
	if(UNIX AND NOT APPLE)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(SFLGTK REQUIRED gtk+-3.0 x11 xfixes xrandr xinerama xxf86vm wayland-client wayland-cursor fontconfig)
		target_include_directories(${target}_sfllibs PRIVATE ${SFLGTK_INCLUDE_DIRS})
		target_compile_options(${target}_sfllibs PRIVATE ${SFLGTK_CFLAGS_OTHER})
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

	add_library(${target}_sflslirp OBJECT ${SFLSLIRPSOURCES})
	target_include_directories(${target}_sflslirp PRIVATE "${SFLDIR}" "${B2SRC}/slirp")
	if(MSVC)
		target_compile_options(${target}_sflslirp PRIVATE /W0)
	else()
		target_compile_options(${target}_sflslirp PRIVATE -w)
	endif()

	if(CMAKE_SYSTEM_NAME MATCHES RetroCarbon)
		add_application(${target} "${SFLDIR}/sflcarbon.r" CREATOR "${creator}")
		target_sources(${target} PRIVATE $<TARGET_OBJECTS:${target}_sflcore> $<TARGET_OBJECTS:${target}_sfllibs> $<TARGET_OBJECTS:${target}_sflslirp>)
		set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
	else()
		add_executable(${target} $<TARGET_OBJECTS:${target}_sflcore> $<TARGET_OBJECTS:${target}_sfllibs> $<TARGET_OBJECTS:${target}_sflslirp>)
	endif()
	if(WIN32)
		target_link_libraries(${target} PRIVATE user32 gdi32 winmm ole32 shell32 advapi32 comdlg32 comctl32 uuid ws2_32)
	endif()
	if(UNIX AND NOT APPLE)
		target_link_libraries(${target} PRIVATE ${SFLGTK_LIBRARIES} dl pthread m)
	endif()
	if(APPLE)
		target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio" "-framework AVFoundation" "-framework IOKit" "-framework ForceFeedback")
	endif()
endfunction()
