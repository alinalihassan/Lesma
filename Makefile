# Lesma Makefile
# Shortcuts for common development tasks

.PHONY: all debug release configure-debug configure-release build test clean \
        format tidy fix lint help

# Default target
all: debug

# =============================================================================
# Build targets
# =============================================================================

## Configure and build debug
debug: configure-debug
	cmake --build build/Debug

## Configure and build release
release: configure-release
	cmake --build build/Release

## Configure debug (uses CMake presets)
configure-debug:
	cmake --preset Debug

## Configure release (uses CMake presets)
configure-release:
	cmake --preset Release

## Build without reconfiguring (debug)
build:
	cmake --build build/Debug

## Build release without reconfiguring
build-release:
	cmake --build build/Release

# =============================================================================
# Test targets
# =============================================================================

## Run tests (debug)
test: debug
	ctest --test-dir build/Debug --output-on-failure

## Run tests (release)
test-release: release
	ctest --test-dir build/Release --output-on-failure

# =============================================================================
# Code quality targets
# =============================================================================

## Format all source files with clang-format
format:
	@echo "Formatting source files..."
	@find src -name '*.cpp' -o -name '*.h' | xargs clang-format -i
	@echo "Done."

# Detect OS for platform-specific settings
UNAME_S := $(shell uname -s)

# Clang-tidy extra args (fixes stdlib path mismatch between Apple Clang and Homebrew LLVM)
ifeq ($(UNAME_S),Darwin)
  SDKROOT := $(shell xcrun --show-sdk-path)
  TIDY_EXTRA_ARGS := -extra-arg=-isystem -extra-arg=$(SDKROOT)/usr/include/c++/v1 \
                     -extra-arg=-isystem -extra-arg=$(SDKROOT)/usr/include
else
  TIDY_EXTRA_ARGS :=
endif

## Run clang-tidy checks (no fixes)
lint:
	@echo "Running clang-tidy..."
	@run-clang-tidy -p build/Debug $(TIDY_EXTRA_ARGS) -header-filter='src/.*' 'src/.*\.cpp$$' 2>&1 | tail -100

## Run clang-tidy and apply fixes
tidy:
	@echo "Running clang-tidy with fixes..."
	@run-clang-tidy -p build/Debug $(TIDY_EXTRA_ARGS) -header-filter='src/.*' -fix 'src/.*\.cpp$$'
	@echo "Done."

## Full fix: run tidy + format
fix: tidy format
	@echo "All fixes applied."

# =============================================================================
# Clean targets
# =============================================================================

## Clean debug build
clean:
	rm -rf build/Debug

## Clean release build
clean-release:
	rm -rf build/Release

## Clean all builds
clean-all:
	rm -rf build

# =============================================================================
# Help
# =============================================================================

## Show this help
help:
	@echo "Lesma Makefile"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Build targets:"
	@echo "  debug            Configure and build debug (default)"
	@echo "  release          Configure and build release"
	@echo "  build            Build debug without reconfiguring"
	@echo "  build-release    Build release without reconfiguring"
	@echo "  configure-debug  Configure debug only"
	@echo "  configure-release Configure release only"
	@echo ""
	@echo "Test targets:"
	@echo "  test             Run tests (debug)"
	@echo "  test-release     Run tests (release)"
	@echo ""
	@echo "Code quality:"
	@echo "  format           Format code with clang-format"
	@echo "  lint             Run clang-tidy (no fixes)"
	@echo "  tidy             Run clang-tidy and apply naming fixes"
	@echo "  fix              Run tidy + format"
	@echo ""
	@echo "Clean:"
	@echo "  clean            Clean debug build"
	@echo "  clean-release    Clean release build"
	@echo "  clean-all        Clean all builds"
