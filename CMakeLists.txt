cmake_minimum_required(VERSION 3.16)

# Include other Cmake files
include(cmake/CPM.cmake)
include(cmake/VersionFromGit.cmake)

# Get version
version_from_git(INCLUDE_HASH OFF)

# Project
project(Lesma VERSION ${VERSION})

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
CPMAddPackage("gh:Neargye/nameof@0.10.1")
CPMAddPackage("gh:catchorg/Catch2@3.1.0")
CPMAddPackage("gh:CLIUtils/CLI11@2.2.0")
CPMAddPackage(
  NAME fmt
  GIT_TAG 9.0.0
  GITHUB_REPOSITORY fmtlib/fmt
)

# Directories
get_filename_component(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(LLVM_DIR ${ROOT_DIR}/llvm ABSOLUTE)
get_filename_component(STDLIB_DIR ${SRC_DIR}/stdlib ABSOLUTE)
get_filename_component(BUILD_DIR ${CMAKE_BINARY_DIR} ABSOLUTE)

# Version configuration
configure_file("${SRC_DIR}/liblesma/Common/LesmaVersion.h.in" "${SRC_DIR}/liblesma/Common/LesmaVersion.h" @ONLY)

# Include directories
include_directories(${SRC_DIR})
include_directories(${LIB_DIR})

# Look for local dependencies
if (EXISTS "${LLVM_DIR}" AND IS_DIRECTORY "${LLVM_DIR}") # Prefer local LLVM installation
  message(STATUS "Found local installation of LLVM and Clang")
  set(LLVM_DIR ./llvm/build-release/lib/cmake/llvm)
  set(Clang_DIR ./llvm/build-release/lib/cmake/clang)
endif()

# LLVM configuration
find_package(LLVM 14 CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS core support demangle mcjit native orcjit)

# Clang configuration
find_package(Clang REQUIRED)

include_directories(SYSTEM ${CLANG_INCLUDE_DIRS})
add_definitions(${CLANG_DEFINITIONS})

set(CLANG_LIBS clangTooling)

# Executable configuration
set(LIB_NAME lesma_lib)
set(CLI_NAME lesma)
set(TESTS_NAME tests)

set(COMMON_SOURCES
  src/liblesma/Common/Utils.cpp
  src/liblesma/Frontend/Lexer.cpp
  src/liblesma/Frontend/Parser.cpp
  src/liblesma/Token/Token.cpp
  src/liblesma/Backend/Codegen.cpp
  src/liblesma/Symbol/SymbolTable.cpp
  src/liblesma/Symbol/SymbolType.cpp
  src/liblesma/Driver/Driver.cpp
)
set(CLI_SOURCES src/cli/main.cpp)
set(TEST_SOURCES  tests/test.cpp)

# Move Standard Library
file(COPY ${STDLIB_DIR} DESTINATION $ENV{HOME}/.lesma/)

# Libraries required for the compiler itself
set(COMMON_LIBS ${LLVM_LIBS} ${CLANG_LIBS} fmt nameof)

# Lesma Library

## Options
### Shared/Dynamic or Static library?
option(LESMA_BUILD_SHARED_LIBS "Build libraries as shared as opposed to static" ${BUILD_SHARED_LIB})

if (LESMA_BUILD_SHARED_LIBS)
  set(LESMA_LIB_TYPE SHARED)
else()
  set(LESMA_LIB_TYPE STATIC)
endif()

## Create Library
add_library(${LIB_NAME} ${LESMA_LIB_TYPE} ${COMMON_SOURCES})
target_link_libraries(${LIB_NAME} ${COMMON_LIBS})

target_include_directories(${LIB_NAME} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

# Lesma executables
add_executable(${CLI_NAME} ${CLI_SOURCES})
add_executable(tests ${TEST_SOURCES})

target_link_libraries(${CLI_NAME}   PRIVATE ${LIB_NAME} CLI11)
target_link_libraries(${TESTS_NAME} PRIVATE ${LIB_NAME} Catch2::Catch2WithMain)

# Tests
enable_testing()

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
  add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_compiler_tests.sh)
endif (BASH_PROGRAM)

add_test(test_unit_tests tests)

# CPack
set(LESMA_URL "https://lesma-lang.com/")

include(InstallRequiredSystemLibraries)

install(TARGETS ${CLI_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY ${SRC_DIR}/stdlib DESTINATION .)

set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_VENDOR Lesma)
set(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_HOMEPAGE_URL ${LESMA_URL})
set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/docs/static/img/logo.svg")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
include(CPack)