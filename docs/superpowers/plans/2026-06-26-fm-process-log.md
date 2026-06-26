# FM-PROCESS-LOG Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `*magit-process*` transparency buffer (opened with `$`) showing every state-changing git operation neomg performs, labeled `$` for real subprocesses and `≈ … (via libgit2)` for in-process ops.

**Architecture:** A new process-wide, mutex-guarded ring-buffer module (`mg.magit.proclog`) is appended to from three sites — the CLI mutation helpers in `git.cppm` (captured output), the network `_cli` wrappers in `bridge.cpp` (terminal-inherited, no output), and the libgit2 mutating wrappers in `bridge.cpp` (`≈` equivalents). A C ABI `mg_magit_process_log` streams entries newest-first as tagged lines; the editor renders them into a read-only buffer.

**Tech Stack:** C++23 named modules (clang-mp-21 on macOS / distro clang on Linux), libgit2, the existing `detail::run_git`/`detail::git_terminal` subprocess helpers, the mg C editor core (keymaps, `bfind`/`popbuf`/`addlinef`).

## Global Constraints

- All new engine code lives behind `ENABLE_NATIVE_MAGIT`; the **OFF build (`cpp-tsan`, `ENABLE_ZIG_STATUS=OFF`) must still compile and pass** — `proclog` has no Zig dependency, so it compiles in both.
- Two label kinds only: `$ ` prefix = real subprocess (literal argv); `≈ ` prefix + ` (via libgit2)` suffix = in-process op (never literally executed).
- Coverage: log **mutations + network only**. Never log reads (status/log/diff/blame) or the background fswatch refresh.
- Ring-buffer cap = **200** entries; oldest evicted first. **In-memory, session-scoped** (no disk persistence).
- Render **newest-first**; output indented two spaces under its command line.
- New line-kind constants: `MG_LINE_PROCESS_CMD 15`, `MG_LINE_PROCESS_OUT 16` (next free after `MG_LINE_CONFLICT_HUNK 14`).
- Verify each task on macOS (`cmake --preset cpp` + `ctest`); verify the full feature on macOS + Alpine/musl (Docker) + the OFF build before any PR (the verify-locally-before-CI rule).
- `mg_magit_emit_fn` signature is `void (*)(void *ctx, const char *line, int kind, const char *path, int hunk)`. Process lines pass `path = nullptr, hunk = 0`.

---

### Task 1: Process-log sink module + viewer C ABI

**Files:**
- Create: `src/magit/proclog.cppm`
- Modify: `src/magit/CMakeLists.txt:15` (add `proclog.cppm` to the CXX_MODULES file set)
- Modify: `src/magit/bridge.h` (add line-kind defines + `mg_magit_process_log` decl)
- Modify: `src/magit/bridge.cpp` (implement `mg_magit_process_log`; add `import mg.magit.proclog;`)
- Create: `tests/test_proclog.cpp`
- Modify: `tests/CMakeLists.txt` (register `test_proclog`)

**Interfaces:**
- Produces (consumed by Tasks 2-4):
  - `mg::magit::proclog::entry { char kind; std::string command; std::string output; bool ok; long duration_ms; }`
  - `void mg::magit::proclog::record(char kind, std::string command, std::string output, bool ok, long duration_ms)`
  - `std::vector<entry> mg::magit::proclog::snapshot()` — chronological copy, under lock
  - `void mg::magit::proclog::clear()`
  - `std::string mg::magit::proclog::argv_to_command(std::span<const std::string> argv)` — render argv as a shell-style line, single-quoting any arg containing a space/tab/`'`
  - C ABI: `int mg_magit_process_log(mg_magit_emit_fn emit, void *ctx)`

> Marker convention (used throughout): the stored `entry.kind` byte is `'$'`
> for a real subprocess (literal argv, real output) and `'~'` for an in-process
> libgit2 op (the `command` is the EQUIVALENT git command, output empty). The
> viewer (Step 6) maps `'~'` to the rendered `≈` glyph. Store the caller's byte
> verbatim.

- [ ] **Step 1: Write the failing unit test**

