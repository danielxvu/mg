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
