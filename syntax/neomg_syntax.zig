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
            const end = std.mem.find(u8, rest, lang.block_close);
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
    const dot = std.mem.findScalarLast(u8, path, '.') orelse return LANG_GENERIC;
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
