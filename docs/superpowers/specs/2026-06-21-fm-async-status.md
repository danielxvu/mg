# FM-ASYNC-STATUS — background the status-buffer build off the UI thread

## Why (and why only this)

After the perf pass, the synchronous status rebuild is ~84ms on a 60k-commit
repo (fine) but ~234ms on a 3,499-ref / 37k-file monorepo (felt lag). The
dominant terms are irreducible per call: `repo_status` 147ms (single-threaded
workdir scan) + `branches`/`tags` ~80ms (per-op refdb iteration). The only way
to stop the UI blocking on them is to compute the buffer **off the UI thread**.

This is the project's highest-risk change (concurrency around a single-threaded
editor + interruptible terminal input), so it's scoped as a deliberate project,
not a quick win. It is **always-on and self-tuning**: on a small repo the
snapshot recomputes in ~1ms so the UI is effectively synchronous; on a big repo
it's non-blocking with brief, bounded staleness.

## Threading model (the safe core)

The **only** safe sharing is *immutable data*, produced on a worker and consumed
on the UI thread. No mg buffer/window/line is ever touched off the UI thread.

- Reuse the existing `monitor` thread (bridge.cpp) — it already runs
  `repo_status` off-thread for the modeline with its own libgit2 handle (proven
  safe; libgit2 is thread-safe across separate `git_repository` handles).
- The monitor produces a **status snapshot**: the *collapsed* status-buffer line
  stream (no inline expansions) as `vector<{string line; int kind; string path;
  int hunk}>`, plus an `epoch` (see below). Published by building a fresh vector
  and swapping it under a `std::mutex`. The UI copies it out under the lock.
- Cold start: the first `magit-status` before any snapshot exists computes
  **synchronously once** (blocks ~150ms on the big repo), then the monitor keeps
  it warm.

## UI build (magit_build) — replay + splice

`magit_build` no longer calls the synchronous `mg_magit_status_buffer`. Instead:
1. copy the latest snapshot;
2. replay its lines into the buffer; when a line is a file entry
   (UNSTAGED/STAGED/UNTRACKED) whose path is in the *expanded* set, compute that
   one file's diff inline (`file_diff`, ~4–16ms, on the UI thread) and splice it.

Expansions stay on the UI thread because they're cheap and depend on UI state;
the expensive base (status scan + revwalk + ref enum) is the snapshot. The
expansion-interleaving problem is solved by splice-on-replay.

## The crux: staleness after the user's own mutation

When the user stages/commits via the bridge, the snapshot is briefly stale until
the monitor recomputes. Decision after weighing options:

**Optimistic render + async reconcile (real-magit behavior).** Render the latest
snapshot instantly (possibly one mutation stale), and converge to fresh within
~150ms:
- Every bridge **mutation** (stage/unstage/commit/discard/…) bumps a global
  atomic `mutation_epoch` and nudges the monitor to recompute immediately (the
  fs watcher also fires on the `.git/index` write; the explicit nudge cuts
  latency). The snapshot records the epoch it was built at.
- `magit_build` renders the snapshot regardless; if `snapshot.epoch <
  mutation_epoch` it also sets a "stale" marker in the header (e.g. a `…`) so the
  brief lag is legible, and the reconcile redraw clears it.

Rejected alternatives: (a) *synchronous on user actions* — keeps the ~150ms lag
we're trying to remove; (b) *local snapshot patching* (move the staged file
ourselves) — duplicates git's status logic, fragile.

## UI wake (the trickiest plumbing)

The main loop checks `mg_magit_take_dirty()` once per iteration, then blocks in
`getkey`→`ttwait`→`poll`. A snapshot that becomes ready while the UI is idle
must redraw **without** a keypress: `ttwait`'s `poll` must include a wake fd the
monitor can signal.

**Use the most efficient pollable wake per platform — not a self-pipe.** The
fswatch module already abstracts exactly this primitive for its own thread;
expose a *pollable* wake fd from it and add it to `ttwait`'s `poll` set:
- **Linux:** `eventfd` (1 fd, 8-byte counter, no pipe buffer) — already used by
  the inotify backend.
- **macOS/BSD:** a dedicated `kqueue` fd + `EVFILT_USER`, triggered with
  `NOTE_TRIGGER`; a kqueue descriptor is itself `poll`-able, so its fd goes in
  the set with no data-pipe. (`watcher::wake()` already triggers EVFILT_USER.)
