# FM-QUICK-KEYBINDS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Four small Emacs keybinding gaps in one PR: `C-l` recenter cycle, `M-g` prefix → goto-line, `C-x z` repeat, `C-h k` → describe-key.

**Architecture:** Mostly keymap wiring plus two small behaviors (a recenter cycle in `reposition`, a `repeat` command reading the last-dispatched function). All plain C, both builds.

**Tech Stack:** core C (`window.c`, `kbd.c`, `keymap.c`, `funmap.c`, `def.h`); pty test harness.

## Global Constraints

- All changes are plain C, unguarded — must compile and pass on macOS, Alpine/musl, and `c-legacy` (`-Wall -Wextra -Werror`).
- Match existing style (OpenBSD KNF, tabs, `PF`, `KEYMAPE`).
- Keymap `map_element`s within a keymap MUST stay sorted by ascending `k_num` (`doscan` scans while `c > k_num`).
- Verified Emacs 30.2: `C-l` = recenter-top-bottom `(middle top bottom)`; `M-g g`/`M-g M-g` → goto-line; `C-x z` = repeat; `C-h k` = describe-key.

---

### Task 1: `C-h k` → describe-key

**Files:** Modify `src/keymap.c` (the `cHa` array). Test: `tests/test_editor.cpp`.

- [ ] **Step 1: Failing test**
```cpp
TEST_CASE("C-h k describes a key (like C-h c)")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "hi\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool ok = false;
    if (wait_for(master, "hi", std::chrono::seconds(8))) {
        (void)!::write(master, "\x08k", 2);   // C-h k
        (void)!::write(master, "\x06", 1);    // C-f  -> should report forward-char
        ok = wait_for(master, "forward-char", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(ok);
}
```
- [ ] **Step 2: Run → FAIL** (`C-h k` is `rescan`; no `forward-char` message).
`cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*C-h k describes*"`
- [ ] **Step 3: Bind it.** In `src/keymap.c`, `cHa` array, change the `/* k */` entry:
```c
	desckey,		/* k */
```
(was `rescan,			/* k */`).
- [ ] **Step 4: Run → PASS.**
- [ ] **Step 5: Commit** `git add src/keymap.c tests/test_editor.cpp && git commit -m "feat(keys): C-h k describes a key (bind to describe-key-briefly)"`

---

### Task 2: `M-g` prefix → goto-line

**Files:** Modify `src/keymap.c` (new `gotomap` + arrays; split the metamap `'['..'h'` element). Test: `tests/test_editor.cpp`.

**Interfaces:** `gotoline`, `markpara`, `metasqf`, `metasqlmap` already exist.

