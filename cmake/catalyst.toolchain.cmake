# Mac Catalyst toolchain for PocketShaver.
#
#	 cmake -S . -B build-catalyst -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.toolchain.cmake
#
# CMAKE_SYSTEM_NAME=iOS with CMAKE_OSX_SYSROOT=macosx is CMake's Mac Catalyst
# combination (arm64/x86_64-apple-ios-macabi). Does not use any .xcodeproj.
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT macosx)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "15.2" CACHE STRING
		"iOS deployment target used by Mac Catalyst")
endif()
set(CMAKE_MACOSX_BUNDLE YES)
set(MACEMU_APPLE_TARGET "catalyst" CACHE STRING
	"Apple target (macos|ios|catalyst)")
