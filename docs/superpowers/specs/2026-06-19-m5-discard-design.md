# M5 — discard changes (`k`) in the status buffer

**Status:** approved (roadmap item) 2026-06-19 · **Branch:** `cpp-refactor`

`k` on a file in `*magit-status*` throws away its changes, after confirmation.

## Semantics (decided)

- **Untracked file** → delete it from the worktree.
- **Tracked file** (unstaged and/or staged changes) → revert it to HEAD
  (`git_checkout_head` with `GIT_CHECKOUT_FORCE` for that path), discarding both
  worktree and index changes for the file.
- Always **confirm** in the UI (`eyesno`) — discard is destructive and
  irreversible.

## M5-1 — `mg.git` discard (testable)

`discard(repo, file) -> std::expected<void, error>`:
- `git_status_file`; if `GIT_STATUS_WT_NEW` (untracked), remove the worktree file
  (`git_repository_workdir` + `std::filesystem::remove`); else
  `git_checkout_head` with `GIT_CHECKOUT_FORCE` + a one-path pathspec.
- Tests (`test_git`): discard an untracked file → it is gone and absent from
  `repo_status`; discard a modified tracked file → back to unmodified.

## M5-2 — bridge (testable)

`int mg_magit_discard(const char *repo, const char *path)` → `mg::git::discard`.
Test (`test_bridge`): discard an untracked file → it drops out of the composed
status.

## M5-3 — C command (build + pty)

`magit_discard(f, n)` bound to `k` in `magit-status-mode`:
- Resolve the file at point (reuse `magit_at_point`). Any file row (untracked /
  unstaged / staged) is discardable.
- `eyesno("Discard changes to <path>")`; on yes, `mg_magit_discard(cwd, path)`,
  then refresh. On no / non-file line, do nothing.
- Verify (pty): `k` on an untracked file → confirm → it disappears from the
  buffer. OFF build unaffected.

## Out of scope

Hunk/line-level discard (comes with M7 diffs); discarding whole sections.
