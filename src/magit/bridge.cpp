// bridge.cpp -- C++ implementation of the magit extern "C" bridge (task M2d-1).
//
// A regular TU (not a module) that imports the engine modules and exposes the
// plain-C API in bridge.h. A background thread runs the M2a-M2c pipeline
// (watch -> libgit2 status -> summarize) and publishes the modeline string;
// the C core reads it through the bridge. Cancellation is instant via the
// wakeable watcher (stop_flag + wake()).

#include "bridge.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <expected>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

import mg.coro;
import mg.fswatch;
import mg.git;
import mg.magit;
import mg.magit.proclog;

// Map a rebase outcome to the bridge code: 1 done, 2 paused on conflicts,
// 0 failure. (Defined here so every rebase-style bridge fn can use it.)
static int rebase_code(
    const std::expected<mg::git::rebase_result, mg::git::error> &r)
{
    if (!r)
        return 0;
    if (*r == mg::git::rebase_result::conflicts)
        return 2;
    if (*r == mg::git::rebase_result::stopped)
        return 3; // interactive `edit`: amend, then continue
    return 1;
}

// Same mapping for apply-style ops (merge / revert / cherry-pick / pull):
// 1 done, 2 left conflicts on disk (resolve + commit), 0 failure.
static int apply_code(
    const std::expected<mg::git::apply_result, mg::git::error> &r)
{
    if (!r)
        return 0;
    return *r == mg::git::apply_result::conflicts ? 2 : 1;
}

// All the data the *magit-status* view is composed from. Gathered by querying
// the repo (gather_status_view) or, for the monitor's incremental path, built
// up by patching `status` in place while reusing the cached rest. Composed into
// the line stream by compose_status_view -- the two halves keep "where the data
// comes from" separate from "how it is rendered", so the monitor can swap a
// scoped/incremental status set in without touching composition. (At namespace
// scope so the monitor can hold one as a member and the composer, defined
// below, shares the type.)
struct status_view {
    std::optional<mg::git::head_info> head;
    std::optional<mg::git::upstream_info> upstream;
    bool rebasing = false;
    bool bisecting = false;
    std::vector<mg::magit::file_status> status; // the incremental target (S)
    std::vector<mg::git::conflict_entry> conflicts;
    std::vector<mg::git::commit_brief> unpulled, unpushed, recent;
    std::vector<mg::git::stash_entry> stashes;
    std::vector<mg::git::branch_entry> branches;
    std::vector<std::string> tags;
    std::vector<mg::git::worktree_entry> worktrees;
    std::vector<mg::git::submodule_entry> submodules;
};
status_view gather_status_view(const char *repo);
int compose_status_view(const status_view &v, const char *repo_path,
                        const char *const *expanded, int n_expanded,
                        mg_magit_emit_fn emit, void *ctx);

namespace {

// One captured status-buffer line (the collapsed snapshot stores these).
struct snap_line {
    std::string line;
    int kind;
    std::string path;
    int hunk;
};
void snap_capture(void *ctx, const char *line, int kind, const char *path,
                  int hunk)
{
    static_cast<std::vector<snap_line> *>(ctx)->push_back(
        {line ? line : "", kind, path ? path : "", hunk});
}

// A cheap fingerprint of the repo's status-affecting state: the index file's
// size + mtime plus HEAD's short oid. The monitor stamps each snapshot with the
// fingerprint it was built at; the UI recomputes it per render (~sub-ms) and, on
// a mismatch, knows the snapshot predates an on-disk change (the user's own
// mutation OR an external `git` command) and shows a "refreshing" marker until
// the worker catches up. This needs no instrumentation of the mutating bridge
// ops and catches changes from any source.
std::string repo_fingerprint(const std::string &repo)
{
    std::string fp;
    struct stat st {};
    if (::stat((repo + "/.git/index").c_str(), &st) == 0) {
        fp += std::to_string(static_cast<long long>(st.st_size));
        fp += '.';
#if defined(__APPLE__)
        fp += std::to_string(st.st_mtimespec.tv_sec);
        fp += '.';
        fp += std::to_string(st.st_mtimespec.tv_nsec);
#else
        fp += std::to_string(st.st_mtim.tv_sec);
        fp += '.';
        fp += std::to_string(st.st_mtim.tv_nsec);
#endif
    }
    if (auto head = mg::git::read_head(repo); head) {
        fp += '.';
        fp += head->short_oid;
    }
    return fp;
}

// Shared UI wake: a self-pipe that any background producer (the status monitor
// and the async job worker) pokes to nudge the idle UI poll() (ttgetc) into a
// redraw / result-apply with no keypress. Opened before the worker threads
// start and closed after they join (in mg_magit_start/stop), so the fds are
// never touched concurrently with open/close. signal()/drain() are otherwise
// safe across threads: a pipe write/read is kernel-synchronized.
struct ui_wake {
    int rd = -1; // pollable read end (UI side)
    int wr = -1; // write end (producers poke a byte)

