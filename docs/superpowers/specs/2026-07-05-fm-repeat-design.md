# FM-REPEAT — `C-x z` repeat-last-command (done right)

## Why

Emacs `C-x z` is `repeat`: re-run the last command, and keep pressing `z` to
repeat more (verified on 30.2). neomg has no such command. A first attempt (in
the FM-QUICK-KEYBINDS branch) re-invoked the last function pointer **directly**,
bypassing the `mgwrap`/`doin` dispatch — which silently corrupted data:
`selfinsert` reads its char from the global `key.k_chars` (still holding the
`C-x z` keystroke → typing `x` then `C-x z` inserted `xz`), and `undo` keys off
`rptcount` (only `mgwrap` maintains it) so `C-x z` after `C-x u` reversed the
undo. That attempt was dropped. This spec does it correctly: capture the
dispatch context and re-run *through* `mgwrap`.

## Design

### Context capture in `mgwrap` (`src/kbd.c`)

`mgwrap(PF funct, int f, int n)` is the single command-dispatch point. It
already tracks the last function (a `static PF ofp`) for `rptcount`. Widen that
into module globals and capture the full context — for every real command
(keeping the existing exclusions of `rescan`/`negative_argument`/
`digit_argument`/`universal_argument`, and adding `repeat` so it never records
itself):
- `PF last_command` (replaces the `static ofp`),
- `int last_f`, `int last_n` — the numeric-prefix args,
- `struct key last_key` — a snapshot of the global `key` (`{k_count,
  k_chars[MAXKEY]}`, a fixed struct → plain assignment), i.e. the key sequence
  that invoked the command.

The existing `rptcount` bookkeeping (`funct == last_command ? rptcount++ :
rptcount = 0`) is preserved with `last_command` in place of `ofp`.

### `repeat(int f, int n)` (`src/kbd.c`)

```
if (last_command == NULL) -> dobeep + "No last command to repeat" + FALSE
loop:
    key = last_key;                          // restore invoking key sequence
    s = mgwrap(last_command, last_f, last_n);// re-run THROUGH mgwrap
    if (s != TRUE) return (s);               // ABORT or FALSE -> stop
    update(CMODE);
    c = getkey(FALSE);
    if (c != 'z') break;                     // Emacs: press the repeat char to repeat
ungetkey(c);                                 // non-z key resumes normal dispatch
return (TRUE);
```

Why this is correct:
- **selfinsert**: reads `key.k_chars[k_count-1]`; restoring `key` yields the
  original char, not the `z` (`x` → `xx`).
- **undo**: routing through `mgwrap` runs the `rptcount` bookkeeping, so a
  repeated `undo` sees `rptcount > 0` and *continues* the chain (Emacs behavior)
  instead of resetting to the head (which reversed it).
- **ABORT**: `gotoline` and other commands return `ABORT` (2) on a cancelled
  prompt; the `!= TRUE` check stops the loop rather than treating it as success.
- **numeric prefix**: `last_f`/`last_n` are reused, so `C-u 5 C-n` then `C-x z`
  moves 5 lines (the repeat's own `f`/`n` are intentionally ignored).
- **no recursion**: `repeat` is excluded from being stored as `last_command`, so
  `mgwrap(last_command, …)` can never dispatch `repeat`.

### Binding

- `src/keymap.c`: `cXmap` grows `KEYMAPE (6)` → `(7)` with a sorted `'z'`
  element (`cXz[] = { repeat }`) after the `'^'..'u'` element.
- `src/funmap.c`: `{repeat, "repeat", 1, NULL}`.
- `src/def.h`: `int repeat(int, int);` prototype.

All plain C, both builds.

## Edge cases

- `last_command` is zero-initialized (BSS) → NULL until the first real command;
  `repeat` guards it.
- A repeated command that reads its own input from the minibuffer (e.g. a search)
  prompts each iteration — acceptable, matches re-running it.
- If `getkey` returns a non-`z` key, it is `ungetkey`'d so normal dispatch runs
  it (no key lost or duplicated).

## Testing (pty)

- **selfinsert**: type `x`, `C-x z` → buffer has `xx`; another `z` → `xxx`
  (the original bug produced `xz`).
- **undo continuation**: make several separate edits, `C-x u` (undo one), then
  `C-x z` → undoes the *next older* edit (not a redo of the undo). Assert the
  buffer keeps shrinking toward the original, and does not grow back.
- **movement + prefix**: `C-n` then `C-x z` → point moved two lines total (kill
  the landing line and assert which line emptied); and `C-u 3 C-n` then `C-x z`
  → moved 3 + 3.
- **no prior command**: fresh session, `C-x z` → "No last command to repeat"
  (beep), buffer unchanged.

Verify macOS + Alpine/musl + `c-legacy` (all plain C, ships in both).

## Out of scope (todo.md)

- `C-u N C-x z` (repeat N times) and `repeat-mode`.
- `repeat` inside keyboard-macro replay (`executemacro` bypasses `mgwrap`, so
  `last_command` reflects pre-replay state — pre-existing architectural trait).
- Repeating by pressing the *last key of the repeated command* (Emacs also
  allows this); only the `z` repeat char is supported.
