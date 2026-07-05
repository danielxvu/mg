# FM-REGION-HIGHLIGHT — visible active region (transient-mark)

## Why

neomg's mark is a persistent per-window position with no active/inactive concept,
and `display.c` never references it — so setting the mark (`C-SPC`) and moving
point shows **no** selection. GNU Emacs highlights the active region by default
(transient-mark-mode, verified on 30.2: setting the mark and moving emits a
region-background SGR). This is the single highest-daily-impact Emacs-fidelity
gap. Add a visible, auto-deactivating region highlight.

## Decided model (from brainstorming)

An **active-region** flag approximating transient-mark. The mark itself is
unchanged; a per-window "active" bit gates the highlight so a stale mark doesn't
stay lit.

- **Per-window flag:** a free bit in `struct mgwin.w_flag`, e.g. `WMARKED`
  (the plan verifies a bit is free; `w_flag` is distinct from the redisplay
  `w_rflag`). Per-window (not global) so a second window never highlights
  spuriously when you switch to it.
- **Activated** by the mark-setting commands (`src/basic.c`): `setmark`
  (`C-SPC`), `swapmark` (`C-x C-x`), and `isetmark` (used by mark-paragraph
  `M-h` and mark-whole-buffer `C-x h`) → `curwp->w_flag |= WMARKED`.
- **Deactivated** (clear `curwp->w_flag & WMARKED`) by:
  - `ctrlg` (`C-g`, `src/main.c`);
  - `lchange` (`src/line.c`) — called on *every* buffer modification, so
    self-insert / kill / yank / indent drop the region, exactly like Emacs;
  - `copyregion` (`M-w`, `src/region.c`) — explicit, since copy doesn't edit
    (Emacs's `kill-ring-save` also deactivates);
  - clearing the mark (`setmark` when it toggles the mark off) leaves it
    inactive.

This yields the real flow: `C-SPC` → move to watch the selection grow →
kill/copy/edit clears it. It is an approximation (Emacs deactivates after *most*
commands; this deactivates on edit/copy/`C-g`); finer deactivation is a later
note. `killregion` (`C-w`) edits, so `lchange` deactivates it for free.

## Architecture

All highlight rendering reuses the existing `MG_HL_BIT` standout machinery (the
ediff refine-highlight): a cell with `MG_HL_BIT` set renders via
`ttcolor(CMODE)` → `enter_standout_mode` (reverse video). That machinery is
`#ifdef ENABLE_NATIVE_MAGIT`, so the region highlight is **neomg-build only**;
the plain-C `c-legacy` build stays faithful to upstream mg (which has no region
highlight). The flag/activation/deactivation (basic.c/main.c/line.c/region.c)
is plain C and compiles in both builds; only the display application is gated.

### Redisplay (`src/display.c`, `update` + the full-render loop)

1. **Force a full repaint while active.** In `update()`, before rendering a
   window, `if (wp->w_flag & WMARKED) wp->w_rflag |= WFFULL;` — so the region
   repaints as point moves (a `WFEDIT` single-line update can't move the
   highlight). Transient state, so the per-keystroke full repaint is acceptable.

2. **Precompute the ordered span** (once, when about to render a WMARKED
   `wp == curwp` with a mark set): order the two endpoints by buffer position
   using `w_dotline`/`w_markline` (then `w_doto`/`w_marko` on a tie) into
   `rs_lp,rs_off` (start) → `re_lp,re_off` (end).

3. **Per-line highlight range.** The full-render loop already walks `lp`
   top-to-bottom (buffer order), so `rs_lp` is reached before `re_lp`. Maintain
   an `inside` state and derive each line's `[from,to)` column range:
   - single-line (`rs_lp == re_lp`): on that line, `[rs_off, re_off)`;
   - start line: `[rs_off, EOL]`, set `inside`;
   - end line: `[0, re_off)`, clear `inside`;
   - a line while `inside`: `[0, EOL]` (whole line);
   - otherwise: no highlight.
   Pass the range to `vt_render_line` (via a small pair of globals it reads, or
   an added parameter — the plan picks the cleaner one), which sets `MG_HL_BIT`
   on cells whose `ci` is in `[from,to)` — beside the existing ediff/color
   application, so it composes.

The reduced `WFEDIT` path is bypassed while active (step 1 forces `WFFULL`), so
only the full-render site needs the region logic.

## Error handling / edges

- No mark in `curwp` → nothing highlighted (guard on `w_markp != NULL`).
- Mark == point (empty region) → `[off, off)` is empty → nothing highlighted.
- Region spanning off-screen lines → only visible lines render; the span logic
  is per-visible-line, so off-screen parts are simply not drawn.
- `MG_HL_BIT` is stripped before the cell is encoded (`display.c:950`), so it
  never leaks into the buffer content; it is display-only.
- The past-EOL blank is not highlighted in v1 (only actual characters, cols
  `0..llength`); the region text is clearly marked, which is the point.
- Multiple windows: each has its own `WMARKED`; only `curwp`'s active region
  draws. Simultaneous highlighted regions in split windows are out of scope.

## Testing

- **Editor** (`test_editor`, pty; neomg build): open a file with a known line
  (e.g. `hello world`), `C-a` to BOL, `C-SPC` (set mark), `C-f`×5 (extend the
  region over `hello`). Assert the reverse-video standout escape (the
  `enter_standout_mode` sequence — under `TERM=xterm`, `\033[7m`; verify the
  exact bytes empirically during implementation, as FM-LOG-RICH did for its
  color SGR) appears in the render. Then self-insert a char (or `C-g`) and
  assert a subsequent repaint no longer contains the standout run around that
  text — the region deactivated. Single session, drained exit via `quit_neomg`.
  (If asserting standout *absence* proves timing-fragile, assert the positive
  case on activation and that `C-g` returns the buffer to a plain render via a
  distinguishing marker.)

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`) + that the plain-C `c-legacy`
build still compiles (the flag code is unguarded; the display application is
`ENABLE_NATIVE_MAGIT`-gated, so c-legacy is unchanged).

## Out of scope (recorded in todo.md)

- Simultaneous highlighted regions across split windows.
- Rectangle-region highlight; shift-selection.
- Highlighting the past-EOL newline for full-width region rows.
- Finer Emacs deactivation (deactivate after most non-movement commands).
