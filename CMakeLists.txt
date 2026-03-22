cmake_minimum_required(VERSION 3.24)

# Export compile commands for LSP support (clangd)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Macro
macro(add_feature_option OPTION_NAME FEATURE_NAME DESCRIPTION DEFAULT_VALUE)
  option(${OPTION_NAME} ${DESCRIPTION} ${DEFAULT_VALUE})
  if (${OPTION_NAME})
    list(APPEND VCPKG_MANIFEST_FEATURES "${FEATURE_NAME}")
  endif ()
endmacro()

# Lesma features
add_feature_option(LESMA_BUILD_CLI "cli" "Build CLI" ON)
add_feature_option(LESMA_BUILD_TESTS "tests" "Build Tests" OFF)
add_feature_option(LESMA_BUILD_BENCHMARKS "benchmarks" "Build Benchmarks" OFF)
add_feature_option(LESMA_BUILD_DIST "dist" "Build Dist" OFF)
add_feature_option(LESMA_BUILD_LSP "lsp" "Build Lesma LSP server (lesma-lsp)" OFF)
option(LESMA_SANITIZE_ADDRESS "Build with AddressSanitizer and LeakSanitizer for memory error and leak detection (Debug builds, all platforms)" OFF)

# Get version from vcpkg
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json VCPKG_MANIFEST)
string(JSON LESMA_VERSION GET ${VCPKG_MANIFEST} version)
set(VERSION ${LESMA_VERSION})

