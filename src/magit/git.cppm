// mg.git -- libgit2-backed working-tree status reader (task M2c-2).
//
// Reads structured status straight from the repository (no subprocess, no
// porcelain text). The libgit2 C handles are owned by RAII wrappers so they are
// freed on every path; errors surface as std::expected, never raw int codes.

module;
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <git2.h>

export module mg.git;

import mg.magit;

// ---- internal RAII + mapping helpers (not exported) -----------------------
namespace mg::git::detail {

// Refcounted libgit2 lifetime guard (the +threadsafe build is installed).
struct init_guard {
    init_guard() { git_libgit2_init(); }
    ~init_guard() { git_libgit2_shutdown(); }
    init_guard(const init_guard &) = delete;
    init_guard &operator=(const init_guard &) = delete;
};

using repo_ptr = std::unique_ptr<
    git_repository, decltype([](git_repository *r) { git_repository_free(r); })>;
using status_list_ptr = std::unique_ptr<
    git_status_list, decltype([](git_status_list *s) { git_status_list_free(s); })>;
using ref_ptr = std::unique_ptr<
    git_reference, decltype([](git_reference *r) { git_reference_free(r); })>;
using commit_ptr = std::unique_ptr<
    git_commit, decltype([](git_commit *c) { git_commit_free(c); })>;
using revwalk_ptr = std::unique_ptr<
    git_revwalk, decltype([](git_revwalk *w) { git_revwalk_free(w); })>;
using index_ptr = std::unique_ptr<
    git_index, decltype([](git_index *i) { git_index_free(i); })>;
using object_ptr = std::unique_ptr<
    git_object, decltype([](git_object *o) { git_object_free(o); })>;
using tree_ptr = std::unique_ptr<
    git_tree, decltype([](git_tree *t) { git_tree_free(t); })>;
using sig_ptr = std::unique_ptr<
    git_signature, decltype([](git_signature *s) { git_signature_free(s); })>;
using diff_ptr = std::unique_ptr<
    git_diff, decltype([](git_diff *d) { git_diff_free(d); })>;
using patch_ptr = std::unique_ptr<
    git_patch, decltype([](git_patch *p) { git_patch_free(p); })>;
using branch_iter_ptr = std::unique_ptr<
    git_branch_iterator,
    decltype([](git_branch_iterator *i) { git_branch_iterator_free(i); })>;

// 8-char abbreviated oid, like git's default short form.
inline std::string short_oid(const git_oid *oid)
{
    char buf[9];
    git_oid_tostr(buf, sizeof buf, oid);
    return std::string(buf);
}

inline std::string full_oid(const git_oid *oid)
{
    char buf[GIT_OID_HEXSZ + 1];
    git_oid_tostr(buf, sizeof buf, oid);
    return std::string(buf);
}

// git_apply hunk_cb that applies exactly one hunk (by 0-based position) and
// skips the rest. Hunks are visited in order, so a running counter is enough.
struct hunk_filter {
    std::size_t target;
    std::size_t seen = 0;
};
int apply_one_hunk(const git_diff_hunk *, void *payload)
{
    auto *f = static_cast<hunk_filter *>(payload);
    return f->seen++ == f->target ? 0 : 1; // 0 = apply this hunk, >0 = skip it
}

mg::magit::file_status map_entry(const git_status_entry *e)
{
    using mg::magit::status;
    mg::magit::file_status fs{status::unmodified, status::unmodified, "", {}};
    const unsigned s = e->status;

    if (s & GIT_STATUS_INDEX_NEW)
        fs.index = status::added;
    else if (s & GIT_STATUS_INDEX_MODIFIED)
        fs.index = status::modified;
    else if (s & GIT_STATUS_INDEX_DELETED)
        fs.index = status::deleted;
    else if (s & GIT_STATUS_INDEX_RENAMED)
        fs.index = status::renamed;
    else if (s & GIT_STATUS_INDEX_TYPECHANGE)
        fs.index = status::modified;

    if (s & GIT_STATUS_WT_NEW)
        fs.worktree = status::untracked;
    else if (s & GIT_STATUS_WT_MODIFIED)
        fs.worktree = status::modified;
    else if (s & GIT_STATUS_WT_DELETED)
        fs.worktree = status::deleted;
    else if (s & GIT_STATUS_WT_RENAMED)
        fs.worktree = status::renamed;
    else if (s & GIT_STATUS_WT_TYPECHANGE)
        fs.worktree = status::modified;

    const char *p = nullptr;
    if (e->index_to_workdir && e->index_to_workdir->new_file.path)
        p = e->index_to_workdir->new_file.path;
    else if (e->head_to_index && e->head_to_index->new_file.path)
        p = e->head_to_index->new_file.path;
    if (p)
        fs.path = p;
    return fs;
}

} // namespace mg::git::detail

