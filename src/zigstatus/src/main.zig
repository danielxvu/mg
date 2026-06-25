const std = @import("std");
const status = @import("status.zig");
const walk = @import("walk.zig");

const Sink = struct {
    out: std.ArrayList(u8) = .empty,
    gpa: std.mem.Allocator,
    // std.atomic.Mutex: simple spinlock, no io needed.
    mutex: std.atomic.Mutex = .unlocked,
};
fn emit(ctx: *anyopaque, x: u8, y: u8, path: []const u8) void {
    const s: *Sink = @ptrCast(@alignCast(ctx));
    // Spinlock: busy-wait until acquired (fine for low contention).
    while (!s.mutex.tryLock()) {}
    defer s.mutex.unlock();
    s.out.append(s.gpa, x) catch return;
    s.out.append(s.gpa, y) catch return;
    s.out.append(s.gpa, ' ') catch return;
    s.out.appendSlice(s.gpa, path) catch return;
    s.out.append(s.gpa, '\n') catch return;
}

pub fn main(init: std.process.Init) !void {
    const argv = init.minimal.args.vector;
    if (argv.len < 2) return;
    var sink = Sink{ .gpa = init.gpa };
    defer sink.out.deinit(init.gpa);
    try status.run(init.io, init.gpa, std.mem.span(argv[1]), emit, &sink);
    // 0.16: stdout write uses Io-based API
    try std.Io.File.stdout().writeStreamingAll(init.io, sink.out.items);
}
