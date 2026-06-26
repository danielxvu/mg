// Integration test for the extern "C" magit bridge (task M2d-1).
// Exercises the C API end to end -- start the background monitor on a libgit2
// fixture repo, then read the published modeline through the bridge. No mg core.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <git2.h>

#include "bridge.h"

namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_bridge_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}

// One staged file + one untracked file (built with libgit2, no shell git).
fs::path make_repo_with_changes()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "staged.txt") << "hi";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);

    std::ofstream(dir / "untracked.txt") << "yo";
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

// A repo with one commit, then a staged change and an untracked file.
fs::path make_repo_full()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "base.txt") << "base";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "base.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid coid;
    REQUIRE(git_commit_create(&coid, repo, "HEAD", sig, sig, nullptr,
                              "initial commit", tree, 0, nullptr) == 0);
    git_signature_free(sig);
    git_tree_free(tree);

    std::ofstream(dir / "staged.txt") << "s";
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    std::ofstream(dir / "untracked.txt") << "u";

    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

bool any_line_has(const std::vector<std::string> &lines, const std::string &needle)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) {
        return l.find(needle) != std::string::npos;
    });
}

// a.txt committed, then modified in the worktree (an unstaged change).
fs::path make_repo_unstaged()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    std::ofstream(dir / "a.txt") << "line one\n";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid toid;
    REQUIRE(git_index_write_tree(&toid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &toid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid coid;
    REQUIRE(git_commit_create(&coid, repo, "HEAD", sig, sig, nullptr, "c1", tree,
                              0, nullptr) == 0);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    std::ofstream(dir / "a.txt") << "line one\nmore line\n"; // unstaged edit
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
// A repo whose committed file has two changes close enough to land in ONE
// unstaged hunk (line indices: 0 ' a, 1 -b, 2 +B, 3 ' c, 4 -d, 5 +D, 6 ' e).
fs::path make_repo_one_hunk()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    std::ofstream(dir / "f.txt") << "a\nb\nc\nd\ne\n";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "f.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid toid;
    REQUIRE(git_index_write_tree(&toid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &toid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid coid;
    REQUIRE(git_commit_create(&coid, repo, "HEAD", sig, sig, nullptr, "c1", tree,
                              0, nullptr) == 0);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    std::ofstream(dir / "f.txt") << "a\nB\nc\nD\ne\n"; // two unstaged changes
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
// A repo whose checked-out branch "topic" tracks "master" and is 1 ahead /
// 1 behind (empty commits on each side; upstream is the local master branch).
fs::path make_repo_ahead_behind()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    std::ofstream(dir / "a.txt") << "base\n";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid toid;
    REQUIRE(git_index_write_tree(&toid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &toid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid c1;
    REQUIRE(git_commit_create(&c1, repo, "HEAD", sig, sig, nullptr, "C1", tree,
                              0, nullptr) == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *topic = nullptr;
    REQUIRE(git_branch_create(&topic, repo, "topic", base, 0) == 0);
    REQUIRE(git_branch_set_upstream(topic, "master") == 0);
    git_reference_free(topic);
    const git_commit *parents[1] = {base};
    git_oid c2, c3;
    REQUIRE(git_commit_create(&c2, repo, "refs/heads/master", sig, sig, nullptr,
                              "C2", tree, 1, parents) == 0);
    REQUIRE(git_commit_create(&c3, repo, "refs/heads/topic", sig, sig, nullptr,
                              "C3", tree, 1, parents) == 0);
    REQUIRE(git_repository_set_head(repo, "refs/heads/topic") == 0);
    git_commit_free(base);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
// A repo with one commit, an extra branch, and one stashed modification.
fs::path make_repo_stash_branch()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    std::ofstream(dir / "a.txt") << "one\n";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid toid;
    REQUIRE(git_index_write_tree(&toid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &toid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    git_oid coid;
    REQUIRE(git_commit_create(&coid, repo, "HEAD", sig, sig, nullptr, "c1", tree,
                              0, nullptr) == 0);
    git_commit *target = nullptr;
    REQUIRE(git_commit_lookup(&target, repo, &coid) == 0);
    git_reference *branch = nullptr;
    REQUIRE(git_branch_create(&branch, repo, "feature", target, 0) == 0);
    git_reference_free(branch);
    git_commit_free(target);

    std::ofstream(dir / "a.txt") << "one\ntwo\n"; // dirty -> stashable
    git_oid soid;
    REQUIRE(git_stash_save(&soid, repo, sig, "WIP work", 0) == 0);

    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

// Commit `body` to `name` in `dir` (dir must be an initialized repo).
void commit_file(const fs::path &dir, const char *name, const std::string &body,
                 const char *message)
{
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    std::ofstream(dir / name) << body;
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, name) == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "Test", "t@example.com") == 0);
    git_oid head_oid;
    bool born = git_reference_name_to_id(&head_oid, repo, "HEAD") == 0;
    git_commit *parent = nullptr;
    if (born)
        REQUIRE(git_commit_lookup(&parent, repo, &head_oid) == 0);
    const git_commit *parents[1] = {parent};
    git_oid commit_oid;
    REQUIRE(git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                              message, tree, born ? 1 : 0,
                              born ? parents : nullptr) == 0);
    if (parent)
        git_commit_free(parent);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    git_repository_free(repo);
}

// Set user.name/user.email so git_signature_default works (amend/commit).
void set_test_config(const fs::path &dir)
{
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, repo) == 0);
    git_config_set_string(cfg, "user.name", "Test");
    git_config_set_string(cfg, "user.email", "t@example.com");
    git_config_free(cfg);
    git_repository_free(repo);
    git_libgit2_shutdown();
}

// A repo with a single commit on HEAD (built with libgit2).
fs::path make_repo_with_commit(const char *message)
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "a.txt") << "content";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);

    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);

    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "Test", "t@example.com") == 0);

    git_oid commit_oid;
    REQUIRE(git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                              message, tree, 0, nullptr) == 0);

    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
} // namespace

TEST_CASE("mg_magit_commit_amend / reword / head_message through the bridge")
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, repo) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    std::ofstream(dir / "f.txt") << "x";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "f.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();

    REQUIRE(mg_magit_commit(dir.string().c_str(), "original") == 1);
    // reword changes the message in place
    CHECK(mg_magit_commit_reword(dir.string().c_str(), "reworded") == 1);

    char buf[256] = {0};
    int len = mg_magit_head_message(dir.string().c_str(), buf, sizeof buf);
    CHECK(len > 0);
    CHECK(std::string(buf).find("reworded") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("mg_magit_checkout and mg_magit_stash_drop act through the bridge")
{
    auto dir = make_repo_stash_branch(); // 1 stash + branch "feature"

    // Checkout the feature branch -> status buffer reports it as current.
    REQUIRE(mg_magit_checkout(dir.string().c_str(), "feature") == 1);
    {
        std::vector<std::string> lines;
        mg_magit_status_buffer(
            dir.string().c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
            },
            &lines);
        CHECK(any_line_has(lines, "On branch feature"));
    }

    // Drop the stash -> the Stashes section disappears.
    REQUIRE(mg_magit_stash_drop(dir.string().c_str(), 0) == 1);
    {
        std::vector<std::string> lines;
        mg_magit_status_buffer(
            dir.string().c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
            },
            &lines);
        CHECK(!any_line_has(lines, "Stashes"));
    }
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_status_buffer emits Stashes and Branches sections")
{
    auto dir = make_repo_stash_branch();

    struct row { std::string line; int kind; };
    std::vector<row> rows;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);

    std::vector<std::string> lines;
    for (const auto &r : rows)
        lines.push_back(r.line);
    CHECK(any_line_has(lines, "Stashes (1)"));
    CHECK(any_line_has(lines, "WIP work"));
    CHECK(any_line_has(lines, "Branches (2)"));
    CHECK(any_line_has(lines, "feature"));

    bool stash_kind = false, branch_kind = false;
    for (const auto &r : rows) {
        if (r.kind == MG_LINE_STASH)
            stash_kind = true;
        if (r.kind == MG_LINE_BRANCH)
            branch_kind = true;
    }
    CHECK(stash_kind);
    CHECK(branch_kind);
    fs::remove_all(dir);
}

