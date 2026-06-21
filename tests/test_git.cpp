// Integration tests for the mg.git libgit2 status reader (task M2c-2).
// The fixture repo is built with libgit2 itself -- no shell git invoked.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// A repo whose committed file has two changes close enough (within git's
// 3-line context) to land in ONE unstaged hunk -- the substrate for region
// staging, where we stage only part of a single hunk.
fs::path make_repo_with_one_hunk_two_changes()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);

    // Lines 2 (b->B) and 4 (d->D) are only one line apart, so the 3-line
    // context overlaps and git emits a single hunk covering both.
    commit_file(dir, "f.txt", "a\nb\nc\nd\ne\n", "base");
    std::ofstream(dir / "f.txt") << "a\nB\nc\nD\ne\n";

    git_libgit2_shutdown();
    return dir;
}

// A repo whose checked-out branch "topic" tracks "master" and is 1 commit
// ahead (one commit only on topic) and 1 behind (one commit only on master).
// No remote: the upstream is a local branch (branch.topic.remote = ".").
fs::path make_repo_ahead_behind()
{
    auto dir = make_temp_dir();
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);

    commit_file(dir, "a.txt", "base\n", "C1"); // master @ C1, HEAD -> master

    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_commit_tree(&tree, base) == 0);

    // Branch topic at C1, tracking master.
    git_reference *topic = nullptr;
    REQUIRE(git_branch_create(&topic, repo, "topic", base, 0) == 0);
    REQUIRE(git_branch_set_upstream(topic, "master") == 0);
    git_reference_free(topic);

    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    const git_commit *parents[1] = {base};
    git_oid c2, c3;
    // C2 on master (topic is now 1 behind); C3 on topic (1 ahead). Reusing
    // C1's tree makes them empty commits -- only the graph shape matters here.
    REQUIRE(git_commit_create(&c2, repo, "refs/heads/master", sig, sig, nullptr,
                              "C2", tree, 1, parents) == 0);
    REQUIRE(git_commit_create(&c3, repo, "refs/heads/topic", sig, sig, nullptr,
                              "C3", tree, 1, parents) == 0);
    REQUIRE(git_repository_set_head(repo, "refs/heads/topic") == 0);

    git_signature_free(sig);
    git_tree_free(tree);
    git_commit_free(base);
    git_repository_free(repo);
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
// master at C1; branch "feature" one commit ahead (adds b.txt). HEAD stays on
// master, so merging feature is a fast-forward.
fs::path make_repo_ff_branch()
{
    auto dir = make_repo_with_commit("C1"); // master @ C1 with a.txt
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);

    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *feature = nullptr;
    REQUIRE(git_branch_create(&feature, repo, "feature", base, 0) == 0);
    git_reference_free(feature);

    // Build a tree with a.txt (from C1) + a new b.txt, commit it onto feature.
    std::ofstream(dir / "b.txt") << "bee\n";
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "b.txt") == 0);
    git_oid tree_oid;
    REQUIRE(git_index_write_tree(&tree_oid, idx) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_tree_lookup(&tree, repo, &tree_oid) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    const git_commit *parents[1] = {base};
    git_oid c2;
    REQUIRE(git_commit_create(&c2, repo, "refs/heads/feature", sig, sig, nullptr,
                              "C2 on feature", tree, 1, parents) == 0);
    // Leave the working tree/index matching C1 (remove the staged b.txt).
    REQUIRE(git_index_remove_bypath(idx, "b.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    std::filesystem::remove(dir / "b.txt");

    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(idx);
    git_commit_free(base);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return dir;
}
// master and feature diverge by touching DIFFERENT files (a clean merge):
// C1 base; feature adds b.txt; master then changes a.txt. HEAD on master.
fs::path make_repo_diverged()
{
    auto dir = make_repo_ff_branch(); // master @ C1; feature 1 ahead (b.txt)
    // Advance master with its own commit (changes a.txt), diverging from feature.
    commit_file(dir, "a.txt", "content\nmaster line\n", "C2 on master");
    return dir;
}
// HEAD on "feature" (C1 -> C3 adds c.txt); "master" is C1 -> C2 adds b.txt.
// Different files -> rebasing feature onto master is clean. Built with the
// engine's own checkout + the commit_file helper.
fs::path make_repo_for_rebase()
{
    auto dir = make_repo_with_commit("C1"); // master @ C1 (a.txt), HEAD master
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *feat = nullptr;
    REQUIRE(git_branch_create(&feat, repo, "feature", base, 0) == 0);
    git_reference_free(feat);
    git_commit_free(base);
    git_repository_free(repo);
    git_libgit2_shutdown();

    commit_file(dir, "b.txt", "bee\n", "C2 on master"); // master advances
    REQUIRE(mg::git::checkout_branch(dir.string(), "feature").has_value());
    commit_file(dir, "c.txt", "cee\n", "C3 on feature"); // feature advances
    return dir;
}

// Like make_repo_for_rebase but master and feature change the SAME line of
// a.txt, so rebasing feature onto master conflicts. HEAD = feature.
fs::path make_repo_rebase_conflict()
{
    auto dir = make_repo_with_commit("C1"); // a.txt = "content", HEAD master
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *feat = nullptr;
    REQUIRE(git_branch_create(&feat, repo, "feature", base, 0) == 0);
    git_reference_free(feat);
    git_commit_free(base);
    git_repository_free(repo);
    git_libgit2_shutdown();

    commit_file(dir, "a.txt", "master change\n", "C2 master");
    REQUIRE(mg::git::checkout_branch(dir.string(), "feature").has_value());
    commit_file(dir, "a.txt", "feature change\n", "C3 feature");
    return dir;
}

// HEAD on "feature" = master(C1) -> C2(b.txt) -> C3(c.txt) -> C4(d.txt);
// master stays at C1. For exercising interactive-rebase plans over C2/C3/C4.
fs::path make_repo_for_interactive()
{
    auto dir = make_repo_with_commit("C1"); // master @ C1 (a.txt), HEAD master
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *feat = nullptr;
    REQUIRE(git_branch_create(&feat, repo, "feature", base, 0) == 0);
    git_reference_free(feat);
    git_commit_free(base);
    git_repository_free(repo);
    git_libgit2_shutdown();

    REQUIRE(mg::git::checkout_branch(dir.string(), "feature").has_value());
    commit_file(dir, "b.txt", "B\n", "C2");
    commit_file(dir, "c.txt", "C\n", "C3");
    commit_file(dir, "d.txt", "D\n", "C4");
    return dir;
}

// Resolve the conflicted a.txt to `body` and stage it (clears the conflict).
void resolve_and_stage(const fs::path &dir, const char *body)
{
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    std::ofstream(dir / "a.txt") << body;
    git_index *idx = nullptr;
    REQUIRE(git_repository_index(&idx, repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "a.txt") == 0); // stages + clears conflict
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
}

// A working repo with `origin` pointing at a local bare repo (file path, no
// network), its current branch pushed there. Returns work dir, bare dir, and
// the branch name.
struct remote_fixture {
    fs::path work;
    fs::path bare;
    std::string branch;
};
remote_fixture make_repo_with_remote()
{
    auto bare = make_temp_dir();
    auto work = make_repo_with_commit("C1"); // a.txt = "content"
    git_libgit2_init();

    git_repository *braw = nullptr;
    REQUIRE(git_repository_init(&braw, bare.string().c_str(), /*bare=*/1) == 0);
    git_repository_free(braw);

    git_repository *wraw = nullptr;
    REQUIRE(git_repository_open(&wraw, work.string().c_str()) == 0);

    // Current branch shorthand (libgit2's default may be master or main).
    git_reference *head = nullptr;
    REQUIRE(git_repository_head(&head, wraw) == 0);
    std::string branch = git_reference_shorthand(head);
    git_reference_free(head);

    git_remote *remote = nullptr;
    REQUIRE(git_remote_create(&remote, wraw, "origin",
                              bare.string().c_str()) == 0);
    std::string spec = "refs/heads/" + branch + ":refs/heads/" + branch;
    char *specs[1] = {const_cast<char *>(spec.c_str())};
    git_strarray refspecs = {specs, 1};
    git_push_options popts;
    git_push_options_init(&popts, GIT_PUSH_OPTIONS_VERSION);
    REQUIRE(git_remote_push(remote, &refspecs, &popts) == 0);
    git_remote_free(remote);
    git_repository_free(wraw);
    git_libgit2_shutdown();
    return {work, bare, branch};
}

// Add an empty commit (reusing the tip's tree) onto `refname` in the repo at
// `dir`; works on a bare repo. Returns nothing.
void advance_ref(const fs::path &dir, const std::string &refname,
                 const char *message)
{
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid tip;
    REQUIRE(git_reference_name_to_id(&tip, repo, refname.c_str()) == 0);
    git_commit *parent = nullptr;
    REQUIRE(git_commit_lookup(&parent, repo, &tip) == 0);
    git_tree *tree = nullptr;
    REQUIRE(git_commit_tree(&tree, parent) == 0);
    git_signature *sig = nullptr;
    REQUIRE(git_signature_now(&sig, "T", "t@t") == 0);
    const git_commit *parents[1] = {parent};
    git_oid out;
    REQUIRE(git_commit_create(&out, repo, refname.c_str(), sig, sig, nullptr,
                              message, tree, 1, parents) == 0);
    git_signature_free(sig);
    git_tree_free(tree);
    git_commit_free(parent);
    git_repository_free(repo);
    git_libgit2_shutdown();
}
} // namespace

TEST_CASE("resolve_userpass uses the URL username and prompts for the rest")
{
    // Stub prompt: hidden -> "secret", visible -> "alice"; counts its calls.
    static int calls;
    calls = 0;
    mg::git::cred_prompt p = [](const char *, int hidden, char *out, int n,
                                void *) -> int {
        ++calls;
        std::snprintf(out, n, "%s", hidden ? "secret" : "alice");
        return 1;
    };

    // Username present in the URL -> only the password is prompted.
    auto r1 = mg::git::resolve_userpass("bob", p, nullptr);
    REQUIRE(r1.has_value());
    CHECK(r1->user == "bob");
    CHECK(r1->pass == "secret");
    CHECK(calls == 1);

    // No URL username -> both prompted.
    calls = 0;
    auto r2 = mg::git::resolve_userpass(nullptr, p, nullptr);
    REQUIRE(r2.has_value());
    CHECK(r2->user == "alice");
    CHECK(r2->pass == "secret");
    CHECK(calls == 2);
}

TEST_CASE("resolve_userpass fails on cancel or with no prompt")
{
    mg::git::cred_prompt cancel = [](const char *, int, char *, int,
                                     void *) -> int { return 0; };
    CHECK_FALSE(mg::git::resolve_userpass("x", cancel, nullptr).has_value());
    CHECK_FALSE(mg::git::resolve_userpass(nullptr, nullptr, nullptr).has_value());
}

TEST_CASE("fetch_remote fails gracefully on an unreachable remote")
{
    auto dir = make_repo_with_commit("C1");
    git_libgit2_init();
    git_repository *wraw = nullptr;
    REQUIRE(git_repository_open(&wraw, dir.string().c_str()) == 0);
    git_remote *remote = nullptr;
    // A bogus local path: no repo there -> fetch must error, not crash, with
    // the credentials callback wired in.
    REQUIRE(git_remote_create(&remote, wraw, "origin",
                              "/nonexistent/mg-no-such-repo.git") == 0);
    git_remote_free(remote);
    git_repository_free(wraw);
    git_libgit2_shutdown();

    CHECK_FALSE(mg::git::fetch_remote(dir.string(), "origin").has_value());
    fs::remove_all(dir);
}

TEST_CASE("push_remote uploads the current branch to the remote")
{
    auto fx = make_repo_with_remote();
    commit_file(fx.work, "a.txt", "content\nlocal change\n", "C2 local");

    REQUIRE(mg::git::push_remote(fx.work.string(), "origin").has_value());

    // The bare repo's branch now matches the local HEAD.
    auto local = mg::git::read_head(fx.work.string());
    REQUIRE(local.has_value());
    CHECK(local->summary == "C2 local");
    git_libgit2_init();
    git_repository *braw = nullptr;
    REQUIRE(git_repository_open(&braw, fx.bare.string().c_str()) == 0);
    git_oid bare_tip;
    CHECK(git_reference_name_to_id(&bare_tip, braw,
                                   ("refs/heads/" + fx.branch).c_str()) == 0);
    git_repository_free(braw);
    git_libgit2_shutdown();
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("fetch_remote updates the remote-tracking ref")
{
    auto fx = make_repo_with_remote();
    advance_ref(fx.bare, "refs/heads/" + fx.branch, "C2 upstream"); // bare ahead

    REQUIRE(mg::git::fetch_remote(fx.work.string(), "origin").has_value());

    // refs/remotes/origin/<branch> now exists and points at the new commit.
    git_libgit2_init();
    git_repository *wraw = nullptr;
    REQUIRE(git_repository_open(&wraw, fx.work.string().c_str()) == 0);
    git_oid tracking;
    CHECK(git_reference_name_to_id(
              &tracking, wraw,
              ("refs/remotes/origin/" + fx.branch).c_str()) == 0);
    git_repository_free(wraw);
    git_libgit2_shutdown();
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("pull_remote fast-forwards HEAD onto upstream changes")
{
    auto fx = make_repo_with_remote();
    advance_ref(fx.bare, "refs/heads/" + fx.branch, "C2 upstream"); // bare ahead

    REQUIRE(mg::git::pull_remote(fx.work.string(), "origin").has_value());

    auto h = mg::git::read_head(fx.work.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C2 upstream"); // fast-forwarded to the upstream commit
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("push_remote set_upstream records the branch's upstream")
{
    auto fx = make_repo_with_remote();
    set_test_config(fx.work);

    REQUIRE(mg::git::push_remote(fx.work.string(), "origin", false, true)
                .has_value());
    auto up = mg::git::upstream_status(fx.work.string());
    REQUIRE(up.has_value());
    CHECK(up->has_upstream);
    CHECK(up->name == "origin/" + fx.branch);
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("push_remote force pushes a rewritten history that a plain push rejects")
{
    auto fx = make_repo_with_remote();
    set_test_config(fx.work);
    // Rewrite local history so it diverges from what the remote has.
    REQUIRE(mg::git::commit_amend(fx.work.string(), "amended C1").has_value());

    CHECK_FALSE(mg::git::push_remote(fx.work.string(), "origin").has_value());
    REQUIRE(mg::git::push_remote(fx.work.string(), "origin", true).has_value());

    // The bare repo's branch now matches the rewritten local HEAD.
    auto h = mg::git::read_head(fx.work.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "amended C1");
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("pull_rebase brings the branch up to the remote")
{
    auto fx = make_repo_with_remote();
    set_test_config(fx.work);
    advance_ref(fx.bare, "refs/heads/" + fx.branch, "C2 upstream"); // bare ahead

    auto r = mg::git::pull_rebase(fx.work.string(), "origin");
    REQUIRE(r.has_value());
    CHECK(*r == mg::git::rebase_result::done);
    auto h = mg::git::read_head(fx.work.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C2 upstream");
    fs::remove_all(fx.work);
    fs::remove_all(fx.bare);
}

TEST_CASE("rebase_onto replays the branch's commits on top of upstream")
{
    auto dir = make_repo_for_rebase(); // HEAD=feature (C1->C3); master=C1->C2

    REQUIRE(mg::git::rebase_onto(dir.string(), "master").has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->branch == "feature");
    CHECK(h->summary == "C3 on feature");

    // C2 is now an ancestor of feature's replayed tip: all three commits are
    // reachable from HEAD (order is by commit time, which collides in the
    // fixture, so check membership rather than position).
    auto cs = mg::git::recent_commits(dir.string(), 10);
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 3);
    bool c1 = false, c2 = false, c3 = false;
    for (const auto &c : *cs) {
        c1 = c1 || c.summary == "C1";
        c2 = c2 || c.summary == "C2 on master";
        c3 = c3 || c.summary == "C3 on feature";
    }
    CHECK(c1);
    CHECK(c2); // C2 was NOT reachable from feature before the rebase
    CHECK(c3);
    CHECK(fs::exists(dir / "b.txt")); // from C2
    CHECK(fs::exists(dir / "c.txt")); // from C3
    fs::remove_all(dir);
}

// HEAD on the default branch (C1); branch "other" = C1 -> adds e.txt. For
// cherry-picking "other"'s commit onto the default branch.
fs::path make_repo_cherrypick(std::string &base_branch_out)
{
    auto dir = make_repo_with_commit("C1"); // a.txt, HEAD on the default branch
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    base_branch_out = h->branch;

    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_oid c1;
    REQUIRE(git_reference_name_to_id(&c1, repo, "HEAD") == 0);
    git_commit *base = nullptr;
    REQUIRE(git_commit_lookup(&base, repo, &c1) == 0);
    git_reference *other = nullptr;
    REQUIRE(git_branch_create(&other, repo, "other", base, 0) == 0);
    git_reference_free(other);
    git_commit_free(base);
    git_repository_free(repo);
    git_libgit2_shutdown();

    REQUIRE(mg::git::checkout_branch(dir.string(), "other").has_value());
    commit_file(dir, "e.txt", "E\n", "add e on other");
    REQUIRE(mg::git::checkout_branch(dir.string(), base_branch_out).has_value());
    return dir;
}

// Resolve a revspec (e.g. "feature~2") to its full oid -- topologically exact,
// unlike recent_commits' time order (the fixture's commits share a timestamp).
static std::string oid_of(const fs::path &dir, const char *rev)
{
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_object *o = nullptr;
    REQUIRE(git_revparse_single(&o, repo, rev) == 0);
    char buf[GIT_OID_HEXSZ + 1];
    git_oid_tostr(buf, sizeof buf, git_object_id(o));
    git_object_free(o);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return std::string(buf);
}

TEST_CASE("cherry_pick applies another branch's commit onto HEAD")
{
    std::string base;
    auto dir = make_repo_cherrypick(base); // HEAD=base(C1); other adds e.txt

    REQUIRE_FALSE(fs::exists(dir / "e.txt")); // not on base yet
    REQUIRE(mg::git::cherry_pick(dir.string(), "other").has_value());

    CHECK(fs::exists(dir / "e.txt")); // picked onto base
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->branch == base);
    CHECK(h->summary == "add e on other");
    fs::remove_all(dir);
}

TEST_CASE("cherry_pick fails on a bad revision")
{
    std::string base;
    auto dir = make_repo_cherrypick(base);
    CHECK_FALSE(mg::git::cherry_pick(dir.string(), "no-such-rev").has_value());
    fs::remove_all(dir);
}

TEST_CASE("commits_range lists commits after onto, oldest first")
{
    auto dir = make_repo_for_interactive(); // feature: C1->C2->C3->C4

    auto cs = mg::git::commits_range(dir.string(), "master"); // master = C1
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 3);
    CHECK((*cs)[0].summary == "C2"); // oldest first
    CHECK((*cs)[1].summary == "C3");
    CHECK((*cs)[2].summary == "C4");
    CHECK((*cs)[0].oid.size() == 40);
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive drops a commit from the middle of the plan")
{
    using mg::git::rebase_action;
    using mg::git::rebase_step;
    auto dir = make_repo_for_interactive(); // feature: C1->C2(b)->C3(c)->C4(d)

    // plan oldest-first: pick C2, drop C3, pick C4.
    std::vector<rebase_step> plan = {
        {rebase_action::pick, oid_of(dir, "feature~2")}, // C2
        {rebase_action::drop, oid_of(dir, "feature~1")}, // C3
        {rebase_action::pick, oid_of(dir, "feature")},   // C4
    };
    REQUIRE(mg::git::rebase_interactive(dir.string(), "master", plan).has_value());

    CHECK(fs::exists(dir / "b.txt"));        // C2 kept
    CHECK_FALSE(fs::exists(dir / "c.txt"));  // C3 dropped
    CHECK(fs::exists(dir / "d.txt"));        // C4 kept
    auto after = mg::git::recent_commits(dir.string(), 10);
    REQUIRE(after.has_value());
    CHECK(after->size() == 3); // C1, C2', C4'
    for (const auto &c : *after)
        CHECK(c.summary != "C3");
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive reword replaces a commit's message")
{
    using mg::git::rebase_action;
    using mg::git::rebase_step;
    auto dir = make_repo_for_interactive(); // C2/C3/C4

    std::vector<rebase_step> plan = {
        {rebase_action::pick, oid_of(dir, "feature~2"), ""},          // C2
        {rebase_action::reword, oid_of(dir, "feature~1"), "reworded C3"}, // C3
        {rebase_action::pick, oid_of(dir, "feature"), ""},            // C4
    };
    REQUIRE(mg::git::rebase_interactive(dir.string(), "master", plan).has_value());

    auto after = mg::git::recent_commits(dir.string(), 10);
    REQUIRE(after.has_value());
    bool reworded = false, old = false;
    for (const auto &c : *after) {
        reworded = reworded || c.summary == "reworded C3";
        old = old || c.summary == "C3";
    }
    CHECK(reworded);
    CHECK_FALSE(old);
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive edit stops at the marked commit, continue resumes")
{
    using mg::git::rebase_action;
    using mg::git::rebase_result;
    using mg::git::rebase_step;
    auto dir = make_repo_for_interactive(); // feature: C1->C2(b)->C3(c)->C4(d)

    std::vector<rebase_step> plan = {
        {rebase_action::pick, oid_of(dir, "feature~2"), ""}, // C2
        {rebase_action::edit, oid_of(dir, "feature~1"), ""}, // C3: stop here
        {rebase_action::pick, oid_of(dir, "feature"), ""},   // C4
    };
    auto r = mg::git::rebase_interactive(dir.string(), "master", plan);
    REQUIRE(r.has_value());
    CHECK(*r == rebase_result::stopped);
    CHECK(fs::exists(dir / "b.txt"));       // C2 applied
    CHECK(fs::exists(dir / "c.txt"));       // stopped AT C3
    CHECK_FALSE(fs::exists(dir / "d.txt")); // C4 not yet
    CHECK(mg::git::rebase_in_progress(dir.string()));

    // Simulate the user's edit: an extra commit while stopped.
    commit_file(dir, "extra.txt", "e\n", "extra during edit");
    auto cont = mg::git::rebase_continue(dir.string());
    REQUIRE(cont.has_value());
    CHECK(*cont == rebase_result::done);
    CHECK(fs::exists(dir / "d.txt"));       // C4 replayed after the edit
    CHECK(fs::exists(dir / "extra.txt"));   // the edit survived
    CHECK_FALSE(mg::git::rebase_in_progress(dir.string()));
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive edit can be aborted back to the original branch")
{
    using mg::git::rebase_action;
    using mg::git::rebase_result;
    using mg::git::rebase_step;
    auto dir = make_repo_for_interactive();

    std::vector<rebase_step> plan = {
        {rebase_action::pick, oid_of(dir, "feature~2"), ""},
        {rebase_action::edit, oid_of(dir, "feature~1"), ""},
        {rebase_action::pick, oid_of(dir, "feature"), ""},
    };
    auto r = mg::git::rebase_interactive(dir.string(), "master", plan);
    REQUIRE(r.has_value());
    CHECK(*r == rebase_result::stopped);

    REQUIRE(mg::git::rebase_abort(dir.string()).has_value());
    CHECK_FALSE(mg::git::rebase_in_progress(dir.string()));
    // Restored to the original feature tip: all of C2/C3/C4 present.
    CHECK(fs::exists(dir / "b.txt"));
    CHECK(fs::exists(dir / "c.txt"));
    CHECK(fs::exists(dir / "d.txt"));
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C4");
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive pauses on a replay conflict; resolve+continue")
{
    using mg::git::conflict_side;
    using mg::git::rebase_action;
    using mg::git::rebase_result;
    using mg::git::rebase_step;
    // HEAD=feature (a.txt "feature change"); master has a.txt "master change".
    auto dir = make_repo_rebase_conflict();
    commit_file(dir, "y.txt", "y\n", "Cf2 clean"); // a clean tail commit
    std::string cf1 = oid_of(dir, "feature~1");    // "C3 feature" (conflicts)
    std::string cf2 = oid_of(dir, "feature");      // "Cf2 clean"

    std::vector<rebase_step> plan = {
        {rebase_action::pick, cf1, ""}, // conflicts when replayed onto master
        {rebase_action::pick, cf2, ""}, // clean tail
    };
    auto r = mg::git::rebase_interactive(dir.string(), "master", plan);
    REQUIRE(r.has_value());
    CHECK(*r == rebase_result::conflicts); // paused, not aborted
    CHECK(mg::git::rebase_in_progress(dir.string()));
    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);
    CHECK((*cs)[0].path == "a.txt");
    CHECK_FALSE(fs::exists(dir / "y.txt")); // tail not replayed yet

    // Resolve to theirs (the replayed feature commit) and commit it.
    REQUIRE(mg::git::resolve_conflict(dir.string(), "a.txt",
                                      conflict_side::theirs)
                .has_value());
    REQUIRE(mg::git::commit(dir.string(), "Cf1 resolved").has_value());

    auto cont = mg::git::rebase_continue(dir.string());
    REQUIRE(cont.has_value());
    CHECK(*cont == rebase_result::done);
    CHECK(fs::exists(dir / "y.txt"));           // tail replayed
    CHECK_FALSE(mg::git::rebase_in_progress(dir.string()));
    fs::remove_all(dir);
}

TEST_CASE("rebase_continue refuses while replay conflicts are unresolved")
{
    using mg::git::rebase_action;
    using mg::git::rebase_result;
    using mg::git::rebase_step;
    auto dir = make_repo_rebase_conflict();
    std::string cf1 = oid_of(dir, "feature");

    std::vector<rebase_step> plan = {{rebase_action::pick, cf1, ""}};
    auto r = mg::git::rebase_interactive(dir.string(), "master", plan);
    REQUIRE(r.has_value());
    CHECK(*r == rebase_result::conflicts);

    // Continue without resolving -> error (the guard), state preserved.
    CHECK_FALSE(mg::git::rebase_continue(dir.string()).has_value());
    CHECK(mg::git::rebase_in_progress(dir.string()));
    fs::remove_all(dir);
}

TEST_CASE("rebase_interactive fixup folds a commit into the previous one")
{
    using mg::git::rebase_action;
    using mg::git::rebase_step;
    auto dir = make_repo_for_interactive();

    // pick C2, fixup C3 into it, pick C4.
    std::vector<rebase_step> plan = {
        {rebase_action::pick, oid_of(dir, "feature~2")},  // C2
        {rebase_action::fixup, oid_of(dir, "feature~1")}, // C3 -> folded into C2
        {rebase_action::pick, oid_of(dir, "feature")},    // C4
    };
    REQUIRE(mg::git::rebase_interactive(dir.string(), "master", plan).has_value());

    // Both b.txt (C2) and c.txt (C3's change) are present, but C3 is gone as a
    // separate commit and its message did not survive (fixup keeps C2's).
    CHECK(fs::exists(dir / "b.txt"));
    CHECK(fs::exists(dir / "c.txt"));
    CHECK(fs::exists(dir / "d.txt"));
    auto after = mg::git::recent_commits(dir.string(), 10);
    REQUIRE(after.has_value());
    CHECK(after->size() == 3); // C1, (C2+C3), C4'
    for (const auto &c : *after)
        CHECK(c.summary != "C3");
    fs::remove_all(dir);
}

TEST_CASE("rebase_onto pauses on conflict; rebase_abort restores the branch")
{
    auto dir = make_repo_rebase_conflict(); // feature & master change a.txt

    auto r = mg::git::rebase_onto(dir.string(), "master");
    REQUIRE(r.has_value());
    CHECK(*r == mg::git::rebase_result::conflicts);
    CHECK(mg::git::rebase_in_progress(dir.string()));

    REQUIRE(mg::git::rebase_abort(dir.string()).has_value());
    CHECK_FALSE(mg::git::rebase_in_progress(dir.string()));
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C3 feature"); // original feature tip restored
    fs::remove_all(dir);
}

TEST_CASE("rebase_continue finishes the rebase once the conflict is resolved")
{
    auto dir = make_repo_rebase_conflict();

    auto r = mg::git::rebase_onto(dir.string(), "master");
    REQUIRE(r.has_value());
    REQUIRE(*r == mg::git::rebase_result::conflicts);

    resolve_and_stage(dir, "resolved\n");
    auto c = mg::git::rebase_continue(dir.string());
    REQUIRE(c.has_value());
    CHECK(*c == mg::git::rebase_result::done);
    CHECK_FALSE(mg::git::rebase_in_progress(dir.string()));

    // The resolved content is committed, and C2 master is now an ancestor.
    std::ifstream f(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body == "resolved\n");
    auto cs = mg::git::recent_commits(dir.string(), 10);
    REQUIRE(cs.has_value());
    bool c2 = false;
    for (const auto &cm : *cs)
        c2 = c2 || cm.summary == "C2 master";
    CHECK(c2);
    fs::remove_all(dir);
}

TEST_CASE("merge_branch makes a merge commit for a clean divergent merge")
{
    auto dir = make_repo_diverged(); // master and feature changed different files

    REQUIRE(mg::git::merge_branch(dir.string(), "feature").has_value());

    // A merge commit (two parents) now sits on HEAD, with both changes present.
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary.find("Merge branch 'feature'") != std::string::npos);
    CHECK(fs::exists(dir / "b.txt"));            // feature's file
    std::ifstream f(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body.find("master line") != std::string::npos); // master's change kept
    fs::remove_all(dir);
}

TEST_CASE("reset_to hard moves HEAD and resets the working tree")
{
    auto dir = make_repo_with_commit("C1"); // a.txt = "content"
    commit_file(dir, "a.txt", "content\nmore\n", "C2");

    auto cs = mg::git::recent_commits(dir.string(), 2); // [C2, C1]
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 2);

    REQUIRE(mg::git::reset_to(dir.string(), (*cs)[1].oid,
                              mg::git::reset_mode::hard)
                .has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C1");                 // HEAD moved back
    std::ifstream f(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body == "content");                  // working tree reset (no "more")
    fs::remove_all(dir);
}

TEST_CASE("revert_commit undoes a commit in a new commit")
{
    auto dir = make_repo_with_commit("C1"); // a.txt = "content"
    commit_file(dir, "a.txt", "content\nmore\n", "C2 add more");

    auto cs = mg::git::recent_commits(dir.string(), 1); // newest = C2
    REQUIRE(cs.has_value());

    REQUIRE(mg::git::revert_commit(dir.string(), (*cs)[0].oid).has_value());

    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary.find("Revert") != std::string::npos); // a new revert commit
    std::ifstream f(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body == "content");                  // C2's change undone
    fs::remove_all(dir);
}

TEST_CASE("merge_branch fast-forwards onto an ahead branch")
{
    auto dir = make_repo_ff_branch(); // master @ C1, feature 1 ahead (adds b.txt)

    REQUIRE(mg::git::merge_branch(dir.string(), "feature").has_value());

    // Fast-forwarded: HEAD now at feature's commit, b.txt in the working tree.
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->summary == "C2 on feature");
    CHECK(fs::exists(dir / "b.txt"));
    fs::remove_all(dir);
}

TEST_CASE("merge_branch leaves conflicts on disk, then commit completes it")
{
    auto dir = make_repo_rebase_conflict(); // HEAD feature, a.txt diverged

    auto m = mg::git::merge_branch(dir.string(), "master");
    REQUIRE(m.has_value());
    CHECK(*m == mg::git::apply_result::conflicts); // left on disk, not aborted

    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);
    CHECK((*cs)[0].path == "a.txt");

    // Resolve to theirs (master) and commit -> a real two-parent merge commit.
    REQUIRE(mg::git::resolve_conflict(dir.string(), "a.txt",
                                      mg::git::conflict_side::theirs)
                .has_value());
    REQUIRE(mg::git::commit(dir.string(), "Merge master").has_value());

    // MERGE_HEAD cleared and the commit has two parents.
    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    CHECK(git_repository_state(repo) == GIT_REPOSITORY_STATE_NONE);
    git_oid head;
    REQUIRE(git_reference_name_to_id(&head, repo, "HEAD") == 0);
    git_commit *c = nullptr;
    REQUIRE(git_commit_lookup(&c, repo, &head) == 0);
    CHECK(git_commit_parentcount(c) == 2);
    git_commit_free(c);
    git_repository_free(repo);
    git_libgit2_shutdown();
    fs::remove_all(dir);
}

TEST_CASE("cherry_pick leaves conflicts on disk instead of erroring")
{
    auto dir = make_repo_rebase_conflict(); // HEAD feature (a.txt=feature change)
    std::string c2 = oid_of(dir, "master"); // C2: a.txt = master change

    auto cp = mg::git::cherry_pick(dir.string(), c2);
    REQUIRE(cp.has_value());
    CHECK(*cp == mg::git::apply_result::conflicts);
    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);
    CHECK((*cs)[0].path == "a.txt");
    fs::remove_all(dir);
}

TEST_CASE("revert_commit leaves conflicts when the inverse does not apply")
{
    auto dir = make_repo_with_commit("C1");       // a.txt = "content"
    commit_file(dir, "a.txt", "content\nmore\n", "C2 add more");
    commit_file(dir, "a.txt", "totally different\n", "C3 rewrite");
    std::string c2 = oid_of(dir, "HEAD~1");        // the commit to revert

    auto rv = mg::git::revert_commit(dir.string(), c2);
    REQUIRE(rv.has_value());
    CHECK(*rv == mg::git::apply_result::conflicts); // can't cleanly undo
    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    CHECK(cs->size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("BENCH repo_status (set MG_BENCH_REPO)")
{
	const char *repo = std::getenv("MG_BENCH_REPO");
	if (repo == nullptr)
		return;
	for (int i = 0; i < 5; ++i) {
		auto t0 = std::chrono::steady_clock::now();
		auto st = mg::git::repo_status(repo);
		auto t1 = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		MESSAGE("repo_status: " << (st ? (int)st->size() : -1)
		    << " entries in " << ms << " ms");
	}
}

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

TEST_CASE("stage_region stages only the selected lines of a single hunk")
{
    auto dir = make_repo_with_one_hunk_two_changes();

    // One hunk: ' a / -b / +B / c / -d / +D / e' (line indices 0..6).
    auto d = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/false);
    REQUIRE(d.has_value());
    REQUIRE(d->size() == 1);

    // Select just the b->B change: hunk line indices 1 (-b) and 2 (+B).
    REQUIRE(mg::git::stage_region(dir.string(), "f.txt", /*hunk_index=*/0,
                                  /*sel_first=*/1, /*sel_last=*/2)
                .has_value());

    // Staged side now has b->B but NOT d->D.
    auto staged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/true);
    REQUIRE(staged.has_value());
    bool staged_B = false, staged_D = false;
    for (const auto &h : *staged)
        for (const auto &l : h.lines) {
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                staged_B = true;
            if (l.origin == '+' && l.content.find('D') != std::string::npos)
                staged_D = true;
        }
    CHECK(staged_B);
    CHECK_FALSE(staged_D);

    // Unstaged side keeps only d->D.
    auto unstaged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/false);
    REQUIRE(unstaged.has_value());
    bool unstaged_B = false, unstaged_D = false;
    for (const auto &h : *unstaged)
        for (const auto &l : h.lines) {
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                unstaged_B = true;
            if (l.origin == '+' && l.content.find('D') != std::string::npos)
                unstaged_D = true;
        }
    CHECK_FALSE(unstaged_B);
    CHECK(unstaged_D);
    fs::remove_all(dir);
}

TEST_CASE("recent_commits carries the full commit oid")
{
    auto dir = make_repo_with_commit("only commit");
    auto cs = mg::git::recent_commits(dir.string(), 1);
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);
    CHECK((*cs)[0].oid.size() == 40);                  // full sha-1 hex
    CHECK((*cs)[0].oid.rfind((*cs)[0].short_oid, 0) == 0); // short is a prefix
    fs::remove_all(dir);
}

TEST_CASE("commit_diff returns the diff of a commit against its parent")
{
    auto dir = make_repo_with_commit("first"); // a.txt = "content"
    commit_file(dir, "a.txt", "content\nsecond line\n", "second");

    auto cs = mg::git::recent_commits(dir.string(), 1); // newest = "second"
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);

    auto d = mg::git::commit_diff(dir.string(), (*cs)[0].oid);
    REQUIRE(d.has_value());
    bool added = false;
    for (const auto &h : *d)
        for (const auto &l : h.lines)
            if (l.origin == '+' && l.content.find("second line") != std::string::npos)
                added = true;
    CHECK(added);
    fs::remove_all(dir);
}

TEST_CASE("commit_diff of the root commit shows all its lines as additions")
{
    auto dir = make_repo_with_commit("root"); // a.txt = "content", no parent
    auto cs = mg::git::recent_commits(dir.string(), 1);
    REQUIRE(cs.has_value());

    auto d = mg::git::commit_diff(dir.string(), (*cs)[0].oid);
    REQUIRE(d.has_value());
    REQUIRE_FALSE(d->empty()); // diff against the empty tree
    fs::remove_all(dir);
}

TEST_CASE("commit_diff resolves a stash rev to show the stashed change")
{
    auto dir = make_repo_with_stash("WIP"); // a.txt "content" -> "content changed"

    auto d = mg::git::commit_diff(dir.string(), "stash@{0}");
    REQUIRE(d.has_value());
    bool shows_change = false;
    for (const auto &h : *d)
        for (const auto &l : h.lines)
            if (l.content.find("content changed") != std::string::npos)
                shows_change = true;
    CHECK(shows_change);
    fs::remove_all(dir);
}

TEST_CASE("stash_push stashes the working tree, stash_pop restores it")
{
    auto dir = make_repo_with_commit("base"); // a.txt = "content"
    set_test_config(dir);                      // for the stasher signature
    std::ofstream(dir / "a.txt") << "dirty change"; // unstaged modification

    REQUIRE(mg::git::stash_push(dir.string(), "WIP").has_value());

    // Working tree is clean and the stash list has one entry.
    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    CHECK(st->empty());
    auto sl = mg::git::stashes(dir.string());
    REQUIRE(sl.has_value());
    REQUIRE(sl->size() == 1);

    // Pop brings the change back and empties the stash list.
    REQUIRE(mg::git::stash_pop(dir.string(), 0).has_value());
    auto st2 = mg::git::repo_status(dir.string());
    REQUIRE(st2.has_value());
    CHECK_FALSE(st2->empty());
    auto sl2 = mg::git::stashes(dir.string());
    REQUIRE(sl2.has_value());
    CHECK(sl2->empty());
    fs::remove_all(dir);
}

TEST_CASE("stash_push with nothing to stash is an error")
{
    auto dir = make_repo_with_commit("base"); // clean tree
    set_test_config(dir);
    CHECK_FALSE(mg::git::stash_push(dir.string(), "nope").has_value());
    fs::remove_all(dir);
}

TEST_CASE("create_tag / tags / delete_tag round-trip")
{
    auto dir = make_repo_with_commit("base");

    auto empty = mg::git::tags(dir.string());
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    REQUIRE(mg::git::create_tag(dir.string(), "v1.0", "HEAD").has_value());
    auto ts = mg::git::tags(dir.string());
    REQUIRE(ts.has_value());
    REQUIRE(ts->size() == 1);
    CHECK((*ts)[0] == "v1.0");

    REQUIRE(mg::git::delete_tag(dir.string(), "v1.0").has_value());
    auto ts2 = mg::git::tags(dir.string());
    REQUIRE(ts2.has_value());
    CHECK(ts2->empty());

    // Deleting a missing tag is an error.
    CHECK_FALSE(mg::git::delete_tag(dir.string(), "nope").has_value());
    fs::remove_all(dir);
}

TEST_CASE("add_worktree / worktrees / remove_worktree round-trip")
{
    auto dir = make_repo_with_commit("base");
    auto wtpath = make_temp_dir();
    fs::remove(wtpath); // git_worktree_add wants the path not to exist yet

    auto none = mg::git::worktrees(dir.string());
    REQUIRE(none.has_value());
    CHECK(none->empty());

    REQUIRE(mg::git::add_worktree(dir.string(), "wt1", wtpath.string())
                .has_value());
    CHECK(fs::exists(wtpath));
    auto wts = mg::git::worktrees(dir.string());
    REQUIRE(wts.has_value());
    REQUIRE(wts->size() == 1);
    CHECK((*wts)[0].name == "wt1");

    REQUIRE(mg::git::remove_worktree(dir.string(), "wt1").has_value());
    CHECK_FALSE(fs::exists(wtpath)); // working-tree dir removed
    auto after = mg::git::worktrees(dir.string());
    REQUIRE(after.has_value());
    CHECK(after->empty());
    fs::remove_all(dir);
}

TEST_CASE("set_note / read_note / remove_note round-trip")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir); // note author/committer signature

    auto none = mg::git::read_note(dir.string(), "HEAD");
    REQUIRE(none.has_value());
    CHECK(none->empty());

    REQUIRE(mg::git::set_note(dir.string(), "HEAD", "see issue #7").has_value());
    auto got = mg::git::read_note(dir.string(), "HEAD");
    REQUIRE(got.has_value());
    CHECK(*got == "see issue #7");

    // set overwrites.
    REQUIRE(mg::git::set_note(dir.string(), "HEAD", "updated").has_value());
    CHECK(*mg::git::read_note(dir.string(), "HEAD") == "updated");

    REQUIRE(mg::git::remove_note(dir.string(), "HEAD").has_value());
    CHECK(mg::git::read_note(dir.string(), "HEAD")->empty());
    fs::remove_all(dir);
}

