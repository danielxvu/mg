# FM-ZIG-READ-ENGINE Phase 2 (Hybrid Integration) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make neomg's **cold full-status** ~2–2.4× faster by computing the worktree dimension with the Phase-1a Zig walker (FFI'd as a static lib) and the staged (index-vs-HEAD) dimension with libgit2 — merged into the same `file_status` records, byte-identical to today's libgit2 status, with a libgit2 full-status fallback. Gated by `ENABLE_ZIG_STATUS` (default OFF).

**Architecture:** A C-ABI Zig static lib (`libneomg_zig.a`) emits the worktree dimension (Y: modified/deleted/untracked) via a callback. A new C++ `hybrid_status(repo)` calls it, computes the staged dimension (X) with libgit2 `git_diff_tree_to_index(HEAD-tree, index)`, merges per path into `std::vector<mg::magit::file_status>`, and falls back to the existing libgit2 `repo_status` if the Zig side returns the "unsupported" sentinel. CMake builds + links the Zig lib only when `ENABLE_ZIG_STATUS=ON`.

**Tech Stack:** Zig 0.16 (`/opt/local/bin/zig`; static lib via `zig build`), C++23 module `mg.git` + `bridge.cpp`, libgit2, CMake (custom command driving zig), doctest, the existing `bench/` harness.

## Global Constraints

