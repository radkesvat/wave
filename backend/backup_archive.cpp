#include "backup_archive.h"

#include <miniz.h>

#include <algorithm>
#include <stdexcept>

namespace wave {
namespace {

struct ZipHandle {
    mz_zip_archive zip{};

    ~ZipHandle() { mz_zip_end(&zip); }
};

bool valid_entry_name(std::string_view name)
{
    if (name.empty() || name.size() >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE || name.back() == '/' ||
        std::ranges::any_of(name, [](unsigned char c) { return c < ' ' || c == 0x7f || c == '\\'; })) {
        return false;
    }
    for (;;) {
        const auto separator = name.find('/');
        const auto component = name.substr(0, separator);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        name.remove_prefix(separator + 1);
    }
}

bool regular_entry(const mz_zip_archive_file_stat& entry)
{
    // ZIP external attributes can carry both Unix file mode and DOS attributes.
    constexpr unsigned unix_type_mask = 0170000;
    constexpr unsigned unix_regular_file = 0100000;
    constexpr unsigned dos_non_file = 0x08 | 0x10 | 0x40 | 0x400;
    const auto unix_type = (entry.m_external_attr >> 16) & unix_type_mask;
    return !entry.m_is_directory && !(entry.m_external_attr & dos_non_file) &&
           (unix_type == 0 || unix_type == unix_regular_file);
}

std::size_t append_zip(void* opaque, mz_uint64 offset, const void* data, std::size_t size) noexcept
{
    auto& output = *static_cast<std::string*>(opaque);
    if (offset != output.size() || size > max_backup_archive_bytes - output.size()) {
        return 0;
    }
    try {
        output.append(static_cast<const char*>(data), size);
        return size;
    } catch (...) {
        return 0;
    }
}

} // namespace

std::string write_backup_archive(const BackupEntries& entries)
{
    if (entries.size() > max_backup_entries) {
        throw std::runtime_error("The workspace has too many files for a backup.");
    }
    std::size_t total = 0;
    for (const auto& [path, contents] : entries) {
        if (!valid_entry_name(path)) {
            throw std::runtime_error("The workspace has an invalid backup file name.");
        }
        if (contents.size() > max_backup_entry_bytes || contents.size() > max_backup_archive_bytes - total) {
            throw std::runtime_error("The workspace exceeds the backup size limit.");
        }
        total += contents.size();
    }

    std::string output;
    ZipHandle archive;
    archive.zip.m_pWrite = append_zip;
    archive.zip.m_pIO_opaque = &output;
    if (!mz_zip_writer_init(&archive.zip, 0)) {
        throw std::runtime_error("Could not create the backup ZIP.");
    }
    for (const auto& [path, contents] : entries) {
        if (!mz_zip_writer_add_mem(&archive.zip, path.c_str(), contents.data(), contents.size(), MZ_DEFAULT_LEVEL)) {
            throw std::runtime_error("Could not write the backup ZIP within the size limit.");
        }
    }
    if (!mz_zip_writer_finalize_archive(&archive.zip)) {
        throw std::runtime_error("Could not finalize the backup ZIP within the size limit.");
    }
    return output;
}

BackupEntries read_backup_archive(std::string_view bytes)
{
    if (bytes.size() > max_backup_archive_bytes) {
        throw std::runtime_error("The backup ZIP exceeds the size limit.");
    }
    ZipHandle archive;
    if (!mz_zip_reader_init_mem(&archive.zip, bytes.data(), bytes.size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        throw std::runtime_error("Choose a valid Wave backup ZIP.");
    }
    const auto count = mz_zip_reader_get_num_files(&archive.zip);
    if (count > max_backup_entries) {
        throw std::runtime_error("The backup ZIP contains too many files.");
    }

    BackupEntries entries;
    std::size_t total = 0;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat entry{};
        if (!mz_zip_reader_file_stat(&archive.zip, index, &entry) || !entry.m_is_supported || !regular_entry(entry)) {
            throw std::runtime_error("The backup ZIP contains an unsafe or unsupported entry.");
        }
        const auto name_size = mz_zip_reader_get_filename(&archive.zip, index, nullptr, 0);
        if (!name_size || name_size > sizeof(entry.m_filename)) {
            throw std::runtime_error("The backup ZIP contains an invalid file name.");
        }
        const std::string_view name(entry.m_filename, name_size - 1);
        if (!valid_entry_name(name)) {
            throw std::runtime_error("The backup ZIP contains an invalid file name.");
        }
        if (entry.m_uncomp_size > max_backup_entry_bytes || entry.m_uncomp_size > max_backup_archive_bytes - total) {
            throw std::runtime_error("The expanded backup ZIP exceeds the size limit.");
        }
        const auto [file, inserted] = entries.try_emplace(std::string(name));
        if (!inserted) {
            throw std::runtime_error("The backup ZIP contains duplicate file names.");
        }
        const auto size = static_cast<std::size_t>(entry.m_uncomp_size);
        auto& contents = file->second;
        contents.resize(size);
        if ((!size && entry.m_crc32 != 0) ||
            !mz_zip_validate_file(&archive.zip, index, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY) ||
            !mz_zip_reader_extract_to_mem(&archive.zip, index, contents.data(), size, 0)) {
            throw std::runtime_error("The backup ZIP contains a damaged file.");
        }
        total += size;
    }
    return entries;
}

} // namespace wave
