# FM-REPO-SESSION — stateful repo handle to lift the index-read floor

## Why

Every `mg.git` function is stateless: `git_repository_open_ext` → read the index
→ do the work → close. For a long-lived editor whose monitor thread runs the
*same* queries over and over, that re-opens the repo and re-reads the full index
on every call. It's the root of every throughput limit we hit:

- the **~20 ms incremental floor** (FM-FSMONITOR-LITE: a scoped status still
  re-reads the whole 37.5k-entry index),
- the **~225 ms cold scan** and **~100 ms ref enumeration**,
- and why **FM-PARALLEL-STATUS failed** (N partitions each re-opened + re-read
  the index, ~5× slower).

The fix is to stop re-opening: hold one `git_repository` handle open for the
monitor's lifetime and reuse it. libgit2 caches the index/odb/refdb in the
handle and reloads only what changed, so repeated queries on a warm handle skip
the re-parse — *if* libgit2 behaves the way we expect (see the gate below).

## The make-or-break gate (validate BEFORE the rewrite)

The entire payoff rests on one assumption: **a scoped status on a reused handle,
when the index is unchanged, skips the full index re-read.** That is the warm
(worktree-edit) case — the common one — where the index doesn't change.

**Phase 0 is a throwaway spike, not the rewrite:** open one handle, run
`git_status` (scoped to a dir) in a loop on roll20 *without* mutating the index,
and measure 2nd-call latency vs the cold per-call ~20 ms.

**RESULT: PASSED — go.** Raw-libgit2 probe on roll20:

| | per-call open | reused handle (2nd+ call) |
| --- | --- | --- |
| scoped to a small subdir | 16.8 ms | **0.6 ms** (~28×) |
| whole repo | 144 ms | 120 ms (~1.2×) |

A reused handle skips the index re-read: scoped status drops 16.8 → 0.6 ms, so
the ~20 ms incremental floor *is* the per-call index re-parse and reuse removes
it. `UPDATE_INDEX` does not defeat reuse (0.60 vs 0.54 ms), so we keep it. The
whole-repo case barely moves (the 37k-file `lstat` walk is inherent per call) —
which is fine: the win lands exactly on the **warm incremental path** (worktree
edits → scoped status), the common case, taking it ~20 ms → ~0.6 ms, below
git+fsmonitor. The cold `full_refresh` re-opens and stays ~120–140 ms,
unchanged.

## Design (assuming the gate passes)

A `mg::git::session` owning one open `git_repository`, exposing the read queries
the monitor repeats as methods that reuse the handle:

```cpp
class session {
public:
    static std::expected<session, error> open(std::string path);
    std::expected<std::vector<file_status>, error> status();
    std::expected<std::vector<file_status>, error> status_scoped(span<string>);
    std::expected<head_info, error> read_head();
    /* branches(), tags(), recent_commits(n), upstream_*(), stashes(),
       conflicts(), worktrees(), submodules(), rebase_in_progress(),
       bisect_active() — everything gather_status_view() calls */
private:
    detail::repo_ptr repo_;   // the long-lived handle
};
```

**No duplication:** each query's body moves into a `*_impl(git_repository*, …)`
helper; the free functions become `session::open(path)->method()` thin wrappers
(backward-compatible), and `session` methods call the impls on the held handle.

## Thread-safety boundary (the lesson from FM-PARALLEL-STATUS)

libgit2 handles are not safe for concurrent use. The win comes from *reuse on
one thread*, never *sharing across threads* — which is exactly what made
parallel-status contend on libgit2's global locks. So:

- The **monitor** holds its own session (monitor-thread-private), reused for all
  status/ref reads.
- The **blame/log worker** keeps its own (worker-thread-private) — optional
  Phase 3.
- **Mutations** (stage/commit/checkout/… on the UI thread) stay **stateless
  free functions** — one-shot and infrequent; caching buys nothing and would
  reintroduce cross-thread sharing.

No session is ever shared between threads → no locking, no contention.

