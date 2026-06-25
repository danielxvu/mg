# Spike verdict: custom parallel status walker vs libgit2 — **GO (beats it everywhere)**

**Question:** can a custom parallel walker (Zig 0.16) beat libgit2's cold
full-status across repo shapes — enough to be worth a custom read path (e.g. to
match Sublime Merge's cold speed)?

**Answer: yes — 2.4–3.1× faster than libgit2 on every repo measured**, once the
walker is *index-driven* (only descend directories that contain tracked files,
exactly like `git status -unormal`). It took three iterations to get there, and
the journey is the real lesson.

## The arc (each version fixed what the previous one naively skipped)

| Version | what it added | roll20 | d20app | verdict |
|---|---|---|---|---|
| v1 naive walk | parallel lstat everything | 57 ms | **2,187 ms** | descended gitignored `node_modules` (388k files) → 15× *slower* |
| v2 + gitignore prune | parse .gitignore, skip ignored dirs | 61 ms | 472 ms | still descended 165k untracked-but-unignored files → 9× *slower* on d20app |
| **v3 index-driven** | parse `.git/index`; only descend dirs with tracked content | **58 ms** | **17 ms** | **beats libgit2 everywhere** |
| v3.1 + classify | index mtime/size compare (fair vs libgit2's full status) | 61 ms | 16 ms | classify is ~free; result holds |

Index-driven pruning **subsumes** gitignore pruning *for the walk*: a directory
with no tracked content isn't descended whether it's ignored (`node_modules`) or
merely untracked (`privatesheets/`). gitignore is still needed to *label*
untracked-vs-ignored in the output, but not to decide whether to walk — and
walking is the cost.

## Final numbers (Apple Silicon, 9 runs, median; vs libgit2 `repo_status`)

| Repo | tracked | v3.1 Zig | libgit2 | speedup |
|---|---|---|---|---|
| transmission (1.8k) | 1.8k | **3.9 ms** | 11.9 ms | 3.1× |
| d20app (7k tracked, 68k untracked, 151k ignored) | 7.3k | **16.4 ms** | 51.6 ms | 3.1× |
| roll20 (37.5k) | 37.5k | **61 ms** | 146 ms | 2.4× |

**Why it wins:** it does the *same work* git/libgit2 do (index-driven descent,
stat-compare) but (a) **parallel** across cores — libgit2's walk is serial — and
(b) **lean** — no allocation of a full status-entry list, just counts. Same
algorithm, better constants.

## What this is and isn't

- It **is** a faithful measure of the dominant cost: the index-driven walk +
  lstat + mtime/size classify. Classification added ~0 (the walk dominates).
- It is **not** yet a complete status engine. The correctness tail still to add:
  gitignore *labeling* of the untracked entries (cheap — few entries), content
  hash on racy-clean entries, submodules, sparse-checkout, filters, renames,
  conflicts, index extensions. The walk is the 90%; the tail is real work but
  doesn't change the speed story.

## Leverage (the honest caveat)

This beats libgit2 on the **cold** path (first open / `.git` change). neomg's
**warm incremental** path (scoped rescan on a reused handle, 0.8–16 ms — already
shipped) already beats even this for the common interactive case. So a v3 read
engine would specifically make **cold opens 2.4–3.1× faster** (roll20 146→~60 ms)
— most valuable on huge repos / branch switches, less so for steady editing.

## Recommendation

The "can't beat libgit2 cheaply" conclusion was **wrong** (v1 was a premature
KILL on an incomplete experiment). A custom index-driven parallel walker **does**
beat libgit2 everywhere, by 2.4–3.1×, and Zig is a good fit (clean C ABI for FFI
into neomg's engine; the 0.16 `std.Io` model handles the threaded walk well).

Worth pursuing **if cold-open latency on large repos is a priority**. The build
is: finish the correctness tail, compile the Zig walker as a static lib, FFI it
into the bridge/monitor as the cold-status fast path with libgit2 as the
correctness fallback for the long tail. The warm path stays as-is (it already
wins).

## Toolchain / Zig notes

- Zig **0.14/0.15 cannot link the macOS 26.5 SDK** (`__availability_version_check`
  unresolved); **0.16** works.
- The 0.16 **`std.Io` overhaul** is a large API shift (Io-as-parameter,
  `Io.Group` structured concurrency, the `main(init: std.process.Init)` entry
  point, unmanaged `ArrayList`, fs under `std.Io.Dir`). Written by reading the
  stdlib source on disk, not from memory.
