# FM-DIFF-CTL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In `*magit-status*`, `+`/`-` grow/shrink expanded-diff context lines and `w` toggles ignore-whitespace; settings persist + re-render, and stay correct vs. hunk staging.

**Architecture:** A process-global diff-view config in `mg.git` (context + ignore-ws), applied at **every hunk-numbering diff-options site** (the display `file_diff` AND the shared staging helper `path_scoped_diff_opts`) so display and staging indices stay aligned. `-w` is display-only; hunk staging is disabled while it's on. Bridge setter + a `-2` sentinel on the staging C ABIs; editor keys + state + header indicator.

**Tech Stack:** libgit2 `git_diff_options` (`context_lines`, `GIT_DIFF_IGNORE_WHITESPACE`); the mg C editor (keymaps, `magit_refresh`).

## Global Constraints

- The view config is read on the main thread (status build + staging are synchronous editor commands); a plain module global is sufficient (the background monitor never calls `file_diff`).
- **Context is applied at every hunk-numbering site** — `file_diff` (display) AND `path_scoped_diff_opts` (the shared helper for all 5 staging ops: `stage_hunk`/`unstage_hunk`/`stage_region`/`unstage_region`/`discard_region`) — so a context change never desyncs displayed hunk indices from staging. `staged_status` (the status-SET computation) is NOT touched.
- `-w` (`GIT_DIFF_IGNORE_WHITESPACE`) is set **only** for display (`file_diff`); staging ops never ignore whitespace. While `-w` is on, hunk/region staging is disabled (the displayed ws-ignored hunks aren't applyable / don't match the exact-diff staging numbering).
- Context clamp: `[0, 32]`. Default view: context 3, ws significant.
- Staging C ABI sentinel: `-2` = "blocked, ignore-ws is on" (distinct from `0` = failed, `1` = ok).
- **Test hygiene:** the diff-view config is a process-global, so any test that calls `set_diff_view`/`mg_magit_set_diff_view` with non-default values MUST reset to `(3, false)` at the end (and ideally set its expected view at the start) — otherwise it leaks into other `file_diff`-based tests in the same binary (`test_git`/`test_bridge` run many cases in one process).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine diff-view config

**Files:**
- Modify: `src/magit/git.cppm` (globals + `set_diff_view`/getters; apply context in `path_scoped_diff_opts` + `file_diff`; ws in `file_diff`)
- Test: `tests/test_git.cpp`

**Interfaces (produced; Tasks 2-3 consume):**
- `void mg::git::set_diff_view(int context, bool ignore_ws)` — clamps context to `[0,32]`.
- `int mg::git::diff_view_context()` ; `bool mg::git::diff_view_ignore_ws()`.

- [ ] **Step 1: Write the failing engine test**

Add to `tests/test_git.cpp` (uses `make_repo_with_commit`/`commit_file`/`set_test_config`):

```cpp
TEST_CASE("set_diff_view controls context lines and whitespace in file_diff")
{
    // a committed file, then an unstaged change with far-apart context
    auto dir = make_repo_with_commit("base"); // commits a.txt? use commit_file below
    set_test_config(dir);
    commit_file(dir, "f.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", "add f");
    std::ofstream(dir / "f.txt") << "1\n2\n3\n4\n5\nSIX\n7\n8\n9\n10\n"; // change line 6

    // default context 3: line "10" (4 away from the change) is NOT in the hunk
    mg::git::set_diff_view(3, false);
    auto d3 = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/false);
    REQUIRE(d3.has_value());
    bool has10_ctx3 = false;
    for (auto &h : *d3) for (auto &l : h.lines) if (l.content.find("10") != std::string::npos) has10_ctx3 = true;
    CHECK_FALSE(has10_ctx3);

    // context 6: now line "10" (4 away) IS within the widened context
    mg::git::set_diff_view(6, false);
    auto d6 = mg::git::file_diff(dir.string(), "f.txt", false);
    REQUIRE(d6.has_value());
    bool has10_ctx6 = false;
    for (auto &h : *d6) for (auto &l : h.lines) if (l.content.find("10") != std::string::npos) has10_ctx6 = true;
    CHECK(has10_ctx6);

    CHECK(mg::git::diff_view_context() == 6);
    mg::git::set_diff_view(3, false); // reset for other tests
    fs::remove_all(dir);
}

TEST_CASE("set_diff_view ignore-whitespace hides a whitespace-only change")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    commit_file(dir, "w.txt", "alpha\nbravo\n", "add w");
    std::ofstream(dir / "w.txt") << "alpha \nbravo\n"; // trailing space on line 1 only

    mg::git::set_diff_view(3, true);  // ignore whitespace
    auto d = mg::git::file_diff(dir.string(), "w.txt", false);
    REQUIRE(d.has_value());
    CHECK(d->empty());                // whitespace-only change -> no hunks
    CHECK(mg::git::diff_view_ignore_ws());
    mg::git::set_diff_view(3, false);
    fs::remove_all(dir);
}
```

