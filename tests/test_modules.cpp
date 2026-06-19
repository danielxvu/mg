// Verifies the C++20 *named module* build pipeline end-to-end (task F3).
//
// This is the architectural bet the whole refactor rests on: that the build
// can compile an `export module`, scan its import graph (P1689 via
// clang-scan-deps), produce a BMI, and resolve an `import` from another TU.
// Apple Clang cannot do this; the MacPorts clang-21 toolchain can. If this
// goes red, no real module (mg.magit, mg.text, …) can be built.
//
// Note the mix of a classic header (`#include <doctest>`) and a named module
// (`import mg.probe`) in one TU -- that interop is exactly what lets us adopt
// modules incrementally while keeping the doctest harness.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

import mg.probe;

TEST_CASE("a C++20 named module compiles, imports, and resolves its exports")
{
    CHECK(mg::probe::answer() == 42);
}
