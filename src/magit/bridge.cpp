// bridge.cpp -- C++ implementation of the magit extern "C" bridge (task M2d-1).
//
// A regular TU (not a module) that imports the engine modules and exposes the
// plain-C API in bridge.h. A background thread runs the M2a-M2c pipeline
// (watch -> libgit2 status -> summarize) and publishes the modeline string;
// the C core reads it through the bridge. Cancellation is instant via the
// wakeable watcher (stop_flag + wake()).

#include "bridge.h"

#include <atomic>
#include <cstring>
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

namespace {

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
        {
            std::lock_guard lk(mu_);
            current_ = std::move(line);
        }
        dirty_.store(true);
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
    std::atomic<bool> dirty_{false};
    mg::stop_flag stop_;
    std::optional<mg::fswatch::watcher> watcher_;
    std::thread thread_;
};

std::unique_ptr<monitor> g_monitor;

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
    g_monitor = std::make_unique<monitor>(repo_path ? repo_path : ".");
}

extern "C" void mg_magit_stop(void) { g_monitor.reset(); }

extern "C" int mg_magit_take_dirty(void)
{
    return g_monitor ? g_monitor->take_dirty() : 0;
}

extern "C" int mg_magit_modeline(char *buf, size_t buflen)
{
    return g_monitor ? g_monitor->modeline(buf, buflen) : 0;
}

extern "C" int mg_magit_status_buffer(const char *repo_path,
                                      mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;

    int n = 0;
    auto out = [&](const std::string &line, int kind = MG_LINE_OTHER,
                   const char *path = nullptr) {
        emit(ctx, line.c_str(), kind, path);
        ++n;
    };

    if (auto head = mg::git::read_head(repo_path)) {
        out("On branch " +
            (head->branch.empty() ? std::string("(unknown)") : head->branch));
        if (!head->short_oid.empty())
            out("Head:     " + head->short_oid + " " + head->summary);
    }

    if (auto st = mg::git::repo_status(repo_path)) {
        using S = mg::magit::status;
        std::vector<const mg::magit::file_status *> untracked, unstaged, staged;
        for (const auto &e : *st) {
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
                           bool labeled, bool use_index, int kind) {
            if (v.empty())
                return;
            out("");
            out(std::string(title) + " (" + std::to_string(v.size()) + ")");
            for (const auto *e : v) {
                std::string text =
                    labeled ? "  " + std::string(state_word(use_index ? e->index
                                                                      : e->worktree)) +
                                  "  " + e->path
                            : "  " + e->path;
                out(text, kind, e->path.c_str());
            }
        };
        section("Untracked files", untracked, false, false, MG_LINE_UNTRACKED);
        section("Unstaged changes", unstaged, true, false, MG_LINE_UNSTAGED);
        section("Staged changes", staged, true, true, MG_LINE_STAGED);
    }

    if (auto commits = mg::git::recent_commits(repo_path, 10);
        commits && !commits->empty()) {
        out("");
        out("Recent commits");
        for (const auto &c : *commits)
            out("  " + c.short_oid + " " + c.summary);
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
