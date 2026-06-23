// mg.fswatch -- OS-abstracted, event-driven filesystem watcher (task M2a,
// wakeable enhancement).
//
// One interface over two backends, selected at compile time:
//   * kqueue  on macOS / FreeBSD / OpenBSD / NetBSD
//   * inotify on Linux
// wait() blocks *indefinitely* until a watched path changes OR wake() is called
// from another thread -- no polling, no idle wake-ups. The wake primitive is the
// OS-native one (kqueue EVFILT_USER / inotify eventfd). The coroutine layer
// (M2b) and the background-thread bridge (M2d) build on top of this.

module;
#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#  include <filesystem>
#  include <unordered_map>
#endif

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#  include <poll.h>
#  include <sys/eventfd.h>
#  include <sys/inotify.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <array>
#  include <sys/event.h>
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
            queue_fd_ = std::exchange(other.queue_fd_, -1);
            wake_fd_  = std::exchange(other.wake_fd_, -1);
#if defined(__linux__)
            wd_path_  = std::move(other.wd_path_);
            ignores_  = std::move(other.ignores_);
            degraded_ = std::exchange(other.degraded_, false);
#else
            watch_fds_ = std::move(other.watch_fds_);
            paths_     = std::move(other.paths_);
#endif
        }
        return *this;
    }
    ~watcher() { close_all(); }

    // Open the queue and watch every root for write/delete/rename events.
    //
    // On Linux the roots are watched *recursively* (inotify is not recursive on
    // its own): every directory under each root is registered, the tree is kept
    // in sync as directories are created/removed, and a queue overflow triggers
    // a coarse resync. `ignores` lists path prefixes to skip (e.g. a repo's
    // `.git/objects`, which churns hugely and is irrelevant to status). On
    // kqueue (macOS) the roots are watched as given (non-recursive) and
    // `ignores` is unused -- see the FM-LINUX-FIRSTCLASS spec.
    static std::expected<watcher, watch_error>
    create(std::span<const std::string> roots,
           std::span<const std::string> ignores = {});

    // Block until a watched path changes or wake() is called. The returned
    // vector is empty when woken (no filesystem change).
    std::expected<std::vector<fs_event>, watch_error> wait();

    // Unblock a wait() running on another thread. Thread-safe; a wake issued
    // while no one is waiting is remembered and consumed by the next wait().
    void wake() noexcept;

    int fd() const noexcept { return queue_fd_; }

private:
    watcher() = default;

    // EVFILT_USER ident for wake() (kqueue). Its own filter space, so it never
    // collides with the per-path EVFILT_VNODE registrations. Unused on inotify.
    static constexpr std::uintptr_t kWakeIdent = 0;

    void close_all() noexcept
    {
#if !defined(__linux__)
        for (int f : watch_fds_) // kqueue: per-dir open fds
            if (f >= 0)
                ::close(f);
        watch_fds_.clear();
#endif
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        if (queue_fd_ >= 0) { // closing the inotify fd drops all its watches
            ::close(queue_fd_);
            queue_fd_ = -1;
        }
    }

    int queue_fd_ = -1; // kqueue fd, or inotify fd
    int wake_fd_  = -1; // inotify: eventfd; kqueue: unused (-1)
#if defined(__linux__)
    // Recursive inotify state: every watched directory's wd -> its path, the
    // ignore prefixes, and a degraded flag set if the kernel watch limit
    // (max_user_watches) is hit while building/extending the tree.
    std::unordered_map<int, std::string> wd_path_;
    std::vector<std::string> ignores_;
    bool degraded_ = false;
    void add_tree(const std::string &dir); // recursively watch dir + subdirs
    bool is_ignored(const std::string &path) const;
#else
    std::vector<int> watch_fds_;      // kqueue: per-dir open fds
    std::vector<std::string> paths_;  // parallel to watch_fds_
#endif
};

// Coroutine adapter: a lazy stream of fs_events. Loops until the stop_flag is
// requested; the owner cancels a *blocked* stream by request_stop() + w.wake().
// Takes the watcher by reference -- the owner (monitor) must outlive the stream.
mg::generator<fs_event> watch_stream(watcher &w, mg::stop_flag stop)
{
    while (!stop.stop_requested()) {
        auto events = w.wait();
        if (!events)
            co_return;
        for (auto &e : *events)
            co_yield e;
    }
}

#if defined(__linux__) // ---------------------------------------- inotify ----
// Recursive watcher: inotify is per-directory, not recursive, so we register
// every directory under each root and keep the set in sync as the tree changes.

bool watcher::is_ignored(const std::string &path) const
{
    for (const auto &ig : ignores_)
        if (path == ig ||
            (path.size() > ig.size() && path.compare(0, ig.size(), ig) == 0 &&
             path[ig.size()] == '/'))
            return true;
    return false;
}

