# Apple CMake helpers (macOS / iOS / Mac Catalyst).
# This is the CMake build. It does not load or invoke any .xcodeproj.
#
#	 cmake -S . -B build && cmake --build build --target SheepShaver
#	 cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#		 -DCMAKE_OSX_SYSROOT=iphonesimulator && cmake --build build-ios --target PocketShaver
#	 cmake -S . -B build-catalyst -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.toolchain.cmake \
#		 && cmake --build build-catalyst --config Debug --target PocketShaver
#	 Metal compositor (PocketShaver.xcodeproj: SDL2 + video_sdl2.cpp):
#	   -DGFXACCEL_BACKEND=metal
#	 SDL-GPU compositor ("PocketShaver SDLGPU.xcodeproj": SDL3 + video_sdl3.cpp):
#	   -DUSE_SDLGPU=ON  (default)
#
# iOS/Catalyst + Swift currently need CMake's Xcode *generator* (cmake -G Xcode).
# That still is CMake: it writes its own build files; it does not use
# SheepShaver/src/MacOSX/*.xcodeproj.

if(CMAKE_VERSION VERSION_LESS 3.28)
	message(FATAL_ERROR
		"Apple builds need CMake 3.28+ for xcframework support (found ${CMAKE_VERSION})")
endif()

set(MACEMU_APPLE_MACOS_DEPLOYMENT_TARGET "12.1")
set(MACEMU_APPLE_IOS_DEPLOYMENT_TARGET "15.2")
set(MACEMU_APPLE_BUNDLE_ID_SHEEPSHAVER "net.cebix.sheepshaver")
set(MACEMU_APPLE_BUNDLE_ID_POCKETSHAVER "com.carbjo.pocketshaver")

# ---------------------------------------------------------------------------
# Which Apple apps to build
# ---------------------------------------------------------------------------
function(macemu_apple_init)
	if(NOT APPLE)
		return()
	endif()

	set(APPLE_SYSROOT "${CMAKE_OSX_SYSROOT}")
	set(APPLE_PLATFORM "${MACEMU_APPLE_TARGET}")
	if(NOT APPLE_PLATFORM)
		if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
			if(APPLE_SYSROOT MATCHES "MacOSX" OR APPLE_SYSROOT STREQUAL "macosx")
				set(APPLE_PLATFORM catalyst)
			else()
				set(APPLE_PLATFORM ios)
			endif()
		else()
			set(APPLE_PLATFORM macos)
		endif()
		set(MACEMU_APPLE_TARGET "${APPLE_PLATFORM}" CACHE STRING
			"Apple target (macos|ios|catalyst)")
	endif()
	set_property(CACHE MACEMU_APPLE_TARGET PROPERTY STRINGS macos ios catalyst)

	set(APPLE_XCODE_GENERATOR FALSE)
	if(CMAKE_GENERATOR STREQUAL "Xcode")
		set(APPLE_XCODE_GENERATOR TRUE)
	endif()
	set(APPLE_IOS_CMAKE_SYSTEM FALSE)
	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		set(APPLE_IOS_CMAKE_SYSTEM TRUE)
	endif()

	# One CMake configure = one Apple platform.
	#   macos     -> SheepShaver
	#   ios       -> PocketShaver (iPhone / iPad / simulator)
	#   catalyst  -> PocketShaver (Mac Catalyst)
	set(BUILD_SHEEPSHAVER_APP FALSE)
	set(BUILD_POCKETSHAVER_APP FALSE)
	if(APPLE_IOS_CMAKE_SYSTEM OR MACEMU_APPLE_TARGET STREQUAL "ios" OR MACEMU_APPLE_TARGET STREQUAL "catalyst")
		set(BUILD_POCKETSHAVER_APP TRUE)
	else()
		set(BUILD_SHEEPSHAVER_APP TRUE)
	endif()

	if(BUILD_POCKETSHAVER_APP AND NOT APPLE_IOS_CMAKE_SYSTEM)
		message(FATAL_ERROR
			"iOS and Mac Catalyst builds need a CMake toolchain:\n"
			"  cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DCMAKE_OSX_SYSROOT=iphonesimulator\n"
			"  cmake -S . -B build-catalyst -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.toolchain.cmake")
	endif()

	set(MACEMU_BUILD_DESKTOP_SHEEPSHAVER ${BUILD_SHEEPSHAVER_APP} PARENT_SCOPE)
	set(MACEMU_BUILD_POCKETSHAVER ${BUILD_POCKETSHAVER_APP} PARENT_SCOPE)
	set(MACEMU_APPLE_TARGET "${APPLE_PLATFORM}" PARENT_SCOPE)
	set(MACEMU_APPLE_XCODE ${APPLE_XCODE_GENERATOR} PARENT_SCOPE)
	if(APPLE_XCODE_GENERATOR)
		set(CMAKE_XCODE_GENERATE_SCHEME YES PARENT_SCOPE)
	endif()

	message(STATUS "Apple target        : ${APPLE_PLATFORM}")
	message(STATUS "  SheepShaver.app   : ${BUILD_SHEEPSHAVER_APP}")
	message(STATUS "  PocketShaver.app  : ${BUILD_POCKETSHAVER_APP}")
