#include "core_config_files.h"

#include <nlohmann/json.hpp>

#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wave {
namespace {

namespace fs = std::filesystem;
constexpr char configs_directory[] = "configs";
constexpr char config_file[] = "config.json";

bool trusted_path(const fs::path& path, mode_t type)
{
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return false;
    }
    if ((info.st_mode & S_IFMT) != type || (info.st_uid != ::geteuid() && info.st_uid != 0) ||
        (info.st_mode & (S_IWGRP | S_IWOTH))) {
        errno = EACCES;
        return false;
    }
    return true;
}

bool prepare_root(const fs::path& path)
{
    return (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) && trusted_path(path, S_IFDIR);
}

bool write_config(int fd, std::string_view remaining)
{
    while (!remaining.empty()) {
        const auto written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        remaining.remove_prefix(static_cast<std::size_t>(written));
    }
    return ::fsync(fd) == 0;
}

bool sync_directory(const fs::path& path)
{
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
        return false;
    }
    const bool saved = ::fsync(directory) == 0;
    ::close(directory);
    return saved;
}

} // namespace

CoreConfigFiles::CoreConfigFiles(const fs::path& directory) : root_(directory / configs_directory)
{
    prepare_root(root_);
}

fs::path CoreConfigFiles::relative_path(std::string_view name)
{
    return fs::path(configs_directory) / name / config_file;
}

bool CoreConfigFiles::available(std::string_view name) const
{
    const auto directory = root_ / name;
    return trusted_path(root_, S_IFDIR) && trusted_path(directory, S_IFDIR) &&
           trusted_path(directory / config_file, S_IFREG);
}

bool CoreConfigFiles::create(std::string_view name, bool reuse_existing, bool& created) const
{
    created = false;
    const auto directory = root_ / name;
    const auto path = directory / config_file;
    const nlohmann::json config{{"name", name}, {"config-version", 1}, {"core-minimum-version", 0},
                               {"nodes", nlohmann::json::array()}};
    const auto contents = config.dump(2) + '\n';
    if (!prepare_root(root_)) {
        return false;
    }
    const bool new_directory = ::mkdir(directory.c_str(), 0700) == 0;
    if (!new_directory && (errno != EEXIST || !trusted_path(directory, S_IFDIR))) {
        return false;
    }
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        if (new_directory) {
            ::rmdir(directory.c_str());
            return false;
        }
        return errno == EEXIST && reuse_existing && available(name);
    }
    const bool saved = write_config(file, contents);
    ::close(file);
    if (!saved || !sync_directory(directory) || !sync_directory(root_)) {
        ::unlink(path.c_str());
        if (new_directory) {
            ::rmdir(directory.c_str());
        }
        return false;
    }
    created = true;
    return true;
}

bool CoreConfigFiles::remove(std::string_view name) const
{
    if (!trusted_path(root_, S_IFDIR)) {
        return errno == ENOENT;
    }
    const auto directory = root_ / name;
    if (!trusted_path(directory, S_IFDIR)) {
        return errno == ENOENT;
    }
    const auto path = directory / config_file;
    if (trusted_path(path, S_IFREG)) {
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            return false;
        }
    } else if (errno != ENOENT) {
        return false;
    }
    // Preserve auxiliary files an operator put next to the config.
    ::rmdir(directory.c_str());
    return true;
}

} // namespace wave
