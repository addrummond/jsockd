#!/bin/sh

set -e

OS=$(uname)
ARCH=$(uname -m)

if [ -z "$MAKE" ]; then
    MAKE="make"
    if [ "GNU Make" != "$( ( make --version 2>/dev/null || true ) | head -n 1 | awk '{print $1,$2}' )" ]; then
        if [ "GNU Make" = "$( ( gmake --version 2>/dev/null || true ) | head -n 1 | awk '{print $1,$2}' )" ]; then
            MAKE=gmake
        else
            foundmake=0
            case "$OS$ARCH" in
                *MINGW64*x86_64*)
                    if [ "GNU Make" = "$( ( "/c/Program Files (x86)/GnuWin32/bin/make.exe" --version 2>/dev/null || true ) | head -n 1 | awk '{print $1,$2}' )" ]; then
                        MAKE="/c/Program Files (x86)/GnuWin32/bin/make.exe"
                        foundmake=1
                    fi
                    ;;
                *)
                    ;;
            esac
            if [ "$foundmake" = "0" ]; then
                echo "Cannot find GNU Make. Please install it as 'make' or 'gmake', or set the MAKE environment variable to point to it."
                exit 1
            fi
        fi
    fi
fi

QUICKJS_COMMIT=f1139494d18a2053630c5ed3384a42bb70db3c53

FILC_VERSION=0.677
FILC_CLANG="${FILC_CLANG:-$HOME/filc-${FILC_VERSION}-linux-x86_64/build/bin/clang}"

rm -rf .scratch
mkdir -p .scratch
cd .scratch
git clone https://github.com/bellard/quickjs
cd quickjs
git checkout $QUICKJS_COMMIT
git apply ../../quickjs-patch.patch

# Unicode tables fetch script uses wget, which is not available on all systems.
# Modify to use curl.
sed -e 's/^\( *\)wget \(.*\) -O \(.*\)$/\1curl \2 -o \3/' unicode_download.sh > unicode_download_curl.sh
chmod +x unicode_download_curl.sh

# This fails from time to time (presumably just due to network flakiness),
# which is annoying in CI, so retry a few times before giving up.
retries=0
while [ $retries -lt 5 ]; do
    if ./unicode_download_curl.sh; then
      break
    fi
    retries=$(($retries + 1))
    sleep 10
done

