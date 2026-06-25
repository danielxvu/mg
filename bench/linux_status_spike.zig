// Linux status-perf spike — Zig 0.16
//
// Variants:
//   (a) getdents64 directory listing + per-file statx  (lstat-equivalent baseline)
//   (b) getdents64 + statx with AT_STATX_DONT_SYNC     (avoids metadata sync)
//   (c) io_uring batch statx                           (async batch; skips if unavailable)
//
// Usage:  linux_status_spike <repo-path>
// Build:  zig build-exe bench/linux_status_spike.zig -OReleaseFast
// Cross:  zig build-exe bench/linux_status_spike.zig -target x86_64-linux-gnu -OReleaseFast
//
// Uses std.os.linux raw syscalls throughout — no std.Io.Dir — because the goal
// is to measure the minimal-overhead Linux path directly.

const std = @import("std");
const Io = std.Io;
const linux = std.os.linux;
const posix = std.posix;

// ── Index entry ───────────────────────────────────────────────────────────────

const IndexEntry = struct {
    mtime_ns: i64, // mtime_sec*1e9 + mtime_nsec
    size: u32, // git index stores u32
    mode: u32, // full POSIX mode e.g. 0o100644
    seen: bool = false, // marked during walk to detect DELETED
};

// ── Index parsing ─────────────────────────────────────────────────────────────

fn readBigU32(data: []const u8, offset: usize) u32 {
    return (@as(u32, data[offset]) << 24) |
        (@as(u32, data[offset + 1]) << 16) |
        (@as(u32, data[offset + 2]) << 8) |
        @as(u32, data[offset + 3]);
}

const IndexMap = std.StringHashMap(IndexEntry);

/// Parse .git/index (path = git_dir_path + "/index").
/// Caller owns all memory; free with freeIndexMap().
fn parseGitIndex(git_dir_path: []const u8, allocator: std.mem.Allocator) !IndexMap {
    const index_path_s = try std.fmt.allocPrint(allocator, "{s}/index", .{git_dir_path});
    defer allocator.free(index_path_s);
    const index_path = try allocator.dupeZ(u8, index_path_s);
    defer allocator.free(index_path);

    const fd = blk: {
        const r = linux.open(index_path.ptr, .{ .ACCMODE = .RDONLY, .CLOEXEC = true }, 0);
        switch (linux.errno(r)) {
            .SUCCESS => break :blk @as(linux.fd_t, @intCast(r)),
            else => |e| return posix.unexpectedErrno(e),
        }
    };
    defer _ = linux.close(fd);

    // Stat to get file size
    var stx: linux.Statx = undefined;
    {
        const r = linux.statx(fd, "", linux.AT.EMPTY_PATH, linux.STATX.BASIC_STATS, &stx);
        switch (linux.errno(r)) {
            .SUCCESS => {},
            else => |e| return posix.unexpectedErrno(e),
        }
    }
    const file_size: usize = @intCast(stx.size);
    if (file_size < 12) return error.InvalidIndex;

    const buf = try allocator.alloc(u8, file_size);
    defer allocator.free(buf);

    // Read whole file
    var total: usize = 0;
    while (total < file_size) {
        const r = linux.read(fd, buf.ptr + total, file_size - total);
        switch (linux.errno(r)) {
            .SUCCESS => {
                const n: usize = @intCast(@as(isize, @bitCast(r)));
                if (n == 0) break;
                total += n;
            },
            .INTR => continue,
            else => |e| return posix.unexpectedErrno(e),
        }
    }
    if (total < 12) return error.InvalidIndex;
    const data = buf[0..total];

    if (!std.mem.eql(u8, data[0..4], "DIRC")) return error.InvalidIndex;
    const version = readBigU32(data, 4);
    if (version != 2 and version != 3) return error.UnsupportedIndexVersion;
    const entry_count = readBigU32(data, 8);

    var map = IndexMap.init(allocator);
    try map.ensureTotalCapacity(entry_count + entry_count / 4);

    var pos: usize = 12;
    for (0..entry_count) |_| {
        if (pos + 62 > data.len) break;
        const entry_start = pos;

        const mtime_sec: i64 = @intCast(readBigU32(data, pos + 8));
        const mtime_nsec: i64 = @intCast(readBigU32(data, pos + 12));
        const mode = readBigU32(data, pos + 24);
        const size = readBigU32(data, pos + 36);
        const flags = (@as(u16, data[pos + 60]) << 8) | @as(u16, data[pos + 61]);
        const name_len_hint = flags & 0x0FFF;

        pos += 62;
        if (version >= 3 and (flags & 0x4000) != 0) pos += 2;

        const path_start = pos;
        const path_len: usize = if (name_len_hint != 0x0FFF) name_len_hint else blk: {
            var i: usize = 0;
            while (pos + i < data.len and data[pos + i] != 0) : (i += 1) {}
            break :blk i;
        };
        if (path_start + path_len > data.len) break;
        const path_slice = data[path_start .. path_start + path_len];

        const bytes_used = (pos - entry_start) + path_len + 1;
        const padded = (bytes_used + 7) & ~@as(usize, 7);
        pos = entry_start + padded;

        const mtime_ns: i64 = mtime_sec * 1_000_000_000 + mtime_nsec;
        const owned_path = try allocator.dupe(u8, path_slice);
        try map.put(owned_path, .{
            .mtime_ns = mtime_ns,
            .size = size,
            .mode = mode,
        });
    }
    return map;
}

