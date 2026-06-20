// mg.utf8 -- codepoint layer over utf8proc (task U1).
//
// The substrate for UTF-8 display + navigation: decode one codepoint from a
// byte span (codepoint + byte length + display width) and classify codepoints.
// utf8proc is stateless, so this is a thin, pure wrapper -- the value is a
// typed, tested API (+ an extern "C" bridge in bridge.cpp for the C core).
// Gated by ENABLE_CPP_UPGRADES; the OFF build stays byte-oriented.

module;
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
