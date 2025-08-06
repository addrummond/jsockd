#!/bin/sh
set -e

if [ ! -d ../.scratch/quickjs ] || [ -z "$(find ../.scratch/quickjs -type f -name 'libquickjs_*.a')" ]; then
    echo
    echo "*** It looks like you haven't built the QuickJS library yet.     ***"
    echo "*** Go to the parent directory and run ./build_quickjs.sh first. ***"
    echo "*** You only have to do this once. Exiting.                      ***"
    exit 1
fi

BUILD_TYPE=${1:-Debug}
BUILD_DIR="build_$BUILD_TYPE"

cmake_cross_opts=""
if ! [ -z "$TOOLCHAIN_FILE" ]; then
  BUILD_DIR="${BUILD_DIR}_$(basename $TOOLCHAIN_FILE)"
  cmake_cross_opts="-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE"
fi

echo "Running CMake for build type $BUILD_TYPE in directory $BUILD_DIR ..."
cmake -S. -B$BUILD_DIR -D CMAKE_BUILD_TYPE=$BUILD_TYPE $cmake_cross_opts
cmake --build $BUILD_DIR $BUILD_OPTS

if [ "$2" = "run" ]; then
  shift ; shift
  exec $BUILD_DIR/js_server $@
elif [ "$2" = "test" ]; then
  shift ; shift
  exec $BUILD_DIR/js_server_tests $@
fi