TEST_CASE("bridge publishes a summarized modeline for a repo")
{
    auto dir = make_repo_with_changes();

    mg_magit_start(dir.string().c_str());

    // The monitor publishes asynchronously on its thread; poll with a bound.
    bool dirty = false;
    for (int i = 0; i < 500 && !dirty; ++i) {
        if (mg_magit_take_dirty())
            dirty = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(dirty);

    char buf[128] = {0};
    int len = mg_magit_modeline(buf, sizeof buf);
    CHECK(len > 0);

    // Modeline is "<branch> git *1 ?1": a non-empty branch, then the status.
    std::string ml(buf);
    auto pos = ml.find(" git ");
    REQUIRE(pos != std::string::npos);
    CHECK(pos > 0);                            // non-empty branch prefix
    CHECK(ml.substr(pos + 1) == "git *1 ?1");  // status part unchanged

    mg_magit_stop();
    fs::remove_all(dir);
}

// Count this process's open file descriptors via /dev/fd (present on macOS and
// Linux). Used to prove the monitor opens no per-directory watch fds when there
// is nothing to watch.
static int count_open_fds()
{
    std::error_code ec;
    int n = 0;
    for (auto it = fs::directory_iterator("/dev/fd", ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
        ++n;
    return n;
}

// Regression (latent since the monitor was wired into the C core): launched
// OUTSIDE any git repository, mg_magit_start watched the launch directory
// unconditionally -- the macOS kqueue backend opens one fd per directory, so
// starting mg in a large non-repo tree (e.g. ~/src, 20k+ dirs) exhausted the
// process fd table and made every later fopen/opendir fail with EMFILE,
// breaking file-open, dired, and minibuffer completion. The monitor must stay
// inert outside a repo.
TEST_CASE("monitor opens no per-dir watch fds when launched outside a repo")
{
    auto nonrepo = make_temp_dir(); // a plain directory, NOT a git repo
    for (int i = 0; i < 64; ++i)
        fs::create_directories(nonrepo / ("d" + std::to_string(i)));

    const int before = count_open_fds();
    mg_magit_start(nonrepo.string().c_str());
    const int after = count_open_fds();

    // With no repo there is nothing to track, so the modeline is empty too.
    char buf[128] = {0};
    CHECK(mg_magit_modeline(buf, sizeof buf) == 0);

    mg_magit_stop();
    fs::remove_all(nonrepo);

    // Pre-fix (macOS): kqueue opens an fd for the dir + its 64 subdirs.
    // Post-fix: no repo -> no monitor -> no per-dir watch fds (a small slack
    // covers libgit2's transient discovery handles). On Linux inotify uses a
    // single fd regardless, so this mainly guards the macOS exhaustion path.
    CHECK(after - before < 8);
}

// FM-ASYNC-STATUS thread-safety gate. The monitor thread publishes snapshots
// (writing current_/snapshot_/snapshot_fp_ under its mutex and poking the wake
// pipe) while the UI thread hammers every read-side accessor the editor uses:
// the modeline copy, the dirty flag, the snapshot replay (which also recomputes
// the fingerprint), and the wake-pipe fd + drain. Run under ThreadSanitizer
// (preset cpp-tsan) the real assertion is "zero races reported"; reaching the
// end with the threads joined cleanly is the pass under a normal build.
TEST_CASE("monitor snapshot/modeline accessors are race-free under mutation")
{
    auto dir = make_repo_full();
    auto repo = dir.string();

    mg_magit_start(repo.c_str());

    std::atomic<bool> stop{false};
    auto sink = [](void *, const char *, int, const char *, int) {};

    std::thread ui([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            char buf[128];
            (void)mg_magit_modeline(buf, sizeof buf);
            (void)mg_magit_take_dirty();
            (void)mg_magit_status_snapshot(repo.c_str(), nullptr, 0, sink,
                                           nullptr);
            (void)mg_magit_wake_fd();
            mg_magit_drain_wake();
        }
    });

    // Keep the worker busy: each new file changes the workdir scan and the
    // index/fingerprint, forcing repeated publishes that race the UI reads.
    for (int i = 0; i < 60; ++i) {
        std::ofstream(dir / ("race" + std::to_string(i) + ".txt")) << i;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop.store(true, std::memory_order_relaxed);
    ui.join();
    mg_magit_stop();
    fs::remove_all(dir);
    CHECK(true);
}

TEST_CASE("mg_magit_status_buffer composes branch, sections, and commits")
{
    auto dir = make_repo_full();

    struct row { std::string line; int kind; std::string path; };
    std::vector<row> rows;
    int count = mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int kind, const char *path, int) {
            static_cast<std::vector<row> *>(ctx)->push_back(
                {line, kind, path ? path : ""});
        },
        &rows);

    std::vector<std::string> lines;
    for (const auto &r : rows)
        lines.push_back(r.line);

    CHECK(count == static_cast<int>(rows.size()));
    CHECK(any_line_has(lines, "On branch "));
    CHECK(any_line_has(lines, "initial commit"));     // HEAD summary + commit list
    CHECK(any_line_has(lines, "Untracked files (1)"));
    CHECK(any_line_has(lines, "Staged changes (1)"));
    CHECK(any_line_has(lines, "Recent commits"));

    // The file rows carry the right kind + path for staging.
    bool untracked_ok = false, staged_ok = false, section_ok = false;
    for (const auto &r : rows) {
        if (r.kind == MG_LINE_UNTRACKED && r.path == "untracked.txt")
            untracked_ok = true;
        if (r.kind == MG_LINE_STAGED && r.path == "staged.txt")
            staged_ok = true;
        // Section headers are tagged MG_LINE_SECTION for M-n/M-p navigation.
        if (r.kind == MG_LINE_SECTION && r.line.find("Staged changes") != std::string::npos)
            section_ok = true;
    }
    CHECK(untracked_ok);
    CHECK(staged_ok);
    CHECK(section_ok);

    fs::remove_all(dir);
}

TEST_CASE("mg_magit_stage stages the file at a path")
{
    auto dir = make_repo_with_changes(); // untracked.txt is untracked
    CHECK(mg_magit_stage(dir.string().c_str(), "untracked.txt") == 1);

    std::vector<std::string> lines;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int, const char *, int) {
            static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
        },
        &lines);
    CHECK(any_line_has(lines, "Staged changes (2)")); // staged.txt + untracked.txt
    CHECK(!any_line_has(lines, "Untracked files"));   // none left

    fs::remove_all(dir);
}

