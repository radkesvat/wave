#pragma once

#include <stop_token>
#include <string>
#include <string_view>

namespace wave {

struct PanelUpdateCheck {
    std::string latest_version;
    std::string error;
    bool update_available = false;
};

PanelUpdateCheck check_panel_update(std::string_view installed_version, std::stop_token stop);

} // namespace wave
