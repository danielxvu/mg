# FM-EDIFF-3 — live 3-pane visual conflict ediff

## Corrected feasibility

Earlier slices deferred this as "needs a display-subsystem rewrite". On
investigation that was an overclaim: `ttcolor` already emits standout
(`enter_standout_mode`, termcap `so`) for `CMODE` — the modeline uses it. So
per-line reverse-video highlighting is achievable; it just needs the shared
`display.c` render loop to set a body line's `v_color = CMODE`, which is done
**OFF-safely** behind `#ifdef ENABLE_NATIVE_MAGIT` (already the pattern there
for the modeline). `struct line` is opaque, so the highlight is a pointer-set
membership test, not a per-line flag.

## What it is

`E` on a Conflicts line opens three stacked panes:
- **merged** (top, interactive, `magit-ediff-mode`): the working file with
  markers; the active conflict region rendered in standout.
- **theirs** (middle) / **ours** (bottom): the active region's two sides.

Keys (on the merged pane): `n`/`p` step regions (all panes follow), `a` keep
ours, `b` keep theirs, `RET` keep both, `q` close. Resolving the last region
stages the file and tears the panes down to the status buffer. Reuses the
`conflict_hunks` / `resolve_conflict_hunk` engine (#65) unchanged.

## The display.c hook (the only core change)

```c
#ifdef ENABLE_NATIVE_MAGIT
int magit_line_highlighted(struct buffer *, struct line *);  /* in magit_cmd.c */
#endif
...
vscreen[i]->v_color = CTEXT;
#ifdef ENABLE_NATIVE_MAGIT
if (lp != headp && magit_line_highlighted(wp->w_bufp, lp))
    vscreen[i]->v_color = CMODE;     /* standout */
#endif
```

Added at both body-render spots (WFEDIT single-line, WFFULL full-window).
`magit_line_highlighted` returns true for the merged pane's active-region line
pointers. Compiles out in the OFF build (verified: 0 magit symbols, byte-clean).

## Window choreography (the bug that bit)

`splitwind` walks `w_linep -> w_dotp`; `showbuffer` sets `w_dotp` from the new
buffer but leaves `w_linep` pointing into the OLD buffer. Splitting *after*
showing the special buffers made that walk run across buffers and crash. Fix:
**onlywind, then splitwind twice while the window still shows the status buffer
(consistent w_linep/w_dotp), then `showbuffer` the three pane buffers** (with
`WFFRAME` so redisplay reframes). Only the merged pane carries the keymap;
side panes are passive.

## Build / verify

- New bridge `mg_magit_conflict_hunk_side(repo, path, index, side, emit)` — one
  region's one side, for the side panes.
- Engine: unchanged (reuses #65).
- tmux-verified end-to-end: 3 panes render; the active region shows `^[[7m`
  standout (capture -e); `n` steps all panes to region 2; `b` then `a` resolve
  per-region (mixed result FEAT_TOP…MAIN_BOT); staged; torn down to status.
- 176/176; cpp + c-legacy clean, 0 warnings; OFF 0 magit symbols.
- One stacked PR `fm-ediff-3-panes` → `fm-rb-replay-conflict` (#68).

## Honestly still out

Synchronized *free scrolling* (line-by-line, not region-stepping) and
intra-line refinement — would need a real line-diff (e.g. header-only `dtl`) and
finer color than whole-line standout. Region-stepping covers the conflict
workflow; the rest is polish.
