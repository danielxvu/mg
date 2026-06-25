const std = @import("std");
const status = @import("status.zig");
const walk = @import("walk.zig");

const CEmit = *const fn (ctx: ?*anyopaque, path: [*]const u8, path_len: usize, x: u8, y: u8) callconv(.c) void;
const Bridge = struct { cemit: CEmit, cctx: ?*anyopaque };

fn zigEmit(ctx: *anyopaque, x: u8, y: u8, path: []const u8) void {
    const b: *Bridge = @ptrCast(@alignCast(ctx));
    b.cemit(b.cctx, path.ptr, path.len, x, y);
}

pub export fn neomg_zig_worktree_status(repo: [*]const u8, repo_len: usize, emit: CEmit, ctx: ?*anyopaque) callconv(.c) c_int {
    var threaded = std.Io.Threaded.init(std.heap.c_allocator, .{});
    defer threaded.deinit();
    const io = threaded.io();
    var bridge = Bridge{ .cemit = emit, .cctx = ctx };
    status.run(io, std.heap.c_allocator, repo[0..repo_len], zigEmit, &bridge) catch return -1;
    return 0;
}
