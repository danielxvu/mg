# FM-KILL-RING Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the single kill buffer into a bounded kill ring, and add `M-y` (yank-pop) so earlier kills survive and can be cycled — fixing real data loss.

**Architecture:** `src/yank.c`'s `kbufp` globals become a fixed array `kr[NKILL]` of `{buf,used,size,start}` entries. `kdelete()` rotates to the next slot instead of freeing; `kinsert`/`kgrow` write `kr[kr_head]`; `kremove` reads `kr[kr_yptr]`. `yank` and a new `yank_pop` share a static `do_yank(n)` and are gated by a new `CFYANK` command flag.

**Tech Stack:** core C (`yank.c`, `def.h`, `keymap.c`, `funmap.c`); the pty test harness.

## Global Constraints

- `yank.c` is core, unguarded plain C — this ships in **both** builds (neomg and `c-legacy`). It must compile and pass on all platforms, including `c-legacy` (`-Wall -Wextra -Werror`).
- `NKILL = 60`. Match existing yank.c style (OpenBSD KNF, `RSIZE`, `CHARMASK`, tabs).
- Behavior for a single kill+`C-y` must be unchanged (regression).

---

### Task 1: Kill ring + M-y (yank-pop)

**Files:**
- Modify: `src/yank.c` (globals, `kdelete`, `kinsert`, `kgrow`, `kremove`, `yank`; add `do_yank`, `yank_pop`)
- Modify: `src/def.h` (`CFYANK` define; `yank_pop` prototype)
- Modify: `src/keymap.c` (metamap `y` → `yank_pop`)
- Modify: `src/funmap.c` (`yank-pop` funmap entry)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Produces: `int yank_pop(int, int)` (new command); `#define CFYANK 0x0008`.
- `kdelete`/`kinsert`/`kremove`/`yank` keep their existing signatures (`void kdelete(void)`, `int kinsert(int,int)`, `int kremove(int)`, `int yank(int,int)`).

- [ ] **Step 1: Write the failing test** (append to `tests/test_editor.cpp`)

Kill three distinct lines *non-consecutively* (a `C-n` between kills breaks the `CFKILL` chain so each is its own ring entry), then yank + yank-pop and assert the ring retained all three.
```cpp
TEST_CASE("kill ring: M-y (yank-pop) cycles through non-consecutive kills")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "AAA\nBBB\nCCC\nsink\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    std::string content;
    if (wait_for(master, "AAA", std::chrono::seconds(8))) {
        // Kill "AAA", "BBB", "CCC" as three separate ring entries.
        // C-a C-k C-k kills the line text then the newline; C-n moves to the
        // next line (a non-kill command) so the next C-k starts a NEW entry.
        (void)!::write(master, "\x01\x0b\x0b", 3);   // line 1: kill "AAA" + NL
        drain(master, 200);
        (void)!::write(master, "\x01\x0b\x0b", 3);   // now line "BBB": kill it
        drain(master, 200);
        (void)!::write(master, "\x01\x0b\x0b", 3);   // now line "CCC": kill it
        drain(master, 200);
        // Point is on "sink". Go to end of buffer, open a fresh line.
        (void)!::write(master, "\x1b>", 2);          // M-> end of buffer
        (void)!::write(master, "\r", 1);             // newline
        (void)!::write(master, "\x19", 1);           // C-y  -> newest kill "CCC\n"
        (void)!::write(master, "\x1by", 2);          // M-y  -> "BBB\n"
        (void)!::write(master, "\x1by", 2);          // M-y  -> "AAA\n"
        (void)!::write(master, "\x18\x13", 2);       // C-x C-s
        (void)wait_for(master, "Wrote", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    content = ss.str();
    fs::remove_all(dir);
    // After C-y then two M-y, the yanked text is the *oldest* kill "AAA".
    // Its survival proves the ring kept all three (the data-loss fix).
    CHECK(content.find("AAA") != std::string::npos);
}

TEST_CASE("kill ring: M-y without a preceding yank is refused")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "hello\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool refused = false;
    if (wait_for(master, "hello", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1by", 2);          // M-y with no prior yank
        refused = wait_for(master, "Previous command was not a yank",
                           std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(refused);
}
```

- [ ] **Step 2: Run → FAIL.**
`cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*kill ring*"`
Expected: FAIL — `yank_pop`/`M-y` don't exist yet (`M-y` is currently `rescan`, so it does nothing and neither assertion holds).

