const std = @import("std");
const index = @import("index.zig");
const walk = @import("walk.zig");
const gitignore = @import("gitignore.zig");
pub const Record = struct { x: u8, y: u8, path: []const u8 };
pub fn run(io: std.Io, gpa: std.mem.Allocator, repo: []const u8, emit: walk.EmitFn, ctx: *anyopaque) !void {
    var root = try std.Io.Dir.openDirAbsolute(io, repo, .{ .iterate = true });
    defer root.close(io);
    const bytes = root.readFileAlloc(io, ".git/index", gpa, .unlimited) catch &[_]u8{};
    defer gpa.free(bytes);
    var idx = index.parse(gpa, bytes);
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
