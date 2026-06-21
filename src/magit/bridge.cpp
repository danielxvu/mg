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
    auto chomp = [](std::string s) {
        if (!s.empty() && s.back() == '\n')
            s.pop_back();
        return s;
    };
    auto emit_diff = [&](const std::string &path, bool staged) {
        auto hunks = mg::git::file_diff(repo_path, path, staged);
        if (!hunks)
            return;
        for (int hi = 0; hi < static_cast<int>(hunks->size()); ++hi) {
            out(chomp((*hunks)[hi].header), MG_LINE_HUNK, path.c_str(), hi);
            for (const auto &l : (*hunks)[hi].lines)
                out(std::string(1, l.origin) + chomp(l.content), MG_LINE_DIFF,
                    path.c_str(), hi);
        }
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

extern "C" int mg_magit_fetch(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::fetch_remote(repo_path, remote ? remote : "origin")
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_push(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::push_remote(repo_path, remote ? remote : "origin")
                   .has_value()
               ? 1
               : 0;
}

extern "C" int mg_magit_pull(const char *repo_path, const char *remote)
{
    if (repo_path == nullptr)
        return 0;
    return mg::git::pull_remote(repo_path, remote ? remote : "origin")
                   .has_value()
               ? 1
               : 0;
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
    return mg::git::revert_commit(repo_path, rev).has_value() ? 1 : 0;
}

extern "C" int mg_magit_merge(const char *repo_path, const char *name)
{
    if (repo_path == nullptr || name == nullptr || name[0] == '\0')
        return 0;
    return mg::git::merge_branch(repo_path, name).has_value() ? 1 : 0;
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
