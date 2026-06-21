// Unit tests for the mg.utf8 codepoint layer (task U1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>

import mg.utf8;

using namespace mg::utf8;

TEST_CASE("decode_first: ASCII is one byte, width 1")
{
    auto d = decode_first("A");
    CHECK(d.cp == U'A');
    CHECK(d.bytes == 1);
    CHECK(d.width == 1);
}

TEST_CASE("decode_first: 2-byte e-acute")
{
    auto d = decode_first("\xC3\xA9"); // U+00E9 é
    CHECK(d.cp == U'é');
    CHECK(d.bytes == 2);
    CHECK(d.width == 1);
}

TEST_CASE("decode_first: 3-byte CJK is wide (width 2)")
{
    auto d = decode_first("\xE6\x97\xA5"); // U+65E5 日
    CHECK(d.cp == U'日');
    CHECK(d.bytes == 3);
    CHECK(d.width == 2);
}

TEST_CASE("decode_first: 4-byte emoji is wide")
{
    auto d = decode_first("\xF0\x9F\x98\x80"); // U+1F600 😀
    CHECK(d.cp == U'\U0001F600');
    CHECK(d.bytes == 4);
    CHECK(d.width == 2);
}

TEST_CASE("decode_first: combining mark has width 0")
{
    auto d = decode_first("\xCC\x81"); // U+0301 combining acute
    CHECK(d.cp == U'́');
    CHECK(d.bytes == 2);
    CHECK(d.width == 0);
}

TEST_CASE("decode_first: invalid lead byte advances one, replacement char")
{
    auto d = decode_first("\xFF");
    CHECK(d.cp == U'�');
    CHECK(d.bytes == 1);
    CHECK(d.width == 1);
}

TEST_CASE("decode_first: truncated multibyte at end of span never over-reads")
{
    // A 2-byte lead with no continuation available (span length 1).
    std::string_view lead("\xC3", 1);
    auto d = decode_first(lead);
    CHECK(d.cp == U'�');
    CHECK(d.bytes == 1);
}

TEST_CASE("decode_first: empty span yields zero bytes")
{
    auto d = decode_first(std::string_view());
    CHECK(d.bytes == 0);
}

TEST_CASE("char_width matches decode for representative codepoints")
{
    CHECK(char_width(U'A') == 1);
    CHECK(char_width(U'日') == 2); // 日
    CHECK(char_width(U'́') == 0); // combining acute
}

TEST_CASE("encode produces the right byte counts (round-trips decode)")
{
    char buf[4];
    CHECK(encode(U'A', buf) == 1);
    CHECK(buf[0] == 'A');
    CHECK(encode(U'é', buf) == 2);
    CHECK(encode(U'日', buf) == 3);
    CHECK(encode(U'\U0001F600', buf) == 4);
    // round-trip: encode then decode_first yields the same codepoint
    int n = encode(U'日', buf);
    auto d = decode_first(std::string_view(buf, n));
    CHECK(d.cp == U'日');
}

TEST_CASE("is_word covers letters, digits, and non-ASCII letters")
{
    CHECK(is_word(U'A'));
    CHECK(is_word(U'5'));
    CHECK(is_word(U'é')); // é
    CHECK(is_word(U'日')); // 日
    CHECK_FALSE(is_word(U' '));
    CHECK_FALSE(is_word(U'.'));
    CHECK_FALSE(is_word(U'!'));
}

TEST_CASE("is_space covers ASCII blanks and Unicode separators")
{
    CHECK(is_space(U' '));
    CHECK(is_space(U'\t'));
    CHECK(is_space(U'\n'));
    CHECK(is_space(U' ')); // no-break space (Zs)
    CHECK_FALSE(is_space(U'A'));
    CHECK_FALSE(is_space(U'.'));
}

TEST_CASE("to_upper / to_lower map ASCII, accented Latin, Greek, Cyrillic")
{
    CHECK(to_upper(U'a') == U'A');
    CHECK(to_lower(U'Z') == U'z');
    CHECK(to_upper(U'é') == U'É');  // 1:1, same 2-byte length
    CHECK(to_lower(U'Ñ') == U'ñ');
    CHECK(to_upper(U'α') == U'Α');  // Greek
    CHECK(to_lower(U'Я') == U'я');  // Cyrillic
}

TEST_CASE("to_upper / to_lower leave non-letters and same-case unchanged")
{
    CHECK(to_upper(U'5') == U'5');
    CHECK(to_upper(U'日') == U'日'); // no case
    CHECK(to_upper(U'A') == U'A');  // already upper
    CHECK(to_lower(U'z') == U'z');  // already lower
}

TEST_CASE("grapheme_break: combining marks attach, distinct letters separate")
{
    // A combining mark does NOT break from its base (same grapheme cluster).
    CHECK_FALSE(grapheme_break(U'e', U'́'));  // e + combining acute
    CHECK_FALSE(grapheme_break(U'a', U'̈'));  // a + combining diaeresis
    // Two combining marks stack on the same cluster.
    CHECK_FALSE(grapheme_break(U'́', U'̧'));
    // Distinct base letters DO break.
    CHECK(grapheme_break(U'e', U'f'));
    CHECK(grapheme_break(U'́', U'b')); // mark then a new base
    CHECK(grapheme_break(U'A', U'1'));
}

TEST_CASE("grapheme_len spans a base codepoint plus its combining marks")
{
    std::string nfd = "e\u0301f"; // e + combining acute + f (decomposed)
    CHECK(grapheme_len(nfd) == 3);            // "e" + 2-byte acute
    CHECK(grapheme_len(std::string_view(nfd).substr(3)) == 1); // "f"
    CHECK(grapheme_len("abc") == 1);          // plain ASCII
    CHECK(grapheme_len("\u00e9") == 2);      // precomposed (NFC) e-acute, one cp
    CHECK(grapheme_len("") == 0);
}

TEST_CASE("grapheme_back finds the start of the grapheme left of pos")
{
    std::string nfd = "e\u0301f"; // bytes: e(1) acute(2) f(1)
    CHECK(grapheme_back(nfd, 4) == 3); // left of 'f' end -> start of 'f'
    CHECK(grapheme_back(nfd, 3) == 0); // left of cluster end -> start of e+acute
    CHECK(grapheme_back("abc", 2) == 1);
    CHECK(grapheme_back(nfd, 0) == 0);
}
