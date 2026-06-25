// Real git status spike: index parse + parallel worktree walk + lstat compare
// Zig 0.16 std.Io.Dir API
// Target: ~/src/roll20-private-sheets (37,515 tracked files, clean working tree)

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;

// ── Index entry (what we store per tracked path) ──────────────────────────────

const IndexEntry = struct {
    mtime_ns: i96, // mtime_sec*1e9 + mtime_nsec, same units as Io.Timestamp.nanoseconds
    size: u32, // file size from index (u32 in git format)
    mode: u32, // full POSIX mode (e.g. 0o100644)
    seen: bool = false, // set during walk to detect DELETED
};

// ── Read & parse .git/index ───────────────────────────────────────────────────

fn readBigEndianU32(data: []const u8, offset: usize) u32 {
    return (@as(u32, data[offset]) << 24) |
        (@as(u32, data[offset + 1]) << 16) |
        (@as(u32, data[offset + 2]) << 8) |
        @as(u32, data[offset + 3]);
}

const IndexMap = std.StringHashMap(IndexEntry);

/// Parse .git/index and build a hashmap path -> IndexEntry.
/// Caller owns all allocated memory; free with freeIndexMap().
fn parseGitIndex(
    io: Io,
    git_dir: Dir,
    allocator: std.mem.Allocator,
) !IndexMap {
    // Read the whole index file into memory
    const index_data = try git_dir.readFileAlloc(io, "index", allocator, .unlimited);
    defer allocator.free(index_data);

    // Verify magic "DIRC"
    if (index_data.len < 12) return error.InvalidIndex;
    if (!std.mem.eql(u8, index_data[0..4], "DIRC")) return error.InvalidIndex;

    const version = readBigEndianU32(index_data, 4);
    if (version != 2 and version != 3) return error.UnsupportedIndexVersion;

    const entry_count = readBigEndianU32(index_data, 8);

    var map = IndexMap.init(allocator);
    try map.ensureTotalCapacity(entry_count + entry_count / 4);

    var pos: usize = 12; // skip 12-byte header

    for (0..entry_count) |_| {
        if (pos + 62 > index_data.len) break;

        // Each entry starts at `entry_start` (= current pos).
        // Fixed 62-byte header layout:
        //   [0..4)   ctime_sec
        //   [4..8)   ctime_nsec
        //   [8..12)  mtime_sec
        //  [12..16)  mtime_nsec
        //  [16..20)  dev
        //  [20..24)  ino
        //  [24..28)  mode
        //  [28..32)  uid
        //  [32..36)  gid
        //  [36..40)  size
        //  [40..60)  SHA1 (20 bytes)
        //  [60..62)  flags (2 bytes)
        // Followed by: optional 2-byte extended flags (v3+ only, when bit 14 of flags is set)
        // Followed by: NUL-terminated path
        // The entire entry (from entry_start) is padded to an 8-byte boundary
        // with at least 1 NUL byte (the path terminator counts).
        const entry_start = pos;

        const mtime_sec = readBigEndianU32(index_data, pos + 8);
        const mtime_nsec = readBigEndianU32(index_data, pos + 12);
        const mode = readBigEndianU32(index_data, pos + 24);
        const size = readBigEndianU32(index_data, pos + 36);

        // flags: bit 14 = extended (v3+), low 12 bits = name length hint
        const flags = (@as(u16, index_data[pos + 60]) << 8) | @as(u16, index_data[pos + 61]);
        const name_len_hint = flags & 0x0FFF; // 0xFFF means name >= 0xFFF bytes

        // Skip past fixed header (and optional extended flags in v3+)
        pos += 62;
        if (version >= 3 and (flags & 0x4000) != 0) {
            pos += 2; // skip 2-byte extended flags
        }

        // Read NUL-terminated path
        const path_start = pos;
        const path_len: usize = if (name_len_hint != 0x0FFF) name_len_hint else blk: {
            var i: usize = 0;
            while (pos + i < index_data.len and index_data[pos + i] != 0) : (i += 1) {}
            break :blk i;
        };

        if (path_start + path_len > index_data.len) break;
        const path_slice = index_data[path_start .. path_start + path_len];

        // Advance pos: entry is padded to 8-byte boundary from entry_start.
        // bytes_used = (pos - entry_start) + path_len + 1 (NUL)
        // next entry starts at: entry_start + round_up_8(bytes_used)
        const bytes_used = (pos - entry_start) + path_len + 1;
        const padded = (bytes_used + 7) & ~@as(usize, 7);
        pos = entry_start + padded;

        // Convert mtime to nanoseconds (i96) to match Io.Timestamp.nanoseconds
        const mtime_ns: i96 = @as(i96, mtime_sec) * 1_000_000_000 + @as(i96, mtime_nsec);

        // Duplicate the path key (we need it to outlive index_data)
        const owned_path = try allocator.dupe(u8, path_slice);
        try map.put(owned_path, .{
            .mtime_ns = mtime_ns,
            .size = size,
            .mode = mode,
        });
    }

    return map;
}