TEST_CASE("conflicts lists unmerged paths and resolve_conflict picks a side")
{
    auto dir = make_repo_rebase_conflict(); // HEAD feature, a.txt diverged
    // Rebasing feature onto master replays "feature change" onto "master
    // change" -> pauses on the a.txt conflict, state left on disk.
    auto r = mg::git::rebase_onto(dir.string(), "master");
    REQUIRE(r.has_value());
    REQUIRE(*r == mg::git::rebase_result::conflicts);

    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() == 1);
    CHECK((*cs)[0].path == "a.txt");

    // The path shows as unmerged in the status (previously invisible).
    auto st = mg::git::repo_status(dir.string());
    REQUIRE(st.has_value());
    bool found = false;
    for (const auto &e : *st)
        if (e.path == "a.txt") {
            found = true;
            CHECK((e.index == mg::magit::status::unmerged ||
                   e.worktree == mg::magit::status::unmerged));
        }
    CHECK(found);

    // Stage 2 (ours) == master's "master change"; stage 3 (theirs) == the
    // replayed feature commit's "feature change". Take theirs.
    REQUIRE(mg::git::resolve_conflict(dir.string(), "a.txt",
                                      mg::git::conflict_side::theirs)
                .has_value());
    auto cs2 = mg::git::conflicts(dir.string());
    REQUIRE(cs2.has_value());
    CHECK(cs2->empty()); // resolved

    std::ifstream in(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "feature change\n");
    fs::remove_all(dir);
}

