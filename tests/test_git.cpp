// Integration tests for the mg.git libgit2 status reader (task M2c-2).
// The fixture repo is built with libgit2 itself -- no shell git invoked.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <git2.h>

import mg.magit;
import mg.git;

namespace fs = std::filesystem;
using namespace mg::magit;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_git_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}

// A repo with one staged file and one untracked file, created via libgit2.
fs::path make_repo_with_changes()
{
    auto dir = make_temp_dir();
    git_libgit2_init();

    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "staged.txt") << "hello";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);

    std::ofstream(dir / "untracked.txt") << "world"; // left unstaged

    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
} // namespace

TEST_CASE("repo_status reports a staged and an untracked entry")
{
    auto dir = make_repo_with_changes();

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    REQUIRE(st->size() == 2);

    bool saw_staged = false, saw_untracked = false;
    for (const auto &e : *st) {
        if (e.path == "staged.txt") {
            CHECK(e.index == status::added);
            saw_staged = true;
        } else if (e.path == "untracked.txt") {
            CHECK(e.worktree == status::untracked);
            saw_untracked = true;
        }
    }
    CHECK(saw_staged);
    CHECK(saw_untracked);

    fs::remove_all(dir);
}

TEST_CASE("repo_status fails on a path that is not a git repository")
{
    auto dir = make_temp_dir(); // empty dir, no .git
    auto st = mg::git::repo_status(dir.string());
    CHECK_FALSE(st.has_value());
    fs::remove_all(dir);
}
