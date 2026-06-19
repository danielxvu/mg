# M2b — coroutine layer: `generator<T>` + `watch_stream`

**Status:** approved 2026-06-19 · **Task:** M2b (second of four M2 sub-tasks) ·
**Branch:** `cpp-refactor`

## Context

M2's integration model is a background thread + SIGWINCH-style flag (see the
M2a spec). M2b is the **coroutine layer** that turns the pull-based `mg.fswatch`
watcher into a lazy event *stream* the thread can iterate.

**Toolchain finding:** clang-21's libc++ has `<coroutine>` but **not** C++23
`<generator>`, so we hand-roll a small `generator<T>`.

**Amendment (during impl):** `std::stop_token` was the planned cancellation
type, but `<stop_token>` **fails to link through a module's global module
fragment** on clang-21's libc++ (the inline `__atomic_unique_lock::__set_locked_bit`
symbol isn't emitted — reproduced minimally; a toolchain bug, not our code). So
cancellation uses a tiny header-only `mg::stop_flag` (a shared `atomic<bool>`)
with the same `request_stop()`/`stop_requested()` contract. M2d will pair it
with a plain `std::thread` + RAII join wrapper instead of `std::jthread`.

## Components

### `mg.coro` — generic `generator<T>` (`src/magit/coro.cppm`)

A minimal, move-only, RAII coroutine generator:

```cpp
export module mg.coro;
export namespace mg {
template <class T> class generator {       // owns a std::coroutine_handle
  struct promise_type {                     // yield_value stores; return_void ends
      … initial_suspend = suspend_always (lazy)
      … final_suspend   = suspend_always (handle stays valid until destroyed)
      … unhandled_exception → std::terminate   // runs on a bg thread; must not throw out
  };
  // input-iterator surface: begin() resumes to the first co_yield, end() is a
  // null-handle sentinel, operator* returns the current value, ++ resumes.
  // move-only; destructor calls handle.destroy().
};
}
```

### `watch_stream` — async face of the watcher (added to `mg.fswatch`)

`mg.fswatch` gains `import mg.coro;` and exports:

```cpp
mg::generator<fs_event>
watch_stream(watcher w, mg::stop_flag stop, std::chrono::milliseconds timeout);
```

```cpp
while (!stop.stop_requested()) {
    auto evs = w.wait(timeout);
    if (!evs) co_return;            // watcher error ends the stream
    for (auto &e : *evs) co_yield e;
}
```

Consumer (M2d): `for (auto ev : watch_stream(std::move(w), tok, 250ms)) { … }`.
The watcher's `timeout` bounds how quickly the loop re-checks the stop token, so
dropping the owning `std::jthread` ends the stream within one timeout.

## Data flow

`watcher.wait()` (blocking, M2a) → `watch_stream` co_yields each `fs_event`
(M2b) → [M2c will run git + summarize] → [M2d publishes to the modeline].

## Error handling

A watcher error ends the stream (`co_return`). M2b does not surface the error
object; M2d decides whether to re-create the watcher. (Simplification — noted.)
`generator` itself never throws across the coroutine boundary (`terminate`).

## TDD slices

*`mg.coro`:*
1. a finite `generator<int>` yields `0,1,2` in order via range-for.
2. an **infinite** generator can be partially consumed and then safely
   destroyed while suspended (`handle.destroy()` unwinds it; ASan-verifiable).

*`watch_stream`:*
3. a **pre-stopped** `stop_token` → empty stream (`begin() == end()`, zero iters).
4. a change in the watched dir → the stream yields an `fs_event` whose `path`
   is that dir.

## Amendment (wakeable watcher, 2026-06-19)

`watch_stream` now takes the watcher **by reference** and drops the timeout:
`watch_stream(watcher &w, stop_flag stop)`. It loops on the watcher's blocking
`wait()` (M2a wakeable enhancement); the owner (M2d's monitor) cancels a blocked
stream with `stop.request_stop()` + `w.wake()`. By-reference means the owner must
outlive the stream — the monitor owns the watcher and joins its thread before
the watcher is destroyed.

## Out of scope

Git invocation (M2c) and the thread + `extern "C"` bridge (M2d). M2b is pure
coroutine glue, tested **synchronously** by pulling the generator on the test
thread (no background thread yet).

## Testing

`test_coro` (imports `mg.coro`) and new cases in `test_fswatch` (imports both
`mg.coro` and `mg.fswatch`); temp dirs via `mkdtemp` as in M2a.