fn freeIndexMap(map: *IndexMap, allocator: std.mem.Allocator) void {
    var it = map.iterator();
    while (it.next()) |e| allocator.free(e.key_ptr.*);
    map.deinit();
}

fn resetSeenFlags(map: *IndexMap) void {
    var it = map.iterator();
    while (it.next()) |e| {
        e.value_ptr.seen = false;
    }
}

// ── Status counters ───────────────────────────────────────────────────────────

const StatusCounts = struct {
    modified: std.atomic.Value(usize),
    untracked: std.atomic.Value(usize),
    files_seen: std.atomic.Value(usize),
    dirs_seen: std.atomic.Value(usize),
};

// ── Syscall helpers ───────────────────────────────────────────────────────────

fn openDirFd(parent_fd: linux.fd_t, name: [*:0]const u8) !linux.fd_t {
    const r = linux.openat(parent_fd, name, .{
        .ACCMODE = .RDONLY,
        .DIRECTORY = true,
        .NOFOLLOW = true,
        .CLOEXEC = true,
    }, 0);
    switch (linux.errno(r)) {
        .SUCCESS => return @intCast(r),
        .NOENT => return error.FileNotFound,
        .NOTDIR => return error.NotDir,
        .ACCES => return error.AccessDenied,
        .PERM => return error.PermissionDenied,
        else => |e| return posix.unexpectedErrno(e),
    }
}

fn openAbsDirFd(abs_path: []const u8, allocator: std.mem.Allocator) !linux.fd_t {
    const zpath = try allocator.dupeZ(u8, abs_path);
    defer allocator.free(zpath);
    const r = linux.open(zpath.ptr, .{
        .ACCMODE = .RDONLY,
        .DIRECTORY = true,
        .CLOEXEC = true,
    }, 0);
    switch (linux.errno(r)) {
        .SUCCESS => return @intCast(r),
        else => |e| return posix.unexpectedErrno(e),
    }
}

// ── Shared comparison ─────────────────────────────────────────────────────────

fn compareAndMark(
    file_rel: []const u8,
    stx: *const linux.Statx,
    index_map: *IndexMap,
    counts: *StatusCounts,
) void {
    const S_IFMT: u32 = 0o170000;
    const raw_mode: u32 = stx.mode;
    const is_symlink = (raw_mode & S_IFMT) == 0o120000;
    const stat_mode: u32 = if (is_symlink) raw_mode & S_IFMT else raw_mode;
    const stat_size: u32 = @truncate(stx.size);
    const stat_mtime_ns: i64 = stx.mtime.sec *% 1_000_000_000 +% @as(i64, stx.mtime.nsec);

    if (index_map.getPtr(file_rel)) |idx| {
        idx.seen = true;
        if (stat_mtime_ns != idx.mtime_ns or stat_size != idx.size or stat_mode != idx.mode) {
            _ = counts.modified.fetchAdd(1, .monotonic);
        }
    } else {
        _ = counts.untracked.fetchAdd(1, .monotonic);
    }
}

