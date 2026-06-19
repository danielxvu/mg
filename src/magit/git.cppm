// mg.git -- libgit2-backed working-tree status reader (task M2c-2).
//
// Reads structured status straight from the repository (no subprocess, no
// porcelain text). The libgit2 C handles are owned by RAII wrappers so they are
// freed on every path; errors surface as std::expected, never raw int codes.

module;
#include <cstddef>
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

// 8-char abbreviated oid, like git's default short form.
inline std::string short_oid(const git_oid *oid)
{
    char buf[9];
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
    std::string summary;
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

std::expected<std::vector<commit_brief>, error>
recent_commits(std::string path, std::size_t n);

// Stage `file` (relative to the repo root) into the index.
std::expected<void, error> stage(std::string repo, std::string file);

// Unstage `file`: reset its index entry to HEAD (or drop it if unborn).
std::expected<void, error> unstage(std::string repo, std::string file);

// Discard `file`'s changes: delete it if untracked, else revert it to HEAD.
std::expected<void, error> discard(std::string repo, std::string file);

// Commit the staged tree with `message`; returns the new commit's short oid.
std::expected<std::string, error> commit(std::string repo, std::string message);

// The hunks of `path`'s diff: unstaged (workdir vs index) or staged (index vs HEAD).
std::expected<std::vector<hunk>, error>
file_diff(std::string repo, std::string path, bool staged);

// Stage just hunk `hunk_index` (0-based, as numbered by file_diff(.,.,false))
// of `path` into the index, leaving the file's other hunks unstaged.
std::expected<void, error>
stage_hunk(std::string repo, std::string path, std::size_t hunk_index);

// Unstage just hunk `hunk_index` (0-based, as numbered by file_diff(.,.,true))
// of `path`, returning that hunk to the working tree's unstaged changes.
std::expected<void, error>
unstage_hunk(std::string repo, std::string path, std::size_t hunk_index);

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

    std::vector<hunk> out;
    const size_t ndeltas = git_diff_num_deltas(diff.get());
    for (size_t di = 0; di < ndeltas; ++di) {
        git_patch *raw_patch = nullptr;
        if (git_patch_from_diff(&raw_patch, diff.get(), di) != 0)
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

} // namespace mg::git
