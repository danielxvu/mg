// mg.magit.proclog -- a process-wide, mutex-guarded ring buffer of the git
// operations neomg performs, for the *magit-process* transparency buffer.
// Appended to from the CLI mutation helpers (git.cppm) and the libgit2 mutating
// wrappers (bridge.cpp); read back via snapshot(). In-memory, session-scoped.
module;
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

export module mg.magit.proclog;

export namespace mg::magit::proclog {

// One logged operation. `kind` is a raw marker byte: '$' = real subprocess
// (literal argv in `command`, real `output`), '~' = in-process libgit2 op
// (`command` is the EQUIVALENT git command, `output` empty). `ok` is the
// success of the op; `duration_ms` is wall time for '$' ops (0 for '~').
struct entry {
    char kind;
    std::string command;
    std::string output;
    bool ok;
    long duration_ms;
};

void record(char kind, std::string command, std::string output, bool ok,
            long duration_ms);
std::vector<entry> snapshot();
void clear();
std::string argv_to_command(std::span<const std::string> argv);

} // namespace mg::magit::proclog

// ---- implementation (module-internal global: one ring per process) ----------
namespace mg::magit::proclog {
namespace {
constexpr std::size_t kCap = 200;
std::mutex g_mu;
std::deque<entry> g_ring; // guarded by g_mu
} // namespace

void record(char kind, std::string command, std::string output, bool ok,
            long duration_ms)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.push_back({kind, std::move(command), std::move(output), ok,
                      duration_ms});
    while (g_ring.size() > kCap)
        g_ring.pop_front();
}

std::vector<entry> snapshot()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return {g_ring.begin(), g_ring.end()};
}

void clear()
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.clear();
}

std::string argv_to_command(std::span<const std::string> argv)
{
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i)
            out += ' ';
        const std::string &a = argv[i];
        bool needs_quote = a.empty() ||
                           a.find_first_of(" \t'") != std::string::npos;
        if (!needs_quote) {
            out += a;
            continue;
        }
        // single-quote, escaping embedded single quotes as '\'' (POSIX idiom).
        out += '\'';
        for (char c : a) {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += '\'';
    }
    return out;
}

} // namespace mg::magit::proclog
