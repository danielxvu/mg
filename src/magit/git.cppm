// mg.git -- libgit2-backed working-tree status reader (task M2c-2).
//
// Reads structured status straight from the repository (no subprocess, no
// porcelain text). The libgit2 C handles are owned by RAII wrappers so they are
// freed on every path; errors surface as std::expected, never raw int codes.

module;
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <git2.h>

#include <spawn.h>    // posix_spawnp -- run the real `git` for hook/sign/auth ops
#include <sys/wait.h> // waitpid
#include <unistd.h>   // pipe, read, close

// The process environment, for posix_spawnp. Declared in the global module
// fragment so it attaches to the global module (not mg.git) and resolves to the
// executable's real `environ` -- a namespace-scoped extern would mangle wrong.
extern "C" char **environ;

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

// Run the real `git -C <repo> <args...>` and capture its combined stdout+stderr
// (FM-GIT-CLI-WRITES). Mutations that must honour hooks / signing / credentials
// go through this instead of libgit2, which skips all of them. argv is passed
// directly to posix_spawnp -- never a shell -- so repo/branch names with
// metacharacters are inert. stdout+stderr share one pipe (no two-stream
// deadlock; hook/error text comes back as one blob to surface on failure).
struct git_run {
    int code;          // process exit code (-1 if spawn/wait failed)
    std::string output; // combined stdout+stderr
};
inline git_run run_git(const std::string &repo,
                       const std::vector<std::string> &args)
{
    int pfd[2];
    if (::pipe(pfd) != 0)
        return {-1, "pipe() failed"};

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1); // child stdout -> pipe
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2); // child stderr -> pipe
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);

    std::vector<std::string> full{"git", "-C", repo};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(full.size() + 1);
    for (auto &s : full)
        argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "git", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pfd[1]); // parent only reads
    if (rc != 0) {
        ::close(pfd[0]);
        return {-1, "failed to run git"};
    }

    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pfd[0], buf, sizeof buf)) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pfd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(out)};
}

// Backs make_ignore_predicate: a long-lived repo handle + the workdir prefix,
// guarded by a mutex so the predicate is safe to call from the monitor's two
// threads (the initial walk runs on the caller's thread, later dynamic walks on
// the monitor thread -- sequential, but the lock supplies the barrier and
// satisfies libgit2's one-thread-at-a-time rule).
struct ignore_checker {
    init_guard guard;       // keep libgit2 alive for this handle's lifetime
    git_repository *repo = nullptr;
    std::string prefix;     // repo workdir, with trailing '/'
    std::mutex mu;
    ~ignore_checker()
    {
        if (repo != nullptr)
            git_repository_free(repo);
    }
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

    // A conflicted (unmerged) path: mark both columns so it surfaces as a
    // distinct Conflicts entry rather than vanishing.
    if (s & GIT_STATUS_CONFLICTED) {
        fs.index = status::unmerged;
        fs.worktree = status::unmerged;
    }

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

// Process-wide libgit2 lifetime control. Each engine call already holds a
// refcounted detail::init_guard, but when the refcount crosses 0<->1 libgit2
// sets up / tears down global state (notably OpenSSL on Linux). If two threads
// do that concurrently it is a data race (ThreadSanitizer-confirmed). Callers
// that run the engine on multiple threads should hold one global_init() for the
// whole lifetime -- before the threads start, released after they join -- so
// the refcount never returns to 0 mid-flight.
inline void global_init() noexcept { git_libgit2_init(); }
inline void global_shutdown() noexcept { git_libgit2_shutdown(); }

// Patch a running status set in place: drop every entry under one of `dirs`
// (workdir-relative directories) and append `scoped` -- the authoritative
// repo_status_scoped result for those same dirs. After patching, `base` equals
// a fresh full repo_status for the union of changes, so the incremental monitor
// never drifts from ground truth (pinned by the property test). Handles
// add/modify/delete/untracked/rename and collapsed untracked-dir entries (a
// "dir/" path is under "dir").
inline void apply_status_patch(std::vector<mg::magit::file_status> &base,
                               std::span<const mg::magit::file_status> scoped,
                               std::span<const std::string> dirs)
{
    auto under = [&](const std::string &p) {
        for (const auto &d : dirs)
            if (p == d || (p.size() > d.size() &&
                           p.compare(0, d.size(), d) == 0 && p[d.size()] == '/'))
                return true;
        return false;
    };
    std::erase_if(base, [&](const mg::magit::file_status &f) {
        return under(f.path);
    });
    base.insert(base.end(), scoped.begin(), scoped.end());
}

// A predicate `pred(absolute_dir)` -> true if that directory is gitignored and
// so should not be watched (gitignored content cannot change `git status`, and
// trees like node_modules/build dominate the watch set otherwise). `.git` and
// everything under it is never ignored -- the watcher needs it for staging /
// commit detection. Returns an empty function if the repo can't be opened (the
// caller then watches everything, the prior behaviour). Thread-safe.
inline std::function<bool(const std::string &)>
make_ignore_predicate(std::string repo_path)
{
    auto chk = std::make_shared<detail::ignore_checker>();
    if (git_repository_open(&chk->repo, repo_path.c_str()) != 0)
        return {};
    // Use the caller's spelling of the repo root as the prefix, NOT
    // git_repository_workdir() -- the latter canonicalizes symlinks (e.g. macOS
    // /var -> /private/var), which would mismatch the watcher's paths and make
    // the predicate silently no-op. The watcher's absolute paths are all built
    // from repo_path, so a plain prefix strip yields the workdir-relative path
    // git_ignore_path_is_ignored wants (it cares about the relative string, not
    // how the root is spelled).
    chk->prefix = repo_path;
    if (chk->prefix.empty())
        return {};
    if (chk->prefix.back() != '/')
        chk->prefix += '/';
    return [chk](const std::string &abs) -> bool {
        // Only worktree paths are checkable; anything not under the workdir
        // (notably the .git dir) is never gitignored from the watcher's view.
        if (abs.size() <= chk->prefix.size() ||
            abs.compare(0, chk->prefix.size(), chk->prefix) != 0)
            return false;
        std::string rel = abs.substr(chk->prefix.size());
        if (rel == ".git" || rel.starts_with(".git/"))
            return false; // git ignores .git itself; we must still watch it
        // The predicate is only ever asked about directories; a trailing slash
        // tells libgit2 so, so directory-only patterns ("node_modules/") match
        // (it does not stat the path to discover dir-ness itself).
        rel += '/';
        std::lock_guard lk(chk->mu);
        int ignored = 0;
        return git_ignore_path_is_ignored(&ignored, chk->repo, rel.c_str()) == 0 &&
               ignored != 0;
    };
}

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

struct worktree_entry {
    std::string name;
    std::string path;
};

struct submodule_entry {
    std::string name;
    std::string path;
};

// One unmerged path in the index (`has_ancestor` false for add/add conflicts).
struct conflict_entry {
    std::string path;
    bool has_ancestor;
};

// Which side to keep when resolving a conflict: `ours` is the current branch /
// rebase-onto (index stage 2); `theirs` is the merged-in / replayed commit
// (stage 3); `both` keeps ours then theirs (hunk-level only).
enum class conflict_side { ours, theirs, both };

// One conflict region parsed from a working-tree file's merge markers.
struct conflict_hunk {
    std::string ours;
    std::string theirs;
};

// One source line annotated with the commit that last touched it.
struct blame_line {
    std::string short_oid;
    std::string author;
    std::string text;
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

// One staged entry: a path changed between HEAD and the index (X column).
struct staged_entry {
    std::string path;
    mg::magit::status x;
};

// Index-vs-HEAD diff (staged changes, the "X" column). On an unborn HEAD (no
// commits yet), every index entry is staged-added. Only paths that actually
// differ are returned (UNMODIFIED paths are excluded).
std::expected<std::vector<staged_entry>, error>
staged_status(std::string repo);

// Like repo_status, but limited to entries matching `pathspecs` (workdir-
// relative dirs and/or files; git pathspec matching, so "src" covers src/**).
// Empty `pathspecs` == the whole repo (repo_status delegates here). Opens its
// own repo handle, so parallel callers can each scope a disjoint partition on a
// private thread; the union of a complete, disjoint partition equals the full
// repo_status. Powers both incremental (changed dirs) and parallel (partition)
// status -- see the FM-FSMONITOR-LITE spec.
std::expected<std::vector<mg::magit::file_status>, error>
repo_status_scoped(std::string path, std::span<const std::string> pathspecs);

// A reusable repository handle (FM-REPO-SESSION). The free functions above open
// + read the index + close on every call; a session opens once and keeps the
// handle, so repeated scoped queries skip the index re-read -- measured ~28x on
// the warm path (16.8ms -> 0.6ms). Use one PER THREAD, never shared (libgit2
// handles aren't concurrency-safe). The monitor holds one for its incremental
// status, re-opening on .git changes (fresh refs/index) and reusing across
// worktree edits (index unchanged -> warm + correct). PIMPL so the opaque
// libgit2 handle never crosses the module boundary.
class session {
public:
    static std::expected<session, error> open(std::string path);
    session(session &&) noexcept;
    session &operator=(session &&) noexcept;
    ~session();
    session(const session &) = delete;
    session &operator=(const session &) = delete;

