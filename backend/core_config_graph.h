#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace wave {

nlohmann::json make_users_starter_config(std::string_view name, std::uint16_t port);
nlohmann::json config_auth_nodes(const nlohmann::json& config);
std::string auth_database_name(const nlohmann::json& node, std::string_view config_name,
                               const std::filesystem::path& executable_directory);
std::string validate_config_graph(const nlohmann::json& config, const nlohmann::json& previous);
std::string configure_auth_databases(nlohmann::json& config, const nlohmann::json& previous,
                                     std::string_view config_name,
                                     const std::filesystem::path& executable_directory);

} // namespace wave
