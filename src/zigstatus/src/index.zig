const std = @import("std");
pub const Entry = struct { mtime_sec: i64, mtime_nsec: i64, size: i64, sha: [20]u8, mode: u32 };
pub const Index = struct {
    files: std.StringHashMap(Entry),
    dirs: std.StringHashMap(void),
    pub fn deinit(self: *Index) void { self.files.deinit(); self.dirs.deinit(); }
};
// Signalled when the index is in a state this v2-only parser cannot represent
// faithfully (a non-v2 header, or any conflicted/stage>0 entry). The C-ABI
// boundary maps this to the -1 sentinel so C++ falls back to libgit2 -- the
// "fail closed" contract: better to be slow-but-correct than fast-but-wrong.
//
// NOTE: a *valid empty v2 index* is NOT unsupported -- it returns an empty Index
// so the caller's walk_all path runs (correct for a genuinely empty repo).
pub const ParseError = error{Unsupported};

// Keys slice into `data` (caller keeps it alive). v2 only.
// Returns error.Unsupported for any index this parser can't faithfully model.
pub fn parse(gpa: std.mem.Allocator, data: []const u8) ParseError!Index {
    var idx = Index{ .files = std.StringHashMap(Entry).init(gpa), .dirs = std.StringHashMap(void).init(gpa) };
    errdefer idx.deinit();
    // An absent/empty index file (no .git/index yet) is a valid empty repo, not
    // an error -- let walk_all run. A present-but-malformed header is unsupported.
    if (data.len == 0) return idx;
    if (data.len < 12 or !std.mem.eql(u8, data[0..4], "DIRC")) return error.Unsupported;
    // v3 (extended flags) and v4 (path-prefix compression) lay out entries
    // differently; split-index (a v2/v3 header + a `link` extension referencing a
    // shared base) is likewise not modelled. Fail closed on anything but plain v2.
    if (std.mem.readInt(u32, data[4..8], .big) != 2) return error.Unsupported;
    const count = std.mem.readInt(u32, data[8..12], .big);
    var off: usize = 12;
    var i: u32 = 0;
    while (i < count and off + 62 <= data.len) : (i += 1) {
        const mtime_sec = std.mem.readInt(u32, data[off + 8 ..][0..4], .big);
        const mtime_nsec = std.mem.readInt(u32, data[off + 12 ..][0..4], .big);
        const mode = std.mem.readInt(u32, data[off + 24 ..][0..4], .big);
        const size = std.mem.readInt(u32, data[off + 36 ..][0..4], .big);
        // Bits 12-13 of the 16-bit flags hold the merge stage. Any nonzero stage
        // means a conflicted (unmerged) entry; this walker reports plain ' M',
        // which would double-list the file (worktree + Conflicts). Fail closed.
        const flags = std.mem.readInt(u16, data[off + 60 ..][0..2], .big);
        if ((flags >> 12) & 0x3 != 0) return error.Unsupported;
        // A gitlink entry (mode 0160000) is a submodule: its directory holds a
        // nested repo, not worktree files. The walker would descend and mis-scan
        // it; libgit2 excludes submodules. Fail closed so C++ uses libgit2.
        if (mode & 0o170000 == 0o160000) return error.Unsupported;
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
    // After the entries come optional extensions, then a 20-byte trailer hash.
    // A "link" extension means this is a SPLIT index: the bulk of the entries
    // live in a separate sharedindex.<hash> file we don't read, so the entries
    // above are incomplete. Even a v2 header can carry it (git update-index
    // --split-index writes v2 + link), so the version check alone misses it.
    // Walk the extension chain (4-byte signature + 4-byte big-endian length)
    // and fail closed if we see "link". (version != 2 already covers the common
    // v4-based manyFiles/untracked-cache split setups; this catches v2 ones.)
    var ext = off;
    while (ext + 8 <= data.len -| 20) {
        const sig = data[ext..][0..4];
        if (std.mem.eql(u8, sig, "link")) return error.Unsupported;
        const ext_len = std.mem.readInt(u32, data[ext + 4 ..][0..4], .big);
        ext += 8 + ext_len;
    }
    return idx;
}
