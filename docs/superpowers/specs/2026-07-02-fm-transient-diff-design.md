# FM-TRANSIENT-DIFF — a live diff-view popup (`d`)

## Why

FM-DIFF-CTL added live diff-view controls — `+`/`-` (context `-U<n>`) and `w`
(ignore-whitespace `-w`) — as bare keys in `*magit-status*`. They work but are
undiscoverable. This wraps them in a `d` popup (matches Magit's diff dispatch),
so the current view + the keys are visible. Continues the FM-TRANSIENT-DEPTH /
FM-TRANSIENT-PUSH theme of surfacing controls in transients.

## Decided model (from brainstorming)

A **live settings popup**, not a sticky-args-then-apply transient (the diff
controls already apply immediately, so the sticky model would add an awkward
apply step). `d` opens a small popup; `+`/`-`/`w` pressed *inside* it adjust the
view live and the popup stays open showing the new state; `q`/`C-g`/`ESC` closes.
The bare `+`/`-`/`w` keys in `*magit-status*` stay — the popup is an additional,
discoverable entry point, not a replacement.

## Architecture (editor-only; reuses FM-DIFF-CTL machinery)

A dedicated `magit_diff_transient` command in `src/magit_cmd.c`, bound to `d` in
`magitmap` (`d` is free there — the `d`/drop binding is in a submenu). No engine
or bridge changes: the diff-view state (`magit_diff_context` 0–32,
`magit_diff_ignore_ws`), `mg_magit_set_diff_view`, and the status refresh already
exist.

It uses the same popup scaffolding as `magit_transient` (open `*magit-transient*`
via `bfind` + `popbuf`; save the status window/buffer `stwp`/`stbp`; tear down
with `delwind` + restore on exit), but with a **live key loop** instead of the
infix-toggle / action-dispatch model:

```
save stwp/stbp (the *magit-status* window/buffer); popbuf the popup
for (;;):
    render the diff view into the popup buffer; update
    k = getkey()
    '+' -> if context < 32: context++      \
    '-' -> if context > 0:  context--       > apply live (see below), re-render
    'w' -> toggle ignore_ws                 /
    'q' / C-g / ESC -> break
    else -> ignore (re-render)
tear down popup; restore stwp/stbp
```

**Apply live.** On each `+`/`-`/`w` the handler: (1) updates
`magit_diff_context` / `magit_diff_ignore_ws`, (2) calls
`mg_magit_set_diff_view(...)`, (3) calls `magit_refresh(f, n)`, (4) re-renders the
popup (the loop then `update()`s). No window/buffer swap is needed:
`magit_refresh` does `bfind("*magit-status*")` + `magit_build(bp)` — it rebuilds
the status buffer **by name**, not `curbp`, so it works correctly even though the
popup buffer is current. `magit_build` marks the status window dirty, so the
loop's `update(CMODE)` redraws both the status window (with the new diff /
`Diff: -U<n> …` header from FM-DIFF-CTL) and the popup. Because the popup is a few
lines, the diff above it re-renders visibly.

(This is the same refresh-by-name call the existing `magit_diff_more/less/ws`
already make, so the diff-view path is unchanged — the popup just drives it in a
loop instead of one keypress at a time.)

## Rendering

The popup shows the current view and its keys, e.g.:
```
Diff view
 +/-   context      -U3
 w     whitespace   shown            (w -> ignored)
 q     close
```
The context number and the whitespace label update as keys are pressed.

## Error handling / edges

- Context clamps to 0–32 (existing `magit_diff_more/less` bounds); at a bound,
  the key is a no-op that still re-renders.
- Opening the popup and closing without changes leaves the view untouched.
- `d` is `*magit-status*`-only; it does not shadow anything (currently unbound
  there). It must be inserted in `magitmap` in ascending key order (the map is
  binary-searched by `doscan`).

## Testing

- **Editor** (`test_editor`, pty): `d` in `*magit-status*` opens the popup (a
  `Diff view` / `-U3` label renders); press `+` and confirm `*magit-status*`
  shows the `Diff: -U4` header (proving live-apply + refresh); `q` closes and the
  status is intact. Single session, presence-only, drained exit via `quit_neomg`.
  (`make_repo` needs no unstaged hunk — the `Diff:` header is emitted whenever the
  context ≠ 3 or whitespace is ignored, so `-U4` appears from the setting alone.)

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Out of scope (deferred — tracked in todo.md if pursued)

- Per-file diff args, or a full Magit diff dispatch (diffing arbitrary
  commits/ranges) — this wraps the existing live status-view controls only.
- A `-w`/context transient in `*magit-log*` or other buffers.
