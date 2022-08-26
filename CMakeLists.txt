cmake_minimum_required(VERSION 3.16)

# Get version
find_package(Git)
execute_process(
        COMMAND           ${GIT_EXECUTABLE} describe --tag --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE   LESMA_TAG
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE v "" LESMA_VERSION "${LESMA_TAG}")
string(REPLACE "." ";" VERSION_LIST "${LESMA_VERSION}")
list(GET ${VERSION_LIST} 0 LESMA_VERSION_MAJOR)
list(GET ${VERSION_LIST} 1 LESMA_VERSION_MINOR)
list(GET ${VERSION_LIST} 2 LESMA_VERSION_PATCH)

# Other variables
set(LESMA_URL "https://lesma-lang.com/")

# Project
project(Lesma VERSION ${LESMA_VERSION})

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
include(cmake/CPM.cmake)

CPMAddPackage("gh:Neargye/nameof@0.10.1")
CPMAddPackage("gh:catchorg/Catch2@2.13.8")
CPMAddPackage("gh:CLIUtils/CLI11@2.2.0")
CPMAddPackage(
  NAME fmt
  GIT_TAG 9.0.0
  GITHUB_REPOSITORY fmtlib/fmt
)

# Directories
get_filename_component(ROOT_DIR ${CMAKE_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(BUILD_DIR ${CMAKE_BINARY_DIR} ABSOLUTE)

# Version configuration
configure_file(
        "${SRC_DIR}/liblesma/Common/LesmaVersion.h.in"
        "${SRC_DIR}/liblesma/Common/LesmaVersion.h"
        @ONLY)

# Include directories
include_directories(${SRC_DIR})
include_directories(${LIB_DIR})

# LLVM configuration
set(LLVM_DIR ./llvm/build-release/lib/cmake/llvm)
find_package(LLVM 14 CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS core support demangle mcjit native orcjit)

# Clang configuration
set(Clang_DIR ./llvm/build-release/lib/cmake/clang)
find_package(Clang REQUIRED)

include_directories(SYSTEM ${CLANG_INCLUDE_DIRS})
add_definitions(${CLANG_DEFINITIONS})

set(CLANG_LIBS clangTooling)

# Executable configuration
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

# Libraries required for the compiler itself
set(COMMON_LIBS ${LLVM_LIBS} ${CLANG_LIBS} fmt nameof)

add_executable(${CLI_NAME} ${COMMON_SOURCES} ${CLI_SOURCES})
add_executable(tests ${COMMON_SOURCES} ${TEST_SOURCES})

target_link_libraries(${CLI_NAME}   PRIVATE ${COMMON_LIBS} CLI11)
target_link_libraries(${TESTS_NAME} PRIVATE ${COMMON_LIBS} Catch2::Catch2)

# Tests
enable_testing()

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
  add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run_compiler_tests.sh)
endif (BASH_PROGRAM)

add_test(test_unit_tests tests)

# CPack
include(InstallRequiredSystemLibraries)
install(TARGETS ${CLI_NAME} RUNTIME DESTINATION bin)
set(CPACK_GENERATOR "STGZ;TGZ")
set(CPACK_PACKAGE_VENDOR Lesma)
set(CPACK_PACKAGE_VERSION_MAJOR ${LESMA_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${LESMA_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${LESMA_VERSION_PATCH})
set(CPACK_PACKAGE_HOMEPAGE_URL ${LESMA_URL})
set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/docs/static/img/logo.svg")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")
include(CPack)