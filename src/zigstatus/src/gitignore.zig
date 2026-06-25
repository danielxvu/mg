const std = @import("std");
pub const Rule = struct {
    negate: bool,
    dir_only: bool,
    anchored: bool, // matched vs path relative to `base`; else basename at any depth
    base: []const u8, // repo-relative dir of the owning .gitignore ("" = root)
    pattern: []const u8, // cleaned: no '!', no leading '/', no trailing '/'
};
fn globSeg(pat: []const u8, s: []const u8) bool {
    var pi: usize = 0; var si: usize = 0; var star: ?usize = null; var ss: usize = 0;
    while (si < s.len) {
        if (pi < pat.len and (pat[pi] == '?' or pat[pi] == s[si])) { pi += 1; si += 1; }
        else if (pi < pat.len and pat[pi] == '*') { star = pi; ss = si; pi += 1; }
        else if (star) |sp| { pi = sp + 1; ss += 1; si = ss; }
        else return false;
    }
    while (pi < pat.len and pat[pi] == '*') pi += 1;
    return pi == pat.len;
}
fn globPath(pat: []const u8, path: []const u8) bool {
    var pit = std.mem.splitScalar(u8, pat, '/');
    var sit = std.mem.splitScalar(u8, path, '/');
    while (true) {
        const ps = pit.next(); const sp = sit.next();
        if (ps == null and sp == null) return true;
        if (ps == null or sp == null) return false;
        if (!globSeg(ps.?, sp.?)) return false;
    }
}
// `path` is the entry's repo-relative path; `name` its basename. Last match wins.
pub fn ignored(rules: []const Rule, path: []const u8, name: []const u8, is_dir: bool) bool {
    var ig = false;
    for (rules) |r| {
        if (r.dir_only and !is_dir) continue;
        if (r.base.len != 0) {
            if (path.len <= r.base.len or !std.mem.startsWith(u8, path, r.base) or path[r.base.len] != '/') continue;
        }
        const rel = if (r.base.len == 0) path else path[r.base.len + 1 ..];
        const hit = if (r.anchored) globPath(r.pattern, rel) else globSeg(r.pattern, name);
        if (hit) ig = !r.negate;
    }
    return ig;
}
// Parse a .gitignore buffer; pattern slices point into `bytes` (keep it alive).
pub fn parseInto(list: *std.ArrayListUnmanaged(Rule), gpa: std.mem.Allocator, bytes: []const u8, base: []const u8) void {
    var it = std.mem.splitScalar(u8, bytes, '\n');
    while (it.next()) |raw| {
        var line = std.mem.trimEnd(u8, raw, "\r ");
        if (line.len == 0 or line[0] == '#') continue;
        var negate = false;
        if (line[0] == '!') { negate = true; line = line[1..]; }
        if (line.len == 0) continue;
        var dir_only = false;
        if (line[line.len - 1] == '/') { dir_only = true; line = line[0 .. line.len - 1]; }
        if (line.len == 0) continue;
        var anchored = std.mem.indexOfScalar(u8, line, '/') != null;
        if (line[0] == '/') { anchored = true; line = line[1..]; }
        list.append(gpa, .{ .negate = negate, .dir_only = dir_only, .anchored = anchored, .base = base, .pattern = line }) catch {};
    }
}
