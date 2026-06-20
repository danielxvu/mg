// Integration test for the extern "C" magit bridge (task M2d-1).
// Exercises the C API end to end -- start the background monitor on a libgit2
// fixture repo, then read the published modeline through the bridge. No mg core.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
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
