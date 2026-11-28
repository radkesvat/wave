#include "core_config_files.h"

#include "core_config_name.h"

#include <openssl/sha.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>

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
    struct stat info{};
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

std::string revision_of(std::string_view contents)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(contents.data()), contents.size(), digest);
    constexpr char hex[] = "0123456789abcdef";
    std::string revision;
    revision.reserve(2 * sizeof(digest));
    for (const auto byte : digest) {
        revision.push_back(hex[byte >> 4]);
        revision.push_back(hex[byte & 0xf]);
    }
    return revision;
}

} // namespace

std::string_view core_config_registry_error(const nlohmann::json& configs, bool exact_rows)
{
    if (!configs.is_array() || configs.size() > max_workspace_configs) {
        return "must be an array of up to 64 configs.";
    }
    std::set<std::string_view> names;
    std::set<int> ids;
    for (const auto& row : configs) {
        const auto name = row.find("name");
        const auto id = row.find("id");
        const auto enabled = row.find("enabled");
        const auto* text = name == row.end() ? nullptr : name->get_ptr<const std::string*>();
        const auto value = id != row.end() && id->is_number_integer() ? id->get<std::uint64_t>() : 0;
        if (!text || text->empty() || core_config_name(*text) != *text || value == 0 ||
            value >= std::numeric_limits<int>::max() || enabled == row.end() || !enabled->is_boolean() ||
            (exact_rows && row.size() != 3) || !names.insert(*text).second || !ids.insert(static_cast<int>(value)).second) {
            return "contains an invalid or duplicate config.";
        }
    }
    return {};
}

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

CoreConfigDocument CoreConfigFiles::read(std::string_view name) const
{
    if (!available(name)) {
        return {{}, {}, "The config file is missing or unsafe."};
    }
    const auto path = root_ / name / config_file;
    const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) {
        return {{}, {}, "Could not open the config file."};
    }
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & (S_IWGRP | S_IWOTH)) ||
        (info.st_uid != ::geteuid() && info.st_uid != 0)) {
        ::close(file);
        return {{}, {}, "The config file is unsafe."};
    }
    std::string contents;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(file, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            ::close(file);
            return {{}, {}, "Could not read the config file."};
        }
        if (count == 0) {
            break;
        }
        if (contents.size() + static_cast<std::size_t>(count) > max_core_config_bytes) {
            ::close(file);
            return {{}, {}, "Wave can edit configs up to 256 KiB."};
        }
        contents.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(file);
    constexpr char invalid[] = "The config file must contain a valid JSON object.";
    std::string error;
    auto value = parse_plain_core_config_json(contents, invalid, error);
    if (!error.empty()) {
        return {{}, {}, std::move(error)};
    }
    if (!value.is_object()) {
        return {{}, {}, invalid};
    }
    return {std::move(value), revision_of(contents), {}};
}

std::optional<std::string> CoreConfigFiles::save(std::string_view name, std::string_view serialized) const
{
    if (serialized.size() > max_core_config_bytes || !available(name)) {
        return std::nullopt;
    }
    const auto directory = root_ / name;
    auto pending = (directory / ".config-XXXXXX").string();
    const int file = ::mkostemp(pending.data(), O_CLOEXEC);
    if (file < 0) {
        return std::nullopt;
    }
    const bool saved = write_config(file, serialized);
    ::close(file);
    if (!saved || ::rename(pending.c_str(), (directory / config_file).c_str()) != 0) {
        ::unlink(pending.c_str());
        return std::nullopt;
    }
    // Once renamed, the new config is visible and may reference newly created databases.
    sync_directory(directory);
    return revision_of(serialized);
}

bool CoreConfigFiles::create(std::string_view name, bool reuse_existing, bool& created,
                             const nlohmann::json& config) const
{
    created = false;
    const auto directory = root_ / name;
    const auto path = directory / config_file;
    const auto contents = config.dump(2) + '\n';
    if (contents.size() > max_core_config_bytes || !prepare_root(root_)) {
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
