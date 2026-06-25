# zigstatus — Phase 1a: worktree status engine

A standalone Zig 0.16 implementation of the **worktree dimension** of `git status`:
untracked files/directories, worktree-modified, and worktree-deleted, with full
gitignore exclusion (including nested `.gitignore` files and `!`-negation).

Output format matches `git status --porcelain` (for the worktree column) exactly.

## Scope (Phase 1a)

Phase 1a covers the **worktree dimension only** — the right-hand column of the
two-character `XY` status code:

| Code | Meaning |
|------|---------|
| `?? path` | Untracked file (or top-level untracked directory collapsed to `dir/`) |
| ` M path` | Worktree-modified (content differs from the index) |
| ` D path` | Worktree-deleted (tracked file absent from the worktree) |

**Explicitly deferred to Phase 1b:** staged/index-vs-HEAD dimension (the `X`
column), conflicts, index v3/v4 extended flags, submodule/sparse-checkout
fallback, and renames. Until Phase 1b, the `X` column is always a space.

## Build

Requires Zig **0.16.0** (pinned in `bench/walk-spike/.tool-versions`).

```
cd src/zigstatus
/opt/local/bin/zig build -Doptimize=ReleaseFast
# binary: zig-out/bin/zigstatus
```

Usage: `zigstatus <repo-root>` — prints `XY path` lines to stdout, one per entry.

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

## Phase 1b (next)

Phase 1b will add:

- **Staged / index-vs-HEAD dimension** (the `X` column): new files added to the
  index, staged modifications, staged deletions.
- **Conflicts** (`UU`, `AA`, `DD`, etc.) from the index conflict-flag bits.
- Index v3/v4 extended flags and submodule/sparse-checkout graceful fallback.

The walk architecture (index-driven parallel descent) is unchanged; Phase 1b adds
a HEAD-tree parse and a second pass over the index entries.
