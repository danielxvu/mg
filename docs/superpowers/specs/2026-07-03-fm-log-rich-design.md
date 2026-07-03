# FM-LOG-RICH — decorated, colored log rows

## Why

`*magit-log*` shows `<short-oid> <summary>` with no indication of which
branches/tags point at a commit. `git log --decorate` (and Magit) show
` (HEAD -> master, origin/main, tag: v1.0)` after the oid, colored by ref kind.
This is the highest-visibility gap left in the log buffer.

Scope note: todo.md's FM-LOG-RICH entry also listed an `--all` multi-ref graph —
that already works (`--all` shipped in FM-TRANSIENT-DEPTH, `--graph` in FM-LP;
`log_query` emits both together). The per-file pickaxe UX is deferred (below).
This feature is **decoration + coloring**.

## Decided model (from brainstorming)

Always-on decoration (like `git log --decorate`; no new transient key), colored
by ref kind via the existing per-cell color system:

| segment | kind | color (existing `ttfgkind` palette) |
|---------|------|--------------------------------------|
| `HEAD` / the `HEAD ->` arrow | 4 | cyan |
| branch after `HEAD -> ` | 2 | green |
| `tag: <name>` | 5 | yellow |
| name containing `/` (remote) | 1 | blue |
| other name (local branch) | 2 | green |
| parens, commas | 0 | default |

## Architecture

**Two log paths need decoration** (a gap caught in plan review): the default
`l l` log and per-file `l f` log are composed from **libgit2** commits in the
bridge (`mg_magit_log_buffer` / `mg_magit_log_file_buffer` →
`short_oid + " " + summary`); only filtered logs take the CLI `log_query` path.
So: the CLI path uses git's `%d`, and the libgit2 paths use a new engine
**decorations map**. Both produce the same text shape, which is all the
colorizer depends on. (The two sources may order refs slightly differently —
cosmetic, documented here.)

### 1a. Engine — CLI path (`src/magit/git.cppm`, `log_query`)

The log format gains git's `%d` decorate placeholder as its own `%x1f` field:
`--format=%x1f%H%x1f%h%x1f%d%x1f%s` — the fields are then graph art, full oid,
short oid, decoration, summary. Git renders `%d` as ` (HEAD -> master, tag: v1)`,
or empty. The row parser trims `%d`'s leading space and composes the row text
directly (`log_row` gains **no** new member — nothing consumes the decoration
separately, YAGNI):
- decorated: `<short> (HEAD -> master, tag: v1) <summary>` (git-oneline order)
- undecorated: `<short> <summary>` — **byte-identical to today**.

Graph connector lines (no oid fields) are unchanged.

### 1b. Engine — the decorations map (`src/magit/git.cppm`, new function)

`decorations(std::string repo) -> std::expected<std::map<std::string,
std::string>, error>`: full-oid → composed decoration (no parens), e.g.
`"HEAD -> master, origin/main, tag: v1"`. Built over libgit2:
- HEAD: `git_repository_head` — symbolic ⇒ remember the checked-out branch
  (its segment renders `HEAD -> <name>`); detached ⇒ a bare `HEAD` segment at
  HEAD's oid.
- Branches: `git_branch_iterator` with `GIT_BRANCH_ALL` (local + remote),
  name via `git_branch_name`, oid via the resolved reference target.
- Tags: `git_reference_iterator_glob_new(repo, "refs/tags/*")`, each **peeled
  to the commit** (`git_reference_peel(GIT_OBJECT_COMMIT)`) so annotated tags
  decorate the commit they tag, segment `tag: <name>`.
- Segment order per oid: the `HEAD -> ` branch first, then local branches,
  remote branches, tags (iterator order within groups).

### 2. Bridge (`src/magit/bridge.cpp`)

`mg_magit_log_buffer` and `mg_magit_log_file_buffer` fetch the decorations map
once per build and compose `<short> (<dec>) <summary>` when the commit's oid is
in the map, `<short> <summary>` otherwise (byte-identical). The CLI query path
(`mg_magit_log_query_buffer`) stays pass-through — `%d` already placed the
decoration correctly (including under `--graph`, where the bridge cannot know
where graph art ends). Reflog and rebase-todo rows are NOT decorated (scope).
No ABI change.

