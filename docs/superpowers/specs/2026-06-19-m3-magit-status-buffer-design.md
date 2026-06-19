# M3 — `*magit-status*` buffer

**Status:** approved 2026-06-19 · **Branch:** `cpp-refactor`

A read-only Magit-style status buffer, invoked via `M-x magit-status` (bound to
`C-x g`). Builds on the M2 engine; adds refs + commit-DAG reads via libgit2.

## Target layout

```
On branch master
Head:     e6ece04 feat(magit): wire git modeline into the C core

Untracked files (2)
  open.txt
  untracked.txt

Unstaged changes (1)
  modified   foo.c

Staged changes (1)
  added      staged.txt

Recent commits
  e6ece04 feat(magit): wire git modeline into the C core
  b8d034c docs(plan): record M2d-1 PR #6 in the stack
```

## M3-1 — `mg.git` refs + DAG reads (libgit2, testable)

- `struct head_info { std::string branch, short_oid, summary; }` and
  `read_head(repo) -> std::expected<head_info, error>` (via
  `git_repository_head` / `git_reference_shorthand` / `git_commit_summary`).
  Unborn branch (no commits) → branch set, `short_oid`/`summary` empty.
- `struct commit_brief { std::string short_oid, summary; }` and
  `recent_commits(repo, n) -> std::expected<std::vector<commit_brief>, error>`
  via **`git_revwalk`** (push HEAD, `GIT_SORT_TIME`, take ≤ n). Unborn → empty.
- Short oid = 8 hex chars (`git_oid_tostr`).
- Tests (`test_git`): a libgit2 fixture repo *with a commit* → `read_head`
  returns the branch + that commit's short_oid/summary; `recent_commits(_,5)`
  returns it newest-first; an unborn repo → empty commits.

## M3-2 — bridge status composition (testable)

Callback-emit API (no buffer-size limit; formatting stays in C++):
```c
typedef void (*mg_magit_emit_fn)(void *ctx, const char *line);
int mg_magit_status_buffer(const char *repo, mg_magit_emit_fn emit, void *ctx);
```
`bridge.cpp` composes `read_head` + `repo_status` (grouped into untracked /
unstaged / staged) + `recent_commits`, calling `emit(ctx, line)` per line, and
returns the line count. Tests (`test_bridge`): emit into a `std::vector<string>`;
assert the `On branch` header, the section headers with counts, and a commit line
for a libgit2 fixture.

## M3-3 — C command + bindings (build + pty verified)

- `src/magit_cmd.c` (compiled into `mg` only under `#ifdef ENABLE_NATIVE_MAGIT`):
  `magit_status(int f, int n)` → `bfind("*magit-status*", TRUE)` → `bclear` →
  `BFREADONLY` → `mg_magit_status_buffer(getcwd, emit, bp)` where `emit` =
  `addlinef(bp, "%s", line)` → `popbuf`. Mirrors `listbuffers`.
- Register `magit_status` in `funmap.c` `functnames[]` and bind `C-x g` in
  `keymap.c` — both `#ifdef ENABLE_NATIVE_MAGIT`.
- Declared in a small `src/magit_cmd.h` (or `def.h` guard).
- Verify: cpp build links; `M-x magit-status` / `C-x g` in a pty-driven repo
  shows the buffer; OFF build unaffected (no command, no symbols).

## Refresh & scope

Refresh = re-run the command. **Out of scope (later):** interactive
staging/unstaging, diffs/hunks, section folding/navigation, auto-refresh of the
buffer on fs events.

## Follow-on (after M3): branch name in the modeline

Reuse `read_head().branch` so the modeline reads e.g. `master git *1 ?2`
(extend `summarize` or the monitor's published string).