    void open_pipe()
    {
        int fds[2];
        if (::pipe(fds) == 0) {
            rd = fds[0];
            wr = fds[1];
            for (int fd : fds) {
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
    }

    void close_pipe()
    {
        if (rd >= 0)
            ::close(rd);
        if (wr >= 0)
            ::close(wr);
        rd = wr = -1;
    }

    // Non-blocking: a full pipe already means "something changed", so dropping
    // this byte is fine.
    void signal() noexcept
    {
        if (wr >= 0) {
            char b = 1;
            ssize_t r = ::write(wr, &b, 1);
            (void)r;
        }
    }

    void drain() noexcept
    {
        if (rd < 0)
            return;
        char buf[64];
        while (::read(rd, buf, sizeof buf) > 0)
            ; // drain to empty (non-blocking; stops at EAGAIN)
    }
};

ui_wake g_wake;

class monitor {
public:
    explicit monitor(std::string repo) : repo_(std::move(repo))
    {
        // The watcher's recursive tree walk opens one fd per directory (macOS
        // kqueue) and can take seconds on a large repo -- so it is built on the
        // monitor thread (see run()), NOT here. Doing it here would block
        // mg_magit_start and freeze editor startup until the walk finished. The
        // ctor only launches the thread, so mg_magit_start returns immediately.
        thread_ = std::thread([this] { run(); });
    }

    ~monitor()
    {
        stop_.request_stop();
        {
            // watcher_ is created on the monitor thread (run()); guard the read.
            std::lock_guard lk(mu_);
            if (watcher_)
                watcher_->wake(); // release a blocked wait() immediately
        }
        if (thread_.joinable())
            thread_.join();
    }

    monitor(const monitor &) = delete;
    monitor &operator=(const monitor &) = delete;

    int take_dirty() noexcept { return dirty_.exchange(false) ? 1 : 0; }

    // Copy the latest status snapshot + the fingerprint it was built at; false
    // if none built yet (cold start).
    bool copy_snapshot(std::vector<snap_line> &out, std::string &fp)
    {
        std::lock_guard lk(mu_);
        if (!have_snapshot_)
            return false;
        out = snapshot_;
        fp = snapshot_fp_;
        return true;
    }

    int modeline(char *buf, std::size_t n)
    {
        if (buf == nullptr || n == 0)
            return 0;
        std::lock_guard lk(mu_);
        std::size_t len = current_.size();
        if (len >= n)
            len = n - 1;
        std::memcpy(buf, current_.data(), len);
        buf[len] = '\0';
        return static_cast<int>(len);
    }

private:
    // Compose the cached view_ into the published snapshot + modeline, stamp the
    // fingerprint, and wake the UI. The expensive scan is NOT here: view_ is
    // either freshly gathered (full_refresh) or incrementally patched
    // (reconcile). Runs on the monitor thread only; view_ needs no lock.
    void publish_view()
    {
        std::string line;
        if (view_.head && !view_.head->branch.empty())
            line = view_.head->branch + " ";
        line += mg::magit::summarize(view_.status); // counts from cached status

        // Stamp the fingerprint BEFORE composing: if a mutation lands meanwhile
        // the snapshot looks stale and the UI just re-reconciles (never reverse).
        std::string fp = repo_fingerprint(repo_);
        std::vector<snap_line> snap;
        (void)compose_status_view(view_, repo_.c_str(), nullptr, 0, snap_capture,
                                  &snap);
        {
            std::lock_guard lk(mu_);
            current_ = std::move(line);
            snapshot_ = std::move(snap);
            snapshot_fp_ = std::move(fp);
            have_snapshot_ = true;
        }
        dirty_.store(true);
        g_wake.signal(); // nudge the idle UI to redraw
    }

    // Re-query everything (the full scan). The correct baseline and the fallback
    // for anything the incremental path can't handle. Also re-opens the reused
    // session: full_refresh runs exactly when refs/index could have moved (.git
    // change / cold start), so a fresh handle here keeps the session's caches
    // valid for the warm incremental reuses that follow.
    void full_refresh()
    {
        if (auto s = mg::git::session::open(repo_))
            session_.emplace(std::move(*s));
        else
            session_.reset();
        view_ = gather_status_view(repo_.c_str());
        publish_view();
    }

    // Apply a batch of changed dirs: a worktree-only batch scope-patches view_'s
    // status (O(changed) -- no full scan); anything that moves the baseline
    // (a .git change, the repo root itself, the resync marker, or a degraded
    // watcher) falls back to a full_refresh().
    void reconcile(const std::vector<mg::fswatch::fs_event> &events)
    {
        bool need_full = watcher_ && watcher_->degraded();
        std::vector<std::string> dirs;
        for (const auto &e : events) {
            if (e.path.empty()) { // resync marker (e.g. inotify overflow)
                need_full = true;
                break;
            }
            std::string rel;
            if (e.path.size() > repo_.size() &&
                e.path.compare(0, repo_.size(), repo_) == 0) {
                rel = e.path.substr(repo_.size());
                if (!rel.empty() && rel.front() == '/')
                    rel.erase(0, 1);
            }
            // repo root itself, or any .git change (staging/commit/refs move the
            // whole-tree baseline) -> full rescan.
            if (rel.empty() || rel == ".git" || rel.starts_with(".git/")) {
                need_full = true;
                break;
            }
            dirs.push_back(std::move(rel));
        }
        if (need_full) {
            full_refresh();
            return;
        }
        // Scope the scan to the changed dirs on the REUSED session handle: the
        // index is unchanged on a worktree edit, so libgit2 skips the re-read
        // (~0.6ms vs ~20ms per-call open). Fall back to a one-shot scan if the
        // session failed to open.
        auto scoped = session_ ? session_->status_scoped(dirs)
                               : mg::git::repo_status_scoped(repo_, dirs);
        if (!scoped) { // scan error -> safe fallback
            full_refresh();
            return;
        }
        mg::git::apply_status_patch(view_.status, *scoped, dirs);
        publish_view();
    }

    void run()
    {
        full_refresh(); // initial baseline, before any event (one status scan)

        // Build the recursive watcher HERE, on the monitor thread: the tree walk
        // opens one fd per directory (macOS kqueue) and can take seconds on a
        // large repo. The initial status above is already published, so the UI
        // is fully responsive while this runs in the background. Watch the
        // worktree root + .git (so commits/branch switches wake us); exclude
        // .git/objects and, via the git-aware predicate, every gitignored tree.
        {
            std::vector<std::string> roots{repo_, repo_ + "/.git"};
            std::vector<std::string> ignores{repo_ + "/.git/objects"};
            auto w = mg::fswatch::watcher::create(
                roots, ignores, mg::git::make_ignore_predicate(repo_));
            if (w) {
                std::lock_guard lk(mu_);
                watcher_.emplace(std::move(*w));
            }
        }
        if (!watcher_)
            return;
        while (!stop_.stop_requested()) {
            auto events = watcher_->wait();
            if (!events)
                break;            // watcher error
            if (events->empty())
                continue;         // woken by wake() (e.g. stop), no change
            reconcile(*events);
        }
    }

    std::string repo_;
    std::mutex mu_;
    std::string current_;
    std::vector<snap_line> snapshot_;
    std::string snapshot_fp_;
    bool have_snapshot_ = false;
    status_view view_; // monitor-thread-private; the incremental status cache
    std::optional<mg::git::session> session_; // reused handle for scoped status
    std::atomic<bool> dirty_{false};
    mg::stop_flag stop_;
    std::optional<mg::fswatch::watcher> watcher_;
    std::thread thread_;
};

std::unique_ptr<monitor> g_monitor;

// --- Async per-file build worker (FM-ASYNC-BLAME) --------------------------
// A pending request; the mailbox keeps the latest one per kind (coalescing).
struct async_job {
    int kind;
    std::string repo;
    std::string path;
    int n;
    unsigned gen;
};

// A finished build: the captured line stream, tagged with the request it came
// from so the UI can drop it if a newer request has since superseded it.
struct async_result {
    int kind;
    std::string path;
    unsigned gen;
    std::vector<snap_line> lines;
};

// One worker thread that drains expensive per-file builds (blame / log-file)
// off the UI thread. The UI requests; the worker computes (reusing the proven
// synchronous bridge fns with a capturing emit) and queues an immutable result.
// Latest request per kind wins; a result superseded before it is published is
// skipped. Its own libgit2 work uses fresh handles, so it is independent of the
// monitor thread (a 1.2s blame must not stall the status recompute).
class job_runner {
public:
    job_runner() { thread_ = std::thread([this] { run(); }); }

    ~job_runner()
    {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    job_runner(const job_runner &) = delete;
    job_runner &operator=(const job_runner &) = delete;

    unsigned request(int kind, std::string repo, std::string path, int n)
    {
        std::lock_guard lk(mu_);
        unsigned g = ++gen_;
        pending_[kind] = async_job{kind, std::move(repo), std::move(path), n, g};
        cv_.notify_all();
        return g;
    }

    bool peek(int &kind, std::string &path, unsigned &gen)
    {
        std::lock_guard lk(mu_);
        if (ready_.empty())
            return false;
        kind = ready_.front().kind;
        path = ready_.front().path;
        gen = ready_.front().gen;
        return true;
    }

    bool take(std::vector<snap_line> &out, int &kind, std::string &path,
              unsigned &gen)
    {
        std::lock_guard lk(mu_);
        if (ready_.empty())
            return false;
        out = std::move(ready_.front().lines);
        kind = ready_.front().kind;
        path = ready_.front().path;
        gen = ready_.front().gen;
        ready_.pop_front();
        return true;
    }

private:
    void run()
    {
        for (;;) {
            async_job job;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !pending_.empty(); });
                if (stop_)
                    return;
                auto it = pending_.begin(); // any kind; drained round-robin
                job = std::move(it->second);
                pending_.erase(it);
            }

            // The slow part, off the UI thread, no lock held.
            std::vector<snap_line> lines;
            if (job.kind == MG_ASYNC_BLAME)
                (void)mg_magit_blame_file(job.repo.c_str(), job.path.c_str(),
                                          snap_capture, &lines);
            else
                (void)mg_magit_log_file_buffer(job.repo.c_str(),
                                               job.path.c_str(), job.n,
                                               snap_capture, &lines);

            {
                std::lock_guard lk(mu_);
                // Skip if a newer request for this kind already supersedes us
                // (the UI would drop it anyway; don't bother publishing).
                auto it = pending_.find(job.kind);
                if (it != pending_.end() && it->second.gen > job.gen)
                    continue;
                ready_.push_back(async_result{job.kind, std::move(job.path),
                                              job.gen, std::move(lines)});
            }
            g_wake.signal(); // nudge the idle UI to apply the result
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::map<int, async_job> pending_; // latest request per kind
    std::deque<async_result> ready_;
    unsigned gen_ = 0;
    std::thread thread_;
};

std::unique_ptr<job_runner> g_jobs;

// Human label for a status code, for the magit-status sections.
const char *state_word(mg::magit::status s)
{
    using S = mg::magit::status;
    switch (s) {
    case S::added:    return "new file";
    case S::deleted:  return "deleted";
    case S::renamed:  return "renamed";
    case S::copied:   return "copied";
    case S::unmerged: return "unmerged";
    default:          return "modified";
    }
}

// Record an in-process libgit2 mutation as its equivalent git command.
void proc_eq(const std::string &cmd, bool ok)
{
    mg::magit::proclog::record('~', cmd, "", ok, 0);
}

} // namespace

extern "C" void mg_magit_start(const char *repo_path)
{
    // Hold one libgit2 init for the whole monitor+worker lifetime so the global
    // refcount never returns to 0 while they run -- otherwise a per-call
    // init_guard hitting 0 on one thread races libgit2's global (OpenSSL) setup
    // on another (TSan-confirmed on the Linux/OpenSSL backend).
    mg::git::global_init();
    g_wake.open_pipe(); // before any worker thread that may signal it
    // Only run the monitor when launched inside a git repository. Outside one
    // there is nothing to track, and recursively watching an arbitrary tree
    // (e.g. `mg ~/src`) opens a watch fd per directory and exhausts the process
    // fd table -- breaking file-open, dired, and completion. discover_workdir
    // searches upward, so launching in any subdirectory of a repo still works;
    // we watch the bounded workdir rather than the raw launch directory.
    if (auto workdir = mg::git::discover_workdir(repo_path ? repo_path : ".")) {
        g_monitor = std::make_unique<monitor>(*workdir);
        g_jobs = std::make_unique<job_runner>();
    }
}

extern "C" void mg_magit_stop(void)
{
    g_jobs.reset();      // joins the worker thread
    g_monitor.reset();   // joins the monitor thread
    g_wake.close_pipe(); // no thread touches g_wake after the joins
    mg::git::global_shutdown(); // drop the lifetime ref now the threads are gone
}

extern "C" int mg_magit_take_dirty(void)
{
    return g_monitor ? g_monitor->take_dirty() : 0;
}

extern "C" int mg_magit_wake_fd(void) { return g_wake.rd; }

extern "C" void mg_magit_drain_wake(void) { g_wake.drain(); }

extern "C" int mg_magit_modeline(char *buf, size_t buflen)
{
    return g_monitor ? g_monitor->modeline(buf, buflen) : 0;
}

// --- Async per-file builds (FM-ASYNC-BLAME) --------------------------------
extern "C" unsigned mg_magit_async_request(int kind, const char *repo,
                                           const char *path, int n)
{
    if (g_jobs == nullptr || repo == nullptr || path == nullptr)
        return 0;
    return g_jobs->request(kind, repo, path, n);
}

extern "C" int mg_magit_async_peek(int *kind, char *path_out, size_t path_cap,
                                   unsigned *gen)
{
    if (g_jobs == nullptr)
        return -1;
    int k;
    std::string p;
    unsigned g;
    if (!g_jobs->peek(k, p, g))
        return -1;
    if (kind != nullptr)
        *kind = k;
    if (gen != nullptr)
        *gen = g;
    if (path_out != nullptr && path_cap > 0) {
        std::size_t len = p.size();
        if (len >= path_cap)
            len = path_cap - 1;
        std::memcpy(path_out, p.data(), len);
        path_out[len] = '\0';
    }
    return k;
}

extern "C" int mg_magit_async_take(mg_magit_emit_fn emit, void *ctx)
{
    if (g_jobs == nullptr || emit == nullptr)
        return -1;
    std::vector<snap_line> lines;
    int k;
    std::string p;
    unsigned g;
    if (!g_jobs->take(lines, k, p, g))
        return -1;
    int n = 0;
    for (const auto &e : lines) {
        emit(ctx, e.line.c_str(), e.kind,
             e.path.empty() ? nullptr : e.path.c_str(), e.hunk);
        ++n;
    }
    return n;
}

// Emit a file's diff hunks (MG_LINE_HUNK header + MG_LINE_DIFF lines) for an
// expanded entry. Shared by the synchronous status build and the snapshot
// replay so the two render byte-identical output. Returns lines emitted.
static int emit_file_diff(const char *repo, const char *path, bool staged,
                          mg_magit_emit_fn emit, void *ctx)
{
    auto chomp = [](std::string s) {
        if (!s.empty() && s.back() == '\n')
            s.pop_back();
        return s;
    };
    auto hunks = mg::git::file_diff(repo, path, staged);
    if (!hunks)
        return 0;
    int n = 0;
    for (int hi = 0; hi < static_cast<int>(hunks->size()); ++hi) {
        std::string h = chomp((*hunks)[hi].header);
        emit(ctx, h.c_str(), MG_LINE_HUNK, path, hi);
        ++n;
        for (const auto &l : (*hunks)[hi].lines) {
            std::string line = std::string(1, l.origin) + chomp(l.content);
            emit(ctx, line.c_str(), MG_LINE_DIFF, path, hi);
            ++n;
        }
    }
    return n;
}

// Run every query the full status build needs into one view.
status_view gather_status_view(const char *repo)
{
    status_view v;
    if (auto h = mg::git::read_head(repo))
        v.head = std::move(*h);
    if (auto u = mg::git::upstream_status(repo))
        v.upstream = std::move(*u);
    v.rebasing = mg::git::rebase_in_progress(repo);
    v.bisecting = mg::git::bisect_active(repo);
    // Cold-path status: hybrid_status uses the Zig worktree walk + libgit2
    // staged column when compiled with MG_ZIG_STATUS (ENABLE_ZIG_STATUS=ON),
    // and falls back to repo_status (full libgit2 scan) otherwise -- so this
    // call is unconditional and byte-identical to the old repo_status call in
    // the OFF build.
    //
    // orig_path (renames/copies): staged renames are detected (libgit2
    // HEAD->index rename detection in the oracle; git_diff_find_similar in the
    // hybrid's X column) and carry their source path, which compose_status_view
    // renders Magit-style as "old -> new". Worktree (unstaged) renames are still
    // shown as a delete+add pair: the hybrid's Y column is the Zig walker, which
    // has no content-similarity detector, so enabling INDEX_TO_WORKDIR rename
    // detection in the oracle alone would desync it from the hybrid.
    if (auto s = mg::git::hybrid_status(repo))
        v.status = std::move(*s);
    if (auto c = mg::git::conflicts(repo))
        v.conflicts = std::move(*c);
    if (auto up = mg::git::upstream_commits(repo, /*unpushed=*/false))
        v.unpulled = std::move(*up);
    if (auto up = mg::git::upstream_commits(repo, /*unpushed=*/true))
        v.unpushed = std::move(*up);
    if (auto st = mg::git::stashes(repo))
        v.stashes = std::move(*st);
    if (auto rc = mg::git::recent_commits(repo, 10))
        v.recent = std::move(*rc);
    if (auto b = mg::git::branches(repo))
        v.branches = std::move(*b);
    if (auto t = mg::git::tags(repo))
        v.tags = std::move(*t);
    if (auto w = mg::git::worktrees(repo))
        v.worktrees = std::move(*w);
    if (auto sm = mg::git::submodules(repo))
        v.submodules = std::move(*sm);
    return v;
}

// Render `v` to the line stream. `repo_path` is only used to compute inline
// diffs for expanded entries. Output is the exact composition the inline
// status build used to produce (the test suite + determinism anchor pin it).
int compose_status_view(const status_view &v, const char *repo_path,
                        const char *const *expanded, int n_expanded,
                        mg_magit_emit_fn emit, void *ctx)
{
    int n = 0;
    auto out = [&](const std::string &line, int kind = MG_LINE_OTHER,
                   const char *path = nullptr, int hunk = -1) {
        emit(ctx, line.c_str(), kind, path, hunk);
        ++n;
    };
    auto is_expanded = [&](const std::string &p) {
        for (int i = 0; i < n_expanded; ++i)
            if (expanded != nullptr && expanded[i] != nullptr && p == expanded[i])
                return true;
        return false;
    };
    auto emit_diff = [&](const std::string &path, bool staged) {
        n += emit_file_diff(repo_path, path.c_str(), staged, emit, ctx);
    };

    if (v.head) {
        out("On branch " + (v.head->branch.empty() ? std::string("(unknown)")
                                                    : v.head->branch));
        if (!v.head->short_oid.empty())
            out("Head:     " + v.head->short_oid + " " + v.head->summary);
    }

    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv = "Diff:     -U" + std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        out(dv);
    }

