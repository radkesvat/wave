#pragma once

#include "theme.h"

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <string>

namespace wave {

class PreviewSession;

struct LogsViewState {
    ImGuiTextFilter search;
    std::array<bool, 5> visible_sources{true, true, true, true, true};
    bool auto_scroll = true;
    std::string local_feedback;
    theme::Color local_feedback_color = theme::Color::primary_text;
};

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow);
std::string describe_logs(const LogsViewState& state, const nlohmann::json& logs);

} // namespace wave
