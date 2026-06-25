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

**Phase 2 gap — `orig_path` (renames/copies):** `hybrid_status` leaves
`file_status.orig_path` empty for staged renames.  The `mg_magit_status_buffer`
renderer in `bridge.cpp` does not read `orig_path` — it renders renames using
only `e->path` (the destination path) — so there is no visible gap in the
status buffer output.  External callers that inspect `orig_path` directly will
see an empty optional for staged renames in the hybrid path.  Populating it
requires `staged_status` to return the old path, and `hybrid_status` to set it;
this is deferred to a future phase.

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
- Index v3/v4 extended flags and submodule/sparse-checkout graceful fallback.
- **`orig_path` for staged renames:** the hybrid path leaves `orig_path` empty.
  The status buffer in `bridge.cpp` does not use `orig_path` (renders renames
  by destination path only), so this is a Phase 2 gap with no visible effect
  on the status buffer output.  External callers that inspect `orig_path`
  directly would need this populated.