DEBUG_CFLAGS="-DDUMP_LEAKS"
RELEASE_CFLAGS="-O2"
FILC_RELEASE_CFLAGS="-O2"

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
        native|windows_x64_msvc|mac_arm64|linux_x86_64|linux_arm64|linux_x86_64_filc)
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
    mkdir -p ../../tools-bin
    "$MAKE" clean
    case "$platform" in
        native)
            if [ "$OS" = OpenBSD ]; then
                MAKE_OPTS='CONFIG_FREEBSD=y LIBS=-lm LIBS+=-lpthread HOST_LIBS=-lm HOST_LIBS+=-lpthread'
                EXTRA_CFLAGS="-Denviron=NULL -Dsighandler_t=sig_t -Dmalloc_usable_size='0&&'"
                git apply ../../openbsd-quickjs.patch
            fi
            # Debug build for quickjs library
            # shellcheck disable=SC2086 # Intended splitting of MAKE_OPTS
            CFLAGS="$DEBUG_CFLAGS $EXTRA_CFLAGS" "$MAKE" CONFIG_LTO=n $MAKE_OPTS
            cp qjs ../../tools-bin
            cp qjsc ../../tools-bin
            mv libquickjs.a "/tmp/libquickjs_${OS}_${ARCH}_Debug.a"
            # Release build for quickjs library
            "$MAKE" clean
            # shellcheck disable=SC2086 # Intended splitting of MAKE_OPTS
            CFLAGS="$RELEASE_CFLAGS $EXTRA_CFLAGS" "$MAKE" CONFIG_LTO=y $MAKE_OPTS
            mv libquickjs.a "/tmp/libquickjs_${OS}_${ARCH}_Release.a"
            ;;
        windows_x64_msvc)
            windows_x64_msvc=1
            git apply ../../draft-win-patch
            mkdir pthreads-win32-include
            mkdir pthreads-win32-lib
            BASE_FTP='ftp://sourceware.org/pub/pthreads-win32/prebuilt-dll-2-9-1-release'
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreads-win32-include/pthread.h   "$BASE_FTP/include/pthread.h"
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreads-win32-include/sched.h     "$BASE_FTP/include/sched.h"
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreads-win32-include/semaphore.h "$BASE_FTP/include/semaphore.h"
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreads-win32-include/pthread.h   "$BASE_FTP/include/pthread.h"
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreads-win32-lib/pthreadVC2.lib  "$BASE_FTP/lib/x64/pthreadVC2.lib"
            # DLLs in cwd next to executable
            curl -fsSL --disable-epsv --ftp-method nocwd -o pthreadVC2.dll                     "$BASE_FTP/dll/x64/pthreadVC2.dll"
            # TODO checksum
            curl -o msvcr100.dll https://people.torproject.org/~gk/mirrors/sources/msvcr100.dll
            /c/Program\ Files\ */GnuWin32/bin/make.exe clean
            /c/Program\ Files\ */GnuWin32/bin/make.exe -d CONFIG_WIN32=y CC=cl HOST_CC=cl AR=lib CFLAGS='/nologo /std:c17 /experimental:c11atomics -Ipthreads-win32-include -D_WINSOCKAPI_ -DWIN32_LEAN_AND_MEAN' LDFLAGS='/LIBPATH:pthreads-win32-lib pthreadVC2.lib'
            ;;
        mac_arm64)
            if [ "$OS" = "Darwin" ] && [ "$ARCH" = "arm64" ]; then
                echo "You seem to be on a MacOS/ARM64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'mac_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" "$MAKE" CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Debug.a
            # Release
            "$MAKE" clean
            CFLAGS="$RELEASE_CFLAGS" "$MAKE" CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Release.a
            ;;
        linux_x86_64)
            if [ "$OS" = "Linux" ] && [ "$ARCH" = "x86_64" ]; then
                echo "You seem to be on a Linux/x86_64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_x86_64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" "$MAKE" CONFIG_LTO=n
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Debug.a
            "$MAKE" clean
            # Release
            CFLAGS="$RELEASE_CFLAGS" "$MAKE" CONFIG_LTO=y
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Release.a
            ;;
        linux_arm64)
            if [ "$OS" = "Linux" ] && [ "$ARCH" = "arm64" ]; then
                echo "You seem to be on a Linux/arm64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" "$MAKE" CONFIG_LTO=n CROSS_PREFIX=aarch64-linux-gnu-
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Debug.a
            # Release
            "$MAKE" clean
            CFLAGS="$RELEASE_CFLAGS" "$MAKE" CONFIG_LTO=y CROSS_PREFIX=aarch64-linux-gnu-
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Release.a
            ;;
        linux_x86_64_filc)
            # Debug
            # See https://github.com/pizlonator/pizlonated-quickjs/commit/258a4a291fd0f080614e5b345528478c31e51705#diff-45f1ae674139f993bf8a99c382c1ba4863272a6fec2f492d76d7ff1b2cfcfbe2R56-R5187 for diff the patch is based on
            git apply ../../fil-c-quickjs.patch
            CFLAGS="$DEBUG_CFLAGS -static" LDFLAGS="-static" "$MAKE" CC="$FILC_CLANG" CONFIG_LTO= CONFIG_CLANG=y
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_filc_Debug.a
            # Release
            "$MAKE" clean
            CFLAGS="$FILC_RELEASE_CFLAGS -static" LDFLAGS="-static" "$MAKE" CC="$FILC_CLANG" CONFIG_LTO= CONFIG_CLANG=y
            git apply -R ../../fil-c-quickjs.patch
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_filc_Release.a
            ;;
        *)
            echo "Unknown platform: $platform"
            exit 1
            ;;
    esac
done

if [ "$windows_x64_msvc" = 1 ]; then
   echo "Build done!"
   ls -la
else
    cp /tmp/libquickjs_*.a .
fi
