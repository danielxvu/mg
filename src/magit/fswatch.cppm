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
#include <filesystem>      // recursive tree walk (both backends)
#include <functional>      // ignore predicate (git-aware, supplied by caller)
#include <span>
#include <string>
#include <unordered_map>   // wd/fd -> dir path
#include <unordered_set>   // kqueue: dedup re-adds
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#  include <poll.h>
#  include <sys/eventfd.h>
#  include <sys/inotify.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <array>
#  include <sys/event.h>
#  include <sys/resource.h> // raise RLIMIT_NOFILE for the per-dir watch fds
#  include <sys/types.h>
#else
#  error "mg.fswatch: unsupported platform (needs kqueue or inotify)"
#endif

export module mg.fswatch;

import mg.coro;

export namespace mg::fswatch {

// A directory that changed since the last wait(). One wait() returns the set of
// distinct changed dirs, so the consumer can scope an incremental status to
// them. A single event with an EMPTY path is the "resync" marker (e.g. inotify
// queue overflow): the consumer should do a full rescan. See also degraded().
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
            ignores_     = std::move(other.ignores_);
            ignore_pred_ = std::move(other.ignore_pred_);
            degraded_    = std::exchange(other.degraded_, false);
#if defined(__linux__)
            wd_path_  = std::move(other.wd_path_);
#else
            fd_path_  = std::move(other.fd_path_);
            watched_  = std::move(other.watched_);
#endif
        }
        return *this;
    }
    ~watcher() { close_all(); }

    // Open the queue and watch every root *recursively* for write/delete/rename
    // events. Both backends register every directory under each root (inotify
    // per-wd, kqueue per-fd) and keep the set in sync as dirs are created or
    // removed.
    //
    // `ignores` lists path prefixes to skip (e.g. a repo's `.git/objects`, which
    // churns hugely and is irrelevant to status). `ignore_pred`, if set, is also
    // consulted per directory: the walk skips any directory for which it returns
    // true. The monitor supplies a git-aware predicate so gitignored trees
    // (node_modules, build/, …) -- which cannot affect `git status` and would
    // otherwise dominate the watch set -- are never watched. It is a plain
    // std::function so mg.fswatch stays git-agnostic.
    static std::expected<watcher, watch_error>
    create(std::span<const std::string> roots,
           std::span<const std::string> ignores = {},
           std::function<bool(const std::string &)> ignore_pred = {});

    // Block until a watched path changes or wake() is called. The returned
    // vector is empty when woken (no filesystem change).
    std::expected<std::vector<fs_event>, watch_error> wait();

    // Unblock a wait() running on another thread. Thread-safe; a wake issued
    // while no one is waiting is remembered and consumed by the next wait().
    void wake() noexcept;

    int fd() const noexcept { return queue_fd_; }

    // True once the kernel watch/fd limit was hit while building/extending the
    // tree: the watch set is incomplete, so changes in unwatched dirs can be
    // missed. The consumer should fall back to a full rescan when degraded.
    bool degraded() const noexcept { return degraded_; }

private:
    watcher() = default;

    // EVFILT_USER ident for wake() (kqueue). Its own filter space, so it never
    // collides with the per-path EVFILT_VNODE registrations. Unused on inotify.
    static constexpr std::uintptr_t kWakeIdent = 0;

    void close_all() noexcept
    {
#if !defined(__linux__)
        for (const auto &kv : fd_path_) // kqueue: per-dir open fds
            if (kv.first >= 0)
                ::close(kv.first);
        fd_path_.clear();
        watched_.clear();
#else
        wd_path_.clear(); // inotify wds are dropped when queue_fd_ is closed
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

    // Recursive-watch state shared by both backends: the ignore prefixes and a
    // degraded flag set when the kernel watch/fd limit is hit while building or
    // extending the tree. add_tree() recursively registers a dir + its subdirs;
    // is_ignored() tests a path against the prefixes.
    std::vector<std::string> ignores_;
    std::function<bool(const std::string &)> ignore_pred_;
    bool degraded_ = false;
    void add_tree(const std::string &dir);
    bool is_ignored(const std::string &path) const;
#if defined(__linux__)
    std::unordered_map<int, std::string> wd_path_; // inotify: watch desc -> dir
#else
    std::unordered_map<int, std::string> fd_path_; // kqueue: open fd -> dir
    std::unordered_set<std::string> watched_;      // kqueue: avoid re-watching
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

// True if `path` is, or is nested under, any ignore prefix. Shared by both
// backends; the recursive walk skips these subtrees.
bool watcher::is_ignored(const std::string &path) const
{
    for (const auto &ig : ignores_)
        if (path == ig ||
            (path.size() > ig.size() && path.compare(0, ig.size(), ig) == 0 &&
             path[ig.size()] == '/'))
            return true;
    return ignore_pred_ && ignore_pred_(path);
}

#if defined(__linux__) // ---------------------------------------- inotify ----
// Recursive watcher: inotify is per-directory, not recursive, so we register
// every directory under each root and keep the set in sync as the tree changes.

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
                std::span<const std::string> ignores,
                std::function<bool(const std::string &)> ignore_pred)
{
    watcher w;
    w.queue_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"inotify_init1", errno});
    w.wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w.wake_fd_ < 0)
        return std::unexpected(watch_error{"eventfd", errno});

    w.ignores_.assign(ignores.begin(), ignores.end());
    w.ignore_pred_ = std::move(ignore_pred);
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

    // Drain every queued event (read until EAGAIN), collecting the distinct
    // directories that changed so the consumer can scope an incremental status
    // to just them. A queue overflow means we lost track -> emit the resync
    // marker (an event with an empty path) so the consumer does a full rescan.
    std::unordered_set<std::string> dirs;
    bool overflow = false;
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
            if (ev->mask & IN_Q_OVERFLOW) {
                overflow = true;
            } else if (auto it = wd_path_.find(ev->wd); it != wd_path_.end()) {
                dirs.insert(it->second);
                if ((ev->mask & IN_ISDIR) &&
                    (ev->mask & (IN_CREATE | IN_MOVED_TO)) && ev->len > 0)
                    // New subdirectory: watch it (and rescan -- files may have
                    // appeared between mkdir and our add).
                    add_tree(it->second + "/" + ev->name);
                else if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF))
                    wd_path_.erase(it); // watch gone; forget it
            }
            ptr += sizeof(inotify_event) + ev->len;
        }
    }

    if (overflow) {
        out.push_back(fs_event{std::string{}}); // resync marker
        return out;
    }
    out.reserve(dirs.size());
    for (const auto &d : dirs)
        out.push_back(fs_event{d});
    return out;
}

