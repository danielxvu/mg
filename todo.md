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
- **Status:** ✅ clang-21 (21.1.8) installed; C++20 named-module build verified
  end-to-end (F3). ⚠ **scan-deps gotcha:** CMake auto-probes a *suffix-free*
  `clang-scan-deps` but MacPorts ships `clang-scan-deps-mp-21`. Point CMake at
  the real one: `/opt/local/libexec/llvm-21/bin/clang-scan-deps` (baked into the
  `cpp` preset — just use the preset).

## Build / test quickref  (use the CMake presets — they encode the toolchains)

```sh
# Modern C++23 + modules + tests (MacPorts clang-21, Ninja):
cmake --preset cpp && cmake --build --preset cpp && ctest --preset cpp
# Faithful legacy plain-C build (Apple Clang, ENABLE_CPP_UPGRADES=OFF):
cmake --preset c-legacy && cmake --build --preset c-legacy
```

## Tasks

### Foundation
- [x] **F1 — CMake skeleton + doctest test scaffolding.** Options, C++23, CTest,
      doctest fetch, sanity test (harness + `std::expected`). _(commit 80dec5e)_
- [x] **F2 — Faithful legacy C build.** `cmake/config.h.in` + `src/CMakeLists.txt`
      feature-detect the host (`check_function_exists`/`check_include_file`
      mirroring `configure.ac`), generate `config.h` into the build tree, compile
      the default source set + `lib/` fallbacks for missing libc funcs (macOS
      needs only `reallocarray`), and link the termcap/curses cascade. Upstream
      `-W -Wall -Wextra -Wno-unused-parameter` → **0 warnings**. `mg` builds under
      both Apple Clang (c-legacy) and clang-21 (cpp). **Smoke test:** `mg -h`
      exits 0 + prints `usage:`. _(commit: F2)_
- [x] **F3 — MacPorts clang-21 toolchain + verify C++20 modules.** Added
      `CMakePresets.json` (`cpp` = clang-21+Ninja+scan-deps; `c-legacy` = Apple
      Clang). `tests/mg.probe.cppm` (`export module mg.probe;`) + `test_modules`
      import it green; `mg_add_module_test()` helper added for future modules.
      OFF path still configures under Apple Clang with no modules. _(commit: F3)_

### Native Magit — FIRST real module (gated by ENABLE_NATIVE_MAGIT)
> Promoted ahead of the core-buffer work: it is **greenfield** (zero coupling to
> `def.h`) so it validates the whole pipeline — module + bridge + TDD — on a
> clean slate. Needs F3 (modules) first to be a true `export module`.
- [x] **M1 — `mg.magit` working-tree status parser.** `export module mg.magit`
      at `src/magit/magit.cppm` (module library `mg_magit`, gated by
      `ENABLE_NATIVE_MAGIT`, enabled in the `cpp` preset). Value types
      (`status` enum, `file_status`, `parse_error`) + `parse_status_line` /
      `parse_status` returning `std::expected`; `std::views::split` for lines.
      11 doctest cases (XY codes, rename `->`, multi-line, malformed). 0 warnings.
      Commit-DAG (`git log`) + refs are later slices. _(commit: M1)_  See spec
      `docs/superpowers/specs/2026-06-19-m1-magit-status-parser-design.md`.
