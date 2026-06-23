// Unit tests for the mg.fswatch OS-abstracted filesystem watcher (task M2a).
// Both backends (kqueue on macOS, inotify on Linux) are recursive and arm
// synchronously, so these run identically on both platforms. Uses real temp
// dirs via mkdtemp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
    // The event is queued before wait(), so wait() returns without blocking.
    { std::ofstream(dir / "newfile.txt") << "x"; }

    auto evs = w->wait();
    REQUIRE(evs.has_value());
    REQUIRE(evs->size() >= 1);
    CHECK((*evs)[0].path == dir.string());

    fs::remove_all(dir);
}

TEST_CASE("watcher::wake unblocks a blocked wait")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> paths{dir.string()};
    auto w = watcher::create(paths);
    REQUIRE(w.has_value());

    // wait() on another thread blocks indefinitely (no change, no timeout);
    // wake() from this thread must release it, so join() returns.
    std::thread blocked([&] { (void)w->wait(); });
    w->wake();
    blocked.join();        // only returns if wait() unblocked
    CHECK(true);

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
    auto stream = watch_stream(*w, stop); // watcher passed by reference
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
    auto stream = watch_stream(*w, stop); // watcher passed by reference

    { std::ofstream(dir / "f.txt") << "x"; }

    auto it = stream.begin();
    REQUIRE(it != stream.end());
    CHECK((*it).path == dir.string());

    fs::remove_all(dir);
}

// ---- recursive watching (FM-LINUX-FIRSTCLASS) -------------------------------
// Both backends are recursive: inotify registers every dir under each root;
// kqueue opens an fd per dir. These run on both platforms (synchronous arming
// on both, so "touch then wait" is race-free).

// A deep edit under a pre-existing nested directory must fire.
TEST_CASE("watcher watches pre-existing nested directories recursively")
{
    auto dir = make_temp_dir();
    fs::create_directories(dir / "a" / "b" / "c");
    std::array<std::string, 1> roots{dir.string()};
    auto w = watcher::create(roots);
    REQUIRE(w.has_value());

    { std::ofstream(dir / "a" / "b" / "c" / "deep.txt") << "x"; }

    auto evs = w->wait();
    REQUIRE(evs.has_value());
    CHECK(evs->size() >= 1);

    fs::remove_all(dir);
}

// A directory created *after* watching must be picked up dynamically, so a file
// later created inside it fires too.
TEST_CASE("watcher dynamically watches directories created after create()")
{
    auto dir = make_temp_dir();
    std::array<std::string, 1> roots{dir.string()};
    auto w = watcher::create(roots);
    REQUIRE(w.has_value());

    fs::create_directory(dir / "fresh"); // fires IN_CREATE|IN_ISDIR on root
    auto first = w->wait();              // processing it watches "fresh"
    REQUIRE(first.has_value());

    { std::ofstream(dir / "fresh" / "f.txt") << "x"; } // inside the new dir
    auto second = w->wait();
    REQUIRE(second.has_value());
    CHECK(second->size() >= 1); // only fires if "fresh" got watched

    fs::remove_all(dir);
}

// A change under an ignored prefix must not wake the watcher: the ignored dir
// is never registered, so the only thing that releases wait() is the wake().
TEST_CASE("watcher skips ignored subtrees")
{
    auto dir = make_temp_dir();
    fs::create_directory(dir / "ig");
    std::array<std::string, 1> roots{dir.string()};
    std::array<std::string, 1> ignores{(dir / "ig").string()};
    auto w = watcher::create(roots, ignores);
    REQUIRE(w.has_value());

    std::thread t([&] {
        { std::ofstream(dir / "ig" / "f.txt") << "x"; } // under the ignored dir
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        w->wake();
    });
    auto evs = w->wait();
    t.join();

    REQUIRE(evs.has_value());
    CHECK(evs->empty()); // woken only; the ignored change produced no event

    fs::remove_all(dir);
}

// The optional ignore predicate is consulted per directory (git-agnostic here:
// a synthetic predicate). A watched, non-matching dir still fires; a matching
// one is skipped entirely. (The monitor backs this with libgit2's gitignore
// check so node_modules/build trees aren't watched.)
TEST_CASE("watcher honors a custom ignore predicate")
{
    auto dir = make_temp_dir();
    fs::create_directory(dir / "skipme");
    fs::create_directory(dir / "keep");
    std::array<std::string, 1> roots{dir.string()};
    auto pred = [](const std::string &p) {
        return p.find("/skipme") != std::string::npos;
    };
    auto w = watcher::create(roots, {}, pred);
    REQUIRE(w.has_value());

    // A change under the non-matching dir fires.
    { std::ofstream(dir / "keep" / "f.txt") << "x"; }
    auto kept = w->wait();
    REQUIRE(kept.has_value());
    CHECK(kept->size() >= 1);

    // A change under the predicate-matched dir does not (woken only).
    std::thread t([&] {
        { std::ofstream(dir / "skipme" / "f.txt") << "x"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        w->wake();
    });
    auto skipped = w->wait();
    t.join();
    REQUIRE(skipped.has_value());
    CHECK(skipped->empty());

    fs::remove_all(dir);
}

// wait() returns the *set* of distinct changed dirs (not a single coarse event)
// so the consumer can scope an incremental status to exactly them.
TEST_CASE("watcher reports each distinct changed directory")
{
    auto dir = make_temp_dir();
    fs::create_directory(dir / "d1");
    fs::create_directory(dir / "d2");
    std::array<std::string, 1> roots{dir.string()};
    auto w = watcher::create(roots);
    REQUIRE(w.has_value());

    { std::ofstream(dir / "d1" / "f.txt") << "x"; }
    { std::ofstream(dir / "d2" / "g.txt") << "y"; }

    auto evs = w->wait();
    REQUIRE(evs.has_value());
    auto has = [&](const fs::path &p) {
        return std::any_of(evs->begin(), evs->end(),
                           [&](const auto &e) { return e.path == p.string(); });
    };
    CHECK(has(dir / "d1"));
    CHECK(has(dir / "d2"));
    CHECK(evs->size() >= 2);

    fs::remove_all(dir);
}
