// mg.coro -- a minimal, generic, move-only generator<T> coroutine (task M2b).
//
// clang-21's libc++ does not yet ship C++23 <generator>, so we own a small one.
// Lazy (suspends at start and after each co_yield); RAII over the coroutine
// handle. Designed for single-consumer pull on a background thread, so an
// escaped exception calls std::terminate rather than crossing the boundary.

module;
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>

export module mg.coro;

export namespace mg {

// Minimal, copyable, header-only cooperative cancellation flag. Used instead of
// std::stop_token, whose <stop_token> inline symbols fail to emit through a
// module boundary on clang-21's libc++ (verified). Copies share one atomic, so
// a request_stop() on any copy is seen by all -- same contract as stop_token.
class stop_flag {
public:
    void request_stop() noexcept { flag_->store(true, std::memory_order_relaxed); }
    bool stop_requested() const noexcept
    {
        return flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_ =
        std::make_shared<std::atomic<bool>>(false);
};

template <class T>
class generator {
public:
    struct promise_type {
        T value_;

        generator get_return_object()
        {
            return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T v) noexcept
        {
            value_ = std::move(v);
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    generator() noexcept = default;
    generator(generator &&o) noexcept : h_(std::exchange(o.h_, {})) {}
    generator &operator=(generator &&o) noexcept
    {
        if (this != &o) {
            if (h_)
                h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    generator(const generator &) = delete;
    generator &operator=(const generator &) = delete;
    ~generator()
    {
        if (h_)
            h_.destroy();
    }

    // Single-pass input iterator. Holds a non-owning copy of the handle; a null
    // handle is the end sentinel.
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;

        iterator() noexcept = default;
        explicit iterator(handle_type h) noexcept : h_(h) {}

        iterator &operator++()
        {
            h_.resume();
            if (h_.done())
                h_ = {};
            return *this;
        }
        void operator++(int) { ++*this; }

        const T &operator*() const { return h_.promise().value_; }

        bool operator==(const iterator &o) const noexcept { return h_ == o.h_; }

    private:
        handle_type h_{};
    };

    iterator begin()
    {
        if (h_) {
            h_.resume();
            if (h_.done())
                return {};
        }
        return iterator{h_};
    }
    iterator end() noexcept { return {}; }

private:
    explicit generator(handle_type h) noexcept : h_(h) {}

    handle_type h_{};
};

} // namespace mg
