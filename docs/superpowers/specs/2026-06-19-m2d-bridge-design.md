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
- **Live-while-idle decision (deferred to M2d-2):** mirror the `winch_flag`
  path in `ttgetc`'s EINTR handler so a git change redraws the modeline while
  mg is blocked in `read()`. Requires the monitor to signal the main thread.
  Alternative: update-on-next-interaction (no signal). Decide at M2d-2.

## Out of scope (M2d-1)

Any edit to `src/*.c`. Recursive directory watching, and choosing exactly which
`.git` paths to watch beyond worktree-root + `.git`, are follow-ups.