## Staleness (the real risk of a long-lived handle)

A long-lived handle can hold stale caches — most importantly the **refdb** (does
it see an external `git commit`'s new ref?) and config. We sidestep this with
the access pattern the monitor already has:

- **`.git` change (commit / stage / branch switch) → re-open the session.** This
  is the cold/`full_refresh` path; a fresh handle = fresh caches. Re-opening is
  ~sub-ms and refs only change here.
- **Worktree change → reuse the session.** Refs/index are unchanged here, so the
  warm handle is correct *and* fast (the index-read skip).

So the session is re-opened exactly when external state could have moved it, and
reused exactly when nothing it caches could have changed. Clean and safe.

## Scope

- **In:** `session` for the monitor's repeated reads; the monitor re-opens on
  `.git`/full-refresh and reuses on worktree changes; the shared-impl refactor so
  free functions delegate.
- **Out:** stateful mutations; a global/shared session; cross-thread sharing;
  the worker session (optional follow-up); fsmonitor-style index *diffing*
  (separate, larger, and libgit2-gated).

## Tests

- **Equivalence:** `session.status()/status_scoped()/branches()/…` ==
  the free-function results (byte-identical entry sets) — pins the refactor.
- **Staleness:** after an external commit, a *re-opened* session reflects the new
  ref/HEAD; a session *reused* across a worktree change returns correct scoped
  status (the warm path).
- **Benchmark:** warm scoped status on a reused session vs per-call open on
  roll20 — confirm the floor drops (Phase 0's promise, now in the real path).
- **TSan:** the monitor session is monitor-thread-private → no new races
  (confirm 0 reports; this is the property that distinguishes it from
  parallel-status).

## Phases

0. **Spike (gate):** raw libgit2 loop — does a reused handle skip the index
   re-read on an unchanged index? Go/no-go. *(Throwaway; nothing shipped.)*
1. **`session` class + shared-impl refactor**; free functions delegate;
   equivalence + staleness tests. No behavior change.
2. **Monitor uses a session** (re-open on full_refresh, reuse on incremental);
   benchmark the warm-path win; TSan.
3. *(Optional)* worker session for blame/log.

## Outcome (shipped)

Phases 0–2 landed; Phase 3 (worker session) skipped — blame/log are one-shot
per request, not a repeated-query loop, so a session there buys ~nothing.

- **Phase 0** spike: passed (16.8 → 0.6 ms reused scoped status).
- **Phase 1** `mg::git::session` (PIMPL so the opaque handle never crosses the
  module boundary): `open` / `status` / `status_scoped`, holding one handle +
  one libgit2 init for its lifetime. The free functions delegate
  (`session::open(path)->status_scoped(...)`), so there's one implementation.
  Tests: session results == the free functions', and a *reused* handle picks up
  later worktree changes (the warm path's correctness).
- **Phase 2** the monitor holds `std::optional<session>`, re-opens it in
  `full_refresh` (.git change / cold start → fresh refs+index) and reuses it in
  `reconcile` for scoped status across worktree edits — so the warm incremental
  refresh runs on the cached handle (~0.6 ms, the Phase-0 number) instead of
  re-opening (~20 ms). Monitor-thread-private, so it stays race-free by
  construction (the property parallel-status lacked).

Validated: macOS / Alpine / Linux-arm64-TSan all **195/195, 0 races**; the cold
path (re-open) is unchanged, so everyday git ops don't regress.

## Risk + honest call

The risk isn't concurrency (the session is single-threaded by construction) —
it's that **libgit2 may not actually skip the index re-read**, making the rewrite
a large mechanical churn for little gain. That's why Phase 0 is a cheap spike
that can kill the milestone before any real work. If it passes, the warm
incremental path could drop from ~20 ms to sub-ms — below git+fsmonitor — for
the cost of an engine-API refactor whose blast radius is contained by the
free-functions-delegate compatibility shim. The cold path is unchanged (it
re-opens, as today), so there's no regression risk to the common git operations.
