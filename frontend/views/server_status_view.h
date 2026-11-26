#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace wave {

class PreviewSession;
struct PanelControlsState;

bool server_status_has_columns(float content_width, float font_size);
float render_server_status(const nlohmann::json& server, PanelControlsState& controls,
                           PreviewSession& session, bool stacked);
std::string describe_server_status(const nlohmann::json& server);

} // namespace wave
