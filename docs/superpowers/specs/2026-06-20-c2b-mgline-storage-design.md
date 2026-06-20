# C2 phase (b) — `mg.line` C++ line storage

**Status:** approved (user chose the C++ `mg.line` option) 2026-06-20 ·
**Branch:** `cpp-refactor`

Phase (a) sealed `struct line` behind accessors. Phase (b) swaps the storage
*implementation* behind that seam: under `ENABLE_CPP_UPGRADES`, a C++23 `mg.line`
module owns the line's bytes with RAII (`std::vector<char>`) instead of manual
`malloc`/`realloc`/`free`. The OFF build keeps the faithful C storage. This
advances the C++-replaces-C thesis on the editor's most central data structure
and gives UTF-8-aware line ops a future home (see the UTF-8 goal).

## Why `std::vector<char>`, not `std::string` / a piece table

The accessors expose a **size vs used** distinction (`lsize` = allocated buffer,
`llength` = logical bytes) and `ltext()` must return a **contiguous** `char*`
that callers scan/`bcopy`/index directly. `std::vector<char>` maps 1:1:
`buf.size()` ↔ `l_size`, a separate `used` ↔ `l_used`, `buf.data()` ↔ `l_text`
(contiguous). `std::string` conflates length with size (writing `data()[i]` for
`i>=size()` is UB); a piece table fragments storage and would have to
materialize `ltext()` every call. `vector<char>` is the faithful, contiguous,
RAII fit.

```cpp
// mg.line (C++), the opaque struct line under ENABLE_CPP_UPGRADES:
struct line {
    line *l_fp, *l_bp;        // links stay pointer-based (unchanged list model)
    int   used;               // == l_used
    std::vector<char> buf;    // size() == l_size; data() == l_text (contiguous)
};
```

## The storage / editor split

`line.c` contains two layers; phase (b) cleaves them along the seam:
- **Storage core** (→ C++ under `#ifdef`): the `struct line` layout, the 11
  accessors, `lalloc` (construct), `lrealloc` (grow), and the raw release of a
  line's bytes. The *only* code that knows the layout.
- **Editor layer** (stays C, both builds): `lfree` (window/mark fixups + relink,
  then call the storage release), `linsert`, `ldelete`, `lnewline`,
  `ldelnewline`, `lreplace`, … — they touch storage ONLY through accessors +
  `lalloc`/`lrealloc`. After C2b-1 they contain zero direct member access.

## Decomposition

- **C2b-1 (prep, pure C, both builds):** convert `line.c`'s ~50 editor-layer
  member accesses to accessors (links → `lforw`/`lback`/`lsetforw`/`lsetback`;
  text → `ltext`/`llength`/`lgetc`/`lputc`/`lsetlen`; memmoves over
  `&ltext(lp)[i]`). Leave only the storage core raw (lalloc/lrealloc bodies +
  the `free` in lfree). Behavior-preserving; verify both builds.
- **C2b-2 (the swap):** `src/line/` → `mg.line` module + `extern "C"` bridge
  providing `lalloc`/`lrealloc`/`lfreestore` + the 11 accessors over the
  `vector<char>` layout. In `line.c`, `#ifdef ENABLE_CPP_UPGRADES` the storage
  core comes from the bridge (struct undefined here, accessors/lalloc/lrealloc
  not defined here, lfree's release calls `lfreestore`); `#else` keeps today's C
  storage. Root CMake links `mg→mg_line` under the flag. Verify: ON uses
  `vector<char>` storage (no `malloc` of `l_text`), OFF unchanged, both behave
  identically, 0 warnings.

## Risks / notes

- `lalloc(0)` then writing via `lputc(lp, llength(lp), c)` + `lsetlen` assumes
  capacity ≥ used+1; the editor calls `lrealloc` first (it already does). Keep
  that contract: `lputc`/`ltext`-write callers must `lrealloc` to size first
  (they do). `vector::resize` zero-fills new bytes (C `realloc` left them
  uninitialized) — harmless, only `[0,used)` is read.
- Lifetime: a C++ `struct line` must be `new`/`delete`d, not `malloc`/`free`d —
  so `lalloc`/`lfreestore` cross the bridge; `lfree`'s editor fixups stay C and
  call `lfreestore` at the end.
- libc allocation parity: `lrealloc` failure path returns FALSE just like today.

## Out of scope

UTF-8 codepoint ops (future, atop this); changing the linked-list line model;
moving `linsert`/`ldelete` logic into C++ (only storage moves).
