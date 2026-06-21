# FM-EDIFF-2 — per-region (hunk-level) conflict resolution

## Reality check on mg's display

A literal ediff (synchronized ours│merged│theirs live windows + intra-line
refinement) is **not faithfully buildable** on mg's display:
- mg has no faces/overlays — only whole-line color (`CTEXT`/`CMODE` in def.h).
  Intra-line refinement highlighting has nowhere to render.
- mg has no linked/synchronized window scrolling; `wheadp`/`splitwind` give
  independent windows only.

Building those would be a display-subsystem project, not a magit feature. What
*is* faithful and achievable — and is the functional core users reach ediff for
— is **choosing a side per conflict region** (finer than the file-level
ours/theirs of #63). In a terminal, one navigable buffer showing each region's
both sides beats three tiny unsynced windows.

## The slice

A `*magit-ediff*` buffer for one conflicted file: each conflict region rendered
with its ours / theirs text; `n`/`p` move between regions; `a` keep ours, `b`
keep theirs, `RET` keep both. Resolving rewrites that region in the working-tree
file; when no markers remain the file is staged and the buffer closes.

Entry: `E` on a Conflicts line (capital, vs `e`'s file-level transient).

## Engine (mg.git) — the testable core

- `struct conflict_hunk { std::string ours; std::string theirs; };`
- `conflict_hunks(repo, path)` — parse the working-tree file's merge markers
  (`<<<<<<<` … `=======` … `>>>>>>>`, plus diff3 `|||||||` base which is
  dropped) into regions, in order.
- extend `conflict_side` with `both`.
- `resolve_conflict_hunk(repo, path, index, side)` — re-parse, replace the
  index-th region's whole marker block with ours / theirs / ours+theirs, write
  the file. (Re-parsing each call keeps indices honest as regions collapse.)
- file staging when clean reuses the existing `stage` path (add_bypath clears
  the conflict once markers are gone).

## Bridge

- `mg_magit_conflict_hunks(repo, path, emit, ctx)` — emit one line per region
  (a short label + a preview) with a new `MG_LINE_CONFLICT_HUNK` kind carrying
  the index; or a structured count. (Render detail lives in the C UI.)
- `mg_magit_resolve_conflict_hunk(repo, path, index, side)` — side 0 ours / 1
  theirs / 2 both. 1 ok, 0 fail.

## UI

`*magit-ediff*` buffer (read-only), built from `conflict_hunks`: a header per
region (`Conflict N/M`) then its ours/theirs blocks. `a`/`b`/RET resolve the
region at point and rebuild; `n`/`p` navigate; `q` closes. When the rebuild
finds zero regions, stage the file and report "resolved".

## TDD

1. Engine doctest: a file with two conflict regions → `conflict_hunks` returns
   2 with the right ours/theirs; `resolve_conflict_hunk(0, ours)` leaves region
   0's ours text and one remaining region; resolving both → no markers; diff3
   `|||||||` base is parsed and dropped.
2. Bridge test: emit hunks for a conflicted file; resolve one via the bridge.
3. tmux: produce a 2-region conflict, `E`, `a` then `b`, file resolved + staged.

## Gates

- cpp + c-legacy clean, 0 warnings; OFF 0 magit symbols.
- One stacked PR `fm-ediff-2-hunks` → `fm-ediff-apply` (#65).

## Honestly out of scope

Synchronized live multi-pane scrolling and intra-line refinement — they need
faces/overlays + linked windows mg doesn't have. Noted, not half-built.
