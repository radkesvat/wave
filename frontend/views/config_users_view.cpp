#include "config_users_view.h"

#include "dialog.h"
#include "schematic_users_ui.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Editor = ConfigUsersState::Editor;
using Operation = ConfigUsersState::Operation;
using Section = ConfigUsersState::Section;
using theme::Color;
using view::Json;

constexpr char offline_only[] = "Stop WaterWall to access this users database.";
constexpr std::uint64_t megabytes_per_gigabyte = 1'000;

struct NumberField {
    const char* path;
    const char* label;
    const char* hint;
    std::string_view help;
    SchematicUserNumberBounds bounds;
    bool gigabytes = false;
};

const Json& user_ui()
{
    return view::field(view::field(waterwall_schematic(), "ui"), "users");
}

const Json& user_field(std::string_view role)
{
    return view::field(view::field(user_ui(), "fields"), role);
}

std::string_view user_key(std::string_view role)
{
    return view::string_field(user_field(role), "key");
}

NumberField number_field(const Json& descriptor)
{
    const auto path = view::string_field(descriptor, "path");
    const auto defaults = path == "/id"                        ? explicit_user_id_bounds
                          : path == "/record-stat-interval-ms" ? user_stat_interval_bounds
                                                               : SchematicUserNumberBounds{};
    const auto bounds = schematic_user_number_bounds(descriptor, defaults);
    const bool gigabytes = view::bool_field(descriptor, "gigabytes");
    return {
        .path = path.data(),
        .label = view::string_field(descriptor, "label").data(),
        .hint = view::string_field(descriptor, "hint").data(),
        .help = view::string_field(descriptor, "help"),
        .bounds = gigabytes ? whole_megabyte_bounds(bounds) : bounds,
        .gigabytes = gigabytes,
    };
}

std::vector<NumberField> editable_numbers(bool creating)
{
    std::vector<NumberField> result;
    const auto add = [&](const Json& descriptor) {
        if (descriptor.is_object() && !view::string_field(descriptor, "path").empty()) {
            result.push_back(number_field(descriptor));
        }
    };
    if (creating) {
        add(user_field("id"));
    }
    add(user_field("group"));
    for (const auto& field : view::array_field(user_ui(), "access_fields")) {
        add(field);
    }
    for (const auto& group : view::array_field(user_ui(), "limit_groups")) {
        for (const auto& field : view::array_field(group, "fields")) {
            add(field);
        }
    }
    return result;
}

std::string number_text(const Json& user, const char* path)
{
    const Json::json_pointer key(path);
    if (!user.contains(key)) {
        return {};
    }
    const auto& value = user.at(key);
    return value.is_number_integer() ? value.dump() : std::string(view::string_value(value));
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional(value) : std::nullopt;
}

std::string gigabytes_from_megabytes(std::uint64_t megabytes)
{
    return std::format("{}.{:03}", megabytes / megabytes_per_gigabyte, megabytes % megabytes_per_gigabyte);
}

std::string display_number(const Json& user, const NumberField& field)
{
    auto text = number_text(user, field.path);
    if (field.gigabytes) {
        if (const auto bytes = parse_unsigned(text)) {
            return gigabytes_from_megabytes(*bytes / bytes_per_megabyte);
        }
    }
    return text;
}

std::optional<std::uint64_t> parse_gigabytes_as_megabytes(std::string_view text)
{
    // Shift the decimal point to get an integer count of MB without losing precision.
    std::string megabytes(text);
    std::size_t fractional_digits = 0;
    if (const auto point = megabytes.find('.'); point != std::string::npos) {
        fractional_digits = megabytes.size() - point - 1;
        if (fractional_digits > 3 || megabytes.size() == 1) {
            return std::nullopt;
        }
        megabytes.erase(point, 1);
    }
    megabytes.append(3 - fractional_digits, '0');
    return parse_unsigned(megabytes);
}

void clear_password(std::string& password)
{
    std::ranges::fill(password, '\0');
    password.clear();
}

