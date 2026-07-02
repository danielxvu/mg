# FM-LOG-TRANSIENT-KEY Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind `l` in `*magit-log*` to open the log transient, so log args can be tweaked without returning to `*magit-status*`.

**Architecture:** One keymap entry — no new commands, no action changes. `maglogmap` gains `{ 'l', 'l', magit_l, NULL }` (`magit_l` = the existing `{ magit_menu_log }` that `l` runs in `*magit-status*`).

**Tech Stack:** the mg C keymap (`KEYMAPE`) + the pty editor test harness.

## Global Constraints

- Insert the new entry in **strict ascending key order** (`doscan` binary-searches the keymap): `'l'` (108) between `'g'` (103) and `'q'` (113). No assert covers `maglogmap` ordering, so the pty test is the guard — an out-of-order insert makes `doscan` miss `l` and the test fails.
- Scope: `*magit-log*` only (not `*magit-reflog*`).
- `l` no longer moves the cursor down in `*magit-log*` — intended (matches Magit; nav via `n`/`p`/arrows).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Bind `l` in `maglogmap` + pty test

**Files:**
- Modify: `src/magit_cmd.c` (`maglogmap`, ~line 485)
- Test: `tests/test_editor.cpp` (helpers `make_repo`/`wait_for`/`quit_neomg`/`NEOMG_BINARY`/`forkpty`)

**Interfaces:**
- Consumes: the existing `magit_l[]` = `{ magit_menu_log }` (opens `magit_transient(&log_menu, …)`); `quit_neomg` (from `fm-pty-exit-drain`).

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp`:
```cpp
TEST_CASE("l in *magit-log* re-opens the log transient")
{
    auto repo = make_repo(); // commits "init"
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool reopened = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "l", 1);           // open the log transient
            if (wait_for(master, "--grep", std::chrono::seconds(8))) {
                (void)!::write(master, "l", 1);       // l l: run the log -> *magit-log*
                if (wait_for(master, "init", std::chrono::seconds(8))) { // log rendered
                    (void)!::write(master, "l", 1);   // l IN *magit-log* -> NEW binding
                    reopened = wait_for(master, "--grep", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid); // drains + reaps (C-g closes the transient first)
    fs::remove_all(repo);
    CHECK(reopened); // pressing l in *magit-log* rendered the log transient
}
```
(`make_repo` commits `-m init`, so `init` is the commit summary that appears only once `*magit-log*` renders — a marker the transient menu and status buffer don't show. The final `--grep` wait is a fresh render of the transient, this time opened from `*magit-log*`.)

- [ ] **Step 2: Run → FAIL.** `cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*magit-log* re-opens*"`
Expected: FAIL — `l` in `*magit-log*` currently runs `llinedown` (cursor down), so no transient renders and `reopened` stays false (times out).

- [ ] **Step 3: Add the keymap entry**

`src/magit_cmd.c` — change `maglogmap` (~line 485) from `KEYMAPE(6)` to `KEYMAPE(7)`, bump both counts, and insert the `l` entry between `g` and `q` (ascending):
```c
static struct KEYMAPE (7) maglogmap = {
	7,
	7,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), maglog_ret, NULL },	/* RET: show commit */
		{ 'A', 'A', maglog_A, NULL },			/* A: cherry-pick at point */
		{ 'T', 'T', maglog_T, NULL },			/* T: set/remove note */
		{ 'V', 'V', maglog_V, NULL },			/* V: revert commit */
		{ 'g', 'g', maglog_g, NULL },			/* g: refresh */
		{ 'l', 'l', magit_l, NULL },			/* l: log transient (tweak args in place) */
		{ 'q', 'q', maglog_q, NULL }			/* q: close */
	}
};
```

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(log): l re-opens the log transient in *magit-log*"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-logkey .`.
- [ ] OFF: `cmake --build build-tsan && ./build-tsan/tests/test_editor` (the new pty test under TSan).
- [ ] Manual smoke: open `*magit-log*` (`l l`), press `l` → the log transient opens; toggle `--all`/`--author=`/etc. and run → `*magit-log*` rebuilds in place; `q` still closes; `n`/`p` still navigate.
- [ ] Update `todo.md`: mark FM-LOG-TRANSIENT-KEY done.
- [ ] Open the PR stacked on `fm-pty-exit-drain` (#100).
