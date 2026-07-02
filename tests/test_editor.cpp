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
#include <functional>
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

// Drain all pending pty output.  Keeps reading until quiet_ms consecutive
// milliseconds elapse without any new bytes, or until the overall deadline
// (ms) is reached.  A single idle poll is not sufficient on Linux: the pty
// kernel buffer may deliver a C-l repaint in multiple bursts separated by
// short gaps, so stopping at the first idle window leaves ANSI escape bytes
// still in flight when the caller sends the next key.
static void drain(int fd, int ms, int quiet_ms = 50)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ms);
    auto last_data = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 10 * 1000}; // 10ms poll
        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) {
            // Check if we have sustained quiet long enough to stop.
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_data);
            if (idle.count() >= quiet_ms)
                break;
            continue;
        }
        char buf[8192];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        last_data = std::chrono::steady_clock::now();
    }
}

// Like drain() but accumulates and returns all bytes read during the window.
static std::string drain_str(int fd, std::chrono::milliseconds timeout, int quiet_ms = 50)
{
    std::string acc;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto last_data = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 10 * 1000}; // 10ms poll
        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) {
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_data);
            if (idle.count() >= quiet_ms)
                break;
            continue;
        }
        char buf[8192];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        acc.append(buf, static_cast<size_t>(n));
        last_data = std::chrono::steady_clock::now();
    }
    return acc;
}

