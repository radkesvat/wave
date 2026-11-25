#include "secondary_views.h"

#include "browser_ui.h"
#include "preview_session.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using namespace std::string_view_literals;

constexpr ImVec4 mint{0.40f, 0.90f, 0.77f, 1.0f};
constexpr ImVec4 warm{0.98f, 0.74f, 0.47f, 1.0f};
constexpr ImU32 plot_surface = IM_COL32(14, 25, 36, 255);
constexpr ImU32 plot_grid = IM_COL32(38, 57, 72, 255);
constexpr ImU32 plot_muted = IM_COL32(161, 184, 197, 255);
constexpr ImU32 plot_mint = IM_COL32(104, 229, 195, 255);
constexpr ImU32 plot_blue = IM_COL32(121, 175, 244, 255);
constexpr ImU32 plot_amber = IM_COL32(246, 192, 119, 255);

const Json& field(const Json& object, std::string_view key)
{
    static const Json empty;
    const auto it = object.find(key);
    return it == object.end() ? empty : *it;
}

std::string_view string_value(const Json& value, std::string_view fallback = ""sv)
{
    return value.is_string() ? value.get_ref<const std::string&>() : fallback;
}

std::string_view string_field(const Json& object, std::string_view key, std::string_view fallback = ""sv)
{
    return string_value(field(object, key), fallback);
}

double number(const Json& value, double fallback = 0)
{
    if (value.is_number()) {
        const double result = value.get<double>();
        if (std::isfinite(result)) {
            return result;
        }
    }
    return fallback;
}

double numeric_field(const Json& object, std::string_view key, double fallback = 0)
{
    return number(field(object, key), fallback);
}

int integer_field(const Json& object, std::string_view key, int fallback = 0)
{
    return static_cast<int>(std::clamp(numeric_field(object, key, fallback), -1.0e9, 1.0e9));
}

bool bool_field(const Json& object, std::string_view key, bool fallback = false)
{
    const auto& value = field(object, key);
    return value.is_boolean() ? value.get<bool>() : fallback;
}

const Json& array_field(const Json& object, std::string_view key)
{
    static const Json empty = Json::array();
    const auto& value = field(object, key);
    return value.is_array() ? value : empty;
}

void text(std::string_view value)
{
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

void wrapped(std::string_view value)
{
    ImGui::PushTextWrapPos();
    text(value);
    ImGui::PopTextWrapPos();
}

template <std::size_t N>
void fill(std::array<char, N>& buffer, std::string_view value)
{
    const auto length = std::min(value.size(), buffer.size() - 1);
    std::copy_n(value.begin(), length, buffer.begin());
    buffer[length] = '\0';
}

template <std::size_t N>
bool edit(const char* label, std::array<char, N>& buffer, const char* hint = "",
          ImGuiInputTextFlags flags = 0)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputTextWithHint("##value", hint, buffer.data(), buffer.size(), flags);
    ImGui::PopID();
    return changed;
}

bool edit_number(const char* label, int& value)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputInt("##value", &value, 0);
    ImGui::PopID();
    return changed;
}

void feedback(const PreviewSession& session, std::initializer_list<std::string_view> actions)
{
    const PreviewFeedback* latest = nullptr;
    for (const auto action : actions) {
        const auto* result = session.feedback_for(action);
        if (result && (!latest || result->revision > latest->revision)) {
            latest = result;
        }
    }
    if (latest && !latest->message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, latest->ok ? mint : warm);
        wrapped(latest->message);
        ImGui::PopStyleColor();
    }
}

struct Series {
    const Json& samples;
    const char* name;
    ImU32 color;
};

void chart(const char* title, const char* unit, double ceiling, float height,
           std::initializer_list<Series> series)
{
    ImGui::SeparatorText(title);
    for (const auto& line : series) {
        if (!line.samples.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(line.color), "%s  %.1f %s",
                               line.name, number(line.samples.back()), unit);
        }
    }

    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy({width, height});
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, {corner.x + width, corner.y + height}, plot_surface, 6);
    if (width < 92) {
        return;
    }
    const float left = corner.x + 37;
    const float right = corner.x + width - 10;
    const float top = corner.y + 13;
    const float bottom = corner.y + height - 25;
    for (int tick = 0; tick <= 4; ++tick) {
        const float y = bottom - (bottom - top) * tick / 4;
        draw.AddLineH(left, right, y, plot_grid);
        const auto label = std::format("{:.0f}", ceiling * tick / 4);
        draw.AddText({left - ImGui::CalcTextSize(label.c_str()).x - 5, y - 8},
                     plot_muted, label.c_str());
    }
    draw.AddText({left, bottom + 5}, plot_muted, "Past hour");
    draw.AddText({right - ImGui::CalcTextSize("Now").x, bottom + 5}, plot_muted, "Now");

    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered() && mouse.x >= left && mouse.x <= right &&
        mouse.y >= top && mouse.y <= bottom;
    const float fraction = hovered ? (mouse.x - left) / (right - left) : 1;
    if (hovered) {
        draw.AddLineV(mouse.x, top, bottom, plot_muted);
    }

    constexpr std::size_t max_points = 120;
    for (const auto& line : series) {
        const auto count = std::min(line.samples.size(), max_points);
        if (count < 2) {
            continue;
        }
        std::array<ImVec2, max_points> points;
        const auto first = line.samples.size() - count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto rate = std::clamp(number(line.samples[first + i]) / ceiling, 0.0, 1.0);
            points[i] = {left + (right - left) * i / (count - 1),
                         bottom - (bottom - top) * static_cast<float>(rate)};
        }
        draw.AddPolyline(points.data(), static_cast<int>(count), line.color, 2);
        const auto selected = static_cast<std::size_t>(std::lround(fraction * (count - 1)));
        draw.AddCircleFilled(points[selected], hovered ? 4 : 3, line.color);
    }
    if (hovered && ImGui::BeginTooltip()) {
        for (const auto& line : series) {
            if (!line.samples.empty()) {
                const auto index = static_cast<std::size_t>(std::lround(fraction * (line.samples.size() - 1)));
                ImGui::Text("%s %.1f %s", line.name, number(line.samples[index]), unit);
            }
        }
        ImGui::EndTooltip();
    }
}

