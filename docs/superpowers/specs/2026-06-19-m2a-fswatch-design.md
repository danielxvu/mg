# M2a — `mg.fswatch` OS-abstracted filesystem watcher

**Status:** approved 2026-06-19 · **Task:** M2a (first of four M2 sub-tasks) ·
**Branch:** `cpp-refactor`

## Context: M2 decomposition

M2 ("coroutine poller feeding the modeline") is too big for one spec. It splits
into four sub-tasks, built in order:

- **M2a — `mg.fswatch`** *(this spec)*: OS-abstracted, event-driven filesystem
  watcher. Synchronous resource: blocking `wait(timeout)`. The OS-specific part.
- **M2b**: coroutine layer — `co_await` awaitables over the watcher (and later
  the git subprocess) driving a `git_monitor` pipeline.
- **M2c**: run `git status --porcelain` on change, summarize via M1's
  `parse_status` into a modeline string.
- **M2d**: `extern "C"` bridge + minimal C-core hook. **Integration model
  (decided):** a background `std::thread` blocks in the watcher, runs git,
  publishes a snapshot, sets a dirty flag and nudges the UI read — mirroring
  mg's existing `winch_flag`/SIGWINCH idiom (tty.c:48, main.c:251). Concurrency
  stays inside C++; the C core gets only a flag check + a status getter.

## Amendment (wakeable watcher, 2026-06-19)

`wait(timeout)` was replaced by a **blocking `wait()`** (no timeout) plus a
thread-safe **`wake()`**, so the watcher is fully event-driven with zero idle
wake-ups. `wake()` uses the OS-native primitive — kqueue `EVFILT_USER` (no extra
fd) / inotify `eventfd`. A blocked `wait()` returns on a real fs change *or* a
`wake()` (empty vector). Cancellation = `stop_flag` + `wake()`. The
slice-3 "times out to empty" test became the "`wake()` unblocks a blocked
`wait()`" test (a thread blocks in `wait()`, the main thread `wake()`s it).

## Goal (M2a)

Event-driven, *not* periodic-polling, watching of a set of paths, behind one
interface over two OS backends. Greenfield, zero coupling to mg's C core.

## Module & build

- `export module mg.fswatch;` at `src/magit/fswatch.cppm`, compiled into the
  `mg_magit` module library (gated by `ENABLE_NATIVE_MAGIT`).
- Consumed by a `test_fswatch` doctest binary that links `mg_magit`.

## Backends (compile-time selected, zero runtime cost)

```
#if defined(__linux__)                                          -> inotify
#elif __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__     -> kqueue
#else  #error unsupported platform
```

One identical public interface over both. On macOS the **kqueue** backend
compiles and is genuinely TDD'd here. The **inotify** backend is written to
mirror it but can only run on Linux — marked untested-on-this-host; Linux CI
covers it. (kqueue also covers FreeBSD/OpenBSD/NetBSD, so testing it here
validates the mechanism for three of the four target OSes.)

## Public interface (`namespace mg::fswatch`)

```cpp
struct fs_event   { std::string path; };               // which watched path fired
struct watch_error { std::string message; int err; };  // err = errno at failure

class watcher {                       // RAII, move-only
  public:
    static std::expected<watcher, watch_error>
        create(std::span<const std::string> paths);

    std::expected<std::vector<fs_event>, watch_error>
        wait(std::chrono::milliseconds timeout);

    int fd() const noexcept;          // kqueue/inotify descriptor (poll-able)
    ~watcher();                       // closes the queue fd + per-path fds
};
```

## Semantics

- `create` opens the OS queue and registers each path for write/delete/rename
  notification (kqueue: an `O_RDONLY` fd per path with
  `NOTE_WRITE|NOTE_DELETE|NOTE_RENAME|NOTE_EXTEND`; inotify:
  `IN_MODIFY|IN_CREATE|IN_DELETE|IN_MOVE`). Any path that can't be opened →
  `unexpected(watch_error{…, errno})`, after closing whatever was already opened.
- `wait(timeout)` blocks up to `timeout` and returns the events that fired —
  an **empty vector on timeout (not an error)**. `EINTR` is treated as a
  spurious empty return. Events coalesce per watched path.
- Move-only; the destructor closes all descriptors. A moved-from watcher holds
  `fd() == -1` and is inert.

## TDD slices (real, via `mkdtemp` temp dirs on macOS/kqueue)

1. `create` on an existing directory → success, `fd() >= 0`.
2. create a file inside the watched dir → `wait(1s)` returns ≥1 event whose
   `path` is the watched dir.
3. no change → `wait(50ms)` returns an **empty** vector (success, not error).
4. `create` on a nonexistent path → `unexpected`, `error().err == ENOENT`.

## Out of scope for M2a

The background thread, coroutines, running git, and the `extern "C"` bridge
(those are M2b–d). Recursive directory trees and watch-descriptor limits are
follow-ups; M2a watches exactly the paths it is given.

## Testing

doctest `test_fswatch` target; temp dirs via `mkdtemp`, cleaned up with
`std::filesystem::remove_all`. No reliance on mg's repo or live git.