- **`ENABLE_ZIG_STATUS` defaults OFF.** The default `cpp` and `c-legacy` builds must be **unaffected** (C++ suite still 206/206; no `neomg_zig`/Zig symbols linked; no Zig required to build). Only `-DENABLE_ZIG_STATUS=ON` pulls in the Zig lib.
- **Correctness floor:** the hybrid status MUST be byte-identical to libgit2's `mg::git::repo_status` (same set of `{path, index_status, worktree_status}`) on the validation corpus. On ANY Zig-side error or unsupported state (sentinel `-1`), fall back to libgit2 `repo_status` — so neomg is **never less correct than today**.
- **Zig 0.16.0** (`/opt/local/bin/zig`); std at `/opt/local/lib/zig/std/` is authoritative (read it — `main(init: std.process.Init)` for the exe; for the lib export use `export fn … callconv(.c)`; unmanaged `ArrayList`; `std.Io` fs). The Phase-1a walker lives in `src/zigstatus/src/` and already does the worktree dimension + the validation harness (`test/validate.py`) — reuse it; don't reimplement.
- **Scope:** worktree dimension from Zig, staged (X) from libgit2, **no object-DB reimplementation** (the spec's hybrid decision). Warm incremental path (`status_scoped`) stays libgit2 — untouched. Writes stay on the git CLI.
- Build (ON): `cmake --preset cpp -DENABLE_ZIG_STATUS=ON && cmake --build --preset cpp`. Tests: `ctest --preset cpp`. OFF check: `cmake --preset cpp` (default) builds with no Zig.
- Commit trailer on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp
  ```

---

## File Structure

- `src/zigstatus/src/lib.zig` — the C-ABI export `neomg_zig_worktree_status` (thin wrapper over the existing `status.run`).
- `src/zigstatus/include/neomg_zig.h` — C header declaring the export + the emit callback typedef.
- `src/zigstatus/build.zig` — add a static-library artifact (`neomg_zig`) alongside the existing exe.
- `src/magit/git.cppm` — `staged_status` (libgit2 tree-to-index, the X column) + `hybrid_status` (merge + fallback), under `mg::git`.
- `src/magit/bridge.cpp` / wiring — the cold-status path uses `hybrid_status` when compiled with `MG_ZIG_STATUS`.
- `CMakeLists.txt` + `src/magit/CMakeLists.txt` — `ENABLE_ZIG_STATUS` option; custom command building the Zig lib; link + `MG_ZIG_STATUS` define.
- `tests/test_git.cpp` — staged-column test + the hybrid==libgit2 equivalence corpus.

---

## Task 1: Zig C-ABI worktree-status export + static lib

**Files:**
- Create: `src/zigstatus/src/lib.zig`, `src/zigstatus/include/neomg_zig.h`
- Modify: `src/zigstatus/build.zig`, `src/zigstatus/src/main.zig` (dogfood the export)

**Interfaces:**
- Produces (C ABI):
  ```c
  // neomg_zig.h
  typedef void (*neomg_zig_emit_fn)(void *ctx, const char *path, size_t path_len,
                                    char x, char y);
  // Emits the WORKTREE dimension (y = ' '/'M'/'D'/'?'; x always ' ' here).
  // Returns 0 on success, -1 if the repo/index is unsupported (caller falls back).
  int neomg_zig_worktree_status(const char *repo, size_t repo_len,
                                neomg_zig_emit_fn emit, void *ctx);
  ```

- [ ] **Step 1: Write the export (`lib.zig`)**

`src/zigstatus/src/lib.zig`:
```zig
const std = @import("std");
const status = @import("status.zig");
const walk = @import("walk.zig");

const CEmit = *const fn (ctx: ?*anyopaque, path: [*]const u8, path_len: usize, x: u8, y: u8) callconv(.c) void;
const Bridge = struct { cemit: CEmit, cctx: ?*anyopaque };

fn zigEmit(ctx: *anyopaque, x: u8, y: u8, path: []const u8) void {
    const b: *Bridge = @ptrCast(@alignCast(ctx));
    b.cemit(b.cctx, path.ptr, path.len, x, y);
}

export fn neomg_zig_worktree_status(repo: [*]const u8, repo_len: usize, emit: CEmit, ctx: ?*anyopaque) callconv(.c) c_int {
    var threaded = std.Io.Threaded.init(std.heap.c_allocator, .{});
    defer threaded.deinit();
    const io = threaded.io();
    var bridge = Bridge{ .cemit = emit, .cctx = ctx };
    status.run(io, std.heap.c_allocator, repo[0..repo_len], zigEmit, &bridge) catch return -1;
    return 0;
}
```
(Confirm against `status.zig`: `run(io, gpa, repo, emit, ctx)` and the `EmitFn` signature `fn(ctx:*anyopaque, x:u8, y:u8, path:[]const u8) void`. Adjust the wrapper to match exactly. The lib needs its OWN Threaded/io because there's no `std.process.Init` here — verify `std.Io.Threaded.init(gpa, .{})` is the right 0.16 call.)

- [ ] **Step 2: Header**

`src/zigstatus/include/neomg_zig.h`:
```c
#ifndef NEOMG_ZIG_H
#define NEOMG_ZIG_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*neomg_zig_emit_fn)(void *ctx, const char *path, size_t path_len, char x, char y);
int neomg_zig_worktree_status(const char *repo, size_t repo_len, neomg_zig_emit_fn emit, void *ctx);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: build.zig — add the static lib**

In `src/zigstatus/build.zig`, after the exe, add:
```zig
const lib = b.addLibrary(.{
    .name = "neomg_zig",
    .linkage = .static,
    .root_module = b.createModule(.{
        .root_source_file = b.path("src/lib.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    }),
});
b.installArtifact(lib);
```
(Verify `b.addLibrary` / `.linkage = .static` against the 0.16 `std.Build` API — the exact builder API may differ; read `/opt/local/lib/zig/std/Build.zig`. The install puts it at `zig-out/lib/libneomg_zig.a`.)

- [ ] **Step 4: Dogfood — main.zig calls the export, harness still green**

Change `src/zigstatus/src/main.zig` to call `neomg_zig_worktree_status` (via `@import("lib.zig")`) with a C-style emit thunk that formats `XY path`, instead of calling `status.run` directly. This proves the export path works.

Run: `cd src/zigstatus && /opt/local/bin/zig build -Doptimize=ReleaseFast && python3 test/validate.py`
Expected: 8/8 `ok` (the export path produces identical output) AND `ls zig-out/lib/libneomg_zig.a` exists.

- [ ] **Step 5: Commit**
```bash
git add src/zigstatus
git commit -m "feat(zigstatus): C-ABI neomg_zig_worktree_status + static lib (FM-ZIG P2)\n\n<trailer>"
```

---

## Task 2: CMake builds + links the Zig static lib (gated, default OFF)

**Files:** Modify `CMakeLists.txt` (the `ENABLE_ZIG_STATUS` option + find zig), `src/magit/CMakeLists.txt` (custom command + link + define).

**Interfaces:** When `ENABLE_ZIG_STATUS=ON`, `mg_magit` links `libneomg_zig.a`, the compile define `MG_ZIG_STATUS` is set, and `neomg_zig.h` is on the include path. OFF → none of this; default build unchanged.

- [ ] **Step 1: Add the option + locate zig (root `CMakeLists.txt`)**
```cmake
option(ENABLE_ZIG_STATUS "Use the Zig worktree-status walker for cold status (needs zig 0.16)" OFF)
if(ENABLE_ZIG_STATUS)
  find_program(ZIG_EXE zig HINTS /opt/local/bin /opt/homebrew/bin /usr/local/bin REQUIRED)
  message(STATUS "ENABLE_ZIG_STATUS: building the Zig status lib with ${ZIG_EXE}")
endif()
```

- [ ] **Step 2: Build the Zig lib via a custom command + link it (`src/magit/CMakeLists.txt`)**
```cmake
if(ENABLE_ZIG_STATUS)
    set(ZIG_DIR ${CMAKE_SOURCE_DIR}/src/zigstatus)
    set(ZIG_LIB ${ZIG_DIR}/zig-out/lib/libneomg_zig.a)
    add_custom_command(
        OUTPUT ${ZIG_LIB}
        COMMAND ${ZIG_EXE} build -Doptimize=ReleaseFast
        WORKING_DIRECTORY ${ZIG_DIR}
        DEPENDS ${ZIG_DIR}/src/lib.zig ${ZIG_DIR}/src/status.zig
                ${ZIG_DIR}/src/walk.zig ${ZIG_DIR}/src/index.zig
                ${ZIG_DIR}/src/gitignore.zig ${ZIG_DIR}/src/sha1.zig
                ${ZIG_DIR}/build.zig
        COMMENT "Building Zig status lib (libneomg_zig.a)")
    add_custom_target(neomg_zig_lib DEPENDS ${ZIG_LIB})
    add_dependencies(mg_magit neomg_zig_lib)
    target_link_libraries(mg_magit PRIVATE ${ZIG_LIB})
    target_include_directories(mg_magit PRIVATE ${ZIG_DIR}/include)
    target_compile_definitions(mg_magit PRIVATE MG_ZIG_STATUS)
endif()
```
(The Zig static lib also needs libc symbols it referenced; on macOS linking `libneomg_zig.a` into the C++ target may need `-lc` already present — verify the link succeeds; if undefined Zig-runtime symbols appear, link the zig compiler-rt or add the needed system libs. Discover the exact link requirements here.)

- [ ] **Step 3: Verify ON builds + OFF is unaffected**

Run (ON): `cmake --preset cpp -DENABLE_ZIG_STATUS=ON 2>&1 | tail -3 && cmake --build --preset cpp 2>&1 | tail -3` → builds, links `libneomg_zig.a`.
Run (OFF/default): `rm -rf build && cmake --preset cpp && cmake --build --preset cpp && ctest --preset cpp 2>&1 | tail -2` → 206/206, and `nm build/src/mg 2>/dev/null | grep -c neomg_zig` → 0.

- [ ] **Step 4: Commit** `build: ENABLE_ZIG_STATUS option builds+links the Zig status lib (FM-ZIG P2)`.

---

## Task 3: libgit2 staged (X) column

**Files:** Modify `src/magit/git.cppm` (declaration ~ near `repo_status`; definition near it), `tests/test_git.cpp`.

**Interfaces:**
- Produces: `mg::git::staged_status(std::string repo) -> std::expected<std::vector<staged_entry>, error>` where `staged_entry { std::string path; mg::magit::status x; }` — the index-vs-HEAD status per changed path (added/modified/deleted/renamed staged). On an unborn HEAD (no commit), every index entry is staged-added.

- [ ] **Step 1: Write the failing test**

In `tests/test_git.cpp`:
```cpp
TEST_CASE("staged_status reports index-vs-HEAD changes (X column)")
{
    auto dir = make_repo_with_commit("base"); // a.txt committed
    set_test_config(dir);
    // stage a modify + a new file
    std::ofstream(dir / "a.txt") << "changed";
    std::ofstream(dir / "b.txt") << "new";
    REQUIRE(mg::git::stage(dir.string(), "a.txt").has_value());
    REQUIRE(mg::git::stage(dir.string(), "b.txt").has_value());

    auto s = mg::git::staged_status(dir.string());
    REQUIRE(s.has_value());
    std::map<std::string, mg::magit::status> x;
    for (auto &e : *s) x[e.path] = e.x;
    CHECK(x["a.txt"] == mg::magit::status::modified);
    CHECK(x["b.txt"] == mg::magit::status::added);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run — verify FAIL** (`staged_status` undefined). `cmake --build --preset cpp && ./build/tests/test_git --test-case="staged_status*"`.

- [ ] **Step 3: Implement via `git_diff_tree_to_index`**

In `mg::git`, declare `staged_entry`/`staged_status`; implement: open repo; resolve HEAD → commit → tree (NULL tree if unborn); `git_repository_index`; `git_diff_tree_to_index(&diff, repo, head_tree, index, &opts)`; iterate `git_diff_num_deltas` → map `GIT_DELTA_ADDED→added`, `MODIFIED→modified`, `DELETED→deleted`, `RENAMED→renamed`, using `delta->new_file.path`. Reuse the existing RAII wrappers (`detail::repo_ptr`, `tree_ptr`, `index_ptr`, `diff_ptr` if present; else add a `git_diff` deleter). Return the vector.

- [ ] **Step 4: Run — verify PASS.** `./build/tests/test_git --test-case="staged_status*"` → PASS.

- [ ] **Step 5: Commit** `feat(magit): staged_status — index-vs-HEAD X column via libgit2 (FM-ZIG P2)`.

---

## Task 4: Hybrid merge + fallback (the equivalence spine)

**Files:** Modify `src/magit/git.cppm` (`hybrid_status`) + `src/magit/bridge.cpp` (the C++ emit thunk calling the Zig lib), `tests/test_git.cpp`.

**Interfaces:**
- Consumes: `neomg_zig_worktree_status` (Task 1, via `neomg_zig.h`, only when `MG_ZIG_STATUS`), `staged_status` (Task 3), the existing `repo_status` (fallback).
- Produces: `mg::git::hybrid_status(std::string repo) -> std::expected<std::vector<mg::magit::file_status>, error>` — identical shape to `repo_status`. When `MG_ZIG_STATUS` is undefined, `hybrid_status` simply calls `repo_status` (so the merge code is only compiled with the Zig lib).

- [ ] **Step 1: Write the failing equivalence test**

In `tests/test_git.cpp` (a helper that compares two status vectors as sets of `{path,x,y}`):
```cpp
static std::set<std::tuple<std::string,int,int>> status_set(const std::vector<mg::magit::file_status>& v){
    std::set<std::tuple<std::string,int,int>> s;
    for (auto& f : v) s.insert({f.path, (int)f.index_status, (int)f.worktree_status});
    return s;
}
TEST_CASE("hybrid_status == libgit2 repo_status (byte-identical) on a mixed tree")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    std::ofstream(dir / "a.txt") << "worktree-mod";          // ' M'
    std::ofstream(dir / "b.txt") << "staged-new"; mg::git::stage(dir.string(),"b.txt"); // 'A '
    fs::remove(dir / /* a committed file */ "a.txt"); // adjust to your fixture
    std::ofstream(dir / "u.txt") << "untracked";             // '??'
    auto h = mg::git::hybrid_status(dir.string());
    auto g = mg::git::repo_status(dir.string());
    REQUIRE(h.has_value()); REQUIRE(g.has_value());
    CHECK(status_set(*h) == status_set(*g));
    fs::remove_all(dir);
}
```
(Build the fixture so it exercises X-only, Y-only, both, and untracked. `repo_status` returns `file_status` with `index_status`/`worktree_status` — confirm those field names in `mg.magit`.)

- [ ] **Step 2: Run — verify FAIL** (`hybrid_status` undefined). Build with `-DENABLE_ZIG_STATUS=ON`.

- [ ] **Step 3: Implement the merge + fallback**

`hybrid_status(repo)`:
```cpp
#ifdef MG_ZIG_STATUS
  // 1. Zig worktree dimension -> map path -> file_status{x=unmodified, y}
  std::map<std::string, mg::magit::file_status> by_path;
  struct Cap { std::map<std::string, mg::magit::file_status>* m; };
  Cap cap{&by_path};
  int rc = neomg_zig_worktree_status(repo.c_str(), repo.size(),
      [](void* ctx, const char* p, size_t n, char x, char y){
          auto* c = static_cast<Cap*>(ctx);
          auto& f = (*c->m)[std::string(p,n)];
          f.path = std::string(p,n);
          f.worktree_status = map_xy(y); // ' '->unmodified,'M'->modified,'D'->deleted,'?'->untracked
      }, &cap);
  if (rc != 0) return repo_status(repo); // unsupported -> libgit2 fallback
  // 2. libgit2 staged (X) column -> set f.index_status
  auto st = staged_status(repo);
  if (!st) return repo_status(repo);
  for (auto& e : *st) { auto& f = by_path[e.path]; f.path = e.path; f.index_status = e.x; }
  // 3. flatten
  std::vector<mg::magit::file_status> out;
  for (auto& [_, f] : by_path) out.push_back(f);
  return out;
