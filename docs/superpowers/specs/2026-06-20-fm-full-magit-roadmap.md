# Full Magit — roadmap (gap analysis + prioritized slices)

**Status:** approved direction (user: "full magit support; complete what we have
first, then pick the most valuable") 2026-06-20 · **Branch:** `cpp-refactor`

Studied real magit (cloned `magit/magit`, read `magit-dispatch`, `magit-status`
sections, `magit-apply`, `magit-commit`). This roadmap maps the gap and orders
the work: **Phase A completes our partial features to magit fidelity; Phase B
adds the highest-value missing commands.** Every slice keeps the existing
gating (`ENABLE_NATIVE_MAGIT`) and the per-iteration TDD + stacked-PR rhythm.

## What we have (M1–M9)

modeline status · `*magit-status*` (branch/HEAD, untracked/unstaged/staged,
stashes, recent commits, branches) · stage/unstage file + hunk · discard file +
stash · commit (basic) · inline diffs (TAB) · RET visit · `?` help · M-n/M-p
nav · stash apply/drop · branch checkout.

## Phase A — complete existing features (do first)

- **FM-C — commit, full** (completes M6): `amend` (c a), `extend` (c e),
  `reword` (c w). `c` becomes a prefix submap (`c c` commit / `c a` / `c e` /
  `c w`), mirroring magit's commit menu. libgit2: amend = new commit with HEAD's
  parents + index tree + message; extend = amend keeping HEAD's message; reword
  = amend keeping HEAD's tree.
- **FM-S — staging, full** (completes M4/M7): **line/region staging** (stage/
  unstage/discard the selected lines of a hunk — magit's signature), plus
  **stage-all** (S) / **unstage-all** (U). Region patch = a one-hunk patch with
  only the marked +/- lines (others as context), `git_apply` to the index.
- **FM-T — status sections, full** (completes M3/M8): **unpushed/unpulled**
  (ahead/behind upstream) sections; push/pull remote + upstream in the headers;
  **section folding** (TAB collapses any section, not just files).
- **FM-B — branches, full** (completes M9): create (b c), delete (b k), rename
  (b m) — `git_branch_create`/`_delete`/`_move`.
- **FM-Z — stash, full** (completes M9): create/push (z z), pop (z p), show
  (RET on a stash → its diff) — `git_stash_save`/`pop`, diff of the stash.

## Phase B — highest-value new commands

- **FM-L — log buffer** (l): a `*magit-log*` buffer (revwalk, oneline + graph-ish),
  `RET` shows a commit's diff. Very high value, builds on existing revwalk.
- **FM-R — remote ops** (f fetch / F pull / P push): `git_remote_fetch`/`push`.
  ⚠ needs network transports — utf8proc-style check first (libgit2 has SSH via
  libssh2 + HTTPS via Security.framework, both linked; verify on a test remote).
- **FM-X — reset / revert / merge** (X / V / m): `git_reset`, revert a commit,
  merge a branch.

## Recommended order & "most valuable" picks

Phase A in the order above (FM-C first — `amend` is the most-used missing
command and a clean libgit2 add). Within Phase B, **FM-L (log)** and **FM-R
(push/pull/fetch)** are the highest daily value. Line-level staging (FM-S) is
magit's signature feature and the most impressive completion.

## Out of scope (for now)

Bisect, ediff, submodule/subtree, worktree, notes, blame, rebase-interactive,
gitignore editing, bookmarks, the transient *popup UI* itself (we use prefix
submaps instead). Revisit after Phase A/B.

## Per-slice contract

`mg.git` op (TDD in `test_git`) → bridge (TDD in `test_bridge`) → `magit_cmd.c`
keys → pty end-to-end. Build `cmake --preset cpp && ctest --preset cpp`; OFF
build unaffected (magit is `ENABLE_NATIVE_MAGIT`-gated). Stacked PR per slice.