    // Same results as repo_status / repo_status_scoped, on the held handle.
    std::expected<std::vector<mg::magit::file_status>, error> status();
    std::expected<std::vector<mg::magit::file_status>, error>
    status_scoped(std::span<const std::string> pathspecs);

private:
    struct impl;
    explicit session(std::unique_ptr<impl> p);
    std::unique_ptr<impl> p_;
};

std::expected<head_info, error> read_head(std::string path);

// The current branch's upstream tracking status (name + ahead/behind counts).
std::expected<upstream_info, error> upstream_status(std::string path);

// Commits that diverge from the upstream: unpushed (on HEAD, not upstream) when
// `unpushed` is true, else unpulled (on upstream, not HEAD). Empty if no
// upstream. Newest first.
std::expected<std::vector<commit_brief>, error>
upstream_commits(std::string path, bool unpushed);

// Commits in (onto, HEAD], oldest first -- the commits an interactive rebase
// onto `onto` would replay, in todo order.
std::expected<std::vector<commit_brief>, error>
commits_range(std::string path, std::string onto);

std::expected<std::vector<commit_brief>, error>
recent_commits(std::string path, std::size_t n);

// Up to `n` recent commits (newest first) that changed `file` -- a commit
// whose blob for `file` differs from its first parent's (or that introduces
// it). magit's "log of a file".
std::expected<std::vector<commit_brief>, error>
log_file(std::string repo, std::string file, std::size_t n);

// FM-LP: a row of `git log` output. A commit row carries its full oid; a pure
// graph-connector line (--graph) has oid == "" and is non-actionable.
struct log_row {
    std::string text;   // <graph art> + short_oid + " " + summary, or a connector line
    std::string oid;    // full 40-hex sha for a commit row; "" otherwise
};

// Options for log_query (the CLI-backed log reader used by l g / l r / l s / l G).
struct log_options {
    std::size_t max_count = 0;   // 0 = no -n limit
    bool        graph     = false;
    std::string range;           // "" = default (HEAD); else e.g. "main..HEAD"
    std::string file;            // "" = repo-wide; else restrict to a path
    char        pickaxe   = 0;   // 0 = none, 'S' = occurrence-count, 'G' = regex
    std::string pickaxe_term;
};

// Run `git log` with the requested options and parse it into rows. graph +
// pickaxe have no libgit2 equivalent, so this goes through the real git binary
// (detail::run_git, argv array -- no shell). A non-zero git exit -> error
// carrying git's output (e.g. a bad range).
std::expected<std::vector<log_row>, error>
log_query(std::string repo, log_options opts);

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

// Outcome of an apply-style operation (merge / revert / cherry-pick): it either
// finished, or left conflicts on disk (index + working-tree markers + the
// in-progress *_HEAD state) for the user to resolve and then commit.
enum class apply_result { done, conflicts };

// Revert commit `rev`, recording the inverse as a new commit on HEAD. On
// conflict, leaves the conflicted tree + REVERT_HEAD in place (resolve + commit).
std::expected<apply_result, error>
revert_commit(std::string repo, std::string rev);

// Merge local branch `name` into HEAD: fast-forward when possible, else a
// merge commit. On conflict, leaves the conflicted tree + MERGE_HEAD in place
// (resolve + commit makes the merge commit).
std::expected<apply_result, error>
merge_branch(std::string repo, std::string name);

// Outcome of a rebase step: finished, paused on a conflict (the on-disk rebase
// state is left for continue/skip/abort), or stopped at an interactive `edit`
// (the remaining plan is persisted; amend, then continue).
enum class rebase_result { done, conflicts, stopped };

// Cherry-pick commit `rev` onto HEAD as a new commit (keeping its author +
// message). On conflict, leaves the conflicted tree + CHERRY_PICK_HEAD in place
// (resolve + commit).
std::expected<apply_result, error>
cherry_pick(std::string repo, std::string rev);

// Rebase the current branch onto `upstream` (a branch name / revspec): replay
// HEAD's commits since the merge-base on top of upstream. Pauses (leaving the
// conflict in the tree + the rebase in progress) on the first conflict.
std::expected<rebase_result, error>
rebase_onto(std::string repo, std::string upstream);

// Continue / skip / abort a paused rebase. continue commits the (resolved)
// current operation then resumes; skip drops the current commit; abort restores
// the pre-rebase state. continue/skip may themselves pause on a later conflict.
std::expected<rebase_result, error> rebase_continue(std::string repo);
std::expected<rebase_result, error> rebase_skip(std::string repo);
std::expected<void, error> rebase_abort(std::string repo);

// Is a rebase currently in progress (paused) in `repo`?
bool rebase_in_progress(std::string repo);

// One entry of an interactive-rebase plan: what to do with a commit.
enum class rebase_action { pick, drop, squash, fixup, reword, edit };
struct rebase_step {
    rebase_action action;
    std::string oid;       // full sha-1 hex of the commit
    std::string message;   // reword: the new commit message (else ignored)
};

// Interactive rebase: replay `plan` (oldest first) on top of `onto`, then move
// the current branch to the result. pick = apply as-is; drop = omit;
// squash/fixup = fold into the previous kept commit (squash concatenates the
// messages, fixup keeps the previous message); reword = new message; edit =
// apply, then STOP with the commit checked out (amend, then rebase_continue);
// reordering is just the plan order. Built on cherry-pick (in-memory). Returns
// `done`, `stopped` (paused at an edit; remaining plan persisted), or an error
// (a cherry-pick conflict leaves the repo untouched).
std::expected<rebase_result, error>
rebase_interactive(std::string repo, std::string onto,
                   std::vector<rebase_step> plan);

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

// Push the current branch to `remote` (same-named ref). `force` uses a +refspec
// (force-push); `set_upstream` records remote/branch as the branch's upstream.
std::expected<void, error>
push_remote(std::string repo, std::string remote, bool force = false,
            bool set_upstream = false);

// Fetch `remote`, then merge the current branch's remote-tracking ref into HEAD
// (fast-forward or merge commit; conflicts abort). Magit's pull.
std::expected<apply_result, error>
pull_remote(std::string repo, std::string remote);

// Fetch `remote`, then rebase the current branch onto its remote-tracking ref
// (pull --rebase). May pause on conflict (like rebase_onto).
std::expected<rebase_result, error>
pull_rebase(std::string repo, std::string remote);

// FM-GIT-CLI-WRITES P3: run `git -C repo args...` *inheriting the current
// stdio* (the real terminal), for interactive network ops -- so the user's
// credential helper / SSH agent / GPG pinentry / progress meter all work, which
// the libgit2 path cannot do. The caller (UI thread) must put the tty in cooked
// mode around this. Returns git's exit code (0 = success), or -1 if `git` could
// not be executed at all -- the signal to fall back to the libgit2 remote path.
int git_terminal(std::string repo, std::vector<std::string> args);

// Create local branch `name` at HEAD (does not switch to it).
std::expected<void, error> create_branch(std::string repo, std::string name);

// Delete local branch `name`.
std::expected<void, error> delete_branch(std::string repo, std::string name);

// Rename local branch `from` to `to`.
std::expected<void, error>
rename_branch(std::string repo, std::string from, std::string to);

// Create tag `name` at `target` (a revspec, e.g. "HEAD"). An empty `message`
// makes a lightweight tag; otherwise an annotated tag with that message.
std::expected<void, error>
create_tag(std::string repo, std::string name, std::string target,
           std::string message = "");

// Delete tag `name`.
std::expected<void, error> delete_tag(std::string repo, std::string name);

// The repository's tag names (sorted by libgit2).
std::expected<std::vector<std::string>, error> tags(std::string repo);

// Append `pattern` as a line to the repository's top-level .gitignore.
std::expected<void, error> ignore_path(std::string repo, std::string pattern);

// Blame `path` (workdir version): one entry per line, in file order, naming the
// commit + author that last touched it.
std::expected<std::vector<blame_line>, error>
blame_file(std::string repo, std::string path);

// Bisect (binary search for the first bad commit). start records the bad/good
// bounds and checks out the midpoint; mark records the current commit good or
// bad and advances; reset returns to the starting branch. start/mark return a
// human-readable status ("Bisecting: N left, testing <oid>" or
// "<oid> is the first bad commit"). No libgit2 API -- implemented over refs.
std::expected<std::string, error>
bisect_start(std::string repo, std::string bad, std::string good);
std::expected<std::string, error> bisect_mark(std::string repo, bool is_bad);
std::expected<void, error> bisect_reset(std::string repo);
bool bisect_active(std::string repo);

// The repository's linked worktrees (name + absolute path).
std::expected<std::vector<worktree_entry>, error> worktrees(std::string repo);

// Add a worktree `name` checked out at `path` (creates a branch `name`).
std::expected<void, error>
add_worktree(std::string repo, std::string name, std::string path);

// Remove worktree `name`: prune its admin files and its working-tree dir.
std::expected<void, error> remove_worktree(std::string repo, std::string name);

// The repository's registered submodules (name + path), from .gitmodules.
std::expected<std::vector<submodule_entry>, error>
submodules(std::string repo);

// Unmerged paths in the index (empty when there is no conflict in progress).
std::expected<std::vector<conflict_entry>, error> conflicts(std::string repo);

// Resolve conflict `path` by keeping `side` (ours/theirs): write that stage's
// blob to the working tree and stage it (clearing the conflict). Errors if
// `path` is not conflicted or the chosen side is absent.
std::expected<void, error>
resolve_conflict(std::string repo, std::string path, conflict_side side);

// Parse the working-tree file `path`'s merge markers into conflict regions
// (in order). Empty when the file has no markers.
std::expected<std::vector<conflict_hunk>, error>
conflict_hunks(std::string repo, std::string path);

// Replace the `index`-th conflict region in `path` with `side` (ours / theirs /
// both), rewriting the working-tree file. Re-parses, so call with index 0..N as
// regions collapse. Errors if `index` is out of range.
std::expected<void, error>
resolve_conflict_hunk(std::string repo, std::string path, std::size_t index,
                      conflict_side side);

// Word-level refinement: return `text` with each word NOT shared with `other`
// (by a word-level LCS) wrapped in `open`..`close` -- the textual form of
// ediff's intra-line highlight. Whitespace + newlines are preserved. Returns
// `text` unchanged if either side is very large.
std::string refine_words(std::string text, std::string other, std::string open,
                         std::string close);

// Set / remove / read the (default refs/notes/commits) note on commit `rev`.
// set overwrites any existing note; read returns "" when there is none.
std::expected<void, error>
set_note(std::string repo, std::string rev, std::string message);
std::expected<void, error> remove_note(std::string repo, std::string rev);
std::expected<std::string, error> read_note(std::string repo, std::string rev);

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

// Defined below; used by create_tag (annotated) before its definition.
static detail::sig_ptr default_signature(git_repository *repo);

// Defined below (near commit); used by merge/cherry-pick/revert above it.
static std::expected<apply_result, error>
apply_via_cli(const std::string &repo, std::vector<std::string> args);

// The session holds the open handle + one libgit2 init for its lifetime (the
// guard outlives the handle: members destroy in reverse, so repo frees before
// shutdown).
struct session::impl {
    detail::init_guard guard;
    detail::repo_ptr repo;
};

session::session(std::unique_ptr<impl> p) : p_(std::move(p)) {}
session::session(session &&) noexcept = default;
session &session::operator=(session &&) noexcept = default;
session::~session() = default;

std::expected<session, error> session::open(std::string path)
{
    auto p = std::make_unique<impl>(); // default impl: inits libgit2, null repo
    git_repository *raw = nullptr;
    // flags = 0 makes open_ext walk up parent directories (the default), so
    // launching mg in any subdirectory of a repository still finds it.
    if (git_repository_open_ext(&raw, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    p->repo.reset(raw);
    return session(std::move(p));
}

std::expected<std::vector<mg::magit::file_status>, error>
session::status_scoped(std::span<const std::string> pathspecs)
{
    git_status_options opts;
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    // EXCLUDE_SUBMODULES: submodules have their own status section (a recursive
    // per-submodule status scan dominates the cost on submodule-heavy repos).
    // UPDATE_INDEX: persist the refreshed stat cache so repeat calls are faster.
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                 GIT_STATUS_OPT_EXCLUDE_SUBMODULES |
                 GIT_STATUS_OPT_UPDATE_INDEX;

    // Limit the scan to the given pathspecs (default git pathspec matching, so a
    // directory "src" covers src/**). Empty == whole repo. The pointers borrow
    // the caller's strings, which outlive this synchronous call; libgit2 does
    // not mutate them.
    std::vector<char *> specs;
    specs.reserve(pathspecs.size());
    for (const auto &s : pathspecs)
        specs.push_back(const_cast<char *>(s.c_str()));
    if (!specs.empty()) {
        opts.pathspec.strings = specs.data();
        opts.pathspec.count = specs.size();
    }

    git_status_list *raw_list = nullptr;
    if (git_status_list_new(&raw_list, p_->repo.get(), &opts) != 0)
        return std::unexpected(last_error());
    detail::status_list_ptr list(raw_list);

    std::vector<mg::magit::file_status> out;
    const size_t n = git_status_list_entrycount(list.get());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(detail::map_entry(git_status_byindex(list.get(), i)));
    return out;
}

std::expected<std::vector<mg::magit::file_status>, error> session::status()
{
    return status_scoped({});
}

// The free functions open a one-shot session and delegate -- the body lives in
// session::status_scoped, so there is one implementation.
std::expected<std::vector<mg::magit::file_status>, error>
repo_status_scoped(std::string path, std::span<const std::string> pathspecs)
{
    auto s = session::open(std::move(path));
    if (!s)
        return std::unexpected(s.error());
    return s->status_scoped(pathspecs);
}

std::expected<std::vector<mg::magit::file_status>, error>
repo_status(std::string path)
{
    return repo_status_scoped(std::move(path), {});
}

std::expected<std::vector<staged_entry>, error>
staged_status(std::string repo_path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, repo_path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    // Resolve HEAD to a tree; NULL tree if HEAD is unborn (no commits yet).
    git_tree *raw_tree = nullptr;
    git_reference *raw_head = nullptr;
    int head_rc = git_repository_head(&raw_head, repo.get());
    if (head_rc != GIT_EUNBORNBRANCH && head_rc != GIT_ENOTFOUND) {
        if (head_rc != 0)
            return std::unexpected(last_error());
        detail::ref_ptr head(raw_head);
        const git_oid *head_oid = git_reference_target(head.get());
        if (head_oid != nullptr) {
            git_commit *raw_commit = nullptr;
            if (git_commit_lookup(&raw_commit, repo.get(), head_oid) != 0)
                return std::unexpected(last_error());
            detail::commit_ptr commit(raw_commit);
            if (git_commit_tree(&raw_tree, commit.get()) != 0)
                return std::unexpected(last_error());
        }
    }
    detail::tree_ptr head_tree(raw_tree); // may be null for unborn HEAD

    // Get the repo index (read from disk).
    git_index *raw_index = nullptr;
    if (git_repository_index(&raw_index, repo.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr index(raw_index);

    // Diff HEAD tree (or NULL for unborn) vs index.
    git_diff_options diff_opts;
    git_diff_options_init(&diff_opts, GIT_DIFF_OPTIONS_VERSION);

    git_diff *raw_diff = nullptr;
    if (git_diff_tree_to_index(&raw_diff, repo.get(), head_tree.get(),
                               index.get(), &diff_opts) != 0)
        return std::unexpected(last_error());
    detail::diff_ptr diff(raw_diff);

    using mg::magit::status;
    std::vector<staged_entry> out;
    const std::size_t n = git_diff_num_deltas(diff.get());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const git_diff_delta *delta = git_diff_get_delta(diff.get(), i);
        if (delta == nullptr)
            continue;
        staged_entry e;
        switch (delta->status) {
        case GIT_DELTA_ADDED:
            e.x = status::added;
            e.path = delta->new_file.path ? delta->new_file.path : "";
            break;
        case GIT_DELTA_DELETED:
            e.x = status::deleted;
            e.path = delta->old_file.path ? delta->old_file.path : "";
            break;
        case GIT_DELTA_MODIFIED:
        case GIT_DELTA_TYPECHANGE:
            e.x = status::modified;
            e.path = delta->new_file.path ? delta->new_file.path : "";
            break;
        case GIT_DELTA_RENAMED:
            e.x = status::renamed;
            e.path = delta->new_file.path ? delta->new_file.path : "";
            break;
        case GIT_DELTA_COPIED:
            e.x = status::copied;
            e.path = delta->new_file.path ? delta->new_file.path : "";
            break;
        default:
            continue; // skip UNMODIFIED, IGNORED, UNTRACKED, etc.
        }
        if (!e.path.empty())
            out.push_back(std::move(e));
    }
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
commits_range(std::string path, std::string onto)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    std::vector<commit_brief> out;

    git_object *raw_onto = nullptr;
    if (git_revparse_single(&raw_onto, repo.get(), onto.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr onto_obj(raw_onto);

    git_revwalk *raw_walk = nullptr;
    if (git_revwalk_new(&raw_walk, repo.get()) != 0)
        return std::unexpected(last_error());
    detail::revwalk_ptr walk(raw_walk);
    // Topological + reverse -> oldest first, which is rebase todo order.
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
    if (git_revwalk_push_head(walk.get()) != 0)
        return out;
    git_revwalk_hide(walk.get(), git_object_id(onto_obj.get()));

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

    // GIT_SORT_TIME/TOPOLOGICAL force libgit2 to pre-walk the whole reachable
    // history to guarantee order -- ~300ms on a 60k-commit repo with no
    // commit-graph. A lazy (unsorted) walk stops after we have enough, then we
    // sort the collected commits by committer time ourselves: ~200x faster and
    // newest-first. We over-collect a margin past `n` so a branchy frontier
    // still yields the true most-recent `n` (exact for linear history).
    git_revwalk_sorting(walk.get(), GIT_SORT_NONE);
    if (git_revwalk_push_head(walk.get()) != 0)
        return out; // unborn / no HEAD -> no commits

    const std::size_t cap = n + 256;
    std::vector<std::pair<commit_brief, git_time_t>> tmp;
    git_oid oid;
    while (tmp.size() < cap && git_revwalk_next(&oid, walk.get()) == 0) {
        commit_brief cb;
        cb.short_oid = detail::short_oid(&oid);
        cb.oid = detail::full_oid(&oid);
        git_time_t t = 0;
        git_commit *raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), &oid) == 0) {
            detail::commit_ptr commit(raw_commit);
            if (const char *s = git_commit_summary(commit.get()))
                cb.summary = s;
            t = git_commit_time(commit.get());
        }
        tmp.emplace_back(std::move(cb), t);
    }
    // Newest first; stable so same-timestamp commits keep walk (ancestry) order.
    std::stable_sort(tmp.begin(), tmp.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
    if (tmp.size() > n)
        tmp.resize(n);
    out.reserve(tmp.size());
    for (auto &e : tmp)
        out.push_back(std::move(e.first));
    return out;
}

std::expected<std::vector<commit_brief>, error>
log_file(std::string path, std::string file, std::size_t n)
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
    // Topological keeps a child strictly before its parents (newest-first for a
    // linear history) even when commits share a timestamp.
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    if (git_revwalk_push_head(walk.get()) != 0)
        return out; // unborn / no HEAD -> no commits

    // The blob oid for `file` in `commit`'s tree, or nullopt if absent.
    auto blob_at = [&](git_commit *commit) -> std::optional<git_oid> {
        git_tree *raw_tree = nullptr;
        if (git_commit_tree(&raw_tree, commit) != 0)
            return std::nullopt;
        detail::tree_ptr tree(raw_tree);
        git_tree_entry *raw_entry = nullptr;
        if (git_tree_entry_bypath(&raw_entry, tree.get(), file.c_str()) != 0)
            return std::nullopt;
        std::unique_ptr<git_tree_entry, decltype(&git_tree_entry_free)> entry(
            raw_entry, git_tree_entry_free);
        return *git_tree_entry_id(entry.get());
    };

    git_oid oid;
    while (out.size() < n && git_revwalk_next(&oid, walk.get()) == 0) {
        git_commit *raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), &oid) != 0)
            continue;
        detail::commit_ptr commit(raw_commit);
        auto here = blob_at(commit.get());

        // Touched iff `file`'s blob differs from the first parent's (or this is
        // a root commit that introduces the file).
        bool touched;
        if (git_commit_parentcount(commit.get()) == 0) {
            touched = here.has_value();
        } else {
            git_commit *raw_parent = nullptr;
            std::optional<git_oid> there;
            if (git_commit_parent(&raw_parent, commit.get(), 0) == 0) {
                detail::commit_ptr parent(raw_parent);
                there = blob_at(parent.get());
            }
            touched = here.has_value() != there.has_value() ||
                      (here && there && !git_oid_equal(&*here, &*there));
        }
        if (!touched)
            continue;

        commit_brief cb;
        cb.short_oid = detail::short_oid(&oid);
        cb.oid = detail::full_oid(&oid);
        if (const char *s = git_commit_summary(commit.get()))
            cb.summary = s;
        out.push_back(std::move(cb));
    }
    return out;
}

std::expected<std::vector<log_row>, error>
log_query(std::string repo, log_options opts)
{
    std::vector<std::string> args{"log"};
    if (opts.graph)
        args.emplace_back("--graph");
    if (opts.max_count > 0) {
        args.emplace_back("-n");
        args.emplace_back(std::to_string(opts.max_count));
    }
    if (opts.pickaxe == 'S')
        args.emplace_back("-S" + opts.pickaxe_term);
    else if (opts.pickaxe == 'G')
        args.emplace_back("-G" + opts.pickaxe_term);
    // Leading %x1f so --graph's art lands in field[0] and the same parser
    // handles graph + non-graph lines uniformly.
    args.emplace_back("--format=%x1f%H%x1f%h%x1f%s");
    if (!opts.range.empty())
        args.emplace_back(opts.range);
    if (!opts.file.empty()) {
        args.emplace_back("--");
        args.emplace_back(opts.file);
    }

    auto run = detail::run_git(repo, args);
    if (run.code != 0) {
        std::string msg = run.output.empty() ? "git log failed" : run.output;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        return std::unexpected(error{0, std::move(msg)});
    }

    std::vector<log_row> rows;
    const std::string &out = run.output;
    std::size_t start = 0;
    while (start <= out.size()) {
        std::size_t nl = out.find('\n', start);
        std::string line =
            out.substr(start, nl == std::string::npos ? std::string::npos
                                                      : nl - start);
        if (nl == std::string::npos) {
            if (line.empty())
                break;
        }
        // Split on the US (0x1f) separator.
        std::vector<std::string> f;
        std::size_t p = 0;
        for (;;) {
            std::size_t s = line.find('\x1f', p);
            if (s == std::string::npos) {
                f.push_back(line.substr(p));
                break;
            }
            f.push_back(line.substr(p, s - p));
            p = s + 1;
        }
        if (f.size() >= 4) {
            // f[0]=graph art, f[1]=full, f[2]=short, f[3]=summary
            log_row row;
            row.text = f[0] + f[2] + " " + f[3];
            row.oid = f[1];
            rows.push_back(std::move(row));
        } else if (!line.empty()) {
            rows.push_back(log_row{line, ""}); // connector-only line
        }
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return rows;
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

std::expected<void, error>
create_tag(std::string repo, std::string name, std::string target,
           std::string message)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_object *raw_obj = nullptr;
    if (git_revparse_single(&raw_obj, r.get(), target.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw_obj);

    git_oid oid;
    if (message.empty()) {
        if (git_tag_create_lightweight(&oid, r.get(), name.c_str(), obj.get(),
                                       /*force=*/0) != 0)
            return std::unexpected(last_error());
    } else {
        detail::sig_ptr sig = default_signature(r.get());
        if (!sig)
            return std::unexpected(last_error());
        if (git_tag_create(&oid, r.get(), name.c_str(), obj.get(), sig.get(),
                           message.c_str(), /*force=*/0) != 0)
            return std::unexpected(last_error());
    }
    return {};
}

std::expected<void, error> delete_tag(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    if (git_tag_delete(r.get(), name.c_str()) != 0)
        return std::unexpected(last_error());
    return {};
}

// Resolve a revspec to a commit oid (helper for the note ops).
static std::expected<git_oid, error>
resolve_oid(git_repository *repo, const std::string &rev)
{
    git_object *raw = nullptr;
    if (git_revparse_single(&raw, repo, rev.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw);
    return *git_object_id(obj.get());
}

std::expected<void, error>
set_note(std::string repo, std::string rev, std::string message)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    auto oid = resolve_oid(r.get(), rev);
    if (!oid)
        return std::unexpected(oid.error());
    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());
    git_oid note_oid;
    if (git_note_create(&note_oid, r.get(), nullptr, sig.get(), sig.get(),
                        &*oid, message.c_str(), /*force=*/1) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<void, error> remove_note(std::string repo, std::string rev)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    auto oid = resolve_oid(r.get(), rev);
    if (!oid)
        return std::unexpected(oid.error());
    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());
    if (git_note_remove(r.get(), nullptr, sig.get(), sig.get(), &*oid) != 0)
        return std::unexpected(last_error());
    return {};
}

std::expected<std::string, error> read_note(std::string repo, std::string rev)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    auto oid = resolve_oid(r.get(), rev);
    if (!oid)
        return std::unexpected(oid.error());
    git_note *raw_note = nullptr;
    if (git_note_read(&raw_note, r.get(), nullptr, &*oid) != 0)
        return std::string(); // no note (or no notes ref) -> empty
    std::unique_ptr<git_note, decltype(&git_note_free)> note(raw_note,
                                                             git_note_free);
    const char *m = git_note_message(note.get());
    return std::string(m != nullptr ? m : "");
}

std::expected<std::vector<worktree_entry>, error> worktrees(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_strarray names = {nullptr, 0};
    if (git_worktree_list(&names, r.get()) != 0)
        return std::unexpected(last_error());
    std::vector<worktree_entry> out;
    for (std::size_t i = 0; i < names.count; ++i) {
        worktree_entry e{names.strings[i], ""};
        git_worktree *raw_wt = nullptr;
        if (git_worktree_lookup(&raw_wt, r.get(), names.strings[i]) == 0) {
            std::unique_ptr<git_worktree, decltype(&git_worktree_free)> wt(
                raw_wt, git_worktree_free);
            if (const char *p = git_worktree_path(wt.get()))
                e.path = p;
        }
        out.push_back(std::move(e));
    }
    git_strarray_dispose(&names);
    return out;
}

std::expected<std::vector<submodule_entry>, error>
submodules(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Fast path: no .gitmodules -> no configured submodules. Avoids
    // git_submodule_foreach scanning the whole index (~30ms on a 37k-file
    // repo) every status refresh on the common no-submodule case.
    if (const char *wd = git_repository_workdir(r.get());
        wd != nullptr &&
        !std::filesystem::exists(std::filesystem::path(wd) / ".gitmodules"))
        return std::vector<submodule_entry>{};

    std::vector<submodule_entry> out;
    auto cb = [](git_submodule *sm, const char *name, void *payload) -> int {
        auto *v = static_cast<std::vector<submodule_entry> *>(payload);
        const char *p = git_submodule_path(sm);
        v->push_back({name ? name : "", p ? p : ""});
        return 0;
    };
    if (git_submodule_foreach(r.get(), cb, &out) != 0)
        return std::unexpected(last_error());
    return out;
}

std::expected<std::vector<conflict_entry>, error> conflicts(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Unmerged entries only exist while a merge/rebase/cherry-pick/revert is in
    // progress (which sets repository state). When the state is clean -- the
    // common case -- skip the index conflict scan (~16ms on a 37k-file index).
    if (git_repository_state(r.get()) == GIT_REPOSITORY_STATE_NONE)
        return std::vector<conflict_entry>{};

    git_index *raw_idx = nullptr;
    if (git_repository_index(&raw_idx, r.get()) != 0)
        return std::unexpected(last_error());
    detail::index_ptr idx(raw_idx);

    git_index_conflict_iterator *raw_it = nullptr;
    if (git_index_conflict_iterator_new(&raw_it, idx.get()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_index_conflict_iterator,
                    decltype(&git_index_conflict_iterator_free)>
        it(raw_it, git_index_conflict_iterator_free);

    std::vector<conflict_entry> out;
    const git_index_entry *anc = nullptr, *our = nullptr, *their = nullptr;
    int rc;
    while ((rc = git_index_conflict_next(&anc, &our, &their, it.get())) == 0) {
        const char *p = their ? their->path
                        : our  ? our->path
                        : anc  ? anc->path
                               : nullptr;
        out.push_back({p ? p : "", anc != nullptr});
    }
    if (rc != GIT_ITEROVER)
        return std::unexpected(last_error());
    return out;
}

std::expected<void, error>
resolve_conflict(std::string repo, std::string path, conflict_side side)
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

    const git_index_entry *anc = nullptr, *our = nullptr, *their = nullptr;
    if (git_index_conflict_get(&anc, &our, &their, idx.get(), path.c_str()) != 0)
        return std::unexpected(last_error()); // not a conflicted path
    const git_index_entry *chosen = side == conflict_side::ours ? our : their;
    if (chosen == nullptr)
        return std::unexpected(error{0, "chosen side absent in conflict"});

    git_blob *raw_blob = nullptr;
    if (git_blob_lookup(&raw_blob, r.get(), &chosen->id) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_blob, decltype(&git_blob_free)> blob(raw_blob,
                                                             git_blob_free);

    const char *wd = git_repository_workdir(r.get());
    if (wd == nullptr)
        return std::unexpected(error{0, "bare repository has no workdir"});
    std::ofstream out(std::filesystem::path(wd) / path, std::ios::binary);
    if (!out)
        return std::unexpected(error{0, "cannot write " + path});
    out.write(static_cast<const char *>(git_blob_rawcontent(blob.get())),
              static_cast<std::streamsize>(git_blob_rawsize(blob.get())));
    out.close();

    // add_bypath stages the resolved file and clears the conflict stages.
    if (git_index_add_bypath(idx.get(), path.c_str()) != 0)
        return std::unexpected(last_error());
    if (git_index_write(idx.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

namespace {
// One parsed conflict region: its [begin,end) line range and each side's lines.
struct hunk_span {
    std::size_t begin, end; // covers the <<< .. >>> block, end exclusive
    std::vector<std::string> ours, theirs;
};

std::vector<std::string> read_lines(const std::filesystem::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    return lines;
}

// Parse merge-marker regions from `lines` (2-way and diff3; base is dropped).
std::vector<hunk_span> parse_conflicts(const std::vector<std::string> &lines)
{
    std::vector<hunk_span> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("<<<<<<<", 0) != 0)
            continue;
        hunk_span h{i, i, {}, {}};
        bool in_theirs = false, in_base = false;
        std::size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            const std::string &l = lines[j];
            if (l.rfind(">>>>>>>", 0) == 0)
                break;
            if (l.rfind("|||||||", 0) == 0) {
                in_base = true;
                continue;
            }
            if (l.rfind("=======", 0) == 0) {
                in_base = false;
                in_theirs = true;
                continue;
            }
            if (in_base)
                continue; // diff3 base: drop
            (in_theirs ? h.theirs : h.ours).push_back(l);
        }
        h.end = (j < lines.size()) ? j + 1 : j; // include the >>> line
        out.push_back(std::move(h));
        i = j;
    }
    return out;
}

std::string join_lines(const std::vector<std::string> &lines)
{
    std::string s;
    for (const auto &l : lines)
        s += l + "\n";
    return s;
}
} // namespace

std::expected<std::vector<conflict_hunk>, error>
conflict_hunks(std::string repo, std::string path)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    const char *wd = git_repository_workdir(r.get());
    if (wd == nullptr)
        return std::unexpected(error{0, "bare repository has no workdir"});

    std::vector<conflict_hunk> out;
    for (const auto &h :
         parse_conflicts(read_lines(std::filesystem::path(wd) / path)))
        out.push_back({join_lines(h.ours), join_lines(h.theirs)});
    return out;
}

namespace {
// Split `s` into (token, is_word) pairs: a word is a maximal non-space run, a
// separator a maximal space/tab/newline run. Reassembling the tokens == `s`.
std::vector<std::pair<std::string, bool>> tokenize(const std::string &s)
{
    std::vector<std::pair<std::string, bool>> toks;
    std::size_t i = 0;
    while (i < s.size()) {
        bool word = !std::isspace((unsigned char)s[i]);
        std::size_t j = i;
        while (j < s.size() && (!std::isspace((unsigned char)s[j])) == word)
            ++j;
        toks.emplace_back(s.substr(i, j - i), word);
        i = j;
    }
    return toks;
}
} // namespace

std::string refine_words(std::string text, std::string other, std::string open,
                         std::string close)
{
    auto toks = tokenize(text);
    std::vector<std::string> tw, ow;
    for (const auto &t : toks)
        if (t.second)
            tw.push_back(t.first);
    for (const auto &t : tokenize(other))
        if (t.second)
            ow.push_back(t.first);

    const std::size_t n = tw.size(), m = ow.size();
    if (n == 0 || n * m > 4'000'000u) // nothing to do / too large
        return text;

    // LCS over word sequences; backtrack to mark text words that are shared.
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j)
            dp[i][j] = tw[i - 1] == ow[j - 1]
                           ? dp[i - 1][j - 1] + 1
                           : std::max(dp[i - 1][j], dp[i][j - 1]);
    std::vector<bool> shared(n, false);
    for (std::size_t i = n, j = m; i > 0 && j > 0;) {
        if (tw[i - 1] == ow[j - 1]) {
            shared[i - 1] = true;
            --i;
            --j;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }

    std::string outs;
    std::size_t w = 0;
    for (const auto &t : toks) {
        if (t.second && !shared[w])
            outs += open + t.first + close;
        else
            outs += t.first;
        if (t.second)
            ++w;
    }
    return outs;
}

std::expected<void, error>
resolve_conflict_hunk(std::string repo, std::string path, std::size_t index,
                      conflict_side side)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);
    const char *wd = git_repository_workdir(r.get());
    if (wd == nullptr)
        return std::unexpected(error{0, "bare repository has no workdir"});
    std::filesystem::path file = std::filesystem::path(wd) / path;