export namespace mg::git {

struct error {
    int klass;            // libgit2 error class
    std::string message;
};

struct head_info {
    std::string branch;     // shorthand, e.g. "master"
    std::string short_oid;  // empty on an unborn branch
    std::string summary;    // HEAD commit's first line; empty if unborn
};

struct commit_brief {
    std::string short_oid;
    std::string oid;       // full 40-char sha-1 hex (for lookups)
    std::string summary;
};

struct stash_entry {
    std::size_t index;       // 0 = most recent (stash@{0})
    std::string message;
    std::string short_oid;
};

struct branch_entry {
    std::string name;        // shorthand, e.g. "master"
    bool is_head;            // true for the currently checked-out branch
};

struct upstream_info {
    bool has_upstream;       // false when the current branch tracks nothing
    std::string name;        // upstream shorthand, e.g. "origin/master"
    std::size_t ahead;       // commits on HEAD but not upstream (to push)
    std::size_t behind;      // commits on upstream but not HEAD (to pull)
};

// One line of a diff: origin is git's marker ('+', '-', ' ', etc.); content
// includes the trailing newline.
struct diff_line {
    char origin;
    std::string content;
};

struct hunk {
    std::string header;            // e.g. "@@ -1,3 +1,4 @@"
    std::vector<diff_line> lines;
};

std::expected<std::vector<mg::magit::file_status>, error>
repo_status(std::string path);

std::expected<head_info, error> read_head(std::string path);

// The current branch's upstream tracking status (name + ahead/behind counts).
std::expected<upstream_info, error> upstream_status(std::string path);

// Commits that diverge from the upstream: unpushed (on HEAD, not upstream) when
// `unpushed` is true, else unpulled (on upstream, not HEAD). Empty if no
// upstream. Newest first.
std::expected<std::vector<commit_brief>, error>
upstream_commits(std::string path, bool unpushed);

std::expected<std::vector<commit_brief>, error>
recent_commits(std::string path, std::size_t n);

// The repository's stash entries, most recent first.
std::expected<std::vector<stash_entry>, error> stashes(std::string path);

// The repository's local branches; one entry has is_head == true.
std::expected<std::vector<branch_entry>, error> branches(std::string path);

// Reapply stash `index` to the working tree, keeping it in the stash list.
// Stash the working-tree + index changes away with `message` (magit's z z).
std::expected<void, error> stash_push(std::string repo, std::string message);

// Apply stash `index` and drop it on success (magit's z p).
std::expected<void, error> stash_pop(std::string repo, std::size_t index);

std::expected<void, error> stash_apply(std::string repo, std::size_t index);

// Delete stash `index` from the stash list.
std::expected<void, error> stash_drop(std::string repo, std::size_t index);

// Check out local branch `name`, moving HEAD and updating the working tree.
std::expected<void, error> checkout_branch(std::string repo, std::string name);

enum class reset_mode { soft, mixed, hard };

// Reset HEAD (and per `mode` the index/working tree) to commit `rev`.
std::expected<void, error>
reset_to(std::string repo, std::string rev, reset_mode mode);

// Revert commit `rev`, recording the inverse as a new commit on HEAD.
std::expected<void, error> revert_commit(std::string repo, std::string rev);

// Merge local branch `name` into HEAD: fast-forward when possible, else a
// merge commit. Errors (leaving the tree clean) on conflicts.
std::expected<void, error> merge_branch(std::string repo, std::string name);

// Rebase the current branch onto `upstream` (a branch name / revspec): replay
// HEAD's commits since the merge-base on top of upstream. On conflict, abort
// (restoring the original state) and return an error. Non-interactive.
std::expected<void, error> rebase_onto(std::string repo, std::string upstream);

// UI prompt for credentials: fill `out` (size outlen) with the user's answer
// to `prompt`; `hidden` requests non-echoing input (passwords). Return 1 on
// success, 0 on abort. `udata` is opaque (passed through from set_cred_prompt).
using cred_prompt = int (*)(const char *prompt, int hidden, char *out,
                            int outlen, void *udata);

// Register the prompt used to answer HTTPS user/password auth during
// fetch/push (nullptr disables interactive auth -> ssh-agent only).
void set_cred_prompt(cred_prompt fn, void *udata);

struct userpass {
    std::string user;
    std::string pass;
};

// Resolve an HTTPS user/password via `prompt`: the username defaults to
// `username_from_url` when present (no prompt), else is prompted; the password
// is always prompted (hidden). Unexpected if there is no prompt or the user
// aborts. Pure (no libgit2/network) so it is directly unit-testable.
std::expected<userpass, error>
resolve_userpass(const char *username_from_url, cred_prompt prompt, void *udata);

// Fetch from `remote` (default refspecs), updating remote-tracking refs.
std::expected<void, error> fetch_remote(std::string repo, std::string remote);

// Push the current branch to `remote` (same-named ref).
std::expected<void, error> push_remote(std::string repo, std::string remote);

// Fetch `remote`, then merge the current branch's remote-tracking ref into HEAD
// (fast-forward or merge commit; conflicts abort). Magit's pull.
std::expected<void, error> pull_remote(std::string repo, std::string remote);

// Create local branch `name` at HEAD (does not switch to it).
std::expected<void, error> create_branch(std::string repo, std::string name);

// Delete local branch `name`.
std::expected<void, error> delete_branch(std::string repo, std::string name);

// Rename local branch `from` to `to`.
std::expected<void, error>
rename_branch(std::string repo, std::string from, std::string to);

// Stage `file` (relative to the repo root) into the index.
std::expected<void, error> stage(std::string repo, std::string file);

// Unstage `file`: reset its index entry to HEAD (or drop it if unborn).
std::expected<void, error> unstage(std::string repo, std::string file);

// Stage every change (modifications, new files, deletions) -- like `git add -A`.
std::expected<void, error> stage_all(std::string repo);

// Unstage everything: reset the index to HEAD, keeping the working tree.
std::expected<void, error> unstage_all(std::string repo);

// Discard `file`'s changes: delete it if untracked, else revert it to HEAD.
std::expected<void, error> discard(std::string repo, std::string file);

// Commit the staged tree with `message`; returns the new commit's short oid.
std::expected<std::string, error> commit(std::string repo, std::string message);

// Amend HEAD: replace it with a commit of the current index tree and `message`,
// keeping HEAD's parents. Returns the amended commit's short oid.
std::expected<std::string, error>
commit_amend(std::string repo, std::string message);

// Extend HEAD: amend in the staged changes but keep HEAD's message.
std::expected<std::string, error> commit_extend(std::string repo);

// Reword HEAD: change only HEAD's message (keep its tree).
std::expected<std::string, error>
commit_reword(std::string repo, std::string message);

// HEAD commit's full message (for pre-filling an amend/reword buffer).
std::expected<std::string, error> head_message(std::string repo);

// The hunks of `path`'s diff: unstaged (workdir vs index) or staged (index vs HEAD).
std::expected<std::vector<hunk>, error>
file_diff(std::string repo, std::string path, bool staged);

// The hunks introduced by commit `rev` (a sha-1 hex string), i.e. its tree vs
// its first parent's (vs the empty tree for a root commit).
std::expected<std::vector<hunk>, error>
commit_diff(std::string repo, std::string rev);

// Stage just hunk `hunk_index` (0-based, as numbered by file_diff(.,.,false))
// of `path` into the index, leaving the file's other hunks unstaged.
std::expected<void, error>
stage_hunk(std::string repo, std::string path, std::size_t hunk_index);

// Unstage just hunk `hunk_index` (0-based, as numbered by file_diff(.,.,true))
// of `path`, returning that hunk to the working tree's unstaged changes.
std::expected<void, error>
unstage_hunk(std::string repo, std::string path, std::size_t hunk_index);

// Stage only the lines [sel_first, sel_last] (0-based indices within hunk
// `hunk_index` of file_diff(.,.,false)) into the index. Unselected additions
// are dropped; unselected deletions are demoted to context. This is magit's
// line/region staging.
std::expected<void, error>
stage_region(std::string repo, std::string path, std::size_t hunk_index,
             std::size_t sel_first, std::size_t sel_last);

// Unstage only the lines [sel_first, sel_last] (0-based indices within hunk
// `hunk_index` of file_diff(.,.,true)) back to the working tree. The reverse
// of stage_region.
std::expected<void, error>
unstage_region(std::string repo, std::string path, std::size_t hunk_index,
               std::size_t sel_first, std::size_t sel_last);

// Discard only the lines [sel_first, sel_last] (0-based indices within hunk
// `hunk_index` of file_diff(.,.,false)) from the working tree, reverting them
// to the index. Destructive (the change is lost).
std::expected<void, error>
discard_region(std::string repo, std::string path, std::size_t hunk_index,
               std::size_t sel_first, std::size_t sel_last);

} // namespace mg::git

