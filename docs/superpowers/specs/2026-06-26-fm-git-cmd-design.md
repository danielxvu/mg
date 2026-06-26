# FM-GIT-CMD — `:` run an arbitrary git command

## Why

Magit's `:` (`magit-git-command`) is the escape hatch: run any git command and
see its output, without leaving the status buffer. neomg has the dedicated keys
for the common ops but no way to run an ad-hoc `git <whatever>`. todo.md's
"Magit-parity gaps" lists it as medium value, and it has high synergy with the
`*magit-process*` transparency buffer just landed (FM-PROCESS-LOG) — the output
goes straight there.

## The design is almost entirely reuse

- `detail::run_git(repo, argv)` — the existing captured `posix_spawn` subprocess
  (argv-direct, no shell — injection-safe), already used by commit/merge/push.
- `mg::magit::proclog::record` / `argv_to_command` — the existing process log.
- `*magit-process*` buffer + `magit_process` command — the existing viewer.
- `eread` — the existing minibuffer prompt.
- `:` (0x3a) is free in `magitmap`.

The only genuinely new code is a small command-line tokenizer and the wiring.
(Explicitly NOT Zig — one-shot glue + a trivial parser; Zig's place here is the
worktree walk, not this.)

## Decided model

### Engine (`mg.git`)
```
// Tokenize a command line into argv, honoring '…' and "…" quoting (no shell);
// run `git <argv>` captured, record a '$' proclog entry, return the exit code.
int run_git_command(std::string repo, std::string cmdline);
```
- **Tokenizer** (`tokenize_cmdline(const std::string&) -> std::vector<std::string>`):
  split on unquoted whitespace; a `'…'` run is literal; a `"…"` run is literal
  (no backslash/variable expansion needed for v1 — git args rarely need it, and
  no shell means no metacharacter risk). Empty tokens dropped. This is what lets
  `: commit -m "a b"` pass `a b` as one arg.
- Runs `detail::run_git(repo, argv)` (captured), then
  `proclog::record('$', "git " + argv_to_command(argv), run.output, run.code == 0, ms)`
  — identical to how `commit_via_cli` logs. Returns `run.code` (or -1 if the
  cmdline tokenizes to empty / spawn failed).

### Runner = captured `run_git` (matches Magit's `:`)
All `:` commands run with captured stdout+stderr → the process log. **Interactive
/ network commands** (`: push` needing a password prompt, `: rebase -i` needing
an editor, `: log` with a pager) do NOT get a tty, so they hang or fail — this is
the documented limitation (Magit's `:` has the same shape). The status footer /
docs steer users to the dedicated keys (`P` push, `F` pull, `f` fetch, `r`
rebase, `l` log) for those. Auto-routing network subcommands to the tty-inherit
path is deferred (see Out of scope → roadmap).

### Bridge (C ABI)
```
int mg_magit_git_command(const char *repo, const char *cmdline);
```
→ `mg::git::run_git_command(repo, cmdline)`. Returns the exit code (or -1).

### Editor (`magit_cmd.c`)
- Bind `:` in `magitmap` → `magit_git_command`:
  1. `eread("git: ", line, sizeof line, EFNEW | EFCR, …)` — abort on empty.
  2. `getbufcwd(cwd, …)` (the buffer's repo, per the repo-from-buffer-dir rule).
  3. `mg_magit_git_command(cwd, line)`.
  4. `magit_refresh` — rebuild `*magit-status*` in place so any mutation the
     command made is reflected immediately (don't rely on the fs-watch monitor's
     timing, and it may be off).
  5. Open `*magit-process*` (reuse `magit_process`) so the user lands on the
     command's output — the deliverable.
- `:` lives in `magit-status-mode` only (v1).

## Rendering

After `:`, the `*magit-process*` buffer shows the new entry newest-first, e.g.
`$ git status  (0.1s, ok)` followed by git's output indented beneath — exactly
the existing process-log rendering (no new render code).

## Error handling / edges

- Empty cmdline (just RET): abort, no entry logged.
- A leading `git ` in the input (user types `: git status`): strip a single
  leading `git`/`git ` token so both `: status` and `: git status` work.
- Tokenize failure (unbalanced quote): treat the unterminated quote as running to
  end-of-line (lenient), or log nothing + `ewprintf` a note — pick lenient
  (close the quote at EOL) so a stray quote doesn't silently no-op.
- A command that fails (non-zero exit): still logged (`ok=false`) with its output
  — the user sees the error in `*magit-process*`. That's the point.
- Interactive/network command hangs: the documented limitation; the captured
  `run_git` blocks until the child exits or EOFs its input — for a password
  prompt that reads `/dev/tty` it will error out (no tty), not hang forever.

## Testing

- **Engine** (`test_git`): `tokenize_cmdline` — `status` → `[status]`;
  `commit -m "a b"` → `[commit, -m, a b]`; `log --oneline 'a b'` → quotes
  respected; leading `git ` stripped. `run_git_command(repo, "status")` returns 0
  and records a `$` proclog entry whose command contains "git status" and whose
  output contains a status keyword (e.g. "branch"). A failing command
  (`run_git_command(repo, "notacmd")`) records `ok=false`. Reset proclog between.
- **Bridge** (`test_bridge`): `mg_magit_git_command(repo, "status")` returns 0 and
  the proclog snapshot has the `$ git status` entry.
- **Editor** (`test_editor`, pty): in `*magit-status*`, `:` `status` RET →
  `*magit-process*` opens showing `git status` output. Robust pty patterns
  (`C-x 1`, forced repaint, drain, event waits).

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Phases (each independently testable)

1. **Engine** `tokenize_cmdline` + `run_git_command` (+ proclog record) + engine
   tests.
2. **Bridge** `mg_magit_git_command` + bridge test.
3. **Editor** `:` binding + `magit_git_command` (eread → run → show
   `*magit-process*`) + pty test.

## Out of scope (deferred — tracked in todo.md)

- **FM-GIT-CMD-TTY** — auto-routing interactive/network subcommands (push/pull/
  fetch/`rebase -i`) to the tty-inherit path (`git_terminal`) so they work from
  `:`. v1 is captured-only (use the dedicated keys for those).
- `:` in the log / diff / other magit buffers (v1 is status-mode).
- Shell features (pipes, redirection, `&&`, variable/glob expansion) — `:` runs a
  single `git` invocation, argv-direct, no shell.
- Command history / completion in the `:` prompt.