- [ ] **Step 3: The flag + prototype (`src/def.h`)**

Beside `CFCPCN`/`CFKILL`/`CFINS` (~line 143):
```c
#define CFYANK	0x0008		/* Last command was a yank/yank-pop */
```
Beside the existing `int yank(int, int);` prototype, add:
```c
int		 yank_pop(int, int);
```

- [ ] **Step 4: Rewrite the kill storage (`src/yank.c`)**

Replace the globals block (`kbufp`/`kused`/`ksize`/`kstart` + the `kgrow` decl, lines ~16-22) with:
```c
#define KBLOCK	 8192		/* Kill grow.			 */
#define NKILL	 60		/* Kill-ring size (Emacs kill-ring-max=120). */

struct kill {
	char	*buf;		/* entry data			 */
	RSIZE	 used;		/* # bytes used			 */
	RSIZE	 size;		/* # bytes allocated		 */
	RSIZE	 start;		/* # first used byte (front insert) */
};
static struct kill kr[NKILL];	/* the kill ring		 */
static int	 kr_head;	/* newest entry			 */
static int	 kr_n;		/* # entries in use (<= NKILL)	 */
static int	 kr_yptr;	/* yank pointer (entry a yank reads) */

static int	 kgrow(int);
static int	 do_yank(int);
```
Replace `kdelete` (rotate instead of free):
```c
void
kdelete(void)
{
	/*
	 * Called when a new kill context starts. Rotate to the next ring slot
	 * (reclaiming the oldest when full) so the PREVIOUS kill survives for
	 * M-y -- the old behavior freed it, silently losing it.
	 */
	if (kr_n > 0)
		kr_head = (kr_head + 1) % NKILL;
	if (kr[kr_head].buf != NULL) {
		free(kr[kr_head].buf);
		kr[kr_head].buf = NULL;
	}
	kr[kr_head].used = kr[kr_head].size = kr[kr_head].start = 0;
	if (kr_n < NKILL)
		kr_n++;
	kr_yptr = kr_head;
}
```
Replace `kinsert` (write the current entry):
```c
int
kinsert(int c, int dir)
{
	struct kill	*k = &kr[kr_head];

	if (dir == KNONE)
		return (TRUE);
	if (k->used == k->size && dir == KFORW && kgrow(dir) == FALSE)
		return (FALSE);
	if (k->start == 0 && dir == KBACK && kgrow(dir) == FALSE)
		return (FALSE);
	if (dir == KFORW)
		k->buf[k->used++] = c;
	else if (dir == KBACK)
		k->buf[--k->start] = c;
	else
		panic("broken kinsert call");	/* Oh shit! */
	return (TRUE);
}
```
Replace `kgrow` (grow the current entry):
```c
static int
kgrow(int dir)
{
	struct kill	*k = &kr[kr_head];
	int		 nstart;
	char		*nbufp;

	if ((unsigned)(k->size + KBLOCK) <= (unsigned)k->size) {
		/* probably 16 bit unsigned */
		dobeep();
		ewprintf("Kill buffer size at maximum");
		return (FALSE);
	}
	if ((nbufp = malloc((unsigned)(k->size + KBLOCK))) == NULL) {
		dobeep();
		ewprintf("Can't get %ld bytes", (long)(k->size + KBLOCK));
		return (FALSE);
	}
	nstart = (dir == KBACK) ? (k->start + KBLOCK) : (KBLOCK / 4);
	bcopy(&(k->buf[k->start]), &(nbufp[nstart]), (int)(k->used - k->start));
	free(k->buf);
	k->buf = nbufp;
	k->size += KBLOCK;
	k->used = k->used - k->start + nstart;
	k->start = nstart;
	return (TRUE);
}
```
Replace `kremove` (read the yank entry):
```c
int
kremove(int n)
{
	struct kill	*k = &kr[kr_yptr];

	if (n < 0 || n + k->start >= k->used)
		return (-1);
	return (CHARMASK(k->buf[n + k->start]));
}
```

- [ ] **Step 5: `do_yank` + `yank` + `yank_pop` (`src/yank.c`)**

