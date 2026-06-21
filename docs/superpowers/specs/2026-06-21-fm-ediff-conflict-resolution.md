# FM-EDIFF — conflict resolution (magit's ediff use-case)

## Reframing

In magit, "ediff" is overwhelmingly used for one thing: **resolving merge/rebase
conflicts**. The visual side-by-side diff of two arbitrary buffers is an editor
feature; the *git* value is picking ours/theirs/both on a conflicted file and
staging the result. So the slice that matters is conflict resolution — and it
also closes the deferred **rebase conflict-resume** gap (the sequencer already
pauses on conflict; today the only way forward is hand-editing markers).

## What exists / what's missing (grounded)

- Rebase sequencer (`rebase_onto`/`pull_rebase`/`rebase_continue`) **pauses and
  leaves the conflicted index + workdir on disk** (`git_index_has_conflicts` →
  `rebase_result::conflicts`); `r r` continues. ✅ engine state is there.
- libgit2 gives us everything to resolve: `git_index` conflict entries
  (ancestor/our/their `git_index_entry`), `git_merge_file` (rendered markers),
  blob lookup for each side, `git_index_add`/`git_checkout` to write+stage. ✅
- **Gap 1:** the `status` enum (mg.magit) has no `conflicted`; `map_entry`
  never maps `GIT_STATUS_CONFLICTED` → conflicted files can be **invisible** in
  *magit-status*.
- **Gap 2:** no engine call lists conflicts or resolves one.
- **Gap 3:** no UI — no Conflicts section, no resolve keys.
- `splitwind()` exists (for the deferred visual panes).

## Slice boundary

### FM-EDIFF-1 (this slice) — detect + file-level resolution

The whole working loop, minus the visual 3-pane:

- **Engine (mg.git):**
  - add `status::conflicted`; map `GIT_STATUS_CONFLICTED` in `map_entry`.
  - `conflicts(repo)` → `vector<conflict_entry>{ path, has_ancestor }` from the
    index conflict iterator.
  - `resolve_conflict(repo, path, side)` with `side ∈ {ours, theirs}`: look up
    that stage's blob, write it to the workdir file, `git_index_add_bypath` +
    clear the conflict (`git_index_conflict_remove`). Returns void/expected.
  - (RET-to-edit needs nothing new: the on-disk file already has `<<<<<<<`
    markers; opening it is the existing file-visit path. Staging it with `s`
    after a manual edit already works.)
- **Bridge:** `mg_magit_conflicts(...)` emit (a new `MG_LINE_CONFLICT` kind) and
  `mg_magit_resolve_conflict(repo, path, take_theirs)`.
- **UI:** a **"Conflicts (N)"** status section (rendered like Tags/Worktrees);
  on a conflict line a small **transient** (we have transients now!) — `o` take
  ours, `t` take theirs, `RET` open file to hand-edit. After all are resolved,
  `r r` continues a paused rebase end-to-end.

This delivers the real workflow and unblocks rebase conflict-resume.

### FM-EDIFF-2 (deferred) — the visual 3-pane ediff

`splitwind` three ways into ours | merged | theirs, synchronized scrolling,
per-hunk `a`/`b` selection, and **intra-line refinement** (optional
header-only `dtl` Myers diff). This is the genuinely large UI piece — the
synchronized multi-pane redisplay and refinement, not the git logic. Explicitly
out of this slice.

Also a small adjacent follow-up: make `merge`/`revert`/`cherry-pick` **leave the
conflicted state** (like rebase) instead of aborting, so their conflicts feed
the same resolution flow. Currently they error and roll back.

## TDD plan

1. **Engine doctests** (reuse the `make_repo_rebase_conflict` fixture):
   - after a paused rebase, `conflicts()` lists the conflicted path;
   - `resolve_conflict(path, ours)` leaves the workdir file == ours' content and
     `git_index_has_conflicts` false; same for `theirs`;
   - `status` for the path reports `conflicted` before resolution.
2. **Bridge tests:** the status buffer shows `Conflicts (1)`; resolving via
   `mg_magit_resolve_conflict` clears it.
3. **tmux end-to-end:** drive a real rebase conflict → Conflicts section →
   take theirs on each → `r r` → "Rebase complete"; verify file contents.

## Verification gates

- `cpp` + `c-legacy` clean, 0 warnings; **OFF build 0 magit symbols**.
- Adding `status::conflicted` must not perturb the OFF build (enum lives in the
  module, gated).
- One stacked PR: `fm-ediff-conflict` → `fm-tr-popup` (#63). FM-EDIFF-2 is a
  later, separate slice.

## Why this slice, in this order

It's the part of "ediff" git users actually reach for, it reuses the conflict
state the rebase sequencer already produces, and it retires the longest-standing
deferred item (conflict-resume). The visual panes are real work but lower daily
value — deferred honestly rather than half-built.
