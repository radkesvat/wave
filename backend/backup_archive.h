#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace wave {

inline constexpr std::size_t max_backup_archive_bytes = 128 * 1024 * 1024;
inline constexpr std::size_t max_backup_entry_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t max_backup_entries = 4096;

using BackupEntries = std::map<std::string, std::string, std::less<>>;

std::string write_backup_archive(const BackupEntries& entries);
BackupEntries read_backup_archive(std::string_view archive);

} // namespace wave
