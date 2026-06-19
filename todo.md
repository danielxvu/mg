# mg → modern C++23 refactor — Ralph Loop backlog

This file is the **single source of truth** between agent iterations. Each loop:
pick exactly **one** unchecked task, do the full **TDD cycle** (Red → Green →
Refactor), verify, then make an **atomic commit** and check the task off here.

## Architectural contract (must always hold)

- `ENABLE_CPP_UPGRADES=OFF` (default) → builds exactly like upstream plain C.
- `ENABLE_CPP_UPGRADES=ON` → compiles the modern C++23 modules + their tests.
- `ENABLE_NATIVE_MAGIT=OFF` (default) → no native Git engine; requires upgrades ON.
- New C++ never pollutes the legacy C core: bridge via `extern "C"` / opaque ptrs.
- Prefer: C++20 modules · ranges/views · `std::expected`/monadic `optional` ·
  coroutines for async. No new C-style int return codes; no raw owning pointers.

## Build / test quickref

```sh
# Modern C++ modules + tests:
cmake -B build -DENABLE_CPP_UPGRADES=ON && cmake --build build && ctest --test-dir build
# Faithful legacy C build (default):
cmake -B build-c && cmake --build build-c
```

## Tasks

### Foundation
- [x] **F1 — CMake skeleton + doctest test scaffolding.** Root `CMakeLists.txt`
      with `ENABLE_CPP_UPGRADES` / `ENABLE_NATIVE_MAGIT` options, C++23, CTest, a
      `tests/` subdir fetching doctest, and a sanity test proving the harness +
      `std::expected` both work. _(commit: bootstrap)_
- [ ] **F2 — Faithful legacy C build.** Generate `src/config.h` via CMake feature
      checks (`strlcpy`, `strlcat`, `reallocarray`, `strtonum`, `fparseln`,
      `futimens`, `openpty`, …; mirror `configure.ac`). Build the upstream `mg`
      binary from `src/*.c` with upstream `-W -Wall -Wextra` flags when
      `ENABLE_CPP_UPGRADES=OFF`. Test: binary builds and `mg --version` runs.
- [ ] **F3 — Enable & verify C++20 modules in CMake.** Switch the C++ side to the
      Ninja generator + `CMAKE_CXX_SCAN_FOR_MODULES`; compile a trivial
      `export module mg.probe;` and unit-test importing it. Confirms the module
      toolchain before building real modules on top of it.

### Core engine (each is its own module + extern "C" bridge)
- [ ] **C1 — `mg.text` module: string/tokenization utilities** using
      `std::ranges::views` (split, trim, word-iteration) to replace ad-hoc loops
      in `util.c`/`word.c`. Pure, easily unit-tested first slice.
- [ ] **C2 — `mg.line` PieceTable buffer module.** Replace the legacy
      doubly-linked `struct line` gap/realloc model with a piece-table; expose an
      `extern "C"` shim matching `line.c`'s API so `display.c` is untouched.
- [ ] **C3 — `std::expected` file-IO layer** over `fileio.c`: open/read/write/stat
      returning `expected<…, mg::io_error>`, propagated with `.and_then()`/
      `.or_else()` instead of int codes.

### Native Magit (gated by ENABLE_NATIVE_MAGIT)
- [ ] **M1 — `src/magit/` skeleton + DAG types.** Commit/ref value types, parser
      unit-tested against fixture `git log` output.
- [ ] **M2 — Coroutine background poller.** `co_await`-based non-blocking
      `git status`/log polling feeding the status line.

## Notes for the next iteration
- **Branch:** all refactor work lives on `cpp-refactor` (NOT `master`). Stay on
  the checked-out branch; commit atomically per iteration. PR at milestones.
- doctest is pinned to `v2.4.11` (FetchContent). It emits one harmless CMake
  deprecation warning from its own bundled `cmake_minimum_required` — ignore.
- `tests/CMakeLists.txt` exposes `mg_add_test(name srcs…)`; reuse it per module.
- Next up: **F2** (faithful C build via generated `config.h`).
