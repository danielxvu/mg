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
#include <sstream>

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

TEST_CASE("y opens the *magit-refs* overview")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool opened = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "y", 1);        // open the refs overview
            opened = wait_for(master, "Branches (", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(opened); // y rendered *magit-refs* with the Branches section
}

/*
 * Regression test for the magit_meta owner-gate (review of FM-SHOW-REFS):
 * magit_at_point() resolves the row under point via the *magit-status*-only
 * magit_meta[] array, with no owner-buffer check. Invoked from *magit-refs*
 * (whose own per-line layout is unrelated), it used to index that STALE
 * array at the refs buffer's line position -- so `b b` (checkout) or `b k`
 * (delete) could act on the wrong branch entirely.
 *
 * The repo has exactly 8 local branches (the default + 7 extras) and no
 * other branch/upstream/stash state, so *magit-status*'s line layout is
 * pinned: 0 On branch, 1 Head:, 2 blank, 3 Untracked files (1), 4
 * untracked.txt, 5 blank, 6 Recent commits, 7 <commit summary>, 8 blank, 9
 * Branches (8), 10.. the 8 branch rows. magit_meta[10] is therefore always
 * a real MG_LINE_BRANCH row -- the stale target this bug would resolve to.
 *
 * *magit-refs* for the same repo has only 11 lines (0..10): 0 the "Refs
 * (HEAD: ...)" header, 1 blank, 2 "Branches (8)", 3..10 the 8 branch rows.
 * Moving point to the LAST line (10 C-n presses from the top) makes
 * magit_at_point's line-index walk over *magit-refs* produce idx == 10 --
 * the same index that, pre-fix, hits a real branch row in the stale status
 * meta, so `b b` would silently check out that (wrong) branch with no
 * echoed message at all. Post-fix, the owner gate (curbp != magit_meta_bp)
 * fires unconditionally and `b b` reports "Not on a branch".
 */
TEST_CASE("b b in *magit-refs* does not act on stale status meta")
{
    auto repo = make_repo(); // git repo, 1 commit "init", untracked.txt present
    for (int i = 0; i < 7; ++i)
        sh(repo.string(), "git branch extra" + std::to_string(i)); // 8 branches total
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
        // Confirm the 8-branch layout rendered (pins magit_meta[10] to a
        // real branch row) before switching to *magit-refs*.
        if (wait_for(master, "Branches (8)", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "y", 1);        // open *magit-refs*
            if (wait_for(master, "Branches (", std::chrono::seconds(8))) {
                for (int i = 0; i < 10; ++i)
                    (void)!::write(master, "\x0e", 1); // C-n x10: to row 10
                (void)!::write(master, "bb", 2); // b (branch menu) b (checkout)
                ok = wait_for(master, "Not on a branch",
                    std::chrono::seconds(8));
            }
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

TEST_CASE("l in *magit-log* re-opens the log transient")
{
    auto repo = make_repo(); // commits "init"
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool reopened = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "l", 1);           // open the log transient
            if (wait_for(master, "--grep", std::chrono::seconds(8))) {
                (void)!::write(master, "l", 1);       // l l: run the log -> *magit-log*
                if (wait_for(master, "init", std::chrono::seconds(8))) { // log rendered
                    (void)!::write(master, "l", 1);   // l IN *magit-log* -> the new binding
                    reopened = wait_for(master, "--grep", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid); // drains + reaps (C-g closes the transient first)
    fs::remove_all(repo);
    CHECK(reopened); // pressing l in *magit-log* rendered the log transient
}

TEST_CASE("d opens the diff-view popup and + applies live to *magit-status*")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool opened = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "d", 1);           // open the diff-view popup
            opened = wait_for(master, "Diff view", std::chrono::seconds(8));
            if (opened) {
                (void)!::write(master, "+", 1);       // more context -> -U4
                // "Diff:" (colon) is emitted ONLY by *magit-status* when
                // context != 3 -- so it proves the live refresh, not just the
                // popup's re-render (the popup title is "Diff view").
                applied = wait_for(master, "Diff:", std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid); // C-g closes the popup, then quits
    fs::remove_all(repo);
    CHECK(opened);  // d opened the diff-view popup
    CHECK(applied); // + applied live: *magit-status* re-rendered with the Diff: header
}

TEST_CASE("f opens the fetch transient with --prune/--tags/--all")
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
            (void)!::write(master, "f", 1);           // open the fetch transient
            listed = wait_for(master, "--prune", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed); // f now opens the fetch transient (renders --prune)
}

TEST_CASE("l transient lists the extra log args (--since/--reverse/--no-merges)")
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
            (void)!::write(master, "l", 1);           // log transient
            listed = wait_for(master, "--no-merges", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed); // the new infixes render in the l transient
}

