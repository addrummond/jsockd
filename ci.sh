#!/bin/sh

set -e

FILC_VERSION=0.675
FILC_CHECKSUM=ad2684a3532ad98bd0a952d0906dbd27afa857720d5be12e17ffbe547d74bb0c

if ! [ -z "$GITHUB_WORKSPACE" ] && [ "$1" != "setup" ] && [ "$1" != "github_actions_create_release" ]; then
    eval $(mise env)
fi

# Useful when running the script locally
if [ -z "$GITHUB_WORKSPACE" ]; then
    GITHUB_WORKSPACE=$(pwd)
else
    GITHUB_WORKSPACE=$(
      cd -- "$(dirname -- "$0")" >/dev/null 2>&1 || exit
      pwd -P
    )
fi

case $1 in
    setup)
        sudo apt-get update
        sudo apt-get install -y unzip libncurses-dev valgrind gcc-aarch64-linux-gnu
        curl -L https://github.com/jdx/mise/releases/download/v2025.8.18/mise-v2025.8.18-linux-x64 --output - | sudo tee -a /usr/local/bin/mise > /dev/null
        if [ $(sha256sum /usr/local/bin/mise | awk '{ print $1 }') != "7265c5f8099bec212009fcd05bdb786423e9a06e316eddb4362a9869a1950c57" ]; then
            echo "Bad checksum for mise"
            exit 1
        fi
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
        curl -L https://github.com/pizlonator/llvm-project-deluge/releases/download/v${FILC_VERSION}/filc-${FILC_VERSION}-linux-x86_64.tar.xz -o ~/filc-${FILC_VERSION}-linux-x86_64.tar.xz
        if [ $(sha256sum ~/filc-${FILC_VERSION}-linux-x86_64.tar.xz | awk '{ print $1 }') != "$FILC_CHECKSUM" ]; then
            echo "SHA256 checksum of filc-${FILC_VERSION}-linux-x86_64.tar.xz does not match expected value."
            exit 1
        fi
        ( cd ~ && tar -xf ~/filc-${FILC_VERSION}-linux-x86_64.tar.xz && cd filc-${FILC_VERSION}-linux-x86_64 && ./setup.sh )
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

    check_jsockd_server_formatting)
        (
            set -e
            cd jsockd_server
            npm i # clang-format is installed via npm
            format_errors=$(CLANG_FORMAT_COMMAND="./node_modules/.bin/clang-format --dry-run --Werror" ./format.sh 2>&1)
            if [ ! -z "$format_errors" ]; then
                echo "jsockd_server code formatting errors found:"
                echo "$format_errors"
                exit 1
            fi
        )
        ;;

    check_jsockd_version_constants)
        # If you insert a line containing the marker __jsockd_version_check__
        # before a line containing a version constant, then this script will
        # check that the version constant matches the current git tag.
        (
            set +e
            cd $GITHUB_WORKSPACE
            tag=$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null)
            if [ -z $? ]; then
                case "$tag" in
                    "v"*)
                        echo "Checking that all in-code JSockD version constants match the tag ${tag}..."
                        find ./ \( -name '*.go' -o -name '*.c' -o -name '*.h' -o -name '*.ex*' -o -name '*.md' \) -exec awk '/__jsockd_version_check__/ { vc=1 } !/__jsockd_version_check__/ { if (vc == 1) { vc = 0; print $0 } }' {} \; | \
                        sed -e 's/^.*\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*$/\1/' | \
                        while IFS= read -r v; do
                            if [ "v$v" != "$tag" ]; then
                                echo "Version constant $v does not match tag $tag"
                                exit 1
                            fi
                        done
                        ;;
                    *)
                        echo "Not performing JSockD version constant check because tag $tag doesn't seem to be a version."
                esac
            else
                echo "Not performing JSockD version constant check because HEAD is not tagged."
            fi
        )
        ;;

    build_jsockd_server)
        (
            set -e
            cd jsockd_server
            npm i
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    run_jsockd_server_tests)
        (
            set -e
            cd jsockd_server

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

            echo "\n\nRunning tests for debug build...\n\n"
            ./mk.sh Debug test

            echo "\n\nRunning tests for release build...\n\n"
            ./mk.sh Release test
        )
        ;;

    run_jsockd_go_client_tests)
        (
            set -e
            cd $GITHUB_WORKSPACE/clients/go/jsockdclient
            export JSOCKD="$GITHUB_WORKSPACE/jsockd_server/build_Debug/jsockd"
            go test ./...
        )
        ;;

    run_jsockd_go_stress_tests_with_debug_build)
        (
            set -e
            cd $GITHUB_WORKSPACE/jsockd_server/tests/go
            go run main.go "$GITHUB_WORKSPACE/jsockd_server/build_Debug/jsockd"
        )
        ;;

    run_jsockd_go_stress_tests_with_release_build)
        (
            set -e
            cd $GITHUB_WORKSPACE/jsockd_server/tests/go
            go run main.go "$GITHUB_WORKSPACE/jsockd_server/build_Release/jsockd"
        )
        ;;

    build_jsockd_server_linux_arm64)
        (
            set -e
            export TOOLCHAIN_FILE=TC-gcc-arm64.cmake
            cd jsockd_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    build_jsockd_server_darwin_aarch64)
        (
            set -e
            export TOOLCHAIN_FILE=TC-oa64.cmake
            cd jsockd_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    build_jsockd_server_linux_x86_64_filc)
        (
            set -e
            export TOOLCHAIN_FILE=TC-fil-c-CI.cmake
            cd jsockd_server
            ./mk.sh Debug
            ./mk.sh Release
        )
        ;;

    run_jsockd_server_valgrind_tests)
        ./jsockd_server/tests/valgrind_server/run.sh
        ;;

    run_jsockd_server_valgrind_tests_with_non_newline_sep)
        (
            set -e
            export JSOCKD_JS_SERVER_SOCKET_SEP_CHAR_HEX="7C" # '|'
            ./jsockd_server/tests/valgrind_server/run.sh
        )
        ;;

    run_jsockd_server_valgrind_eval_tests)
        ./jsockd_server/tests/valgrind_eval/run.sh
        ;;

    run_jsockd_server_e2e_tests)
        for test_script in jsockd_server/tests/e2e/*.sh; do
            if [ -f "$test_script" ]; then
                printf "\n\nRunning E2E test script %s\n\n" "$test_script"
                $test_script
            fi
        done
        ;;

    package_binaries)
        (
            set -e

            VERSION=$(git describe --match "v*.*.*" --exact-match --tags || true)
            if [ -z "$VERSION" ]; then
                echo "Could not determine version from git tags; won't package binaries..."
                exit 0
            fi

            VERSION=$(echo $VERSION | sed -e 's/^v//')

            cd jsockd_server
            mkdir -p jsockd-release-artifacts

            # Package Linux x86_64
            D="jsockd-${VERSION}-linux-x86_64"
            mkdir jsockd-release-artifacts/$D
            cp build_Release/jsockd jsockd-release-artifacts/$D
            tar -C jsockd-release-artifacts -czf $D.tar.gz $D

            # Package Linux x86_64 Fil-C.
            D="jsockd-${VERSION}-linux-x86_64_filc"
            mkdir -p jsockd-release-artifacts/$D
            cp build_Release_TC-fil-c-CI.cmake/jsockd jsockd-release-artifacts/$D
            tar -C jsockd-release-artifacts -czf $D.tar.gz $D

            # Package Linux ARM64
            D="jsockd-${VERSION}-linux-arm64"
            mkdir jsockd-release-artifacts/$D
            cp build_Release_TC-gcc-arm64.cmake/jsockd jsockd-release-artifacts/$D
            tar -C jsockd-release-artifacts -czf $D.tar.gz $D

            # Package MacOS ARM64
            D="jsockd-${VERSION}-macos-arm64"
            mkdir jsockd-release-artifacts/$D
            cp build_Release_TC-oa64.cmake/jsockd jsockd-release-artifacts/$D
            tar -C jsockd-release-artifacts -czf $D.tar.gz $D

            # Create checksums
            sha256sum *.tar.gz > checksums.txt

            # Sign archives with ED25519 private key.
            echo "$JSOCKD_RELEASE_ARTEFACT_PRIVATE_SIGNING_KEY" | sed 's/[[:space:]]//g' | base64 -d > jsockd_binary_private_signing_key.pem
            for f in jsockd-*.tar.gz; do
               openssl pkeyutl -sign -inkey jsockd_binary_private_signing_key.pem -out /dev/stdout -rawin -in $f | base64 | tr -d '\n' >> ed25519_signatures.txt
               printf "\t%s" "$f" >> ed25519_signatures.txt
               printf "\n" >> ed25519_signatures.txt
            done
        )
        ;;

    github_actions_create_release)
        if [ ! -z "$2" ]; then
            (
                set -e
                cd jsockd_server
                gh release create $2 --title $2
                gh release upload $2 jsockd-*.tar.gz checksums.txt ed25519_signatures.txt
            )
        fi
        ;;

    *)
        echo "Command not recognized: $1"
        exit 1
        ;;
esac
