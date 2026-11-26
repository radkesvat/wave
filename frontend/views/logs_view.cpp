#include "logs_view.h"

#include "preview_session.h"
#include "view_helpers.h"

#include <format>
#include <string_view>

namespace wave {
namespace {

using theme::Color;

std::string_view log_field(const nlohmann::json& record, const char* name)
{
    const auto field = record.find(name);
    if (field == record.end() || !field->is_string()) {
        return {};
    }
    return field->get_ref<const std::string&>();
}

Color level_color(std::string_view level)
{
    if (level == "success") {
        return Color::success;
    }
    if (level == "warning") {
        return Color::warning;
    }
    if (level == "error") {
        return Color::error;
    }
    return Color::info;
}

} // namespace

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow)
{
    ImGui::PushID("logs");
    ImGui::TextUnformatted("Logs");
    const auto logs = session.data().find("logs");
    const bool ready = session.status() == PreviewStatus::ready && logs != session.data().end();
    bool turned_on = false;

    if (theme::button("Options")) {
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
    if (theme::button("Clear")) {
        state.local_feedback.clear();
        if (!session.send({{"action", "clear_logs"}})) {
            state.local_feedback = "Could not clear preview logs.";
            state.local_feedback_color = Color::error;
        }
    }
    ImGui::SameLine();
    const bool copy = theme::button("Copy");
    ImGui::EndDisabled();

    if (!narrow) {
        ImGui::SameLine();
    }
    const float search_label_space = ImGui::CalcTextSize("Search").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(-search_label_space);
    const bool search_changed = state.search.DrawWithHint("Search##logs", "Message, level, or time");
    if (search_changed) {
        state.local_feedback.clear();
    }
    if (!state.local_feedback.empty()) {
        view::wrapped(state.local_feedback, state.local_feedback_color);
    }
    if (!ready) {
        ImGui::TextDisabled("%s", session.status() == PreviewStatus::loading ? "Loading preview logs…"
                                                                             : "Preview logs unavailable.");
        ImGui::PopID();
        return;
    }

    std::string copied;
    int visible = 0;
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgba(Color::divider));
    const bool entries_visible = ImGui::BeginChild("Log entries", {0, 0}, ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    if (entries_visible) {
        ImGui::PushFont(nullptr, theme::secondary_font_size);
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
            ImGui::TextColored(theme::rgba(Color::secondary_text), "%.*s", static_cast<int>(timestamp.size()),
                               timestamp.empty() ? "" : timestamp.data());
            ImGui::SameLine();
            ImGui::TextColored(theme::rgba(level_color(level)), "%.*s", static_cast<int>(level.size()),
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
        ImGui::PopFont();
    }
    ImGui::EndChild();

    if (copy) {
        if (!copied.empty()) {
            ImGui::SetClipboardText(copied.c_str());
        }
        state.local_feedback =
            visible == 0 ? "No matching entries to copy."
                         : std::format("Copied {} log {}.", visible, visible == 1 ? "entry" : "entries");
        state.local_feedback_color = visible == 0 ? Color::info : Color::success;
    }
    ImGui::PopID();
}

} // namespace wave
