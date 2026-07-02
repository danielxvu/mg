# FM-LOG-TRANSIENT-KEY — bind the log transient in `*magit-log*`

## Why

Found in the FM-TRANSIENT-DEPTH review: `*magit-log*` doesn't bind `l` to re-open
the log transient the way Magit does, so tweaking log args (`--author`/`--grep`/
`--all`/`-n`, or switching `l l`/`l f`/`l g`/`l r`) means bouncing back to
`*magit-status*`. This closes that parity gap.

## Decided model

Bind `l` in the `*magit-log*` keymap to the *same* command that opens the log
transient from `*magit-status*` (`magit_menu_log` → `magit_transient(&log_menu,
…)`). Pressing `l` in `*magit-log*` opens the transient; toggling an infix and
re-running an action rebuilds `*magit-log*` in place — no round-trip to status.

**Decisions (from brainstorming):**
- Bind `l` (matches Magit, where `l` in the log buffer *is* the log transient).
  It overrides the default line-down on `l`; navigation remains via `n`/`p`/
  arrows/`C-n`.
- Scope: `*magit-log*` only. `*magit-reflog*` is a separate buffer/concern and is
  out of scope.

## Architecture

The whole change is one keymap entry — no new commands, no action changes.

- `maglogmap` (`src/magit_cmd.c`, the `*magit-log*` keymap) gains
  `{ 'l', 'l', magit_l, NULL }`. `magit_l[]` = `{ magit_menu_log }` already exists
  (it's what `l` runs in `*magit-status*`).
- `maglogmap` grows `KEYMAPE(6)` → `KEYMAPE(7)` and its element counts `6, 6` →
  `7, 7`.
- The entry is inserted in **strict ascending key order**: `'l'` (108) sits
  between `'g'` (103) and `'q'` (113). The keymap MUST stay sorted because
  `doscan` binary-searches it. Note: the existing asserts do NOT cover this —
  `magit_assert_keymap_sorted` checks only `magitmap`, and
  `magit_assert_menus_consistent` checks only the transient submaps. So the
  ascending order is enforced by care and by the pty test: an out-of-order
  insert makes `doscan` miss `l`, and the test (which presses `l` in
  `*magit-log*`) would then fail.

The transient's actions already operate on the current buffer's repo via
`getbufcwd` and rebuild `*magit-log*` (that is what `l l` does today from
`*magit-status*`), so invoking them from within `*magit-log*` needs no change —
only the entry point is new.

## Error handling / edges

- `l` no longer moves the cursor down in `*magit-log*` (intended; matches Magit).
  Other navigation keys are unaffected.
- Opening the transient from `*magit-log*` and cancelling it (`C-g`/`ESC`) returns
  to `*magit-log*` unchanged (existing `magit_transient` restore behavior).
- Running a log action from `*magit-log*` rebuilds `*magit-log*` in place (it is
  already the log builder's target buffer).

## Testing

- **Editor** (`test_editor`, pty): from `*magit-log*` (reached via `l l` in
  `*magit-status*`), press `l` and confirm the log transient renders (a menu
  label such as `--grep` appears). Ideally also toggle an infix (e.g. `--all`)
  and run it, confirming `*magit-log*` rebuilds without leaving the buffer.
  Single transient session, presence-only assertions, drained exit via
  `quit_neomg` (from `fm-pty-exit-drain`).

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (deferred — tracked in todo.md if pursued)

- Binding the log transient in `*magit-reflog*`.
- Any new log-transient actions/args (this only adds an entry point to the
  existing transient).