// ---- definition -----------------------------------------------------------
namespace mg::git {

static error last_error()
{
    const git_error *e = git_error_last();
    return error{e ? e->klass : 0,
                 e && e->message ? e->message : "unknown libgit2 error"};
}

std::expected<std::vector<mg::magit::file_status>, error>
repo_status(std::string path)
{
    detail::init_guard guard;

    // flags = 0 makes open_ext walk up parent directories (the default), so
    // launching mg in any subdirectory of a repository still finds it.
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    git_status_options opts;
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;

    git_status_list *raw_list = nullptr;
    if (git_status_list_new(&raw_list, repo.get(), &opts) != 0)
        return std::unexpected(last_error());
    detail::status_list_ptr list(raw_list);

    std::vector<mg::magit::file_status> out;
    const size_t n = git_status_list_entrycount(list.get());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(detail::map_entry(git_status_byindex(list.get(), i)));
    return out;
}

std::expected<head_info, error> read_head(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    head_info hi;

    git_reference *raw_ref = nullptr;
    int rc = git_repository_head(&raw_ref, repo.get());
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
        // No commits yet: take the branch name from the symbolic HEAD target.
        git_reference *sym = nullptr;
        if (git_reference_lookup(&sym, repo.get(), "HEAD") == 0) {
            detail::ref_ptr s(sym);
            if (const char *t = git_reference_symbolic_target(s.get())) {
                std::string ref = t; // e.g. "refs/heads/master"
                auto slash = ref.rfind('/');
                hi.branch = slash == std::string::npos ? ref : ref.substr(slash + 1);
            }
        }
        return hi;
    }
    if (rc != 0)
        return std::unexpected(last_error());
    detail::ref_ptr ref(raw_ref);

    if (const char *sh = git_reference_shorthand(ref.get()))
        hi.branch = sh;

    if (const git_oid *oid = git_reference_target(ref.get())) {
        hi.short_oid = detail::short_oid(oid);
        git_commit *raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), oid) == 0) {
            detail::commit_ptr commit(raw_commit);
            if (const char *s = git_commit_summary(commit.get()))
                hi.summary = s;
        }
    }
    return hi;
}

std::expected<upstream_info, error> upstream_status(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    upstream_info info{false, "", 0, 0};

    git_reference *raw_head = nullptr;
    int rc = git_repository_head(&raw_head, repo.get());
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND)
        return info; // unborn branch: nothing to track yet
    if (rc != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);

    git_reference *raw_up = nullptr;
    rc = git_branch_upstream(&raw_up, head.get());
    if (rc == GIT_ENOTFOUND)
        return info; // no upstream configured
    if (rc != 0)
        return std::unexpected(last_error());
    detail::ref_ptr up(raw_up);

    if (const char *sh = git_reference_shorthand(up.get()))
        info.name = sh;

    const git_oid *local = git_reference_target(head.get());
    const git_oid *upstream = git_reference_target(up.get());
    if (local != nullptr && upstream != nullptr) {
        std::size_t ahead = 0, behind = 0;
        if (git_graph_ahead_behind(&ahead, &behind, repo.get(), local,
                                   upstream) != 0)
            return std::unexpected(last_error());
        info.ahead = ahead;
        info.behind = behind;
    }
    info.has_upstream = true;
    return info;
}

std::expected<std::vector<commit_brief>, error>
upstream_commits(std::string path, bool unpushed)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    std::vector<commit_brief> out;

    git_reference *raw_head = nullptr;
    if (git_repository_head(&raw_head, repo.get()) != 0)
        return out; // unborn -> nothing diverges
    detail::ref_ptr head(raw_head);

    git_reference *raw_up = nullptr;
    if (git_branch_upstream(&raw_up, head.get()) != 0)
        return out; // no upstream
    detail::ref_ptr up(raw_up);

    const git_oid *head_oid = git_reference_target(head.get());
    const git_oid *up_oid = git_reference_target(up.get());
    if (head_oid == nullptr || up_oid == nullptr)
        return out;

    git_revwalk *raw_walk = nullptr;
    if (git_revwalk_new(&raw_walk, repo.get()) != 0)
        return std::unexpected(last_error());
    detail::revwalk_ptr walk(raw_walk);
    git_revwalk_sorting(walk.get(), GIT_SORT_TIME);

    // unpushed = HEAD ^upstream ; unpulled = upstream ^HEAD.
    const git_oid *push = unpushed ? head_oid : up_oid;
    const git_oid *hide = unpushed ? up_oid : head_oid;
    if (git_revwalk_push(walk.get(), push) != 0 ||
        git_revwalk_hide(walk.get(), hide) != 0)
        return std::unexpected(last_error());

    git_oid oid;
    while (git_revwalk_next(&oid, walk.get()) == 0) {
        commit_brief cb;
        cb.short_oid = detail::short_oid(&oid);
        cb.oid = detail::full_oid(&oid);
        git_commit *raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), &oid) == 0) {
            detail::commit_ptr commit(raw_commit);
            if (const char *s = git_commit_summary(commit.get()))
                cb.summary = s;
        }
        out.push_back(std::move(cb));
    }
    return out;
}

std::expected<std::vector<commit_brief>, error>
recent_commits(std::string path, std::size_t n)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    std::vector<commit_brief> out;

    git_revwalk *raw_walk = nullptr;
    if (git_revwalk_new(&raw_walk, repo.get()) != 0)
        return std::unexpected(last_error());
    detail::revwalk_ptr walk(raw_walk);

    git_revwalk_sorting(walk.get(), GIT_SORT_TIME);
    if (git_revwalk_push_head(walk.get()) != 0)
        return out; // unborn / no HEAD -> no commits

    git_oid oid;
    while (out.size() < n && git_revwalk_next(&oid, walk.get()) == 0) {
        commit_brief cb;
        cb.short_oid = detail::short_oid(&oid);
        cb.oid = detail::full_oid(&oid);
        git_commit *raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), &oid) == 0) {
            detail::commit_ptr commit(raw_commit);
            if (const char *s = git_commit_summary(commit.get()))
                cb.summary = s;
        }
        out.push_back(std::move(cb));
    }
    return out;
}

std::expected<std::vector<stash_entry>, error> stashes(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    std::vector<stash_entry> out;
    auto cb = [](size_t index, const char *message, const git_oid *stash_id,
                 void *payload) -> int {
        auto *v = static_cast<std::vector<stash_entry> *>(payload);
        v->push_back(stash_entry{index, message ? message : "",
                                 detail::short_oid(stash_id)});
        return 0; // continue
    };
    if (git_stash_foreach(repo.get(), cb, &out) != 0)
        return std::unexpected(last_error());
    return out;
}

std::expected<std::vector<branch_entry>, error> branches(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    git_branch_iterator *raw_iter = nullptr;
    if (git_branch_iterator_new(&raw_iter, repo.get(), GIT_BRANCH_LOCAL) != 0)
        return std::unexpected(last_error());
    detail::branch_iter_ptr iter(raw_iter);

    std::vector<branch_entry> out;
    git_reference *raw_ref = nullptr;
    git_branch_t type;
    int rc;
    while ((rc = git_branch_next(&raw_ref, &type, iter.get())) == 0) {
        detail::ref_ptr ref(raw_ref);
        branch_entry e;
        if (const char *sh = git_reference_shorthand(ref.get()))
            e.name = sh;
        e.is_head = git_branch_is_head(ref.get()) == 1;
        out.push_back(std::move(e));
    }
    if (rc != GIT_ITEROVER)
        return std::unexpected(last_error());
    return out;
}