TEST_CASE("*magit-log* shows colored ref decoration on the HEAD row")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool decorated = false, colored = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            // The strict two-wait_for form (wait for "(HEAD -> ", then
            // separately wait for the ESC[36m+HEAD run) is unreliable here:
            // the colorizer emits the SGR switch exactly between '(' and 'H',
            // so "(HEAD -> " is never a contiguous byte run once colored, and
            // a single read() of the repaint can swallow the SGR bytes before
            // the first wait_for returns, starving the second. Instead, drain
            // the whole repaint into one buffer and search it for both
            // needles. "HEAD ->" (no leading paren, no trailing space) is the
            // exact 7-byte span the colorizer keeps as one uninterrupted run,
            // so it survives coloring; ESC[36m immediately precedes it.
            std::string out = drain_str(master, std::chrono::seconds(8));
            decorated = out.find("HEAD ->") != std::string::npos;
            colored = out.find("\x1b[36mHEAD") != std::string::npos;
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(decorated); // the log row carries (HEAD -> ...
    CHECK(colored);   // ...and the colorizer painted HEAD cyan
}

TEST_CASE("+ in *magit-commit* rebuilds the diff with more context")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool oncommit = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            if (wait_for(master, "init", std::chrono::seconds(8))) {
                (void)!::write(master, "\r", 1);   // RET: show the commit
                oncommit = wait_for(master, "commit ", std::chrono::seconds(8));
                if (oncommit) {
                    (void)!::write(master, "+", 1); // more context in commit view
                    // The Diff: header (Task 2) appears only at non-default ctx,
                    // and only *magit-commit* re-renders it here.
                    applied = wait_for(master, "Diff:", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(oncommit); // RET opened the commit view
    CHECK(applied);  // + rebuilt it with the new context (Diff: header rendered)
}

TEST_CASE("w in *magit-commit* toggles ignore-whitespace")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool oncommit = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            if (wait_for(master, "init", std::chrono::seconds(8))) {
                (void)!::write(master, "\r", 1);   // RET: show the commit
                oncommit = wait_for(master, "commit ", std::chrono::seconds(8));
                if (oncommit) {
                    (void)!::write(master, "w", 1); // toggle ignore-whitespace
                    // At the default context (3) the Diff: header is suppressed
                    // unless -w is also set, so its appearance proves the toggle
                    // reached the commit view: "Diff:     -U3 -w".
                    applied = wait_for(master, "-w", std::chrono::seconds(8));
                }
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(oncommit); // RET opened the commit view
    CHECK(applied);  // w rebuilt it with -w (Diff:     -U3 -w rendered)
}

TEST_CASE("+ inside the d popup rebuilds *magit-commit*'s diff")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool oncommit = false, popup = false, applied = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            if (wait_for(master, "init", std::chrono::seconds(8))) {
                (void)!::write(master, "\r", 1);   // RET: show the commit
                oncommit = wait_for(master, "commit ", std::chrono::seconds(8));
                if (oncommit) {
                    (void)!::write(master, "d", 1); // d: open the diff-view popup
                    popup = wait_for(master, "Diff view", std::chrono::seconds(8));
                    if (popup) {
                        (void)!::write(master, "+", 1); // grow context from the popup
                        // The popup applies via magit_diff_view_refresh(stbp, ...) --
                        // the stbp-targeted path -- which rebuilds *magit-commit*
                        // itself (not just the popup) and forces its window to
                        // redraw, so the Diff: header shows up on screen here.
                        applied = wait_for(master, "Diff:", std::chrono::seconds(8));
                        (void)!::write(master, "q", 1); // q: close the popup
                    }
                }
            }
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(oncommit); // RET opened the commit view
    CHECK(popup);    // d opened the diff-view popup
    CHECK(applied);  // + (inside the popup) rebuilt *magit-commit*'s Diff: header
}

TEST_CASE("find-file opens a missing-directory path without prompting; save creates it")
{
    auto dir = make_temp_dir();               // a plain temp dir (no repo needed)
    std::ofstream(dir / "seed.txt") << "seed\n"; // seed file -> default-dir = this dir
    const std::string seed = (dir / "seed.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", seed.c_str(), (char *)nullptr); _exit(127); }

    bool opened = false, no_open_prompt = false, save_prompt = false, wrote = false;
    if (wait_for(master, "seed.txt", std::chrono::seconds(8))) {
        // C-x C-f  nd/f.txt  RET  -- the directory "nd/" does not exist
        (void)!::write(master, "\x18\x06", 2);           // C-x C-f
        (void)!::write(master, "nd/f.txt\r", 9);
        // The buffer must open with NO open-time prompt (Emacs behavior).
        std::string acc = drain_str(master, std::chrono::seconds(2));
        no_open_prompt = acc.find("Missing directory") == std::string::npos;
        opened = acc.find("f.txt") != std::string::npos;  // new file in the modeline
        // Edit + save -> the prompt appears NOW.
        (void)!::write(master, "x", 1);
        (void)!::write(master, "\x18\x13", 2);            // C-x C-s
        save_prompt = wait_for(master, "Missing directory, create",
                               std::chrono::seconds(8));
        if (save_prompt) {
            (void)!::write(master, "y", 1);               // create the directory
            wrote = wait_for(master, "Wrote", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    bool on_disk = fs::exists(dir / "nd" / "f.txt");
    fs::remove_all(dir);
    CHECK(opened);         // buffer opened for the missing-dir path
    CHECK(no_open_prompt); // NO prompt at open (was a blocking prompt before)
    CHECK(save_prompt);    // prompt now appears at save time
    CHECK(wrote);          // save wrote the file after creating the directory
    CHECK(on_disk);        // directory + file actually created on disk
}

TEST_CASE("C-t at end of line transposes in place (does not cross to the next line)")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "ab\ncd\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    if (wait_for(master, "ab", std::chrono::seconds(8))) {
        (void)!::write(master, "\x05", 1);      // C-e : end of line 1 (after 'b')
        drain(master, 300);                     // let it settle
        (void)!::write(master, "\x14", 1);      // C-t : transpose-chars
        (void)!::write(master, "\x18\x13", 2);  // C-x C-s : save
        (void)wait_for(master, "Wrote", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    const std::string content = ss.str();
    fs::remove_all(dir);
    // Emacs transposes the last two chars of the line in place; it must NOT
    // move 'b' onto the next line ("a\nbcd\n" was the corruption bug).
    CHECK(content == "ba\ncd\n");
}

TEST_CASE("dired visits a symlink's target, not a garbled 'name -> target' string")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "zzztarget.txt") << "TARGETBODY_UNIQUE\n";
    fs::create_symlink(dir / "zzztarget.txt", dir / "aaalink"); // aaalink -> zzztarget.txt
    const std::string d = dir.string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", d.c_str(), (char *)nullptr); _exit(127); }
    bool visited = false;
    if (wait_for(master, "aaalink", std::chrono::seconds(8))) { // dired rendered
        // point starts on the first entry (the symlink). Visit it.
        (void)!::write(master, "f", 1);
        // A correct name resolves the symlink and opens the target's content;
        // the bug opened an empty "(New file)" named after garbage.
        visited = wait_for(master, "TARGETBODY_UNIQUE", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(visited); // f on a symlink row opened its target, not a (New file)
}

TEST_CASE("M-t (transpose-words) honors a numeric prefix argument")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "one two three four\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    if (wait_for(master, "one two", std::chrono::seconds(8))) {
        (void)!::write(master, "\x06\x06\x06", 3); // C-f x3 : point after "one"
        drain(master, 300);
        (void)!::write(master, "\x1b" "2", 2);      // M-2 : prefix arg 2
        (void)!::write(master, "\x1b" "t", 2);      // M-t : transpose-words
        (void)!::write(master, "\x18\x13", 2);      // C-x C-s
        (void)wait_for(master, "Wrote", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    const std::string content = ss.str();
    fs::remove_all(dir);
    // Emacs: C-u 2 M-t drags "one" past two words -> "two three one four".
    CHECK(content == "two three one four\n");
}

TEST_CASE("query-replace quits on q (not just RET/ESC)")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "foo foo foo\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool quit_ok = false;
    if (wait_for(master, "foo foo", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1b%", 2);                 // M-% : query-replace
        if (wait_for(master, "Query replace", std::chrono::seconds(8))) {
            (void)!::write(master, "foo\r", 4);             // search pattern
            (void)!::write(master, "bar\r", 4);             // replacement
            if (wait_for(master, "Query replacing", std::chrono::seconds(8))) {
                (void)!::write(master, "q", 1);             // quit
                // Fixed: q ends query-replace ("Replaced 0 occurrences").
                // Bug: q reprinted the "y/n or ..." help and looped.
                quit_ok = wait_for(master, "Replaced 0 occurrences",
                                   std::chrono::seconds(8));
            }
        }
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    const std::string content = ss.str();
    fs::remove_all(dir);
    CHECK(quit_ok);                       // q exited query-replace
    CHECK(content == "foo foo foo\n");    // nothing replaced (quit at first match)
}

TEST_CASE("an unbound key reports 'is undefined', not 'Quit'")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "seed.txt") << "hello\n";
    const std::string p = (dir / "seed.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool undef = false, said_quit = false;
    if (wait_for(master, "hello", std::chrono::seconds(8))) {
        (void)!::write(master, "\x18r", 2);   // C-x r : unbound (rescan default)
        std::string acc = drain_str(master, std::chrono::seconds(2));
        undef = acc.find("is undefined") != std::string::npos;
        said_quit = acc.find("Quit") != std::string::npos;
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(undef);            // "<key> is undefined" shown (Emacs behavior)
    CHECK_FALSE(said_quit);  // not conflated with C-g's "Quit"
}

TEST_CASE("the active region renders in standout, and deactivates on edit")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "REGIONWORD tail\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool shown = false, gone_after_edit = false;
    if (wait_for(master, "REGIONWORD", std::chrono::seconds(8))) {
        (void)!::write(master, "\x01", 1);   // C-a : beginning of line (col 0)
        drain(master, 300);
        (void)!::write(master, "\x00", 1);   // C-SPC (C-@) : set mark
        for (int i = 0; i < 10; i++)         // C-f x10 : extend over REGIONWORD
            (void)!::write(master, "\x06", 1);
        // Region cols [0,10) = REGIONWORD -> standout starts at col 0, so the
        // render emits ESC[7m immediately before the word (the modeline
        // standout never precedes "REGIONWORD").
        shown = wait_for(master, "\x1b[7mREGIONWORD", std::chrono::seconds(8));
        (void)!::write(master, "x", 1);      // self-insert -> deactivates the region
        (void)!::write(master, "\x0c", 1);   // C-l : force a repaint
        std::string after = drain_str(master, std::chrono::seconds(2));
        gone_after_edit = after.find("\x1b[7mREGIONWORD") == std::string::npos;
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(shown);           // the active region painted REGIONWORD in standout
    CHECK(gone_after_edit); // editing deactivated it (no standout on the word)
}

TEST_CASE("region highlight covers whole middle lines (multi-line)")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "AAAA\nBBBB\nCCCC\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 40;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool mid = false;
    if (wait_for(master, "AAAA", std::chrono::seconds(8))) {
        (void)!::write(master, "\x01\x00\x0e\x0e", 4); // C-a C-SPC C-n C-n (region L1..L3)
        mid = wait_for(master, "\x1b[7mBBBB", std::chrono::seconds(8)); // middle line whole
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(mid);   // a fully-inside middle line renders in standout
}

TEST_CASE("M-h (mark-paragraph) activates the region highlight")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "PARA words here\nmore para text\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 40;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool mh = false;
    if (wait_for(master, "PARA", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bh", 2);   // M-h : mark-paragraph
        (void)!::write(master, "\x0c", 1);    // C-l : force a repaint
        mh = wait_for(master, "\x1b[7mPARA", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(mh);   // M-h set a *visible* region (regression: it skipped WMARKED)
}

TEST_CASE("kill ring: M-y (yank-pop) cycles through non-consecutive kills")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "AAA\nBBB\nCCC\nsink\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    std::string content;
    if (wait_for(master, "AAA", std::chrono::seconds(8))) {
        (void)!::write(master, "\x01\x0b\x0b", 3);   // line "AAA": C-a kill line + NL
        drain(master, 200);
        (void)!::write(master, "\x01\x0b\x0b", 3);   // now "BBB": new entry
        drain(master, 200);
        (void)!::write(master, "\x01\x0b\x0b", 3);   // now "CCC": new entry
        drain(master, 200);
        (void)!::write(master, "\x1b>", 2);          // M-> end of buffer
        (void)!::write(master, "\r", 1);             // newline
        (void)!::write(master, "\x19", 1);           // C-y  -> newest kill "CCC"
        (void)!::write(master, "\x1by", 2);          // M-y  -> "BBB"
        (void)!::write(master, "\x1by", 2);          // M-y  -> "AAA"
        (void)!::write(master, "\x18\x13", 2);       // C-x C-s
        (void)wait_for(master, "Wrote", std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    std::ifstream in(p); std::stringstream ss; ss << in.rdbuf();
    content = ss.str();
    fs::remove_all(dir);
    // After C-y + two M-y the yank is the OLDEST kill "AAA"; its survival
    // proves the ring kept all three (the single-buffer version lost them).
    CHECK(content.find("AAA") != std::string::npos);
}

TEST_CASE("kill ring: M-y without a preceding yank is refused")
{
    auto dir = make_temp_dir();
    std::ofstream(dir / "t.txt") << "hello\n";
    const std::string p = (dir / "t.txt").string();
    winsize ws{}; ws.ws_row = 24; ws.ws_col = 80;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", p.c_str(), (char *)nullptr); _exit(127); }
    bool refused = false;
    if (wait_for(master, "hello", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1by", 2);          // M-y with no prior yank
        refused = wait_for(master, "Previous command was not a yank",
                           std::chrono::seconds(8));
    }
    quit_neomg(master, pid);
    fs::remove_all(dir);
    CHECK(refused);
}
