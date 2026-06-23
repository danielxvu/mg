# FM-FSMONITOR-LITE — incremental, watcher-driven status

## Why

Benchmarked on roll20-private-sheets (37,515 tracked files, clean, warm):

| path | time |
| --- | --- |
| mg `repo_status` (libgit2 full workdir scan) | 179 ms |
| mg `status_buffer` (scan + refs + revwalk) | ~233–303 ms |
| `git status` cold (subprocess, full scan) | 101 ms |
| `git status` **+ fsmonitor** (warm) | **28 ms** |

mg (and gitui — same git2/libgit2) is the *slowest* on the raw scan: libgit2's
workdir walk is ~1.8× git's native C, and ref enumeration is ~100 ms for 3,499
refs. Our async work hides this from the UI (renders in ~0.17 ms off a warm
snapshot), but the **worker still does a full 179–300 ms scan on every change**
— heavy CPU/battery, and it can never reach fsmonitor's 28 ms because libgit2
has no fsmonitor integration (the FM-ASYNC-STATUS "syscall ceiling").

But mg already runs a **recursive fs-watcher that knows which directories
changed**. Feeding that into a *scoped* status — re-examining only the changed
dirs instead of all 37.5k files — turns the per-change cost from O(repo) into
O(changed), landing near (or below) fsmonitor's number **without** needing
libgit2 to add daemon support. This is the one move that makes mg's
slowest-on-paper scan genuinely fast, and the watcher to power it already exists.

## The core idea

Maintain an authoritative **running status set** `S` (path → status entry) on the
monitor thread. Instead of recomputing `S` from scratch each change:

1. **Cold start / fallback:** compute `S` once with a full `repo_status`.
2. **On a worktree change to dirs `D₁..Dₙ`:** run a *scoped* status limited to
   those dirs (libgit2 `git_status_options.pathspec`), then for each `Dᵢ`
   replace `S`'s slice under `Dᵢ` with the scoped result. Scoped status returns
   exactly the still-changed files under `Dᵢ`; any prior `S` entry under `Dᵢ`
   not in the new result has become clean → removed. This "replace subtree"
   reconciliation is correct for add / modify / delete / untracked / rename.
3. The published snapshot is built from `S` + cached branch/refs/recent-commits.

Cost per worktree edit: O(files in the changed dirs) — typically a handful —
instead of 37.5k `lstat`s.

## What falls back to a full rescan (the safety boundary)

Incremental-by-path is only valid when a path's change is *local*. These shift
the baseline for many paths, so they trigger a full `repo_status` (correct, and
far less frequent than worktree edits):

- **`.git/index`** (staging/unstaging changes the staged-vs-HEAD classification).
- **`.git/HEAD`, `.git/refs/**`, `.git/logs/**`** (commit, branch switch, reset,
  merge — the whole tree's baseline moves). Also refreshes the cached
  branch/refs/recent-commits (the ~100 ms ref work) which is otherwise reused.
- **Watcher `degraded_`** (hit the watch/fd limit → the change set is untrusted).
- **`IN_Q_OVERFLOW` / a resync marker** from the watcher.
- **First status after `magit-status` opens** (cold).

So: high-frequency *worktree* edits (editor saves, build output in tracked
dirs, generated files) go incremental; the rarer index/ref mutations do the full
scan they already do today. No regression, pure upside on the churn case.

## Granularity: directories, not files

kqueue reports only *which directory* changed (not the filename); inotify could
give file-level but we design to the lower common denominator. The watcher
reports the **set of changed directories**; scoped status re-examines each.
Dir-level is plenty: re-statusing one directory is microseconds vs the whole
repo.

## Pieces

1. **Engine — `mg::git::repo_status_scoped(repo, std::span<std::string> paths)`**
   → status entries limited to `paths` (workdir-relative) via
   `git_status_options.pathspec`. Same entry shape as `repo_status`.
2. **Watcher — report the changed-dir set.** `wait()` stops coalescing to a
   single `fs_event` and returns the distinct changed dirs (it already has each
   dir from `wd_path_`/`fd_path_`). Overflow/degraded → a distinguished resync
   marker (e.g. an `fs_event` with empty path). Still git-agnostic.
3. **Monitor — incremental reconcile.** Hold `S` (path→entry) + cached
   refs/commits. Per published batch: strip the repo prefix off each changed
   dir; if any is under `.git` (or a resync marker, or `degraded_`) → full
   rescan + refresh the cached refs/commits; else scoped-status each worktree
   dir and patch `S`. Build the snapshot from `S` + cache.
4. **Snapshot build** consumes `S` instead of a fresh full scan.

## The correctness anchor (non-negotiable)

Incremental status is where real fsmonitor implementations grow stale-status
bugs. The gate, mirroring FM-ASYNC-STATUS's "replay == sync" anchor:

- **Property/determinism doctest:** apply a randomized sequence of worktree
  mutations (create / modify / delete / rename / mkdir+file / make-clean-again,
  across nested dirs) and after **each** step assert the incrementally-maintained
  `S` equals a fresh full `repo_status` — byte-for-byte the same entry set. Any
  divergence fails. This pins incremental output to the proven full scan.
- **Scoped-status doctest:** `repo_status_scoped(repo, {dir})` == full
  `repo_status` filtered to `dir`, for tracked/untracked/deleted/nested cases.
- **Fallback doctest:** an index/HEAD change forces a full rescan (assert the
  staged reclassification is picked up).
- **TSan:** the reconcile runs on the monitor thread against the same `S` the
  snapshot copy reads under the existing mutex — must stay race-free.

## Phases (each builds + tests green; macOS + Linux + OFF clean)

1. **Scoped status** — `repo_status_scoped` + its doctest. No behavior change.
2. **Changed-dir reporting** — watcher returns the dir set + resync marker;
   doctests on both backends (Linux-gated where needed). Monitor still does a
   full rescan (no incremental yet) — just consumes the richer events.
3. **Incremental reconcile** — `S` + scoped patching + `.git`/overflow fallback;
   the property-equality anchor test. This is the risk phase.
4. **Snapshot from `S` + cached refs**; benchmark (target: incremental refresh
   ≤ single-digit ms vs 179–303 ms full, on roll20/d20app); TSan + tmux
   (edit a deep file → status updates with the scoped path only; commit → full
   rescan still correct).

## Risk + honest call

Highest-complexity feature so far: a running cache that must never drift from
ground truth. Mitigations: the always-correct full-scan path stays as the
baseline and the fallback for anything ambiguous; the equality-to-full-status
property test is the hard gate; dir-level granularity keeps reconciliation
simple. If phase 3's reconciliation proves fragile, phases 1–2 are independently
useful (scoped status is a reusable primitive) and shippable. Benefit is
concentrated on large repos with frequent worktree churn; small repos already
recompute in ~1 ms so they're unaffected.

## Out of scope

- Incremental **index/staging** status (full rescan on `.git/index`; later).
- Incremental ref/commit data (refreshed on `.git` change).
- A persistent daemon or the git fsmonitor IPC protocol — this is in-process,
  watcher-driven, single repo.
- File-level (vs dir-level) change tracking — dir-level suffices.
