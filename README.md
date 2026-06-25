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
repo and build a full status view*. On **roll20-private-sheets (37,515 files)**:

| | Full status view (all sections) | Incremental (edit in one dir) |
|---|---|---|
| **Emacs + Magit** | **≈ 555 ms** | — (no incremental path; rebuilds fully) |
| **neomg** | **≈ 239 ms** (~2.3× faster) | **0.8 ms** (small dir) … **16 ms** (290-file dir) |

Both numbers are *measured the same way* — neomg's via `mg_magit_status_buffer`,
the actual buffer build that emits every section (not just the libgit2 status
list); Magit's via `magit-refresh`, corroborated three ways (below). Both are
roughly **flat clean→dirty**, because the cost is section-gathering, not the
diff. The ~2.3× on the full view is the honest headline.

The **incremental** column is where the architecture pays off: Magit has no
incremental path (every refresh is a full rebuild), while neomg rescans just the
edited directory on its warm handle — **independent of the repo's 37k-file
total**. That's not a like-for-like "2.3× → 700×" claim: it's a different, cheaper
operation that one-shot tools simply can't do.

Honest caveats, stated plainly:

- neomg's status buffer shows a **file list** (diffs expand on `TAB`); Magit
  renders diffs **inline**. On a heavily-dirty tree Magit does more rendering —
  so part of the full-view gap is a UX choice (lazy vs eager diffs), not pure
  engine speed.
- neomg's libgit2 walk is **not** faster than git's hand-tuned C; a bare
  `repo_status` is ~146 ms here. The wins are the *incremental* path and that the
  full build runs **off the UI thread**, so the editor never blocks.

### Where the lighter tools sit (reference, not same-basis)

These do *less* than a full Magit/neomg view, so they're a reference point, not a
head-to-head — and we report them by architecture, since none expose an internal
status timer:

- **`git status`** (the change list only, not a full view): ~40 ms default
  config here, ~25 ms with the built-in `fsmonitor` warm, ~82 ms with the
  untracked cache disabled.
- **lazygit** (shells out to `git`) and **gitui** (libgit2) rebuild their view
  **one-shot per refresh** with no persistent incremental session — each refresh
  is bounded by a full status pass (≈ the `git status` and libgit2-walk figures
  respectively). They don't get cheaper when little changed.

### Method

- Machine: Apple Silicon (arm64), macOS, `git` 2.50.1; **roll20-private-sheets,
  37,515 tracked files**, measured clean and dirty (30 changes in one dir;
  restored + verified clean after).
- neomg: the committed [`bench/status_bench.cpp`](bench/status_bench.cpp)
  (`neomg_bench <repo> [dir]`), min + median of 9 runs, calling the real engine
  paths (`mg_magit_status_buffer` for the full view; `session::status_scoped`
  for the incremental one).
- Emacs + Magit: real Emacs 30.2 (Magit in an isolated package dir), **three
  independent measurements that converge**: outer `float-time` (~555 ms), Magit's
  own `magit-refresh-verbose` total (~560 ms), and a live-frame `benchmark-run`
  via `emacs --daemon` + `emacsclient` (~549 ms). The per-section breakdown
  confirms the cost is synchronous `git` subprocesses, not redraw.

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

Like `mg`, **neomg's own code is in the public domain**. A built binary also
links libgit2 (GPLv2 *with a linking exception* — which is exactly what makes a
public-domain front end legitimate), plus utf8proc and ncurses. Full details and
notices: **[LICENSING.md](LICENSING.md)**.

[mg]: https://github.com/troglobit/mg
[Magit]: https://magit.vc
[README.upstream.md]: README.upstream.md
