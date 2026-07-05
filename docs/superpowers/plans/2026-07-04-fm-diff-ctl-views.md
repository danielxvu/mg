# FM-DIFF-CTL-VIEWS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `d` diff-view controls (`+`/`-` context, `w` whitespace) apply to `*magit-commit*`, refreshing it in place; the commit view shows its active `Diff: -U<n>` config.

**Architecture:** Engine `commit_diff` honors the diff-view atomics; bridge `mg_magit_commit_diff` emits the config header like status; editor adds a view-aware `magit_diff_view_refresh()` router so the existing controls + `d` popup drive whichever diff buffer is current, and binds `+`/`-`/`w`/`d` in `magcommitmap`. The config stays global (one knob, all views).

**Tech Stack:** C++23 module (`mg.git`, libgit2); the mg keymap/buffer machinery; the pty test harness.

## Global Constraints

- Config stays **global** (`g_diff_context`/`g_diff_ignore_ws`) — one knob for status + commit views, like Magit's `magit-diff-context`.
- Status behavior is **byte-identical**: from `*magit-status*` the router always calls `magit_refresh`.
- Whitespace-ignore in the commit view is pure display (read-only buffer, no staging concern).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine — `commit_diff` honors the diff-view config

**Files:**
- Modify: `src/magit/git.cppm` (`commit_diff` ~4061, the `git_diff_tree_to_tree` call ~4097)
- Test: `tests/test_git.cpp`

**Interfaces:**
- Produces: `commit_diff` hunks computed with `g_diff_context` context lines and `g_diff_ignore_ws` applied.

- [ ] **Step 1: Write the failing tests** (mirroring the existing `file_diff` context/whitespace tests at ~3516)