    std::vector<std::string> lines = read_lines(file);
    std::vector<hunk_span> hunks = parse_conflicts(lines);
    if (index >= hunks.size())
        return std::unexpected(error{0, "conflict hunk index out of range"});
    const hunk_span &h = hunks[index];

    std::vector<std::string> chosen;
    if (side == conflict_side::ours || side == conflict_side::both)
        chosen.insert(chosen.end(), h.ours.begin(), h.ours.end());
    if (side == conflict_side::theirs || side == conflict_side::both)
        chosen.insert(chosen.end(), h.theirs.begin(), h.theirs.end());

    std::vector<std::string> result(lines.begin(),
                                    lines.begin() + (std::ptrdiff_t)h.begin);
    result.insert(result.end(), chosen.begin(), chosen.end());
    result.insert(result.end(), lines.begin() + (std::ptrdiff_t)h.end,
                  lines.end());

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected(error{0, "cannot write " + path});
    out << join_lines(result);
    return {};
}

std::expected<void, error>
add_worktree(std::string repo, std::string name, std::string path)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_worktree_add_options opts;
    git_worktree_add_options_init(&opts, GIT_WORKTREE_ADD_OPTIONS_VERSION);
    git_worktree *raw_wt = nullptr;
    if (git_worktree_add(&raw_wt, r.get(), name.c_str(), path.c_str(), &opts) != 0)
        return std::unexpected(last_error());
    git_worktree_free(raw_wt);
    return {};
}

