# M9 — act on stashes & branches

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

M8-3 surfaced Stashes and Branches as read-only sections. M9 makes them
actionable: apply/drop a stash, check out a branch. Two slices, built on the
existing `MG_LINE_STASH` / `MG_LINE_BRANCH` meta the buffer already records.

## M9-1 — stash actions (engine + bridge + keys)

```cpp
std::expected<void, error> stash_apply(std::string repo, std::size_t index);
std::expected<void, error> stash_drop (std::string repo, std::size_t index);
```
- `stash_apply` = `git_stash_apply` (reapply, keep the stash); `stash_drop` =
  `git_stash_drop` (delete it). Both `std::expected`/RAII.
- Bridge `mg_magit_stash_apply` / `mg_magit_stash_drop` (take the stash index).
- `magit_cmd.c`: on a `MG_LINE_STASH` line, `a` → apply, `k` → drop (extend the
  existing discard key). The stash index is the meta `hunk` field reused, or
  parsed from `stash@{N}` — reuse `hunk` (set it to the index in the emit).
- Tests (`test_git`): a stashed change → `stash_apply` makes the file dirty
  again with the stash still listed; `stash_drop` empties `stashes()`.

## M9-2 — branch checkout (engine + bridge + key)

```cpp
std::expected<void, error> checkout_branch(std::string repo, std::string name);
```
- `git_checkout_tree(branch_tree, SAFE)` then `git_repository_set_head(
  "refs/heads/<name>")`. Refuses (surfaces the libgit2 error) on conflicts.
- Bridge `mg_magit_checkout` (branch name).
- `magit_cmd.c`: on a `MG_LINE_BRANCH` line, `b` → checkout. The branch name is
  the meta `path` (set it in the emit, sans the `* ` marker).
- Tests (`test_git`): `checkout_branch` to a second branch → `read_head().branch`
  is that branch.

## Keys (kept in ascending order; guard covers it)

`a` (0x61) and `b` (0x62) slot between `?` (0x3F) and `c` (0x63). `k` (discard)
gains a stash case. All other keys unchanged.

## Out of scope

Stash pop (apply+drop in one) — `a`+`k` suffices for now; creating a stash from
the buffer; branch create/delete/rename; remote-tracking checkout; conflict
resolution UI (a failed checkout just reports the error).
