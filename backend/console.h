#pragma once

#include <filesystem>
#include <memory>

namespace httplib {
class Server;
}

namespace wave {

class AuthStore;

void configure_console_route(httplib::Server& server, std::shared_ptr<AuthStore> store,
                             const std::filesystem::path& directory);

} // namespace wave