# Prefer vcpkg's built-in release-only triplets so ports like LLVM are not
# built twice (debug + release). This must happen before project().
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES [[vcpkg\.cmake]])
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_lesma_triplet_arch "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _lesma_triplet_arch)
      set(_lesma_triplet_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT _lesma_triplet_arch)
      set(_lesma_triplet_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    if(NOT _lesma_triplet_arch)
      execute_process(COMMAND /usr/bin/uname -m OUTPUT_VARIABLE _lesma_triplet_arch
                      OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(_lesma_triplet_arch MATCHES "^(arm64|aarch64)$")
      set(_lesma_default_triplet "arm64-osx-release")
    else()
      set(_lesma_default_triplet "x64-osx-release")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_lesma_triplet_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(NOT _lesma_triplet_arch)
      set(_lesma_triplet_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    if(NOT _lesma_triplet_arch)
      execute_process(COMMAND /usr/bin/uname -m OUTPUT_VARIABLE _lesma_triplet_arch
                      OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(_lesma_triplet_arch MATCHES "^(arm64|aarch64)$")
      set(_lesma_default_triplet "arm64-linux-release")
    else()
      set(_lesma_default_triplet "x64-linux-release")
    endif()
  endif()

  if(DEFINED _lesma_default_triplet AND
     (NOT DEFINED CACHE{VCPKG_TARGET_TRIPLET} OR
      VCPKG_TARGET_TRIPLET STREQUAL "" OR
      VCPKG_TARGET_TRIPLET STREQUAL "arm64-osx" OR
      VCPKG_TARGET_TRIPLET STREQUAL "x64-osx" OR
      VCPKG_TARGET_TRIPLET STREQUAL "arm64-linux" OR
      VCPKG_TARGET_TRIPLET STREQUAL "x64-linux"))
    set(VCPKG_TARGET_TRIPLET "${_lesma_default_triplet}" CACHE STRING
        "vcpkg triplet" FORCE)
  endif()
endif()

# Project
project(Lesma VERSION ${VERSION} DESCRIPTION "Lesma Compiler")

# C++ Flags and settings
if (CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Debug mode")
elseif (CMAKE_BUILD_TYPE MATCHES Release)
  message(STATUS "Release mode")
endif ()

set(CMAKE_CXX_STANDARD 23)
add_definitions(${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -frtti)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# AddressSanitizer + LeakSanitizer (works on macOS Apple Silicon and Linux; use instead of Valgrind on macOS)
if (LESMA_SANITIZE_ADDRESS)
  if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "LESMA_SANITIZE_ADDRESS is best used with CMAKE_BUILD_TYPE=Debug; current type is ${CMAKE_BUILD_TYPE}")
  endif ()
  set(SANITIZE_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZE_FLAGS}")
  message(STATUS "AddressSanitizer and LeakSanitizer enabled (memory and leak detection)")
endif ()

# External dependencies
find_package(fmt CONFIG REQUIRED)
find_package(nameof CONFIG REQUIRED)
find_path(PLF_NANOTIMER_INCLUDE_DIRS "plf_nanotimer.h")

# Directories
get_filename_component(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(STDLIB_DIR ${SRC_DIR}/stdlib ABSOLUTE)
get_filename_component(BUILD_DIR ${CMAKE_BINARY_DIR} ABSOLUTE)

# Version configuration
configure_file(
  "${SRC_DIR}/liblesma/Common/LesmaVersion.h.in"
  "${SRC_DIR}/liblesma/Common/LesmaVersion.h"
  @ONLY
)

# Include directories
include_directories(${SRC_DIR})
include_directories(${LIB_DIR})

# LLVM + LLD from vcpkg manifest (see vcpkg.json). Pin CMake to vcpkg's install so Homebrew LLVM
# is not picked up when CMAKE_PREFIX_PATH / LLVM_DIR point at /opt/homebrew.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  set(_LESMA_VCPKG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  if(EXISTS "${_LESMA_VCPKG_ROOT}/share/llvm/cmake/LLVMConfig.cmake")
    unset(LLVM_DIR CACHE)
    set(LLVM_DIR "${_LESMA_VCPKG_ROOT}/share/llvm/cmake" CACHE PATH "LLVMConfig.cmake from vcpkg"
        FORCE)
  elseif(EXISTS "${_LESMA_VCPKG_ROOT}/lib/cmake/llvm/LLVMConfig.cmake")
    unset(LLVM_DIR CACHE)
    set(LLVM_DIR "${_LESMA_VCPKG_ROOT}/lib/cmake/llvm" CACHE PATH "LLVMConfig.cmake from vcpkg"
        FORCE)
  endif()
  foreach(_LESMA_LLD_CAND
          "${_LESMA_VCPKG_ROOT}/share/lld/cmake/LLDConfig.cmake"
          "${_LESMA_VCPKG_ROOT}/lib/cmake/lld/LLDConfig.cmake")
    if(EXISTS "${_LESMA_LLD_CAND}")
      unset(LLD_DIR CACHE)
      get_filename_component(_LESMA_LLD_DIR "${_LESMA_LLD_CAND}" DIRECTORY)
      set(LLD_DIR "${_LESMA_LLD_DIR}" CACHE PATH "LLDConfig.cmake from vcpkg" FORCE)
      break()
    endif()
  endforeach()
endif()

find_package(LLVM CONFIG REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS core support demangle native orcjit)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

# LLD configuration
find_package(LLD CONFIG REQUIRED)
message(STATUS "Found LLDConfig.cmake in ${LLD_CMAKE_DIR}")
include_directories(SYSTEM ${LLD_INCLUDE_DIRS})
set(LLD_LIBS lldCommon lldELF lldCOFF lldMachO)

# Executable configuration
set(LIB_NAME lesma)

set(COMMON_SOURCES
  src/liblesma/Common/Utils.cpp
  src/liblesma/Frontend/Lexer.cpp
  src/liblesma/Frontend/Parser.cpp
  src/liblesma/Token/Token.cpp
  src/liblesma/Typecheck/AnalysisIndex.cpp
  src/liblesma/Typecheck/Typechecker.cpp
  src/liblesma/Backend/Codegen.cpp
  src/liblesma/Backend/CodegenTypeUtils.cpp
  src/liblesma/Backend/MangleUtils.cpp
  src/liblesma/Symbol/SymbolTable.cpp
  src/liblesma/Symbol/TypeUtils.cpp
  src/liblesma/Driver/Driver.cpp
  )

# Move Lesma Standard Library
file(COPY ${STDLIB_DIR} DESTINATION $ENV{HOME}/.lesma/)

# Libraries required for the compiler itself
set(COMMON_LIBS ${LLVM_LIBS} ${LLD_LIBS} fmt::fmt nameof::nameof)
set(COMMON_INCLUDE ${PLF_NANOTIMER_INCLUDE_DIRS})

# Lesma Library

## Create Library
add_library(${LIB_NAME} ${COMMON_SOURCES})
target_link_libraries(${LIB_NAME} ${COMMON_LIBS})

target_include_directories(${LIB_NAME} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  ${COMMON_INCLUDE}
  ${LLVM_INCLUDE_DIRS}
  ${LLD_INCLUDE_DIRS}
  "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

# CLI
if (LESMA_BUILD_CLI)
  message(STATUS "Building Lesma CLI")
  find_package(CLI11 CONFIG REQUIRED)

  set(CLI_NAME lesmac)
  set(CLI_SOURCES src/cli/main.cpp)

  add_executable(${CLI_NAME} ${CLI_SOURCES})
  target_link_libraries(${CLI_NAME} PRIVATE ${LIB_NAME} CLI11::CLI11)
  set_target_properties(${CLI_NAME} PROPERTIES OUTPUT_NAME ${LIB_NAME})
endif ()

# Tests
if (LESMA_BUILD_TESTS)
  message(STATUS "Building Lesma Tests")

  enable_testing()

  find_package(GTest CONFIG REQUIRED)

  set(TESTS_NAME tests)
  set(TEST_SOURCES tests/test.cpp)

  add_executable(${TESTS_NAME} ${TEST_SOURCES})
  target_link_libraries(${TESTS_NAME} PRIVATE ${LIB_NAME} GTest::gmock GTest::gtest_main)

  find_program(BASH_PROGRAM bash)
  if (BASH_PROGRAM)
    add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_tests.sh ${CMAKE_CURRENT_BINARY_DIR}/lesma)
  endif (BASH_PROGRAM)

  add_test(AllTestsInMain ${TESTS_NAME})
endif ()

# Benchmarks
if (LESMA_BUILD_BENCHMARKS)
  message(STATUS "Building Lesma Benchmarks")

  find_package(benchmark CONFIG REQUIRED)

  set(BENCHMARKS_NAME benchmark)
  set(BENCHMARK_SOURCES benchmark/benchmark.cpp)

  add_executable(${BENCHMARKS_NAME} ${BENCHMARK_SOURCES})
  target_link_libraries(${BENCHMARKS_NAME} PRIVATE ${LIB_NAME} benchmark::benchmark_main)
endif ()

# LSP server (lesma-lsp)
if (LESMA_BUILD_LSP)
  message(STATUS "Building Lesma LSP server")
  set(LSP_INSTALL OFF CACHE BOOL "Skip install for LSP when used as subproject")
  set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "Skip LSP examples")
  include(FetchContent)
  FetchContent_Declare(
    lsp_framework
    GIT_REPOSITORY https://github.com/leon-bckl/lsp-framework.git
    GIT_TAG master
  )
  FetchContent_MakeAvailable(lsp_framework)

  find_package(libgit2 CONFIG REQUIRED)

  set(LSP_SERVER_SOURCES src/lsp/main.cpp src/lsp/DocumentStore.cpp src/lsp/LspCompletion.cpp
      src/lsp/LspUtf16.cpp src/lsp/WorkspaceLesFiles.cpp)
  add_executable(lesma-lsp ${LSP_SERVER_SOURCES})
  target_link_libraries(lesma-lsp PRIVATE ${LIB_NAME} lsp libgit2::libgit2package)
  target_include_directories(lesma-lsp PRIVATE ${lsp_framework_SOURCE_DIR})
endif ()

# CPack
if (LESMA_BUILD_DIST)
  include(InstallRequiredSystemLibraries)

  install(TARGETS ${CLI_NAME} RUNTIME DESTINATION bin)
  install(DIRECTORY ${SRC_DIR}/stdlib DESTINATION .)

  set(CPACK_GENERATOR "TGZ")
  set(CPACK_PACKAGE_VENDOR Lesma)
  set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
  set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
  set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
  set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://lesma-lang.com/")
  set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/tools/docs/src/assets/logo.svg")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
  include(CPack)
endif ()