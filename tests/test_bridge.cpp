// Integration test for the extern "C" magit bridge (task M2d-1).
// Exercises the C API end to end -- start the background monitor on a libgit2
// fixture repo, then read the published modeline through the bridge. No mg core.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
    CHECK(std::string(buf) == "git *1 ?1");

    mg_magit_stop();
    fs::remove_all(dir);
}
