#pragma once

#include <memory>

namespace httplib {
class Server;
}

namespace wave {

class AuthStore;
class PanelDatabase;

void configure_preview_routes(httplib::Server& server, std::shared_ptr<AuthStore> store,
                              std::shared_ptr<PanelDatabase> database);

} // namespace wave
