// mg.git -- libgit2-backed working-tree status reader (task M2c-2).
//
// Reads structured status straight from the repository (no subprocess, no
// porcelain text). The libgit2 C handles are owned by RAII wrappers so they are
// freed on every path; errors surface as std::expected, never raw int codes.

module;
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
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

// 8-char abbreviated oid, like git's default short form.
inline std::string short_oid(const git_oid *oid)
{
    char buf[9];
    git_oid_tostr(buf, sizeof buf, oid);
    return std::string(buf);
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
    std::string summary;
};

std::expected<std::vector<mg::magit::file_status>, error>
repo_status(std::string path);

std::expected<head_info, error> read_head(std::string path);

std::expected<std::vector<commit_brief>, error>
recent_commits(std::string path, std::size_t n);

// Stage `file` (relative to the repo root) into the index.
std::expected<void, error> stage(std::string repo, std::string file);

// Unstage `file`: reset its index entry to HEAD (or drop it if unborn).
std::expected<void, error> unstage(std::string repo, std::string file);

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

} // namespace mg::git