void select_user(ConfigUsersState& state, Editor editor, const Json& user = {})
{
    state.editor = editor;
    state.section = Section::account;
    state.open_account = editor == Editor::add || editor == Editor::edit;
    state.selected_user = user;
    state.user_id = view::string_field(user, user_key("id"));
    state.name = view::string_field(user, user_key("name"));
    state.email = view::string_field(user, user_key("email"));
    state.notes = view::string_field(user, user_key("notes"));
    state.network = view::string_field(user, user_key("network"));
    clear_password(state.password);
    clear_password(state.saved_password);
    state.password_unavailable = false;
    state.enabled = view::bool_field(user, user_key("enabled"), true);
    state.numbers.clear();
    if (editor == Editor::add || editor == Editor::edit) {
        for (const auto& field : editable_numbers(editor == Editor::add)) {
            state.numbers.emplace(field.path, display_number(user, field));
        }
    }
    state.validation.clear();
    state.read_error.clear();
}

void submit(ConfigUsersState& state, PreviewSession& session, Json request, Operation operation)
{
    state.read_error.clear();
    state.request = session.submit(std::move(request), 5000);
    state.operation = operation;
    state.passive_read = false;
    if (!state.request) {
        if (operation == Operation::list) {
            state.read_error = "The user connection is unavailable.";
        } else if (operation == Operation::password) {
            state.password_unavailable = true;
        }
    }
}

void refresh(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
             std::size_t page, bool passive = false)
{
    submit(state, session,
           {{"action", "get_config_users"}, {"config_id", config_id}, {"node", node}, {"page", page}},
           Operation::list);
    state.passive_read = passive;
    state.refresh_needed = false;
}