// ── Variant (a): getdents64 + statx per file ─────────────────────────────────

fn walkA(
    dir_fd: linux.fd_t,
    rel_prefix: []const u8,
    index_map: *IndexMap,
    counts: *StatusCounts,
    allocator: std.mem.Allocator,
) void {
    var buf: [65536]u8 align(@alignOf(linux.dirent64)) = undefined;

    while (true) {
        const r = linux.getdents64(dir_fd, &buf, buf.len);
        const rc: isize = @bitCast(r);
        if (rc <= 0) break;
        const n: usize = @intCast(rc);

        var off: usize = 0;
        while (off < n) {
            const d: *linux.dirent64 = @ptrCast(@alignCast(buf[off..].ptr));
            off += d.reclen;

            const name_ptr: [*:0]const u8 = @ptrCast(&d.name);
            const name = std.mem.span(name_ptr);
            if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
            if (std.mem.eql(u8, name, ".git") and rel_prefix.len == 0) continue;

            if (d.type == linux.DT.DIR) {
                _ = counts.dirs_seen.fetchAdd(1, .monotonic);
                const sub_rel = if (rel_prefix.len == 0)
                    allocator.dupe(u8, name) catch continue
                else
                    std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name }) catch continue;
                defer allocator.free(sub_rel);

                const name_z = allocator.dupeZ(u8, name) catch continue;
                defer allocator.free(name_z);
                const sub_fd = openDirFd(dir_fd, name_z.ptr) catch continue;
                defer _ = linux.close(sub_fd);
                walkA(sub_fd, sub_rel, index_map, counts, allocator);
            } else if (d.type == linux.DT.REG or d.type == linux.DT.LNK or d.type == linux.DT.UNKNOWN) {
                _ = counts.files_seen.fetchAdd(1, .monotonic);
                const file_rel = if (rel_prefix.len == 0)
                    allocator.dupe(u8, name) catch continue
                else
                    std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name }) catch continue;
                defer allocator.free(file_rel);

                const name_z = allocator.dupeZ(u8, name) catch continue;
                defer allocator.free(name_z);

                var stx: linux.Statx = undefined;
                const mask: linux.STATX = .{ .TYPE = true, .MODE = true, .SIZE = true, .MTIME = true };
                const sr = linux.statx(dir_fd, name_z.ptr, linux.AT.SYMLINK_NOFOLLOW, mask, &stx);
                if (linux.errno(sr) != .SUCCESS) {
                    if (index_map.getPtr(file_rel) == null)
                        _ = counts.untracked.fetchAdd(1, .monotonic);
                    continue;
                }
                compareAndMark(file_rel, &stx, index_map, counts);
            }
        }
    }
}

// ── Variant (b): getdents64 + statx with AT_STATX_DONT_SYNC ─────────────────

fn walkB(
    dir_fd: linux.fd_t,
    rel_prefix: []const u8,
    index_map: *IndexMap,
    counts: *StatusCounts,
    allocator: std.mem.Allocator,
) void {
    var buf: [65536]u8 align(@alignOf(linux.dirent64)) = undefined;

    while (true) {
        const r = linux.getdents64(dir_fd, &buf, buf.len);
        const rc: isize = @bitCast(r);
        if (rc <= 0) break;
        const n: usize = @intCast(rc);

        var off: usize = 0;
        while (off < n) {
            const d: *linux.dirent64 = @ptrCast(@alignCast(buf[off..].ptr));
            off += d.reclen;

            const name_ptr: [*:0]const u8 = @ptrCast(&d.name);
            const name = std.mem.span(name_ptr);
            if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
            if (std.mem.eql(u8, name, ".git") and rel_prefix.len == 0) continue;

            if (d.type == linux.DT.DIR) {
                _ = counts.dirs_seen.fetchAdd(1, .monotonic);
                const sub_rel = if (rel_prefix.len == 0)
                    allocator.dupe(u8, name) catch continue
                else
                    std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name }) catch continue;
                defer allocator.free(sub_rel);

                const name_z = allocator.dupeZ(u8, name) catch continue;
                defer allocator.free(name_z);
                const sub_fd = openDirFd(dir_fd, name_z.ptr) catch continue;
                defer _ = linux.close(sub_fd);
                walkB(sub_fd, sub_rel, index_map, counts, allocator);
            } else if (d.type == linux.DT.REG or d.type == linux.DT.LNK or d.type == linux.DT.UNKNOWN) {
                _ = counts.files_seen.fetchAdd(1, .monotonic);
                const file_rel = if (rel_prefix.len == 0)
                    allocator.dupe(u8, name) catch continue
                else
                    std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name }) catch continue;
                defer allocator.free(file_rel);

                const name_z = allocator.dupeZ(u8, name) catch continue;
                defer allocator.free(name_z);

                var stx: linux.Statx = undefined;
                const flags: u32 = linux.AT.SYMLINK_NOFOLLOW | linux.AT.STATX_DONT_SYNC;
                const mask: linux.STATX = .{ .TYPE = true, .MODE = true, .SIZE = true, .MTIME = true };
                const sr = linux.statx(dir_fd, name_z.ptr, flags, mask, &stx);
                if (linux.errno(sr) != .SUCCESS) {
                    if (index_map.getPtr(file_rel) == null)
                        _ = counts.untracked.fetchAdd(1, .monotonic);
                    continue;
                }
                compareAndMark(file_rel, &stx, index_map, counts);
            }
        }
    }
}