TEST_CASE("refine_words wraps only the words unique to a side")
{
    using mg::git::refine_words;
    // "quick" differs; the shared words stay plain.
    CHECK(refine_words("the quick brown fox", "the slow brown fox", "[-", "-]") ==
          "the [-quick-] brown fox");
    CHECK(refine_words("the slow brown fox", "the quick brown fox", "{+", "+}") ==
          "the {+slow+} brown fox");
    // Whitespace + newlines preserved; multiple changed words each wrapped.
    CHECK(refine_words("a X\nc Y", "a b\nc d", "<", ">") == "a <X>\nc <Y>");
    // Identical -> nothing wrapped.
    CHECK(refine_words("same line", "same line", "[-", "-]") == "same line");
    // Pure insertion on this side.
    CHECK(refine_words("one two three", "one three", "[-", "-]") ==
          "one [-two-] three");
}

TEST_CASE("conflict_hunks parses regions and resolve_conflict_hunk rewrites them")
{
    auto dir = make_repo_with_commit("C1");
    // Hand-write a file with two conflict regions (2-way markers).
    std::ofstream(dir / "f.txt")
        << "top\n"
        << "<<<<<<< HEAD\n" << "ours one\n" << "=======\n" << "theirs one\n"
        << ">>>>>>> other\n"
        << "middle\n"
        << "<<<<<<< HEAD\n" << "ours two\n" << "=======\n" << "theirs two\n"
        << ">>>>>>> other\n"
        << "bottom\n";

    auto hs = mg::git::conflict_hunks(dir.string(), "f.txt");
    REQUIRE(hs.has_value());
    REQUIRE(hs->size() == 2);
    CHECK((*hs)[0].ours == "ours one\n");
    CHECK((*hs)[0].theirs == "theirs one\n");
    CHECK((*hs)[1].theirs == "theirs two\n");

    // Resolve region 0 -> ours; one region remains.
    REQUIRE(mg::git::resolve_conflict_hunk(dir.string(), "f.txt", 0,
                                           mg::git::conflict_side::ours)
                .has_value());
    auto hs2 = mg::git::conflict_hunks(dir.string(), "f.txt");
    REQUIRE(hs2.has_value());
    REQUIRE(hs2->size() == 1);
    CHECK((*hs2)[0].ours == "ours two\n");

    // Resolve the last -> theirs; no markers remain.
    REQUIRE(mg::git::resolve_conflict_hunk(dir.string(), "f.txt", 0,
                                           mg::git::conflict_side::theirs)
                .has_value());
    auto hs3 = mg::git::conflict_hunks(dir.string(), "f.txt");
    REQUIRE(hs3.has_value());
    CHECK(hs3->empty());

    std::ifstream in(dir / "f.txt");
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "top\nours one\nmiddle\ntheirs two\nbottom\n");
    fs::remove_all(dir);
}

