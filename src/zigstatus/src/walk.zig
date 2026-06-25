// Index-driven parallel walk for zigstatus.
// Adapted from bench/walk-spike/walk_spike.zig (Zig 0.16).
// Only descends directories that contain tracked files (per the git index),
// matching git status -unormal behaviour.  Per-file action: call emit(ctx, x, y, path).
//
// Returns a seen-set (StringHashMapUnmanaged(void)) of every tracked path that
// was found on disk.  The caller (status.run) uses this to detect deleted files:
// any tracked path absent from the seen-set was not present in the worktree.
// Keys are stable slices into the index bytes; no copies needed.
const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const index = @import("index.zig");
const sha1 = @import("sha1.zig");

pub const EmitFn = *const fn (ctx: *anyopaque, x: u8, y: u8, path: []const u8) void;

const Job = struct {
    io: Io,
    gpa: std.mem.Allocator,
    root: Dir,
    names: []const []const u8,
    idx: *const index.Index,
    walk_all: bool, // no usable index -> descend everything
    start: usize,
    stride: usize,
    emit: EmitFn,
    ctx: *anyopaque,
    // Populated by worker after it finishes; merged into the global seen-set
    // by run() after Io.Group.await (single-threaded at that point, no locking).
    seen: std.StringHashMapUnmanaged(void) = .empty,
};

const Ctx = struct {
    io: Io,
    gpa: std.mem.Allocator,
    idx: *const index.Index,
    walk_all: bool,
    path: [4096]u8 = undefined,
    plen: usize = 0,
    emit: EmitFn,
    emit_ctx: *anyopaque,
    // Per-worker seen-set; keys slice into index bytes (stable for the run lifetime).
    seen: *std.StringHashMapUnmanaged(void),
};

// Classify a tracked file: if worktree content differs from index, emit ` M`.
// `dir` is the parent directory, `name` is the filename within `dir`,
// `path` is the repo-relative path (for emit), `ie` is the index entry.
fn classifyTracked(io: Io, gpa: std.mem.Allocator, dir: Dir, name: []const u8, path: []const u8, ie: index.Entry, emit: EmitFn, ctx: *anyopaque) void {
    const st = dir.statFile(io, name, .{ .follow_symlinks = false }) catch return;
    const mt_sec: i64 = @intCast(@divTrunc(st.mtime.nanoseconds, 1_000_000_000));
    const mt_nsec: i64 = @intCast(@mod(st.mtime.nanoseconds, 1_000_000_000));
    var changed = ie.size != @as(i64, @intCast(st.size));
    if (!changed) {
        // Size matches; only hash if mtime differs from index (racy-clean check).
        if (ie.mtime_sec != mt_sec or ie.mtime_nsec != mt_nsec) {
            const content = dir.readFileAlloc(io, name, gpa, .unlimited) catch return;
            defer gpa.free(content);
            changed = !std.mem.eql(u8, &ie.sha, &sha1.gitBlob(gpa, content));
        }
    }
    if (changed) emit(ctx, ' ', 'M', path);
}

// Suppress false-deletes: when a tracked dir can't be opened, mark its index
// files as seen (they degrade to unmodified rather than "deleted").
fn markSubtreeSeen(idx: *const index.Index, seen: *std.StringHashMapUnmanaged(void), gpa: std.mem.Allocator, dirpath: []const u8) void {
    var it = idx.files.keyIterator();
    while (it.next()) |k| {
        const p = k.*;
        if (p.len > dirpath.len and std.mem.startsWith(u8, p, dirpath) and p[dirpath.len] == '/')
            seen.put(gpa, p, {}) catch {};
    }
}

// Descend `dir`. `ctx.path[0..plen]` is its repo-relative path (no leading slash).
fn walkDir(ctx: *Ctx, dir: Dir) void {
    var it = dir.iterate();
    while (it.next(ctx.io) catch null) |ent| {
        if (std.mem.eql(u8, ent.name, ".git")) continue;
        const is_dir = ent.kind == .directory;
        if (!is_dir and ent.kind != .file and ent.kind != .sym_link) continue;

        const save = ctx.plen;
        // Build repo-relative path: prepend existing prefix + "/" + name.
        // (At top-level, plen==0 so we don't insert a leading slash.)
        if (ctx.plen > 0) {
            ctx.path[ctx.plen] = '/';
            ctx.plen += 1;
        }
        @memcpy(ctx.path[ctx.plen ..][0..ent.name.len], ent.name);
        ctx.plen += ent.name.len;
        const cur = ctx.path[0..ctx.plen];

        if (is_dir) {
            // Index-driven: only descend dirs that contain tracked files.
            if (ctx.walk_all or ctx.idx.dirs.contains(cur)) {
                var sub = dir.openDir(ctx.io, ent.name, .{ .iterate = true }) catch {
                    // Permission denied or other error: if this is a tracked dir,
                    // mark its index files as seen so they aren't falsely reported deleted.
                    if (ctx.idx.dirs.contains(cur))
                        markSubtreeSeen(ctx.idx, ctx.seen, ctx.gpa, cur);
                    ctx.plen = save;
                    continue;
                };
                defer sub.close(ctx.io);
                walkDir(ctx, sub);
            }
            // Untracked dirs: not descended (git status -unormal behaviour).
        } else {
            // Classify: not in index -> untracked.
            if (!ctx.idx.files.contains(cur)) {
                // Copy path to stack buffer for emit (cur slices into ctx.path which is stack-allocated
                // and valid for this call, but emit may be called from threads so we pass cur directly --
                // the walk is synchronous within each worker, so the slice is stable for the duration of emit).
                ctx.emit(ctx.emit_ctx, '?', '?', cur);
            } else {
                // Tracked file found on disk: record in the seen-set using the index key
                // (stable slice into index bytes), then classify for modifications.
                const idx_key = ctx.idx.files.getKey(cur).?;
                ctx.seen.put(ctx.gpa, idx_key, {}) catch {};
                const ie = ctx.idx.files.get(cur).?;
                classifyTracked(ctx.io, ctx.gpa, dir, ent.name, cur, ie, ctx.emit, ctx.emit_ctx);
            }
        }
        ctx.plen = save;
    }
}