std::expected<void, error> remove_worktree(std::string repo, std::string name)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_worktree *raw_wt = nullptr;
    if (git_worktree_lookup(&raw_wt, r.get(), name.c_str()) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_worktree, decltype(&git_worktree_free)> wt(
        raw_wt, git_worktree_free);

    // Remove the working-tree directory first, then prune the admin files.
    if (const char *p = git_worktree_path(wt.get())) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(p), ec);
    }
    git_worktree_prune_options popts;
    git_worktree_prune_options_init(&popts, GIT_WORKTREE_PRUNE_OPTIONS_VERSION);
    popts.flags = GIT_WORKTREE_PRUNE_VALID | GIT_WORKTREE_PRUNE_WORKING_TREE;
    if (git_worktree_prune(wt.get(), &popts) != 0)
        return std::unexpected(last_error());
    return {};
}

// ---- bisect (no libgit2 API; state in refs/bisect/* + a start-branch file) --
namespace {
const char *const BISECT_BAD = "refs/bisect/bad";
const char *const BISECT_GOOD = "refs/bisect/good";

std::filesystem::path bisect_start_file(git_repository *repo)
{
    return std::filesystem::path(git_repository_path(repo)) / "MG_BISECT_START";
}

// Detached-checkout `oid`; return its short oid.
std::expected<std::string, error>
bisect_checkout(git_repository *repo, const git_oid *oid)
{
    git_object *raw = nullptr;
    if (git_object_lookup(&raw, repo, oid, GIT_OBJECT_COMMIT) != 0)
        return std::unexpected(last_error());
    detail::object_ptr obj(raw);
    git_checkout_options chk;
    git_checkout_options_init(&chk, GIT_CHECKOUT_OPTIONS_VERSION);
    chk.checkout_strategy = GIT_CHECKOUT_SAFE;
    if (git_checkout_tree(repo, obj.get(), &chk) != 0)
        return std::unexpected(last_error());
    if (git_repository_set_head_detached(repo, oid) != 0)
        return std::unexpected(last_error());
    return detail::short_oid(oid);
}

// From the current bad/good refs: checkout the midpoint of (good, bad], or
// report the culprit when one suspect remains.
std::expected<std::string, error> bisect_step(git_repository *repo)
{
    git_oid bad;
    if (git_reference_name_to_id(&bad, repo, BISECT_BAD) != 0)
        return std::unexpected(last_error());
    git_oid good;
    const bool have_good =
        git_reference_name_to_id(&good, repo, BISECT_GOOD) == 0;

    git_revwalk *raw_walk = nullptr;
    if (git_revwalk_new(&raw_walk, repo) != 0)
        return std::unexpected(last_error());
    detail::revwalk_ptr walk(raw_walk);
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL);
    git_revwalk_push(walk.get(), &bad);
    if (have_good)
        git_revwalk_hide(walk.get(), &good);