TEST_CASE("conflict_hunks drops the diff3 base section and both keeps each side")
{
    auto dir = make_repo_with_commit("C1");
    std::ofstream(dir / "f.txt")
        << "<<<<<<< HEAD\n" << "ours\n"
        << "||||||| base\n" << "base text\n"
        << "=======\n" << "theirs\n"
        << ">>>>>>> other\n";

    auto hs = mg::git::conflict_hunks(dir.string(), "f.txt");
    REQUIRE(hs.has_value());
    REQUIRE(hs->size() == 1);
    CHECK((*hs)[0].ours == "ours\n");       // base dropped
    CHECK((*hs)[0].theirs == "theirs\n");

    REQUIRE(mg::git::resolve_conflict_hunk(dir.string(), "f.txt", 0,
                                           mg::git::conflict_side::both)
                .has_value());
    std::ifstream in(dir / "f.txt");
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "ours\ntheirs\n"); // both sides kept, markers gone
    fs::remove_all(dir);
}

TEST_CASE("resolve_conflict with ours keeps the current side")
{
    auto dir = make_repo_rebase_conflict();
    REQUIRE(mg::git::rebase_onto(dir.string(), "master").has_value());
    REQUIRE(mg::git::resolve_conflict(dir.string(), "a.txt",
                                      mg::git::conflict_side::ours)
                .has_value());
    std::ifstream in(dir / "a.txt");
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == "master change\n"); // ours = the onto (master)
    auto cs = mg::git::conflicts(dir.string());
    REQUIRE(cs.has_value());
    CHECK(cs->empty());
    fs::remove_all(dir);
}

