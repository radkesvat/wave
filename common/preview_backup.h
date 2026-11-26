#pragma once

#include <array>
#include <cstddef>

namespace wave {

inline constexpr char preview_backup_format[] = "wave-preview-backup";
inline constexpr int preview_backup_version = 1;
inline constexpr std::size_t max_preview_restore_request_bytes = 16 * 1024;
inline constexpr std::size_t max_preview_backup_file_bytes = 24 * 1024;
inline constexpr std::array preview_backup_tls_fields{
    "domain", "certificate_path", "private_key_path"};

} // namespace wave
