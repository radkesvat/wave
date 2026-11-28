#pragma once

#include "backup_archive.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wave {

class PanelDatabase;
struct WaterWallSchematic;

struct WorkspaceBackup {
    nlohmann::json manifest;
    BackupEntries files;
    std::vector<std::string> absent_files;
    std::vector<std::string> missing_dependencies;
    std::vector<std::string> compatibility_warnings;
    std::shared_ptr<const WaterWallSchematic> schematic;
};

using WorkspaceRestoreFiles = std::map<std::string, std::array<std::uintmax_t, 5>, std::less<>>;

BackupEntries capture_workspace_backup(const std::filesystem::path& directory, const nlohmann::json& settings,
                                      const nlohmann::json& configs);
WorkspaceBackup prepare_workspace_backup(const std::filesystem::path& directory, BackupEntries entries);
WorkspaceRestoreFiles existing_workspace_restore_files(const std::filesystem::path& directory,
                                                      const WorkspaceBackup& backup);

// Recover before loading the selected schematic or opening the workspace.
void recover_workspace_restore(const PanelDatabase& database);
void apply_workspace_backup(PanelDatabase& database, const WorkspaceBackup& backup, const std::string& restore_id);

} // namespace wave
