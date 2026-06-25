// Index-driven parallel walk for zigstatus.
// Adapted from bench/walk-spike/walk_spike.zig (Zig 0.16).
// Only descends directories that contain tracked files (per the git index),
// matching git status -unormal behaviour.  Per-file action: call emit(ctx, x, y, path).
const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const index = @import("index.zig");

pub const EmitFn = *const fn (ctx: *anyopaque, x: u8, y: u8, path: []const u8) void;

const Job = struct {
    io: Io,
    root: Dir,
    names: []const []const u8,
    idx: *const index.Index,
    walk_all: bool, // no usable index -> descend everything
    start: usize,
    stride: usize,
    emit: EmitFn,
    ctx: *anyopaque,
};

const Ctx = struct {
    io: Io,
    idx: *const index.Index,
    walk_all: bool,
    path: [4096]u8 = undefined,
    plen: usize = 0,
    emit: EmitFn,
    emit_ctx: *anyopaque,
};

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
            }
            // Tracked and stat-unchanged -> emit nothing (Task 1 scope).
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
        var d = job.root.openDir(job.io, name, .{ .iterate = true }) catch continue;
        defer d.close(job.io);
        var ctx: Ctx = .{
            .io = job.io,
            .idx = job.idx,
            .walk_all = job.walk_all,
            .emit = job.emit,
            .emit_ctx = job.ctx,
        };
        @memcpy(ctx.path[0..name.len], name);
        ctx.plen = name.len;
        walkDir(&ctx, d);
    }
}

// Also walk root-level files (not in any subdirectory).
fn walkRootFiles(io: Io, root: Dir, idx: *const index.Index, emit: EmitFn, ctx: *anyopaque) !void {
    var it = root.iterate();
    while (try it.next(io)) |ent| {
        if (std.mem.eql(u8, ent.name, ".git")) continue;
        if (ent.kind != .file and ent.kind != .sym_link) continue;
        // Root-level file: path == name.
        if (!idx.files.contains(ent.name)) {
            emit(ctx, '?', '?', ent.name);
        }
    }
}

pub fn run(
    io: Io,
    gpa: std.mem.Allocator,
    root: Dir,
    idx: *const index.Index,
    emit: EmitFn,
    ctx: *anyopaque,
) !void {
    const walk_all = idx.files.count() == 0;

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
    try walkRootFiles(io, root, idx, emit, ctx);

    if (names.items.len == 0) return;

    const ncpu = std.Thread.getCpuCount() catch 4;
    const nthreads = @min(ncpu, @max(names.items.len, 1));

    const jobs = try gpa.alloc(Job, nthreads);
    defer gpa.free(jobs);
    for (0..nthreads) |w|
        jobs[w] = .{
            .io = io,
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
}
