# M7 — inline diffs + hunk staging (the big one)

**Status:** approved (roadmap item) 2026-06-19 · **Branch:** `cpp-refactor`

`TAB` on a file in `*magit-status*` expands it to show its diff; `s`/`u` on a
hunk stage/unstage just that hunk. Decomposed into four slices; built incrementally.

## M7-1 — `mg.git` diff reading (testable, this slice)

```cpp
struct diff_line { char origin; std::string content; }; // '+', '-', ' ', …
struct hunk      { std::string header; std::vector<diff_line> lines; };
std::expected<std::vector<hunk>, error>
file_diff(std::string repo, std::string path, bool staged);
```
- `staged == false` → `git_diff_index_to_workdir` (unstaged); `staged == true` →
  `git_diff_tree_to_index` (staged), each scoped to `path` via a pathspec.
- `git_patch_from_diff` → iterate `git_patch_get_hunk` /
  `git_patch_get_line_in_hunk`, mapping each line's origin + content.
- Tests (`test_git`): commit a file, append a line → `file_diff(_, _, false)`
  has a hunk with a `'+'` line carrying the new text; staging it →
  `file_diff(_, _, true)` shows it on the staged side.

## M7-2 — render diffs inline (bridge + buffer)

- `mg_magit_status_buffer` gains an "expanded paths" input; under each expanded
  file it emits the hunk header + diff lines (kind `MG_LINE_HUNK` / `MG_LINE_DIFF`
  with the owning path + hunk index).
- `magit_cmd.c`: `TAB` toggles the file at point in an "expanded" set, then
  refreshes. The line→meta map records hunk indices for hunk ops.

## M7-3 — hunk-level staging (the hard part)

- `stage_hunk(repo, path, hunk_index)` / `unstage_hunk(...)`: build a one-hunk
  patch and `git_apply` it to the index (`GIT_APPLY_LOCATION_INDEX`), or
  reverse-apply to unstage. Bridge `mg_magit_stage_hunk` / `unstage_hunk`.
- Tests: stage one hunk of a two-hunk change → only that hunk is staged.

## M7-4 — C interaction

- In `magit-status-mode`: `TAB` expand/collapse; on a hunk line, `s` →
  `stage_hunk`, `u` → `unstage_hunk`; refresh keeps point near the hunk.

## Order & risk

M7-1 (read) and M7-2 (render) are the high-value, lower-risk half — they make
diffs *visible*. M7-3 (`git_apply` hunk staging) is the genuinely hard part and
may need its own iteration. Build 1 → 2 → 3 → 4.

## Out of scope

Line-level (sub-hunk) staging; word-diff; image/binary diffs; diff of untracked
files (show whole-file add).
