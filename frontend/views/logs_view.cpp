#include "logs_view.h"

#include "preview_session.h"

#include <format>
#include <string_view>

namespace wave {
namespace {

std::string_view log_field(const nlohmann::json& record, const char* name)
{
    const auto field = record.find(name);
    if (field == record.end() || !field->is_string()) {
        return {};
    }
    return field->get_ref<const std::string&>();
}

ImVec4 level_color(std::string_view level)
{
    if (level == "success") {
        return {0.41f, 0.90f, 0.77f, 1.0f};
    }
    if (level == "warning" || level == "error") {
        return {0.97f, 0.75f, 0.47f, 1.0f};
    }
    return {0.62f, 0.74f, 0.83f, 1.0f};
}

} // namespace

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow)
{
    ImGui::PushID("logs");
    ImGui::TextUnformatted("Logs");
    const auto logs = session.data().find("logs");
    const bool ready = session.status() == PreviewStatus::ready && logs != session.data().end();
    bool turned_on = false;

    if (ImGui::Button("Options")) {
        ImGui::OpenPopup("Log options");
    }
    if (ImGui::BeginPopup("Log options")) {
        if (ImGui::Checkbox("Auto-scroll", &state.auto_scroll)) {
            turned_on = state.auto_scroll;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Clear")) {
        state.local_feedback.clear();
        if (!session.send({{"action", "clear_logs"}})) {
            state.local_feedback = "Could not clear preview logs.";
        }
    }
    ImGui::SameLine();
    const bool copy = ImGui::Button("Copy");
    ImGui::EndDisabled();

    if (!narrow) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-65.0f);
    const bool search_changed = state.search.DrawWithHint("Search##logs", "Message, level, or time");
    if (search_changed) {
        state.local_feedback.clear();
    }
    if (!state.local_feedback.empty()) {
        ImGui::TextWrapped("%s", state.local_feedback.c_str());
    }
    if (!ready) {
        ImGui::TextDisabled("%s", session.status() == PreviewStatus::loading ? "Loading preview logs…"
                                                                             : "Preview logs unavailable.");
        ImGui::PopID();
        return;
    }

    std::string copied;
    int visible = 0;
    if (ImGui::BeginChild("Log entries", {0, 0}, ImGuiChildFlags_Borders)) {
        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        for (const auto& log : *logs) {
            const auto timestamp = log_field(log, "ts");
            const auto level = log_field(log, "level");
            const auto message = log_field(log, "message");
            if (message.empty()) {
                continue;
            }
            if (copy || state.search.IsActive()) {
                const auto row = std::format("{} [{}] {}", timestamp, level, message);
                if (!state.search.PassFilter(row.c_str())) {
                    continue;
                }
                if (copy) {
                    copied += row;
                    copied += '\n';
                }
            }
            ++visible;
            ImGui::TextDisabled("%.*s", static_cast<int>(timestamp.size()),
                                timestamp.empty() ? "" : timestamp.data());
            ImGui::SameLine();
            ImGui::TextColored(level_color(level), "%.*s", static_cast<int>(level.size()),
                               level.empty() ? "" : level.data());
            if (!narrow) {
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
        }
        if (visible == 0) {
            ImGui::TextDisabled("%s", logs->empty() ? "No preview log entries" : "No matching entries");
        }
        if (search_changed) {
            ImGui::SetScrollY(0);
        } else if (state.auto_scroll && (at_bottom || turned_on)) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (copy) {
        if (!copied.empty()) {
            ImGui::SetClipboardText(copied.c_str());
        }
        state.local_feedback =
            visible == 0 ? "No matching entries to copy."
                         : std::format("Copied {} log {}.", visible, visible == 1 ? "entry" : "entries");
    }
    ImGui::PopID();
}

} // namespace wave
