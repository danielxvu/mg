// Integration tests for the mg.git libgit2 status reader (task M2c-2).
// The fixture repo is built with libgit2 itself -- no shell git invoked.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <git2.h>

import mg.magit;
import mg.git;

namespace fs = std::filesystem;
using namespace mg::magit;

namespace {
fs::path make_temp_dir()
{
    std::string buf = (fs::temp_directory_path() / "mg_git_XXXXXX").string();
    char *p = ::mkdtemp(buf.data());
    REQUIRE(p != nullptr);
    return fs::path(p);
}

// A repo with one staged file and one untracked file, created via libgit2.
fs::path make_repo_with_changes()
{
    auto dir = make_temp_dir();
    git_libgit2_init();

    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    std::ofstream(dir / "staged.txt") << "hello";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "staged.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);

    std::ofstream(dir / "untracked.txt") << "world"; // left unstaged

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

// A repo whose committed file has two far-apart regions changed on disk,
// producing two independent hunks in the unstaged diff.
fs::path make_repo_with_two_hunks()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);

    // 12 lines a..l; changes to line 2 and line 11 stay >6 lines apart, so
    // git's 3-line context never merges them into one hunk.
    commit_file(dir, "f.txt", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n", "base");
    std::ofstream(dir / "f.txt") << "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nK\nl\n";

    git_libgit2_shutdown();
    return dir;
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
// A repo with one commit, then a modification stashed away.
fs::path make_repo_with_stash(const char *stash_message)
{
    auto dir = make_repo_with_commit("base"); // a.txt committed as "content"
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);

    std::ofstream(dir / "a.txt") << "content changed"; // dirty -> stashable
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "Test", "t@example.com") == 0);
    git_oid stash_oid;
    REQUIRE(git_stash_save(&stash_oid, repo, sig, stash_message, 0) == 0);

    git_signature_free(sig);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}

// A repo with a commit (default branch) plus an extra branch `extra`.
fs::path make_repo_with_branch(const char *extra)
{
    auto dir = make_repo_with_commit("base");
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);

    git_oid head_oid;
    REQUIRE(git_reference_name_to_id(&head_oid, repo, "HEAD") == 0);
    git_commit *target = nullptr;
    REQUIRE(git_commit_lookup(&target, repo, &head_oid) == 0);
    git_reference *branch = nullptr;
    REQUIRE(git_branch_create(&branch, repo, extra, target, 0) == 0);

    git_reference_free(branch);
    git_commit_free(target);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
} // namespace

TEST_CASE("repo_status reports a staged and an untracked entry")
{
    auto dir = make_repo_with_changes();

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    REQUIRE(st->size() == 2);

    bool saw_staged = false, saw_untracked = false;
    for (const auto &e : *st) {
        if (e.path == "staged.txt") {
            CHECK(e.index == status::added);
            saw_staged = true;
        } else if (e.path == "untracked.txt") {
            CHECK(e.worktree == status::untracked);
            saw_untracked = true;
        }
    }
    CHECK(saw_staged);
    CHECK(saw_untracked);

    fs::remove_all(dir);
}

TEST_CASE("repo_status fails on a path that is not a git repository")
{
    auto dir = make_temp_dir(); // empty dir, no .git
    auto st = mg::git::repo_status(dir.string());
    CHECK_FALSE(st.has_value());
    fs::remove_all(dir);
}

TEST_CASE("read_head returns the branch and HEAD commit")
{
    auto dir = make_repo_with_commit("first commit\n\nbody text");
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK_FALSE(h->branch.empty());     // e.g. "master"
    CHECK(h->short_oid.size() == 8);
    CHECK(h->summary == "first commit"); // summary is the first line
    fs::remove_all(dir);
}

TEST_CASE("recent_commits returns commits, newest first")
{
    auto dir = make_repo_with_commit("only commit");
    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c.has_value());
    REQUIRE(c->size() == 1);
    CHECK((*c)[0].summary == "only commit");
    CHECK((*c)[0].short_oid.size() == 8);
    fs::remove_all(dir);
}

TEST_CASE("recent_commits on an unborn repo is empty (not an error)")
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);
    git_libgit2_shutdown();

    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c.has_value());
    CHECK(c->empty());
    fs::remove_all(dir);
}

