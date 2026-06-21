# FM-RB-REPLAY-CONFLICT — conflict-resumable interactive rebase replay

## The last rebase gap

The interactive executor (`run_plan`) cherry-picks each step in memory; on a
conflicting step it **aborts with an error** ("interactive rebase conflict
(aborted)"). Every other conflict path in mg now pauses for resolution
(non-interactive rebase #46/#47, merge/revert/cherry-pick #64) — interactive
*replay* is the one that still bails.

## Why it composes (no new mechanism)

FM-RB-EDIT (#66) already built everything needed:
- `run_plan` persists `{orig-HEAD, remaining steps}` to `.git/MG_REBASE_TODO`
  and returns a pause result;
- `rebase_continue` resumes `run_plan` from the user's HEAD as the new tip;
- `rebase_abort` resets to the saved HEAD; `rebase_in_progress` tracks the file.

A replay conflict is the *same pause*, with one difference: at an `edit` stop
HEAD becomes the cleanly-applied commit, whereas at a conflict HEAD must be the
**previous good tip** with the conflict materialized in the working tree, so the
user resolves it (the existing `e o`/`e t` flow) and commits — that commit is
the applied step. The remaining plan then replays on `r r`. Identical resume
path.

## Engine change (localized to one branch of `run_plan`)

Replace the conflict-abort:

```cpp
if (git_index_has_conflicts(idx.get()))
    return std::unexpected(error{0, "interactive rebase conflict (aborted)"});
```

with materialize-and-pause:

1. `set_head_to(tip)` — move the branch to the last good tip + checkout clean.
2. repo-level `git_cherrypick(repo, apply, SAFE)` — re-apply the conflicting
   commit onto HEAD, leaving conflict markers in the working tree + the
   conflicted index + CHERRY_PICK_HEAD (same as standalone cherry_pick #64).
3. `write_rebase_todo(orig_head, steps[i+1..])` — persist the rest.
4. `return rebase_result::conflicts;`

`rebase_continue`'s MG_REBASE_TODO branch additionally **guards**: if the index
still `has_conflicts`, refuse ("resolve conflicts first") rather than silently
dropping the unresolved step. On a clean resume, `commit()` (which already
honors/cleans CHERRY_PICK_HEAD, #64) has produced the resolved commit as HEAD,
and the tail replays — possibly pausing again at the next conflict (the file is
re-persisted each time; removed only on `done`).

## Bridge / UI

- Bridge: none. `rebase_interactive` already returns via `rebase_code`
  (conflicts → 2); `mg_magit_rebase_interactive` already uses it.
- UI: the *git-rebase-todo* run handler gains an `rc == 2` case → ewprintf
  "Conflict during rebase -- resolve (e o / e t), then r r" + refresh. The
  Conflicts + Rebasing status sections already render (rebase_in_progress +
  conflicts()), and `r r`/`r a` already drive continue/abort.

## TDD

1. Engine: `make_repo_for_interactive` + a commit that touches the same file as
   another on master so a replay step conflicts; a plan replaying it onto master
   returns `conflicts`, `rebase_in_progress` true, `conflicts()` lists the path.
   Resolve via `resolve_conflict`, `commit`, `rebase_continue` → `done`, tail
   applied. A second case: `rebase_abort` after the pause restores the tip.
2. Engine: continue with the index still conflicted → error (the guard).
3. tmux: `r i` plan that conflicts on replay → Conflicts section → `e t` →
   `c c` → `r r` → complete.

## Gates

- cpp + c-legacy clean, 0 warnings; OFF 0 magit symbols.
- One stacked PR `fm-rb-replay-conflict` → `fm-rb-edit` (#67).

## Honest note (squash/fixup)

A conflicting `squash`/`fixup` step resolves as a normal pick (a separate commit
the user commits), not folded into the previous commit — folding-after-resolve
would need to re-run the squash on continue. Rare (most conflicts are picks);
documented, not silently wrong.
