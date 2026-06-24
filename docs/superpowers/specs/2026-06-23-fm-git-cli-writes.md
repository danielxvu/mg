# FM-GIT-CLI-WRITES — route mutations through real `git` (the read/write split)

## Why

mg does *writes* through libgit2, which silently diverges from real git on the
things users most depend on:

- **Hooks don't run.** libgit2 `git_commit_create`/merge/etc. never fire
  `pre-commit`, `commit-msg`, `post-commit`, `pre-push`, `post-merge`. A repo
  whose workflow relies on lint/format/policy hooks gets *none* of them through
  mg today — a real correctness gap, not a perf one.
- **Signing is limited.** `commit.gpgsign` / `gpg.format=ssh` aren't honored the
  way native git does them.
- **Credentials are reimplemented.** Network ops use mg's own HTTPS prompt
  (`mg_magit_set_cred_prompt`) instead of the user's configured credential
  helper / SSH agent.
- Config-driven filters, `commit.template`, etc. can differ.

The fix is the Sublime Merge model (validated in our comparison): **reads stay
on libgit2** (fast — the session/incremental work we just shipped), **mutations
go through the real `git` binary** so behaviour is *exactly* git's, with zero
reimplementation of the write side.

## The split principle

Route through `git` only the ops where the CLI adds real compatibility — those
that **run hooks, sign, or authenticate**. Leave the purely-mechanical local
ops (and all reads) on libgit2, where there is no gap:

| through `git` CLI | stays on libgit2 |
| --- | --- |
| commit (+ amend / extend / reword) — hooks + signing | stage / unstage / discard / reset |
| merge / cherry-pick / revert — hooks + signing | branch / tag / stash create-delete |
| push / pull / fetch — credentials + pre-push | notes, bisect refs |
| | **all reads** (status / log / diff / blame) |

Staging has no hooks and no auth, so libgit2 is already correct *and* fast there
— moving it would only add subprocess cost. The win is concentrated in the
hook/sign/auth ops.

## `run_git` (the mechanism)

A small helper: `run_git(repo, {args...}) → {int code, string out, string err}`
that execs `git -C <repo> <args>` via `posix_spawn`/`fork+exec` with an **argv
array (never a shell)** — no injection surface. Exit code + captured stderr map
to mg's existing result codes; conflict-leaving ops (`merge`/`cherry-pick`/
`revert`) map a non-zero exit **plus** an in-progress repo state to mg's
"conflicts" code (2), exactly as the libgit2 versions do today.

## The crux: a TUI handing the terminal to git

mg owns the terminal (raw mode, alt screen). git subcommands may need it —
credential/passphrase prompts, GPG `pinentry-tty`, progress meters, hook output.
Two regimes:

- **Network / interactive (push / pull / fetch):** **suspend the TUI and inherit
  the terminal** — restore cooked mode, run git with stdio = the real terminal
  (so prompts/progress/auth work like running git from a shell), then re-enter
  raw mode and redraw. mg already has this machinery (`spawn.c` / the `!`
  shell-out path); reuse it. This is how the user's credential helper + SSH
  agent + GPG just work.
- **Local (commit / merge / cherry-pick / revert):** run with output **captured
  to a buffer** (like magit's process buffer) and shown on failure (e.g. a
  `pre-commit` hook that rejects). Signing via a cached gpg-agent needs no
  prompt; first-time `pinentry-tty` is the edge — fall back to suspend-inherit
  if git exits asking for a terminal, or document `pinentry` setup.

## Synergy with what we just built

After any CLI mutation, `.git` changes on disk → the **monitor's watcher fires →
status auto-refreshes** (incremental or full). So there's no manual post-command
refresh to wire — the fs-watch + session + reconcile pipeline already reacts.
The mutation just runs git; the UI updates itself.

## Tests

- **Hooks fire:** a repo with a `pre-commit` that exits non-zero → mg's commit
  fails and surfaces the hook output; a `commit-msg` hook rewrites/validates.
  (Impossible to test today because libgit2 skips hooks — this is the headline.)
- **Signing:** with a test signing key + `commit.gpgsign=true`, the commit is
  signed (`git verify-commit` / `%G?`).
- **Conflict mapping:** a CLI `merge`/`cherry-pick` that conflicts → mg's code 2
  + the repo left mid-merge (matches the libgit2 behaviour the UI expects).
- **No-shell safety:** a repo/branch name with shell metacharacters is passed as
  argv and not interpreted.
- **OFF build** unaffected (mutations are `ENABLE_NATIVE_MAGIT`-gated already).

## Phases (highest-value / lowest-terminal-risk first)

1. ✅ **DONE** — **`run_git` + commit through the CLI** (`git commit -m <msg>`
   and the amend/extend/reword variants) — the headline: hooks + signing now
   run. Output captured (combined stdout+stderr via one pipe), surfaced as the
   error message on hook rejection. The watcher auto-refreshes after.
   `detail::run_git` (posix_spawnp, argv array — never a shell) + a shared
   `commit_via_cli` helper that maps a non-zero exit to an error carrying git's
   output and returns the new HEAD's `--short=8` oid. `commit`, `commit_amend`
   (`--amend -m`), `commit_extend` (`--amend --no-edit`), `commit_reword`
   (`--amend --only -m`, no pathspec → message-only) all delegate; `amend_impl`
   dropped. `head_message` strips git's stripspace trailing newline so content
   round-trips. Tests: pre-commit-hook rejection fails commit + commit_amend and
   surfaces the output (impossible on the libgit2 path). 197/197 macOS+Alpine,
   OFF build unaffected.
2. **merge / cherry-pick / revert** through the CLI (hooks + signing + the
   conflict-exit mapping).
3. **push / pull / fetch** through the CLI with TUI suspend-and-inherit, so the
   real credential helper / SSH agent / progress work; retire
   `mg_magit_set_cred_prompt`.

Each phase: macOS + Alpine + the OFF build green; the libgit2 read path
untouched.

## Risks + honest call

- **Terminal save/restore** is the real risk (raw mode + alt screen around a
  subprocess that may itself touch the tty). Mitigated by reusing mg's existing
  spawn/suspend path and starting with capture-to-buffer (phase 1) before the
  inherit-terminal network ops (phase 3).
- **Interactive rebase stays on mg's own UI** (the `*git-rebase-todo*` buffer +
  in-memory executor) — routing `rebase -i` through git's `$GIT_SEQUENCE_EDITOR`
  would fight mg's existing interface. Out of scope.
- Slight per-mutation subprocess cost (fork+exec ~ms) — irrelevant for
  user-initiated one-shot actions, and the correctness gain (hooks/signing/auth)
  dwarfs it.

## Out of scope

- Moving reads to the CLI (libgit2 + the session is faster — keep it).
- Moving mechanical local ops (stage/branch/tag/stash) — no hook/sign/auth gap.
- Interactive rebase (mg's own UI).
- A general async process-buffer framework (capture-then-show suffices).