Replace the `yank` function with the shared helper plus the two commands:
```c
/*
 * Insert the yank-pointer entry n times at point, leaving the mark at the
 * start of the last insert and point at its end (so M-y can replace it).
 * Shared by yank (C-y) and yank_pop (M-y).
 */
static int
do_yank(int n)
{
	struct line	*lp;
	int		 c, i, nline;

	nline = 0;
	undo_boundary_enable(FFRAND, 0);
	while (n--) {
		/* mark around last yank */
		isetmark();
		i = 0;
		while ((c = kremove(i)) >= 0) {
			if (c == *curbp->b_nlchr) {
				if (enewline(FFRAND, 1) == FALSE)
					return (FALSE);
				++nline;
			} else {
				if (linsert(1, c) == FALSE)
					return (FALSE);
			}
			++i;
		}
	}
	/* cosmetic adjustment: keep the insert on-screen */
	lp = curwp->w_linep;
	if (curwp->w_dotp == lp) {
		while (nline-- && lback(lp) != curbp->b_headp)
			lp = lback(lp);
		curwp->w_linep = lp;
		curwp->w_rflag |= WFFULL;
	}
	undo_boundary_enable(FFRAND, 1);
	return (TRUE);
}

int
yank(int f, int n)
{
	if (n < 0)
		return (FALSE);
	kr_yptr = kr_head;		/* yank the newest entry */
	if (do_yank(n) == FALSE)
		return (FALSE);
	thisflag |= CFYANK;
	return (TRUE);
}

/*
 * M-y: replace the text just yanked with the previous kill-ring entry,
 * cycling one step older each press. Only valid right after a yank/yank-pop.
 */
int
yank_pop(int f, int n)
{
	struct region	 reg;
	int		 oldest;

	if ((lastflag & CFYANK) == 0) {
		dobeep();
		ewprintf("Previous command was not a yank");
		return (FALSE);
	}
	/* Delete the last-yanked text (mark..point) without touching the ring. */
	if (getregion(&reg) != TRUE)
		return (FALSE);
	curwp->w_dotp = reg.r_linep;
	curwp->w_doto = reg.r_offset;
	curwp->w_dotline = reg.r_lineno;
	if (ldelete(reg.r_size, KNONE) == FALSE)
		return (FALSE);
	/* Cycle one entry older, wrapping within the used entries. */
	oldest = (kr_head - (kr_n - 1) + NKILL) % NKILL;
	if (kr_yptr == oldest)
		kr_yptr = kr_head;
	else
		kr_yptr = (kr_yptr - 1 + NKILL) % NKILL;
	if (do_yank(1) == FALSE)
		return (FALSE);
	thisflag |= CFYANK;
	return (TRUE);
}
```
(`getregion`/`struct region`/`ldelete`/`KNONE` are already declared in `def.h`, which `yank.c` includes.)

- [ ] **Step 6: Bind M-y (`src/keymap.c` + `src/funmap.c`)**

`src/keymap.c` — in the metamap, change the `y` slot:
```c
	yank_pop,		/* y */
```
(was `rescan,			/* y */`).
`src/funmap.c` — beside the `yank` entry (`{yank, "yank", 1, NULL},`):
```c
	{yank_pop, "yank-pop", 1, NULL},
```

- [ ] **Step 7: Run → PASS.** `cmake --build build && ctest --test-dir build --output-on-failure`. Smoke: kill two lines separately, `C-y` then `M-y`, watch the yank swap to the older kill; a plain single kill + `C-y` still yanks the last kill.

- [ ] **Step 8: Commit**
```bash
git add src/yank.c src/def.h src/keymap.c src/funmap.c tests/test_editor.cpp
git commit -m "feat(kill-ring): bounded kill ring + M-y yank-pop (was single buffer, lost prior kills)"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-killring .`.
- [ ] **c-legacy (this ships there — must pass):** `cmake --build --preset c-legacy` (0 warnings under `-Werror`); smoke `neomg` kill+yank if feasible.
- [ ] TSan (editor): `cmake --build build-tsan && ./build-tsan/tests/test_editor --test-case="*kill ring*"`.
- [ ] Manual smoke: `C-k C-k` (consecutive) still appends into one entry; `C-w`/`M-w` then `C-y`/`M-y`; `M-y` past the oldest wraps to the newest; `M-y` with no prior yank beeps.
- [ ] Update `todo.md`: mark FM-KILL-RING done; record the out-of-scope items (numeric `M-y`, browse-kill-ring, persistence, clipboard).
- [ ] Open the PR based on `neomg`.