```cpp
TEST_CASE("set_diff_view controls context lines in commit_diff")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    commit_file(dir, "f.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", "add f");
    commit_file(dir, "f.txt", "1\n2\n3\n4\n5\nSIX\n7\n8\n9\n10\n", "change 6"); // HEAD

    mg::git::set_diff_view(3, false); // line "10" (4 from the change) NOT in ctx 3
    auto d3 = mg::git::commit_diff(dir.string(), "HEAD");
    REQUIRE(d3.has_value());
    bool has10_c3 = false;
    for (auto &h : *d3) for (auto &l : h.lines)
        if (l.content.find("10") != std::string::npos) has10_c3 = true;
    CHECK_FALSE(has10_c3);

    mg::git::set_diff_view(6, false); // ctx 6 widens to include "10"
    auto d6 = mg::git::commit_diff(dir.string(), "HEAD");
    REQUIRE(d6.has_value());
    bool has10_c6 = false;
    for (auto &h : *d6) for (auto &l : h.lines)
        if (l.content.find("10") != std::string::npos) has10_c6 = true;
    CHECK(has10_c6);

    mg::git::set_diff_view(3, false); // reset for other tests
    fs::remove_all(dir);
}

TEST_CASE("set_diff_view ignore-whitespace hides a whitespace-only commit_diff")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    commit_file(dir, "w.txt", "alpha\nbravo\n", "add w");
    commit_file(dir, "w.txt", "alpha \nbravo\n", "trailing space"); // HEAD, ws-only

    mg::git::set_diff_view(3, true);
    auto d = mg::git::commit_diff(dir.string(), "HEAD");
    REQUIRE(d.has_value());
    CHECK(d->empty()); // ws-only change -> no hunks when ignoring whitespace
    mg::git::set_diff_view(3, false);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (context is fixed at 3, whitespace never ignored). `cmake --build build --target test_git && ./build/tests/test_git --test-case="*commit_diff*"`

- [ ] **Step 3: Add the diff options**

In `commit_diff` (~4061), replace the `git_diff_tree_to_tree(&raw_diff, r.get(), parent_tree.get(), tree.get(), nullptr)` call with an options-carrying one:
```cpp
    git_diff_options dopts;
    git_diff_options_init(&dopts, GIT_DIFF_OPTIONS_VERSION);
    dopts.context_lines =
        static_cast<uint32_t>(g_diff_context.load(std::memory_order_relaxed));
    if (g_diff_ignore_ws.load(std::memory_order_relaxed))
        dopts.flags |= GIT_DIFF_IGNORE_WHITESPACE;

    git_diff *raw_diff = nullptr;
    if (git_diff_tree_to_tree(&raw_diff, r.get(), parent_tree.get(), tree.get(),
                              &dopts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);
    return diff_to_hunks(diff.get());
```

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(diff): commit_diff honors the diff-view context/whitespace config"
```

---

### Task 2: Bridge — `mg_magit_commit_diff` emits the `Diff:` config header

**Files:**
- Modify: `src/magit/bridge.cpp` (`mg_magit_commit_diff` ~1202, right after the `commit <rev>` header)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Produces: the commit-view stream includes a `Diff:     -U<n> [-w]` line (as `MG_LINE_OTHER`) right after `commit <rev>`, only when the config is non-default (context != 3 or ignore-ws).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("mg_magit_commit_diff emits the Diff config header when non-default")
{
    auto dir = make_repo_with_commit("c0");
    std::string d = dir.string();
    auto lines = [&]() {
        std::vector<std::string> v;
        mg_magit_commit_diff(d.c_str(), "HEAD",
            [](void *c, const char *l, int, const char *, int) {
                static_cast<std::vector<std::string>*>(c)->push_back(l);
            }, &v);
        return v;
    };
    auto has_diff = [](const std::vector<std::string> &v) {
        for (auto &l : v) if (l.rfind("Diff:", 0) == 0) return true;
        return false;
    };
    mg::git::set_diff_view(3, false);
    CHECK_FALSE(has_diff(lines()));         // default -> no header
    mg::git::set_diff_view(5, false);
    auto v5 = lines();
    bool u5 = false;
    for (auto &l : v5) if (l.find("-U5") != std::string::npos) u5 = true;
    CHECK(has_diff(v5));                     // non-default -> header
    CHECK(u5);                               // shows -U5
    mg::git::set_diff_view(3, false);        // reset
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (no `Diff:` line in the commit stream).

- [ ] **Step 3: Emit the header**

In `mg_magit_commit_diff` (~1202), right after `out_no_path(std::string("commit ") + rev, MG_LINE_SECTION, -1);`:
```cpp
    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv = "Diff:     -U" +
            std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        out_no_path(dv, MG_LINE_OTHER, -1);
    }
```
(Verbatim match to the status header at bridge.cpp:727-731.)

- [ ] **Step 4: Run → PASS.** Full suite.

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(diff): commit view shows the Diff: -U<n> config header"
```

---

### Task 3: Editor — the view-aware refresh router + commit-view bindings

**Files:**
- Modify: `src/magit_cmd.c` (forward decls ~145; `magit_commit_rev` static ~224; `magcommitmap` ~562; `magit_commit_build`/`magit_show_rev` refactor ~2741; `magit_diff_view_refresh` new; `magit_diff_more/less/ws` ~4700; the `d` popup ~4774)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: Tasks 1–2 (the commit view honors the config + shows the `Diff:` header); `magit_plus`/`magit_minus`/`magit_w`/`magit_d` PF arrays; `magit_refresh`; `magit_commit_bp`.

- [ ] **Step 1: Write the failing pty test**

```cpp
TEST_CASE("+ in *magit-commit* rebuilds the diff with more context")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool oncommit = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            if (wait_for(master, "init", std::chrono::seconds(8))) {
                (void)!::write(master, "\r", 1);   // RET: show the commit
                oncommit = wait_for(master, "commit ", std::chrono::seconds(8));
                if (oncommit) {
                    (void)!::write(master, "+", 1); // more context in commit view
                    // The Diff: header (Task 2) appears only at non-default ctx,
                    // and only *magit-commit* re-renders it here.
                    applied = wait_for(master, "Diff:", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(oncommit); // RET opened the commit view
    CHECK(applied);  // + rebuilt it with the new context (Diff: header rendered)
}
```
Run → FAIL (`+` unbound in `magcommitmap` — nothing happens).

- [ ] **Step 2: Add the rev store, the build/show refactor, and the router**

Forward decls near the other statics (~145):
```c
static int	magit_commit_build(struct buffer *, const char *);
static int	magit_diff_view_refresh(struct buffer *, int, int);
```
The remembered rev, next to `magit_commit_bp` (~224):
```c
static char	magit_commit_rev[256];	/* rev shown in *magit-commit* (for refresh) */
```
Refactor `magit_show_rev` (~2741) — extract the buffer rebuild into `magit_commit_build`, storing the rev; `magit_show_rev` = build + popbuf:
```c
/* Rebuild *magit-commit* `bp` to show `rev`'s diff (honoring the diff-view
 * config). Stores `rev` so magit_diff_view_refresh can re-render in place. */
static int
magit_commit_build(struct buffer *bp, const char *rev)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd));
	(void)strlcpy(magit_commit_rev, rev, sizeof(magit_commit_rev));
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	magit_commit_bp = bp;		/* gate display.c's color hook on this */
	magit_cell_color_reset();	/* freed lines may be reused; drop memo */
	magit_commit_meta_count = 0;
	if (mg_magit_commit_diff(cwd, rev, magit_diff_emit, bp) == 0) {
		ewprintf("No diff for %s", rev);
		return (FALSE);
	}
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	return (TRUE);
}

/* Pop a read-only *magit-commit* buffer showing `rev`'s diff. */
static int
magit_show_rev(const char *rev)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if ((bp = bfind("*magit-commit*", TRUE)) == NULL)
		return (FALSE);
	if (magit_commit_build(bp, rev) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-commit-view-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}
```
(This preserves the exact prior behavior; only the body is split and the rev is stored.)

Add the router (anywhere after `magit_commit_build` and before `magit_diff_more`, e.g. just above `magit_diff_more` ~4700):
```c
/* Refresh whichever diff view `target` is: the commit view rebuilds from its
 * remembered rev (redrawing its windows); anything else refreshes *magit-status*
 * (byte-identical to the old magit_refresh path). */
static int
magit_diff_view_refresh(struct buffer *target, int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if (target == magit_commit_bp && magit_commit_rev[0] != '\0') {
		if ((bp = bfind("*magit-commit*", FALSE)) == NULL)
			return (FALSE);
		if (magit_commit_build(bp, magit_commit_rev) != TRUE)
			return (FALSE);
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
			if (wp->w_bufp == bp) {
				wp->w_dotp = bp->b_dotp;
				wp->w_doto = 0;
				wp->w_markp = NULL;
				wp->w_marko = 0;
				wp->w_rflag |= WFFULL;
			}
		return (TRUE);
	}
	return (magit_refresh(f, n));
}
```

- [ ] **Step 3: Route the controls + bind them in the commit view**

`magit_diff_more/less/ws` (~4700-4725): change each `return (magit_refresh(f, n));` to `return (magit_diff_view_refresh(curbp, f, n));`.

The `d` popup (`magit_diff_transient` ~4774): change `(void)magit_refresh(f, n);` to `(void)magit_diff_view_refresh(stbp, f, n);` (**`stbp`** — the buffer saved when the popup opened; inside the loop `curbp` is the transient popup). Leave the `stwp->w_rflag |= WFFULL;` line — it forces the underlying window's redraw for both status and commit.

`magcommitmap` (~562): `KEYMAPE (1)` → `(5)`, both counts `1` → `5`, entries in strict ascending order:
```c
static struct KEYMAPE (5) magcommitmap = {
	5,
	5,
	rescan,
	{
		{ '+', '+', magit_plus, NULL },		/* +: more diff context */
		{ '-', '-', magit_minus, NULL },	/* -: less diff context */
		{ 'd', 'd', magit_d, NULL },		/* d: diff-view popup */
		{ 'q', 'q', magcommit_q, NULL },	/* q: close */
		{ 'w', 'w', magit_w, NULL }		/* w: toggle -w */
	}
};
```
(`magit_plus`/`magit_minus`/`magit_w`/`magit_d` are defined earlier in the file, ~449-467 — before `magcommitmap`.)

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(diff): +/-/w/d drive the commit view via a view-aware refresh router"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-diffviews .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: `RET` a commit → `*magit-commit*`; `+`/`-` change its context live (the `Diff: -U<n>` header appears/updates), `w` toggles whitespace, `d` opens the popup and adjusts in place, `q` closes; back in `*magit-status*`, `+`/`-`/`w`/`d` still drive the status diff; the context set in one view carries to the other (global knob).
- [ ] Update `todo.md`: mark FM-DIFF-CTL-VIEWS done; record the deferrals (per-buffer independent context; other diff surfaces).
- [ ] Open the PR based on `neomg`.
