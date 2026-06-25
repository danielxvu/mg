neomg
=====

**neomg** is a fork of [`mg`][mg] — the public-domain Micro Emacs clone — that
adds a **native [Magit][]-style git porcelain** and a modern C++23 engine, while
staying the same small, fast, no-config terminal editor. It installs as `neomg`
so it lives happily next to your distro's stock `mg`.

If you like `mg`'s speed and Emacs muscle memory but miss Magit when you drop to
a terminal, that's the gap neomg fills — `C-x g` opens a real status buffer:
stage/unstage hunks, commit, branch, stash, rebase (interactive too), cherry-pick,
ediff conflict resolution, blame, and a log browser — driven straight from the
repository, updating itself as files change.

> neomg is a fork. For the editor fundamentals it inherits unchanged — the
> keybinding model, philosophy, and history — see **[README.upstream.md][]**.


What's different from `mg`
--------------------------

`mg` has no git integration at all. Everything below is net-new in neomg, behind
a build-time `ENABLE_NATIVE_MAGIT` flag (the OFF build is byte-for-byte upstream):

| Area | stock `mg` | neomg |
|------|-----------|-------|
| Git UI | none | a `*magit-status*` buffer (`C-x g`): sections, fold, navigate |
| Staging | — | file **and hunk/region** stage / unstage / discard |
| Commit | — | commit / amend / extend / reword, through real `git` (**your hooks + signing run**) |
| Branch / stash / tag | — | create / checkout / delete / apply / drop, + sections |
| Merge / cherry-pick / revert | — | through real `git`; conflicts land in an ediff resolver (`e o`/`e t`, `c c`) |
| Rebase | — | non-interactive **and** interactive (`*git-rebase-todo*`: pick/squash/fixup/reword/edit, reorder) |
| Remotes | — | fetch / pull / push (incl. `--rebase`, `--force-with-lease`, `-u`) via real `git`, so your **credential helper / SSH agent / GPG** work |
| Log | — | `l l` all · `l f` file · **`l g` graph** · **`l r` range** · **`l s`/`l G` pickaxe** |
| Blame, bisect, worktrees, submodules, notes, gitignore | — | all present |
| Engine | C | C++23 named modules over libgit2, async status off the UI thread |

### The read / write split

neomg reads git state with **libgit2** (fast, in-process, no subprocess) but
routes **mutations that must honour hooks, signing, or credentials** —
commit/amend, merge/cherry-pick/revert, push/pull/fetch — through the **real
`git` binary**. So behaviour is exactly git's where it matters (a `pre-commit`
hook that rejects actually blocks your commit; `commit.gpgsign` signs; your
credential helper prompts), with libgit2's speed for everything else.

### What it doesn't do (vs Magit)

neomg is a Magit-*inspired* porcelain covering the everyday workflow — it is not
a Magit reimplementation, and Magit is far deeper. If you rely on these, stay on
Emacs + Magit:

- **No `forge`** (GitHub/GitLab issues & PRs), no Gerrit/review integrations.
- **No elisp extensibility** — you can't script it the way Magit users extend
  everything; the keymap and sections are what's built in.
- **Shallower transients** — argument/infix menus exist but cover common flags,
  not Magit's exhaustive set.
- **Logs aren't the full Magit log** — no `--decorate` refs coloring, no `--all`
  multi-ref graph (HEAD only), pickaxe is repo-wide (no per-file UI prompt yet).
- **Edges remain** — e.g. a conflicting squash/fixup mid interactive-rebase
  resolves as a normal pick rather than folding. See [`todo.md`](todo.md) for the
  honest running list.

It's young (one author, no tagged release yet). The git *engine* is well-tested
(200+ tests, Linux + macOS, TSan-clean), but treat it as beta and keep backups.


Performance: status refresh on a large repo
-------------------------------------------

