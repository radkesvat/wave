#include "config_live_users_view.h"

#include "view_helpers.h"

#include <charconv>
#include <format>

namespace wave {
namespace {

using theme::Color;
using view::Json;

constexpr char unavailable[] = "Live users are unavailable.";
constexpr double poll_interval = 2.0;

void request_users(ConfigLiveUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    Json request{{"action", "get_live_config_users"}, {"config_id", config_id}, {"node", node}};
    if (!state.cursors.empty()) {
        request["cursor"] = state.cursors.back();
    }
    state.request = session.submit(std::move(request), 5000);
    state.next_poll = ImGui::GetTime() + poll_interval;
    if (!state.request) {
        state.users = nullptr;
        state.next_cursor.clear();
        state.error = unavailable;
    }
}

void apply_reply(ConfigLiveUsersState& state, PreviewReply& reply)
{
    auto& data = reply.result;
    auto rows = data.find("users");
    if (reply.ok() && view::bool_field(data, "available") && rows != data.end() && rows->is_array()) {
        state.total = data.value("total", rows->size());
        state.next_cursor = view::string_field(data, "next_cursor");
        state.users = std::move(*rows);
        state.error.clear();
        return;
    }

    state.users = nullptr;
    state.next_cursor.clear();
    if (view::bool_field(data, "reset_cursor") && !state.cursors.empty()) {
        state.cursors.clear();
        state.error.clear();
        state.next_poll = 0;
    } else if (!reply.ok()) {
        state.error = reply.message.empty() ? unavailable : std::move(reply.message);
    } else {
        state.cursors.clear();
        state.error.clear();
    }
}

std::string usage_label(const Json& value)
{
    double bytes = view::number(value, -1);
    if (value.is_string()) {
        const auto text = view::string_value(value);
        std::uint64_t count = 0;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, count);
        if (parsed.ec == std::errc{} && parsed.ptr == end) {
            bytes = static_cast<double>(count);
        }
    }
    if (bytes < 0) {
        return "Unavailable";
    }
    constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    std::size_t unit = 0;
    while (unit + 1 < units.size() && bytes >= 1024) {
        bytes /= 1024;
        ++unit;
    }
    return std::format("{:.{}f} {}", bytes, unit == 0 ? 0 : 1, units[unit]);
}

void render_rows(const Json& users, bool narrow)
{
    if (!ImGui::BeginTable("Live users", narrow ? 2 : 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        return;
    }
    ImGui::TableSetupColumn("User");
    if (narrow) {
        ImGui::TableSetupColumn("Usage");
    } else {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Upload");
        ImGui::TableSetupColumn("Download");
    }
    ImGui::TableHeadersRow();
    for (const auto& user : users) {
        const auto& id = view::field(user, "id");
        const auto name = view::string_field(user, "name");
        const auto status = view::bool_field(user, "enabled") ? "Enabled" : "Disabled";
        const auto& usage = view::field(user, "usage");
        const auto upload = usage_label(view::field(usage, "u"));
        const auto download = usage_label(view::field(usage, "d"));
        ImGui::TableNextColumn();
        if (name.empty() && id.is_number_integer()) {
            view::wrapped(id.dump());
        } else {
            view::wrapped(name.empty() ? view::string_value(id, "Unnamed user") : name);
        }
        if (narrow) {
            view::wrapped(status, Color::secondary_text);
            ImGui::TableNextColumn();
            view::wrapped(std::format("Upload {}\nDownload {}", upload, download));
        } else {
            ImGui::TableNextColumn();
            view::text(status);
            ImGui::TableNextColumn();
            view::text(upload);
            ImGui::TableNextColumn();
            view::text(download);
        }
    }
    ImGui::EndTable();
}

void render_paging(ConfigLiveUsersState& state)
{
    if (state.cursors.empty() && state.next_cursor.empty()) {
        return;
    }
    ImGui::BeginDisabled(state.request.has_value());
    ImGui::BeginDisabled(state.cursors.empty());
    const bool previous = theme::button("Previous");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.next_cursor.empty());
    const bool next = theme::button("Next");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    view::text(std::format("Page {}", state.cursors.size() + 1));
    if (previous || next) {
        if (previous) {
            state.cursors.pop_back();
        } else {
            state.cursors.push_back(std::move(state.next_cursor));
        }
        state.next_cursor.clear();
        state.users = nullptr;
        state.next_poll = 0;
    }
}

} // namespace

void render_config_live_users(ConfigLiveUsersState& state, PreviewSession& session, int config_id,
                              std::string_view node, bool eligible, bool narrow)
{
    const int frame = ImGui::GetFrameCount();
    eligible &= session.ready();
    if (!eligible || state.last_frame != frame - 1) {
        session.abandon(state.request);
        state = {};
    }
    state.last_frame = frame;
    state.scroll_bounds = {};
    if (!eligible) {
        return;
    }

    if (auto reply = session.take_reply(state.request)) {
        apply_reply(state, *reply);
    }
    if (!state.request && ImGui::GetTime() >= state.next_poll) {
        request_users(state, session, config_id, node);
    }
    if (!state.error.empty()) {
        view::wrapped(state.error, Color::error);
    }
    if (!state.users.is_array()) {
        return;
    }
    render_paging(state);
    if (!state.users.is_array()) {
        return;
    }
    if (ImGui::BeginChild("Live user list", {0, 0}, ImGuiChildFlags_Borders)) {
        if (ImGui::GetScrollMaxY() > 0) {
            const auto position = ImGui::GetWindowPos();
            const auto size = ImGui::GetWindowSize();
            state.scroll_bounds = {position.x, position.y, size.x, size.y};
        }
        if (state.users.empty()) {
            view::wrapped("No users.", Color::secondary_text);
        } else {
            render_rows(state.users, narrow);
        }
    }
    ImGui::EndChild();
}

std::string describe_config_live_users(const ConfigLiveUsersState& state)
{
    std::string description = "AuthenticationServer live users.";
    if (!state.error.empty()) {
        description += ' ' + state.error;
    }
    if (state.users.is_array()) {
        description += std::format(" {} users.", state.total);
        if (!state.cursors.empty() || !state.next_cursor.empty()) {
            description += std::format(" Page {}.", state.cursors.size() + 1);
        }
    }
    return description;
}

} // namespace wave
