// Unit tests for the hand-rolled generic generator<T> coroutine (task M2b).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <coroutine>   // std::coroutine_traits must be visible to DEFINE a coroutine,
                       // even though generator<T> itself comes from the module import
#include <vector>

import mg.coro;

namespace {
mg::generator<int> count_to(int n)
{
    for (int i = 0; i < n; ++i)
        co_yield i;
}

mg::generator<int> naturals()
{
    for (int i = 0;; ++i)
        co_yield i;
}
} // namespace

TEST_CASE("generator yields a finite sequence in order")
{
    std::vector<int> got;
    for (int x : count_to(3))
        got.push_back(x);
    CHECK(got == std::vector<int>{0, 1, 2});
}

TEST_CASE("an infinite generator is partially consumed and safely destroyed")
{
    auto g = naturals();
    auto it = g.begin();
    CHECK(*it == 0);
    ++it;
    CHECK(*it == 1);
    // g (infinite, suspended) is destroyed here: handle.destroy() unwinds it.
}

TEST_CASE("stop_flag: copies share state")
{
    mg::stop_flag a;
    mg::stop_flag b = a;           // shares the same atomic
    CHECK_FALSE(a.stop_requested());
    b.request_stop();
    CHECK(a.stop_requested());     // request on a copy is visible everywhere
}
