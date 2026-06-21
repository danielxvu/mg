# FM-EDIFF-4 — word-level refinement in the ediff panes

## The verified boundary

The remaining ediff "polish" was intra-line refinement. Investigation found a
**real** architectural limit (this time, not an overclaim): `struct video` holds
**one `v_color` per line**, and `uline` emits the whole line in that single
color (`vtcell` has no per-cell attribute). So per-*character* color highlight
needs a per-cell attribute added to the video cell + a rewritten `uline`/output
path — a large, invasive change to shared core display used by the OFF build.
Not worth it for polish.

## What is buildable (and shipped)

The *value* of refinement — seeing **which words differ** between ours/theirs —
is achievable **textually**, with no core change: a small word-level diff wraps
the words unique to each side in wdiff-style markers, rendered in the existing
ediff side panes.

- Engine `refine_words(text, other, open, close)`: tokenize into words +
  whitespace runs, word-level LCS, wrap each word of `text` not shared with
  `other` in `open`..`close`; whitespace/newlines preserved; bails (returns
  `text`) if either side is huge (n*m > 4M). Self-contained — no `dtl`/3rd-party
  dependency.
- Bridge `mg_magit_conflict_hunk_side` now refines: the ours pane wraps its
  unique words in `[-...-]`, the theirs pane in `{+...+}`.
- UI: unchanged — the side panes already call that bridge, so they render
  refined automatically.

## Verify

- Engine doctest: `refine_words("the quick brown fox", "the slow brown fox",
  "[-","-]") == "the [-quick-] brown fox"` + whitespace/insertion/identical
  cases. Bridge test: the two panes emit `[-quick-]` / `{+slow+}`.
- tmux: a merge conflict on a line that shares words shows the ediff ours pane
  `the [-quick-] brown fox`, theirs `the {+slow+} brown fox`.
- 178/178; cpp + c-legacy clean, 0 warnings; OFF 0 magit symbols.
- One stacked PR `fm-ediff-4-refine` → `fm-ediff-3-panes` (#69).

## Genuinely out (architectural)

- Per-*character* color refinement — needs per-cell video attributes (core
  rewrite). The textual markers are the faithful substitute given whole-line
  color.
- Synchronized free (line) scrolling across full-file panes — needs cross-file
  line alignment; region-step nav (#68) covers the conflict workflow.
