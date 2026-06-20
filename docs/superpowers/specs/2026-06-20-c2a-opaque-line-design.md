# C2 phase (a) — make `struct line` opaque (accessor API)

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

The structural centerpiece of the refactor. `struct line` (`def.h`) is public
and its members are poked directly across the core; until it is hidden behind an
API there is nothing to swap a piece-table implementation *behind* (C2 phase b).
Phase (a) is a **behavior-preserving pure-C refactor** (NOT `#ifdef`-gated — it
changes the legacy core for both builds) that encapsulates `struct line`.

## Discovery (measured)

```
struct line { l_fp; l_bp; l_size; l_used; l_text; }   // 5 members, def.h
```
- **189 direct member accesses across 13 files** (grep `(->|.)l_member`).
- `line.c` owns 59 (the allocator/manipulator — keeps the full definition).
- The other **12 files / ~130 accesses** are the leakage to encapsulate:
  extend.c 24, echo.c 11, kbd.c 10, buffer.c 10, log.c 7, macro.c/file.c/
  dired.c 4, tags.c 3, re_search.c 2, word.c/util.c 1.
- **Reads** dominate and mostly already have macros (`lforw`/`lback`/`llength`/
  `ltext`/`lgetc`). The only read with no accessor is `l_size`.
- **Writes outside line.c are only `l_used`, `l_fp`, `l_bp`** — overwhelmingly
  line *relinking* (keyboard-macro `maclcur` splices, buffer line insertion).
  No external code writes `l_text` or `l_size`.

## Feasibility — opacity IS achievable

- **No by-value uses**: zero `.l_member` accesses; every line is reached by
  pointer. `lalloc` `malloc(sizeof(*lp))`; buffers hold `b_headp` as a pointer.
- So `struct line`'s full definition can live in `line.c` alone, with `def.h`
  carrying only an incomplete `struct line;` + accessor declarations.

## Accessor API

Existing (keep): `lforw lback lgetc lputc llength ltext`. **Add:**
```c
int          lsize   (struct line *lp);            /* read l_size            */
void         lsetlen (struct line *lp, int n);     /* write l_used           */
void         lsetforw(struct line *lp, struct line *lq);  /* write l_fp       */
void         lsetback(struct line *lp, struct line *lq);  /* write l_bp       */
```
That is the *entire* external mutation surface (l_used + the two links).

## The macro→function pivot

The existing accessors are function-call-syntax macros, so converting them to
real functions changes **no call sites** — only the definition. True opacity
*requires* this (a member-dereferencing macro can't compile against an
incomplete type). Plan: do the bulk access→accessor conversion while `struct
line` is still public (accessors as macros), then flip macros→functions and move
the struct into line.c in one final atomic step.

## Decomposition (sub-slices, each build-verified, both presets, 0 warnings)

- **C2a-1** (this slice): add the 4 new accessors (macros for now); convert the
  low-count files (word.c, util.c, re_search.c, tags.c) as proof of pattern.
- **C2a-2**: convert the macro-splice + buffer files (extend.c, kbd.c, echo.c,
  macro.c, buffer.c, file.c, log.c, dired.c) to accessors.
- **C2a-3**: convert line.c's internal uses to accessors where natural; then
  **flip**: accessor macros → functions (decls in def.h, defs in line.c), move
  `struct line` definition into line.c, leave `struct line;` opaque in def.h.
  Compile-time proof that no leak remains (incomplete type rejects stray access).

## Verification each slice

`cmake --build --preset cpp && ctest --preset cpp` (both build + tests),
`cmake --build --preset c-legacy` (OFF, 0 warnings under -W -Wall -Wextra),
`mg -h` smoke on both; behavior unchanged (it is a pure encapsulation refactor).

## Out of scope (phase a)

The piece-table storage itself (phase b); UTF-8 column/width work (design axis
to weigh when phase b lands — see the UTF-8 goal); any user-visible change.
