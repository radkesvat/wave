#pragma once

#include <algorithm>
#include <limits>

#include <fcntl.h>
#include <linux/close_range.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace wave {

// Takes ownership of fd and keeps it clear of stdio redirection in the child.
inline int above_stdio(int fd)
{
    if (fd < 0 || fd > STDERR_FILENO) {
        return fd;
    }
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    ::close(fd);
    return moved;
}

inline rlim_t open_file_limit()
{
    struct rlimit descriptors{};
    return ::getrlimit(RLIMIT_NOFILE, &descriptors) == 0
        ? std::min<rlim_t>(descriptors.rlim_cur, std::numeric_limits<int>::max())
        : 1024;
}

inline void close_inherited_files(int error_pipe, rlim_t descriptor_limit)
{
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3u, std::numeric_limits<unsigned int>::max(), CLOSE_RANGE_CLOEXEC) == 0) {
        return;
    }
#endif
    for (int fd = 3; static_cast<rlim_t>(fd) < descriptor_limit; ++fd) {
        if (fd != error_pipe) {
            ::close(fd);
        }
    }
}

} // namespace wave
