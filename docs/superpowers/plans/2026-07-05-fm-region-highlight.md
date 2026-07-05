# FM-REGION-HIGHLIGHT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The active region (mark…point) renders in standout (reverse video), and auto-deactivates on edit / `C-g` / copy — Emacs's transient-mark behavior.

**Architecture:** A per-window `w_flag` bit `WMARKED` gates a display-time standout of the mark…point span, reusing the ediff `MG_HL_BIT` machinery (neomg build only; c-legacy unchanged). Mark commands set the bit; `lchange`/`ctrlg`/`copyregion` clear it. `update()` forces a full repaint on a `WMARKED` window and precomputes the ordered span; `vt_render_line` standouts the in-span cells.

**Tech Stack:** core C (`basic.c`/`line.c`/`main.c`/`region.c`/`def.h`); the `ENABLE_NATIVE_MAGIT` display layer (`display.c`); the pty test harness.

## Global Constraints

- Display application is `#ifdef ENABLE_NATIVE_MAGIT` (reuses `MG_HL_BIT`); the flag + hooks are plain C (both builds). The `c-legacy` build must still compile and behave exactly as before (no highlight).
- `w_flag` currently uses only `WEPHEM` (0x01); `WMARKED = 0x02` is free. (`w_flag` is distinct from the redisplay `w_rflag`.)
- `MG_HL_BIT` is display-only (stripped at `display.c:950` before encoding) — never leaks into buffer content.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`) + `c-legacy` compiles.

---

### Task 1: Visible, auto-deactivating region highlight

**Files:**
- Modify: `src/def.h` (the `WMARKED` define, near `WEPHEM` ~334); `src/basic.c` (`isetmark`/`swapmark`/`clearmark`); `src/line.c` (`lchange`); `src/main.c` (`ctrlg`); `src/region.c` (`copyregion`); `src/display.c` (helpers + `update()` + `vt_render_line`)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Adds `#define WMARKED 0x02` (w_flag bit). No other new public symbols; display helpers are `static` in `display.c`.

- [ ] **Step 1: Write the failing pty test**

The modeline is *always* standout, so the test must detect standout **added around the region text**, not merely "a standout escape exists." Approach: put a unique word on line 1, capture the render with the region **inactive** (baseline) vs **active over that word**, and assert the active render has *more* standout-enter escapes (the region adds a run). Finalize the exact enter-standout bytes empirically first (see Step 2 note) — candidate `\x1b[7m` under `TERM=xterm`.
```cpp
TEST_CASE("the active region renders in standout, and deactivates on edit")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "REGIONWORD tail\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    auto count_so = [](const std::string &s) {
        size_t n = 0, i = 0;
        while ((i = s.find("\x1b[7m", i)) != std::string::npos) { ++n; i += 3; }
        return n;                       // enter-standout runs (adjust bytes per Step 2)
    };
    int base = 0, active = 0, after_edit = 0;
    if (wait_for(master, "REGIONWORD", std::chrono::seconds(8))) {
        (void)!::write(master, "\x01", 1);                 // C-a : BOL (region inactive)
        base = (int)count_so(drain_str(master, std::chrono::seconds(1)));
        (void)!::write(master, "\x00\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06", 11);
                                                           // C-SPC then C-f x10 over REGIONWORD
        active = (int)count_so(drain_str(master, std::chrono::seconds(2)));
        (void)!::write(master, "x", 1);                    // self-insert -> deactivates
        after_edit = (int)count_so(drain_str(master, std::chrono::seconds(2)));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(active > base);              // the active region added a standout run
    CHECK(after_edit <= base);        // editing deactivated it
}
```
(`\x00` is `C-SPC` = set-mark. If sending a NUL through the pty is unreliable, use the explicit `set-mark-command` via `M-x` or bind check — but `C-@`/NUL is standard for set-mark and the harness writes raw bytes.)

- [ ] **Step 2: Run → FAIL, and pin the standout bytes.**
`cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*active region renders*"`
Expected: FAIL — no region highlight yet, so `active == base`. **Before implementing**, confirm the actual enter-standout escape neomg emits: the modeline already renders in standout, so `drain_str` at startup will contain it — grep the captured bytes for the `enter_standout_mode` sequence and set `count_so`'s needle to match (likely `\x1b[7m`).

- [ ] **Step 3: The flag + activation/deactivation (plain C)**

