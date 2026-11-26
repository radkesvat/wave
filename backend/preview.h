#pragma once

#include <memory>

namespace httplib {
class Server;
}

namespace wave {

class AuthStore;

void configure_preview_route(httplib::Server& server, std::shared_ptr<AuthStore> store);

} // namespace wave
