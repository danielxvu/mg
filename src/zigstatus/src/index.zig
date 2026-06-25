const std = @import("std");
pub const Entry = struct { mtime_sec: i64, mtime_nsec: i64, size: i64, sha: [20]u8, mode: u32 };
pub const Index = struct {
    files: std.StringHashMap(Entry),
    dirs: std.StringHashMap(void),
    pub fn deinit(self: *Index) void { self.files.deinit(); self.dirs.deinit(); }
};
// Keys slice into `data` (caller keeps it alive). v2 only; returns empty on other versions.
pub fn parse(gpa: std.mem.Allocator, data: []const u8) Index {
    var idx = Index{ .files = std.StringHashMap(Entry).init(gpa), .dirs = std.StringHashMap(void).init(gpa) };
    if (data.len < 12 or !std.mem.eql(u8, data[0..4], "DIRC")) return idx;
    if (std.mem.readInt(u32, data[4..8], .big) != 2) return idx;
    const count = std.mem.readInt(u32, data[8..12], .big);
    var off: usize = 12;
    var i: u32 = 0;
    while (i < count and off + 62 <= data.len) : (i += 1) {
        const mtime_sec = std.mem.readInt(u32, data[off + 8 ..][0..4], .big);
        const mtime_nsec = std.mem.readInt(u32, data[off + 12 ..][0..4], .big);
        const mode = std.mem.readInt(u32, data[off + 24 ..][0..4], .big);
        const size = std.mem.readInt(u32, data[off + 36 ..][0..4], .big);
        var sha: [20]u8 = undefined;
        @memcpy(&sha, data[off + 40 ..][0..20]);
        var p = off + 62;
        while (p < data.len and data[p] != 0) p += 1;
        const path = data[off + 62 .. p];
        idx.files.put(path, .{ .mtime_sec = mtime_sec, .mtime_nsec = mtime_nsec, .size = size, .sha = sha, .mode = mode }) catch {};
        for (path, 0..) |c, j| if (c == '/') { idx.dirs.put(path[0..j], {}) catch {}; };
        const namelen = p - (off + 62);
        off += (70 + namelen) & ~@as(usize, 7);
    }
    return idx;
}
