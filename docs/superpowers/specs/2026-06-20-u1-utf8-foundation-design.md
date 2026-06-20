# UTF-8 support — milestone overview + U1 (foundation)

**Status:** approved (user: utf8proc, ship Display+nav first, extend to Full)
2026-06-20 · **Branch:** `cpp-refactor`

mg is byte-oriented Latin-1 today. This milestone makes it edit UTF-8 files
without corruption (correct display width, char-wise cursor/delete, word ops on
non-ASCII), then leaves the door open to Full (grapheme clusters + NFC). The
engine is **utf8proc** (installed: 3.2.3) — the light, editor-grade Unicode
library (Neovim/Julia) — wrapped in C++ like `mg.git` wraps libgit2. All UTF-8
is gated by `ENABLE_CPP_UPGRADES`; the OFF build stays byte-for-byte upstream.

## Milestone decomposition (each a slice/PR, #ifdef-gated)

- **U1 — foundation (THIS spec):** `mg.utf8` C++ wrapper over utf8proc +
  `extern "C"` bridge + doctest tests. No C-core changes.
- **U2 — display width:** `display.c` column math uses codepoint width so
  multi-byte chars render at the right width (no garbling).
- **U3 — cursor/delete by character:** `forward-char`/`backward-char`/delete
  step whole codepoints (`basic.c`, line ops).
- **U4 — classification:** word motion / `ISWORD` over non-ASCII codepoints.
- **Later → Full:** grapheme-cluster cursor moves + NFC normalization (both are
  utf8proc functions; additive on this base).

## U1 — the foundation

### New library `mg_utf8` (`src/utf8/`)

A C++ wrapper over utf8proc, found via `pkg-config` (`libutf8proc`), gated by
`ENABLE_CPP_UPGRADES` at the root — same shape as `mg.git`/`mg.io`. utf8proc is
stateless, so no RAII handles; the wrapper adds type safety, a tested C++ API,
and the C bridge.

### C++ API (`mg::utf8`)

```cpp
namespace mg::utf8 {

struct decoded {
    char32_t cp;     // the codepoint (U+FFFD on an invalid sequence)
    int      bytes;  // bytes consumed (>=1; 1 for an invalid lead byte)
    int      width;  // display columns: 0 (combining), 1, or 2 (wide); >=0
};

// Decode the first codepoint of `s` (a byte span). Never reads past s.size();
// an invalid/truncated sequence yields {U+FFFD, 1, 1} so callers always advance.
decoded decode_first(std::string_view s);

// Codepoint classification (utf8proc category-backed), for word motion etc.
bool is_word(char32_t cp);    // letter | number | connector ('_') -- see note
bool is_space(char32_t cp);

} // namespace mg::utf8
```
- `decode_first` uses `utf8proc_iterate` (bounded by length) for the codepoint +
  byte count, and `utf8proc_charwidth` for width. Invalid bytes → replacement
  char, 1 byte, width 1 — so display/cursor always make forward progress and
  never desync (the faithful "show something, don't hang" behavior).
- `is_word`: utf8proc category in {Lu,Ll,Lt,Lm,Lo,Nd,Nl,No,Pc}. ⚠ This makes
  non-ASCII letters word-constituents; ASCII stays governed by the existing
  `mg.text` byte table in the C core. The two are reconciled in U4 (the C side
  picks byte-table vs codepoint based on the lead byte) — U1 only provides the
  predicate.

### `extern "C"` bridge (`src/utf8/bridge.h`)

The primitives the C core (U2/U3/U4) will call, no C++ types crossing:
```c
/* Decode one codepoint at s[0..len). Returns bytes consumed (>=1); fills
 * *cp and *width (display columns). Invalid -> 1 byte, cp=0xFFFD, width=1. */
int mg_utf8_decode(const char *s, int len, unsigned int *cp, int *width);

int mg_utf8_is_word(unsigned int cp);   /* 1/0 */
int mg_utf8_char_width(unsigned int cp); /* 0,1,2 */
```

### Tests (`test_utf8`, doctest)

- ASCII `"A"` → {0x41, 1, 1}.
- 2-byte `"é"` (C3 A9) → {0xE9, 2, 1}.
- 3-byte `"日"` (E6 97 A5) → {0x65E5, 3, **2**} (wide).
- 4-byte emoji `"😀"` (F0 9F 98 80) → {0x1F600, 4, 2}.
- Combining acute `"́"` (CC 81) → width **0**.
- Invalid lead byte `0xFF` → {0xFFFD, 1, 1} (always advances).
- Truncated 2-byte lead at end of span → {0xFFFD, 1, 1} (no over-read).
- `is_word`: 'A'/'5'/'é'/'日' true; ' '/'.'/'!' false. `is_space`: ' '/TAB true.
- Bridge `mg_utf8_decode`/`mg_utf8_is_word`/`mg_utf8_char_width` mirror the above.

### Build wiring

`src/utf8/CMakeLists.txt`: `pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET
libutf8proc)`, `add_library(mg_utf8)` (bridge.cpp + the C++ API TU),
`target_link_libraries(mg_utf8 PUBLIC PkgConfig::UTF8PROC)`. Root `CMakeLists`:
`add_subdirectory(src/utf8)` under `if(ENABLE_CPP_UPGRADES)`; later slices link
`mg→mg_utf8`. `tests/`: `test_utf8` links `mg_utf8`.

## Verification (U1)

cpp suite green incl. `test_utf8`; OFF build unchanged (no `src/utf8`, no
utf8proc link, 0 `mg_utf8` symbols in `build-c`). No behavior change yet (U1 is
not wired into the C core).

## Out of scope (U1)

Any C-core wiring (U2+); grapheme clusters / normalization (Full); input-path
byte assembly (codepoints store fine as bytes already); BOM handling; encodings
other than UTF-8.
