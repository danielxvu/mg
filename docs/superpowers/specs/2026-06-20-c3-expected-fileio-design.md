# C3 — `std::expected` file-IO layer (`mg.io`)

**Status:** approved (roadmap item) 2026-06-20 · **Branch:** `cpp-refactor`

The pivot back to the core C++23 thesis. mg's `fileio.c` speaks in `int` return
codes (`FIOSUC`/`FIOFNF`/`FIOEOF`/`FIOERR`/`FIODIR`) and out-params. C3 builds a
modern, **greenfield** module `mg.io` that expresses the same primitives as
`std::expected<T, io_error>` and composes them with `.and_then()`/`.or_else()`
— no C-style int codes, no out-params, errors as values.

Like the magit modules, `mg.io` is **isolated**: gated by `ENABLE_CPP_UPGRADES`,
tested standalone via doctest, and does NOT touch the legacy C core (the OFF
build is byte-for-byte upstream). Wiring it into `fileio.c`'s call sites is an
explicit, separately-gated future step — out of scope here.

## C3-1 — core ops (this slice)

`src/io/io.cppm` → `export module mg.io;` (library `mg_io`).

```cpp
struct io_error { int code; std::string message; };   // code == errno (0 = none)
struct file_info { std::uintmax_t size; std::time_t mtime; bool is_dir; bool writable; };

std::expected<file_info,   io_error> stat_file (std::string path);
std::expected<std::string, io_error> read_file (std::string path);
std::expected<void,        io_error> write_file(std::string path, std::string_view data,
                                                int mode = 0644);
```
- POSIX `open`/`read`/`write`/`fstat` under the hood (matches what `fileio.c`
  ultimately syscalls, and gives real `errno` fidelity: `ENOENT`, `EISDIR`, …).
  A move-only RAII `fd` guard closes on every path.
- `read_file` rejects directories (`EISDIR`) so it can't silently slurp a dir.
- Tests (`test_io`): write→read round-trip; `stat_file` reports size/dir/writable;
  missing path → `io_error{ENOENT, …}`; reading a directory → `EISDIR`.

## C3-2 — monadic composition

```cpp
std::expected<std::vector<std::string>, io_error> read_lines(std::string path);
std::expected<void, io_error> copy_file(std::string from, std::string to);
```
- `read_lines` = `read_file(path).and_then(split-on-'\n')` — pure `.and_then()`.
- `copy_file`  = `read_file(from).and_then([&](auto s){ return write_file(to, s); })`.
- An `.or_else()` example: `read_file(path).or_else(default-on-ENOENT)` helper in
  the tests, proving the fallback combinator.
- Tests: `read_lines` splits correctly + propagates `ENOENT`; `copy_file`
  duplicates content and fails cleanly when the source is missing.

## Build wiring

- New `src/io/CMakeLists.txt`: `add_library(mg_io)` with a PUBLIC `CXX_MODULES`
  file set (`io.cppm`); `target_include_directories` PUBLIC for any future
  `extern "C"` header. Root `CMakeLists.txt`: `add_subdirectory(src/io)` under
  `if(ENABLE_CPP_UPGRADES)` (NOT gated on `ENABLE_NATIVE_MAGIT` — this is a core
  module). `tests/`: `test_io` links `mg_io`, registered under
  `if(ENABLE_CPP_UPGRADES)`.

## Out of scope

Rewiring `fileio.c`/`fileio` to call `mg.io`; gzip/compression (mg's `isgzip`);
backup-file logic; `~`/path expansion (mg's `expandtilde`/`adjustname` stay C);
async/coroutine IO; mmap. Plain read/write/stat with monadic composition only.
