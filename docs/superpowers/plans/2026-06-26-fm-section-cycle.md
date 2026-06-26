# FM-SECTION-CYCLE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `M-1`/`M-2`/`M-3` set the whole `*magit-status*` buffer's visibility level (collapse → files → hunks), reusing the existing fold/expand state sets and rebuild.

**Architecture:** Editor-only (`src/magit_cmd.c`). Three commands rewrite `magit_folded[]` / `magit_expanded[]` by scanning the current buffer (via the per-line `magit_meta[]` map + line text), then rebuild via the existing `magit_refresh`. Bound in the status `magit_metamap`. No engine/bridge changes.

**Tech Stack:** the mg C editor core (keymaps, `magit_meta[]`, `magit_section_key`, `magit_refresh`), forkpty test harness.

## Global Constraints

- Editor-only; **no engine/bridge/module changes**. Builds in every config (ON, OFF) since it's C editor code.
- Reset-mode/level int: `1` = collapse (headers only), `2` = files (default), `3` = hunks expanded.
- `M-1` folds ALL sections (every `MG_LINE_SECTION`, incl. non-file sections — matches Magit level-1). `M-3` expands only diffable files (`MG_LINE_UNSTAGED` + `MG_LINE_STAGED`; untracked aren't diffable).
- Reuse `magit_section_key()` to derive fold keys (sections have no `path` — the key comes from the line TEXT). File paths come from `magit_meta[idx].path`.
- Respect the existing caps: `MAGIT_MAX_FOLDED` (32), `MAGIT_MAX_EXPANDED` (64); fill up to them (same bound `magit_toggle_*` already has).
- `magit_metamap` is `KEYMAPE(2)` (M-n/M-p); adding M-1/M-2/M-3 means `KEYMAPE(5)`, both count fields `2→5`, entries strict-ascending (`'1'`=0x31 < `'2'` < `'3'` < `'n'`=0x6e < `'p'`). `doscan` binary-searches — wrong count/order silently breaks the meta prefix.
- Existing `TAB` (`magit_toggle_expand`) is unchanged.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: `M-1`/`M-2`/`M-3` global visibility levels + binding

**Files:**
- Modify: `src/magit_cmd.c` (forward decls + 3 PFs; `magit_show_level` + 3 wrappers; extend `magit_metamap`)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes (all existing in `magit_cmd.c`): `magit_meta[]` (`{kind,hunk,path}` per line) + `magit_meta_count`; `magit_folded[]`/`magit_folded_count` (cap `MAGIT_MAX_FOLDED`); `magit_expanded[]`/`magit_expanded_count` (cap `MAGIT_MAX_EXPANDED`); `magit_section_key(line, key, size)`; `magit_refresh(f, n)`; `bfirstlp`/`lforw`/`ltext`/`llength`; line kinds `MG_LINE_SECTION`/`MG_LINE_UNSTAGED`/`MG_LINE_STAGED`.

- [ ] **Step 1: Write the failing pty test (M-3 expands)**

Add to `tests/test_editor.cpp` (it has `make_repo`, `forkpty`, `wait_for`, `drain`, `sh`). Build a repo with a committed file that is then modified (an unstaged hunk) so `M-3` produces a visible diff line. The strong positive signal is the diff content appearing:

```cpp
TEST_CASE("M-3 expands all files to hunks in *magit-status*")
{
    auto repo = make_repo();                 // git repo + tracked.txt committed
    // modify the tracked file so there's an unstaged hunk to expand
    { std::ofstream f((repo / "tracked.txt"), std::ios::app); f << "ALPHA_LINE\n"; }
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
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            // C-x 1: make *magit-status* the ONLY window. magit-status splits
            // (the tracked.txt file buffer stays in the top window), so without
            // this the file's "ALPHA_LINE"/name would be on screen regardless of
            // the magit level and contaminate the needles.
            (void)!::write(master, "\x18" "1", 2);
            (void)!::write(master, "\x1b" "3", 2);   // M-3: expand all
            (void)!::write(master, "\x0c", 1);        // C-l: force full repaint
            ok = wait_for(master, "ALPHA_LINE", std::chrono::seconds(8)); // hunk line
        }
    }
    const char quit[] = "\x18\x03";
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}
```

> NOTE: confirm `make_repo()` commits `tracked.txt` (the FM-REFLOG/editor tests use it). The appended `ALPHA_LINE` is the unstaged change; at the default level it is NOT shown (only the file name), and `M-3` expands the hunk so `+ALPHA_LINE` renders. If `make_repo` doesn't commit tracked.txt, adjust to commit it then append.

Run: `cmake --preset cpp && cmake --build build --target test_editor && ./build/tests/test_editor -tc="M-3 expands all files to hunks in *magit-status*"`
Expected: FAIL — `M-3` is unbound, the hunk never appears.

- [ ] **Step 2: Add the level commands + forward decls + PFs**

In `src/magit_cmd.c`, near the other forward decls (~line 122-150):
```c
static int	magit_show_level_1(int, int);
static int	magit_show_level_2(int, int);
static int	magit_show_level_3(int, int);
```

Add the implementation (place it near `magit_toggle_expand`/`magit_toggle_fold`, ~line 2638):
```c
/*
 * Global visibility level for *magit-status* (Magit's M-1/M-2/M-3,
 * magit-section-show-level-N-all). Rewrites the fold/expand sets by scanning the
 * current buffer, then rebuilds:
 *   level 1 = collapse (every section folded, headers only)
 *   level 2 = files    (nothing folded, nothing expanded -- the default)
 *   level 3 = expand   (every diffable file's inline diff shown)
 */
static int
magit_show_level(int level, int f, int n)
{
	struct line	*lp;
	int		 idx = 0;

	magit_folded_count = 0;
	magit_expanded_count = 0;
	if (level == 2)
		return (magit_refresh(f, n));	/* both cleared -> default view */

	for (lp = bfirstlp(curbp);
	    lp != curbp->b_headp && idx < magit_meta_count;
	    lp = lforw(lp), idx++) {
		int kind = magit_meta[idx].kind;
		if (level == 1 && kind == MG_LINE_SECTION) {
			char	hdr[PATH_MAX], key[PATH_MAX];
			int	len = llength(lp);
			int	j, dup = 0;

			if (len >= (int)sizeof(hdr))
				len = (int)sizeof(hdr) - 1;
			memcpy(hdr, ltext(lp), len);
			hdr[len] = '\0';
			magit_section_key(hdr, key, sizeof(key));
			for (j = 0; j < magit_folded_count; j++)
				if (strcmp(magit_folded[j], key) == 0) { dup = 1; break; }
			if (!dup && magit_folded_count < MAGIT_MAX_FOLDED)
				(void)strlcpy(magit_folded[magit_folded_count++], key,
				    PATH_MAX);
		} else if (level == 3 && (kind == MG_LINE_UNSTAGED ||
		    kind == MG_LINE_STAGED)) {
			const char	*p = magit_meta[idx].path;
			int		 j, dup = 0;

			if (p[0] == '\0')
				continue;
			for (j = 0; j < magit_expanded_count; j++)
				if (strcmp(magit_expanded[j], p) == 0) { dup = 1; break; }
			if (!dup && magit_expanded_count < MAGIT_MAX_EXPANDED)
				(void)strlcpy(magit_expanded[magit_expanded_count++], p,
				    PATH_MAX);
		}
	}
	return (magit_refresh(f, n));
}

static int magit_show_level_1(int f, int n) { return (magit_show_level(1, f, n)); }
static int magit_show_level_2(int f, int n) { return (magit_show_level(2, f, n)); }
static int magit_show_level_3(int f, int n) { return (magit_show_level(3, f, n)); }
```

Add the PF arrays near the other meta PFs (by `magit_meta_n`/`magit_meta_p`):
```c
static PF meta_1[] = { magit_show_level_1 };
static PF meta_2[] = { magit_show_level_2 };
static PF meta_3[] = { magit_show_level_3 };
```

(Dedup guards matter: the same path can appear in BOTH Unstaged and Staged when a file has staged + unstaged changes; one expand entry suffices.)

- [ ] **Step 3: Extend `magit_metamap` to bind M-1/M-2/M-3**

Change `magit_metamap` from `KEYMAPE(2)` to `KEYMAPE(5)`, both count fields `2→5`, inserting the digit entries ASCENDING before `'n'`:
```c
static struct KEYMAPE (5) magit_metamap = {
	5,
	5,
	rescan,
	{
		{ '1', '1', meta_1, NULL },	/* M-1: collapse to headers */
		{ '2', '2', meta_2, NULL },	/* M-2: show files */
		{ '3', '3', meta_3, NULL },	/* M-3: expand hunks */
		{ 'n', 'n', magit_meta_n, NULL },	/* M-n: next section */
		{ 'p', 'p', magit_meta_p, NULL }	/* M-p: previous section */
	}
};
```
(`'1'`=49 < `'2'`=50 < `'3'`=51 < `'n'`=110 < `'p'`=112 — strict ascending; count 5 matches 5 entries. This overrides M-1..M-3 digit-argument in status mode, exactly as Magit does.)

- [ ] **Step 4: Run the M-3 test (verify PASS)**

Run: `cmake --build build --target test_editor && ./build/tests/test_editor -tc="M-3 expands all files to hunks in *magit-status*"`
Expected: PASS (the `+ALPHA_LINE` hunk renders after `M-3`).

- [ ] **Step 5: Add the M-1 collapse + M-2 round-trip test**

Add to `tests/test_editor.cpp`. The collapse assertion is inherently about a body being hidden — make it robust with a FULL repaint (C-l) so the screen is a clean snapshot, then assert the hunk content is gone while the section header remains, and round-trip back with M-2:

```cpp
TEST_CASE("M-1 collapses section bodies; M-2 restores files")
{
    auto repo = make_repo();
    { std::ofstream f((repo / "tracked.txt"), std::ios::app); f << "ALPHA_LINE\n"; }
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
    bool collapsed = false, restored = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);    // C-x 1: only *magit-status*
            (void)!::write(master, "\x1b" "3", 2);   // M-3: expand (hunk shown)
            (void)!::write(master, "\x0c", 1);
            if (wait_for(master, "ALPHA_LINE", std::chrono::seconds(8))) {
                // M-1: collapse. Force a clean full repaint, drain to quiet,
                // then the snapshot must keep the header but drop the file body.
                (void)!::write(master, "\x1b" "1", 2);
                (void)!::write(master, "\x0c", 1);
                std::string snap = drain(master, std::chrono::milliseconds(1500));
                collapsed = snap.find("Unstaged changes") != std::string::npos &&
                            snap.find("tracked.txt") == std::string::npos;
                // M-2: restore files (the file name returns, no hunk)
                (void)!::write(master, "\x1b" "2", 2);
                (void)!::write(master, "\x0c", 1);
                restored = wait_for(master, "tracked.txt", std::chrono::seconds(8));
            }
        }
    }
    const char quit[] = "\x18\x03";
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(collapsed);
    CHECK(restored);
}
```

> The `drain()` helper (added in the FM-REFLOG work, present on this branch's base) reads until a sustained-quiet window — pair it with the C-l forced repaint so the captured `snap` is a full, settled screen, making the "tracked.txt absent" check reliable. Confirm `drain`'s exact signature in `tests/test_editor.cpp` and match it. If `make_repo`'s committed file is not literally `tracked.txt`, use the actual name in the absence check.

Run: `cmake --build build --target test_editor && ./build/tests/test_editor -tc="M-1 collapses section bodies; M-2 restores files"`
Expected: PASS.

- [ ] **Step 6: Full suite + commit**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` (all pass — confirm no other pty test regressed).

```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(magit): M-1/M-2/M-3 global section visibility levels"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure` — all green.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-seccycle .` — builds (runs ctest as a build step). **This is the gate that caught the FM-REFLOG pty flake** — the two new pty tests must pass here, not just on macOS.
- [ ] OFF build: `cmake --build build-tsan && ctest --test-dir build-tsan -R "editor"` — compiles + the editor tests pass with `ENABLE_ZIG_STATUS=OFF`.
- [ ] Manual smoke: open `*magit-status*` in a dirty repo; `M-1` collapses to headers, `M-2` shows files, `M-3` expands all hunks; `TAB` on a single item still toggles only that item.
- [ ] Open the PR (stacked on `fm-reflog` until #93 merges, then re-target `neomg`).
