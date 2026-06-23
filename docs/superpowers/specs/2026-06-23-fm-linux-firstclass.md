# FM-LINUX-FIRSTCLASS — best-of-class Linux: recursive inotify + platform tooling

## Why

The Docker pass (PR pending) got mg building + 185/185 on Arch and Alpine, but
the inotify backend that now compiles is, by its own comment, "written to mirror
the kqueue backend." It watches only the explicit roots **non-recursively**, so
a nested change — `src/sub/file.c` edited, a commit writing `.git/refs/heads/*`
and `.git/logs/*`, a branch switch — does **not** wake the monitor. That is
adequate for a port; it is not best-of-class. Linux is becoming the primary
workstation, so this makes the Linux story first-class on two axes:

1. **The watcher (code).** A real recursive inotify implementation.
2. **The platform (tooling).** Native build presets, CI, install/packaging.

## Part 1 — recursive inotify watcher

### Design decisions

- **Recursion is an inotify (Linux) capability; kqueue (macOS) stays as-is.**
  Making kqueue recursive means one open fd per directory and hits macOS's
  256-fd default on any real tree — a separate, lower-value effort (macOS is now
  secondary). The shared interface gains an optional ignore-list; the *recursion*
  is backend-internal. macOS behavior is unchanged (no regression); Linux gets
  the better implementation. Parity is noted as future work.
- **Coarse events.** The monitor recomputes the whole status on *any* change, so
  the watcher need not report precise paths or match rename cookies — it emits
  one "something changed" event per `wait()`. This keeps recursion tractable
  (no per-event path bookkeeping for the consumer) and matches the consumer.
- **Ignore-list, not git-awareness.** `create(roots, ignores)` skips any path
  under an ignore prefix. The monitor passes `ignore = {<repo>/.git/objects}` —
  the one guaranteed giant (256 fanout dirs + pack churn) that is irrelevant to
  status. The watcher stays git-agnostic; smarter git-aware ignores (skip
  `.gitignore`d trees) are a later monitor-side enhancement.

### Behavior (the best-of-class checklist)

- **Recursive registration:** on `create`, walk each root and `inotify_add_watch`
  every directory (skipping ignored prefixes). Maintain a `wd → dirpath` map.
- **Dynamic tree:** on `IN_CREATE|IN_ISDIR` / `IN_MOVED_TO|IN_ISDIR`, add a watch
  for the new dir *and rescan it* (files created between mkdir and the add would
  otherwise be missed). On `IN_DELETE_SELF` / `IN_IGNORED`, drop the wd from the
  map (the kernel auto-removes the watch).
- **Overflow safety:** `IN_Q_OVERFLOW` → emit a change event (the monitor does a
  full recompute, which *is* the resync) rather than losing state.
- **Drain to EAGAIN:** read the inotify fd in a loop until `EAGAIN`, parsing
  multiple/var-length records per read; never leave events queued.
- **Watch-limit grace:** if `inotify_add_watch` fails `ENOSPC`
  (`max_user_watches` exhausted on a huge tree), stop deepening, set a "degraded"
  flag, and still function on the watches held; surface it once so behavior is
  honest rather than silently partial.
- **Wake unchanged:** the existing `eventfd` wake stays; `wait()` still returns
  empty on wake.

### Interface change

`create(std::span<const std::string> roots, std::span<const std::string>
ignores = {})`. kqueue ignores `ignores` and watches `roots` as today; inotify
recurses `roots` minus `ignores`. The monitor passes
`roots = {repo, repo/.git}`, `ignores = {repo/.git/objects}` (inotify dedups the
overlapping `.git`; kqueue keeps watching both tops as before).

### Tests (Linux-gated, run in CI + Docker)

- recursive: create nested dirs, touch a deep file → `wait()` returns an event.
- dynamic: after `create`, `mkdir` a new subdir then touch a file in it → event.
- ignore: a change under an ignored prefix → no event (wake still works).
- overflow/drain: many rapid changes → all coalesced, no hang, `wait()` returns.
- existing kqueue tests stay green on macOS unchanged.

## Part 2 — platform tooling