    std::vector<git_oid> suspects;
    git_oid o;
    while (git_revwalk_next(&o, walk.get()) == 0)
        suspects.push_back(o);

    if (suspects.size() <= 1) // only `bad` remains -> it is the culprit
        return std::string(detail::full_oid(&bad)) + " is the first bad commit";

    auto sh = bisect_checkout(repo, &suspects[suspects.size() / 2]);
    if (!sh)
        return std::unexpected(sh.error());
    return "Bisecting: " + std::to_string(suspects.size() - 1) +
           " revisions left, testing " + *sh;
}
} // namespace

std::expected<std::string, error>
bisect_start(std::string repo, std::string bad, std::string good)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    auto bad_oid = resolve_oid(r.get(), bad);
    if (!bad_oid)
        return std::unexpected(bad_oid.error());
    auto good_oid = resolve_oid(r.get(), good);
    if (!good_oid)
        return std::unexpected(good_oid.error());

    // Remember the starting branch so reset can return to it.
    git_reference *raw_head = nullptr;
    if (git_repository_head(&raw_head, r.get()) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);
    const char *branch = git_reference_shorthand(head.get());
    std::ofstream(bisect_start_file(r.get())) << (branch ? branch : "");

    git_reference *tmp = nullptr;
    if (git_reference_create(&tmp, r.get(), BISECT_BAD, &*bad_oid, 1,
                             "bisect bad") != 0)
        return std::unexpected(last_error());
    git_reference_free(tmp);
    if (git_reference_create(&tmp, r.get(), BISECT_GOOD, &*good_oid, 1,
                             "bisect good") != 0)
        return std::unexpected(last_error());
    git_reference_free(tmp);

    return bisect_step(r.get());
}

