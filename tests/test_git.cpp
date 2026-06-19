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

// A repo with a single commit on HEAD (built with libgit2).
fs::path make_repo_with_commit(const char *message)
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "a.txt") << "content";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);

    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);

    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "Test", "t@example.com") == 0);

    git_oid commit_oid;
    REQUIRE(git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                              message, tree, 0, nullptr) == 0);

    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
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

TEST_CASE("read_head returns the branch and HEAD commit")
{
    auto dir = make_repo_with_commit("first commit\n\nbody text");
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK_FALSE(h->branch.empty());     // e.g. "master"
    CHECK(h->short_oid.size() == 8);
    CHECK(h->summary == "first commit"); // summary is the first line
    fs::remove_all(dir);
}

TEST_CASE("recent_commits returns commits, newest first")
{
    auto dir = make_repo_with_commit("only commit");
    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c.has_value());
    REQUIRE(c->size() == 1);
    CHECK((*c)[0].summary == "only commit");
    CHECK((*c)[0].short_oid.size() == 8);
    fs::remove_all(dir);
}

TEST_CASE("recent_commits on an unborn repo is empty (not an error)")
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);
    git_libgit2_shutdown();

    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c.has_value());
    CHECK(c->empty());
    fs::remove_all(dir);
}

TEST_CASE("stage() moves an untracked file into the index")
{
    auto dir = make_repo_with_changes(); // untracked.txt is untracked
    REQUIRE(mg::git::stage(dir.string(), "untracked.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    bool staged = false;
    for (const auto &e : *st)
        if (e.path == "untracked.txt")
            staged = (e.index == status::added);
    CHECK(staged);
    fs::remove_all(dir);
}

TEST_CASE("unstage() drops a staged-new file back to untracked")
{
    auto dir = make_repo_with_changes(); // staged.txt is a staged new file (unborn)
    REQUIRE(mg::git::unstage(dir.string(), "staged.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    bool untracked = false;
    for (const auto &e : *st)
        if (e.path == "staged.txt")
            untracked = (e.worktree == status::untracked);
    CHECK(untracked);
    fs::remove_all(dir);
}

TEST_CASE("unstage() resets a staged modification to HEAD")
{
    auto dir = make_repo_with_commit("base"); // a.txt committed with "content"

    // Modify a.txt and stage the modification.
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    std::ofstream(dir / "a.txt") << "changed";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();

    REQUIRE(mg::git::unstage(dir.string(), "a.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        if (e.path == "a.txt") {
            CHECK(e.index == status::unmodified);  // unstaged from the index
            CHECK(e.worktree == status::modified);  // still changed on disk
        }
    fs::remove_all(dir);
}
