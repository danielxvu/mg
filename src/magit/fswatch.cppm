// mg.fswatch -- OS-abstracted, event-driven filesystem watcher (task M2a).
//
// One interface over two backends, selected at compile time:
//   * kqueue  on macOS / FreeBSD / OpenBSD / NetBSD
//   * inotify on Linux
// Synchronous resource: blocking wait(timeout). The coroutine layer (M2b) and
// the background-thread bridge (M2d) build on top of this. Greenfield -- no
// coupling to mg's C core.

module;
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#  include <poll.h>
#  include <sys/inotify.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <array>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <sys/types.h>
#else
#  error "mg.fswatch: unsupported platform (needs kqueue or inotify)"
#endif

export module mg.fswatch;

import mg.coro;

export namespace mg::fswatch {

// Which watched path fired. Events coalesce to one per path per wait().
struct fs_event {
    std::string path;
};

struct watch_error {
    std::string message;
    int err;            // errno at the point of failure
};

// RAII, move-only owner of the OS watch queue and per-path descriptors.
class watcher {
public:
    watcher(const watcher &) = delete;
    watcher &operator=(const watcher &) = delete;

    watcher(watcher &&other) noexcept { *this = std::move(other); }
    watcher &operator=(watcher &&other) noexcept
    {
        if (this != &other) {
            close_all();
            queue_fd_  = std::exchange(other.queue_fd_, -1);
            watch_fds_ = std::move(other.watch_fds_);
            paths_     = std::move(other.paths_);
        }
        return *this;
    }
    ~watcher() { close_all(); }

    // Open the queue and register every path for write/delete/rename events.
    static std::expected<watcher, watch_error>
    create(std::span<const std::string> paths);

    // Block up to `timeout`; return the paths that fired (empty on timeout).
    std::expected<std::vector<fs_event>, watch_error>
    wait(std::chrono::milliseconds timeout);

    int fd() const noexcept { return queue_fd_; }

private:
    watcher() = default;

    void close_all() noexcept
    {
        for (int f : watch_fds_)
            if (f >= 0)
                ::close(f);
        watch_fds_.clear();
        if (queue_fd_ >= 0) {
            ::close(queue_fd_);
            queue_fd_ = -1;
        }
    }

    int queue_fd_ = -1;               // kqueue fd, or inotify fd
    std::vector<int> watch_fds_;      // kqueue: per-path open fds; inotify: wds
    std::vector<std::string> paths_;  // parallel to watch_fds_
};

// Coroutine adapter: a lazy stream of fs_events driven by the watcher. Loops
// until the stop_token is requested; the watcher's `timeout` bounds how quickly
// a stop is noticed. A watcher error ends the stream.
mg::generator<fs_event>
watch_stream(watcher w, mg::stop_flag stop, std::chrono::milliseconds timeout)
{
    while (!stop.stop_requested()) {
        auto events = w.wait(timeout);
        if (!events)
            co_return;
        for (auto &e : *events)
            co_yield e;
    }
}

#if defined(__linux__) // ---------------------------------------- inotify ----
// NOTE: written to mirror the kqueue backend; exercised only on Linux CI.

std::expected<watcher, watch_error>
watcher::create(std::span<const std::string> paths)
{
    watcher w;
    w.queue_fd_ = ::inotify_init1(IN_NONBLOCK);
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"inotify_init1", errno});

    for (const auto &p : paths) {
        int wd = ::inotify_add_watch(w.queue_fd_, p.c_str(),
                                     IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE);
        if (wd < 0)
            return std::unexpected(watch_error{"inotify_add_watch: " + p, errno});
        w.watch_fds_.push_back(wd);
        w.paths_.push_back(p);
    }
    return w;
}

std::expected<std::vector<fs_event>, watch_error>
watcher::wait(std::chrono::milliseconds timeout)
{
    pollfd pfd{queue_fd_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (pr < 0)
        return errno == EINTR ? std::expected<std::vector<fs_event>, watch_error>{}
                              : std::unexpected(watch_error{"poll", errno});
    std::vector<fs_event> out;
    if (pr == 0)
        return out; // timeout

    alignas(inotify_event) char buf[4096];
    ssize_t len = ::read(queue_fd_, buf, sizeof buf);
    if (len < 0)
        return (errno == EAGAIN || errno == EINTR)
                   ? out
                   : std::unexpected(watch_error{"read", errno});

    std::vector<bool> seen(paths_.size(), false);
    for (char *ptr = buf; ptr < buf + len;) {
        auto *ev = reinterpret_cast<inotify_event *>(ptr);
        for (std::size_t i = 0; i < watch_fds_.size(); ++i)
            if (watch_fds_[i] == ev->wd && !seen[i]) {
                seen[i] = true;
                out.push_back(fs_event{paths_[i]});
            }
        ptr += sizeof(inotify_event) + ev->len;
    }
    return out;
}

#else // ----------------------------------------------------------- kqueue ----

std::expected<watcher, watch_error>
watcher::create(std::span<const std::string> paths)
{
    watcher w;
    w.queue_fd_ = ::kqueue();
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"kqueue", errno});

    for (const auto &p : paths) {
        int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0)
            return std::unexpected(watch_error{"open: " + p, errno});

        struct kevent kev;
        EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND, 0,
               reinterpret_cast<void *>(static_cast<std::intptr_t>(w.paths_.size())));
        if (::kevent(w.queue_fd_, &kev, 1, nullptr, 0, nullptr) < 0) {
            ::close(fd);
            return std::unexpected(watch_error{"kevent register: " + p, errno});
        }
        w.watch_fds_.push_back(fd);
        w.paths_.push_back(p);
    }
    return w;
}

std::expected<std::vector<fs_event>, watch_error>
watcher::wait(std::chrono::milliseconds timeout)
{
    timespec ts;
    ts.tv_sec  = timeout.count() / 1000;
    ts.tv_nsec = (timeout.count() % 1000) * 1'000'000;

    std::array<struct kevent, 16> evs;
    int n = ::kevent(queue_fd_, nullptr, 0, evs.data(),
                     static_cast<int>(evs.size()), &ts);
    if (n < 0)
        return errno == EINTR ? std::expected<std::vector<fs_event>, watch_error>{}
                              : std::unexpected(watch_error{"kevent wait", errno});

    std::vector<fs_event> out;
    std::vector<bool> seen(paths_.size(), false);
    for (int i = 0; i < n; ++i) {
        auto idx = static_cast<std::size_t>(
            reinterpret_cast<std::intptr_t>(evs[i].udata));
        if (idx < paths_.size() && !seen[idx]) {
            seen[idx] = true;
            out.push_back(fs_event{paths_[idx]});
        }
    }
    return out;
}

#endif

} // namespace mg::fswatch
