# U2 — UTF-8 display (codepoint video cells)

**Status:** proposed (awaiting approval of approach) 2026-06-20 ·
**Branch:** `cpp-refactor`

Make `display.c` render UTF-8 correctly: `café` / `日本語` show as real glyphs
(not `caf\303\251`), aligned, with the cursor on the right column. This is the
**hardest slice** of the refactor — an architectural change to the render
pipeline — so it is designed carefully and gated by `ENABLE_CPP_UPGRADES` (OFF
stays byte-for-byte upstream).

## The core problem

mg's virtual screen is **byte-per-cell**: `struct video { char *v_text; }`, one
byte per *column*. `vtputc` octal-escapes bytes ≥ 0x80 (the garbling), and the
whole pipeline (`uline` emit, `hash`, `ucopy`, diffing) assumes byte == column.
UTF-8 breaks this: `é` is 2 bytes / 1 column, `日` is 3 bytes / 2 columns. There
is no way to keep byte-cells and get correct columns — the cell must become a
**display column** holding a codepoint.

## Design: codepoint video cells (a `vtcell` typedef)

Concentrate the divergence in a type alias so most of the 9 functions are
untouched:
```c
#ifdef ENABLE_CPP_UPGRADES
typedef int  vtcell;          /* a codepoint; VT_CONT marks the 2nd column of a
                                 wide char; space (0x20) is the blank cell */
#else
typedef char vtcell;
#endif
```
`v_text` becomes `vtcell *`. Cells are still assigned, compared (`==`), copied
(`memcpy`), and cleared to `' '` — all of which work for either `char` or `int`.
Only **3 functions interpret cell content** and need real `#ifdef` logic:

- **`vtputc` / `vtpute`** — ON: store a codepoint and advance `vtcol` by its
  **width** (1, or 2 with a trailing `VT_CONT` cell; width-0 combining marks are
  dropped for now). OFF: today's byte path.
- **`uline`** (terminal emit) — ON: encode each cell's codepoint to UTF-8 and
  `ttputc` the bytes; skip `VT_CONT` cells. OFF: `ttputc(byte)`.

`hash`/`ucopy`/`vteeol`/`vtinit`/`vtresize` change only by using `vtcell` +
`sizeof(vtcell)` (the typedef absorbs them). `cost`/`magic_cookie` math is
column-based already and is unaffected.

## Feeding codepoints in: the render loops

The four line-scan loops (`display.c:498/508/566` in `update`, `698` in
`updext`) change from
`for (j=0; j<llength; j++) vtputc(lgetc(lp,j))` to a UTF-8 walk:
```c
for (j = 0; j < llength(lp); ) {
    unsigned int cp; int w;
    int n = mg_utf8_decode(&ltext(lp)[j], llength(lp) - j, &cp, &w);
    vtputuc(cp, w, wp);   /* new: store codepoint+width */
    j += n;
}
```
ASCII stays single-byte (`mg_utf8_decode` returns 1) so its path is unchanged.
A shared helper avoids duplicating the walk.

## Cursor column correctness

The dot→column computations must count **display columns**, not bytes:
- `display.c:~529` (the in-`update` dot-column loop) and `util.c` `getcolpos`
  walk the line to dot; under `#ifdef` they decode codepoints and advance by
  `width` (and tabs/ctrl as today). This puts the hardware cursor on the right
  column.

## Build / wiring

Link `mg → mg_utf8` (root CMake, under `ENABLE_CPP_UPGRADES`). `display.c` /
`util.c` include `utf8/bridge.h` under `#ifdef`. No new test binary (display is
integration-tested); correctness is verified by **pty screen capture**.

## Verification

- **pty**: open a file containing `café`, `naïve`, `日本語`, a combining
  sequence, and an emoji; capture the rendered screen and assert the glyphs
  appear (not `\ooo`), CJK occupies 2 columns, and the modeline column count for
  dot at end-of-line matches display width (e.g. `café` → column 4, not 5).
- Cursor lands correctly: put dot after `é`, the reported column is right.
- cpp suite still green; OFF build byte-for-byte upstream (display.c `#else`
  path unchanged, 0 warnings, no utf8proc link in `build-c`).

## Risks / scope

- **Width-0 combining marks**: dropped (not composed) in U2 — full grapheme
  handling is the later "Full" slice. A lone combining mark renders as nothing
  (acceptable; it doesn't corrupt columns).
- **Double-width at the right margin**: if a width-2 char would straddle the
  last column, emit `$`/space as today's overflow handling does (guard `vtcol +
  2 > ncol`).
- The `#ifdef` divergence is concentrated in 3 functions + the shared render
  walk + the two column loops — bounded, but it is the most delicate change in
  the project. OFF path is left exactly as upstream.

## Out of scope (U2)

Cursor *motion* / delete by char (U3); word classification (U4); grapheme
clusters + NFC (Full); input-path assembly (codepoints already store as bytes).
