#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <stop_token>
#include <string>

namespace wave {

std::string install_waterwall(const std::filesystem::path& directory, const nlohmann::json& schematic,
                              std::stop_token stop);

} // namespace wave