// ── Variant (c): io_uring batch statx ────────────────────────────────────────

const IoUring = linux.IoUring;
const URING_BATCH: u16 = 256;

const UringEntry = struct {
    file_rel: []u8,
    name_z: [:0]u8,
    dir_fd: linux.fd_t,
    stx: linux.Statx,
};

fn collectFiles(
    dir_fd: linux.fd_t,
    rel_prefix: []const u8,
    files: *std.array_list.Managed(UringEntry),
    open_dirs: *std.array_list.Managed(linux.fd_t),
    counts: *StatusCounts,
    allocator: std.mem.Allocator,
) error{OutOfMemory}!void {
    var buf: [65536]u8 align(@alignOf(linux.dirent64)) = undefined;

    while (true) {
        const r = linux.getdents64(dir_fd, &buf, buf.len);
        const rc: isize = @bitCast(r);
        if (rc <= 0) break;
        const n: usize = @intCast(rc);

        var off: usize = 0;
        while (off < n) {
            const d: *linux.dirent64 = @ptrCast(@alignCast(buf[off..].ptr));
            off += d.reclen;

            const name_ptr: [*:0]const u8 = @ptrCast(&d.name);
            const name = std.mem.span(name_ptr);
            if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
            if (std.mem.eql(u8, name, ".git") and rel_prefix.len == 0) continue;

            if (d.type == linux.DT.DIR) {
                _ = counts.dirs_seen.fetchAdd(1, .monotonic);
                const sub_rel = if (rel_prefix.len == 0)
                    try allocator.dupe(u8, name)
                else
                    try std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name });
                defer allocator.free(sub_rel);

                const name_z = try allocator.dupeZ(u8, name);
                defer allocator.free(name_z);
                const sub_fd = openDirFd(dir_fd, name_z.ptr) catch continue;
                try open_dirs.append(sub_fd);
                try collectFiles(sub_fd, sub_rel, files, open_dirs, counts, allocator);
            } else if (d.type == linux.DT.REG or d.type == linux.DT.LNK or d.type == linux.DT.UNKNOWN) {
                _ = counts.files_seen.fetchAdd(1, .monotonic);
                const file_rel = if (rel_prefix.len == 0)
                    try allocator.dupe(u8, name)
                else
                    try std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, name });
                const name_z = try allocator.dupeZ(u8, name);
                try files.append(.{
                    .file_rel = file_rel,
                    .name_z = name_z,
                    .dir_fd = dir_fd,
                    .stx = undefined,
                });
            }
        }
    }
}

