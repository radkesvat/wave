#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace wave {

struct WaterWallSchematic;

struct OfflineUsersBinding {
    std::string database;
    std::uint64_t generation = 0;

    bool operator==(const OfflineUsersBinding&) const = default;
};

using OfflineUsersBindings = std::map<std::string, OfflineUsersBinding, std::less<>>;

bool uses_auth_database(const nlohmann::json& node);
nlohmann::json make_empty_core_config(std::string_view name);
nlohmann::json make_users_starter_config(std::string_view name, std::uint16_t port,
                                        bool allow_live_user_management = true);
nlohmann::json config_auth_nodes(const nlohmann::json& config);
std::string auth_database_name(const nlohmann::json& node, std::string_view config_name,
                               const std::filesystem::path& executable_directory);
std::string auth_database_name(const nlohmann::json& node, std::string_view config_name,
                               const std::filesystem::path& executable_directory,
                               const nlohmann::json& node_definition);
std::string validate_config_graph(const nlohmann::json& config, const nlohmann::json& previous,
                                  const WaterWallSchematic* schematic = nullptr);
std::string configure_auth_databases(nlohmann::json& config, const OfflineUsersBindings& previous,
                                     std::string_view config_name,
                                     const std::filesystem::path& executable_directory);

} // namespace wave