/// Free index map: free all owned key strings, then free the map itself.
fn freeIndexMap(map: *IndexMap, allocator: std.mem.Allocator) void {
    var it = map.iterator();
    while (it.next()) |entry| {
        allocator.free(entry.key_ptr.*);
    }
    map.deinit();
}

// ── Status counters ──────────────────────────────────────────────────────────

const StatusCounts = struct {
    modified: std.atomic.Value(usize),
    untracked: std.atomic.Value(usize),
    files_seen: std.atomic.Value(usize),
    dirs_seen: std.atomic.Value(usize),
};

// ── Walk context (path-building + index lookup) ───────────────────────────────

/// Walk `dir` whose path relative to repo root is `rel_prefix` (e.g. "src/foo").
/// For each file, look up `rel_prefix/name` in the index.
fn walkDirStatus(
    io: Io,
    dir: Dir,
    rel_prefix: []const u8, // e.g. "" for root, "foo/bar" for subdirs
    index_map: *IndexMap,
    counts: *StatusCounts,
    path_buf: []u8, // scratch buffer for building relative paths
    allocator: std.mem.Allocator,
) void {
    var it = dir.iterate();
    while (it.next(io) catch null) |entry| {
        if (entry.kind == .directory) {
            if (std.mem.eql(u8, entry.name, ".git")) continue;

            // Build new rel path: rel_prefix + "/" + name  (or just name at root)
            var sub_rel: []u8 = undefined;
            if (rel_prefix.len == 0) {
                sub_rel = allocator.dupe(u8, entry.name) catch continue;
            } else {
                sub_rel = std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, entry.name }) catch continue;
            }
            defer allocator.free(sub_rel);

            _ = counts.dirs_seen.fetchAdd(1, .monotonic);
            _ = dir.statFile(io, entry.name, .{ .follow_symlinks = false }) catch {};
            const sub_dir = dir.openDir(io, entry.name, .{
                .iterate = true,
                .access_sub_paths = true,
            }) catch continue;
            defer sub_dir.close(io);
            walkDirStatus(io, sub_dir, sub_rel, index_map, counts, path_buf, allocator);
        } else {
            // Build relative path for this file
            var file_rel: []u8 = undefined;
            if (rel_prefix.len == 0) {
                file_rel = allocator.dupe(u8, entry.name) catch continue;
            } else {
                file_rel = std.fmt.allocPrint(allocator, "{s}/{s}", .{ rel_prefix, entry.name }) catch continue;
            }
            defer allocator.free(file_rel);

            _ = counts.files_seen.fetchAdd(1, .monotonic);

            // lstat the file
            const st = dir.statFile(io, entry.name, .{ .follow_symlinks = false }) catch {
                // can't stat → treat as untracked
                if (index_map.getPtr(file_rel) == null) {
                    _ = counts.untracked.fetchAdd(1, .monotonic);
                }
                continue;
            };

            // Look up in index
            if (index_map.getPtr(file_rel)) |idx| {
                idx.seen = true;
                // Compare: mtime, size, mode
                // For symlinks git stores only the file-type bits (0o120000),
                // stripping permission bits.  Match that behaviour.
                const stat_mode_raw = @intFromEnum(st.permissions);
                const S_IFMT: u32 = 0o170000;
                const is_symlink = (stat_mode_raw & S_IFMT) == 0o120000;
                const stat_mode: u32 = if (is_symlink) stat_mode_raw & S_IFMT else stat_mode_raw;
                const stat_size: u32 = @truncate(st.size); // index stores u32
                if (st.mtime.nanoseconds != idx.mtime_ns or
                    stat_size != idx.size or
                    stat_mode != idx.mode)
                {
                    _ = counts.modified.fetchAdd(1, .monotonic);
                }
                // else: unmodified (clean)
            } else {
                // Not in index → untracked (ignoring .gitignore for now)
                _ = counts.untracked.fetchAdd(1, .monotonic);
            }
        }
    }
}

