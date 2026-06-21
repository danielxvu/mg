# FM-TR — transient menu popups (magit's signature UI)

## Goal

Replace silent prefix submaps with magit-style **transient popups**: pressing a
prefix (`c`, `r`, `t`, `b`, `z`, `F`, `P`, `X`, `W`, `Z`, `l`) pops a window
listing the available keys + a human description, then reads one key and
dispatches it. This retrofits *every* menu already built, with no engine change.

This is the only "framework-feeling" magit gap left. The earlier worry that it
needs a TUI library is wrong: mg's own `display.c`/window engine + the existing
`help_help` pattern already provide everything. (ncurses is linked only as a
terminfo capability backend, not a widget layer — pulling in ncurses
`libmenu`/FTXUI/notcurses would fight `display.c`, not help.)

## What exists we reuse

- `help.c:help_help` — the exact loop: prompt, `funct = doscan(kp, getkey(FALSE),
  NULL)`, then `(*funct)(f,n)`. The transient is this in a popup.
- `doscan(KEYMAP*, key, NULL)` — resolves a key to its `PF` in a keymap. Our
  submaps (`magit_logmenu`, `magit_commitmenu`, …) stay the dispatch source of
  truth.
- `popbuf(bp, WNONE)` + `addlinef` + `bclear` — already used for `*magit-log*` /
  `*magit-blame*`. The popup is just another read-only buffer window.
- `update(CMODE)` — force redisplay before blocking on `getkey`.
- `magit_assert_keymap_sorted` — the existing self-check we extend.

## The gap: descriptions

`function_name(PF)` only resolves commands registered in the global symbol
table; magit submap functions are `static` in `magit_cmd.c` and bound directly,
so they have no name there. So each menu needs a small descriptor literal:

```c
struct magit_menu_item { KCHAR key; const char *desc; };
struct magit_menu {
	const char *title;                 /* e.g. "Log" */
	KEYMAP *map;                       /* the existing submap, for dispatch */
	const struct magit_menu_item *items;
	int n;
};
```

The descriptor carries *labels only*; dispatch stays via `map` + `doscan`, so
the two can't drift on the function pointer (and a test asserts every `items`
key exists in `map` and vice-versa).

## The generic transient

```c
static int
magit_transient(struct magit_menu *m, int f, int n)
{
	/* render into *magit-transient*, popbuf, update(CMODE) */
	/* k = getkey(FALSE); if ESC/C-g -> delete popup, return ABORT */
	/* fn = doscan(m->map, k, NULL); delete popup; */
	/* return fn ? (*fn)(f,n) : (ewprintf("%c is undefined", k), FALSE) */
}
```

Each prefix becomes a thin command, e.g.:

```c
static struct magit_menu log_menu = { "Log", (KEYMAP*)&magit_logmenu,
	(struct magit_menu_item[]){ {'l',"log all"}, {'f',"log file"} }, 2 };
static int magit_l_transient(int f, int n){ return magit_transient(&log_menu,f,n); }
```

and the `magitmap` entry flips from a prefix (`NULL` PF + `k_prefmap`) to a
real command (`magit_l_transient` PF + `NULL` prefmap):

```c
{ 'l', 'l', magit_l, NULL },   /* magit_l[] = { magit_l_transient } */
```

The submap struct is unchanged — only how we *enter* it changes.

## Slice boundary (chosen: popup **+** infixes)

**This slice: popup display for all 11 prefixes, AND sticky argument infixes
wired where the bridge already takes the argument** — so toggles do real work
immediately, with no bridge expansion:

- **Push (`P`)** — infixes `-f` force, `-u` set-upstream. `mg_magit_push(repo,
  remote, force, set_upstream)` *already* takes both flags. Action `p` pushes
  with the current toggle state (replacing the separate `P f`/`P u` commands).
- **Log (`l`)** — infix `-n` max-count (value, default 100).
  `mg_magit_log_buffer(repo, n, …)` *already* takes `n`. Actions `l`/`f` read it.

Menus without an arg-ready bridge (commit, tag, reset, branch, stash, rebase,
worktree, bisect, pull) show the **action items only** — faithful, since not
every magit transient has infixes. New infixes for those land when their bridge
op grows the argument (a per-op follow-up, not this slice).

The infix loop is magit-exact: the popup stays open while infix keys toggle
state and re-render; it closes only on an action key or ESC/C-g.

**Deferred: `--graph` rendering** (needs an engine graph layer) and
**nested/chained transients** (our menus are one level).

## TDD plan

1. **C self-check test (no tty):** extend the keymap-sorted assertion with a
   `magit_assert_menu_consistent` — every `magit_menu`'s items match its
   `map`'s keys exactly, and items are ascending. Runs at `magit_status`
   init like the existing sorted check. (Red: add a deliberately-missing
   description, watch it fail.)
2. **tmux end-to-end:** press `l` → assert the popup shows `Log`, `l  log all`,
   `f  log file`; press `f` → assert `*magit-log*` opens filtered. Repeat a
   spot-check for `c` (commit) and `Z` (bisect) so the prefix→command
   conversion is proven for representative menus.
3. **Regression:** every prior submap key still dispatches (`c c`, `r e`,
   `t t`, `Z s` …) — covered by the tmux spot-checks + the existing flows.

## Verification gates (per the standing bar)

- `cpp` + `c-legacy` build clean, 0 warnings.
- **OFF build still 0 magit symbols** (all under `ENABLE_NATIVE_MAGIT`).
- Keymap-sorted + new menu-consistent asserts pass.
- One stacked PR: `fm-tr-popup` → `fm-lp` (#62).

## Why this is the right next slice

Highest value-to-risk of the remaining residual: it's pure reuse of keymaps we
already own, no new dependency, no engine change, and it visibly upgrades
*every* menu at once — the single most "magit-like" thing still missing.
