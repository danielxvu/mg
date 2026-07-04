# FM-SHOW-REFS — the `y` refs overview (*magit-refs*)

## Why

The status buffer's Branches/Tags sections are deliberately bounded (top 20,
"+N more") because a big repo has thousands of refs — the section's own comment
says "Magit shows all refs in a dedicated refs view, never inline in status."
This is that view: `magit-show-refs` (`y`), a `*magit-refs*` buffer listing all
local branches, remote branches, and tags, with ahead/behind counts vs HEAD.

## Decided model (from brainstorming)

Faithful to Magit's `magit-refs-mode`:
- **`RET` visits** — shows the ref's tip commit in `*magit-commit*` (Magit's
  `magit-visit-ref` default), reusing the log buffer's RET machinery (ref rows
  carry their tip oid). Checkout is NOT on RET.
- **`b`** opens the branch menu (checkout lives there), `g` rebuilds, `q` quits.

Layout:
```
Refs (repo HEAD: master)

Branches (3)
* master          origin/master
  feature-x       ahead 2, behind 5
  old-thing       behind 40

Remotes (2)
  origin/master
  origin/feature-x  ahead 1

Tags (4)
  v1.0
  ...
```
- Local branches: `* ` HEAD marker (matching the status section). Non-HEAD rows
  show ahead/behind **vs HEAD** (omit a zero side; equal tips show nothing).
  The HEAD row shows its upstream shorthand instead (from `upstream_info`-style
  lookup), or nothing if untracked.
- Remote branches: one flat `Remotes (N)` section (grouping per remote is a
  deferral), ahead/behind vs HEAD the same way.
- Tags: names only (Magit shows no counts for tags), **full list** — this is
  the dedicated view, no `kMaxRefsShown` cap.

## Architecture

### 1. Engine (`src/magit/git.cppm`)

New types + function, following the `branches()`/`decorations()` idioms:
```cpp
struct ref_row {
    std::string name;     // shorthand: "master", "origin/master", "v1.0"
    std::string oid;      // full tip oid (tags: peeled to the commit)
    bool        is_head;  // locals only
    long        ahead;    // commits on this ref not on HEAD (-1 = unknown)
    long        behind;   // commits on HEAD not on this ref (-1 = unknown)
};
struct refs_overview_result {
    std::string head_name;   // "master", or "" when detached/unborn
    std::string head_oid;    // "" when unborn
    std::string upstream;    // HEAD's upstream shorthand, "" if none
    std::vector<ref_row> locals, remotes, tags;
};
std::expected<refs_overview_result, error> refs_overview(std::string repo);
```
- Branches via `git_branch_iterator` `GIT_BRANCH_ALL` (split local/remote by
  type; skip unresolvable refs like the `origin/HEAD` symbolic ref — for THIS
  listing it is noise, unlike decoration where git itself shows it).
- Tags via the `refs/tags/*` glob iterator, peeled to commits (the
  `decorations()` idiom); `ahead`/`behind` = -1 (not computed).
- Ahead/behind per branch vs HEAD's oid via `git_graph_ahead_behind` (the
  `upstream_info` idiom, note the argument order gives "ref vs HEAD"); on
  error, -1/-1 (row still listed, no counts). HEAD's own row: 0/0 skipped.
- Unborn HEAD: empty result with `head_name`/`head_oid` empty (not an error).

### 2. Bridge (`src/magit/bridge.cpp`, `bridge.h`)

```c
int mg_magit_refs_buffer(const char *repo_path, mg_magit_emit_fn emit, void *ctx);
```
Emits, via the existing emit ABI:
- Header: `Refs (HEAD: master)` / `Refs (HEAD detached at <short>)` /
  `Refs (no commits yet)` as `MG_LINE_SECTION`.
- `Branches (N)` section (`MG_LINE_SECTION`), rows as `MG_LINE_BRANCH` with
  **`path` = the full tip oid** (NOT the name — RET reuses the commit-view
  action, which reads an oid). Row text: `* name  <upstream>` for HEAD,
  `  name  ahead A, behind B` otherwise (count phrase omitted when 0/unknown).
- `Remotes (N)` rows as `MG_LINE_BRANCH` too (same RET semantics).
- `Tags (N)` rows as `MG_LINE_TAG`, `path` = the peeled tip oid.
Returns the line count, 0 when the repo can't be read.

**Note:** the status buffer's `MG_LINE_BRANCH` rows carry `path` = the branch
NAME (checkout-on-RET there); the refs buffer's carry the OID. The two live in
different buffers with different keymaps, so the meaning of `path` is
per-buffer — documented at both emit sites.

### 3. Editor (`src/magit_cmd.c`)

- `magit_show_refs` bound to `y` in `magitmap` (free key; strict-ascending
  insert). Follows the `*magit-log*` open/build pattern exactly: `bfind`
  `*magit-refs*`, persist `b_cwd`, `bclear`, emit via a callback that records
  per-line kind/oid (the log's oid-map pattern or a refs-local equivalent),
  `popbuf`, set a `magit-refs-mode` with a `magrefsmap` keymap.
- `magrefsmap`: `RET` → show the row's tip commit in `*magit-commit*` (the same
  action the log's RET uses, reading the recorded oid), `b` → the branch menu
  (`magit_menu_branch`), `g` → rebuild the refs buffer, `q` → close. Keys in
  strict-ascending order; register the keymap + mode alongside the log's.
- NOT a color buffer in v1 (recorded deferral).

## Error handling / edges

- Unborn HEAD (fresh `git init`): buffer renders `Refs (no commits yet)` and
  empty sections — no crash, no error dialog.
- Detached HEAD: header `Refs (HEAD detached at <short>)`; no branch row is
  starred; counts still computed vs the detached oid.
- No remotes / no tags: those sections are omitted entirely.
- `git_graph_ahead_behind` failure on one ref: that row lists without counts.
- RET on a section header / blank line: the reused `magit_log_visit` action
  reports "Not on a commit" (its existing message), no action taken.

## Testing

- **Engine** (`test_git`): a fixture with two local branches where one is
  ahead 1 / behind 1 of the other (commit on each after branching) + a tag →
  `refs_overview` returns the right `locals` names/counts/`is_head`, the tag
  with a peeled oid, and `head_name`. Detached HEAD case: `head_name` empty,
  rows still listed.
- **Bridge** (`test_bridge`): `mg_magit_refs_buffer` emits the `Branches (`
  section, a starred HEAD row, and a `MG_LINE_BRANCH` row whose `path` is a
  40-hex oid (not the name).
- **Editor** (`test_editor`, pty): `y` in `*magit-status*` opens `*magit-refs*`
  (waits for `Branches (`); single session, presence-only, drained exit via
  `quit_neomg`.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (recorded in todo.md)

- Coloring the refs buffer (local green / remote blue / tag yellow, like the
  log decorations).
- Per-remote sub-sections; `k` delete-branch from the refs buffer; Magit's
  comparison-ref selection (`magit-refs-set-comparison`); a `y` transient with
  args (Magit's `y` is itself a transient; v1 goes straight to the buffer).