// Returns elapsed ns, or -1 if io_uring is unavailable (prints message).
fn variantC(
    io: Io,
    root_path: []const u8,
    index_map: *IndexMap,
    counts: *StatusCounts,
    allocator: std.mem.Allocator,
) !i64 {
    resetSeenFlags(index_map);
    counts.modified.store(0, .monotonic);
    counts.untracked.store(0, .monotonic);
    counts.files_seen.store(0, .monotonic);
    counts.dirs_seen.store(0, .monotonic);

    var ring = IoUring.init(URING_BATCH, 0) catch |err| switch (err) {
        error.PermissionDenied, error.SystemOutdated => {
            std.debug.print("  io_uring unavailable (sandbox/old kernel) — variant (c) skipped\n", .{});
            return -1;
        },
        else => {
            std.debug.print("  io_uring init error: {} — variant (c) skipped\n", .{err});
            return -1;
        },
    };
    defer ring.deinit();

    const t0 = Io.Timestamp.now(io, .awake);

    const root_fd = try openAbsDirFd(root_path, allocator);
    defer _ = linux.close(root_fd);

    var files = std.array_list.Managed(UringEntry).init(allocator);
    defer {
        for (files.items) |*fe| {
            allocator.free(fe.file_rel);
            allocator.free(fe.name_z);
        }
        files.deinit();
    }
    var open_dirs = std.array_list.Managed(linux.fd_t).init(allocator);
    defer {
        for (open_dirs.items) |fd| _ = linux.close(fd);
        open_dirs.deinit();
    }

    try collectFiles(root_fd, "", &files, &open_dirs, counts, allocator);

    // Submit batches
    const flags: u32 = linux.AT.SYMLINK_NOFOLLOW | linux.AT.STATX_DONT_SYNC;
    const mask: linux.STATX = .{ .TYPE = true, .MODE = true, .SIZE = true, .MTIME = true };

    var i: usize = 0;
    while (i < files.items.len) {
        const batch_end = @min(i + URING_BATCH, files.items.len);
        for (i..batch_end) |j| {
            const fe = &files.items[j];
            _ = try ring.statx(@intCast(j), fe.dir_fd, fe.name_z, flags, mask, &fe.stx);
        }
        _ = try ring.submit_and_wait(@intCast(batch_end - i));

        var cqes: [URING_BATCH]linux.io_uring_cqe = undefined;
        const n_cqe = try ring.copy_cqes(cqes[0..URING_BATCH], @intCast(batch_end - i));
        for (cqes[0..n_cqe]) |cqe| {
            const idx: usize = @intCast(cqe.user_data);
            if (cqe.res < 0) {
                if (index_map.getPtr(files.items[idx].file_rel) == null)
                    _ = counts.untracked.fetchAdd(1, .monotonic);
                continue;
            }
            compareAndMark(files.items[idx].file_rel, &files.items[idx].stx, index_map, counts);
        }
        i = batch_end;
    }

    var deleted: usize = 0;
    {
        var it = index_map.iterator();
        while (it.next()) |e| {
            if (!e.value_ptr.seen) deleted += 1;
        }
    }

    const t1 = Io.Timestamp.now(io, .awake);
    const elapsed_ns: i64 = @intCast(t1.nanoseconds - t0.nanoseconds);
    std.debug.print("  files={d} dirs={d} mod={d} untracked={d} del={d}\n", .{
        counts.files_seen.load(.monotonic),
        counts.dirs_seen.load(.monotonic),
        counts.modified.load(.monotonic),
        counts.untracked.load(.monotonic),
        deleted,
    });
    return elapsed_ns;
}

// ── Parallel walk plumbing ────────────────────────────────────────────────────

const WalkVariant = enum { a, b };

const ThreadCtx = struct {
    abs_paths: []const []const u8,
    rel_names: []const []const u8,
    index_map: *IndexMap,
    counts: *StatusCounts,
    allocator: std.mem.Allocator,
    variant: WalkVariant,
};

fn threadFn(ctx: ThreadCtx) void {
    for (ctx.abs_paths, ctx.rel_names) |abs_path, rel_name| {
        _ = ctx.counts.dirs_seen.fetchAdd(1, .monotonic);
        const fd = openAbsDirFd(abs_path, ctx.allocator) catch continue;
        defer _ = linux.close(fd);
        switch (ctx.variant) {
            .a => walkA(fd, rel_name, ctx.index_map, ctx.counts, ctx.allocator),
            .b => walkB(fd, rel_name, ctx.index_map, ctx.counts, ctx.allocator),
        }
    }
}