TEST_CASE("mg_magit_status_buffer emits diff lines for an expanded file")
{
    auto dir = make_repo_unstaged();
    const char *expanded[1] = {"a.txt"};

    struct row { std::string line; int kind; std::string path; int hunk; };
    std::vector<row> rows;
    mg_magit_status_buffer(
        dir.string().c_str(), expanded, 1,
        [](void *ctx, const char *line, int kind, const char *path, int hunk) {
            static_cast<std::vector<row> *>(ctx)->push_back(
                {line, kind, path ? path : "", hunk});
        },
        &rows);

    bool hunk_hdr = false, diff_add = false;
    for (const auto &r : rows) {
        if (r.kind == MG_LINE_HUNK && r.path == "a.txt" && r.hunk == 0)
            hunk_hdr = true;
        if (r.kind == MG_LINE_DIFF && r.path == "a.txt" &&
            r.line.find("more line") != std::string::npos)
            diff_add = true;
    }
    CHECK(hunk_hdr);
    CHECK(diff_add);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_stage_hunk stages just one hunk through the bridge")
{
    auto dir = make_repo_unstaged(); // a.txt: "line one\n" + unstaged "more line\n"
    CHECK(mg_magit_stage_hunk(dir.string().c_str(), "a.txt", 0) == 1);

    // The expanded staged view now carries the "more line" addition.
    const char *expanded[1] = {"a.txt"};
    struct row { std::string line; int kind; };
    std::vector<row> rows;
    mg_magit_status_buffer(
        dir.string().c_str(), expanded, 1,
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);

    bool staged_section = false, staged_add = false;
    for (const auto &r : rows) {
        if (r.line.find("Staged changes") != std::string::npos)
            staged_section = true;
        if (r.kind == MG_LINE_DIFF &&
            r.line.find("more line") != std::string::npos)
            staged_add = true;
    }
    CHECK(staged_section);
    CHECK(staged_add);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_stage_region stages just the selected lines through the bridge")
{
    auto dir = make_repo_one_hunk(); // one hunk: b->B and d->D
    // Stage only the b->B change (hunk 0, line indices 1..2).
    CHECK(mg_magit_stage_region(dir.string().c_str(), "f.txt", 0, 1, 2) == 1);

    // The expanded staged view carries +B but not +D.
    const char *expanded[1] = {"f.txt"};
    struct row { std::string line; int kind; };
    std::vector<row> rows;
    mg_magit_status_buffer(
        dir.string().c_str(), expanded, 1,
        [](void *ctx, const char *line, int kind, const char *, int) {
            static_cast<std::vector<row> *>(ctx)->push_back({line, kind});
        },
        &rows);

    // Scan only the Staged section's diff lines (the Unstaged section still
    // shows +D, which we must not mistake for a staged change).
    bool in_staged = false, staged_B = false, staged_D = false;
    for (const auto &r : rows) {
        if (r.line.find("Staged changes") != std::string::npos)
            in_staged = true;
        else if (r.line.find("Unstaged changes") != std::string::npos)
            in_staged = false;
        if (in_staged && r.kind == MG_LINE_DIFF && r.line[0] == '+') {
            if (r.line.find('B') != std::string::npos)
                staged_B = true;
            if (r.line.find('D') != std::string::npos)
                staged_D = true;
        }
    }
    CHECK(staged_B);
    CHECK_FALSE(staged_D);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_status_buffer shows the upstream and ahead/behind counts")
{
    auto dir = make_repo_ahead_behind(); // topic tracks master, 1 ahead/1 behind
    std::string text;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int, const char *, int) {
            (static_cast<std::string *>(ctx))->append(line).append("\n");
        },
        &text);
    CHECK(text.find("master") != std::string::npos);   // upstream name
    CHECK(text.find("ahead 1") != std::string::npos);
    CHECK(text.find("behind 1") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_status_buffer emits Unpushed and Unpulled sections")
{
    auto dir = make_repo_ahead_behind(); // C3 unpushed, C2 unpulled
    std::string text;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int, const char *, int) {
            (static_cast<std::string *>(ctx))->append(line).append("\n");
        },
        &text);
    CHECK(text.find("Unpushed") != std::string::npos);
    CHECK(text.find("C3") != std::string::npos);
    CHECK(text.find("Unpulled") != std::string::npos);
    CHECK(text.find("C2") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_log_buffer emits commit lines carrying the full oid")
{
    auto dir = make_repo_one_hunk(); // one commit "c1" on the default branch
    struct row { std::string line; int kind; std::string path; };
    std::vector<row> rows;
    int n = mg_magit_log_buffer(
        dir.string().c_str(), 10,
        [](void *ctx, const char *line, int kind, const char *path, int) {
            static_cast<std::vector<row> *>(ctx)->push_back(
                {line, kind, path ? path : ""});
        },
        &rows);
    REQUIRE(n >= 1);
    bool found = false;
    std::string oid;
    for (const auto &r : rows)
        if (r.kind == MG_LINE_COMMIT && r.line.find("c1") != std::string::npos) {
            found = true;
            oid = r.path;
        }
    CHECK(found);
    CHECK(oid.size() == 40); // full oid in the path

    // That oid feeds mg_magit_commit_diff, which emits the commit's diff.
    std::string text;
    int dn = mg_magit_commit_diff(
        dir.string().c_str(), oid.c_str(),
        [](void *ctx, const char *line, int, const char *, int) {
            static_cast<std::string *>(ctx)->append(line).append("\n");
        },
        &text);
    CHECK(dn >= 1);
    CHECK(text.find("commit ") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_log_file_buffer emits only commits that touched the file")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        if (!std::filesystem::exists(dir / ".git"))
            REQUIRE(git_repository_init(&r2, repo.c_str(), 0) == 0);
        else
            REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        git_config *cfg = nullptr;
        REQUIRE(git_repository_config(&cfg, r2) == 0);
        git_config_set_string(cfg, "user.name", "T");
        git_config_set_string(cfg, "user.email", "t@t");
        git_config_free(cfg);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, r2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, r2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, r2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, r2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, r2, "HEAD", sig, sig, nullptr, msg, tree,
                                  born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(r2);
        git_libgit2_shutdown();
    };
    commit("a.txt", "a\n", "C1");
    commit("target.txt", "t\n", "add target");
    commit("a.txt", "a\nb\n", "edit a");
    commit("target.txt", "t\nu\n", "edit target");

    std::vector<std::string> lines;
    int n = mg_magit_log_file_buffer(
        repo.c_str(), "target.txt", 50,
        [](void *ctx, const char *line, int, const char *, int) {
            static_cast<std::vector<std::string> *>(ctx)->push_back(line);
        },
        &lines);
    CHECK(n == 2);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("edit target") != std::string::npos); // newest first
    CHECK(lines[1].find("add target") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_log_query_buffer -S finds the introducing commit")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    git_libgit2_init();
    commit_file(dir, "f.txt", "x\nNEEDLE_XYZ\ny\n", "introduce needle");
    commit_file(dir, "g.txt", "noise\n", "noise");
    git_libgit2_shutdown();

    struct cap { std::vector<std::string> lines; int others = 0; } c;
    int n = mg_magit_log_query_buffer(
        dir.string().c_str(), /*graph=*/0, /*range=*/nullptr, /*file=*/nullptr,
        /*pickaxe_kind=*/'S', /*pickaxe_term=*/"NEEDLE_XYZ", /*n=*/0,
        [](void *ctx, const char *line, int kind, const char *, int) {
            auto *p = static_cast<cap *>(ctx);
            p->lines.emplace_back(line);
            if (kind == MG_LINE_OTHER)
                p->others++;
        },
        &c);
    REQUIRE(n == 1);
    CHECK(c.lines[0].find("introduce needle") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_stash_push then stash_pop round-trip through the bridge")
{
    auto dir = make_repo_one_hunk(); // committed f.txt with a dirty change
    auto repo = dir.string();
    auto status_text = [&] {
        std::string t;
        mg_magit_status_buffer(
            repo.c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                (static_cast<std::string *>(ctx))->append(line).append("\n");
            },
            &t);
        return t;
    };

    CHECK(mg_magit_stash_push(repo.c_str(), "WIP") == 1);
    CHECK(status_text().find("Stashes (1)") != std::string::npos); // stashed

    CHECK(mg_magit_stash_pop(repo.c_str(), 0) == 1);
    CHECK(status_text().find("Stashes") == std::string::npos);     // popped
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_push and mg_magit_pull act through the bridge")
{
    // Bare remote + working repo with origin configured + branch pushed.
    auto bare = make_temp_dir();
    git_libgit2_init();
    git_repository *braw = nullptr;
    REQUIRE(git_repository_init(&braw, bare.string().c_str(), 1) == 0);
    git_repository_free(braw);
    git_libgit2_shutdown();

    auto work = make_repo_one_hunk(); // committed f.txt + a dirty change
    auto wp = work.string();
    git_libgit2_init();
    git_repository *wraw = nullptr;
    REQUIRE(git_repository_open(&wraw, wp.c_str()) == 0);
    git_reference *head = nullptr;
    REQUIRE(git_repository_head(&head, wraw) == 0);
    std::string branch = git_reference_shorthand(head);
    git_reference_free(head);
    git_remote *remote = nullptr;
    REQUIRE(git_remote_create(&remote, wraw, "origin", bare.string().c_str()) == 0);
    git_remote_free(remote);
    git_repository_free(wraw);
    git_libgit2_shutdown();

    // Push the current branch to the bare remote, then a no-op pull (no new
    // upstream commits) should still succeed (already up to date).
    CHECK(mg_magit_push(wp.c_str(), "origin", 0, 0) == 1);
    CHECK(mg_magit_pull(wp.c_str(), "origin") == 1);

    // The bare repo now has the branch.
    git_libgit2_init();
    git_repository *b2 = nullptr;
    REQUIRE(git_repository_open(&b2, bare.string().c_str()) == 0);
    git_oid tip;
    CHECK(git_reference_name_to_id(&tip, b2, ("refs/heads/" + branch).c_str()) == 0);
    git_repository_free(b2);
    git_libgit2_shutdown();
    fs::remove_all(work);
    fs::remove_all(bare);
}

TEST_CASE("mg_magit_rebase replays the current branch onto upstream")
{
    // master: C1 -> C2 (b.txt); feature: C1 -> C3 (c.txt); HEAD = feature.
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r = nullptr;
    REQUIRE(git_repository_init(&r, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r);
    git_libgit2_shutdown();

    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *repo2 = nullptr;
        REQUIRE(git_repository_open(&repo2, repo.c_str()) == 0);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, repo2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, repo2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, repo2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, repo2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, repo2, "HEAD", sig, sig, nullptr, msg,
                                  tree, born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(repo2);
        git_libgit2_shutdown();
    };

    commit("a.txt", "a\n", "C1");
    REQUIRE(mg_magit_branch_create(repo.c_str(), "feature") == 1);
    commit("b.txt", "b\n", "C2 on master"); // advances the default branch
    REQUIRE(mg_magit_checkout(repo.c_str(), "feature") == 1);
    commit("c.txt", "c\n", "C3 on feature");

    CHECK(mg_magit_rebase(repo.c_str(), "master") == 1); // clean -> done
    // After rebase, feature includes master's b.txt (C2 is now an ancestor).
    CHECK(std::filesystem::exists(dir / "b.txt"));
    CHECK(std::filesystem::exists(dir / "c.txt"));
    // A bogus upstream fails gracefully.
    CHECK(mg_magit_rebase(repo.c_str(), "no-such-ref") == 0);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_tag_create/delete and the Tags section through the bridge")
{
    auto dir = make_repo_one_hunk(); // a committed repo
    auto repo = dir.string();
    auto status_text = [&] {
        std::string t;
        mg_magit_status_buffer(
            repo.c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                (static_cast<std::string *>(ctx))->append(line).append("\n");
            },
            &t);
        return t;
    };

    CHECK(status_text().find("Tags") == std::string::npos); // none yet
    CHECK(mg_magit_tag_create(repo.c_str(), "v2.0", "HEAD", NULL) == 1);
    std::string s = status_text();
    CHECK(s.find("Tags (1)") != std::string::npos);
    CHECK(s.find("v2.0") != std::string::npos);

    CHECK(mg_magit_tag_delete(repo.c_str(), "v2.0") == 1);
    CHECK(status_text().find("Tags") == std::string::npos); // gone
    CHECK(mg_magit_tag_delete(repo.c_str(), "v2.0") == 0);  // already gone
    fs::remove_all(dir);
}

// Regression: a tag-heavy repo (d20app has ~2900 tags) must not emit one line
// per tag -- that bloated the status buffer to thousands of lines, which was
// slow to build + render and showed as a hanging/blank *magit-status*. The
// Tags section is capped; the full count stays in the header.
TEST_CASE("the Tags section is capped on a tag-heavy repo")
{
    auto dir = make_repo_one_hunk(); // a committed repo
    auto repo = dir.string();
    for (int i = 0; i < 30; ++i) {
        std::string name = "v" + std::to_string(i);
        REQUIRE(mg_magit_tag_create(repo.c_str(), name.c_str(), "HEAD", NULL) == 1);
    }

    struct Ctx {
        int tag_lines = 0;
        std::string text;
    } c;
    mg_magit_status_buffer(
        repo.c_str(), nullptr, 0,
        [](void *p, const char *line, int kind, const char *, int) {
            auto *cc = static_cast<Ctx *>(p);
            if (kind == MG_LINE_TAG)
                ++cc->tag_lines;
            cc->text.append(line).append("\n");
        },
        &c);

    CHECK(c.text.find("Tags (30)") != std::string::npos); // header: true count
    CHECK(c.tag_lines <= 20);                             // but the list is bounded
    CHECK(c.text.find("more") != std::string::npos);      // "... and N more"
    fs::remove_all(dir);
}

TEST_CASE("the Submodules section lists registered submodules through the bridge")
{
    auto parent = make_repo_one_hunk();
    auto sub = make_repo_one_hunk();
    auto repo = parent.string();
    auto status_text = [&] {
        std::string t;
        mg_magit_status_buffer(
            repo.c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                (static_cast<std::string *>(ctx))->append(line).append("\n");
            },
            &t);
        return t;
    };

    CHECK(status_text().find("Submodules") == std::string::npos); // none yet

    git_libgit2_init();
    git_repository *r = nullptr;
    REQUIRE(git_repository_open(&r, repo.c_str()) == 0);
    git_submodule *sm = nullptr;
    REQUIRE(git_submodule_add_setup(&sm, r, ("file://" + sub.string()).c_str(),
                                    "vendor/lib", 1) == 0);
    git_submodule_free(sm);
    git_repository_free(r);
    git_libgit2_shutdown();

    std::string s = status_text();
    CHECK(s.find("Submodules (1)") != std::string::npos);
    CHECK(s.find("vendor/lib") != std::string::npos);
    fs::remove_all(parent);
    fs::remove_all(sub);
}

TEST_CASE("mg_magit_cherrypick applies another branch's commit onto HEAD")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *repo2 = nullptr;
        REQUIRE(git_repository_open(&repo2, repo.c_str()) == 0);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, repo2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, repo2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, repo2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, repo2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, repo2, "HEAD", sig, sig, nullptr, msg,
                                  tree, born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(repo2);
        git_libgit2_shutdown();
    };

    commit("a.txt", "a\n", "C1");
    // The default branch name (libgit2 may use master or main).
    git_libgit2_init();
    git_repository *rb = nullptr;
    REQUIRE(git_repository_open(&rb, repo.c_str()) == 0);
    git_reference *hr = nullptr;
    REQUIRE(git_repository_head(&hr, rb) == 0);
    std::string base = git_reference_shorthand(hr);
    git_reference_free(hr);
    git_repository_free(rb);
    git_libgit2_shutdown();

    REQUIRE(mg_magit_branch_create(repo.c_str(), "other") == 1);
    REQUIRE(mg_magit_checkout(repo.c_str(), "other") == 1);
    commit("e.txt", "E\n", "add e on other");
    REQUIRE(mg_magit_checkout(repo.c_str(), base.c_str()) == 1);
    REQUIRE_FALSE(std::filesystem::exists(dir / "e.txt"));

    CHECK(mg_magit_cherrypick(repo.c_str(), "other") == 1);
    CHECK(std::filesystem::exists(dir / "e.txt")); // picked onto base
    CHECK(mg_magit_cherrypick(repo.c_str(), "no-such-rev") == 0);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_rebase_interactive drops a commit via the plan")
{
    // feature: C1 -> C2(b) -> C3(c) -> C4(d) on the default branch (no extra
    // branch needed -- onto = C1).
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *repo2 = nullptr;
        REQUIRE(git_repository_open(&repo2, repo.c_str()) == 0);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, repo2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, repo2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, repo2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, repo2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, repo2, "HEAD", sig, sig, nullptr, msg,
                                  tree, born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(repo2);
        git_libgit2_shutdown();
    };
    auto oid_of = [&](const char *rev) {
        git_libgit2_init();
        git_repository *repo2 = nullptr;
        REQUIRE(git_repository_open(&repo2, repo.c_str()) == 0);
        git_object *o = nullptr;
        REQUIRE(git_revparse_single(&o, repo2, rev) == 0);
        char buf[GIT_OID_HEXSZ + 1];
        git_oid_tostr(buf, sizeof buf, git_object_id(o));
        git_object_free(o);
        git_repository_free(repo2);
        git_libgit2_shutdown();
        return std::string(buf);
    };

    commit("a.txt", "a\n", "C1");
    commit("b.txt", "b\n", "C2");
    commit("c.txt", "c\n", "C3");
    commit("d.txt", "d\n", "C4");
    std::string c1 = oid_of("HEAD~3"), c2 = oid_of("HEAD~2");
    std::string c3 = oid_of("HEAD~1"), c4 = oid_of("HEAD");

    mg_magit_rebase_step steps[3] = {
        {0, c2.c_str()}, // pick C2
        {1, c3.c_str()}, // drop C3
        {0, c4.c_str()}, // pick C4
    };
    CHECK(mg_magit_rebase_interactive(repo.c_str(), c1.c_str(), steps, 3) == 1);
    CHECK(std::filesystem::exists(dir / "b.txt"));
    CHECK_FALSE(std::filesystem::exists(dir / "c.txt")); // dropped
    CHECK(std::filesystem::exists(dir / "d.txt"));
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_rebase_interactive stops at edit (3); continue finishes")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, r2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, r2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, r2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, r2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, r2, "HEAD", sig, sig, nullptr, msg, tree,
                                  born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(r2);
        git_libgit2_shutdown();
    };
    auto oid_of = [&](const char *rev) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        git_object *o = nullptr;
        REQUIRE(git_revparse_single(&o, r2, rev) == 0);
        char buf[GIT_OID_HEXSZ + 1];
        git_oid_tostr(buf, sizeof buf, git_object_id(o));
        git_object_free(o);
        git_repository_free(r2);
        git_libgit2_shutdown();
        return std::string(buf);
    };

    commit("a.txt", "a\n", "C1");
    commit("b.txt", "b\n", "C2");
    commit("c.txt", "c\n", "C3");
    commit("d.txt", "d\n", "C4");
    std::string c1 = oid_of("HEAD~3"), c2 = oid_of("HEAD~2");
    std::string c3 = oid_of("HEAD~1"), c4 = oid_of("HEAD");

    mg_magit_rebase_step steps[3] = {
        {0, c2.c_str(), nullptr}, // pick C2
        {5, c3.c_str(), nullptr}, // edit C3: stop
        {0, c4.c_str(), nullptr}, // pick C4
    };
    CHECK(mg_magit_rebase_interactive(repo.c_str(), c1.c_str(), steps, 3) == 3);
    CHECK(mg_magit_rebase_in_progress(repo.c_str()) == 1);
    CHECK(std::filesystem::exists(dir / "c.txt"));        // stopped at C3
    CHECK_FALSE(std::filesystem::exists(dir / "d.txt"));  // C4 not yet

    // Amend during the stop, then continue.
    std::ofstream(dir / "edited.txt") << "x\n";
    REQUIRE(mg_magit_stage(repo.c_str(), "edited.txt") == 1);
    REQUIRE(mg_magit_commit(repo.c_str(), "edit work") == 1);
    CHECK(mg_magit_rebase_continue(repo.c_str()) == 1);   // done
    CHECK(mg_magit_rebase_in_progress(repo.c_str()) == 0);
    CHECK(std::filesystem::exists(dir / "d.txt"));         // C4 replayed
    CHECK(std::filesystem::exists(dir / "edited.txt"));    // edit survived
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_bisect finds the first bad commit through the bridge")
{
    // a.txt(good) -> b.txt -> c.txt(bug) -> d.txt, all on the default branch.
    auto dir = make_temp_dir();
    auto repo = dir.string();
    auto commit = [&](const char *file, const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        if (!std::filesystem::exists(dir / ".git"))
            REQUIRE(git_repository_init(&r2, repo.c_str(), 0) == 0);
        else
            REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        git_config *cfg = nullptr;
        REQUIRE(git_repository_config(&cfg, r2) == 0);
        git_config_set_string(cfg, "user.name", "T");
        git_config_set_string(cfg, "user.email", "t@t");
        git_config_free(cfg);
        std::ofstream(dir / file) << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, r2) == 0);
        REQUIRE(git_index_add_bypath(idx, file) == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, r2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, r2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, r2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, r2, "HEAD", sig, sig, nullptr, msg, tree,
                                  born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(r2);
        git_libgit2_shutdown();
    };
    commit("a.txt", "a\n", "C1");
    commit("b.txt", "b\n", "C2");
    commit("c.txt", "c\n", "C3"); // the bug enters here
    commit("d.txt", "d\n", "C4");

    char msg[256] = {0};
    CHECK(mg_magit_bisect_start(repo.c_str(), "HEAD", "HEAD~3", msg, sizeof msg) == 1);
    CHECK(mg_magit_bisect_active(repo.c_str()) == 1);

    // c.txt present in the working tree == the checked-out commit is bad.
    for (int guard = 0; guard < 10; ++guard) {
        int bad = std::filesystem::exists(dir / "c.txt") ? 1 : 0;
        CHECK(mg_magit_bisect_mark(repo.c_str(), bad, msg, sizeof msg) == 1);
        if (std::string(msg).find("first bad commit") != std::string::npos)
            break;
    }
    CHECK(std::string(msg).find("first bad commit") != std::string::npos);

    CHECK(mg_magit_bisect_reset(repo.c_str()) == 1);
    CHECK(mg_magit_bisect_active(repo.c_str()) == 0);
    CHECK(std::filesystem::exists(dir / "d.txt")); // back at the branch tip
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_rebase pauses on conflict; abort clears the in-progress state")
{
    // feature and master both change a.txt -> rebasing feature conflicts.
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *repo2 = nullptr;
        REQUIRE(git_repository_open(&repo2, repo.c_str()) == 0);
        std::ofstream(dir / "a.txt") << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, repo2) == 0);
        REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, repo2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, repo2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, repo2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, repo2, "HEAD", sig, sig, nullptr, msg,
                                  tree, born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(repo2);
        git_libgit2_shutdown();
    };

    commit("base\n", "C1");
    REQUIRE(mg_magit_branch_create(repo.c_str(), "feature") == 1);
    commit("master change\n", "C2 master");
    REQUIRE(mg_magit_checkout(repo.c_str(), "feature") == 1);
    commit("feature change\n", "C3 feature");

    CHECK(mg_magit_rebase(repo.c_str(), "master") == 2); // paused on conflict
    CHECK(mg_magit_rebase_in_progress(repo.c_str()) == 1);
    CHECK(mg_magit_rebase_abort(repo.c_str()) == 1);
    CHECK(mg_magit_rebase_in_progress(repo.c_str()) == 0);
    fs::remove_all(dir);
}

