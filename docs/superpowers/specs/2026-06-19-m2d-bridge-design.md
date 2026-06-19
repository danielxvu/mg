# M2d — background monitor + `extern "C"` modeline bridge

**Status:** approved 2026-06-19 · **Task:** M2d (last M2 sub-task) ·
**Branch:** `cpp-refactor`

Composes M2a–M2c into a running feature and wires it into mg. Split so the C++
half is testable without touching the legacy core.

## M2d-1 — C++ monitor + `extern "C"` bridge (no C-core edits)

### `monitor` (in `src/magit/bridge.cpp`, internal)

Owns the whole pipeline on a background thread:

- Members: `mg::fswatch::watcher` (worktree root + `.git`), `mg::stop_flag`,
  `std::thread`, and published state — a `std::mutex`-guarded `std::string`
  current line + `std::atomic<bool> dirty`.
- Thread body: publish once at start, then
  `for (auto ev : watch_stream(watcher_, stop_)) { publish(); }` where
  `publish()` = `summarize(repo_status(repo_))` stored under the lock + `dirty=true`.
- Cancellation (destructor): `stop_.request_stop(); watcher_.wake(); thread_.join();`
  — instant, via the wakeable watcher. The destructor joins before members tear
  down, so the by-reference `watch_stream` never dangles.

### Bridge API (`src/magit/bridge.h`, plain C / `extern "C"`)

```c
void mg_magit_start(const char *repo_path); /* spawn the monitor singleton */
void mg_magit_stop(void);                   /* stop + join */
int  mg_magit_take_dirty(void);             /* read-and-clear, like winch_flag */
int  mg_magit_modeline(char *buf, size_t n);/* copy current line under lock; returns len */
```

`bridge.cpp` (regular TU, `import`s the modules) defines these over a
`std::unique_ptr<monitor>` singleton. The buffer-fill getter means no C++ string
lifetime crosses the FFI boundary. The C side only ever calls these from the
main thread; cross-thread state is the monitor's mutex/atomic.

### Test (`test_bridge`, no mg core)

Build a libgit2 fixture repo (1 staged + 1 untracked) → `mg_magit_start` → poll
`mg_magit_take_dirty` (timeout-bounded) until set → `mg_magit_modeline` returns
`"git *1 ?1"` → `mg_magit_stop`.

## M2d-2 — wire into the C core (separate iteration, the delicate part)

- `main.c`: near the `winch_flag` check, `if (mg_magit_take_dirty()) sgarbf = TRUE;`
  and `mg_magit_start(...)` / `mg_magit_stop()` in startup/teardown.
- `display.c` `modeline()`: append `mg_magit_modeline(buf, n)` to the mode line.
- **Live-while-idle decision (resolved 2026-06-19): on-interaction.** This
  matches default Magit/VC behavior (refresh on command/save, not continuous
  watching). The main-loop `take_dirty()` check redraws on the next interaction;
  the fs-watcher already makes detection instant, so the modeline is never more
  than one keypress stale. Live-while-idle (signal à la winch, or the
  Emacs-native watch-fd-in-`ttgetc`-poll) stays a clean follow-up — the watcher
  already exposes `fd()` for the latter.

**M2d-2 done (2026-06-19):** hooks added under `#ifdef ENABLE_NATIVE_MAGIT`
(`main.c` start/atexit-stop + dirty→`sgarbf`; `display.c` modeline append);
root CMake links `mg → mg_magit`, defines the macro, sets `LINKER_LANGUAGE CXX`;
`repo_status` uses `git_repository_open_ext` (walk-up). Verified: cpp `mg` shows
`git *1 ?2` in the modeline (pty-driven in a temp repo); OFF `mg` has **zero**
`mg_magit` symbols (`nm`); both build 0-warning.

## Out of scope (M2d-1)

Any edit to `src/*.c`. Recursive directory watching, and choosing exactly which
`.git` paths to watch beyond worktree-root + `.git`, are follow-ups.
