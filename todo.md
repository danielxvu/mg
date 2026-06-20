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
  - [x] **M2c** — **libgit2** status reader + summary (pivoted from subprocess;
        libgit2 1.9.4 +threadsafe). Spec:
        `docs/superpowers/specs/2026-06-19-m2c-libgit2-status-design.md`.
    - [x] **M2c-1** — retired M1's porcelain parser (kept `status`/`file_status`
          types); added pure `summarize(span<file_status>) -> string`
          (`"git clean"` / `"git *2 +1 ?3"`). Forward removal; `m1-magit` (PR #2)
          untouched. _(commit: M2c-1)_
    - [x] **M2c-2** — `mg.git` module: RAII-wrapped libgit2 (init guard +
          `unique_ptr` handle owners), `repo_status(path) -> expected<vector<
          file_status>, mg::git::error>` via `git_status_list`. libgit2 linked
          to `mg_magit` via pkg-config. Integration-tested with a libgit2-built
          fixture (no shell git). _(commit: M2c-2)_
  - [ ] **M2d** — background monitor + `extern "C"` modeline bridge. Spec:
        `docs/superpowers/specs/2026-06-19-m2d-bridge-design.md`.
    - [x] **M2d-1** — `monitor` (owns watcher + `std::thread` + `stop_flag` +
          mutex/atomic published state) runs `watch_stream → repo_status →
          summarize`; `extern "C"` bridge (`bridge.h`/`bridge.cpp`):
          `mg_magit_start`/`stop`/`take_dirty`/`modeline(buf,n)`. Cancellation =
          `request_stop()` + `watcher.wake()` + `join`. Tested end-to-end on a
          libgit2 fixture, no C-core edits. _(commit: M2d-1)_
    - [x] **M2d-2** — wired into the C core (first legacy-C edits, all under
          `#ifdef ENABLE_NATIVE_MAGIT`): `main.c` `mg_magit_start(cwd)` +
          `atexit(mg_magit_stop)` + `take_dirty()→sgarbf` by `winch_flag`;
          `display.c` modeline appends `mg_magit_modeline()`. Root CMake links
          `mg→mg_magit` + `LINKER_LANGUAGE CXX`. **Live-update = on-interaction**
          (Magit-faithful). Verified end-to-end: cpp `mg` shows `git *1 ?2` in a
          pty-driven temp repo; OFF `mg` has 0 `mg_magit` symbols. _(commit: M2d-2)_

### Core engine (later — honestly scoped after review)
- [x] **C1 — `mg.text` pure leaf utilities.** `constexpr` char classification
      (`is_word`/`is_ctrl`/`is_upper`/`is_lower`/`is_eosp`/`is_digit`) over a
      256-byte table transcribed byte-for-byte from `cinfo.c` (verified all 256
      identical, quirks preserved) + `next_tabstop` (`ntabstop`). No mutable
      global; usable in constant expressions. Greenfield `mg_text`, not yet
      wired in. _(commit: C1, PR #19)_ Spec:
      `docs/superpowers/specs/2026-06-20-c1-text-utils-design.md`. ⚠ word.c/
      util.c editor commands remain in C (coupled to `curwp`/`curbp`).
- [ ] **C2 — EPIC (multi-iteration): opaque lines, then PieceTable.** ⚠ Review
      found `struct line` is **public** (`def.h:229`) and **24 of ~30 `.c` files**
      poke `l_text/l_used/l_size/lforw/lback` directly — there is no API to hide
      behind, so this is NOT a drop-in module swap. Sub-tasks: (a) introduce an
      accessor API + make `struct line` opaque across all 24 files; (b) only then
      swap the storage model to a piece-table behind that API.
- [x] **C3 — `std::expected` file-IO layer** (`mg.io`). `stat_file`/`read_file`/
      `write_file` → `expected<…, io_error>` (errno-faithful, POSIX + RAII fd
      guard); `read_lines`/`copy_file` compose via `.and_then()`, tests cover
      `.or_else()`. Greenfield `mg_io` module gated by `ENABLE_CPP_UPGRADES`,
      not yet wired into `fileio.c`. 10 doctest cases. _(commit: C3, PR #18)_
      Spec: `docs/superpowers/specs/2026-06-20-c3-expected-fileio-design.md`.

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

## ▶ RESUME HERE (next session)

**🎉 C1 + C3 COMPLETE** — two core modules landed (`mg.text` PR #19, `mg.io`
PR #18), both greenfield/tested/not-yet-wired. The pattern is proven; the next
step that actually advances the refactor is to **wire a modern module into the
C core** (so far everything is parallel + dormant). Directions:
- **C3.5 — wire `mg.io` in** (recommended): give `mg.io` an `extern "C"` bridge
  and route ONE safe `fileio.c` read through it under `#ifdef ENABLE_CPP_UPGRADES`
  (e.g. `startupfile`/config reads, or a slurp helper) — proving the modern
  layer can replace legacy I/O without disturbing the OFF build. The magit work
  (main.c/display.c under `#ifdef`) is the template. Smaller, high-signal.
- **C1.5 — wire `mg.text` in**: route `chrdef.h`'s `ISWORD`/etc. (or `word.c`
  call sites) through `mg.text` under `#ifdef`, retiring the mutable global for
  the upgraded build.
- **C2 — opaque `struct line` → piece table** (the epic): `struct line` public
  (`def.h:229`), ~24 `.c` files poke `l_text`/`l_used`/`lforw`. (a) accessor API
  making it opaque across all files, THEN (b) piece-table storage. Spec first.
Recommend **C3.5** — it converts a dormant module into a real replacement and
de-risks the whole "C++ replaces C" thesis. Build: `cmake --preset cpp &&
ctest --preset cpp`. Freeze the next branch on `c1-text`.

## Future goals (not yet scheduled)
- **UTF-8 support** (user, 2026-06-20). mg is byte-oriented Latin-1 today (C1
  `mg.text` ships a faithful 256-byte table). Eventually needs codepoint-aware
  classification + display width in `mg.text` (a NEW API beside the byte table,
  don't retrofit C1) and multi-byte awareness in column math + the C2 line
  storage. `mg.io` is byte-transparent, unaffected. Flag as a design axis when
  speccing C2.

## Notes for the next iteration
- **Branch / PR workflow:** ongoing work rides the rolling `cpp-refactor` tip
  (NOT `master`); commit atomically per iteration. At each milestone, freeze a
  named branch at that tip and open a **stacked** PR (base = the previous
  milestone's branch, so each PR shows only its own diff). Done so far:
  `foundation`→master (#1), `m1-magit`→foundation (#2), `m2a-fswatch`→m1-magit
  (#3), `m2b-coro`→m2a-fswatch (#4), `m2c-libgit2`→m2b-coro (#5),
  `m2d1-bridge`→m2c-libgit2 (#6), `m2d2-modeline`→m2d1-bridge (#7),
  `m3-status-buffer`→m2d2-modeline (#8), `m4-staging`→m3-status-buffer (#9), `m5-discard`→m4-staging (#10),
  `m6-commit`→m5-discard (#11),
  `m7-diffs`→m6-commit (#12), `m7-hunks`→m7-diffs (#13),
  `m8-nav`→m7-hunks (#14), `m8-sections`→m8-nav (#15),
  `m8-nav2`→m8-sections (#16), `m9-actions`→m8-nav2 (#17),
  `c3-io`→m9-actions (#18), `c1-text`→c3-io (#19).
  Next milestone (C3.5 / wire-in) freezes its branch on `c1-text`.
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
- **Wakeable watcher (2026-06-19)** — replaced the 250ms cancellation heartbeat:
  `mg.fswatch::watcher` now has a blocking `wait()` (no timeout) + thread-safe
  `wake()` (kqueue `EVFILT_USER` / inotify `eventfd`); `watch_stream(watcher&,
  stop_flag)` cancels via `request_stop()` + `wake()`. Fully event-driven, zero
  idle wake-ups. (Forward-evolves M2a/M2b; specs amended.)
- **M2c done** — git access is now libgit2 (structured, no subprocess). `mg.git`
  RAII-wraps the C handles; pattern for wrapping other C libs. libgit2 sets up
  the future commit-DAG (`git_revwalk`) work too.
- **🎉 M2 COMPLETE** — native-magit modeline (`git *N +M ?K`, on-interaction).
- **🎉 M3 COMPLETE** — `*magit-status*` buffer via `M-x magit-status` / `C-x g`
  (gotoline stays in the OFF build + on `M-x goto-line`). Shows branch/HEAD,
  untracked/unstaged/staged sections, recent commits (`git_revwalk`). Verified
  end-to-end via pty. Spec:
  `docs/superpowers/specs/2026-06-19-m3-magit-status-buffer-design.md`.
    - M3-1 read_head + recent_commits (libgit2 refs/revwalk); M3-2 bridge
      `mg_magit_status_buffer` (callback-emit composition); M3-3 the C command +
      funmap/keymap glue, all `#ifdef ENABLE_NATIVE_MAGIT`.
- **🎉 Branch name in the modeline** — `main git *1 ?2`.
- **🎉 M4 COMPLETE — interactive staging.** In the `*magit-status*` buffer:
  `s` stage / `u` unstage the file at point, `g` refresh, `q` close. Verified
  end-to-end via pty (s moves a file Untracked→Staged, buffer refreshes in
  place). Spec: `docs/superpowers/specs/2026-06-19-m4-interactive-staging-design.md`.
    - M4-1 `mg.git` stage()/unstage(); M4-2 enriched emit (kind+path) +
      stage/unstage bridge; M4-3 magit-status-mode keymap + point→file map +
      refresh (re-points windows after bclear).
- **🎉 M7 COMPLETE — inline diffs + hunk staging.** `TAB` expands a file's diff
  inline; `s`/`u` on a hunk's `@@` (or any diff line) stage/unstage just that
  hunk. Engine: `mg.git` `stage_hunk`/`unstage_hunk` via `git_apply` +
  `hunk_cb` filter — stage applies the index→workdir diff to the index; unstage
  applies the *reverse* (index_tree→HEAD_tree, built with `git_diff_tree_to_tree`
  since `git_apply` has no reverse flag). Bridge `mg_magit_stage_hunk`/
  `unstage_hunk`; `magit_cmd.c` routes `s`/`u` on `MG_LINE_HUNK`/`MG_LINE_DIFF`
  to the hunk ops. Verified end-to-end via pty (staged b→B, left k→K unstaged).
  Spec: `docs/superpowers/specs/2026-06-19-m7-diffs-hunks-design.md`.
- **🎉 M8 COMPLETE (breadth)** — ✅ M8-1 `RET` visits the file at point (other
  window; pure C, non-prompting twin of poptofile) · ✅ M8-2 `?` pops a
  read-only `*magit-help*` key legend · ✅ M8-3 stash/branches sections
  (`mg.git` `stashes`/`branches` libgit2 listers; `MG_LINE_STASH`/`_BRANCH`) ·
  ✅ M8-4 section nav `M-n`/`M-p` (`ESC` meta-prefix submap; headers tagged
  `MG_LINE_SECTION`; other meta keys fall through via `rescan`). Spec:
  `docs/superpowers/specs/2026-06-19-m8-magit-breadth-design.md`.
  ⚠ magit keymap entries MUST stay in ascending key order (`doscan` scan) —
  now enforced: `magit_assert_keymap_sorted()` panics on first `C-x g` if not.
- **🎉 M9 COMPLETE — act on stashes/branches.** `a` apply / `k` drop a stash,
  `b` check out a branch. `mg.git` `stash_apply`/`stash_drop`/`checkout_branch`;
  emit carries the stash index (hunk field) + branch name (path); bridge
  `mg_magit_stash_apply`/`stash_drop`/`checkout`. Spec:
  `docs/superpowers/specs/2026-06-20-m9-magit-actions-design.md`.
- **Magit roadmap:** ✅ M5 discard (`k`) · ✅ M6 commit (`c`) · ✅ M7 inline
  diffs + hunk staging · ✅ M8 breadth (RET/?/stash+branches/section-nav) ·
  ✅ M9 act on stashes/branches (a/b/k). Next: M10 (remotes/log) or pivot to
  the core C++23 engine (C3 file-IO recommended).
- **Other:** live-while-idle modeline; auto-refresh the status buffer on fs
  events; merge the PR stack (#1–#8).
- Follow-ups (when needed): C-quoted/special-char paths, `-z` NUL format,
  commit-DAG (`git log`) and refs parsing, recursive worktree watching.
