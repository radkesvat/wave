#pragma once

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace wave {

class PreviewSession;

struct LogsViewState {
    ImGuiTextFilter search;
    std::set<std::string, std::less<>> hidden_sources;
    bool auto_scroll = true;
    std::optional<std::uint64_t> clear_request;
};

void update_logs_view(LogsViewState& state, PreviewSession& session);
void render_logs(LogsViewState& state, PreviewSession& session, bool narrow);
std::string describe_logs(const LogsViewState& state, const nlohmann::json& logs);

} // namespace wave
