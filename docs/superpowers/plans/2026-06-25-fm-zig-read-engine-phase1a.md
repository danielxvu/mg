# FM-ZIG-READ-ENGINE Phase 1a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone Zig 0.16 `zigstatus` that reports the **worktree** status dimension (modified / deleted / untracked, with gitignore exclusion) for a repo, byte-for-byte matching `git status --porcelain` on repos whose changes are worktree-only (nothing staged, no conflicts).

**Architecture:** Reuse the spike's proven index-driven parallel walk. Parse `.git/index` (v2) → tracked files (path→mtime/size/sha) + tracked-dir set. Walk only tracked-containing dirs (`git -unormal`); for each entry, classify against the index (modified via stat-compare + racy-clean SHA-1 fallback; deleted; untracked) and apply a gitignore matcher to exclude ignored entries. Emit `XY<space>path` porcelain lines. A Python harness builds repos in known states, runs git + zigstatus, normalizes, and diffs — that diff IS the test.

**Tech Stack:** Zig 0.16 (MacPorts `/opt/local/bin/zig`; pinned in `bench/walk-spike/.tool-versions`), `std.Io` (Threaded → io, `Io.Group` fan-out), Python 3 (validation harness), git (the oracle).

## Global Constraints

- **Zig 0.16.0** only (0.14/0.15 cannot link the macOS 26.5 SDK). Build: `/opt/local/bin/zig build -Doptimize=ReleaseFast` from `src/zigstatus/`. The std API is the 0.16 shape — read `/opt/local/lib/zig/std/` for signatures; do NOT assume 0.14/0.15 APIs (`ArrayList` is unmanaged `.empty`/`append(gpa,x)`; fs is under `std.Io.Dir` and ops take `io`; entry point is `main(init: std.process.Init)`; timing is `Io.Timestamp.now(io, .awake)`).
- **Oracle:** `git -C <repo> status --porcelain` (v1, default `-unormal`). The comparison is the **normalized set of `XY path` lines** (sorted; the 2-char code + path). Not `--ignored` (ignored entries must be ABSENT from output).
- **Scope (1a):** worktree dimension only — `_M` (modified), `_D` (deleted), `??` (untracked), and ignored-excluded. Where the index column (X) would be non-blank (staged changes) or there are conflicts, the TEST REPOS in 1a never create those states, so X is always blank here. Index-vs-HEAD and conflicts are Phase 1b.
- This is standalone Zig — **no C++/FFI integration** (that is Phase 2). The OFF/default neomg build is untouched.
- `bench/walk-spike/` (the throwaway spike) is the reference implementation to copy the walk from — `src/zigstatus/` is the real, kept code.
- Commit trailer on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp
  ```

---

## File Structure

- `src/zigstatus/build.zig` — Zig build: a `zigstatus` executable (ReleaseFast).
- `src/zigstatus/src/index.zig` — git index v2 parser → `Index` (StringHashMap path→Entry{mtime_sec,mtime_nsec,size,sha[20],mode}; plus the tracked-dir set).
- `src/zigstatus/src/gitignore.zig` — ignore matcher (rules + `isIgnored`), ported from the spike's v2 matcher.
- `src/zigstatus/src/walk.zig` — index-driven parallel walk; per-entry worktree classify; emits records via a callback.
- `src/zigstatus/src/sha1.zig` — git blob SHA-1 (for racy-clean disambiguation).
- `src/zigstatus/src/status.zig` — orchestrator: open repo, parse index, walk, collect `{path, x:u8, y:u8}` records.
- `src/zigstatus/src/main.zig` — CLI: `zigstatus <repo>` → prints `XY path` lines (porcelain order/format).
- `src/zigstatus/test/validate.py` — the byte-for-byte harness: builds repos in states, runs git + zigstatus, normalizes, diffs.

---

## Task 1: Project skeleton + validation harness + untracked files

**Files:**
- Create: `src/zigstatus/build.zig`, `src/zigstatus/src/index.zig`, `src/zigstatus/src/walk.zig`, `src/zigstatus/src/status.zig`, `src/zigstatus/src/main.zig`, `src/zigstatus/test/validate.py`

**Interfaces:**
- Produces: `zigstatus <repo>` prints, one per line, `XY<space>path` (X,Y each one of ` MAD?`; untracked = `??`), matching `git status --porcelain`. Internally: `index.parse(gpa, bytes) -> Index` with `Index.files: StringHashMap(Entry)` and `Index.dirs: StringHashMap(void)`; `status.run(io, gpa, repo, emit, ctx)` walks and calls `emit(ctx, x, y, path)`.

- [ ] **Step 1: Write the failing test (harness + untracked case)**

Create `src/zigstatus/test/validate.py`:
```python
#!/usr/bin/env python3
# Byte-for-byte validation: build a repo in a known state, compare `zigstatus`
# to `git status --porcelain` (normalized: sorted "XY path" lines).
import subprocess, sys, tempfile, os, shutil

