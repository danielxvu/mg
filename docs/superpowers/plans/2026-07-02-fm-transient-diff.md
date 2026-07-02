# FM-TRANSIENT-DIFF Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A live diff-view popup bound to `d` in `*magit-status*` — `+`/`-`/`w` adjust context/whitespace inside the popup, applying immediately.

**Architecture:** Editor-only. A `magit_diff_transient` command reuses the `magit_transient` popup scaffolding (`bfind` + `popbuf` open; `delwind` + restore close) with a live key loop, driving the existing FM-DIFF-CTL machinery (`magit_diff_context`/`magit_diff_ignore_ws`, `mg_magit_set_diff_view`, `magit_refresh`). No engine/bridge changes.

**Tech Stack:** mg C keymap + popup (`popbuf`/`getkey`/`update`/`addlinef`); the pty editor test harness.

## Global Constraints

- No engine or bridge changes. `magit_refresh` rebuilds `*magit-status*` **by name** (`bfind` + `magit_build`), so calling it from inside the popup refreshes the status buffer correctly even though the popup buffer is current — no window/buffer swap needed.
- `d` inserted in `magitmap` in **strict ascending key order** between `'c'` (99) and `'e'` (101); `magit_assert_keymap_sorted` panics at init if `magitmap` is out of order. Bump `KEYMAPE(36)` → `(37)` and both counts `36` → `37`.
- Context clamps 0–32; whitespace is a toggle. The bare `+`/`-`/`w` keys in `*magit-status*` stay unchanged.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: `magit_diff_transient` popup + `d` binding + pty test

**Files:**
- Modify: `src/magit_cmd.c` (new `magit_diff_transient` + render; forward decl; `magit_d[]`; `magitmap` entry)
- Test: `tests/test_editor.cpp` (helpers `make_repo`/`wait_for`/`quit_neomg`/`NEOMG_BINARY`/`forkpty`)

**Interfaces:**
- Consumes: `magit_diff_context`/`magit_diff_ignore_ws` (globals), `mg_magit_set_diff_view`, `magit_refresh`, and the `magit_transient` popup primitives (`bfind`, `popbuf`, `delwind`, `getkey`, `update`, `addlinef`, `bclear`, `bfirstlp`, `WNONE`/`WFFULL`/`CMODE`/`CCHR`).

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp`:
```cpp
TEST_CASE("d opens the diff-view popup and + applies live to *magit-status*")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool opened = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "d", 1);           // open the diff-view popup
            opened = wait_for(master, "Diff view", std::chrono::seconds(8));
            if (opened) {
                (void)!::write(master, "+", 1);       // more context -> -U4
                // The "Diff:" header (colon) is emitted ONLY by *magit-status*
                // when context != 3 -- so seeing it proves the live refresh, not
                // just the popup's own re-render. (The popup title is "Diff view".)
                applied = wait_for(master, "Diff:", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid); // C-g closes the popup, then quits
    fs::remove_all(repo);
    CHECK(opened);  // d opened the diff-view popup
    CHECK(applied); // + applied live: *magit-status* re-rendered with the Diff: header
}
```

- [ ] **Step 2: Run → FAIL.** `cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*diff-view popup*"`
Expected: FAIL — `d` is unbound in `*magit-status*` today, so no popup renders (`opened` false).

- [ ] **Step 3: Add the command, render, and binding**

`src/magit_cmd.c` — forward declaration next to the other diff decls (~line 184, after `magit_diff_ws`):
```c
static int	magit_diff_transient(int, int);
```
Add the render + command (place near `magit_diff_more/less/ws`, ~line 4415, so the globals are in scope):
```c
/* Render the live diff-view popup: current context + whitespace + keys. */
static void
magit_diff_transient_render(struct buffer *bp)
{
	(void)bclear(bp);
	(void)addlinef(bp, "Diff view");
	(void)addlinef(bp, " +/-  context      -U%d", magit_diff_context);
	(void)addlinef(bp, " w    whitespace   %s",
	    magit_diff_ignore_ws ? "ignored" : "shown");
	(void)addlinef(bp, " q    close");
}

/* d: a live diff-view popup. +/- adjust context, w toggles -w; each applies
 * immediately (set_diff_view + refresh *magit-status*) and the popup stays open.
 * q / C-g / ESC close. Reuses magit_transient's popup scaffolding. */
static int
magit_diff_transient(int f, int n)
{
	struct buffer	*bp, *stbp;
	struct mgwin	*wp, *stwp;
	int		 k;

	stwp = curwp;
	stbp = curbp;
	if ((bp = bfind("*magit-transient*", TRUE)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);

	for (;;) {
		magit_diff_transient_render(bp);
		wp->w_dotp = bfirstlp(bp);
		wp->w_doto = 0;
		wp->w_rflag |= WFFULL;
		update(CMODE);
		k = getkey(FALSE);
		if (k == CCHR('G') || k == CCHR('[') || k == 'q') /* C-g / ESC / q */
			break;
		if (k == '+' || k == '-' || k == 'w') {
			if (k == '+' && magit_diff_context < 32)
				magit_diff_context++;
			else if (k == '-' && magit_diff_context > 0)
				magit_diff_context--;
			else if (k == 'w')
				magit_diff_ignore_ws = !magit_diff_ignore_ws;
			mg_magit_set_diff_view(magit_diff_context,
			    magit_diff_ignore_ws);
			(void)magit_refresh(f, n); /* rebuilds *magit-status* by name */
			stwp->w_rflag |= WFFULL;   /* force the status window redraw */
		}
		/* any other key: ignored; loop re-renders */
	}

	curwp = wp;
	curbp = wp->w_bufp;
	(void)delwind(f, n);
	curwp = stwp;
	curbp = stbp;
	return (TRUE);
}
```
Add the PF array near `magit_plus`/`magit_minus`/`magit_w` (~line 451):
```c
static PF magit_d[]     = { magit_diff_transient };	/* d: diff-view popup */
```
Insert the `magitmap` entry between `'c'` and `'e'` (lines 1084–1085), and bump the size/counts:
```c
static struct KEYMAPE (37) magitmap = {
	37,
	37,
	rescan,
	{
		...
		{ 'c', 'c', magit_c, NULL }, /* c: commit menu */
		{ 'd', 'd', magit_d, NULL },			/* d: diff-view popup */
		{ 'e', 'e', magit_e, NULL },			/* e: resolve conflict */
		...
```
(Change only the `KEYMAPE(36)`→`(37)` line, the two `36`→`37` count lines, and add the one `'d'` entry. Leave every other entry as-is.)

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`. (If `applied` is false but `opened` is true, the status window isn't redrawing behind the popup — the `stwp->w_rflag |= WFFULL` line above forces it; confirm it's present.)

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(diff): d opens a live diff-view popup (context/whitespace)"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-diffkey .`.
- [ ] OFF: `cmake --build build-tsan && ./build-tsan/tests/test_editor` (the new pty test under TSan).
- [ ] Manual smoke: in `*magit-status*`, `d` opens the popup; `+`/`-` change the context and the diff above re-renders live (the `Diff: -U<n>` header updates); `w` toggles whitespace; `q` closes; the bare `+`/`-`/`w` keys still work outside the popup.
- [ ] Update `todo.md`: record FM-TRANSIENT-DIFF done (add an entry under the transient gaps).
- [ ] Open the PR based on `neomg` (the stack was merged; this is a fresh branch off `neomg`).