- A self-pipe is the portable fallback only where neither exists.

This reuses the existing wake mechanism (fewer fds, no pipe buffer, no
duplicated code) instead of inventing a self-pipe.

Notes:
- `poll()` with 2-3 fds is the right multiplexer; epoll/kqueue-as-multiplexer
  win only at hundreds of fds, so do **not** "upgrade" `ttwait`.
- Strictly `#ifdef ENABLE_NATIVE_MAGIT` in ttyio.c so the OFF build's `poll` set
  is byte-identical (the established gating discipline).
- The wake fd is owned by the monitor (created in `mg_magit_start`, closed in
  `mg_magit_stop`), exposed to ttyio via a small accessor.

## Invariants (must hold)

1. No mg buffer/window/line touched off the UI thread.
2. UI never blocks on `repo_status` except the cold-start build.
3. Eventually consistent: the buffer converges to the latest on-disk state
   within ~one recompute (~150ms) of any change, including the user's own.
4. OFF (`c-legacy`) build byte-identical: every hook `#ifdef`-gated; 0 magit
   symbols.

## Test plan

- **Determinism (doctest):** the snapshot replayed == the synchronous
  `mg_magit_status_buffer` output for the same repo+expanded set (same lines).
  This is the correctness anchor — async must produce identical content.
- **Thread safety:** a TSan build of a test that spins `start`/refresh/`stop`
  while a worker mutates the repo (stage/commit in a loop) — zero races.
- **tmux end-to-end:** (a) open status on the big repo → instant render, no
  block; (b) stage a file → converges to staged within a beat (stale marker
  then clears); (c) external `git` change while idle → screen updates with no
  keypress (self-pipe wake); (d) `q`/stop cleans up the thread + pipe.
- **Perf:** re-profile — `magit_build` UI-thread time should drop to the replay
  + expansion cost (single-digit ms) on both big repos; the 147/234ms moves to
  the worker.

## Phased implementation (each phase builds + tests green, OFF byte-clean)

1. **Snapshot capture** — monitor builds + stores the collapsed snapshot
   (refactor `mg_magit_status_buffer`'s composition so the monitor and the sync
   path share it); add `mg_magit_status_snapshot(repo, expanded, n, emit, ctx)`
   that replays + splices. Doctest: replay == sync output.
2. **UI consumes snapshot** — `magit_build` calls the snapshot path; cold-start
   falls back to sync. (Still updates only on keystroke — already non-blocking.)
3. **Mutation epoch + nudge** — mutations bump epoch + signal recompute; stale
   marker in the header.
4. **Self-pipe wake** — ttyio `poll` includes the pipe; idle updates redraw.
5. **TSan + tmux validation; re-profile.**

## Risk + honest call

Highest-risk change in the project: shared-state concurrency + interruptible
input + shared core (ttyio/display) edits. Mitigations: immutable-snapshot
pattern (no shared mutable editor state), reuse of the proven monitor thread,
strict `#ifdef` gating, a TSan gate, and the determinism test pinning async
output to the sync output. Benefit is concentrated on large/pathological repos;
small repos are unaffected (snapshot ~1ms). If at phase 3–4 the wake plumbing or
staleness UX proves fragile, phases 1–2 alone still remove the *cold-path* block
and are shippable on their own.

## Syscall ceiling (what async does and doesn't change)

Async **moves** the ~37k-`lstat` `repo_status` scan off the UI thread; it does
not make the scan itself cheaper. The real syscall-level wins git core uses --
**fsmonitor** (query a daemon for changed paths, skipping `lstat` of unchanged
files) and the **untracked cache** (skip dirs by unchanged mtime) -- are gated
by libgit2 (≤1.9 has no fsmonitor; partial untracked-cache). We already set
`GIT_STATUS_OPT_UPDATE_INDEX` (the tracked-file stat cache libgit2 does support).
So off-thread is the lever we have; a faster scan would need libgit2 support or
recursive worktree watching (FSEvents/fanotify) to do incremental status -- both
separate, larger efforts.

## Out of scope

- Backgrounding `blame`/`log_file` (same pattern, separate project).
- Local snapshot patching for zero-staleness mutations.
- A general worker pool — one monitor thread suffices.
- A faster status *scan* (fsmonitor / incremental) — libgit2-gated; see above.
