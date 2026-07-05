# FM-DIFF-CTL-VIEWS — diff-view controls in the commit view

## Why

FM-DIFF-CTL / FM-TRANSIENT-DIFF gave `*magit-status*` live diff-view controls —
`+`/`-` context and `w` ignore-whitespace, via bare keys and the `d` popup. But
they only affect the status buffer's expanded diffs. `commit_diff` — the engine
behind `*magit-commit*` (reached via `RET` on a log / refs / stash row, and
revert/cherry-pick previews) — calls `git_diff_tree_to_tree(…, nullptr)`, so the
commit view is stuck at context 3 with whitespace always shown. The "log diffs"
in the todo item ARE this commit view (the log buffer shows no inline diff), so
the commit view is the single place to close the gap.

## Decided model (from brainstorming)

The diff-view config stays global (one `magit-diff-context` knob for all diff
views, like Magit). Two parts: make `commit_diff` honor it (engine), and make
the existing controls drive the commit view with an in-place refresh (editor).

## Architecture

### 1. Engine (`src/magit/git.cppm`, `commit_diff` ~4061)

Build a `git_diff_options` reading the diff-view atomics and pass it to
`git_diff_tree_to_tree` (mirroring `file_diff` at ~4038):
```cpp
    git_diff_options dopts;
    git_diff_options_init(&dopts, GIT_DIFF_OPTIONS_VERSION);
    dopts.context_lines =
        static_cast<uint32_t>(g_diff_context.load(std::memory_order_relaxed));
    if (g_diff_ignore_ws.load(std::memory_order_relaxed))
        dopts.flags |= GIT_DIFF_IGNORE_WHITESPACE;
    // ... git_diff_tree_to_tree(&raw_diff, r.get(), parent_tree.get(),
    //                            tree.get(), &dopts) ...
```
Whitespace-ignore is pure display here — the commit view is read-only, so there
is no staging correctness concern (unlike `file_diff`, whose ignore-ws path
blocks staging).

### 1b. Bridge (`src/magit/bridge.cpp`, `mg_magit_commit_diff` ~1202)

Emit the same `Diff: -U<n> [-w]` config header the status buffer shows (from
bridge.cpp:727), after the `commit <rev>` header, when the config is non-default
— so the commit view visibly reflects its active context/whitespace setting (and
gives the user confirmation when they adjust it in place):
```cpp
    out_no_path(std::string("commit ") + rev, MG_LINE_SECTION, -1);
    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv = "Diff:     -U" +
            std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        out_no_path(dv, MG_LINE_OTHER, -1);
    }
    // ... existing Note / hunk emission ...
```
This mirrors the status format verbatim (extract a tiny shared helper if it
reads cleanly; otherwise the two-line duplication is acceptable and localized).

### 2. Editor (`src/magit_cmd.c`)

The existing `magit_diff_more/less/ws` and the `d` popup all refresh via
`magit_refresh`, which rebuilds `*magit-status*` **by name** — so bound in the
commit view they would rebuild the wrong buffer. Introduce a view-aware router.

- **Remember the rev.** A static `char magit_commit_rev[256]`. Factor
  `magit_show_rev` into:
  - `magit_commit_build(struct buffer *bp, const char *rev)` — the existing
    body (bclear, `magit_commit_bp = bp`, `magit_cell_color_reset()`,
    `magit_commit_meta_count = 0`, `mg_magit_commit_diff(...)`), plus
    `strlcpy(magit_commit_rev, rev, ...)`. Pure buffer rebuild, no window ops.
  - `magit_show_rev(rev)` — calls `magit_commit_build`, then `popbuf` + sets the
    window dot + mode (as today).

- **The router** `magit_diff_view_refresh(struct buffer *target, int f, int n)`:
  - if `target == magit_commit_bp` and `magit_commit_rev[0] != '\0'`: rebuild
    `*magit-commit*` via `magit_commit_build(bp, magit_commit_rev)` and mark the
    windows showing it dirty (the `for (wp … w_rflag |= WFFULL)` loop, like
    `magit_reflog_build`), so it redraws in place;
  - else: `magit_refresh(f, n)` (status — byte-identical to today).

- **Reroute the controls.** `magit_diff_more/less/ws` call
  `magit_diff_view_refresh(curbp, f, n)` (curbp is the status/commit buffer when
  a bare key fires). The `d` popup (`magit_diff_transient`) calls
  `magit_diff_view_refresh(stbp, f, n)` — **`stbp`, the buffer saved when the
  popup opened**, because inside the loop `curbp` is the transient popup itself.
  The popup's existing `stwp->w_rflag |= WFFULL` still forces the underlying
  window's redraw (status or commit).

- **Bind the controls in `magcommitmap`** (today only `q`): add `+`/`-`/`w`
  (existing `magit_plus`/`magit_minus`/`magit_w` PFs) and `d` (`magit_d` →
  `magit_diff_transient`). `KEYMAPE(1)` → `(5)`, both counts, entries in strict
  ascending order: `'+'`(43) `'-'`(45) `'d'`(100) `'q'`(113) `'w'`(119).

## Error handling / edges

- `magit_commit_rev` empty (no commit shown yet): the router's commit branch is
  guarded → falls through to `magit_refresh` (harmless; only reachable if the
  controls fire outside any commit view, which the keymap prevents anyway).
- A remembered rev that no longer resolves (history rewritten under the buffer):
  `magit_commit_build` → `mg_magit_commit_diff` returns 0 → the existing
  "No diff for <rev>" path; the buffer shows the message, no crash.
- Status behavior is unchanged: from `*magit-status*` the router always picks
  `magit_refresh`, so the diff still rebuilds exactly as before.
- The global config means adjusting context in the commit view also changes the
  next status diff — intended (one knob), and matches Magit.

## Testing

- **Engine** (`test_git`): a fixture commit whose diff has a hunk with context
  around it. `set_diff_view(1, …)` then `commit_diff` yields fewer context lines
  than `set_diff_view(5, …)` (assert the emitted hunk line count grows with
  context, or that a specific far-context line appears only at 5). A
  whitespace-only change + `set_diff_view(ctx, true)` drops the hunk. Reset the
  global config at test end so other tests are unaffected.
- **Bridge** (`test_bridge`): after `set_diff_view(5, false)`,
  `mg_magit_commit_diff` emits a line starting `Diff:     -U5`; at the default
  `set_diff_view(3, false)` no `Diff:` line is emitted (reset after).
- **Editor** (`test_editor`, pty): `RET` a commit → `*magit-commit*`; press `+`
  and confirm the `Diff:` config header now appears in the commit buffer (it is
  absent at the default context 3, present at 4 after `+`), proving the commit
  view rebuilt with the new setting. `w` toggles the `-w` suffix. Single
  session, drained exit via `quit_neomg`. (`make_repo`'s single commit is enough
  — the header reflects the setting, independent of the diff's content.)

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (recorded in todo.md)

- Per-buffer independent context (the knob stays global).
- Applying the config to any other diff surface (there is none beyond status +
  commit today).
- A diff-args transient in the commit view beyond the shared `d` popup.