- [ ] **M2 — Coroutine background poller** (decomposed; integration model:
      **background thread + SIGWINCH-style dirty flag**, mirroring mg's
      `winch_flag`/`main.c:251`). Spec:
      `docs/superpowers/specs/2026-06-19-m2a-fswatch-design.md`.
  - [x] **M2a — `mg.fswatch`** OS-abstracted, event-driven watcher: kqueue
        (macOS/FreeBSD/OpenBSD/NetBSD) + inotify (Linux), compile-time selected,
        one `std::expected`/RAII/move-only interface (`create`/`wait`/`fd`).
        4 doctest slices on the kqueue backend (create, event, timeout, ENOENT).
        ⚠ inotify backend written but **untested on this host** — Linux CI only.
        _(commit: M2a)_
  - [x] **M2b** — coroutine layer. Hand-rolled `mg::generator<T>` (clang-21
        libc++ lacks C++23 `<generator>`) + `watch_stream(watcher, stop_flag,
        timeout)` in `mg.fswatch` turning the watcher into a lazy `co_yield`
        event stream. ⚠ Pivoted cancellation from `std::stop_token` to a
        header-only `mg::stop_flag` (shared `atomic<bool>`): `<stop_token>`
        mis-links through a module's global fragment on clang-21 (toolchain bug,
        reproduced). 4 slices (generator finite/infinite, watch_stream
        stopped/event) + stop_flag. _(commit: M2b)_
  - [ ] **M2c** — run `git status --porcelain` on a fired event, summarize via
        M1's `parse_status` into a modeline string.
  - [ ] **M2d** — `extern "C"` bridge + minimal C-core hook: a `std::thread`
        (+ `mg::stop_flag` & RAII join — not `std::jthread`, see M2b) runs
        `for (auto ev : watch_stream(...))`, on each event sets a dirty flag
        (checked by `winch_flag` in `main.c`) and publishes the summary a
        modeline getter returns (`display.c`/`modes.c`). First real C↔C++ bridge.

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
- **2026-06-19 (F3):** did F3 before F2 — clang-21 had just been installed, so
  verifying the module pipeline was the highest-value de-risk and it unblocks
  M1. F2 (faithful C build) is independent and remains the open foundation task.
  Module build order confirmed working: dyndep scan → mg.probe.cppm.o → importer.
- **2026-06-19 (F2):** faithful C build done. Only macOS libc delta is
  `reallocarray` (→ `lib/reallocarray.c`); all other AC_REPLACE_FUNCS link.
  `def.h:13` includes `config.h` via a TAB, so it reaches every TU. Foundation
  (F1–F3) complete; next is the first real feature module, **M1 (mg.magit)**.

## Notes for the next iteration
- **Branch / PR workflow:** ongoing work rides the rolling `cpp-refactor` tip
  (NOT `master`); commit atomically per iteration. At each milestone, freeze a
  named branch at that tip and open a **stacked** PR (base = the previous
  milestone's branch, so each PR shows only its own diff). Done so far:
  `foundation`→master (#1), `m1-magit`→foundation (#2), `m2a-fswatch`→m1-magit
  (#3). Next milestone (M2b…) freezes a branch based on `m2a-fswatch`.
- doctest pinned `v2.4.11` (FetchContent); one harmless CMake deprecation warning
  from its own bundled `cmake_minimum_required` — ignore.
- `tests/CMakeLists.txt` exposes `mg_add_test(name srcs…)` and, for modules,
  `mg_add_module_test(name SOURCES … MODULES …)`. Reuse per module.
- **Module pipeline is live** — real modules (M1 `mg.magit`) can be built now.
- **Foundation F1–F3 + F2 all complete.** OFF path (c-legacy) builds upstream
  mg; ON path (cpp) builds mg + C++ modules + tests. Both green.
- **M1 done** — `mg.magit` status parser is the first live feature module.
  Pattern for future modules: module library under `src/<name>/`, PUBLIC
  CXX_MODULES file set, test binary links the library and `import`s it.
- **M2a done** — `mg.fswatch` watcher (kqueue verified on macOS). Pattern for
  platform code: compile-time `#if` backend selection inside the module's global
  fragment, one public interface. ⚠ The **inotify** backend is unverified on
  this Mac (no Linux headers) — do not assume it compiles until Linux CI runs.
- **M2b done** — `mg::generator<T>` + `watch_stream`. ⚠ **Toolchain landmine:**
  standard headers with libc++ runtime symbols (e.g. `<stop_token>`) can
  mis-link through a module's global fragment on clang-21 — prefer header-only
  primitives across module boundaries (`<atomic>`, `<expected>`, ranges are safe).
- **Next up: M2c** — run `git status --porcelain` on a watch event and summarize
  via M1's `parse_status` into a modeline string.
- M1 follow-ups (when needed): C-quoted/special-char paths, `-z` NUL format,
  commit-DAG (`git log`) and refs parsing.
