# Convenience GNU make wrapper around the CMake build (CMake is the real build
# system; this just saves typing for the common local workflow).
#
#   make install-local                 # build + install -> ~/.local/bin/neomg
#   make install-local PREFIX=~/opt    # ...into ~/opt/bin instead
#   make build                         # just build (-> build/src/neomg)
#   make test                          # build + run the ctest suite
#   make uninstall-local               # remove what install-local laid down
#   make clean                         # remove the build dir
#
# The default `cpp` preset is the macOS toolchain (MacPorts clang-21 + zig 0.16,
# native Magit + Zig hybrid status). On Linux: `make install-local PRESET=cpp-linux`.
#
# Named GNUmakefile so GNU make picks it ahead of any autotools-generated
# Makefile (the repo ships an upstream Makefile.am).

PREFIX    ?= $(HOME)/.local
PRESET    ?= cpp
BUILD_DIR ?= build

.DEFAULT_GOAL := build
.PHONY: build install-local uninstall-local test clean

## build: configure (idempotent) + compile via the selected preset
build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET)

## install-local: build, then install the neomg binary + man page into $(PREFIX)
install-local: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)
	@echo "==> installed $(PREFIX)/bin/neomg  (run 'neomg' from any git repo)"

## uninstall-local: delete the files the last install-local wrote
uninstall-local:
	@if [ -f $(BUILD_DIR)/install_manifest.txt ]; then \
		xargs rm -vf < $(BUILD_DIR)/install_manifest.txt; \
	else \
		echo "no $(BUILD_DIR)/install_manifest.txt -- run 'make install-local' first"; \
	fi

## test: build + run the full ctest suite
test: build
	ctest --preset $(PRESET)

## clean: remove the build directory
clean:
	rm -rf $(BUILD_DIR)