double traffic_ceiling(const Json& upload, const Json& download)
{
    double peak = 0;
    for (const auto* samples : {&upload, &download}) {
        for (const auto& sample : *samples) {
            peak = std::max(peak, number(sample));
        }
    }
    return std::max(4.0, 4.0 * std::ceil(peak / 4.0));
}

void server_info(const Json& server)
{
    ImGui::SeparatorText("Server information");
    if (ImGui::BeginTable("server-details", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Current value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        auto row = [](const char* label, std::string_view value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            wrapped(value);
        };
        const int uptime = integer_field(server, "uptime_minutes");
        row("Name", string_field(server, "name", "Wave sample"));
        row("Region", string_field(server, "region", "Preview"));
        row("Platform", string_field(server, "platform", "Demo"));
        row("Version", string_field(server, "version", "0.1.0"));
        row("Uptime", std::format("{}d {}h {}m", uptime / 1440, uptime / 60 % 24, uptime % 60));
        row("CPU", std::format("{} cores at {:.1f}%", integer_field(server, "cpu_cores", 4),
                               numeric_field(server, "cpu_percent")));
        row("Memory", std::format("{:.1f} / {:.1f} GiB", numeric_field(server, "memory_used_gib"),
                                  numeric_field(server, "memory_total_gib")));
        row("Users", std::format("{} total, {} enabled, {} online",
                                 integer_field(server, "users_total"),
                                 integer_field(server, "users_enabled"),
                                 integer_field(server, "users_active")));
        row("Upload total", std::format("{:.1f} GiB", numeric_field(server, "upload_total")));
        row("Download total", std::format("{:.1f} GiB", numeric_field(server, "download_total")));
        ImGui::EndTable();
    }
}

void render_server(const Json& server, bool narrow)
{
    const bool columns = !narrow && ImGui::GetContentRegionAvail().x >= 580 &&
        ImGui::BeginTable("server-plots", 2, ImGuiTableFlags_SizingStretchSame);
    if (columns) {
        ImGui::TableNextColumn();
    }
    chart("CPU history", "%", 100, 145, {{array_field(server, "cpu_history"), "CPU", plot_mint}});
    if (columns) {
        ImGui::TableNextColumn();
    }
    chart("Memory history", "GiB", std::max(1.0, numeric_field(server, "memory_total_gib")), 145,
          {{array_field(server, "memory_history"), "Memory", plot_blue}});
    if (columns) {
        ImGui::EndTable();
    }

    const bool traffic_columns = !narrow && ImGui::GetContentRegionAvail().x >= 720 &&
        ImGui::BeginTable("server-summary", 2, ImGuiTableFlags_SizingStretchSame);
    if (traffic_columns) {
        ImGui::TableNextColumn();
    }
    server_info(server);
    if (traffic_columns) {
        ImGui::TableNextColumn();
    }
    const auto& upload = array_field(server, "upload_history");
    const auto& download = array_field(server, "download_history");
    chart("Traffic history", "MiB/s", traffic_ceiling(upload, download), 168,
          {{upload, "Upload", plot_mint}, {download, "Download", plot_amber}});
    if (traffic_columns) {
        ImGui::EndTable();
    }
}

void sync_settings(SecondaryViewsState::Settings& state, const Json& settings)
{
    if (state.dirty || settings == state.source || !settings.is_object()) {
        return;
    }
    fill(state.name, string_field(settings, "name"));
    fill(state.username, string_field(settings, "admin_username"));
    fill(state.domain, string_field(settings, "domain"));
    fill(state.websocket_path, string_field(settings, "websocket_path"));
    fill(state.certificate_path, string_field(settings, "certificate_path"));
    fill(state.private_key_path, string_field(settings, "private_key_path"));
    fill(state.site_template, string_field(settings, "site_template"));
    state.port = integer_field(settings, "port", 2030);
    state.multiplex = bool_field(settings, "multiplex");
    state.source = settings;
}

void render_settings(SecondaryViewsState::Settings& state, PreviewSession& session, bool narrow)
{
    const auto& settings = field(session.data(), "settings");
    const auto* result = session.feedback_for("apply_settings");
    if (state.applying && result && result->revision > state.apply_feedback) {
        state.applying = false;
        if (result->ok) {
            state.dirty = false;
            state.password.fill('\0');
        }
    } else if (state.applying && ImGui::GetTime() >= state.apply_deadline) {
        state.applying = false;
        state.apply_timed_out = true;
    }
    sync_settings(state, settings);
    ImGui::BeginDisabled(state.applying);
    const bool columns = !narrow && ImGui::GetContentRegionAvail().x >= 680 &&
        ImGui::BeginTable("settings-columns", 2, ImGuiTableFlags_SizingStretchSame);
    if (columns) {
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Panel identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        state.dirty |= edit("Panel name", state.name);
        state.dirty |= edit("Demo admin name", state.username);
        state.dirty |= edit("New demo password", state.password, "Keep current demo password",
                            ImGuiInputTextFlags_Password);
    }
    if (columns) {
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Routing preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        state.dirty |= edit("Domain", state.domain, "preview.example");
        state.dirty |= edit_number("Sample port", state.port);
        state.dirty |= edit("Sample WebSocket path", state.websocket_path, "/demo");
        state.dirty |= edit("Landing page template", state.site_template, "Wave welcome");
        state.dirty |= ImGui::Checkbox("Multiplex routes", &state.multiplex);
    }
    if (columns) {
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Example file paths")) {
        state.dirty |= edit("Certificate path", state.certificate_path);
        state.dirty |= edit("Private key path", state.private_key_path);
    }

    const bool valid = state.name[0] && state.username[0] && state.domain[0] && state.site_template[0] &&
        state.websocket_path[0] == '/' && state.port >= 1 && state.port <= 65535;
    if (state.dirty && !valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, warm);
        ImGui::TextWrapped("Enter a panel name, admin name, domain, template, /path and port 1-65535.");
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!state.dirty || !valid);
    if (ImGui::Button("Apply changes")) {
        ImGui::OpenPopup("Apply preview settings");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Backup preview")) {
        state.backup = Json{{"format", "wave-preview"}, {"settings", settings},
                            {"users", array_field(session.data(), "users")}}.dump(2);
        session.send({{"action", "backup_settings"}});
        ImGui::OpenPopup("Sample backup");
    }
    if (state.dirty) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Revert edits")) {
            state.dirty = false;
            state.source = nullptr;
            state.password.fill('\0');
            state.apply_timed_out = false;
            sync_settings(state, settings);
        }
    }
    ImGui::EndDisabled();
    if (state.applying) {
        ImGui::TextDisabled("Applying sample panel settings...");
    } else if (state.apply_timed_out) {
        ImGui::PushStyleColor(ImGuiCol_Text, warm);
        ImGui::TextWrapped("Preview did not confirm the update. Check the connection and try again.");
        ImGui::PopStyleColor();
    }
    if (!state.applying && !state.apply_timed_out) {
        feedback(session, {"apply_settings"});
    }

    fit_preview_dialog(430);
    if (ImGui::BeginPopupModal("Apply preview settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Save these values to this Wave process for the preview?");
        if (ImGui::Button("Confirm apply")) {
            Json draft = {{"name", state.name.data()}, {"admin_username", state.username.data()},
                          {"domain", state.domain.data()}, {"port", state.port},
                          {"websocket_path", state.websocket_path.data()},
                          {"site_template", state.site_template.data()},
                          {"certificate_path", state.certificate_path.data()},
                          {"private_key_path", state.private_key_path.data()},
                          {"multiplex", state.multiplex}};
            if (state.password[0]) {
                draft["password"] = state.password.data();
            }
            state.apply_feedback = session.feedback_revision();
            if (session.send({{"action", "apply_settings"}, {"settings", std::move(draft)}})) {
                state.applying = true;
                state.apply_timed_out = false;
                state.apply_deadline = ImGui::GetTime() + 5.0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    fit_preview_dialog(540);
    if (ImGui::BeginPopupModal("Sample backup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Applied preview settings and sample user records");
        ImGui::InputTextMultiline("##sample-backup", state.backup.data(), state.backup.size() + 1,
                                 {-1, 170}, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy backup")) {
            ImGui::SetClipboardText(state.backup.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        feedback(session, {"backup_settings"});
        ImGui::EndPopup();
    }
}

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
    const auto command = history_step(history.items, history.position, data->EventKey == ImGuiKey_UpArrow ? -1 : 1);
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, command.data(), command.data() + command.size());
    return 0;
}

void render_console(SecondaryViewsState::Console& state, PreviewSession& session, bool narrow)
{
    const auto& console = field(session.data(), "console");
    const auto& lines = array_field(console, "output");
    const auto& history = array_field(console, "history");
    const bool running = bool_field(console, "running");
    ImGui::TextColored(running ? mint : warm, "%s", running ? "Bash preview running" : "Bash preview stopped");
    ImGui::TextWrapped("Try help, uptime, whoami or status for canned responses.");

    if (ImGui::Button("Options")) {
        ImGui::OpenPopup("Console options");
    }
    if (ImGui::BeginPopup("Console options")) {
        ImGui::Checkbox("Auto-scroll", &state.autoscroll);
        if (ImGui::Button(running ? "Stop console" : "Start console")) {
            session.send({{"action", running ? "stop_console" : "start_console"}});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##console")) {
        session.send({{"action", "clear_console"}});
        state.copied = false;
    }
    ImGui::SameLine();
    const bool copy = ImGui::Button("Copy##console");
    if (!narrow) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##console-search", "Search output", state.search.data(), state.search.size());
    ImGuiTextFilter filter(state.search.data());
    std::string copied;
    const auto last = lines.empty() ? ""sv : string_value(lines.back());
    const bool output_changed = state.output_size != lines.size() || last != state.last_line;
    if (ImGui::BeginChild("console-output", {0, narrow ? 208.0f : 252.0f}, ImGuiChildFlags_Borders)) {
        for (const auto& line : lines) {
            const auto value = string_value(line);
            if (!filter.PassFilter(value.data(), value.data() + value.size())) {
                continue;
            }
            if (value.starts_with("$ ")) {
                ImGui::PushStyleColor(ImGuiCol_Text, mint);
                wrapped(value);
                ImGui::PopStyleColor();
            } else {
                wrapped(value);
            }
        }
        if (state.autoscroll && output_changed) {
            ImGui::SetScrollHereY(1);
        }
    }
    ImGui::EndChild();
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
        ImGui::TextColored(mint, "Console output copied");
    }

    ImGui::BeginDisabled(!running);
    ConsoleHistory navigation{history, state.history_position};
    ImGui::SetNextItemWidth(narrow ? -1.0f : -170.0f);
    const bool submitted = ImGui::InputTextWithHint("##console-command", "Demo command",
        state.command.data(), state.command.size(), ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CallbackHistory, handle_console_history, &navigation);
    if (!narrow) {
        ImGui::SameLine();
    }
    const bool run = ImGui::Button("Run");
    if ((submitted || run) && state.command[0] &&
        session.send({{"action", "console_command"}, {"input", state.command.data()}})) {
        state.command[0] = '\0';
        state.history_position = -1;
        state.copied = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Previous")) {
        fill(state.command, history_step(history, state.history_position, -1));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Next")) {
        fill(state.command, history_step(history, state.history_position, 1));
    }
    ImGui::EndDisabled();
    feedback(session, {"console_command", "clear_console", "stop_console", "start_console"});
}

using Selection = SecondaryViewsState::Topology::Selection;

bool in_map(const Json& item, std::string_view map)
{
    const auto membership = string_field(item, "map", "both");
    return membership == "both" || membership == map;
}

std::string_view node_label(const Json& nodes, std::string_view id)
{
    for (const auto& node : nodes) {
        if (string_field(node, "id") == id) {
            return string_field(node, "label", id);
        }
    }
    return id;
}

void inspect(SecondaryViewsState::Topology& state, Selection selection, std::string_view id)
{
    state.selection = selection;
    state.selected = id;
}

float distance_to_link(ImVec2 mouse, ImVec2 start, ImVec2 end)
{
    const float x = end.x - start.x;
    const float y = end.y - start.y;
    const float length_squared = x * x + y * y;
    const float fraction = length_squared > 0 ? std::clamp(
        ((mouse.x - start.x) * x + (mouse.y - start.y) * y) / length_squared, 0.0f, 1.0f) : 0.0f;
    return std::hypot(mouse.x - start.x - x * fraction, mouse.y - start.y - y * fraction);
}

struct GraphNode {
    const Json& data;
    std::string_view id;
    ImVec2 position;
};

void draw_topology(SecondaryViewsState::Topology& state, const Json& nodes, const Json& links,
                   std::string_view map, bool narrow)
{
    ImGui::BeginDisabled(state.zoom <= 0.7f);
    if (ImGui::Button("-##zoom")) {
        state.zoom = std::max(0.7f, state.zoom / 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.zoom >= 2.5f);
    if (ImGui::Button("+##zoom")) {
        state.zoom = std::min(2.5f, state.zoom * 1.25f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fit map")) {
        state.zoom = 1;
        state.offset_x = state.offset_y = 0;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", state.zoom * 100);
    ImGui::TextWrapped("Drag to pan. Select a node or connection to inspect it.");

    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = narrow ? 230.0f : 290.0f;
    const auto corner = ImGui::GetCursorScreenPos();
    const ImVec2 bottom{corner.x + width, corner.y + height};
    const bool selected = ImGui::InvisibleButton("##network-topology", {width, height});
    if (ImGui::IsItemActivated()) {
        state.graph_dragged = false;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 7)) {
        const auto delta = ImGui::GetIO().MouseDelta;
        state.offset_x = std::clamp(state.offset_x + delta.x, -width, width);
        state.offset_y = std::clamp(state.offset_y + delta.y, -height, height);
        state.graph_dragged = true;
    }
    const bool inspect_graph = selected && !state.graph_dragged;
    const auto mouse = ImGui::GetIO().MousePos;
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, bottom, plot_surface, 6);
    draw.PushClipRect(corner, bottom, true);
    constexpr float grid = 28;
    for (float x = corner.x + std::fmod(state.offset_x, grid); x < bottom.x; x += grid) {
        draw.AddLineV(x, corner.y, bottom.y, plot_grid);
    }
    for (float y = corner.y + std::fmod(state.offset_y, grid); y < bottom.y; y += grid) {
        draw.AddLineH(corner.x, bottom.x, y, plot_grid);
    }

    const float node_width = narrow ? 71.0f : 103.0f;
    constexpr float node_height = 34.0f;
    const ImVec2 center{corner.x + width / 2 + state.offset_x, corner.y + height / 2 + state.offset_y};
    const float span_x = std::max(1.0f, width - node_width - 22) * state.zoom;
    const float span_y = (height - node_height - 22) * state.zoom;
    std::vector<GraphNode> graph_nodes;
    graph_nodes.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!in_map(node, map)) {
            continue;
        }
        const float x = static_cast<float>(std::clamp(numeric_field(node, "x", 0.5), 0.0, 1.0));
        const float y = static_cast<float>(std::clamp(numeric_field(node, "y", 0.5), 0.0, 1.0));
        graph_nodes.push_back({node, string_field(node, "id"),
            {center.x + (x - 0.5f) * span_x, center.y + (y - 0.5f) * span_y}});
    }
    const auto lookup_node = [&](std::string_view id) {
        return std::ranges::find(graph_nodes, id, &GraphNode::id);
    };
    float nearest_distance = 8.0f;
    std::string_view nearest_link;
    for (const auto& link : links) {
        if (!in_map(link, map)) {
            continue;
        }
        const auto from = lookup_node(string_field(link, "from"));
        const auto to = lookup_node(string_field(link, "to"));
        if (from == graph_nodes.end() || to == graph_nodes.end()) {
            continue;
        }
        const auto id = string_field(link, "id");
        const bool active = state.selection == Selection::link && state.selected == id;
        draw.AddLine(from->position, to->position, active ? plot_mint : plot_blue, active ? 3.0f : 1.5f);
        if (inspect_graph) {
            const float distance = distance_to_link(mouse, from->position, to->position);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_link = id;
            }
        }
    }
    if (!nearest_link.empty()) {
        inspect(state, Selection::link, nearest_link);
    }
    for (const auto& node : graph_nodes) {
        const ImVec2 start{node.position.x - node_width / 2, node.position.y - node_height / 2};
        const ImVec2 end{start.x + node_width, start.y + node_height};
        if (inspect_graph && mouse.x >= start.x && mouse.x <= end.x &&
            mouse.y >= start.y && mouse.y <= end.y) {
            inspect(state, Selection::node, node.id);
        }
        const bool active = state.selection == Selection::node && state.selected == node.id;
        draw.AddRectFilled(start, end, IM_COL32(25, 50, 66, 255), 6);
        draw.AddRect(start, end, active ? plot_mint : plot_blue, 6, active ? 2.5f : 1.0f);
        const auto label = string_field(node.data, "label", node.id);
        auto* font = ImGui::GetFont();
        const float font_size = narrow ? 13.0f : 15.0f;
        const float wrap_width = node_width - 12;
        const auto* label_end = label.data() + label.size();
        const auto extent = font->CalcTextSizeA(font_size, FLT_MAX, wrap_width, label.data(), label_end);
        draw.AddText(font, font_size, {node.position.x - extent.x / 2, node.position.y - extent.y / 2},
                     IM_COL32(234, 243, 247, 255), label.data(), label_end, wrap_width);
    }
    draw.PopClipRect();
}