void update(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    if (auto reply = session.take_reply(state.request)) {
        state.passive_read = false;
        if (state.operation == Operation::password) {
            const auto password =
                reply->ok() ? view::string_field(reply->result, "password") : std::string_view{};
            state.password_unavailable = password.empty();
            if (!state.password_unavailable) {
                state.saved_password = password;
                state.password = password;
            }
        } else if (reply->ok()) {
            const auto users = reply->result.find("users");
            if (users != reply->result.end() && users->is_array()) {
                state.editable = view::bool_field(reply->result, "editable");
                state.page = reply->result.value("page", std::size_t{0});
                state.page_count = reply->result.value("page_count", std::size_t{0});
                state.total = reply->result.value("total", users->size());
                state.users = std::move(*users);
                if (state.operation == Operation::mutation) {
                    select_user(state, Editor::none);
                }
            } else if (state.operation == Operation::mutation) {
                session.add_local_log("error",
                                      "Wave sent an invalid user list. Refresh users before retrying.");
            } else {
                state.read_error = "Wave sent an invalid user list.";
            }
        } else if (state.operation == Operation::list) {
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
        refresh(state, session, config_id, node, state.page, true);
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
    const auto set_text = [&](std::string_view role, const std::string& value) {
        const auto key = user_key(role);
        if (!key.empty() && value != view::string_field(state.selected_user, key)) {
            patch.fields[std::string(key)] = value;
        }
    };
    set_text("name", state.name);
    set_text("email", state.email);
    set_text("notes", state.notes);
    set_text("network", state.network);
    const auto enabled_key = user_key("enabled");
    if (!enabled_key.empty() &&
        (creating || state.enabled != view::bool_field(state.selected_user, enabled_key, true))) {
        patch.fields[std::string(enabled_key)] = state.enabled;
    }
    for (const auto& field : editable_numbers(creating)) {
        const auto input = state.numbers.find(field.path);
        if (input == state.numbers.end() || input->second == display_number(state.selected_user, field)) {
            continue;
        }
        const auto& text = input->second;
        const auto original = number_text(state.selected_user, field.path);
        const Json::json_pointer key(field.path);
        if (text.empty()) {
            if (!creating && !original.empty()) {
                patch.fields[key] = nullptr;
            }
            continue;
        }
        const auto value = field.gigabytes ? parse_gigabytes_as_megabytes(text) : parse_unsigned(text);
        if (!value || *value < field.bounds.minimum || *value > field.bounds.maximum) {
            patch.issue = field.gigabytes
                              ? std::format("{} must be between {} and {}, with up to three decimal places.",
                                            field.label, gigabytes_from_megabytes(field.bounds.minimum),
                                            gigabytes_from_megabytes(field.bounds.maximum))
                              : std::format("{} requires a whole number from {} to {}.", field.label,
                                            field.bounds.minimum, field.bounds.maximum);
            return patch;
        }
        const auto canonical = std::to_string(field.gigabytes ? *value * bytes_per_megabyte : *value);
        if (canonical != original) {
            patch.fields[key] = canonical;
        }
    }
    return patch;
}

void edit_text(const char* id, const char* label, std::string& text, const char* hint, std::string_view help,
               ImGuiInputTextFlags flags = 0)
{
    ImGui::PushID(id);
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
    std::string saved_quota_help;
    auto help = field.help;
    if (field.gigabytes) {
        const auto bytes = parse_unsigned(number_text(state.selected_user, field.path));
        if (bytes && *bytes % bytes_per_megabyte != 0) {
            saved_quota_help = std::format("{} The saved quota is {} bytes. It keeps its exact value until "
                                           "you change this field.",
                                           help, *bytes);
            help = saved_quota_help;
        }
    }
    auto [input, inserted] = state.numbers.try_emplace(field.path);
    if (inserted) {
        input->second = display_number(state.selected_user, field);
    }
    edit_text(field.path, field.label, input->second, field.hint, help, ImGuiInputTextFlags_CharsDecimal);
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
        const std::pair<const char*, std::string*> entries[] = {
            {"name", &state.name}, {"email", &state.email}, {"password", &state.password}};
        for (const auto& [role, input] : entries) {
            const auto& field = user_field(role);
            if (!field.is_object()) {
                continue;
            }
            ImGui::TableNextColumn();
            const bool secret = std::string_view(role) == "password";
            edit_text(role, view::string_field(field, "label").data(), *input,
                      view::string_field(field, secret && !creating ? "edit_hint" : "hint").data(),
                      view::string_field(field, "help"),
                      secret && creating ? ImGuiInputTextFlags_Password : ImGuiInputTextFlags_None);
        }
        if (const auto& id = user_field("id"); id.is_object()) {
            ImGui::TableNextColumn();
            if (creating) {
                edit_number(state, number_field(id));
            } else {
                ImGui::AlignTextToFramePadding();
                view::text(view::string_field(id, "label"));
                view::help_marker(view::string_field(id, "edit_help"));
                view::wrapped(state.user_id, Color::secondary_text);
            }
        }
        ImGui::EndTable();
    }
    if (const auto& notes = user_field("notes"); notes.is_object()) {
        view::text(view::string_field(notes, "label"));
        ImGui::InputTextMultiline("##user-notes", &state.notes, {-1, 84});
    }
}

void render_access(ConfigUsersState& state, bool narrow)
{
    if (const auto& enabled = user_field("enabled"); enabled.is_object()) {
        ImGui::Checkbox(view::string_field(enabled, "label").data(), &state.enabled);
        view::help_marker(view::string_field(enabled, "help"));
    }
    if (begin_fields("Access fields", narrow)) {
        if (const auto& group = user_field("group"); group.is_object()) {
            ImGui::TableNextColumn();
            edit_number(state, number_field(group));
        }
        if (const auto& network = user_field("network"); network.is_object()) {
            ImGui::TableNextColumn();
            edit_text("network", view::string_field(network, "label").data(), state.network,
                      view::string_field(network, "hint").data(), view::string_field(network, "help"));
        }
        for (const auto& descriptor : view::array_field(user_ui(), "access_fields")) {
            ImGui::TableNextColumn();
            edit_number(state, number_field(descriptor));
        }
        ImGui::EndTable();
    }
}

void render_limits(ConfigUsersState& state, bool narrow)
{
    view::wrapped(view::string_field(user_ui(), "limits_help"), Color::secondary_text);
    for (const auto& group : view::array_field(user_ui(), "limit_groups")) {
        const auto label = view::string_field(group, "label");
        ImGui::SeparatorText(label.data());
        if (begin_fields(label.data(), narrow)) {
            for (const auto& descriptor : view::array_field(group, "fields")) {
                ImGui::TableNextColumn();
                edit_number(state, number_field(descriptor));
            }
            ImGui::EndTable();
        }
    }
}

void render_activity(const Json& user)
{
    for (const auto& group : view::array_field(user_ui(), "activity_groups")) {
        const auto label = view::string_field(group, "label");
        ImGui::SeparatorText(label.data());
        if (!ImGui::BeginTable(label.data(), 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            continue;
        }
        for (const auto& descriptor : view::array_field(group, "fields")) {
            const auto reading = number_field(descriptor);
            ImGui::TableNextColumn();
            view::wrapped(reading.label);
            ImGui::TableNextColumn();
            auto value = number_text(user, reading.path);
            if (reading.gigabytes) {
                if (const auto bytes = parse_unsigned(value)) {
                    const auto megabytes = *bytes / bytes_per_megabyte;
                    value = *bytes > 0 && megabytes == 0 ? "<0.001" : gigabytes_from_megabytes(megabytes);
                }
            }
            view::wrapped(value.empty() ? "Not recorded" : std::string_view(value), Color::secondary_text);
        }
        ImGui::EndTable();
    }
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
    const auto password_key = user_key("password");
    const bool password_changed = !password_key.empty() && !state.password.empty() &&
                                  (creating || state.password != state.saved_password);
    const bool changed = !patch.fields.empty() || password_changed;
    ImGui::BeginDisabled(!writable || !state.validation.empty() ||
                         (creating && !password_key.empty() && !password_changed) ||
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
            if (password_changed) {
                patch.fields[std::string(password_key)] = state.password;
            }
            request["user"] = std::move(patch.fields);
        }
        submit(state, session, std::move(request), Operation::mutation);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.request && state.operation != Operation::password);
    if (theme::button("Cancel")) {
        session.abandon(state.request);
        select_user(state, Editor::none);
    }
    ImGui::EndDisabled();
    if (state.request && !state.passive_read) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (!state.validation.empty()) {
        view::wrapped(state.validation, Color::error);
    }
    if (state.password_unavailable && state.section == Section::account) {
        view::wrapped("Could not load the saved password.", Color::warning);
    }
}

void render_users(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                  bool writable, bool narrow)
{
    constexpr char default_group[] = "Default";
    constexpr char enabled_label[] = "Enabled";
    constexpr char disabled_label[] = "Disabled";
    if (!ImGui::BeginTable("Offline users", narrow ? 2 : 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("User");
    if (!narrow) {
        float group_width = std::max(ImGui::CalcTextSize("Group").x, ImGui::CalcTextSize(default_group).x);
        for (const auto& user : state.users) {
            const auto group = view::string_field(user, "gid", default_group);
            group_width =
                std::max(group_width, ImGui::CalcTextSize(group.data(), group.data() + group.size()).x);
        }
        const float state_width =
            std::max({ImGui::CalcTextSize("State").x, ImGui::CalcTextSize(enabled_label).x,
                      ImGui::CalcTextSize(disabled_label).x});
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, group_width);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, state_width);
    }
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    const auto cell_text = [](std::string_view value, Color color) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(color));
        view::text(value);
        ImGui::PopStyleColor();
    };
    for (const auto& user : state.users) {
        const auto id = view::string_field(user, user_key("id"));
        const auto name = view::string_field(user, user_key("name"));
        const auto email = view::string_field(user, user_key("email"));
        const auto group = view::string_field(user, "gid");
        const bool enabled = view::bool_field(user, "enabled");
        const auto status = enabled ? enabled_label : disabled_label;
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
            cell_text(group.empty() ? default_group : group, Color::secondary_text);
            ImGui::TableNextColumn();
            cell_text(status, status_color);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!writable);
        ImGui::BeginDisabled(oversized);
        if (theme::small_button("Edit")) {
            select_user(state, Editor::edit, user);
            submit(state, session,
                   {{"action", "get_config_user_password"},
                    {"config_id", config_id},
                    {"node", node},
                    {"user_id", state.user_id}},
                   Operation::password);
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
    if (!state.users.is_array() || state.page_count <= 1) {
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
    const bool pending_editor_request = state.request && state.operation != Operation::list;
    if (state.last_frame != frame - 1 && !pending_editor_request) {
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
    if (state.request && !state.passive_read) {
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
                render_users(state, session, config_id, node, writable && state.editor == Editor::none,
                             narrow);
            }
        }
    }
    ImGui::EndChild();
}

std::string describe_config_users(const ConfigUsersState& state)
{
    std::string description(view::string_field(user_ui(), "offline_description", "Offline users."));
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
        switch (state.operation) {
        case Operation::list:
            description += "Loading users.";
            break;
        case Operation::password:
            description += "Loading saved password.";
            break;
        case Operation::mutation:
            description += busy_description;
            break;
        }
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
