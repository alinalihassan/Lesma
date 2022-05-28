cmake_minimum_required(VERSION 3.16)
project(Lesma)

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
CPMAddPackage("gh:CLIUtils/CLI11@2.1.2")
CPMAddPackage(
  NAME fmt
  GIT_TAG 8.1.1
  GITHUB_REPOSITORY fmtlib/fmt
)

# Directories
get_filename_component(ROOT_DIR ${CMAKE_SOURCE_DIR} ABSOLUTE)
get_filename_component(SRC_DIR ${ROOT_DIR}/src ABSOLUTE)
get_filename_component(LIB_DIR ${ROOT_DIR}/lib ABSOLUTE)
get_filename_component(BUILD_DIR ${CMAKE_BINARY_DIR} ABSOLUTE)

include_directories(${SRC_DIR})
include_directories(${LIB_DIR})

# LLVM configuration
find_package(LLVM 13 CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS core support demangle mcjit native orcjit)

# Clang configuration
find_package(Clang REQUIRED)

include_directories(${CLANG_INCLUDE_DIRS})
add_definitions(${CLANG_DEFINITIONS})

set(CLANG_LIBS clangTooling)

# Executable configuration
set(CLI_NAME lesma)

set(COMMON_SOURCES
  src/liblesma/Common/Utils.cpp
  src/liblesma/Frontend/Lexer.cpp
  src/liblesma/Frontend/Parser.cpp
  src/liblesma/Token/Token.cpp
  src/liblesma/Backend/Codegen.cpp
  src/liblesma/Backend/DebugInfo.cpp
  src/liblesma/Symbol/SymbolTable.cpp
  src/liblesma/Symbol/SymbolType.cpp
)
set(CLI_SOURCES src/cli/main.cpp)
set(TEST_SOURCES  tests/test.cpp)

# Libraries required for the compiler itself
set(COMMON_LIBS ${LLVM_LIBS} ${CLANG_LIBS} fmt nameof)

add_executable(${CLI_NAME} ${COMMON_SOURCES} ${CLI_SOURCES})
add_executable(tests ${COMMON_SOURCES} ${TEST_SOURCES})

target_link_libraries(${CLI_NAME} PRIVATE ${COMMON_LIBS} CLI11)
target_link_libraries(tests       PRIVATE ${COMMON_LIBS} Catch2::Catch2)


# Tests
enable_testing()

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
  add_test(test_lesma_sources ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run_compiler_tests.sh)
endif (BASH_PROGRAM)

add_test(test_unit_tests tests)

# CPack
install(TARGETS ${CLI_NAME} RUNTIME DESTINATION bin)
set(CPACK_ABSOLUTE_DESTINATION_FILES ${ROOT_DIR}/dist)
include(CPack)