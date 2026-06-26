# Task 3 Report: Editor keys (`+`/`-`/`w`) + staging warn (FM-DIFF-CTL)

## Status

DONE

## What Was Built

Implemented the editor surface for FM-DIFF-CTL Task 3 in `src/magit_cmd.c` and `tests/test_editor.cpp`.

### Files Changed

- **`src/magit_cmd.c`**: All changes:
  1. **State variables** (after `magit_expanded_count`): `magit_diff_context = 3` and `magit_diff_ignore_ws`.
  2. **Forward declarations** (after `magit_todo_abort`): `magit_diff_more`, `magit_diff_less`, `magit_diff_ws`.
  3. **PF arrays** (after `magit_u`, before `magit_z`): `magit_w`, `magit_plus`, `magit_minus`.
  4. **`magitmap` keymap**: `KEYMAPE(32)→(35)`, both count fields `32→35`. Inserted `'+'`(0x2b) and `'-'`(0x2d) after `'$'` and before `'?'`; inserted `'w'`(0x77) after `'u'` and before `'z'`. All 35 entries verified strict-ascending.
  5. **3 command implementations** (before `#endif`): `magit_diff_more`, `magit_diff_less`, `magit_diff_ws` — each clamps/toggles state, calls `mg_magit_set_diff_view`, calls `magit_refresh`.
  6. **`-2` sentinel** at all 5 staging call sites: `mg_magit_stage_region` + `mg_magit_stage_hunk` (in `magit_s`); `mg_magit_unstage_region` + `mg_magit_unstage_hunk` (in `magit_unstage`); `mg_magit_discard_region` (in `magit_discard`). Pattern: capture `rc`, `-2 → ewprintf("Turn off -w (w) to stage hunks") + return FALSE`, then existing failure message path.

- **`tests/test_editor.cpp`**: `TEST_CASE("+ grows diff context; w blocks hunk staging with a warning")`:
  - `make_repo()` + overwrite `tracked.txt` with 10 lines (change on line 6, far line `TENLINE` on line 10).
  - `M-3` expand all → wait for `SIX` visible.
  - `+++` grow context 3→6 → wait for `TENLINE` (robust "grew" signal).
  - `w` toggle ws + 1200ms drain.
  - 9x `C-n` to navigate to the `@@` hunk line (determined empirically from ANSI escape debug output; buffer layout after w: On branch / Head: / Diff:-U6-w / blank / Untracked(1) / untracked.txt / blank / Unstaged(1) / tracked.txt / `@@`).
  - `s` stage → `wait_for("Turn off -w")` (robust "warned" signal).

## TDD Evidence

**RED** (before implementation): `+`/`w` unbound → `CHECK(grew)` false.
**GREEN** (after implementation): both `grew` and `warned` true.

## Results

| Platform | Result |
|----------|--------|
| macOS `cmake --build build && ctest --test-dir build` | 265/265 PASS |
| Alpine `docker build -f docker/Dockerfile.alpine -t mg-diffctl-fix .` | 265/265 PASS |
| OFF compile `cmake --build build-tsan` | Build SUCCESS |

## Commit

- `60ed444` feat(diff): +/- context, w ignore-whitespace keys; warn on ws-blocked staging

## Concerns

None. The `C-n` navigation count (9) is deterministic: derived from the fixed `make_repo()` repo structure plus the `Diff: -U6 -w` header line that appears when context≠3 OR ws is on (both true after `+++` + `w`). The layout does not vary across runs.

## Review-fix (post-approval, test-only)

Review approved with two Important test-robustness findings (CI-flake class that bit FM-REFLOG). Both fixed in `tests/test_editor.cpp` only — production `src/magit_cmd.c` logic untouched:

1. **Removed the wall-clock `drain_str(1200ms)`** before the C-n navigation, replaced with event waits: `wait_for(master, "Diff:", 8s)` (the `Diff: -U.. -w` header renders only after `w` takes effect + repaint lands) followed by `wait_for(master, "@@", 8s)` (re-confirms a hunk header is visible). Wall-clock sleeps before sending more keys were the flake vector under loaded CI.
2. **Guarded the `w` → C-n → `s` → `warned` block in `if (grew) { … }`** so the `grew` and `warned` assertions stay independent — a context-growth failure no longer runs the warn sequence and produces a confusing mixed result.