std::expected<std::string, error> bisect_mark(std::string repo, bool is_bad)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") != 0)
        return std::unexpected(last_error());
    git_reference *tmp = nullptr;
    if (git_reference_create(&tmp, r.get(), is_bad ? BISECT_BAD : BISECT_GOOD,
                             &head_oid, 1, "bisect mark") != 0)
        return std::unexpected(last_error());
    git_reference_free(tmp);
    return bisect_step(r.get());
}

std::expected<void, error> bisect_reset(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    std::string branch;
    {
        std::ifstream in(bisect_start_file(r.get()));
        std::getline(in, branch);
    }
    git_reference_remove(r.get(), BISECT_BAD);
    git_reference_remove(r.get(), BISECT_GOOD);
    std::error_code ec;
    std::filesystem::remove(bisect_start_file(r.get()), ec);

    if (!branch.empty())
        return checkout_branch(repo, branch);
    return {};
}

bool bisect_active(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return false;
    detail::repo_ptr r(raw);
    git_oid oid;
    return git_reference_name_to_id(&oid, r.get(), BISECT_BAD) == 0;
}

std::expected<std::vector<blame_line>, error>
blame_file(std::string repo, std::string path)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_blame_options opts;
    git_blame_options_init(&opts, GIT_BLAME_OPTIONS_VERSION);
    git_blame *raw_blame = nullptr;
    if (git_blame_file(&raw_blame, r.get(), path.c_str(), &opts) != 0)
        return std::unexpected(last_error());
    std::unique_ptr<git_blame, decltype(&git_blame_free)> blame(raw_blame,
                                                                git_blame_free);

    // Read the working-tree file so we can pair each line with its hunk.
    const char *wd = git_repository_workdir(r.get());
    if (wd == nullptr)
        return std::unexpected(error{0, "no work tree"});
    std::ifstream f(std::filesystem::path(wd) / path);
    if (!f)
        return std::unexpected(error{0, "cannot read file"});

    std::vector<blame_line> out;
    std::string text;
    std::size_t lineno = 1;
    while (std::getline(f, text)) {
        blame_line bl;
        bl.text = text;
        const git_blame_hunk *h =
            git_blame_get_hunk_byline(blame.get(), lineno);
        if (h != nullptr) {
            bl.short_oid = detail::short_oid(&h->final_commit_id);
            if (h->final_signature != nullptr && h->final_signature->name)
                bl.author = h->final_signature->name;
        }
        out.push_back(std::move(bl));
        ++lineno;
    }
    return out;
}

std::expected<void, error> ignore_path(std::string repo, std::string pattern)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    const char *wd = git_repository_workdir(r.get());
    if (wd == nullptr)
        return std::unexpected(error{0, "no work tree"});

    std::ofstream out(std::filesystem::path(wd) / ".gitignore",
                      std::ios::app);
    if (!out)
        return std::unexpected(error{0, "cannot open .gitignore"});
    out << pattern << '\n';
    if (!out)
        return std::unexpected(error{0, "write to .gitignore failed"});
    return {};
}

std::expected<std::vector<std::string>, error> tags(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_strarray arr = {nullptr, 0};
    if (git_tag_list(&arr, r.get()) != 0)
        return std::unexpected(last_error());
    std::vector<std::string> out;
    out.reserve(arr.count);
    for (std::size_t i = 0; i < arr.count; ++i)
        out.emplace_back(arr.strings[i]);
    git_strarray_dispose(&arr);
    return out;
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

std::expected<apply_result, error>
revert_commit(std::string repo, std::string rev)
{
    // Through real git so hooks fire + the revert commit is signed. --no-edit
    // takes git's default `Revert "<summary>"` message non-interactively; a
    // conflicting revert is left in progress (REVERT_HEAD + markers).
    return apply_via_cli(repo, {"revert", "--no-edit", std::move(rev)});
}

// Merge an already-resolved annotated commit into HEAD: up-to-date (noop) /
// fast-forward (checkout + move ref) / true merge (write a 2-parent `msg`
// commit). On conflict, leaves the conflicted index + working tree + MERGE_HEAD
// in place and returns `conflicts` (resolve + commit completes the merge).
static std::expected<apply_result, error>
merge_annotated(git_repository *repo, git_annotated_commit *their,
                const std::string &msg)
{
    const git_annotated_commit *heads[1] = {their};
    git_merge_analysis_t analysis;
    git_merge_preference_t pref;
    if (git_merge_analysis(&analysis, &pref, repo, heads, 1) != 0)
        return std::unexpected(last_error());

    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)
        return apply_result::done; // already contains their commit

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
        return apply_result::done;
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
    if (git_index_has_conflicts(idx.get()))
        // git_merge already wrote the conflicted index + working tree +
        // MERGE_HEAD; leave it for the user to resolve and commit.
        return apply_result::conflicts;

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
    return apply_result::done;
}

std::expected<apply_result, error>
merge_branch(std::string repo, std::string name)
{
    // Through real git so a merge commit fires hooks + is signed; git also
    // picks fast-forward vs. true merge and writes the default
    // `Merge branch '<name>'` message. --no-edit keeps it non-interactive; a
    // conflicting merge is left in progress (MERGE_HEAD + markers). A bad branch
    // name exits non-zero with no conflict -> a real error.
    return apply_via_cli(repo, {"merge", "--no-edit", std::move(name)});
}

using rebase_ptr =
    std::unique_ptr<git_rebase, decltype(&git_rebase_free)>;

// Drive a rebase from its current position: optionally commit the current
// (resolved) operation first, then apply/commit each remaining operation.
// Pauses (rebase_result::conflicts, on-disk state retained) at the first
// operation whose merge leaves conflicts; otherwise finishes.
static std::expected<rebase_result, error>
rebase_drive(git_repository *repo, git_rebase *rebase, git_signature *sig,
             bool commit_current)
{
    if (commit_current) {
        git_oid id;
        int crc = git_rebase_commit(&id, rebase, nullptr, sig, nullptr, nullptr);
        if (crc != 0 && crc != GIT_EAPPLIED)
            return std::unexpected(last_error());
    }

    git_rebase_operation *op = nullptr;
    int rc;
    while ((rc = git_rebase_next(&op, rebase)) == 0) {
        git_index *raw_idx = nullptr;
        if (git_repository_index(&raw_idx, repo) != 0)
            return std::unexpected(last_error());
        detail::index_ptr idx(raw_idx);
        if (git_index_has_conflicts(idx.get()))
            return rebase_result::conflicts; // pause; state stays on disk

        git_oid id;
        int crc = git_rebase_commit(&id, rebase, nullptr, sig, nullptr, nullptr);
        if (crc == GIT_EAPPLIED)
            continue; // already present upstream -> dropped
        if (crc != 0)
            return std::unexpected(last_error());
    }
    if (rc != GIT_ITEROVER)
        return std::unexpected(last_error());
    if (git_rebase_finish(rebase, sig) != 0)
        return std::unexpected(last_error());
    return rebase_result::done;
}

std::expected<apply_result, error>
cherry_pick(std::string repo, std::string rev)
{
    // Through real git so the picked commit fires hooks + is signed; git keeps
    // the original author + message (as the libgit2 path did). A conflicting
    // pick is left in progress (CHERRY_PICK_HEAD + markers); a bad rev exits
    // non-zero with no conflict -> a real error.
    return apply_via_cli(repo, {"cherry-pick", std::move(rev)});
}

std::expected<rebase_result, error>
rebase_onto(std::string repo, std::string upstream)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

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
    if (git_rebase_init(&raw_rebase, r.get(), nullptr, up.get(), nullptr,
                        &ropts) != 0)
        return std::unexpected(last_error());
    rebase_ptr rebase(raw_rebase, git_rebase_free);

    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());
    return rebase_drive(r.get(), rebase.get(), sig.get(), /*commit_current=*/false);
}

