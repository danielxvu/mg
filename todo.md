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

## Toolchain decision (resolved 2026-06-19)

- **C side:** Apple Clang 21 (system default) builds all legacy `src/*.c`.
- **C++ side:** **MacPorts `clang-21`** (`/opt/local/bin/clang++-mp-21`). Apple's
  Clang ships **no `clang-scan-deps`**, so CMake cannot build C++20 named modules
  with it — verified, it refuses to generate. MacPorts mainline LLVM has the
  scanner + full module support. Major version 21 matches Apple Clang ⇒ aligned
  libc++ ABI when the MacPorts-built C++ objects link the Apple-built C objects.
- **Install (privileged, user-run):** `sudo port install clang-21`
  → driver `clang++-mp-21`, scanner `clang-scan-deps-mp-21`.
- **Status:** ⏳ install pending. F2 is toolchain-independent and proceeds now;
  F3 onward (anything with `import`/`export module`) needs clang-21 present.

## Build / test quickref

```sh
# Faithful legacy C build (default, Apple Clang):
cmake -B build-c && cmake --build build-c
# Modern C++ modules + tests (needs MacPorts clang-21):
cmake -B build -DENABLE_CPP_UPGRADES=ON \
      -DCMAKE_CXX_COMPILER=/opt/local/bin/clang++-mp-21 -G Ninja
cmake --build build && ctest --test-dir build
```

## Tasks

### Foundation
- [x] **F1 — CMake skeleton + doctest test scaffolding.** Options, C++23, CTest,
      doctest fetch, sanity test (harness + `std::expected`). _(commit 80dec5e)_
- [ ] **F2 — Faithful legacy C build.** _(toolchain-independent — do next.)_
      Generate `src/config.h` via CMake feature checks mirroring `configure.ac`:
      `HAVE_STRLCPY/STRLCAT/REALLOCARRAY/STRTONUM/FPARSELN/FUTIMENS/OPENPTY/`
      `LOGIN_TTY/…`. For each missing symbol, compile the matching `lib/*.c`
      fallback (macOS lacks `strtonum`, `fparseln`). Build `mg` from `src/*.c`
      with upstream `-W -Wall -Wextra -Wno-unused-parameter` when
      `ENABLE_CPP_UPGRADES=OFF`. **Smoke test:** `mg -h` exits 0 and prints
      `usage:` (NOT `--version` — mg has no version flag; getopt = `hnRb:f:u:`).
- [ ] **F3 — MacPorts clang-21 toolchain + verify C++20 modules.** _(needs
      `sudo port install clang-21`.)_ Drive the C++ side with `clang++-mp-21` +
      Ninja + `CMAKE_CXX_SCAN_FOR_MODULES`. Compile a trivial `export module
      mg.probe;` and unit-test importing it. Gate the module path so the C-only
      OFF build still works under Apple Clang.

### Native Magit — FIRST real module (gated by ENABLE_NATIVE_MAGIT)
> Promoted ahead of the core-buffer work: it is **greenfield** (zero coupling to
> `def.h`) so it validates the whole pipeline — module + bridge + TDD — on a
> clean slate. Needs F3 (modules) first to be a true `export module`.
- [ ] **M1 — `src/magit/` DAG value types + porcelain parser.** `mg.magit`
      module: commit/ref/oid value types; parse `git log --pretty`/`git status
      --porcelain` into them. Unit-tested against checked-in fixture strings (no
      live git needed). Pure logic — ranges/views for tokenization, `std::expected`
      for parse errors.
- [ ] **M2 — Coroutine background poller.** `co_await`-based non-blocking
      `git status`/log polling feeding the status line via an `extern "C"` shim.

### Core engine (later — honestly scoped after review)
- [ ] **C1 — `mg.text` pure leaf utilities.** Only genuinely free-standing
      helpers (split/trim/char-class) that take **no** editor state. ⚠ Most of
      `word.c`/`util.c` is coupled to `curwp`/`curbp` + line internals — do NOT
      claim a clean lift of those; extract only the pure bits.
- [ ] **C2 — EPIC (multi-iteration): opaque lines, then PieceTable.** ⚠ Review
      found `struct line` is **public** (`def.h:229`) and **24 of ~30 `.c` files**
      poke `l_text/l_used/l_size/lforw/lback` directly — there is no API to hide
      behind, so this is NOT a drop-in module swap. Sub-tasks: (a) introduce an
      accessor API + make `struct line` opaque across all 24 files; (b) only then
      swap the storage model to a piece-table behind that API.
- [ ] **C3 — `std::expected` file-IO layer** over `fileio.c`: open/read/write/stat
      returning `expected<…, mg::io_error>`, chained with `.and_then()`/`.or_else()`.

## Review log
- **2026-06-19 (post-F1 review):** verified toolchain & milestones against source.
  Found: (A) Apple Clang can't build C++20 modules → MacPorts clang-21 decision;
  (B) C2 piece-table "display.c untouched" was false (24 files touch line guts) →
  re-scoped as an epic; (C) F2 `--version` smoke test invalid → use `mg -h`;
  (D) C1 over-claimed purity → narrowed. Reordered to put greenfield magit first.

## Notes for the next iteration
- **Branch:** all refactor work lives on `cpp-refactor` (NOT `master`). Stay on
  the checked-out branch; commit atomically per iteration. PR at milestones.
- doctest pinned `v2.4.11` (FetchContent); one harmless CMake deprecation warning
  from its own bundled `cmake_minimum_required` — ignore.
- `tests/CMakeLists.txt` exposes `mg_add_test(name srcs…)`; reuse it per module.
- **Next up: F2** (faithful C build via generated `config.h`) — unblocked now.