TEST_CASE("log_file returns only the commits that touched a given file")
{
    auto dir = make_repo_with_commit("base"); // commits a.txt
    commit_file(dir, "foo.txt", "one\n", "add foo");        // touches foo
    commit_file(dir, "bar.txt", "x\n", "add bar");          // not foo
    commit_file(dir, "foo.txt", "one\ntwo\n", "edit foo");  // touches foo
    commit_file(dir, "bar.txt", "x\ny\n", "edit bar");      // not foo

    auto log = mg::git::log_file(dir.string(), "foo.txt", 50);
    REQUIRE(log.has_value());
    REQUIRE(log->size() == 2); // only "add foo" and "edit foo"
    CHECK((*log)[0].summary == "edit foo"); // newest first
    CHECK((*log)[1].summary == "add foo");

    auto missing = mg::git::log_file(dir.string(), "nope.txt", 50);
    REQUIRE(missing.has_value());
    CHECK(missing->empty());
    fs::remove_all(dir);
}

TEST_CASE("submodules lists each registered submodule's name and path")
{
    auto parent = make_repo_with_commit("base");
    auto sub = make_repo_with_commit("sub-base"); // a repo to embed

    git_libgit2_init();
    git_repository *r = nullptr;
    REQUIRE(git_repository_open(&r, parent.string().c_str()) == 0);
    git_submodule *sm = nullptr;
    REQUIRE(git_submodule_add_setup(&sm, r, ("file://" + sub.string()).c_str(),
                                    "libs/sub", 1) == 0);
    git_submodule_free(sm);
    git_repository_free(r);
    git_libgit2_shutdown();

    auto subs = mg::git::submodules(parent.string());
    REQUIRE(subs.has_value());
    REQUIRE(subs->size() == 1);
    CHECK((*subs)[0].name == "libs/sub");
    CHECK((*subs)[0].path == "libs/sub");

    auto none = mg::git::submodules(sub.string()); // a repo with no submodules
    REQUIRE(none.has_value());
    CHECK(none->empty());

    fs::remove_all(parent);
    fs::remove_all(sub);
}

