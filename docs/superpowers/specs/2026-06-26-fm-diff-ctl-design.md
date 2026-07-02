# FM-DIFF-CTL — diff context + whitespace controls

## Why

Reading diffs in `*magit-status*` is core to the workflow, but the expanded
file diffs are fixed at git's default 3 lines of context with whitespace always
significant. Magit lets you grow/shrink context and ignore whitespace on the
fly. todo.md's "Magit-parity gaps" lists this as high value. This adds:

- `+` / `-` — grow / shrink the context lines of expanded file diffs.
- `w` — toggle ignore-whitespace (`-w`).

Word-level/refined intra-line highlighting is a separate, more complex concern
(new per-hunk diff computation) and is deferred to a follow-up (FM-DIFF-REFINE).

## The load-bearing constraint: display ↔ staging hunk alignment

`file_diff` (the display path) and the staging ops (`stage_hunk`/`unstage_hunk`/
`stage_region`/`unstage_region`/`discard_region`) each build their **own**
`git_diff_options` (today all at the default context 3). Hunks are numbered "as
numbered by `file_diff`", and `s`/`u`/`k` act by that index. So if the *displayed*
context diverged from what staging recomputes, the indices would mismatch and you
would stage the WRONG hunk. Therefore the context setting must be applied
uniformly at **every diff-options site that numbers hunks**, not just the display.

## Decided model

### Engine (`mg.git`)
A process-global diff-view config in `git.cppm`:
```
g_diff_context     : int   (default 3)
g_diff_ignore_ws   : bool  (default false)
void set_diff_view(int context, bool ignore_ws);   // setter (clamps context >= 0)
```
- **Every hunk-numbering diff-options site** sets `opts.context_lines =
  g_diff_context`: the display (`file_diff`) AND the staging/numbering ops
  (`stage_hunk`, `unstage_hunk`, `stage_region`, `unstage_region`,
  `discard_region`). Implement via a small shared helper
  `apply_diff_view(git_diff_options &opts, bool for_display)` called at each site,
  so the list can't drift. `for_display=true` (only `file_diff`) additionally sets
  `opts.flags |= GIT_DIFF_IGNORE_WHITESPACE` when `g_diff_ignore_ws`. The staging
  ops pass `for_display=false` — they set context (to stay index-aligned) but
  NEVER ignore whitespace (a patch must apply exactly).
- The config is read on the UI thread (status build + staging are synchronous
  editor commands) AND on the monitor thread (to compose the `Diff:` header in
  `compose_status_view` → `publish_view`). Therefore both globals must be
  `std::atomic<int>` / `std::atomic<bool>` (relaxed load/store — single scalars,
  no ordering dependency). The background monitor never calls `file_diff`
  directly, but it does read the view config for the header — the atomics
  eliminate the UI-write / monitor-read data race.

### Whitespace ↔ staging guard (the key behavioral decision)
When `g_diff_ignore_ws` is **on**, the displayed hunks are whitespace-collapsed:
they can't be cleanly applied, and their numbering (ws-ignored) wouldn't match the
exact-diff staging ops. So **hunk/region staging is disabled while `-w` is on**:
`mg_magit_stage_hunk`/`unstage_hunk`/`stage_region`/`unstage_region`/
`discard_region` return a distinct sentinel (e.g. `-2`) when `g_diff_ignore_ws`,
and the editor surfaces "Turn off -w (w) to stage hunks." Whole-file
stage/unstage/discard (which don't use the diff) still work. (This matches
Magit, which can't apply a whitespace-ignored diff.)

### Bridge (C ABI)
`void mg_magit_set_diff_view(int context, int ignore_ws);` → calls the engine
setter. The five hunk/region staging C ABIs gain the `-2`-when-ws sentinel
(checked before doing the libgit2 work).

### Editor (`magit_cmd.c`)
- State: `magit_diff_context` (default 3, clamped 0–32) + `magit_diff_ignore_ws`.
- Bindings in `magitmap` (all free): `+` → context+1; `-` → context−1 (floor 0);
  `w` → toggle ignore-ws. Each calls `mg_magit_set_diff_view(...)` then
  `magit_refresh` (rebuild applies the new view).
- The status header shows the non-default state, e.g. appends `(diff: -U6 -w)` to
  the branch/header line so the active controls are discoverable.
- The hunk/region staging commands (`s`/`u`/`k`), on the `-2` sentinel, `ewprintf`
  the "turn off -w" note instead of refreshing.

## Rendering

Expanding a file at context 6 shows 6 lines around each change; `-w` drops
whitespace-only hunks/lines. The header reflects `(diff: -U<n>[ -w])` when not at
the default (`-U3`, ws-significant).

## Error handling / edges

- Context floor 0 (no context); a sensible ceiling (32) to bound buffer size.
- `+`/`-`/`w` outside `*magit-status*`: the bindings live in `magit-status-mode`
  only, so they're inert elsewhere.
- ws-on + `s`/`u`/`k` on a hunk/region: the `-2` sentinel → warn, no mutation.
- Whole-file `s`/`u` (on a file line, not a hunk) is unaffected by `-w` (it stages
  the path, not a patch).
- The setting persists for the session (resets to defaults on restart) — like the
  fold/expand state.

## Testing (pty + engine)

- **Engine** (`test_git`): `set_diff_view(6, false)` then `file_diff` returns hunks
  whose context spans 6 lines (assert a context line 6 away from a change is
  present at 6, absent at 3); `set_diff_view(3, true)` on a whitespace-only change
  yields no hunks (clean). Reset between cases.
- **Bridge** (`test_bridge`): with ignore-ws set, `mg_magit_stage_hunk` returns the
  `-2` sentinel (no staging); with ws off it stages normally. Index alignment:
  after `set_diff_view(6,…)`, the displayed hunk count and the stageable hunk
  indices still correspond (stage hunk 0 stages the right change).
- **Editor** (`test_editor`, pty): expand a file with a multi-line change; `+`
  shows a context line that was absent at default; `w` makes a whitespace-only
  change vanish; with `w` on, `s` on a hunk shows the warn note. Robust pty
  patterns (forced repaint, drain, unambiguous needles, `C-x 1`).

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

## Phases (each independently testable)

1. **Engine view-config + `apply_diff_view` helper** wired into `file_diff` +
   the staging sites; `set_diff_view`; engine tests (context span, ws-clean,
   alignment). The ws-staging sentinel in the engine staging ops.
2. **Bridge** `mg_magit_set_diff_view` + the `-2` sentinel surfaced through the
   staging C ABIs; bridge tests.
3. **Editor** bindings (`+`/`-`/`w`) + state + header indicator + the staging
   warn-on-sentinel; pty tests.

## Out of scope

- Word-level / refined intra-line diff highlighting (→ FM-DIFF-REFINE).
- Applying the controls to the commit-view (`commit_diff`) or log diffs — v1 is
  the status buffer's expanded diffs (the shared `file_diff` path). A follow-up
  can route `commit_diff` through `apply_diff_view` too.
- Per-hunk or per-file context overrides; diff-algorithm selection (patience/histogram).
- A diff-args transient (that's FM-TRANSIENT-DEPTH).