    if (v.upstream && v.upstream->has_upstream) {
        std::string line = "Upstream: " + v.upstream->name;
        if (v.upstream->ahead != 0 || v.upstream->behind != 0)
            line += " [ahead " + std::to_string(v.upstream->ahead) + ", behind " +
                    std::to_string(v.upstream->behind) + "]";
        out(line);
    }

    if (v.rebasing) {
        out("");
        out("Rebasing -- resolve conflicts, then r r (continue) / r s (skip) / "
            "r a (abort)",
            MG_LINE_SECTION);
    }

    if (v.bisecting) {
        out("");
        out("Bisecting -- test the checked-out commit, then Z b (bad) / Z g "
            "(good); Z r resets",
            MG_LINE_SECTION);
    }

    {
        using S = mg::magit::status;
        std::vector<const mg::magit::file_status *> untracked, unstaged, staged;
        for (const auto &e : v.status) {
            if (e.index == S::unmerged || e.worktree == S::unmerged)
                continue; // shown in the dedicated Conflicts section below
            if (e.worktree == S::untracked) {
                untracked.push_back(&e);
                continue;
            }
            if (e.index != S::unmodified)
                staged.push_back(&e);
            if (e.worktree != S::unmodified)
                unstaged.push_back(&e);
        }

        auto section = [&](const char *title,
                           const std::vector<const mg::magit::file_status *> &vec,
                           bool labeled, bool use_index, int kind, bool diffable) {
            if (vec.empty())
                return;
            out("");
            out(std::string(title) + " (" + std::to_string(vec.size()) + ")",
                MG_LINE_SECTION);
            for (const auto *e : vec) {
                // Renames/copies show both endpoints Magit-style ("old -> new");
                // the row's path stays the destination so staging/visiting still
                // target the file. The arrow is earned by whichever column this
                // section renders (index for staged, worktree for unstaged).
                S code = use_index ? e->index : e->worktree;
                std::string name =
                    (code == S::renamed || code == S::copied) && e->orig_path
                        ? *e->orig_path + " -> " + e->path
                        : e->path;
                std::string text =
                    labeled ? "  " + std::string(state_word(code)) + "  " + name
                            : "  " + name;
                out(text, kind, e->path.c_str());
                if (diffable && is_expanded(e->path))
                    emit_diff(e->path, use_index);
            }
        };
        if (!v.conflicts.empty()) {
            out("");
            out("Conflicts (" + std::to_string(v.conflicts.size()) +
                    ") -- e o/e t whole file - E ediff (per region) - RET edit",
                MG_LINE_SECTION);
            for (const auto &c : v.conflicts)
                out("  " + c.path, MG_LINE_CONFLICT, c.path.c_str());
        }
        section("Untracked files", untracked, false, false, MG_LINE_UNTRACKED, false);
        section("Unstaged changes", unstaged, true, false, MG_LINE_UNSTAGED, true);
        section("Staged changes", staged, true, true, MG_LINE_STAGED, true);
    }