Create `tests/test_proclog.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

import mg.magit.proclog;

#include <string>
#include <vector>

using namespace mg::magit::proclog;

TEST_CASE("record then snapshot preserves chronological order")
{
    clear();
    record('$', "git fetch origin", "", true, 5);
    record('~', "git add foo", "", true, 0);
    auto s = snapshot();
    REQUIRE(s.size() == 2);
    CHECK(s[0].command == "git fetch origin");
    CHECK(s[0].kind == '$');
    CHECK(s[1].command == "git add foo");
    CHECK(s[1].kind == '~');
}

TEST_CASE("ring buffer evicts the oldest past the cap of 200")
{
    clear();
    for (int i = 0; i < 250; ++i)
        record('$', "git op " + std::to_string(i), "", true, 0);
    auto s = snapshot();
    REQUIRE(s.size() == 200);
    CHECK(s.front().command == "git op 50");  // 0..49 evicted
    CHECK(s.back().command == "git op 249");
}

TEST_CASE("argv_to_command single-quotes args with spaces")
{
    std::vector<std::string> argv{"git", "commit", "-m", "fix the bug"};
    CHECK(argv_to_command(argv) == "git commit -m 'fix the bug'");
    std::vector<std::string> plain{"git", "add", "foo.c"};
    CHECK(argv_to_command(plain) == "git add foo.c");
}
```

- [ ] **Step 2: Add `test_proclog` to the test CMake + try to build (verify it fails to compile — module missing)**

Add to `tests/CMakeLists.txt` inside the `if(ENABLE_NATIVE_MAGIT)` block (after the `test_git` block, mirroring it):

```cmake
    add_executable(test_proclog test_proclog.cpp)
    target_link_libraries(test_proclog PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_proclog)
```

Run: `cmake --preset cpp && cmake --build build --target test_proclog`
Expected: FAIL — `module 'mg.magit.proclog' not found`.

- [ ] **Step 3: Create the module (minimal) + register it in CMake**

Create `src/magit/proclog.cppm`:

```cpp
// mg.magit.proclog -- a process-wide, mutex-guarded ring buffer of the git
// operations neomg performs, for the *magit-process* transparency buffer.
// Appended to from the CLI mutation helpers (git.cppm) and the libgit2 mutating
// wrappers (bridge.cpp); read back via snapshot(). In-memory, session-scoped.
module;
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

export module mg.magit.proclog;

export namespace mg::magit::proclog {

// One logged operation. `kind` is a raw marker byte: '$' = real subprocess
// (literal argv in `command`, real `output`), '~' = in-process libgit2 op
// (`command` is the EQUIVALENT git command, `output` empty). `ok` is the
// success of the op; `duration_ms` is wall time for '$' ops (0 for '~').
struct entry {
    char kind;
    std::string command;
    std::string output;
    bool ok;
    long duration_ms;
};

void record(char kind, std::string command, std::string output, bool ok,
            long duration_ms);
std::vector<entry> snapshot();
void clear();
std::string argv_to_command(std::span<const std::string> argv);

} // namespace mg::magit::proclog

// ---- implementation (module-internal global: one ring per process) ----------
namespace mg::magit::proclog {
namespace {
constexpr std::size_t kCap = 200;
std::mutex g_mu;
std::deque<entry> g_ring; // guarded by g_mu
} // namespace

void record(char kind, std::string command, std::string output, bool ok,
            long duration_ms)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.push_back({kind, std::move(command), std::move(output), ok,
                      duration_ms});
    while (g_ring.size() > kCap)
        g_ring.pop_front();
}

std::vector<entry> snapshot()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return {g_ring.begin(), g_ring.end()};
}

void clear()
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.clear();
}

std::string argv_to_command(std::span<const std::string> argv)
{
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i)
            out += ' ';
        const std::string &a = argv[i];
        bool needs_quote = a.empty() ||
                           a.find_first_of(" \t'") != std::string::npos;
        if (!needs_quote) {
            out += a;
            continue;
        }
        // single-quote, escaping embedded single quotes as '\'' (POSIX idiom).
        out += '\'';
        for (char c : a) {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += '\'';
    }
    return out;
}

} // namespace mg::magit::proclog
```

Register it: change `src/magit/CMakeLists.txt:15` from
`PUBLIC FILE_SET CXX_MODULES FILES coro.cppm magit.cppm fswatch.cppm git.cppm)`
to
`PUBLIC FILE_SET CXX_MODULES FILES coro.cppm magit.cppm fswatch.cppm git.cppm proclog.cppm)`

