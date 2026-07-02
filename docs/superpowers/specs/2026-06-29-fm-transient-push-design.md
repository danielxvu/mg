# FM-TRANSIENT-PUSH — push extras + a pull-args transient

## Why

Magit's push/pull transients expose argument toggles before running. neomg's
`P` push transient has `-f`/`-u` infixes (`push_infixes`), but the `F` pull
transient has none (its menu's infix slot is `NULL`) — it's just two actions
(`F p` pull/merge, `F r` pull --rebase). todo.md's FM-TRANSIENT-PUSH continues
the FM-TRANSIENT-DEPTH gap (deepening the argument menus): add the two most
useful push args and give pull real argument toggles.

## Decided model (from brainstorming)

### Push transient (`P`) — two new infixes on top of `-f`/`-u`
- **`--tags`** (key `t`, FLAG) — also push annotated tags reachable from the
  pushed commits (`git push --tags`).
- **`--dry-run`** (key `d`, FLAG) — preview what a push *would* do without
  changing the remote. When set, `P p` runs a **captured** push whose output
  goes to `*magit-process*` so the user can read the preview (see below);
  unset, `P p` is the normal interactive (tty) push.

`--dry-run` composes with `-f`/`-u`/`--tags` (e.g. a forced dry-run preview).

### Pull transient (`F`) — keep `F p`/`F r`, add two modifier infixes
- **`--autostash`** (key `a`, FLAG) — stash dirty changes, pull, then pop
  (`git pull --autostash`). Works with both merge (`F p`) and rebase (`F r`)
  pulls — the universally handy modifier.
- **`--ff-only`** (key `f`, FLAG) — refuse anything but a fast-forward
  (`git pull --ff-only`). A safe pull; most meaningful for `F p` (merge).

**Pull-structure decision:** keep `F p` (merge) / `F r` (rebase) as the two
*actions* and add `--autostash`/`--ff-only` as orthogonal *modifiers* that apply
to whichever action runs. This is additive and non-breaking, rather than
collapsing rebase into a `--rebase` infix (more Magit-literal but it churns the
existing `F r` binding). The transient does not police nonsensical combinations
(e.g. `--ff-only` with `F r`) — git reports those, exactly as Magit leaves it.

### Infix keys (no collisions)
- Push menu actions: `p`. New infixes `t`/`d` are free (existing infixes `f`/`u`).
- Pull menu actions: `p`/`r`. New infixes `a`/`f` are free.
The transient checks infix keys before the action map, so an infix key shadows;
none of `t`/`d`/`a`/`f` collide with the action keys above.

## Architecture (extends the existing net-op chain)

Today the editor commands run network ops through one dispatcher:
`magit_do_push` → `magit_run_net(MNET_PUSH, cwd, force, set_upstream, banner)`
→ `mg_magit_push_cli(cwd, force, set_upstream)` which builds
`{"push", [--force-with-lease], [-u], "origin", "HEAD"}`, runs it over
`git_terminal` (tty-inherit — credential helper / SSH agent / progress all
work), and records a `$` proclog entry. `magit_run_net` returns `-2` (user
declined the tty suspend), `-1` (git not on PATH → fall back to the libgit2
`mg_magit_push`), `0` (ok), or another non-zero (failure). Pull is the mirror:
`mg_magit_pull_cli(cwd, rebase)` builds `{"pull", [--rebase], "--no-edit",
"origin"}`.

### Bridge (C ABI) — grow the CLI runners' flag params
- `mg_magit_push_cli(repo, force, set_upstream, **int tags**)` — append
  `--tags` when set. `--tags` is a **CLI-path feature**; the libgit2 fallback
  (`mg_magit_push`, hit only when git is off PATH) stays a plain push without
  `--tags` — a documented limitation of that already-degraded path, not worth
  the tag-refspec plumbing for a case real dev machines never hit.
- `mg_magit_pull_cli(repo, rebase, **int autostash, int ff_only**)` — append
  `--autostash` / `--ff-only` when set.
- **New captured push for `--dry-run`:** `mg_magit_push_dry_run(repo, force,
  set_upstream, tags)` builds `{"push", "--dry-run", [flags], "origin",
  "HEAD"}` and runs it through the **hardened captured `run_git`** (the
  FM-GIT-CMD path: stdin→/dev/null, `--no-pager`), records a `$` proclog entry
  with the **captured output**, and returns the exit code. It does NOT use
  `git_terminal` and does NOT suspend the tty (there is nothing interactive to
  show — the point is to read the result afterward).

### Editor (`magit_cmd.c`)
- `push_infixes[]` gains `{ 't', "--tags", FLAG, 0, "", 0 }` and
  `{ 'd', "--dry-run", FLAG, 0, "", 0 }` (the trailing `0` is the
  FM-TRANSIENT-DEPTH `clear_on_empty` field; FLAGs never clear).
- `pull_menu` gets a new `pull_infixes[]` (`--autostash`, `--ff-only`) and the
  menu's currently-`NULL` infix slot is wired to it + its count.
- `magit_push` reads `push_infixes` `--tags`/`--dry-run`. When `--dry-run` is
  set it calls `mg_magit_push_dry_run(...)` and then opens `*magit-process*`
  (reusing `magit_process`) so the preview is on screen; otherwise it runs the
  existing `magit_do_push` (tty) path with `--tags` threaded through
  `magit_run_net` → `mg_magit_push_cli`.
- `magit_pull` / `magit_pull_rebase` read the new pull infixes and thread them
  through `magit_run_net` → `mg_magit_pull_cli`.
- `magit_run_net` carries the extra per-op flags (it currently passes two ints,
  `force`/`set_upstream`; extend it to carry `tags` for push and
  `autostash`/`ff_only` for pull — a small signature/flags change, kept minimal).

## Rendering

The `P` and `F` transients render the new infixes via the existing
`magit_transient_render` (FLAG on/off display) — no new render code. A
`--dry-run` push lands the user in `*magit-process*` showing the captured
`$ git push --dry-run …  (ok)` entry with git's preview output beneath it
(exactly the FM-PROCESS-LOG rendering).

## Error handling / edges

- All four new infixes default **off** → `P`/`F` behave byte-identically to
  today when nothing is toggled.
- **`--dry-run` auth:** a dry-run still contacts the remote. Over captured
  `run_git` it works for non-interactive auth (SSH agent / cached credentials —
  the common dev case, same creds the normal push uses) and, thanks to the
  `run_git` hardening (stdin→/dev/null), a password-HTTPS remote **fails fast
  rather than hangs** — the documented limitation, identical to FM-GIT-CMD's
  `:`. The captured failure output still lands in `*magit-process*`.
- **`--ff-only` that can't fast-forward** → git exits non-zero; the failure is
  surfaced (tty output / `ewprintf`), not a crash.
- **`--autostash`** is git-native (stash → pull → pop); a pop conflict is git's
  to report.
- **Nonsensical combos** (`--ff-only` with `F r`, etc.) are not blocked — git
  reports them, matching Magit's loose-arg behavior.
- The libgit2 push fallback (git off PATH) keeps working unchanged — it does a
  plain push (no `--tags`); `--dry-run` only ever runs captured and never
  reaches the fallback. Both are documented limitations of the rare git-less
  path, not regressions of the normal CLI path.

## Testing

- **Engine** (`test_git`): `mg_magit_push_dry_run` against a repo with a local
  "remote" (a bare repo added as `origin`) records a `$` proclog entry whose
  command contains `push --dry-run` and whose `ok` reflects the exit code, and
  the remote is **unchanged** (dry-run mutated nothing). Pull-arg argv: assert
  `--autostash`/`--ff-only`/`--tags` land in the built argv (via a seam that
  exposes the argv, or via `git_terminal`/`run_git` behavior on a local remote).
- **Bridge** (`test_bridge`): `mg_magit_push_cli(..., tags=1)` /
  `mg_magit_pull_cli(..., autostash, ff_only)` thread the flags; the dry-run
  bridge fn records the proclog entry and returns the code.
- **Editor** (`test_editor`, pty): toggling `--tags`/`--dry-run` in `P` and
  `--autostash`/`--ff-only` in `F` is reflected in the transient; a `--dry-run`
  push opens `*magit-process*` with a `push --dry-run` entry. **Single transient
  session, presence-only assertions, no buffer round-trip** (per the
  FM-TRANSIENT-DEPTH musl lesson — see the pty-test memory).

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Phases (each independently testable)

1. **Engine + bridge push:** `--tags` through `mg_magit_push_cli` +
   `push_remote`/`mg_magit_push` fallback; the captured `mg_magit_push_dry_run`
   (run_git + proclog) + engine/bridge tests.
2. **Engine + bridge pull:** `--autostash`/`--ff-only` through
   `mg_magit_pull_cli` + tests.
3. **Editor:** `push_infixes` `--tags`/`--dry-run` (+ the dry-run →
   `*magit-process*` branch in `magit_push`); `pull_infixes`
   `--autostash`/`--ff-only` wired into `pull_menu` + `magit_pull`/
   `magit_pull_rebase`; `magit_run_net` flag plumbing; pty tests.

## Out of scope (deferred — tracked in todo.md)

- `--no-ff` (force a merge commit), `--force` (hard, vs the existing
  `--force-with-lease`).
- Push/pull to a **chosen remote or refspec** (still hardwired to `origin` /
  `HEAD` / the upstream).
- A **fetch transient** with args (`--prune`, `--tags`, `--all`).
- `FM-LOG-TRANSIENT-KEY` (bind the log transient in `*magit-log*`) — separate
  deferred item from FM-TRANSIENT-DEPTH.
