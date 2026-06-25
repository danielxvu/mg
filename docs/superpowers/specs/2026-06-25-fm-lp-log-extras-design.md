# FM-LP log extras — `--graph`, commit-range, and pickaxe (`-S`/`-G`)

## Why

The dedicated log buffer (`l l` log all, `l f` log file) covers linear history
and act-at-point (RET visit / V revert / T note / g refresh), but three log
features magit users rely on are missing:

- **`--graph`** — the ASCII commit DAG (lanes + `*`/`|`/`\`/`/` connectors) that
  shows branch/merge topology.
- **commit-range** logs — `git log A..B`, the commits in a range.
- **pickaxe** — `git log -S<string>` (commits that change the number of
  occurrences of a string) and `git log -G<regex>` (commits whose diff contains
  a line matching the regex): "which commit touched this code?"

**libgit2 has no equivalent for graph art or pickaxe** — no graph renderer, no
pickaxe in `git_revwalk`. So those two can only be done correctly by running
real `git`. This is consistent with the FM-GIT-CLI-WRITES read/write-split
principle: route to the `git` CLI exactly where libgit2 has no equivalent (not
"moving reads for speed" — it's the only correct option). Commit-range is the
one part libgit2 *can* do (`git_revwalk_push_range`), but it rides the same CLI
reader for uniformity.

## Approach

Add a **single CLI-backed log reader** used only by the new modes. The existing
`l l` / `l f` (libgit2 `recent_commits` / `log_file`) and the status buffer's
inline "Recent commits" section stay on libgit2, untouched and fast — matching
the rule that reads stay on libgit2 where it works. The new reader reuses
`detail::run_git` (argv array, never a shell) from FM-GIT-CLI-WRITES.

All new modes produce the **same `MG_LINE_COMMIT` row model** as the existing
log, so the existing `magit-log-mode` keymap (RET / V / T / g) works for every
variant with no change.

## Engine (`mg.git`)

```cpp
struct log_row {
    std::string text;  // display: <graph art> + short_oid + " " + summary,
                       //          OR a pure graph-connector line
    std::string oid;   // full 40-hex sha for a commit row; "" for a connector line
};

struct log_options {
    std::size_t max_count = 0;   // 0 = no -n limit
    bool        graph     = false;
    std::string range;           // "" = default (HEAD); else e.g. "main..HEAD"
    std::string file;            // "" = repo-wide; else restrict to a path
    char        pickaxe   = 0;   // 0 = none, 'S' = occurrence-count, 'G' = regex
    std::string pickaxe_term;    // the -S string / -G regex
};

std::expected<std::vector<log_row>, error>
log_query(std::string repo, log_options opts);
```

Builds and runs, via `detail::run_git` (capture):

```
git -C <repo> log [--graph] [-n N] [-S<term> | -G<term>] [<range>]
    --format=%x1f%H%x1f%h%x1f%s [-- <file>]
```

The leading `%x1f` is deliberate: with `--graph`, git prefixes each commit line
with graph art *before* the format string, so the line is
`<art>\x1f<full>\x1f<short>\x1f<summary>`. Parse per line by splitting on the
US separator `\x1f`:

- **≥ 4 fields** → a commit row: `text = field[0] (graph art) + field[2] (short)
  + " " + field[3] (summary)`, `oid = field[1] (full)`.
- **no `\x1f`** → a pure connector line (graph mode only, e.g. `| |` / `|\`):
  `text = line`, `oid = ""`.

Graph art is only `*|/\_ ` + spaces, so it never contains `\x1f`; `%s` (subject)
is a single line and will not in practice contain it. A non-zero git exit maps
to `std::unexpected(error{...})` carrying git's output (e.g. a bad range).

Pickaxe semantics are git's exactly: `-S<term>` is literal occurrence-count by
default; `-G<term>` is a regex over diff content. No reimplementation.

## Bridge

```c
/* path = full oid on MG_LINE_COMMIT rows; connector lines come as
 * MG_LINE_OTHER with path = NULL. pickaxe_kind: 0 none / 'S' / 'G'. */
int mg_magit_log_query_buffer(const char *repo_path, int graph,
        const char *range, const char *file, int pickaxe_kind,
        const char *pickaxe_term, int n,
        mg_magit_emit_fn emit, void *ctx);
```

Calls `mg::git::log_query`, emits each `log_row`: commit rows as
`MG_LINE_COMMIT` (path = `oid`), connector rows as `MG_LINE_OTHER` (path =
NULL). Returns the number of lines emitted, 0 on failure.

## UI (`magit_cmd.c`) — new `l` menu entries

The `l` log menu gains four entries (kept in ascending key order, enforced by
`magit_assert_keymap_sorted`):

- **`l g`** — graph log of HEAD (`graph = true`).
- **`l r`** — prompt `"Log range: "` (e.g. `main..HEAD`) → range log; abort on
  empty input.
- **`l s`** — prompt `"Pickaxe -S (string): "` → `pickaxe = 'S'`.
- **`l G`** — prompt `"Pickaxe -G (regex): "` → `pickaxe = 'G'`.

All reuse the existing `-n` infix (`magit_log_limit`) and populate the **same**
log buffer + `magit-log-mode` as `l l`/`l f`, so act-at-point is unchanged.
Connector lines carry no oid, so RET/V/T no-op on them. Everything is under
`#ifdef ENABLE_NATIVE_MAGIT`; the OFF build is unaffected.

## Testing (TDD)

Engine (`test_git`):
- **graph**: a repo with a real merge (two divergent branches merged) → the rows
  include the merge commit and both parents, at least one connector line exists
  (`oid == ""`), and a known commit's `oid` is correct.
- **range**: `log_query(repo, {.range = "A..B"})` returns exactly the commits in
  `(A, B]` (cross-check against the existing interactive-rebase fixture).
- **pickaxe `-S`**: a repo where a distinctive string is introduced in commit X
  and removed in commit Y → `-S<string>` returns X and Y and not unrelated
  commits.
- **pickaxe `-G`**: a regex matching a changed line returns the commits whose
  diff matches.

Bridge (`test_bridge`):
- `mg_magit_log_query_buffer` with `-S` finds the introducing commit; a graph
  run emits at least one `MG_LINE_OTHER` connector line.

UI:
- tmux smoke test: `l g` renders graph art in the log buffer; `l s <string>`
  populates the buffer with the matching commit; RET on a commit row still
  visits its diff.

Cross-platform: macOS `ctest --preset cpp` + Alpine container both green; OFF
build (`c-legacy`) has 0 magit symbols.

## Out of scope (YAGNI)

- Per-commit graph coloring / decoration refs (`--decorate`).
- `--all` / multi-ref graph (HEAD only for v1).
- Pickaxe with a separate file restriction surfaced in the UI (the engine's
  `file` field supports `-- <file>`, but no dedicated UI prompt yet — a trivial
  later add).
- Toggling graph on the existing `l l` buffer (graph is its own `l g` entry,
  matching mg's one-entry-per-variant menu model).

## Phases

1. ✅ **DONE** — **Engine `log_query`** + tests (graph / range / `-S` / `-G`).
2. ✅ **DONE** — **Bridge `mg_magit_log_query_buffer`** + tests.
3. ✅ **DONE** — **UI** `l g` / `l r` / `l s` / `l G` + the menu wiring;
   tmux-verified (graph art + merge commit for `l g`; `l s` finds the
   introducing commit).

🎉 **FM-LP COMPLETE.** macOS + Alpine 205/205, OFF build 0 magit symbols.
Branch `fm-lp-log-extras`, stacked on `fm-git-cli-writes`.

Each phase: macOS + Alpine + the OFF build green.