fn collectTopLevelDirs(root_path: []const u8, allocator: std.mem.Allocator) !std.array_list.Managed([]u8) {
    const root_fd = try openAbsDirFd(root_path, allocator);
    defer _ = linux.close(root_fd);

    var dirs = std.array_list.Managed([]u8).init(allocator);
    var buf: [65536]u8 align(@alignOf(linux.dirent64)) = undefined;

    while (true) {
        const r = linux.getdents64(root_fd, &buf, buf.len);
        const rc: isize = @bitCast(r);
        if (rc <= 0) break;
        const n: usize = @intCast(rc);

        var off: usize = 0;
        while (off < n) {
            const d: *linux.dirent64 = @ptrCast(@alignCast(buf[off..].ptr));
            off += d.reclen;
            const name_ptr: [*:0]const u8 = @ptrCast(&d.name);
            const name = std.mem.span(name_ptr);
            if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
            if (std.mem.eql(u8, name, ".git")) continue;
            if (d.type == linux.DT.DIR) {
                const p = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ root_path, name });
                try dirs.append(p);
            }
        }
    }
    return dirs;
}

// ── Run one variant (single or multi-thread) ──────────────────────────────────

const RunResult = struct {
    ns: i64,
    files: usize,
    dirs: usize,
    modified: usize,
    untracked: usize,
    deleted: usize,
};

