# FM-EDIFF-5 — per-cell color: true character-level refinement

## Going all the way

FM-EDIFF-4 (#69) rendered word-level refinement as text markers (`[-..-]`/
`{+..+}`) because mg's video model is whole-line color (`struct video.v_color`,
`uline` emits a line in one color; `vtcell` had no per-cell attribute). This
slice adds **per-cell color** so refined words render in real reverse video,
not bracket text -- contained, and OFF-byte-clean by `#ifdef`.

## The mechanism (display.c, all under ENABLE_NATIVE_MAGIT)

`vtcell` is an `int` codepoint (CPP_UPGRADES, required by NATIVE_MAGIT) with
spare high bits (codepoints are 21-bit). Bit 30 = `MG_HL_BIT`, a per-cell
standout flag:

- `vt_render_line`: after emitting a char's cell(s), OR `MG_HL_BIT` in when
  `magit_cell_highlighted(buf, line, charcol)` is true. `charcol` is a codepoint
  index; the cell loop ORs the bit into the 1-or-2 cells the char occupies
  (skipping VT_CONT).
- `uline`: in both emit loops, `cellcolor(cell, base)` returns CMODE when the
  bit is set else the line's base color; the loop emits a `ttcolor` transition
  only when it changes. **No-op when no cell carries the bit -> every normal
  line is byte-identical**; the optimized path restores the base color before
  its erase.
- `ttputcell`: strips `MG_HL_BIT` before encoding the codepoint.
- The bit lives in the cell value, so the existing diff/hash/erase logic detects
  highlight changes for free (a re-highlight changes the cell -> redraw).

The merged pane keeps its whole-line standout (#68, the update() v_color hook);
per-cell is purely additive for the side panes. OFF build: every addition is
`#ifdef`'d out -> 0 magit symbols, byte-clean.

## The UI side (magit_cmd.c)

The bridge still emits refined lines with wdiff markers (`refine_words`, #69).
The side-pane emit callback now **strips the markers, inserts plain text, and
records the marked char ranges** (codepoint-indexed, so UTF-8 is correct) per
line into `magit_ediff_ref[]`. `magit_cell_highlighted` answers the renderer
from that table. Cleared on rebuild + on quit.

## Verify

- 178/178 (engine/bridge unchanged from #69); cpp + c-legacy clean, 0 warnings;
  OFF 0 magit symbols.
- tmux (capture -e): the ediff ours pane shows plain `the quick brown fox` with
  only `quick` in `^[[7m` standout; theirs `slow`; the merged region keeps its
  whole-line standout. No `[-..-]` text in the panes anymore.
- One stacked PR `fm-ediff-5-cell-color` -> `fm-ediff-4-refine` (#70).

## Now genuinely complete

The earlier "needs a display rewrite" for per-character color is done -- via a
spare cell bit + a no-op-when-absent `uline` hook, not a full rewrite. The only
remaining ediff item is synchronized free (line) scrolling across full-file
panes (needs cross-file line alignment), which region-step nav already covers
functionally.