TEST_CASE("Conflicts section + resolve, then continue the rebase via the bridge")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        std::ofstream(dir / "a.txt") << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, r2) == 0);
        REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, r2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, r2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, r2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, r2, "HEAD", sig, sig, nullptr, msg, tree,
                                  born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(r2);
        git_libgit2_shutdown();
    };
    auto status_text = [&] {
        std::string t;
        mg_magit_status_buffer(
            repo.c_str(), nullptr, 0,
            [](void *ctx, const char *line, int, const char *, int) {
                static_cast<std::string *>(ctx)->append(line).append("\n");
            },
            &t);
        return t;
    };

    commit("base\n", "C1");
    REQUIRE(mg_magit_branch_create(repo.c_str(), "feature") == 1);
    commit("master change\n", "C2 master");
    REQUIRE(mg_magit_checkout(repo.c_str(), "feature") == 1);
    commit("feature change\n", "C3 feature");

    CHECK(mg_magit_rebase(repo.c_str(), "master") == 2); // paused on conflict
    std::string s = status_text();
    CHECK(s.find("Conflicts (1)") != std::string::npos);
    CHECK(s.find("a.txt") != std::string::npos);

    // Keep theirs (the replayed feature commit) and continue.
    CHECK(mg_magit_resolve_conflict(repo.c_str(), "a.txt", 1) == 1);
    CHECK(status_text().find("Conflicts") == std::string::npos); // resolved
    CHECK(mg_magit_rebase_continue(repo.c_str()) == 1);          // done
    CHECK(mg_magit_rebase_in_progress(repo.c_str()) == 0);

    std::ifstream in(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "feature change\n");
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_conflict_hunks emits regions; resolve_conflict_hunk rewrites")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_repository_free(r0);
    git_libgit2_shutdown();

    std::ofstream(dir / "f.txt")
        << "top\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> x\n"
        << "mid\n<<<<<<< HEAD\nA\n=======\nB\n>>>>>>> x\nbot\n";

    int headers = 0;
    struct ctx { int *headers; } c{&headers};
    int n = mg_magit_conflict_hunks(
        repo.c_str(), "f.txt",
        [](void *p, const char *line, int kind, const char *, int) {
            if (kind == MG_LINE_CONFLICT_HUNK &&
                std::string(line).rfind("Conflict ", 0) == 0)
                ++*static_cast<ctx *>(p)->headers;
        },
        &c);
    CHECK(n > 0);
    CHECK(headers == 2); // two regions

    // Resolve region 0 -> ours; one region remains.
    CHECK(mg_magit_resolve_conflict_hunk(repo.c_str(), "f.txt", 0, 0) == 1);
    headers = 0;
    mg_magit_conflict_hunks(
        repo.c_str(), "f.txt",
        [](void *p, const char *line, int kind, const char *, int) {
            if (kind == MG_LINE_CONFLICT_HUNK &&
                std::string(line).rfind("Conflict ", 0) == 0)
                ++*static_cast<ctx *>(p)->headers;
        },
        &c);
    CHECK(headers == 1);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_conflict_hunk_side refines the words unique to each side")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_repository_free(r0);
    git_libgit2_shutdown();

    std::ofstream(dir / "f.txt")
        << "<<<<<<< HEAD\nthe quick brown fox\n=======\n"
        << "the slow brown fox\n>>>>>>> x\n";

    auto side = [&](int s) {
        std::string t;
        mg_magit_conflict_hunk_side(
            repo.c_str(), "f.txt", 0, s,
            [](void *ctx, const char *line, int, const char *, int) {
                static_cast<std::string *>(ctx)->append(line).append("\n");
            },
            &t);
        return t;
    };
    CHECK(side(0).find("the [-quick-] brown fox") != std::string::npos);
    CHECK(side(1).find("the {+slow+} brown fox") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_merge returns 2 (left conflicts), then commit completes it")
{
    auto dir = make_temp_dir();
    auto repo = dir.string();
    git_libgit2_init();
    git_repository *r0 = nullptr;
    REQUIRE(git_repository_init(&r0, repo.c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, r0) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    git_repository_free(r0);
    git_libgit2_shutdown();

    auto commit = [&](const char *body, const char *msg) {
        git_libgit2_init();
        git_repository *r2 = nullptr;
        REQUIRE(git_repository_open(&r2, repo.c_str()) == 0);
        std::ofstream(dir / "a.txt") << body;
        git_index *idx = nullptr;
        REQUIRE(git_repository_index(&idx, r2) == 0);
        REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
        REQUIRE(git_index_write(idx) == 0);
        git_oid toid;
        REQUIRE(git_index_write_tree(&toid, idx) == 0);
        git_tree *tree = nullptr;
        REQUIRE(git_tree_lookup(&tree, r2, &toid) == 0);
        git_signature *sig = nullptr;
        REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
        git_oid head;
        bool born = git_reference_name_to_id(&head, r2, "HEAD") == 0;
        git_commit *parent = nullptr;
        if (born)
            git_commit_lookup(&parent, r2, &head);
        const git_commit *parents[1] = {parent};
        git_oid out;
        REQUIRE(git_commit_create(&out, r2, "HEAD", sig, sig, nullptr, msg, tree,
                                  born ? 1 : 0, born ? parents : nullptr) == 0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(idx);
        git_repository_free(r2);
        git_libgit2_shutdown();
    };

    commit("base\n", "C1");
    REQUIRE(mg_magit_branch_create(repo.c_str(), "feature") == 1);
    commit("main change\n", "C2 main");
    REQUIRE(mg_magit_checkout(repo.c_str(), "feature") == 1);
    commit("feature change\n", "C3 feature");

    CHECK(mg_magit_merge(repo.c_str(), "master") == 2); // left conflicts
    CHECK(mg_magit_resolve_conflict(repo.c_str(), "a.txt", 1) == 1); // theirs
    CHECK(mg_magit_commit(repo.c_str(), "Merge master") == 1);       // completes

    // Two-parent merge commit, in-progress state cleared.
    git_libgit2_init();
    git_repository *r = nullptr;
    REQUIRE(git_repository_open(&r, repo.c_str()) == 0);
    CHECK(git_repository_state(r) == GIT_REPOSITORY_STATE_NONE);
    git_oid head;
    REQUIRE(git_reference_name_to_id(&head, r, "HEAD") == 0);
    git_commit *c = nullptr;
    REQUIRE(git_commit_lookup(&c, r, &head) == 0);
    CHECK(git_commit_parentcount(c) == 2);
    git_commit_free(c);
    git_repository_free(r);
    git_libgit2_shutdown();
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_revert and mg_magit_merge act through the bridge")
{
    // make_repo_one_hunk: committed f.txt with a dirty working-tree change.
    // Commit that change so there are two commits, then revert the top one.
    auto dir = make_repo_one_hunk();
    auto repo = dir.string();
    REQUIRE(mg_magit_stage(repo.c_str(), "f.txt") == 1);
    REQUIRE(mg_magit_commit(repo.c_str(), "second") == 1);

    auto head = [&] {
        char buf[256] = {0};
        mg_magit_head_message(repo.c_str(), buf, sizeof buf);
        return std::string(buf);
    };
    CHECK(head() == "second");

    // Revert HEAD -> a new "Revert ..." commit on top.
    char oid[64] = {0};
    {   // grab HEAD's oid via the log buffer
        struct cap { std::string oid; } c;
        mg_magit_log_buffer(
            repo.c_str(), 1,
            [](void *ctx, const char *, int kind, const char *path, int) {
                if (kind == MG_LINE_COMMIT && path)
                    static_cast<cap *>(ctx)->oid = path;
            },
            &c);
        REQUIRE(c.oid.size() == 40);
        std::snprintf(oid, sizeof oid, "%s", c.oid.c_str());
    }
    CHECK(mg_magit_revert(repo.c_str(), oid) == 1);
    CHECK(head().find("Revert") != std::string::npos);

    // Bad branch name -> merge fails gracefully.
    CHECK(mg_magit_merge(repo.c_str(), "no-such-branch") == 0);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_branch_create/rename/delete act through the bridge")
{
    auto dir = make_repo_one_hunk(); // a commit on the default branch
    auto repo = dir.string();

    CHECK(mg_magit_branch_create(repo.c_str(), "feature") == 1);
    CHECK(mg_magit_branch_rename(repo.c_str(), "feature", "feature2") == 1);
    CHECK(mg_magit_branch_delete(repo.c_str(), "feature2") == 1);
    CHECK(mg_magit_branch_delete(repo.c_str(), "feature2") == 0); // already gone
    CHECK(mg_magit_branch_create(repo.c_str(), "") == 0);         // empty name
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_discard removes an untracked file")
{
    auto dir = make_repo_with_changes(); // untracked.txt is untracked
    CHECK(mg_magit_discard(dir.string().c_str(), "untracked.txt") == 1);
    CHECK_FALSE(fs::exists(dir / "untracked.txt"));
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_commit commits the staged tree")
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, repo) == 0);
    git_config_set_string(cfg, "user.name", "T");
    git_config_set_string(cfg, "user.email", "t@t");
    git_config_free(cfg);
    std::ofstream(dir / "f.txt") << "x";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "f.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();

    CHECK(mg_magit_commit(dir.string().c_str(), "bridge commit") == 1);

    std::vector<std::string> lines;
    mg_magit_status_buffer(
        dir.string().c_str(), nullptr, 0,
        [](void *ctx, const char *line, int, const char *, int) {
            static_cast<std::vector<std::string> *>(ctx)->emplace_back(line);
        },
        &lines);
    CHECK(any_line_has(lines, "bridge commit"));   // appears under Recent commits
    CHECK(!any_line_has(lines, "Staged changes")); // nothing staged now
    fs::remove_all(dir);
}

TEST_CASE("BENCH bridge buffer builds (set MG_BENCH_REPO[, MG_BENCH_FILE])")
{
	const char *repo = std::getenv("MG_BENCH_REPO");
	if (repo == nullptr)
		return;
	const char *file = std::getenv("MG_BENCH_FILE");
	static int sink;
	auto count = [](void *ctx, const char *, int, const char *, int) {
		(void)ctx;
		sink++;
	};
	auto bench = [&](const char *name, auto fn) {
		double best = 1e18;
		int n = 0;
		for (int i = 0; i < 3; ++i) {
			auto t0 = std::chrono::steady_clock::now();
			n = fn();
			auto t1 = std::chrono::steady_clock::now();
			double ms =
			    std::chrono::duration<double, std::milli>(t1 - t0).count();
			if (ms < best)
				best = ms;
		}
		MESSAGE(name << ": " << best << " ms (lines=" << n << ")");
	};
	bench("status_buffer (collapsed)", [&] {
		return mg_magit_status_buffer(repo, nullptr, 0, count, nullptr);
	});
	bench("log_buffer(100)          ", [&] {
		return mg_magit_log_buffer(repo, 100, count, nullptr);
	});
	bench("commit_diff(HEAD)        ", [&] {
		return mg_magit_commit_diff(repo, "HEAD", count, nullptr);
	});
	if (file != nullptr) {
		const char *ex[1] = {file};
		bench("status_buffer (1 expanded)", [&] {
			return mg_magit_status_buffer(repo, ex, 1, count, nullptr);
		});
		bench("log_file_buffer(100)     ", [&] {
			return mg_magit_log_file_buffer(repo, file, 100, count, nullptr);
		});
		bench("blame_file               ", [&] {
			return mg_magit_blame_file(repo, file, count, nullptr);
		});
	}

	// FM-ASYNC-STATUS: with the monitor warm, the UI's status build is the
	// snapshot replay (a vector copy + fingerprint stat/read-head + emit) --
	// the expensive scan/revwalk/ref-enum that dominates status_buffer above
	// has moved to the worker thread. Compare this against status_buffer.
	mg_magit_start(repo);
	for (int i = 0; i < 500 && !mg_magit_take_dirty(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	bench("status_snapshot (warm UI) ", [&] {
		return mg_magit_status_snapshot(repo, nullptr, 0, count, nullptr);
	});
	mg_magit_stop();
}

TEST_CASE("status snapshot replay is byte-identical to the synchronous build")
{
	auto dir = make_repo_one_hunk(); // f.txt committed + two unstaged edits
	auto repo = dir.string();
	// Add a staged change (a 2nd file) + an untracked file for full coverage.
	std::ofstream(dir / "g.txt") << "x\ny\n";
	REQUIRE(mg_magit_stage(repo.c_str(), "g.txt") == 1);
	std::ofstream(dir / "u.txt") << "untracked\n";

	struct row { std::string s; };
	auto collect = [&](bool snap, const char *const *ex, int nex) {
		std::vector<std::string> v;
		auto cb = [](void *ctx, const char *line, int kind, const char *path,
		             int hunk) {
			char buf[64];
			snprintf(buf, sizeof buf, "%d|%s|%d|", kind, path ? path : "", hunk);
			static_cast<std::vector<std::string> *>(ctx)->push_back(
			    std::string(buf) + (line ? line : ""));
		};
		if (snap)
			mg_magit_status_snapshot(repo.c_str(), ex, nex, cb, &v);
		else
			mg_magit_status_buffer(repo.c_str(), ex, nex, cb, &v);
		return v;
	};

	CHECK(collect(true, nullptr, 0) == collect(false, nullptr, 0));   // collapsed
	const char *ex[2] = {"f.txt", "g.txt"};
	CHECK(collect(true, ex, 2) == collect(false, ex, 2));             // expanded
	CHECK(!collect(true, ex, 2).empty());
	fs::remove_all(dir);
}

// FM-ASYNC-BLAME determinism anchor: the worker's captured result for a blame /
// a log-file must equal the synchronous bridge output for the same repo+path,
// line-for-line (kind|path|hunk|text). This pins the async path to the proven
// sync path -- the worker must change *when* the work runs, not *what* it emits.
namespace {
void fmt_capture(void *ctx, const char *line, int kind, const char *path,
                 int hunk)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%d|%s|%d|", kind, path ? path : "", hunk);
	static_cast<std::vector<std::string> *>(ctx)->push_back(
	    std::string(buf) + (line ? line : ""));
}
void discard(void *, const char *, int, const char *, int) {}
} // namespace

TEST_CASE("async blame/log-file results match the synchronous build")
{
	auto dir = make_repo_one_hunk(); // f.txt committed
	auto repo = dir.string();
	mg_magit_start(repo.c_str());

	auto sync_lines = [&](int kind) {
		std::vector<std::string> v;
		if (kind == MG_ASYNC_BLAME)
			mg_magit_blame_file(repo.c_str(), "f.txt", fmt_capture, &v);
		else
			mg_magit_log_file_buffer(repo.c_str(), "f.txt", 50, fmt_capture, &v);
		return v;
	};

	auto async_lines = [&](int kind) {
		unsigned gen = mg_magit_async_request(kind, repo.c_str(), "f.txt", 50);
		REQUIRE(gen > 0);
		std::vector<std::string> v;
		for (int i = 0; i < 500; ++i) {
			int k;
			char p[256];
			unsigned g;
			bool got = false;
			while (mg_magit_async_peek(&k, p, sizeof p, &g) >= 0) {
				if (g == gen) {
					CHECK(k == kind);
					CHECK(std::string(p) == "f.txt");
					mg_magit_async_take(fmt_capture, &v);
					got = true;
					break;
				}
				mg_magit_async_take(discard, nullptr); // drop a stale result
			}
			if (got)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return v;
	};

	CHECK(async_lines(MG_ASYNC_BLAME) == sync_lines(MG_ASYNC_BLAME));
	CHECK(async_lines(MG_ASYNC_LOG_FILE) == sync_lines(MG_ASYNC_LOG_FILE));
	CHECK(!async_lines(MG_ASYNC_BLAME).empty());

	mg_magit_stop();
	fs::remove_all(dir);
}

// FM-ASYNC-BLAME thread-safety gate. The UI thread hammers request/peek/take
// while the worker thread drains and publishes results -- racing the mailbox
// (pending_ + gen_), the ready queue, and the shared wake pipe. Under TSan
// (cpp-tsan) the assertion is zero races; reaching the end joined cleanly is
// the pass under a normal build.
TEST_CASE("async request/peek/take is race-free against the worker")
{
	auto dir = make_repo_one_hunk();
	auto repo = dir.string();
	mg_magit_start(repo.c_str());

	for (int i = 0; i < 200; ++i) {
		int kind = (i & 1) ? MG_ASYNC_LOG_FILE : MG_ASYNC_BLAME;
		(void)mg_magit_async_request(kind, repo.c_str(), "f.txt", 20);
		int k;
		char p[256];
		unsigned g;
		while (mg_magit_async_peek(&k, p, sizeof p, &g) >= 0)
			mg_magit_async_take(discard, nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// drain the tail
	for (int i = 0; i < 200; ++i) {
		int k;
		char p[256];
		unsigned g;
		if (mg_magit_async_peek(&k, p, sizeof p, &g) >= 0)
			mg_magit_async_take(discard, nullptr);
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	mg_magit_stop();
	fs::remove_all(dir);
	CHECK(true);
}

// Latest-wins: rapid-fire requests for the same kind always deliver the most
// recent generation, and never a generation we never asked for.
TEST_CASE("async delivers the latest request, never a phantom generation")
{
	auto dir = make_repo_one_hunk();
	auto repo = dir.string();
	mg_magit_start(repo.c_str());

	unsigned last = 0;
	for (int i = 0; i < 4; ++i)
		last = mg_magit_async_request(MG_ASYNC_BLAME, repo.c_str(), "f.txt", 0);
	REQUIRE(last > 0);

	bool saw_last = false;
	for (int i = 0; i < 500 && !saw_last; ++i) {
		int k;
		char p[256];
		unsigned g;
		while (mg_magit_async_peek(&k, p, sizeof p, &g) >= 0) {
			CHECK(g <= last); // never newer than anything we requested
			mg_magit_async_take(discard, nullptr);
			if (g == last)
				saw_last = true;
		}
		if (!saw_last)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(saw_last);

	mg_magit_stop();
	fs::remove_all(dir);
}
