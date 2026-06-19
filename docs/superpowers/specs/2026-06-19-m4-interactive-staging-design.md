# M4 — interactive staging in the `*magit-status*` buffer

**Status:** approved 2026-06-19 · **Branch:** `cpp-refactor`

Stage / unstage whole files from the status buffer with `s` / `u`, then refresh.
Builds the buffer-interaction infrastructure (mode keymap, point→file resolution,
refresh) that later features reuse.

## M4-1 — `mg.git` index ops (testable)

- `stage(repo, path) -> std::expected<void, error>`: `git_status_file` to decide,
  then `git_index_add_bypath` (modified/untracked/new) or `git_index_remove_bypath`
  (worktree-deleted) + `git_index_write`.
- `unstage(repo, path) -> std::expected<void, error>`: if HEAD exists,
  `git_reset_default` the path to HEAD (staged change → back to unstaged); if the
  branch is unborn, `git_index_remove_bypath` (staged-new → untracked).
- RAII wrappers for `git_index` / `git_object`.
- Tests (`test_git`): stage an untracked file → `repo_status` shows it staged;
  unstage a staged-new file (unborn) → untracked; unstage a staged modification
  (committed repo) → unstaged-modified.

## M4-2 — bridge: enriched emit + stage/unstage (testable)

- Extend the emit callback so the C side can map a buffer line to a file:
  ```c
  enum { MG_LINE_OTHER, MG_LINE_UNTRACKED, MG_LINE_UNSTAGED, MG_LINE_STAGED };
  typedef void (*mg_magit_emit_fn)(void *ctx, const char *line, int kind,
                                   const char *path);  /* path NULL for non-file lines */
  ```
  `mg_magit_status_buffer` passes the kind + path per line.
- `int mg_magit_stage(const char *repo, const char *path)` /
  `mg_magit_unstage(...)` → call `mg::git::stage`/`unstage`; return 1 on success.
- Tests (`test_bridge`): emit into a vector of `{line, kind, path}`; assert the
  untracked/staged entries carry the right kind+path; `mg_magit_stage` then
  `repo_status` shows the file staged.

## M4-3 — C interaction layer (build + pty verified)

- A `magit-status-mode` keymap (set up like `listbufmap` in `listbuffers`):
  `s` `magit_stage`, `u` `magit_unstage`, `g` `magit_refresh`, `q` (bury buffer).
- On each render, `magit_cmd.c` records a static **line → {kind, path}** map from
  the enriched emit callback (one *magit-status* buffer, so a single static map).
- `magit_stage`/`magit_unstage` resolve the file at the dot's line number
  (`getlinenum`/`count_lines`), call `mg_magit_stage`/`unstage`, then **refresh**
  (re-render in place, keeping point near the same line).
- `magit_refresh` re-renders; `magit_status` and refresh share the build routine.
- Verify (pty): open status, move onto an untracked file, `s` → it appears under
  Staged; `u` on a staged file → back. OFF build unaffected.

## Scope & out of scope

**In:** whole-file stage/unstage, refresh, quit, the mode keymap + point
resolution. **Out (this task):** hunk/line staging, discard, commit, diffs.

## Future roadmap (planned, not dropped)

- **M5 — discard** (`k`): revert an unstaged change / delete an untracked file
  (`git_checkout_head` for tracked, unlink for untracked) with confirmation.
- **M6 — commit** (`c`): pop a commit-message buffer; `C-c C-c` runs
  `git_commit_create`; refresh.
- **M7 — inline diffs + hunk/line staging** (`TAB` to expand a file to its diff;
  `s` on a hunk): the big one — `git_diff`, hunk model, `git_apply`.
- **M8 — breadth:** extra sections (stashes, unpushed/unpulled vs upstream,
  in-progress merge/rebase), `l` log, `z` stash, `f`/`F`/`P` fetch/pull/push,
  `b`/`t` branch/tag, `?` help popup, `RET` visit-file.
