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
#include <atomic>             // FSEvents: lock-free degraded flag
#include <cerrno>
#include <condition_variable> // FSEvents: bridge the callback thread to wait()
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>      // recursive tree walk (kqueue/inotify)
#include <functional>      // ignore predicate (git-aware, supplied by caller)
#include <memory>          // FSEvents: own the (non-movable) callback state
#include <mutex>           // FSEvents: guard the pending-events buffer
#include <span>
#include <string>
#include <unordered_map>   // wd/fd -> dir path
#include <unordered_set>   // kqueue: dedup re-adds
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

// macOS default backend is FSEvents (one stream for the whole worktree, no
// per-directory fds, no startup walk). Define MG_FSWATCH_FORCE_KQUEUE to use
// the kqueue backend on macOS instead -- this exercises the BSD kqueue code
// path on the Mac (Darwin's kqueue is the same EVFILT_VNODE API the BSDs use),
// so the BSD-only backend stays continuously tested without a BSD machine.
#if defined(__APPLE__) && !defined(MG_FSWATCH_FORCE_KQUEUE)
#  define MG_FSWATCH_FSEVENTS 1
#endif

#if defined(__linux__)
#  include <poll.h>
#  include <sys/eventfd.h>
#  include <sys/inotify.h>
#elif defined(MG_FSWATCH_FSEVENTS)
#  include <CoreServices/CoreServices.h> // FSEvents
#  include <dispatch/dispatch.h>         // serial queue for the stream callback
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <array>
#  include <sys/event.h>
#  include <sys/resource.h> // raise RLIMIT_NOFILE for the per-dir watch fds
#  include <sys/types.h>
#else
#  error "mg.fswatch: unsupported platform (needs FSEvents, kqueue, or inotify)"
#endif

export module mg.fswatch;

import mg.coro;

// Module-internal helpers (not exported).
namespace mg::fswatch::detail {

// True if `path` is, or is nested under, any ignore prefix in `ignores`, or the
// caller's `pred` rejects it. Shared by every backend's filtering.
inline bool path_ignored(const std::vector<std::string> &ignores,
                         const std::function<bool(const std::string &)> &pred,
                         const std::string &path)
{
    for (const auto &ig : ignores)
        if (path == ig ||
            (path.size() > ig.size() && path.compare(0, ig.size(), ig) == 0 &&
             path[ig.size()] == '/'))
            return true;
    return pred && pred(path);
}

#if defined(MG_FSWATCH_FSEVENTS)
// State the FSEvents callback (running on a dispatch queue) shares with wait()
// (running on the monitor thread). Heap-owned by the watcher via unique_ptr so
// the watcher stays movable while this -- with its non-movable mutex/condvar --
// keeps a stable address for the stream's callback context.
struct fse_state {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> pending; // distinct changed dirs; "" == resync marker
    bool woken = false;               // wake() was called (guarded by mu)
    std::atomic<bool> degraded{false}; // FSEvents dropped events / asked for a rescan
    std::vector<std::string> ignores; // filter state (callback owns its copy)
    std::function<bool(const std::string &)> ignore_pred;
};

// FSEvents delivers coalesced change notifications here on the dispatch queue.
// Paths are directories (dir-level stream). Drop/overflow flags become the
// resync marker + degraded. Filtered, deduped, and handed to wait() via cv.
inline void fse_callback(ConstFSEventStreamRef, void *info, size_t n,
                         void *paths, const FSEventStreamEventFlags flags[],
                         const FSEventStreamEventId[])
{
    auto *st = static_cast<fse_state *>(info);
    auto **cpaths = static_cast<char **>(paths);
    std::vector<std::string> add;
    add.reserve(n);
    bool resync = false;
    for (size_t i = 0; i < n; ++i) {
        if (flags[i] & (kFSEventStreamEventFlagUserDropped |
                        kFSEventStreamEventFlagKernelDropped |
                        kFSEventStreamEventFlagMustScanSubDirs)) {
            resync = true; // we lost track -> consumer must full-rescan
            continue;
        }
        std::string p = cpaths[i];
        // FSEvents reports directory paths with a trailing '/'; strip it so paths
        // match the kqueue/inotify convention and the consumer's repo prefix
        // (which has none).
        if (p.size() > 1 && p.back() == '/')
            p.pop_back();
        if (!path_ignored(st->ignores, st->ignore_pred, p))
            add.push_back(std::move(p));
    }
    if (add.empty() && !resync)
        return;
    {
        std::lock_guard lk(st->mu);
        if (resync) {
            st->degraded = true;
            st->pending.emplace_back(); // empty path == resync marker
        }
        for (auto &p : add)
            st->pending.push_back(std::move(p));
    }
    st->cv.notify_one();
}
#endif // MG_FSWATCH_FSEVENTS

} // namespace mg::fswatch::detail

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
            ignores_     = std::move(other.ignores_);
            ignore_pred_ = std::move(other.ignore_pred_);