- [ ] **Step 4: Run the unit test (verify PASS)**

Run: `cmake --build build --target test_proclog && ./build/tests/test_proclog`
Expected: PASS (3 cases — the sink unit tests from Step 1).

- [ ] **Step 5: Write the failing viewer (C ABI) test**

Add to `tests/test_proclog.cpp` (needs the C ABI header):

```cpp
#include "bridge.h"  // mg_magit_process_log + MG_LINE_PROCESS_*

TEST_CASE("mg_magit_process_log streams entries newest-first, tagged")
{
    clear();
    record('~', "git add foo.c", "", true, 0);
    record('$', "git push origin HEAD", "(output shown in terminal)", true, 3100);

    struct row { std::string line; int kind; };
    std::vector<row> rows;
    int n = mg_magit_process_log(
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);

    REQUIRE(n == static_cast<int>(rows.size()));
    REQUIRE(rows.size() >= 3); // push CMD + push OUT + add CMD
    // Newest (push) first; '$' literal; output indented under it.
    CHECK(rows[0].kind == MG_LINE_PROCESS_CMD);
    CHECK(rows[0].line.rfind("$ git push origin HEAD", 0) == 0);
    CHECK(rows[0].line.find("3.1s") != std::string::npos);
    CHECK(rows[1].kind == MG_LINE_PROCESS_OUT);
    CHECK(rows[1].line.find("output shown in terminal") != std::string::npos);
    // Then the libgit2 add, rendered with the ≈ marker.
    CHECK(rows.back().kind == MG_LINE_PROCESS_CMD);
    CHECK(rows.back().line.find("git add foo.c") != std::string::npos);
    CHECK(rows.back().line.find("via libgit2") != std::string::npos);
}

TEST_CASE("mg_magit_process_log emits an empty-state line when nothing logged")
{
    clear();
    std::vector<std::string> lines;
    int n = mg_magit_process_log(
        [](void *ctx, const char *line, int, const char *, int) {
            static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
        },
        &lines);
    CHECK(n == 1);
    CHECK(lines.size() == 1);
    CHECK(lines[0].find("No git operations") != std::string::npos);
}
```

`test_proclog` already links `mg_magit`; `bridge.h` is at `src/magit/bridge.h`. Add its include dir if needed: in `tests/CMakeLists.txt` after the `add_executable(test_proclog …)` line add
`target_include_directories(test_proclog PRIVATE ${CMAKE_SOURCE_DIR}/src/magit)`.

Run: `cmake --build build --target test_proclog`
Expected: FAIL — `mg_magit_process_log` undefined + `MG_LINE_PROCESS_CMD` undeclared.

- [ ] **Step 6: Add the line-kind defines + decl, implement the viewer**

In `src/magit/bridge.h`, after the `#define MG_LINE_CONFLICT_HUNK 14` line, add:

```c
#define MG_LINE_PROCESS_CMD   15  /* a process-log command line ($ or ≈) */
#define MG_LINE_PROCESS_OUT   16  /* an output line under a process-log command */
```

In the same header, near the other viewer decls (after `mg_magit_log_graph_buffer`), add:

```c
/* Stream the session's git-operation log, newest entry first. Each entry emits
 * one MG_LINE_PROCESS_CMD header ("$ <cmd>  (1.2s, ok)" for real subprocesses,
 * "≈ <cmd>  (via libgit2)" for in-process ops) followed by its output as
 * MG_LINE_PROCESS_OUT lines (indented). Emits a single MG_LINE_OTHER
 * empty-state line when nothing has been logged. Returns the line count. */
int mg_magit_process_log(mg_magit_emit_fn emit, void *ctx);
```

In `src/magit/bridge.cpp`, add near the other module imports at the top:
`import mg.magit.proclog;`

Then implement (place it next to the other `extern "C"` viewers, e.g. after `mg_magit_log_buffer`):

