# FM-EDIFF-6 — full-version panes + synchronized scrolling

## The last ediff item

Real ediff shows the full ours / theirs versions side by side and scrolls them
in lockstep. #68 showed region slices in the side panes with region-step nav;
this slice makes the panes show the **full versions** and scroll **together**,
keeping common lines aligned. No `dtl`: the alignment is derived from the
conflict-marker structure, not a general line-diff.

## Build (one pass, magit_cmd.c)

`magit_ediff_build_all` reads the working file once and builds all three buffers:
- **merged** = the raw file (markers).
- **ours / theirs** = the FULL versions (every region resolved to that side):
  common lines go to all three; an ours-section line goes to merged + ours; a
  theirs-section line to merged + theirs; diff3 base to merged only.
- per merged line, `aln_ours[mi]`/`aln_theirs[mi]` record the corresponding side
  line (common lines map 1:1; inside a region they clamp to the region start) --
  the alignment, free from the marker structure.
- the active region's merged lines (whole-line highlight) and side lines (for
  word-refinement, #69/#70) are recorded as before.

## Alignment + scrolling

`magit_ediff_sync_scroll`: take the merged pane's dot line index `T`, then force
every pane's top line (`w_linep`) -- merged to its dot, ours/theirs to
`aln_*[T]`. Crucially it **clears `WFFRAME`** so the redisplay reframe (which
centers the dot and clamps in a tall window) can't override the explicit
`w_linep`; that was the bug that left the tall ours pane stuck at line 0.

`magit-ediff-mode` binds `C-n`/`C-p` (line) and `C-v`/`M-v` (page) to wrappers
that run the normal motion in the merged pane, then re-sync -- so all three
scroll together with common lines aligned. `n`/`p` region-step, `a`/`b`/`RET`
resolve, `q` close are unchanged. Word-level refinement (#70) is reattached to
the active region's side lines each rebuild.

## Verify

- 178/178; cpp + c-legacy clean, 0 warnings; OFF 0 magit symbols (the slice is
  entirely in NATIVE_MAGIT-gated magit_cmd.c; no display.c change).
- tmux: panes open aligned on the region (merged `<<<`, theirs `MAIN_X`, ours
  `FEAT_X` each at window top); `C-p x2` scrolls all three to `ctx7` at top
  (aligned); `a` resolves + stages + closes.
- One stacked PR `fm-ediff-6-sync-scroll` -> `fm-ediff-5-cell-color` (#71).

## Native Magit: complete

This was the last open ediff item. The visual conflict resolver now has
full-version panes, synchronized scrolling, region-step nav, whole-line region
highlight, and true per-character word refinement. Remaining residual is
external/edge only: GPG signing (external key) and conflicting squash/fixup
folding (documented).
