const std = @import("std");
const lib = @import("lib.zig");

const Sink = struct {
    out: std.ArrayList(u8) = .empty,
    gpa: std.mem.Allocator,
    // std.atomic.Mutex: simple spinlock, no io needed.
    mutex: std.atomic.Mutex = .unlocked,
};

fn cEmit(ctx: ?*anyopaque, path: [*]const u8, path_len: usize, x: u8, y: u8) callconv(.c) void {
    const s: *Sink = @ptrCast(@alignCast(ctx));
    while (!s.mutex.tryLock()) {}
    defer s.mutex.unlock();
    s.out.append(s.gpa, x) catch return;
    s.out.append(s.gpa, y) catch return;
    s.out.append(s.gpa, ' ') catch return;
    s.out.appendSlice(s.gpa, path[0..path_len]) catch return;
    s.out.append(s.gpa, '\n') catch return;
}

pub fn main(init: std.process.Init) !void {
    const argv = init.minimal.args.vector;
    if (argv.len < 2) return;
    var sink = Sink{ .gpa = init.gpa };
    defer sink.out.deinit(init.gpa);
    const repo = std.mem.span(argv[1]);
    const rc = lib.neomg_zig_worktree_status(repo.ptr, repo.len, cEmit, &sink);
    if (rc != 0) return error.ZigStatusFailed;
    try std.Io.File.stdout().writeStreamingAll(init.io, sink.out.items);
}
