#pragma once

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace wave {

class PreviewSession;

struct LogsViewState {
    ImGuiTextFilter search;
    std::array<bool, 5> visible_sources{true, true, true, true, true};
    bool auto_scroll = true;
    std::optional<std::uint64_t> clear_request;
};

void update_logs_view(LogsViewState& state, PreviewSession& session);
void render_logs(LogsViewState& state, PreviewSession& session, bool narrow);
std::string describe_logs(const LogsViewState& state, const nlohmann::json& logs);

} // namespace wave
