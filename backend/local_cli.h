#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace wave {

struct PanelSettings;

std::optional<int> run_local_command(int argc, char* argv[]);
void validate_ready_file(const std::filesystem::path& path, const std::filesystem::path& directory);
void publish_listener_ready(const std::filesystem::path& path, const PanelSettings& settings, int port,
                            std::string_view base_path) noexcept;

} // namespace wave
