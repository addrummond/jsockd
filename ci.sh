#!/bin/sh

set -e

if [ "$1" != "setup" ]; then
    eval $(mise env)
fi

case $1 in
    setup)
        sudo apt-get update
        sudo apt-get install -y unzip libncurses-dev valgrind gcc-aarch64-linux-gnu patchelf
        curl -L https://github.com/jdx/mise/releases/download/v2025.5.17/mise-v2025.5.17-linux-x64 --output - | sudo tee -a /usr/local/bin/mise > /dev/null
        sudo chmod +x /usr/local/bin/mise
        # mise takes ages to install CMake, so use a binary distribution
        # instead (still taking the version from .tool-versions).
        ARCH=$(uname -p)
        CMAKE_VERSION=$(fgrep cmake .tool-versions | awk '{print $2}')
        CMAKE_DIST_NAME="cmake-${CMAKE_VERSION}-linux-${ARCH}"
        curl -L --output "${CMAKE_DIST_NAME}.tar.gz" "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${CMAKE_DIST_NAME}.tar.gz"
        tar -xzf "${CMAKE_DIST_NAME}.tar.gz"
        # Add CMake 4 path to beginning of special $GITHUB_PATH file
        cp $GITHUB_PATH /tmp/ghp
        echo "$(pwd)/${CMAKE_DIST_NAME}/bin" > $GITHUB_PATH
        # Make ~/bin and also add it to the path file
        mkdir -p ~/bin
        echo "$HOME/bin" >> $GITHUB_PATH
        cat /tmp/ghp >> $GITHUB_PATH
        mise install node
        curl -L https://github.com/pizlonator/llvm-project-deluge/releases/download/v0.668.8/filc-0.668.8-linux-x86_64.tar.xz -o ~/filc-0.668.8-linux-x86_64.tar.xz
        if [ $(sha256sum ~/filc-0.668.8-linux-x86_64.tar.xz | awk '{ print $1 }') != "562e00b64634fc8c21804616d03a4210cec26751733104f9f49627f2363a3859" ]; then
            echo "SHA256 checksum of filc-0.668.8-linux-x86_64.tar.xz does not match expected value."
            exit 1
        fi
        ( cd ~ && tar -xf ~/filc-0.668.8-linux-x86_64.tar.xz && cd filc-0.668.8-linux-x86_64 && ./setup.sh )
        ;;

    log_versions)
        mise list
        cmake --version
        node --version
        clang-format --version
        ;;

    build_quickjs)
        ./build_quickjs.sh native linux_arm64 mac_arm64 linux_x86_64_filc
        ;;

    check_js_server_formatting)
        (
            set -e
            cd js_server
            npm i # clang-format is installed via npm
            format_errors=$(CLANG_FORMAT_COMMAND="./node_modules/.bin/clang-format --dry-run --Werror" ./format.sh 2>&1)
            if [ ! -z "$format_errors" ]; then
                echo "js_server code formatting errors found:"
                echo "$format_errors"
                exit 1
            fi
        )
        ;;

    build_js_server)
        (
            set -e
            cd js_server
            npm i
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    run_js_server_tests)
        (
            set -e
            cd js_server

            # Check that all tests are included in the list of tests.
            test_list=$(awk '/^ *TEST_LIST *=/ { found = 1 } found { print $0 }' tests/unit/tests.c)
            all_tests=$(grep 'void TEST_.*' tests/unit/tests.c | sed -e 's/.*void TEST_\(.*\)(.*/\1/')
            error=false
            for t in $all_tests; do
                if ! ( echo $test_list | grep -q "T($t)" tests/unit/tests.c ); then
                    echo "Test TEST_$t not included in TEST_LIST in tests/unit/tests.c"
                    error=true
                fi
            done
            if [ "$error" = "true" ]; then
                echo "Some unit tests are not included in the test list (see above). Aborting."
                exit 1
            fi

            ./mk.sh Debug test
        )
        ;;

    build_js_server_linux_arm64)
        (
            set -e
            export TOOLCHAIN_FILE=TC-gcc-arm64.cmake
            cd js_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    build_js_server_darwin_aarch64)
        (
            set -e
            export TOOLCHAIN_FILE=TC-oa64.cmake
            cd js_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    build_js_server_linux_x86_64_filc)
        (
            set -e
            export TOOLCHAIN_FILE=TC-fil-c-CI.cmake
            cd js_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    run_js_server_valgrind_tests)
        exit 0
        ./js_server/tests/valgrind/run.sh
        ;;

    run_js_server_valgrind_tests_with_non_newline_sep)
        exit 0
        (
            set -e
            export JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX="7C" # '|'
            ./js_server/tests/valgrind/run.sh
        )
        ;;

    run_js_server_e2e_tests)
        exit 0
        for test_script in js_server/tests/e2e/*.sh; do
            if [ -f "$test_script" ]; then
                printf "\n\nRunning E2E test script %s\n\n" "$test_script"
                $test_script
            fi
        done
        ;;

    run_js_server_fuzz_tests)
        ./js_server/tests/fuzz/fuzz.sh
        ;;

    package_binaries)
        (
            set -e
            cd js_server
            mkdir -p jsockd-release-artifacts

            echo "$JSOCKD_RELEASE_ARTEFACT_PRIVATE_SIGNING_KEY" | sed 's/[[:space:]]//g' | base64 -d > jsockd_binary_private_signing_key.pem

            # Package Linux x86_64
            mkdir jsockd-release-artifacts/jsockd-linux-x86_64
            cp build_Release/js_server jsockd-release-artifacts/jsockd-linux-x86_64
            cp ../tools-bin/compile_es6_module_Linux_x86_64 jsockd-release-artifacts/jsockd-linux-x86_64/compile_es6_module
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64/js_server_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64/js_server
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64/compile_es6_module_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64/compile_es6_module
            tar -czf jsockd-release-artifacts/jsockd-linux-x86_64.tar.gz jsockd-release-artifacts/jsockd-linux-x86_64

            # Package Linux x86_64 Fil-C. For now we include the regular x86_64 build of compile_es6_module.
            mkdir -p jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server
            patchelf --set-rpath '$ORIGIN' build_Release_TC-fil-c-CI.cmake/js_server
            patchelf --set-interpreter ld-yolo-x86_64.so build_Release_TC-fil-c-CI.cmake/js_server
            cp build_Release_TC-fil-c-CI.cmake/js_server jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/
            cp -L /home/runner/filc-0.668.8-linux-x86_64/pizfix/lib/ld-yolo-x86_64.so jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/ld-yolo-x86_64.so
            cp -L /home/runner/filc-0.668.8-linux-x86_64/pizfix/lib/libc.so jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/libc.so
            cp -L /home/runner/filc-0.668.8-linux-x86_64/pizfix/lib/libpizlo.so jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/libpizlo.so
            cp ../tools-bin/compile_es6_module_Linux_x86_64 jsockd-release-artifacts/jsockd-linux-x86_64_filc/compile_es6_module
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/js_server
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64_filc/ld-yolo-x86_64.so_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/ld-yolo-x86_64.so
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64_filc/libc.so_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/libc.so
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64_filc/libpizlo.so_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64_filc/js_server/libpizlo.so
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-x86_64_filc/compile_es6_module_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-x86_64_filc/compile_es6_module
            tar -czf jsockd-release-artifacts/jsockd-linux-x86_64_filc.tar.gz jsockd-release-artifacts/jsockd-linux-x86_64_filc

            # Package Linux ARM64
            mkdir jsockd-release-artifacts/jsockd-linux-arm64
            cp build_Release_TC-gcc-arm64.cmake/js_server jsockd-release-artifacts/jsockd-linux-arm64
            cp ../tools-bin/compile_es6_module_Linux_arm64 jsockd-release-artifacts/jsockd-linux-arm64/compile_es6_module
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-arm64/js_server_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-arm64/js_server
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-linux-arm64/compile_es6_module_signature.bin -rawin -in jsockd-release-artifacts/jsockd-linux-arm64/compile_es6_module
            tar -czf jsockd-release-artifacts/jsockd-linux-arm64.tar.gz jsockd-release-artifacts/jsockd-linux-arm64

            # Package MacOS ARM64
            mkdir jsockd-release-artifacts/jsockd-macos-arm64
            cp build_Release_TC-oa64.cmake/js_server jsockd-release-artifacts/jsockd-macos-arm64
            cp ../tools-bin/compile_es6_module_Darwin_arm64 jsockd-release-artifacts/jsockd-macos-arm64/compile_es6_module
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-macos-arm64/js_server_signature.bin -rawin -in jsockd-release-artifacts/jsockd-macos-arm64/js_server
            openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out jsockd-release-artifacts/jsockd-macos-arm64/compile_es6_module_signature.bin -rawin -in jsockd-release-artifacts/jsockd-macos-arm64/compile_es6_module
            tar -czf jsockd-release-artifacts/jsockd-macos-arm64.tar.gz jsockd-release-artifacts/jsockd-macos-arm64

            # Create checksums
            cd jsockd-release-artifacts
            sha256sum *.tar.gz > checksums.txt
        )
        ;;

    github_actions_create_release)
        if [ ! -z "$2" ]; then
            (
                set -e
                cd js_server
                gh release create $2 --title $2
                gh release upload $2 jsockd-release-artifacts/*.tar.gz jsockd-release-artifacts/*.txt
            )
        fi
        ;;

    *)
        echo "Command not recognized: $1"
        exit 1
        ;;
esac
