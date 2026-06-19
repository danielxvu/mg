// Integration test for the extern "C" magit bridge (task M2d-1).
// Exercises the C API end to end -- start the background monitor on a libgit2
// fixture repo, then read the published modeline through the bridge. No mg core.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <git2.h>

#include "bridge.h"

namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_bridge_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}

// One staged file + one untracked file (built with libgit2, no shell git).
fs::path make_repo_with_changes()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "staged.txt") << "hi";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);

    std::ofstream(dir / "untracked.txt") << "yo";
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

// A repo with one commit, then a staged change and an untracked file.
fs::path make_repo_full()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "base.txt") << "base";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "base.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid coid;
    REQUIRE(git_commit_create(&coid, repo, "HEAD", sig, sig, nullptr,
                              "initial commit", tree, 0, nullptr) == 0);
    git_signature_free(sig);
    git_tree_free(tree);

    std::ofstream(dir / "staged.txt") << "s";
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    std::ofstream(dir / "untracked.txt") << "u";

    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

bool any_line_has(const std::vector<std::string> &lines, const std::string &needle)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) {
        return l.find(needle) != std::string::npos;
    });
}
} // namespace

TEST_CASE("bridge publishes a summarized modeline for a repo")
{
    auto dir = make_repo_with_changes();

    mg_magit_start(dir.string().c_str());

    // The monitor publishes asynchronously on its thread; poll with a bound.
    bool dirty = false;
    for (int i = 0; i < 500 && !dirty; ++i) {
        if (mg_magit_take_dirty())
            dirty = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(dirty);

    char buf[128] = {0};
    int len = mg_magit_modeline(buf, sizeof buf);
    CHECK(len > 0);

    // Modeline is "<branch> git *1 ?1": a non-empty branch, then the status.
    std::string ml(buf);
    auto pos = ml.find(" git ");
    REQUIRE(pos != std::string::npos);
    CHECK(pos > 0);                            // non-empty branch prefix
    CHECK(ml.substr(pos + 1) == "git *1 ?1");  // status part unchanged

    mg_magit_stop();
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_status_buffer composes branch, sections, and commits")
{
    auto dir = make_repo_full();

    std::vector<std::string> lines;
    int count = mg_magit_status_buffer(
        dir.string().c_str(),
        [](void *ctx, const char *line) {
            static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
        },
        &lines);

    CHECK(count == static_cast<int>(lines.size()));
    CHECK(any_line_has(lines, "On branch "));
    CHECK(any_line_has(lines, "initial commit"));     // HEAD summary + commit list
    CHECK(any_line_has(lines, "Untracked files (1)"));
    CHECK(any_line_has(lines, "untracked.txt"));
    CHECK(any_line_has(lines, "Staged changes (1)"));
    CHECK(any_line_has(lines, "staged.txt"));
    CHECK(any_line_has(lines, "Recent commits"));

    fs::remove_all(dir);
}
