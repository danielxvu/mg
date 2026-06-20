// Unit tests for the mg.utf8 codepoint layer (task U1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
