#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace wave {

struct WaterWallUsersEndpoint {
    std::string address;
    std::shared_ptr<const std::vector<std::uint16_t>> ports;
    std::string client;
    std::string token;
    std::string config;
};

struct WaterWallUsersListener {
    std::string address;
    std::shared_ptr<const std::vector<std::uint16_t>> ports;
    const nlohmann::json& clients;
};

enum class WaterWallUsersOperation { status, list, get, create, update, password };

inline constexpr std::array waterwall_users_operations{
    WaterWallUsersOperation::status, WaterWallUsersOperation::list, WaterWallUsersOperation::get,
    WaterWallUsersOperation::create, WaterWallUsersOperation::update, WaterWallUsersOperation::password};

constexpr std::string_view waterwall_users_operation_name(WaterWallUsersOperation operation)
{
    switch (operation) {
    case WaterWallUsersOperation::status: return "status";
    case WaterWallUsersOperation::list: return "list-users";
    case WaterWallUsersOperation::get: return "get-user";
    case WaterWallUsersOperation::create: return "create-user";
    case WaterWallUsersOperation::update: return "update-user";
    case WaterWallUsersOperation::password: return "change-user-password";
    }
    return {};
}

struct WaterWallUsersRequest {
    WaterWallUsersOperation operation;
    std::string id;
    std::string cursor;
    std::string if_match;
    std::string password;
    nlohmann::json fields = nullptr;
    std::size_t list_limit = 50;
};

enum class WaterWallUsersOutcome { applied, conflict, unknown, rejected, not_sent };

struct WaterWallUsersRevisions {
    std::uint64_t config;
    std::uint64_t stats;
    std::uint64_t count;
    std::string edit_token;
    std::optional<std::uint64_t> accounting_time_ms;
};

struct WaterWallUsersReply {
    nlohmann::json result;
    std::string error;
    WaterWallUsersOutcome outcome = WaterWallUsersOutcome::not_sent;
    // Kept apart from the browser-facing projection. The token is opaque.
    std::optional<WaterWallUsersRevisions> revisions;
};

bool has_waterwall_users_api(const nlohmann::json& config);
std::optional<std::uint16_t> available_waterwall_api_port(std::span<const nlohmann::json> configs);
std::vector<WaterWallUsersListener> waterwall_users_listeners(const std::map<int, nlohmann::json>& launched, int id);
std::vector<WaterWallUsersEndpoint> waterwall_users_endpoints(std::span<const WaterWallUsersListener> listeners,
                                                             std::string_view config, std::string_view node,
                                                             WaterWallUsersOperation operation, bool& ambiguous);
bool valid_waterwall_user_id(std::string_view id);
bool valid_waterwall_user_edit_token(std::string_view token);
bool valid_waterwall_users_cursor(std::string_view cursor);
WaterWallUsersReply request_waterwall_users(pid_t core_pid, std::span<const WaterWallUsersEndpoint> endpoints,
                                            std::string_view node, const WaterWallUsersRequest& request,
                                            bool legacy_list, const std::function<bool()>& run_is_current);

} // namespace wave
