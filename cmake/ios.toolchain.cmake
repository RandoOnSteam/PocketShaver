# iPhone / iPadOS toolchain for PocketShaver.
#
# Simulator (recommended for command-line testing):
#	 cmake -S . -B build-ios -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#		 -DCMAKE_OSX_SYSROOT=iphonesimulator
#	 cmake --build build-ios --target PocketShaver -- -sdk iphonesimulator
#
# Device:
#	 cmake -S . -B build-ios -G Xcode \
#		 -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake
#	 cmake --build build-ios --target PocketShaver -- -sdk iphoneos
set(CMAKE_SYSTEM_NAME iOS)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "15.2" CACHE STRING
		"iOS deployment target")
endif()
set(CMAKE_MACOSX_BUNDLE YES)
set(MACEMU_APPLE_TARGET "ios" CACHE STRING
	"Apple target (macos|ios|catalyst)")
