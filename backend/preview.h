#pragma once

#include "tls.h"

#include <functional>
#include <memory>

namespace httplib {
class Server;
}

namespace wave {

class AuthStore;
class PanelDatabase;

struct PanelTlsActivation {
    std::unique_ptr<httplib::Server> listener;
    TlsSettings settings;
    std::string authority;
};

using ActivatePanelTls = std::function<void(PanelTlsActivation)>;

struct PreviewRoutes {
    std::function<void(httplib::Server&)> configure;
    std::function<void(bool)> finish_tls_activation;
};

PreviewRoutes make_preview_routes(std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database,
                                  ActivatePanelTls activate_tls);

} // namespace wave
