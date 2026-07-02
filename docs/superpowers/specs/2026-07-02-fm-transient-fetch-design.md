# FM-TRANSIENT-FETCH — a fetch transient (`f`)

## Why

`P` (push) and `F` (pull) are transients with args; `f` (fetch) is still a bare
one-key action with no args. This completes the network-op trio: give `f` a
transient with `--prune` / `--tags` / `--all`, mirroring FM-TRANSIENT-PUSH.

## Decided model (from brainstorming)

`f` opens a fetch transient (like `P`/`F`); the action `f f` runs the fetch with
the toggled args. **Plain fetch becomes `f f`** — consistent with `P p` / `F p`
(the one behavior change: `f` alone no longer fetches instantly). Args are FLAG
infixes:
- **`--prune`** — drop remote-tracking refs deleted upstream.
- **`--tags`** — fetch all tags.
- **`--all`** — fetch all remotes; the argv then omits `origin`
  (`git fetch --all [--prune] [--tags]`), otherwise
  `git fetch [--prune] [--tags] origin`.

## Architecture (mirrors FM-TRANSIENT-PUSH; no captured/dry-run path — fetch is tty)

### Bridge (C ABI)
`mg_magit_fetch_cli(repo)` → `mg_magit_fetch_cli(repo, int prune, int tags,
int all)`. Build argv: start `{"fetch"}`; if `all` append `--all` (no remote arg),
else the remote is `origin`; append `--prune`/`--tags` when set; when not `all`,
append `origin` last. Run over `git_terminal` (tty), record the `$` proclog entry
(unchanged pattern).

### Editor (`magit_cmd.c`)
- Two new flag bits by the others: `#define MNET_PRUNE 0x20`,
  `#define MNET_ALL 0x40`. Fetch `--tags` reuses the existing `MNET_TAGS` (0x04)
  — same literal flag.
- `magit_run_net`'s `MNET_FETCH` case decodes them:
  `mg_magit_fetch_cli(cwd, flags & MNET_PRUNE, flags & MNET_TAGS, flags & MNET_ALL)`.
- A `fetch_menu` (title "Fetch") with one action item `f` ("fetch") and
  `fetch_infixes[]` = `{ --prune, --tags, --all }` (all FLAG, `clear_on_empty` 0),
  plus a `KEYMAPE(1) magit_fetchmenu` binding `f` → the fetch action, mirroring
  `pull_menu`/`magit_pullmenu`.
- `magit_menu_fetch` = `magit_transient(&fetch_menu, f, n)`; `magit_fetch` (the
  action) reads `fetch_infixes` into a flags value and calls
  `magit_run_net(MNET_FETCH, cwd, flags, "Fetching...")`.
- Rebind `f` in `magitmap`: `magit_f[]` = `{ magit_menu_fetch }` (was
  `{ magit_fetch }`). No keymap-order change (same `'f'` key, new target).
- The libgit2 fallback (`mg_magit_fetch`, when git is off PATH) stays a plain
  fetch — the flags are CLI-path only, a documented limitation (same as push
  `--tags`).

## Rendering

The `f` transient renders via the existing `magit_transient_render`: the
`--prune`/`--tags`/`--all` infixes (FLAG on/off) + the `f fetch` action. No new
render code.

## Error handling / edges

- All three default off → `f f` runs `git fetch origin`, byte-identical to
  today's `f`.
- `--all` and an explicit `origin` are mutually exclusive in the argv (handled by
  the build: `--all` omits `origin`).
- A failed fetch (no origin, auth) is surfaced (tty output / `ewprintf`), not a
  crash — unchanged from today.
- `f` opening a menu instead of fetching is the intended behavior change; it does
  not collide (same key, new command).

## Testing

- **Bridge** (`test_bridge`): `mg_magit_fetch_cli(..., prune, tags, all)` threads
  the flags — `prune=1,tags=1,all=0` logs `fetch … --prune --tags … origin`;
  `all=1` logs `fetch --all …` with **no `origin`** — via the iterate-and-match
  proclog idiom (`kind=='$'`).
- **Editor** (`test_editor`, pty): `f` opens the fetch transient (a `--prune` /
  `--all` label renders); toggling one then `f f` runs it (no origin → fails fast
  but is attempted). Single session, presence-only, drained exit via `quit_neomg`.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (deferred — tracked in todo.md if pursued)

- Fetching a specific remote / branch / refspec (still `origin`, or `--all`).
- `--depth`/shallow, `--force` fetch.
