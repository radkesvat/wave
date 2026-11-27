#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib {
struct Request;
class Server;
} // namespace httplib

namespace wave {

class AuthStore;
class PanelDatabase;

std::string panel_route(std::string_view suffix);
void configure_authenticated_routes(httplib::Server& server, const std::filesystem::path& site_dir,
                                    std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database, bool secure,
                                    const std::string& authority = {});
std::function<bool()> websocket_session(const std::shared_ptr<AuthStore>& store,
                                        const httplib::Request& request);

} // namespace wave
