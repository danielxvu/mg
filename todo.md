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
- [x] **C2 — EPIC: opaque lines, then modern storage. DONE.**
      ✅ **(a)** accessor API + `struct line` opaque, compile-enforced (PRs
      #22/#23/#24). ✅ **(b)** storage swapped to the `mg.line` C++ library
      (`std::vector<char>`, RAII) under `ENABLE_CPP_UPGRADES`, behind the sealed
      accessors (C2b-1 #25, C2b-2 #26). OFF stays faithful C. User chose the C++
      module over a literal piece table (ltext-contiguous requirement made the
      piece table a poor fit). The line storage is now the home for UTF-8 ops.
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

**Full Magit IN PROGRESS** (user: "full magit; complete what we have first, then
most valuable"). Roadmap + gap analysis vs real magit:
`docs/superpowers/specs/2026-06-20-fm-full-magit-roadmap.md`. Magit source cloned
to `/tmp/magit-src` (re-clone if gone: `git clone --depth 1
https://github.com/magit/magit`). ✅ **FM-C done** (PR #31, branch `fm-c-commit`):
`c` is a commit menu — `c c` commit / `c a` amend / `c e` extend / `c w` reword
(mg.git `commit_amend`/`extend`/`reword`/`head_message`; message buffer prefills
+ dispatches). Next, in priority order (Phase A — complete existing):
- ✅ **FM-S1 done** (stage-all `S` / unstage-all `U`, PR #32, branch
  `fm-s1-stageall`): mg.git `stage_all`/`unstage_all`.
- ✅ **FM-S2 done** (line/region staging — magit's signature, PR #33, branch
  `fm-s2-region`). Engine `stage_region`/`unstage_region` in mg.git build a
  synthetic one-hunk unified-diff patch over the selected line indices
  (unselected `+` dropped, unselected `-` demoted to context) and
  `git_diff_from_buffer`→`git_apply` to the index; unstage uses the reverse
  `index_tree→HEAD` diff so demoted-context lines match the apply baseline.
  Bridge `mg_magit_stage_region`/`unstage_region`. UI: `magit_region()`
  resolves a mark↔point span over MG_LINE_DIFF lines of one hunk into
  `(hunk, path, first_li, last_li)` (li = buffer_idx − hunk_header_idx − 1);
  `s`/`u` consult it first, region beats whole-hunk. Set the mark with C-SPC.
  Verified via pty + system git (stage one of two changes; unstage one of two).
  ⚠ region-discard (`k` on a region) deferred — needs a workdir-baseline
  reverse patch (no workdir tree); follow-up. Per-slice commits FM-S2-1/2/3.
- ✅ **FM-T done** (status sections, PR #34, branch `fm-t-sections`).
  FM-T-1 `upstream_status` engine (`git_branch_upstream` + `git_graph_ahead_
  behind`); FM-T-2 header `Upstream: <name> [ahead N, behind M]`; FM-T-3
  `upstream_commits` revwalk → "Unpulled commits"/"Unpushed commits" sections;
  FM-T-4 TAB folds ANY MG_LINE_SECTION header (folds keyed by title minus the
  " (N)" count; `magit_emit` suppresses a folded body). Engine+bridge via
  doctest (1-ahead/1-behind libgit2 fixture), folding via tmux capture-pane.
- ✅ **FM-B done** (branch create/delete/rename, PR #35, branch `fm-b-branches`).
  mg.git `create_branch`/`delete_branch`/`rename_branch` (git_branch_create at
  peeled HEAD / _delete / _move); bridge `mg_magit_branch_create/delete/rename`.
  UI: `b` is now a prefix submap — `b b` checkout, `b c` create, `b k` delete,
  `b m` rename (create prompts; delete/rename default to the branch at point,
  delete confirms). Engine+bridge doctest; pty-verified create + rename.
- ✅ **FM-Z (push/pop) done** (PR #36, branch `fm-z-stash`). mg.git `stash_push`
  (git_stash_save, signature via git_signature_default + "mg@localhost"
  fallback) / `stash_pop` (git_stash_pop = apply+drop); bridge
  `mg_magit_stash_push`/`stash_pop`. UI: `z` prefix submap — `z z` push (prompts
  for a message), `z p` pop (stash at point else stash@{0}). Engine+bridge
  doctest; pty-verified push→pop round-trip. ⚠ **stash-show deferred** (RET on a
  stash → its diff): an M7-diff-sized slice (diff the stash commit vs its
  parent into a read-only buffer) — documented follow-up.
Phase B (new, highest value): ✅ **FM-L done** (log buffer, PR #37, branch
`fm-l-log`) — mg.git `commit_diff` (commit tree vs first parent / empty tree)
+ full `oid` on `commit_brief`; bridge `mg_magit_log_buffer` (MG_LINE_COMMIT,
oid in path) + `mg_magit_commit_diff`; UI `l` opens *magit-log* (magit-log-mode:
RET→*magit-commit* diff view, g refresh, q close). tmux-verified. ✅ **FM-X done** (reset/revert/merge, PR #38, branch `fm-x-rrm`) — mg.git
`reset_to`(soft/mixed/hard), `revert_commit` (in-memory git_revert_commit →
"Revert" commit), `merge_branch` (analysis → ff / merge-commit; conflicts
abort cleanly); bridge `mg_magit_reset/revert/merge`; UI `X` reset submap
(h/m/s), `V` revert (status: prompt; log: commit at point), `m` merge.
doctest + pty-verified. ✅ **FM-R done** (fetch/pull/push, PR #39, branch `fm-r-remotes`) — mg.git
`fetch_remote`/`push_remote` (current branch)/`pull_remote` (fetch + merge via
the shared `merge_annotated` helper); bridge `mg_magit_fetch/push/pull`; UI
`f` fetch / `F` pull / `P` push (origin). TDD + pty-verified against LOCAL
bare remotes (no network). ⚠ no credentials callback yet → authenticated
ssh/https remotes are a follow-up; local/unauthenticated transports work.

**🎉 EVERYDAY-WORKFLOW ROADMAP COMPLETE** (Phase A + Phase B) — this covers the
daily Magit loop, but it is **NOT full Magit parity** (real gaps audited
2026-06-26, tracked under "Magit-parity gaps vs real Magit" below). ✅ Follow-ups also done
(PR #40, branch `fm-followups`): FM-S2 **region-discard** (`k` on a marked
region → `discard_region` via a GIT_DIFF_REVERSE workdir patch); FM-Z
**stash-show** (`RET` on a stash → `commit_diff "stash@{N}"` in the
*magit-commit* view; also fixed a latent crash — log/commit-view modes now
registered in magit_status init, not lazily in `l`); FM-R **remote auth**
(ssh-agent credentials callback on fetch/push; https/password prompt still a
follow-up). ✅ **Interactive HTTPS auth done too** (PR #41, branch `fm-auth`):
prompt callback threaded C-core→bridge→engine; `magit_cred_prompt` (no-echo
password read) answers libgit2's USERPASS; pure `resolve_userpass` unit-tested;
proven end-to-end via a local 401 server (tmux). **The everyday workflow is
covered — but this is not full Magit parity.** Real gaps remain (reflog, section
cycling, diff controls, deeper transients, …), audited 2026-06-26 and tracked
under "Magit-parity gaps vs real Magit" below.
Per-slice TDD (test_git → test_bridge → magit_cmd.c → pty) + stacked PR.

---
**UTF-8 support — Display+nav COMPLETE** (U1–U4, PRs #27–#30). Spec:
`docs/superpowers/specs/2026-06-20-u1-utf8-foundation-design.md`.
✅ **U1 done** (PR #27, branch `u1-utf8`): `mg.utf8` C++ codepoint layer over
utf8proc — `decode_first`(→cp/bytes/width), `char_width`, `is_word`, `is_space`
+ `extern "C"` bridge (`mg_utf8_decode`/`char_width`/`is_word`). Tested
standalone (11 cases), NOT wired into the C core yet. ⚠ **OFF dir is `build-c`.**
Next slices (each `#ifdef`-gated, OFF unchanged):
- ✅ **U2 done** (PR #28, branch `u2-display`): codepoint video cells (`vtcell`
  typedef) — `display.c` renders UTF-8 glyphs at correct width, cursor column is
  display-width-based. ⚠ extended (horizontally-scrolled) lines still byte-wise
  (`updext`/`vtpute`) — a documented follow-up.
- ✅ **U3 done** (PR #29, branch `u3-cursor`): `forwchar`/`backchar` step whole
  codepoints; `forwdel`/`backdel` delete the byte span of n chars (whole-char
  delete). ASCII byte-identical; UTF-8 verified.
- ✅ **U4 done** (PR #30, branch `u4-classify`): `inword()` classifies by
  codepoint (ASCII→byte table, ≥0x80→`mg_utf8_is_word`); word counters byte-count
  via `fwd_bytes`/`bwd_bytes`; case ops ASCII-only guard; `grabword` whole-char.
  **🎉 Display+navigation UTF-8 target COMPLETE (U1–U4): UTF-8 files open & edit
  cleanly.**
- **Later → Full** (optional, utf8proc has the primitives):
  ✅ **U5 done** — non-ASCII case mapping (PR #42, branch `u5-case`): mg.utf8
  `to_upper`/`to_lower` + `M-u`/`M-l`/`M-c` recase non-ASCII codepoints in place
  (same-byte-length common case; length-changing/multi-char mappings skipped).
  ✅ **U6 done** — extended-line (`updext`) UTF-8 (PR #43, branch `u6-updext`):
  `vtputeuc` + `vt_render_line_ext` render horizontally-scrolled lines by
  codepoint (was byte-wise mojibake); tmux-verified.
  ✅ **U7 done** — grapheme-cluster cursor moves (PR #44, branch `u7-grapheme`):
  mg.utf8 `grapheme_break`/`grapheme_len`/`grapheme_back`; forwchar/backchar +
  forwdel/backdel step/delete whole base+combining clusters; pty-verified on NFD.
  ✅ **U8 done** — NFC normalize command (PR #45, branch `u8-nfc`): mg.utf8
  `normalize_nfc` + `M-x normalize-buffer` rewrites each line to NFC
  (ldelete+region_put_data); pty-verified vs Python NFC.
  Remaining (niche): byte-length-changing case mappings (dotless i, ß→SS —
  splice + undo accounting); combining-mark *display* composition (vtcell model
  holds one cp/cell, so marks are dropped — U2 note).
Build: `cmake --build --preset cpp && ctest --preset cpp` +
`cmake --build --preset c-legacy` (0 warnings). Freeze next branch on `u1-utf8`.

## Magit-parity gaps vs real Magit (audited 2026-06-26)

The everyday-workflow roadmap above is done, and two recent additions landed
since: staged-rename display (`orig_path`, PR #91) and the `*magit-process*`
transparency buffer (`$`, PR #92). What follows is the honest delta vs GNU
Emacs + Magit — verified against the source on 2026-06-26 (0 source hits =
absent). Keep this in sync with the README's "What it doesn't do (vs Magit)".

### High value (everyday polish Magit users feel)
- [x] **FM-REFLOG — reflog view + reset-at-point.** ✅ DONE (landed on neomg):
  `l h` → read-only `*magit-reflog*` (newest-first; RET shows an entry's diff);
  `x s`/`x m`/`x h` resets HEAD to the entry at point (hard confirms), reusing
  `reset_to`. Magit-faithful — no one-key destructive undo; the mode is always chosen.
- [x] **FM-SECTION-CYCLE — global section visibility levels.** ✅ DONE (landed on
  neomg): `M-1`/`M-2`/`M-3` (Magit's `magit-section-show-level-N-all`) collapse to
  headers / show files / expand hunks, reusing the fold/expand state. (S-TAB isn't
  receivable in mg's TUI, so M-1/2/3 is the binding.)
- [x] **FM-DIFF-CTL — diff context + whitespace controls.** ✅ DONE (PR #96):
  `+`/`-` grow/shrink `git_diff_options.context_lines` (applied to display AND the
  staging ops so hunk indices stay aligned); `w` toggles `-w`
  (`GIT_DIFF_IGNORE_WHITESPACE`, display-only — hunk staging disabled while on);
  `Diff: -U<n> -w` header (rendered live from atomic globals). Final review caught
  + fixed a monitor/UI data race on the view config.
  - [ ] **FM-DIFF-REFINE (deferred from FM-DIFF-CTL)** — word-level / refined
    intra-line diff highlighting. The ediff cell-highlight *display* infra
    (`magit_cell_highlighted` / `magit_ediff_ref`) is reusable, but the per-hunk
    word-diff *computation* is new — its own slice.
  - [x] **FM-DIFF-CTL-VIEWS** — ✅ DONE (branch `fm-diff-ctl-views`): the `d`
    diff-view controls (`+`/`-` context, `w` whitespace) now drive `*magit-commit*`
    too. Engine `commit_diff` honors the config; the commit view shows its active
    `Diff: -U<n> [-w]` header; a view-aware `magit_diff_view_refresh()` router
    rebuilds whichever diff buffer is current so the bare keys + the `d` popup work
    in both. ("Log diffs" ARE the commit view — the log buffer has no inline
    diff.) The config stays global (one knob, all views, like Magit). A
    whole-branch review caught + fixed a use-after-free in the in-place refresh
    (bclear→refill→window-fixup is now atomic on every path).
- [x] **FM-TRANSIENT-DEPTH — deepen the argument menus.** ✅ **log slice DONE**
  (PR #98): adds `--author=` / `--grep=` / `--all` infixes to the `l` transient on
  top of `-n`. Sticky (persist like `-n`) and clearable (empty input unsets a VALUE
  infix via a `clear_on_empty` flag, so `-n` keeps "empty keeps 100"); a sticky
  filter combines with `l f` (`git log <filter> -- file`).
  Spec: `docs/superpowers/specs/2026-06-28-fm-transient-depth-design.md`.
  - [ ] **FM-TRANSIENT-PUSH (deferred)** — more push infixes (`--tags`,
    `--dry-run`) + a pull transient with args.
  - [x] **FM-TRANSIENT-DIFF** — ✅ DONE (branch `fm-transient-diff`): `d` in
    *magit-status* opens a live diff-view popup over FM-DIFF-CTL's context /
    whitespace controls (`+`/`-`/`w` inside the popup apply immediately, popup
    stays open; `q` closes). The bare `+`/`-`/`w` keys still work.
  - [x] **FM-TRANSIENT-LOG-MORE** — ✅ DONE (branch `fm-transient-log-more`):
    `--since`/`--until`/`--reverse`/`--merges`/`--no-merges` infixes in the `l`
    transient (keys S/U/v/M/N). Also did the deferred C-ABI cleanup —
    `mg_magit_log_query_buffer` takes a `struct mg_log_query *` instead of 12
    positional params. (Rebase/merge/reset transients remain — separate items.)
  - [x] **FM-LOG-TRANSIENT-KEY (found in FM-TRANSIENT-DEPTH review)** — ✅ DONE
    (branch `fm-log-transient-key`): `l` in `*magit-log*` (`maglogmap`) now opens
    the log transient (bound to `magit_l` / `magit_menu_log`, the same command as
    in `*magit-status*`), so `--author`/`--grep`/`--all`/`-n` and `l l`/`f`/`g`/`r`
    can be tweaked without leaving the log buffer. Matches Magit (`l` overrides
    line-down). Scope: `*magit-log*` only.
  - [x] **FM-TRANSIENT-FETCH** — ✅ DONE (branch `fm-transient-fetch`): `f` opens
    a fetch transient (`--prune`/`--tags`/`--all` infixes); `f f` fetches with the
    toggled args. Completes the network-op trio (P push, F pull, f fetch). Plain
    fetch is now `f f`. `--all` fetches all remotes (argv omits origin).

### Medium value
- [x] **FM-LOG-RICH — ref decoration + coloring.** ✅ DONE (branch
  `fm-log-rich`): log rows show `(HEAD -> master, origin/main, tag: v1)` colored
  by ref kind (HEAD cyan, local green, remote blue, tag yellow) on ALL log paths
  — `%d` on the CLI query path, an engine `decorations()` oid→refs map (libgit2,
  annotated tags peeled) on the default/per-file paths, and a grammar-based
  colorizer joining `*magit-log*` to the per-cell color system. (The entry's old
  "`--all` graph" claim was stale — `--all` + `--graph` already combined since
  FM-TRANSIENT-DEPTH/FM-LP.)
  Spec: `docs/superpowers/specs/2026-07-03-fm-log-rich-design.md`.
  - [ ] **FM-LOG-PICKAXE-FILE (deferred)** — per-file pickaxe UX: `l s`/`l G`
    prompt for an optional file scope (the engine already supports
    `-S<term> -- <file>`).
  - [ ] **FM-LOG-DECORATE-TOGGLE (deferred)** — a `--decorate` on/off infix in
    the `l` transient (decoration is always-on today, like git's default).
- [~] **FM-GIT-CMD — `:` run arbitrary git** (`magit-git-command`). IN PROGRESS
  (branch `fm-git-cmd`): `:` prompts (`eread`), tokenizes (quote-aware, no shell),
  runs captured via the existing `detail::run_git`, records a `$` `proclog` entry,
  shows `*magit-process*`. Spec: `docs/superpowers/specs/2026-06-26-fm-git-cmd-design.md`.
  - [ ] **FM-GIT-CMD-TTY (deferred from FM-GIT-CMD)** — auto-route interactive /
    network subcommands (`push`/`pull`/`fetch`/`rebase -i`) from `:` to the
    tty-inherit path (`git_terminal`); v1 is captured-only (use the dedicated keys
    for those).
  - [ ] **FM-PROCLOG-CAP (deferred from FM-GIT-CMD)** — truncate a captured
    entry's output at N KB with a "… (truncated)" marker so a `git log` or
    `git diff` over a large repo doesn't blow up the `*magit-process*` buffer.
- [x] **FM-SHOW-REFS — `y` refs overview** ✅ DONE (branch `fm-show-refs`): a
  `*magit-refs*` buffer of local branches, remotes, and tags with ahead/behind
  vs HEAD. `RET` shows a ref's tip commit (reuses the log's oid-map machinery),
  `b` opens the branch menu, `g`/`q` refresh/close. Engine `refs_overview()`
  (branch iterator + peel + `git_graph_ahead_behind`); bridge
  `mg_magit_refs_buffer`; editor as the 3rd shared-oid-map user. A whole-branch
  review caught + fixed a stale-cross-buffer-meta hazard: `magit_at_point` AND
  `magit_section_move` are now owner-gated (`magit_meta_bp`).
  Spec: `docs/superpowers/specs/2026-07-03-fm-show-refs-design.md`.
  - [ ] **FM-REFS-CHECKOUT (deferred)** — checkout-at-point from `*magit-refs*`
    rows (`b b` is inert there today: rows carry the tip oid, not a name, and
    the meta gate blocks status-buffer resolution). Needs name-carrying refs
    meta or a refs-local at-point lookup.
  - [ ] **FM-REFS-SECTIONS (deferred)** — refs-local M-n/M-p section nav (it is
    inert from `*magit-refs*` today, since `magit_meta` is status-only).
  - [ ] **FM-REFS-COLOR (deferred)** — color the refs buffer (local green /
    remote blue / tag yellow), reusing the FM-LOG-RICH palette.
  - [ ] **FM-REFS-MORE (deferred)** — per-remote sub-sections; `k` delete-branch
    from the refs buffer; comparison-ref selection; `y` as a transient.
- [ ] **FM-PROCLOG-2 — log hunk/region staging** (FM-PROCESS-LOG follow-up).
  `stage_hunk`/`unstage_hunk`/`stage_region`/`unstage_region`/`discard_region`
  log nothing today; the engine knows the path + hunk, so log them as
  `≈ git apply --cached` / `git apply -R` and close that transparency seam.

### Lower / niche
- [ ] **FM-AM — `magit-am`** (apply mailbox/`.patch` series) + matching sequencer
  states; plus the known interactive-rebase edge (a conflicting squash/fixup
  resolves as a normal pick rather than folding — README "Edges remain").
- [x] **FM-FSEVENTS-EXIT-HANG — MISDIAGNOSED; real cause was a pty-test harness bug.**
  ✅ FIXED (branch `fm-pty-exit-drain`). The "FSEvents teardown hang" was a wrong
  guess (made by a stalled implementer). A sampled backtrace of the hung neomg
  proved it: the MAIN thread was blocked in `ttflush -> write()` to the pty, and
  the monitor thread was idle in a normal `wait()` — teardown was never reached,
  `FSEventStreamStop`/`dispatch_sync_f` never called. Root cause: the pty tests
  stop reading the master fd after `C-x C-c`, so neomg's exit-time redraws fill
  the pty kernel buffer and `write()` blocks, so neomg never processes the quit.
  A differential repro confirmed it (no-drain → wedges; drain → clean exit,
  status 0, in-flight FSEvent and all). There is NO neomg bug (a real terminal
  always drains). Fix: a `quit_neomg()` harness helper that drains the master
  while reaping (SIGKILL only as fallback), used by every pty test; the omitted
  end-to-end `push --dry-run` test is restored and passes.

### Core-editor Emacs fidelity (non-magit)
- [x] **FM-FINDFILE-NEWFILE — open missing-directory paths like Emacs.** ✅ DONE
  (branch `fm-findfile-newfile`): `C-x C-f` on a path whose directory doesn't
  exist no longer blocks with `Missing directory, create?` at open — the buffer
  opens read-write (Emacs-verified, 30.2), and the create-directory prompt moves
  to save (`writeout`). Core `src/file.c` only. Spec:
  `docs/superpowers/specs/2026-07-05-fm-findfile-newfile-design.md`.

  **Emacs-fidelity audit (2026-07-05)** — 3 source surveys, each divergence
  verified against neomg's source AND live behavior / real GNU Emacs 30.2 (not
  asserted). Split into real BUGS (correctness — do first), systemic UX, and
  fidelity gaps.

  _Bugs (correctness — data-corrupting or misleading):_
  - [x] **FM-TRANSPOSE-EOL** — ✅ DONE (branch `fm-transpose-eol`): `C-t` at
    end-of-line moved a char onto the NEXT line (`twiddle` did backdel+forwchar,
    crossing the line) — `ab⏎cd` + `C-e C-t` gave `a⏎bcd`. Now transposes the
    last two chars in place (`ba⏎cd`), like Emacs.
  - [x] **FM-DIRED-SYMLINK** — ✅ DONE (branch `fm-dired-symlink`): `d_makename`
    now truncates a symlink row's name at ` -> ` (type char 'l' at index 2), so
    `f`/RET opens the target and `d`/`x` deletes the symlink (was: empty
    `(New file)`, and delete failed leaving the symlink). Data-loss fixed.
  - [x] **FM-TRANSPOSE-WORDS-ARG** — ✅ DONE (`fm-transpose-words`): removed the
    `n=1` override so `C-u N M-t` drags the word past N words (== Emacs; undo
    reverts). Negative prefix rejected (was a NULL-deref crash).
  - [x] **FM-QUERY-REPLACE-Q** — ✅ DONE (`fm-query-replace-q`): `q` now quits
    query-replace (added to the RET/ESC quit cases), matching Emacs.

  _Systemic UX:_
  - [x] **FM-UNBOUND-UNDEFINED** — ✅ DONE (`fm-unbound-undefined`): unbound keys
    report "<key> is undefined" (via the %k formatter) instead of `Quit`; C-g
    still shows `Quit`. Macro define/replay keep the abort.
  - [ ] **FM-HELP-KEYS** — `C-h k`/`f`/`v`/`m`/`w` missing (`keymap.c cHa[]`);
    pressing them prints `Quit`. At least add `C-h k` (describe-key).

  _Fidelity gaps (missing/inert; ranked by daily impact):_
  - [x] **FM-REGION-HIGHLIGHT** — ✅ DONE (`fm-region-highlight`): the active
    region renders in reverse video (per-window `WMARKED` w_flag bit; set by the
    mark commands incl. M-h, cleared by edit/`C-g`/`M-w`; `display.c` standouts
    the ordered mark..point span via the ediff `MG_HL_BIT` path, neomg-build
    only). A whole-branch review caught + fixed 2 bugs (M-h didn't activate; the
    de-extend pass used stale span state). Deferred minor: a horizontally
    *scrolled* (extended) line doesn't show its region standout (same gap as the
    ediff highlight) — `FM-REGION-EXTLINE`.
  - [x] **FM-KILL-RING** — ✅ DONE (`fm-kill-ring`, #116): bounded ring
    (NKILL=60); `kdelete` rotates instead of freeing; `M-y` (yank-pop) cycles.
    Review caught 2 bugs (KNONE ring pollution; `C-u 0 C-y` stale-region delete).
    Deferred: `C-u N M-y` numeric cycling, browse-kill-ring, persistence,
    clipboard; minor warts `FM-KR-TRANSPOSEPARA-DBLKILL` (transpose-paragraphs
    calls kdelete twice) and `FM-KR-YANK-ARG-MARK` (`C-u N C-y` marks only the
    last copy, so `M-y` after it replaces only that one).
  - [ ] **FM-MINIBUFFER-HISTORY** — `M-p`/`M-n` dead in the minibuffer (`echo.c`
    has no history ring); Emacs cycles per-prompt input history.
  - [ ] **FM-FILENAME-EXPAND** — no `$VAR`/`${VAR}` expansion in typed filenames
    (`fileio.c expandtilde` does only `~`); `$HOME/x` → "Missing directory".
  - [ ] **FM-SYMLINK-VISIT** — `adjustname` always `realpath()`s (`fileio.c:358`),
    so a symlink's buffer is renamed to its target and two links to one file
    collapse to one buffer (no uniquify). Emacs keeps the typed path.
  - [ ] **FM-DABBREV** — `M-/` (dabbrev-expand) unbound.
  - [ ] **FM-RECENTER-CYCLE** — `C-l` only centers; Emacs cycles center/top/bottom.
  - [ ] **FM-GOTO-PREFIX** — `M-g` dead (no goto-line/next-error prefix map).
  - [ ] **FM-AUTOSAVE** — no auto-save (`#file#`) / `recover-file`; crash loses work.
  - [ ] **FM-WINDOW-VSPLIT** — no `C-x 3` side-by-side split (window model is
    stack-only), no `C-x +` balance-windows. (Bigger — needs a column window model.)
  - [ ] **FM-REPEAT** — no `C-x z` / repeat-last-command.
  - [ ] **FM-RECTANGLES** — no `C-x r` (rectangles/registers/bookmarks) at all.
  - [ ] **FM-COMMENT-DWIM** — `M-;` inert (no comment-region/comment-dwim).

  Verified as NON-divergences (mg matches Emacs; do NOT "fix"): backup `~` files,
  `C-h` prefix itself, undo (incl. redo-via-replay), `C-x C-c` per-buffer
  `Save file <name>?`, isearch (`C-s`/`C-r`), "file changed on disk" supersession
  warning, `M-.` ctags, `C-z` suspend, `C-u C-u` quadruple, `M-x` TAB completion,
  buffer-name uniquify (`name<2>`).

### Out of scope by design (NOT gaps to close — documented stance)
- **`forge`** (GitHub/GitLab issues & PRs, Gerrit) — a separate Magit package,
  not core porcelain. neomg targets local git, not host integrations.
- **Elisp extensibility** — neomg is a compiled porcelain; the keymap and
  sections are what's built in. Emacs-style scriptability is out of scope.

## Future goals (not yet scheduled)
- **UTF-8 in the magit porcelain** — neomg is byte-oriented Latin-1; Emacs+Magit
  are full Unicode. Editor display/nav UTF-8 landed (U1–U8), but the magit buffer
  paths assume bytes — a real parity axis for non-Latin-1 repos.
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
  `c3-io`→m9-actions (#18), `c1-text`→c3-io (#19),
  `c3_5-fisdir`→c1-text (#20), `c1_5-text`→c3_5-fisdir (#21),
  `c2a1-line`→c1_5-text (#22), `c2a2-line`→c2a1-line (#23),
  `c2a3-line`→c2a2-line (#24), `c2b1-line`→c2a3-line (#25),
  `c2b2-line`→c2b1-line (#26), `u1-utf8`→c2b2-line (#27),
  `u2-display`→u1-utf8 (#28), `u3-cursor`→u2-display (#29),
  `u4-classify`→u3-cursor (#30), `fm-c-commit`→u4-classify (#31),
  `fm-s1-stageall`→fm-c-commit (#32), `fm-s2-region`→fm-s1-stageall (#33),
  `fm-t-sections`→fm-s2-region (#34), `fm-b-branches`→fm-t-sections (#35),
  `fm-z-stash`→fm-b-branches (#36), `fm-l-log`→fm-z-stash (#37),
  `fm-x-rrm`→fm-l-log (#38), `fm-r-remotes`→fm-x-rrm (#39),
  `fm-followups`→fm-r-remotes (#40), `fm-auth`→fm-followups (#41),
  `u5-case`→fm-auth (#42), `u6-updext`→u5-case (#43),
  `u7-grapheme`→u6-updext (#44), `u8-nfc`→u7-grapheme (#45),
  `fm-rb1-rebase`→u8-nfc (#46), `fm-rb2-seq`→fm-rb1-rebase (#47),
  `fm-rb3a-iexec`→fm-rb2-seq (#48), `fm-rb3b-todo`→fm-rb3a-iexec (#49),
  `fm-a-cherrypick`→fm-rb3b-todo (#50), `fm-tag`→fm-a-cherrypick (#51),
  `fm-pv`→fm-tag (#52), `fm-tag-annotated`→fm-pv (#53),
  `fm-ignore`→fm-tag-annotated (#54), `fm-note`→fm-ignore (#55),
  `fm-wt`→fm-note (#56), `fm-blame`→fm-wt (#57),
  `fm-rb-reword`→fm-blame (#58), `fm-bisect`→fm-rb-reword (#59),
  `fm-sub`→fm-bisect (#60), `fm-lp`→fm-sub (#61),
  `fm-tr-popup`→fm-lp (#62), `fm-ediff-conflict`→fm-tr-popup (#63),
  `fm-ediff-apply`→fm-ediff-conflict (#64), `fm-ediff-2-hunks`→fm-ediff-apply (#65),
  `fm-rb-edit`→fm-ediff-2-hunks (#66),
  `fm-rb-replay-conflict`→fm-rb-edit (#67),
  `fm-ediff-3-panes`→fm-rb-replay-conflict (#68),
  `fm-ediff-4-refine`→fm-ediff-3-panes (#69),
  `fm-ediff-5-cell-color`→fm-ediff-4-refine (#70),
  `fm-ediff-6-sync-scroll`→fm-ediff-5-cell-color (#71),
  `fm-ediff-perf`→fm-ediff-6-sync-scroll (#72),
  `fm-status-perf`→fm-ediff-perf (#73), `fm-log-perf`→fm-status-perf (#74),
  `fm-status-perf2`→fm-log-perf (#75), `fm-async-status-p1`→fm-log-perf (#76),
  `fm-async-status-p2`→fm-async-status-p1 (#77),
  `fm-async-blame`→fm-async-status-p2 (#78),
  `fm-linux-firstclass`→fm-async-blame (#79),
  `fm-linux-parity`→fm-linux-firstclass (#80),
  `fm-gitignore-watch`→fm-linux-parity (#81),
  `fm-status-scoped`→fm-gitignore-watch (#82),
  `fm-status-incremental`→fm-status-scoped (#83),
  `fm-repo-session`→fm-status-incremental (#84). 🎉 **FM-ASYNC-STATUS COMPLETE**
  — status build off the UI thread (233ms→0.17ms warm on the 37.5k-file repo),
  idle self-pipe wake redraws with no keypress, fingerprint-based staleness.
  🎉 **FM-ASYNC-BLAME COMPLETE** (#78) — blame / per-file log run on a job-runner
  thread (request/response, latest-wins, generation stale-drop); the UI pops a
  placeholder and fills on the idle wake. cpp-tsan gate 185/185, 0 races.
  🎉 **FM-LINUX-FIRSTCLASS COMPLETE** (#79) — recursive inotify watcher (nested
  edits + .git/refs|logs wake the monitor; dynamic add, overflow resync, ENOSPC
  degrade, .git/objects ignored); cpp-linux[-tsan] presets; Arch+Alpine CI; install
  rules. Arch/Alpine 188/188 in-container; macOS kqueue path unchanged.
  🎉 **FM-LINUX-PARITY COMPLETE** (#80) — recursive kqueue (macOS parity, the 3
  recursive tests now run on both backends); FIXED a real libgit2/OpenSSL global-
  init data race (per-call init refcount crossing 0↔1 across the monitor+worker
  threads) by holding one global_init for the bridge lifetime; tests/tsan.supp
  filters libgit2 false positives; CI TSan now a required gate. Linux TSan
  188/188, 0 races; macOS 188/188.
  🎉 **FM-GITIGNORE-WATCH COMPLETE** (#81) — watcher skips gitignored trees via a
  libgit2-backed predicate (node_modules/build can't affect status); on ~/src
  d20app 50,975→~1,189 watched dirs, dsp 49,463→~315. fswatch stays git-agnostic
  (std::function predicate); .git always exempt. macOS/Alpine/Linux-TSan 190/190.
  🎉 **FM-FSMONITOR-LITE COMPLETE** (#82 primitive, #83 reconcile) — incremental
  watcher-driven status: monitor holds a status_view cache, scope-patches only
  the changed dirs (repo_status_scoped + apply_status_patch), reuses cached refs;
  .git/root/resync/degraded → full rescan. Per-worktree-change cost ~300ms→~20ms
  (~15x, ≈ git+fsmonitor) on roll20. Property test pins incremental==full;
  194/194 macOS+Alpine+Linux-TSan, 0 races.
  ⛔ **FM-PARALLEL-STATUS — tried, NOT viable, reverted.** Parallelizing the cold
  full scan (fan-out + partitioned repo_status_scoped) ran ~5x SLOWER on roll20
  (~225ms→~1080ms): libgit2 re-reads the full index per partition + serializes on
  its global locks. Confirms the only lever is *not scanning* (incremental), not
  scanning in parallel — same reason git uses fsmonitor, not threads. Serial
  gather stays. (Negative result documented in the FM-FSMONITOR-LITE spec.)
  🎉 **FM-REPO-SESSION COMPLETE** (#84) — reusable mg::git::session (one held
  libgit2 handle, PIMPL across the module boundary); a Phase-0 spike proved a
  reused handle skips the index re-read (16.8ms→0.6ms scoped, ~28x). The monitor
  re-opens the session on .git/full-refresh and reuses it for scoped status on
  worktree edits → warm incremental refresh ~20ms→~0.6ms, below git+fsmonitor.
  Monitor-thread-private → race-free by construction. 195/195 macOS+Alpine+TSan.
  🚧 **FM-GIT-CLI-WRITES (read/write split) — Phase 1 done** (branch
  `fm-git-cli-writes`, stacked on `fm-repo-session`; spec
  `docs/superpowers/specs/2026-06-23-fm-git-cli-writes.md`). Reads stay on
  libgit2 (fast — the session work); *mutations that run hooks / sign /
  authenticate* go through the real `git` so behaviour is exactly git's. P1:
  `detail::run_git` (posix_spawnp, argv array — never a shell — combined
  stdout+stderr captured) + `commit_via_cli`; `commit` and the amend family
  (`commit_amend`/`commit_extend`/`commit_reword` → `git commit --amend`
  [`--no-edit`/`--only`]) now fire pre-commit/commit-msg/post-commit hooks and
  honour `commit.gpgsign`, which libgit2's create/amend skipped entirely. A
  pre-commit hook that rejects now fails the commit and surfaces its output
  (impossible on the old path — the headline). head_message strips git's
  stripspace trailing newline; rev-parse `--short=8` keeps mg's 8-hex oid.
  197/197 macOS+Alpine, OFF build magit-gated. ✅ **P2 done** — merge /
  cherry-pick / revert routed through `git` (so the resulting commit fires
  hooks + is signed); shared `apply_via_cli` maps exit-code→apply_result
  (0=done; non-zero+conflicted index=conflicts with *_HEAD+markers left for
  e o/e t + c c; other non-zero=error). ~190 lines of libgit2
  apply-then-commit-by-hand deleted; merge_annotated kept for pull. 198/198
  macOS+Alpine. ✅ **P3 done** — push/pull/fetch through real git with TUI
  suspend-and-inherit so the user's credential helper / SSH agent / GPG /
  progress all work. Engine `git_terminal(repo,args)` (inherits stdio, no
  redirect; -1 if git unspawnable); bridge `mg_magit_{fetch,push,pull}_cli`;
  C-UI `magit_run_net` does the spawncli dance (ttcooked+tttidy → git on the
  real tty → ENTER pause → ttreinit+ttraw+sgarbf). **Kept the libgit2 fallback**
  (user choice): on -1, fall back to the libgit2 path + cred prompt. Verified by
  a tmux smoke test (P p suspends, live git progress, ENTER, clean redraw, bare
  remote received the push) + file:// unit tests. 200/200 macOS+Alpine, OFF
  clean. 🎉 **FM-GIT-CLI-WRITES COMPLETE (read/write split, P1-P3).** PR #85.
  Magit core (A+B, #31-#41) done; UTF-8 "Full" U5-U8 (#42-#45). **Phase C**
  (honest gap vs real magit; spec
  `docs/superpowers/specs/2026-06-21-fm-phase-c-roadmap.md`): ✅ FM-RB-1
  non-interactive rebase (`r e`/`r u`, #46) · ✅ FM-RB-2 sequencer (pause on
  conflict + `r r`/`r s`/`r a` + Rebasing section, #47) · ✅ FM-RB-3a interactive
  executor engine+bridge (`rebase_interactive` pick/drop/squash/fixup via
  in-memory cherry-pick, #48) · ✅ FM-RB-3b `*git-rebase-todo*` buffer (`r i`,
  p/d/s/f actions, M-n/M-p reorder, C-c C-c run, #49). 🎉 **FM-RB rebase COMPLETE**
  (non-interactive + sequencer + interactive). Next branch freezes on
  `fm-rb3b-todo`. ✅ FM-A cherry-pick (`A`, #50) · ✅ FM-TAG tags (`t t`/`t k` +
  Tags section, #51) · ✅ FM-PV push/pull variants (`P p/f/u`, `F p/r`, #52) ·
  ✅ annotated tags (`t a`, #53). ✅ gitignore (`i`, #54) · ✅ git notes
  (`T`, #55) · ✅ worktrees (`W a`/`W k` + section, #56) · ✅ blame (`B`, #57) ·
  ✅ rebase reword (`w` in todo, #58) · ✅ bisect (`Z` menu + section, #59) ·
  ✅ submodules listing (`Submodules` section, #60) · ✅ log of a file (`l l`/
  `l f`, #61). ✅ **transient popup UI + argument infixes** (#62): magit's
  signature menus — every prefix pops a window of action keys + sticky infixes
  (Push `-f`/`-u`, Log `-n`), built on the `help_help`/`popbuf` pattern, no new
  dep. ✅ **conflict resolution** (#63): `GIT_STATUS_CONFLICTED`→unmerged,
  `conflicts()` + `resolve_conflict(ours|theirs)`, a "Conflicts" status section,
  and `e` conflict transient (`e o`/`e t`, RET to edit) — closes the rebase
  conflict-resume gap. ✅ **merge/revert/cherry-pick leave conflicts** (#64):
  `apply_result`, repo-level `git_merge`/`git_revert`/`git_cherrypick` leave the
  conflicted state, `commit()` honors MERGE_HEAD (2-parent) + clears state — all
  four ops now feed the `e o`/`e t` flow. ✅ **ediff — per-region conflict
  resolution** (#65): `conflict_hunks`/`resolve_conflict_hunk` (2-way + diff3),
  `E` opens a `*magit-ediff*` buffer (a ours / b theirs / RET both per region);
  resolving the last region stages the file. ✅ **interactive rebase `edit`**
  (#66): `rebase_action::edit` + `rebase_result::stopped`; the executor stops at
  an edit, persists the remaining plan to `.git/MG_REBASE_TODO`, and `r r`
  resumes from the amended HEAD (`r a` aborts). ✅ **conflict-resumable
  interactive replay** (#67): a cherry-pick conflict mid-replay now pauses
  (materialize on disk + persist the rest) instead of aborting — resolve via
  `e o`/`e t`, `c c`, then `r r`. 🎉 **Interactive rebase fully complete.**
  ✅ **live 3-pane visual ediff** (#68): `E` opens merged|theirs|ours stacked
  panes with the active region in reverse-video (ENABLE_NATIVE_MAGIT display
  hook), n/p region-step, a/b/RET resolve — the marquee "visual" piece.
  ✅ **word-level refinement** (#69): `refine_words` (word-LCS) marks the words
  unique to each side in the ediff panes — intra-line refinement.
  ✅ **per-cell color** (#70): the refined words now render in real reverse
  video (`MG_HL_BIT` spare cell bit + a no-op-when-absent `uline` hook, all
  `#ifdef ENABLE_NATIVE_MAGIT` so OFF stays byte-clean) — the "needs a display
  rewrite" item, done without one. ✅ **full-version panes + synchronized
  scrolling** (#71): the side panes show the full ours/theirs versions and
  scroll in lockstep with merged (alignment derived from the marker structure,
  no line-diff lib); `C-n`/`C-p`/`C-v`/`M-v` move all three together. 🎉 **ediff
  feature-complete.** Next branch freezes on `fm-ediff-6-sync-scroll`.

  **Phase C status — implemented:** rebase (onto/sequencer/interactive +
  reword), cherry-pick, tags (lightweight+annotated+section), push/pull
  variants, gitignore (`i`), git notes (`T`), worktrees (`W` + section), blame
  (`B`), bisect (`Z` + section), submodules listing (section), log of a file
  (`l f`). The earlier "out of scope" list was an overclaim — all the
  libgit2-feasible items above are now done (#54–#61).

  🎉 **FM-SYNTAX-HIGHLIGHT COMPLETE** (branch `fm-syntax-highlight`, stacked on
  `fm-lp-log-extras`; spec `docs/superpowers/specs/2026-06-25-fm-syntax-highlight-diffs-design.md`):
  Syntax-highlighted diffs across all surfaces — status inline hunks, commit
  diff, and ediff panes. Four layers: (1) Zig 0.16.0 lite tokenizer
  (`syntax/neomg_syntax.zig`, C ABI `neomg_syntax.h`, 9 languages + generic
  fallback) compiled to a static lib via `build.zig`, linked into `mg_magit` by
  CMake; (2) `vtcell` 4-bit color-index field + `ttfgcolor` (terminfo
  `set_a_foreground`, 16-color palette) + `MG_COLOR_SHIFT`/`MG_COLOR_MASK` in
  `display.c`; (3) `magit_cell_color` glue tokenizes each diff line after the
  `+/-/space` gutter and ORs per-kind colors into cells; (4) ediff reconciliation
  composes syntax foreground with `MG_HL_BIT` reverse-video — a syntax+active-
  region cell emits both SGR attributes. `zig` added to Dockerfiles, bench.yml,
  and PKGBUILD. Dockerfiles install `zig` from rolling distro repos
  (0.16.0 at time of writing; a commented tarball fallback is provided for
  hard-pin needs). OFF build: 0 magit/zig
  symbols, no color escapes. macOS + Arch/Alpine green; docker gate left for CI
  (cannot run from task-6 environment).

  🎉 **FM-LP log extras COMPLETE** (branch `fm-lp-log-extras`, stacked on
  `fm-git-cli-writes`; spec `docs/superpowers/specs/2026-06-25-fm-lp-log-extras-design.md`):
  `l g` graph, `l r` commit-range (A..B), `l s`/`l G` pickaxe (-S/-G). One
  CLI-backed `mg::git::log_query` (reuses run_git; parses `git log [--graph]
  [-S|-G] [<range>] --format=%x1f...` into MG_LINE_COMMIT rows, connector lines
  as MG_LINE_OTHER) — graph + pickaxe have no libgit2 equivalent. `l l`/`l f` +
  the status Recent-commits section stay on libgit2. 205/205 macOS+Alpine, OFF
  0 magit symbols. tmux-verified.
  🧪 **FM-ZIG-READ-ENGINE — spike PASSED (GO), candidate milestone.** A custom
  index-driven parallel status walker in **Zig 0.16** beats libgit2's cold
  full-status **2.4–3.1× on every repo measured** (roll20 61ms vs 146ms; d20app
  16ms vs 52ms; transmission 3.9ms vs 12ms). The lesson took 3 iterations: v1
  naive walk = 15× SLOWER (descended gitignored node_modules); v2 +gitignore
  prune = still 9× slower on d20app (descended untracked-but-unignored trees);
  **v3 index-driven** (parse `.git/index`, only descend dirs with tracked
  content, like `git -unormal`) = beats everywhere. Wins by doing git's *same*
  algorithm but parallel + lean. Throwaway spike + write-up in
  `bench/walk-spike/` (`VERDICT.md`). Honest caveats: it's the walk (~90%), not
  a full status engine (correctness tail: gitignore *labeling*, racy-clean
  hashing, submodules/sparse/filters/renames/conflicts); and it's a COLD-path
  win — the warm incremental path (0.8–16ms) already wins for steady editing.
  Spec: `docs/superpowers/specs/2026-06-25-fm-zig-read-engine.md`. (Contrast
  with FM-PARALLEL-STATUS below, which failed *inside* libgit2; this succeeds by
  replacing the read path entirely.)
  **Phase 1a done** (worktree status validated byte-for-byte vs `git status
  --porcelain` on 8 cases: untracked, modified, modified_same_size, deleted,
  untracked_dir, ignored_file, mixed, nested_ignore). Branch: `fm-zig-read-engine`.

  **Phase 2 done** (FM-ZIG-READ-ENGINE cold-path hybrid, default ON in the
  `cpp`/`cpp-linux` presets + `docker/` images; raw CMake option still OFF so a
  bare `cmake` build needs no Zig). Cross-platform win confirmed in CI: full
  status-buffer build ~37% faster on x86_64 Linux (260→164 ms median, 37.5k
  files) and ~21% on macOS; conflicted/v4/submodule/sparse all fail closed to
  libgit2. Default flip carried the `.pic=true` musl fix (`build.zig`) +
  Zig-0.16 provisioning in both Dockerfiles + `bench.yml`; Alpine/musl build
  verified locally.
  T1: Zig C-ABI static lib (`neomg_zig_worktree_status`). T2: CMake
  `ENABLE_ZIG_STATUS` gate (Zig .a linked into `mg_magit` on macOS via Apple-ld
  repack, directly on Linux). T3: `staged_status` (libgit2 X column). T4:
  `hybrid_status` (Zig Y + libgit2 X merged, falls back to `repo_status` on any
  failure or when OFF). T5 (this): cold path in `gather_status_view` routed
  through `hybrid_status` unconditionally (OFF == `repo_status`); benchmarked
  ~1.2× faster on full_buffer (roll20: 194ms ON vs 230ms OFF; d20app: 65ms ON
  vs 82ms OFF); ON+OFF suites both 100% (214/214 ON, 207/207 OFF); OFF has 0
  zig symbols; Linux CI job added (`zig-hybrid` in `.github/workflows/linux.yml`,
  Arch x86_64, installs zig 0.16.0 tarball, builds+ctests ON path). Note:
  `orig_path` (rename source path) is not populated by `hybrid_status` — the
  status buffer renders renames using only `e->path` (destination), so there
  is no visible gap in the status output; documented in bridge.cpp + README.

  **Not done (honest residual — each needs work outside the current model):**
    · FM-CV commit variants: instant fixup/squash (use `r i` for now),
      signoff/no-verify; **GPG sign** (needs an external signing key).
    · conflicting `squash`/`fixup` during interactive replay resolves as a
      normal pick (separate commit), not folded into the previous commit —
      folding-after-resolve would need to re-run the squash on continue. Rare;
      documented in the FM-RB-REPLAY-CONFLICT spec. (Pick/edit/reword replay
      conflicts resume fully, #67.)
    · submodule **add/update/sync** — clone + network (testable via a local bare
      remote, but not yet built); listing only for now.
    · (none — the ediff is feature-complete: full-version panes + synchronized
      scrolling #71, per-character color refinement #70, region nav + highlight
      #68. The "needs a real line-diff" worry was avoided: alignment derives from
      the conflict-marker structure.)
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