endfunction()

# Must be a macro: enable_language() inside a function does not export
# CMAKE_<LANG>_COMPILE_OBJECT to the calling directory.
macro(macemu_apple_enable_languages)
	if(APPLE)
		enable_language(OBJC)
		enable_language(OBJCXX)
		if(MACEMU_BUILD_POCKETSHAVER)
			enable_language(Swift)
			set(CMAKE_Swift_LANGUAGE_VERSION 5)
		endif()
		macemu_apple_setup_catalyst_xcodebuild()
	endif()
endmacro()

# After Swift is detected. cmake --build passes -target, so xcodebuild
# ignores -destination and builds iphoneos. Use the PocketShaver scheme
# only for this project's xcodeproj; compiler tests pass through.
function(macemu_apple_setup_catalyst_xcodebuild)
	if(NOT MACEMU_APPLE_TARGET STREQUAL "catalyst")
		return()
	endif()
	if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
		return()
	endif()
	find_program(MACEMU_APPLE_XCODEBUILD NAMES xcodebuild
		PATHS /usr/bin
			/Applications/Xcode.app/Contents/Developer/usr/bin
		NO_DEFAULT_PATH)
	if(NOT MACEMU_APPLE_XCODEBUILD)
		find_program(MACEMU_APPLE_XCODEBUILD NAMES xcodebuild)
	endif()
	if(NOT MACEMU_APPLE_XCODEBUILD)
		message(FATAL_ERROR "xcodebuild not found (needed for Mac Catalyst)")
	endif()
	set(MACEMU_CATALYST_XCODEBUILD "${CMAKE_BINARY_DIR}/cmake-xcodebuild-catalyst")
	file(WRITE "${MACEMU_CATALYST_XCODEBUILD}"
"#!/bin/bash
real=\"${MACEMU_APPLE_XCODEBUILD}\"
is_ps=
for a in \"\$@\"; do
	case \"\$a\" in
		*macemu.xcodeproj*) is_ps=1 ;;
	esac