const Json* selected_item(const Json& items, std::string_view id)
{
    const auto item = std::ranges::find_if(items, [&](const Json& value) {
        return string_field(value, "id") == id;
    });
    return item == items.end() ? nullptr : &*item;
}

void topology_details(const SecondaryViewsState::Topology& state, const Json& nodes,
                      const Json& links, const Json& routes)
{
    if (state.selected.empty()) {
        return;
    }
    ImGui::SeparatorText("Inspection");
    const Json* selected = nullptr;
    switch (state.selection) {
    case Selection::node:
        selected = selected_item(nodes, state.selected);
        if (selected) {
            wrapped(std::format("{} ({})", string_field(*selected, "label"), string_field(*selected, "type")));
            wrapped(string_field(*selected, "address"));
        }
        break;
    case Selection::link:
        selected = selected_item(links, state.selected);
        if (selected) {
            wrapped(std::format("{} ({})", string_field(*selected, "label"), string_field(*selected, "type")));
            wrapped(std::format("{} to {}", node_label(nodes, string_field(*selected, "from")),
                                node_label(nodes, string_field(*selected, "to"))));
        }
        break;
    case Selection::route:
        selected = selected_item(routes, state.selected);
        if (selected) {
            wrapped(string_field(*selected, "name"));
            std::string path;
            for (const auto& node : array_field(*selected, "path")) {
                if (!path.empty()) {
                    path += " > ";
                }
                path += node_label(nodes, string_value(node));
            }
            wrapped(path);
        }
        break;
    }
    if (selected) {
        wrapped(string_field(*selected, "description"));
    }
}