(Confirm `file_diff`'s hunk/line struct field names against `git.cppm` — the diff returns hunks each with a `lines` vector whose entries have a `content` string; adjust the field access if they differ.)

- [ ] **Step 2: Run → FAIL** (`set_diff_view`/`diff_view_*` undefined).
Run: `cmake --preset cpp && cmake --build build --target test_git`

- [ ] **Step 3: Add the globals + accessors**

In `src/magit/git.cppm`, add module-internal globals near the other engine globals (e.g. by `g_zig_fastpath_taken`), and exported accessors in `export namespace mg::git`:

```cpp
// (module-internal, git.cppm scope) current diff-view config, read by file_diff
// + path_scoped_diff_opts so display and staging share one context.
namespace { int g_diff_context = 3; bool g_diff_ignore_ws = false; }
```
```cpp
// (in export namespace mg::git)
void set_diff_view(int context, bool ignore_ws)
{
    g_diff_context = context < 0 ? 0 : (context > 32 ? 32 : context);
    g_diff_ignore_ws = ignore_ws;
}
int diff_view_context() { return g_diff_context; }
bool diff_view_ignore_ws() { return g_diff_ignore_ws; }
```
Add their declarations to the export-namespace declaration block (near `file_diff`'s decl).

- [ ] **Step 4: Apply context at the two diff sites + ws at display**

In `path_scoped_diff_opts` (git.cppm:3735), after the init + pathspec, add:
```cpp
    opts.context_lines = g_diff_context;   // share the view's context with staging
```
In `file_diff` (git.cppm:~3652), after `git_diff_options_init(&opts, …)`:
```cpp
    opts.context_lines = g_diff_context;
    if (g_diff_ignore_ws)
        opts.flags |= GIT_DIFF_IGNORE_WHITESPACE;
```

- [ ] **Step 5: Run → PASS.** Then full suite `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 6: Commit**
```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(diff): engine diff-view config (context + ignore-ws), shared by display + staging"
```

---

### Task 2: Bridge setter + `-2` staging sentinel + header indicator

**Files:**
- Modify: `src/magit/bridge.h` (decl), `src/magit/bridge.cpp` (setter; `-2` in 5 staging wrappers; header line)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Consumes: `mg::git::set_diff_view`/`diff_view_context`/`diff_view_ignore_ws` (Task 1).
- Produces (Task 3 consumes): `void mg_magit_set_diff_view(int context, int ignore_ws)`; the 5 hunk/region staging C ABIs return `-2` when ignore-ws is on.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_bridge.cpp`:
```cpp
TEST_CASE("staging a hunk is blocked (-2) while ignore-whitespace is on")
{
    auto dir = make_repo_unstaged(); // a tracked file with an unstaged hunk
    mg_magit_set_diff_view(3, /*ignore_ws=*/1);
    CHECK(mg_magit_stage_hunk(dir.string().c_str(), "a.txt", 0) == -2);
    mg_magit_set_diff_view(3, 0);     // ws off -> normal staging (1) or 0
    CHECK(mg_magit_stage_hunk(dir.string().c_str(), "a.txt", 0) != -2);
    mg_magit_set_diff_view(3, 0);
    fs::remove_all(dir);
}

TEST_CASE("status header shows the diff-view indicator when non-default")
{
    auto dir = make_repo_full();
    mg_magit_set_diff_view(6, 1);
    std::vector<std::string> lines;
    mg_magit_status_buffer(dir.string().c_str(), nullptr, 0,
        [](void *c, const char *l, int, const char *, int) {
            static_cast<std::vector<std::string> *>(c)->emplace_back(l); }, &lines);
    bool shown = false;
    for (auto &l : lines) if (l.find("-U6") != std::string::npos && l.find("-w") != std::string::npos) shown = true;
    CHECK(shown);
    mg_magit_set_diff_view(3, 0);     // reset (default -> no indicator)
    fs::remove_all(dir);
}
```
(Confirm the helper that yields an unstaged hunk — `make_repo_unstaged` exists in `test_bridge.cpp`; use its actual changed-file name in place of `a.txt`.)

Run → FAIL (`mg_magit_set_diff_view` undefined; no `-2`; no indicator).

- [ ] **Step 2: Add the C ABI decl + setter**

`src/magit/bridge.h` (near the other action decls):
```c
/* Set the diff-view config for *magit-status* expanded diffs: context lines
 * (clamped 0..32) and ignore-whitespace (0/1). While ignore_ws is on, the
 * hunk/region staging calls return -2 (blocked). */
void mg_magit_set_diff_view(int context, int ignore_ws);
```
`src/magit/bridge.cpp`:
```cpp
extern "C" void mg_magit_set_diff_view(int context, int ignore_ws)
{
    mg::git::set_diff_view(context, ignore_ws != 0);
}
```

- [ ] **Step 3: Add the `-2` sentinel to the 5 staging wrappers**

In each of `mg_magit_stage_hunk`, `mg_magit_unstage_hunk`, `mg_magit_stage_region`, `mg_magit_unstage_region`, `mg_magit_discard_region` (bridge.cpp:1222-1270), after the existing null/arg guards and BEFORE the engine call, add:
```cpp
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
```
(Example for `mg_magit_stage_hunk`:)
```cpp
extern "C" int mg_magit_stage_hunk(const char *repo_path, const char *path, int hunk)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;
    return mg::git::stage_hunk(repo_path, path, hunk).has_value() ? 1 : 0;
}
```

- [ ] **Step 4: Add the header indicator in `compose_status_view`**

In `src/magit/bridge.cpp`, right after the `out("On branch " + …)` head line (bridge.cpp:721), add a `Diff:` line when the view is non-default:
```cpp
    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv = "Diff:     -U" + std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        out(dv);
    }
```
(Match the `out(...)` signature used for the adjacent `Head:`/`Upstream:` lines — likely `out(std::string)` defaulting to `MG_LINE_OTHER`.)

- [ ] **Step 5: Run → PASS.** Full suite + OFF build:
`cmake --build build && ctest --test-dir build --output-on-failure` ; `cmake --build build-tsan --target test_bridge && ./build-tsan/tests/test_bridge`.

- [ ] **Step 6: Commit**
```bash
git add src/magit/bridge.h src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(diff): mg_magit_set_diff_view + -2 staging sentinel + header indicator"
```

---

### Task 3: Editor keys (`+`/`-`/`w`) + staging warn

**Files:**
- Modify: `src/magit_cmd.c` (state; 3 commands + PFs; `magitmap` bindings; `-2` warn at 5 staging sites)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: `mg_magit_set_diff_view` (Task 2); the 5 staging C ABIs' `-2` (Task 2); `magit_refresh`.

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp` (uses `make_repo`, `forkpty`, `wait_for`, `drain`/`drain_str`, `C-x 1`). Build a repo whose tracked file gets a change with far context, expand it, and exercise the keys:

```cpp
TEST_CASE("+ grows diff context; w blocks hunk staging with a warning")
{
    auto repo = make_repo(); // tracked.txt committed
    // overwrite with a change on line 6, lines 1-10, so context controls matter
    std::ofstream(repo / "tracked.txt") << "1\n2\n3\n4\n5\nSIX\n7\n8\n9\nTENLINE\n";
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::chdir(fs::temp_directory_path().c_str());   // monitor-inert determinism
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool grew = false, warned = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);     // C-x 1: only *magit-status*
            // expand the file (TAB on its line) -- navigate to it first
            (void)!::write(master, "\x1b" "3", 2);     // M-3: expand all (hunks)
            (void)!::write(master, "\x0c", 1);
            if (wait_for(master, "SIX", std::chrono::seconds(8))) {
                // grow context: TENLINE (4 lines from the change) appears at -U6
                (void)!::write(master, "+++", 3);       // context 3->6
                (void)!::write(master, "\x0c", 1);
                grew = wait_for(master, "TENLINE", std::chrono::seconds(8));
                // turn on -w, then try to stage a hunk -> warn
                (void)!::write(master, "w", 1);
                (void)!::write(master, "\x0c", 1);
                drain_str(master, std::chrono::milliseconds(1200));
                (void)!::write(master, "s", 1);         // stage hunk at point
                warned = wait_for(master, "Turn off -w", std::chrono::seconds(8));
            }
        }
    }
    const char quit[] = "\x18\x03"; (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(grew);
    CHECK(warned);
}
```
> NOTE: the exact key to land point on a hunk for `s` may need a nav step (e.g. `M-3` expands all so a hunk line exists; `s` acts on the line at point — if point isn't on a hunk, add `C-n`s to reach one). Confirm against how the existing staging pty flow drives `s`. The robust signals are "TENLINE" (context grew) and "Turn off -w" (warn).

Run → FAIL (`+`/`w` unbound).

- [ ] **Step 2: Add state + the 3 commands + PFs**

In `src/magit_cmd.c` near the other state (by `magit_expanded`):
```c
static int	magit_diff_context = 3;   /* current -U<n> for *magit-status* diffs */
static int	magit_diff_ignore_ws;     /* 0/1: -w */
```
Forward decls + commands:
```c
static int	magit_diff_more(int, int);
static int	magit_diff_less(int, int);
static int	magit_diff_ws(int, int);

