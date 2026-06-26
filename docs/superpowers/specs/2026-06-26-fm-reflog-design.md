# FM-REFLOG — reflog view + reset-at-point (Magit-faithful undo)

## Why

The reflog is git's safety net: every move of `HEAD` (commit, reset, checkout,
rebase, merge…) is recorded, so any "lost" state is recoverable. neomg has no way
to *see* it — the engine only *writes* reflog messages (via `set_head_to`), never
reads them. This is the highest-value remaining Magit gap (todo.md "Magit-parity
gaps"): Magit's `l h` reflog browser + reset-to-here is how users undo mistakes.

This adds the **HEAD reflog view** and a **reset-at-point** action. Per the
design decision, there is **no one-key destructive "undo"** — the user always
picks the reset mode, so a reset is never a surprise. "Undo the last operation"
is simply: open `l h`, move to the `HEAD@{1}` line, reset there.

## Decided model

- **View:** `l h` opens a read-only `*magit-reflog*` buffer of HEAD's reflog,
  newest entry (`HEAD@{0}`) first. `RET` on an entry shows that entry's commit
  diff (reusing the `*magit-commit*` view). `g` refreshes, `q` closes.
- **Reset-at-point:** a reset prefix `x` → `x s` / `x m` / `x h` resets `HEAD` to
  the oid of the entry at point (soft / mixed / hard), reusing the existing
  `reset_to`. `x h` (hard, discards the working tree) **confirms first**.
- **HEAD reflog only** for v1 (current-branch `l H` and stash reflogs are a
  follow-up). Cap ~100 entries.

## Architecture (mirrors the FM-L `*magit-log*` 3-layer pattern)

### Engine — `mg.git`
```
struct reflog_entry {
    std::string oid;        // full oid HEAD pointed to AFTER this op (new oid)
    std::string short_oid;  // 8-char abbrev for display
    std::string selector;   // "HEAD@{i}"
    std::string message;    // git_reflog_entry_message, e.g. "commit: fix X"
};
std::expected<std::vector<reflog_entry>, error> reflog(std::string repo, int max);
```
Built from `git_reflog_read(&rl, repo, "HEAD")` → `git_reflog_entrycount(rl)` →
per-index `git_reflog_entry_byindex(rl, i)` → `git_reflog_entry_id_new(e)` (the
oid HEAD pointed to after the op) + `git_reflog_entry_message(e)`. `selector` is
`"HEAD@{i}"`; entries are returned newest-first (the reflog's natural order),
capped at `max`. A repo with no HEAD reflog yet (extremely fresh) yields an empty
vector, not an error. **The reset action reuses the existing
`reset_to(repo, oid, reset_mode)`** — no new reset engine.

### Bridge — C ABI
```
int mg_magit_reflog_buffer(const char *repo, int n, mg_magit_emit_fn emit, void *ctx);
```
Emits one **`MG_LINE_COMMIT`** line per entry — `"<short_oid> HEAD@{i} <message>"`
— with the full oid in `path`, so the editor's existing per-line-oid map +
`RET`-to-diff machinery work unchanged. Returns the line count (0 on failure).
Reset reuses the existing `mg_magit_reset(repo, oid, mode)`. **No new line-kind.**

### Editor — `magit_cmd.c`
- Bind **`h`** in the `l` log menu (`magit_logmenu`, `KEYMAPE(6)→(7)`, ascending
  order, both count fields bumped) → `magit_reflog`: builds `*magit-reflog*` via
  `mg_magit_reflog_buffer` + a per-line oid map (mirrors `magit_log_build` /
  `magit_log_emit`), in a new **`magit-reflog-mode`** registered in the
  `magit_status` init block.
- `magit-reflog-mode` keymap: `RET` → show the entry's diff (reuse the
  `*magit-commit*` path via `commit_diff` on the oid at point), `g` → refresh,
  `q` → close, and a reset prefix `x` → `x s`/`x m`/`x h` → `mg_magit_reset(repo,
  oid_at_point, mode)` (hard confirms), then refresh the status buffer.

## Rendering

```
abc12345 HEAD@{0} commit: fix rename
def67890 HEAD@{1} reset: moving to HEAD~1
1234abcd HEAD@{2} checkout: moving from feature to master
```
Newest first; the oid (`path`) drives `RET`-diff and reset-at-point. Capped at
~100 entries (note the cap if more exist).

## Error handling / edges

- No-HEAD-reflog (pristine repo): empty buffer, no error.
- Detached HEAD: HEAD reflog still exists; renders normally.
- Reset to an oid that is the current HEAD: a no-op reset (harmless).
- `x h` on dirty worktree: the confirm prompt is the guard; git's hard reset then
  discards — that's the documented behavior the confirm warns about.

## Testing (TDD)

- **Engine** (`test_git`): build a repo, perform commit → `reset_to` → checkout,
  call `reflog(repo, 100)`; assert the entries' count, newest-first order,
  `selector` values (`HEAD@{0..}`), oids (match the known commit oids), and that
  messages contain the expected verbs. Plus: `max` caps the count; a fresh repo
  yields ≥1 entry.
- **Bridge** (`test_bridge`): `mg_magit_reflog_buffer` emits `MG_LINE_COMMIT`
  lines whose `path` carries the full oid and whose text contains `HEAD@{0}`;
  reset-at-point is the existing `mg_magit_reset(repo, <reflog oid>, mixed)`
  moving HEAD (covered by the existing reset path — add an assertion that
  resetting to a reflog oid lands HEAD there).
- **Editor** (`test_editor`, pty): launch in a repo with ≥2 HEAD moves, `M-x
  magit-status`, `l h`, assert the buffer shows `HEAD@{0}`.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`) + OFF (`cpp-tsan`).

## Phases (each independently testable)

1. **Engine `reflog()`** — the struct + `git_reflog_*` reader + `test_git` cases.
2. **Bridge `mg_magit_reflog_buffer`** — emit `MG_LINE_COMMIT` lines + the bridge
   test (incl. the reset-to-reflog-oid assertion reusing `mg_magit_reset`).
3. **Editor view** — `l h` → `*magit-reflog*` (`magit-reflog-mode`, RET-diff/g/q),
   `h` bound in the log menu; pty test.
4. **Reset-at-point** — the `x s`/`x m`/`x h` prefix in `magit-reflog-mode`
   (hard confirms) → `mg_magit_reset`, refresh; pty test of a reset round-trip.

## Out of scope

- One-key destructive "undo" (decided against — reset-at-point with an explicit
  mode is the model).
- Current-branch reflog (`l H`) and stash reflog — follow-up.
- Reflog expiry / `git reflog delete` — neomg reads, never prunes.
- Cherry-pick/revert/note actions on reflog entries (those live in `*magit-log*`).
