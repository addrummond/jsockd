cmake_minimum_required(VERSION 4.0)

project(
  js_server
  VERSION 1.0
  LANGUAGES C)

MESSAGE(STATUS "Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

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
    set(OPTIMIZE_SIZE ON)
    add_compile_definitions(CMAKE_BUILD_TYPE_RELEASE)
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

set(JS_SERVER_SOURCES
  src/custom_module_loader.c
  src/wait_group.c
  src/hash_cache.c
  src/line_buf.c
  src/utils.c
  src/hex.c
  src/verify_bytecode.c
  ${ED25519_LIB_SOURCES}
  )

add_executable(js_server src/main.c ${JS_SERVER_SOURCES})
target_compile_features(js_server PUBLIC c_std_11)

add_executable(js_server_tests tests/unit/tests.c tests/unit/lib/pcg.c ${JS_SERVER_SOURCES})
target_compile_features(js_server_tests PUBLIC c_std_11)

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
    ${CMAKE_SOURCE_DIR}/../.scratch/quickjs/libquickjs_${CMAKE_SYSTEM_NAME}_${QUICKJS_LIB_ARCH}_${CMAKE_BUILD_TYPE}.a
  INTERFACE_INCLUDE_DIRECTORIES
    "${CMAKE_SOURCE_DIR}/../.scratch/quickjs;${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(js_server qjs m)
target_link_libraries(js_server_tests qjs m)