// ── Single-threaded full status ───────────────────────────────────────────────

fn singleThreadedStatus(
    io: Io,
    root_path: []const u8,
    allocator: std.mem.Allocator,
) !struct { ns: i64, files: usize, dirs: usize, modified: usize, untracked: usize, deleted: usize } {
    const t0 = Io.Timestamp.now(io, .awake).nanoseconds;

    // Parse index
    const git_index_path = try std.fmt.allocPrint(allocator, "{s}/.git", .{root_path});
    defer allocator.free(git_index_path);
    const git_dir = try Dir.openDirAbsolute(io, git_index_path, .{ .access_sub_paths = true });
    defer git_dir.close(io);

    var index_map = try parseGitIndex(io, git_dir, allocator);
    defer freeIndexMap(&index_map, allocator);

    // Walk + compare
    var counts = StatusCounts{
        .modified = std.atomic.Value(usize).init(0),
        .untracked = std.atomic.Value(usize).init(0),
        .files_seen = std.atomic.Value(usize).init(0),
        .dirs_seen = std.atomic.Value(usize).init(0),
    };

    const root = try Dir.openDirAbsolute(io, root_path, .{
        .iterate = true,
        .access_sub_paths = true,
    });
    defer root.close(io);

    var path_buf: [4096]u8 = undefined;
    walkDirStatus(io, root, "", &index_map, &counts, &path_buf, allocator);

    // Count DELETED: index entries not seen during walk
    var deleted: usize = 0;
    var it = index_map.iterator();
    while (it.next()) |entry| {
        if (!entry.value_ptr.seen) deleted += 1;
    }

    const t1 = Io.Timestamp.now(io, .awake).nanoseconds;

    return .{
        .ns = @intCast(t1 - t0),
        .files = counts.files_seen.load(.monotonic),
        .dirs = counts.dirs_seen.load(.monotonic),
        .modified = counts.modified.load(.monotonic),
        .untracked = counts.untracked.load(.monotonic),
        .deleted = deleted,
    };
}

// ── Multi-threaded full status ────────────────────────────────────────────────

/// Collect top-level subdirs (excluding .git) as absolute paths
fn collectTopLevelDirs(
    io: Io,
    root_path: []const u8,
    allocator: std.mem.Allocator,
) !std.ArrayList([]u8) {
    var dirs: std.ArrayList([]u8) = .empty;
    const root = try Dir.openDirAbsolute(io, root_path, .{
        .iterate = true,
        .access_sub_paths = true,
    });
    defer root.close(io);

    var it = root.iterate();
    while (try it.next(io)) |entry| {
        if (entry.kind == .directory) {
            if (std.mem.eql(u8, entry.name, ".git")) continue;
            const sub_path = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ root_path, entry.name });
            try dirs.append(allocator, sub_path);
        }
    }
    return dirs;
}

