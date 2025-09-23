cmake_minimum_required(VERSION 4.0)

project(
  jsockd_js_server
  VERSION 1.0
  LANGUAGES C)

MESSAGE(STATUS "Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

execute_process(COMMAND git describe --tags --match "v*.*.*"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_STRING
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL NONE)
string(REPLACE "v" "" VERSION_STRING "${VERSION_STRING}")
string(STRIP VERSION_STRING "${VERSION_STRING}")
MESSAGE(STATUS "Version string: '${VERSION_STRING}'")

# If the version string matches /-[0-9]+-g[0-9a-f]+$/ then the current commit is not
# tagged, so we don't know the version
if((NOT "${VERSION_STRING}" STREQUAL "") AND NOT (VERSION_STRING MATCHES "-[0-9]+-g[0-9a-f]+$"))
    string(REGEX REPLACE "-[0-9]+-g[0-9a-f]+$" "" VERSION_STRING "${VERSION_STRING}")
    add_compile_definitions(VERSION=${VERSION_STRING})
endif()

if(UNIX)
    add_compile_definitions(UNIX)
    if (APPLE)
        add_compile_definitions(MACOS)
    endif()
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_definitions(LINUX)
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL Release)
    add_compile_definitions(CMAKE_BUILD_TYPE_RELEASE)
    add_compile_options(-O2)
endif()

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    add_compile_definitions(CMAKE_BUILD_TYPE_DEBUG)

    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(-O0 -Wall -Wextra -Wpedantic -Werror
          -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
          -Wno-missing-field-initializers)
    endif()
    if (CMAKE_C_COMPILER_ID MATCHES "GNU")
        add_compile_options(-O0 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    add_link_options(-lm)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "BSD")
    add_link_options(-lpthread)
endif()

if(CMAKE_C_COMPILER MATCHES "filc")
    set(FILC "_filc")
endif()

# Add https://github.com/Cyan4973/xxHash/ hash library
set(XXHASH_BUILD_XXHSUM OFF)        # Don't build command line tool
option(BUILD_SHARED_LIBS OFF)       # Build static library
add_subdirectory(src/lib/xxHash/build/cmake xxhash_build EXCLUDE_FROM_ALL)

# Generate the QuickJS bytecode for the backtrace module
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_backtrace.c
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tools-bin/qjsc -c -o ${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_backtrace.c -N g_backtrace_module_bytecode ${CMAKE_CURRENT_SOURCE_DIR}/src/js/backtrace.mjs
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/js/backtrace.mjs
  )

# Bundle the shims module
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs
    COMMAND sh -c "( cd ${CMAKE_CURRENT_SOURCE_DIR} && npm i ) && ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.bin/esbuild ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/shims.mjs --bundle --outfile=${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs --format=esm"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/shims.mjs
    VERBATIM
)

# Generate the QuickJS bytecode source file for the shims module
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_shims.c
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tools-bin/qjsc -c -o ${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_shims.c -N g_shims_module_bytecode ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs
  )

set(ED25519_LIB_SOURCES
  src/lib/ed25519/add_scalar.c
  src/lib/ed25519/fe.c
  src/lib/ed25519/ge.c
  src/lib/ed25519/key_exchange.c
  src/lib/ed25519/keypair.c
  src/lib/ed25519/sc.c
  src/lib/ed25519/seed.c
  src/lib/ed25519/sha512.c
  src/lib/ed25519/sign.c
  src/lib/ed25519/verify.c
)

set(JSOCKD_SERVER_SOURCES
  src/custom_module_loader.c
  src/wait_group.c
  src/hash_cache.c
  src/line_buf.c
  src/utils.c
  src/hex.c
  src/verify_bytecode.c
  src/cmdargs.c
  src/mmap_file.c
  src/modcompiler.c
  src/console.c
  src/log.c
  src/js/gen_backtrace.c
  src/js/gen_shims.c
  ${ED25519_LIB_SOURCES}
  )

# This lib is just here to stop CMake building most of the sources twice, once
# for the main executable and once for the tests.
add_library(jsockd_lib STATIC ${JSOCKD_SERVER_SOURCES})
target_compile_features(jsockd_lib PUBLIC c_std_11)

add_executable(jsockd src/main.c)
target_link_libraries(jsockd jsockd_lib xxHash::xxhash)
target_compile_features(jsockd PUBLIC c_std_11)

add_executable(jsockd_tests tests/unit/tests.c tests/unit/lib/pcg.c)
target_link_libraries(jsockd_tests jsockd_lib xxHash::xxhash)
target_compile_features(jsockd_tests PUBLIC c_std_11)

if (CMAKE_SIZEOF_VOID_P EQUAL 8 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch"))
    set(QUICKJS_LIB_ARCH "arm64")
elseif (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "86" OR CMAKE_SYSTEM_PROCESSOR MATCHES "amd64")
    set(QUICKJS_LIB_ARCH "x86_64")
else()
    message(FATAL_ERROR "No QuickJS lib has been built for this architecture: ${CMAKE_SYSTEM_PROCESSOR} / ${CMAKE_SIZEOF_VOID_P}")
endif()

add_library(qjs STATIC IMPORTED)
set_target_properties(qjs PROPERTIES
  IMPORTED_LOCATION
    ${CMAKE_SOURCE_DIR}/../.scratch/quickjs/libquickjs_${CMAKE_SYSTEM_NAME}_${QUICKJS_LIB_ARCH}${FILC}_${CMAKE_BUILD_TYPE}.a
  INTERFACE_INCLUDE_DIRECTORIES
    "${CMAKE_SOURCE_DIR}/../.scratch/quickjs;${CMAKE_SOURCE_DIR}/src/lib"
)
target_link_libraries(jsockd_lib qjs m xxHash::xxhash)
