// mg.magit -- native Git working-tree status parser (task M1).
//
// Pure logic: parses `git status --porcelain=v1` text into value types, with
// errors reported through std::expected (no integer codes, no exceptions).
// Greenfield -- zero coupling to mg's C core.

module;                 // global module fragment for header includes
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

export module mg.magit;

namespace mg::magit {
// Module-internal: is `c` one of the porcelain v1 status characters?
constexpr bool is_status_char(char c)
{
    switch (c) {
    case ' ': case 'M': case 'A': case 'D': case 'R':
    case 'C': case 'U': case '?': case '!':
        return true;
    default:
        return false;
    }
}
} // namespace mg::magit

export namespace mg::magit {

// Porcelain status codes. The enumerator values ARE the literal characters git
// prints, so parsing a status column is a direct static_cast<status>(ch).
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

// One entry of `git status` output: the two-column XY code plus the path.
struct file_status {
    status index;                          // X -- staged / index side
    status worktree;                       // Y -- unstaged / working-tree side
    std::string path;
    std::optional<std::string> orig_path;  // set for renames/copies (orig -> path)
};

struct parse_error {
    std::string message;
    std::string line;
};

// Parse a single porcelain line ("XY<space>PATH", or "XY<space>ORIG -> PATH"
// for renames/copies).
std::expected<file_status, parse_error> parse_status_line(std::string_view line)
{
    // Need at least "XY<space>P": two codes, a separator space, one path char.
    if (line.size() < 4 || line[2] != ' ')
        return std::unexpected(parse_error{"line too short", std::string(line)});
    if (!is_status_char(line[0]) || !is_status_char(line[1]))
        return std::unexpected(parse_error{"unknown status code", std::string(line)});

    file_status fs;
    fs.index    = static_cast<status>(line[0]);
    fs.worktree = static_cast<status>(line[1]);

    std::string_view rest = line.substr(3);
    if (auto arrow = rest.find(" -> "); arrow != std::string_view::npos) {
        fs.orig_path = std::string(rest.substr(0, arrow));
        fs.path      = std::string(rest.substr(arrow + 4));
    } else {
        fs.path = std::string(rest);
    }
    return fs;
}

// Parse whole `git status --porcelain` output: one entry per non-empty line,
// short-circuiting to the first line that fails to parse.
std::expected<std::vector<file_status>, parse_error>
parse_status(std::string_view porcelain)
{
    std::vector<file_status> entries;
    for (const auto chunk : porcelain | std::views::split('\n')) {
        std::string_view line(chunk.begin(), chunk.end());
        if (line.empty())
            continue;
        auto parsed = parse_status_line(line);
        if (!parsed)
            return std::unexpected(std::move(parsed.error()));
        entries.push_back(std::move(*parsed));
    }
    return entries;
}

} // namespace mg::magit
