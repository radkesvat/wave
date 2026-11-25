#include "secondary_views.h"

#include "browser_ui.h"
#include "view_helpers.h"

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

using namespace std::string_view_literals;
using namespace view;

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
    if (auto reply = session.take_reply(state.apply_request)) {
        if (reply->ok()) {
            state.dirty = false;
            state.password.fill('\0');
        } else if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "Preview did not confirm the update. Check the connection and try again.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Try again after reconnecting.";
        }
        state.apply_reply = std::move(reply);
    }
    sync_settings(state, settings);
    ImGui::BeginDisabled(state.apply_request.has_value());
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
            state.apply_reply.reset();
            sync_settings(state, settings);
        }
    }
    ImGui::EndDisabled();
    if (state.apply_request) {
        ImGui::TextDisabled("Applying sample panel settings...");
    } else if (state.apply_reply) {
        feedback(*state.apply_reply);
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
            state.apply_request = session.submit({{"action", "apply_settings"}, {"settings", std::move(draft)}},
                                                 5000);
            if (state.apply_request) {
                state.apply_reply.reset();
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
        if (!ImGui::IsPopupOpen("Set up a sample bot")) {
            session.abandon(state.connect_request);
        }
        return;
    }
    if (auto reply = session.take_reply(state.connect_request)) {
        state.setup = reply->ok() ? BotState::Setup::complete : BotState::Setup::token;
        if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "Preview did not confirm the bot connection. Try again.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Try again after reconnecting.";
        }
        state.connect_reply = std::move(reply);
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
            state.connect_request = session.submit({{"action", "bot_connect"}, {"token", state.token.data()}},
                                                   5000);
            if (state.connect_request) {
                state.setup = BotState::Setup::connecting;
                state.connect_reply.reset();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Back")) {
            state.setup = BotState::Setup::intro;
        }
        if (state.connect_reply) {
            feedback(*state.connect_reply);
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
            session.abandon(state.connect_request);
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
        session.abandon(state.connect_request);
        state.setup = BotState::Setup::intro;
        state.connect_reply.reset();
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

void submit_bot_save(BotState::Save& save, PreviewSession& session, Json settings)
{
    save.request = session.submit({{"action", "apply_bot_settings"}, {"settings", std::move(settings)}}, 5000);
    if (save.request) {
        save.reply.reset();
    } else {
        save.reply = PreviewReply{PreviewReply::Status::disconnected,
                                  "The preview connection is unavailable."};
    }
}

bool finish_bot_save(BotState::Save& save, PreviewSession& session, bool& dirty)
{
    auto reply = session.take_reply(save.request);
    if (!reply) {
        return false;
    }
    if (reply->status == PreviewReply::Status::timed_out) {
        reply->message = "Preview did not confirm the update. Try again.";
    } else if (reply->status == PreviewReply::Status::disconnected) {
        reply->message = "The preview connection closed. Try again after reconnecting.";
    }
    const bool saved = reply->ok();
    save.reply = std::move(reply);
    if (saved) {
        dirty = false;
    }
    return saved;
}

void show_bot_save(const BotState::Save& save)
{
    if (save.request) {
        ImGui::TextDisabled("Saving sample bot settings...");
    } else if (save.reply) {
        feedback(*save.reply);
    }
}

void bot_settings(BotState& state, PreviewSession& session)
{
    ImGui::BeginDisabled(state.settings_save.request.has_value());
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
        submit_bot_save(state.settings_save, session, {
            {"notifications", state.notifications}, {"allow_status", state.allow_status},
            {"allow_config", state.allow_config}, {"notification_rules", std::move(rules)},
            {"permissions", std::move(permissions)}});
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    show_bot_save(state.settings_save);
}

void bot_overrides(BotState& state, PreviewSession& session)
{
    ImGui::BeginDisabled(state.overrides_save.request.has_value());
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
    if (ImGui::Button("Save overrides")) {
        submit_bot_save(state.overrides_save, session, {{"overrides", {
            {"domain", state.domain.data()}, {"sni", state.sni.data()},
            {"ws_host", state.ws_host.data()}, {"port", state.port}}}});
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    show_bot_save(state.overrides_save);
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
    const bool settings_saved = finish_bot_save(state.settings_save, session, state.settings_dirty);
    const bool overrides_saved = finish_bot_save(state.overrides_save, session, state.overrides_dirty);
    if (settings_saved || overrides_saved) {
        state.source = nullptr;
    }
    sync_bot(state, bot);
    if (ImGui::BeginTabBar("bot-views", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Connection")) {
            bot_connection(state, session, bot, narrow);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            bot_settings(state, session);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overrides")) {
            bot_overrides(state, session);
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
