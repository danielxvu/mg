# FM-TRANSIENT-DEPTH (log args, first slice) — `--author`/`--grep`/`--all`

## Why

Magit's transient menus let you toggle command arguments before running. neomg's
infix infrastructure already exists (`magit_transient` renders + toggles `FLAG`
and `VALUE` infixes; `log_infixes` has `-n`, `push_infixes` has `-f`/`-u`), but
the menus only expose a couple of flags. todo.md lists FM-TRANSIENT-DEPTH (deepen
the arg menus toward Magit's set) as the deepest remaining gap.

It's broad (log/push/diff menus), so this is the **first slice**: the `l` log
transient — the richest and most-used, already with one infix (`-n`) to extend.
Push-extras and a diff-args transient are deferred to follow-up slices (tracked
in todo.md).

## Decided model — three new log infixes

Add to the `l` log transient (on top of the existing `-n`):
- **`--author=`** (VALUE) — filter by author substring.
- **`--grep=`** (VALUE) — filter by commit-message substring.
- **`--all`** (FLAG) — log all refs, not just HEAD.

These are sticky infixes (the existing `magit_transient` renders + toggles them
with no new transient code) and apply to the log actions (`l l`, `l f`).

## Architecture (extends the existing log path)

The log is git-CLI based: `mg::git::log_query` builds a `git log` argv
(git.cppm:~1561 — already appends `--graph`, `-n N`, `-S/-G`, `--format`, range,
`-- file`). The slice threads three more args through the existing chain.

### Engine (`mg.git`)
The `log_opts` (the internal options the args builder reads) gains:
`std::string author; std::string grep; bool all;`. The builder appends, when set:
`--author=<author>`, `--grep=<grep>`, `--all`. Empty strings / `all=false` append
nothing (default = today's behavior). Order doesn't matter to git; place them near
the existing `-n`/`-S` appends.

### Bridge (C ABI)
`mg_magit_log_query_buffer` gains three params after the existing ones —
consistent with its current per-arg style (a struct refactor of the 8-param ABI
is out of scope):
```c
int mg_magit_log_query_buffer(const char *repo_path, int graph,
    const char *range, const char *file, int pickaxe_kind,
    const char *pickaxe_term, int n,
    const char *author, const char *grep, int all,   /* NEW */
    mg_magit_emit_fn emit, void *ctx);
```
`author`/`grep` are NULL or "" when unset; `all` is 0/1. The wrapper passes them
into `log_opts`.

### Editor (`magit_cmd.c`)
- `log_infixes[]` gains three entries: `{ 'a', "--author", MAGIT_INFIX_VALUE, 0, "" }`,
  `{ 'g', "--grep", MAGIT_INFIX_VALUE, 0, "" }`, `{ 'A', "--all", MAGIT_INFIX_FLAG, 0, "" }`.
  (Keys chosen to not collide with the log menu's ACTION keys — the menu's
  actions are `l`/`f`/`g`/`r`/`s`/`G`/`h`; the transient checks infix keys before
  the action map, so an infix key shadows an action key. Pick infix keys that are
  NOT log actions: `a` (author), `=` or `G`-free… — confirm against the log
  action keys and choose non-colliding keys. Proposed: `a` author, `~` grep, `*`
  all — or whatever is free; the exact keys are a detail settled in the plan.)
- `magit_log_build` reads the infix state (like it already reads
  `log_infixes[0].value` → `magit_log_limit`) into `magit_log_author` /
  `magit_log_grep` / `magit_log_all`, and routes through
  `mg_magit_log_query_buffer` when ANY of graph/range/pickaxe/**author/grep/all**
  is active (extend the existing condition), passing the three new args. The plain
  `l l` with no infixes still uses the simple `mg_magit_log_buffer` (unchanged).
- `magit_log_query_reset` clears the new infix values/flag too (so a fresh `l`
  query starts clean), matching how it resets range/pickaxe today.

## Rendering

The `l` transient popup shows the new infixes with their state (the existing
`magit_transient_render` handles `FLAG` on/off + `VALUE` current-value display).
A filtered `*magit-log*` shows only matching commits; `--all` includes commits
from all refs.

## Error handling / edges

- All three default to inactive → the log is byte-identical to today.
- `--author=`/`--grep=` with an empty value (toggled then cleared): treat empty as
  unset (append nothing) — don't pass `--author=` with no value (git would match
  all / error oddly).
- A filter that matches nothing → an empty `*magit-log*` (valid, not an error).
- The infix keys must not collide with the log menu's action keys (the transient
  checks infixes first) — verified in the plan.
- `magit_log_limit` (the `-n` infix) interaction is unchanged.

## Testing

- **Engine** (`test_git`): build a repo with commits by two authors and distinct
  messages. `log_query(author="Alice")` returns only Alice's commits;
  `log_query(grep="fixbug")` returns only the matching message;
  `log_query(all=true)` on a repo with a commit on a non-HEAD branch includes it
  (where the default HEAD-only log does not). Default (no filters) == the plain
  log.
- **Bridge** (`test_bridge`): `mg_magit_log_query_buffer(..., author, grep, all,
  …)` threads the args (the emitted commit set reflects the filter).
- **Editor** (`test_editor`, pty): `l` shows the `--author`/`--grep`/`--all`
  infixes; toggling `--all` then running `l l` changes the rendered log (a
  non-HEAD commit appears). Robust pty patterns.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Phases (each independently testable)

1. **Engine** `log_opts` + the args builder (`--author`/`--grep`/`--all`) +
   `test_git` filter tests.
2. **Bridge** `mg_magit_log_query_buffer` gains the three params + `test_bridge`.
3. **Editor** the three `log_infixes` + `magit_log_build` wiring + reset + pty.

## Out of scope (deferred — tracked in todo.md)

- **FM-TRANSIENT-PUSH** — more push infixes (`--tags`, `--dry-run`) + a pull
  transient with args.
- **FM-TRANSIENT-DIFF** — a diff-args transient wrapping the FM-DIFF-CTL
  context/whitespace controls (currently the `+`/`-`/`w` keys).
- Other log args (`--since`/`--until`, `--reverse`, `--merges`/`--no-merges`),
  rebase/merge/reset transients.
- A struct-based refactor of the `mg_magit_log_query_buffer` C ABI (it grows by
  three params here; a struct is a separate cleanup).
