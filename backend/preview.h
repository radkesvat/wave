#pragma once

#include "panel_settings.h"

#include <functional>
#include <memory>

namespace httplib {
class Server;
}

namespace wave {

class AuthStore;
class PanelDatabase;

struct ClosePanelListener {
    void operator()(httplib::Server* listener) const noexcept;
};

struct PanelListenerRestart {
    std::unique_ptr<httplib::Server, ClosePanelListener> listener;
    PanelSettings settings;
    int port;
    bool bind_after_stop;
};

using PreparePanelRestart = std::function<PanelListenerRestart(std::unique_ptr<httplib::Server>, PanelSettings)>;
using RestartPanel = std::function<void(PanelListenerRestart)>;

struct PreviewRoutes {
    std::function<void(httplib::Server&)> configure;
    std::function<void(bool)> finish_listener_restart;
};

PreviewRoutes make_preview_routes(std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database,
                                  PreparePanelRestart prepare_restart, RestartPanel restart_panel);

} // namespace wave
