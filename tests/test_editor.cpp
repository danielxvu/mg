// Editor-level (pty) test -- drives the built neomg BINARY through a pseudo-tty
// to cover the C command layer (src/magit_cmd.c), which the engine/bridge tests
// cannot reach (they call the C++ API with explicit repo paths). This is the
// safety net for the class of bug where a magit command used the process cwd
// instead of the current buffer's directory.
//
// Scenario: launch neomg from a directory that is NOT a git repo, but open a
// file that lives INSIDE one. The process cwd is the non-repo dir; the buffer's
// directory is the repo. `magit-status` must operate on the buffer's repo --
// if it used getcwd() (the old bug) the buffer would come up empty.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <csignal>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#  include <pty.h>     // forkpty (glibc/musl)
#elif defined(__FreeBSD__)
#  include <libutil.h> // forkpty (FreeBSD)
#else                  // macOS, OpenBSD, NetBSD
#  include <util.h>    // forkpty
#endif

namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_editor_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}

int sh(const std::string &dir, const std::string &cmd)
{
    return std::system(("cd '" + dir + "' && " + cmd + " >/dev/null 2>&1").c_str());
}

// A git repo (system git, no libgit2 link needed here) with one commit and one
// untracked file, so `magit-status` has a branch + an "Untracked files" section.
fs::path make_repo()
{
    auto dir = make_temp_dir();
    const std::string d = dir.string();
    REQUIRE(sh(d, "git init") == 0);
    REQUIRE(sh(d, "git config user.email t@t.test && git config user.name tester") == 0);
    std::ofstream(dir / "tracked.txt") << "hello\n";
    REQUIRE(sh(d, "git add tracked.txt && git commit -m init") == 0);
    std::ofstream(dir / "untracked.txt") << "yo\n";
    return dir;
}

// Read from the pty until `needle` appears in the accumulated output, or until
// `timeout` elapses. Robust to render timing: slow environments just take
// longer; only a buffer that NEVER renders the content fails.
bool wait_for(int fd, const char *needle, std::chrono::milliseconds timeout)
{
    std::string acc;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 100 * 1000}; // 100ms
        if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0) {
            char buf[8192];
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0)
                break; // child exited / pty closed
            acc.append(buf, static_cast<size_t>(n));
            if (acc.find(needle) != std::string::npos)
                return true;
        }
    }
    return acc.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("magit-status renders on the buffer's repo when neomg is launched outside it")
{
    auto repo = make_repo();
    auto nonrepo = make_temp_dir(); // launch cwd: NOT a git repo
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{};
    ws.ws_row = 40;
    ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child: cwd = the non-repo dir, but open a file inside the repo.
        if (::chdir(nonrepo.string().c_str()) != 0)
            _exit(126);
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }

    bool rendered = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) { // editor is up
        const char ms[] = "\x1bxmagit-status\r"; // M-x magit-status RET
        (void)!::write(master, ms, sizeof ms - 1);
        // The status buffer must show the branch header. With the old getcwd bug
        // it would scan the (non-repo) launch dir and come up empty.
        rendered = wait_for(master, "On branch", std::chrono::seconds(8));
    }

    const char quit[] = "\x18\x03"; // C-x C-c
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid)
            break;
        usleep(100 * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    ::close(master);
    fs::remove_all(repo);
    fs::remove_all(nonrepo);

    CHECK(rendered);
}

TEST_CASE("l h opens the *magit-reflog* buffer from magit-status")
{
    auto repo = make_repo(); // git init + 1 commit + 1 untracked
    // a second commit so the reflog has >=2 entries
    sh(repo.string(), "git commit --allow-empty -m second");
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        const char ms[] = "\x1bxmagit-status\r";
        (void)!::write(master, ms, sizeof ms - 1);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "lh", 2); // l (log menu) h (reflog)
            ok = wait_for(master, "HEAD@{0}", std::chrono::seconds(8));
        }
    }
    const char quit[] = "\x18\x03";
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}

/*
 * Regression test for the shared oid-map wrong-oid hazard (FM-REFLOG review):
 * open *magit-log* (builds oid map for that buffer), then open *magit-reflog*
 * (rebuilds the map for the reflog buffer), then switch back to *magit-log*
 * via C-x b without a rebuild.  RET must produce "Not on a commit" -- the
 * guard `curbp != magit_log_oid_bp` fires and returns NULL -- never a diff
 * built from the reflog entry that happened to sit at the same row index.
 */
TEST_CASE("RET in *magit-log* after switching from *magit-reflog* shows Not on a commit")
{
    auto repo = make_repo();
    sh(repo.string(), "git commit --allow-empty -m second");
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            // l l: open the log buffer (map built for *magit-log*)
            (void)!::write(master, "ll", 2);
            if (wait_for(master, "second", std::chrono::seconds(8))) {
                // q: close log, return to *magit-status* so l h is available
                (void)!::write(master, "q", 1);
                if (wait_for(master, "On branch", std::chrono::seconds(8))) {
                    // l h: open the reflog (map rebuilt for *magit-reflog*)
                    (void)!::write(master, "lh", 2);
                    if (wait_for(master, "HEAD@{0}", std::chrono::seconds(8))) {
                        // C-x b *magit-log* RET: switch back without rebuilding
                        // \x18 = C-x, then 'b' triggers usebuffer prompt
                        const char switchbuf[] = "\x18""b*magit-log*\r";
                        (void)!::write(master, switchbuf, sizeof switchbuf - 1);
                        // wait for the log buffer to be current again
                        if (wait_for(master, "second", std::chrono::seconds(8))) {
                            // RET on what was a commit line -- guard must fire
                            (void)!::write(master, "\r", 1);
                            // safe outcome: stale-map guard returns NULL
                            ok = wait_for(master, "Not on a commit",
                                std::chrono::seconds(8));
                        }
                    }
                }
            }
        }
    }
    const char quit[] = "\x18\x03";
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}

TEST_CASE("$ opens the *magit-process* buffer from magit-status")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }

    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        const char ms[] = "\x1bxmagit-status\r"; // M-x magit-status RET
        (void)!::write(master, ms, sizeof ms - 1);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            const char dollar[] = "$";  // open the process buffer
            (void)!::write(master, dollar, 1);
            // empty-state line proves the buffer rendered (no ops logged yet)
            ok = wait_for(master, "No git operations", std::chrono::seconds(8));
        }
    }

    const char quit[] = "\x18\x03"; // C-x C-c
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}

TEST_CASE("x m in *magit-reflog* resets HEAD to the entry at point")
{
    auto repo = make_repo();
    sh(repo.string(), "git commit --allow-empty -m second");
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "lh", 2);                 // open reflog
            if (wait_for(master, "HEAD@{1}", std::chrono::seconds(8))) {
                (void)!::write(master, "\x0e", 1);           // C-n: down to HEAD@{1}
                (void)!::write(master, "xm", 2);             // x m: mixed reset
                // after reset, the newest entry is a reset: line
                ok = wait_for(master, "reset:", std::chrono::seconds(8));
            }
        }
    }
    const char quit[] = "\x18\x03";
    (void)!::write(master, quit, sizeof quit - 1);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(ok);
}
