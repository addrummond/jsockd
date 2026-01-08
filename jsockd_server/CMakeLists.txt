cmake_minimum_required(VERSION 4.0)

project(jsockd_js_server VERSION 1.0 LANGUAGES C)

message(STATUS "Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

# Derive version from git tag if available and the current commit is tagged
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v*.*.*"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE VERSION_STRING
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL NONE
  )
  string(REPLACE "v" "" VERSION_STRING "${VERSION_STRING}")
  string(STRIP VERSION_STRING "${VERSION_STRING}")
  if(VERSION_STRING)
    message(STATUS "Version string: '${VERSION_STRING}'")
  endif()
endif()

# If the version string matches /-[0-9]+-g[0-9a-f]+$/ then the current commit is not tagged
# and we don't know the version. If it is a clean tag, use it.
set(_use_git_version OFF)
if(VERSION_STRING AND NOT VERSION_STRING MATCHES "-[0-9]+-g[0-9a-f]+$")
  string(REGEX REPLACE "-[0-9]+-g[0-9a-f]+$" "" VERSION_STRING "${VERSION_STRING}")
  set(_use_git_version ON)
endif()

# Toolchain suffix for filc builds
if(CMAKE_C_COMPILER MATCHES "filc")
  set(FILC "_filc")
endif()

# Add xxHash without global side effects (static only)
set(_save_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "Do not build xxhsum tool")
add_subdirectory(src/lib/xxHash/build/cmake xxhash_build EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_save_BUILD_SHARED_LIBS}")

# Generate the QuickJS bytecode for the backtrace module
add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_backtrace.c"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools-bin/qjsc"
          -c
          -o "${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_backtrace.c"
          -N g_backtrace_module_bytecode
          "${CMAKE_CURRENT_SOURCE_DIR}/src/js/backtrace.mjs"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/js/backtrace.mjs" ../.scratch/quickjs/quickjs.c
  VERBATIM
)

# Bundle the shims module
add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs"
  COMMAND sh -c "( cd ${CMAKE_CURRENT_SOURCE_DIR} && npm ci ) && \
                 ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.bin/esbuild \
                 ${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/shims.mjs \
                 --bundle \
                 --outfile=${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs \
                 --format=esm"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/shims.mjs" ../.scratch/quickjs/quickjs.c
  VERBATIM
)

# Generate the QuickJS bytecode source file for the shims module
add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_shims.c"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools-bin/qjsc"
          -c
          -o "${CMAKE_CURRENT_SOURCE_DIR}/src/js/gen_shims.c"
          -N g_shims_module_bytecode
          "${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/js/shims/bundle.mjs" ../.scratch/quickjs/quickjs.c
  VERBATIM
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
  src/textencodedecode.c
  src/globals.c
  src/backtrace.c
  src/threadstate.c
  src/messages.c
  src/js/gen_backtrace.c
  src/js/gen_shims.c
  ${ED25519_LIB_SOURCES}
)

# Core static library holding most sources to avoid duplicate compilation between exe and tests
add_library(jsockd_lib STATIC)
target_sources(jsockd_lib PRIVATE ${JSOCKD_SERVER_SOURCES})
target_compile_features(jsockd_lib PUBLIC c_std_23)

# Embed VERSION if we have a clean (tagged) git version
if(_use_git_version)
  target_compile_definitions(jsockd_lib PUBLIC VERSION="${VERSION_STRING}")
endif()

target_compile_options(jsockd_lib PUBLIC $<$<CONFIG:Release>:-O2>)
if (CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(jsockd_lib PUBLIC
    $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wpedantic -Werror
      -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
      -Wno-missing-field-initializers>
  )
elseif (CMAKE_C_COMPILER_ID MATCHES "GNU")
  target_compile_options(jsockd_lib PUBLIC
    $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter -Wno-unknown-pragmas>
  )
endif()

# Build-type macros (optional, preserved behavior) via config-only generator expressions
target_compile_definitions(jsockd_lib
  PUBLIC
    $<$<CONFIG:Release>:CMAKE_BUILD_TYPE_RELEASE>
    $<$<CONFIG:Debug>:CMAKE_BUILD_TYPE_DEBUG>
)

# For x86-64, ensure we have CX16 (CMPXCHG16B) support for 128-bit atomics (Clang/GCC)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "86" OR CMAKE_SYSTEM_PROCESSOR MATCHES "amd64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"))
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR CMAKE_C_COMPILER_ID MATCHES "GNU")
    target_compile_options(jsockd_lib PUBLIC -march=x86-64 -mcx16 -mtune=generic)
  endif()
endif()

# Determine architecture for QuickJS imported lib
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch"))
  set(QUICKJS_LIB_ARCH "arm64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "86" OR CMAKE_SYSTEM_PROCESSOR MATCHES "amd64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"))
  set(QUICKJS_LIB_ARCH "x86_64")
else()
  message(FATAL_ERROR "No QuickJS lib has been built for this architecture: ${CMAKE_SYSTEM_PROCESSOR} / ${CMAKE_SIZEOF_VOID_P}")
endif()

add_library(qjs STATIC IMPORTED)
set(_qjs_prefix "${CMAKE_SOURCE_DIR}/../.scratch/quickjs/libquickjs_${CMAKE_SYSTEM_NAME}_${QUICKJS_LIB_ARCH}${FILC}_")
set_target_properties(qjs PROPERTIES
  IMPORTED_LOCATION_DEBUG          "${_qjs_prefix}Debug.a"
  IMPORTED_LOCATION_RELEASE        "${_qjs_prefix}Release.a"
  INTERFACE_INCLUDE_DIRECTORIES    "${CMAKE_SOURCE_DIR}/../.scratch/quickjs;${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(jsockd_lib PUBLIC qjs xxHash::xxhash)

if(UNIX AND NOT APPLE)
  target_link_libraries(jsockd_lib PUBLIC m)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU")
  target_link_libraries(jsockd_lib PUBLIC atomic)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD" OR CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  target_link_libraries(jsockd_lib PUBLIC pthread)
endif()

add_executable(jsockd src/main.c)
target_link_libraries(jsockd PRIVATE jsockd_lib)
target_compile_features(jsockd PRIVATE c_std_23)

add_executable(jsockd_tests tests/unit/tests.c tests/unit/lib/pcg.c)
target_link_libraries(jsockd_tests PRIVATE jsockd_lib)
target_compile_features(jsockd_tests PRIVATE c_std_23)
