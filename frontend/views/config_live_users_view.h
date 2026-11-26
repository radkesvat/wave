#pragma once

#include "browser_ui.h"
#include "preview_session.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wave {

struct ConfigLiveUsersState {
    nlohmann::json users;
    std::optional<std::uint64_t> request;
    // The first page has no cursor; each entry holds the cursor for a subsequent page.
    std::vector<std::string> cursors;
    std::string next_cursor;
    std::string error;
    std::size_t total = 0;
    double next_poll = 0;
    int last_frame = -1;
    CanvasViewport scroll_bounds{};
};

void render_config_live_users(ConfigLiveUsersState& state, PreviewSession& session, int config_id,
                              std::string_view node, bool eligible, bool narrow);
std::string describe_config_live_users(const ConfigLiveUsersState& state);

} // namespace wave
