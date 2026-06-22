# FM-ASYNC-BLAME — background the per-file blame / log builds off the UI thread

## Why (and why this is *not* just "FM-ASYNC-STATUS again")

After the status build moved off the UI thread, the remaining interactive
stalls are the **per-file** commands invoked from `*magit-status*`:

| command | bridge call | measured (big repos) |
| --- | --- | --- |
| `B` blame file | `mg_magit_blame_file` | **0.3 – 1.2 s** |
| `l f` log file | `mg_magit_log_file_buffer` | **150 – 360 ms** |

Each runs **synchronously on the UI thread** inside the command (`magit_blame`,
`magit_log_open`): the bridge does the libgit2 work inline, then the buffer is
popped. For ~1 s the editor is frozen — no redraw, no `C-g`.

The crucial difference from FM-ASYNC-STATUS: status is **one global thing** the
monitor keeps *warm* and recomputes on fs events. Blame and log-file are
**on-demand, parameterised by a path, ephemeral** — there is no single "current
blame" to keep warm, and the user fires them deliberately and infrequently. So
the keep-warm snapshot model does not apply. This is a **request → response**
async problem: dispatch one expensive job, render a placeholder, swap the result
in when it lands, and drop it if it has been superseded.

That makes the cost/benefit honestly *weaker* than FM-ASYNC-STATUS (less
frequent, user-initiated, and the request/response machinery — job worker +
coalescing + placeholder + stale-drop — is more moving parts than an immutable
snapshot swap). See **Lighter alternative** before committing to the full build.

## What's reusable vs new

Reused from FM-ASYNC-STATUS (already built, proven):
- The **wake pipe** + `ttgetc` poll + `getkey`/main-loop idle handler (Phase 4).
  A finished job pokes the same pipe; the idle handler also drains job results.
- The **`snap_line` capture** pattern: a worker emits into a `vector<snap_line>`
  via the existing `mg_magit_emit_fn`, handed to the UI under a mutex; the UI
  builds the buffer on the UI thread. **No buffer/window touched off-thread.**
- The **`cpp-tsan` gate** + the immutable-handoff discipline + the determinism
  style of test (worker output == the current synchronous output, byte-for-byte).

New (the request/response core):
- A single **job worker thread** with a one-slot, latest-wins mailbox (NOT the
  status monitor thread — a 1.2 s blame must not stall status recomputes). It
  owns its own libgit2 handle (thread-safe across separate `git_repository`s).
