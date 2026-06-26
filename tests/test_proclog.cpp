#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

import mg.magit.proclog;

#include "bridge.h"  // mg_magit_process_log + MG_LINE_PROCESS_*

#include <string>
#include <vector>

using namespace mg::magit::proclog;

TEST_CASE("record then snapshot preserves chronological order")
{
    clear();
    record('$', "git fetch origin", "", true, 5);
    record('~', "git add foo", "", true, 0);
    auto s = snapshot();
    REQUIRE(s.size() == 2);
    CHECK(s[0].command == "git fetch origin");
    CHECK(s[0].kind == '$');
    CHECK(s[1].command == "git add foo");
    CHECK(s[1].kind == '~');
}

TEST_CASE("ring buffer evicts the oldest past the cap of 200")
{
    clear();
    for (int i = 0; i < 250; ++i)
        record('$', "git op " + std::to_string(i), "", true, 0);
    auto s = snapshot();
    REQUIRE(s.size() == 200);
    CHECK(s.front().command == "git op 50");  // 0..49 evicted
    CHECK(s.back().command == "git op 249");
}

TEST_CASE("argv_to_command single-quotes args with spaces")
{
    std::vector<std::string> argv{"git", "commit", "-m", "fix the bug"};
    CHECK(argv_to_command(argv) == "git commit -m 'fix the bug'");
    std::vector<std::string> plain{"git", "add", "foo.c"};
    CHECK(argv_to_command(plain) == "git add foo.c");
}

TEST_CASE("argv_to_command escapes embedded single quotes (POSIX '\\'' idiom)")
{
    std::vector<std::string> argv{"git", "commit", "-m", "it's"};
    CHECK(argv_to_command(argv) == "git commit -m 'it'\\''s'");
}

TEST_CASE("argv_to_command single-quotes args containing a tab")
{
    std::vector<std::string> argv{"git", "grep", "a\tb"};
    CHECK(argv_to_command(argv) == "git grep 'a\tb'");
}

TEST_CASE("mg_magit_process_log streams entries newest-first, tagged")
{
    clear();
    record('~', "git add foo.c", "", true, 0);
    record('$', "git push origin HEAD", "(output shown in terminal)", true, 3100);

    struct row { std::string line; int kind; };
    std::vector<row> rows;
    int n = mg_magit_process_log(
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);

    REQUIRE(n == static_cast<int>(rows.size()));
    REQUIRE(rows.size() >= 3); // push CMD + push OUT + add CMD
    // Newest (push) first; '$' literal; output indented under it.
    CHECK(rows[0].kind == MG_LINE_PROCESS_CMD);
    CHECK(rows[0].line.rfind("$ git push origin HEAD", 0) == 0);
    CHECK(rows[0].line.find("3.1s") != std::string::npos);
    CHECK(rows[1].kind == MG_LINE_PROCESS_OUT);
    CHECK(rows[1].line.find("output shown in terminal") != std::string::npos);
    // Then the libgit2 add, rendered with the ≈ marker.
    CHECK(rows.back().kind == MG_LINE_PROCESS_CMD);
    CHECK(rows.back().line.find("git add foo.c") != std::string::npos);
    CHECK(rows.back().line.find("via libgit2") != std::string::npos);
}

TEST_CASE("mg_magit_process_log emits an empty-state line when nothing logged")
{
    clear();
    struct row { std::string line; int kind; };
    std::vector<row> rows;
    int n = mg_magit_process_log(
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);
    CHECK(n == 1);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].line.find("No git operations") != std::string::npos);
    CHECK(rows[0].kind == MG_LINE_OTHER); // spec: empty state is MG_LINE_OTHER
}
