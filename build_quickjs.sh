#!/bin/sh

set -e

MAKE=${MAKE:-make}

__BUST_CACHE=1 # increment to bust QuickJS build cache manually if needed

QUICKJS_COMMIT=20d2b404af65edc5d66ee6f11a59f930ea3d1b88

FILC_VERSION=0.670

rm -rf .scratch
mkdir -p .scratch
cd .scratch
git clone https://github.com/bellard/quickjs
cd quickjs
git checkout $QUICKJS_COMMIT

# Unicode tables fetch script uses wget, which is not available on all systems.
# Modify to use curl.
sed -e 's/^\( *\)wget \(.*\) -O \(.*\)$/\1curl \2 -o \3/' unicode_download.sh > unicode_download_curl.sh
chmod +x unicode_download_curl.sh

# This fails from time to time (presumably just due to network flakiness),
# which is annoying in CI, so retry a few times before giving up.
retries=0
while [ $retries -lt 5 ]; do
    if [ ./unicode_download_curl.sh ]; then
      break
    fi
    retries=$(($retries + 1))
    sleep 10
done

DEBUG_CFLAGS="-DDUMP_LEAKS"
RELEASE_CFLAGS="-O2"
FILC_RELEASE_CFLAGS="-O2"

OS=$(uname)
ARCH=$(uname -m)
if [ "$ARCH" = aarch64 ]; then
    ARCH=arm64
elif [ "$ARCH" = amd64 ]; then
    ARCH=x86_64
fi

# Go through the requested builds
if [ -z "$*" ]; then
    platforms=native
else
    platforms="$*"
fi
for platform in $platforms; do
    case "$platform" in
        native|mac_arm64|linux_x86_64|linux_arm64|linux_x86_64_filc)
            # Valid platforms, continue
            ;;
        *)
            echo "Unknown platform: $platform"
            exit 1
            ;;
    esac
done
echo "Building for platforms: $platforms"
rm -f /tmp/libquickjs_*.a || true
for platform in $platforms; do
    echo "Building for platform $platform..."
    $MAKE clean
    case "$platform" in
        native)
            # Debug build for quickjs library
            CFLAGS="$DEBUG_CFLAGS" $MAKE CONFIG_LTO=n
            cp qjs ../../tools-bin
            cp qjsc ../../tools-bin
            mv libquickjs.a /tmp/libquickjs_${OS}_${ARCH}_Debug.a

            # Release build for quickjs library
            $MAKE clean
            CFLAGS="$RELEASE_CFLAGS" $MAKE CONFIG_LTO=y
            mv libquickjs.a /tmp/libquickjs_${OS}_${ARCH}_Release.a
            ;;
        mac_arm64)
            if [ "$OS" = "Darwin" ] && [ "$ARCH" = "arm64" ]; then
                echo "You seem to be on a MacOS/ARM64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'mac_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" $MAKE CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Debug.a
            # Release
            $MAKE clean
            CFLAGS="$RELEASE_CFLAGS" $MAKE CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Release.a
            ;;
        linux_x86_64)
            if [ "$OS" = "Linux" ] && [ "$ARCH" = "x86_64" ]; then
                echo "You seem to be on a Linux/x86_64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_x86_64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" $MAKE CONFIG_LTO=n
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Debug.a # this will get killed by make clean
            $MAKE clean
            # Release
            CFLAGS="$RELEASE_CFLAGS" $MAKE CONFIG_LTO=y
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Release.a
            ;;
        linux_arm64)
            if [ "$OS" = "Linux" ] && [ "$ARCH" = "arm64" ]; then
                echo "You seem to be on a Linux/arm64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" $MAKE CONFIG_LTO=n CROSS_PREFIX=aarch64-linux-gnu-
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Debug.a
            # Release
            $MAKE clean
            CFLAGS="$RELEASE_CFLAGS" $MAKE CONFIG_LTO=y CROSS_PREFIX=aarch64-linux-gnu-
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Release.a
            ;;
        linux_x86_64_filc)
            # Debug
            # See https://github.com/pizlonator/pizlonated-quickjs/commit/258a4a291fd0f080614e5b345528478c31e51705#diff-45f1ae674139f993bf8a99c382c1ba4863272a6fec2f492d76d7ff1b2cfcfbe2R56-R5187 for diff the patch is based on
            git apply ../../fil-c-quickjs.patch
            CFLAGS="$DEBUG_CFLAGS" $MAKE CC=~/filc-${FILC_VERSION}-linux-x86_64/build/bin/clang CONFIG_LTO= CONFIG_CLANG=y
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_filc_Debug.a
            # Release
            $MAKE clean
            CFLAGS="$FILC_RELEASE_CFLAGS" $MAKE CC=~/filc-${FILC_VERSION}-linux-x86_64/build/bin/clang CONFIG_LTO= CONFIG_CLANG=y
            git apply -R ../../fil-c-quickjs.patch
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_filc_Release.a
            ;;
        *)
            echo "Unknown platform: $platform"
            exit 1
            ;;
    esac
done
cp /tmp/libquickjs_*.a .