```cpp
extern "C" int mg_magit_process_log(mg_magit_emit_fn emit, void *ctx)
{
    auto entries = mg::magit::proclog::snapshot();
    if (entries.empty()) {
        emit(ctx, "No git operations recorded this session.", MG_LINE_OTHER,
             nullptr, 0);
        return 1;
    }
    int n = 0;
    // newest first
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto &e = *it;
        std::string head;
        if (e.kind == '$') {
            head = "$ " + e.command + "  (";
            // duration as seconds with one decimal, then ok/fail
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1fs", e.duration_ms / 1000.0);
            head += buf;
            head += e.ok ? ", ok)" : ", failed)";
        } else { // '~' -> libgit2
            head = "\xE2\x89\x88 " + e.command + "  (via libgit2"; // ≈ = U+2248
            head += e.ok ? ")" : ", failed)";
        }
        emit(ctx, head.c_str(), MG_LINE_PROCESS_CMD, nullptr, 0);
        ++n;
        // output lines, indented two spaces
        const std::string &out = e.output;
        std::size_t start = 0;
        while (start < out.size()) {
            std::size_t nl = out.find('\n', start);
            std::string line = out.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() || nl != std::string::npos) {
                emit(ctx, ("  " + line).c_str(), MG_LINE_PROCESS_OUT, nullptr, 0);
                ++n;
            }
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }
    return n;
}
```

Run: `cmake --build build --target test_proclog && ./build/tests/test_proclog`
Expected: PASS (5 cases).

- [ ] **Step 7: Build everything + run the full suite (no regressions)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all pass (the existing 232 + the new `test_proclog` cases).

- [ ] **Step 8: Commit**

```bash
git add src/magit/proclog.cppm src/magit/CMakeLists.txt src/magit/bridge.h \
        src/magit/bridge.cpp tests/test_proclog.cpp tests/CMakeLists.txt
git commit -m "feat(proclog): process-log ring-buffer module + viewer C ABI"
```

---

### Task 2: `$` capture wiring — real git subprocesses

**Files:**
- Modify: `src/magit/git.cppm` (add `import mg.magit.proclog;` + `#include <chrono>`; log in the CLI mutation helpers `commit_via_cli` and `apply_via_cli`)
- Modify: `src/magit/bridge.cpp` (log in `mg_magit_fetch_cli`/`push_cli`/`pull_cli`)
- Modify: `tests/test_bridge.cpp` (capture + terminal + hook-rejection tests)

**Interfaces:**
- Consumes: `mg::magit::proclog::record`, `argv_to_command` (Task 1).
- The CLI helpers already build a `std::vector<std::string> args` and call `detail::run_git(repo, args)`. Log AFTER the call, prepending `"git"` to the argv for the command string. Do **not** log inside `detail::run_git` itself — it is also used for the `rev-parse --short=8 HEAD` read (git.cppm:3427), which must not appear in the log.

- [ ] **Step 1: Write the failing capture test**

Add to `tests/test_bridge.cpp` (it already has `make_repo_full`, `make_repo_with_changes`, and `#include "bridge.h"`; add `import mg.magit.proclog;` at the top with the other imports if not present):

```cpp
TEST_CASE("a CLI commit logs a $ entry with output and ok=true")
{
    auto dir = make_repo_with_changes(); // staged.txt is staged
    mg::magit::proclog::clear();
    REQUIRE(mg_magit_commit(dir.string().c_str(), "log me") == 1);

    auto s = mg::magit::proclog::snapshot();
    bool found = false;
    for (const auto &e : s)
        if (e.kind == '$' && e.command.find("git commit") != std::string::npos) {
            CHECK(e.ok);
            CHECK(e.command.find("log me") != std::string::npos);
            found = true;
        }
    CHECK(found);
    fs::remove_all(dir);
}
```