static int
magit_diff_more(int f, int n)
{
	if (magit_diff_context < 32)
		magit_diff_context++;
	mg_magit_set_diff_view(magit_diff_context, magit_diff_ignore_ws);
	return (magit_refresh(f, n));
}
static int
magit_diff_less(int f, int n)
{
	if (magit_diff_context > 0)
		magit_diff_context--;
	mg_magit_set_diff_view(magit_diff_context, magit_diff_ignore_ws);
	return (magit_refresh(f, n));
}
static int
magit_diff_ws(int f, int n)
{
	magit_diff_ignore_ws = !magit_diff_ignore_ws;
	mg_magit_set_diff_view(magit_diff_context, magit_diff_ignore_ws);
	return (magit_refresh(f, n));
}
```
PF arrays (by the other PFs):
```c
static PF magit_plus[]  = { magit_diff_more };
static PF magit_minus[] = { magit_diff_less };
static PF magit_w[]     = { magit_diff_ws };
```

- [ ] **Step 3: Bind `+`/`-`/`w` in `magitmap`**

`magitmap` is `KEYMAPE(32)` → `KEYMAPE(35)`, both count fields `32→35`. Insert in strict-ascending order:
- `'+'` (0x2b) and `'-'` (0x2d) go AFTER `'$'` (0x24) and BEFORE `'?'` (0x3f):
```c
		{ '$', '$', magit_dollar, NULL },		/* $: process log */
		{ '+', '+', magit_plus, NULL },			/* +: more diff context */
		{ '-', '-', magit_minus, NULL },		/* -: less diff context */
		{ '?', '?', magit_qmark, NULL },		/* ?: key help */