ZIG = "/opt/local/bin/zig"
BIN = os.path.join(os.path.dirname(__file__), "..", "zig-out", "bin", "zigstatus")

def sh(cwd, *args): subprocess.run(args, cwd=cwd, check=True, capture_output=True)

def git_status(repo):
    out = subprocess.run(["git", "-C", repo, "status", "--porcelain"],
                         capture_output=True, text=True).stdout
    return sorted(l for l in out.splitlines() if l)

def zig_status(repo):
    out = subprocess.run([os.path.abspath(BIN), repo], capture_output=True, text=True).stdout
    return sorted(l for l in out.splitlines() if l)

def make_repo(setup):
    d = tempfile.mkdtemp(prefix="zigstatus-")
    sh(d, "git", "init", "-q"); sh(d, "git", "config", "user.email", "t@e"); sh(d, "git", "config", "user.name", "t")
    setup(d)
    return d

CASES = {}
def case(fn): CASES[fn.__name__] = fn; return fn

@case
def untracked(d):
    open(os.path.join(d, "tracked.txt"), "w").write("x\n")
    sh(d, "git", "add", "tracked.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "new1.txt"), "w").write("a\n")
    open(os.path.join(d, "new2.md"), "w").write("b\n")

def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    fails = 0
    for name, setup in CASES.items():
        if only and name != only: continue
        d = make_repo(setup)
        try:
            g, z = git_status(d), zig_status(d)
            if g == z: print(f"ok   {name}")
            else:
                fails += 1; print(f"FAIL {name}\n  git: {g}\n  zig: {z}")
        finally: shutil.rmtree(d, ignore_errors=True)
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
```

- [ ] **Step 2: Create the Zig skeleton so the harness can build/run it**

`src/zigstatus/build.zig`:
```zig
const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const exe = b.addExecutable(.{
        .name = "zigstatus",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true, // std.Threaded needs libc on macOS
        }),
    });
    b.installArtifact(exe);
}
```

`src/zigstatus/src/index.zig` (v2 parser; the spike's logic, kept):
```zig
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
```

`src/zigstatus/src/walk.zig` — copy `bench/walk-spike/walk_spike.zig`'s parallel index-driven walk (the `Job`/`Ctx`/`walkDir`/`worker` machinery and `Io.Group` fan-out), but instead of counting, call an `emit(ctx, x: u8, y: u8, path: []const u8)` callback. For Task 1, the only classification is: a file present on disk and **not** in `index.files` → untracked → `emit(ctx, ' ', '?', path)` rendered later as `??`. (Modified/deleted come in Tasks 2–3.) Tracked files present and stat-unchanged → emit nothing.

`src/zigstatus/src/status.zig`:
```zig
const std = @import("std");
const index = @import("index.zig");
const walk = @import("walk.zig");
pub const Record = struct { x: u8, y: u8, path: []const u8 };
pub fn run(io: std.Io, gpa: std.mem.Allocator, repo: []const u8, emit: walk.EmitFn, ctx: *anyopaque) !void {
    var root = try std.Io.Dir.openDirAbsolute(io, repo, .{ .iterate = true });
    defer root.close(io);
    const bytes = root.readFileAlloc(io, ".git/index", gpa, .unlimited) catch &[_]u8{};
    defer gpa.free(bytes);
    var idx = index.parse(gpa, bytes);
    defer idx.deinit();
    try walk.run(io, gpa, root, &idx, emit, ctx); // parallel index-driven walk
}
```

`src/zigstatus/src/main.zig`:
```zig
const std = @import("std");
const status = @import("status.zig");
const walk = @import("walk.zig");

