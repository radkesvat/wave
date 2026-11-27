#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace wave {

std::string run_waterwall_tool(std::vector<std::string> command, const std::filesystem::path& target,
                               std::size_t limit, std::chrono::milliseconds timeout, std::stop_token stop);

} // namespace wave