    auto commit_section = [&](const char *title,
                              const std::vector<mg::git::commit_brief> &commits) {
        if (commits.empty())
            return;
        out("");
        out(std::string(title) + " (" + std::to_string(commits.size()) + ")",
            MG_LINE_SECTION);
        for (const auto &c : commits)
            out("  " + c.short_oid + " " + c.summary);
    };
    commit_section("Unpulled commits", v.unpulled);
    commit_section("Unpushed commits", v.unpushed);

    if (!v.stashes.empty()) {
        out("");
        out("Stashes (" + std::to_string(v.stashes.size()) + ")", MG_LINE_SECTION);
        for (const auto &s : v.stashes)
            out("  stash@{" + std::to_string(s.index) + "} " + s.message,
                MG_LINE_STASH, nullptr, static_cast<int>(s.index));
    }

    if (!v.recent.empty()) {
        out("");
        out("Recent commits", MG_LINE_SECTION);
        for (const auto &c : v.recent)
            out("  " + c.short_oid + " " + c.summary);
    }

    // Branches/tags are an overview, not an exhaustive ref dump: a big repo can
    // have thousands of tags (d20app: ~2900), and emitting them all bloats the
    // status buffer into tens of thousands of lines -- slow to build and slow to
    // render (the buffer appears to "hang"). Show a bounded head and a "+N more"
    // line; the full count stays in the section header. (Magit shows all refs in
    // a dedicated refs view, never inline in status.)
    constexpr std::size_t kMaxRefsShown = 20;

    if (!v.branches.empty()) {
        out("");
        out("Branches (" + std::to_string(v.branches.size()) + ")",
            MG_LINE_SECTION);
        std::size_t shown = 0;
        for (const auto &b : v.branches) {
            if (shown++ >= kMaxRefsShown)
                break;
            // NOTE: path = the branch NAME here (RET checks it out) -- unlike
            // *magit-refs* rows (mg_magit_refs_buffer), whose path is the tip OID.
            out(std::string(b.is_head ? "* " : "  ") + b.name, MG_LINE_BRANCH,
                b.name.c_str());
        }
        if (v.branches.size() > kMaxRefsShown)
            out("  ... and " + std::to_string(v.branches.size() - kMaxRefsShown) +
                " more", MG_LINE_OTHER);
    }

    if (!v.tags.empty()) {
        out("");
        out("Tags (" + std::to_string(v.tags.size()) + ")", MG_LINE_SECTION);
        std::size_t shown = 0;
        for (const auto &t : v.tags) {
            if (shown++ >= kMaxRefsShown)
                break;
            out("  " + t, MG_LINE_TAG, t.c_str());
        }
        if (v.tags.size() > kMaxRefsShown)
            out("  ... and " + std::to_string(v.tags.size() - kMaxRefsShown) +
                " more", MG_LINE_OTHER);
    }

    if (!v.worktrees.empty()) {
        out("");
        out("Worktrees (" + std::to_string(v.worktrees.size()) + ")",
            MG_LINE_SECTION);
        for (const auto &w : v.worktrees)
            out("  " + w.name + "  " + w.path, MG_LINE_WORKTREE, w.name.c_str());
    }

    if (!v.submodules.empty()) {
        out("");
        out("Submodules (" + std::to_string(v.submodules.size()) + ")",
            MG_LINE_SECTION);
        for (const auto &s : v.submodules)
            out("  " + s.path, MG_LINE_SUBMODULE, s.path.c_str());
    }