const Sink = struct {
    out: std.ArrayList(u8) = .empty,
    gpa: std.mem.Allocator,
    mutex: std.Thread.Mutex = .{},
};
fn emit(ctx: *anyopaque, x: u8, y: u8, path: []const u8) void {
    const s: *Sink = @ptrCast(@alignCast(ctx));
    s.mutex.lock();
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
    try std.fs.File.stdout().writeAll(sink.out.items); // 0.16: File.stdout()
}
```
(Define `pub const EmitFn = *const fn (ctx: *anyopaque, x: u8, y: u8, path: []const u8) void;` in `walk.zig`. The walk emits paths repo-relative with `/` separators, matching git. Emit is called from worker threads → the Sink mutex serializes. Confirm `std.fs.File.stdout()` / the 0.16 stdout-write idiom against `/opt/local/lib/zig/std/` and adjust if needed.)

- [ ] **Step 3: Build + run the harness — verify the untracked case is handled (or see the diff)**

Run:
```bash
cd src/zigstatus && /opt/local/bin/zig build -Doptimize=ReleaseFast
python3 test/validate.py untracked
```
Expected first run may FAIL if path formatting differs (e.g. git quotes/sorts differently); iterate `walk`/`main` until: `ok   untracked`.

- [ ] **Step 4: Commit**
```bash
git add src/zigstatus
git commit -m "feat(zigstatus): skeleton + validation harness + untracked files (FM-ZIG P1a)