TEST_CASE("bisect narrows a linear history to the first bad commit")
{
    // C1(good) -> C2 -> C3(bad introduced here) -> C4(bad), HEAD on the branch.
    auto dir = make_repo_for_interactive(); // C1..C4 on "feature"
    std::string c1 = oid_of(dir, "feature~3");
    std::string c3 = oid_of(dir, "feature~1");

    auto s = mg::git::bisect_start(dir.string(), "feature", c1); // bad=C4 good=C1
    REQUIRE(s.has_value());
    CHECK(mg::git::bisect_active(dir.string()));

    // Drive the search: the regression was introduced at C3, so a commit is
    // "bad" iff its summary is C3 or C4. Each mark checks out the next midpoint.
    std::string msg;
    for (int guard = 0; guard < 10; ++guard) {
        auto cur = mg::git::read_head(dir.string()); // the detached commit
        REQUIRE(cur.has_value());
        bool bad = (cur->summary == "C3" || cur->summary == "C4");
        auto r = mg::git::bisect_mark(dir.string(), bad);
        REQUIRE(r.has_value());
        msg = *r;
        if (msg.find("first bad commit") != std::string::npos)
            break;
    }
    CHECK(msg.find(c3) != std::string::npos); // culprit is C3
    CHECK(msg.find("first bad commit") != std::string::npos);

    REQUIRE(mg::git::bisect_reset(dir.string()).has_value());
    CHECK_FALSE(mg::git::bisect_active(dir.string()));
    auto h = mg::git::read_head(dir.string());
    REQUIRE(h.has_value());
    CHECK(h->branch == "feature"); // back on the starting branch
    fs::remove_all(dir);
}