(Confirm `mg_magit_commit`'s signature in `bridge.h`; it is `int mg_magit_commit(const char *repo_path, const char *message)` returning 1 on success. Adjust the call if the message is passed differently.)

Run: `cmake --build build --target test_bridge && ./build/tests/test_bridge -tc="a CLI commit logs a \$ entry with output and ok=true"`
Expected: FAIL — no matching entry (nothing logs yet).

- [ ] **Step 2: Log in the CLI mutation helpers**

In `src/magit/git.cppm`, add at the top with the other imports/includes:
```cpp
import mg.magit.proclog;
```
and in the `module;` preamble includes add `#include <chrono>`.

Find `commit_via_cli` (the helper that `commit`/`commit_amend`/`commit_extend`/`commit_reword` delegate to — it builds `args` then calls `detail::run_git(repo, args)` and on success does a `rev-parse --short=8 HEAD`). Wrap the mutation `run_git` call with timing + a log append. Concretely, at the site `auto run = detail::run_git(repo, args);` for the COMMIT/APPLY argv (NOT the rev-parse), change to:

```cpp
    auto t0 = std::chrono::steady_clock::now();
    auto run = detail::run_git(repo, args);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    {
        std::vector<std::string> cmd;
        cmd.reserve(args.size() + 1);
        cmd.push_back("git");
        for (const auto &a : args) cmd.push_back(a);
        mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                                   run.output, run.code == 0, ms);
    }
```

Apply the **same** wrapping at the `detail::run_git(repo, ...)` mutation sites inside `apply_via_cli` (merge/cherry-pick/revert; git.cppm:3418 and 3452). Leave the `rev-parse --short=8 HEAD` call (git.cppm:3427) untouched — it is a read.

- [ ] **Step 3: Run the capture test (verify PASS)**

Run: `cmake --build build --target test_bridge && ./build/tests/test_bridge -tc="a CLI commit logs a \$ entry with output and ok=true"`
Expected: PASS.

- [ ] **Step 4: Write + pass the hook-rejection (ok=false) test**

Add to `tests/test_bridge.cpp` (mirror the pre-commit-hook substrate already used in the commit tests — find the existing hook test for the exact `chmod`/hook-write idiom and reuse it):

```cpp
TEST_CASE("a commit rejected by a pre-commit hook logs ok=false with output")
{
    auto dir = make_repo_with_changes();
    // install a pre-commit hook that always fails
    fs::create_directories(dir / ".git" / "hooks");
    {
        std::ofstream h(dir / ".git" / "hooks" / "pre-commit");
        h << "#!/bin/sh\necho REJECTED_BY_HOOK 1>&2\nexit 1\n";
    }
    fs::permissions(dir / ".git" / "hooks" / "pre-commit",
                    fs::perms::owner_all, fs::perm_options::add);

    mg::magit::proclog::clear();
    mg_magit_commit(dir.string().c_str(), "blocked");

    auto s = mg::magit::proclog::snapshot();
    bool found = false;
    for (const auto &e : s)
        if (e.kind == '$' && e.command.find("git commit") != std::string::npos) {
            CHECK_FALSE(e.ok);
            CHECK(e.output.find("REJECTED_BY_HOOK") != std::string::npos);
            found = true;
        }
    CHECK(found);
    fs::remove_all(dir);
}
```

Run the case. Expected: PASS (the helper already captures combined stdout+stderr and returns non-zero on hook failure).

- [ ] **Step 5: Write the failing network ($ terminal) test, then wire it**

Add to `tests/test_bridge.cpp`:

```cpp
TEST_CASE("a CLI push logs a $ entry with the terminal-output note")
{
    // make_repo_full has a commit; create a bare remote and wire origin.
    auto dir = make_repo_full();
    auto bare = make_temp_dir();
    REQUIRE(std::system(("git init --bare '" + bare.string() + "' >/dev/null 2>&1").c_str()) == 0);
    REQUIRE(std::system(("git -C '" + dir.string() + "' remote add origin '" + bare.string() + "' >/dev/null 2>&1").c_str()) == 0);

    mg::magit::proclog::clear();
    mg_magit_push_cli(dir.string().c_str(), /*force=*/0, /*set_upstream=*/1);

    auto s = mg::magit::proclog::snapshot();
    bool found = false;
    for (const auto &e : s)
        if (e.kind == '$' && e.command.find("git push") != std::string::npos) {
            CHECK(e.output.find("output shown in terminal") != std::string::npos);
            found = true;
        }
    CHECK(found);
    fs::remove_all(dir);
    fs::remove_all(bare);
}
```

Run: expect FAIL (push doesn't log yet).

Then in `src/magit/bridge.cpp`, add `import mg.magit.proclog;` at the top if not already present (Task 1 added it), and in each of `mg_magit_fetch_cli`/`mg_magit_push_cli`/`mg_magit_pull_cli`, capture the argv and log after `git_terminal` returns. For `mg_magit_push_cli`:

```cpp
extern "C" int mg_magit_push_cli(const char *repo_path, int force,
                                 int set_upstream)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"push"};
    if (force)
        args.emplace_back("--force-with-lease");
    if (set_upstream)
        args.emplace_back("-u");
    args.emplace_back("origin");
    args.emplace_back("HEAD");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}
```

Apply the same shape to `mg_magit_fetch_cli` (`args = {"fetch","origin"}`) and `mg_magit_pull_cli` (build `cmd` from `args` before `std::move`). Run the test. Expected: PASS.

- [ ] **Step 6: Full suite + OFF build, then commit**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` (expect all pass).
Run the OFF build (proclog is Zig-independent so it must compile):
`cmake --build build-tsan --target test_bridge test_proclog && ./build-tsan/tests/test_bridge` (expect pass).

```bash
git add src/magit/git.cppm src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(proclog): log real git subprocesses ($ entries)"
```

---

### Task 3: `≈` libgit2 wiring — mechanical mutations

**Files:**
- Modify: `src/magit/bridge.cpp` (add a `proc_eq` helper; append in each mutating libgit2 wrapper)
- Modify: `tests/test_bridge.cpp` (`≈` entry tests + the reads-excluded assertion)

**Interfaces:**
- Consumes: `mg::magit::proclog::record` (Task 1).
- Adds a file-local helper in `bridge.cpp`:
  `static void proc_eq(const std::string &cmd, bool ok) { mg::magit::proclog::record('~', cmd, "", ok, 0); }`

The equivalent command per wrapper (append `proc_eq(...)` just before each `return`, using the wrapper's already-computed success bool):

| Wrapper | Equivalent command string |
| --- | --- |
| `mg_magit_stage(path)` | `"git add " + path` |
| `mg_magit_unstage(path)` | `"git reset HEAD -- " + path` |
| `mg_magit_stage_all` | `"git add -A"` |
| `mg_magit_unstage_all` | `"git reset HEAD"` |
| `mg_magit_discard(path)` | `"git checkout -- " + path` |
| `mg_magit_reset(rev,mode)` | `"git reset --<soft\|mixed\|hard> " + rev` |
| `mg_magit_branch_create(name,...)` | `"git branch " + name` |
| `mg_magit_branch_delete(name)` | `"git branch -D " + name` |
| `mg_magit_branch_rename(old,new)` | `"git branch -m " + old + " " + new` |
| `mg_magit_checkout(rev)` | `"git checkout " + rev` |
| `mg_magit_tag_create(name,...)` | `"git tag " + name` |
| `mg_magit_tag_delete(name)` | `"git tag -d " + name` |
| `mg_magit_stash_push` | `"git stash push"` |
| `mg_magit_stash_pop` | `"git stash pop"` |
| `mg_magit_stash_apply` | `"git stash apply"` |
| `mg_magit_stash_drop` | `"git stash drop"` |
| `mg_magit_ignore(path)` | `"git check-ignore / echo >> .gitignore " + path` → use `"echo " + path + " >> .gitignore"` |
| `mg_magit_note_set` / `_remove` | `"git notes add"` / `"git notes remove"` |
| `mg_magit_bisect_start/good/bad/reset` | `"git bisect start"` / `"… good"` / `"… bad"` / `"… reset"` |
| rebase actions (`mg_magit_rebase`, `_continue`, `_skip`, `_abort`, `_interactive`) | `"git rebase <args>"` / `"git rebase --continue"` / `--skip` / `--abort` — **one entry per user action**, never per replayed commit |

> DRY note: only add `proc_eq` to the **mutating** wrappers above. Do NOT add it
> to `stage_region`/`unstage_region`/`discard_region`/`stage_hunk`/`unstage_hunk`
> if reusing those is awkward — they ultimately call the same libgit2 staging;
> log them as `"git add -p " + path` / `"git reset -p " + path` if
> straightforward, else defer (note the omission in the commit message). Never
> add it to a read-only wrapper.

- [ ] **Step 1: Write the failing `≈` + reads-excluded tests**

Add to `tests/test_bridge.cpp`:

```cpp
TEST_CASE("staging logs a ≈ git add entry; status reads log nothing")
{
    auto dir = make_repo_with_changes(); // untracked.txt present
    mg::magit::proclog::clear();

    REQUIRE(mg_magit_stage(dir.string().c_str(), "untracked.txt") == 1);

    auto after_stage = mg::magit::proclog::snapshot();
    bool found = false;
    for (const auto &e : after_stage)
        if (e.kind == '~' && e.command == "git add untracked.txt") {
            CHECK(e.ok);
            found = true;
        }
    CHECK(found);

    // A read must NOT add an entry.
    std::size_t before = mg::magit::proclog::snapshot().size();
    std::vector<std::string> sink;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *c, const char *l, int, const char *, int) {
            static_cast<std::vector<std::string> *>(c)->emplace_back(l);
        },
        &sink);
    CHECK(mg::magit::proclog::snapshot().size() == before);

    fs::remove_all(dir);
}

