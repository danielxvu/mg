# Linux status-perf spike — design notes

## Variants implemented

### (a) getdents64 + per-file statx
Mirrors git's own per-file lstat loop: iterate a directory with `getdents64`
(one syscall returns many entries including `d_type`, avoiding per-name stat
for directories), then call `statx(AT_SYMLINK_NOFOLLOW, {TYPE|MODE|SIZE|MTIME})`
per regular file.  `d_type` lets us skip the statx call for entries that are
clearly directories, avoiding unnecessary kernel round-trips for the common case.

### (b) getdents64 + statx with `AT_STATX_DONT_SYNC`
Identical walk, but passes `AT_STATX_DONT_SYNC` alongside `AT_SYMLINK_NOFOLLOW`.
On local filesystems this is a no-op; on NFS / distributed filesystems it avoids
a metadata-sync round-trip and so should be universally safe to use.  This is the
Linux analogue of macOS `getattrlistbulk` — the key difference is that
`getattrlistbulk` returns all metadata for an entire directory in one call, while
on Linux we still issue one `statx` per file but with a minimal field mask and
`DONT_SYNC`.  Whether that gap closes with batching is the io_uring experiment.

### (c) io_uring batch statx (OPTIONAL)
Two-phase approach: first traverse the tree with `getdents64` collecting all
(dir_fd, name_z) pairs, then submit them in batches of 256 SQEs via
`IoUring.statx(user_data, fd, path, flags, mask, &stx_buf)` and harvest CQEs.
Avoids the syscall-per-file overhead for the statx phase by letting the kernel
pipeline them.

**Availability guard:** `IoUring.init(entries, 0)` is called at runtime;
if it returns `error.PermissionDenied` (GitHub runner seccomp) or
`error.SystemOutdated` (kernel < 5.4), the variant prints
`io_uring unavailable (sandbox/old kernel) — variant (c) skipped`
and returns −1, which the caller treats as a skip (no fatal).

## Zig 0.16 Linux API details (verified against on-disk source)

| Symbol | Source | Notes |
|---|---|---|
| `linux.getdents64(fd, buf, len)` | `std/os/linux.zig:886` | returns raw usize; error check via `linux.errno()` |
| `linux.statx(dirfd, path, flags, mask, buf)` | `std/os/linux.zig:2459` | `path` is `[*:0]const u8` |
| `linux.dirent64` | `std/os/linux.zig:6297` | `{ino, off, reclen, type, name: [0]u8}` |
| `linux.DT.{REG,DIR,LNK,UNKNOWN}` | `std/os/linux.zig:5091` | numeric values 8,4,10,0 |
| `linux.STATX` | `std/os/linux.zig:7168` | packed struct(u32), fields TYPE/MODE/SIZE/MTIME/etc |
| `linux.STATX.BASIC_STATS` | `std/os/linux.zig:7210` | = `@bitCast(@as(u32, 0x7ff))` |
| `linux.Statx` | `std/os/linux.zig:7254` | `mode: u16`, `size: u64`, `mtime: statx_timestamp` |
| `linux.statx_timestamp` | `std/os/linux.zig:7244` | `{sec: i64, nsec: u32, __pad1: u32}` |
| `linux.AT.SYMLINK_NOFOLLOW` | `std/os/linux.zig:3691` | = `0x100` |
| `linux.AT.EMPTY_PATH` | `std/os/linux.zig:3703` | = `0x1000`, used to statx by fd |
| `linux.AT.STATX_DONT_SYNC` | `std/os/linux.zig:3715` | = `0x4000` |
| `linux.AT.FDCWD` | `std/os/linux.zig:3688` | = −100 |
| `linux.IoUring` | `std/os/linux/IoUring.zig:27` | `init(entries: u16, flags: u32)` |
| `IoUring.statx(ud, fd, path, flags, mask, buf)` | `IoUring.zig:962` | path is `[:0]const u8` |
| `IoUring.submit_and_wait(n)` | `IoUring.zig:164` | returns `!u32` |
| `IoUring.copy_cqes(slice, n)` | `IoUring.zig:279` | returns `!u32` |

**Key 0.16 API gotchas:**

- `std.ArrayList(T)` in 0.16 is the **unmanaged** type (no embedded allocator).
  Use `std.array_list.Managed(T).init(allocator)` for the classic embedded-allocator
  style, or use `.empty` + pass allocator per call to the unmanaged type.
- `std.heap.GeneralPurposeAllocator` is gone; use `std.heap.DebugAllocator`.
- `std.process.argsAlloc` is gone; `main` takes `init: std.process.Init` and
  args are accessed via `init.minimal.args.iterate()`.
- `std.fmt.allocPrintZ` is gone; use `allocPrint` + `allocator.dupeZ(u8, ...)`.
- Timing: `std.time.nanoTimestamp()` is gone; use `Io.Timestamp.now(io, .awake).nanoseconds`.
- `while (it.next()) |e| if (cond) x += 1;` is a parse error; must use braces.

## Benchmark repo generation (`gen_status_repo.sh`)

Creates 150 directories × 250 files each = 37,500 tracked files plus 1 root
README = 37,501 total.  Four file types rotate deterministically (`.c`, `.js`,
`.json`, `.md`) with content that references only the dir/file index — no
timestamps or random data, so the commit SHA is reproducible across machines.
The repo is `git init` + `git add -A` + single commit → clean working tree
verified before exit.

## Cross-compile result

Both targets compiled clean on macOS (Apple Silicon, Zig 0.16 from MacPorts):

```
zig build-exe bench/linux_status_spike.zig -target x86_64-linux-gnu  -OReleaseFast  ✓
zig build-exe bench/linux_status_spike.zig -target x86_64-linux-musl -OReleaseFast  ✓
```

The binary will not run on macOS (Linux syscalls); CI runs it on `ubuntu-latest`.