    return n;
}

extern "C" int mg_magit_status_buffer(const char *repo_path,
                                      const char *const *expanded,
                                      int n_expanded, mg_magit_emit_fn emit,
                                      void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;
    return compose_status_view(gather_status_view(repo_path), repo_path,
                               expanded, n_expanded, emit, ctx);
}

// Render the status buffer by replaying a *collapsed* snapshot (no inline
// diffs) and splicing each expanded file's diff back in on the fly. Output is
// byte-identical to mg_magit_status_buffer (shared emit_file_diff + the same
// collapsed composition).
//
// The collapsed base comes from the monitor thread's warm snapshot, so the
// expensive workdir scan + revwalk + ref enumeration never run on the UI
// thread. When the monitor has no snapshot yet (cold start, or the monitor is
// not running) we build it synchronously this once. If the snapshot predates
// the repo's current on-disk state (the user just staged/committed, or an
// external `git` ran) we render the latest snapshot anyway and prepend a
// "refreshing" marker; the monitor recomputes within ~one scan and the next
// redraw clears it. The expansions are always computed live on the UI thread
// (cheap, and they depend on UI state, not the snapshot).
extern "C" int mg_magit_status_snapshot(const char *repo_path,
                                        const char *const *expanded,
                                        int n_expanded, mg_magit_emit_fn emit,
                                        void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;

    std::vector<snap_line> snap;
    std::string snap_fp;
    bool stale = false;
    if (g_monitor && g_monitor->copy_snapshot(snap, snap_fp)) {
        stale = snap_fp != repo_fingerprint(repo_path);
    } else {
        // Cold start / monitor off: build the collapsed snapshot synchronously.
        (void)mg_magit_status_buffer(repo_path, nullptr, 0, snap_capture, &snap);
    }

    auto is_expanded = [&](const std::string &p) {
        for (int i = 0; i < n_expanded; ++i)
            if (expanded != nullptr && expanded[i] != nullptr && p == expanded[i])
                return true;
        return false;
    };

    int n = 0;
    if (stale) {
        emit(ctx, "    (refreshing...)", MG_LINE_OTHER, nullptr, -1);
        ++n;
    }

    // Emit the Diff: header live from the current (atomic) view config.
    // The snapshot's baked Diff: line reflects the monitor's last publish_view,
    // which may predate a set_diff_view call on the UI thread (repo_fingerprint
    // ignores the view config, so a toggle does NOT mark the snapshot stale).
    // Reading the atomic globals here always produces the current value and
    // keeps the header calculation off the monitor thread.
    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv =
            "Diff:     -U" + std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        emit(ctx, dv.c_str(), MG_LINE_OTHER, nullptr, -1);
        ++n;
    }

    for (const auto &e : snap) {
        // Skip baked Diff: lines — emitted live above from current atomics.
        if (e.line.rfind("Diff:", 0) == 0)
            continue;
        emit(ctx, e.line.c_str(), e.kind,
             e.path.empty() ? nullptr : e.path.c_str(), e.hunk);
        ++n;
        if ((e.kind == MG_LINE_UNSTAGED || e.kind == MG_LINE_STAGED) &&
            !e.path.empty() && is_expanded(e.path))
            n += emit_file_diff(repo_path, e.path.c_str(),
                                e.kind == MG_LINE_STAGED, emit, ctx);
    }
    return n;
}

extern "C" int mg_magit_log_buffer(const char *repo_path, int n_commits,
                                   mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr || n_commits <= 0)
        return 0;

    int n = 0;
    auto commits = mg::git::recent_commits(repo_path, n_commits);
    if (!commits)
        return 0;
    // FM-LOG-RICH: same "(refs)" decoration the CLI %d path shows.
    auto decs = mg::git::decorations(repo_path);
    for (const auto &c : *commits) {
        std::string text = c.short_oid + " ";
        if (decs) {
            auto it = decs->find(c.oid);
            if (it != decs->end())
                text += "(" + it->second + ") ";
        }
        text += c.summary;
        emit(ctx, text.c_str(), MG_LINE_COMMIT, c.oid.c_str(), -1);
        ++n;
    }
    return n;
}

extern "C" int mg_magit_reflog_buffer(const char *repo, int n,
                                      mg_magit_emit_fn emit, void *ctx)
{
    auto rl = mg::git::reflog(repo ? repo : ".", n);
    if (!rl)
        return 0;
    int count = 0;
    for (const auto &e : *rl) {
        std::string text = e.short_oid + " " + e.selector + " " + e.message;
        emit(ctx, text.c_str(), MG_LINE_COMMIT, e.oid.c_str(), 0);
        ++count;
    }
    return count;
}

