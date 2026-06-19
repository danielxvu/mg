// Unit tests for the mg.fswatch OS-abstracted filesystem watcher (task M2a).
// Exercised here against the kqueue backend (macOS); the inotify backend mirrors
// it and is covered by Linux CI. Uses real temp dirs via mkdtemp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

import mg.coro;
import mg.fswatch;

using namespace mg::fswatch;
namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_fswatch_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}
} // namespace

TEST_CASE("watcher::create succeeds on an existing directory")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};

    auto w = watcher::create(paths);
    REQUIRE(w.has_value());
    CHECK(w->fd() >= 0);

    fs::remove_all(dir);
}

TEST_CASE("watcher::wait reports an event when a watched directory changes")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};
    auto w = watcher::create(paths);
    REQUIRE(w.has_value());

    // Mutate the watched directory by adding a file (fires NOTE_WRITE/IN_CREATE).
    { std::ofstream(dir / "newfile.txt") << "x"; }

    auto evs = w->wait(std::chrono::seconds(2));
    REQUIRE(evs.has_value());
    REQUIRE(evs->size() >= 1);
    CHECK((*evs)[0].path == dir.string());

    fs::remove_all(dir);
}

TEST_CASE("watcher::wait times out to an empty list when nothing changes")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};
    auto w = watcher::create(paths);
    REQUIRE(w.has_value());

    auto evs = w->wait(std::chrono::milliseconds(50));
    REQUIRE(evs.has_value());       // timeout is success, not error
    CHECK(evs->empty());

    fs::remove_all(dir);
}

TEST_CASE("watcher::create fails on a nonexistent path")
{
    std::array<std::string, 1> paths{"/nonexistent/path/xyzzy"};
    auto w = watcher::create(paths);
    REQUIRE_FALSE(w.has_value());
    CHECK(w.error().err == ENOENT);
}

TEST_CASE("watch_stream over a pre-stopped token is an empty stream")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};
    auto w = watcher::create(paths);
    REQUIRE(w.has_value());

    mg::stop_flag stop;
    stop.request_stop(); // stop before any iteration
    auto stream = watch_stream(std::move(*w), stop,
                               std::chrono::milliseconds(500));
    CHECK(stream.begin() == stream.end()); // zero iterations

    fs::remove_all(dir);
}

TEST_CASE("watch_stream yields an event when the watched dir changes")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};
    auto w = watcher::create(paths);
    REQUIRE(w.has_value());

    mg::stop_flag stop;
    auto stream = watch_stream(std::move(*w), stop,
                               std::chrono::seconds(2));

    { std::ofstream(dir / "f.txt") << "x"; }

    auto it = stream.begin();
    REQUIRE(it != stream.end());
    CHECK((*it).path == dir.string());

    fs::remove_all(dir);
}
