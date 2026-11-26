#pragma once

#include <algorithm>
#include <limits>

#include <linux/close_range.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace wave {

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