std::expected<void, error> stash_push(std::string repo, std::string message)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Prefer the configured identity; fall back to a placeholder so a stash
    // still works in a repo with no user.name/user.email.
    git_signature *raw_sig = nullptr;
    if (git_signature_default(&raw_sig, r.get()) != 0 &&
        git_signature_now(&raw_sig, "mg", "mg@localhost") != 0)
        return std::unexpected(last_error());
    detail::sig_ptr sig(raw_sig);

    git_oid oid;
    // GIT_STASH_DEFAULT stashes tracked changes (index + working tree). A clean
    // tree -> GIT_ENOTFOUND, surfaced as an error (nothing to stash).
    if (git_stash_save(&oid, r.get(), sig.get(), message.c_str(),
                       GIT_STASH_DEFAULT) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> stash_pop(std::string repo, std::size_t index)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    if (git_stash_pop(r.get(), index, nullptr) != 0) // applies, then drops
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> stash_apply(std::string repo, std::size_t index)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    if (git_stash_apply(r.get(), index, nullptr) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> stash_drop(std::string repo, std::size_t index)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    if (git_stash_drop(r.get(), index) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> checkout_branch(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_ref = nullptr;
    if (git_branch_lookup(&raw_ref, r.get(), name.c_str(), GIT_BRANCH_LOCAL) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr ref(raw_ref);

    git_object *raw_tree = nullptr;
    if (git_reference_peel(&raw_tree, ref.get(), GIT_OBJECT_TREE) != 0)
        return std::unexpected(last_error());
    detail::object_ptr tree(raw_tree);

    // SAFE refuses to clobber conflicting local edits (surfaced as an error).
    git_checkout_options opts;
    git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_SAFE;
    if (git_checkout_tree(r.get(), tree.get(), &opts) != 0)
        return std::unexpected(last_error());

    if (git_repository_set_head(r.get(), git_reference_name(ref.get())) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> create_branch(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Peel HEAD to the commit the new branch should point at.
    git_object *raw_head = nullptr;
    if (git_revparse_single(&raw_head, r.get(), "HEAD") != 0)
        return std::unexpected(last_error());
    detail::object_ptr head(raw_head);

    git_reference *raw_branch = nullptr;
    if (git_branch_create(&raw_branch, r.get(), name.c_str(),
                          reinterpret_cast<git_commit *>(head.get()),
                          /*force=*/0) != 0)
        return std::unexpected(last_error());
    git_reference_free(raw_branch);
    return {};
}

std::expected<void, error> delete_branch(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_ref = nullptr;
    if (git_branch_lookup(&raw_ref, r.get(), name.c_str(), GIT_BRANCH_LOCAL) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr ref(raw_ref);

    if (git_branch_delete(ref.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error>
rename_branch(std::string repo, std::string from, std::string to)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_ref = nullptr;
    if (git_branch_lookup(&raw_ref, r.get(), from.c_str(), GIT_BRANCH_LOCAL) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr ref(raw_ref);

    git_reference *raw_new = nullptr;
    if (git_branch_move(&raw_new, ref.get(), to.c_str(), /*force=*/0) != 0)
        return std::unexpected(last_error());
    git_reference_free(raw_new);
    return {};
}

// Configured identity, falling back to a placeholder so commits/reverts/merges
// still work in a repo with no user.name/user.email.
static detail::sig_ptr default_signature(git_repository *repo)
{
    git_signature *raw = nullptr;
    if (git_signature_default(&raw, repo) != 0 &&
        git_signature_now(&raw, "mg", "mg@localhost") != 0)
        return detail::sig_ptr(nullptr);
    return detail::sig_ptr(raw);
}

std::expected<void, error>
reset_to(std::string repo, std::string rev, reset_mode mode)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_obj = nullptr;
    if (git_revparse_single(&raw_obj, r.get(), rev.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw_obj);

    const git_reset_t t = mode == reset_mode::soft   ? GIT_RESET_SOFT
                          : mode == reset_mode::hard ? GIT_RESET_HARD
                                                     : GIT_RESET_MIXED;
    if (git_reset(r.get(), obj.get(), t, nullptr) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> revert_commit(std::string repo, std::string rev)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_obj = nullptr;
    if (git_revparse_single(&raw_obj, r.get(), rev.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw_obj);
    git_commit *raw_target = nullptr;
    if (git_commit_lookup(&raw_target, r.get(), git_object_id(obj.get())) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr target(raw_target);

    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") != 0)
        return std::unexpected(last_error());
    git_commit *raw_head = nullptr;
    if (git_commit_lookup(&raw_head, r.get(), &head_oid) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr head(raw_head);

    // Compute the reverted tree in memory (no working-tree state machine).
    git_index *raw_idx = nullptr;
    if (git_revert_commit(&raw_idx, r.get(), target.get(), head.get(), 0,
                          nullptr) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    if (git_index_has_conflicts(idx.get()))
        return std::unexpected(error{0, "revert has conflicts"});

    git_oid tree_oid;
    if (git_index_write_tree_to(&tree_oid, idx.get(), r.get()) != 0)
        return std::unexpected(last_error());
    git_tree *raw_tree = nullptr;
    if (git_tree_lookup(&raw_tree, r.get(), &tree_oid) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr tree(raw_tree);

    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());

    const char *summary = git_commit_summary(target.get());
    std::string msg = "Revert \"" + std::string(summary ? summary : "") + "\"";
    const git_commit *parents[1] = {head.get()};
    git_oid commit_oid;
    if (git_commit_create(&commit_oid, r.get(), "HEAD", sig.get(), sig.get(),
                          nullptr, msg.c_str(), tree.get(), 1, parents) != 0)
        return std::unexpected(last_error());

    // Bring the working tree + index to the new commit's content.
    git_checkout_options opts;
    git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;
    if (git_checkout_tree(r.get(), reinterpret_cast<git_object *>(tree.get()),
                          &opts) != 0)
        return std::unexpected(last_error());
    return {};
}

// Merge an already-resolved annotated commit into HEAD: up-to-date (noop) /
// fast-forward (checkout + move ref) / true merge (write a 2-parent `msg`
// commit). Conflicts abort cleanly (state_cleanup + reset --hard) with an error.
static std::expected<void, error>
merge_annotated(git_repository *repo, git_annotated_commit *their,
                const std::string &msg)
{
    const git_annotated_commit *heads[1] = {their};
    git_merge_analysis_t analysis;
    git_merge_preference_t pref;
    if (git_merge_analysis(&analysis, &pref, repo, heads, 1) != 0)
        return std::unexpected(last_error());

    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)
        return {}; // already contains their commit

    const git_oid *their_oid = git_annotated_commit_id(their);

    if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
        git_object *raw_target = nullptr;
        if (git_object_lookup(&raw_target, repo, their_oid,
                              GIT_OBJECT_COMMIT) != 0)
            return std::unexpected(last_error());
        detail::object_ptr target(raw_target);

        git_checkout_options copts;
        git_checkout_options_init(&copts, GIT_CHECKOUT_OPTIONS_VERSION);
        copts.checkout_strategy = GIT_CHECKOUT_SAFE;
        if (git_checkout_tree(repo, target.get(), &copts) != 0)
            return std::unexpected(last_error());

        git_reference *raw_head = nullptr;
        if (git_repository_head(&raw_head, repo) != 0)
            return std::unexpected(last_error());
        detail::ref_ptr head(raw_head);
        git_reference *raw_new = nullptr;
        if (git_reference_set_target(&raw_new, head.get(), their_oid,
                                     "merge: fast-forward") != 0)
            return std::unexpected(last_error());
        git_reference_free(raw_new);
        return {};
    }

    git_merge_options mopts;
    git_merge_options_init(&mopts, GIT_MERGE_OPTIONS_VERSION);
    git_checkout_options copts;
    git_checkout_options_init(&copts, GIT_CHECKOUT_OPTIONS_VERSION);
    copts.checkout_strategy = GIT_CHECKOUT_SAFE;
    if (git_merge(repo, heads, 1, &mopts, &copts) != 0)
        return std::unexpected(last_error());

    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, repo) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    if (git_index_has_conflicts(idx.get())) {
        git_repository_state_cleanup(repo);
        git_object *raw_head_obj = nullptr;
        if (git_revparse_single(&raw_head_obj, repo, "HEAD") == 0) {
            detail::object_ptr ho(raw_head_obj);
            git_reset(repo, ho.get(), GIT_RESET_HARD, nullptr);
        }
        return std::unexpected(error{0, "merge conflicts"});
    }

    git_oid tree_oid;
    if (git_index_write_tree(&tree_oid, idx.get()) != 0)
        return std::unexpected(last_error());
    git_tree *raw_tree = nullptr;
    if (git_tree_lookup(&raw_tree, repo, &tree_oid) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr tree(raw_tree);

    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, repo, "HEAD") != 0)
        return std::unexpected(last_error());
    git_commit *raw_head_commit = nullptr;
    if (git_commit_lookup(&raw_head_commit, repo, &head_oid) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr head_commit(raw_head_commit);
    git_commit *raw_their_commit = nullptr;
    if (git_commit_lookup(&raw_their_commit, repo, their_oid) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr their_commit(raw_their_commit);

    detail::sig_ptr sig = default_signature(repo);
    if (!sig)
        return std::unexpected(last_error());

    const git_commit *parents[2] = {head_commit.get(), their_commit.get()};
    git_oid merge_oid;
    if (git_commit_create(&merge_oid, repo, "HEAD", sig.get(), sig.get(),
                          nullptr, msg.c_str(), tree.get(), 2, parents) != 0)
        return std::unexpected(last_error());

    git_repository_state_cleanup(repo);
    return {};
}

std::expected<void, error> merge_branch(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_ref = nullptr;
    if (git_branch_lookup(&raw_ref, r.get(), name.c_str(), GIT_BRANCH_LOCAL) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr ref(raw_ref);

    git_annotated_commit *raw_their = nullptr;
    if (git_annotated_commit_from_ref(&raw_their, r.get(), ref.get()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)>
        their(raw_their, git_annotated_commit_free);

    return merge_annotated(r.get(), their.get(), "Merge branch '" + name + "'");
}

std::expected<void, error> rebase_onto(std::string repo, std::string upstream)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Resolve `upstream` to an annotated commit (the new base).
    git_object *raw_obj = nullptr;
    if (git_revparse_single(&raw_obj, r.get(), upstream.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw_obj);
    git_annotated_commit *raw_up = nullptr;
    if (git_annotated_commit_lookup(&raw_up, r.get(),
                                    git_object_id(obj.get())) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)>
        up(raw_up, git_annotated_commit_free);

    git_rebase_options ropts;
    git_rebase_options_init(&ropts, GIT_REBASE_OPTIONS_VERSION);

    git_rebase *raw_rebase = nullptr;
    // branch = NULL -> rebase HEAD; onto = NULL -> onto `upstream`.
    if (git_rebase_init(&raw_rebase, r.get(), nullptr, up.get(), nullptr,
                        &ropts) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_rebase, decltype(&git_rebase_free)> rebase(
        raw_rebase, git_rebase_free);

    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());

    git_rebase_operation *op = nullptr;
    int rc;
    while ((rc = git_rebase_next(&op, rebase.get())) == 0) {
        git_oid id;
        // author = NULL keeps the original; committer = sig; message = NULL
        // keeps the original commit message.
        int crc = git_rebase_commit(&id, rebase.get(), nullptr, sig.get(),
                                    nullptr, nullptr);
        if (crc == GIT_EAPPLIED)
            continue; // commit already present upstream -> dropped
        if (crc != 0) {
            git_rebase_abort(rebase.get());
            return std::unexpected(error{0, "rebase conflict (aborted)"});
        }
    }
    if (rc != GIT_ITEROVER) {
        error e = last_error();
        git_rebase_abort(rebase.get());
        return std::unexpected(e);
    }
    if (git_rebase_finish(rebase.get(), sig.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

// Process-wide credential prompt (set by the UI via set_cred_prompt).
static cred_prompt g_cred_prompt = nullptr;
static void *g_cred_udata = nullptr;

void set_cred_prompt(cred_prompt fn, void *udata)
{
    g_cred_prompt = fn;
    g_cred_udata = udata;
}

std::expected<userpass, error>
resolve_userpass(const char *username_from_url, cred_prompt prompt, void *udata)
{
    if (prompt == nullptr)
        return std::unexpected(error{0, "no credential prompt available"});

    userpass up;
    char buf[256];
    if (username_from_url != nullptr && username_from_url[0] != '\0') {
        up.user = username_from_url;
    } else {
        if (prompt("Username: ", 0, buf, sizeof buf, udata) != 1)
            return std::unexpected(error{0, "authentication cancelled"});
        up.user = buf;
    }
    if (prompt("Password: ", 1, buf, sizeof buf, udata) != 1)
        return std::unexpected(error{0, "authentication cancelled"});
    up.pass = buf;
    return up;
}

// Credentials for fetch/push: try the ssh-agent (the common `git@host:...`
// case); for HTTPS user/password, prompt via the registered UI callback;
// answer username-only probes from the URL. PASSTHROUGH (op fails cleanly)
// when nothing applies -- e.g. no prompt registered for a userpass request.
static int credentials_cb(git_credential **out, const char *url,
                          const char *username_from_url,
                          unsigned int allowed_types, void *payload)
{
    (void)url;
    (void)payload;
    const char *user = username_from_url ? username_from_url : "git";
    if (allowed_types & GIT_CREDENTIAL_SSH_KEY)
        return git_credential_ssh_key_from_agent(out, user);
    if ((allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) &&
        g_cred_prompt != nullptr) {
        auto up = resolve_userpass(username_from_url, g_cred_prompt, g_cred_udata);
        if (!up)
            return GIT_EUSER; // user cancelled -> abort the transfer
        return git_credential_userpass_plaintext_new(out, up->user.c_str(),
                                                     up->pass.c_str());
    }
    if (allowed_types & GIT_CREDENTIAL_USERNAME)
        return git_credential_username_new(out, user);
    return GIT_PASSTHROUGH;
}

std::expected<void, error> fetch_remote(std::string repo, std::string remote)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_remote *raw_remote = nullptr;
    if (git_remote_lookup(&raw_remote, r.get(), remote.c_str()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_remote, decltype(&git_remote_free)> rem(
        raw_remote, git_remote_free);

    git_fetch_options opts;
    git_fetch_options_init(&opts, GIT_FETCH_OPTIONS_VERSION);
    opts.callbacks.credentials = credentials_cb;
    // nullptr refspecs -> the remote's configured fetch refspecs.
    if (git_remote_fetch(rem.get(), nullptr, &opts, nullptr) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> push_remote(std::string repo, std::string remote)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_head = nullptr;
    if (git_repository_head(&raw_head, r.get()) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);
    const char *branch = git_reference_shorthand(head.get());
    if (branch == nullptr)
        return std::unexpected(error{0, "not on a branch"});

    git_remote *raw_remote = nullptr;
    if (git_remote_lookup(&raw_remote, r.get(), remote.c_str()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_remote, decltype(&git_remote_free)> rem(
        raw_remote, git_remote_free);

    std::string b(branch);
    std::string spec = "refs/heads/" + b + ":refs/heads/" + b;
    char *specs[1] = {const_cast<char *>(spec.c_str())};
    git_strarray refspecs = {specs, 1};
    git_push_options opts;
    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    opts.callbacks.credentials = credentials_cb;
    if (git_remote_push(rem.get(), &refspecs, &opts) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> pull_remote(std::string repo, std::string remote)
{
    if (auto fetched = fetch_remote(repo, remote); !fetched)
        return fetched;

    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_reference *raw_head = nullptr;
    if (git_repository_head(&raw_head, r.get()) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);
    const char *branch = git_reference_shorthand(head.get());
    if (branch == nullptr)
        return std::unexpected(error{0, "not on a branch"});

    // Merge the just-updated remote-tracking ref into HEAD.
    std::string tracking = "refs/remotes/" + remote + "/" + std::string(branch);
    git_reference *raw_track = nullptr;
    if (git_reference_lookup(&raw_track, r.get(), tracking.c_str()) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr track(raw_track);

    git_annotated_commit *raw_their = nullptr;
    if (git_annotated_commit_from_ref(&raw_their, r.get(), track.get()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)>
        their(raw_their, git_annotated_commit_free);

    return merge_annotated(r.get(), their.get(), "Merge " + tracking);
}

std::expected<void, error> stage(std::string repo, std::string file)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);

    unsigned int flags = 0;
    const bool deleted = git_status_file(&flags, r.get(), file.c_str()) == 0 &&
                         (flags & GIT_STATUS_WT_DELETED);

    const int rc = deleted ? git_index_remove_bypath(idx.get(), file.c_str())
                           : git_index_add_bypath(idx.get(), file.c_str());
    if (rc != 0 || git_index_write(idx.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> unstage(std::string repo, std::string file)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_head = nullptr;
    if (git_revparse_single(&raw_head, r.get(), "HEAD") == 0) {
        detail::object_ptr head(raw_head);
        char *paths[1] = {const_cast<char *>(file.c_str())};
        git_strarray pathspec = {paths, 1};
        if (git_reset_default(r.get(), head.get(), &pathspec) != 0)
            return std::unexpected(last_error());
        return {};
    }

    // Unborn branch (no HEAD): just remove the staged entry from the index.
    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    if (git_index_remove_bypath(idx.get(), file.c_str()) != 0 ||
        git_index_write(idx.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> stage_all(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);

    git_strarray all = {nullptr, 0}; // empty pathspec = every path
    // add_all stages new + modified; update_all also records deletions.
    if (git_index_add_all(idx.get(), &all, GIT_INDEX_ADD_DEFAULT, nullptr,
                          nullptr) != 0 ||
        git_index_update_all(idx.get(), &all, nullptr, nullptr) != 0 ||
        git_index_write(idx.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> unstage_all(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_head = nullptr;
    if (git_revparse_single(&raw_head, r.get(), "HEAD") == 0) {
        detail::object_ptr head(raw_head);
        // Reset the index to HEAD; HEAD == target so the branch does not move,
        // and MIXED leaves the working tree untouched.
        if (git_reset(r.get(), head.get(), GIT_RESET_MIXED, nullptr) != 0)
            return std::unexpected(last_error());
        return {};
    }
    // Unborn branch: everything staged is "new", so clear the index.
    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    if (git_index_clear(idx.get()) != 0 || git_index_write(idx.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> discard(std::string repo, std::string file)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    unsigned int flags = 0;
    const bool untracked = git_status_file(&flags, r.get(), file.c_str()) == 0 &&
                           (flags & GIT_STATUS_WT_NEW);

    if (untracked) {
        const char *wd = git_repository_workdir(r.get());
        if (wd == nullptr)
            return std::unexpected(error{0, "no work tree"});
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(wd) / file, ec);
        if (ec)
            return std::unexpected(error{0, ec.message()});
        return {};
    }

    // Tracked: force-checkout the path from HEAD, dropping worktree+index edits.
    git_checkout_options opts;
    git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;
    char *paths[1] = {const_cast<char *>(file.c_str())};
    opts.paths.strings = paths;
    opts.paths.count = 1;
    if (git_checkout_head(r.get(), &opts) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<std::string, error> commit(std::string repo, std::string message)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_signature *raw_sig = nullptr;
    if (git_signature_default(&raw_sig, r.get()) != 0)
        return std::unexpected(last_error()); // user.name/user.email unset
    detail::sig_ptr sig(raw_sig);

    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);

    git_oid tree_oid;
    if (git_index_write_tree(&tree_oid, idx.get()) != 0)
        return std::unexpected(last_error());
    git_tree *raw_tree = nullptr;
    if (git_tree_lookup(&raw_tree, r.get(), &tree_oid) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr tree(raw_tree);

    // Parent = current HEAD commit, if the branch is born.
    git_commit *raw_parent = nullptr;
    git_oid head_oid;
    bool has_parent = git_reference_name_to_id(&head_oid, r.get(), "HEAD") == 0 &&
                      git_commit_lookup(&raw_parent, r.get(), &head_oid) == 0;
    detail::commit_ptr parent(raw_parent);
    const git_commit *parents[1] = {parent.get()};

    git_oid commit_oid;
    if (git_commit_create(&commit_oid, r.get(), "HEAD", sig.get(), sig.get(),
                          nullptr, message.c_str(), tree.get(),
                          has_parent ? 1 : 0, has_parent ? parents : nullptr) != 0)
        return std::unexpected(last_error());

    return detail::short_oid(&commit_oid);
}

// Shared amend over HEAD: `message` (NULL keeps HEAD's), and the current index
// tree when `use_index_tree` (else keep HEAD's tree). Author is preserved; the
// committer is refreshed. update_ref = "HEAD" moves the branch.
static std::expected<std::string, error>
amend_impl(std::string repo, const char *message, bool use_index_tree)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") != 0)
        return std::unexpected(last_error());
    git_commit *raw_head = nullptr;
    if (git_commit_lookup(&raw_head, r.get(), &head_oid) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr head(raw_head);

    git_signature *raw_sig = nullptr;
    if (git_signature_default(&raw_sig, r.get()) != 0)
        return std::unexpected(last_error()); // user.name/user.email unset
    detail::sig_ptr sig(raw_sig);

    detail::tree_ptr tree;
    if (use_index_tree) {
        git_index *raw_idx = nullptr;
        if (git_repository_index(&raw_idx, r.get()) != 0)
            return std::unexpected(last_error());
        detail::index_ptr idx(raw_idx);
        git_oid tree_oid;
        if (git_index_write_tree(&tree_oid, idx.get()) != 0)
            return std::unexpected(last_error());
        git_tree *raw_tree = nullptr;
        if (git_tree_lookup(&raw_tree, r.get(), &tree_oid) != 0)
            return std::unexpected(last_error());
        tree.reset(raw_tree);
    }

    git_oid out;
    if (git_commit_amend(&out, head.get(), "HEAD", nullptr, sig.get(), nullptr,
                         message, tree.get()) != 0)
        return std::unexpected(last_error());
    return detail::short_oid(&out);
}

std::expected<std::string, error>
commit_amend(std::string repo, std::string message)
{
    return amend_impl(std::move(repo), message.c_str(), true);
}

std::expected<std::string, error> commit_extend(std::string repo)
{
    return amend_impl(std::move(repo), nullptr, true);
}

std::expected<std::string, error>
commit_reword(std::string repo, std::string message)
{
    return amend_impl(std::move(repo), message.c_str(), false);
}

std::expected<std::string, error> head_message(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") != 0)
        return std::unexpected(last_error());
    git_commit *raw_c = nullptr;
    if (git_commit_lookup(&raw_c, r.get(), &head_oid) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr c(raw_c);
    const char *m = git_commit_message(c.get());
    return std::string(m != nullptr ? m : "");
}

// Flatten a libgit2 diff into our hunk/diff_line value types.
static std::vector<hunk> diff_to_hunks(git_diff *diff)
{
    std::vector<hunk> out;
    const size_t ndeltas = git_diff_num_deltas(diff);
    for (size_t di = 0; di < ndeltas; ++di) {
        git_patch *raw_patch = nullptr;
        if (git_patch_from_diff(&raw_patch, diff, di) != 0)
            continue;
        detail::patch_ptr patch(raw_patch);

        const size_t nhunks = git_patch_num_hunks(patch.get());
        for (size_t hi = 0; hi < nhunks; ++hi) {
            const git_diff_hunk *gh = nullptr;
            size_t nlines = 0;
            if (git_patch_get_hunk(&gh, &nlines, patch.get(), hi) != 0)
                continue;
            hunk h;
            h.header.assign(gh->header, gh->header_len);
            for (size_t li = 0; li < nlines; ++li) {
                const git_diff_line *gl = nullptr;
                if (git_patch_get_line_in_hunk(&gl, patch.get(), hi, li) != 0)
                    continue;
                h.lines.push_back(diff_line{
                    gl->origin, std::string(gl->content, gl->content_len)});
            }
            out.push_back(std::move(h));
        }
    }
    return out;
}

std::expected<std::vector<hunk>, error>
file_diff(std::string repo, std::string path, bool staged)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_diff_options opts;
    git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
    char *paths[1] = {const_cast<char *>(path.c_str())};
    opts.pathspec.strings = paths;
    opts.pathspec.count = 1;

    git_diff *raw_diff = nullptr;
    if (staged) {
        // index vs HEAD's tree (peeled; null if unborn).
        git_object *raw_tree = nullptr;
        git_revparse_single(&raw_tree, r.get(), "HEAD^{tree}");
        detail::object_ptr tree(raw_tree);
        if (git_diff_tree_to_index(&raw_diff, r.get(),
                                   reinterpret_cast<git_tree *>(tree.get()),
                                   nullptr, &opts) != 0)
            return std::unexpected(last_error());
    } else {
        if (git_diff_index_to_workdir(&raw_diff, r.get(), nullptr, &opts) != 0)
            return std::unexpected(last_error());
    }
    detail::diff_ptr diff(raw_diff);
    return diff_to_hunks(diff.get());
}

std::expected<std::vector<hunk>, error>
commit_diff(std::string repo, std::string rev)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_obj = nullptr;
    if (git_revparse_single(&raw_obj, r.get(), rev.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw_obj);

    git_commit *raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, r.get(), git_object_id(obj.get())) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr commit(raw_commit);

    git_tree *raw_tree = nullptr;
    if (git_commit_tree(&raw_tree, commit.get()) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr tree(raw_tree);

    // First parent's tree, or null (-> empty tree) for a root commit.
    detail::tree_ptr parent_tree(nullptr);
    if (git_commit_parentcount(commit.get()) > 0) {
        git_commit *raw_parent = nullptr;
        if (git_commit_parent(&raw_parent, commit.get(), 0) != 0)
            return std::unexpected(last_error());
        detail::commit_ptr parent(raw_parent);
        git_tree *raw_pt = nullptr;
        if (git_commit_tree(&raw_pt, parent.get()) != 0)
            return std::unexpected(last_error());
        parent_tree.reset(raw_pt);
    }

    git_diff *raw_diff = nullptr;
    if (git_diff_tree_to_tree(&raw_diff, r.get(), parent_tree.get(), tree.get(),
                              nullptr) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);
    return diff_to_hunks(diff.get());
}

// Apply one hunk of `diff` (scoped to a single path) to the index.
static std::expected<void, error>
apply_hunk_to_index(git_repository *repo, git_diff *diff, std::size_t hunk_index)
{
    detail::hunk_filter filter{hunk_index};
    git_apply_options aopts;
    git_apply_options_init(&aopts, GIT_APPLY_OPTIONS_VERSION);
    aopts.hunk_cb = detail::apply_one_hunk;
    aopts.payload = &filter;
    if (git_apply(repo, diff, GIT_APPLY_LOCATION_INDEX, &aopts) != 0)
        return std::unexpected(last_error());
    return {};
}

// Fill `opts` with a single-path pathspec (the storage must outlive the diff).
static void path_scoped_diff_opts(git_diff_options &opts, char **path_storage)
{
    git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
    opts.pathspec.strings = path_storage;
    opts.pathspec.count = 1;
}

std::expected<void, error>
stage_hunk(std::string repo, std::string path, std::size_t hunk_index)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    char *paths[1] = {const_cast<char *>(path.c_str())};
    git_diff_options opts;
    path_scoped_diff_opts(opts, paths);

    // index -> workdir : the unstaged changes; applying a hunk to the index
    // stages exactly that hunk.
    git_diff *raw_diff = nullptr;
    if (git_diff_index_to_workdir(&raw_diff, r.get(), nullptr, &opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    return apply_hunk_to_index(r.get(), diff.get(), hunk_index);
}

std::expected<void, error>
unstage_hunk(std::string repo, std::string path, std::size_t hunk_index)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Snapshot the index as a tree (its staged content).
    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    git_oid index_tree_oid;
    if (git_index_write_tree(&index_tree_oid, idx.get()) != 0)
        return std::unexpected(last_error());
    git_tree *raw_index_tree = nullptr;
    if (git_tree_lookup(&raw_index_tree, r.get(), &index_tree_oid) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr index_tree(raw_index_tree);

    // HEAD's tree (null on an unborn branch -> diff against the empty tree).
    git_object *raw_head_tree = nullptr;
    git_revparse_single(&raw_head_tree, r.get(), "HEAD^{tree}");
    detail::object_ptr head_tree(raw_head_tree);

    char *paths[1] = {const_cast<char *>(path.c_str())};
    git_diff_options opts;
    path_scoped_diff_opts(opts, paths);

    // index -> HEAD : the reverse of the staged view (file_diff(.,.,true)), so
    // applying a hunk to the index rolls just that hunk back toward HEAD.
    git_diff *raw_diff = nullptr;
    if (git_diff_tree_to_tree(&raw_diff, r.get(), index_tree.get(),
                              reinterpret_cast<git_tree *>(head_tree.get()),
                              &opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    return apply_hunk_to_index(r.get(), diff.get(), hunk_index);
}

// Find, in `diff`, the patch for the delta whose new-side path is `path`.
// Returns nullptr if no delta matches.
static detail::patch_ptr patch_for_path(git_diff *diff, const std::string &path)
{
    const size_t ndeltas = git_diff_num_deltas(diff);
    for (size_t di = 0; di < ndeltas; ++di) {
        const git_diff_delta *delta = git_diff_get_delta(diff, di);
        if (!delta || !delta->new_file.path || path != delta->new_file.path)
            continue;
        git_patch *raw = nullptr;
        if (git_patch_from_diff(&raw, diff, di) != 0)
            return nullptr;
        return detail::patch_ptr(raw);
    }
    return nullptr;
}

// Synthesize a single-hunk unified-diff text covering only the selected lines
// of `patch`'s hunk `hunk_index`. Unselected additions are omitted; unselected
// deletions become context (so they survive unchanged on the side we apply
// to). The header uses the hunk's old_start for both sides because the patch is
// applied against that baseline (the index for staging, HEAD for unstaging).
static std::string build_region_patch(git_patch *patch, std::size_t hunk_index,
                                       std::size_t sel_first,
                                       std::size_t sel_last,
                                       const std::string &path)
{
    const git_diff_hunk *gh = nullptr;
    size_t nlines = 0;
    if (git_patch_get_hunk(&gh, &nlines, patch, hunk_index) != 0)
        return {};

    std::string body;
    int old_count = 0, new_count = 0;
    for (size_t li = 0; li < nlines; ++li) {
        const git_diff_line *gl = nullptr;
        if (git_patch_get_line_in_hunk(&gl, patch, hunk_index, li) != 0)
            continue;
        std::string content(gl->content, gl->content_len);
        const bool selected = li >= sel_first && li <= sel_last;
        switch (gl->origin) {
        case GIT_DIFF_LINE_CONTEXT:
            body += ' ' + content;
            ++old_count, ++new_count;
            break;
        case GIT_DIFF_LINE_ADDITION:
            if (selected) {
                body += '+' + content;
                ++new_count;
            } // unselected addition: dropped entirely
            break;
        case GIT_DIFF_LINE_DELETION:
            if (selected) {
                body += '-' + content;
                ++old_count;
            } else { // unselected deletion: keep the line as context
                body += ' ' + content;
                ++old_count, ++new_count;
            }
            break;
        default: // EOFNL markers etc.: pass through verbatim
            body += content;
            break;
        }
    }

    char header[128];
    std::snprintf(header, sizeof header, "@@ -%d,%d +%d,%d @@\n", gh->old_start,
                  old_count, gh->old_start, new_count);
    return "diff --git a/" + path + " b/" + path + "\n--- a/" + path +
           "\n+++ b/" + path + "\n" + header + body;
}

// Apply a hand-built unified-diff text to `location` (INDEX for staging).
static std::expected<void, error>
apply_patch_text(git_repository *repo, const std::string &text,
                 git_apply_location_t location)
{
    git_diff *raw = nullptr;
    if (git_diff_from_buffer(&raw, text.data(), text.size()) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr partial(raw);

    git_apply_options aopts;
    git_apply_options_init(&aopts, GIT_APPLY_OPTIONS_VERSION);
    if (git_apply(repo, partial.get(), location, &aopts) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error>
stage_region(std::string repo, std::string path, std::size_t hunk_index,
             std::size_t sel_first, std::size_t sel_last)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    char *paths[1] = {const_cast<char *>(path.c_str())};
    git_diff_options opts;
    path_scoped_diff_opts(opts, paths);

    // index -> workdir: the unstaged changes; a region patch built from this
    // and applied to the index stages exactly the selected lines.
    git_diff *raw_diff = nullptr;
    if (git_diff_index_to_workdir(&raw_diff, r.get(), nullptr, &opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    detail::patch_ptr patch = patch_for_path(diff.get(), path);
    if (!patch)
        return std::unexpected(error{0, "no diff for path"});

    std::string text =
        build_region_patch(patch.get(), hunk_index, sel_first, sel_last, path);
    return apply_patch_text(r.get(), text, GIT_APPLY_LOCATION_INDEX);
}

std::expected<void, error>
discard_region(std::string repo, std::string path, std::size_t hunk_index,
               std::size_t sel_first, std::size_t sel_last)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    char *paths[1] = {const_cast<char *>(path.c_str())};
    git_diff_options opts;
    path_scoped_diff_opts(opts, paths);
    // REVERSE -> workdir->index: its hunks mirror the unstaged view's (so the
    // selected indices carry over) and its context lines come from the workdir,
    // matching the apply baseline. Applying to the workdir reverts the selected
    // lines back toward the index.
    opts.flags |= GIT_DIFF_REVERSE;

    git_diff *raw_diff = nullptr;
    if (git_diff_index_to_workdir(&raw_diff, r.get(), nullptr, &opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    detail::patch_ptr patch = patch_for_path(diff.get(), path);
    if (!patch)
        return std::unexpected(error{0, "no diff for path"});

    std::string text =
        build_region_patch(patch.get(), hunk_index, sel_first, sel_last, path);
    return apply_patch_text(r.get(), text, GIT_APPLY_LOCATION_WORKDIR);
}

std::expected<void, error>
unstage_region(std::string repo, std::string path, std::size_t hunk_index,
               std::size_t sel_first, std::size_t sel_last)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Snapshot the index as a tree (its staged content), as unstage_hunk does.
    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);
    git_oid index_tree_oid;
    if (git_index_write_tree(&index_tree_oid, idx.get()) != 0)
        return std::unexpected(last_error());
    git_tree *raw_index_tree = nullptr;
    if (git_tree_lookup(&raw_index_tree, r.get(), &index_tree_oid) != 0)
        return std::unexpected(last_error());
    detail::tree_ptr index_tree(raw_index_tree);

    git_object *raw_head_tree = nullptr;
    git_revparse_single(&raw_head_tree, r.get(), "HEAD^{tree}");
    detail::object_ptr head_tree(raw_head_tree);

    char *paths[1] = {const_cast<char *>(path.c_str())};
    git_diff_options opts;
    path_scoped_diff_opts(opts, paths);

    // index -> HEAD: the reverse of the staged view. Its hunks mirror the
    // staged diff's (same context, same line positions), so the selected
    // indices carry over; its context lines come from the index, matching the
    // apply baseline. A region patch built here and applied to the index rolls
    // exactly the selected lines back toward HEAD.
    git_diff *raw_diff = nullptr;
    if (git_diff_tree_to_tree(&raw_diff, r.get(), index_tree.get(),
                              reinterpret_cast<git_tree *>(head_tree.get()),
                              &opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    detail::patch_ptr patch = patch_for_path(diff.get(), path);
    if (!patch)
        return std::unexpected(error{0, "no diff for path"});

    std::string text =
        build_region_patch(patch.get(), hunk_index, sel_first, sel_last, path);
    return apply_patch_text(r.get(), text, GIT_APPLY_LOCATION_INDEX);
}

} // namespace mg::git
