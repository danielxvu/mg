#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "neomg_syntax.h"
#include <cstring>

TEST_CASE("neomg_highlight_line tags a C keyword")
{
    NeomgSpan spans[32];
    const char *line = "int x = 42;";
    uint8_t lang = neomg_lang_from_path("foo.c");
    size_t n = neomg_highlight_line(lang, line, std::strlen(line), spans, 32);
    CHECK(n > 0);
    bool saw_keyword = false, saw_number = false;
    for (size_t i = 0; i < n; ++i) {
        if (spans[i].kind == NEOMG_KEYWORD) saw_keyword = true;
        if (spans[i].kind == NEOMG_NUMBER) saw_number = true;
    }
    CHECK(saw_keyword);
    CHECK(saw_number);
}

TEST_CASE("neomg_lang_from_path returns 0 (generic) for unknown extension")
{
    // LANG_GENERIC == 0; unknown extensions must fall through to the generic tokenizer
    CHECK(neomg_lang_from_path("x.unknownext") == 0);
}

TEST_CASE("neomg_lang_from_path returns 0 (generic) for no extension")
{
    CHECK(neomg_lang_from_path("Makefile") == 0);
}

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
