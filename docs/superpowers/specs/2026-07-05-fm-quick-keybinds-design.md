# FM-QUICK-KEYBINDS — four small Emacs keybinding gaps

## Why

Four everyday Emacs reflexes are missing or partial in neomg. Each is small and
independent; they ship as one themed PR. (`M-/` dabbrev, originally grouped
here, is a real completion engine and is split into its own feature spec —
`FM-DABBREV`.) Emacs 30.2 behavior verified empirically for each.

## The four

### 1. `C-l` recenter cycle (middle → top → bottom)

Emacs `C-l` is `recenter-top-bottom` with `recenter-positions = (middle top
bottom)` (verified): the first press centers, the next puts point's line at the
top, the next at the bottom, then cycles. neomg's `reposition` (`window.c:45`)
only centers (`w_frame = 0`). Add cycling:
- A new command flag `CFRECT` (next free bit, `0x0010`) marks "last command was
  a recenter". `reposition` reads it from `lastflag` to know if this is a
  consecutive press.
- A static step counter (0=middle, 1=top, 2=bottom) advances on each consecutive
  press and resets to 0 when the previous command was not a recenter.
- `reposition` sets `w_frame` to the position for the step. The exact `w_frame`
  values for middle/top/bottom are confirmed empirically during implementation
  against the existing reframe logic (top ≈ 1, bottom ≈ `w_ntrows`, middle = the
  current `0` sentinel). An explicit numeric argument keeps today's behavior
  (position at line n) and does not cycle.
- `reposition` sets `thisflag |= CFRECT`.

### 2. `M-g` → goto-line (faithful prefix map)

Emacs `M-g` is a prefix; `M-g g` and `M-g M-g` (and further repeated `M-g`s) →
`goto-line` (verified). neomg leaves `M-g` unbound (`metasqf['g'] == rescan`)
though it has `gotoline`. Implement a real prefix map (full faithfulness):

- **New `gotomap` (`KEYMAPE (2)`, default `rescan`), two sorted elements:**
  - `{ CCHR('['), CCHR('['), gotomap_esc, (KEYMAP *)&gotomap }` where
    `gotomap_esc[] = { NULL }` — a following `ESC` (the second `M-`) re-enters
    `gotomap` **itself** (a legal self-referential static-address initializer),
    so `M-g M-g`, `M-g M-g M-g`, … all resolve (Emacs's meta-repeat).
  - `{ 'g', 'g', gotomap_g, NULL }` where `gotomap_g[] = { gotoline }` — the
    plain `g` runs `goto-line`.
- **Split the metamap `'['..'h'` element.** It currently shares one
  `metasqf` array + the `M-[` prefix slot (`metasqlmap`); mg allows one prefix
  key per `map_element`, so `'g'` needs its own element. `doscan` scans elements
  by ascending `k_num` (`kbd.c:160`) and descends into `k_prefmap` when the
  `k_funcp` entry is `NULL` — so replace `{ '[', 'h', metasqf, &metasqlmap }`
  with three sorted elements:
  - `{ '[', 'f', metasqf, (KEYMAP *)&metasqlmap }` (unchanged behavior; `metasqf`
    indices 0–11 still map `'['`..`'f'`; keeps the `M-[` escape-sequence prefix),
  - `{ 'g', 'g', metag, (KEYMAP *)&gotomap }` with `metag[] = { NULL }` (the `'g'`
    prefix key → `gotomap`),
  - `{ 'h', 'h', metah, NULL }` with `metah[] = { markpara }` (`M-h` unchanged).
  `metamap` grows `KEYMAPE (8)` → `KEYMAPE (10)`; the new elements stay in
  ascending key order between the existing `'%'`/`'*'..'>'` and `'l'..'}'`
  elements.

### 3. `C-x z` → repeat

Emacs `C-x z` is `repeat` (re-run the last command; press `z` again to repeat
more; verified). neomg stores no last command. Add:
- A module global `PF last_command` in `kbd.c`, set in `mgwrap` (the single
  dispatch point, `kbd.c:501`, `return ((*funct)(f,n))`) to `funct` — **except**
  when `funct` is `repeat` itself (so repeat re-runs the command before it, not
  itself).
- A new `repeat(int f, int n)` command: if `last_command` is NULL → beep; else
  call it, then loop reading keys while the user keeps pressing `z`, re-invoking
  each time (Emacs's "keep pressing the last key" behavior). Bind `C-x z`
  (`cXmap['z']`).

### 4. `C-h k` → describe-key

Emacs `C-h k` is `describe-key` (verbose); `C-h c` is `describe-key-briefly`.
neomg has `desckey` (= describe-key-briefly) at `C-h c` but leaves `C-h k`
unbound. neomg has no doc strings, so "briefly" is as verbose as it gets — bind
`C-h k` to the same `desckey` (`helpmap['k']`) so the `C-h k` reflex works.

## Architecture / files

- `src/def.h` — `CFRECT` flag; `repeat` prototype.
- `src/window.c` — `reposition` cycling.
- `src/kbd.c` — `last_command` global + `mgwrap` hook; `repeat` command (or a
  small `extend.c`/`window.c` home — plan picks; `kbd.c` is natural since the
  dispatch lives there).
- `src/keymap.c` — new `gotomap` (+ `gotomap_esc`/`gotomap_g`/`metag`/`metah`
  arrays); split the metamap `'['..'h'` element into three (`KEYMAPE(8)`→`(10)`);
  `cXmap['z'] = repeat`; `helpmap['k'] = desckey`.
- `src/funmap.c` — `repeat` funmap entry (`{repeat, "repeat", 1, NULL}`).

All plain C, both builds (no `ENABLE_*` gating).

## Testing (pty, one case per binding)

- **C-l cycle:** on a buffer taller than the window, put point mid-buffer; press
  `C-l` three times; assert point's line lands at (approximately) center, then
  the top row, then the bottom row of the window across the three presses (assert
  via the cursor-position escape row, or the set of visible lines). If asserting
  exact rows is brittle, assert that three consecutive `C-l` produce three
  *different* framings (not all identical, which is the bug).
- **M-g:** `M-g g` then `3 RET` → point on line 3; and separately `M-g M-g`
  then `2 RET` → point on line 2 (assert both prefix forms reach `goto-line`,
  e.g. the `Goto line` prompt appears, then confirm the resulting line).
- **C-x z:** self-insert nothing; run a repeatable command (e.g. `C-n` moves
  down), then `C-x z` → the command runs again (point moved another line);
  assert. And `C-x z` with no prior command beeps (no crash).
- **C-h k:** `C-h k C-f` → assert the echo line shows `forward-char` (or the
  key-is-bound-to message), same as `C-h c`.

Verify macOS + Alpine/musl + the `c-legacy` build (all plain C, ships in both).

## Out of scope (todo.md)

- `M-/` dabbrev-expand → its own spec (`FM-DABBREV`).
- `M-g` other sub-bindings (`M-g n`/`M-g p` next/prev-error, `M-g c` goto-char) —
  only `goto-line` is wired; the rest stay unbound in `gotomap`.
- `C-u N C-x z` repeat-count; `repeat-mode`.
