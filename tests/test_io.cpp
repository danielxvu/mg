// Unit tests for the mg.io std::expected file-IO layer (task C3).
// Real files in a per-test temp dir; no mocks -- exercises the POSIX path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cerrno>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>

import mg.io;

namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_io_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}
} // namespace

TEST_CASE("write_file then read_file round-trips content")
{
    auto dir = make_temp_dir();
    auto p = (dir / "f.txt").string();

    REQUIRE(mg::io::write_file(p, "hello\nworld\n").has_value());
    auto r = mg::io::read_file(p);
    REQUIRE(r.has_value());
    CHECK(*r == "hello\nworld\n");
    fs::remove_all(dir);
}

TEST_CASE("read_file on a missing path fails with ENOENT")
{
    auto dir = make_temp_dir();
    auto r = mg::io::read_file((dir / "nope.txt").string());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ENOENT);
    fs::remove_all(dir);
}

TEST_CASE("read_file on a directory fails with EISDIR")
{
    auto dir = make_temp_dir();
    auto r = mg::io::read_file(dir.string());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == EISDIR);
    fs::remove_all(dir);
}

TEST_CASE("stat_file reports size, dir flag, and writability")
{
    auto dir = make_temp_dir();
    auto p = (dir / "f.txt").string();
    REQUIRE(mg::io::write_file(p, "12345").has_value());

    auto st = mg::io::stat_file(p);
    REQUIRE(st.has_value());
    CHECK(st->size == 5);
    CHECK_FALSE(st->is_dir);
    CHECK(st->writable);

    auto dst = mg::io::stat_file(dir.string());
    REQUIRE(dst.has_value());
    CHECK(dst->is_dir);
    fs::remove_all(dir);
}

TEST_CASE("stat_file on a missing path fails with ENOENT")
{
    auto dir = make_temp_dir();
    auto st = mg::io::stat_file((dir / "nope").string());
    REQUIRE_FALSE(st.has_value());
    CHECK(st.error().code == ENOENT);
    fs::remove_all(dir);
}

TEST_CASE("read_lines splits on newlines (no trailing empty line)")
{
    auto dir = make_temp_dir();
    auto p = (dir / "f.txt").string();
    REQUIRE(mg::io::write_file(p, "alpha\nbeta\ngamma\n").has_value());

    auto lines = mg::io::read_lines(p);
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() == 3);
    CHECK((*lines)[0] == "alpha");
    CHECK((*lines)[1] == "beta");
    CHECK((*lines)[2] == "gamma");
    fs::remove_all(dir);
}

TEST_CASE("read_lines propagates the underlying error (ENOENT)")
{
    auto dir = make_temp_dir();
    auto lines = mg::io::read_lines((dir / "nope.txt").string());
    REQUIRE_FALSE(lines.has_value());
    CHECK(lines.error().code == ENOENT);
    fs::remove_all(dir);
}

TEST_CASE("copy_file duplicates content via and_then")
{
    auto dir = make_temp_dir();
    auto src = (dir / "src.txt").string();
    auto dst = (dir / "dst.txt").string();
    REQUIRE(mg::io::write_file(src, "payload\n").has_value());

    REQUIRE(mg::io::copy_file(src, dst).has_value());
    auto r = mg::io::read_file(dst);
    REQUIRE(r.has_value());
    CHECK(*r == "payload\n");
    fs::remove_all(dir);
}

TEST_CASE("copy_file fails cleanly when the source is missing")
{
    auto dir = make_temp_dir();
    auto r = mg::io::copy_file((dir / "nope.txt").string(),
                               (dir / "dst.txt").string());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ENOENT);
    CHECK_FALSE(fs::exists(dir / "dst.txt")); // never created the destination
    fs::remove_all(dir);
}

TEST_CASE("or_else supplies a fallback on ENOENT")
{
    auto dir = make_temp_dir();
    auto r = mg::io::read_file((dir / "nope.txt").string())
                 .or_else([](mg::io::io_error e)
                              -> std::expected<std::string, mg::io::io_error> {
                     if (e.code == ENOENT)
                         return std::string("default");
                     return std::unexpected(e);
                 });
    REQUIRE(r.has_value());
    CHECK(*r == "default");
    fs::remove_all(dir);
}
