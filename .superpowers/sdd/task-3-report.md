# Task 3 Report: Editor `:` binding + command (FM-GIT-CMD)

## Status: COMPLETE

## Commits

- `7e32633` feat(git-cmd): : prompts + runs a git command into *magit-process*

## What was built

### 1. Failing test added first (TDD)
Added `TEST_CASE(": runs a git command and shows it in *magit-process*")` to
`tests/test_editor.cpp`. Used `make_repo()` (produces `tracked.txt` + `untracked.txt`),
`sizeof gc - 1` write idiom (no manual byte count), and `"git rev-parse --abbrev-ref HEAD"`
rendered in `*magit-process*` as the needle (safe non-interactive read, unambiguous).

### 2. Forward decl + PF added (`src/magit_cmd.c`)
- Forward decl `static int magit_git_command(int, int);` appended to the existing decl block
  (after `magit_diff_ws`).
- PF `static PF magit_colon[] = { magit_git_command };` added after `magit_minus[]`.

### 3. Keymap updated (`src/magit_cmd.c`)
`magitmap` changed from `KEYMAPE(35)` to `KEYMAPE(36)`, both count fields `35→36`.
`{ ':', ':', magit_colon, NULL }` inserted between `'-'` (0x2d) and `'?'` (0x3f) — strict
ascending order preserved. `magit_assert_keymap_sorted()` validates at runtime.

### 4. Command implementation (`src/magit_cmd.c`)
`magit_git_command` added before `#endif /* ENABLE_NATIVE_MAGIT */`:
- `eread("git: ", ...)` with `EFNEW|EFCR`; aborts on empty/cancel
- `getbufcwd(cwd, ...)` for the buffer's repo directory
- `mg_magit_git_command(cwd, cmdline)` runs the command (logs `$` proclog entry)
- `magit_refresh(f, n)` reflects any mutation in `*magit-status*`
- `return magit_process(f, n)` lands on the command's output

## Test results

### macOS
`cmake --build build && ctest --test-dir build --output-on-failure`
→ **270/270 PASS** (new test #265 passes in 2.83s)

### Alpine (`docker build -f docker/Dockerfile.alpine -t mg-gitcmd .`)
→ **BUILD SUCCEEDED, 270/270 PASS** (ctest green on musl/Alpine)

### OFF compile (`cmake --build build-tsan`)
→ **Build SUCCESS** (zero errors; pre-existing macOS SDK version warnings from Zig objects only)

## Files modified
- `/Users/dvu/src/mg/src/magit_cmd.c`
- `/Users/dvu/src/mg/tests/test_editor.cpp`

## Concerns
None. Clean reuse of existing patterns (`magit_log_range_cmd` for `eread`+`magit_process`).
The keymap sort assertion catches ordering errors at startup.

## Final-review fix wave

### Changes to `detail::run_git` (`src/magit/git.cppm`)

1. **`#include <fcntl.h>`** added to the global-fragment includes (line 28, before `<spawn.h>`), supplying `O_RDONLY` for the new `posix_spawn_file_actions_addopen` call.

2. **Stdin redirect to `/dev/null`**: `posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)` inserted as the first file-action in `run_git`, before the stdout/stderr dup2 calls. This ensures any child process (pager, credential prompt, etc.) immediately gets EOF on stdin rather than blocking on the parent's stdin.

3. **`--no-pager` in exec argv**: The `full` vector was changed from `{"git", "-C", repo}` to `{"git", "--no-pager", "-C", repo}`. This suppresses git's built-in pager for commands like `log`, `diff`, `show`. The logged command string is built separately (`cmd` vector, still `"git " + argv...` without `--no-pager`), so proclog entries read `git log --oneline` as expected — the `--no-pager` is exec-only.

### New test — RED → GREEN

**Test #191**: `run_git_command log does not hang (--no-pager + stdin=/dev/null)`
- **RED before fix**: Before the `--no-pager`/stdin fix, `git log --oneline` would block waiting for the pager if `GIT_PAGER` or `core.pager` is set (or if git defaults to `less`). The test would hang indefinitely.
- **GREEN after fix**: The test passes in 0.04s. It asserts:
  - `run_git_command(repo, "log --oneline")` returns 0
  - The proclog `$` entry exists and its `output` is non-empty (contains the commit subject "initial commit for log")
  - `run_git_command(repo, "diff")` on a clean repo also returns promptly (exits 0)

### Test results

| Platform | Tests | Status |
|----------|-------|--------|
| macOS (`cmake --build build && ctest`)  | 271/271 | PASS |
| Alpine/musl (`docker build -f docker/Dockerfile.alpine`) | 271/271 | PASS |

### Confirmation

1. `run_git` now has `posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)` before the stdout/stderr dup2 calls — confirmed in place.
2. `--no-pager` is in the exec argv only (the `full` vector), not in the `cmd` vector used for proclog — confirmed: proclog entries still read `git log --oneline`, not `git --no-pager log --oneline`.
3. New test #191 is GREEN.
4. All pre-existing tests (commit hook-rejection, merge/cherry-pick conflict CLI-write tests, etc.) still pass: 271/271 on both macOS and Alpine.

### Concern

None. The `--no-pager` + `/dev/null` stdin approach is the minimal correct fix and is idempotent with the existing `apply_via_cli` / `commit_via_cli` callers. The `git_terminal` path (which intentionally inherits stdio for interactive use) is untouched.