TEST_CASE("tag creation logs a ≈ git tag entry")
{
    auto dir = make_repo_full(); // has a commit to tag
    mg::magit::proclog::clear();
    mg_magit_tag_create(dir.string().c_str(), "v0.1", nullptr); // confirm arity in bridge.h
    auto s = mg::magit::proclog::snapshot();
    bool found = false;
    for (const auto &e : s)
        if (e.kind == '~' && e.command.find("git tag v0.1") != std::string::npos)
            found = true;
    CHECK(found);
    fs::remove_all(dir);
}
```

Run both. Expected: FAIL — no `≈` entries; the reads-excluded check passes vacuously (still good).

- [ ] **Step 2: Add the helper + wire the wrappers**

In `src/magit/bridge.cpp`, add the helper near the top of the anonymous namespace (after `state_word`):

```cpp
// Record an in-process libgit2 mutation as its equivalent git command.
void proc_eq(const std::string &cmd, bool ok)
{
    mg::magit::proclog::record('~', cmd, "", ok, 0);
}
```

Then edit each mutating wrapper per the table. Example for `mg_magit_stage`:

```cpp
extern "C" int mg_magit_stage(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    bool ok = mg::git::stage(repo_path, path).has_value();
    proc_eq(std::string("git add ") + path, ok);
    return ok ? 1 : 0;
}
```

Confirm each wrapper's success expression and argument names against the actual `bridge.cpp` source before editing; keep the existing return semantics unchanged.

- [ ] **Step 3: Run the `≈` tests (verify PASS)**

Run: `cmake --build build --target test_bridge && ./build/tests/test_bridge -tc="staging logs a ≈ git add entry; status reads log nothing" -tc="tag creation logs a ≈ git tag entry"`
Expected: PASS.

- [ ] **Step 4: Full suite + OFF build, then commit**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` (all pass).
Run: `cmake --build build-tsan --target test_bridge && ./build-tsan/tests/test_bridge` (OFF build passes).

