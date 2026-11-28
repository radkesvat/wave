#pragma once

#include "core_config_json.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib {
struct Request;
struct Response;
class Server;
} // namespace httplib

namespace wave {

class AuthStore;
class PanelDatabase;

inline constexpr std::string_view backup_download_route = "/api/backup";
inline constexpr std::string_view backup_prepare_route = "/api/backup/prepare";
inline constexpr std::string_view backup_apply_route = "/api/backup/apply";
inline constexpr std::string_view workspace_http_route = "/api/workspace";
inline constexpr std::size_t max_panel_json_bytes = 4096;
inline constexpr std::size_t max_workspace_action_bytes = max_core_config_bytes + 1024;

std::string panel_route(std::string_view suffix);
using PanelJsonHandler = std::function<void(const httplib::Request&, httplib::Response&, std::string_view)>;
void post_panel_json(httplib::Server& server, const std::string& route, PanelJsonHandler handler,
                     std::size_t body_limit = max_panel_json_bytes);
std::string authenticated_session_id(const std::shared_ptr<AuthStore>& store, const httplib::Request& request);
void configure_authenticated_routes(httplib::Server& server, const std::filesystem::path& site_dir,
                                    std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database, bool secure,
                                    const std::string& authority = {});
std::function<bool()> websocket_session(const std::shared_ptr<AuthStore>& store,
                                        const std::shared_ptr<PanelDatabase>& database, const httplib::Request& request);

} // namespace wave
