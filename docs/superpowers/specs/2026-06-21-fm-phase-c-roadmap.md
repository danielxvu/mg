# Full Magit — Phase C roadmap (honest gap vs real magit)

**Status:** Phases A+B complete (PRs #31–#45) — the daily-driver core. This
phase maps the *remaining* gap to real Magit and prioritizes it. Same gating
(`ENABLE_NATIVE_MAGIT`), per-slice TDD (`test_git` → `test_bridge` →
`magit_cmd.c` keys → pty), and stacked-PR rhythm. libgit2 1.9.4 (rebase API
present).

## What we already have (A+B)

Status buffer (untracked/unstaged/staged, stashes, branches, recent commits,
unpushed/unpulled) · stage/unstage/discard by file, hunk, region · stage-all/
unstage-all · commit/amend/extend/reword · inline diffs + section folding · log
buffer + commit diff · branch checkout/create/delete/rename · stash push/pop/
apply/drop/show · reset/revert/merge · fetch/pull/push (+ ssh-agent & HTTPS
auth) · visit/help/section-nav.

## Gap analysis — the big missing pieces

1. **Rebase** (`magit-rebase`, `r`) — the flagship. Non-interactive (onto
   upstream / elsewhere / a subset) + **interactive** (`*git-rebase-todo*`:
   pick/reword/edit/squash/fixup/drop + reorder + exec) + the **sequencer**
   (continue/skip/abort, conflict surfacing). libgit2 gives the apply-loop
   (`git_rebase_init/next/commit/finish/abort`); the interactive *plan* and
   squash/reword semantics are ours to build on top.
2. **Cherry-pick** (`A`) — apply a commit (and a sequence; continue/skip/abort).
3. **Tags** (`t`) — create (lightweight + annotated), delete.
4. **Log power** — `--graph`, file/range/branch logs, `-S`/`-G` search, reflog,
   and **acting on the commit at point** (cherry-pick / revert / reset-here /
   branch-here).
5. **Push/pull/fetch variants** — force, set-upstream (`-u`), push tags, push
   elsewhere; pull rebase-vs-merge; fetch all / prune.
6. **Commit variants** — fixup / squash / instant-fixup, `--signoff`,
   `--no-verify`, GPG signing.
7. **Transient UI** — real switches/options/flags with live state. We use prefix
   submaps; a faithful transient port is a separate, large effort.
8. **In-progress sections + process buffer** — rebase/merge/sequence state in
   the status buffer; a `*magit-process*` view of git output.
9. **Long tail** — bisect, submodule/subtree, worktree, blame, notes, ediff,
   gitignore editing, bookmarks, multi-item marking.

## Prioritized slices

- **FM-RB — rebase** (flagship; do first). Decomposed:
  - **FM-RB-1** — engine non-interactive `rebase_onto(repo, upstream, onto?)`
    via `git_rebase_init` → loop `git_rebase_next`/`git_rebase_commit` →
    `git_rebase_finish`; on conflict, `git_rebase_abort` + error (clean tree).
    Bridge + UI `r u` (onto upstream) / `r e` (onto a prompted branch). pty: a
    linear rebase replays commits onto the new base.
  - **FM-RB-2** — sequencer: `r r` continue / `r s` skip / `r a` abort; a
    "Rebasing" in-progress status section; conflicts left in the tree for the
    user to resolve then continue.
  - **FM-RB-3** — interactive: a `*git-rebase-todo*` buffer
    (pick/reword/edit/squash/fixup/drop + reorder lines) executed via the
    rebase loop. The hardest slice.
- **FM-A — cherry-pick** (`A A` pick / `A a` apply): `git_cherrypick`.
- **FM-TAG — tags** (`t t` create / `t k` delete): `git_tag_create`/`_delete`.
- **FM-LP — log power**: log args (`--graph`-ish, file/range), act-at-point in
  `*magit-log*` (cherry-pick / revert / reset-here / branch-here).
- **FM-PV — push/pull/fetch variants**: force, `-u`, tags; pull rebase; fetch
  prune.
- **FM-CV — commit variants**: fixup/squash/instant-fixup, signoff.

## Out of scope (still — revisit after FM-RB/A/TAG/LP)

The transient *popup UI* port itself, bisect, submodule/subtree, worktree,
blame, ediff, notes, gitignore editing, bookmarks. GPG signing depends on a
configured key + agent (like ssh auth — wire but hard to verify in-sandbox).

## Per-slice contract

`mg.git` op (TDD in `test_git`, libgit2 fixtures, no shell git) → `extern "C"`
bridge (TDD in `test_bridge`) → `magit_cmd.c` keys → pty/tmux end-to-end. Build
`cmake --preset cpp && ctest --preset cpp`; OFF build (`c-legacy`) unaffected.
Stacked PR per slice, frozen on the previous tip.
