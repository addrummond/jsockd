# CMake toolchain file used by CI to build the project for ARM64 architecture
# on MacOS using clang.

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_CROSSCOMPILING TRUE)

set(CMAKE_FIND_ROOT_PATH /home/runner/work/_actions/Timmmm/setup-osxcross/v2/osxcross/target /home/runner/work/_actions/Timmmm/setup-osxcross/v2/osxcross/target/bin)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER oa64-clang)
set(CMAKE_INSTALL_NAME_TOOL aarch64-apple-darwin24.5-install_name_tool)
set(CMAKE_OTOOL aarch64-apple-darwin24.5-otool)
set(CMAKE_AR aarch64-apple-darwin24.5-ar)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
