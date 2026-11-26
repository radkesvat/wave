#pragma once

#include "tls.h"

#include <optional>

namespace wave {

struct PanelSettings {
    std::string listen_host;
    int listen_port;
    std::optional<TlsSettings> tls;
};

PanelSettings load_panel_settings(const char* executable, const std::filesystem::path& site_dir);

} // namespace wave