// Register `dir` and, depth-first, every directory beneath it (skipping ignored
// subtrees). Idempotent: inotify returns the same wd for an already-watched
// dir, so re-adding after a create is harmless. Sets degraded_ on ENOSPC (the
// kernel's max_user_watches is exhausted) and stops deepening.
void watcher::add_tree(const std::string &dir)
{
    namespace fs = std::filesystem;
    // Directory events that can change git status: writes, attribute/mtime
    // touches, the full create/delete/move set, and the dir itself moving or
    // being deleted. IN_ONLYDIR keeps a watch from landing on a regular file.
    constexpr uint32_t kMask = IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE |
                               IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                               IN_MOVE_SELF;
    std::vector<std::string> stack{dir};
    while (!stack.empty()) {
        std::string d = std::move(stack.back());
        stack.pop_back();
        if (is_ignored(d))
            continue;

        int wd = ::inotify_add_watch(queue_fd_, d.c_str(), kMask | IN_ONLYDIR);
        if (wd < 0) {
            if (errno == ENOSPC || errno == EMFILE)
                degraded_ = true; // out of watches; function on what we have
            continue;             // unreadable/vanished dir: skip, keep going
        }
        wd_path_[wd] = d;

        std::error_code ec;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied,
                                       ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2) && !ec2)
                stack.push_back(it->path().string());
        }
    }
}

std::expected<watcher, watch_error>
watcher::create(std::span<const std::string> roots,
                std::span<const std::string> ignores)
{
    watcher w;
    w.queue_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"inotify_init1", errno});
    w.wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w.wake_fd_ < 0)
        return std::unexpected(watch_error{"eventfd", errno});

    w.ignores_.assign(ignores.begin(), ignores.end());
    for (const auto &r : roots)
        w.add_tree(r);
    if (w.wd_path_.empty()) // nothing watchable at all -> a real setup failure
        return std::unexpected(watch_error{"inotify_add_watch (no roots)", errno});
    return w;
}

void watcher::wake() noexcept
{
    uint64_t one = 1;
    [[maybe_unused]] ssize_t r = ::write(wake_fd_, &one, sizeof one);
}

std::expected<std::vector<fs_event>, watch_error>
watcher::wait()
{
    pollfd pfds[2] = {{queue_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
    int pr = ::poll(pfds, 2, -1); // block indefinitely
    if (pr < 0)
        return errno == EINTR ? std::expected<std::vector<fs_event>, watch_error>{}
                              : std::unexpected(watch_error{"poll", errno});

    std::vector<fs_event> out;
    if (pfds[1].revents & POLLIN) { // woken via wake()
        uint64_t drain;
        [[maybe_unused]] ssize_t r = ::read(wake_fd_, &drain, sizeof drain);
        return out;
    }
    if (!(pfds[0].revents & POLLIN))
        return out;

    // Drain every queued event (read until EAGAIN). The consumer recomputes the
    // whole status on any change, so we coalesce to a single coarse event but
    // still process each record to keep the watch tree in sync.
    bool changed = false;
    alignas(inotify_event) char buf[8192];
    for (;;) {
        ssize_t len = ::read(queue_fd_, buf, sizeof buf);
        if (len <= 0) {
            if (len < 0 && errno != EAGAIN && errno != EINTR)
                return std::unexpected(watch_error{"read", errno});
            break;
        }
        for (char *ptr = buf; ptr < buf + len;) {
            auto *ev = reinterpret_cast<inotify_event *>(ptr);
            changed = true;

            if (ev->mask & IN_Q_OVERFLOW) {
                // Lost events: the full recompute the consumer does is the
                // resync; nothing more to do but report a change.
            } else if ((ev->mask & IN_ISDIR) &&
                       (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                // New subdirectory: watch it (and rescan -- files may have
                // appeared between mkdir and our add).
                auto it = wd_path_.find(ev->wd);
                if (it != wd_path_.end() && ev->len > 0)
                    add_tree(it->second + "/" + ev->name);
            } else if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                wd_path_.erase(ev->wd); // watch gone; forget it
            }
            ptr += sizeof(inotify_event) + ev->len;
        }
    }

    if (changed)
        out.push_back(fs_event{wd_path_.empty() ? std::string{}
                                                : wd_path_.begin()->second});
    return out;
}

#else // ----------------------------------------------------------- kqueue ----

std::expected<watcher, watch_error>
watcher::create(std::span<const std::string> paths,
                std::span<const std::string> /*ignores: kqueue is non-recursive*/)
{
    watcher w;
    w.queue_fd_ = ::kqueue();
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"kqueue", errno});

    // A user-triggerable event so wake() can unblock a blocked wait().
    struct kevent uev;
    EV_SET(&uev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(w.queue_fd_, &uev, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(watch_error{"kevent EVFILT_USER", errno});

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

void watcher::wake() noexcept
{
    struct kevent uev;
    EV_SET(&uev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    ::kevent(queue_fd_, &uev, 1, nullptr, 0, nullptr);
}

std::expected<std::vector<fs_event>, watch_error>
watcher::wait()
{
    std::array<struct kevent, 16> evs;
    int n = ::kevent(queue_fd_, nullptr, 0, evs.data(),
                     static_cast<int>(evs.size()), nullptr); // block indefinitely
    if (n < 0)
        return errno == EINTR ? std::expected<std::vector<fs_event>, watch_error>{}
                              : std::unexpected(watch_error{"kevent wait", errno});

    std::vector<fs_event> out;
    std::vector<bool> seen(paths_.size(), false);
    for (int i = 0; i < n; ++i) {
        if (evs[i].filter == EVFILT_USER)
            continue; // woken via wake(), not a filesystem change
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