fn runVariant(
    io: Io,
    root_path: []const u8,
    index_map: *IndexMap,
    n_threads: usize,
    variant: WalkVariant,
    allocator: std.mem.Allocator,
) !RunResult {
    resetSeenFlags(index_map);

    var counts = StatusCounts{
        .modified = std.atomic.Value(usize).init(0),
        .untracked = std.atomic.Value(usize).init(0),
        .files_seen = std.atomic.Value(usize).init(0),
        .dirs_seen = std.atomic.Value(usize).init(0),
    };

    var top_dirs = try collectTopLevelDirs(root_path, allocator);
    defer {
        for (top_dirs.items) |p| allocator.free(p);
        top_dirs.deinit();
    }

    const t0 = Io.Timestamp.now(io, .awake);

    // Root-level files on main thread
    {
        const root_fd = try openAbsDirFd(root_path, allocator);
        defer _ = linux.close(root_fd);
        var buf: [65536]u8 align(@alignOf(linux.dirent64)) = undefined;
        while (true) {
            const r = linux.getdents64(root_fd, &buf, buf.len);
            const rc: isize = @bitCast(r);
            if (rc <= 0) break;
            const n: usize = @intCast(rc);
            var off: usize = 0;
            while (off < n) {
                const d: *linux.dirent64 = @ptrCast(@alignCast(buf[off..].ptr));
                off += d.reclen;
                const name_ptr: [*:0]const u8 = @ptrCast(&d.name);
                const name = std.mem.span(name_ptr);
                if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
                if (std.mem.eql(u8, name, ".git")) continue;
                if (d.type == linux.DT.REG or d.type == linux.DT.LNK or d.type == linux.DT.UNKNOWN) {
                    _ = counts.files_seen.fetchAdd(1, .monotonic);
                    const name_z = allocator.dupeZ(u8, name) catch continue;
                    defer allocator.free(name_z);
                    var stx: linux.Statx = undefined;
                    const dont_sync: u32 = if (variant == .b) linux.AT.STATX_DONT_SYNC else @as(u32, 0);
                    const fl: u32 = linux.AT.SYMLINK_NOFOLLOW | dont_sync;
                    const mask: linux.STATX = .{ .TYPE = true, .MODE = true, .SIZE = true, .MTIME = true };
                    const sr = linux.statx(root_fd, name_z.ptr, fl, mask, &stx);
                    if (linux.errno(sr) != .SUCCESS) {
                        if (index_map.getPtr(name) == null)
                            _ = counts.untracked.fetchAdd(1, .monotonic);
                        continue;
                    }
                    compareAndMark(name, &stx, index_map, &counts);
                }
            }
        }
    }

    const actual_threads = @min(n_threads, top_dirs.items.len);
    if (actual_threads == 0) {
        const t1 = Io.Timestamp.now(io, .awake);
        var deleted: usize = 0;
        {
            var it = index_map.iterator();
            while (it.next()) |e| {
                if (!e.value_ptr.seen) deleted += 1;
            }
        }
        return .{
            .ns = @intCast(t1.nanoseconds - t0.nanoseconds),
            .files = counts.files_seen.load(.monotonic),
            .dirs = counts.dirs_seen.load(.monotonic),
            .modified = counts.modified.load(.monotonic),
            .untracked = counts.untracked.load(.monotonic),
            .deleted = deleted,
        };
    }

    // Round-robin assign top-level dirs to threads
    const thread_abs = try allocator.alloc(std.ArrayListUnmanaged([]const u8), actual_threads);
    defer allocator.free(thread_abs);
    const thread_rel = try allocator.alloc(std.ArrayListUnmanaged([]const u8), actual_threads);
    defer allocator.free(thread_rel);
    for (0..actual_threads) |i| {
        thread_abs[i] = .empty;
        thread_rel[i] = .empty;
    }
    defer for (0..actual_threads) |i| {
        thread_abs[i].deinit(allocator);
        thread_rel[i].deinit(allocator);
    };

    for (top_dirs.items, 0..) |abs_p, i| {
        const idx = i % actual_threads;
        try thread_abs[idx].append(allocator, abs_p);
        const base = if (std.mem.lastIndexOf(u8, abs_p, "/")) |s| abs_p[s + 1 ..] else abs_p;
        try thread_rel[idx].append(allocator, base);
    }

    const ctxs = try allocator.alloc(ThreadCtx, actual_threads);
    defer allocator.free(ctxs);
    for (0..actual_threads) |i| {
        ctxs[i] = .{
            .abs_paths = thread_abs[i].items,
            .rel_names = thread_rel[i].items,
            .index_map = index_map,
            .counts = &counts,
            .allocator = allocator,
            .variant = variant,
        };
    }

    const threads = try allocator.alloc(std.Thread, actual_threads);
    defer allocator.free(threads);
    for (threads, ctxs) |*th, ctx| {
        th.* = try std.Thread.spawn(.{}, threadFn, .{ctx});
    }
    for (threads) |th| th.join();

    const t1 = Io.Timestamp.now(io, .awake);
    var deleted: usize = 0;
    {
        var it = index_map.iterator();
        while (it.next()) |e| {
            if (!e.value_ptr.seen) deleted += 1;
        }
    }
    return .{
        .ns = @intCast(t1.nanoseconds - t0.nanoseconds),
        .files = counts.files_seen.load(.monotonic),
        .dirs = counts.dirs_seen.load(.monotonic),
        .modified = counts.modified.load(.monotonic),
        .untracked = counts.untracked.load(.monotonic),
        .deleted = deleted,
    };
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn median5(samples: *[5]i64) i64 {
    std.mem.sort(i64, samples, {}, std.sort.asc(i64));
    return samples[2];
}

// ── main ──────────────────────────────────────────────────────────────────────

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const allocator = init.gpa;

    var args_it = init.minimal.args.iterate();
    _ = args_it.skip(); // argv[0]
    const root_path = args_it.next() orelse {
        std.debug.print("Usage: linux_status_spike <repo-path>\n", .{});
        return error.MissingArg;
    };

    const cpu_count = std.Thread.getCpuCount() catch 4;
    std.debug.print("Target: {s}\nCPU count: {d}\n\n", .{ root_path, cpu_count });

    const git_dir = try std.fmt.allocPrint(allocator, "{s}/.git", .{root_path});
    defer allocator.free(git_dir);

    // Parse index once; share across all runs (reset .seen per run inside runVariant)
    const t_idx0 = Io.Timestamp.now(io, .awake);
    var index_map = try parseGitIndex(git_dir, allocator);
    defer freeIndexMap(&index_map, allocator);
    const t_idx1 = Io.Timestamp.now(io, .awake);
    std.debug.print("Index parse: {d}ms  ({d} entries)\n\n", .{
        @divTrunc(@as(i64, @intCast(t_idx1.nanoseconds - t_idx0.nanoseconds)), std.time.ns_per_ms),
        index_map.count(),
    });

    const N = 5;
    const thread_counts = [_]usize{ 1, 4, cpu_count };
    const variant_names = [_][]const u8{ "(a) statx/file", "(b) statx+DONT_SYNC" };
    const variants = [_]WalkVariant{ .a, .b };

    // results[variant_idx][thread_count_idx] = median ns
    var results: [2][3]i64 = .{ .{ 0, 0, 0 }, .{ 0, 0, 0 } };
    var first_result: RunResult = undefined;
    var got_first = false;

    for (variants, 0..) |variant, vi| {
        for (thread_counts, 0..) |n_threads, ti| {
            std.debug.print("=== {s}  threads={d} ===\n", .{ variant_names[vi], n_threads });
            var samples: [N]i64 = undefined;
            for (0..N) |i| {
                const res = try runVariant(io, root_path, &index_map, n_threads, variant, allocator);
                samples[i] = res.ns;
                std.debug.print("  [{d}] {d}ms  mod={d} untracked={d} del={d}\n", .{
                    i + 1,
                    @divTrunc(res.ns, std.time.ns_per_ms),
                    res.modified,
                    res.untracked,
                    res.deleted,
                });
                if (!got_first) {
                    first_result = res;
                    got_first = true;
                }
            }
            results[vi][ti] = median5(&samples);
            std.debug.print("\n", .{});
        }
    }

    // Variant (c): io_uring
    std.debug.print("=== (c) io_uring batch statx (single-threaded collection + async submit) ===\n", .{});
    {
        var counts_c = StatusCounts{
            .modified = std.atomic.Value(usize).init(0),
            .untracked = std.atomic.Value(usize).init(0),
            .files_seen = std.atomic.Value(usize).init(0),
            .dirs_seen = std.atomic.Value(usize).init(0),
        };
        var samples_c: [N]i64 = undefined;
        var uring_ok = true;
        for (0..N) |i| {
            const ns = variantC(io, root_path, &index_map, &counts_c, allocator) catch |err| blk: {
                std.debug.print("  io_uring error: {} — skipped\n", .{err});
                break :blk @as(i64, -1);
            };
            if (ns < 0) {
                uring_ok = false;
                break;
            }
            samples_c[i] = ns;
            std.debug.print("  [{d}] {d}ms\n", .{ i + 1, @divTrunc(ns, std.time.ns_per_ms) });
        }
        if (uring_ok) {
            const c_med = median5(&samples_c);
            std.debug.print("  median: {d}ms\n", .{@divTrunc(c_med, std.time.ns_per_ms)});
        }
        std.debug.print("\n", .{});
    }

    // ── Summary table ──────────────────────────────────────────────────────────
    std.debug.print("=== RESULTS  (median of {d} warm runs) ===\n", .{N});
    if (got_first) {
        std.debug.print("Repo: {d} files  {d} dirs\n", .{ first_result.files, first_result.dirs });
        std.debug.print("Status: modified={d}  untracked={d}  deleted={d}\n", .{
            first_result.modified, first_result.untracked, first_result.deleted,
        });
        if (first_result.modified != 0 or first_result.deleted != 0) {
            std.debug.print("WARNING: tree is not clean — benchmark on a clean repo for best results\n", .{});
        }
    }
    std.debug.print("\n{s:<24} {s:>12} {s:>12} {d:>11}t\n", .{ "Variant", "1-thread", "4-thread", cpu_count });
    std.debug.print("{s}\n", .{"-" ** 62});
    for (variants, 0..) |_, vi| {
        std.debug.print("{s:<24} {d:>11}ms {d:>11}ms {d:>11}ms\n", .{
            variant_names[vi],
            @divTrunc(results[vi][0], std.time.ns_per_ms),
            @divTrunc(results[vi][1], std.time.ns_per_ms),
            @divTrunc(results[vi][2], std.time.ns_per_ms),
        });
    }

    const best_a = @min(results[0][1], results[0][2]);
    const best_b = @min(results[1][1], results[1][2]);
    std.debug.print("\nBest multi-thread: (a)={d}ms  (b)={d}ms\n", .{
        @divTrunc(best_a, std.time.ns_per_ms),
        @divTrunc(best_b, std.time.ns_per_ms),
    });
    if (best_b < best_a) {
        std.debug.print("Verdict: (b) DONT_SYNC beats (a) by {d}ms on this hardware\n", .{
            @divTrunc(best_a - best_b, std.time.ns_per_ms),
        });
    } else {
        std.debug.print("Verdict: (b) did not beat (a) — per-file statx is sufficient\n", .{});
    }
}
