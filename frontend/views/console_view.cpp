#include "console_view.h"

#include "view_helpers.h"

namespace wave {

namespace {

using namespace view;
using theme::Color;
using theme::rgba;

using namespace std::string_view_literals;

std::string_view history_step(const Json& history, int& position, int step)
{
    if (history.empty()) {
        return ""sv;
    }
    const int size = static_cast<int>(history.size());
    if (position < 0) {
        position = step < 0 ? size - 1 : -1;
    } else {
        position = std::clamp(position + step, 0, size);
        if (position == size) {
            position = -1;
        }
    }
    return position < 0 ? ""sv : string_value(history[static_cast<std::size_t>(position)]);
}

struct ConsoleHistory {
    const Json& items;
    int& position;
};

int handle_console_history(ImGuiInputTextCallbackData* data)
{
    auto& history = *static_cast<ConsoleHistory*>(data->UserData);
    const auto command =
        history_step(history.items, history.position, data->EventKey == ImGuiKey_UpArrow ? -1 : 1);
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, command.data(), command.data() + command.size());
    return 0;
}

} // namespace

void render_console(ConsoleViewState& state, PreviewSession& session, bool narrow)
{
    const auto& console = field(session.data(), "console");
    const auto& lines = array_field(console, "output");
    const auto& history = array_field(console, "history");
    const bool running = bool_field(console, "running");
    ImGui::TextColored(rgba(running ? Color::success : Color::warning), "%s",
                       running ? "Bash preview running" : "Bash preview stopped");
    ImGui::TextWrapped("Try help, uptime, whoami or status for canned responses.");

    if (theme::button("Options")) {
        ImGui::OpenPopup("Console options");
    }
    if (ImGui::BeginPopup("Console options")) {
        ImGui::Checkbox("Auto-scroll", &state.autoscroll);
        if (theme::button(running ? "Stop console" : "Start console")) {
            session.send({{"action", running ? "stop_console" : "start_console"}});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (theme::button("Clear##console")) {
        session.send({{"action", "clear_console"}});
        state.copied = false;
    }
    ImGui::SameLine();
    const bool copy = theme::button("Copy##console");
    if (!narrow) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##console-search", "Search output", state.search.data(), state.search.size());
    ImGuiTextFilter filter(state.search.data());
    std::string copied;
    const auto last = lines.empty() ? ""sv : string_value(lines.back());
    const bool output_changed = state.output_size != lines.size() || last != state.last_line;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(Color::console));
    if (ImGui::BeginChild("console-output", {0, narrow ? 208.0f : 252.0f}, ImGuiChildFlags_Borders)) {
        for (const auto& line : lines) {
            const auto value = string_value(line);
            if (!filter.PassFilter(value.data(), value.data() + value.size())) {
                continue;
            }
            if (value.starts_with("$ ")) {
                wrapped(value, Color::accent);
            } else {
                wrapped(value);
            }
        }
        if (state.autoscroll && output_changed) {
            ImGui::SetScrollHereY(1);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    state.output_size = lines.size();
    state.last_line = last;
    if (copy) {
        for (const auto& line : lines) {
            const auto value = string_value(line);
            if (filter.PassFilter(value.data(), value.data() + value.size())) {
                copied.append(value).append("\n");
            }
        }
        ImGui::SetClipboardText(copied.c_str());
        state.copied = true;
    }
    if (state.copied) {
        ImGui::TextColored(rgba(Color::success), "Console output copied");
    }

    ImGui::BeginDisabled(!running);
    ConsoleHistory navigation{history, state.history_position};
    constexpr const char* run_label = "Run";
    constexpr const char* previous_label = "Previous";
    constexpr const char* next_label = "Next";
    const float controls_width = button_width(run_label) + button_width(previous_label) +
                                 button_width(next_label) + 3 * ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(narrow ? -1.0f : -controls_width);
    const bool submitted = ImGui::InputTextWithHint(
        "##console-command", "Demo command", state.command.data(), state.command.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, handle_console_history,
        &navigation);
    if (!narrow) {
        ImGui::SameLine();
    }
    const bool run = theme::button(run_label);
    if ((submitted || run) && state.command[0] &&
        session.send({{"action", "console_command"}, {"input", state.command.data()}})) {
        state.command[0] = '\0';
        state.history_position = -1;
        state.copied = false;
    }
    ImGui::SameLine();
    if (theme::small_button(previous_label)) {
        fill(state.command, history_step(history, state.history_position, -1));
    }
    ImGui::SameLine();
    if (theme::small_button(next_label)) {
        fill(state.command, history_step(history, state.history_position, 1));
    }
    ImGui::EndDisabled();
    feedback(session, {"console_command", "clear_console", "stop_console", "start_console"});
}

} // namespace wave
