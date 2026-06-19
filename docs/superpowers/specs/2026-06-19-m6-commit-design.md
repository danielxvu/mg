# M6 — commit (`c`) from the status buffer

**Status:** approved (roadmap item) 2026-06-19 · **Branch:** `cpp-refactor`

`c` in `*magit-status*` opens a commit-message buffer; `C-c C-c` commits the
staged changes, `C-c C-k` aborts.

## M6-1 — `mg.git` commit (testable)

`commit(repo, message) -> std::expected<std::string, error>` (returns the new
short oid):
- `git_signature_default` (reads `user.name`/`user.email`; error if unset).
- `git_index_write_tree` → `git_tree_lookup` (the staged tree).
- Parent = current HEAD commit if any (unborn → no parent / root commit).
- `git_commit_create(.., "HEAD", sig, sig, NULL, message, tree, n_parents, ..)`.
- Tests (`test_git`): a repo with `user.*` config + a staged file → `commit("msg")`
  succeeds, `read_head().summary == "msg"`, `recent_commits` includes it, and the
  staged file is no longer in `repo_status`.

## M6-2 — bridge (testable)

`int mg_magit_commit(const char *repo, const char *message)` → 1/0.
Test (`test_bridge`): stage a file, commit, the status no longer lists it.

## M6-3 — C command + commit buffer (build + pty)

- `c` in `magit-status-mode` → `magit_commit`: create an **editable** buffer
  `*magit-commit*` (not read-only), switch to it, set a `magit-commit-mode`
  keymap, show a hint line.
- `magit-commit-mode` keymap has a **`C-c` prefix submap**: `C-c C-c` →
  `magit_commit_finish`, `C-c C-k` → `magit_commit_abort` (mirrors `cXmap`'s
  prefix structure).
- `magit_commit_finish`: read the buffer's text (concatenate lines), reject if
  empty, `mg_magit_commit(cwd, msg)`, kill the commit buffer, return to
  `*magit-status*` + refresh.
- `magit_commit_abort`: kill the commit buffer, return to status.
- Verify (pty): `c` → type a message → `C-c C-c` → a new commit appears under
  "Recent commits" and the staged file leaves the status. OFF build unaffected.

## Out of scope

`--amend`, sign-off, message templates / comments stripping, editing an existing
commit, hooks.