(The 9×C-n count and the spec-verbatim "Turn off -w (w) to stage hunks" wording were left as-is per review guidance.)

Re-verification:
| Platform | Result |
|----------|--------|
| macOS `./build/tests/test_editor` | 9/9 PASS |
| Alpine `docker build -f docker/Dockerfile.alpine -t mg-diffctl-fix .` | 265/265 PASS |

Commit: `f8986ae` test(diff): event-wait + grew-guard for the +/-/w pty test (CI robustness)

---

## Final-review fix wave

### Item 1 (data race): atomic globals

Changed `g_diff_context` (plain `int`) and `g_diff_ignore_ws` (plain `bool`) in
`src/magit/git.cppm` to `std::atomic<int>` and `std::atomic<bool>`. `set_diff_view`
stores with `std::memory_order_relaxed`; the two getters load with relaxed. The
`#include <atomic>` was already present in the global-fragment (line 9). Updated
all direct reads of the atomics inside `file_diff` and `path_scoped_diff_opts`
(which cannot use the getter because they are inside the same anonymous namespace)
to use `.load(std::memory_order_relaxed)`. All read sites in `bridge.cpp` already
called the exported getters — no changes there.

Commit: `496b9bd` fix(diff): make g_diff_context/g_diff_ignore_ws std::atomic (data-race fix)

### Item 2 (header lag): live Diff: emit in snapshot replay

Chose **approach (a)**: emit the `Diff:` header live in `mg_magit_status_snapshot`
by reading the current atomic globals, and skip any baked `Diff:`-prefix line from
the snapshot during replay.

**Why (a) over (b):** Approach (b) would fold the view config into `repo_fingerprint`,
requiring a per-render stat+read-head call (already done) PLUS a string encoding of
two more values. More plumbing, more state to keep consistent, and the fingerprint
would then silently grow for all callers. Approach (a) is a 15-line insert in one
function (`mg_magit_status_snapshot`), keeps the header logic entirely on the UI
thread (where the atomic read is), and is impossible to drift from `compose_status_view`
because it is literally the same two-line condition copy-pasted.

Added deterministic bridge test `"snapshot replay reflects current diff-view (not
baked header)"` in `tests/test_bridge.cpp`: starts the monitor, waits (poll loop,
max 2s) for an initial snapshot at default view (3,0), confirms no `Diff:` header at
default, calls `set_diff_view(6,1)` with no fs event (exactly the lag scenario),
then asserts the very next `status_snapshot` call emits `-U6 -w` exactly once
(the baked empty-snapshot line is filtered out).

Commit: `a03f6cb` fix(diff): snapshot replay emits Diff: header live from current atomic globals

### Item 3 (doc): spec concurrency note corrected

Updated `docs/superpowers/specs/2026-06-26-fm-diff-ctl-design.md` (Decided model >
Engine section) and `docs/superpowers/plans/2026-06-26-fm-diff-ctl.md` (Global
Constraints): both now state that the monitor reads the view config for the `Diff:`
header and that the globals must therefore be `std::atomic<int>` / `std::atomic<bool>`.

Commit: `68ce22a` docs(diff-ctl): correct concurrency note — monitor reads view config for Diff: header

### Verification results

| Platform | Result |
|----------|--------|
| macOS `ctest --test-dir build --output-on-failure` | 266/266 PASS (+1 new test) |
| Alpine `docker build -f docker/Dockerfile.alpine -t mg-diffctl-fix .` | 266/266 PASS |
| TSan `ctest --test-dir build-tsan -R "git\|bridge"` | 54/54 PASS, 0 races |

**TSan coverage note:** The `"monitor snapshot/modeline accessors are race-free under
mutation"` test starts the monitor (which calls `compose_status_view` →
`diff_view_context()`/`diff_view_ignore_ws()` on the monitor thread) while the UI
thread hammers `mg_magit_status_snapshot` (which also reads those globals in the
new live-header block). Under TSan this is a meaningful concurrent exercise of the
now-atomic globals — the atomics eliminate the race that was previously invisible
because the pty test `chdir`s away and the monitor never starts in that scenario.
