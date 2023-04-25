cmake_minimum_required(VERSION 3.24)

# Lesma build options
option(LESMA_BUILD_CLI "Build CLI" OFF)
if (LESMA_BUILD_CLI)
  list(APPEND VCPKG_MANIFEST_FEATURES "cli")
endif ()

option(LESMA_BUILD_TESTS "Build Tests" OFF)
if (LESMA_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif ()

option(LESMA_BUILD_DIST "Build Dist" OFF)
if (LESMA_BUILD_DIST)
  list(APPEND VCPKG_MANIFEST_FEATURES "dist")
endif ()

option(LESMA_BUILD_LLVM "Build LLVM" OFF)
if (LESMA_BUILD_LLVM)
  list(APPEND VCPKG_MANIFEST_FEATURES "build-llvm")
endif ()

# Get version from vcpkg
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json VCPKG_MANIFEST)
string(JSON LESMA_VERSION GET ${VCPKG_MANIFEST} version)
set(VERSION ${LESMA_VERSION})

# Set the vcpkg toolchain file
set(VCPKG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/vcpkg")
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")

# Project
project(Lesma VERSION ${VERSION} DESCRIPTION "Lesma Compiler")

# C++ Flags and settings
if (CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Debug mode")
elseif (CMAKE_BUILD_TYPE MATCHES Release)
  message(STATUS "Release mode")
endif ()

set(CMAKE_CXX_STANDARD 17)
add_definitions(${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -frtti)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# External dependencies
find_package(fmt CONFIG REQUIRED)
find_package(nameof CONFIG REQUIRED)

# Directories
get_filename_component(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(LLVM_DIR ${ROOT_DIR}/llvm ABSOLUTE)
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

# Look for local dependencies
if (EXISTS "${LLVM_DIR}" AND IS_DIRECTORY "${LLVM_DIR}") # Prefer local LLVM installation
  message(STATUS "Found local installation of LLVM and Clang")
  set(LLVM_DIR ./llvm/build-release/lib/cmake/llvm)
  set(Clang_DIR ./llvm/build-release/lib/cmake/clang)
endif ()

# LLVM configuration
find_package(LLVM 16 CONFIG REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS core support demangle mcjit native orcjit)

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
  src/liblesma/Backend/Codegen.cpp
  src/liblesma/Symbol/SymbolTable.cpp
  src/liblesma/Driver/Driver.cpp
  )

# Move Lesma Standard Library
file(COPY ${STDLIB_DIR} DESTINATION $ENV{HOME}/.lesma/)

# Libraries required for the compiler itself
set(COMMON_LIBS ${LLVM_LIBS} ${LLD_LIBS} fmt::fmt nameof::nameof)

# Lesma Library

## Create Library
add_library(${LIB_NAME} ${COMMON_SOURCES})
target_link_libraries(${LIB_NAME} ${COMMON_LIBS})

target_include_directories(${LIB_NAME} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
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
  find_package(Catch2 CONFIG REQUIRED)

  set(TESTS_NAME tests)
  set(TEST_SOURCES tests/test.cpp)

  add_executable(tests ${TEST_SOURCES})
  target_link_libraries(${TESTS_NAME} PRIVATE ${LIB_NAME} Catch2::Catch2WithMain)

  enable_testing()

  find_program(BASH_PROGRAM bash)
  if (BASH_PROGRAM)
    add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_tests.sh ${CMAKE_CURRENT_BINARY_DIR}/lesma)
  endif (BASH_PROGRAM)

  add_test(test_unit_tests tests)
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
  set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/docs/static/img/logo.svg")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
  include(CPack)
endif ()