void render_map(SecondaryViewsState::Topology& state, const Json& topology, std::string_view map,
                bool narrow)
{
    const auto& nodes = array_field(topology, "nodes");
    const auto& links = array_field(topology, "links");
    const auto& routes = array_field(topology, "routes");
    draw_topology(state, nodes, links, map, narrow);
    topology_details(state, nodes, links, routes);
    if (ImGui::CollapsingHeader("Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& node : nodes) {
            if (in_map(node, map)) {
                const auto id = string_field(node, "id");
                const auto label = std::format("{}##{}", string_field(node, "label"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::node && state.selected == id)) {
                    inspect(state, Selection::node, id);
                }
            }
        }
    }
    if (ImGui::CollapsingHeader("Connections")) {
        for (const auto& link : links) {
            if (in_map(link, map)) {
                const auto id = string_field(link, "id");
                const auto label = std::format("{}##{}", string_field(link, "label"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::link && state.selected == id)) {
                    inspect(state, Selection::link, id);
                }
            }
        }
    }
    if (ImGui::CollapsingHeader("Routes")) {
        for (const auto& route : routes) {
            if (in_map(route, map)) {
                const auto id = string_field(route, "id");
                const auto label = std::format("{}##{}", string_field(route, "name"), id);
                if (ImGui::Selectable(label.c_str(),
                                      state.selection == Selection::route && state.selected == id)) {
                    inspect(state, Selection::route, id);
                }
            }
        }
    }
}