```bash
git add src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(proclog): log libgit2 mutations (≈ entries); exclude reads"
```

---

### Task 4: Editor command + `*magit-process*` buffer + `$` binding

**Files:**
- Modify: `src/magit_cmd.c` (forward decl + `PF`; emit callback; `magit_process` command; minimal `magit-process-mode` keymap; bind `$` in `magitmap`)
- Modify: `tests/test_editor.cpp` (pty test: `$` opens the buffer)

**Interfaces:**
- Consumes: `mg_magit_process_log` + `MG_LINE_PROCESS_*` (Task 1).
- Mirrors the existing read-only emit→buffer idiom: `bfind(name, TRUE)` → set `b_flag |= BFIGNDIRTY | BFREADONLY` → emit lines via `addlinef(bp, "%s", line)` (see the `magit_log_emit` callback near `src/magit_cmd.c:1323`) → `popbuf(bp, WNONE)`. Mirror the **`magit-commit-view-mode`** registration (the existing "q closes a read-only buffer" mode) for `magit-process-mode`.

- [ ] **Step 1: Write the failing pty editor test**

Add to `tests/test_editor.cpp` (it already has `make_repo`, `forkpty`, `wait_for`). The robust assertion uses the empty-state line, so no cursor navigation is needed:

```cpp
TEST_CASE("$ opens the *magit-process* buffer from magit-status")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }

    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        const char ms[] = "\x1bxmagit-status\r"; // M-x magit-status RET
        (void)!::write(master, ms, sizeof ms - 1);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            const char dollar[] = "$";  // open the process buffer
            (void)!::write(master, dollar, 1);
            // empty-state line proves the buffer rendered (no ops logged yet)
            ok = wait_for(master, "No git operations", std::chrono::seconds(8));
        }
    }

    const char quit[] = "\x18\x03"; // C-x C-c
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}
```

