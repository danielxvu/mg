# FM-SECTION-CYCLE — global visibility levels (M-1/M-2/M-3)

## Why

Magit users constantly cycle section visibility — collapse everything to scan
the shape of a status, expand everything to read the diffs. neomg today only has
flat per-item `TAB` (toggle one section's body, or one file's inline diff). The
"show me the whole picture at level N" gesture is missing — todo.md's
"Magit-parity gaps" lists it as high value.

Magit's literal `S-TAB` (`magit-section-cycle-global`) is **not receivable** in
mg's TUI (the input layer doesn't decode `CSI Z` / back-tab). But Magit *also*
ships `M-1`/`M-2`/`M-3`/`M-4` (`magit-section-show-level-N-all`), which ARE
bindable via mg's ESC→metamap path and are arguably better (direct level, no
guessing the current cycle state). This implements those.

## Decided model

Three keys set the whole `*magit-status*` buffer's visibility level, reusing the
two existing state sets (`magit_folded[]` = section bodies hidden;
`magit_expanded[]` = file inline-diffs shown). Existing `TAB`
(`magit_toggle_expand` — toggle one section / one file) is unchanged.

| Key | Level | Effect (state) |
| --- | --- | --- |
| `M-1` | collapse — headers only | every section key → `magit_folded`; clear `magit_expanded` |
| `M-2` | files (today's default) | clear `magit_folded`; clear `magit_expanded` |
| `M-3` | expand — hunks shown | clear `magit_folded`; every Unstaged+Staged file path → `magit_expanded` |

neomg's status hierarchy is section → file → hunk; the three levels map to those
depths. Untracked files have no inline diff (`section(... diffable=false)` in
`compose_status_view`), so `M-3` does not expand them — consistent with `TAB`.

## Architecture (editor-only — NO engine/bridge changes)

All three commands live in `src/magit_cmd.c` and reuse the existing rebuild path.

- `magit_show_level_1/2/3(int, int)` — each:
  1. Walks the buffer's lines in parallel with the existing per-line
     `magit_meta[]` array (index `i` ↔ buffer line `i`; `magit_meta[i]` records
     `{kind, hunk, path}`). For each line, `magit_meta[i].kind` classifies it:
     - `MG_LINE_SECTION` → the fold key is derived from the **line's text**
       (trailing " (N)" count stripped, exactly as `magit_section_folded` derives
       it — sections have no `path`, so the key must come from the text).
     - `MG_LINE_UNSTAGED` / `MG_LINE_STAGED` → the diffable file path is
       `magit_meta[i].path`.
  2. Rewrites `magit_folded[]` / `magit_expanded[]` per the level table (clearing
     and refilling; respecting the existing `MAGIT_MAX_FOLDED` / `MAGIT_MAX_EXPANDED`
     caps — if a repo exceeds a cap, fill up to it, same bound the per-item path
     already has).
  3. Rebuilds in place via the existing `magit_build(bp)` (snapshot replay), which
     already applies both state sets. Point returns to top, like `g` refresh.
- A shared helper does the buffer scan + set-rewrite; the three commands differ
  only in which sets they populate.

The section-key collection mirrors `magit_section_folded`'s key derivation (line
text minus the trailing " (N)" count) so the keys match what the rebuild checks.

## Binding

`M-1`/`M-2`/`M-3` are added to `magit_metamap` (the status-mode ESC-prefix map
where `M-x` is bound). Insert `'1'`/`'2'`/`'3'` entries in strict-ascending key
order (`'1'`=0x31 < `'2'` < `'3'` < `'x'`=0x78), and bump the `KEYMAPE(N)` size +
both count fields to match (`doscan` binary-searches — wrong order/count silently
breaks the meta prefix). Only `magit-status-mode` binds these.

## Error handling / edges

- Not in `*magit-status*` (no sections): the scan finds nothing; the level
  commands no-op gracefully (rebuild a buffer with empty sets). The bindings live
  only in `magit-status-mode`, so this is mostly moot.
- A section with no body (e.g. an empty section isn't rendered) — folding its key
  is harmless (no body to suppress).
- `M-1` folds non-file sections too (Stashes/Branches/Recent commits) — intended
  (Magit level-1 collapses to top headers).
- Re-pressing the same level is idempotent (sets recomputed identically).

## Testing (pty)

A repo with a staged file and an unstaged file (each with known diff content):
- `M-3` → a hunk/diff line for the changed file appears (was absent at the
  default level) — the strong positive signal that files expanded.
- `M-1` → section bodies collapse to headers (the file line under a section is no
  longer rendered after a forced repaint; the section header remains).
- `M-2` → files shown again without hunks.

Use the robust pty patterns established in FM-REFLOG: force a full repaint after
each level change (so the incremental display emits every row), drain to a
sustained-quiet window before asserting, and prefer positive/unambiguous needles
(a diff line for `M-3`; a section header for the collapsed state). Where a level
assertion is inherently about absence, verify it via a positive round-trip
(`M-3` shows the hunk → `M-1` → `M-3` again shows it) rather than asserting a
needle is missing.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`) — the
feature is editor C, built in all configs.

## Phases (each independently testable)

1. **The shared scan + the three level commands** (`magit_show_level_{1,2,3}` +
   the buffer-scan helper), with a unit-ish driver if feasible, else folded into
   the pty test.
2. **Bind `M-1`/`M-2`/`M-3`** in `magit_metamap` + the pty test exercising all
   three levels.

(If phase 1's commands aren't observable without the binding, merge into a single
task — the deliverable is the three working keys + the pty test.)

## Out of scope

- Per-section `1`/`2`/`3` (section-local show-level) — follow-up.
- `S-TAB` cycle — mg can't decode `CSI Z`; would need input-layer work.
- Fold indicators (▶/▼) on section headers — a separate display change.
- `M-4`+ (deeper levels) — neomg's hierarchy is only 3 deep.