// ---- interactive-rebase `edit`: persist the remaining plan across a stop ----
namespace {
std::filesystem::path rebase_todo_file(git_repository *repo)
{
    return std::filesystem::path(git_repository_path(repo)) / "MG_REBASE_TODO";
}

char action_to_char(rebase_action a)
{
    switch (a) {
    case rebase_action::drop:   return 'd';
    case rebase_action::squash: return 's';
    case rebase_action::fixup:  return 'f';
    case rebase_action::reword: return 'w';
    case rebase_action::edit:   return 'e';
    default:                    return 'p';
    }
}

rebase_action action_from_char(char c)
{
    switch (c) {
    case 'd': return rebase_action::drop;
    case 's': return rebase_action::squash;
    case 'f': return rebase_action::fixup;
    case 'w': return rebase_action::reword;
    case 'e': return rebase_action::edit;
    default:  return rebase_action::pick;
    }
}

// Persist {orig_head, remaining steps}; one step per line "<char> <oid> <msg>".
void write_rebase_todo(git_repository *repo, const std::string &orig_head,
                       const std::vector<rebase_step> &rest)
{
    std::ofstream out(rebase_todo_file(repo), std::ios::trunc);
    out << orig_head << "\n";
    for (const auto &s : rest)
        out << action_to_char(s.action) << ' ' << s.oid << ' ' << s.message
            << "\n";
}

struct todo_state {
    std::string orig_head;
    std::vector<rebase_step> steps;
};

std::optional<todo_state> read_rebase_todo(git_repository *repo)
{
    std::ifstream in(rebase_todo_file(repo));
    if (!in)
        return std::nullopt;
    todo_state st;
    if (!std::getline(in, st.orig_head))
        return std::nullopt;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2)
            continue;
        rebase_step s;
        s.action = action_from_char(line[0]);
        std::size_t sp = line.find(' ', 2); // after "<char> "
        if (sp == std::string::npos) {
            s.oid = line.substr(2);
        } else {
            s.oid = line.substr(2, sp - 2);
            s.message = line.substr(sp + 1);
        }
        st.steps.push_back(std::move(s));
    }
    return st;
}

// Move the current branch to `commit` and reset the working tree to it.
std::expected<void, error>
set_head_to(git_repository *repo, git_commit *commit, const char *reflog)
{
    git_reference *raw_head = nullptr;
    if (git_repository_head(&raw_head, repo) != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);
    git_reference *raw_new = nullptr;
    if (git_reference_set_target(&raw_new, head.get(), git_commit_id(commit),
                                 reflog) != 0)
        return std::unexpected(last_error());
    git_reference_free(raw_new);

    git_checkout_options chk;
    git_checkout_options_init(&chk, GIT_CHECKOUT_OPTIONS_VERSION);
    chk.checkout_strategy = GIT_CHECKOUT_FORCE;
    if (git_checkout_tree(repo, reinterpret_cast<git_object *>(commit), &chk) !=
        0)
        return std::unexpected(last_error());
    return {};
}

// Replay `plan` onto `tip` (owned). On an `edit` step: materialize HEAD at the
// applied commit, persist {orig_head, remaining}, return stopped. Else finish
// (move HEAD to the final tip) -> done. A cherry-pick conflict -> error.
std::expected<rebase_result, error>
run_plan(git_repository *repo, git_signature *sig, detail::commit_ptr tip,
         bool have_tip, const std::vector<rebase_step> &plan,
         const std::string &orig_head)
{
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const rebase_step &step = plan[i];
        if (step.action == rebase_action::drop)
            continue;

        git_oid aoid;
        if (git_oid_fromstr(&aoid, step.oid.c_str()) != 0)
            return std::unexpected(error{0, "bad commit id in rebase plan"});
        git_commit *raw_apply = nullptr;
        if (git_commit_lookup(&raw_apply, repo, &aoid) != 0)
            return std::unexpected(last_error());
        detail::commit_ptr apply(raw_apply);

        rebase_action act = step.action;
        if ((act == rebase_action::squash || act == rebase_action::fixup) &&
            !have_tip)
            act = rebase_action::pick; // nothing to fold into yet

        git_index *raw_idx = nullptr;
        if (git_cherrypick_commit(&raw_idx, repo, apply.get(), tip.get(), 0,
                                  nullptr) != 0)
            return std::unexpected(last_error());
        detail::index_ptr idx(raw_idx);
        if (git_index_has_conflicts(idx.get())) {
            // Pause for resolution: put the last good tip on HEAD, then apply
            // the conflicting commit to the working tree (markers +
            // CHERRY_PICK_HEAD) and persist the rest. The user resolves +
            // commits (that commit = this step), then `r r` resumes.
            if (auto m = set_head_to(repo, tip.get(), "rebase -i (conflict)");
                !m)
                return std::unexpected(m.error());
            git_cherrypick_options cpopts;
            git_cherrypick_options_init(&cpopts,
                                        GIT_CHERRYPICK_OPTIONS_VERSION);
            cpopts.checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;
            if (git_cherrypick(repo, apply.get(), &cpopts) != 0)
                return std::unexpected(last_error());
            write_rebase_todo(repo, orig_head,
                              {plan.begin() + (std::ptrdiff_t)i + 1, plan.end()});
            return rebase_result::conflicts;
        }

        git_oid tree_oid;
        if (git_index_write_tree_to(&tree_oid, idx.get(), repo) != 0)
            return std::unexpected(last_error());
        git_tree *raw_tree = nullptr;
        if (git_tree_lookup(&raw_tree, repo, &tree_oid) != 0)
            return std::unexpected(last_error());
        detail::tree_ptr tree(raw_tree);

        git_oid new_oid;
        if (act == rebase_action::pick || act == rebase_action::reword ||
            act == rebase_action::edit) {
            const char *msg = act == rebase_action::reword
                                  ? step.message.c_str()
                                  : git_commit_message(apply.get());
            const git_commit *parents[1] = {tip.get()};
            if (git_commit_create(&new_oid, repo, nullptr,
                                  git_commit_author(apply.get()), sig, nullptr,
                                  msg, tree.get(), 1, parents) != 0)
                return std::unexpected(last_error());
        } else { // squash / fixup: replace `tip` with the combined commit
            detail::commit_ptr tparent(nullptr);
            const git_commit *parents[1];
            int nparents = 0;
            if (git_commit_parentcount(tip.get()) > 0) {
                git_commit *raw_tp = nullptr;
                if (git_commit_parent(&raw_tp, tip.get(), 0) != 0)
                    return std::unexpected(last_error());
                tparent.reset(raw_tp);
                parents[0] = tparent.get();
                nparents = 1;
            }
            std::string msg = git_commit_message(tip.get());
            if (act == rebase_action::squash) {
                const char *am = git_commit_message(apply.get());
                msg += "\n\n";
                msg += (am != nullptr ? am : "");
            }
            if (git_commit_create(&new_oid, repo, nullptr,
                                  git_commit_author(tip.get()), sig, nullptr,
                                  msg.c_str(), tree.get(), nparents,
                                  nparents ? parents : nullptr) != 0)
                return std::unexpected(last_error());
        }

        git_commit *raw_new = nullptr;
        if (git_commit_lookup(&raw_new, repo, &new_oid) != 0)
            return std::unexpected(last_error());
        tip.reset(raw_new);
        have_tip = true;

        if (step.action == rebase_action::edit) {
            if (auto m = set_head_to(repo, tip.get(), "rebase -i (edit)"); !m)
                return std::unexpected(m.error());
            write_rebase_todo(repo, orig_head,
                              {plan.begin() + (std::ptrdiff_t)i + 1, plan.end()});
            return rebase_result::stopped;
        }
    }

    if (auto m = set_head_to(repo, tip.get(), "rebase -i (finish)"); !m)
        return std::unexpected(m.error());
    return rebase_result::done;
}
} // namespace

// Reopen a paused rebase and resume it (commit_current: commit the resolved
// op first = continue; false = drop it = skip).
static std::expected<rebase_result, error>
rebase_resume(std::string repo, bool commit_current)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    git_rebase_options ropts;
    git_rebase_options_init(&ropts, GIT_REBASE_OPTIONS_VERSION);
    git_rebase *raw_rebase = nullptr;
    if (git_rebase_open(&raw_rebase, r.get(), &ropts) != 0)
        return std::unexpected(last_error());
    rebase_ptr rebase(raw_rebase, git_rebase_free);

    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());
    return rebase_drive(r.get(), rebase.get(), sig.get(), commit_current);
}

std::expected<rebase_result, error> rebase_continue(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Interactive `edit` stop or paused replay conflict: resume the persisted
    // plan from the user's HEAD.
    if (auto todo = read_rebase_todo(r.get())) {
        // Refuse to continue while replay conflicts are unresolved.
        git_index *raw_idx = nullptr;
        if (git_repository_index(&raw_idx, r.get()) != 0)
            return std::unexpected(last_error());
        detail::index_ptr cidx(raw_idx);
        if (git_index_has_conflicts(cidx.get()))
            return std::unexpected(
                error{0, "resolve conflicts and commit before continuing"});

        detail::sig_ptr sig = default_signature(r.get());
        if (!sig)
            return std::unexpected(last_error());
        git_oid head_oid;
        if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") != 0)
            return std::unexpected(last_error());
        git_commit *raw_tip = nullptr;
        if (git_commit_lookup(&raw_tip, r.get(), &head_oid) != 0)
            return std::unexpected(last_error());
        detail::commit_ptr tip(raw_tip);
        auto res = run_plan(r.get(), sig.get(), std::move(tip),
                            /*have_tip=*/true, todo->steps, todo->orig_head);
        if (res && *res == rebase_result::done) {
            std::error_code ec;
            std::filesystem::remove(rebase_todo_file(r.get()), ec);
        }
        return res;
    }
    return rebase_resume(repo, /*commit_current=*/true);
}

