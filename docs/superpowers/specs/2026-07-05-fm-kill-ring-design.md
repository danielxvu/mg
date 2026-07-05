# FM-KILL-RING — a real kill ring with M-y (yank-pop)

## Why

neomg's kill "ring" is a single growable byte buffer (`kbufp` in `src/yank.c`).
`kdelete()` — called at the start of every *new* (non-consecutive) kill
sequence — **frees** it. So killing text, moving, then killing again silently
destroys the first kill: it can never be recovered. Emacs keeps a *ring* of
kills and `M-y` (yank-pop) cycles through them. This is real data loss, and
`M-y` is muscle memory for Emacs users. Add a bounded kill ring + `M-y`.

## Decided model (from brainstorming)

Turn the single kill buffer into a fixed-size ring; the one behavioral change
is that `kdelete()` **rotates to the next slot instead of freeing**.

### Ring structure (`src/yank.c`)

Replace the module globals `kbufp`/`kused`/`ksize`/`kstart` with:
```c
#define NKILL 60		/* kill-ring size (Emacs kill-ring-max is 120) */
struct kill { char *buf; RSIZE used, size, start; };
static struct kill kr[NKILL];
static int kr_head;		/* newest entry (kills append/rotate here)   */
static int kr_n;		/* number of entries in use (<= NKILL)       */
static int kr_yptr;		/* yank pointer: entry a yank/yank-pop reads */
```
- **`kdelete()`** (new kill context) no longer frees. It rotates to a fresh
  slot: if `kr_n > 0`, `kr_head = (kr_head + 1) % NKILL`; free+zero that slot
  (reclaiming the oldest entry when the ring is full); `if (kr_n < NKILL)
  kr_n++`; `kr_yptr = kr_head`. (First kill ever: `kr_n == 0` → use slot 0
  without advancing.)
- **`kinsert`/`kgrow`** operate on the current entry `kr[kr_head]` (was
  `kbufp`/`kused`/`ksize`/`kstart`). Consecutive kills skip `kdelete()` (via the
  existing `CFKILL` check in the kill commands), so they append to the same
  entry — unchanged behavior.
- **`kremove(n)`** reads from the *yank* entry `kr[kr_yptr]`:
  `if (n < 0 || n + kr[kr_yptr].start >= kr[kr_yptr].used) return -1;`
  `return CHARMASK(kr[kr_yptr].buf[n + kr[kr_yptr].start]);`

### Yank and yank-pop

- **`yank` (C-y)** — before its insert loop, set `kr_yptr = kr_head` (yank the
  newest). The existing body (which calls `isetmark()` so the mark ends at the
  yank start and point at the yank end) is otherwise unchanged. At the end, set
  `thisflag |= CFYANK`.
- **`yank_pop` (M-y)** — new command, bound to `ESC y` (currently `rescan`
  in the metamap; `funmap`: `{yank_pop, "yank-pop", 1, NULL}`):
  1. If `(lastflag & CFYANK) == 0` → `dobeep`; `ewprintf("Previous command was
     not a yank")`; `return (FALSE)`.
  2. Delete the just-yanked region. After a yank, mark…point spans it:
     `getregion(&reg)`; move point to the region start
     (`reg.r_linep`/`reg.r_offset`/`reg.r_lineno`); `ldelete(reg.r_size, KNONE)`
     (removes text *without* pushing to the ring).
  3. Cycle one entry older within the used range: the oldest is
     `(kr_head - (kr_n - 1) + NKILL) % NKILL`; if `kr_yptr` is the oldest, wrap
     to `kr_head`, else `kr_yptr = (kr_yptr - 1 + NKILL) % NKILL`.
  4. Insert `kr[kr_yptr]` exactly as `yank` inserts (share a static helper
     `do_yank(n)` that both call: `isetmark()` + the `kremove` loop + framing),
     leaving mark…point around the new text.
  5. `thisflag |= CFYANK`; `return (TRUE)`.

### Command flag

Add `#define CFYANK 0x0008` (`src/def.h`, beside `CFCPCN`/`CFKILL`/`CFINS`).
`yank` and `yank_pop` set it in `thisflag`; `yank_pop` reads it from `lastflag`.
Any other command clears it (the main loop resets `thisflag` each iteration), so
`M-y` only chains immediately after `C-y`/`M-y` — matching Emacs.

## Error handling / edges

- `M-y` with no preceding yank → beep + message, no buffer change.
- Empty ring / empty entry → `kremove` returns -1 immediately → yank inserts
  nothing (same as today's empty kill buffer).
- Ring not yet full: `kr_n` bounds the `M-y` cycle so it never reads an unused
  (NULL `buf`) slot.
- Ring full (NKILL entries): rotating reuses the oldest slot; its old `buf` is
  freed then reallocated. No leak, no unbounded growth.
- A yanked region the user edited before `M-y`: `getregion` uses the current
  mark…point, so `M-y` deletes whatever that now spans. This matches Emacs's
  "only right after a yank" contract (guarded by `CFYANK`); we accept it.
- `kremove`/`kinsert`/`kgrow` must consistently use `kr[...]` — no stale
  references to the removed `kbufp` globals (a missed one would not compile,
  which is the safety net).

## Testing

- **Editor** (`test_editor`, pty): seed a buffer with three distinct lines.
  Kill them *non-consecutively* (kill line, move to break the `CFKILL` chain,
  kill next …) so each becomes its own ring entry. `C-y` yanks the newest;
  assert it. `M-y` → assert the buffer now shows the previous kill in its place;
  `M-y` again → the first kill — proving the earlier kills survived (the
  data-loss fix). A second case: `M-y` with no preceding yank → assert the
  "Previous command was not a yank" message and an unchanged buffer.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`) + the plain-C `c-legacy`
  build (`yank.c` is core, unguarded C — unlike the region highlight, the kill
  ring ships in the legacy build too, so it must compile and pass there).

## Out of scope (recorded in todo.md)

- `C-u N M-y` numeric-argument cycling (`M-y` cycles one entry per press in v1).
- `browse-kill-ring` / a kill-ring listing buffer.
- Cross-session persistence.
- Interaction with the system clipboard.