const ThreadCtx = struct {
    // Absolute paths this thread owns (each is a top-level subdir)
    abs_paths: []const []const u8,
    // Relative names (base name only, e.g. "src") matching abs_paths
    rel_names: []const []const u8,
    // Shared index map (read-only after construction, but we mutate `seen`)
    index_map: *IndexMap,
    counts: *StatusCounts,
    thread_alloc: std.mem.Allocator,
};

fn threadFn(ctx: ThreadCtx) void {
    var t_threaded = std.Io.Threaded.init_single_threaded;
    const tio = t_threaded.io();
    for (ctx.abs_paths, ctx.rel_names) |abs_path, rel_name| {
        _ = ctx.counts.dirs_seen.fetchAdd(1, .monotonic);
        const sub = Dir.openDirAbsolute(tio, abs_path, .{
            .iterate = true,
            .access_sub_paths = true,
        }) catch continue;
        defer sub.close(tio);

        var path_buf: [4096]u8 = undefined;
        walkDirStatus(tio, sub, rel_name, ctx.index_map, ctx.counts, &path_buf, ctx.thread_alloc);
    }
}

fn multiThreadedStatus(
    io: Io,
    root_path: []const u8,
    allocator: std.mem.Allocator,
    n_threads: usize,
    index_map: *IndexMap, // pre-built, shared across threads (read-only + seen flags)
) !struct { ns: i64, files: usize, dirs: usize, modified: usize, untracked: usize, deleted: usize } {
    // Reset all 'seen' flags in the index (for repeated runs)
    {
        var it = index_map.iterator();
        while (it.next()) |entry| {
            entry.value_ptr.seen = false;
        }
    }

    var counts = StatusCounts{
        .modified = std.atomic.Value(usize).init(0),
        .untracked = std.atomic.Value(usize).init(0),
        .files_seen = std.atomic.Value(usize).init(0),
        .dirs_seen = std.atomic.Value(usize).init(0),
    };

    var top_dirs = try collectTopLevelDirs(io, root_path, allocator);
    defer {
        for (top_dirs.items) |p| allocator.free(p);
        top_dirs.deinit(allocator);
    }

    const t0 = Io.Timestamp.now(io, .awake).nanoseconds;

    // Handle root-level files on main thread
    {
        const root = try Dir.openDirAbsolute(io, root_path, .{
            .iterate = true,
            .access_sub_paths = true,
        });
        defer root.close(io);
        var it = root.iterate();
        while (try it.next(io)) |entry| {
            if (entry.kind != .directory) {
                _ = counts.files_seen.fetchAdd(1, .monotonic);
                const st = root.statFile(io, entry.name, .{ .follow_symlinks = false }) catch {
                    if (index_map.getPtr(entry.name) == null) {
                        _ = counts.untracked.fetchAdd(1, .monotonic);
                    }
                    continue;
                };
                if (index_map.getPtr(entry.name)) |idx| {
                    idx.seen = true;
                    const stat_mode_raw = @intFromEnum(st.permissions);
                    const S_IFMT: u32 = 0o170000;
                    const is_symlink = (stat_mode_raw & S_IFMT) == 0o120000;
                    const stat_mode: u32 = if (is_symlink) stat_mode_raw & S_IFMT else stat_mode_raw;
                    const stat_size: u32 = @truncate(st.size);
                    if (st.mtime.nanoseconds != idx.mtime_ns or
                        stat_size != idx.size or
                        stat_mode != idx.mode)
                    {
                        _ = counts.modified.fetchAdd(1, .monotonic);
                    }
                } else {
                    _ = counts.untracked.fetchAdd(1, .monotonic);
                }
            }
        }
    }

    const actual_threads = @min(n_threads, top_dirs.items.len);

    if (actual_threads == 0) {
        const t1 = Io.Timestamp.now(io, .awake).nanoseconds;
        return .{
            .ns = @intCast(t1 - t0),
            .files = counts.files_seen.load(.monotonic),
            .dirs = counts.dirs_seen.load(.monotonic),
            .modified = counts.modified.load(.monotonic),
            .untracked = counts.untracked.load(.monotonic),
            .deleted = 0,
        };
    }

    // Build per-thread path lists (round-robin)
    const thread_abs_lists = try allocator.alloc(std.ArrayList([]const u8), actual_threads);
    defer allocator.free(thread_abs_lists);
    const thread_rel_lists = try allocator.alloc(std.ArrayList([]const u8), actual_threads);
    defer allocator.free(thread_rel_lists);

    for (0..actual_threads) |i| {
        thread_abs_lists[i] = .empty;
        thread_rel_lists[i] = .empty;
    }
    defer for (0..actual_threads) |i| {
        thread_abs_lists[i].deinit(allocator);
        thread_rel_lists[i].deinit(allocator);
    };

    for (top_dirs.items, 0..) |abs_p, i| {
        const idx = i % actual_threads;
        try thread_abs_lists[idx].append(allocator, abs_p);
        // Extract base name (part after last '/')
        const base = if (std.mem.lastIndexOf(u8, abs_p, "/")) |slash|
            abs_p[slash + 1 ..]
        else
            abs_p;
        try thread_rel_lists[idx].append(allocator, base);
    }

    const thread_ctxs = try allocator.alloc(ThreadCtx, actual_threads);
    defer allocator.free(thread_ctxs);
    for (0..actual_threads) |i| {
        thread_ctxs[i] = .{
            .abs_paths = thread_abs_lists[i].items,
            .rel_names = thread_rel_lists[i].items,
            .index_map = index_map,
            .counts = &counts,
            .thread_alloc = allocator,
        };
    }

    const threads = try allocator.alloc(std.Thread, actual_threads);
    defer allocator.free(threads);
    for (threads, thread_ctxs) |*th, ctx| {
        th.* = try std.Thread.spawn(.{}, threadFn, .{ctx});
    }
    for (threads) |th| th.join();

    const t1 = Io.Timestamp.now(io, .awake).nanoseconds;

    // Count DELETED
    var deleted: usize = 0;
    {
        var it = index_map.iterator();
        while (it.next()) |entry| {
            if (!entry.value_ptr.seen) deleted += 1;
        }
    }

    return .{
        .ns = @intCast(t1 - t0),
        .files = counts.files_seen.load(.monotonic),
        .dirs = counts.dirs_seen.load(.monotonic),
        .modified = counts.modified.load(.monotonic),
        .untracked = counts.untracked.load(.monotonic),
        .deleted = deleted,
    };
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn median(samples: []i64) i64 {
    std.mem.sort(i64, samples, {}, std.sort.asc(i64));
    return samples[samples.len / 2];
}

fn nowNs(io: Io) i96 {
    return Io.Timestamp.now(io, .awake).nanoseconds;
}

// ── main ──────────────────────────────────────────────────────────────────────

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const allocator = init.gpa;

    var args_it = init.minimal.args.iterate();
    _ = args_it.skip();
    const target = args_it.next() orelse {
        std.debug.print("Usage: spike <target_dir>\n", .{});
        return error.MissingArg;
    };

    const cpu_count = std.Thread.getCpuCount() catch 4;
    std.debug.print("Target: {s}\nCPU count: {d}\n\n", .{ target, cpu_count });

    const N = 5;

    // ── Single-threaded full status ──────────────────────────────────────────
    std.debug.print("Running single-threaded full status ({d} iterations)...\n", .{N});
    var st_samples: [N]i64 = undefined;
    var first_result: @TypeOf(try singleThreadedStatus(io, target, allocator)) = undefined;

    for (0..N) |i| {
        const r = try singleThreadedStatus(io, target, allocator);
        st_samples[i] = r.ns;
        if (i == 0) first_result = r;
        std.debug.print("  [{d}] {d}ms  (files={d} dirs={d} modified={d} untracked={d} deleted={d})\n", .{
            i + 1,
            @divTrunc(r.ns, std.time.ns_per_ms),
            r.files,
            r.dirs,
            r.modified,
            r.untracked,
            r.deleted,
        });
    }
    const st_median_ns = median(&st_samples);

    std.debug.print("\nStatus counts (first run): modified={d}  untracked={d}  deleted={d}\n", .{
        first_result.modified,
        first_result.untracked,
        first_result.deleted,
    });
    std.debug.print("(Expected for clean tree: modified=0, deleted=0; untracked≈0 without .gitignore)\n\n", .{});

    // ── Multi-threaded full status ───────────────────────────────────────────
    // Build index once and reuse across all multi-thread runs
    const git_index_path = try std.fmt.allocPrint(allocator, "{s}/.git", .{target});
    defer allocator.free(git_index_path);
    const git_dir = try Dir.openDirAbsolute(io, git_index_path, .{ .access_sub_paths = true });
    defer git_dir.close(io);

    const t_index_start = nowNs(io);
    var shared_index = try parseGitIndex(io, git_dir, allocator);
    const t_index_end = nowNs(io);
    defer freeIndexMap(&shared_index, allocator);
    std.debug.print("Index parse time: {d}ms ({d} entries)\n\n", .{
        @divTrunc(@as(i64, @intCast(t_index_end - t_index_start)), std.time.ns_per_ms),
        shared_index.count(),
    });

    const thread_counts = [_]usize{ 4, 8, cpu_count };
    var mt_medians: [3]i64 = undefined;

    for (thread_counts, 0..) |n_threads, ti| {
        std.debug.print("Running multi-threaded full status ({d} threads, {d} iterations)...\n", .{ n_threads, N });
        var mt_samples: [N]i64 = undefined;
        for (0..N) |i| {
            const r = try multiThreadedStatus(io, target, allocator, n_threads, &shared_index);
            mt_samples[i] = r.ns;
            std.debug.print("  [{d}] {d}ms  (files={d} dirs={d} modified={d} untracked={d} deleted={d})\n", .{
                i + 1,
                @divTrunc(r.ns, std.time.ns_per_ms),
                r.files,
                r.dirs,
                r.modified,
                r.untracked,
                r.deleted,
            });
        }
        mt_medians[ti] = median(&mt_samples);
    }

    // ── Results summary ──────────────────────────────────────────────────────
    std.debug.print("\n=== RESULTS ===\n", .{});
    std.debug.print("Files: {d}  Dirs: {d}\n", .{ first_result.files, first_result.dirs });
    std.debug.print("Single-thread full-status median: {d}ms\n", .{@divTrunc(st_median_ns, std.time.ns_per_ms)});
    for (thread_counts, mt_medians) |n, m| {
        std.debug.print("Multi-thread @{d:2} full-status median: {d}ms\n", .{ n, @divTrunc(m, std.time.ns_per_ms) });
    }

    var best_mt: i64 = mt_medians[0];
    for (mt_medians[1..]) |m| {
        if (m < best_mt) best_mt = m;
    }
    const best_ms = @divTrunc(best_mt, std.time.ns_per_ms);

    const st_ms = @divTrunc(st_median_ns, std.time.ns_per_ms);

    std.debug.print("\nBaselines: libgit2=146ms  git-status=82ms (no untracked cache)\n", .{});
    std.debug.print("Walk-only baselines (prev spike): single=78ms  best-MT=57ms\n", .{});
    std.debug.print("Full-status single-thread: {d}ms  (+{d}ms over walk-only 78ms)\n", .{
        st_ms,
        st_ms - 78,
    });
    std.debug.print("Full-status best MT:       {d}ms  (+{d}ms over walk-only 57ms)\n", .{
        best_ms,
        best_ms - 57,
    });

    if (best_ms < 82) {
        std.debug.print("Verdict: GO — full status faster than git-status baseline\n", .{});
    } else if (best_ms < 146) {
        std.debug.print("Verdict: MARGINAL — full status faster than libgit2, slower than git-status\n", .{});
    } else {
        std.debug.print("Verdict: NO-GO — full status not faster than libgit2 baseline\n", .{});
    }
}