- **Linux presets** in `CMakePresets.json`: `cpp-linux` (configure) mirroring
  `cpp` but with distro `clang`/`clang++`, `CMAKE_POLICY_VERSION_MINIMUM=3.5`,
  Ninja — so `cmake --preset cpp-linux && ctest --preset cpp-linux` works
  natively on Arch with no `-D` soup. A `cpp-linux-tsan` variant (native Linux
  TSan just works — no Apple-runtime dance).
- **CI** (`.github/workflows/linux.yml`): matrix over Arch + Alpine, building via
  the Dockerfiles and running the full suite (incl. the new recursive tests on
  real inotify). This is where the Linux-gated tests actually execute.
- **Install/packaging:** ensure `cmake --install` lays down the `mg` binary, the
  `mg.1` man page, and the sample startup file with correct paths; document the
  Arch build (`cmake --preset cpp-linux`) in the Dockerfile header / README note.

## Phased implementation (each builds + tests green; macOS + OFF unchanged)

1. **Recursive inotify** + ignore-list interface + monitor passes the ignore;
   Linux-gated doctests. Validate in the Alpine/Arch containers.
2. **Linux presets** (`cpp-linux`, `cpp-linux-tsan`); switch the Dockerfiles to
   use them (drop the inline `-D` soup).
3. **CI workflow** (Arch + Alpine matrix) running build + full ctest.
4. **Install/packaging** rules + a short README/Docker note.

## Outcome (shipped)

All four phases landed and validated in containers:

- **Recursive inotify** — `add_tree()` walks each root depth-first, watches every
  directory (wd→path map), skips ignored prefixes; `wait()` drains to EAGAIN,
  adds+rescans dirs created at runtime, drops removed wds, treats `IN_Q_OVERFLOW`
  as a coarse change, and degrades on `ENOSPC`. Three Linux-gated doctests
  (recursive / dynamic / ignore) pass on **real inotify**: Alpine 188/188, Arch
  188/188. macOS (kqueue) unchanged at 185/185.
- **Presets** — `cpp-linux` + `cpp-linux-tsan`; the Dockerfiles use them (no more
  inline `-D` soup). `cmake --preset cpp-linux && ctest --preset cpp-linux` works
  natively on Arch.
- **CI** — `.github/workflows/linux.yml`: Arch + Alpine build+test (x86_64
  runner, native), plus an informational native-Linux TSan canary.
- **Install** — `cmake --install` lays down `bin/mg` + `share/man/man1/mg.1`
  only (doctest excluded via `EXCLUDE_FROM_ALL`). Verified on macOS + Arch.

Follow-up (resolved both caveats):

- **macOS parity** — the kqueue backend is now recursive too (per-dir fd +
  EVFILT_VNODE, dynamic rescan-on-change, `RLIMIT_NOFILE` raised, degrade on
  EMFILE). The 3 recursive doctests now run on **both** platforms (synchronous
  arming on both; FSEvents was rejected because its async arming + coalescing
  latency would make the tests flaky). macOS is now 188/188.
- **Linux TSan** — what looked like an "arm64 artifact" was two real things:
  (1) libgit2's uninstrumented global mutexes (false positives) → filtered via
  `tests/tsan.supp`; and (2) a **genuine data race** — the per-call
  `git_libgit2_init`/`shutdown` refcount crossing 0↔1 concurrently on the
  monitor and worker threads, racing libgit2's global **OpenSSL** setup/teardown
  (invisible on macOS, which uses SecureTransport). Fixed by holding one
  `mg::git::global_init()` for the whole monitor+worker lifetime
  (`mg_magit_start`→`stop`) so the refcount never returns to 0 mid-flight. Linux
  TSan is now **188/188, 0 races**, and the CI TSan job is a required gate (no
  longer a canary). The two latent Linux-only build bugs (lib/ include path,
  inotify ternary) were fixed in the preceding Docker commit.

## Out of scope

- Recursive kqueue / full macOS parity (separate; macOS now secondary).
- git-aware ignore of `.gitignore`d trees (monitor-side; later).
- fsmonitor/fanotify daemon-style incremental status (libgit2-gated; the
  FM-ASYNC-STATUS spec's "syscall ceiling" note).
- A general watchman replacement — scope is mg's status monitor needs.
