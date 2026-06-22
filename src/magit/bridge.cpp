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
        // Watch the worktree root and .git (catches edits, staging, commits).
        std::vector<std::string> paths{repo_, repo_ + "/.git"};
        if (auto w = mg::fswatch::watcher::create(paths))
            watcher_.emplace(std::move(*w));
        thread_ = std::thread([this] { run(); });
    }

    ~monitor()
    {
        stop_.request_stop();
        if (watcher_)
            watcher_->wake(); // release a blocked wait() immediately
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
    void publish()
    {
        std::string line;
        if (auto head = mg::git::read_head(repo_);
            head && !head->branch.empty())
            line = head->branch + " ";

        auto st = mg::git::repo_status(repo_);
        line += st ? mg::magit::summarize(*st) : std::string("git ?");

        // Full collapsed status snapshot, computed here on the worker thread so
        // the UI's status build is a cheap replay. Stamp it with the fingerprint
        // read BEFORE the scan: if a mutation lands mid-scan the snapshot looks
        // stale and the UI just re-reconciles (never the reverse).
        std::string fp = repo_fingerprint(repo_);
        std::vector<snap_line> snap;
        (void)mg_magit_status_buffer(repo_.c_str(), nullptr, 0, snap_capture,
                                     &snap);
        {
            std::lock_guard lk(mu_);
            current_ = std::move(line);
            snapshot_ = std::move(snap);
            snapshot_fp_ = std::move(fp);
            have_snapshot_ = true;
        }
        dirty_.store(true);

        // Nudge the UI out of its blocking poll() so it redraws even when idle.
        g_wake.signal();
    }

    void run()
    {
        publish(); // initial read, before any event
        if (!watcher_)
            return;
        for (auto ev : mg::fswatch::watch_stream(*watcher_, stop_)) {
            (void)ev;
            publish();
        }
    }

    std::string repo_;
    std::mutex mu_;
    std::string current_;
    std::vector<snap_line> snapshot_;
    std::string snapshot_fp_;
    bool have_snapshot_ = false;
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

} // namespace

extern "C" void mg_magit_start(const char *repo_path)
{
    g_wake.open_pipe(); // before any worker thread that may signal it
    g_monitor = std::make_unique<monitor>(repo_path ? repo_path : ".");
    g_jobs = std::make_unique<job_runner>();
}

