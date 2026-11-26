#pragma once

#include "theme.h"

#include <imgui.h>

#include <string>

namespace wave {

class PreviewSession;

struct LogsViewState {
    ImGuiTextFilter search;
    bool auto_scroll = true;
    std::string local_feedback;
    theme::Color local_feedback_color = theme::Color::primary_text;
};

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow);

} // namespace wave
