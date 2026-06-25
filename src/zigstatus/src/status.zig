const std = @import("std");
const index = @import("index.zig");
const walk = @import("walk.zig");
const gitignore = @import("gitignore.zig");
pub const Record = struct { x: u8, y: u8, path: []const u8 };
pub fn run(io: std.Io, gpa: std.mem.Allocator, repo: []const u8, emit: walk.EmitFn, ctx: *anyopaque) !void {
    var root = try std.Io.Dir.openDirAbsolute(io, repo, .{ .iterate = true });
    defer root.close(io);
    // A missing .git/index (FileNotFound) is a genuine empty repo (freshly
    // `git init`ed, nothing staged): use empty bytes so parse yields an empty
    // Index and walk_all runs -- correct. But any OTHER read error (permissions,
    // I/O failure, a truncated read) must NOT be silently treated as "empty" --
    // that would report the whole worktree as untracked with no fallback (fail
    // OPEN). Map such errors to error.Unsupported so the C-ABI returns -1 and
    // C++ falls back to libgit2.
    const bytes = root.readFileAlloc(io, ".git/index", gpa, .unlimited) catch |err| switch (err) {
        error.FileNotFound => &[_]u8{},
        else => return error.Unsupported,
    };
    defer gpa.free(bytes);
    // Fail closed: parse returns error.Unsupported for conflicted or non-v2
    // indexes; propagate it so lib.zig's C-ABI export returns -1 and C++ falls
    // back to libgit2. A valid empty index is NOT an error (genuine empty repo).
    var idx = try index.parse(gpa, bytes);
    defer idx.deinit();

    // Parse root-level gitignore sources into a seed rules list.
    // Pattern slices point into the file bytes; keep both alive for the entire run.
    var seed_rules: std.ArrayListUnmanaged(gitignore.Rule) = .empty;
    defer seed_rules.deinit(gpa);

    const root_gitignore = root.readFileAlloc(io, ".gitignore", gpa, .unlimited) catch null;
    defer if (root_gitignore) |b| gpa.free(b);
    if (root_gitignore) |b| gitignore.parseInto(&seed_rules, gpa, b, "");

    const exclude_bytes = root.readFileAlloc(io, ".git/info/exclude", gpa, .unlimited) catch null;
    defer if (exclude_bytes) |b| gpa.free(b);
    if (exclude_bytes) |b| gitignore.parseInto(&seed_rules, gpa, b, "");

    var seen = try walk.run(io, gpa, root, &idx, seed_rules.items, emit, ctx);
    defer seen.deinit(gpa);
    // Post-walk deleted sweep: any tracked file not seen on disk was deleted.
    // This is single-threaded (after the parallel walk joined), no mutex needed.
    var kit = idx.files.keyIterator();
    while (kit.next()) |k| {
        if (!seen.contains(k.*)) emit(ctx, ' ', 'D', k.*);
    }
}
