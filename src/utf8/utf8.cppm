// mg.utf8 -- codepoint layer over utf8proc (task U1).
//
// The substrate for UTF-8 display + navigation: decode one codepoint from a
// byte span (codepoint + byte length + display width) and classify codepoints.
// utf8proc is stateless, so this is a thin, pure wrapper -- the value is a
// typed, tested API (+ an extern "C" bridge in bridge.cpp for the C core).
// Gated by ENABLE_CPP_UPGRADES; the OFF build stays byte-oriented.

module;
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <utf8proc.h>

export module mg.utf8;

export namespace mg::utf8 {

struct decoded {
    char32_t cp;     // the codepoint (U+FFFD on an invalid/truncated sequence)
    int      bytes;  // bytes consumed (>= 1 for a non-empty span; 0 if empty)
    int      width;  // display columns: 0 (combining), 1, or 2 (wide)
};

// Decode the first codepoint of `s`, bounded by s.size() (never over-reads).
// An invalid or truncated sequence yields {U+FFFD, 1, 1} so callers always make
// forward progress and never desync; an empty span yields {U+FFFD, 0, 0}.
decoded decode_first(std::string_view s);

// Display width of a codepoint (0 combining, 1, or 2 wide).
int char_width(char32_t cp);

// Encode `cp` as UTF-8 into `out` (must hold >= 4 bytes); returns byte count.
int encode(char32_t cp, char *out);

// Word constituent: any letter/number, plus connector punctuation ('_' etc.).
bool is_word(char32_t cp);

// Simple (1:1) Unicode case mapping. Non-letters and codepoints with no case
// map to themselves. Codepoints whose full case mapping is multi-character
// (e.g. U+00DF) are left unchanged.
char32_t to_upper(char32_t cp);
char32_t to_lower(char32_t cp);

// Is there a grapheme-cluster boundary between codepoints `a` and `b`? false
// means `b` extends `a`'s cluster (e.g. a base letter followed by a combining
// mark), so the cursor should treat them as one unit.
bool grapheme_break(char32_t a, char32_t b);

// Byte length of the first grapheme cluster in `s` (a base codepoint plus any
// following combining marks). 0 for empty input.
std::size_t grapheme_len(std::string_view s);

// Start byte offset of the grapheme cluster ending at `pos` within `s` (i.e.
// where the cursor lands stepping one grapheme left of `pos`). 0 if pos == 0.
std::size_t grapheme_back(std::string_view s, std::size_t pos);

// Whitespace: ASCII blanks/newlines/tabs and Unicode space/line/para separators.
bool is_space(char32_t cp);

} // namespace mg::utf8

// ---- definitions ----------------------------------------------------------
namespace mg::utf8 {

decoded decode_first(std::string_view s)
{
    if (s.empty())
        return {U'�', 0, 0};

    utf8proc_int32_t cp = -1;
    const utf8proc_ssize_t n = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(s.data()),
        static_cast<utf8proc_ssize_t>(s.size()), &cp);
    if (n < 1 || cp < 0)
        return {U'�', 1, 1}; // invalid/truncated: advance one byte

    int w = utf8proc_charwidth(cp);
    if (w < 0)
        w = 1;
    return {static_cast<char32_t>(cp), static_cast<int>(n), w};
}

int char_width(char32_t cp)
{
    const int w = utf8proc_charwidth(static_cast<utf8proc_int32_t>(cp));
    return w < 0 ? 1 : w;
}

int encode(char32_t cp, char *out)
{
    const utf8proc_ssize_t n = utf8proc_encode_char(
        static_cast<utf8proc_int32_t>(cp),
        reinterpret_cast<utf8proc_uint8_t *>(out));
    return n > 0 ? static_cast<int>(n) : 0;
}

bool is_word(char32_t cp)
{
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(cp))) {
    case UTF8PROC_CATEGORY_LU: // letters
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_ND: // numbers
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
    case UTF8PROC_CATEGORY_PC: // connector punctuation ('_')
        return true;
    default:
        return false;
    }
}

char32_t to_upper(char32_t cp)
{
    return static_cast<char32_t>(
        utf8proc_toupper(static_cast<utf8proc_int32_t>(cp)));
}

char32_t to_lower(char32_t cp)
{
    return static_cast<char32_t>(
        utf8proc_tolower(static_cast<utf8proc_int32_t>(cp)));
}

bool grapheme_break(char32_t a, char32_t b)
{
    // NULL state -> stateless break check (covers combining marks; complex
    // emoji ZWJ / regional-indicator pairs aren't fully tracked).
    return utf8proc_grapheme_break_stateful(
        static_cast<utf8proc_int32_t>(a), static_cast<utf8proc_int32_t>(b),
        nullptr);
}

std::size_t grapheme_len(std::string_view s)
{
    if (s.empty())
        return 0;
    decoded d = decode_first(s);
    std::size_t len = d.bytes ? d.bytes : 1;
    char32_t prev = d.cp;
    while (len < s.size()) {
        decoded nx = decode_first(s.substr(len));
        if (nx.bytes == 0 || grapheme_break(prev, nx.cp))
            break;
        len += nx.bytes;
        prev = nx.cp;
    }
    return len;
}

std::size_t grapheme_back(std::string_view s, std::size_t pos)
{
    auto cp_start = [&](std::size_t p) { // start of the codepoint ending at p
        std::size_t q = p - 1;
        while (q > 0 && (static_cast<unsigned char>(s[q]) & 0xC0) == 0x80)
            --q;
        return q;
    };
    if (pos == 0)
        return 0;
    std::size_t p = cp_start(pos);
    while (p > 0) {
        std::size_t q = cp_start(p);
        decoded a = decode_first(s.substr(q));
        decoded b = decode_first(s.substr(p));
        if (grapheme_break(a.cp, b.cp))
            break;
        p = q;
    }
    return p;
}

bool is_space(char32_t cp)
{
    if (cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' ||
        cp == U'\f' || cp == U'\v')
        return true;
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(cp))) {
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP:
        return true;
    default:
        return false;
    }
}

} // namespace mg::utf8