fn worker(job: *Job) void {
    var i = job.start;
    while (i < job.names.len) : (i += job.stride) {
        const name = job.names[i];
        if (!job.walk_all and !job.idx.dirs.contains(name)) {
            // Top-level untracked dir — not descended (no tracked content).
            continue;
        }
        var d = job.root.openDir(job.io, name, .{ .iterate = true }) catch {
            // Permission denied or other error on a top-level tracked dir:
            // mark its index files as seen so they aren't falsely reported deleted.
            if (job.idx.dirs.contains(name))
                markSubtreeSeen(job.idx, &job.seen, job.gpa, name);
            continue;
        };
        defer d.close(job.io);
        var ctx: Ctx = .{
            .io = job.io,
            .gpa = job.gpa,
            .idx = job.idx,
            .walk_all = job.walk_all,
            .emit = job.emit,
            .emit_ctx = job.ctx,
            .seen = &job.seen,
        };
        @memcpy(ctx.path[0..name.len], name);
        ctx.plen = name.len;
        walkDir(&ctx, d);
    }
}

// Also walk root-level files (not in any subdirectory).
// Tracked root-level files found on disk are recorded in `seen`.
fn walkRootFiles(io: Io, gpa: std.mem.Allocator, root: Dir, idx: *const index.Index, emit: EmitFn, ctx: *anyopaque, seen: *std.StringHashMapUnmanaged(void)) !void {
    var it = root.iterate();
    while (try it.next(io)) |ent| {
        if (std.mem.eql(u8, ent.name, ".git")) continue;
        if (ent.kind != .file and ent.kind != .sym_link) continue;
        // Root-level file: path == name.
        if (!idx.files.contains(ent.name)) {
            emit(ctx, '?', '?', ent.name);
        } else {
            // Record tracked root file as seen (stable index key).
            const idx_key = idx.files.getKey(ent.name).?;
            seen.put(gpa, idx_key, {}) catch {};
            const ie = idx.files.get(ent.name).?;
            classifyTracked(io, gpa, root, ent.name, ent.name, ie, emit, ctx);
        }
    }
}

// Run the index-driven parallel walk.
// Returns a StringHashMapUnmanaged(void) of every tracked path seen on disk.
// The caller is responsible for calling .deinit(gpa) on the returned set.
pub fn run(
    io: Io,
    gpa: std.mem.Allocator,
    root: Dir,
    idx: *const index.Index,
    emit: EmitFn,
    ctx: *anyopaque,
) !std.StringHashMapUnmanaged(void) {
    const walk_all = idx.files.count() == 0;

    // seen accumulates all tracked paths found on disk; returned to caller.
    var seen: std.StringHashMapUnmanaged(void) = .empty;

    // Collect top-level directory names (allocate copies so they outlive the iterator).
    var names: std.ArrayList([]const u8) = .empty;
    defer {
        for (names.items) |n| gpa.free(n);
        names.deinit(gpa);
    }
    {
        var it = root.iterate();
        while (try it.next(io)) |ent| {
            if (ent.kind == .directory and !std.mem.eql(u8, ent.name, ".git"))
                try names.append(gpa, try gpa.dupe(u8, ent.name));
        }
    }

    // Handle root-level files first (single-threaded, no job needed).
    try walkRootFiles(io, gpa, root, idx, emit, ctx, &seen);

    if (names.items.len == 0) return seen;

    const ncpu = std.Thread.getCpuCount() catch 4;
    const nthreads = @min(ncpu, @max(names.items.len, 1));

    const jobs = try gpa.alloc(Job, nthreads);
    defer gpa.free(jobs);
    for (0..nthreads) |w|
        jobs[w] = .{
            .io = io,
            .gpa = gpa,
            .root = root,
            .names = names.items,
            .idx = idx,
            .walk_all = walk_all,
            .start = w,
            .stride = nthreads,
            .emit = emit,
            .ctx = ctx,
        };

    var group: Io.Group = .init;
    for (0..nthreads) |w| group.async(io, worker, .{&jobs[w]});
    group.await(io) catch {};

    // Merge per-worker seen-sets into the global seen (single-threaded after await).
    for (0..nthreads) |w| {
        var kit = jobs[w].seen.keyIterator();
        while (kit.next()) |k| seen.put(gpa, k.*, {}) catch {};
        jobs[w].seen.deinit(gpa);
    }

    return seen;
}