void render_topology(SecondaryViewsState& state, const Json& topology, bool narrow)
{
    if (ImGui::BeginTabBar("topology-views")) {
        if (ImGui::BeginTabItem("Inside Map")) {
            ImGui::PushID("inside-map");
            render_map(state.inside_map, topology, "inside", narrow);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Outside Map")) {
            ImGui::PushID("outside-map");
            render_map(state.outside_map, topology, "outside", narrow);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

using BotState = SecondaryViewsState::Bot;

struct Option {
    const char* key;
    const char* label;
};

constexpr std::array notification_options{
    Option{"login_failed", "Demo sign-in failed"}, Option{"login_succeeded", "Demo sign-in succeeded"},
    Option{"settings_changed", "Settings changed"}, Option{"high_cpu", "High CPU usage"},
    Option{"high_memory", "High memory usage"}, Option{"server_reboot", "Server reboot"},
    Option{"panel_started", "Panel started"}, Option{"user_expired", "User duration expired"},
    Option{"traffic_limit", "Traffic limit reached"}, Option{"user_created", "User created"},
    Option{"user_disabled", "User disabled"}, Option{"user_enabled", "User enabled"},
    Option{"user_deleted", "User deleted"}, Option{"user_support", "User requests support"},
};

constexpr std::array permission_options{
    Option{"status", "View status"}, Option{"traffic", "View traffic usage"},
    Option{"ip_limit", "View address limit"}, Option{"speed_limit", "View speed limit"},
    Option{"traffic_limit", "View traffic limit"}, Option{"expiration", "View time remaining"},
    Option{"notes", "View user notes"},
};

void sync_bot(BotState& state, const Json& bot)
{
    if (bot == state.source || !bot.is_object()) {
        return;
    }
    if (!state.settings_dirty) {
        state.notifications = bool_field(bot, "notifications");
        state.allow_status = bool_field(bot, "allow_status");
        state.allow_config = bool_field(bot, "allow_config");
        const auto& rules = field(bot, "notification_rules");
        for (std::size_t i = 0; i < notification_options.size(); ++i) {
            const auto& rule = field(rules, notification_options[i].key);
            state.rules[i] = {bool_field(rule, "enabled"), bool_field(rule, "sound")};
        }
        const auto& permissions = field(bot, "permissions");
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            state.permissions[i] = bool_field(permissions, permission_options[i].key);
        }
    }
    if (!state.overrides_dirty) {
        const auto& overrides = field(bot, "overrides");
        fill(state.domain, string_field(overrides, "domain"));
        fill(state.sni, string_field(overrides, "sni"));
        fill(state.ws_host, string_field(overrides, "ws_host"));
        state.port = integer_field(overrides, "port");
    }
    state.source = bot;
}

void bot_setup_dialog(BotState& state, PreviewSession& session, const Json& bot)
{
    fit_preview_dialog(500);
    if (!ImGui::BeginPopupModal("Set up a sample bot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const auto* result = session.feedback_for("bot_connect");
    if (state.setup == BotState::Setup::connecting) {
        if (result && result->revision > state.connect_feedback) {
            state.setup = result->ok ? BotState::Setup::complete : BotState::Setup::token;
        } else if (ImGui::GetTime() >= state.connect_deadline) {
            state.setup = BotState::Setup::token;
            state.connect_timed_out = true;
        }
    }
    switch (state.setup) {
    case BotState::Setup::intro:
        ImGui::SeparatorText("1. Bot setup");
        ImGui::TextWrapped("Connect a sample bot to preview notifications, user configuration and messages.");
        if (ImGui::Button("Next")) {
            state.setup = BotState::Setup::token;
        }
        break;
    case BotState::Setup::token:
        ImGui::SeparatorText("2. Demo token");
        edit("Sample token", state.token, "wave-preview-token");
        ImGui::TextWrapped("Use the supplied sample token or enter another demo value.");
        ImGui::BeginDisabled(std::string_view(state.token.data()).size() < 4);
        if (ImGui::Button("Connect sample bot")) {
            state.connect_feedback = session.feedback_revision();
            if (session.send({{"action", "bot_connect"}, {"token", state.token.data()}})) {
                state.setup = BotState::Setup::connecting;
                state.connect_timed_out = false;
                state.connect_deadline = ImGui::GetTime() + 5.0;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Back")) {
            state.setup = BotState::Setup::intro;
        }
        if (state.connect_timed_out) {
            ImGui::PushStyleColor(ImGuiCol_Text, warm);
            ImGui::TextWrapped("Preview did not confirm the bot connection. Try again.");
            ImGui::PopStyleColor();
        } else if (result && result->revision > state.connect_feedback && !result->ok) {
            feedback(session, {"bot_connect"});
        }
        break;
    case BotState::Setup::connecting:
        ImGui::TextUnformatted("Connecting the sample bot...");
        break;
    case BotState::Setup::complete:
        ImGui::SeparatorText("3. Sample bot connected");
        wrapped(string_field(bot, "admin_link"));
        if (ImGui::Button("Done")) {
            ImGui::CloseCurrentPopup();
        }
        break;
    }
    if (state.setup != BotState::Setup::complete) {
        if (state.setup != BotState::Setup::connecting) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void bot_connection(BotState& state, PreviewSession& session, const Json& bot, bool narrow)
{
    const bool connected = bool_field(bot, "connected");
    if (connected) {
        const auto name = string_field(bot, "name", "Wave sample bot");
        ImGui::TextColored(mint, "Sample bot connected");
        wrapped(name);
    } else {
        ImGui::TextColored(warm, "Sample bot disconnected");
    }
    if (ImGui::Button(connected ? "Open setup demo" : "Set up bot")) {
        state.setup = BotState::Setup::intro;
        state.connect_feedback = session.feedback_revision();
        state.connect_timed_out = false;
        fill(state.token, "wave-preview-token");
        ImGui::OpenPopup("Set up a sample bot");
    }
    if (connected) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Copy access link")) {
            const std::string link{string_field(bot, "admin_link")};
            ImGui::SetClipboardText(link.c_str());
            state.access_link_copied = !link.empty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) {
            ImGui::OpenPopup("Disconnect sample bot");
        }
    }
    if (state.access_link_copied && connected) {
        ImGui::TextColored(mint, "Sample access link copied");
    }
    feedback(session, {"bot_connect", "bot_disconnect"});
    bot_setup_dialog(state, session, bot);
    fit_preview_dialog(380);
    if (ImGui::BeginPopupModal("Disconnect sample bot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Disconnect the bot from this preview process?");
        if (ImGui::Button("Confirm disconnect") && session.send({{"action", "bot_disconnect"}})) {
            state.access_link_copied = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void bot_settings(BotState& state, PreviewSession& session, const Json& bot)
{
    state.settings_dirty |= ImGui::Checkbox("Admin notifications", &state.notifications);
    state.settings_dirty |= ImGui::Checkbox("Allow status requests", &state.allow_status);
    state.settings_dirty |= ImGui::Checkbox("Allow demo configuration requests", &state.allow_config);

    if (ImGui::CollapsingHeader("Notification rules", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("bot-notifications", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 54);
            ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 49);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < notification_options.size(); ++i) {
                ImGui::PushID(notification_options[i].key);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                wrapped(notification_options[i].label);
                ImGui::TableNextColumn();
                state.settings_dirty |= ImGui::Checkbox("##enabled", &state.rules[i].enabled);
                ImGui::TableNextColumn();
                state.settings_dirty |= ImGui::Checkbox("##sound", &state.rules[i].sound);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("User permissions")) {
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            state.settings_dirty |= ImGui::Checkbox(permission_options[i].label, &state.permissions[i]);
        }
    }

    ImGui::BeginDisabled(!state.settings_dirty);
    if (ImGui::Button("Save bot settings")) {
        Json rules = Json::object();
        for (std::size_t i = 0; i < notification_options.size(); ++i) {
            rules[notification_options[i].key] = {{"enabled", state.rules[i].enabled},
                                                  {"sound", state.rules[i].sound}};
        }
        Json permissions = Json::object();
        for (std::size_t i = 0; i < permission_options.size(); ++i) {
            permissions[permission_options[i].key] = state.permissions[i];
        }
        if (session.send({{"action", "apply_bot_settings"}, {"settings", {
                {"notifications", state.notifications}, {"allow_status", state.allow_status},
                {"allow_config", state.allow_config}, {"notification_rules", std::move(rules)},
                {"permissions", std::move(permissions)}, {"overrides", field(bot, "overrides")}}}})) {
            state.settings_dirty = false;
        }
    }
    ImGui::EndDisabled();
    feedback(session, {"apply_bot_settings"});
}

void bot_overrides(BotState& state, PreviewSession& session, const Json& bot)
{
    ImGui::TextWrapped("Use sample overrides in the configuration that this preview bot shows.");
    state.overrides_dirty |= edit("Domain", state.domain, "preview.example");
    state.overrides_dirty |= edit("Server name (SNI)", state.sni);
    state.overrides_dirty |= edit("WebSocket host", state.ws_host);
    state.overrides_dirty |= edit_number("Port override", state.port);
    const bool valid = state.port >= 0 && state.port <= 65535;
    if (!valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, warm);
        ImGui::TextWrapped("Enter 0 for the default port or a value from 1 to 65535.");
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!state.overrides_dirty || !valid);
    if (ImGui::Button("Save overrides") &&
        session.send({{"action", "apply_bot_settings"}, {"settings", {
            {"notifications", bool_field(bot, "notifications")}, {"allow_status", bool_field(bot, "allow_status")},
            {"allow_config", bool_field(bot, "allow_config")}, {"overrides", {
                {"domain", state.domain.data()}, {"sni", state.sni.data()},
                {"ws_host", state.ws_host.data()}, {"port", state.port}}}}}})) {
        state.overrides_dirty = false;
    }
    ImGui::EndDisabled();
    feedback(session, {"apply_bot_settings"});
}

void bot_actions(BotState& state, PreviewSession& session, const Json& bot)
{
    constexpr const char* audiences[]{"All preview users", "Enabled preview users", "Preview admins"};
    ImGui::TextUnformatted("Message audience");
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##bot-audience", &state.audience, audiences, IM_ARRAYSIZE(audiences));
    ImGui::TextUnformatted("Demo message");
    ImGui::InputTextMultiline("##bot-message", state.message.data(), state.message.size(), {-1, 76});
    ImGui::BeginDisabled(!state.message[0]);
    if (ImGui::Button("Message preview")) {
        ImGui::OpenPopup("Preview bot message");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Test status")) {
        session.send({{"action", "bot_action"}, {"kind", "test status"},
                      {"audience", audiences[state.audience]}});
    }
    const auto message = string_field(bot, "last_message");
    if (!message.empty()) {
        ImGui::SeparatorText("Latest simulated message");
        wrapped(message);
    }
    feedback(session, {"bot_action"});

    fit_preview_dialog(460);
    if (ImGui::BeginPopupModal("Preview bot message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(audiences[state.audience]);
        ImGui::Separator();
        wrapped(state.message.data());
        if (ImGui::Button("Simulate send") &&
            session.send({{"action", "bot_action"}, {"kind", "message"}, {"message", state.message.data()},
                          {"audience", audiences[state.audience]}})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void render_bot(BotState& state, PreviewSession& session, bool narrow)
{
    const auto& bot = field(session.data(), "bot");
    sync_bot(state, bot);
    if (ImGui::BeginTabBar("bot-views", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Connection")) {
            bot_connection(state, session, bot, narrow);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            bot_settings(state, session, bot);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overrides")) {
            bot_overrides(state, session, bot);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Actions")) {
            bot_actions(state, session, bot);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void render_about()
{
    ImGui::TextColored(mint, "Wave 0.1.0");
    ImGui::TextWrapped("Wave is a browser panel for the WaterWall core. This workspace uses sample data.");
    ImGui::SeparatorText("Projects");
    ImGui::TextLinkOpenURL("Wave source", "https://github.com/radkesvat/wave");
    ImGui::TextLinkOpenURL("WaterWall core", "https://github.com/radkesvat/WaterWall");
    ImGui::SeparatorText("Notices");
    ImGui::TextLinkOpenURL("Wave (Mozilla Public License 2.0)", "/licenses/wave.txt");
    ImGui::TextLinkOpenURL("Dear ImGui (MIT)", "/licenses/dear-imgui.txt");
    ImGui::TextLinkOpenURL("Karla (SIL Open Font License)", "/licenses/karla.txt");
    ImGui::TextLinkOpenURL("nlohmann/json (MIT)", "/licenses/nlohmann-json.txt");
    ImGui::TextLinkOpenURL("nlohmann/json third-party notices", "/licenses/nlohmann-json-third-party.txt");
    ImGui::TextLinkOpenURL("Nayuki QR Code generator (MIT)", "/licenses/nayuki-qr.txt");
}

} // namespace

void render_secondary(SecondaryViewsState& state, SecondaryView view, PreviewSession& session,
                      bool narrow)
{
    ImGui::PushID(static_cast<int>(view));
    switch (view) {
    case SecondaryView::server:
        render_server(field(session.data(), "server"), narrow);
        break;
    case SecondaryView::settings:
        render_settings(state.settings, session, narrow);
        break;
    case SecondaryView::console:
        render_console(state.console, session, narrow);
        break;
    case SecondaryView::topology:
        render_topology(state, field(session.data(), "topology"), narrow);
        break;
    case SecondaryView::bot:
        render_bot(state.bot, session, narrow);
        break;
    case SecondaryView::about:
        render_about();
        break;
    }
    ImGui::PopID();
}

} // namespace wave