extern "C" void mg_magit_stop(void)
{
    g_jobs.reset();    // joins the worker thread
    g_monitor.reset(); // joins the monitor thread
    g_wake.close_pipe(); // no thread touches g_wake after the joins
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

extern "C" int mg_magit_status_buffer(const char *repo_path,
                                      const char *const *expanded,
                                      int n_expanded, mg_magit_emit_fn emit,
                                      void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;

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

    if (auto head = mg::git::read_head(repo_path)) {
        out("On branch " +
            (head->branch.empty() ? std::string("(unknown)") : head->branch));
        if (!head->short_oid.empty())
            out("Head:     " + head->short_oid + " " + head->summary);
    }

    if (auto up = mg::git::upstream_status(repo_path); up && up->has_upstream) {
        std::string line = "Upstream: " + up->name;
        if (up->ahead != 0 || up->behind != 0)
            line += " [ahead " + std::to_string(up->ahead) + ", behind " +
                    std::to_string(up->behind) + "]";
        out(line);
    }

    if (mg::git::rebase_in_progress(repo_path)) {
        out("");
        out("Rebasing -- resolve conflicts, then r r (continue) / r s (skip) / "
            "r a (abort)",
            MG_LINE_SECTION);
    }

    if (mg::git::bisect_active(repo_path)) {
        out("");
        out("Bisecting -- test the checked-out commit, then Z b (bad) / Z g "
            "(good); Z r resets",
            MG_LINE_SECTION);
    }

    if (auto st = mg::git::repo_status(repo_path)) {
        using S = mg::magit::status;
        std::vector<const mg::magit::file_status *> untracked, unstaged, staged;
        for (const auto &e : *st) {
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
                           const std::vector<const mg::magit::file_status *> &v,
                           bool labeled, bool use_index, int kind, bool diffable) {
            if (v.empty())
                return;
            out("");
            out(std::string(title) + " (" + std::to_string(v.size()) + ")",
                MG_LINE_SECTION);
            for (const auto *e : v) {
                std::string text =
                    labeled ? "  " + std::string(state_word(use_index ? e->index
                                                                      : e->worktree)) +
                                  "  " + e->path
                            : "  " + e->path;
                out(text, kind, e->path.c_str());
                if (diffable && is_expanded(e->path))
                    emit_diff(e->path, use_index);
            }
        };
        if (auto cf = mg::git::conflicts(repo_path); cf && !cf->empty()) {
            out("");
            out("Conflicts (" + std::to_string(cf->size()) +
                    ") -- e o/e t whole file - E ediff (per region) - RET edit",
                MG_LINE_SECTION);
            for (const auto &c : *cf)
                out("  " + c.path, MG_LINE_CONFLICT, c.path.c_str());
        }
        section("Untracked files", untracked, false, false, MG_LINE_UNTRACKED, false);
        section("Unstaged changes", unstaged, true, false, MG_LINE_UNSTAGED, true);
        section("Staged changes", staged, true, true, MG_LINE_STAGED, true);
    }

    auto commit_section = [&](const char *title,
                              std::vector<mg::git::commit_brief> &&commits) {
        if (commits.empty())
            return;
        out("");
        out(std::string(title) + " (" + std::to_string(commits.size()) + ")",
            MG_LINE_SECTION);
        for (const auto &c : commits)
            out("  " + c.short_oid + " " + c.summary);
    };
    if (auto up = mg::git::upstream_commits(repo_path, /*unpushed=*/false))
        commit_section("Unpulled commits", std::move(*up));
    if (auto up = mg::git::upstream_commits(repo_path, /*unpushed=*/true))
        commit_section("Unpushed commits", std::move(*up));

    if (auto stashes = mg::git::stashes(repo_path);
        stashes && !stashes->empty()) {
        out("");
        out("Stashes (" + std::to_string(stashes->size()) + ")",
            MG_LINE_SECTION);
        for (const auto &s : *stashes)
            out("  stash@{" + std::to_string(s.index) + "} " + s.message,
                MG_LINE_STASH, nullptr, static_cast<int>(s.index));
    }

    if (auto commits = mg::git::recent_commits(repo_path, 10);
        commits && !commits->empty()) {
        out("");
        out("Recent commits", MG_LINE_SECTION);
        for (const auto &c : *commits)
            out("  " + c.short_oid + " " + c.summary);
    }

    if (auto branches = mg::git::branches(repo_path);
        branches && !branches->empty()) {
        out("");
        out("Branches (" + std::to_string(branches->size()) + ")",
            MG_LINE_SECTION);
        for (const auto &b : *branches)
            out(std::string(b.is_head ? "* " : "  ") + b.name, MG_LINE_BRANCH,
                b.name.c_str());
    }

    if (auto tags = mg::git::tags(repo_path); tags && !tags->empty()) {
        out("");
        out("Tags (" + std::to_string(tags->size()) + ")", MG_LINE_SECTION);
        for (const auto &t : *tags)
            out("  " + t, MG_LINE_TAG, t.c_str());
    }

    if (auto wts = mg::git::worktrees(repo_path); wts && !wts->empty()) {
        out("");
        out("Worktrees (" + std::to_string(wts->size()) + ")", MG_LINE_SECTION);
        for (const auto &w : *wts)
            out("  " + w.name + "  " + w.path, MG_LINE_WORKTREE, w.name.c_str());
    }

    if (auto subs = mg::git::submodules(repo_path); subs && !subs->empty()) {
        out("");
        out("Submodules (" + std::to_string(subs->size()) + ")",
            MG_LINE_SECTION);
        for (const auto &s : *subs)
            out("  " + s.path, MG_LINE_SUBMODULE, s.path.c_str());
    }

    return n;
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
    for (const auto &e : snap) {
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
    for (const auto &c : *commits) {
        emit(ctx, (c.short_oid + " " + c.summary).c_str(), MG_LINE_COMMIT,
             c.oid.c_str(), -1);
        ++n;
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
    for (const auto &c : *commits) {
        emit(ctx, (c.short_oid + " " + c.summary).c_str(), MG_LINE_COMMIT,
             c.oid.c_str(), -1);
        ++n;
    }
    return n;
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
    auto out = [&](const std::string &line, int kind, int hunk) {
        emit(ctx, line.c_str(), kind, nullptr, hunk);
        ++n;
    };
    auto chomp = [](std::string s) {
        if (!s.empty() && s.back() == '\n')
            s.pop_back();
        return s;
    };

    out(std::string("commit ") + rev, MG_LINE_SECTION, -1);
    if (auto note = mg::git::read_note(repo_path, rev); note && !note->empty()) {
        std::string nb = *note;
        if (!nb.empty() && nb.back() == '\n')
            nb.pop_back();
        out("Note:", MG_LINE_OTHER, -1);
        out("  " + nb, MG_LINE_OTHER, -1);
    }
    for (int hi = 0; hi < static_cast<int>(hunks->size()); ++hi) {
        out(chomp((*hunks)[hi].header), MG_LINE_HUNK, hi);
        for (const auto &l : (*hunks)[hi].lines)
            out(std::string(1, l.origin) + chomp(l.content), MG_LINE_DIFF, hi);
    }
    return n;
}

extern "C" int mg_magit_stage(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    return mg::git::stage(repo_path, path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_unstage(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    return mg::git::unstage(repo_path, path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_discard(const char *repo_path, const char *path)
{
    if (repo_path == nullptr || path == nullptr)
        return 0;
    return mg::git::discard(repo_path, path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_stage_all(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::stage_all(repo_path).has_value() ? 1 : 0;
}

extern "C" int mg_magit_unstage_all(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::unstage_all(repo_path).has_value() ? 1 : 0;
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

extern "C" int mg_magit_stage_hunk(const char *repo_path, const char *path,
                                   int hunk)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0)
        return 0;
    return mg::git::stage_hunk(repo_path, path, hunk).has_value() ? 1 : 0;
}

extern "C" int mg_magit_unstage_hunk(const char *repo_path, const char *path,
                                     int hunk)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0)
        return 0;
    return mg::git::unstage_hunk(repo_path, path, hunk).has_value() ? 1 : 0;
}

extern "C" int mg_magit_stage_region(const char *repo_path, const char *path,
                                     int hunk, int first, int last)
{
    if (repo_path == nullptr || path == nullptr || hunk < 0 || first < 0 ||
        last < first)
        return 0;
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
    return mg::git::discard_region(repo_path, path, hunk, first, last)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_stash_apply(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    return mg::git::stash_apply(repo_path, index).has_value() ? 1 : 0;
}

extern "C" int mg_magit_stash_drop(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    return mg::git::stash_drop(repo_path, index).has_value() ? 1 : 0;
}

extern "C" int mg_magit_stash_push(const char *repo_path, const char *message)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::stash_push(repo_path, message == nullptr ? "" : message)
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_stash_pop(const char *repo_path, int index)
{
    if (repo_path == nullptr || index < 0)
        return 0;
    return mg::git::stash_pop(repo_path, index).has_value() ? 1 : 0;
}

extern "C" int mg_magit_checkout(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr)
        return 0;
    return mg::git::checkout_branch(repo_path, name).has_value() ? 1 : 0;
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

extern "C" int mg_magit_reset(const char *repo_path, const char *rev, int mode)
{
    if (repo_path == nullptr || rev == nullptr || mode < 0 || mode > 2)
        return 0;
    const mg::git::reset_mode m = mode == 0   ? mg::git::reset_mode::soft
                                  : mode == 2 ? mg::git::reset_mode::hard
                                              : mg::git::reset_mode::mixed;
    return mg::git::reset_to(repo_path, rev, m).has_value() ? 1 : 0;
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
    return mg::git::create_tag(repo_path, name, target ? target : "HEAD",
                               message ? message : "")
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_tag_delete(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return mg::git::delete_tag(repo_path, name).has_value() ? 1 : 0;
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
    return mg::git::ignore_path(repo_path, pattern).has_value() ? 1 : 0;
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
    return mg::git::set_note(repo_path, rev, message).has_value() ? 1 : 0;
}

extern "C" int mg_magit_note_remove(const char *repo_path, const char *rev)
{
    if (repo_path == nullptr || rev == nullptr)
        return 0;
    return mg::git::remove_note(repo_path, rev).has_value() ? 1 : 0;
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
    if (!r)
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
    if (!r)
        return 0;
    fill_msg(*r, out, outlen);
    return 1;
}

extern "C" int mg_magit_bisect_reset(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::bisect_reset(repo_path).has_value() ? 1 : 0;
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
    return rebase_code(mg::git::rebase_onto(repo_path, upstream));
}

extern "C" int mg_magit_rebase_continue(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return rebase_code(mg::git::rebase_continue(repo_path));
}

extern "C" int mg_magit_rebase_skip(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return rebase_code(mg::git::rebase_skip(repo_path));
}

extern "C" int mg_magit_rebase_abort(const char *repo_path)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::rebase_abort(repo_path).has_value() ? 1 : 0;
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
    return rebase_code(
        mg::git::rebase_interactive(repo_path, onto, std::move(plan)));
}

extern "C" int mg_magit_branch_create(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return mg::git::create_branch(repo_path, name).has_value() ? 1 : 0;
}

extern "C" int mg_magit_branch_delete(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return mg::git::delete_branch(repo_path, name).has_value() ? 1 : 0;
}

extern "C" int mg_magit_branch_rename(const char *repo_path, const char *from,
                                      const char *to)
{
    if (repo_path == nullptr || from == nullptr || to == nullptr ||
        from[0] == '\0' || to[0] == '\0')
        return 0;
    return mg::git::rename_branch(repo_path, from, to).has_value() ? 1 : 0;
}