#if defined(MG_FSWATCH_FSEVENTS)
            stream_ = std::exchange(other.stream_, nullptr);
            queue_  = std::exchange(other.queue_, nullptr);
            st_     = std::move(other.st_);
#else
            queue_fd_ = std::exchange(other.queue_fd_, -1);
            wake_fd_  = std::exchange(other.wake_fd_, -1);
            degraded_ = std::exchange(other.degraded_, false);
#  if defined(__linux__)
            wd_path_  = std::move(other.wd_path_);
#  else
            fd_path_  = std::move(other.fd_path_);
            watched_  = std::move(other.watched_);
#  endif
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

    int fd() const noexcept
    {
#if defined(MG_FSWATCH_FSEVENTS)
        return -1; // FSEvents delivers via a dispatch queue; no pollable fd
#else
        return queue_fd_;
#endif
    }

    // True once event delivery degraded: the kernel watch/fd limit was hit
    // (kqueue/inotify), or FSEvents dropped events / asked for a full rescan.
    // The watch set / event stream is incomplete, so the consumer should fall
    // back to a full rescan.
    bool degraded() const noexcept
    {
#if defined(MG_FSWATCH_FSEVENTS)
        return st_->degraded.load(std::memory_order_relaxed);
#else
        return degraded_;
#endif
    }

private:
    watcher() = default;

    // EVFILT_USER ident for wake() (kqueue). Its own filter space, so it never
    // collides with the per-path EVFILT_VNODE registrations. Unused on inotify.
    static constexpr std::uintptr_t kWakeIdent = 0;

    void close_all() noexcept
    {
#if defined(MG_FSWATCH_FSEVENTS)
        if (stream_) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_); // unschedule from the queue
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (queue_) {
            // Flush any callback block already queued before we free st_, so the
            // callback can never touch freed state (no new blocks: invalidated).
            dispatch_sync_f(queue_, nullptr, [](void *) {});
            dispatch_release(queue_);
            queue_ = nullptr;
        }
        st_.reset(); // safe: no callback can run after stop+invalidate+flush
#else
#  if !defined(__linux__)
        for (const auto &kv : fd_path_) // kqueue: per-dir open fds
            if (kv.first >= 0)
                ::close(kv.first);
        fd_path_.clear();
        watched_.clear();
#  else
        wd_path_.clear(); // inotify wds are dropped when queue_fd_ is closed
#  endif
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        if (queue_fd_ >= 0) { // closing the inotify fd drops all its watches
            ::close(queue_fd_);
            queue_fd_ = -1;
        }
#endif
    }

    // Ignore prefixes + the git-aware predicate (kqueue/inotify consult these
    // during the recursive walk via is_ignored(); the FSEvents callback keeps
    // its own copy in fse_state and filters there). is_ignored() tests a path
    // against the prefixes.
    std::vector<std::string> ignores_;
    std::function<bool(const std::string &)> ignore_pred_;
    bool is_ignored(const std::string &path) const;

#if defined(MG_FSWATCH_FSEVENTS) // ----------------------------------- FSEvents
    FSEventStreamRef stream_ = nullptr; // one stream for the whole worktree
    dispatch_queue_t queue_  = nullptr; // serial queue the callback runs on
    std::unique_ptr<detail::fse_state> st_; // callback <-> wait() shared state
#else                            // -------------------------- kqueue / inotify
    int queue_fd_ = -1; // kqueue fd, or inotify fd
    int wake_fd_  = -1; // inotify: eventfd; kqueue: unused (-1)
    // degraded flag set when the kernel watch/fd limit is hit while building or
    // extending the tree. add_tree() recursively registers a dir + its subdirs.
    bool degraded_ = false;
    void add_tree(const std::string &dir);
#  if defined(__linux__)
    std::unordered_map<int, std::string> wd_path_; // inotify: watch desc -> dir
#  else
    std::unordered_map<int, std::string> fd_path_; // kqueue: open fd -> dir
    std::unordered_set<std::string> watched_;      // kqueue: avoid re-watching
#  endif
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

