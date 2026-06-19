// mg.magit -- Git working-tree status value types + modeline summary.
//
// Git access moved to libgit2 (mg.git), so the original porcelain *text* parser
// was retired; these value types are the domain model libgit2 results map into,
// and summarize() renders them into the editor modeline string.

module;
#include <optional>
#include <span>
#include <string>

export module mg.magit;

export namespace mg::magit {

// Working-tree status codes (values mirror git's porcelain characters).
enum class status : char {
    unmodified = ' ',
    modified   = 'M',
    added      = 'A',
    deleted    = 'D',
    renamed    = 'R',
    copied     = 'C',
    unmerged   = 'U',
    untracked  = '?',
    ignored    = '!',
};

// One entry of git status: the two-column XY state plus the path.
struct file_status {
    status index;                          // X -- staged / index side
    status worktree;                       // Y -- unstaged / working-tree side
    std::string path;
    std::optional<std::string> orig_path;  // set for renames/copies
};

// Render a compact modeline string: "git clean", or "git" followed by the
// nonzero counts of staged (*), unstaged (+), and untracked (?) entries,
// e.g. "git *2 +1 ?3".
std::string summarize(std::span<const file_status> entries)
{
    int staged = 0, unstaged = 0, untracked = 0;
    for (const auto &e : entries) {
        if (e.worktree == status::untracked) {
            ++untracked;
            continue;
        }
        if (e.index != status::unmodified)
            ++staged;
        if (e.worktree != status::unmodified)
            ++unstaged;
    }
    if (staged == 0 && unstaged == 0 && untracked == 0)
        return "git clean";

    std::string s = "git";
    if (staged)
        s += " *" + std::to_string(staged);
    if (unstaged)
        s += " +" + std::to_string(unstaged);
    if (untracked)
        s += " ?" + std::to_string(untracked);
    return s;
}

} // namespace mg::magit
