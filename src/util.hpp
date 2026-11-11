#pragma once
// Small helpers shared by all modules: syscall error checks and file I/O.

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>

// Throws std::system_error with errno if a syscall returned a negative value.
inline long check(long rc, const std::string& what) {
    if (rc < 0) throw std::system_error(errno, std::generic_category(), what);
    return rc;
}

inline std::string read_file(const std::string& path) {
    int fd = static_cast<int>(check(open(path.c_str(), O_RDONLY | O_CLOEXEC), "open " + path));
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    int err = errno;
    close(fd);
    if (n < 0) throw std::system_error(err, std::generic_category(), "read " + path);
    return out;
}

// Raw write() so kernel errors on cgroup files (EINVAL, EBUSY) are reported, not buffered away.
inline void write_file(const std::string& path, const std::string& data) {
    int fd = static_cast<int>(check(open(path.c_str(), O_WRONLY | O_CLOEXEC), "open " + path));
    ssize_t n = write(fd, data.data(), data.size());
    int err = errno;
    close(fd);
    if (n < 0) throw std::system_error(err, std::generic_category(), "write '" + data + "' to " + path);
}
