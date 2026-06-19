// Sanity test for the modern-C++ refactor harness.
//
// This is intentionally not a `1 == 1` smoke test. The entire architecture
// (see todo.md / §3 monadic error handling) is staked on the C++23 standard
// library being present and functional in the build toolchain. So the sanity
// check verifies the two foundational promises at once:
//
//   1. The doctest unit-test harness compiles, links, and runs under CTest.
//   2. `std::expected` (C++23) is available and behaves monadically.
//
// If this ever goes red, the foundation is broken and no module work can rely
// on `std::expected`/`.and_then()` error pipelines.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <expected>
#include <string>

namespace {

// A toy monadic pipeline mirroring how real modules will propagate errors:
// no integer return codes, no exceptions -- just `std::expected` chaining.
std::expected<int, std::string> parse_positive(int n)
{
    if (n <= 0)
        return std::unexpected("not positive");
    return n;
}

} // namespace

TEST_CASE("doctest harness runs and reports assertions")
{
    CHECK(true);
    CHECK(2 + 2 == 4);
}

TEST_CASE("C++23 std::expected is available and chains monadically")
{
    auto doubled = parse_positive(21).and_then(
        [](int v) -> std::expected<int, std::string> { return v * 2; });

    REQUIRE(doubled.has_value());
    CHECK(doubled.value() == 42);

    auto failed = parse_positive(-1).and_then(
        [](int v) -> std::expected<int, std::string> { return v * 2; });

    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error() == "not positive");
}