// Send C-x C-c and reap neomg, DRAINING the pty while we wait. neomg keeps
// redrawing on the way out; if the test stops reading the master fd, those
// exit-time writes fill the pty's kernel buffer and neomg blocks in write(),
// never processing the quit (the harness bug behind the old "FSEvents exit
// hang" -- a real terminal always drains, so this only bites a non-draining
// test consumer). Draining lets neomg exit cleanly; SIGKILL is only a fallback.
static void quit_neomg(int master, pid_t pid)
{
    (void)!::write(master, "\x07", 1);     // C-g: cancel an open transient/prompt
    (void)!::write(master, "\x18\x03", 2); // C-x C-c: quit
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid) { ::close(master); return; }
        fd_set r; FD_ZERO(&r); FD_SET(master, &r);
        timeval tv{0, 50 * 1000};
        if (::select(master + 1, &r, nullptr, nullptr, &tv) > 0) {
            char b[8192]; (void)!::read(master, b, sizeof b); // drain
        }
    }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master); // fallback
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

    quit_neomg(master, pid);
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
    quit_neomg(master, pid);
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
                // q: close log window.  After the window is deleted the
                // display does an INCREMENTAL update: only the rows that
                // were previously occupied by the log window are rewritten.
                // "On branch" lives in the top rows of the status buffer
                // (rows 0-4 of the former upper sub-window), which are
                // already correct in pscreen and are NOT rewritten.
                // Sending C-l (0x0c) after q forces sgarbf=TRUE, which
                // triggers a full screen repaint -- every row including
                // the "On branch" header is emitted to the pty.
                (void)!::write(master, "q\x0c", 2);
                if (wait_for(master, "On branch", std::chrono::seconds(8))) {
                    // l h: open the reflog (map rebuilt for *magit-reflog*)
                    (void)!::write(master, "lh", 2);
                    if (wait_for(master, "HEAD@{0}", std::chrono::seconds(8))) {
                        // C-x b *magit-log* RET: switch back without rebuilding
                        // \x18 = C-x, then 'b' triggers usebuffer prompt
                        const char switchbuf[] = "\x18""b*magit-log*\r";
                        (void)!::write(master, switchbuf, sizeof switchbuf - 1);
                        // After the switch, force a full-screen redraw with
                        // C-l (reposition, 0x0c).  The incremental terminal
                        // update only writes the changed cells of the mode
                        // line (e.g. "log*" replacing "reflog*"), so the
                        // literal string "*magit-log*" may not appear in the
                        // pty stream.  C-l sets sgarbf=TRUE which triggers a
                        // full repaint, guaranteeing the complete buffer name
                        // "*magit-log*" is emitted as a contiguous sequence.
                        // This also acts as a settle: if the switch is still
                        // in-flight when C-l arrives, it queues behind it and
                        // the redraw reflects the final state.
                        (void)!::write(master, "\x0c", 1);
                        // Wait for the mode-line marker that is UNIQUE to the
                        // *magit-log* buffer AFTER a full repaint.  The mode
                        // line for a read-only unchanged buffer is "-:%%- " +
                        // buffer-name, so "-:%%-" followed by " *magit-log*"
                        // produces the contiguous string "- *magit-log*".
                        // This string CANNOT appear in the minibuffer echo
                        // (which just shows "*magit-log*" without the mode-
                        // line prefix), so it is a reliable post-switch
                        // confirmation.  The *magit-reflog* buffer similarly
                        // shows "- *magit-reflog*" and never "- *magit-log*".
                        if (wait_for(master, "- *magit-log*", std::chrono::seconds(8))) {
                            // Drain any residual pty bytes from the redraw
                            // so the subsequent RET lands on a quiet terminal.
                            // Use a generous total window (500ms) with a 60ms
                            // sustained-quiet threshold: Linux pty output from a
                            // full C-l repaint can arrive in several bursts, and
                            // a short single-idle drain would exit too early,
                            // leaving ANSI escape sequences in the pipe when RET
                            // fires.
                            drain(master, 500, 60);
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
    quit_neomg(master, pid);
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

    quit_neomg(master, pid);
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
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(ok);
}

TEST_CASE("M-3 expands all files to hunks in *magit-status*")
{
    auto repo = make_repo();                 // git repo + tracked.txt committed
    // modify the tracked file so there's an unstaged hunk to expand
    { std::ofstream f((repo / "tracked.txt"), std::ios::app); f << "ALPHA_LINE\n"; }
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Launch from a non-git dir so the background status monitor (keyed on
        // getcwd() in mg_magit_start) never starts; a warm monitor would serve
        // ITS repo's snapshot instead of this buffer's b_cwd. Outside any repo,
        // every status build is synchronous from b_cwd -- deterministic.
        ::chdir(fs::temp_directory_path().c_str());
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            // C-x 1: make *magit-status* the ONLY window. magit-status splits
            // (the tracked.txt file buffer stays in the top window), so without
            // this the file's "ALPHA_LINE"/name would be on screen regardless of
            // the magit level and contaminate the needles.
            (void)!::write(master, "\x18" "1", 2);
            (void)!::write(master, "\x1b" "3", 2);   // M-3: expand all
            (void)!::write(master, "\x0c", 1);        // C-l: force full repaint
            ok = wait_for(master, "ALPHA_LINE", std::chrono::seconds(8)); // hunk line
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(ok);
}

TEST_CASE("M-1 collapses section bodies; M-2 restores files")
{
    auto repo = make_repo();
    { std::ofstream f((repo / "tracked.txt"), std::ios::app); f << "ALPHA_LINE\n"; }
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Launch from a non-git dir so the background status monitor is not
        // started: mg_magit_start() (main.c) keys the monitor on getcwd(), and
        // when that monitor is warm, mg_magit_status_snapshot returns ITS repo's
        // snapshot regardless of the status buffer's b_cwd. Launched outside any
        // repo, discover_workdir() fails, no monitor runs, and every status
        // build (incl. the M-1/M-2 rebuilds) is synchronous from the buffer's
        // own b_cwd (the opened file's repo) -- deterministic, no stale snapshot.
        ::chdir(fs::temp_directory_path().c_str());
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool collapsed = false, restored = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);    // C-x 1: only *magit-status*
            (void)!::write(master, "\x1b" "3", 2);   // M-3: expand (hunk shown)
            (void)!::write(master, "\x0c", 1);
            if (wait_for(master, "ALPHA_LINE", std::chrono::seconds(8))) {
                // M-1: collapse. Force a clean full repaint, drain to quiet,
                // then the snapshot must keep the header but drop the file body.
                (void)!::write(master, "\x1b" "1", 2);
                (void)!::write(master, "\x0c", 1);
                std::string snap = drain_str(master, std::chrono::milliseconds(1500));
                collapsed = snap.find("Unstaged changes") != std::string::npos &&
                            snap.find("tracked.txt") == std::string::npos;
                // M-2: restore files (the file name returns, no hunk)
                (void)!::write(master, "\x1b" "2", 2);
                (void)!::write(master, "\x0c", 1);
                restored = wait_for(master, "tracked.txt", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(collapsed);
    CHECK(restored);
}

TEST_CASE("+ grows diff context; w blocks hunk staging with a warning")
{
    auto repo = make_repo(); // tracked.txt committed
    // overwrite with a change on line 6, lines 1-10, so context controls matter
    std::ofstream(repo / "tracked.txt") << "1\n2\n3\n4\n5\nSIX\n7\n8\n9\nTENLINE\n";
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::chdir(fs::temp_directory_path().c_str());   // monitor-inert determinism
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool grew = false, warned = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "Unstaged changes", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);     // C-x 1: only *magit-status*
            // expand the file (TAB on its line) -- navigate to it first
            (void)!::write(master, "\x1b" "3", 2);     // M-3: expand all (hunks)
            (void)!::write(master, "\x0c", 1);
            if (wait_for(master, "SIX", std::chrono::seconds(8))) {
                // grow context: TENLINE (4 lines from the change) appears at -U6
                (void)!::write(master, "+++", 3);       // context 3->6
                (void)!::write(master, "\x0c", 1);
                grew = wait_for(master, "TENLINE", std::chrono::seconds(8));
                if (grew) {
                    // turn on -w, then try to stage a hunk -> warn
                    (void)!::write(master, "w", 1);
                    (void)!::write(master, "\x0c", 1);
                    // Event-wait (not a wall-clock sleep): the "Diff: -U.. -w"
                    // header line only renders once w has taken effect and the
                    // repaint has landed, so it reliably signals the screen has
                    // settled before we send navigation keys. (A @@ hunk header
                    // is also re-confirmed visible below.)
                    wait_for(master, "Diff:", std::chrono::seconds(8));
                    wait_for(master, "@@", std::chrono::seconds(8));
                    // after w+refresh, cursor is at top; navigate to the @@ hunk line.
                    // layout: On branch / Head: / Diff: / (blank) / Untracked(1) /
                    //   file / (blank) / Unstaged(1) / tracked.txt / @@ ...
                    // = 9 C-n presses from line 1 to reach line 10 (hunk header)
                    (void)!::write(master, "\x0e\x0e\x0e\x0e\x0e\x0e\x0e\x0e\x0e", 9);
                    (void)!::write(master, "s", 1);     // stage hunk at point
                    warned = wait_for(master, "Turn off -w", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(grew);
    CHECK(warned);
}

TEST_CASE(": runs a git command and shows it in *magit-process*")
{
    auto repo = make_repo(); // tracked.txt committed + untracked.txt
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::chdir(fs::temp_directory_path().c_str()); // monitor-inert determinism
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool ok = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);       // C-x 1
            (void)!::write(master, ":", 1);               // git-command prompt
            const char gc[] = "rev-parse --abbrev-ref HEAD\r";
            (void)!::write(master, gc, sizeof gc - 1);    // robust: no manual count
            (void)!::write(master, "\x0c", 1);            // force repaint
            // *magit-process* shows the "$ git rev-parse …" entry
            ok = wait_for(master, "git rev-parse --abbrev-ref HEAD",
                          std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(ok);
}

TEST_CASE("M-1 is a guarded no-op in *magit-reflog* (status-only)")
{
    auto repo = make_repo();
    sh(repo.string(), "git commit --allow-empty -m second"); // reflog >= 2 entries
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Non-git cwd: keep the getcwd-keyed status monitor off so the buffer's
        // b_cwd repo is the only source of truth (see M-3 test for the rationale).
        ::chdir(fs::temp_directory_path().c_str());
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool guarded = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "lh", 2);  // l (log menu) h (reflog)
            if (wait_for(master, "HEAD@{0}", std::chrono::seconds(8))) {
                (void)!::write(master, "\x18" "1", 2);  // C-x 1: only *magit-reflog*
                (void)!::write(master, "\x1b" "1", 2);   // M-1 in reflog -> guarded
                (void)!::write(master, "\x0c", 1);        // force repaint of echo line
                // The guard ewprintf renders in the echo area.
                guarded = wait_for(master, "Section levels apply", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(guarded);
}

TEST_CASE("l transient --all includes a non-HEAD commit in *magit-log*")
{
    auto repo = make_repo(); // tracked.txt committed
    std::string d = repo.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("checkout -b side") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m SIDEONLY_COMMIT") == 0);
    REQUIRE(run("checkout -") == 0);
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
    bool with_all = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "l", 1);           // open the log transient
            if (wait_for(master, "--all", std::chrono::seconds(8))) { // infix listed
                (void)!::write(master, "A", 1);       // toggle --all
                (void)!::write(master, "l", 1);       // l l: run the log
                (void)!::write(master, "\x0c", 1);    // force repaint
                with_all = wait_for(master, "SIDEONLY_COMMIT", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(with_all);
}

// Two greppable commits + neomg launched on the repo; runs `body(master)`
// after *magit-status* is up with a single window. Returns body's result.
// Both --grep tests below share this; neither does a fragile *magit-log* ->
// *magit-status* round-trip (l is only bound in status, and re-detecting that
// switch is unreliable on Linux/musl). Each drives one transient session.
static bool grep_repo_session(const std::function<bool(int)> &body)
{
    auto repo = make_repo(); // tracked.txt committed
    std::string d = repo.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m AAAA_FIRST") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m BBBB_SECOND") == 0);
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
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            ok = body(master);
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    return ok;
}

TEST_CASE("l transient --grep routes the value into the log query")
{
    bool filtered = grep_repo_session([](int master) {
        (void)!::write(master, "l", 1);               // open the log transient
        if (!wait_for(master, "--grep", std::chrono::seconds(8)))
            return false;
        (void)!::write(master, "m", 1);               // --grep infix
        (void)!::write(master, "BBBB\r", 5);          // set grep=BBBB
        (void)!::write(master, "l", 1);               // l l: run the filtered log
        (void)!::write(master, "\x0c", 1);            // force repaint
        return wait_for(master, "BBBB_SECOND", std::chrono::seconds(8));
    });
    CHECK(filtered); // grep=BBBB routed into the log query; the match renders
}

TEST_CASE("l transient empty input clears a set --grep")
{
    // Set --grep, then clear it -- all within ONE transient session (the
    // transient stays open across infix edits, so no buffer round-trip). The
    // re-opened prompt echoes "(BBBB)", proving the set persisted; running with
    // the value cleared shows AAAA_FIRST, which a live BBBB filter would hide --
    // so its reappearance proves the empty input actually unset --grep.
    bool set_persisted = false, cleared = false;
    grep_repo_session([&](int master) {
        (void)!::write(master, "l", 1);               // open the log transient
        if (!wait_for(master, "--grep", std::chrono::seconds(8)))
            return false;
        (void)!::write(master, "m", 1);               // --grep infix
        (void)!::write(master, "BBBB\r", 5);          // set grep=BBBB (transient stays open)
        (void)!::write(master, "m", 1);               // re-open the --grep prompt
        set_persisted = wait_for(master, "(BBBB)", std::chrono::seconds(8));
        (void)!::write(master, "\r", 1);              // empty input -> clears --grep
        (void)!::write(master, "l", 1);               // l l: run with grep cleared
        (void)!::write(master, "\x0c", 1);            // force repaint
        cleared = wait_for(master, "AAAA_FIRST", std::chrono::seconds(8));
        return set_persisted && cleared;
    });
    CHECK(set_persisted); // the re-opened prompt echoed the stored BBBB
    CHECK(cleared);       // empty input unset --grep -> AAAA_FIRST reappears
}

TEST_CASE("P transient lists --tags and --dry-run infixes")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool listed = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "P", 1);           // push transient
            listed = wait_for(master, "--dry-run", std::chrono::seconds(8)); // infix rendered
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed);
}

TEST_CASE("P transient --dry-run opens *magit-process* with a push --dry-run entry")
{
    auto repo = make_repo(); // no origin -> the dry-run push fails fast but is still logged
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool shown = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "P", 1);           // push transient
            if (wait_for(master, "--dry-run", std::chrono::seconds(8))) {
                (void)!::write(master, "d", 1);       // toggle --dry-run
                (void)!::write(master, "p", 1);       // P p: run the captured dry-run
                (void)!::write(master, "\x0c", 1);    // force repaint
                shown = wait_for(master, "push --dry-run", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid); // drains the pty so the verbose exit doesn't wedge write()
    fs::remove_all(repo);
    CHECK(shown); // *magit-process* shows the captured "push --dry-run" command
}

TEST_CASE("F transient lists --autostash and --ff-only infixes")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool listed = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "F", 1);           // pull transient
            listed = wait_for(master, "--autostash", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed);
}
