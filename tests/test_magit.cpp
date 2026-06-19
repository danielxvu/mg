// Unit tests for mg.magit domain types + modeline summary (task M2c-1).
// The porcelain text parser was retired when git access moved to libgit2.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>

import mg.magit;

using namespace mg::magit;

TEST_CASE("summarize: a clean tree")
{
    std::array<file_status, 0> none;
    CHECK(summarize(none) == "git clean");
}

TEST_CASE("summarize: counts staged, unstaged, and untracked")
{
    SUBCASE("one staged change")
    {
        std::array<file_status, 1> e{
            file_status{status::modified, status::unmodified, "a", {}}};
        CHECK(summarize(e) == "git *1");
    }
    SUBCASE("one unstaged change")
    {
        std::array<file_status, 1> e{
            file_status{status::unmodified, status::modified, "a", {}}};
        CHECK(summarize(e) == "git +1");
    }
    SUBCASE("one untracked file")
    {
        std::array<file_status, 1> e{
            file_status{status::untracked, status::untracked, "a", {}}};
        CHECK(summarize(e) == "git ?1");
    }
    SUBCASE("mixed: 2 staged, 1 unstaged, 3 untracked")
    {
        std::array<file_status, 6> e{
            file_status{status::added, status::unmodified, "s1", {}},
            file_status{status::modified, status::unmodified, "s2", {}},
            file_status{status::unmodified, status::modified, "u1", {}},
            file_status{status::untracked, status::untracked, "q1", {}},
            file_status{status::untracked, status::untracked, "q2", {}},
            file_status{status::untracked, status::untracked, "q3", {}},
        };
        CHECK(summarize(e) == "git *2 +1 ?3");
    }
}
