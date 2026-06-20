// Unit tests for mg.text pure leaf utilities (task C1).
// Char classification must match cinfo.c byte-for-byte (quirks included).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

import mg.text;

using namespace mg::text;

TEST_CASE("is_ctrl flags control characters and DEL only")
{
    CHECK(is_ctrl(0x00));
    CHECK(is_ctrl(0x1F));
    CHECK(is_ctrl(0x7F)); // DEL
    CHECK_FALSE(is_ctrl(' '));
    CHECK_FALSE(is_ctrl('A'));
}

TEST_CASE("is_word matches cinfo, including its quirks")
{
    CHECK(is_word('A'));
    CHECK(is_word('z'));
    CHECK(is_word('0'));
    CHECK(is_word('$'));
    CHECK(is_word('%'));
    CHECK(is_word('\''));
    CHECK(is_word(0xC0)); // À
    CHECK(is_word(0xDF)); // ß  (word, but no case)
    // Quirks faithfully copied from cinfo.c (not "correct" Latin-1):
    CHECK(is_word(0xD7));        // multiplication sign marked as a word char
    CHECK_FALSE(is_word('_'));   // underscore is NOT a word char by default
    CHECK_FALSE(is_word(0xD0));  // capital Eth marked as non-word
    CHECK_FALSE(is_word(' '));
    CHECK_FALSE(is_word('!'));
}

TEST_CASE("is_digit flags only 0-9")
{
    CHECK(is_digit('0'));
    CHECK(is_digit('9'));
    CHECK_FALSE(is_digit('a'));
    CHECK_FALSE(is_digit('$'));
}

TEST_CASE("is_upper / is_lower split ASCII and Latin-1 letters")
{
    CHECK(is_upper('A'));
    CHECK(is_upper('Z'));
    CHECK(is_upper(0xC0));       // À
    CHECK_FALSE(is_upper('a'));
    CHECK_FALSE(is_upper(0xE0));  // à

    CHECK(is_lower('a'));
    CHECK(is_lower('z'));
    CHECK(is_lower(0xE0));       // à
    CHECK_FALSE(is_lower('A'));
    CHECK_FALSE(is_lower(0xC0));  // À
}

TEST_CASE("is_eosp flags end-of-sentence punctuation")
{
    CHECK(is_eosp('.'));
    CHECK(is_eosp('!'));
    CHECK(is_eosp('?'));
    CHECK_FALSE(is_eosp(','));
    CHECK_FALSE(is_eosp(' '));
}

TEST_CASE("next_tabstop rounds up to the next multiple of tabw")
{
    CHECK(next_tabstop(0, 8) == 8);
    CHECK(next_tabstop(1, 8) == 8);
    CHECK(next_tabstop(7, 8) == 8);
    CHECK(next_tabstop(8, 8) == 16);
    CHECK(next_tabstop(0, 4) == 4);
    CHECK(next_tabstop(5, 4) == 8);
}

// Classification is usable in constant expressions (the modern win over the
// runtime-mutable global cinfo[] table + macros).
static_assert(is_word('A'));
static_assert(!is_word('_'));
static_assert(is_ctrl(0x00));
static_assert(next_tabstop(7, 8) == 8);
