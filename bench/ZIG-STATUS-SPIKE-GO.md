# Zig fast-status spike — GO (both platforms)

Question: can a custom Zig worktree walk beat libgit2 (neomg's cold status path)
and git itself? Throwaway spike; this records the result. Reference walkers:
`bench/macos_status_spike.zig` (getattrlistbulk) + `bench/linux_status_spike.zig`
(getdents64/statx/io_uring), CI in `.github/workflows/status-spike.yml`.

## Numbers (full status = index parse + parallel walk + compare; clean tree)

macOS (Apple Silicon, warm cache, roll20 = 37,515 files):
| approach | best |
|---|---|
| Zig getattrlistbulk (18T) | **40 ms** |
| Zig per-file lstat (18T) | 61 ms |
| git status (untracked-cache) | 40 ms |
| git status (no cache) | 82 ms |
| libgit2 full status | 146 ms |
| git + fsmonitor | 25 ms |

Linux (x86_64 GitHub CI, synthetic 37,501 files, same job):
| approach | best |
|---|---|
| Zig parallel statx (4T) | **31 ms** |
| Zig io_uring (1T only) | 68 ms |
| git status (default / no-UTC) | ~50 ms |
| index parse | 6 ms |

neomg's WARM incremental refresh (fs-watch + session) is already 0.8–16 ms — under
git+fsmonitor's 25 ms — and is unaffected by any of this.

## Verdict: GO
- Linux: Zig **31 ms beats `git status` 50 ms** (~38%) and crushes libgit2.
- macOS: Zig **ties git's cached best (40 ms)** with no cache, beats libgit2 3.6×.
- Warm/interactive path already beats fsmonitor.
So Zig is the fastest full-scan on both OSes; the cold ceiling is beaten.

## Caveats
CI runners are shared (~2-4 vCPU) → absolute ms noisy, but same-runner/same-repo
comparisons are valid. Synthetic Linux repo (roll20 is private). Still walk +
index-compare only — gitignore / renames / submodules / racy-git is the
correctness tail, to be carried by a **libgit2 fallback**.

## Recommended architecture (the real build)
`hybrid_status` behind `ENABLE_ZIG_STATUS`:
- Zig walk (getattrlistbulk macOS / statx Linux) → worktree modified/untracked/deleted.
- libgit2 for the index/staged side + as the fallback when anything unusual is
  detected (submodules / sparse-checkout / filters → just use libgit2 full status).
- Wire into the monitor's cold/full path; the warm incremental path stays as-is.
