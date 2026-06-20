// mg.io -- a std::expected file-IO layer over the POSIX primitives (task C3).
//
// The modern replacement for fileio.c's int-code / out-param style: stat, read,
// and write expressed as std::expected<T, io_error>, composed with monadic
// .and_then()/.or_else(). Errors are values (errno-faithful), never sentinels.
// Greenfield + isolated: gated by ENABLE_CPP_UPGRADES, the legacy C core is
// untouched.

module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module mg.io;

export namespace mg::io {

// Error as a value. `code` is the failing errno (0 when none); `message` is a
// human string (strerror + context).
struct io_error {
    int code;
    std::string message;
};

struct file_info {
    std::uintmax_t size;
    std::time_t mtime;
    bool is_dir;
    bool writable;   // caller has write permission on the path
};

// stat(2) the path. ENOENT etc. surface as io_error.
std::expected<file_info, io_error> stat_file(std::string path);

// Read the whole file. Rejects a directory with EISDIR (never slurps a dir).
std::expected<std::string, io_error> read_file(std::string path);

// Create/truncate `path` and write `data` (mode applies only on creation).
std::expected<void, io_error> write_file(std::string path, std::string_view data,
                                         int mode = 0644);

// Read the file and split it into lines on '\n'. A trailing newline does NOT
// yield a trailing empty line. Built by chaining read_file().and_then(...).
std::expected<std::vector<std::string>, io_error> read_lines(std::string path);

// Copy `from` to `to`: read_file(from).and_then(write to `to`). The destination
// is only touched if the read succeeds.
std::expected<void, io_error> copy_file(std::string from, std::string to);

} // namespace mg::io

// ---- internal helpers (not exported) --------------------------------------
namespace mg::io::detail {

// Move-only owning file descriptor; closes on every exit path.
class fd {
public:
    fd() = default;
    explicit fd(int f) noexcept : fd_(f) {}
    fd(fd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    fd &operator=(fd &&o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    fd(const fd &) = delete;
    fd &operator=(const fd &) = delete;
    ~fd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    void reset() noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
};

inline io_error errno_error(const char *what)
{
    const int e = errno;
    return io_error{e, std::string(what) + ": " + std::strerror(e)};
}

inline io_error coded_error(int code, const char *what)
{
    return io_error{code, std::string(what) + ": " + std::strerror(code)};
}

} // namespace mg::io::detail

// ---- definitions ----------------------------------------------------------
namespace mg::io {

std::expected<file_info, io_error> stat_file(std::string path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return std::unexpected(detail::errno_error("stat"));

    file_info fi;
    fi.size = static_cast<std::uintmax_t>(st.st_size);
    fi.mtime = st.st_mtime;
    fi.is_dir = S_ISDIR(st.st_mode);
    fi.writable = ::access(path.c_str(), W_OK) == 0;
    return fi;
}

std::expected<std::string, io_error> read_file(std::string path)
{
    detail::fd f(::open(path.c_str(), O_RDONLY));
    if (!f)
        return std::unexpected(detail::errno_error("open"));

    struct stat st;
    if (::fstat(f.get(), &st) != 0)
        return std::unexpected(detail::errno_error("fstat"));
    if (S_ISDIR(st.st_mode))
        return std::unexpected(detail::coded_error(EISDIR, "read"));

    std::string out;
    out.reserve(static_cast<std::size_t>(st.st_size));
    char buf[65536];
    ssize_t n;
    while ((n = ::read(f.get(), buf, sizeof buf)) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    if (n < 0)
        return std::unexpected(detail::errno_error("read"));
    return out;
}

std::expected<void, io_error> write_file(std::string path, std::string_view data,
                                         int mode)
{
    detail::fd f(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode));
    if (!f)
        return std::unexpected(detail::errno_error("open"));

    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(f.get(), data.data() + off, data.size() - off);
        if (n < 0)
            return std::unexpected(detail::errno_error("write"));
        off += static_cast<std::size_t>(n);
    }
    return {};
}

std::expected<std::vector<std::string>, io_error> read_lines(std::string path)
{
    return read_file(std::move(path)).and_then(
        [](std::string content)
            -> std::expected<std::vector<std::string>, io_error> {
            std::vector<std::string> lines;
            std::size_t start = 0;
            while (start <= content.size()) {
                const std::size_t nl = content.find('\n', start);
                if (nl == std::string::npos) {
                    if (start < content.size())
                        lines.emplace_back(content.substr(start));
                    break;
                }
                lines.emplace_back(content.substr(start, nl - start));
                start = nl + 1;
            }
            return lines;
        });
}

std::expected<void, io_error> copy_file(std::string from, std::string to)
{
    return read_file(std::move(from)).and_then(
        [&to](std::string content) { return write_file(to, content); });
}

} // namespace mg::io