// True if `path` is, or is nested under, any ignore prefix. Used by the
// kqueue/inotify recursive walk to skip subtrees. (FSEvents filters in its
// callback via detail::path_ignored directly.)
bool watcher::is_ignored(const std::string &path) const
{
    return detail::path_ignored(ignores_, ignore_pred_, path);
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

#elif defined(MG_FSWATCH_FSEVENTS) // ----------------------------- FSEvents ----
// One FSEventStream watches each root's whole subtree -- FSEvents is inherently
// recursive and auto-covers directories created later, so there are NO
// per-directory fds, no recursive walk, and no startup cost. The stream runs on
// a serial dispatch queue; the callback (detail::fse_callback) filters + buffers
// changed dirs and notifies wait() via the fse_state condvar.

std::expected<watcher, watch_error>
watcher::create(std::span<const std::string> roots,
                std::span<const std::string> ignores,
                std::function<bool(const std::string &)> ignore_pred)
{
    // FSEvents (unlike kqueue/inotify) happily creates a stream for a path that
    // does not exist yet. Match the other backends' contract: a watch with no
    // existing root is a setup failure.
    bool any_exists = false;
    for (const auto &r : roots) {
        std::error_code ec;
        if (std::filesystem::exists(r, ec)) {
            any_exists = true;
            break;
        }
    }
    if (!any_exists)
        return std::unexpected(watch_error{"FSEvents: no existing roots", ENOENT});

    watcher w;
    w.st_ = std::make_unique<detail::fse_state>();
    w.st_->ignores.assign(ignores.begin(), ignores.end());
    w.st_->ignore_pred = std::move(ignore_pred);

    CFMutableArrayRef paths =
        CFArrayCreateMutable(nullptr, static_cast<CFIndex>(roots.size()),
                             &kCFTypeArrayCallBacks);
    if (paths == nullptr)
        return std::unexpected(watch_error{"CFArrayCreateMutable", errno});
    for (const auto &r : roots) {
        CFStringRef s = CFStringCreateWithCString(nullptr, r.c_str(),
                                                  kCFStringEncodingUTF8);
        if (s) {
            CFArrayAppendValue(paths, s);
            CFRelease(s);
        }
    }

    FSEventStreamContext ctx{};
    ctx.info = w.st_.get();
    // Dir-level events (no FileEvents flag); char** paths (no UseCFTypes).
    // NoDefer = deliver the first event of a burst promptly; WatchRoot = also
    // notify if a watched root itself is moved/deleted.
    w.stream_ = FSEventStreamCreate(
        nullptr, &detail::fse_callback, &ctx, paths,
        kFSEventStreamEventIdSinceNow, /*latency*/ 0.15,
        kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (w.stream_ == nullptr)
        return std::unexpected(watch_error{"FSEventStreamCreate", errno});

    w.queue_ = dispatch_queue_create("mg.fswatch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(w.stream_, w.queue_);

    // Exclude the explicit ignore prefixes at the SOURCE: FSEvents watches whole
    // subtrees and can coalesce a deep change up to an ancestor, so callback
    // filtering alone can't keep an ignored subtree from waking us via its
    // unignored parent. SetExclusionPaths drops changes under these paths
    // entirely (max 8 honored; the monitor passes one -- .git/objects). The
    // gitignore *predicate* (node_modules, …) can't be expressed as a static
    // path list, so those are still filtered in the callback (best-effort:
    // an ancestor-coalesced event may slip through to a harmless full rescan).
    if (!ignores.empty()) {
        CFMutableArrayRef excl = CFArrayCreateMutable(
            nullptr, static_cast<CFIndex>(ignores.size()), &kCFTypeArrayCallBacks);
        if (excl) {
            for (const auto &ig : ignores) {
                CFStringRef s = CFStringCreateWithCString(nullptr, ig.c_str(),
                                                          kCFStringEncodingUTF8);
                if (s) {
                    CFArrayAppendValue(excl, s);
                    CFRelease(s);
                }
            }
            FSEventStreamSetExclusionPaths(w.stream_, excl);
            CFRelease(excl);
        }
    }

    if (!FSEventStreamStart(w.stream_))
        return std::unexpected(watch_error{"FSEventStreamStart", errno});
    return w;
}

void watcher::wake() noexcept
{
    {
        std::lock_guard lk(st_->mu);
        st_->woken = true;
    }
    st_->cv.notify_one();
}

std::expected<std::vector<fs_event>, watch_error>
watcher::wait()
{
    std::unique_lock lk(st_->mu);
    st_->cv.wait(lk, [&] { return st_->woken || !st_->pending.empty(); });

    std::vector<fs_event> out;
    st_->woken = false; // consume the wake (empty return == woken, no change)
    if (!st_->pending.empty()) {
        std::unordered_set<std::string> seen;
        out.reserve(st_->pending.size());
        for (auto &p : st_->pending) {
            if (p.empty()) {
                out.push_back(fs_event{}); // resync marker (consumer full-rescans)
            } else if (seen.insert(p).second) {
                out.push_back(fs_event{std::move(p)});
            }
        }
        st_->pending.clear();
    }
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
