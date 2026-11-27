#include "config_users_view.h"

#include "dialog.h"
#include "view_helpers.h"

#include <misc/cpp/imgui_stdlib.h>

#include <charconv>
#include <format>
#include <limits>
#include <span>

namespace wave {
namespace {

using Editor = ConfigUsersState::Editor;
using Section = ConfigUsersState::Section;
using theme::Color;
using view::Json;

constexpr char offline_only[] = "Stop WaterWall to access this users database.";

struct NumberField {
    const char* path;
    const char* label;
    const char* hint;
    std::string_view help;
    std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t minimum = 0;
};

constexpr NumberField account_id{
    "/id",
    "User ID",
    "Automatically assigned",
    "WaterWall uses this unique, nonzero ID to track the account. Leave blank to let Wave assign one.",
    std::numeric_limits<std::uint64_t>::max(),
    1};
constexpr NumberField group_id{"/gid", "Group ID", "Default group",
                               "WaterWall's numeric group identifier. Blank uses group 0."};
constexpr NumberField access_fields[] = {
    {"/record-stat-interval-ms", "Record stats interval (ms)", "WaterWall default",
     "Interval in milliseconds for recording stats. Blank uses WaterWall's default of two minutes.",
     std::numeric_limits<int>::max()},
    {"/time/expire-at-ms", "Expires at (Unix ms)", "No deadline",
     "An absolute Unix epoch timestamp in milliseconds. Blank or 0 leaves this deadline unset."},
    {"/time/expire-after-first-usage-ms", "Expires after first use (ms)", "No duration limit",
     "A duration measured from the account's first use. Enter 86400000 for 24 hours. Blank or 0 leaves it "
     "unset."},
};
constexpr NumberField traffic_fields[] = {
    {"/limit/traffic/up", "Upload traffic (bytes)", "Unlimited",
     "Traffic quotas use charged bytes. Enter 1073741824 for 1 GiB."},
    {"/limit/traffic/down", "Download traffic (bytes)", "Unlimited", {}},
    {"/limit/traffic/total", "Total traffic (bytes)", "Unlimited",
     "Caps combined charged upload and download bytes alongside the individual direction limits."},
};
constexpr NumberField bandwidth_fields[] = {
    {"/limit/bandwidth/up", "Upload bandwidth (bytes/s)", "Unlimited",
     "Bandwidth limits are rates for each direction. Enter 1048576 for 1 MiB/s."},
    {"/limit/bandwidth/down", "Download bandwidth (bytes/s)", "Unlimited", {}},
};
constexpr NumberField connection_fields[] = {
    {"/limit/ips", "IP addresses", "Unlimited",
     "Maximum number of distinct IP addresses in use at the same time for this account."},
    {"/limit/devices", "Devices", "Unlimited", {}},
    {"/limit/connections-in", "Incoming connections", "Unlimited", {}},
    {"/limit/connections-out", "Outgoing connections", "Unlimited", {}},
};
constexpr std::span<const NumberField> number_groups[] = {
    {&account_id, 1}, {&group_id, 1}, access_fields, traffic_fields, bandwidth_fields, connection_fields,
};

std::string number_text(const Json& user, const char* path)
{
    const Json::json_pointer key(path);
    if (!user.contains(key)) {
        return {};
    }
    const auto& value = user.at(key);
    return value.is_number_integer() ? value.dump() : std::string(view::string_value(value));
}

void clear_password(ConfigUsersState& state)
{
    std::ranges::fill(state.password, '\0');
    state.password.clear();
}

void select_user(ConfigUsersState& state, Editor editor, const Json& user = {})
{
    state.editor = editor;
    state.section = Section::account;
    state.open_account = editor == Editor::add || editor == Editor::edit;
    state.selected_user = user;
    state.user_id = view::string_field(user, "id");
    state.name = view::string_field(user, "name");
    state.email = view::string_field(user, "email");
    state.notes = view::string_field(user, "notes");
    state.wireguard_ips = view::string_field(user, "wireguard-allowed-ips");
    clear_password(state);
    state.enabled = view::bool_field(user, "enabled", true);
    state.numbers.clear();
    for (const auto fields : number_groups) {
        for (const auto& field : fields) {
            state.numbers.emplace(field.path, number_text(user, field.path));
        }
    }
    state.validation.clear();
    state.read_error.clear();
}

void submit(ConfigUsersState& state, PreviewSession& session, Json request, bool mutation)
{
    state.read_error.clear();
    state.request = session.submit(std::move(request), 5000);
    state.mutation = mutation;
    if (!state.request && !mutation) {
        state.read_error = "The user connection is unavailable.";
    }
}

void refresh(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
             std::size_t page)
{
    submit(state, session,
           {{"action", "get_config_users"}, {"config_id", config_id}, {"node", node}, {"page", page}}, false);
    state.refresh_needed = false;
}

void update(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    if (auto reply = session.take_reply(state.request)) {
        if (reply->ok()) {
            const auto users = reply->result.find("users");
            if (users != reply->result.end() && users->is_array()) {
                state.editable = view::bool_field(reply->result, "editable");
                state.page = reply->result.value("page", std::size_t{0});
                state.page_count = reply->result.value("page_count", std::size_t{0});
                state.total = reply->result.value("total", users->size());
                state.users = std::move(*users);
                if (state.mutation) {
                    select_user(state, Editor::none);
                }
            } else if (state.mutation) {
                session.add_local_log("error",
                                      "Wave sent an invalid user list. Refresh users before retrying.");
            } else {
                state.read_error = "Wave sent an invalid user list.";
            }
        } else if (!state.mutation) {
            if (reply->status == PreviewReply::Status::timed_out) {
                state.read_error = "Wave did not confirm the request. Refresh users before retrying.";
            } else if (reply->status == PreviewReply::Status::disconnected) {
                state.read_error = "The user connection closed. Refresh after reconnecting.";
            } else {
                state.read_error =
                    reply->message.empty() ? "The user list could not be loaded." : std::move(reply->message);
            }
        }
    }
    if (state.refresh_needed && !state.request && session.ready()) {
        refresh(state, session, config_id, node, state.page);
    }
}

const char* section_name(Section section)
{
    switch (section) {
    case Section::account:
        return "Account";
    case Section::access:
        return "Access";
    case Section::limits:
        return "Limits";
    case Section::activity:
        return "Activity";
    }
    return "";
}

struct UserPatch {
    Json fields = Json::object();
    std::string issue;
};

UserPatch pending_patch(const ConfigUsersState& state, bool creating)
{
    UserPatch patch;
    const auto set_text = [&](const char* key, const std::string& value) {
        if (value != view::string_field(state.selected_user, key)) {
            patch.fields[key] = value;
        }
    };
    set_text("name", state.name);
    set_text("email", state.email);
    set_text("notes", state.notes);
    set_text("wireguard-allowed-ips", state.wireguard_ips);
    if (creating || state.enabled != view::bool_field(state.selected_user, "enabled", true)) {
        patch.fields["enabled"] = state.enabled;
    }
    for (const auto fields : number_groups) {
        for (const auto& field : fields) {
            if (!creating && &field == &account_id) {
                continue;
            }
            const auto& text = state.numbers.at(field.path);
            const auto original = number_text(state.selected_user, field.path);
            const Json::json_pointer key(field.path);
            if (text.empty()) {
                if (!creating && !original.empty()) {
                    patch.fields[key] = nullptr;
                }
                continue;
            }
            std::uint64_t value = 0;
            const auto* end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end || value < field.minimum ||
                value > field.maximum) {
                patch.issue = std::format("{} requires a whole number from {} to {}.", field.label,
                                          field.minimum, field.maximum);
                return patch;
            }
            const auto canonical = std::to_string(value);
            if (canonical != original) {
                patch.fields[key] = canonical;
            }
        }
    }
    return patch;
}

void edit_text(const char* label, std::string& text, const char* hint, std::string_view help = {},
               ImGuiInputTextFlags flags = 0)
{
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (!help.empty()) {
        view::help_marker(help);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##value", hint, &text, flags);
    ImGui::PopID();
}

void edit_number(ConfigUsersState& state, const NumberField& field)
{
    edit_text(field.label, state.numbers.at(field.path), field.hint, field.help,
              ImGuiInputTextFlags_CharsDecimal);
}

bool begin_fields(const char* id, bool narrow)
{
    const int columns = narrow || ImGui::GetContentRegionAvail().x < 30 * ImGui::GetFontSize() ? 1 : 2;
    return ImGui::BeginTable(id, columns,
                             ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings);
}

void render_account(ConfigUsersState& state, bool creating, bool narrow)
{
    if (begin_fields("Account fields", narrow)) {
        ImGui::TableNextColumn();
        edit_text("Name", state.name, "Optional", "An operator label. Enter a unique name if you use one.");
        ImGui::TableNextColumn();
        edit_text("Email", state.email, "Optional");
        ImGui::TableNextColumn();
        edit_text("Password", state.password, creating ? "Required" : "Keep current password",
                  "Use the credential expected by the WaterWall node. SOCKS5 username and password "
                  "authentication uses the combined username:password string.",
                  ImGuiInputTextFlags_Password);
        ImGui::TableNextColumn();
        if (creating) {
            edit_number(state, account_id);
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("User ID");
            view::help_marker(
                "WaterWall uses this unique, nonzero ID to track the account across database updates.");
            view::wrapped(state.user_id, Color::secondary_text);
        }
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("Notes");
    ImGui::InputTextMultiline("##user-notes", &state.notes, {-1, 84});
}

void render_access(ConfigUsersState& state, bool narrow)
{
    ImGui::Checkbox("Enabled", &state.enabled);
    view::help_marker("Controls whether WaterWall accepts this tunnel user's credentials.");
    if (begin_fields("Access fields", narrow)) {
        ImGui::TableNextColumn();
        edit_number(state, group_id);
        ImGui::TableNextColumn();
        edit_text("WireGuard allowed IPs", state.wireguard_ips, "One IPv4 or IPv6 CIDR",
                  "Enter one IPv4 or IPv6 network with a prefix, such as 192.0.2.15/32 or 2001:db8::15/128. "
                  "User ranges must be disjoint. Leave this field blank to clear it.");
        for (const auto& field : access_fields) {
            ImGui::TableNextColumn();
            edit_number(state, field);
        }
        ImGui::EndTable();
    }
}

void render_limits(ConfigUsersState& state, bool narrow)
{
    view::wrapped("Blank or zero leaves a limit unlimited.", Color::secondary_text);
    const auto group = [&](const char* label, std::span<const NumberField> fields) {
        ImGui::SeparatorText(label);
        if (begin_fields(label, narrow)) {
            for (const auto& field : fields) {
                ImGui::TableNextColumn();
                edit_number(state, field);
            }
            ImGui::EndTable();
        }
    };
    group("Traffic", traffic_fields);
    group("Bandwidth", bandwidth_fields);
    group("Concurrent use", connection_fields);
}

void render_activity(const Json& user)
{
    struct Reading {
        const char* label;
        const char* path;
    };
    constexpr Reading timestamps[] = {{"Created (Unix ms)", "/time/created-at-ms"},
                                      {"First used (Unix ms)", "/time/first-usage-at-ms"}};
    constexpr Reading account_stats[] = {
        {"Uploaded (bytes)", "/stats/traffic/up"},
        {"Downloaded (bytes)", "/stats/traffic/down"},
        {"Upload speed (bytes/s)", "/stats/speed/up"},
        {"Download speed (bytes/s)", "/stats/speed/down"},
        {"IP addresses", "/stats/ips"},
        {"Devices", "/stats/devices"},
        {"Incoming connections", "/stats/connections-in"},
        {"Outgoing connections", "/stats/connections-out"},
    };
    const auto group = [&](const char* label, std::span<const Reading> readings) {
        ImGui::SeparatorText(label);
        if (!ImGui::BeginTable(label, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            return;
        }
        for (const auto& reading : readings) {
            ImGui::TableNextColumn();
            view::wrapped(reading.label);
            ImGui::TableNextColumn();
            const auto value = number_text(user, reading.path);
            view::wrapped(value.empty() ? "Not recorded" : std::string_view(value), Color::secondary_text);
        }
        ImGui::EndTable();
    };
    group("Recorded times", timestamps);
    group("Recorded usage", account_stats);
}

void capture_scroll_bounds(ConfigUsersState& state)
{
    if (ImGui::GetScrollMaxY() > 0) {
        const auto position = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        state.scroll_bounds = {position.x, position.y, size.x, size.y};
    }
}

const char* editor_title(Editor editor)
{
    switch (editor) {
    case Editor::add:
        return "Add user";
    case Editor::edit:
        return "Edit user";
    case Editor::remove:
        return "Delete user";
    case Editor::none:
        return "";
    }
    return "";
}

void render_editor(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                   bool writable, bool narrow)
{
    const bool creating = state.editor == Editor::add;
    const bool removing = state.editor == Editor::remove;
    ImGui::SeparatorText(editor_title(state.editor));
    if (removing) {
        const auto& user = state.name.empty() ? state.user_id : state.name;
        view::wrapped(std::format("Delete {} from {}?", user, node));
    } else if (ImGui::BeginTabBar("User editor sections")) {
        const float footer = ImGui::GetFrameHeightWithSpacing() + 2 * ImGui::GetTextLineHeightWithSpacing();
        for (const auto section : {Section::account, Section::access, Section::limits, Section::activity}) {
            const bool reset_account = state.open_account && section == Section::account;
            const auto flags = reset_account ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if ((creating && section == Section::activity) ||
                !ImGui::BeginTabItem(section_name(section), nullptr, flags)) {
                continue;
            }
            state.section = section;
            if (reset_account) {
                ImGui::SetNextWindowScroll({0, 0});
                state.open_account = false;
            }
            if (ImGui::BeginChild("User fields", {0, -footer}, ImGuiChildFlags_Borders)) {
                capture_scroll_bounds(state);
                ImGui::BeginDisabled(!writable);
                switch (section) {
                case Section::account:
                    render_account(state, creating, narrow);
                    break;
                case Section::access:
                    render_access(state, narrow);
                    break;
                case Section::limits:
                    render_limits(state, narrow);
                    break;
                case Section::activity:
                    render_activity(state.selected_user);
                    break;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    auto patch = removing ? UserPatch{} : pending_patch(state, creating);
    state.validation = std::move(patch.issue);
    const bool has_password = !state.password.empty();
    const bool changed = !patch.fields.empty() || has_password;
    ImGui::BeginDisabled(!writable || !state.validation.empty() || (creating && !has_password) ||
                         (!creating && !removing && !changed));
    if (theme::button(removing ? "Delete user" : creating ? "Create user" : "Save user")) {
        Json request{{"config_id", config_id}, {"node", node}, {"page", state.page}};
        request["action"] = removing   ? "delete_config_user"
                            : creating ? "create_config_user"
                                       : "edit_config_user";
        if (!creating) {
            request["user_id"] = state.user_id;
        }
        if (!removing) {
            if (has_password) {
                patch.fields["password"] = state.password;
            }
            request["user"] = std::move(patch.fields);
        }
        submit(state, session, std::move(request), true);
        clear_password(state);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.request.has_value());
    if (theme::button("Cancel")) {
        select_user(state, Editor::none);
    }
    ImGui::EndDisabled();
    if (state.request) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (!state.validation.empty()) {
        view::wrapped(state.validation, Color::error);
    }
}

void render_users(ConfigUsersState& state, bool writable, bool narrow)
{
    if (!ImGui::BeginTable("AuthenticationServer users", narrow ? 2 : 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("User");
    if (!narrow) {
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto& user : state.users) {
        const auto id = view::string_field(user, "id");
        const auto name = view::string_field(user, "name");
        const auto email = view::string_field(user, "email");
        const auto group = view::string_field(user, "gid");
        const bool enabled = view::bool_field(user, "enabled");
        const auto status = enabled ? "Enabled" : "Disabled";
        const auto status_color = enabled ? Color::success : Color::secondary_text;
        const bool oversized = view::bool_field(user, "oversized");
        ImGui::PushID(id.data(), id.data() + id.size());
        ImGui::TableNextColumn();
        if (name.empty()) {
            view::wrapped(std::format("User {}", id));
        } else {
            view::wrapped(name);
            view::wrapped(std::format("ID {}", id), Color::secondary_text);
        }
        if (!email.empty()) {
            view::wrapped(email, Color::secondary_text);
        }
        if (oversized) {
            view::wrapped("This account is too large to edit in Wave.", Color::secondary_text);
        }
        if (narrow) {
            view::wrapped(status, status_color);
            if (!group.empty()) {
                view::wrapped(std::format("Group {}", group), Color::secondary_text);
            }
        } else {
            ImGui::TableNextColumn();
            view::wrapped(group.empty() ? "Default" : group, Color::secondary_text);
            ImGui::TableNextColumn();
            view::wrapped(status, status_color);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!writable);
        ImGui::BeginDisabled(oversized);
        if (theme::small_button("Edit")) {
            select_user(state, Editor::edit, user);
        }
        ImGui::EndDisabled();
        if (!narrow) {
            ImGui::SameLine();
        }
        if (theme::small_button("Delete")) {
            select_user(state, Editor::remove, user);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void render_paging(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    if (state.page_count <= 1) {
        return;
    }
    ImGui::BeginDisabled(state.request.has_value() || !session.ready() || state.editor != Editor::none);
    ImGui::BeginDisabled(state.page == 0);
    if (theme::button("Previous")) {
        refresh(state, session, config_id, node, state.page - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.page + 1 >= state.page_count);
    if (theme::button("Next")) {
        refresh(state, session, config_id, node, state.page + 1);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    view::text(std::format("{} of {}", state.page + 1, state.page_count));
}

} // namespace

void render_config_users(ConfigUsersState& state, PreviewSession& session, int config_id,
                         std::string_view node, bool offline_available, bool narrow)
{
    state.scroll_bounds = {};
    const int frame = ImGui::GetFrameCount();
    if (!offline_available) {
        session.abandon(state.request);
        if (!state.blocked) {
            select_user(state, Editor::none);
            state.users = nullptr;
            state.editable = false;
        }
        state.blocked = true;
        state.refresh_needed = true;
        view::wrapped(offline_only, Color::warning);
        return;
    }
    state.blocked = false;
    const bool pending_mutation = state.request && state.mutation;
    if (state.last_frame != frame - 1 && !pending_mutation) {
        session.abandon(state.request);
        state.users = nullptr;
        state.editable = false;
        state.refresh_needed = true;
    }
    state.last_frame = frame;
    update(state, session, config_id, node);
    const bool writable = state.editable && session.ready() && !state.request;
    if (state.editor != Editor::none) {
        render_editor(state, session, config_id, node, writable, narrow);
        return;
    }
    ImGui::BeginDisabled(!writable || state.editor != Editor::none);
    if (theme::button("Add user")) {
        select_user(state, Editor::add);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.request.has_value() || !session.ready());
    if (theme::button("Refresh users")) {
        refresh(state, session, config_id, node, state.page);
    }
    ImGui::EndDisabled();
    if (state.request) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    } else if (!state.read_error.empty()) {
        view::wrapped(state.read_error, Color::error);
    }
    render_paging(state, session, config_id, node);
    if (ImGui::BeginChild("Users", {0, 0}, ImGuiChildFlags_Borders)) {
        capture_scroll_bounds(state);
        if (state.users.is_array()) {
            if (state.users.empty()) {
                view::wrapped("No users.", Color::secondary_text);
            } else {
                render_users(state, writable && state.editor == Editor::none, narrow);
            }
        }
    }
    ImGui::EndChild();
}

std::string describe_config_users(const ConfigUsersState& state)
{
    std::string description = "AuthenticationServer users.";
    if (state.blocked) {
        return description + ' ' + offline_only;
    }
    if (state.editor != Editor::none) {
        description += ' ';
        description += editor_title(state.editor);
        description += '.';
        if (state.editor != Editor::remove) {
            description += std::format(" {} section.", section_name(state.section));
        }
        if (!state.validation.empty()) {
            description += ' ' + state.validation;
        }
    }
    if (state.request) {
        description += ' ';
        description += state.mutation ? busy_description : "Loading users.";
    } else if (!state.read_error.empty()) {
        description += ' ' + state.read_error;
    }
    if (state.users.is_array()) {
        description += std::format(" {} users.", state.total);
        if (state.page_count > 1) {
            description += std::format(" Page {} of {}.", state.page + 1, state.page_count);
        }
    }
    return description;
}

} // namespace wave