#else // ----------------------------------------------------------- kqueue ----
// Recursive watcher: kqueue's EVFILT_VNODE is per-open-fd (per directory), so
// -- like inotify -- we open + register every directory under each root and
// keep the set in sync. One fd per directory, so we raise RLIMIT_NOFILE and
// degrade (not fail) if it is exhausted on a very large tree.

// Register `dir` and, depth-first, every directory beneath it (skipping ignored
// subtrees and dirs already watched -- open() is not idempotent, so re-adding
// would leak fds). Sets degraded_ if the open-file limit is hit.
void watcher::add_tree(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::vector<std::string> stack{dir};
    while (!stack.empty()) {
        std::string d = std::move(stack.back());
        stack.pop_back();
        if (is_ignored(d) || watched_.count(d))
            continue;

        int wfd = ::open(d.c_str(), O_RDONLY);
        if (wfd < 0) {
            if (errno == EMFILE || errno == ENFILE)
                degraded_ = true; // out of fds; function on what we have
            continue;             // unreadable/vanished dir: skip, keep going
        }
        struct kevent kev;
        EV_SET(&kev, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND, 0, nullptr);
        if (::kevent(queue_fd_, &kev, 1, nullptr, 0, nullptr) < 0) {
            ::close(wfd);
            continue;
        }
        fd_path_[wfd] = d;
        watched_.insert(d);

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
                std::span<const std::string> ignores,
                std::function<bool(const std::string &)> ignore_pred)
{
    // Best-effort: raise the soft open-file limit toward the hard cap so a deep
    // tree's per-directory fds fit. Never lowers it; ignores failure.
    struct rlimit rl;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 10240) {
        rlim_t want = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > 10240)
                          ? 10240
                          : rl.rlim_max;
        if (want > rl.rlim_cur) {
            rl.rlim_cur = want;
            ::setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    watcher w;
    w.queue_fd_ = ::kqueue();
    if (w.queue_fd_ < 0)
        return std::unexpected(watch_error{"kqueue", errno});

    // A user-triggerable event so wake() can unblock a blocked wait(). Its own
    // filter space (EVFILT_USER), so it never collides with the per-dir fds.
    struct kevent uev;
    EV_SET(&uev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(w.queue_fd_, &uev, 1, nullptr, 0, nullptr) < 0)
        return std::unexpected(watch_error{"kevent EVFILT_USER", errno});

    w.ignores_.assign(ignores.begin(), ignores.end());
    w.ignore_pred_ = std::move(ignore_pred);
    for (const auto &r : roots)
        w.add_tree(r);
    if (w.fd_path_.empty()) // nothing watchable at all -> a real setup failure
        return std::unexpected(watch_error{"open (no roots)", errno});
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
    std::array<struct kevent, 32> evs;
    int n = ::kevent(queue_fd_, nullptr, 0, evs.data(),
                     static_cast<int>(evs.size()), nullptr); // block indefinitely
    if (n < 0)
        return errno == EINTR ? std::expected<std::vector<fs_event>, watch_error>{}
                              : std::unexpected(watch_error{"kevent wait", errno});

    std::unordered_set<std::string> dirs; // distinct changed dirs
    std::vector<std::string> rescan;      // dirs whose contents changed
    for (int i = 0; i < n; ++i) {
        if (evs[i].filter == EVFILT_USER)
            continue; // woken via wake(), not a filesystem change
        int wfd = static_cast<int>(evs[i].ident);
        auto it = fd_path_.find(wfd);
        if (it == fd_path_.end())
            continue;

        dirs.insert(it->second);

        if (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME)) {
            // The watched dir vanished/moved: drop it.
            ::close(wfd);
            watched_.erase(it->second);
            fd_path_.erase(it);
        } else {
            // NOTE_WRITE/EXTEND: its entries changed -> maybe new subdirs.
            rescan.push_back(it->second);
        }
    }
    // kqueue doesn't say *what* changed in a dir, so for each changed dir we
    // enumerate its immediate children and recursively watch any new subdir
    // (add_tree skips the changed dir itself, which is already watched).
    namespace fs = std::filesystem;
    for (const auto &d : rescan) {
        std::error_code ec;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied,
                                       ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2) && !ec2 &&
                !watched_.count(it->path().string()))
                add_tree(it->path().string());
        }
    }

    std::vector<fs_event> out;
    out.reserve(dirs.size());
    for (const auto &d : dirs)
        out.push_back(fs_event{d});
    return out;
}

#endif

} // namespace mg::fswatch
