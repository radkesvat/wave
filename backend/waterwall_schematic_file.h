#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace wave {

class PanelDatabase;
struct WaterWallSchematic;

inline constexpr std::string_view bundled_waterwall_version = "1.6.0";

std::filesystem::path waterwall_bin_directory(const std::filesystem::path& backend_directory);
std::filesystem::path waterwall_release_directory(const std::filesystem::path& bin_directory,
                                                  std::string_view version);
std::filesystem::path waterwall_schematic_path(const std::filesystem::path& bin_directory,
                                               std::string_view version);
std::shared_ptr<const WaterWallSchematic> prepare_waterwall_schematic_file(const std::filesystem::path& path);
std::shared_ptr<const WaterWallSchematic> prepare_waterwall_release_schematic(
    const std::filesystem::path& bin_directory, std::string_view version);
void select_waterwall_schematic(PanelDatabase& database);
void load_waterwall_schematic_file(const std::filesystem::path& path);

} // namespace wave
