// Integration test for the mg.text extern "C" bridge (task C1.5).
// This is exactly what chrdef.h's ISWORD/etc. call under ENABLE_CPP_UPGRADES.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bridge.h"

TEST_CASE("predicates match the mg.text classification (1/0)")
{
    CHECK(mg_text_is_word('A') == 1);
    CHECK(mg_text_is_word('$') == 1);
    CHECK(mg_text_is_word(' ') == 0);
    CHECK(mg_text_is_ctrl(0x00) == 1);
    CHECK(mg_text_is_ctrl('A') == 0);
    CHECK(mg_text_is_upper('A') == 1);
    CHECK(mg_text_is_lower('a') == 1);
    CHECK(mg_text_is_eosp('.') == 1);
    CHECK(mg_text_is_eosp(',') == 0);
    CHECK(mg_text_is_digit('7') == 1);
    CHECK(mg_text_is_digit('x') == 0);
}

TEST_CASE("underscore toggle flips is_word('_') for ctags tag matching")
{
    CHECK(mg_text_is_word('_') == 0); // default: not a word char

    mg_text_set_underscore_word(1);
    CHECK(mg_text_is_word('_') == 1); // ctags mode: word char
    CHECK(mg_text_is_word('A') == 1); // other chars unaffected
    CHECK(mg_text_is_word(' ') == 0);

    mg_text_set_underscore_word(0);
    CHECK(mg_text_is_word('_') == 0); // restored
}