extern "C" int mg_magit_refs_buffer(const char *repo_path,
                                    mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;
    auto r = mg::git::refs_overview(repo_path);
    if (!r)
        return 0;
    int n = 0;
    auto out = [&](const std::string &line, int kind, const char *path) {
        emit(ctx, line.c_str(), kind, path, -1);
        ++n;
    };
    // Header: named / detached / unborn.
    if (!r->head_name.empty())
        out("Refs (HEAD: " + r->head_name + ")", MG_LINE_SECTION, nullptr);
    else if (!r->head_oid.empty())
        out("Refs (HEAD detached at " + r->head_oid.substr(0, 8) + ")",
            MG_LINE_SECTION, nullptr);
    else
        out("Refs (no commits yet)", MG_LINE_SECTION, nullptr);

    // "ahead A, behind B" phrase; empty when 0/0 or unknown (-1).
    auto counts = [](const mg::git::ref_row &b) -> std::string {
        std::string s;
        if (b.ahead > 0)
            s += "ahead " + std::to_string(b.ahead);
        if (b.behind > 0)
            s += (s.empty() ? "" : ", ") + std::string("behind ") +
                 std::to_string(b.behind);
        return s;
    };
    auto ref_line = [&](const mg::git::ref_row &b) {
        std::string line = (b.is_head ? "* " : "  ") + b.name;
        // The starred row shows its upstream; others show counts vs HEAD.
        std::string ann = b.is_head ? r->upstream : counts(b);
        if (!ann.empty())
            line += "  " + ann;
        // NOTE: path = the tip OID (see bridge.h) -- RET shows the commit.
        out(line, MG_LINE_BRANCH, b.oid.c_str());
    };

    if (!r->locals.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Branches (" + std::to_string(r->locals.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &b : r->locals)
            ref_line(b);
    }
    if (!r->remotes.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Remotes (" + std::to_string(r->remotes.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &b : r->remotes)
            ref_line(b);
    }
    if (!r->tags.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Tags (" + std::to_string(r->tags.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &t : r->tags)
            out("  " + t.name, MG_LINE_TAG, t.oid.c_str());
    }
    return n;
}

extern "C" int mg_magit_process_log(mg_magit_emit_fn emit, void *ctx)
{
    auto entries = mg::magit::proclog::snapshot();
    if (entries.empty()) {
        emit(ctx, "No git operations recorded this session.", MG_LINE_OTHER,
             nullptr, 0);
        return 1;
    }
    int n = 0;
    // newest first
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto &e = *it;
        std::string head;
        if (e.kind == '$') {
            head = "$ " + e.command + "  (";
            // duration as seconds with one decimal, then ok/fail
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1fs", e.duration_ms / 1000.0);
            head += buf;
            head += e.ok ? ", ok)" : ", failed)";
        } else { // '~' -> libgit2
            head = "\xE2\x89\x88 " + e.command + "  (via libgit2"; // ≈ = U+2248
            head += e.ok ? ")" : ", failed)";
        }
        emit(ctx, head.c_str(), MG_LINE_PROCESS_CMD, nullptr, 0);
        ++n;
        // output lines, indented two spaces
        const std::string &out = e.output;
        std::size_t start = 0;
        while (start < out.size()) {
            std::size_t nl = out.find('\n', start);
            std::string line = out.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() || nl != std::string::npos) {
                emit(ctx, ("  " + line).c_str(), MG_LINE_PROCESS_OUT, nullptr, 0);
                ++n;
            }
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }
    return n;
}

extern "C" int mg_magit_log_file_buffer(const char *repo_path, const char *file,
                                        int n_commits, mg_magit_emit_fn emit,
                                        void *ctx)
{
    if (repo_path == nullptr || file == nullptr || emit == nullptr ||
        n_commits <= 0)
        return 0;

    int n = 0;
    auto commits = mg::git::log_file(repo_path, file, n_commits);
    if (!commits)
        return 0;
    // FM-LOG-RICH: same "(refs)" decoration the CLI %d path shows.
    auto decs = mg::git::decorations(repo_path);
    for (const auto &c : *commits) {
        std::string text = c.short_oid + " ";
        if (decs) {
            auto it = decs->find(c.oid);
            if (it != decs->end())
                text += "(" + it->second + ") ";
        }
        text += c.summary;
        emit(ctx, text.c_str(), MG_LINE_COMMIT, c.oid.c_str(), -1);
        ++n;
    }
    return n;
}

extern "C" int mg_magit_log_query_buffer(const struct mg_log_query *q,
                                         mg_magit_emit_fn emit, void *ctx)
{
    if (q == nullptr || q->repo == nullptr || emit == nullptr)
        return 0;

    mg::git::log_options opts;
    opts.graph = q->graph != 0;
    opts.max_count = q->n > 0 ? static_cast<std::size_t>(q->n) : 0;
    if (q->range) opts.range = q->range;
    if (q->file)  opts.file = q->file;
    if (q->pickaxe_kind == 'S' || q->pickaxe_kind == 'G') {
        opts.pickaxe = static_cast<char>(q->pickaxe_kind);
        opts.pickaxe_term = q->pickaxe_term ? q->pickaxe_term : "";
    }
    if (q->author) opts.author = q->author;
    if (q->grep)   opts.grep = q->grep;
    opts.all = q->all != 0;
    if (q->since) opts.since = q->since;
    if (q->until) opts.until = q->until;
    opts.reverse = q->reverse != 0;
    opts.merges = q->merges != 0;
    opts.no_merges = q->no_merges != 0;

    auto rows = mg::git::log_query(q->repo, std::move(opts));
    if (!rows)
        return 0;
    int count = 0;
    for (const auto &r : *rows) {
        if (r.oid.empty())
            emit(ctx, r.text.c_str(), MG_LINE_OTHER, nullptr, -1);
        else
            emit(ctx, r.text.c_str(), MG_LINE_COMMIT, r.oid.c_str(), -1);
        ++count;
    }
    return count;
}

extern "C" int mg_magit_commit_diff(const char *repo_path, const char *rev,
                                    mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || rev == nullptr || emit == nullptr)
        return 0;

    auto hunks = mg::git::commit_diff(repo_path, rev);
    if (!hunks)
        return 0;

    int n = 0;
    auto chomp = [](std::string s) {
        if (!s.empty() && s.back() == '\n')
            s.pop_back();
        return s;
    };
    auto out_no_path = [&](const std::string &line, int kind, int hunk) {
        emit(ctx, line.c_str(), kind, nullptr, hunk);
        ++n;
    };

    out_no_path(std::string("commit ") + rev, MG_LINE_SECTION, -1);
    if (mg::git::diff_view_context() != 3 || mg::git::diff_view_ignore_ws()) {
        std::string dv = "Diff:     -U" +
            std::to_string(mg::git::diff_view_context());
        if (mg::git::diff_view_ignore_ws())
            dv += " -w";
        out_no_path(dv, MG_LINE_OTHER, -1);
    }
    if (auto note = mg::git::read_note(repo_path, rev); note && !note->empty()) {
        std::string nb = *note;
        if (!nb.empty() && nb.back() == '\n')
            nb.pop_back();
        out_no_path("Note:", MG_LINE_OTHER, -1);
        out_no_path("  " + nb, MG_LINE_OTHER, -1);
    }
    for (int hi = 0; hi < static_cast<int>(hunks->size()); ++hi) {
        const auto &h = (*hunks)[hi];
        const char *path = h.path.empty() ? nullptr : h.path.c_str();
        emit(ctx, chomp(h.header).c_str(), MG_LINE_HUNK, path, hi);
        ++n;
        for (const auto &l : h.lines) {
            emit(ctx,
                 (std::string(1, l.origin) + chomp(l.content)).c_str(),
                 MG_LINE_DIFF, path, hi);
            ++n;
        }
    }
    return n;
}

extern "C" int mg_magit_stage(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    bool ok = mg::git::stage(repo_path, path).has_value();
    proc_eq(std::string("git add ") + path, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_unstage(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    bool ok = mg::git::unstage(repo_path, path).has_value();
    proc_eq(std::string("git reset HEAD -- ") + path, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_discard(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    bool ok = mg::git::discard(repo_path, path).has_value();
    proc_eq(std::string("git checkout -- ") + path, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_stage_all(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    bool ok = mg::git::stage_all(repo_path).has_value();
    proc_eq("git add -A", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_unstage_all(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    bool ok = mg::git::unstage_all(repo_path).has_value();
    proc_eq("git reset HEAD", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_commit(const char *repo_path, const char *message)
{
    if (repo_path == nullptr || message == nullptr)
        return 0;
    return mg::git::commit(repo_path, message).has_value() ? 1 : 0;
}

extern "C" int mg_magit_commit_amend(const char *repo_path, const char *message)
{
    if (repo_path == nullptr || message == nullptr)
        return 0;
    return mg::git::commit_amend(repo_path, message).has_value() ? 1 : 0;
}

extern "C" int mg_magit_commit_extend(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::commit_extend(repo_path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_commit_reword(const char *repo_path, const char *message)
{
    if (repo_path == nullptr || message == nullptr)
        return 0;
    return mg::git::commit_reword(repo_path, message).has_value() ? 1 : 0;
}

extern "C" int mg_magit_head_message(const char *repo_path, char *buf,
                                     size_t buflen)
{
    if (repo_path == nullptr || buf == nullptr || buflen == 0)
        return 0;
    auto m = mg::git::head_message(repo_path);
    if (!m)
        return 0;
    std::size_t len = m->size();
    if (len >= buflen)
        len = buflen - 1;
    std::memcpy(buf, m->data(), len);
    buf[len] = '\0';
    return static_cast<int>(len);
}

extern "C" void mg_magit_set_diff_view(int context, int ignore_ws)
{
    mg::git::set_diff_view(context, ignore_ws != 0);
}

extern "C" int mg_magit_stage_hunk(const char *repo_path, const char *path,
                                   int hunk)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
    return mg::git::stage_hunk(repo_path, path, hunk).has_value() ? 1 : 0;
}

extern "C" int mg_magit_unstage_hunk(const char *repo_path, const char *path,
                                     int hunk)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
    return mg::git::unstage_hunk(repo_path, path, hunk).has_value() ? 1 : 0;
}

extern "C" int mg_magit_stage_region(const char *repo_path, const char *path,
                                     int hunk, int first, int last)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0 || first < 0 ||
        last < first)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
    return mg::git::stage_region(repo_path, path, hunk, first, last).has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_unstage_region(const char *repo_path, const char *path,
                                       int hunk, int first, int last)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0 || first < 0 ||
        last < first)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
    return mg::git::unstage_region(repo_path, path, hunk, first, last)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_discard_region(const char *repo_path, const char *path,
                                       int hunk, int first, int last)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0 || first < 0 ||
        last < first)
        return 0;
    if (mg::git::diff_view_ignore_ws())
        return -2;   // can't stage from a whitespace-ignored diff
    return mg::git::discard_region(repo_path, path, hunk, first, last)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_stash_apply(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    bool ok = mg::git::stash_apply(repo_path, index).has_value();
    proc_eq("git stash apply", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_stash_drop(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    bool ok = mg::git::stash_drop(repo_path, index).has_value();
    proc_eq("git stash drop", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_stash_push(const char *repo_path, const char *message)
{
    if (repo_path == nullptr)
        return 0;
    bool ok = mg::git::stash_push(repo_path, message == nullptr ? "" : message)
                      .has_value();
    proc_eq("git stash push", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_stash_pop(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    bool ok = mg::git::stash_pop(repo_path, index).has_value();
    proc_eq("git stash pop", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_checkout(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr)
        return 0;
    bool ok = mg::git::checkout_branch(repo_path, name).has_value();
    proc_eq(std::string("git checkout ") + name, ok);
    return ok ? 1 : 0;
}

// The UI's credential prompt, registered by the C core; adapted to the engine's
// cred_prompt signature (which carries an unused udata) by this thunk.
static mg_magit_cred_prompt_fn g_ui_cred_prompt = nullptr;

static int cred_prompt_thunk(const char *prompt, int hidden, char *out,
                             int outlen, void *)
{
    return g_ui_cred_prompt ? g_ui_cred_prompt(prompt, hidden, out, outlen) : 0;
}

extern "C" void mg_magit_set_cred_prompt(mg_magit_cred_prompt_fn fn)
{
    g_ui_cred_prompt = fn;
    mg::git::set_cred_prompt(fn ? cred_prompt_thunk : nullptr, nullptr);
}

extern "C" int mg_magit_fetch(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::fetch_remote(repo_path, remote ? remote : "origin")
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_push(const char *repo_path, const char *remote,
                             int force, int set_upstream)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::push_remote(repo_path, remote ? remote : "origin",
                                force != 0, set_upstream != 0)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_pull(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return apply_code(
        mg::git::pull_remote(repo_path, remote ? remote : "origin"));
}

extern "C" int mg_magit_pull_rebase(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return rebase_code(mg::git::pull_rebase(repo_path, remote ? remote : "origin"));
}

// FM-GIT-CLI-WRITES P3: the CLI (terminal-inherit) variants. The caller puts
// the tty in cooked mode first; these just build the argv and run real git.
extern "C" int mg_magit_fetch_cli(const char *repo_path, int prune, int tags,
                                  int all)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"fetch"};
    if (all)   args.emplace_back("--all");
    if (prune) args.emplace_back("--prune");
    if (tags)  args.emplace_back("--tags");
    if (!all)  args.emplace_back("origin"); // --all fetches every remote
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}

extern "C" int mg_magit_push_cli(const char *repo_path, int force,
                                 int set_upstream, int tags)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"push"};
    if (force)        args.emplace_back("--force-with-lease");
    if (set_upstream) args.emplace_back("-u");
    if (tags)         args.emplace_back("--tags");
    args.emplace_back("origin");
    args.emplace_back("HEAD");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}

extern "C" int mg_magit_push_dry_run(const char *repo_path, int force,
                                     int set_upstream, int tags)
{
    if (repo_path == nullptr)
        return -1;
    return mg::git::push_dry_run(repo_path, force != 0, set_upstream != 0,
                                 tags != 0);
}

extern "C" int mg_magit_pull_cli(const char *repo_path, int rebase,
                                 int autostash, int ff_only)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"pull"};
    if (rebase)    args.emplace_back("--rebase");
    if (autostash) args.emplace_back("--autostash");
    if (ff_only)   args.emplace_back("--ff-only");
    args.emplace_back("--no-edit");
    args.emplace_back("origin");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}

extern "C" int mg_magit_reset(const char *repo_path, const char *rev, int mode)
{
    if (repo_path == nullptr || rev == nullptr || mode < 0 || mode > 2)
        return 0;
    const mg::git::reset_mode m = mode == 0   ? mg::git::reset_mode::soft
                                  : mode == 2 ? mg::git::reset_mode::hard
                                              : mg::git::reset_mode::mixed;
    bool ok = mg::git::reset_to(repo_path, rev, m).has_value();
    const char *mode_word = mode == 0 ? "--soft" : mode == 2 ? "--hard" : "--mixed";
    proc_eq(std::string("git reset ") + mode_word + " " + rev, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_revert(const char *repo_path, const char *rev)
{
    if (repo_path == nullptr || rev == nullptr)
        return 0;
    return apply_code(mg::git::revert_commit(repo_path, rev));
}

extern "C" int mg_magit_merge(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return apply_code(mg::git::merge_branch(repo_path, name));
}

// Map a rebase outcome to the bridge code: 1 done, 2 paused on conflicts,
// 0 failure.

extern "C" int mg_magit_tag_create(const char *repo_path, const char *name,
                                   const char *target, const char *message)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    bool ok = mg::git::create_tag(repo_path, name, target ? target : "HEAD",
                                  message ? message : "")
                      .has_value();
    proc_eq(std::string("git tag ") + name, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_tag_delete(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    bool ok = mg::git::delete_tag(repo_path, name).has_value();
    proc_eq(std::string("git tag -d ") + name, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_blame_file(const char *repo_path, const char *path,
                                   mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || path == nullptr || emit == nullptr)
        return 0;
    auto bl = mg::git::blame_file(repo_path, path);
    if (!bl)
        return 0;
    int n = 0;
    for (const auto &l : *bl) {
        std::string author = l.author;
        if (author.size() > 16)
            author.resize(16);
        author.resize(16, ' '); // pad for column alignment
        emit(ctx, (l.short_oid + " " + author + " " + l.text).c_str(),
             MG_LINE_OTHER, nullptr, -1);
        ++n;
    }
    return n;
}

extern "C" int mg_magit_ignore(const char *repo_path, const char *pattern)
{
    if (repo_path == nullptr || pattern == nullptr || pattern[0] == '\0')
        return 0;
    bool ok = mg::git::ignore_path(repo_path, pattern).has_value();
    proc_eq(std::string("echo ") + pattern + " >> .gitignore", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_resolve_conflict(const char *repo_path,
                                         const char *path, int take_theirs)
{
    if (repo_path == nullptr || path == nullptr || path[0] == '\0')
        return 0;
    auto side = take_theirs ? mg::git::conflict_side::theirs
                            : mg::git::conflict_side::ours;
    return mg::git::resolve_conflict(repo_path, path, side).has_value() ? 1 : 0;
}

extern "C" int mg_magit_conflict_hunks(const char *repo_path, const char *path,
                                       mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || path == nullptr || emit == nullptr)
        return 0;
    auto hs = mg::git::conflict_hunks(repo_path, path);
    if (!hs)
        return 0;

    int n = 0;
    auto emit_block = [&](int idx, const char *tag, const std::string &text) {
        std::string label = std::string("  ") + tag;
        emit(ctx, label.c_str(), MG_LINE_CONFLICT_HUNK, path, idx);
        ++n;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t nl = text.find('\n', pos);
            std::size_t end = nl == std::string::npos ? text.size() : nl;
            std::string line = "    " + text.substr(pos, end - pos);
            emit(ctx, line.c_str(), MG_LINE_CONFLICT_HUNK, path, idx);
            ++n;
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
    };
    for (int i = 0; i < static_cast<int>(hs->size()); ++i) {
        std::string header = "Conflict " + std::to_string(i + 1) + "/" +
                             std::to_string(hs->size()) +
                             "  (a ours / b theirs / RET both)";
        emit(ctx, header.c_str(), MG_LINE_CONFLICT_HUNK, path, i);
        ++n;
        emit_block(i, "<<< ours", (*hs)[i].ours);
        emit_block(i, "=== theirs", (*hs)[i].theirs);
        emit(ctx, "", MG_LINE_CONFLICT_HUNK, path, i);
        ++n;
    }
    return n;
}

extern "C" int mg_magit_conflict_hunk_side(const char *repo_path,
                                           const char *path, int index,
                                           int side, mg_magit_emit_fn emit,
                                           void *ctx)
{
    if (repo_path == nullptr || path == nullptr || emit == nullptr ||
        index < 0 || side < 0 || side > 1)
        return 0;
    auto hs = mg::git::conflict_hunks(repo_path, path);
    if (!hs || index >= static_cast<int>(hs->size()))
        return 0;
    // Word-level refinement: wrap the words unique to this side (ediff's
    // intra-line highlight, rendered textually since cells are whole-line color).
    const std::string &mine = side == 0 ? (*hs)[index].ours : (*hs)[index].theirs;
    const std::string &other = side == 0 ? (*hs)[index].theirs : (*hs)[index].ours;
    const std::string text = side == 0 ? mg::git::refine_words(mine, other, "[-", "-]")
                                       : mg::git::refine_words(mine, other, "{+", "+}");

    int n = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::size_t end = nl == std::string::npos ? text.size() : nl;
        emit(ctx, text.substr(pos, end - pos).c_str(), MG_LINE_CONFLICT_HUNK,
             path, index);
        ++n;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return n;
}

extern "C" int mg_magit_resolve_conflict_hunk(const char *repo_path,
                                              const char *path, int index,
                                              int side)
{
    if (repo_path == nullptr || path == nullptr || path[0] == '\0' ||
        index < 0 || side < 0 || side > 2)
        return 0;
    const mg::git::conflict_side s = side == 0   ? mg::git::conflict_side::ours
                                     : side == 1 ? mg::git::conflict_side::theirs
                                                 : mg::git::conflict_side::both;
    return mg::git::resolve_conflict_hunk(repo_path, path,
                                          static_cast<std::size_t>(index), s)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_note_set(const char *repo_path, const char *rev,
                                 const char *message)
{
    if (repo_path == nullptr || rev == nullptr || message == nullptr)
        return 0;
    bool ok = mg::git::set_note(repo_path, rev, message).has_value();
    proc_eq("git notes add", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_note_remove(const char *repo_path, const char *rev)
{
    if (repo_path == nullptr || rev == nullptr)
        return 0;
    bool ok = mg::git::remove_note(repo_path, rev).has_value();
    proc_eq("git notes remove", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_worktree_add(const char *repo_path, const char *name,
                                     const char *path)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0' ||
        path == nullptr || path[0] == '\0')
        return 0;
    return mg::git::add_worktree(repo_path, name, path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_worktree_remove(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return mg::git::remove_worktree(repo_path, name).has_value() ? 1 : 0;
}

extern "C" int mg_magit_cherrypick(const char *repo_path, const char *rev)
{
    if (repo_path == nullptr || rev == nullptr || rev[0] == '\0')
        return 0;
    return apply_code(mg::git::cherry_pick(repo_path, rev));
}

// Copy `msg` into the caller's NUL-terminated `out` (size `outlen`).
static void fill_msg(const std::string &msg, char *out, int outlen)
{
    if (out == nullptr || outlen <= 0)
        return;
    int n = static_cast<int>(msg.size());
    if (n > outlen - 1)
        n = outlen - 1;
    for (int i = 0; i < n; ++i)
        out[i] = msg[i];
    out[n] = '\0';
}

extern "C" int mg_magit_bisect_start(const char *repo_path, const char *bad,
                                     const char *good, char *out, int outlen)
{
    if (repo_path == nullptr || bad == nullptr || bad[0] == '\0' ||
        good == nullptr || good[0] == '\0')
        return 0;
    auto r = mg::git::bisect_start(repo_path, bad, good);
    bool ok = r.has_value();
    proc_eq("git bisect start", ok);
    if (!ok)
        return 0;
    fill_msg(*r, out, outlen);
    return 1;
}

extern "C" int mg_magit_bisect_mark(const char *repo_path, int is_bad,
                                    char *out, int outlen)
{
    if (repo_path == nullptr)
        return 0;
    auto r = mg::git::bisect_mark(repo_path, is_bad != 0);
    bool ok = r.has_value();
    proc_eq(is_bad ? "git bisect bad" : "git bisect good", ok);
    if (!ok)
        return 0;
    fill_msg(*r, out, outlen);
    return 1;
}

extern "C" int mg_magit_bisect_reset(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    bool ok = mg::git::bisect_reset(repo_path).has_value();
    proc_eq("git bisect reset", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_bisect_active(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::bisect_active(repo_path) ? 1 : 0;
}

extern "C" int mg_magit_rebase(const char *repo_path, const char *upstream)
{
    if (repo_path == nullptr || upstream == nullptr || upstream[0] == '\0')
        return 0;
    auto r = mg::git::rebase_onto(repo_path, upstream);
    int code = rebase_code(r);
    proc_eq(std::string("git rebase ") + upstream, code != 0);
    return code;
}

extern "C" int mg_magit_rebase_continue(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    auto r = mg::git::rebase_continue(repo_path);
    int code = rebase_code(r);
    proc_eq("git rebase --continue", code != 0);
    return code;
}

extern "C" int mg_magit_rebase_skip(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    auto r = mg::git::rebase_skip(repo_path);
    int code = rebase_code(r);
    proc_eq("git rebase --skip", code != 0);
    return code;
}

extern "C" int mg_magit_rebase_abort(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    bool ok = mg::git::rebase_abort(repo_path).has_value();
    proc_eq("git rebase --abort", ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_rebase_in_progress(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::rebase_in_progress(repo_path) ? 1 : 0;
}

extern "C" int mg_magit_rebase_todo(const char *repo_path, const char *onto,
                                    mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || onto == nullptr || emit == nullptr)
        return 0;
    auto cs = mg::git::commits_range(repo_path, onto);
    if (!cs)
        return 0;
    int n = 0;
    for (const auto &c : *cs) {
        emit(ctx, (c.short_oid + " " + c.summary).c_str(), MG_LINE_COMMIT,
             c.oid.c_str(), -1);
        ++n;
    }
    return n;
}

extern "C" int mg_magit_rebase_interactive(const char *repo_path,
                                           const char *onto,
                                           const struct mg_magit_rebase_step *steps,
                                           int n)
{
    if (repo_path == nullptr || onto == nullptr || (n > 0 && steps == nullptr))
        return 0;
    std::vector<mg::git::rebase_step> plan;
    plan.reserve(n > 0 ? n : 0);
    for (int i = 0; i < n; ++i) {
        mg::git::rebase_action a;
        switch (steps[i].action) {
        case 1: a = mg::git::rebase_action::drop; break;
        case 2: a = mg::git::rebase_action::squash; break;
        case 3: a = mg::git::rebase_action::fixup; break;
        case 4: a = mg::git::rebase_action::reword; break;
        case 5: a = mg::git::rebase_action::edit; break;
        default: a = mg::git::rebase_action::pick; break;
        }
        plan.push_back({a, steps[i].oid ? steps[i].oid : "",
                        steps[i].message ? steps[i].message : ""});
    }
    auto r = mg::git::rebase_interactive(repo_path, onto, std::move(plan));
    int code = rebase_code(r);
    proc_eq(std::string("git rebase -i ") + onto, code != 0);
    return code;
}

extern "C" int mg_magit_branch_create(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    bool ok = mg::git::create_branch(repo_path, name).has_value();
    proc_eq(std::string("git branch ") + name, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_branch_delete(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    bool ok = mg::git::delete_branch(repo_path, name).has_value();
    proc_eq(std::string("git branch -D ") + name, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_branch_rename(const char *repo_path, const char *from,
                                      const char *to)
{
    if (repo_path == nullptr || from == nullptr || to == nullptr ||
        from[0] == '\0' || to[0] == '\0')
        return 0;
    bool ok = mg::git::rename_branch(repo_path, from, to).has_value();
    proc_eq(std::string("git branch -m ") + from + " " + to, ok);
    return ok ? 1 : 0;
}

extern "C" int mg_magit_git_command(const char *repo, const char *cmdline)
{
    if (repo == nullptr || cmdline == nullptr || cmdline[0] == '\0')
        return -1;
    return mg::git::run_git_command(repo, cmdline);
}
