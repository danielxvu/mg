# M2c — libgit2 status reader + modeline summary

**Status:** approved 2026-06-19 · **Task:** M2c (third of four M2 sub-tasks) ·
**Branch:** `cpp-refactor`

## Pivot

Git access uses **libgit2** (1.9.4, `+threadsafe`, installed) rather than
spawning the `git` binary: structured status entries instead of porcelain text,
no fork/exec per poll, and the foundation for future commit-DAG work
(`git_revwalk`). The C API pairs with our `extern "C"` philosophy.

Consequence: M1's porcelain **parser** (`parse_status`/`parse_status_line`) is
superseded and **retired**. Its value types (`status`, `file_status`) are kept
as the domain model libgit2 maps into. Retirement is a forward removal commit;
the frozen `m1-magit` branch / PR #2 stays as history (PRs #3/#4 are stacked on
it, so it is not rebased).

## M2c-1 — domain cleanup (pure)

- `mg.magit` (`magit.cppm`): **keep** `status` enum + `file_status`; **remove**
  `parse_error`, `parse_status_line`, `parse_status` and their tests.
- **Add** `summarize(std::span<const file_status>) -> std::string`, the modeline
  string. Counts: *staged* = index side changed (not unmodified/untracked);
  *unstaged* = worktree side changed (not unmodified/untracked); *untracked* =
  worktree is untracked. Format (plain ASCII):
  - clean → `"git clean"`
  - else → `"git"` + ` *<staged>` + ` +<unstaged>` + ` ?<untracked>` for the
    nonzero parts, e.g. `"git *2 +1 ?3"`.
- Tests (`test_magit`): empty→clean; one staged→`git *1`; one unstaged→`git +1`;
  one untracked→`git ?1`; mixed→`git *2 +1 ?3`.

## M2c-2 — `mg.git` libgit2 reader (integration)

- New module `mg.git` (`src/magit/git.cppm`), `import mg.magit;`, links libgit2
  (found via `pkg-config`/`find_library`, gated by `ENABLE_NATIVE_MAGIT`).
- **RAII wrappers** owning the C handles: a refcounted libgit2 init guard, and
  move-only owners for `git_repository*` and `git_status_list*` (custom
  deleters). No raw owning pointers escape.
- `git_error { int klass; std::string message; }` from `git_error_last()`.
- `repo_status(std::string path) -> std::expected<std::vector<file_status>, git_error>`:
  open repo → `git_status_list_new` with `GIT_STATUS_OPT_INCLUDE_UNTRACKED` →
  for each entry map `GIT_STATUS_INDEX_*` → `file_status::index` and
  `GIT_STATUS_WT_*` → `file_status::worktree`, path from the entry's delta →
  return the vector (or `git_error`).
- **Integration test** (`test_git`) builds its fixture with libgit2 itself
  (`git_repository_init`, write a file + `git_index_add_bypath`, leave another
  untracked) — no shell git — then asserts `repo_status` returns a staged entry
  and an untracked entry.

## Data flow (with M2d)

watcher event (M2b) → `repo_status(path)` (libgit2, M2c-2) → `summarize(...)`
(M2c-1) → publish to modeline over the `extern "C"` bridge (M2d).

## Error handling

`repo_status` returns `std::expected<…, git_error>`; libgit2 non-zero returns
become `std::unexpected(git_error{…})`. RAII guarantees handles are freed on
every path.

## Out of scope

The background thread + `extern "C"` bridge + C-core hook (M2d). Commit-DAG and
refs via libgit2 are later M-series slices.