TEST_CASE("blame_file annotates each line with its commit and author")
{
    auto dir = make_repo_with_commit("C1");
    commit_file(dir, "a.txt", "line one\n", "C2");            // line 1 settled
    commit_file(dir, "a.txt", "line one\nline two\n", "C3");  // adds line 2

    auto bl = mg::git::blame_file(dir.string(), "a.txt");
    REQUIRE(bl.has_value());
    REQUIRE(bl->size() == 2);
    CHECK((*bl)[0].text == "line one");
    CHECK((*bl)[1].text == "line two");
    // Each line names the commit + author (Test signature from commit_file).
    CHECK((*bl)[0].short_oid.size() == 8);
    CHECK((*bl)[0].author == "Test");
    // The two lines came from different commits.
    CHECK((*bl)[0].short_oid != (*bl)[1].short_oid);
    fs::remove_all(dir);
}

TEST_CASE("ignore_path appends a pattern to .gitignore")
{
    auto dir = make_repo_with_commit("base");

    REQUIRE(mg::git::ignore_path(dir.string(), "build/").has_value());
    REQUIRE(mg::git::ignore_path(dir.string(), "*.o").has_value());

    std::ifstream f(dir / ".gitignore");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body == "build/\n*.o\n");
    fs::remove_all(dir);
}