- [ ] **Step 1: Failing test**
```cpp
TEST_CASE("M-g g and M-g M-g both goto-line")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "L1\nL2\nL3\nL4\nL5\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    auto goto_via = [&](const char *keys, size_t klen, const char *line) {
        int master = -1;
        pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
        REQUIRE(pid >= 0);
        if (pid == 0) { ::setenv("TERM", "xterm", 1);
            ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
        bool ok = false;
        if (wait_for(master, "L1", std::chrono::seconds(8))) {
            (void)!::write(master, keys, klen);       // M-g g  or  M-g M-g
            (void)!::write(master, line, 2);          // "3\r" etc
            (void)!::write(master, "\x0b", 1);        // C-k : kill the target line's text
            (void)!::write(master, "\x18\x13", 2);    // C-x C-s
            ok = (bool)wait_for(master, "Wrote", std::chrono::seconds(8));
        }
        quit_neomg(master, pid);
        std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
        std::string c = ss.str();
        return c;
    };
    // M-g g 3<RET> puts point on line 3 ("L3"); C-k empties it.
    std::string a = goto_via("\x1bg" "g", 3, "3\r");
    CHECK(a.find("L3") == std::string::npos);        // line 3 got killed -> M-g g worked
    CHECK(a.find("L2") != std::string::npos);        // others intact
    // M-g M-g 2<RET> -> line 2 ("L2")
    std::string b = goto_via("\x1bg" "\x1bg", 4, "2\r");
    CHECK(b.find("L2") == std::string::npos);        // M-g M-g worked
    fs::remove_all(dir);
}
```
- [ ] **Step 2: Run → FAIL** (`M-g` unbound; the target line not killed).
- [ ] **Step 3: Add `gotomap` + arrays.** In `src/keymap.c`, before the `metamap` definition, add:
```c
/* M-g prefix (Emacs goto-map): g or M-g (repeated) -> goto-line. */
static PF gotomap_esc[] = {
	NULL			/* ESC -> re-enter gotomap (M-g M-g ...) */
};
static PF gotomap_g[] = {
	gotoline		/* g */
};
struct KEYMAPE (2) gotomap = {
	2,
	2,
	rescan,
	{
		{
			CCHR('['), CCHR('['), gotomap_esc, (KEYMAP *) &gotomap
		},
		{
			'g', 'g', gotomap_g, NULL
		}
	}
};

static PF metag[] = {
	NULL			/* g -> gotomap prefix */
};
static PF metah[] = {
	markpara		/* h */
};
```
- [ ] **Step 4: Split the metamap element.** In the `metamap` definition, change the count `KEYMAPE (8)` → `KEYMAPE (10)`, the two leading counts `8, 8,` → `10, 10,`, and replace the single element:
```c
			{
				'[', 'h', metasqf, (KEYMAP *) &metasqlmap
			},
```
with three sorted elements:
```c
			{
				'[', 'f', metasqf, (KEYMAP *) &metasqlmap
			},
			{
				'g', 'g', metag, (KEYMAP *) &gotomap
			},
			{
				'h', 'h', metah, NULL
			},
```
(The `metasqf` array is unchanged; indices 0–11 still map `'['`..`'f'`. `metasqf`'s now-unused `'g'`/`'h'` tail entries are harmless.)
- [ ] **Step 5: Run → PASS.** Also smoke `M-g x` (unbound letter) beeps/no-ops, and `M-[` arrow-key escapes still work (`metasqlmap` intact).
- [ ] **Step 6: Commit** `git add src/keymap.c tests/test_editor.cpp && git commit -m "feat(keys): M-g prefix map -> goto-line (M-g g / M-g M-g)"`

---

### Task 3: `C-l` recenter cycle (middle → top → bottom)

**Files:** Modify `src/def.h` (`CFRECT` flag), `src/window.c` (`reposition`). Test: `tests/test_editor.cpp`.

- [ ] **Step 1: Failing test.** Three consecutive `C-l` must frame point's line at three *different* rows (bug: all identical).
```cpp
TEST_CASE("C-l cycles recenter middle/top/bottom")
{
    auto dir = make_temp_dir();
    std::ofstream f(dir / "t.txt");
    for (int i = 1; i <= 40; i++) f << "line" << i << "\n";
    f.close();
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    auto last_row = [](const std::string &s) {
        int row = -1; size_t i = 0;
        while ((i = s.find("\x1b[", i)) != std::string::npos) {
            int r = 0, c = 0; char t = 0;
            if (std::sscanf(s.c_str() + i, "\x1b[%d;%d%c", &r, &c, &t) == 3
                && (t == 'H' || t == 'f')) row = r;
            i += 2;
        }
        return row;
    };
    int r1 = -1, r2 = -1, r3 = -1;
    if (wait_for(master, "line1", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1b>", 2);           // M-> end, so point is deep in buffer
        (void)!::write(master, "\x10\x10\x10\x10\x10", 5); // C-p x5 up a bit
        drain(master, 300);
        (void)!::write(master, "\x0c", 1); r1 = last_row(drain_str(master, std::chrono::seconds(1)));
        (void)!::write(master, "\x0c", 1); r2 = last_row(drain_str(master, std::chrono::seconds(1)));
        (void)!::write(master, "\x0c", 1); r3 = last_row(drain_str(master, std::chrono::seconds(1)));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    // middle, then top (smaller row), then bottom (larger row): three distinct.
    CHECK(r1 > 0); CHECK(r2 >= 0); CHECK(r3 > 0);
    CHECK(r2 < r1);   // top is above middle
    CHECK(r3 > r2);   // bottom is below top
}
```
- [ ] **Step 2: Run → FAIL** (today all three center → identical rows).
- [ ] **Step 3: Add `CFRECT` (`src/def.h`)** beside `CFYANK`:
```c
#define CFRECT	0x0010		/* Last command was a recenter (C-l) */
```
- [ ] **Step 4: Cycle in `reposition` (`src/window.c`).** Replace the body:
```c
int
reposition(int f, int n)
{
	static int	step;		/* 0 middle, 1 top, 2 bottom */

	if (f & FFARG) {
		/* explicit arg positions dot at line n; not part of the cycle */
		curwp->w_frame = (n >= 0) ? n + 1 : n;
	} else {
		step = (lastflag & CFRECT) ? (step + 1) % 3 : 0;
		if (step == 0)
			curwp->w_frame = 0;			/* middle */
		else if (step == 1)
			curwp->w_frame = 1;			/* top */
		else
			curwp->w_frame = curwp->w_ntrows;	/* bottom */
		thisflag |= CFRECT;
	}
	curwp->w_rflag |= WFFRAME;
	sgarbf = TRUE;
	return (TRUE);
}
```
(`w_frame` semantics per `display.c:709`: 0 → `ntrows/2` (middle), 1 → row 0 (top), `ntrows` → clamped to row `ntrows-1` (bottom).)
- [ ] **Step 5: Run → PASS.**
- [ ] **Step 6: Commit** `git add src/def.h src/window.c tests/test_editor.cpp && git commit -m "feat(keys): C-l cycles recenter middle/top/bottom (recenter-top-bottom)"`

---

### Task 4: `C-x z` → repeat

**Files:** Modify `src/def.h` (`repeat` prototype), `src/kbd.c` (expose last fn + `repeat`), `src/keymap.c` (`cXmap` `z`), `src/funmap.c`. Test: `tests/test_editor.cpp`.

**Interfaces:** Produces `int repeat(int, int)`.

- [ ] **Step 1: Failing test**
```cpp
TEST_CASE("C-x z repeats the last command")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "a\nb\nc\nd\ne\nf\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool moved = false;
    if (wait_for(master, "a", std::chrono::seconds(8))) {
        (void)!::write(master, "\x01", 1);   // C-a line "a" (line 1)
        (void)!::write(master, "\x0e", 1);   // C-n : down to line 2
        (void)!::write(master, "\x18z", 2);  // C-x z : repeat C-n -> line 3
        (void)!::write(master, "z", 1);      // z again -> line 4
        (void)!::write(master, "\x0b", 1);   // C-k kill this line's text (line 4 = "d")
        (void)!::write(master, "\x18\x13", 2);
        moved = (bool)wait_for(master, "Wrote", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    std::string c = ss.str();
    fs::remove_all(dir);
    // C-n then repeat x2 lands on line 4 ("d"); C-k empties it.
    CHECK(c.find("d") == std::string::npos);
    CHECK(c.find("c") != std::string::npos);
}
```
- [ ] **Step 2: Run → FAIL** (`C-x z` unbound; line "d" not killed).
- [ ] **Step 3: Expose the last function + add `repeat` (`src/kbd.c`).** At file scope near the other globals (`int rptcount;` ~line 35), add:
```c
PF		 last_command;	/* last real command, for repeat (C-x z) */
```
In `mgwrap`, remove the local `static PF ofp;` and use `last_command`, and skip `repeat` too:
```c
mgwrap(PF funct, int f, int n)
{
	if (funct != rescan &&
	    funct != negative_argument &&
	    funct != digit_argument &&
	    funct != universal_argument &&
	    funct != repeat) {
		if (funct == last_command)
			rptcount++;
		else
			rptcount = 0;
		last_command = funct;
	}

	return ((*funct)(f, n));
}
```
Add the command (anywhere in `kbd.c`, e.g. after `mgwrap`):
```c
/*
 * C-x z: repeat the last command; press z (or the last key) again to repeat
 * more, matching Emacs's `repeat'.
 */
int
repeat(int f, int n)
{
	int	 c;

	if (last_command == NULL) {
		dobeep();
		ewprintf("No last command to repeat");
		return (FALSE);
	}
	for (;;) {
		if ((*last_command)(f, n) == FALSE)
			return (FALSE);
		update(CMODE);
		c = getkey(FALSE);
		if (c != 'z')		/* Emacs repeats while the last key is held */
			break;
	}
	ungetkey(c);			/* the non-z key resumes normal dispatch */
	return (TRUE);
}
```
- [ ] **Step 4: Prototype (`src/def.h`)** beside other `kbd.c` prototypes:
```c
int		 repeat(int, int);
```
- [ ] **Step 5: Bind `C-x z` (`src/keymap.c`).** Add a `cXz` array before `cXmap`:
```c
static PF cXz[] = {
	repeat			/* z */
};
```
In `cXmap`, change `KEYMAPE (6)` → `KEYMAPE (7)` and the two counts `6, 6,` → `7, 7,`, and add a 7th element after the `'^', 'u', cXcar` one (keeping ascending order):
```c
			{
				'^', 'u', cXcar, NULL
			},
			{
				'z', 'z', cXz, NULL
			}
```
- [ ] **Step 6: Funmap (`src/funmap.c`)** beside a nearby entry:
```c
	{repeat, "repeat", 1, NULL},
```
- [ ] **Step 7: Run → PASS.** Smoke: `C-x z` with no prior command beeps (no crash).
- [ ] **Step 8: Commit** `git add src/def.h src/kbd.c src/keymap.c src/funmap.c tests/test_editor.cpp && git commit -m "feat(keys): C-x z repeats the last command (repeat)"`

---

## Final verification (before PR)

- [ ] macOS: `cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-qkeys .`.
- [ ] `c-legacy`: `cmake --build --preset c-legacy` (0 warnings under `-Werror`).
- [ ] TSan (editor): `cmake --build build-tsan && ./build-tsan/tests/test_editor` (the 4 new cases).
- [ ] Manual smoke: `C-l` cycle; `M-g g`/`M-g M-g`/`M-g M-g M-g`; `C-x z z z`; `C-h k <key>`; and regressions: `M-[` arrows, `M-h` mark-paragraph, `C-x` bindings, `C-h c`.
- [ ] Update `todo.md`: mark the four done; record `FM-DABBREV` and the deferred `M-g` sub-bindings.
- [ ] Open the PR based on `neomg`.
