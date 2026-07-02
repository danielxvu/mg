# FM-TRANSIENT-LOG-MORE — more log args + a C-ABI struct cleanup

## Why

FM-TRANSIENT-DEPTH gave the `l` transient `--author`/`--grep`/`--all` (+ `-n`).
This adds the next everyday log filters — `--since`/`--until`, `--reverse`,
`--merges`/`--no-merges`. Doing so pushes the log-query C ABI from 10 data
params to ~15, so it also does the **struct refactor FM-TRANSIENT-DEPTH deferred**
("a struct is a separate cleanup") — the args become clean field-adds instead of
five more positional params.

## Decided model (from brainstorming)

Two parts:
1. **C-ABI struct refactor** (behavior-preserving): `mg_magit_log_query_buffer`
   takes a single `const struct mg_log_query *` instead of 10 positional params.
2. **Five new args** as `log_options` / struct fields + `log_infixes`:
   - `--since=<date>` / `--until=<date>` — VALUE infixes (date strings).
   - `--reverse` — FLAG (oldest-first).
   - `--merges` / `--no-merges` — FLAG each (only / exclude merges; mutually
     exclusive by convention — git errors if both, which we surface).

## Architecture

### Part 1 — the struct + refactor (`bridge.h`, `bridge.cpp`, `magit_cmd.c`)

Add to `bridge.h` a POD, all fields explicit:
```c
struct mg_log_query {
    const char *repo;          /* the repo path (was the old ABI's 1st param) */
    int         graph;
    const char *range;         /* NULL/"" = HEAD */
    const char *file;          /* NULL/"" = repo-wide */
    int         pickaxe_kind;  /* 0 / 'S' / 'G' */
    const char *pickaxe_term;
    int         n;             /* -n limit, 0 = none */
    const char *author;        /* NULL = none */
    const char *grep;          /* NULL = none */
    int         all;
    const char *since;         /* NULL = none */
    const char *until;         /* NULL = none */
    int         reverse;
    int         merges;
    int         no_merges;
};
int mg_magit_log_query_buffer(const struct mg_log_query *q,
                              mg_magit_emit_fn emit, void *ctx);
```
`bridge.cpp` maps `q`'s fields to `mg::git::log_options` (the same mapping as
today, plus the new fields; NULL strings stay empty). `magit_log_build`
(`magit_cmd.c:1552`) fills a `struct mg_log_query` from `magit_log_graph`/
`magit_log_range`/`magit_log_pickaxe`/`magit_log_limit` and the `log_infixes[]`,
then passes `&q`. This is the only caller.

### Part 2 — the new args (`git.cppm`, `magit_cmd.c`)

`mg::git::log_options` gains `std::string since; std::string until; bool reverse;
bool merges; bool no_merges;`. `log_query` appends, guarded (empty/false → nothing):
`--since=<since>`, `--until=<until>`, `--reverse`, `--merges`, `--no-merges`.

`log_infixes[]` gains (after `--all` at index 3):
`[4] --since` (VALUE, clearable), `[5] --until` (VALUE, clearable),
`[6] --reverse` (FLAG), `[7] --merges` (FLAG), `[8] --no-merges` (FLAG).
Keys must not collide with the log actions (`G`/`f`/`g`/`h`/`l`/`r`/`s`) or the
existing infixes (`n`/`a`/`m`/`A`); proposed `S` (since), `U` (until), `v`
(reverse), `M` (merges), `N` (no-merges) — verified in the plan.

`magit_log_build` reads the new infixes into the struct, and its
"any filter active → query path" routing condition gains the new infixes
(`[4].value`/`[5].value` non-empty, `[6]/[7]/[8].on`).

## Rendering

The `l` transient renders the new infixes via the existing
`magit_transient_render` (VALUE current-value, FLAG on/off) — no new render code.

## Error handling / edges

- All five default off/empty → the log argv is unchanged (byte-identical).
- Empty `--since=`/`--until=` ⇒ append nothing (like `--author`).
- `--merges` + `--no-merges` both on: git errors; the failure surfaces in the log
  (empty/error), not a crash — the transient does not police it (Magit doesn't).
- Date strings pass through to git verbatim (git parses "2 weeks ago", ISO, etc.).
- `--reverse` with `-n` follows git's own semantics (git applies `-n` before
  reversing) — unchanged, we just pass both.

## Testing

- **Engine** (`test_git`): a fixture with commits at distinct dates + a merge
  commit. `since`/`until` bound the returned set; `no_merges` excludes the merge;
  `merges` returns only it; `reverse` flips the order (assert first vs last row).
  Default (no fields) == the plain log.
- **Bridge** (`test_bridge`): `mg_magit_log_query_buffer(&q, …)` with the struct
  threads `--since`/`--no-merges`/etc. (emitted commit set reflects them).
- **Editor** (`test_editor`, pty): the `l` transient renders the new infixes
  (e.g. `--since` / `--no-merges`); toggling `--no-merges` (or setting `--since`)
  then running `l l` rebuilds `*magit-log*`. Single session, presence-only,
  drained exit via `quit_neomg`.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (deferred — tracked in todo.md if pursued)

- `--first-parent`, `--follow`, `--committer` (vs `--author`), author-date vs
  commit-date selection.
- Rebase/merge/reset transients (separate FM-TRANSIENT items).
