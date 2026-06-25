# FM-ZIG-READ-ENGINE — a Zig cold-status fast path that beats libgit2

## Why

neomg is **libgit2-capped on the cold read path**. Cold full-status (first open
/ `.git` change / branch switch) is ~146 ms on roll20 (37.5k files) because
libgit2 walks the worktree serially. That's the one place neomg currently feels
slower than it could, and it's the gap vs. Sublime Merge's custom read engine.

The FM-ZIG-READ-ENGINE spike (`bench/walk-spike/`, `VERDICT.md`) proved this is
beatable. A custom **index-driven parallel status walker in Zig 0.16** beats
libgit2 cold status **2.4–3.1× on every repo measured**:

| Repo | Zig walker (v3.1) | libgit2 cold | speedup |
| --- | --- | --- | --- |
| transmission (1.8k) | 3.9 ms | 11.9 ms | 3.1× |
| d20app (7k tracked, big untracked/ignored trees) | 16.4 ms | 51.6 ms | 3.1× |
| roll20 (37.5k) | 61 ms | 146 ms | 2.4× |

It wins by running git's *same* algorithm — index-driven descent (only walk dirs
that contain tracked files, like `git status -unormal`) + stat-compare — but
**parallel across cores** (libgit2 is serial) and **lean** (no status-entry
allocation). The spike's three iterations (naive → +gitignore-prune →
index-driven) are documented in the verdict; only the index-driven version
generalizes across repo shapes.

## Honest leverage (read before committing to this)

This wins on the **cold** path only. neomg's **warm incremental** path (scoped
rescan on a reused libgit2 handle, 0.8–16 ms — already shipped) already beats
even the Zig walker for the common interactive case (a worktree edit). So this
milestone specifically makes **cold opens / branch switches 2.4–3.1× faster** —
most valuable on huge repos and frequent branch switching, marginal for steady
editing.

**Pursue this milestone only if cold-open latency on large repos is a priority.**
It is not free: it adds a new toolchain (Zig) and reimplements the correctness
tail of status. The spike measured the *walk* (~90% of the cost); a shippable
engine must add the rest and validate it byte-for-byte against git.

## Scope

**In:** replace *only* the cold full-status read with a Zig walker, behind a
flag, with libgit2 as the correctness fallback for states the walker doesn't
handle. **Out:** the warm incremental path (libgit2 session stays — it already
wins); writes (stay on the `git` CLI per FM-GIT-CLI-WRITES); log/blame/diff
(stay on libgit2); a general Zig git library.

## The correctness tail (what the spike skipped)

The spike does index-driven walk + lstat + mtime/size classify. A correct status
engine additionally needs, validated against `git status --porcelain=v2` on a
corpus:

- **gitignore *labeling*** of non-tracked entries (untracked vs ignored). The
  walk already prunes them; this only labels the few that surface — cheap, but
  needs a real gitignore matcher (the spike's v2 matcher is a starting point).
- **Racy-clean resolution**: when a tracked file's mtime equals the index
  timestamp, git content-hashes to disambiguate. Rare; must be correct.
- **Conflicted/unmerged** index entries (stage 1/2/3) → conflicts.
- **Index v3/v4** (extended flags / path compression) — the spike handles v2
  only.
- **Submodules, sparse-checkout, `.gitattributes` filters, `assume-unchanged`/
  `skip-worktree`** — detect and **fall back to libgit2** rather than get them
  subtly wrong.
- Emit the same `mg::magit::file_status` records the bridge composes today.

## FFI design

- The walker compiles to a **static library with a C ABI** (Zig's clean C
  interop is the reason to use it here):
  `int neomg_zig_status(const char *repo, void (*emit)(void *ctx, const char
  *path, int xy_state), void *ctx)` — returns 0 / a sentinel meaning "unsupported
  state, use the fallback".
- Called from the engine's cold path (`mg::git::repo_status` /
  `session::status()` full refresh) when the flag is on; on the unsupported
  sentinel (or any error), the caller transparently falls back to the existing
  libgit2 status. So correctness is never worse than today.
- Memory: the walker owns its allocations and emits via callback (no cross-ABI
  ownership transfer), matching the existing `emit` bridge pattern.

## Build integration

