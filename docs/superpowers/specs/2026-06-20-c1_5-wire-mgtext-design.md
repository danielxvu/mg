# C1.5 — wire `mg.text` into the C core (retire `cinfo[]`)

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

Symmetric with C3.5: route the C core's character classification through the
modern `mg.text` module under `#ifdef ENABLE_CPP_UPGRADES`, retiring the
runtime-mutable global `char cinfo[256]` (`cinfo.c`). OFF build byte-for-byte
unchanged.

## The one wrinkle: ctags `_` toggle

`tags.c` temporarily sets `cinfo['_'] = _MG_W` so identifiers-with-underscore
match as one token, then restores it. `mg.text`'s table is immutable (`_` is not
a word char), so this runtime mutation must be preserved another way. The
modern form: the *only* mutable bit becomes a single typed toggle in the bridge,
not a 256-byte mutable array.

## Plan

1. **Bridge** — `src/text/bridge.{h,cpp}` (added to `mg_text`):
   ```c
   int  mg_text_is_word(int c);   /* + is_ctrl/is_upper/is_lower/is_eosp/is_digit */
   void mg_text_set_underscore_word(int on);  /* ctags '_' toggle */
   ```
   `mg_text_is_word` returns the toggle for `'_'`, else `mg::text::is_word(c)`.
   The toggle is the bridge's lone piece of state (a `bool`).

2. **`chrdef.h`** — under `#ifdef ENABLE_CPP_UPGRADES`, redefine `ISWORD`/
   `ISCTRL`/`ISUPPER`/`ISLOWER`/`ISEOSP`/`ISDIGIT` to call the bridge predicates
   (`#include "text/bridge.h"`); the `#else` keeps the exact `cinfo[]` macros.
   `TOUPPER`/`TOLOWER`/`CHARMASK`/`CCHR` unchanged (not classification).

3. **`cinfo.c`** — wrap the `char cinfo[256] = {…}` definition in
   `#ifndef ENABLE_CPP_UPGRADES`, genuinely retiring the global for the upgraded
   build. (`getkeyname` stays; it only uses the macros.)

4. **`tags.c`** — under `#ifdef ENABLE_CPP_UPGRADES`, replace the
   `cinfo['_']` save/set/restore with `mg_text_set_underscore_word(1)` …
   `mg_text_set_underscore_word(0)`; `#else` keeps the original.

5. **Build** — root `CMakeLists.txt`: `target_link_libraries(mg PRIVATE mg_text)`
   in the existing `if(ENABLE_CPP_UPGRADES)` block (mg already links mg_io + is
   CXX-linked from C3.5).

## Tests

- `test_text_bridge` (TDD): each predicate matches the `mg::text` table; the
  underscore toggle flips `is_word('_')` false→true→false.
- ON build compiles (all 44 IS* call sites) and runs (`mg -h`); OFF build:
  `cinfo[]` present, macros use it, `tags.c` uses `cinfo['_']`, 0 `mg_text`
  symbols.

## Out of scope

`next_tabstop` wire-in (util.c `ntabstop`, a function not a macro — trivial,
later); UTF-8 / multi-byte classification (a separate future API beside the byte
table — see the UTF-8 goal); `TOUPPER`/`TOLOWER` modernization.
