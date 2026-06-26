# Syntax-Highlighted Diffs (Zig tokenizer + per-cell color) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Syntax-highlight diff content in neomg's `*magit-status*`, `*magit-commit*`, and ediff views, using a bundled Zig tokenizer and a new per-cell terminal-color layer.

**Architecture:** Three layers. (1) A Zig 0.16.0 static lib (`syntax/`) exposing a C ABI line tokenizer. (2) A display color layer: a 4-bit token-kind field in the `vtcell` int (orthogonal to `MG_HL_BIT`), emitted as ANSI SGR foreground escapes by mg's ANSI driver. (3) Glue that, as a diff line is drawn, detects its language from the file path, tokenizes it, and ORs the kind into each cell — composing with ediff's reverse-video region.

**Tech Stack:** Zig 0.16.0 (static lib, C ABI), C/C++ (mg core `display.c`/`tty.c`/`ansi.c`, `magit_cmd.c`), CMake, doctest, the existing `mg.git`/bridge module stack.

## Global Constraints

- All new behavior is `#ifdef ENABLE_NATIVE_MAGIT`-gated; the OFF build (`c-legacy`) links **no** Zig lib and emits **no** color — verified by `nm ./build-c/src/neomg | grep -ic 'magit\|neomg_highlight'` == 0.
- **Zig 0.16.0** (MacPorts `/opt/local/bin/zig`). **Write idiomatic 0.16 — do NOT route around the model with hand-rolled workarounds or low-level escape hatches.** Before writing any Zig, READ the on-disk source: the langref `~/src/zig-docs/langref-0.16.0.html` and the version-matched stdlib `~/src/zig-docs/std/` (e.g. `std/Build.zig` for the build API, `std/Io.zig` for the I/O model). **Verify every std API name/signature against that source — do not guess.** Use a proper `build.zig` with `b.addLibrary(.{ .linkage = .static, .root_module = b.createModule(...) })` and `zig build` — NOT the low-level `zig build-lib`. (The tokenizer is pure computation: it takes no `std.Io` and no allocator — caller-provided buffers — which is itself the idiomatic shape for a pure C-ABI function.)
- `vtcell` is `int` (display.c:53). Reserved bits: `MG_HL_BIT 0x40000000` (bit 30, reverse-video), `VT_CONT (-1)` (wide-char continuation sentinel; never carries color). Codepoints occupy ≤ bit 20. **New:** `MG_COLOR_MASK 0x0F000000` (bits 24–27), `MG_COLOR_SHIFT 24` — a 0-based token kind (0 = none).
- Token kinds (shared Zig↔C, exact values): `0 NORMAL, 1 KEYWORD, 2 STRING, 3 COMMENT, 4 NUMBER, 5 TYPE, 6 FUNCTION, 7 PUNCT`.
- Kind→ANSI SGR foreground: NORMAL→none, KEYWORD→`34`(blue), STRING→`32`(green), COMMENT→`90`(bright-black), NUMBER→`36`(cyan), TYPE→`33`(yellow), FUNCTION→`35`(magenta), PUNCT→none. Reset → `39`.
- Build (ON): `cmake --build --preset cpp`; tests: `ctest --preset cpp`; single test: `./build/tests/test_git --test-case=NAME`. OFF: `cmake --build --preset c-legacy`.
- Per-task gate: macOS `ctest --preset cpp` green + OFF build 0 symbols. Cross-platform (Arch/Alpine) at the integration milestones.
- Work happens in a git worktree on branch `fm-syntax-highlight` (created at execution time via the using-git-worktrees skill).
- Commit trailer (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp
  ```

---

## File Structure

- `syntax/neomg_syntax.zig` — the Zig tokenizer (generic lexer + per-language keyword tables) with `export fn` C ABI.
- `syntax/neomg_syntax.h` — hand-written C header (struct + enum + the two functions).
- `syntax/test_syntax.zig` — Zig unit tests (run with `zig test` during the TDD loop).
- `syntax/build.zig` — idiomatic 0.16 build: `b.addLibrary(.{ .linkage = .static, … })` + a `test` step; `zig build` → `zig-out/lib/libneomg_syntax.a`.
- `src/magit/CMakeLists.txt` — custom command building `libneomg_syntax.a`, linked into `mg_magit`.
- `src/display.c` — cell color macros, ANSI fg emit, `cellcolor`/`ttputcell` updates, the diff-line color hook.
- `src/ansi.c` — (only if a cap slot is added; the plan emits SGR inline, so likely untouched).
- `src/magit_cmd.c` — per-diff-line language lookup (`magit_cell_color` analog) feeding the display hook.
- `tests/test_syntax_bridge.cpp` — C doctest: a diff line → expected per-cell kind sequence.
- `docker/Dockerfile.arch`, `docker/Dockerfile.alpine`, `.github/workflows/*`, `packaging/aur/PKGBUILD` — add `zig`.

---

## Task 1: Zig tokenizer + C ABI + Zig tests

**Files:**
- Create: `syntax/neomg_syntax.zig`, `syntax/neomg_syntax.h`, `syntax/test_syntax.zig`

**Interfaces:**
- Produces (C ABI):
  ```c
  /* neomg_syntax.h */
  #ifndef NEOMG_SYNTAX_H
  #define NEOMG_SYNTAX_H
  #include <stddef.h>
  #include <stdint.h>
  #ifdef __cplusplus
  extern "C" {
  #endif
  enum { NEOMG_NORMAL=0, NEOMG_KEYWORD, NEOMG_STRING, NEOMG_COMMENT,
         NEOMG_NUMBER, NEOMG_TYPE, NEOMG_FUNCTION, NEOMG_PUNCT };
  typedef struct { uint16_t start; uint16_t len; uint8_t kind; } NeomgSpan;
  uint8_t neomg_lang_from_path(const char *path);
  size_t  neomg_highlight_line(uint8_t lang, const char *text, size_t len,
                               NeomgSpan *out, size_t cap);
  #ifdef __cplusplus
  }
  #endif
  #endif
  ```
  (`lang` ids: `0 GENERIC, 1 C, 2 JS, 3 PY, 4 RUST, 5 GO, 6 SHELL, 7 JSON, 8 CSS, 9 HTML`.)

- [ ] **Step 1: Write the failing Zig tests**

Create `syntax/test_syntax.zig`:
```zig
const std = @import("std");
const s = @import("neomg_syntax.zig");

fn kindsOf(lang: u8, text: []const u8, buf: []s.NeomgSpan) []s.NeomgSpan {
    const n = s.highlight(lang, text, buf);
    return buf[0..n];
}

test "C keyword + number + string" {
    var buf: [64]s.NeomgSpan = undefined;
    const spans = kindsOf(s.LANG_C, "int x = 42; // hi", &buf);
    // expect a KEYWORD span ("int"), a NUMBER span ("42"), a COMMENT span ("// hi")
    var saw_kw = false; var saw_num = false; var saw_com = false;
    for (spans) |sp| {
        switch (sp.kind) {
            s.KEYWORD => saw_kw = true,
            s.NUMBER => saw_num = true,
            s.COMMENT => saw_com = true,
            else => {},
        }
    }
    try std.testing.expect(saw_kw and saw_num and saw_com);
}

test "string span covers quotes" {
    var buf: [64]s.NeomgSpan = undefined;
    const spans = kindsOf(s.LANG_C, "x = \"ab\"", &buf);
    var found = false;
    for (spans) |sp| if (sp.kind == s.STRING) {
        try std.testing.expectEqual(@as(u16, 4), sp.start); // the opening quote
        try std.testing.expectEqual(@as(u16, 4), sp.len);   // "ab"
        found = true;
    };
    try std.testing.expect(found);
}

test "lang from path" {
    try std.testing.expectEqual(s.LANG_C, s.langFromPath("a/b.c"));
    try std.testing.expectEqual(s.LANG_JS, s.langFromPath("x.ts"));
    try std.testing.expectEqual(s.LANG_GENERIC, s.langFromPath("x.unknownext"));
}

test "generic fallback: strings/numbers/comments only, no keywords" {
    var buf: [64]s.NeomgSpan = undefined;
    const spans = kindsOf(s.LANG_GENERIC, "int x = 1", &buf);
    for (spans) |sp| try std.testing.expect(sp.kind != s.KEYWORD);
}

test "utf8 safe: multibyte content does not split or overflow" {
    var buf: [64]s.NeomgSpan = undefined;
    _ = kindsOf(s.LANG_C, "x = \"\u{00e9}\u{00e9}\"", &buf); // no crash; spans in-bounds
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd syntax && zig test test_syntax.zig`
Expected: FAIL — `neomg_syntax.zig` / `highlight` not defined.

- [ ] **Step 3: Implement `syntax/neomg_syntax.zig`**

```zig
const std = @import("std");

pub const NeomgSpan = extern struct { start: u16, len: u16, kind: u8 };

// token kinds (must match neomg_syntax.h)
pub const NORMAL: u8 = 0;
pub const KEYWORD: u8 = 1;
pub const STRING: u8 = 2;
pub const COMMENT: u8 = 3;
pub const NUMBER: u8 = 4;
pub const TYPE: u8 = 5;
pub const FUNCTION: u8 = 6;
pub const PUNCT: u8 = 7;

// language ids
pub const LANG_GENERIC: u8 = 0;
pub const LANG_C: u8 = 1;
pub const LANG_JS: u8 = 2;
pub const LANG_PY: u8 = 3;
pub const LANG_RUST: u8 = 4;
pub const LANG_GO: u8 = 5;
pub const LANG_SHELL: u8 = 6;
pub const LANG_JSON: u8 = 7;
pub const LANG_CSS: u8 = 8;
pub const LANG_HTML: u8 = 9;

const Lang = struct {
    keywords: []const []const u8,
    line_comment: []const u8, // "" if none
    block_open: []const u8,   // "" if none
    block_close: []const u8,
    hash_comment: bool,       // '#' starts a comment (py/sh)
};

const C_KW = [_][]const u8{ "int","char","void","return","if","else","for","while","struct","const","static","sizeof","switch","case","break","continue","unsigned","signed","long","short","double","float","enum","union","typedef","extern","do","goto","default" };
const JS_KW = [_][]const u8{ "function","return","if","else","for","while","const","let","var","class","new","this","import","export","from","async","await","try","catch","throw","typeof","instanceof","switch","case","break","continue","default","null","undefined","true","false" };
const PY_KW = [_][]const u8{ "def","return","if","elif","else","for","while","import","from","as","class","try","except","finally","with","lambda","yield","pass","break","continue","None","True","False","and","or","not","in","is" };

fn langOf(id: u8) Lang {
    return switch (id) {
        LANG_C, LANG_RUST, LANG_GO => .{ .keywords = &C_KW, .line_comment = "//", .block_open = "/*", .block_close = "*/", .hash_comment = false },
        LANG_JS, LANG_CSS => .{ .keywords = &JS_KW, .line_comment = "//", .block_open = "/*", .block_close = "*/", .hash_comment = false },
        LANG_PY, LANG_SHELL => .{ .keywords = &PY_KW, .line_comment = "", .block_open = "", .block_close = "", .hash_comment = true },
        else => .{ .keywords = &[_][]const u8{}, .line_comment = "", .block_open = "", .block_close = "", .hash_comment = false }, // GENERIC/JSON/HTML
    };
}

fn isIdentStart(c: u8) bool { return std.ascii.isAlphabetic(c) or c == '_'; }
fn isIdent(c: u8) bool { return std.ascii.isAlphanumeric(c) or c == '_'; }

fn isKeyword(lang: Lang, word: []const u8) bool {
    for (lang.keywords) |kw| if (std.mem.eql(u8, kw, word)) return true;
    return false;
}

// Core: tokenize one line. Slice-based (Zig-native); the C ABI wraps it.
pub fn highlight(lang_id: u8, text: []const u8, out: []NeomgSpan) usize {
    const lang = langOf(lang_id);
    var n: usize = 0;
    var i: usize = 0;
    const emit = struct {
        fn add(o: []NeomgSpan, idx: *usize, start: usize, len: usize, kind: u8) void {
            if (idx.* < o.len and len > 0) { o[idx.*] = .{ .start = @intCast(start), .len = @intCast(len), .kind = kind }; idx.* += 1; }
        }
    }.add;
    while (i < text.len) {
        const c = text[i];
        // line comment
        if (lang.line_comment.len > 0 and std.mem.startsWith(u8, text[i..], lang.line_comment)) {
            emit(out, &n, i, text.len - i, COMMENT); break;
        }
        if (lang.hash_comment and c == '#') { emit(out, &n, i, text.len - i, COMMENT); break; }
        // block comment (best-effort, single line)
        if (lang.block_open.len > 0 and std.mem.startsWith(u8, text[i..], lang.block_open)) {
            const rest = text[i..];
            const end = std.mem.indexOf(u8, rest, lang.block_close);
            const len = if (end) |e| e + lang.block_close.len else rest.len;
            emit(out, &n, i, len, COMMENT); i += len; continue;
        }
        // string
        if (c == '"' or c == '\'') {
            var j = i + 1;
            while (j < text.len and text[j] != c) : (j += 1) { if (text[j] == '\\') j += 1; }
            const len = (if (j < text.len) j + 1 else j) - i;
            emit(out, &n, i, len, STRING); i += len; continue;
        }
        // number
        if (std.ascii.isDigit(c)) {
            var j = i + 1;
            while (j < text.len and (std.ascii.isAlphanumeric(text[j]) or text[j] == '.')) : (j += 1) {}
            emit(out, &n, i, j - i, NUMBER); i = j; continue;
        }
        // identifier / keyword
        if (isIdentStart(c)) {
            var j = i + 1;
            while (j < text.len and isIdent(text[j])) : (j += 1) {}
            const word = text[i..j];
            if (isKeyword(lang, word)) emit(out, &n, i, j - i, KEYWORD);
            i = j; continue;
        }
        i += 1; // skip whitespace / punctuation (NORMAL — no span)
    }
    return n;
}

pub fn langFromPath(path: []const u8) u8 {
    const dot = std.mem.lastIndexOfScalar(u8, path, '.') orelse return LANG_GENERIC;
    const ext = path[dot + 1 ..];
    const map = .{
        .{ "c", LANG_C }, .{ "h", LANG_C }, .{ "cpp", LANG_C }, .{ "cc", LANG_C }, .{ "hpp", LANG_C },
        .{ "js", LANG_JS }, .{ "ts", LANG_JS }, .{ "jsx", LANG_JS }, .{ "tsx", LANG_JS },
        .{ "py", LANG_PY }, .{ "rs", LANG_RUST }, .{ "go", LANG_GO },
        .{ "sh", LANG_SHELL }, .{ "bash", LANG_SHELL },
        .{ "json", LANG_JSON }, .{ "css", LANG_CSS }, .{ "html", LANG_HTML }, .{ "htm", LANG_HTML },
    };
    inline for (map) |m| if (std.mem.eql(u8, ext, m[0])) return m[1];
    return LANG_GENERIC;
}

// ---- C ABI ----
export fn neomg_lang_from_path(path: [*:0]const u8) u8 {
    return langFromPath(std.mem.span(path));
}
export fn neomg_highlight_line(lang: u8, text: [*]const u8, len: usize,
                               out: [*]NeomgSpan, cap: usize) usize {
    return highlight(lang, text[0..len], out[0..cap]);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd syntax && zig test test_syntax.zig`
Expected: PASS (all tests).

- [ ] **Step 5: Write `syntax/neomg_syntax.h`** (the exact header from Interfaces above).

- [ ] **Step 6: Commit**

```bash
git add syntax/
git commit -m "feat(syntax): Zig 0.16 line tokenizer + C ABI (FM-SYNTAX-HIGHLIGHT P1)"
```

---

## Task 2: idiomatic `build.zig` + link the Zig lib into mg_magit

**Files:**
- Create: `syntax/build.zig`
- Modify: `src/magit/CMakeLists.txt`
- Create: `tests/test_syntax_bridge.cpp` (C-side smoke that calls the ABI)
- Modify: `tests/CMakeLists.txt` (register the smoke test)

**Interfaces:**
- Consumes: `neomg_highlight_line`, `neomg_lang_from_path` (Task 1).
- Produces: `zig-out/lib/libneomg_syntax.a` linked into `mg_magit`; `neomg_syntax.h` on the include path.

- [ ] **Step 1: Write `syntax/build.zig` (idiomatic 0.16)**

> First read `~/src/zig-docs/std/Build.zig` (`addLibrary`, `LibraryOptions`, `addModule`/`createModule`, `Module.CreateOptions`) and confirm these names/fields against the on-disk source before writing. Do not use `zig build-lib`.

```zig
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const mod = b.createModule(.{
        .root_source_file = b.path("neomg_syntax.zig"),
        .target = target,
        .optimize = optimize,
    });
    const lib = b.addLibrary(.{
        .name = "neomg_syntax",
        .root_module = mod,
        .linkage = .static,
    });
    b.installArtifact(lib); // -> zig-out/lib/libneomg_syntax.a

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("test_syntax.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the tokenizer tests");
    test_step.dependOn(&run_tests.step);
}
```
Verify: `cd syntax && zig build` produces `zig-out/lib/libneomg_syntax.a`, and `zig build test` runs the Task-1 tests green.

- [ ] **Step 2: Add the Zig build + link to `src/magit/CMakeLists.txt`**

Append (after `add_library(mg_magit)` and its sources):
```cmake
# FM-SYNTAX-HIGHLIGHT: build the Zig tokenizer (idiomatic build.zig) + link it.
if(ENABLE_NATIVE_MAGIT)
    find_program(ZIG_EXECUTABLE zig REQUIRED)
    set(NEOMG_SYNTAX_LIB
        ${CMAKE_SOURCE_DIR}/syntax/zig-out/lib/libneomg_syntax.a)
    add_custom_command(
        OUTPUT ${NEOMG_SYNTAX_LIB}
        COMMAND ${ZIG_EXECUTABLE} build --prefix
                ${CMAKE_SOURCE_DIR}/syntax/zig-out
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/syntax
        DEPENDS ${CMAKE_SOURCE_DIR}/syntax/neomg_syntax.zig
                ${CMAKE_SOURCE_DIR}/syntax/build.zig
        COMMENT "Building Zig syntax tokenizer (zig build)")
    add_custom_target(neomg_syntax_lib DEPENDS ${NEOMG_SYNTAX_LIB})
    add_dependencies(mg_magit neomg_syntax_lib)
    target_link_libraries(mg_magit PUBLIC ${NEOMG_SYNTAX_LIB})
    target_include_directories(mg_magit PUBLIC ${CMAKE_SOURCE_DIR}/syntax)
endif()
```
(Confirm the `--prefix`/output path against `zig build --help`; `zig build` installs to `zig-out` by default, so `--prefix` may be omittable.)

- [ ] **Step 3: Write the failing C smoke test `tests/test_syntax_bridge.cpp`**

```cpp
#include <doctest/doctest.h>
#include "neomg_syntax.h"
#include <cstring>

TEST_CASE("neomg_highlight_line tags a C keyword")
{
    NeomgSpan spans[32];
    const char *line = "int x = 42;";
    uint8_t lang = neomg_lang_from_path("foo.c");
    size_t n = neomg_highlight_line(lang, line, std::strlen(line), spans, 32);
    bool saw_keyword = false, saw_number = false;
    for (size_t i = 0; i < n; ++i) {
        if (spans[i].kind == NEOMG_KEYWORD) saw_keyword = true;
        if (spans[i].kind == NEOMG_NUMBER) saw_number = true;
    }
    CHECK(saw_keyword);
    CHECK(saw_number);
}
```

- [ ] **Step 4: Register it in `tests/CMakeLists.txt`** (inside the `if(ENABLE_NATIVE_MAGIT)` block):
```cmake
    add_executable(test_syntax_bridge test_syntax_bridge.cpp)
    target_link_libraries(test_syntax_bridge PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_syntax_bridge)
```

- [ ] **Step 5: Build + run — verify the link works and the test passes**

Run: `cmake --preset cpp && cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_syntax_bridge`
Expected: builds (Zig lib compiled + linked), test PASS.

- [ ] **Step 6: OFF build links no Zig**

Run: `cmake --build --preset c-legacy 2>&1 | tail -1 && nm ./build-c/src/neomg | grep -ic 'neomg_highlight'`
Expected: `0`.

- [ ] **Step 7: Commit**

```bash
git add src/magit/CMakeLists.txt tests/test_syntax_bridge.cpp tests/CMakeLists.txt
git commit -m "build(syntax): compile+link the Zig tokenizer into mg_magit (FM-SYNTAX-HIGHLIGHT P1)"
```

---

## Task 3: Display color layer (cell kind field + ANSI fg emit + compose)

**Files:**
- Modify: `src/display.c` (color macros near MG_HL_BIT ~line 41; `ttputcell` ~920; `cellcolor` ~942; `uline` ~990)

**Interfaces:**
- Consumes: nothing external.
- Produces: `MG_COLOR_MASK`/`MG_COLOR_SHIFT` macros; `static void ttfgkind(int kind)` (emits ANSI SGR fg for a token kind, gated); `cellcolor`/`ttputcell` honoring the color field. A cell encodes `codepoint | (kind<<MG_COLOR_SHIFT) | [MG_HL_BIT]`.

- [ ] **Step 1: Add the macros** (in `src/display.c`, right after `#define MG_HL_BIT 0x40000000`):
```c
#define MG_COLOR_SHIFT 24
#define MG_COLOR_MASK  0x0F000000   /* bits 24-27: token kind (0 = none) */
```

- [ ] **Step 2: Strip the color field before encoding the glyph** — in `ttputcell` (the `c &= ~MG_HL_BIT;` line under `ENABLE_NATIVE_MAGIT`), change to:
```c
	c &= ~(MG_HL_BIT | MG_COLOR_MASK);	/* drop standout + color before encoding */
```

- [ ] **Step 3: Add the ANSI foreground emit** — add near `ttputcell` (gated `ENABLE_NATIVE_MAGIT`):
```c
/* Emit the ANSI SGR foreground for a token kind (0 = reset to default). mg's
 * ansi.c driver is ANSI-emulation, so we write SGR directly. */
static void
ttfgkind(int kind)
{
	static const char *code[8] = {
		"\033[39m",	/* 0 NORMAL  -> default */
		"\033[34m",	/* 1 KEYWORD -> blue    */
		"\033[32m",	/* 2 STRING  -> green   */
		"\033[90m",	/* 3 COMMENT -> bright black */
		"\033[36m",	/* 4 NUMBER  -> cyan    */
		"\033[33m",	/* 5 TYPE    -> yellow  */
		"\033[35m",	/* 6 FUNCTION-> magenta */
		"\033[39m",	/* 7 PUNCT   -> default */
	};
	const char *s = code[kind & 7];
	while (*s)
		ttputc((int)*s++);
}
```

- [ ] **Step 4: Make `uline`'s per-cell path emit color + standout together** — in the `magit_ediff_active`/per-cell branch of `uline` (around the `cellcolor` use ~line 991), generalize so it tracks the current kind and standout independently. Replace the ediff-only block with (active whenever any cell in the row carries color or HL):
```c
#ifdef ENABLE_NATIVE_MAGIT
		{
			int wantkind = (*cp1 == VT_CONT) ? -1 : ((*cp1 & MG_COLOR_MASK) >> MG_COLOR_SHIFT);
			int wanthl   = (*cp1 != VT_CONT) && (*cp1 & MG_HL_BIT);
			if (wantkind >= 0 && wantkind != curkind) { ttfgkind(wantkind); curkind = wantkind; }
			if (wanthl != curhl) { ttcolor(wanthl ? CMODE : CTEXT); curhl = wanthl; }
		}
#endif
		ttputcell(*cp1++);
```
…with `int curkind = 0, curhl = 0;` declared at the top of `uline` (gated), and a `ttfgkind(0); ttcolor(CTEXT);` reset before `return` in that branch. (This composes: a cell that is both colored and HL gets its fg color *and* reverse-video.)

- [ ] **Step 5: Add a doctest-free visual smoke is not possible here; instead verify no regression** — build and run the full suite (the color field defaults to 0 everywhere until Task 4 sets it, so output is unchanged):

Run: `cmake --build --preset cpp 2>&1 | tail -1 && ctest --preset cpp 2>&1 | grep -E "tests passed|failed"`
Expected: 100% pass (no behavior change yet — nothing sets the color field).

- [ ] **Step 6: OFF build unaffected**

Run: `cmake --build --preset c-legacy 2>&1 | tail -1`
Expected: builds clean (macros + ttfgkind are `ENABLE_NATIVE_MAGIT`-gated).

- [ ] **Step 7: Commit**

```bash
git add src/display.c
git commit -m "feat(display): per-cell token-color field + ANSI fg emit, composed with standout (FM-SYNTAX-HIGHLIGHT P2)"
```

---

## Task 4: Glue — color the status + commit diff surfaces

**Files:**
- Modify: `src/magit_cmd.c` (per-diff-line language + a `magit_cell_color` analog to the existing `magit_cell_highlighted`)
- Modify: `src/display.c` (the vt-fill hook ~line 444, where ediff sets `MG_HL_BIT`)
- Modify: `tests/test_syntax_bridge.cpp` (add the cell-mapping doctest)

**Interfaces:**
- Consumes: `neomg_highlight_line`/`neomg_lang_from_path` (Task 1); `MG_COLOR_SHIFT`/`MG_COLOR_MASK`, `ttfgkind` (Task 3).
- Produces: `int magit_cell_color(struct buffer *bp, struct line *lp, int ci)` — returns the token kind (0–7) for character index `ci` of buffer line `lp`, or 0 if not a magit diff line. Declared in the magit C header consumed by `display.c`.

- [ ] **Step 1: Implement `magit_cell_color` in `magit_cmd.c`**

For a `*magit-status*`/`*magit-commit*` buffer line that is a diff content line (origin `+`/`-`/space), look up the owning file path (the buffer already tracks per-line metadata for diff lines — reuse that map), derive the language once per line, tokenize the line text (skip the origin column), and return the kind covering character `ci`. Concretely:
```c
/* Returns the syntax token kind (0-7) for char ci of a diff line, else 0. */
int
magit_cell_color(struct buffer *bp, struct line *lp, int ci)
{
	const char *path;
	uint8_t lang;
	static struct line *cached_lp;
	static NeomgSpan spans[256];
	static size_t nspans;
	static uint8_t cached_kindcol[1024];

	if (!magit_is_diff_line(bp, lp, &path))   /* existing per-line kind/path lookup */
		return (0);
	if (lp != cached_lp) {                     /* memoize per line */
		const char *text = ltext(lp);
		int len = llength(lp);
		int off = (len > 0 && (text[0]=='+'||text[0]=='-'||text[0]==' ')) ? 1 : 0;
		lang = neomg_lang_from_path(path);
		nspans = neomg_highlight_line(lang, text + off, (size_t)(len - off),
		                              spans, 256);
		memset(cached_kindcol, 0, sizeof(cached_kindcol));
		for (size_t k = 0; k < nspans; k++)
			for (int j = 0; j < spans[k].len && (spans[k].start+off+j) < 1024; j++)
				cached_kindcol[spans[k].start + off + j] = spans[k].kind;
		cached_lp = lp;
	}
	return (ci >= 0 && ci < 1024) ? cached_kindcol[ci] : 0;
}
```
(Use the existing diff-line/path tracking — mirror how `magit_cell_highlighted` finds its state. If no per-line path map exists for status diffs yet, add one keyed by line, populated in the emit callback that already receives `MG_LINE_DIFF` + path.)

- [ ] **Step 2: Hook it in `display.c`** — next to the ediff `MG_HL_BIT` loop (~line 444), add (gated):
```c
		if (magit_native_active) {  /* status/commit/ediff buffers */
			int kind = magit_cell_color(wp->w_bufp, lp, ci);
			if (kind)
				for (k = start; k < vtcol && k < ncol; k++)
					if (vp->v_text[k] != VT_CONT)
						vp->v_text[k] |= (kind << MG_COLOR_SHIFT);
		}
```
(Declare `int magit_cell_color(struct buffer *, struct line *, int);` in the magit header `display.c` includes.)

- [ ] **Step 3: Write the failing cell-mapping doctest** in `tests/test_syntax_bridge.cpp`:
```cpp
TEST_CASE("diff-line tokenization maps columns to kinds (engine-level)")
{
    // The C glue's per-column mapping logic, exercised directly via the ABI:
    NeomgSpan spans[64];
    const char *content = "int n = 7;"; // (origin stripped by the caller)
    uint8_t lang = neomg_lang_from_path("x.c");
    size_t ns = neomg_highlight_line(lang, content, std::strlen(content), spans, 64);
    uint8_t col[64] = {0};
    for (size_t k = 0; k < ns; ++k)
        for (int j = 0; j < spans[k].len; ++j) col[spans[k].start + j] = spans[k].kind;
    CHECK(col[0] == NEOMG_KEYWORD);  // 'i' of "int"
    CHECK(col[8] == NEOMG_NUMBER);   // '7'
}
```

- [ ] **Step 4: Build + run the doctest**

Run: `cmake --build --preset cpp 2>&1 | tail -1 && ./build/tests/test_syntax_bridge`
Expected: PASS.

- [ ] **Step 5: pty smoke — colored diffs in status + commit**

Run a tmux session: open neomg in a repo with a changed `.c`/`.js` file, `C-x g`, `TAB` to expand a hunk; capture the pane and confirm SGR color escapes are present around keywords/strings (e.g. `tmux capture-pane -e -p` shows `\033[34m`/`\033[32m`). Open a commit (`RET` on a recent commit) and confirm the same. Document the captures in the task report.

- [ ] **Step 6: Full suite + OFF build**

Run: `ctest --preset cpp 2>&1 | grep -E "tests passed|failed"` → 100%.
Run: `cmake --build --preset c-legacy && nm ./build-c/src/neomg | grep -ic 'neomg_highlight\|magit_cell_color'` → `0`.

- [ ] **Step 7: Commit**

```bash
git add src/magit_cmd.c src/display.c tests/test_syntax_bridge.cpp
git commit -m "feat(magit): syntax-color status + commit diffs via the Zig tokenizer (FM-SYNTAX-HIGHLIGHT P3)"
```

---

## Task 5: ediff reconciliation (syntax color composed with reverse-video)

**Files:**
- Modify: `src/magit_cmd.c` (ensure `magit_cell_color` returns kinds for ediff-pane buffers too)
- Verify: `src/display.c` (the Task-3 `uline` branch already composes color + HL)

**Interfaces:**
- Consumes: Task 3's compose logic; Task 4's `magit_cell_color`.

- [ ] **Step 1: Extend `magit_is_diff_line`/`magit_cell_color` to recognize ediff-pane content lines** (the merged/ours/theirs panes), deriving the language from the conflicted file's path (the ediff buffer already knows the file under resolution).

- [ ] **Step 2: pty smoke — ediff shows syntax color AND the active region**

tmux: trigger a conflict (the repo fixtures / a scripted merge), `E` to open ediff; capture the pane with `-e` and confirm an active-region line carries BOTH reverse-video (`\033[7m`) AND a syntax fg color (`\033[3Xm`) — i.e. they compose, the region is still distinguishable. Step the region (`n`/`p`) and confirm color persists. Document captures.

- [ ] **Step 3: Full suite + OFF build**

Run: `ctest --preset cpp 2>&1 | grep -E "tests passed|failed"` → 100%; OFF build 0 symbols.

- [ ] **Step 4: Commit**

```bash
git add src/magit_cmd.c
git commit -m "feat(magit): compose syntax color with ediff's active-region highlight (FM-SYNTAX-HIGHLIGHT P4)"
```

---

## Task 6: Build deps + cross-platform + docs/PR

**Files:**
- Modify: `docker/Dockerfile.arch`, `docker/Dockerfile.alpine` (add `zig`)
- Modify: `.github/workflows/linux.yml` (zig available in the build containers — already via the Dockerfiles)
- Modify: `packaging/aur/PKGBUILD` (`makedepends+=('zig')`)
- Modify: `todo.md`, the spec (mark phases done)

- [ ] **Step 1: Add `zig` to the Arch Dockerfile** — append `zig` to the `pacman -S` package list in `docker/Dockerfile.arch`.

- [ ] **Step 2: Add `zig` to Alpine** — `apk add zig` in `docker/Dockerfile.alpine` (or document if Alpine's zig is too old for 0.16; if so, skip the magit/syntax build on Alpine or fetch the 0.16.0 tarball — note the decision in the task report).

- [ ] **Step 3: Add `zig` to the AUR PKGBUILD** — `makedepends=(... 'zig')` in `packaging/aur/PKGBUILD`.

- [ ] **Step 4: Cross-platform gate**

Run: `docker build -f docker/Dockerfile.arch -t mg-arch .` → builds + tests green (Zig lib compiles in-container).
Run: `docker build -f docker/Dockerfile.alpine -t mg-alpine .` → green (or the documented degrade).

- [ ] **Step 5: Mark the spec phases done + update `todo.md`**; commit.

```bash
git add docker/ packaging/aur/PKGBUILD .github/ todo.md docs/superpowers/specs/2026-06-25-fm-syntax-highlight-diffs-design.md
git commit -m "build+docs: zig in CI/Docker/AUR; FM-SYNTAX-HIGHLIGHT complete"
```

- [ ] **Step 6: Push + open the stacked PR** (base `neomg`), summarizing the 4 layers, the measured build-dep addition, and the pty captures.

---

## Self-Review notes

- **Spec coverage:** Zig tokenizer + C ABI (Task 1); 16-color via ANSI SGR in mg's ANSI driver (Task 3 — the spec's "terminfo set_a_foreground" concretely realized as SGR, since ansi.c is an ANSI emulator); per-cell color-index orthogonal to MG_HL_BIT (Task 3); compute-at-render hook where ediff injects attributes (Task 4); all diff surfaces incl. ediff (Tasks 4–5); ediff compose (Tasks 3+5); build-dep zig (Tasks 2,6); OFF build gated (every task's gate); Zig 0.16.0 pinned (Global Constraints). All spec sections map to tasks.
- **Placeholder scan:** the one investigation point is Step-1 of Task 4 ("if no per-line path map exists, add one") — flagged because the exact status-buffer line→path tracking must be read at implementation; the mechanism (mirror `magit_cell_highlighted`) and the fallback (add a line-keyed map populated in the MG_LINE_DIFF emit) are both specified, so it's a real instruction, not a TODO.
- **Type consistency:** token kinds 0–7 and lang ids 0–9 identical across `neomg_syntax.zig`, `neomg_syntax.h`, `ttfgkind`, and `magit_cell_color`; `MG_COLOR_SHIFT`/`MG_COLOR_MASK` consistent display.c↔magit_cmd.c; `NeomgSpan` layout identical Zig (`extern struct`) ↔ C.