### 3. Editor coloring (`src/magit_cmd.c`; zero `display.c` changes)

The per-cell pipeline already exists: `display.c` gates on
`magit_is_color_buffer(bp)`, calls `magit_cell_color(bp, lp, ci)` per cell
(memoized per line in `magit_color_kindcol[1024]`), and renders kinds 0–7 via
the fixed `ttfgkind` SGR palette. Two extensions:
- `magit_is_color_buffer()` also returns true for the `*magit-log*` buffer
  (a new `magit_log_bp` cached pointer, set in the log build path, which also
  calls `magit_cell_color_reset()` after `bclear` like the other color buffers).
- `magit_cell_color()` gets a log-buffer branch: on a line's first queried cell,
  parse the line once into `magit_color_kindcol` (the same memo pattern the
  diff tokenizer uses), then O(1) lookups per cell.

**The decoration parser (one small function):** scan the line for a `(` that
starts the token immediately following a token of 7–12 hex chars (the short
oid — this rule, not "the first token", so graph-art prefixes like `* ` / `| `
on `--graph` rows don't break it); require a closing `)` and that **every**
comma-separated segment matches the ref grammar — `HEAD`, `HEAD -> <ref>`, `tag: <ref>`, or a bare `<ref>` (ref chars:
alnum `_ - / .`). If the group parses, fill the kinds per the table above; if
anything fails to parse, color nothing (kind 0 across the line).

**Why grammar-parse instead of span-passing:** the emit ABI
(`line, kind, path, hunk`) has no span channel; adding one is new ABI surface
for a cosmetic feature. The heuristic's worst failure mode is mis-coloring a
summary that begins with a parenthesized group that also parses entirely as
refs — cosmetic only, and the immediately-after-oid + full-grammar-match guards
make it rare. Coloring is never load-bearing.

## Error handling / edges

- Undecorated rows: text byte-identical to today; parser finds no group → no
  color. Graph connector rows: no oid token → parser bails → no color.
- A summary starting with `(...)`: colored only if every segment parses as a
  ref — acceptable cosmetic false positive (documented here).
- `%d` oddities (detached `HEAD`, `grafted`, `replaced`): whatever git prints;
  segments that fail the grammar simply disable coloring for that line.
- The 1024-column memo bound: cells beyond 1024 return kind 0 (existing
  behavior for diff lines; log lines are far shorter).

## Testing

- **Engine** (`test_git`): (a) `log_query` on a repo with a branch + tag → the
  head row's text contains `(HEAD -> ` and `tag:`; a non-tip commit's row is
  **byte-identical** to the pre-change composition; graph mode still parses
  (connector rows unchanged). (b) `decorations()` → the tip oid maps to a string
  containing `HEAD -> ` and `tag: v1`; a non-tip oid is absent; an annotated
  tag decorates the tagged commit (peel test).
- **Bridge** (`test_bridge`): `mg_magit_log_buffer` (the default-log path)
  emits a head row containing `(HEAD -> ` — proving the libgit2 path decorates,
  not just the CLI path.
- **Editor** (`test_editor`, pty): open `*magit-log*` on a `make_repo` repo and
  assert `(HEAD ->` renders; additionally assert the pty stream contains the
  **SGR bytes** `\033[36m` (cyan) adjacent to `HEAD` — `ttfgkind` writes fixed
  escapes, so this deterministically proves the colorizer fired, not just the
  text. Single transient-free session, presence-only, drained exit via
  `quit_neomg`.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (recorded in todo.md)

- Per-file pickaxe UX (`l s`/`l G` scoped to a file) — the engine already
  supports `-S<term> -- <file>`; the prompt flow is its own small slice.
- A `--decorate` on/off toggle in the `l` transient.
- Decorating `*magit-reflog*` / commit-view headers.
