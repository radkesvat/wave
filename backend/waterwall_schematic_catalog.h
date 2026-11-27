#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wave {

std::string fetch_waterwall_schematics(const std::filesystem::path& bin, std::string_view selected_version,
                                       const nlohmann::json& selected_packages, std::stop_token stop);
std::vector<std::string> cached_waterwall_versions(const std::filesystem::path& bin);
bool is_newer_waterwall_version(std::string_view candidate, std::string_view current);

} // namespace wave
