// Integration test for the mg.io extern "C" bridge (task C3.5).
// Exercises mg_io_isdir end to end against real temp paths -- this is exactly
// what fileio.c's fisdir calls under ENABLE_CPP_UPGRADES.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "bridge.h"

namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_iobr_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}
} // namespace

TEST_CASE("mg_io_isdir returns 1 for a directory")
{
    auto dir = make_temp_dir();
    CHECK(mg_io_isdir(dir.string().c_str()) == 1);
    fs::remove_all(dir);
}

TEST_CASE("mg_io_isdir returns 0 for a regular file")
{
    auto dir = make_temp_dir();
    auto p = (dir / "f.txt").string();
    std::ofstream(p) << "x";
    CHECK(mg_io_isdir(p.c_str()) == 0);
    fs::remove_all(dir);
}

TEST_CASE("mg_io_isdir returns -1 for a missing path")
{
    auto dir = make_temp_dir();
    CHECK(mg_io_isdir((dir / "nope").string().c_str()) == -1);
    fs::remove_all(dir);
}

TEST_CASE("mg_io_isdir returns -1 for a NULL path")
{
    CHECK(mg_io_isdir(nullptr) == -1);
}
