#pragma once

#include <imgui.h>

#include <string>

namespace wave {

class PreviewSession;

struct LogsViewState {
    ImGuiTextFilter search;
    bool auto_scroll = true;
    std::string local_feedback;
};

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow);

} // namespace wave