`src/def.h` — after `#define WEPHEM 0x01` (~334):
```c
#define WMARKED 0x02			/* Window has an active (shown) region. */
```
`src/basic.c` — `isetmark` (sets the mark for `setmark`/mark-paragraph/mark-buffer) add at its end:
```c
	curwp->w_flag |= WMARKED;	/* activate the region highlight */
```
`swapmark` (before its `return (TRUE);`):
```c
	curwp->w_flag |= WMARKED;
```
`clearmark` (before its `return (TRUE);`):
```c
	curwp->w_flag &= ~WMARKED;
```
`src/line.c` — `lchange` (any buffer edit): at the end of the function, deactivate the editing window's region (Emacs deactivates on edit):
```c
	curwp->w_flag &= ~WMARKED;
```
`src/main.c` — `ctrlg` (`C-g`):
```c
ctrlg(int f, int n)
{
	if (curwp != NULL)
		curwp->w_flag &= ~WMARKED;
	return (ABORT);
}
```
`src/region.c` — `copyregion` (`M-w`; copy doesn't edit, so deactivate explicitly), before its success `return`:
```c
	curwp->w_flag &= ~WMARKED;
```
(`killregion`/`C-w` edits → `lchange` clears it; no change needed there.)

- [ ] **Step 4: The display (ENABLE_NATIVE_MAGIT)**

`src/display.c` — add helpers near the other `#ifdef ENABLE_NATIVE_MAGIT` display statics (above `vt_render_line`):
```c
#ifdef ENABLE_NATIVE_MAGIT
/*
 * Active-region standout. update() calls region_hl_setup() once per WMARKED
 * window (ordering the mark/point endpoints by buffer position); the render
 * loop walks lines top-to-bottom and region_hl_line() yields each line's
 * [from,to) column range, which vt_render_line paints with MG_HL_BIT.
 */
static struct line	*reg_sp, *reg_ep;
static int		 reg_so, reg_eo, reg_on, reg_inside;

static void
region_hl_setup(struct mgwin *wp)
{
	reg_on = reg_inside = 0;
	if (!(wp->w_flag & WMARKED) || wp->w_markp == NULL || wp != curwp)
		return;
	if (wp->w_markline < wp->w_dotline ||
	    (wp->w_markline == wp->w_dotline && wp->w_marko < wp->w_doto)) {
		reg_sp = wp->w_markp; reg_so = wp->w_marko;
		reg_ep = wp->w_dotp;  reg_eo = wp->w_doto;
	} else {
		reg_sp = wp->w_dotp;  reg_so = wp->w_doto;
		reg_ep = wp->w_markp; reg_eo = wp->w_marko;
	}
	if (reg_sp == reg_ep && reg_so == reg_eo)	/* empty region */
		return;
	reg_on = 1;
}

/* [*from,*to) cols to standout on `lp`; advances the top-to-bottom span state. */
static void
region_hl_line(struct line *lp, int *from, int *to)
{
	*from = *to = 0;
	if (!reg_on)
		return;
	if (reg_sp == reg_ep) {			/* single-line region */
		if (lp == reg_sp) { *from = reg_so; *to = reg_eo; }
		return;
	}
	if (lp == reg_sp) { *from = reg_so; *to = llength(lp); reg_inside = 1; }
	else if (lp == reg_ep) { *from = 0; *to = reg_eo; reg_inside = 0; }
	else if (reg_inside) { *from = 0; *to = llength(lp); }
}
#endif
```
In `update()`'s window loop (`for (wp = wheadp …)` ~625), immediately inside the loop, force a full repaint for a WMARKED window (before the `if (wp->w_rflag == 0) continue;`):
```c
#ifdef ENABLE_NATIVE_MAGIT
		if (wp->w_flag & WMARKED)
			wp->w_rflag |= WFFULL;	/* active region: repaint fully */
#endif
```
and after that `continue;`, set up the span:
```c
#ifdef ENABLE_NATIVE_MAGIT
		region_hl_setup(wp);
#endif
```
In `vt_render_line` (the `#ifdef ENABLE_NATIVE_MAGIT` first-cell block), compute the line's range once, then in the per-cell loop paint `MG_HL_BIT` for `ci` in `[rfrom,rto)` — beside the existing ediff/color application so they compose:
```c
	int		 rfrom, rto;
	region_hl_line(lp, &rfrom, &rto);
	/* ... in the per-cell loop, after the ediff MG_HL_BIT block: ... */
	if (ci >= rfrom && ci < rto)
		for (k = start; k < vtcol && k < ncol; k++)
			if (vp->v_text[k] != VT_CONT)
				vp->v_text[k] |= MG_HL_BIT;
```
(`ci` is the existing per-cell index; `start`/`vtcol`/`k`/`vp` are already in scope in that block.)

- [ ] **Step 5: Run → PASS.** `cmake --build build && ctest --test-dir build --output-on-failure`. Manually confirm the region shows/hides while typing (smoke).

- [ ] **Step 6: Commit**
```bash
git add src/def.h src/basic.c src/line.c src/main.c src/region.c src/display.c tests/test_editor.cpp
git commit -m "feat(region): highlight the active region (transient-mark), deactivate on edit/C-g/copy"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-region .`.
- [ ] `c-legacy` compiles + runs (highlight is magit-gated; the flag code is inert there): `cmake --preset c-legacy && cmake --build --preset c-legacy` (0 warnings), `neomg -h` exits 0.
- [ ] OFF/TSan (editor test): `cmake --build build-tsan && ./build-tsan/tests/test_editor --test-case="*active region renders*"`.
- [ ] Manual smoke: `C-SPC`, move → region shows and grows; `C-x C-x` keeps it; type a char / `C-w` / `M-w` / `C-g` → region clears; multi-line region highlights whole middle lines; a second window doesn't spuriously highlight.
- [ ] Update `todo.md`: mark FM-REGION-HIGHLIGHT done.
- [ ] Open the PR based on `neomg`.