- Zig added as a build dependency (`bench/walk-spike/.tool-versions` already pins
  `zig 0.16.0`; mise or a system Zig). CMake invokes `zig build-lib` to produce
  the static lib, linked into `mg_magit`. Gated by an option
  (`ENABLE_ZIG_STATUS`, default OFF) so the default build is unaffected.
- AUR PKGBUILD gains `zig` as a `makedepends`.
- ⚠ Toolchain risk: Zig 0.16 is bleeding-edge (the spike already hit a
  0.14/0.15-can't-link-macOS-26.5-SDK wall; 0.16 churns its `std.Io` API). Pin
  the version; expect API maintenance.

## Cross-platform

The spike used `std.Io.Dir` (abstracts `getdirentries`/`getdents`) + `statFile`,
so the walker is portable macOS/Linux. Must verify on Linux (the inotify
platform) and add it to the Linux CI bench. Windows is out of scope (neomg is
POSIX).

## Phases (revised 2026-06-25 — hybrid architecture)

0. ✅ **Spike (gate): PASSED.** Index-driven parallel walk beats libgit2
   2.4–3.1×. (`bench/walk-spike/`.)
1a. ✅ **DONE** — **Zig worktree-status dimension** (modified/deleted/untracked +
   gitignore), validated byte-for-byte vs `git status --porcelain` on 8 cases.
   Standalone Zig, no FFI. (`src/zigstatus/`, PR #88.)
2. **Hybrid integration** (the path chosen over a full pure-Zig reimplementation —
   see the revision note below): FFI the Zig **worktree** walk (the win) into
   neomg's cold-status path, and compute the **staged (X) column** with libgit2's
   `git_diff_tree_to_index` (cheap, mature). Merge into `file_status`; validate
   the hybrid result byte-identical to libgit2's full `git_status_list`; libgit2
   full-status fallback on any unsupported state. C-ABI static lib + CMake
   `ENABLE_ZIG_STATUS` (default OFF).
3. **Wire into the monitor's `full_refresh`**; TSan; Linux verification + CI;
   benchmark the real cold-refresh win end to end.

Each phase: the libgit2 path stays the fallback, so neomg is never less correct
than today; the win is purely cold-path latency.

## Revision (2026-06-25): hybrid over a full pure-Zig reimplementation

The original Phase 1 imagined reimplementing **all** of status in Zig — including
the staged (index-vs-HEAD) column. Scoping it revealed that the staged column
needs git's **object database** (resolve HEAD → tree objects, which are usually
**packed** → zlib + packfile + delta resolution): a multi-week subproject — and
**not where Zig wins**. The spike's 2.4–3.1× came from the *parallel worktree
lstat walk*; the staged column is a cheap, serial, in-memory diff once the tree
is read, and reading packed trees is exactly what libgit2 already does fast.

Measured (Apple Silicon): the staged-only column (`git diff-index --cached HEAD`,
no worktree walk) is **~10 ms and ~constant** on both roll20 (37.5k) and d20app —
vs libgit2's full cold status (146 ms / 52 ms). So:

> **Hybrid** = Zig worktree walk (≈61 ms parallel, the win) **+** libgit2 staged
> column (≈10 ms, independently runnable) ⇒ still **~2–2.4× faster than libgit2's
> full status**, with **zero object-DB reimplementation**.

Decision: **do not reimplement git's object DB in Zig.** Use Zig for the
expensive worktree walk it wins, and libgit2 (already linked) for the cheap
staged column + the long tail. This gets the cold-status win into the editor
sooner and keeps the correctness tail on battle-tested libgit2. (A full pure-Zig
engine remains possible later if a libgit2-free status is ever a goal in itself —
but it buys no speed.)

## Risks + honest call

- **The correctness tail is the real cost** (the spike skipped it). Status has a
  long tail; getting it subtly wrong is worse than slow. Mitigated by
  byte-for-byte validation + libgit2 fallback for anything unhandled.
- **New toolchain** (Zig, bleeding-edge) in the build + packaging — real friction.
- **Cold-path-only leverage** — the warm path already wins, so the user-visible
  benefit is first-open/branch-switch latency on big repos. Worth it there;
  marginal otherwise.
- If pursued and it works, neomg's cold status drops ~2.5–3× (roll20 146→~60 ms)
  and the read path is no longer libgit2-capped — closing the last measured gap
  vs. Sublime Merge.
