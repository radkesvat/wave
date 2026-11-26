#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace httplib {
struct Request;
class Server;
} // namespace httplib

namespace wave {

class AuthStore;

void configure_authenticated_routes(httplib::Server& server, const std::filesystem::path& site_dir,
                                    std::shared_ptr<AuthStore> store, bool secure,
                                    const std::string& authority = {});
std::function<bool()> websocket_session(const std::shared_ptr<AuthStore>& store,
                                        const httplib::Request& request);

} // namespace wave
