// neomg status micro-benchmark -- NOT a unit test; run manually to reproduce
// the README's numbers.
//
//   neomg_bench <repo> [scoped-subdir]
//
// Measures, on <repo>, the engine path the *magit-status* buffer is built from:
//   cold full   -- repo_status(repo): opens a fresh libgit2 handle per call and
//                  walks the whole worktree (the first-open / .git-changed cost)
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

struct stat_ms {
    double min;
    double median;
};

template <class F> static stat_ms bench_ms(int iters, F &&f)
{
    std::vector<double> ts;
    ts.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clk::now();
        f();
        const auto t1 = clk::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ts.begin(), ts.end());
    return {ts.front(), ts[ts.size() / 2]};
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: neomg_bench <repo> [scoped-subdir]\n");
        return 2;
    }
    const std::string repo = argv[1];

    std::string sub;
    if (argc >= 3) {
        sub = argv[2];
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

    const int N = 9;

    if (auto st = mg::git::repo_status(repo); st)
        std::printf("repo: %s  (%zu status entries)\n", repo.c_str(),
                    st->size());
    else {
        std::fprintf(stderr, "repo_status failed: %s\n", st.error().message.c_str());
        return 1;
    }

    const stat_ms cold = bench_ms(N, [&] { (void)mg::git::repo_status(repo); });

    // The *full* status-buffer build: the same work Magit's full refresh does --
    // gather every section (head/upstream, branches, tags, stashes, recent
    // commits, conflicts, …) and compose all lines. This is the honest
    // apples-to-apples vs `magit-refresh`, not just the libgit2 status list.
    auto count_emit = [](void *ctx, const char *, int, const char *, int) {
        ++*static_cast<int *>(ctx);
    };
    int lines = 0;
    const stat_ms full_buf = bench_ms(N, [&] {
        lines = 0;
        (void)mg_magit_status_buffer(repo.c_str(), nullptr, 0, count_emit, &lines);
    });

    auto s = mg::git::session::open(repo);
    if (!s) {
        std::fprintf(stderr, "session::open failed: %s\n", s.error().message.c_str());
        return 1;
    }
    (void)s->status(); // warm the held handle (first call reads the index)
    const stat_ms warm_full = bench_ms(N, [&] { (void)s->status(); });

    const std::vector<std::string> spec{sub};
    (void)s->status_scoped(spec); // warm
    const stat_ms warm_scoped = bench_ms(N, [&] { (void)s->status_scoped(spec); });

    std::printf("                                              %8s  %8s\n", "min", "median");
    std::printf("full status BUFFER build (cold; all sections, %d lines):"
                " %8.2f  %8.2f ms\n", lines, full_buf.min, full_buf.median);
    std::printf("cold full repo_status (libgit2 list only):    %8.2f  %8.2f ms\n",
                cold.min, cold.median);
    std::printf("warm full status (reused session):            %8.2f  %8.2f ms\n",
                warm_full.min, warm_full.median);
    std::printf("warm scoped status [%s] (reused session): %8.3f  %8.3f ms\n",
                sub.c_str(), warm_scoped.min, warm_scoped.median);
    return 0;
}
