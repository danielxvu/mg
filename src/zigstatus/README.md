# zigstatus — Phase 2: worktree status engine (FFI, hybrid cold path)

A Zig 0.16 implementation of the **worktree dimension** of `git status`:
untracked files/directories, worktree-modified, and worktree-deleted, with full
gitignore exclusion (including nested `.gitignore` files and `!`-negation).

In **Phase 2** this engine is compiled as a C-ABI static library
(`libneomg_zig.a`) and called from the C++ `mg_magit` module via the
`neomg_zig_worktree_status` FFI symbol.  The **hybrid cold path**
(`mg::git::hybrid_status`) runs this engine for the Y (worktree) column and
libgit2 for the X (staged/index) column, then merges the two.  The cold
`gather_status_view` in `bridge.cpp` calls `hybrid_status` unconditionally;
when `ENABLE_ZIG_STATUS` is OFF at compile time `hybrid_status` falls back to
the full libgit2 `repo_status` — a build without the flag is byte-identical to
before.  The `cpp` / `cpp-linux` presets now enable the flag by default (the
hybrid is the shipping cold-status path); the raw CMake option still defaults
OFF, so a bare `cmake` build needs no Zig.

Output format matches `git status --porcelain` (for the worktree column) exactly.

## Scope (Phase 2)

Phase 2 covers the **worktree dimension** — the right-hand column of the
two-character `XY` status code:

| Code | Meaning |
|------|---------|
| `?? path` | Untracked file (or top-level untracked directory collapsed to `dir/`) |
| ` M path` | Worktree-modified (content differs from the index) |
| ` D path` | Worktree-deleted (tracked file absent from the worktree) |

**`orig_path` (renames/copies):** staged renames are detected and carry their
source path.  `staged_status` runs `git_diff_find_similar` (explicit
`GIT_DIFF_FIND_RENAMES`, so detection is deterministic regardless of the repo's
`diff.renames` config) and `hybrid_status` propagates `orig_path` onto the X
column; the oracle (`repo_status`) mirrors this via
`GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX`.  `mg_magit_status_buffer` renders the
pair Magit-style as `renamed  old -> new` while keeping the row's path on the
destination (so staging/visiting still target the file).  **Worktree (unstaged)
renames are intentionally still shown as a delete+add pair:** the hybrid's Y
column is the Zig walker, which has no content-similarity detector, so enabling
`GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR` in the oracle alone would desync it
from the hybrid (the equivalence tests would fail).

**Still deferred:** conflicts (`UU`/`AA`/`DD`), index v3/v4 extended flags,
submodule/sparse-checkout graceful fallback.

## Build

Requires Zig **0.16.0** (pinned in `bench/walk-spike/.tool-versions`).

### Standalone CLI
```
cd src/zigstatus
/opt/local/bin/zig build -Doptimize=ReleaseFast
# binary: zig-out/bin/zigstatus
```

Usage: `zigstatus <repo-root>` — prints `XY path` lines to stdout, one per entry.

### As the hybrid cold-status path (Phase 2)
```
cmake --preset cpp        # macOS: enables ENABLE_ZIG_STATUS by default
cmake --build --preset cpp #        (MacPorts zig + Apple-ld repack)
ctest --preset cpp        # all tests pass, hybrid path active
```
To build *without* the hybrid (libgit2-only, no Zig needed), pass
`-DENABLE_ZIG_STATUS=OFF`.

On Linux the `.a` links directly (no repack).  The raw `ENABLE_ZIG_STATUS`
CMake option defaults OFF (a bare `cmake` build is byte-identical and needs no
Zig); the `cpp` / `cpp-linux` presets — and the `docker/` images — turn it ON,
so the canonical build ships the hybrid and requires Zig 0.16.

## Validate

```
cd src/zigstatus
python3 test/validate.py
```

The harness builds a git repo in a temp directory for each case, runs both
`git status --porcelain` and `zigstatus`, and compares the sorted output
byte-for-byte. All 8 cases must print `ok`:

| Case | What it exercises |
|------|-------------------|
| `untracked` | Untracked files alongside tracked ones |
| `modified` | Worktree-modified (content change, size change) |
| `modified_same_size` | Racy-clean: same byte length, different content — triggers SHA-1 hash |
| `deleted` | Tracked file removed from worktree |
| `untracked_dir` | Untracked directory collapsed to `dir/` (git -unormal behaviour) |
| `ignored_file` | Root `.gitignore` excludes files and directories |
| `mixed` | Modified + deleted + untracked + ignored in one repo |
| `nested_ignore` | Nested `.gitignore` with `!`-negation re-including a file |

## Zig version

Pinned to **0.16.0**. The implementation uses `std.Io`-based APIs (introduced in
0.16) for file I/O and directory iteration. `/opt/local/lib/zig/std/` is the
authoritative standard library for this build.

## Phase 2 status (current)

Phase 2 is complete.  The X (staged) column is handled by libgit2 in
`mg::git::staged_status`; the hybrid engine merges Zig Y + libgit2 X.  The
staged/index-vs-HEAD logic is therefore in C++, not Zig.

**Remaining deferred items (pure-Zig implementation):**
- **Conflicts** (`UU`, `AA`, `DD`, etc.) from the index conflict-flag bits.
  The hybrid fail-closes a conflicted index to a full libgit2 `repo_status`
  scan, so conflicts render correctly — they're just not handled in Zig.
- Index v3/v4 extended flags and submodule/sparse-checkout graceful fallback.
- **Worktree-rename detection:** unstaged renames show as a delete+add pair
  because the Zig walker has no content-similarity detector (staged renames,
  detected by libgit2 in the X column, render as `old -> new`). Adding this
  would require hashing/comparing blob contents in the walker.
