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

### 2. `M-g` → goto-line

Emacs `M-g` is a prefix map (`M-g g` / `M-g M-g` → `goto-line`, verified).
neomg leaves `M-g` unbound (`metasqf['g'] == rescan`) though it has `gotoline`.
mg's keymap model allows only one prefix key per `map_element`, and `'g'` sits
in the shared `'['..'h'`/`metasqf` element whose prefix slot is already used by
`M-[` — so a faithful `M-g` *prefix* would require splitting that element,
disproportionate for one binding. **Bind `M-g` directly to `gotoline`**
(`metasqf['g'] = gotoline`): `M-g` prompts for a line immediately, which covers
the reflex. The `M-g g` prefix form is deferred (`FM-MG-PREFIX`, todo.md).

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
- `src/keymap.c` — `metasqf['g'] = gotoline`; `cXmap['z'] = repeat`;
  `helpmap['k'] = desckey`.
- `src/funmap.c` — `repeat` funmap entry (`{repeat, "repeat", 1, NULL}`).

All plain C, both builds (no `ENABLE_*` gating).

## Testing (pty, one case per binding)

- **C-l cycle:** on a buffer taller than the window, put point mid-buffer; press
  `C-l` three times; assert point's line lands at (approximately) center, then
  the top row, then the bottom row of the window across the three presses (assert
  via the cursor-position escape row, or the set of visible lines). If asserting
  exact rows is brittle, assert that three consecutive `C-l` produce three
  *different* framings (not all identical, which is the bug).
- **M-g:** `M-g` then `5 RET` → point on line 5 (assert the line's content is
  current, e.g. via `C-a C-k` then check, or the `Goto line` prompt appears).
- **C-x z:** self-insert nothing; run a repeatable command (e.g. `C-n` moves
  down), then `C-x z` → the command runs again (point moved another line);
  assert. And `C-x z` with no prior command beeps (no crash).
- **C-h k:** `C-h k C-f` → assert the echo line shows `forward-char` (or the
  key-is-bound-to message), same as `C-h c`.

Verify macOS + Alpine/musl + the `c-legacy` build (all plain C, ships in both).

## Out of scope (todo.md)

- `M-/` dabbrev-expand → its own spec (`FM-DABBREV`).
- Faithful `M-g` prefix map (`M-g g`) → `FM-MG-PREFIX`.
- `C-u N C-x z` repeat-count; `repeat-mode`.