```
- `'w'` (0x77) goes AFTER `'u'` (0x75) and BEFORE `'z'` (0x7a):
```c
		{ 'u', 'u', magit_u, NULL },
		{ 'w', 'w', magit_w, NULL },			/* w: toggle -w (ignore whitespace) */
		{ 'z', 'z', magit_z, NULL }			/* z: stash menu */
```
(Verify the exact neighbors in the current file; `doscan` requires strict ascending + the count = 35.)

- [ ] **Step 4: Warn on the `-2` sentinel at the 5 staging call sites**

At each staging call site (magit_cmd.c:3716, 3725, 3753, 3762, 3852), the current shape is `if (mg_magit_..._(...) != 1) { ewprintf("…"); return (FALSE); }`. Change to capture the code and special-case `-2`. Example for the hunk-stage site (3725):
```c
		int rc = mg_magit_stage_hunk(cwd, path, hunk);
		if (rc == -2) {
			ewprintf("Turn off -w (w) to stage hunks");
			return (FALSE);
		}
		if (rc != 1) {
			ewprintf("Stage hunk failed");
			return (FALSE);
		}
```
Apply the same `-2`→"Turn off -w (w) to stage hunks" handling at all 5 sites (region + hunk, stage/unstage/discard).

- [ ] **Step 5: Run the pty test → PASS.** Full suite `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 6: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(diff): +/- context, w ignore-whitespace keys; warn on ws-blocked staging"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-diffctl .` (runs ctest; the gate that catches pty flakes).
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: expand a diff; `+`/`-` change context; `w` toggles whitespace + the `Diff: -U.. -w` header; with `-w` on, `s` on a hunk warns; whole-file `s` still works; staging a hunk at `-U6` stages the right change (index alignment).
- [ ] Open the PR into `neomg` (clean base — no stack this time).
