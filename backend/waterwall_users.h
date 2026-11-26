#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace wave {

struct WaterWallUsersEndpoint {
    std::string address;
    std::uint16_t port;
    std::string client;
    std::string token;
    std::string config;
};

struct WaterWallUsersPage {
    nlohmann::json result;
    std::string error;
};

bool has_waterwall_users_api(const nlohmann::json& config);
std::size_t waterwall_tcp_bindings(const nlohmann::json& config, const WaterWallUsersEndpoint& endpoint);
std::optional<WaterWallUsersEndpoint> waterwall_users_endpoint(const nlohmann::json& config,
                                                               std::string_view node);
bool valid_waterwall_users_cursor(std::string_view cursor);
WaterWallUsersPage list_waterwall_users(pid_t core_pid, const WaterWallUsersEndpoint& endpoint,
                                        std::string_view node, std::string_view cursor);

} // namespace wave