> The biggest *practical* difference from other tools isn't speed — it's
> correctness (your hooks, signing, and credential helper actually run; see
> [the read/write split](#the-read--write-split)). That can't be benchmarked, so
> it's easy to overlook. On **speed**, the honest comparison is below.

### What the number means

A git UI's job is to build a **status view** (branch, ahead/behind, stashes,
changes, recent commits, …) and keep it current. Two regimes:

- **Cold / full** — first open, or after a `.git` change: build the whole view.
- **Warm / incremental** — the common case: you edit one file; only its
  directory needs rescanning.

Other UIs are **one-shot**: every refresh rebuilds the full view — via the `git`
CLI (lazygit, tig), a libgit2 walk (gitui), or many `git` subprocesses (Magit).
neomg is a **persistent process that watches the filesystem** (kqueue/inotify),
holds **one reused libgit2 handle**, and on a worktree change rescans **only the
changed directory**, off the UI thread.

### The same-basis comparison: neomg vs Emacs + Magit

Magit is the porcelain neomg reimplements, so it's the fair fight — both *open a
repo and build a full status view*. Measured across three repo sizes (median;
n=7; min/stdev in the harness output):

| Repo (tracked files) | neomg full view | Emacs + Magit full refresh | neomg incremental (1-dir edit) |
|---|---|---|---|
| transmission (1.8k) | **16 ms** | 210 ms (~13×) | **0.2 ms** |
| d20app (7.3k) | **85 ms** | 552 ms (~6.5×) | **0.4 ms** |
| roll20 (37.5k) | **243 ms** | 558 ms (~2.3×) | **0.8 ms** |

Both numbers are *measured the same way* — neomg's via `mg_magit_status_buffer`
(the real buffer build emitting every section, not just the libgit2 status list);
Magit's via `magit-refresh`, corroborated three independent ways (see Method).

Two honest takeaways the spread forces:

- **neomg's full-view lead over Magit shrinks as the repo grows** — ~13× at 1.8k
  files, ~2.3× at 37k. Magit's ~550 ms is largely fixed subprocess overhead that
  doesn't scale; neomg's full view is a libgit2 walk that *does* scale with file
  count. On a giant monorepo, expect ~2×, not 13×. Anyone who quotes a single
  multiplier is cherry-picking — including past versions of this README.
- **The incremental refresh is the durable win**: 0.2–0.8 ms at *every* size,
  because it rescans the edited directory, not the repo. Magit, lazygit, and
  gitui have no equivalent — every refresh is a full rebuild. This is the
  architectural point, and it's flat across repo size by construction.

Caveats, stated plainly:

- neomg's status buffer shows a **file list** (diffs expand on `TAB`); Magit
  renders diffs **inline**, so on a heavily-dirty tree part of the full-view gap
  is a UX choice (lazy vs eager diffs), not pure engine speed.
- neomg's libgit2 walk is **not** faster than git's hand-tuned C (see the `git
  status` row below). The wins are the *incremental* path and that the full build
  runs **off the UI thread**, so the editor never blocks.

### Reference: the lighter tools (different task / different metric)

`git status` computes a *change list* (less than a full view); lazygit/gitui are
measured as **launch → first render** via a tmux pty — that includes process
startup, so it's a rougher, *different* metric, not per-refresh latency. Shown
for context, not as a head-to-head:

| Repo | `git status` (default / fsmonitor) | lazygit launch→render | gitui launch→render |
|---|---|---|---|
| transmission (1.8k) | 15 / 11 ms | 150 ms | 38 ms |
| d20app (7.3k) | 300 ms\* / 85 ms | 347 ms | 44 ms |
| roll20 (37.5k) | 40 / 25 ms | 285 ms | 38 ms |

\* d20app's `git status` is anomalously slow (slower than the 5× larger roll20) —
almost certainly submodule recursion or a large untracked set. Flagged, not
leaned on. lazygit shells out to `git` and gitui uses libgit2, so per-refresh
each is bounded by a full status pass (no persistent incremental session).

### Method

- One committed harness — [`bench/bench.py`](bench/bench.py) driving
  [`bench/status_bench.cpp`](bench/status_bench.cpp) — runs every tool and reports
  **min / median / stdev** over *n* runs. All measurements are **read-only**.
  Run it on your own repos (see below); a CI job
  ([`.github/workflows/bench.yml`](.github/workflows/bench.yml)) reruns the
  neomg + `git` portion on **x86_64 Linux**, so the numbers aren't a
  single-machine artifact.
- These numbers: Apple Silicon (arm64), macOS, `git` 2.50.1, n=7. Repos measured
  as-is (transmission/d20app carried a few local changes — negligible for these
  walk-dominated full passes).
- neomg via the real engine paths (`mg_magit_status_buffer`,
  `session::status_scoped`). Magit via real Emacs 30.2 with Magit auto-installed
  in an isolated package dir; `magit-refresh` corroborated three ways earlier
  (outer `float-time`, Magit's own `magit-refresh-verbose`, and a live-frame
  `benchmark-run`), all within ~2% — the cost is synchronous `git` subprocesses,
  not redraw.

### Reproduce it

```sh
cmake --build --preset cpp-linux --target neomg_bench   # or --preset cpp on macOS
# Measure every tool it can find (neomg, git, Magit if emacs is present, and
# lazygit/gitui via tmux) across one or more repos, with min/median/stdev:
python3 bench/bench.py /path/to/repo [/path/to/another-repo ...]
```


Install
-------

### Arch Linux (AUR)

A `neomg-git` PKGBUILD lives in [`packaging/aur/`](packaging/aur/PKGBUILD):

```sh
sudo pacman -S --needed clang llvm cmake ninja git libgit2 libutf8proc pkgconf
cd packaging/aur && makepkg -si
```

It builds the `cpp-linux` preset and installs `neomg` + `neomg.1` under `/usr`,
beside your existing `mg`.

### From source

```sh
# deps: a C++23 clang (with clang-scan-deps), cmake, ninja, libgit2, libutf8proc
cmake --preset cpp-linux          # macOS: cmake --preset cpp
cmake --build --preset cpp-linux
ctest --preset cpp-linux          # optional: run the suite
sudo cmake --install build --prefix /usr/local
neomg
```


Magit quickstart
----------------

| Key | Action |
|-----|--------|
| `C-x g` | open the status buffer |
| `s` / `u` | stage / unstage file or hunk at point |
| `k` | discard |
| `c c` | commit (`c a` amend, `c e` extend, `c w` reword) |
| `b` / `z` / `t` | branch / stash / tag menus |
| `l l` `l f` `l g` `l r` `l s` `l G` | log: all · file · graph · range · pickaxe -S · pickaxe -G |
| `r` | rebase menu (`r i` interactive) |
| `P` / `F` | push / pull menus |
| `B` · `Z` · `E` | blame · bisect · ediff conflict resolver |
| `g` / `q` | refresh / quit |
| `?` | in-buffer key legend |


Relationship to `mg` & license
-------------------------------

neomg tracks [`mg`][mg] (Micro Emacs, public domain, originally MicroEMACS v30
by Dave Conroy, 1985). The fork lives on the `neomg` branch; `master` mirrors
upstream `mg` for parity. The editor core — keybindings, modes, philosophy — is
unchanged from upstream and documented in **[README.upstream.md][]**.

Like `mg`, **neomg's own code is in the public domain**. A built binary also
links libgit2 (GPLv2 *with a linking exception* — which is exactly what makes a
public-domain front end legitimate), plus utf8proc and ncurses. Full details and
notices: **[LICENSING.md](LICENSING.md)**.

[mg]: https://github.com/troglobit/mg
[Magit]: https://magit.vc
[README.upstream.md]: README.upstream.md
