cmake_minimum_required(VERSION 3.21)
project(
  Lesma
  VERSION 0.1
  LANGUAGES CXX
)

# External dependencies
include(cmake/CPM.cmake)

CPMAddPackage("gh:Neargye/nameof@0.10.1")
CPMAddPackage(
  NAME fmt
  GIT_TAG 8.1.1
  GITHUB_REPOSITORY fmtlib/fmt
)
CPMAddPackage(
  GITHUB_REPOSITORY jarro2783/cxxopts
  VERSION 3.0.0
  OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL YES"
)

# C++ Flags and settings
if (CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Debug mode")
elseif (CMAKE_BUILD_TYPE MATCHES Release)
  message(STATUS "Release mode")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_COMPILER "clang++")
add_definitions(${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -frtti)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Directories
get_filename_component(ROOT_DIR ${CMAKE_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(BUILD_DIR ${CMAKE_BINARY_DIR} ABSOLUTE)

# Tests
enable_testing()

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
  add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run_compiler_tests.sh)
endif (BASH_PROGRAM)

include_directories(${SRC_DIR})

# LLVM configuration
find_package(LLVM 13 CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS core support demangle mcjit native orcjit)

# Executable configuration
set(EXE_NAME lesma)

include_directories(${CMAKE_SOURCE_DIR})
add_executable(${EXE_NAME}
  src/main.cpp
  src/LesmaJIT.h
  src/Common/Utils.h
  src/Common/Utils.cpp
  src/Common/LesmaError.h
  src/Frontend/Lexer.h
  src/Frontend/Lexer.cpp
  src/Frontend/Parser.h
  src/Frontend/Parser.cpp
  src/Token/Token.cpp
  src/Token/Token.h
  src/Token/TokenType.h
  src/AST/AST.h
  src/Backend/Codegen.cpp
  src/Backend/Codegen.h
  src/Symbol/SymbolTable.cpp
  src/Symbol/SymbolTable.h
)

target_link_libraries(${EXE_NAME} PRIVATE ${LLVM_LIBS} fmt nameof cxxopts)