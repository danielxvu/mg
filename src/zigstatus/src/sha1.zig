const std = @import("std");
// SHA-1 over git's blob object: "blob " ++ decimal(len) ++ "\0" ++ bytes.
pub fn gitBlob(gpa: std.mem.Allocator, bytes: []const u8) [20]u8 {
    var h = std.crypto.hash.Sha1.init(.{});
    var hdr: [32]u8 = undefined;
    const n = std.fmt.bufPrint(&hdr, "blob {d}\x00", .{bytes.len}) catch unreachable;
    h.update(n);
    h.update(bytes);
    _ = gpa;
    var out: [20]u8 = undefined;
    h.final(&out);
    return out;
}
