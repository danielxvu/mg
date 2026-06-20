# C3.5 — wire `mg.io` into the C core (`fisdir`)

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

`mg.io` (C3) and `mg.text` (C1) are dormant: tested, but parallel to the C core,
replacing nothing. C3.5 converts `mg.io` into a *real* replacement by routing a
single, safe legacy I/O call through it — proving the "C++ replaces C" thesis on
the smallest possible surface, with the OFF build byte-for-byte unchanged.

## Target: `fisdir`

`fileio.c`'s `fisdir(path)` does one `stat()` + `S_ISDIR`, returning
`ABORT`/`TRUE`/`FALSE`. It is pure (no buffer state), read-only (can't corrupt
anything), and maps 1:1 to `mg::io::stat_file(path).is_dir`. 7 callers
(file-visiting in `file.c`, `ffropen`, `main.c` arg handling) — exercised every
time mg opens a path.

## Plan

1. **Bridge** — `src/io/bridge.{h,cpp}` (added to the `mg_io` library):
   ```c
   int mg_io_isdir(const char *path);  // 1 = dir, 0 = not, -1 = stat error
   ```
   `bridge.cpp` `import mg.io;` and returns from `stat_file(path)`:
   `!st → -1`, else `st->is_dir ? 1 : 0`. No mg types cross the boundary.

2. **C core** — wrap `fisdir` (already `#ifndef fisdir`-guarded) with an inner
   `#ifdef ENABLE_CPP_UPGRADES`: route to `mg_io_isdir` and map
   `1→TRUE / 0→FALSE / -1→ABORT`; the `#else` keeps the *exact* original
   `stat`/`S_ISDIR` body. `#include "io/bridge.h"` under the same `#ifdef`.

3. **Build** — root `CMakeLists.txt`, in the existing `if(ENABLE_CPP_UPGRADES)`
   block: `target_link_libraries(mg PRIVATE mg_io)`,
   `target_compile_definitions(mg PRIVATE ENABLE_CPP_UPGRADES)`,
   `set_target_properties(mg PROPERTIES LINKER_LANGUAGE CXX)` (mirrors the
   `ENABLE_NATIVE_MAGIT`/`mg_magit` wiring; harmless that the magit block sets
   the same when both are on).

## Tests

- `test_io_bridge` (TDD): `mg_io_isdir` → 1 on a temp dir, 0 on a file, -1 on a
  missing path.
- End-to-end: ON build links and runs; `mg <dir>` opens dired (main.c arg path
  goes through `fisdir`), verified via pty. OFF build: `fisdir` body identical,
  no `mg_io` symbols, `mg -h` still works.

## Out of scope

Routing the hot read loop (`ffropen`/`ffgetline`) or writes through `mg.io`
(higher risk — later); replacing `fstat`-based `ffstat`/`fchecktime`; wiring
`mg.text`. One call site, to prove the mechanism.