Run: `cmake --build build --target test_editor && ./build/tests/test_editor -tc="\$ opens the *magit-process* buffer from magit-status"`
Expected: FAIL — `$` is unbound, so the buffer never opens.

- [ ] **Step 2: Add the emit callback + command**

In `src/magit_cmd.c`, near the other forward decls (around line 128) add:
```c
static int	magit_process(int, int);
```
Near the other `PF` arrays (around line 283) add:
```c
static PF magit_dollar[] = { magit_process };
```
Add the emit callback + command (mirror `magit_log_emit` at ~1323 and the buffer-build idiom):

```c
static void
magit_process_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
	(void)kind; (void)path; (void)hunk;
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

static int
magit_process(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if ((bp = bfind("*magit-process*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	(void)mg_magit_process_log(magit_process_emit, bp);
	bp->b_flag |= BFREADONLY;
	/* attach the minimal process-mode keymap (q closes) */
	bp->b_modes[0] = name_mode("fundamental");
	bp->b_nmodes = 0;
	(void)magit_process_mode(bp);    /* defined in Step 3 */
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp; curwp = wp;
	(void)gotobob(f, n);             /* top: newest entry first */
	return (TRUE);
}
```

> The exact buffer/mode-attach calls (`bclear`, `name_mode`, `b_modes`, the
> mode-enable function) must match how `magit-commit-view-mode` is attached in
> this file — read that mode's command (it opens a read-only `*magit-commit*`
> buffer) and mirror its boilerplate precisely instead of the sketch above.

- [ ] **Step 3: Register the minimal `magit-process-mode` (q closes), mirror `magit-commit-view-mode`**

Read the `magit-commit-view-mode` definition (its `KEYMAPE`, its `*_mode` enable function, and its registration in the mode table). Duplicate it as `magit-process-mode`: a tiny keymap binding `q` to the existing close function used by `*magit-commit*` (the read-only-buffer quit). Reuse the same quit `PF`. Add the mode-enable function `magit_process_mode(struct buffer *)` referenced in Step 2.

- [ ] **Step 4: Bind `$` in the status keymap**

In `src/magit_cmd.c`, the `magitmap` keymap (`struct KEYMAPE (31) magitmap = { 31, 31, rescan, { … } }`). Three edits:
1. `KEYMAPE (31)` → `KEYMAPE (32)`
2. `{ 31, 31, rescan,` → `{ 32, 32, rescan,` (wait: the literal is `\t31,\n\t31,\n\trescan,` — change BOTH `31` count fields to `32`)
3. Insert, in ascending key order — `$` is `0x24`, which sorts **after** the `CCHR('[')`/ESC entry (`0x1B`) and **before** `'?'` (`0x3F`):

```c
		{ CCHR('['), CCHR('['), magit_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ '$', '$', magit_dollar, NULL },		/* $: process log */
		{ '?', '?', magit_qmark, NULL },		/* ?: key help */
```

(Entries MUST stay ascending — `doscan()` relies on it. `$`=0x24 < `?`=0x3F is correct.)

- [ ] **Step 5: Run the pty test (verify PASS)**

Run: `cmake --build build --target test_editor && ./build/tests/test_editor -tc="\$ opens the *magit-process* buffer from magit-status"`
Expected: PASS.

- [ ] **Step 6: Full suite + commit**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` (all pass).

```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(proclog): \$ opens *magit-process* buffer in magit-status"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure` — all green.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-proclog .` — builds (runs ctest as a build step).
- [ ] OFF build: `cmake --build build-tsan && ctest --test-dir build-tsan -R "proclog|bridge"` — proclog compiles/passes with `ENABLE_ZIG_STATUS=OFF`.
- [ ] Manual smoke: open neomg in a repo, stage a file, press `$` → `*magit-process*` shows `≈ git add <file>`; run a commit → `$ git commit …` with output.
- [ ] Open the stacked PR into `neomg` (per the auto-pr-per-milestone workflow).