TEST_CASE("stage() moves an untracked file into the index")
{
    auto dir = make_repo_with_changes(); // untracked.txt is untracked
    REQUIRE(mg::git::stage(dir.string(), "untracked.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    bool staged = false;
    for (const auto &e : *st)
        if (e.path == "untracked.txt")
            staged = (e.index == status::added);
    CHECK(staged);
    fs::remove_all(dir);
}

TEST_CASE("unstage() drops a staged-new file back to untracked")
{
    auto dir = make_repo_with_changes(); // staged.txt is a staged new file (unborn)
    REQUIRE(mg::git::unstage(dir.string(), "staged.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    bool untracked = false;
    for (const auto &e : *st)
        if (e.path == "staged.txt")
            untracked = (e.worktree == status::untracked);
    CHECK(untracked);
    fs::remove_all(dir);
}

TEST_CASE("unstage() resets a staged modification to HEAD")
{
    auto dir = make_repo_with_commit("base"); // a.txt committed with "content"

    // Modify a.txt and stage the modification.
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    std::ofstream(dir / "a.txt") << "changed";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();

    REQUIRE(mg::git::unstage(dir.string(), "a.txt").has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        if (e.path == "a.txt") {
            CHECK(e.index == status::unmodified);  // unstaged from the index
            CHECK(e.worktree == status::modified);  // still changed on disk
        }
    fs::remove_all(dir);
}

TEST_CASE("discard() deletes an untracked file")
{
    auto dir = make_repo_with_changes(); // untracked.txt is untracked
    REQUIRE(mg::git::discard(dir.string(), "untracked.txt").has_value());

    CHECK_FALSE(fs::exists(dir / "untracked.txt")); // gone from disk
    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        CHECK(e.path != "untracked.txt");
    fs::remove_all(dir);
}

TEST_CASE("stage_all stages every change (modified + untracked)")
{
    auto dir = make_repo_with_commit("base"); // a.txt = "content"
    std::ofstream(dir / "a.txt") << "modified";   // unstaged modification
    std::ofstream(dir / "b.txt") << "new";        // untracked

    REQUIRE(mg::git::stage_all(dir.string()).has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    int staged = 0, worktree_dirty = 0;
    for (const auto &e : *st) {
        if (e.index != status::unmodified)
            staged++;
        if (e.worktree != status::unmodified)
            worktree_dirty++;
    }
    CHECK(staged == 2);          // a.txt + b.txt staged
    CHECK(worktree_dirty == 0);  // nothing left unstaged
    fs::remove_all(dir);
}

TEST_CASE("unstage_all resets the index to HEAD, keeping the worktree")
{
    auto dir = make_repo_with_commit("base");
    std::ofstream(dir / "a.txt") << "modified";
    REQUIRE(mg::git::stage(dir.string(), "a.txt").has_value());
    std::ofstream(dir / "b.txt") << "new";
    REQUIRE(mg::git::stage(dir.string(), "b.txt").has_value());

    REQUIRE(mg::git::unstage_all(dir.string()).has_value());

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        CHECK(e.index == status::unmodified); // nothing staged
    // the worktree changes are still present
    bool a_modified = false, b_untracked = false;
    for (const auto &e : *st) {
        if (e.path == "a.txt" && e.worktree == status::modified)
            a_modified = true;
        if (e.path == "b.txt" && e.worktree == status::untracked)
            b_untracked = true;
    }
    CHECK(a_modified);
    CHECK(b_untracked);
    fs::remove_all(dir);
}

TEST_CASE("commit_amend replaces HEAD with the staged tree and new message")
{
    auto dir = make_repo_with_commit("first commit"); // a.txt = "content"
    set_test_config(dir);
    std::ofstream(dir / "a.txt") << "amended content";
    REQUIRE(mg::git::stage(dir.string(), "a.txt").has_value());

    auto r = mg::git::commit_amend(dir.string(), "amended message");
    REQUIRE(r.has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "amended message");
    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c.has_value());
    CHECK(c->size() == 1); // amended in place, not a new commit

    std::ifstream in(dir / "a.txt");
    std::string content;
    std::getline(in, content);
    CHECK(content == "amended content"); // worktree unchanged
    fs::remove_all(dir);
}

TEST_CASE("commit_extend keeps HEAD's message, adds the staged change")
{
    auto dir = make_repo_with_commit("keep this message");
    set_test_config(dir);
    std::ofstream(dir / "b.txt") << "new file";
    REQUIRE(mg::git::stage(dir.string(), "b.txt").has_value());

    REQUIRE(mg::git::commit_extend(dir.string()).has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "keep this message"); // message preserved
    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c->size() == 1);
    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        CHECK(e.path != "b.txt"); // b.txt is now committed
    fs::remove_all(dir);
}

TEST_CASE("commit_reword changes only HEAD's message")
{
    auto dir = make_repo_with_commit("typo mesage");
    set_test_config(dir);

    REQUIRE(mg::git::commit_reword(dir.string(), "fixed message").has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "fixed message");
    auto c = mg::git::recent_commits(dir.string(), 5);
    REQUIRE(c->size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("head_message returns HEAD's full commit message")
{
    auto dir = make_repo_with_commit("subject line\n\nbody text here");
    auto m = mg::git::head_message(dir.string());
    REQUIRE(m.has_value());
    CHECK(m->find("subject line") != std::string::npos);
    CHECK(m->find("body text here") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("file_diff returns hunks for an unstaged change")
{
    auto dir = make_repo_with_commit("base"); // a.txt committed as "content"
    std::ofstream(dir / "a.txt") << "content\nmore line\n"; // unstaged edit

    auto d = mg::git::file_diff(dir.string(), "a.txt", /*staged=*/false);
    REQUIRE(d.has_value());
    REQUIRE_FALSE(d->empty());

    bool added = false;
    for (const auto &h : *d)
        for (const auto &l : h.lines)
            if (l.origin == '+' && l.content.find("more line") != std::string::npos)
                added = true;
    CHECK(added);
    fs::remove_all(dir);
}

TEST_CASE("commit() creates a commit from the staged tree")
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    git_config *cfg = nullptr;
    REQUIRE(git_repository_config(&cfg, repo) == 0);
    git_config_set_string(cfg, "user.name", "Test");
    git_config_set_string(cfg, "user.email", "t@example.com");
    git_config_free(cfg);

    std::ofstream(dir / "f.txt") << "hello";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "f.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();

    auto r = mg::git::commit(dir.string(), "my first commit");
    REQUIRE(r.has_value());
    CHECK(r->size() == 8); // short oid

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "my first commit");

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        CHECK(e.path != "f.txt"); // committed, no longer staged
    fs::remove_all(dir);
}

TEST_CASE("stage_hunk stages only the selected hunk of a two-hunk change")
{
    auto dir = make_repo_with_two_hunks();

    auto unstaged_hunks = [&] {
        auto d = mg::git::file_diff(dir.string(), "f.txt", false);
        return d ? *d : std::vector<mg::git::hunk>{};
    };
    REQUIRE(unstaged_hunks().size() == 2); // two independent hunks, none staged

    // Stage the first hunk (the 'B' change) only.
    REQUIRE(mg::git::stage_hunk(dir.string(), "f.txt", 0).has_value());

    auto staged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/true);
    REQUIRE(staged.has_value());
    bool staged_has_B = false, staged_has_K = false;
    for (const auto &h : *staged)
        for (const auto &l : h.lines) {
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                staged_has_B = true;
            if (l.origin == '+' && l.content.find('K') != std::string::npos)
                staged_has_K = true;
        }
    CHECK(staged_has_B);       // first hunk got staged
    CHECK_FALSE(staged_has_K); // second hunk did not

    // The second hunk ('K') remains unstaged.
    bool unstaged_has_K = false;
    for (const auto &h : unstaged_hunks())
        for (const auto &l : h.lines)
            if (l.origin == '+' && l.content.find('K') != std::string::npos)
                unstaged_has_K = true;
    CHECK(unstaged_has_K);
    fs::remove_all(dir);
}

TEST_CASE("unstage_hunk drops one staged hunk back to unstaged")
{
    auto dir = make_repo_with_two_hunks();
    // Stage everything, leaving two staged hunks.
    REQUIRE(mg::git::stage(dir.string(), "f.txt").has_value());
    auto staged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/true);
    REQUIRE(staged.has_value());
    REQUIRE(staged->size() == 2);

    // Unstage the first staged hunk (the 'B' change).
    REQUIRE(mg::git::unstage_hunk(dir.string(), "f.txt", 0).has_value());

    auto staged_after = mg::git::file_diff(dir.string(), "f.txt", true);
    REQUIRE(staged_after.has_value());
    bool staged_has_B = false, staged_has_K = false;
    for (const auto &h : *staged_after)
        for (const auto &l : h.lines) {
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                staged_has_B = true;
            if (l.origin == '+' && l.content.find('K') != std::string::npos)
                staged_has_K = true;
        }
    CHECK_FALSE(staged_has_B); // first hunk unstaged
    CHECK(staged_has_K);       // second hunk still staged

    // The 'B' change is back on the unstaged side.
    auto unstaged = mg::git::file_diff(dir.string(), "f.txt", false);
    REQUIRE(unstaged.has_value());
    bool unstaged_has_B = false;
    for (const auto &h : *unstaged)
        for (const auto &l : h.lines)
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                unstaged_has_B = true;
    CHECK(unstaged_has_B);
    fs::remove_all(dir);
}

TEST_CASE("stashes() lists a saved stash, newest first")
{
    auto dir = make_repo_with_stash("WIP on work");
    auto s = mg::git::stashes(dir.string());
    REQUIRE(s.has_value());
    REQUIRE(s->size() == 1);
    CHECK((*s)[0].index == 0);
    CHECK((*s)[0].message.find("WIP on work") != std::string::npos);
    CHECK((*s)[0].short_oid.size() == 8);
    fs::remove_all(dir);
}

TEST_CASE("stashes() on a repo with no stashes is empty (not an error)")
{
    auto dir = make_repo_with_commit("base");
    auto s = mg::git::stashes(dir.string());
    REQUIRE(s.has_value());
    CHECK(s->empty());
    fs::remove_all(dir);
}

TEST_CASE("stash_apply reapplies a stashed change, keeping the stash")
{
    auto dir = make_repo_with_stash("WIP"); // a.txt reverted to "content"
    REQUIRE(mg::git::stash_apply(dir.string(), 0).has_value());

    std::ifstream in(dir / "a.txt");
    std::string content;
    std::getline(in, content);
    CHECK(content == "content changed"); // change is back in the worktree

    auto s = mg::git::stashes(dir.string());
    REQUIRE(s.has_value());
    CHECK(s->size() == 1); // apply does not drop
    fs::remove_all(dir);
}

TEST_CASE("stash_drop removes the stash")
{
    auto dir = make_repo_with_stash("WIP");
    REQUIRE(mg::git::stash_drop(dir.string(), 0).has_value());

    auto s = mg::git::stashes(dir.string());
    REQUIRE(s.has_value());
    CHECK(s->empty());
    fs::remove_all(dir);
}

TEST_CASE("branches() lists local branches and flags HEAD")
{
    auto dir = make_repo_with_branch("feature");
    auto b = mg::git::branches(dir.string());
    REQUIRE(b.has_value());
    REQUIRE(b->size() == 2);

    bool saw_feature = false, head_flagged = false;
    int head_count = 0;
    for (const auto &e : *b) {
        if (e.name == "feature")
            saw_feature = true;
        if (e.is_head) {
            head_count++;
            head_flagged = (e.name != "feature"); // HEAD is the default branch
        }
    }
    CHECK(saw_feature);
    CHECK(head_flagged);
    CHECK(head_count == 1); // exactly one current branch
    fs::remove_all(dir);
}

TEST_CASE("checkout_branch switches HEAD to the named branch")
{
    auto dir = make_repo_with_branch("feature");
    REQUIRE(mg::git::checkout_branch(dir.string(), "feature").has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->branch == "feature");
    fs::remove_all(dir);
}

TEST_CASE("discard() reverts a modified tracked file to HEAD")
{
    auto dir = make_repo_with_commit("v1"); // a.txt committed as "content"
    std::ofstream(dir / "a.txt") << "modified content"; // unstaged change

    REQUIRE(mg::git::discard(dir.string(), "a.txt").has_value());

    std::ifstream in(dir / "a.txt");
    std::string content;
    std::getline(in, content);
    CHECK(content == "content"); // reverted to HEAD

    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    for (const auto &e : *st)
        CHECK(e.path != "a.txt"); // clean again
    fs::remove_all(dir);
}