<trailer>"
```

---

## Task 2: Worktree-modified (` M`) + racy-clean SHA-1

**Files:**
- Create: `src/zigstatus/src/sha1.zig`
- Modify: `src/zigstatus/src/walk.zig` (file classify), `src/zigstatus/test/validate.py` (add cases)

**Interfaces:**
- Consumes: `index.Entry` (mtime_sec/nsec, size, sha). Produces: `sha1.gitBlob(gpa, bytes) -> [20]u8` (SHA-1 of `"blob <len>\0" ++ bytes`).

- [ ] **Step 1: Add failing test cases**

Append to `validate.py` CASES:
```python
@case
def modified(d):
    open(os.path.join(d, "a.txt"), "w").write("one\n")
    sh(d, "git", "add", "a.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("one\ntwo\n")  # worktree change

@case
def modified_same_size(d):  # racy: same byte length, different content
    open(os.path.join(d, "a.txt"), "w").write("aaaa\n")
    sh(d, "git", "add", "a.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("bbbb\n")
```

- [ ] **Step 2: Run — verify FAIL**

Run: `cd src/zigstatus && zig build -Doptimize=ReleaseFast && python3 test/validate.py modified`
Expected: FAIL (git shows ` M a.txt`; zig shows nothing — modified not yet classified).

- [ ] **Step 3: Implement git blob SHA-1 + modified classification**

`src/zigstatus/src/sha1.zig`:
```zig
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
```
In `walk.zig`, the file branch (entry in `index.files` as `ie`):
```zig
const st = dir.statFile(io, name, .{ .follow_symlinks = false }) catch return;
const mt_sec = @divTrunc(st.mtime.nanoseconds, 1_000_000_000);
const mt_nsec = @mod(st.mtime.nanoseconds, 1_000_000_000);
var changed = ie.size != @as(i64, @intCast(st.size));
if (!changed) {
    // stat matches on size; if mtime also matches the index exactly it's "racy
    // clean" -- git re-hashes to be sure. Else if mtime differs, content changed.
    if (ie.mtime_sec != mt_sec or ie.mtime_nsec != mt_nsec) {
        const content = dir.readFileAlloc(io, name, gpa, .unlimited) catch return;
        defer gpa.free(content);
        changed = !std.mem.eql(u8, &ie.sha, &sha1.gitBlob(gpa, content));
    }
}
if (changed) emit(ctx, ' ', 'M', cur);
```
(Note: comparing the *full* content hash only when size matches but mtime differs is the cheap-correct path; size-mismatch is an immediate modified. This matches git's "stat says maybe-changed → hash to confirm" without hashing every file.)

- [ ] **Step 4: Run — verify PASS**

Run: `python3 test/validate.py modified && python3 test/validate.py modified_same_size`
Expected: `ok   modified` and `ok   modified_same_size`.

- [ ] **Step 5: Commit**
```bash
git add src/zigstatus
git commit -m "feat(zigstatus): worktree-modified + racy-clean SHA-1 (FM-ZIG P1a)\n\n<trailer>"
```

---

## Task 3: Worktree-deleted (` D`)

**Files:** Modify `src/zigstatus/src/status.zig` (post-walk deleted sweep), `src/zigstatus/test/validate.py`.

**Interfaces:** Consumes `Index.files` + the set of tracked paths *seen on disk* during the walk. Produces ` D path` for tracked files not seen.

- [ ] **Step 1: Add failing test case**
```python
@case
def deleted(d):
    open(os.path.join(d, "a.txt"), "w").write("x\n")
    open(os.path.join(d, "b.txt"), "w").write("y\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    os.remove(os.path.join(d, "b.txt"))  # tracked, deleted from worktree
```

- [ ] **Step 2: Run — verify FAIL** (`git`: ` D b.txt`; `zig`: nothing). Run: `python3 test/validate.py deleted`.

- [ ] **Step 3: Implement the deleted sweep**

The parallel walk can't know "absent" locally, so the walk records the set of tracked paths it *did* stat (a concurrent `StringHashMap(void) seen`, or per-worker sets merged after join). After the walk, in `status.run`: iterate `idx.files`; any path not in `seen` and whose parent dir was actually walked (i.e., its tracked dir wasn't pruned) → `emit(ctx, ' ', 'D', path)`. (A tracked file under a pruned dir cannot happen — tracked dirs are never pruned — so "tracked path not seen" ⇒ deleted.) Emit deleted records after the walk completes (single-threaded, no mutex needed).

- [ ] **Step 4: Run — verify PASS.** `python3 test/validate.py deleted` → `ok   deleted`.

- [ ] **Step 5: Commit** `feat(zigstatus): worktree-deleted sweep (FM-ZIG P1a)`.

---

## Task 4: Untracked-directory collapse (`?? dir/`)

**Files:** Modify `src/zigstatus/src/walk.zig`, `src/zigstatus/test/validate.py`.

**Interfaces:** an untracked directory (not in `idx.dirs`) is emitted as a single `?? dir/` record (trailing slash, not descended), matching `git -unormal` — *but only if it contains at least one file* (git omits a wholly-empty untracked dir).

- [ ] **Step 1: Add failing test case**
```python
@case
def untracked_dir(d):
    open(os.path.join(d, "x.txt"), "w").write("x\n")
    sh(d, "git", "add", "x.txt"); sh(d, "git", "commit", "-qm", "init")
    os.makedirs(os.path.join(d, "sub/deep"))
    open(os.path.join(d, "sub/deep/f.txt"), "w").write("f\n")  # untracked tree
```

- [ ] **Step 2: Run — verify FAIL.** git: `?? sub/`; zig (from Task 1): may emit nothing or `?? sub/deep/f.txt` (wrong). Run `python3 test/validate.py untracked_dir`.

- [ ] **Step 3: Implement collapse**

In `walk.zig`, when a directory entry is NOT in `idx.dirs` (untracked dir): do not descend; check it is non-empty (open + one `iterate` step yields any entry); if non-empty, `emit(ctx, ' ', '?', cur ++ "/")`. The walk already prunes these dirs (index-driven); this adds the single `?? dir/` emission. Do not emit untracked *files* inside it (git collapses to the dir).

- [ ] **Step 4: Run — verify PASS.** `python3 test/validate.py untracked_dir` → `ok`.

- [ ] **Step 5: Commit** `feat(zigstatus): untracked-directory collapse (FM-ZIG P1a)`.

---

## Task 5: gitignore exclusion

**Files:** Create `src/zigstatus/src/gitignore.zig` (port the spike's v2 matcher), modify `walk.zig`, `validate.py`.

**Interfaces:** `gitignore.Matcher` loaded from root `.gitignore` + `.git/info/exclude` + per-directory `.gitignore`; `matcher.ignored(path, name, is_dir) -> bool`. Ignored entries are **excluded** from `??` output (NOT printed). An untracked dir that is entirely ignored is omitted.

- [ ] **Step 1: Add failing test cases**
```python
@case
def ignored_file(d):
    open(os.path.join(d, ".gitignore"), "w").write("*.log\nbuild/\n")
    open(os.path.join(d, "keep.txt"), "w").write("k\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "debug.log"), "w").write("noise\n")   # ignored -> absent
    open(os.path.join(d, "real.txt"), "w").write("r\n")        # untracked -> ??
    os.makedirs(os.path.join(d, "build")); open(os.path.join(d, "build/o.o"), "w").write("x")  # ignored dir -> absent
```

- [ ] **Step 2: Run — verify FAIL.** git shows only `?? real.txt`; zig (pre-ignore) also shows `?? debug.log` and `?? build/`. Run `python3 test/validate.py ignored_file`.

- [ ] **Step 3: Implement the matcher + wire it**

Create `src/zigstatus/src/gitignore.zig` (covers the common cases: comments/blanks, `!` negation, trailing-`/` dir-only, leading-`/`-or-embedded-`/` anchored-to-base, `*`/`?` globs within a segment, floating basename patterns; NOT `**` or char classes — those fall to Phase 1b/fallback):
```zig
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
pub fn parseInto(list: *std.ArrayList(Rule), gpa: std.mem.Allocator, bytes: []const u8, base: []const u8) void {
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
```
Wire into `walk.zig`: each worker keeps a `std.ArrayList(Rule)` seeded (read-only) with the root `.gitignore` + `.git/info/exclude` rules (parsed once in `status.run`, base `""`, kept alive for the run). On entering a directory, `parseInto` its `.gitignore` (read via `dir.readFileAlloc(io,".gitignore",arena,.unlimited)`, arena-allocated per worker), recording `rules.items.len` and `shrinkRetainingCapacity` on exit (rewind). Before emitting `??` for an untracked file or untracked dir, skip it when `gitignore.ignored(rules.items, cur, name, is_dir)` is true. Modified/deleted tracked files are NEVER gated by ignore — ignore only suppresses `??` output.

- [ ] **Step 4: Run — verify PASS.** `python3 test/validate.py ignored_file` → `ok`.

- [ ] **Step 5: Commit** `feat(zigstatus): gitignore exclusion of untracked entries (FM-ZIG P1a)`.

---

## Task 6: Corpus validation + speed re-check + docs

**Files:** Modify `src/zigstatus/test/validate.py` (mixed + nested cases), `bench/walk-spike/VERDICT.md` or a new `src/zigstatus/README.md`; update `todo.md`.

- [ ] **Step 1: Add mixed/nested cases**
```python
@case
def mixed(d):  # modified + deleted + untracked + ignored together
    for n in ("a.txt","b.txt","c.txt"): open(os.path.join(d,n),"w").write("x\n")
    open(os.path.join(d, ".gitignore"), "w").write("*.tmp\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("x\nmod\n")     # _M
    os.remove(os.path.join(d, "b.txt"))                        # _D
    open(os.path.join(d, "u.txt"), "w").write("u\n")          # ??
    open(os.path.join(d, "skip.tmp"), "w").write("i\n")       # ignored

@case
def nested_ignore(d):  # a nested .gitignore re-includes via negation
    os.makedirs(os.path.join(d, "pkg"))
    open(os.path.join(d, ".gitignore"), "w").write("*.gen\n")
    open(os.path.join(d, "pkg/.gitignore"), "w").write("!keep.gen\n")
    open(os.path.join(d, "pkg/x.txt"), "w").write("x\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.gen"), "w").write("g\n")          # ignored
    open(os.path.join(d, "pkg/keep.gen"), "w").write("k\n")   # re-included -> ??
```

- [ ] **Step 2: Run the FULL corpus — verify all PASS**

Run: `cd src/zigstatus && zig build -Doptimize=ReleaseFast && python3 test/validate.py`
Expected: every case prints `ok`, exit 0. (If `nested_ignore` fails, the matcher's negation/nesting needs work — fix `gitignore.zig` until green; this case pins the tricky semantics.)

- [ ] **Step 3: Re-confirm the speed still holds on a real repo**

Run: `./zig-out/bin/zigstatus ~/src/roll20-private-sheets >/dev/null` and time it informally; sanity-check output line count vs `git -C ~/src/roll20-private-sheets status --porcelain | wc -l`. (The full FM-ZIG-READ-ENGINE bench numbers live in `bench/walk-spike/VERDICT.md`; 1a is about *correctness*, but it must not have regressed the walk speed.)

- [ ] **Step 4: Write `src/zigstatus/README.md`** documenting scope (worktree dimension, 1a), the harness (`python3 test/validate.py`), the Zig version, and that Phase 1b adds the staged/index-vs-HEAD dimension + conflicts. Update the `todo.md` FM-ZIG-READ-ENGINE entry: "Phase 1a done (worktree status validated vs git --porcelain on N cases)."

- [ ] **Step 5: Commit** `test(zigstatus): corpus validation green + docs (FM-ZIG P1a complete)`.

---

## Self-Review notes

- **Spec coverage (1a subset):** worktree-modified (T2) + racy hashing (T2), deleted (T3), untracked files (T1), untracked-dir collapse (T4), gitignore exclusion (T5), validated byte-for-byte vs `git status --porcelain` (the harness, T1, exercised every task). Index-vs-HEAD staged status, conflicts, index v3/v4, and submodule/sparse fallback are **explicitly deferred to Phase 1b** (the next plan) — noted in Global Constraints and T6 docs.
- **Type consistency:** `index.Entry{mtime_sec,mtime_nsec,size,sha:[20]u8,mode}` defined in T1, used in T2/T3; `EmitFn = fn(ctx,*anyopaque, x:u8,y:u8,path:[]const u8) void` defined T1, used everywhere; `sha1.gitBlob(gpa,bytes)->[20]u8` T2 used T2. The oracle is `git status --porcelain` normalized to sorted `XY path` throughout.
- **0.16 API risk:** the plan calls out reading `/opt/local/lib/zig/std/` for exact signatures (the `std.Io` shape, `File.stdout()`, unmanaged `ArrayList`) rather than assuming — the spike already proved these work; the implementer copies its walk.
- **No silent caps:** the deleted sweep (T3) reasons explicitly about pruned dirs; the racy-clean path (T2) only hashes when size matches but mtime differs (not every file), matching git's behavior and preserving the speed win.
