# FM-PROCESS-LOG — a `*magit-process*` transparency buffer

## Why

Sublime Merge's and Magit's most-loved trust feature is *seeing what git
actually did*: a running log of every operation, in git terms, with output and
result. neomg performs operations through two paths — real `git` subprocesses
(commit/merge/cherry-pick/revert via `detail::run_git`; push/pull/fetch via
`detail::git_terminal`, per [[fm-git-cli-writes]]) and in-process libgit2
(stage/discard/branch/tag/stash/…). Today none of it is visible: there is no
process buffer. This adds one.

The honest twist vs. Sublime/Magit: those tools shell out for *everything*, so
every log line is a literal command. neomg does mutations-with-hooks/sign/auth
through `git` but keeps mechanical local ops on libgit2 — so a faithful log must
distinguish *what literally ran* from *what the operation is equivalent to*.

## The model (decided)

Equivalent git commands, **labeled by execution path**:

- `$ git …` — a **real subprocess** neomg spawned. Truthful, literal argv.
- `≈ git … (via libgit2)` — an in-process libgit2 op shown as the command it
  corresponds to. The `≈` + `(via libgit2)` makes clear it was *not* literally
  executed (so a user never copy-pastes it expecting byte-identical behaviour).

## Coverage

Log **state-changing operations + network**; never reads, never the background
refresh:

