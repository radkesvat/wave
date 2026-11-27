#include "logs_view.h"

#include "dialog.h"
#include "preview_session.h"
#include "theme.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <format>
#include <string_view>

namespace wave {
namespace {

using theme::Color;

const view::Json& core_sources()
{
    return schematic_field(schematic_field(schematic_field(waterwall_schematic(), "runtime"), "logs"),
                           "sources");
}

std::string_view source_label(std::string_view key)
{
    if (key == "panel") {
        return "Panel";
    }
    for (const auto& source : core_sources()) {
        if (schematic_field(source, "name") == key) {
            return schematic_text(schematic_field(source, "label"));
        }
    }
    return key;
}

bool show_record(const LogsViewState& state, const view::Json& record)
{
    const auto message = view::string_field(record, "message");
    const auto source = view::string_field(record, "source");
    if (state.hidden_sources.contains(source) || message.empty()) {
        return false;
    }
    if (!state.search.IsActive()) {
        return true;
    }
    const auto text =
        std::format("{} {} {}", view::string_field(record, "ts"), source_label(source), message);
    return state.search.PassFilter(text.c_str());
}

bool source_checkbox(const char* label, float label_width, bool& selected)
{
    ImGui::PushID(label);
    const auto position = ImGui::GetCursorScreenPos();
    const bool clicked_label = ImGui::InvisibleButton("##label", {label_width, ImGui::GetFrameHeight()});
    if (ImGui::IsItemVisible()) {
        ImGui::GetWindowDrawList()->AddText({position.x, position.y + ImGui::GetStyle().FramePadding.y},
                                            ImGui::GetColorU32(ImGuiCol_Text), label);
    }
    if (clicked_label) {
        selected = !selected;
    }
    ImGui::SameLine(0, 0);
    const bool changed = ImGui::Checkbox("##checked", &selected) || clicked_label;
    ImGui::PopID();
    return changed;
}

bool render_source_filters(LogsViewState& state)
{
    const auto& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    float used = ImGui::GetItemRectSize().x;
    bool changed = false;
    const auto draw = [&](std::string_view source, const char* label, float extra_spacing) {
        const float label_width = ImGui::CalcTextSize(label).x + style.ItemInnerSpacing.x;
        const float width = label_width + ImGui::GetFrameHeight();
        const float spacing = style.ItemSpacing.x + extra_spacing;
        if (used + spacing + width <= available) {
            ImGui::SameLine(0, spacing);
            used += spacing;
        } else {
            used = 0;
        }
        bool selected = !state.hidden_sources.contains(source);
        if (source_checkbox(label, label_width, selected)) {
            if (selected) {
                state.hidden_sources.erase(std::string(source));
            } else {
                state.hidden_sources.emplace(source);
            }
            changed = true;
        }
        used += width;
    };
    draw("panel", "Panel", style.FramePadding.x);
    for (const auto& source : core_sources()) {
        draw(schematic_text(source.at("name")), source.at("label").get_ref<const std::string&>().c_str(), 0);
    }
    return changed;
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

void update_logs_view(LogsViewState& state, PreviewSession& session)
{
    if (const auto reply = session.take_reply(state.clear_request)) {
        if (reply->ok()) {
            session.clear_local_logs();
        } else if (reply->status == PreviewReply::Status::rejected && !reply->message.empty()) {
            session.add_local_log("error", reply->message);
        }
    }
}

void render_logs(LogsViewState& state, PreviewSession& session, bool narrow)
{
    constexpr char search_hint[] = "Message, source, or time";
    constexpr char auto_scroll_label[] = "Auto-scroll";
    const auto& style = ImGui::GetStyle();
    const float row_end = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float search_width = ImGui::CalcTextSize(search_hint).x + 3 * style.FramePadding.x;
    const float label_extra_width =
        ImGui::CalcTextSize(view::help_marker_label).x + 2 * style.ItemInnerSpacing.x;
    const float search_label_space = ImGui::CalcTextSize("Search").x + label_extra_width;
    const float spinner_space = state.clear_request ? busy_spinner_width + style.ItemSpacing.x : 0;
    const float controls_width = view::button_width("Options") + view::button_width("Clear") +
                                 view::button_width("Copy") + 3 * style.ItemSpacing.x + search_width +
                                 search_label_space + spinner_space;
    const float controls_gap = style.ItemSpacing.x + style.FramePadding.x;

    ImGui::PushID("logs");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Logs");
    const bool source_changed = render_source_filters(state);
    const bool ready = session.ready();
    bool turned_on = false;

    if (ImGui::GetItemRectMax().x + controls_gap + controls_width <= row_end) {
        ImGui::SameLine(0, controls_gap);
    }
    if (theme::button("Options")) {
        ImGui::OpenPopup("Log options");
    }
    // Reserve room for the marker so the popup's auto-sizing keeps it beside the checkbox.
    ImGui::SetNextWindowContentSize(
        {ImGui::GetFrameHeight() + ImGui::CalcTextSize(auto_scroll_label).x + label_extra_width, 0});
    if (ImGui::BeginPopup("Log options")) {
        if (ImGui::Checkbox(auto_scroll_label, &state.auto_scroll)) {
            turned_on = state.auto_scroll;
        }
        view::help_marker("Follow new entries while you are at the bottom of Logs. Scroll up to read "
                          "earlier entries without jumping back down.");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready || state.clear_request.has_value());
    if (theme::button("Clear")) {
        state.clear_request = session.submit({{"action", "clear_logs"}}, 5000);
    }
    ImGui::EndDisabled();
    if (state.clear_request) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    ImGui::SameLine();
    const auto& logs = session.logs();
    ImGui::BeginDisabled(logs.empty());
    const bool copy = theme::button("Copy");
    ImGui::EndDisabled();

    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + search_width + search_label_space <= row_end) {
        ImGui::SameLine();
    }
    const float available = ImGui::GetContentRegionAvail().x - search_label_space;
    ImGui::SetNextItemWidth(std::clamp(available, 1.0f, search_width));
    const bool search_changed = state.search.DrawWithHint("Search##logs", search_hint);
    view::help_marker("Use spaces to require all terms, commas to match alternatives, and quotation marks "
                      "for phrases. Add a minus sign before a term to exclude it.");
    if (!ready && logs.empty()) {
        if (session.status() != PreviewStatus::loading) {
            ImGui::TextDisabled("Logs unavailable.");
        }
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
        for (const auto& log : logs) {
            if (!show_record(state, log)) {
                continue;
            }
            const auto timestamp = view::string_field(log, "ts");
            const auto level = view::string_field(log, "level");
            const auto message = view::string_field(log, "message");
            const auto label = source_label(view::string_field(log, "source"));
            if (copy) {
                copied += std::format("{} [{}] [{}] {}\n", timestamp, label, level, message);
            }
            ++visible;
            ImGui::TextColored(theme::rgba(Color::secondary_text), "%.*s", static_cast<int>(timestamp.size()),
                               timestamp.empty() ? "" : timestamp.data());
            ImGui::SameLine();
            ImGui::TextColored(theme::rgba(Color::secondary_text), "%.*s", static_cast<int>(label.size()),
                               label.empty() ? "" : label.data());
            if (!level.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(theme::rgba(level_color(level)), "%.*s", static_cast<int>(level.size()),
                                   level.data());
            }
            if (!narrow) {
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
        }
        if (visible == 0) {
            ImGui::TextDisabled("%s", logs.empty() ? "No log entries" : "No matching entries");
        }
        if (source_changed || search_changed) {
            ImGui::SetScrollY(0);
        } else if (state.auto_scroll && (at_bottom || turned_on)) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();

    if (!copied.empty()) {
        ImGui::SetClipboardText(copied.c_str());
    }
    ImGui::PopID();
}

std::string describe_logs(const LogsViewState& state, const nlohmann::json& logs)
{
    std::size_t visible = 0;
    std::string_view latest;
    for (const auto& log : logs) {
        if (show_record(state, log)) {
            ++visible;
            latest = view::string_field(log, "message");
        }
    }
    auto description = std::format("{} log entries.", visible);
    if (!latest.empty()) {
        description += " Latest log: ";
        description += latest;
    }
    return description;
}

} // namespace wave
