#!/bin/sh

set -e

__BUST_CACHE=1 # increment to bust QuickJS build cache manually if needed

QUICKJS_COMMIT=1fdc768fdc8571300755cdd3e4654ce99c0255ce

# The qjsc tool in Bellard's QuickJS repo doesn't support binary output.
# This is because QuickJS doesn't do bytecode verification, so it is unsafe to
# load bytecode from untrusted sources. To get around this, we add a signature
# to the bytecode file.
generate_qjsc_wrapper() {
    executable="$1"

    echo "#!/bin/sh"
    echo "set -e"
    echo
    echo "decoded=\$(mktemp)"
    echo "keyout=\$(mktemp)"
    echo "cat <<_END | tr -d \"[:space:]\" | base64 -d > \$decoded"
    base64 < "$executable"
    echo "_END"
    echo
    echo "chmod +x \$decoded"
    echo "module_file=\"\$1\""
    echo "output_file=\"\$2\""
    echo "signing_key=\"\$3\""
    echo "if [ -z \"\$module_file\" ] || [ -z \"\$output_file\" ]; then"
    echo "    echo \"Usage: \$0 <js_module_file> <bytecode_output_file> [private_signing_key_pem_file]\""
    echo "    exit 1"
    echo "fi"
    echo
    echo "JSOCKD_OPENSSL=\"\${JSOCKD_OPENSSL:-openssl}\""
    echo
    echo "("
    echo "    set -e"
    echo "    \$decoded -c -m -o /dev/stdout -N jsockd_js_bytecode \$module_file |"
    echo "    awk '{ if (start > 0) start++ } /{\$/ { start = 1 } start > 1 { if (\$0 != \"};\") print \$0 }' |"
    echo "    sed -e 's/^ //g;s/0x//g;s/,//g;s/,\$//;' |"
    echo "    xxd -r -p > \$output_file"
    echo "    if [ -z \"\$signing_key\" ]; then"
    echo "        echo \"Warning: No signing key provided; bytecode will not be signed.\" >&2"
    echo "        # Add dummy signature to simplify read logic in js_server."
    echo "        truncate -s +64 \$output_file"
    echo "    else"
    echo "        \$JSOCKD_OPENSSL pkeyutl -sign -inkey \$signing_key -rawin -in \$output_file > \$keyout"
    echo "        cat \$keyout >> \$output_file"
    echo "    fi"
    echo ")"
    echo "exit_code=\$?"
    echo "rm -f \$decoded"
    echo "rm -f \$keyout"
    echo "exit \$exit_code"
}

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

OS=$(uname)
ARCH=$(uname -m)
if [ "$ARCH" = aarch64 ]; then
    ARCH=arm64
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
    make clean
    case "$platform" in
        native)
            # Debug build for quickjs library
            CFLAGS="$DEBUG_CFLAGS" make CONFIG_LTO=n
            cp qjs ../../tools-bin
            cp qjsc ../../tools-bin
            generate_qjsc_wrapper qjsc > ../../tools-bin/compile_es6_module
            cp ../../tools-bin/compile_es6_module ../../tools-bin/compile_es6_module_${OS}_${ARCH}
            chmod +x ../../tools-bin/compile_es6_module
            mv libquickjs.a /tmp/libquickjs_${OS}_${ARCH}_Debug.a

            # Release build for quickjs library
            make clean
            CFLAGS="$RELEASE_CFLAGS" make CONFIG_LTO=y
            mv libquickjs.a /tmp/libquickjs_${OS}_${ARCH}_Release.a
            ;;
        mac_arm64)
            if [ "$OS" == "Darwin" ] && [ "$ARCH" == "arm64" ]; then
                echo "You seem to be on a MacOS/ARM64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'mac_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" make CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            generate_qjsc_wrapper qjsc > ../../tools-bin/compile_es6_module_Darwin_arm64
            chmod +x ../../tools-bin/compile_es6_module_Darwin_arm64
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Debug.a
            # Release
            make clean
            CFLAGS="$RELEASE_CFLAGS" make CONFIG_DEFAULT_AR=y CONFIG_CLANG=y CROSS_PREFIX=aarch64-apple-darwin24.5-
            mv libquickjs.a /tmp/libquickjs_Darwin_arm64_Release.a
            ;;
        linux_x86_64)
            if [ "$OS" == "Linux" ] && [ "$ARCH" == "x86_64" ]; then
                echo "You seem to be on a Linux/x86_64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_x86_64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" make CONFIG_LTO=n
            generate_qjsc_wrapper qjsc > ../../tools-bin/compile_es6_module_Linux_x86_64
            cp ../../tools-bin/compile_es6_module_Linux_x86_64 ../../tools-bin/compile_es6_module_x86_64
            chmod +x ../../tools-bin/compile_es6_module_Linux_x86_64
            chmod +x ../../tools-bin/compile_es6_module
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Debug.a # this will get killed by make clean
            make clean
            # Release
            CFLAGS="$RELEASE_CFLAGS" make CONFIG_LTO=y
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_Release.a
            ;;
        linux_arm64)
            if [ "$OS" == "Linux" ] && [ "$ARCH" == "arm64" ]; then
                echo "You seem to be on a Linux/arm64 system, so pass the 'native' argument to 'build_quickjs.sh', not 'linux_arm64'."
                exit 1
            fi
            # Debug
            CFLAGS="$DEBUG_CFLAGS" make CONFIG_LTO=n CROSS_PREFIX=aarch64-linux-gnu-
            generate_qjsc_wrapper qjsc > ../../tools-bin/compile_es6_module_Linux_arm64
            chmod +x ../../tools-bin/compile_es6_module_Linux_arm64
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Debug.a
            # Release
            make clean
            CFLAGS="$RELEASE_CFLAGS" make CONFIG_LTO=y CROSS_PREFIX=aarch64-linux-gnu-
            mv libquickjs.a /tmp/libquickjs_Linux_arm64_Release.a
            ;;
        linux_x86_64_filc)
            # Debug
            # See https://github.com/pizlonator/pizlonated-quickjs/commit/258a4a291fd0f080614e5b345528478c31e51705#diff-45f1ae674139f993bf8a99c382c1ba4863272a6fec2f492d76d7ff1b2cfcfbe2R56-R5187 for diff the patch is based on
            git apply ../../fil-c-quickjs.patch
            CFLAGS="$DEBUG_CFLAGS" make CC=~/filc-0.668.8-linux-x86_64/build/bin/clang CONFIG_LTO= CONFIG_CLANG=y
            generate_qjsc_wrapper qjsc > ../../tools-bin/compile_es6_module_Linux_x86_64_filc
            git apply -R ../../fil-c-quickjs.patch
            mv libquickjs.a /tmp/libquickjs_Linux_x86_64_filc_Debug.a
            # Release
            make clean
            git apply ../../fil-c-quickjs.patch
            CFLAGS="$RELEASE_CFLAGS" make CC=~/filc-0.668.8-linux-x86_64/build/bin/clang CONFIG_LTO= CONFIG_CLANG=y
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