- A **request generation counter** per target buffer: the UI applies a result
  only if its generation still matches the latest request (coalesce/stale-drop —
  fire blame on a.c then b.c → a.c's result is discarded).
- A **placeholder render**: invoking `B`/`l f` immediately pops the buffer with a
  `Blaming <file>…` / `Loading log for <file>…` line and returns; the result
  replaces it on the next idle wake.

## The request/response model

```
magit_blame(B):
  enqueue job{kind=blame, repo, path, gen=++blame_gen, target="*magit-blame*"}
  render placeholder into *magit-blame*, pop it, return    # UI never blocks
worker:
  pop latest job (drop older queued jobs for the same target)
  result = blame_file(repo, path)  -> vector<snap_line>     # the slow part
  publish{gen, target, result}; poke wake pipe
idle handler (getkey/main loop, on wake):
  for each ready result: if result.gen == current gen for its target,
      rebuild that buffer from result.lines (UI thread); else drop it
```

`log f` is the same with `kind=log_file`. `log l` (whole-repo recent commits) is
already ~2.7 ms after PR #74 and `commit_diff` ~10–36 ms — **left synchronous**;
only the two genuinely slow per-file builds go async.

## The crux: superseded requests + buffer identity

- **Coalescing.** Spamming `B` down a file list must not queue N blames. The
  mailbox keeps only the latest request per target buffer; the worker checks the
  generation again before publishing and skips stale work where cheap.
- **Stale-drop on apply.** Even a completed result is discarded if the user has
  since requested a different file (generation moved) or killed the buffer.
- **Re-entrancy safety (the FM-ASYNC-STATUS lesson).** The buffer rebuild runs
  only at the safe idle points (top-level `getkey` / main loop), never inside a
  nested read — identical discipline to `magit_idle_refresh`. A new
  `magit_async_apply()` is the single safe-point entry that fills any ready
  job buffers (and the existing status refresh folds into the same wake).

## Invariants (must hold)

1. No mg buffer/window/line touched off the UI thread (worker emits to a plain
   `vector<snap_line>`; the UI builds the buffer).
2. The UI never blocks on blame/log-file — only the placeholder render runs
   synchronously (microseconds).
3. A result is applied at most once, only if its generation is still current and
   its target buffer still exists.
4. OFF (`c-legacy`) build byte-identical: every hook `#ifdef`-gated; 0 magit
   symbols. Worker lifecycle tied to `mg_magit_start`/`mg_magit_stop`.
5. Green under the `cpp-tsan` gate (worker publish vs UI apply, coalescing).

## Outcome (full async shipped, PR #78)

Built the full request/response worker rather than the lighter progress-message
(per "full async"). Notes vs the plan:

- **One shared UI wake.** The self-pipe was hoisted out of the status `monitor`
  into a module-global `ui_wake` (opened in `mg_magit_start` before the worker
  threads, closed in `mg_magit_stop` after they join). Both the status monitor
  and the job worker poke it; `ttgetc` already polls it. One mechanism feeds the
  one idle handler.
- **Phases 1–3 folded.** The generation tracking, stale-drop, and worker-side
  skip landed together — the UI tracks `magit_blame_gen` / `magit_logfile_gen`,
  and `magit_async_apply()` (run from the idle handler beside the status refresh)
  applies a result only if its generation still matches and its buffer exists,
  else drains and drops it.
- **Cold/no-worker fallback.** If `mg_magit_async_request` returns 0 (worker not
  running), `magit_blame` / `magit_log_build` fall back to the synchronous build.
- **Scope held:** only `blame` and per-file `log` go async; whole-repo `log`
  (~3 ms) and `commit_diff` stay synchronous.

Validation: determinism anchor green (async output == sync, byte-identical);
latest-wins + no-phantom-generation test; a concurrency stress test racing
request/peek/take against the worker — **185/185 under `cpp-tsan`, 0 races**.
tmux end-to-end: `B` on a 400-commit file pops `*magit-blame*` instantly with
`Blaming big.txt...`, then fills with real blame output on the idle wake with
**zero keypresses**. OFF build byte-identical (0 magit symbols).

## Test plan

- **Determinism (doctest):** the worker's `vector<snap_line>` for a blame / a
  log-file equals the current synchronous `mg_magit_blame_file` /
  `mg_magit_log_file_buffer` output for the same repo+path (byte-identical). The
  correctness anchor.
- **Coalescing (doctest):** enqueue blame(a), blame(b) back-to-back; only b's
  result is published/applied; a is dropped. Generation logic unit-tested.
- **Thread safety (cpp-tsan):** spin enqueue/publish/apply while the worker
  computes in a loop — zero races (reuses the gate).
- **tmux end-to-end:** `B` on a big file → buffer pops instantly with the
  placeholder, fills within ~1 s with no editor freeze; firing `B` on a second
  file before the first finishes shows only the second; `q` mid-flight is clean.
- **Perf:** the UI-thread cost of `B`/`l f` drops to the placeholder render
  (sub-ms); the 0.3–1.2 s / 150–360 ms moves to the worker.

## Phased implementation (each phase builds + tests green, OFF byte-clean)

1. **Worker + mailbox + result capture** — a `job_runner` thread (own libgit2
   handle) with a latest-wins mailbox; `enqueue(kind, repo, path, gen)` and
   `take_ready(results)`. Engine/bridge already produce the lines; the worker
   captures them into `vector<snap_line>`. Doctest: worker output == sync output;
   coalescing unit test. No UI change yet.
2. **Placeholder + apply on wake** — `magit_blame`/`magit_log_open` enqueue +
   render the placeholder + pop; `magit_async_apply()` (run from the Phase-4
   idle handler) fills ready buffers by generation. The wake pipe is poked by the
   worker on publish (reuse `mg_magit` wake fd).
3. **Coalescing + stale-drop UX** — generation match on apply; drop results for
   killed buffers; a brief `…` marker while a request is outstanding.
4. **cpp-tsan + tmux validation; re-profile.**

## Lighter alternative (decide first)

Full async is the "right" answer but it is real machinery for a deliberately-
invoked, infrequent command. A **much cheaper 80 % win**: before the synchronous
blame/log call, `ewprintf("Blaming %s…", path)` + `update(CMODE)` so the user
sees *why* the editor paused, and the buffer still fills in place. Zero threads,
zero new failure modes — it removes the "is it hung?" confusion but still blocks
input for the ~1 s. If interactivity-during-blame is not actually a felt pain,
this is the proportionate fix and FM-ASYNC-BLAME can be deferred.

Recommendation: ship the lighter progress-message first (trivial, low-risk),
and only build the full async worker if blocking during blame proves annoying in
practice.

## Out of scope

- Incremental/streaming blame (libgit2 has no streaming blame API ≤1.9).
- A general async/job framework for every bridge call — only the two slow
  per-file builds; status stays on its own keep-warm monitor.
- Keeping blame/log results warm or cached across invocations.
- Backgrounding `commit_diff` / whole-repo `log` (already fast enough).
