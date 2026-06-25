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


Performance: status refresh on a large repo
-------------------------------------------

### Why this is the benchmark that matters

A git UI's job is to show you the *current* state and keep it current. The cost
that dominates interactive feel is **status latency** — how long after a change
until the view is right. There are two regimes:

- **Cold** — first open, or after a `.git` change (commit, branch switch): the
  whole worktree must be walked once.
- **Warm / incremental** — the common case: you edit one file, and only the
  directory it's in needs rescanning.

Most git TUIs are **one-shot**: each refresh recomputes a *full* status — via the
`git` CLI (lazygit, tig) or a libgit2 walk (gitui). They re-pay the cold cost
every time. That's the same problem `git` itself solves with `fsmonitor`.

neomg takes it further. It's a **persistent process that watches the
filesystem** (kqueue on macOS, inotify on Linux), holds **one reused libgit2
handle**, and on a worktree change recomputes status **scoped to just the changed
directory** — off the UI thread, so typing never blocks. The warm refresh is the
number to watch.

> Honest framing: neomg's **cold** open is *not* faster than git/gitui — it's
> libgit2, and comparable (a touch slower than the `git` CLI's hand-tuned
> walk). The win is entirely in the **warm incremental** path, which is the case
> you actually hit while working.

### Method

- Machine: Apple Silicon (arm64), macOS, `git` 2.50.1.
- Repo: a real one — **roll20-private-sheets, 37,515 tracked files**. Measured
  both **clean** and **dirty** (30 changes — 25 modified + 5 untracked — in one
  directory; the files were restored and the tree confirmed clean afterward).
- neomg numbers: the committed [`bench/status_bench.cpp`](bench/status_bench.cpp)
  (`neomg_bench <repo> [dir]`), median of 9 runs, calling the same engine path
  the status buffer is built from.
- `git status`: median of 7 CLI runs, OS cache warm. The headline figure is the
  **default config** (~40 ms); it ranges from 25 ms (built-in `fsmonitor` warm)
  to ~82 ms (untracked cache disabled).
- **Emacs + Magit** (the UI neomg reimplements): measured directly and
  **corroborated three ways** on real Emacs 30.2 (Magit in an isolated package
  dir): an outer `float-time` (~555 ms), Magit's own `magit-refresh-verbose`
  total (~560 ms), and a live-frame `benchmark-run` via `emacs --daemon` +
  `emacsclient` (~549 ms) — all converge, and the per-section breakdown confirms
  the cost is synchronous `git` subprocesses, not redraw.
- lazygit/tig and gitui expose no internal status timer, so they're reported by
  the **cost of the status pass each is built on** — lazygit/tig → the
  `git status` figure, gitui → a libgit2 full status (the same library and walk
  as neomg's cold column). That's the architectural lower bound, not an
  instrumented measurement of their event loop. Reproduce the neomg/git/Magit
  numbers yourself with the commands below.

### Results (roll20-private-sheets, 37,515 files)

| Tool | Refresh model | Cold full | Warm refresh — clean | Warm refresh — 30 changes in one dir |
|------|---------------|-----------|----------------------|--------------------------------------|
| **stock `mg`** | no git UI | n/a | n/a | n/a |
| `git status` (reference) | one-shot | 40 ms (25 ms `fsmonitor`) | — | 40 ms |
| **lazygit** (git CLI) | one-shot, re-walks | ≈ 40 ms | ≈ 40 ms | ≈ 40 ms |
| **gitui** (libgit2) | one-shot, re-walks | ≈ 146 ms | ≈ 146 ms | ≈ 148 ms |
| **Emacs + Magit** | one-shot full refresh | ≈ 555 ms | **≈ 555 ms** | **≈ 560 ms** |
| **neomg** | persistent + incremental | 146 ms (first open only) | **0.8 ms** (small dir) … **16 ms** (290-file dir) | **16 ms** (same dir) |

Two things the dirty column makes clear:

- **Everyone else is flat clean→dirty because they redo a fixed full pass.**
  Magit (~555→560 ms) and the one-shot TUIs don't get cheaper when little has
  changed — they recompute everything every refresh.
- **neomg's incremental refresh scales with the *edited directory*, not the
  repo or the diff.** Editing in a small dir → sub-millisecond; in a 290-file dir
  → ~16 ms — and that's ~the same whether the files are clean or dirty (it's the
  directory walk, not the changes). Either way it's independent of the repo's
  37k-file total, which is the whole point.

The headline comparison is **Emacs + Magit**, the porcelain neomg reimplements:
same UI, but Magit runs *many* `git` subprocesses per refresh (status, diffs,
stashes, unpushed/unpulled logs, …) for **~555 ms every time**, with no
incremental path. neomg's **cold** open (146 ms) already beats one Magit refresh
~4×; its **warm** refresh is **35–700× faster** (16 ms down to sub-ms) because it
watches the filesystem and rescans only the changed directory on a reused
handle, off the UI thread.

### Reproduce it

```sh
cmake --build --preset cpp-linux --target neomg_bench   # or --preset cpp on macOS
./build/tests/neomg_bench /path/to/repo              # cold full + warm scoped (first subdir)
./build/tests/neomg_bench /path/to/repo some/subdir  # scope to a specific directory

# git baseline:
git -C /path/to/repo status --porcelain                          # default config
git -C /path/to/repo -c core.fsmonitor=true status --porcelain   # fsmonitor warm (run twice)

# Emacs + Magit (its own instrumentation):
emacs -Q --batch --eval '(progn (require (quote magit)) (setq magit-refresh-verbose t) \
  (let ((default-directory "/path/to/repo/")) (magit-status-setup-buffer default-directory)))'
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

Like `mg`, neomg is **free and in the public domain**.

[mg]: https://github.com/troglobit/mg
[Magit]: https://magit.vc
[README.upstream.md]: README.upstream.md
