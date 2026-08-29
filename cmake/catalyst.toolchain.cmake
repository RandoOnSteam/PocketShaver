# Mac Catalyst toolchain for PocketShaver.
#
#	 cmake -S . -B build-catalyst -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.toolchain.cmake
#	 cmake --build build-catalyst --config Debug --target PocketShaver
#
# Do not set CMAKE_OSX_ARCHITECTURES here. This file is loaded before
# enable_language(Swift); an iOS x86_64 compiler test has no Swift.
# ARCHS_STANDARD + the Mac Catalyst destination selects x86_64 or arm64.
set(CMAKE_SYSTEM_NAME iOS)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "15.2" CACHE STRING
		"iOS deployment target used by Mac Catalyst")
endif()
set(CMAKE_MACOSX_BUNDLE YES)
set(CMAKE_XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST YES)
set(MACEMU_APPLE_TARGET "catalyst" CACHE STRING
	"Apple target (macos|ios|catalyst)")
