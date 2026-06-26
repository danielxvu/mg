# Local dev convenience wrapper around the CMake build (CMake is the real build
# system for the neomg fork). Provides:
#
#   make install-local                 # build + install -> ~/.local/bin/neomg
#   make install-local PREFIX=~/opt    # ...into ~/opt/bin instead
#   make build                         # just build (-> build/src/neomg)
#   make test                          # build + run the ctest suite
#   make uninstall-local               # remove what install-local laid down
#   make clean                         # remove the build dir
#
# On Linux: append PRESET=cpp-linux.
#
# --- Coexistence with the upstream autotools build -------------------------
# The repo also ships the upstream autotools build (configure.ac / Makefile.am),
# whose CI does `./autogen.sh && ./configure && make`. ./configure generates a
# `Makefile`, but GNU make prefers THIS GNUmakefile over it -- which would
# shadow the autotools build. So: if a generated `Makefile` exists (i.e. the
# tree has been ./configure'd), defer every target to it. The convenience
# targets below are only active on a fresh, unconfigured checkout.

ifneq ($(wildcard Makefile),)

# Configured (autotools) tree: forward everything to the generated Makefile.
MAKEFLAGS += --no-print-directory
.DEFAULT_GOAL := all
%:
	@$(MAKE) -f Makefile $@

else

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

endif
