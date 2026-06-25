// THROWAWAY SPIKE v3 (Zig 0.16) -- not part of neomg. The arc:
//   v1 naive walk            -> 15x SLOWER on d20app (descended node_modules)
//   v2 + gitignore pruning   -> still 9x slower on d20app (descended 165k
//                               UNtracked-but-unignored files git never visits)
//   v3 INDEX-DRIVEN          -> only descend directories that contain tracked
//                               files, exactly like `git status -unormal`. This
//                               subsumes gitignore pruning for the *walk*: any
//                               dir with no tracked content (ignored OR merely
//                               untracked) isn't descended. (gitignore is still
//                               needed to *label* untracked-vs-ignored, but not
//                               to decide whether to walk -- and walking is the
//                               cost.) Question: does it now beat libgit2 cold
//                               (~146 ms roll20, ~52 ms d20app) EVERYWHERE?
//
//   zig build-exe -OReleaseFast -lc walk_spike.zig && ./walk_spike <repo> [--runs N]
const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;

const Local = struct {
    files: u64 = 0, // files stat'd
    dirs: u64 = 0,
    untracked_dirs: u64 = 0,
    modified: u64 = 0, // tracked + stat differs from index
    untracked_files: u64 = 0,
};

const IdxEntry = struct { mtime_sec: i64, size: i64 };

// Parse a git index v2: fill `dirs` (repo-relative dirs containing tracked
// content -- every ancestor of every tracked path; drives the walk) and `files`
// (tracked path -> mtime+size, for the modified/unmodified compare). Keys slice
// into `data`, which the caller keeps alive.
fn parseIndex(dirs: *std.StringHashMap(void), files: *std.StringHashMap(IdxEntry), data: []const u8) void {
    if (data.len < 12 or !std.mem.eql(u8, data[0..4], "DIRC")) return;
    if (std.mem.readInt(u32, data[4..8], .big) != 2) return;
    const count = std.mem.readInt(u32, data[8..12], .big);
    var off: usize = 12;
    var i: u32 = 0;
    while (i < count and off + 62 <= data.len) : (i += 1) {
        const mtime_sec = std.mem.readInt(u32, data[off + 8 ..][0..4], .big);
        const size = std.mem.readInt(u32, data[off + 36 ..][0..4], .big);
        var p = off + 62;
        while (p < data.len and data[p] != 0) p += 1;
        const path = data[off + 62 .. p];
        files.put(path, .{ .mtime_sec = mtime_sec, .size = size }) catch {};
        for (path, 0..) |c, idx| {
            if (c == '/') dirs.put(path[0..idx], {}) catch {};
        }
        const namelen = p - (off + 62);
        off += (70 + namelen) & ~@as(usize, 7);
    }
}

const Job = struct {
    io: Io,
    root: Dir,
    names: []const []const u8,
    tracked: *const std.StringHashMap(void),
    files: *const std.StringHashMap(IdxEntry),
    walk_all: bool, // no usable index -> descend everything
    start: usize,
    stride: usize,
    result: Local = .{},
};

const Ctx = struct {
    io: Io,
    tracked: *const std.StringHashMap(void),
    files: *const std.StringHashMap(IdxEntry),
    walk_all: bool,
    path: [4096]u8 = undefined,
    plen: usize = 0,
    local: *Local,
};

// Descend `dir`. `ctx.path[0..plen]` is its repo-relative path.
fn walkDir(ctx: *Ctx, dir: Dir) void {
    var it = dir.iterate();
    while (it.next(ctx.io) catch null) |ent| {
        if (std.mem.eql(u8, ent.name, ".git")) continue;
        const is_dir = ent.kind == .directory;
        if (!is_dir and ent.kind != .file and ent.kind != .sym_link) continue;

        const save = ctx.plen;
        ctx.path[ctx.plen] = '/';
        @memcpy(ctx.path[ctx.plen + 1 ..][0..ent.name.len], ent.name);
        ctx.plen += 1 + ent.name.len;
        const cur = ctx.path[0..ctx.plen];

        if (is_dir) {
            // Index-driven: only descend dirs that contain tracked files.
            if (ctx.walk_all or ctx.tracked.contains(cur)) {
                var sub = dir.openDir(ctx.io, ent.name, .{ .iterate = true }) catch {
                    ctx.plen = save;
                    continue;
                };
                defer sub.close(ctx.io);
                ctx.local.dirs += 1;
                walkDir(ctx, sub);
            } else {
                ctx.local.untracked_dirs += 1; // reported as one untracked dir; not descended
            }
        } else {
            ctx.local.files += 1;
            const st = dir.statFile(ctx.io, ent.name, .{ .follow_symlinks = false }) catch {
                ctx.plen = save;
                continue;
            };
            // Classify like a real status: tracked (compare to index) vs untracked.
            if (ctx.files.get(cur)) |ie| {
                const mt_sec = @divTrunc(st.mtime.nanoseconds, 1_000_000_000);
                if (ie.size != @as(i64, @intCast(st.size)) or ie.mtime_sec != mt_sec)
                    ctx.local.modified += 1;
            } else ctx.local.untracked_files += 1;
        }
        ctx.plen = save;
    }
}

