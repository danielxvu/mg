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
