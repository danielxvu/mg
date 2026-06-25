// neomg status micro-benchmark -- NOT a unit test; run manually to reproduce
// the README's numbers.
//
//   neomg_bench <repo> [scoped-subdir]
//
// Measures, on <repo>, the engine path the *magit-status* buffer is built from:
//   cold full   -- full status-buffer build; when ENABLE_ZIG_STATUS=ON the cold
//                  gather path uses hybrid_status (Zig worktree + libgit2 staged),
//                  else repo_status (fresh libgit2 handle, full worktree walk)
//   warm full   -- session.status(): whole-repo status on ONE reused handle
//   warm scoped -- session.status_scoped({subdir}): status limited to one dir on
//                  the reused handle -- the common incremental case (a worktree
//                  edit lands in a single directory; the fs-watcher hands the
//                  monitor exactly that dir).
//
// Reports the median of N runs in milliseconds. The warm-scoped number is the
// one that matters for interactive use: it is what every keystroke-triggered
// refresh costs once the session is warm.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bridge.h" // mg_magit_status_buffer: the *full* status-buffer build

import mg.git;
import mg.magit;

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

template <class F> static std::vector<double> bench_ms(int iters, F &&f)
{
    std::vector<double> ts;
    ts.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clk::now();
        f();
        const auto t1 = clk::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return ts;
}

// --raw mode (for bench/bench.py): one "metric<TAB>value_ms" line per sample, so
// the harness aggregates every tool's stats uniformly. Else: human min/median.
static bool g_raw = false;
static void report(const char *metric, const char *human_label,
                   std::vector<double> ts)
{
    if (g_raw) {
        for (double v : ts)
            std::printf("%s\t%.4f\n", metric, v);
        return;
    }
    std::sort(ts.begin(), ts.end());
    std::printf("%-52s min %8.3f  median %8.3f ms\n", human_label, ts.front(),
                ts[ts.size() / 2]);
}

int main(int argc, char **argv)
{
    std::vector<std::string> pos;
    int runs = 9;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--raw")
            g_raw = true;
        else if (a == "--runs" && i + 1 < argc)
            runs = std::max(1, std::atoi(argv[++i]));
        else
            pos.push_back(a);
    }
    if (pos.empty()) {
        std::fprintf(stderr,
                     "usage: neomg_bench <repo> [scoped-subdir] [--runs N] [--raw]\n");
        return 2;
    }
    const std::string repo = pos[0];

    std::string sub;
    if (pos.size() >= 2) {
        sub = pos[1];
    } else {
        // Default scope: the first top-level subdirectory (the "incremental"
        // case of edits landing in one dir).
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(repo, ec)) {
            if (e.is_directory() && e.path().filename() != ".git") {
                sub = e.path().filename().string();
                break;
            }
        }
    }

    const int N = runs;

    if (auto st = mg::git::repo_status(repo); !st) {
        std::fprintf(stderr, "repo_status failed: %s\n", st.error().message.c_str());
        return 1;
    } else if (!g_raw)
        std::printf("repo: %s  (%zu status entries, scope dir '%s')\n",
                    repo.c_str(), st->size(), sub.c_str());

    // The *full* status-buffer build: the same work Magit's full refresh does --
    // gather every section (head/upstream, branches, tags, stashes, recent
    // commits, conflicts, …) and compose all lines. The honest apples-to-apples
    // vs `magit-refresh`, not just the libgit2 status list.
    auto count_emit = [](void *ctx, const char *, int, const char *, int) {
        ++*static_cast<int *>(ctx);
    };
    int lines = 0;
    report("full_buffer", "full status buffer build (all sections)",
           bench_ms(N, [&] {
               lines = 0;
               (void)mg_magit_status_buffer(repo.c_str(), nullptr, 0, count_emit,
                                            &lines);
           }));
    report("repo_status", "cold repo_status (libgit2 list only)",
           bench_ms(N, [&] { (void)mg::git::repo_status(repo); }));

    auto s = mg::git::session::open(repo);
    if (!s) {
        std::fprintf(stderr, "session::open failed: %s\n", s.error().message.c_str());
        return 1;
    }
    (void)s->status(); // warm the held handle (first call reads the index)
    report("warm_full", "warm full status (reused session)",
           bench_ms(N, [&] { (void)s->status(); }));

    const std::vector<std::string> spec{sub};
    (void)s->status_scoped(spec); // warm
    report("warm_scoped", "warm scoped status (reused session)",
           bench_ms(N, [&] { (void)s->status_scoped(spec); }));
    return 0;
}
