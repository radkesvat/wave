#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

namespace wave {

std::string install_waterwall(const std::filesystem::path& directory, std::stop_token stop);

} // namespace wave
