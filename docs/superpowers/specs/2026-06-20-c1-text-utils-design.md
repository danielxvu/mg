# C1 — `mg.text` pure leaf utilities

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

The small core-engine warm-up: lift the genuinely **state-free** bits of the C
core into a modern, `constexpr` module. Most of `word.c`/`util.c` is welded to
`curwp`/`curbp` and stays in C; only two things are truly pure:

1. **Character classification** — `chrdef.h`'s `ISWORD`/`ISCTRL`/… macros over the
   mutable global `char cinfo[256]` table (`cinfo.c`). The DEC-multinational
   class table is fixed data; the modern form is a `constexpr` table + typed
   `bool` predicates, with no mutable global and usable in constant expressions.
2. **`ntabstop(col, tabw)`** — pure column arithmetic for the next tab stop.

## Scope (one slice)

`src/text/text.cppm` → `export module mg.text;` (library `mg_text`).

```cpp
constexpr bool is_word (int c);   // ISWORD   — word constituent
constexpr bool is_ctrl (int c);   // ISCTRL
constexpr bool is_upper(int c);   // ISUPPER
constexpr bool is_lower(int c);   // ISLOWER
constexpr bool is_eosp (int c);   // ISEOSP   — end-of-sentence punctuation
constexpr bool is_digit(int c);   // ISDIGIT
constexpr int  next_tabstop(int col, int tabw);   // ntabstop
```
- The 256-entry class table is transcribed **verbatim** from `cinfo.c` (same
  row layout for easy side-by-side diff), so it is byte-for-byte faithful —
  quirks included (`_` is NOT a word char by default; `×`/0xD7 and `÷`/0xF7 are
  marked word/letter; `Ð`/0xD0, `Þ`/0xDE are not). `int` params masked via
  `static_cast<unsigned char>` exactly like `CHARMASK`.
- Tests (`test_text`): representative + **quirk** bytes across every class
  (controls, DEL, ASCII letters/digits, `$ % ' ! . ?`, `_`, Latin-1 À/à/ß and
  the ×/÷/Ð quirks); `next_tabstop` boundaries; a `static_assert` proving
  compile-time use.

## Out of scope (kept in C, deliberately)

The ctags `_`-toggle hack on `cinfo` (runtime mutation); raw `TOUPPER`/`TOLOWER`
(unconditional ±0x20 arithmetic — unsafe for ß-type chars out of context);
anything touching `curwp`/`curbp`/line internals; trim/split helpers (no current
C-core caller — add when a consumer appears). Not yet wired into the C core.