done
if [[ -z \"\$is_ps\" ]]; then
	exec \"\$real\" \"\$@\"
fi
args=()
while [[ \$# -gt 0 ]]; do
	if [[ \"\$1\" == -target ]]; then
		shift
		[[ \$# -gt 0 ]] && shift
		continue
	fi
	args+=(\"\$1\")
	shift
done
exec \"\$real\" \"\${args[@]}\" -scheme PocketShaver -destination \"platform=macOS,variant=Mac Catalyst\"
")
	file(CHMOD "${MACEMU_CATALYST_XCODEBUILD}" PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
	set(CMAKE_MAKE_PROGRAM "${MACEMU_CATALYST_XCODEBUILD}" CACHE FILEPATH
		"xcodebuild helper that selects Mac Catalyst" FORCE)
endfunction()

# ---------------------------------------------------------------------------
# Vendored SDL2.xcframework (Metal / PocketShaver.xcodeproj) or
# SDL3.xcframework (SDL-GPU / PocketShaver SDLGPU.xcodeproj)
# ---------------------------------------------------------------------------
function(macemu_apple_sdl_xcframework SDL_XCFRAMEWORK_OUT SDL_FRAMEWORK_NAME_OUT)
	if(USE_SDL3)
		set(SDL_FRAMEWORK_NAME "SDL3")
	else()
		set(SDL_FRAMEWORK_NAME "SDL2")
	endif()
	set(SDL_XCFRAMEWORK_CANDIDATES
		"${CMAKE_SOURCE_DIR}/SheepShaver/src/MacOSX/${SDL_FRAMEWORK_NAME}.xcframework"
		"${CMAKE_SOURCE_DIR}/src/MacOSX/${SDL_FRAMEWORK_NAME}.xcframework")
	foreach(SDL_XCFRAMEWORK_CANDIDATE ${SDL_XCFRAMEWORK_CANDIDATES})
		if(EXISTS "${SDL_XCFRAMEWORK_CANDIDATE}/Info.plist")
			set(${SDL_XCFRAMEWORK_OUT} "${SDL_XCFRAMEWORK_CANDIDATE}" PARENT_SCOPE)
			set(${SDL_FRAMEWORK_NAME_OUT} "${SDL_FRAMEWORK_NAME}" PARENT_SCOPE)
			return()
		endif()
	endforeach()
	message(FATAL_ERROR
		"${SDL_FRAMEWORK_NAME}.xcframework not found (tried ${SDL_XCFRAMEWORK_CANDIDATES})")
endfunction()

function(macemu_apple_sdl_slice SDL_XCFRAMEWORK SDL_FRAMEWORK_NAME SDL_SLICE_DIR_OUT)
	set(SDL_SLICE_CANDIDATES)
	if(MACEMU_APPLE_TARGET STREQUAL "catalyst")
		list(APPEND SDL_SLICE_CANDIDATES "ios-arm64_x86_64-maccatalyst")
	endif()
	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		if(CMAKE_OSX_SYSROOT MATCHES "MacOSX" OR CMAKE_OSX_SYSROOT STREQUAL "macosx")
			list(APPEND SDL_SLICE_CANDIDATES "ios-arm64_x86_64-maccatalyst")
		elseif(CMAKE_OSX_SYSROOT MATCHES "iPhoneSimulator" OR CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
			list(APPEND SDL_SLICE_CANDIDATES
				"ios-arm64_x86_64-simulator"
				"ios-arm64_i386_x86_64-simulator")
		else()
			list(APPEND SDL_SLICE_CANDIDATES "ios-arm64")
		endif()
	else()
		list(APPEND SDL_SLICE_CANDIDATES "macos-arm64_x86_64")
	endif()
	list(REMOVE_DUPLICATES SDL_SLICE_CANDIDATES)
	foreach(SDL_SLICE_NAME ${SDL_SLICE_CANDIDATES})
		set(SDL_SLICE_DIR "${SDL_XCFRAMEWORK}/${SDL_SLICE_NAME}")
		if(EXISTS "${SDL_SLICE_DIR}/${SDL_FRAMEWORK_NAME}.framework")
			set(${SDL_SLICE_DIR_OUT} "${SDL_SLICE_DIR}" PARENT_SCOPE)
			return()
		endif()
	endforeach()
	message(FATAL_ERROR
		"${SDL_FRAMEWORK_NAME}.xcframework slice missing (tried ${SDL_SLICE_CANDIDATES} under ${SDL_XCFRAMEWORK})")
endfunction()

function(macemu_apple_find_sdl)
	macemu_apple_sdl_xcframework(SDL_XCFRAMEWORK SDL_FRAMEWORK_NAME)
	macemu_apple_sdl_slice("${SDL_XCFRAMEWORK}" "${SDL_FRAMEWORK_NAME}" SDL_SLICE_DIR)
	set(MACEMU_SDL_XCFRAMEWORK "${SDL_XCFRAMEWORK}" PARENT_SCOPE)
	set(MACEMU_SDL_SLICE "${SDL_SLICE_DIR}" PARENT_SCOPE)
	set(MACEMU_SDL_FRAMEWORK_NAME "${SDL_FRAMEWORK_NAME}" PARENT_SCOPE)

	set(SDL_IMPORTED_TARGET "${SDL_FRAMEWORK_NAME}::${SDL_FRAMEWORK_NAME}")
	set(SDL_HEADERS_TARGET "${SDL_FRAMEWORK_NAME}::Headers")
	set(SDL_FRAMEWORK_PATH "${SDL_SLICE_DIR}/${SDL_FRAMEWORK_NAME}.framework")
	if(NOT TARGET ${SDL_HEADERS_TARGET})
		add_library(${SDL_HEADERS_TARGET} INTERFACE IMPORTED)
		set_target_properties(${SDL_HEADERS_TARGET} PROPERTIES
			INTERFACE_COMPILE_OPTIONS "SHELL:-F \"${SDL_SLICE_DIR}\"")
	endif()
	if(NOT TARGET ${SDL_IMPORTED_TARGET})
		add_library(${SDL_IMPORTED_TARGET} SHARED IMPORTED)
		set_target_properties(${SDL_IMPORTED_TARGET} PROPERTIES
			FRAMEWORK TRUE
			IMPORTED_LOCATION "${SDL_FRAMEWORK_PATH}/${SDL_FRAMEWORK_NAME}"
			INTERFACE_LINK_LIBRARIES ${SDL_HEADERS_TARGET})
	endif()
	set(MACEMU_SDL_TARGET ${SDL_IMPORTED_TARGET} PARENT_SCOPE)
	set(MACEMU_SDL_INCLUDE_TARGET ${SDL_HEADERS_TARGET} PARENT_SCOPE)
	if(USE_SDL3)
		set(USE_SDL3 1 PARENT_SCOPE)
	else()
		set(USE_SDL3 0 PARENT_SCOPE)
		set(USE_SDL2 1 PARENT_SCOPE)
	endif()
	message(STATUS "${SDL_FRAMEWORK_NAME} xcframework    : ${SDL_XCFRAMEWORK}")
	message(STATUS "${SDL_FRAMEWORK_NAME} slice          : ${SDL_SLICE_DIR}")
endfunction()

function(macemu_apple_link_sdl EMULATOR_EXECUTABLE)
	if(NOT MACEMU_SDL_XCFRAMEWORK)
		return()
	endif()
	# Xcode consumes the xcframework and picks the right slice per SDK,
	# which is what PocketShaver needs (iphoneos / simulator / macabi).
	if(CMAKE_GENERATOR STREQUAL "Xcode")
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE "${MACEMU_SDL_XCFRAMEWORK}")
		set_property(TARGET ${EMULATOR_EXECUTABLE} APPEND PROPERTY
			XCODE_EMBED_FRAMEWORKS "${MACEMU_SDL_XCFRAMEWORK}")
		set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
			XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
			XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY YES
			XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS
				"$(inherited) @executable_path/Frameworks @executable_path/../Frameworks")
	else()
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE ${MACEMU_SDL_TARGET})
		target_link_options(${EMULATOR_EXECUTABLE} PRIVATE
			"SHELL:-F ${MACEMU_SDL_SLICE}"
			LINKER:-rpath,@executable_path/../Frameworks)
		add_custom_command(TARGET ${EMULATOR_EXECUTABLE} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory
				"$<TARGET_BUNDLE_DIR:${EMULATOR_EXECUTABLE}>/Contents/Frameworks"
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${MACEMU_SDL_SLICE}/${MACEMU_SDL_FRAMEWORK_NAME}.framework"
				"$<TARGET_BUNDLE_DIR:${EMULATOR_EXECUTABLE}>/Contents/Frameworks/${MACEMU_SDL_FRAMEWORK_NAME}.framework"
			COMMENT "Embed ${MACEMU_SDL_FRAMEWORK_NAME}.framework in ${EMULATOR_EXECUTABLE}"
			VERBATIM)
	endif()
endfunction()

# ---------------------------------------------------------------------------
# Platform sources (SheepShaver/CMakeLists.txt calls these)
# ---------------------------------------------------------------------------
function(macemu_apple_common_sources SHEEPSHAVER_SRC BASILISKII_SRC APPLE_COMMON_SOURCES_OUT)
	set(APPLE_COMMON_SOURCES
		"${SHEEPSHAVER_SRC}/Unix/main_unix.cpp"
		"${SHEEPSHAVER_SRC}/Unix/about_window_unix.cpp"
		"${SHEEPSHAVER_SRC}/Unix/prefs_unix.cpp"
		"${SHEEPSHAVER_SRC}/Unix/user_strings_unix.cpp"
		"${BASILISKII_SRC}/Unix/sys_unix.cpp"
		"${BASILISKII_SRC}/Unix/xpram_unix.cpp"
		"${BASILISKII_SRC}/Unix/timer_unix.cpp"
		"${BASILISKII_SRC}/Unix/serial_unix.cpp"
		"${BASILISKII_SRC}/Unix/ether_unix.cpp"
		"${BASILISKII_SRC}/Unix/rpc_unix.cpp"
		"${BASILISKII_SRC}/Unix/sshpty.c"
		"${BASILISKII_SRC}/Unix/strlcpy.c"
		"${BASILISKII_SRC}/Unix/tinyxml2.cpp"
		"${BASILISKII_SRC}/Unix/disk_sparsebundle.cpp"
		"${BASILISKII_SRC}/dummy/scsi_dummy.cpp"
		"${BASILISKII_SRC}/dummy/prefs_editor_dummy.cpp"
		"${BASILISKII_SRC}/MacOSX/extfs_macosx.cpp"
		"${BASILISKII_SRC}/MacOSX/runtool.c"
		"${BASILISKII_SRC}/SDL/SDLMain.m"
		"${SHEEPSHAVER_SRC}/pict.c"
		"${SHEEPSHAVER_SRC}/MacOSX/Launcher/DiskType.m"
	)
	# B2 Unix TUs include "sysdeps.h" and the compiler searches the file's
	# own directory first (BasiliskII/src/Unix). Force SheepShaver's copy.
	set(SHEEPSHAVER_UNIX_SYSDEPS "${SHEEPSHAVER_SRC}/Unix/sysdeps.h")
	set(BASILISKII_UNIX_SOURCES
		"${BASILISKII_SRC}/Unix/sys_unix.cpp"
		"${BASILISKII_SRC}/Unix/xpram_unix.cpp"
		"${BASILISKII_SRC}/Unix/timer_unix.cpp"
		"${BASILISKII_SRC}/Unix/serial_unix.cpp"
		"${BASILISKII_SRC}/Unix/ether_unix.cpp"
		"${BASILISKII_SRC}/Unix/rpc_unix.cpp"
		"${BASILISKII_SRC}/Unix/sshpty.c"
		"${BASILISKII_SRC}/Unix/strlcpy.c"
		"${BASILISKII_SRC}/Unix/tinyxml2.cpp"
		"${BASILISKII_SRC}/Unix/disk_sparsebundle.cpp"
		"${BASILISKII_SRC}/MacOSX/extfs_macosx.cpp"
		"${BASILISKII_SRC}/MacOSX/clip_macosx64.mm"
		"${BASILISKII_SRC}/MacOSX/sys_darwin.cpp"
		"${BASILISKII_SRC}/MacOSX/utils_macosx.mm"
		"${BASILISKII_SRC}/SDL/SDLMain.m"
	)
	set_source_files_properties(${BASILISKII_UNIX_SOURCES} PROPERTIES
		COMPILE_OPTIONS "-include;${SHEEPSHAVER_UNIX_SYSDEPS}"
		OBJECT_DEPENDS "${SHEEPSHAVER_UNIX_SYSDEPS}")
	set(${APPLE_COMMON_SOURCES_OUT} "${APPLE_COMMON_SOURCES}" PARENT_SCOPE)
endfunction()

function(macemu_apple_macos_sources SHEEPSHAVER_SRC BASILISKII_SRC APPLE_MACOS_SOURCES_OUT)
	set(APPLE_MACOS_SOURCES
		"${BASILISKII_SRC}/MacOSX/clip_macosx64.mm"
		"${BASILISKII_SRC}/MacOSX/sys_darwin.cpp"
		"${BASILISKII_SRC}/MacOSX/utils_macosx.mm"
		"${SHEEPSHAVER_SRC}/MacOSX/prefs_macosx.mm"
		"${SHEEPSHAVER_SRC}/MacOSX/Launcher/VMSettingsController.mm"
	)
	set(${APPLE_MACOS_SOURCES_OUT} "${APPLE_MACOS_SOURCES}" PARENT_SCOPE)
endfunction()

function(macemu_apple_pocketshaver_sources SHEEPSHAVER_SRC POCKETSHAVER_SOURCES_OUT POCKETSHAVER_RESOURCES_OUT)
	set(SHEEPSHAVER_MACOSX_DIR "${SHEEPSHAVER_SRC}/MacOSX")
	file(GLOB_RECURSE POCKETSHAVER_SWIFT_SOURCES CONFIGURE_DEPENDS
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/*.swift"
		"${SHEEPSHAVER_MACOSX_DIR}/Bonjour/*.swift")
	# Two folders share the same Swift filenames; compile the Bootstrap copies.
	list(FILTER POCKETSHAVER_SWIFT_SOURCES EXCLUDE REGEX "Setup Instructions/Compatibility list/")
	file(GLOB_RECURSE POCKETSHAVER_OBJC_SOURCES CONFIGURE_DEPENDS
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/*.mm"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/*.m"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/*.cpp"
		"${SHEEPSHAVER_MACOSX_DIR}/impluse-hfs/*.m")
	set(POCKETSHAVER_SOURCES ${POCKETSHAVER_SWIFT_SOURCES} ${POCKETSHAVER_OBJC_SOURCES})

	set(POCKETSHAVER_RESOURCES
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Swift/Resources/Assets.xcassets"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Swift/Resources/Colors.xcassets"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Swift/Resources/Fonts.xcassets"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Localizable.strings"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/sheepicon.png"
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Readme iOS build.rtf"
		"${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.icns"
	)
	if(NOT GFXACCEL_BACKEND STREQUAL "metal")
		list(APPEND POCKETSHAVER_RESOURCES
			"${SHEEPSHAVER_SRC}/gfxaccel/sdlgpu/sdlgpu_ffp.hlsl")
	endif()
	file(GLOB_RECURSE POCKETSHAVER_HEADERS CONFIGURE_DEPENDS
		"${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/*.h"
		"${SHEEPSHAVER_MACOSX_DIR}/impluse-hfs/*.h"
		"${SHEEPSHAVER_MACOSX_DIR}/Bonjour/*.h")
	set(POCKETSHAVER_HEADER_DIRS)
	foreach(POCKETSHAVER_HEADER IN LISTS POCKETSHAVER_HEADERS)
		get_filename_component(POCKETSHAVER_HEADER_DIR "${POCKETSHAVER_HEADER}" DIRECTORY)
		list(APPEND POCKETSHAVER_HEADER_DIRS "${POCKETSHAVER_HEADER_DIR}")
	endforeach()
	list(REMOVE_DUPLICATES POCKETSHAVER_HEADER_DIRS)
	set(MACEMU_APPLE_POCKETSHAVER_HEADER_DIRS "${POCKETSHAVER_HEADER_DIRS}" PARENT_SCOPE)

	list(APPEND POCKETSHAVER_RESOURCES "${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/iOS Launch Screen.storyboard")
	set(${POCKETSHAVER_SOURCES_OUT} "${POCKETSHAVER_SOURCES}" PARENT_SCOPE)
	set(${POCKETSHAVER_RESOURCES_OUT} "${POCKETSHAVER_RESOURCES}" PARENT_SCOPE)
endfunction()

function(macemu_apple_include_dirs SHEEPSHAVER_SRC BASILISKII_SRC APPLE_INCLUDE_DIRS_OUT)
	set(APPLE_INCLUDE_DIRS
		"${SHEEPSHAVER_SRC}/MacOSX"
		"${SHEEPSHAVER_SRC}/MacOSX/Launcher"
		"${SHEEPSHAVER_SRC}/Unix"
		"${SHEEPSHAVER_SRC}/CrossPlatform"
		"${BASILISKII_SRC}/Unix"
		"${BASILISKII_SRC}/MacOSX"
		"${BASILISKII_SRC}/CrossPlatform"
	)
	set(${APPLE_INCLUDE_DIRS_OUT} "${APPLE_INCLUDE_DIRS}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Per-app bundle / SDK / frameworks
# ---------------------------------------------------------------------------
function(macemu_apple_apply_app EMULATOR_EXECUTABLE APPLE_APP_KIND)
	set(SHEEPSHAVER_MACOSX_DIR "${SS_SRC}/MacOSX")
	if(NOT IS_DIRECTORY "${SHEEPSHAVER_MACOSX_DIR}")
		set(SHEEPSHAVER_MACOSX_DIR "${CMAKE_SOURCE_DIR}/SheepShaver/src/MacOSX")
	endif()

	if(USE_SDL3)
		set(APPLE_SDL_COMPILE_DEFINITION USE_SDL3=1)
	else()
		set(APPLE_SDL_COMPILE_DEFINITION USE_SDL2=1)
	endif()
	target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE
		${APPLE_SDL_COMPILE_DEFINITION}
		_THREAD_SAFE
		ENABLE_MACOSX_ETHERHELPER
		DATADIR=""
		HAVE_MACH_VM=1
	)
	set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
		MACOSX_BUNDLE TRUE
		CXX_STANDARD 14
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS ON
		OBJCXX_STANDARD 14
		C_STANDARD 11
		C_STANDARD_REQUIRED ON
		XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
		XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "gnu++14"
		XCODE_ATTRIBUTE_GCC_C_LANGUAGE_STANDARD "gnu11"
		XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET
			"${MACEMU_APPLE_MACOS_DEPLOYMENT_TARGET}"
	)

	if(APPLE_APP_KIND STREQUAL "macos")
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE
			"-framework Cocoa"
			"-framework Carbon"
			"-framework IOKit"
			"-framework Security"
			"-framework Metal"
			"-framework Foundation"
		)
		set(SHEEPSHAVER_INFOPLIST_IN "${SHEEPSHAVER_MACOSX_DIR}/Info.plist.in")
		if(EXISTS "${SHEEPSHAVER_INFOPLIST_IN}")
			set(SHEEPSHAVER_INFOPLIST "${CMAKE_CURRENT_BINARY_DIR}/${EMULATOR_EXECUTABLE}-Info.plist")
			set(SHEEPSHAVER_BUNDLE_VERSION "2.5")
			configure_file("${SHEEPSHAVER_INFOPLIST_IN}" "${SHEEPSHAVER_INFOPLIST}" @ONLY)
			set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
				MACOSX_BUNDLE_INFO_PLIST "${SHEEPSHAVER_INFOPLIST}"
				MACOSX_BUNDLE_GUI_IDENTIFIER "${MACEMU_APPLE_BUNDLE_ID_SHEEPSHAVER}"
				MACOSX_BUNDLE_BUNDLE_NAME "SheepShaver"
				XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
					"${MACEMU_APPLE_BUNDLE_ID_SHEEPSHAVER}"
				XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
					"${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.entitlements"
				XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
				XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
				XCODE_ATTRIBUTE_AD_HOC_CODE_SIGNING_ALLOWED YES
				XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
				XCODE_ATTRIBUTE_INFOPLIST_PREPROCESS NO
			)
		endif()
		# 64-bit x86 needs a low pagezero so the Mac ROM can live at NULL.
		if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR
			 (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|i386|amd64"))
			target_link_options(${EMULATOR_EXECUTABLE} PRIVATE
				LINKER:-pagezero_size,0x3000)
		endif()
		if(EXISTS "${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.icns")
			target_sources(${EMULATOR_EXECUTABLE} PRIVATE "${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.icns")
			set_source_files_properties("${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.icns"
				PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
		endif()
		if(EXISTS "${SHEEPSHAVER_MACOSX_DIR}/etherhelpertool.c")
			set(ETHERHELPER_EXECUTABLE "${EMULATOR_EXECUTABLE}_etherhelper")
			add_executable(${ETHERHELPER_EXECUTABLE} "${SHEEPSHAVER_MACOSX_DIR}/etherhelpertool.c")
			target_link_libraries(${ETHERHELPER_EXECUTABLE} PRIVATE "-framework Security")
			set_target_properties(${ETHERHELPER_EXECUTABLE} PROPERTIES
				OUTPUT_NAME etherhelpertool
				RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
			add_dependencies(${EMULATOR_EXECUTABLE} ${ETHERHELPER_EXECUTABLE})
			add_custom_command(TARGET ${EMULATOR_EXECUTABLE} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:${ETHERHELPER_EXECUTABLE}>"
					"$<TARGET_BUNDLE_CONTENT_DIR:${EMULATOR_EXECUTABLE}>/Resources/etherhelpertool"
				VERBATIM)
		endif()
	else()
		# PocketShaver (iPhone / iPad / Mac Catalyst).
		target_compile_definitions(${EMULATOR_EXECUTABLE} PRIVATE KPX_IOS=1)
		target_compile_options(${EMULATOR_EXECUTABLE} PRIVATE
			$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
			$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
			$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-weak>
			$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-weak>
		)
		target_link_libraries(${EMULATOR_EXECUTABLE} PRIVATE
			"-framework UIKit"
			"-framework Foundation"
			"-framework Metal"
			"-framework AVFoundation"
			"-framework GameController"
			"-framework CoreAudioKit"
			"-framework CoreHaptics"
			"-framework AudioToolbox"
			"-framework Security"
			"-framework CoreMotion"
			"-framework MultipeerConnectivity"
			"-framework QuartzCore"
			resolv
		)
		set(POCKETSHAVER_BRIDGING_HEADER "${SHEEPSHAVER_MACOSX_DIR}/Supporting files/PocketShaver-Bridging-Header.h")
		set(POCKETSHAVER_INFOPLIST "${SHEEPSHAVER_MACOSX_DIR}/PocketShaver/Info.plist")
		set(POCKETSHAVER_IOS_ENTITLEMENTS "${SHEEPSHAVER_MACOSX_DIR}/SheepShaver.entitlements")
		set(POCKETSHAVER_CATALYST_ENTITLEMENTS "${SHEEPSHAVER_MACOSX_DIR}/PocketShaver-Catalyst.entitlements")
		set(POCKETSHAVER_SDKROOT "")
		if(APPLE_APP_KIND STREQUAL "catalyst")
			set(POCKETSHAVER_SDKROOT iphoneos)
		elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
			if(CMAKE_OSX_SYSROOT MATCHES "MacOSX" OR CMAKE_OSX_SYSROOT STREQUAL "macosx")
				set(POCKETSHAVER_SDKROOT macosx)
				set(APPLE_APP_KIND catalyst)
			elseif(CMAKE_OSX_SYSROOT MATCHES "iPhoneSimulator" OR CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
				set(POCKETSHAVER_SDKROOT iphonesimulator)
			endif()
			# Otherwise leave SDKROOT unset so the iOS toolchain / -sdk flag
			# selects iphoneos or iphonesimulator.
		endif()
		set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
			MACOSX_BUNDLE_INFO_PLIST "${POCKETSHAVER_INFOPLIST}"
			MACOSX_BUNDLE_GUI_IDENTIFIER "${MACEMU_APPLE_BUNDLE_ID_POCKETSHAVER}"
			MACOSX_BUNDLE_BUNDLE_NAME "PocketShaver"
			XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
				"${MACEMU_APPLE_BUNDLE_ID_POCKETSHAVER}"
			XCODE_ATTRIBUTE_PRODUCT_NAME "$(TARGET_NAME)"
			XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET
				"${MACEMU_APPLE_IOS_DEPLOYMENT_TARGET}"
			XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST YES
			XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
			XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator"
			XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
			XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${POCKETSHAVER_BRIDGING_HEADER}"
			XCODE_ATTRIBUTE_SWIFT_STRICT_CONCURRENCY "minimal"
			XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
			XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_WEAK YES
			XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES NO
			XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
			XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
			XCODE_ATTRIBUTE_AD_HOC_CODE_SIGNING_ALLOWED YES
			XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
			XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
			XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED YES
			XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${POCKETSHAVER_IOS_ENTITLEMENTS}"
			XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME SheepShaverAppIcon
			XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "3"
			XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
			XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS
				"$(inherited) @executable_path/Frameworks @executable_path/../Frameworks"
			XCODE_ATTRIBUTE_OTHER_LDFLAGS "$(inherited) -lresolv"
			XCODE_ATTRIBUTE_INFOPLIST_PREPROCESS NO
			XCODE_ATTRIBUTE_MTL_FAST_MATH YES
			LINKER_LANGUAGE Swift
		)
		if(GFXACCEL_BACKEND STREQUAL "metal")
			set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
				"XCODE_ATTRIBUTE_MTL_ENABLE_DEBUG_INFO[variant=Debug]" INCLUDE_SOURCE)
			set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
				"XCODE_ATTRIBUTE_MTL_ENABLE_DEBUG_INFO[variant=Release]" NO)
		endif()
		# CMake's Xcode/Swift support bakes the configure-time iPhoneOS SDK
		# into LIBRARY_SEARCH_PATHS. A Mac Catalyst destination then links
		# iPhoneOS libswift*.tbd (no x86_64; they steal -lswiftCore from
		# MacOSX.sdk). Always search the effective SDK's Swift libs first.
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS
			"$(SDKROOT)/usr/lib/swift $(inherited)")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=macosx*]"
			"$(SDKROOT)/usr/lib/swift $(SDKROOT)/System/iOSSupport/usr/lib/swift $(inherited)")
		# Same for frameworks: CMake bakes iPhoneOS.sdk/System/Library/Frameworks
		# into FRAMEWORK_SEARCH_PATHS, so Catalyst Swift scans iPhoneOS
		# Foundation. Let Xcode use the destination SDK.
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS "$(inherited)")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS[sdk=macosx*]"
			"$(SDKROOT)/System/iOSSupport/System/Library/Frameworks $(inherited)")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			XCODE_ATTRIBUTE_ARCHS "$(ARCHS_STANDARD)")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_ARCHS[sdk=macosx*]" "$(ARCHS_STANDARD)")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY[sdk=macosx*]" "2,6")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY[sdk=macosx*]" "-")
		set_property(TARGET ${EMULATOR_EXECUTABLE} PROPERTY
			"XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS[sdk=macosx*]" "${POCKETSHAVER_CATALYST_ENTITLEMENTS}")
		if(POCKETSHAVER_SDKROOT)
			set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
				XCODE_ATTRIBUTE_SDKROOT "${POCKETSHAVER_SDKROOT}")
		endif()
		if(APPLE_APP_KIND STREQUAL "catalyst")
			set_target_properties(${EMULATOR_EXECUTABLE} PROPERTIES
				XCODE_ATTRIBUTE_SDKROOT iphoneos
				XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator"
				XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST YES
				XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD NO
				XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
				XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
				XCODE_ATTRIBUTE_AD_HOC_CODE_SIGNING_ALLOWED YES
				XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
				XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${POCKETSHAVER_CATALYST_ENTITLEMENTS}"
			)
		endif()
	endif()

	# Ad-hoc sign the .app (and embedded frameworks). Same as
	# `codesign --force --deep -s - Foo.app`, so cmake --build is openable
	# even when xcodebuild is invoked with CODE_SIGNING_ALLOWED=NO.
	# CODESIGNING_FOLDER_PATH is Xcode's built-product path (includes
	# -maccatalyst). Do not use TARGET_BUNDLE_DIR: VERBATIM escapes the
	# EFFECTIVE_PLATFORM_NAME dollar so the suffix never expands.
	add_custom_command(TARGET ${EMULATOR_EXECUTABLE} POST_BUILD
		COMMAND /usr/bin/codesign --force --deep --sign - [=[${CODESIGNING_FOLDER_PATH}]=]
		COMMENT "Ad-hoc codesign ${EMULATOR_EXECUTABLE}.app")
endfunction()