fn worker(job: *Job) void {
    var i = job.start;
    while (i < job.names.len) : (i += job.stride) {
        const name = job.names[i];
        if (!job.walk_all and !job.tracked.contains(name)) {
            job.result.untracked_dirs += 1;
            continue;
        }
        var d = job.root.openDir(job.io, name, .{ .iterate = true }) catch continue;
        defer d.close(job.io);
        job.result.dirs += 1;
        var ctx: Ctx = .{ .io = job.io, .tracked = job.tracked, .files = job.files, .walk_all = job.walk_all, .local = &job.result };
        @memcpy(ctx.path[0..name.len], name);
        ctx.plen = name.len;
        walkDir(&ctx, d);
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const gpa = init.gpa;
    const argv = init.minimal.args.vector;

    if (argv.len < 2) {
        std.debug.print("usage: walk_spike <repo> [--runs N]\n", .{});
        return;
    }
    const repo = std.mem.span(argv[1]);
    var runs: usize = 9;
    var k: usize = 2;
    while (k < argv.len) : (k += 1) {
        const a = std.mem.span(argv[k]);
        if (std.mem.eql(u8, a, "--runs") and k + 1 < argv.len) {
            runs = std.fmt.parseInt(usize, std.mem.span(argv[k + 1]), 10) catch 9;
            k += 1;
        }
    }

    var root = try Dir.openDirAbsolute(io, repo, .{ .iterate = true });
    defer root.close(io);

    // Build the tracked-dir set from .git/index (kept alive for all runs).
    const index_bytes = root.readFileAlloc(io, ".git/index", gpa, .unlimited) catch &[_]u8{};
    defer gpa.free(index_bytes);
    var tracked = std.StringHashMap(void).init(gpa);
    defer tracked.deinit();
    var files = std.StringHashMap(IdxEntry).init(gpa);
    defer files.deinit();
    parseIndex(&tracked, &files, index_bytes);
    const walk_all = tracked.count() == 0;

    var names: std.ArrayList([]const u8) = .empty;
    defer names.deinit(gpa);
    {
        var it = root.iterate();
        while (try it.next(io)) |ent| {
            if (ent.kind == .directory and !std.mem.eql(u8, ent.name, ".git"))
                try names.append(gpa, try gpa.dupe(u8, ent.name));
        }
    }

    const ncpu = std.Thread.getCpuCount() catch 4;
    const nthreads = @min(ncpu, @max(names.items.len, 1));

    const times = try gpa.alloc(f64, runs);
    defer gpa.free(times);
    var last: Local = .{};
    var run: usize = 0;
    var r: usize = 0;
    while (r < runs + 2) : (r += 1) {
        const jobs = try gpa.alloc(Job, nthreads);
        defer gpa.free(jobs);
        for (0..nthreads) |w|
            jobs[w] = .{ .io = io, .root = root, .names = names.items, .tracked = &tracked, .files = &files, .walk_all = walk_all, .start = w, .stride = nthreads };

        const t0 = Io.Timestamp.now(io, .awake);
        var group: Io.Group = .init;
        for (0..nthreads) |w| group.async(io, worker, .{&jobs[w]});
        group.await(io) catch {};
        const ns = t0.durationTo(Io.Timestamp.now(io, .awake)).nanoseconds;
        const ms = @as(f64, @floatFromInt(ns)) / 1_000_000.0;

        var tot: Local = .{};
        for (0..nthreads) |w| {
            tot.files += jobs[w].result.files;
            tot.dirs += jobs[w].result.dirs;
            tot.untracked_dirs += jobs[w].result.untracked_dirs;
            tot.modified += jobs[w].result.modified;
            tot.untracked_files += jobs[w].result.untracked_files;
        }
        last = tot;
        if (r >= 2) {
            times[run] = ms;
            run += 1;
        }
    }

    std.mem.sort(f64, times, {}, std.sort.asc(f64));
    std.debug.print("repo: {s}  (index-driven: {s})\n", .{ repo, if (walk_all) "no index, walking all" else "tracked-dir pruned" });
    std.debug.print("threads: {d} over {d} top-level dirs, runs: {d}\n", .{ nthreads, names.items.len, runs });
    std.debug.print("files stat'd: {d}  modified: {d}  untracked files: {d}  dirs: {d}  untracked dirs pruned: {d}\n", .{ last.files, last.modified, last.untracked_files, last.dirs, last.untracked_dirs });
    std.debug.print("index-driven parallel walk + lstat + classify:  min {d:.2}  median {d:.2} ms\n", .{ times[0], times[times.len / 2] });
}