| logged | not logged |
| --- | --- |
| commit / amend / extend / reword (`$`, captured) | status / log / blame / diff (reads) |
| merge / cherry-pick / revert (`$`, captured) | the fswatch-triggered auto-refresh |
| push / pull / fetch (`$`, terminal-inherited) | async status/blame jobs |
| stage / unstage / discard / reset (`≈`) | |
| branch / tag / stash / checkout / notes / bisect (`≈`) | |
| rebase: one entry per *user action* — start / continue / skip / abort (`≈`, mg's own rebase engine) — never per replayed commit | |

Rationale: the value is "what did neomg *do to my repo*". Reads change nothing,
and the status walk fires on every keystroke-adjacent fs event — logging it
would bury the signal.

## Three entry flavors (from the existing write paths)

1. **`$` captured** — `detail::run_git` (git.cppm) already runs `git -C <repo>
   <argv>` via `posix_spawnp` and captures combined stdout+stderr + exit code.
   A single append at its return covers commit/amend/extend/reword and
   merge/cherry-pick/revert: command (reconstructed from argv), full output,
   ok = (exit == 0), duration.
2. **`$` terminal-inherited** — `detail::git_terminal` (push/pull/fetch) runs git
   with the child inheriting the cooked tty, so **output is not captured** (it
   went to the screen). Log the command + exit result, with output replaced by a
   single `(output shown in terminal)` note. Honest about what we have.
3. **`≈` libgit2** — each mutating libgit2 C-ABI wrapper in bridge.cpp appends
   its equivalent command + result. One line per wrapper (e.g. `mg_magit_stage`
   → `≈ git add <path>`).

## Architecture (mirrors the status-buffer 3-layer pattern)

1. **Sink** — a process-wide, mutex-guarded ring buffer (cap 200) living in the
   magit module so both git.cppm (`run_git`/`git_terminal`) and bridge.cpp (the
   libgit2 wrappers) can append. Entry:
   `{ char kind; std::string command; std::string output; bool ok; long duration_ms; }`.
   Mutex because mutations can run on the async/job threads. Eviction drops the
   oldest when full.
2. **Append API** — `mg::magit::proclog::record(kind, command, output, ok, ms)`
   and `for_each(fn)`; plus a tiny `argv_to_command(span<string>)` helper that
   renders an argv array as a shell-style line (quoting args with spaces) for the
   `$` entries.
3. **Viewer C ABI** — `int mg_magit_process_log(mg_emit_fn, void *ctx)`: streams
   each entry as tagged lines — a header line (`MG_LINE_PROCESS_CMD`) and any
   output lines (`MG_LINE_PROCESS_OUT`), exactly like `mg_magit_status_buffer`.
   Returns the line count.
4. **Editor** — a `magit-process` command bound to `$` in `magit-status-mode`
   (Magit's binding), opening/refreshing a `*magit-process*` buffer that renders
   the streamed lines. A new `MG_LINE_PROCESS_*` kind pair drives any face/keymap.

## Rendering

```
$ git push origin neomg              (3.1s, ok)
    To github.com:you/repo.git
       5f79ed6..a1b2c3d  neomg -> neomg
$ git commit -m "fix: rename"        (0.2s, ok)
    [neomg abc1234] fix: rename
≈ git add src/foo.c                  (via libgit2)
≈ git tag v0.2                       (via libgit2)
```

Most-recent entry rendered at the **top** (you open `$` to see what just
happened). Output indented two spaces under its command. `$` terminal-inherited
ops show `(output shown in terminal)` in place of output. No folding/collapse in
v1 (YAGNI — the ring cap bounds size; folding can come later).

## Persistence

In-memory, **session-scoped**, cleared on restart — same lifetime as Magit's
process buffer. No on-disk log (avoids a new file to manage + a privacy
surface: commit messages / branch names would otherwise persist).

## Tests (TDD)

- **Sink** (unit): `record` then `for_each` preserves order; the ring evicts the
  oldest past the cap; `argv_to_command` quotes an arg containing a space.
- **`≈` entries** (bridge): after `mg_magit_stage(path)` the log holds
  `≈ git add <path> (via libgit2)`; after `mg_magit_tag_create` a `≈ git tag`
  entry. ok reflects the wrapper's result.
- **`$` captured** (bridge): a `mg_magit_commit` logs a `$ git commit …` entry
  whose output contains the new-commit line and ok = true; a commit rejected by
  a `pre-commit` hook logs ok = false carrying git's output (reuses the
  hook-rejection substrate from [[fm-git-cli-writes]]).
- **`$` terminal** (bridge): a `mg_magit_push_cli` to a `file://` bare remote
  logs a `$ git push …` entry with the `(output shown in terminal)` note and the
  right ok.
- **Reads excluded**: a `mg_magit_status_buffer` call adds **no** log entry.
- **Viewer** (render): `mg_magit_process_log` emits the expected tagged line
  stream (CMD header + OUT lines) for a seeded log.

Verified 232+/all green on macOS (`cpp`) + Alpine/musl container (`cpp-linux`)
+ the OFF (`cpp-tsan`) build, per the verify-locally-before-CI rule.

## Phases (each independently testable)

1. **Sink + viewer C ABI** — the ring buffer, `record`/`for_each`/
   `argv_to_command`, `mg_magit_process_log`, and the unit tests. No callers yet.
2. **`$` capture wiring** — append in `run_git` (covers commit/merge/cherry-pick/
   revert) and in the `_cli` network wrappers (`git_terminal` result). Bridge
   tests for captured + terminal + hook-rejection.
3. **`≈` libgit2 wiring** — append in the mechanical mutating wrappers
   (stage/unstage/discard/reset/branch/tag/stash/checkout/notes/bisect/rebase).
   Bridge tests for the `≈` entries + the reads-excluded assertion.
4. **Editor command** — `magit-process` bound to `$`, the `*magit-process*`
   buffer + `MG_LINE_PROCESS_*` kinds, rendering newest-first. pty editor test:
   stage a file, press `$`, assert the buffer shows the `≈ git add` line.

## Out of scope

- Output folding / collapsing (v1 shows all; ring cap bounds it).
- Persisting the log to disk.
- Logging reads (status/log/diff/blame) or the background refresh.
- Re-running a logged command from the buffer (a possible later affordance).
- Routing more ops through `git` to make them literal `$` — the read/write split
  from [[fm-git-cli-writes]] is deliberate; the `≈` label is the honest bridge.
