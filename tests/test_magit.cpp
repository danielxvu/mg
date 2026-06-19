// Unit tests for the mg.magit working-tree status parser (task M1).
// Fixtures are real `git status --porcelain=v1` line shapes, hardcoded for
// determinism (no live git invocation -- that is M2's job).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

import mg.magit;

using namespace mg::magit;

TEST_CASE("parse_status_line: unstaged modified ' M path'")
{
    auto r = parse_status_line(" M src/foo.c");
    REQUIRE(r.has_value());
    CHECK(r->index == status::unmodified);
    CHECK(r->worktree == status::modified);
    CHECK(r->path == "src/foo.c");
    CHECK_FALSE(r->orig_path.has_value());
}

// Same two-column rule as slice 1, exercised across several XY codes. These
// are data-coverage for the generic parser, not new behaviour.
TEST_CASE("parse_status_line: assorted XY codes")
{
    SUBCASE("staged + unstaged modified 'MM'") {
        auto r = parse_status_line("MM src/baz.c");
        REQUIRE(r.has_value());
        CHECK(r->index == status::modified);
        CHECK(r->worktree == status::modified);
        CHECK(r->path == "src/baz.c");
    }
    SUBCASE("staged add 'A '") {
        auto r = parse_status_line("A  newfile");
        REQUIRE(r.has_value());
        CHECK(r->index == status::added);
        CHECK(r->worktree == status::unmodified);
        CHECK(r->path == "newfile");
    }
    SUBCASE("untracked '?\?'") {
        auto r = parse_status_line("?? new.txt");
        REQUIRE(r.has_value());
        CHECK(r->index == status::untracked);
        CHECK(r->worktree == status::untracked);
        CHECK(r->path == "new.txt");
    }
}

TEST_CASE("parse_status_line: rename sets orig_path ('R  old -> new')")
{
    auto r = parse_status_line("R  old.c -> new.c");
    REQUIRE(r.has_value());
    CHECK(r->index == status::renamed);
    CHECK(r->worktree == status::unmodified);
    CHECK(r->path == "new.c");
    REQUIRE(r->orig_path.has_value());
    CHECK(r->orig_path.value() == "old.c");
}

TEST_CASE("parse_status: multi-line porcelain into a vector")
{
    auto r = parse_status("M  a.c\n?? b.txt\n");
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    CHECK((*r)[0].index == status::modified);
    CHECK((*r)[0].path == "a.c");
    CHECK((*r)[1].index == status::untracked);
    CHECK((*r)[1].path == "b.txt");
}

TEST_CASE("parse_status_line: malformed input yields a parse_error")
{
    SUBCASE("too short to hold XY + path") {
        auto r = parse_status_line("M");
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().line == "M");
    }
    SUBCASE("missing path after the code") {
        auto r = parse_status_line("MM ");
        REQUIRE_FALSE(r.has_value());
    }
    SUBCASE("unknown status code") {
        auto r = parse_status_line("ZZ file");
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("parse_status: propagates the first malformed line as an error")
{
    auto r = parse_status("M  a.c\nZZ bad\n");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().line == "ZZ bad");
}