TEST_CASE("create_tag with a message makes an annotated tag")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir); // tagger signature

    // Lightweight: refs/tags/lw resolves directly to a commit.
    REQUIRE(mg::git::create_tag(dir.string(), "lw", "HEAD").has_value());
    // Annotated: refs/tags/ann resolves to a tag object.
    REQUIRE(mg::git::create_tag(dir.string(), "ann", "HEAD", "release notes")
                .has_value());

    git_libgit2_init();
    git_repository *repo = nullptr;
    REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
    git_object *lw = nullptr, *ann = nullptr;
    REQUIRE(git_revparse_single(&lw, repo, "refs/tags/lw") == 0);
    REQUIRE(git_revparse_single(&ann, repo, "refs/tags/ann") == 0);
    CHECK(git_object_type(lw) == GIT_OBJECT_COMMIT);  // lightweight -> commit
    CHECK(git_object_type(ann) == GIT_OBJECT_TAG);    // annotated -> tag object
    git_object_free(lw);
    git_object_free(ann);
    git_repository_free(repo);
    git_libgit2_shutdown();
    fs::remove_all(dir);
}

TEST_CASE("create_branch makes a new branch at HEAD without checking it out")
{
    auto dir = make_repo_with_commit("base"); // on master
    REQUIRE(mg::git::create_branch(dir.string(), "feature").has_value());

    auto bs = mg::git::branches(dir.string());
    REQUIRE(bs.has_value());
    bool has_feature = false, head_still_master = false;
    for (const auto &b : *bs) {
        if (b.name == "feature")
            has_feature = true;
        if (b.is_head && b.name == "master")
            head_still_master = true;
    }
    CHECK(has_feature);
    CHECK(head_still_master); // create does not switch branches
    fs::remove_all(dir);
}

TEST_CASE("delete_branch removes a branch")
{
    auto dir = make_repo_with_branch("doomed"); // master + branch "doomed"
    REQUIRE(mg::git::delete_branch(dir.string(), "doomed").has_value());

    auto bs = mg::git::branches(dir.string());
    REQUIRE(bs.has_value());
    for (const auto &b : *bs)
        CHECK(b.name != "doomed");
    fs::remove_all(dir);
}

TEST_CASE("rename_branch changes a branch's name")
{
    auto dir = make_repo_with_branch("old"); // master + branch "old"
    REQUIRE(mg::git::rename_branch(dir.string(), "old", "renamed").has_value());

    auto bs = mg::git::branches(dir.string());
    REQUIRE(bs.has_value());
    bool has_renamed = false, has_old = false;
    for (const auto &b : *bs) {
        if (b.name == "renamed")
            has_renamed = true;
        if (b.name == "old")
            has_old = true;
    }
    CHECK(has_renamed);
    CHECK_FALSE(has_old);
    fs::remove_all(dir);
}

TEST_CASE("delete_branch on a missing branch is an error")
{
    auto dir = make_repo_with_commit("base");
    CHECK_FALSE(mg::git::delete_branch(dir.string(), "nope").has_value());
    fs::remove_all(dir);
}

TEST_CASE("upstream_status reports the upstream name and ahead/behind counts")
{
    auto dir = make_repo_ahead_behind(); // topic tracks master, 1 ahead/1 behind

    auto u = mg::git::upstream_status(dir.string());
    REQUIRE(u.has_value());
    CHECK(u->has_upstream);
    CHECK(u->name == "master");
    CHECK(u->ahead == 1);
    CHECK(u->behind == 1);
    fs::remove_all(dir);
}

TEST_CASE("upstream_commits lists the unpushed and unpulled commits")
{
    auto dir = make_repo_ahead_behind(); // topic: C3 unpushed, C2 unpulled

    auto unpushed = mg::git::upstream_commits(dir.string(), /*unpushed=*/true);
    REQUIRE(unpushed.has_value());
    REQUIRE(unpushed->size() == 1);
    CHECK((*unpushed)[0].summary == "C3");

    auto unpulled = mg::git::upstream_commits(dir.string(), /*unpushed=*/false);
    REQUIRE(unpulled.has_value());
    REQUIRE(unpulled->size() == 1);
    CHECK((*unpulled)[0].summary == "C2");
    fs::remove_all(dir);
}

TEST_CASE("upstream_commits is empty without an upstream")
{
    auto dir = make_repo_with_commit("base");
    auto c = mg::git::upstream_commits(dir.string(), true);
    REQUIRE(c.has_value());
    CHECK(c->empty());
    fs::remove_all(dir);
}

TEST_CASE("upstream_status reports no upstream when none is configured")
{
    auto dir = make_repo_with_commit("base"); // master, no upstream set
    auto u = mg::git::upstream_status(dir.string());
    REQUIRE(u.has_value());
    CHECK_FALSE(u->has_upstream);
    CHECK(u->ahead == 0);
    CHECK(u->behind == 0);
    fs::remove_all(dir);
}

TEST_CASE("discard_region reverts only the selected lines in the working tree")
{
    auto dir = make_repo_with_one_hunk_two_changes(); // b->B and d->D, unstaged

    // Discard just the b->B change (hunk 0, line indices 1..2).
    REQUIRE(mg::git::discard_region(dir.string(), "f.txt", /*hunk_index=*/0,
                                    /*sel_first=*/1, /*sel_last=*/2)
                .has_value());

    // The working tree keeps d->D but the B reverted back to b.
    std::ifstream f(dir / "f.txt");
    std::string body((std::istreambuf_iterator<char>(f)), {});
    CHECK(body == "a\nb\nc\nD\ne\n");
    fs::remove_all(dir);
}

TEST_CASE("unstage_region unstages only the selected lines of a staged hunk")
{
    auto dir = make_repo_with_one_hunk_two_changes();
    // Stage the whole file: one staged hunk with both b->B and d->D.
    REQUIRE(mg::git::stage(dir.string(), "f.txt").has_value());
    auto staged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/true);
    REQUIRE(staged.has_value());
    REQUIRE(staged->size() == 1);

    // Unstage just the b->B change: staged-hunk line indices 1 (-b) and 2 (+B).
    REQUIRE(mg::git::unstage_region(dir.string(), "f.txt", /*hunk_index=*/0,
                                    /*sel_first=*/1, /*sel_last=*/2)
                .has_value());

    // Staged side keeps only d->D now.
    auto staged_after = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/true);
    REQUIRE(staged_after.has_value());
    bool staged_B = false, staged_D = false;
    for (const auto &h : *staged_after)
        for (const auto &l : h.lines) {
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                staged_B = true;
            if (l.origin == '+' && l.content.find('D') != std::string::npos)
                staged_D = true;
        }
    CHECK_FALSE(staged_B);
    CHECK(staged_D);

    // The b->B change is back on the unstaged side.
    auto unstaged = mg::git::file_diff(dir.string(), "f.txt", /*staged=*/false);
    REQUIRE(unstaged.has_value());
    bool unstaged_B = false;
    for (const auto &h : *unstaged)
        for (const auto &l : h.lines)
            if (l.origin == '+' && l.content.find('B') != std::string::npos)
                unstaged_B = true;
    CHECK(unstaged_B);
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