#else
  return repo_status(repo);
#endif
```
(Adjust `file_status` field names + the `map_xy` mapping to the actual `mg::magit::status` enum. The `bridge.cpp` thunk lives there because `neomg_zig.h` is C; including it in the module `git.cppm` may need it in the global fragment — if the module can't include the C header cleanly, put the Zig call in `bridge.cpp` and have `hybrid_status` take the worktree records as a parameter. Discover the cleanest split; the test pins correctness either way.)

- [ ] **Step 4: Run — verify PASS, then expand the corpus**

Run: `cmake --build --preset cpp (-DENABLE_ZIG_STATUS=ON) && ./build/tests/test_git --test-case="hybrid_status*"` → PASS. Then add corpus variants (clean repo; only-staged; only-worktree; deleted-staged-and-worktree; nested dirs; ignored files present) as additional cases, each asserting `hybrid == repo_status`. All green.

- [ ] **Step 5: Commit** `feat(magit): hybrid_status — Zig worktree + libgit2 staged, == libgit2 (FM-ZIG P2)`.

---

## Task 5: Wire into the cold path + benchmark + Linux CI

**Files:** Modify `src/magit/bridge.cpp` (cold `full_refresh` / the status-buffer gather uses `hybrid_status` when `MG_ZIG_STATUS`), `.github/workflows/` (a Zig-on-Linux job), `bench/` + `todo.md`/`README`.

- [ ] **Step 1: Route the cold path through the hybrid**

In the bridge's cold full-status gather (where `repo_status` / `session::status()` is called for a full refresh — NOT `status_scoped`, the warm path), call `hybrid_status` when compiled with `MG_ZIG_STATUS` (it falls back internally). The warm incremental `status_scoped` stays libgit2. Confirm the magit status buffer is byte-identical with the option ON vs OFF (the existing status tests + a manual diff).

- [ ] **Step 2: Verify the full suite both ways**

Run (ON): `cmake --preset cpp -DENABLE_ZIG_STATUS=ON && cmake --build --preset cpp && ctest --preset cpp 2>&1 | tail -2` → all pass (the status-buffer/bridge tests now exercise the hybrid).
Run (OFF): default build → 206/206, 0 zig symbols.

- [ ] **Step 3: Benchmark the cold win end-to-end**

Extend `bench/` (or a one-off) to time the bridge cold status (`mg_magit_status_buffer`) with `ENABLE_ZIG_STATUS` ON vs OFF on roll20/d20app; confirm the ON path is ~1.8–2.4× faster cold and produces identical output. Record the numbers.

- [ ] **Step 4: Linux CI builds the Zig lib**

Add a CI job (extend `.github/workflows/bench.yml` or `linux.yml`): on Arch x86_64, install `zig`, configure with `-DENABLE_ZIG_STATUS=ON`, build, run `ctest` — so the hybrid path is built+tested on Linux, not just macOS. (Pin/locate the distro's zig 0.16; if unavailable, install via the same mechanism as local.)

- [ ] **Step 5: Docs + commit**

Update `src/zigstatus/README.md` (now FFI'd, hybrid model) + the `todo.md` FM-ZIG entry (Phase 2 done: hybrid cold status, Nx faster, byte-identical, default OFF). Commit `feat(magit): route cold status through the Zig hybrid (ENABLE_ZIG_STATUS) + CI/bench (FM-ZIG P2)`.

---

## Self-Review notes

- **Spec coverage:** Zig C-ABI lib (T1) + CMake gate (T2) = the spec's "C-ABI static lib + CMake ENABLE_ZIG_STATUS"; libgit2 staged column (T3) + hybrid merge with libgit2 fallback (T4) = the spec's hybrid decision (Zig worktree + libgit2 staged, fallback on unsupported); cold-path wiring + benchmark + Linux CI (T5) = the spec's Phase 3 items folded in. The warm path stays libgit2 (Global Constraints). Object-DB reimplementation is explicitly NOT in scope (spec hybrid decision).
- **Correctness floor enforced by T4's equivalence test** (`hybrid == repo_status` as `{path,x,y}` sets) — the spine; every fallback returns `repo_status` verbatim.
- **Default-OFF safety:** T2 step 3 + T5 step 2 both assert the default build is unchanged (206/206, 0 zig symbols, no Zig needed).
- **Type consistency:** `neomg_zig_worktree_status(repo,len,emit,ctx)->int` + `neomg_zig_emit_fn(ctx,path,len,x,y)` identical in T1 header / T4 call; `staged_entry{path,x:mg::magit::status}` T3 used in T4; `hybrid_status(repo)->expected<vector<file_status>,error>` T4 used in T5. Confirm `mg::magit::file_status` field names (`index_status`/`worktree_status`) against `magit.cppm` in T3/T4 — flagged as a discovery point.
- **Discovery points flagged (not placeholders):** the 0.16 `b.addLibrary` builder API (T1.3), the CMake↔zig static-lib link requirements incl. Zig-runtime/libc symbols (T2.2), and the C-header-in-module vs bridge.cpp split (T4.3). Each is validated by a concrete build/test step, not assumed.