std::expected<rebase_result, error> rebase_skip(std::string repo)
{
    return rebase_resume(std::move(repo), /*commit_current=*/false);
}

std::expected<void, error> rebase_abort(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    // Interactive `edit` stop: reset hard to the saved original HEAD.
    if (auto todo = read_rebase_todo(r.get())) {
        git_oid oid;
        if (git_oid_fromstr(&oid, todo->orig_head.c_str()) == 0) {
            git_object *raw_obj = nullptr;
            if (git_object_lookup(&raw_obj, r.get(), &oid, GIT_OBJECT_COMMIT) ==
                0) {
                detail::object_ptr obj(raw_obj);
                git_reset(r.get(), obj.get(), GIT_RESET_HARD, nullptr);
            }
        }
        std::error_code ec;
        std::filesystem::remove(rebase_todo_file(r.get()), ec);
        return {};
    }

    git_rebase_options ropts;
    git_rebase_options_init(&ropts, GIT_REBASE_OPTIONS_VERSION);
    git_rebase *raw_rebase = nullptr;
    if (git_rebase_open(&raw_rebase, r.get(), &ropts) != 0)
        return std::unexpected(last_error());
    rebase_ptr rebase(raw_rebase, git_rebase_free);

    if (git_rebase_abort(rebase.get()) != 0)
        return std::unexpected(last_error());
    return {};
}

bool rebase_in_progress(std::string repo)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return false;
    detail::repo_ptr r(raw);
    if (std::filesystem::exists(rebase_todo_file(r.get())))
        return true; // interactive `edit` stop
    const int st = git_repository_state(r.get());
    return st == GIT_REPOSITORY_STATE_REBASE ||
           st == GIT_REPOSITORY_STATE_REBASE_INTERACTIVE ||
           st == GIT_REPOSITORY_STATE_REBASE_MERGE;
}

std::expected<rebase_result, error>
rebase_interactive(std::string repo, std::string onto,
                   std::vector<rebase_step> plan)
{
    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr r(raw);

    detail::sig_ptr sig = default_signature(r.get());
    if (!sig)
        return std::unexpected(last_error());

    // Remember the original branch tip so an `edit` stop can be aborted.
    std::string orig_head;
    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, r.get(), "HEAD") == 0)
        orig_head = detail::full_oid(&head_oid);

    // Resolve `onto` -> the base commit; run the plan onto it.
    git_object *raw_onto = nullptr;
    if (git_revparse_single(&raw_onto, r.get(), onto.c_str()) != 0)
        return std::unexpected(last_error());
    detail::object_ptr onto_obj(raw_onto);
    git_commit *raw_tip = nullptr;
    if (git_commit_lookup(&raw_tip, r.get(), git_object_id(onto_obj.get())) != 0)
        return std::unexpected(last_error());
    detail::commit_ptr tip(raw_tip);

    return run_plan(r.get(), sig.get(), std::move(tip), /*have_tip=*/false, plan,
                    orig_head);
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

std::expected<void, error>
push_remote(std::string repo, std::string remote, bool force, bool set_upstream)
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
    // A leading '+' makes it a force-push refspec.
    std::string spec = (force ? std::string("+refs/heads/") : "refs/heads/") +
                       b + ":refs/heads/" + b;
    char *specs[1] = {const_cast<char *>(spec.c_str())};
    git_strarray refspecs = {specs, 1};
    git_push_options opts;
    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    opts.callbacks.credentials = credentials_cb;
    if (git_remote_push(rem.get(), &refspecs, &opts) != 0)
        return std::unexpected(last_error());

    if (set_upstream) {
        // Record branch.<b>.remote / .merge -- what `git push -u` writes.
        git_config *raw_cfg = nullptr;
        if (git_repository_config(&raw_cfg, r.get()) != 0)
            return std::unexpected(last_error());
        std::unique_ptr<git_config, decltype(&git_config_free)> cfg(
            raw_cfg, git_config_free);
        git_config_set_string(cfg.get(), ("branch." + b + ".remote").c_str(),
                              remote.c_str());
        git_config_set_string(cfg.get(), ("branch." + b + ".merge").c_str(),
                              ("refs/heads/" + b).c_str());
    }
    return {};
}

std::expected<apply_result, error>
pull_remote(std::string repo, std::string remote)
{
    if (auto fetched = fetch_remote(repo, remote); !fetched)
        return std::unexpected(fetched.error());

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

std::expected<rebase_result, error>
pull_rebase(std::string repo, std::string remote)
{
    if (auto fetched = fetch_remote(repo, remote); !fetched)
        return std::unexpected(fetched.error());

    // Find the current branch's remote-tracking ref, then rebase onto it.
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
    std::string tracking = "refs/remotes/" + remote + "/" + std::string(branch);
    return rebase_onto(repo, tracking);
}

int git_terminal(std::string repo, std::vector<std::string> args)
{
    // Like detail::run_git, but WITHOUT redirecting stdio: the child inherits
    // the caller's terminal (already in cooked mode), so credential helper /
    // SSH agent / GPG pinentry / progress all talk to the real tty. No libgit2.
    std::vector<std::string> full{"git", "-C", std::move(repo)};
    for (auto &a : args)
        full.push_back(std::move(a));
    std::vector<char *> argv;
    argv.reserve(full.size() + 1);
    for (auto &s : full)
        argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "git", nullptr, nullptr, argv.data(), environ);
    if (rc != 0)
        return -1; // git could not be executed -> caller falls back to libgit2

    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

// FM-GIT-CLI-WRITES: run a `git` mutation that lands a new HEAD, then report it.
// A non-zero exit maps to an error carrying git's combined output (so hook
// rejections / signing failures surface verbatim); success returns the new
// HEAD's short oid. Going through the CLI is the only way hooks fire and
// commit.gpgsign is honoured -- libgit2's commit/amend skip both.
static std::expected<std::string, error>
commit_via_cli(const std::string &repo, std::vector<std::string> args)
{
    auto run = detail::run_git(repo, std::move(args));
    if (run.code != 0) {
        std::string msg = run.output.empty() ? "git commit failed" : run.output;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        return std::unexpected(error{0, std::move(msg)});
    }
    // --short=8 keeps mg's 8-hex short-oid convention (detail::short_oid);
    // git's bare --short would abbreviate to its own minimum-unique length.
    auto head = detail::run_git(repo, {"rev-parse", "--short=8", "HEAD"});
    std::string oid = head.output;
    while (!oid.empty() && (oid.back() == '\n' || oid.back() == '\r'))
        oid.pop_back();
    return oid;
}

std::expected<std::string, error> commit(std::string repo, std::string message)
{
    // The staged index and any in-progress MERGE_HEAD are already on disk, so
    // plain `git commit` makes the right (possibly multi-parent) commit and
    // clears the merge / cherry-pick / revert state itself.
    return commit_via_cli(repo, {"commit", "-m", std::move(message)});
}

// FM-GIT-CLI-WRITES (P2): run a `git` op that, on a clean apply, lands a commit
// (and so fires hooks + signing) but on conflict is *left in progress* for the
// user to resolve -- merge / cherry-pick / revert. Map it to apply_result the
// same way the libgit2 versions did: exit 0 => done; a non-zero exit that left
// the index conflicted => conflicts (CHERRY_PICK_HEAD/REVERT_HEAD/MERGE_HEAD and
// markers stay on disk for the `e o`/`e t` + `c c` flow); any other non-zero
// exit (bad ref, hook veto, ...) => a real error carrying git's output.
static std::expected<apply_result, error>
apply_via_cli(const std::string &repo, std::vector<std::string> args)
{
    auto run = detail::run_git(repo, std::move(args));
    if (run.code == 0)
        return apply_result::done;

    detail::init_guard guard;
    git_repository *raw = nullptr;
    if (git_repository_open_ext(&raw, repo.c_str(), 0, nullptr) == 0) {
        detail::repo_ptr r(raw);
        git_index *raw_idx = nullptr;
        if (git_repository_index(&raw_idx, r.get()) == 0) {
            detail::index_ptr idx(raw_idx);
            if (git_index_has_conflicts(idx.get()))
                return apply_result::conflicts;
        }
    }
    std::string msg = run.output.empty() ? "git failed" : run.output;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return std::unexpected(error{0, std::move(msg)});
}

// FM-GIT-CLI-WRITES: the amend family also routes through `git commit --amend`
// so hooks fire and the result is signed -- libgit2's git_commit_amend skipped
// both. `--amend` folds in the staged index by default, which is exactly what
// commit_amend/commit_extend want; commit_reword adds `--only` (no pathspec) so
// git amends only the message and ignores any staged changes.
std::expected<std::string, error>
commit_amend(std::string repo, std::string message)
{
    return commit_via_cli(repo, {"commit", "--amend", "-m", std::move(message)});
}

std::expected<std::string, error> commit_extend(std::string repo)
{
    return commit_via_cli(repo, {"commit", "--amend", "--no-edit"});
}

std::expected<std::string, error>
commit_reword(std::string repo, std::string message)
{
    return commit_via_cli(repo,
                          {"commit", "--amend", "--only", "-m", std::move(message)});
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
    std::string msg(m != nullptr ? m : "");
    // git stores messages with a trailing newline (stripspace); strip it so the
    // returned message is the content the user typed -- matches the old libgit2
    // commit path and what a reword editor should present.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return msg;
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
