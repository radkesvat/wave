#include "config_users_view.h"

#include "dialog.h"
#include "schematic_users_ui.h"
#include "unicode_casefold.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <emscripten.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <tuple>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Editor = ConfigUsersState::Editor;
using Detail = ConfigUsersState::Detail;
using Column = ConfigUsersState::Column;
using Mutation = ConfigUsersState::Mutation;
using Source = ConfigUsersState::Source;
using Section = ConfigUsersState::Section;
using theme::Color;
using view::Json;


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
    if (text.empty()) {
        return std::nullopt;
    }
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

void clear_secrets(ConfigUsersState& state)
{
    clear_password(state.password);
    clear_password(state.saved_password);
    if (!state.password_input_id) {
        return;
    }
    auto& context = *ImGui::GetCurrentContext();
    if (context.ActiveId == state.password_input_id) {
        ImGui::ClearActiveID();
    }
    if (auto* input = ImGui::GetInputTextState(state.password_input_id)) {
        std::fill(input->TextA.begin(), input->TextA.end(), '\0');
        std::fill(input->TextToRevertTo.begin(), input->TextToRevertTo.end(), '\0');
        std::fill(input->CallbackTextBackup.begin(), input->CallbackTextBackup.end(), '\0');
        input->ID = 0;
    }
    auto& deactivated = context.InputTextDeactivatedState;
    if (deactivated.ID == state.password_input_id) {
        std::fill(deactivated.TextA.begin(), deactivated.TextA.end(), '\0');
        deactivated.ID = 0;
    }
    state.password_input_id = 0;
}

void select_user(ConfigUsersState& state, Editor editor, const Json& user = {})
{
    state.editor = editor;
    state.section = Section::account;
    state.open_account = editor == Editor::add || editor == Editor::edit;
    state.edit_token.clear();
    state.review_token.clear();
    state.review_user = nullptr;
    state.review_required = false;
    state.draft_from_roster = false;
    state.draft_removed = false;
    state.draft_scope_lost = false;
    state.changed_fields.clear();
    state.selected_user = user;
    state.user_id = view::string_field(user, "id");
    state.detail_clock = {};
    state.name = view::string_field(user, user_key("name"));
    state.email = view::string_field(user, user_key("email"));
    state.notes = view::string_field(user, user_key("notes"));
    state.network = view::string_field(user, user_key("network"));
    clear_secrets(state);
    if (editor != Editor::none) {
        state.mutation_message.clear();
    }
    state.enabled = view::bool_field(user, user_key("enabled"), true);
    state.numbers.clear();
    state.kept_numbers.clear();
    if (editor == Editor::add || editor == Editor::edit) {
        for (const auto& field : editable_numbers(editor == Editor::add)) {
            state.numbers.emplace(field.path, display_number(user, field));
        }
    }
    state.validation.clear();
    state.detail_error.clear();
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
        if (input == state.numbers.end() ||
            (input->second == display_number(state.selected_user, field) && !state.kept_numbers.contains(field.path))) {
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

ImGuiID edit_text(const char* id, const char* label, std::string& text, const char* hint, std::string_view help,
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
    const auto item_id = ImGui::GetItemID();
    ImGui::PopID();
    return item_id;
}

void edit_number(ConfigUsersState& state, const NumberField& field)
{
    std::string saved_quota_help;
    auto help = field.help;
    // Existing installations can still use a cached schematic with the old example.
    if (help == "Bandwidth limits are rates for each direction. Enter 1048576 for 1 MiB/s.") {
        help = "Bandwidth limits are rates for each direction. Enter 1000000 for 1 MB/s.";
    }
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

void render_account(ConfigUsersState& state, bool creating, bool narrow, std::optional<std::uint64_t> suggested_id)
{
    if (begin_fields("Account fields", narrow)) {
        const std::pair<const char*, std::string*> entries[] = {
            {"name", &state.name}, {"email", &state.email}, {"password", &state.password}};
        for (const auto& [role, input] : entries) {
            const auto& field = user_field(role);
            const bool secret = std::string_view(role) == "password";
            if (!field.is_object() || (secret && !creating && state.source == Source::live)) {
                continue;
            }
            ImGui::TableNextColumn();
            ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_None;
            if (secret) {
                input_flags |= ImGuiInputTextFlags_NoUndoRedo;
                if (creating) {
                    input_flags |= ImGuiInputTextFlags_Password;
                }
            }
            const auto item_id = edit_text(role, view::string_field(field, "label").data(), *input,
                                           view::string_field(field, secret && !creating ? "edit_hint" : "hint").data(),
                                           view::string_field(field, "help"), input_flags);
            if (secret) {
                state.password_input_id = item_id;
            }
        }
        if (const auto& id = user_field("id"); id.is_object()) {
            ImGui::TableNextColumn();
            if (creating) {
                auto field = number_field(id);
                std::string hint;
                if (suggested_id) {
                    const auto value = field.gigabytes ? gigabytes_from_megabytes(*suggested_id / bytes_per_megabyte)
                                                       : std::to_string(*suggested_id);
                    hint = "Next ID · " + value;
                    field.hint = hint.c_str();
                }
                edit_number(state, field);
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

std::string usage_label(const std::optional<UserTraffic>& traffic);
std::string rate_label(const UsersRosterRow* row, bool upload);
const UsersRosterRow* live_rate_row(const ConfigUsersState& state, const PreviewSession& session, std::string_view id);

void render_activity(const Json& user, const UsersRosterRow* live_row)
{
    bool showed_usage = false;
    for (const auto& group : view::array_field(user_ui(), "activity_groups")) {
        const auto label = view::string_field(group, "label");
        ImGui::SeparatorText(label.data());
        if (!ImGui::BeginTable(label.data(), 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            continue;
        }
        for (const auto& descriptor : view::array_field(group, "fields")) {
            const auto reading = number_field(descriptor);
            const std::string_view path = reading.path;
            std::string_view field_label = reading.label;
            std::string value;
            if (path == "/stats/traffic/up" || path == "/stats/traffic/down") {
                if (showed_usage) {
                    continue;
                }
                showed_usage = true;
                field_label = "Traffic usage";
                const auto& traffic = view::field(view::field(user, "stats"), "traffic");
                value = usage_label(live_row ? live_row->traffic : user_traffic(traffic, true));
            } else if (path == "/stats/speed/up" || path == "/stats/speed/down") {
                // The saved speed fields have no measurement time. Use fresh roster observations.
                const bool upload = path == "/stats/speed/up";
                field_label = upload ? "Avg upload speed" : "Avg download speed";
                value = rate_label(live_row, upload);
            } else {
                value = number_text(user, reading.path);
                if (reading.gigabytes) {
                    if (const auto bytes = parse_unsigned(value)) {
                        const auto megabytes = *bytes / bytes_per_megabyte;
                        value = *bytes > 0 && megabytes == 0 ? "<0.001" : gigabytes_from_megabytes(megabytes);
                    }
                }
            }
            ImGui::TableNextColumn();
            view::wrapped(field_label);
            ImGui::TableNextColumn();
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

bool allowed(const ConfigUsersState& state, std::string_view operation)
{
    return state.source == Source::live && state.grants.is_array() &&
           std::ranges::any_of(state.grants, [operation](const Json& grant) {
               return view::string_value(grant) == operation;
           });
}

const char* source_name(Source source)
{
    switch (source) {
    case Source::offline: return "offline";
    case Source::live: return "live";
    case Source::blocked: return "blocked";
    case Source::transition: return "transition";
    }
    return "";
}

Json request_for(const ConfigUsersState& state, const char* action, int config_id, std::string_view node)
{
    Json request{{"action", action}, {"config_id", config_id}, {"node", node}};
    if (state.source == Source::offline || !state.run_id.empty()) {
        request["source"] = source_name(state.source);
        request["run_id"] = state.run_id;
    }
    return request;
}

std::string decimal(const Json& value)
{
    return value.is_number_integer() ? value.dump() : std::string(view::string_value(value));
}

bool is_decimal(std::string_view value)
{
    return !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
}

std::string_view trim_query(std::string_view value)
{
    constexpr std::string_view whitespace = " \n\r\t\v\f";
    const auto first = value.find_first_not_of(whitespace);
    return first == value.npos ? std::string_view{}
                               : value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

std::string exact_id(std::string_view text)
{
    const auto value = parse_unsigned(trim_query(text));
    return value && *value != 0 ? std::to_string(*value) : std::string{};
}

bool has_search(const ConfigUsersState& state)
{
    return state.search_is_id || !state.search_key.empty();
}

bool not_found(const Json& data)
{
    const auto code = view::string_field(data, "code");
    return code == "not_found" || code == "not-found";
}

bool can_list(const ConfigUsersState& state)
{
    return state.source == Source::offline || allowed(state, "list-users");
}

struct CreateIdSuggestion {
    std::optional<std::uint64_t> id;
    std::string_view issue;
};

CreateIdSuggestion suggest_create_id(const ConfigUsersState& state, const PreviewSession& session)
{
    if (state.mutation_request && state.mutation == Mutation::create && state.mutation_source == Source::live) {
        return {parse_unsigned(state.mutation_id), {}};
    }
    const auto* roster = session.users_roster(state.subscription);
    const bool list_denied = roster && roster->state == UsersRoster::State::lost &&
                             roster->loss_cause == UsersRoster::LossCause::permission;
    if (!can_list(state) || !session.supports_users_roster() || list_denied) {
        return {};
    }
    if (!roster || !roster->sequence || roster->state != UsersRoster::State::ready) {
        return {{}, state.source == Source::live ? "Wait for the account list to refresh, or enter an unused ID." : ""};
    }
    const auto& field = user_field("id");
    const auto bounds = schematic_user_number_bounds(field, explicit_user_id_bounds);
    const auto id = next_sequential_user_id(roster->baseline, bounds,
                                            view::bool_field(field, "gigabytes"), state.recent_live_id);
    return {id, id ? "" : exhausted_user_ids};
}

bool list_stale(const UsersRoster* roster)
{
    return roster && (roster->state == UsersRoster::State::stale || roster->state == UsersRoster::State::error ||
                      roster->state == UsersRoster::State::lost);
}

void ensure_roster(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                   bool replace = false)
{
    if ((!replace && (session.users_roster(state.subscription) || !state.binding_error.empty())) ||
        !state.list_error.empty() || !can_list(state) ||
        !session.ready() || !session.document_visible() || !session.supports_users_roster()) {
        return;
    }
    auto target = request_for(state, "users_roster_subscribe", config_id, node);
    if (!state.database_generation.empty()) {
        target["database_generation"] = state.database_generation;
    }
    const auto subscription = session.subscribe_users_roster(std::move(target), replace);
    if (!subscription) {
        state.list_error = "The account list connection is unavailable.";
        return;
    }
    state.subscription = subscription;
    state.roster_generation = 0;
    state.roster_commits = 0;
    state.order_dirty = true;
    state.list_error.clear();
}

void retry_roster(ConfigUsersState& state, PreviewSession& session)
{
    state.list_error.clear();
    if (!session.ready()) {
        session.start(true);
        return;
    }
    const auto* roster = session.users_roster(state.subscription);
    if (!roster || roster->state == UsersRoster::State::lost || !state.binding_error.empty()) {
        ensure_roster(state, session, state.config_id, state.node, true);
    } else {
        session.refresh_users_roster(state.subscription);
    }
}

bool begin_detail(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                  std::string_view id, Detail purpose)
{
    const auto target = std::string(id);
    if (target.empty() || state.detail_request || !session.ready() || !state.binding_error.empty()) {
        return false;
    }
    const char* action = state.source == Source::live ? "get_live_config_user"
                         : purpose == Detail::saved_password ? "get_config_user_password"
                                                              : "get_config_user";
    auto request = request_for(state, action, config_id, node);
    request["user_id"] = target;
    state.detail = purpose;
    state.detail_id = target;
    state.detail_error.clear();
    state.detail_request = session.submit(std::move(request), 5000);
    if (!state.detail_request) {
        state.detail_error = "The account connection is unavailable.";
    }
    return state.detail_request.has_value();
}

void start_mutation(ConfigUsersState& state, PreviewSession& session, Json request, Mutation operation,
                    std::string_view id)
{
    state.mutation = operation;
    state.mutation_id = id;
    state.mutation_source = state.source;
    state.mutation_run = state.run_id;
    state.mutation_database = state.database_generation;
    if (operation == Mutation::create && state.source == Source::live) {
        state.create_target = std::format("account {} in config {} (node {})", id, request.at("config_id").get<int>(),
                                          view::string_field(request, "node"));
    }
    state.mutation_context_changed = false;
    state.create_target_lost = false;
    if (state.source == Source::offline) {
        request["roster"] = true;
    }
    state.mutation_message.clear();
    state.mutation_uncertain = false;
    state.missing_reconciled_create = false;
    state.reconcile_needed = false;
    state.mutation_request = session.submit(std::move(request), 5000);
    if (!state.mutation_request) {
        state.mutation_message = "The request was unavailable before it was sent.";
    }
    if (state.source == Source::live) {
        clear_secrets(state);
    }
}

void changed_fields(const Json& patch, const Json& original, const Json& current,
                    std::vector<std::string>& changes, const std::string& prefix = {})
{
    for (const auto& [key, value] : patch.items()) {
        const auto path = prefix + '/' + key;
        if (value.is_object()) {
            changed_fields(value, view::field(original, key), view::field(current, key), changes, path);
        } else if (view::field(original, key) != view::field(current, key)) {
            changes.push_back(path);
        }
    }
}

std::string field_label(std::string_view path)
{
    for (const auto& field : editable_numbers(false)) {
        if (path == field.path) {
            return field.label;
        }
    }
    for (const auto* role : {"name", "email", "notes", "network", "enabled"}) {
        if (path.starts_with('/') && path.substr(1) == user_key(role)) {
            return std::string(view::string_field(user_field(role), "label", role));
        }
    }
    return std::string(path);
}

void review_latest(ConfigUsersState& state)
{
    auto original = std::move(state.selected_user);
    const auto& fresh = state.review_user;
    for (auto [role, value] : {std::pair{"name", &state.name}, {"email", &state.email},
                               {"notes", &state.notes}, {"network", &state.network}}) {
        const auto key = user_key(role);
        if (*value == view::string_field(original, key)) {
            *value = view::string_field(fresh, key);
        }
    }
    const auto key = user_key("enabled");
    if (state.enabled == view::bool_field(original, key, true)) {
        state.enabled = view::bool_field(fresh, key, true);
    }
    for (const auto& field : editable_numbers(false)) {
        auto& value = state.numbers[field.path];
        if (value == display_number(original, field) && !state.kept_numbers.contains(field.path)) {
            value = display_number(fresh, field);
        } else {
            state.kept_numbers.emplace(field.path);
        }
    }
    state.selected_user = std::move(state.review_user);
    state.edit_token = std::move(state.review_token);
    state.review_required = false;
    state.changed_fields.clear();
    state.detail_error.clear();
    state.mutation_uncertain = false;
    state.mutation_message.clear();
}

void apply_detail(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                  const PreviewReply& reply)
{
    const auto& data = reply.result;
    if (state.detail == Detail::saved_password) {
        const auto password = reply.ok() ? view::string_field(data, "password") : std::string_view{};
        if (password.empty()) {
            state.detail_error = "Could not load the saved password.";
        } else {
            state.saved_password = password;
            if (state.password.empty()) {
                state.password = password;
            }
        }
        return;
    }

    const auto* user = &view::field(data, "user");
    const auto token = view::string_field(data, "if_match", view::string_field(data, "edit_token"));
    const auto id = view::string_field(*user, "id");
    if (!reply.ok() || state.detail_id != id || (state.source == Source::live && token.size() != 48)) {
        if (not_found(data) || (state.source == Source::offline && reply.ok() && id.empty())) {
            state.detail_error = std::format("Account {} could not be found.", state.detail_id);
            if (state.detail == Detail::review && state.detail_id == state.user_id) {
                state.draft_removed = true;
            }
            state.missing_reconciled_create = state.detail == Detail::reconcile &&
                                              state.mutation == Mutation::create &&
                                              state.mutation_source == state.source &&
                                              state.mutation_run == state.run_id;
        } else {
            state.detail_error = reply.message.empty() ? "Could not load account details." : reply.message;
        }
        return;
    }

    switch (state.detail) {
    case Detail::open:
    case Detail::open_saved_password:
    case Detail::lookup:
    case Detail::password: {
        const auto purpose = state.detail;
        if (purpose != Detail::lookup && state.selection != id) {
            break;
        }
        const auto* roster = session.users_roster(state.subscription);
        const auto numeric_id = parse_unsigned(id);
        const bool from_roster = purpose != Detail::lookup && roster && roster->sequence;
        if (from_roster && numeric_id && !roster->baseline.contains(*numeric_id)) {
            state.detail_error = std::format("Account {} is no longer in the roster.", id);
            break;
        }
        select_user(state, purpose == Detail::password ? Editor::password : Editor::edit, *user);
        state.draft_from_roster = from_roster;
        state.edit_token = token;
        state.detail_clock = users_clock_samples(view::field(data, "host_time_ms"),
                                                view::field(data, "accounting_time_ms"), reply.received_at);
        if (purpose == Detail::open_saved_password) {
            begin_detail(state, session, config_id, node, id, Detail::saved_password);
        }
        break;
    }
    case Detail::toggle: {
        const auto* roster = session.users_roster(state.subscription);
        const auto number = parse_unsigned(id);
        const bool present = roster && roster->fresh && number && roster->baseline.contains(*number);
        if (view::bool_field(*user, "enabled") != state.expected_enabled ||
            !allowed(state, "get-user") || !allowed(state, "update-user") || !present) {
            state.mutation_message = "The enabled state or access changed. Review the current account.";
            return;
        }
        Json patch{{"enabled", !state.expected_enabled}};
        auto request = request_for(state, "edit_live_config_user", config_id, node);
        request["user_id"] = id;
        request["if_match"] = token;
        request["user"] = patch;
        start_mutation(state, session, std::move(request), Mutation::toggle, id);
        break;
    }
    case Detail::review:
    case Detail::reconcile:
        if (state.editor == Editor::edit && state.user_id == id) {
            state.review_user = *user;
            state.review_token = token;
            state.detail_clock = users_clock_samples(view::field(data, "host_time_ms"),
                                                    view::field(data, "accounting_time_ms"), reply.received_at);
            state.review_required = true;
            state.changed_fields.clear();
            changed_fields(pending_patch(state, false).fields, state.selected_user, *user, state.changed_fields);
        } else if (state.detail == Detail::review && state.editor == Editor::password &&
                   state.user_id == id && !state.mutation_uncertain) {
            state.edit_token = token;
            state.review_required = false;
        } else if (state.detail == Detail::reconcile) {
            state.detail_error = std::format("Account {} exists. Review its current details.", id);
        }
        if (state.detail == Detail::reconcile && state.mutation == Mutation::password) {
            state.detail_error = "Live details cannot verify the password. Verify the credential separately.";
        }
        break;
    case Detail::saved_password:
        break;
    }
}

bool can_lookup(const ConfigUsersState& state)
{
    return state.source == Source::offline ? state.binding_error.empty() : allowed(state, "get-user");
}

bool can_edit(const ConfigUsersState& state)
{
    return state.source == Source::offline ? state.editable && state.binding_error.empty()
                                           : allowed(state, "get-user") && allowed(state, "update-user");
}

bool can_create(const ConfigUsersState& state)
{
    return state.source == Source::offline ? state.editable && state.binding_error.empty()
                                           : allowed(state, "create-user");
}

bool can_change_password(const ConfigUsersState& state)
{
    return allowed(state, "get-user") && allowed(state, "change-user-password");
}

bool can_review_draft(const ConfigUsersState& state)
{
    return !state.draft_scope_lost && !state.draft_removed &&
           (state.editor == Editor::edit ? can_edit(state)
                                          : state.editor == Editor::password && !state.mutation_uncertain &&
                                                can_change_password(state));
}

void apply_mutation(ConfigUsersState& state, PreviewSession& session, const PreviewReply& reply)
{
    const auto& data = reply.result;
    const auto outcome = view::string_field(data, "outcome");
    const auto id = view::string_field(data, "user_id", view::string_field(data, "id"));
    const bool was_live = state.mutation_source == Source::live;
    const bool assigned = !was_live && state.mutation == Mutation::create && state.mutation_id.empty();
    const bool applied = (!state.mutation_context_changed || state.create_target_lost) && reply.ok() &&
                         !id.empty() && (assigned || id == state.mutation_id) && (!was_live || outcome == "applied");
    const bool live_create = was_live && state.mutation == Mutation::create;
    const bool duplicate = live_create && reply.status == PreviewReply::Status::rejected &&
                           outcome == "rejected" && view::string_field(data, "code") == "id_exists";
    if (live_create && (applied || duplicate) && !state.create_target_lost && !state.mutation_context_changed) {
        state.recent_live_id = std::max(state.recent_live_id, parse_unsigned(state.mutation_id).value_or(0));
    }
    if (applied) {
        state.mutation_message.clear();
        if (state.mutation != Mutation::toggle && !state.create_target_lost) {
            select_user(state, Editor::none);
        }
        if (was_live) {
            if (state.mutation == Mutation::create) {
                session.add_local_log("success", "WaterWall confirmed creation of " + state.create_target + '.');
                if (state.create_target_lost) {
                    state.mutation_message =
                        "WaterWall confirmed creation of " + state.create_target + " in the original database.";
                    return;
                }
                state.selection = state.mutation_id;
                state.selection_from_roster = false;
            }
        } else if (state.mutation == Mutation::remove && state.selection == state.mutation_id) {
            state.selection.clear();
            state.selection_from_roster = false;
        }
        if (!reply.feedback_in_logs_only && !live_create) {
            if (!reply.message.empty()) {
                session.add_local_log("success", reply.message);
            } else if (was_live) {
                session.add_local_log("success", "Applied to the running core. WaterWall saves it on its normal schedule.");
            }
        }
        return;
    }

    if ((state.mutation_context_changed && outcome != "not_sent" && outcome != "rejected") || outcome == "unknown" ||
        reply.status == PreviewReply::Status::timed_out ||
        reply.status == PreviewReply::Status::disconnected || reply.ok()) {
        if (live_create) {
            auto receipt = "Outcome unknown for " + state.create_target +
                           ". Check the original database before submitting another create.";
            session.add_local_log("warning", receipt);
            if (state.create_target_lost) {
                state.mutation_message = std::move(receipt);
                return;
            }
            if (state.editor == Editor::add) {
                const auto field = number_field(user_field("id"));
                state.numbers[field.path] = display_number(Json{{"id", state.mutation_id}}, field);
            }
        }
        state.mutation_uncertain = true;
        state.mutation_message = state.mutation_id.empty()
                                     ? "Outcome unknown. Refresh the database before taking another action."
                                     : std::format("Outcome unknown for account {}. Check its current state before "
                                                   "taking another action.", state.mutation_id);
        state.reconcile_needed = !state.mutation_id.empty();
    } else if (duplicate && !state.create_target_lost && !state.mutation_context_changed) {
        session.refresh_users_roster(state.subscription);
        state.mutation_message = std::format("Account {} already exists. Review the ID and re-enter the password.",
                                             state.mutation_id);
    } else if (outcome == "conflict") {
        state.mutation_message = "Account configuration changed. Review the current values before saving.";
        state.review_required = state.editor == Editor::edit;
        state.reconcile_needed = state.review_required;
        if (state.mutation == Mutation::password) {
            // Reopening the password action obtains a new token and requires a fresh credential.
            select_user(state, Editor::none);
        }
    } else if (was_live || !reply.feedback_in_logs_only || reply.status != PreviewReply::Status::rejected) {
        state.mutation_message = reply.message.empty()
                                     ? "Wave could not confirm the change. Refresh users before retrying."
                                     : reply.message;
        if (was_live && outcome == "not_sent" && state.editor == Editor::edit && state.edit_token.empty()) {
            state.review_required = true;
        }
    }
}

void updates(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    if (auto reply = session.take_reply(state.detail_request)) {
        apply_detail(state, session, config_id, node, *reply);
    }
    if (auto reply = session.take_reply(state.mutation_request)) {
        apply_mutation(state, session, *reply);
    }
    if (auto reply = session.take_reply(state.status_request)) {
        const auto& status = view::field(reply->result, "status");
        if (reply->ok() && status.is_object()) {
            state.database_status = status;
            state.status_error.clear();
        } else {
            state.status_error = reply->message.empty() ? "Could not load database status." : reply->message;
        }
    }
    if (!session.ready() || !state.binding_error.empty() ||
        (state.source != Source::live && state.source != Source::offline)) {
        return;
    }

    if (state.reconcile_needed && !state.detail_request && !state.draft_scope_lost) {
        const bool original_database = state.mutation_database.empty() ? state.mutation_run == state.run_id
                                                                      : state.mutation_database == state.database_generation;
        if (state.mutation == Mutation::create && state.mutation_source == Source::live &&
            !original_database) {
            state.reconcile_needed = false;
            state.detail_error = "Check the original database for this account ID. Its source has changed.";
        } else if (can_lookup(state)) {
            if (begin_detail(state, session, config_id, node, state.mutation_id, Detail::reconcile)) {
                state.reconcile_needed = false;
            }
        } else {
            state.detail_error = "Access does not include account lookup. Verify the account using "
                                 "an authorized client or inspect the database after WaterWall stops.";
        }
    }
    if (state.review_required && state.detail_error.empty() && state.review_user.is_null() &&
        !state.detail_request && !state.mutation_request && can_review_draft(state)) {
        begin_detail(state, session, config_id, node, state.user_id, Detail::review);
    }
}

const char* editor_title(const ConfigUsersState& state)
{
    switch (state.editor) {
    case Editor::add: return "Add user";
    case Editor::edit:
        return state.source == Source::live && !can_edit(state) ? "Account details" : "Edit user";
    case Editor::remove: return "Delete user";
    case Editor::password: return "Change password";
    case Editor::reset_traffic: return "Reset traffic";
    case Editor::none: return "";
    }
    return "";
}

std::string reset_traffic_prompt(std::string_view id)
{
    return std::format("Reset charged upload and download to zero for account {}?", id);
}

void save_editor(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                 UserPatch patch, bool password_changed, std::optional<std::uint64_t> suggested_id)
{
    if (state.editor == Editor::remove || state.editor == Editor::reset_traffic) {
        const bool reset = state.editor == Editor::reset_traffic;
        auto request = request_for(state, reset ? "reset_config_user_traffic" : "delete_config_user", config_id, node);
        request["user_id"] = state.user_id;
        start_mutation(state, session, std::move(request), reset ? Mutation::reset_traffic : Mutation::remove,
                       state.user_id);
        return;
    }
    const bool creating = state.editor == Editor::add;
    const bool password_only = state.editor == Editor::password;
    const bool live = state.source == Source::live;
    const char* action = live ? creating        ? "create_live_config_user"
                              : password_only ? "change_live_config_user_password"
                                              : "edit_live_config_user"
                              : creating      ? "create_config_user"
                                              : "edit_config_user";
    auto request = request_for(state, action, config_id, node);
    const auto mutation = creating ? Mutation::create : password_only ? Mutation::password : Mutation::edit;
    std::string id = creating ? decimal(view::field(patch.fields, "id")) : state.user_id;
    if (live && creating && id.empty()) {
        if (!suggested_id) {
            const auto field = number_field(user_field("id"));
            try {
                std::random_device random;
                std::uniform_int_distribution<std::uint64_t> distribution(field.bounds.minimum, field.bounds.maximum);
                const auto assigned = distribution(random);
                suggested_id = field.gigabytes ? assigned * bytes_per_megabyte : assigned;
            } catch (const std::system_error&) {
                state.mutation_message = "Could not assign an ID. Enter an explicit account ID.";
                return;
            }
        }
        id = std::to_string(*suggested_id);
        patch.fields["id"] = id;
    }
    if (live && !creating) {
        request["if_match"] = state.edit_token;
    }
    if (!creating || live) {
        request["user_id"] = id;
    }
    if (password_only) {
        request["password"] = state.password;
    } else {
        if (password_changed) {
            patch.fields[std::string(user_key("password"))] = state.password;
        }
        request["user"] = std::move(patch.fields);
    }
    start_mutation(state, session, std::move(request), mutation, id);
}

void render_feedback(const ConfigUsersState& state)
{
    if (!state.detail_error.empty()) {
        view::wrapped(state.detail_error, Color::warning);
    }
    if (!state.mutation_message.empty()) {
        view::wrapped(state.mutation_message, state.mutation_uncertain ? Color::warning : Color::secondary_text);
    }
}

void render_review(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    if (state.missing_reconciled_create && state.mutation_uncertain && state.editor == Editor::add) {
        if (theme::button("Review draft and keep account ID")) {
            state.mutation_uncertain = false;
            state.missing_reconciled_create = false;
            state.review_required = false;
            state.mutation_message.clear();
            state.detail_error.clear();
        }
        return;
    }
    if (!state.review_required) {
        return;
    }
    if (state.editor == Editor::add) {
        view::wrapped("The account source changed. Review the draft for this database.", Color::warning);
        ImGui::BeginDisabled(!can_create(state) || state.mutation_uncertain || state.mutation_request);
        if (theme::button("Review draft for this source")) {
            state.review_required = false;
            state.detail_error.clear();
        }
        ImGui::EndDisabled();
        return;
    }
    if (state.review_user.is_object()) {
        if (state.changed_fields.empty()) {
            view::wrapped("Review the current account before resubmitting your draft.", Color::warning);
        } else {
            std::string changes = "Changed since you opened the draft";
            for (const auto& field : state.changed_fields) {
                changes += changes.ends_with("draft") ? " · " : ", ";
                changes += field_label(field);
            }
            view::wrapped(changes, Color::warning);
        }
        for (const auto& field : state.changed_fields) {
            const Json::json_pointer path(field);
            const auto& user = state.review_user;
            const auto current = user.contains(path) ? &user.at(path) : nullptr;
            auto value = !current || current->is_null() ? "Unset"
                       : current->is_string() && current->get_ref<const std::string&>().empty() ? "Empty"
                       : current->is_string() ? current->get<std::string>()
                                              : current->dump();
            for (const auto& number : editable_numbers(false)) {
                if (field == number.path && number.gigabytes && current && !current->is_null()) {
                    value = std::format("{} ({} bytes)", display_number(user, number), value);
                    break;
                }
            }
            view::wrapped(std::format("Current {} · {}", field_label(field), value), Color::secondary_text);
        }
        if (theme::button("Review latest values and keep draft")) {
            review_latest(state);
        }
    } else if (!state.detail_request && !state.mutation_request && !state.mutation_uncertain) {
        view::wrapped(can_review_draft(state) ? "Fresh account details are required before saving this draft."
                                             : "Access to save this account is unavailable.", Color::warning);
        if (can_review_draft(state) && !state.detail_error.empty() && theme::button("Retry account lookup")) {
            begin_detail(state, session, config_id, node, state.user_id, Detail::review);
        }
    }
}

std::string live_account_summary(const ConfigUsersState& state, const PreviewSession& session);

void render_editor(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                   bool narrow)
{
    const bool creating = state.editor == Editor::add;
    const bool removing = state.editor == Editor::remove;
    const bool resetting = state.editor == Editor::reset_traffic;
    const bool confirm_only = removing || resetting;
    const bool password_only = state.editor == Editor::password;
    const bool online = state.source == Source::live;
    const auto* roster = session.users_roster(state.subscription);
    const bool available = session.ready() && (state.source == Source::offline || state.source == Source::live);
    const bool granted = creating      ? can_create(state)
                         : password_only ? can_change_password(state)
                         : confirm_only  ? state.source == Source::offline && can_edit(state) && roster && roster->fresh
                                         : can_edit(state);
    const bool editable = available && granted && (confirm_only || !view::bool_field(state.selected_user, "oversized"));
    const bool pending = state.mutation_request.has_value() || state.detail_request.has_value();
    const bool writable = editable && !pending && !state.review_required && !state.mutation_uncertain &&
                          !state.draft_scope_lost &&
                          !state.draft_removed && (!online || creating || state.edit_token.size() == 48);
    const auto suggestion = creating ? suggest_create_id(state, session) : CreateIdSuggestion{};
    ImGui::SeparatorText(editor_title(state));
    render_feedback(state);
    if (state.draft_removed) {
        view::wrapped("This account was removed. Its draft can no longer be saved.", Color::warning);
    } else if (state.draft_scope_lost) {
        view::wrapped("The account source could not be verified. Reopen the account before saving.", Color::warning);
    } else {
        render_review(state, session, config_id, node);
    }
    if (removing) {
        const auto& user = state.name.empty() ? state.user_id : state.name;
        view::wrapped(std::format("Delete {} from {}?", user, node));
    } else if (resetting) {
        view::wrapped(reset_traffic_prompt(state.user_id));
    } else if (password_only) {
        view::wrapped(std::format("Account {}", state.user_id));
        ImGui::BeginDisabled(!writable);
        state.password_input_id = edit_text("new-password", "New password", state.password, "Required", {},
                                            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
        ImGui::EndDisabled();
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
                    render_account(state, creating, narrow, suggestion.id);
                    break;
                case Section::access:
                    render_access(state, narrow);
                    break;
                case Section::limits:
                    render_limits(state, narrow);
                    break;
                case Section::activity:
                    render_activity(state.selected_user, live_rate_row(state, session, state.user_id));
                    break;
                }
                ImGui::EndDisabled();
                if (section == Section::account && state.editor == Editor::edit && online) {
                    view::wrapped(live_account_summary(state, session), Color::secondary_text);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    auto patch = confirm_only || password_only ? UserPatch{} : pending_patch(state, creating);
    state.validation = std::move(patch.issue);
    if (creating && state.numbers.at("/id").empty() && state.validation.empty()) {
        state.validation = suggestion.issue;
    }
    const auto password_key = user_key("password");
    const bool password_changed = !state.password.empty() &&
                                  (creating || password_only || state.password != state.saved_password);
    if (granted || creating || confirm_only || password_only) {
        const bool password_required = password_only || (creating && !password_key.empty());
        ImGui::BeginDisabled(!writable || !state.validation.empty() ||
                             (password_required && !password_changed) ||
                             (!creating && !confirm_only && !password_only &&
                              patch.fields.empty() && !password_changed));
        if (theme::button(confirm_only ? editor_title(state)
                          : creating ? "Create user"
                          : password_only ? "Change password###save-password"
                                          : "Save user")) {
            save_editor(state, session, config_id, node, std::move(patch), password_changed, suggestion.id);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(state.mutation_request.has_value());
    if (theme::button(granted && !state.mutation_uncertain ? "Cancel" : "Close")) {
        session.abandon(state.detail_request);
        select_user(state, Editor::none);
        state.mutation_message.clear();
        state.mutation_uncertain = false;
    }
    ImGui::EndDisabled();
    if (state.editor == Editor::edit && can_change_password(state)) {
        ImGui::SameLine();
        const bool ready = available && !pending && !state.review_required && !state.mutation_uncertain &&
                           !state.draft_removed && !state.draft_scope_lost && state.edit_token.size() == 48 &&
                           patch.fields.empty() && state.validation.empty();
        ImGui::BeginDisabled(!ready);
        if (theme::button("Change password")) {
            clear_secrets(state);
            state.editor = Editor::password;
        }
        ImGui::EndDisabled();
        if (!patch.fields.empty()) {
            ImGui::SetItemTooltip("Save or cancel your account edits before changing the password.");
        }
    }
    if (pending) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (!state.validation.empty()) {
        view::wrapped(state.validation, Color::error);
    }
}

std::string format_bytes(double bytes)
{
    constexpr std::array units{"B", "kB", "MB", "GB", "TB", "PB", "EB"};
    std::size_t unit = 0;
    while (unit + 1 < units.size() && bytes >= 1'000) {
        bytes /= 1'000;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", std::round(bytes * 10) / 10) : std::format("{:.1f} {}", bytes, units[unit]);
}

std::string usage_label(const std::optional<UserTraffic>& traffic)
{
    return traffic ? format_bytes(static_cast<double>(traffic->total())) : "Unavailable";
}

std::string rate_label(const UsersRosterRow* row, bool upload)
{
    if (!row || !row->rates) {
        return "Unavailable";
    }
    const double rate = upload ? row->rates->upload : row->rates->download;
    return rate > 0 && rate < 0.1 ? "<0.1 B/s" : format_bytes(rate) + "/s";
}

enum class ExpiryState { timed, pending, unlimited, unavailable };

struct Expiry {
    ExpiryState state;
    std::uint64_t remaining = 0;
};

std::uint64_t elapsed_since(double anchor, double now)
{
    return static_cast<std::uint64_t>(std::max(now - anchor, 0.0));
}

std::uint64_t saturating_subtract(std::uint64_t left, std::uint64_t right)
{
    return left > right ? left - right : 0;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right)
{
    return left + std::min(right, std::numeric_limits<std::uint64_t>::max() - left);
}

Expiry effective_expiry(const UsersRosterRow& row, const UsersClockSamples& clock, Source source, double now)
{
    std::optional<std::uint64_t> remaining;
    bool unavailable = row.limited_detail ||
                       (source == Source::live && (!row.absolute_expiry || !row.relative_expiry));
    bool pending = false;
    const auto elapsed = elapsed_since(clock.received_at, now);
    if (row.absolute_expiry.value_or(0) != 0) {
        if (clock.host) {
            remaining = saturating_subtract(*row.absolute_expiry, saturating_add(*clock.host, elapsed));
        } else {
            unavailable = true;
        }
    }
    if (row.relative_expiry.value_or(0) != 0) {
        if (row.first_use.value_or(0) == 0) {
            pending = true;
        } else if (clock.accounting && source == Source::live) {
            const auto accounting_now = saturating_add(*clock.accounting, elapsed);
            const auto relative = *row.first_use > accounting_now
                                      ? saturating_add(*row.relative_expiry, *row.first_use - accounting_now)
                                      : saturating_subtract(*row.relative_expiry, accounting_now - *row.first_use);
            remaining = remaining ? std::min(*remaining, relative) : relative;
        } else {
            unavailable = true;
        }
    }
    if (remaining && (*remaining == 0 || !unavailable)) {
        return {ExpiryState::timed, *remaining};
    }
    if (unavailable) {
        return {ExpiryState::unavailable};
    }
    return {pending ? ExpiryState::pending : ExpiryState::unlimited};
}

std::string expiry_label(Expiry expiry)
{
    switch (expiry.state) {
    case ExpiryState::pending: return "Awaiting first use";
    case ExpiryState::unlimited: return "Unlimited";
    case ExpiryState::unavailable: return "Unavailable";
    case ExpiryState::timed: {
        constexpr std::uint64_t day_ms = 86'400'000;
        if (expiry.remaining == 0) {
            return "Expired";
        }
        const auto days = expiry.remaining / day_ms + (expiry.remaining % day_ms != 0);
        return std::format("{} {}", days, days == 1 ? "day" : "days");
    }
    }
    return {};
}

std::string account_summary(const UsersRosterRow& row, const UsersClockSamples& clock, Source source,
                            const UsersRosterRow* live_row)
{
    return std::format("{} is {}. Group {}. Traffic usage {}. Avg upload speed {}. Avg download speed {}. Days Left {}.",
                       row.id, row.enabled ? "enabled" : "disabled",
                       row.group.value_or(0) == 0 ? "Default" : std::to_string(*row.group),
                       usage_label(live_row ? live_row->traffic : row.traffic),
                       rate_label(live_row, true), rate_label(live_row, false),
                       expiry_label(effective_expiry(row, clock, source, emscripten_get_now())));
}

std::string live_account_summary(const ConfigUsersState& state, const PreviewSession& session)
{
    const auto row = users_roster_row(state.selected_user, true);
    return row ? "Account " + account_summary(*row, state.detail_clock, state.source,
                                               live_rate_row(state, session, state.user_id)) : std::string{};
}

const UsersRosterRow* find_row(const UsersRoster* roster, std::uint64_t id)
{
    if (!roster) {
        return nullptr;
    }
    const auto& rows = roster->visible_rows();
    const auto it = rows.find(id);
    return it == rows.end() ? nullptr : &it->second;
}

const UsersRosterRow* find_row(const UsersRoster* roster, std::string_view id)
{
    const auto number = parse_unsigned(id);
    return number ? find_row(roster, *number) : nullptr;
}

const UsersRosterRow* live_rate_row(const ConfigUsersState& state, const PreviewSession& session, std::string_view id)
{
    const auto* roster = session.users_roster(state.subscription);
    return state.source == Source::live && session.ready() && roster && roster->fresh && roster->sequence
               ? find_row(roster, id) : nullptr;
}

void invalidate_roster_access(ConfigUsersState& state, PreviewSession& session, UsersRoster::LossCause cause)
{
    using Cause = UsersRoster::LossCause;
    const bool list_denial = cause == Cause::permission && state.source == Source::live;
    const bool saved_config_error = cause == Cause::config_unverified && state.source == Source::offline;
    const bool identity_lost = !list_denial && !saved_config_error;
    state.editable = false;
    state.order_dirty = true;
    state.options_id.clear();
    if (state.selection_from_roster || !list_denial) {
        state.selection.clear();
        state.selection_from_roster = false;
    }
    if (!list_denial) {
        clear_secrets(state);
    }
    if (state.detail_request && state.detail == Detail::reconcile && !identity_lost) {
        state.reconcile_needed = true;
    }
    if (!list_denial || state.detail != Detail::lookup) {
        session.abandon(state.detail_request);
    }
    state.edit_token.clear();
    state.review_token.clear();
    state.review_user = nullptr;
    state.changed_fields.clear();
    if (!state.mutation_uncertain) {
        state.detail_error.clear();
    }
    if (identity_lost) {
        state.recent_live_id = 0;
        if (state.editor == Editor::add) {
            select_user(state, Editor::none);
        } else if (state.editor != Editor::none) {
            state.draft_scope_lost = true;
        }
        if (state.mutation == Mutation::create && state.mutation_source == Source::live &&
            (state.mutation_request || state.mutation_uncertain || state.missing_reconciled_create)) {
            // A pending acknowledgement still belongs to the original target. A new lookup cannot verify it.
            state.create_target_lost = true;
            state.reconcile_needed = false;
            state.mutation_uncertain = false;
            state.missing_reconciled_create = false;
            state.mutation_message = std::format("Check account {} in the original database.", state.mutation_id);
        }
    } else if (state.editor == Editor::edit || state.editor == Editor::password) {
        state.review_required = true;
    }
}

void consume_identity_loss(ConfigUsersState& state, PreviewSession& session)
{
    if (state.processed_roster_loss != state.subscription && session.lost_users_roster_identity(state.subscription)) {
        state.processed_roster_loss = state.subscription;
        invalidate_roster_access(state, session, UsersRoster::LossCause::identity);
    }
}

void sync_roster(ConfigUsersState& state, PreviewSession& session)
{
    const auto* roster = session.users_roster(state.subscription);
    if (!roster) {
        if (state.subscription) {
            state.subscription.reset();
            state.order_dirty = true;
        }
        return;
    }
    if (roster->generation != state.roster_generation) {
        state.roster_generation = roster->generation;
        state.order_dirty = true;
    }
    if (roster->state == UsersRoster::State::lost) {
        if (state.processed_roster_loss != roster->subscription) {
            state.processed_roster_loss = roster->subscription;
            if (roster->loss_cause != UsersRoster::LossCause::config_unverified || state.binding_error.empty()) {
                invalidate_roster_access(state, session, roster->loss_cause);
            }
        }
        return;
    }
    if (roster->sequence && state.source == Source::offline && state.binding_error.empty()) {
        state.editable = true;
    }
    if (roster->commits == state.roster_commits) {
        return;
    }
    state.roster_commits = roster->commits;
    if (state.recent_live_id && roster->baseline.contains(state.recent_live_id)) {
        state.recent_live_id = 0;
    }
    const auto contains = [&](std::string_view id) {
        const auto number = parse_unsigned(id);
        return number && roster->baseline.contains(*number);
    };
    if (!state.selection.empty()) {
        if (contains(state.selection)) {
            state.selection_from_roster = true;
        } else if (state.selection_from_roster) {
            state.selection.clear();
            state.selection_from_roster = false;
        }
    }
    if (!state.options_id.empty() && !contains(state.options_id)) {
        state.options_id.clear();
    }
    if (state.detail_request && state.detail == Detail::toggle && !contains(state.detail_id)) {
        session.abandon(state.detail_request);
    }
    if (state.draft_from_roster && !state.user_id.empty() && !contains(state.user_id)) {
        state.draft_removed = true;
        state.edit_token.clear();
        session.abandon(state.detail_request);
        clear_secrets(state);
    }
}

template<class T>
int compare(const T& left, const T& right)
{
    return left < right ? -1 : right < left ? 1 : 0;
}

void rebuild_order(ConfigUsersState& state, const UsersRoster* roster, double now)
{
    const bool clock_changed = roster && roster->clock.received_at != state.sort_clock;
    const bool expiry_changed = state.sort_column == Column::expiry && (clock_changed || now >= state.next_expiry_sort);
    if (!state.order_dirty && !expiry_changed) {
        return;
    }
    state.order_dirty = false;
    state.next_expiry_sort = std::numeric_limits<double>::infinity();
    state.sort_clock = roster ? roster->clock.received_at : 0;
    state.ordered_ids.clear();

    if (!roster) {
        return;
    }

    const auto& rows = roster->visible_rows();
    if (state.search_is_id) {
        const auto id = parse_unsigned(state.search_key);
        if (id && rows.contains(*id)) {
            state.ordered_ids.push_back(*id);
        }
        return;
    }

    struct SortRow {
        const UsersRosterRow* row;
        Expiry expiry;
    };
    std::vector<SortRow> matches;
    matches.reserve(rows.size());
    for (const auto& [id, row] : rows) {
        if (!state.search_key.empty() && row.folded_name.find(state.search_key) == std::string::npos) {
            continue;
        }
        const auto expiry = state.sort_column == Column::expiry ? effective_expiry(row, roster->clock, state.source, now)
                                                               : Expiry{ExpiryState::unavailable};
        if (expiry.state == ExpiryState::timed && expiry.remaining > 0) {
            state.next_expiry_sort = std::min(state.next_expiry_sort, now + static_cast<double>(expiry.remaining));
        }
        matches.push_back({&row, expiry});
    }
    std::ranges::sort(matches, [&](const SortRow& left, const SortRow& right) {
        const auto& a = *left.row;
        const auto& b = *right.row;
        int result = 0;
        switch (state.sort_column) {
        case Column::id: result = compare(a.id, b.id); break;
        case Column::name: result = compare(a.folded_name, b.folded_name); break;
        case Column::enabled: result = compare(a.enabled, b.enabled); break;
        case Column::group: result = compare(a.group.value_or(0), b.group.value_or(0)); break;
        case Column::traffic_usage:
            if (a.traffic.has_value() != b.traffic.has_value()) {
                return a.traffic.has_value();
            }
            if (a.traffic) {
                result = compare(a.traffic->total(), b.traffic->total());
            }
            break;
        case Column::upload_speed:
        case Column::download_speed:
            if (a.rates.has_value() != b.rates.has_value()) {
                return a.rates.has_value();
            }
            if (a.rates) {
                result = state.sort_column == Column::upload_speed ? compare(a.rates->upload, b.rates->upload)
                                                                   : compare(a.rates->download, b.rates->download);
            }
            break;
        case Column::expiry: {
            if ((left.expiry.state == ExpiryState::unavailable) != (right.expiry.state == ExpiryState::unavailable)) {
                return right.expiry.state == ExpiryState::unavailable;
            }
            result = compare(std::tie(left.expiry.state, left.expiry.remaining),
                             std::tie(right.expiry.state, right.expiry.remaining));
            break;
        }
        case Column::options: break;
        }
        return result == 0 ? a.id < b.id : state.sort_descending ? result > 0 : result < 0;
    });
    state.ordered_ids.reserve(matches.size());
    for (const auto& item : matches) {
        state.ordered_ids.push_back(item.row->id);
    }
}

void select_account(ConfigUsersState& state, PreviewSession& session, std::string_view id)
{
    if (state.detail == Detail::lookup ||
        (state.detail_id != id && (state.detail == Detail::open || state.detail == Detail::open_saved_password ||
                                  state.detail == Detail::password))) {
        session.abandon(state.detail_request);
    }
    state.selection = id;
    state.selection_from_roster = true;
}

void toggle_account(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                    std::string_view id, bool enabled)
{
    select_account(state, session, id);
    if (state.source == Source::live) {
        state.expected_enabled = enabled;
        begin_detail(state, session, config_id, node, id, Detail::toggle);
        return;
    }
    auto request = request_for(state, "edit_config_user", config_id, node);
    request["user_id"] = id;
    request["user"] = Json{{"enabled", !enabled}};
    start_mutation(state, session, std::move(request), Mutation::toggle, id);
}

const char* column_label(Column column)
{
    switch (column) {
    case Column::id: return "ID";
    case Column::name: return "Name";
    case Column::enabled: return "Enable";
    case Column::options: return "Options";
    case Column::traffic_usage: return "Traffic usage";
    case Column::upload_speed: return "Avg upload speed";
    case Column::download_speed: return "Avg download speed";
    case Column::expiry: return "Days Left";
    case Column::group: return "Group";
    }
    return "";
}

constexpr std::array columns{Column::id, Column::name, Column::enabled, Column::options,
                             Column::traffic_usage, Column::upload_speed, Column::download_speed, Column::expiry, Column::group};

void draw_clipped_label(ImVec2 position, float width, std::string_view label, bool tooltip = false)
{
    if (ImGui::IsItemVisible()) {
        const auto height = ImGui::GetTextLineHeight();
        const auto end = label.data() + label.size();
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), position, {position.x + width, position.y + height},
                                  position.x + width, label.data(), end, nullptr);
        if ((tooltip || ImGui::CalcTextSize(label.data(), end).x > width) &&
            (ImGui::IsItemHovered() || ImGui::IsItemFocused())) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::clamp(ImGui::GetIO().DisplaySize.x - 40, 1.0f, 480.0f));
            view::text(label);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

bool clipped_cell(const char* widget, std::string_view label, bool selected, bool tooltip = false)
{
    ImGui::AlignTextToFramePadding();
    const auto position = ImGui::GetCursorScreenPos();
    const auto width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size{width, ImGui::GetTextLineHeight()};
    bool activated = false;
    if (widget) {
        activated = ImGui::Selectable(widget, selected, ImGuiSelectableFlags_None, size);
    } else {
        ImGui::Dummy(size);
    }
    draw_clipped_label(position, width, label, tooltip);
    return activated;
}

void render_options(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                    const UsersRoster* roster, bool ready)
{
    if (!ImGui::BeginPopup("Account options")) {
        return;
    }
    const auto* user = find_row(roster, state.options_id);
    if (!user) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const auto id = state.options_id;
    const bool offline = state.source == Source::offline;
    const bool detail_allowed = ready && !user->limited_detail && can_lookup(state);
    if (ImGui::MenuItem(offline ? "Edit account" : "Account details", nullptr, false, detail_allowed)) {
        select_account(state, session, id);
        begin_detail(state, session, config_id, node, id, Detail::open);
    }
    if (offline && ImGui::MenuItem("Edit and show saved password", nullptr, false, detail_allowed)) {
        select_account(state, session, id);
        begin_detail(state, session, config_id, node, id, Detail::open_saved_password);
    }
    if (can_change_password(state) && ImGui::MenuItem("Change password", nullptr, false, ready)) {
        select_account(state, session, id);
        begin_detail(state, session, config_id, node, id, Detail::password);
    }
    const bool can_confirm = offline && ready && can_edit(state);
    const bool remove = offline && ImGui::MenuItem("Delete user", nullptr, false, can_confirm);
    const bool reset = ImGui::MenuItem(offline ? "Reset traffic" : "Reset traffic (stop WaterWall first)",
                                       nullptr, false, can_confirm);
    if (remove || reset) {
        select_account(state, session, id);
        select_user(state, reset ? Editor::reset_traffic : Editor::remove, {{"id", id}, {"name", user->name}});
        state.draft_from_roster = true;
    }
    ImGui::EndPopup();
}

void render_rows(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                 const UsersRoster* roster, bool narrow)
{
    const bool compact = narrow || ImGui::GetContentRegionAvail().x < 50 * ImGui::GetFontSize();
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                       ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                       ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                       (compact ? ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit
                                : ImGuiTableFlags_SizingStretchProp);
    const auto table_id = compact ? "Compact users table" : "Users table";
    if (!ImGui::BeginTable(table_id, columns.size(),
                           flags, {0, 0})) {
        return;
    }
    const ImVec2 enable_switch_size{ImGui::CalcTextSize(column_label(Column::enabled)).x + ImGui::GetFontSize(),
                                    ImGui::GetFrameHeight()};
    for (const auto column : columns) {
        ImGuiTableColumnFlags column_flags = ImGuiTableColumnFlags_None;
        float width = 0;
        switch (column) {
        case Column::id:
            column_flags = ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed;
            width = compact ? 48 : 76;
            break;
        case Column::name:
            column_flags = compact ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch;
            width = compact ? 90 : 2;
            break;
        case Column::enabled:
            column_flags = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize;
            width = enable_switch_size.x;
            break;
        case Column::options:
            column_flags = ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed;
            width = compact ? 68 : 88;
            break;
        case Column::traffic_usage:
        case Column::upload_speed:
        case Column::download_speed:
            column_flags = ImGuiTableColumnFlags_WidthFixed;
            width = ImGui::CalcTextSize(column_label(column)).x + ImGui::GetFontSize();
            break;
        case Column::expiry:
            column_flags = compact ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch;
            width = compact ? 135 : 1;
            break;
        case Column::group:
            column_flags = ImGuiTableColumnFlags_DefaultHide | ImGuiTableColumnFlags_WidthFixed;
            width = 86;
            break;
        }
        ImGui::TableSetupColumn(column_label(column), column_flags, width, static_cast<ImGuiID>(column));
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    if (state.viewed_table != table_id) {
        state.viewed_table = table_id;
        ImGui::TableSetColumnSortDirection(static_cast<int>(state.sort_column) - 1,
                                          state.sort_descending ? ImGuiSortDirection_Descending
                                                                : ImGuiSortDirection_Ascending,
                                          false);
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (!ImGui::TableSetColumnIndex(index)) {
            continue;
        }
        const auto column = columns[index];
        ImGui::TableHeader(column_label(column));
        if (column == Column::traffic_usage) {
            ImGui::SetItemTooltip("Uploaded and downloaded bytes combined, after WaterWall charge multipliers.");
        } else if (column == Column::upload_speed || column == Column::download_speed) {
            ImGui::SetItemTooltip("Average charged bytes per second between this account's last two observed traffic updates.");
        } else if (column == Column::expiry) {
            ImGui::SetItemTooltip("Time until the earliest active expiry. Saved first-use timers require a live accounting clock.");
        }
    }
    if (auto* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsDirty && sort->SpecsCount == 1) {
        state.sort_column = static_cast<Column>(sort->Specs[0].ColumnUserID);
        state.sort_descending = sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        state.order_dirty = true;
        sort->SpecsDirty = false;
    }
    const auto now = emscripten_get_now();
    rebuild_order(state, roster, now);
    capture_scroll_bounds(state);
    const bool ready = session.ready() && roster->fresh && state.binding_error.empty() &&
                       !state.mutation_request && !state.detail_request && !state.mutation_uncertain;
    const bool offline = state.source == Source::offline;
    const auto row_height = ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().CellPadding.y;
    bool open = false;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.ordered_ids.size()), row_height);
    for (; clipper.Step();) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto* user = find_row(roster, state.ordered_ids[index]);
            if (!user) {
                continue;
            }
            const auto id = std::to_string(user->id);
            const auto selected = state.selection == id;
            ImGui::PushID(id.c_str());
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
            if (selected) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::packed(Color::selected));
            }
            bool choose = false;
            if (ImGui::TableSetColumnIndex(0)) {
                choose |= clipped_cell("##select-id", id, selected);
            }
            if (ImGui::TableSetColumnIndex(1)) {
                const bool truncated = user->name_state == "truncated";
                const auto display = user->name.empty() ? (user->name_state == "invalid_encoding" ? "Unavailable" : "—")
                                                       : user->name + (truncated ? "…" : "");
                choose |= clipped_cell("##select-name", display, selected, truncated);
                if (user->limited_detail) {
                    ImGui::SetItemTooltip("This account is too large to edit in Wave.");
                }
            }
            if (choose) {
                select_account(state, session, id);
            }
            if (ImGui::TableSetColumnIndex(2)) {
                if (can_edit(state) && !user->limited_detail && user_field("enabled").is_object()) {
                    ImGui::BeginDisabled(!ready);
                    if (view::toggle_switch("Enable account", enable_switch_size, enable_switch_size, user->enabled)) {
                        toggle_account(state, session, config_id, node, id, user->enabled);
                    }
                    ImGui::SetItemTooltip("%s account %s", user->enabled ? "Disable" : "Enable", id.c_str());
                    ImGui::EndDisabled();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::packed(user->enabled ? Color::success
                                                                                    : Color::secondary_text));
                    clipped_cell(nullptr, user->enabled ? "Enabled" : "Disabled", false);
                    ImGui::PopStyleColor();
                }
            }
            if (ImGui::TableSetColumnIndex(3)) {
                const bool options_allowed = offline ? state.editable : allowed(state, "get-user");
                ImGui::BeginDisabled(!ready || !options_allowed);
                if (theme::small_button("Options")) {
                    select_account(state, session, id);
                    state.options_id = id;
                    open = true;
                }
                ImGui::EndDisabled();
            }
            if (ImGui::TableSetColumnIndex(4)) {
                clipped_cell(nullptr, usage_label(user->traffic), false);
            }
            for (const auto column : {Column::upload_speed, Column::download_speed}) {
                if (ImGui::TableSetColumnIndex(static_cast<int>(column) - 1)) {
                    clipped_cell(nullptr, user->rates ? rate_label(user, column == Column::upload_speed) : "—", false);
                    if (user->rates) {
                        ImGui::SetItemTooltip("Average over %.1f s. Last traffic update %.1f s ago.",
                                              user->rates->interval_ms / 1000, (now - user->traffic_changed_at) / 1000);
                    } else {
                        ImGui::SetItemTooltip("%s", offline ? "Speed estimates require a running core."
                                                           : "Waiting for two observed traffic updates for this account.");
                    }
                }
            }
            if (ImGui::TableSetColumnIndex(7)) {
                clipped_cell(nullptr, expiry_label(effective_expiry(*user, roster->clock, state.source, now)), false);
            }
            if (ImGui::TableSetColumnIndex(8)) {
                clipped_cell(nullptr, user->group.value_or(0) == 0 ? "Default" : std::to_string(*user->group), false);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (open) {
        ImGui::OpenPopup("Account options");
    }
    render_options(state, session, config_id, node, roster, ready);
}

void render_database_status(ConfigUsersState& state, bool narrow)
{
    if (!state.status_open) {
        return;
    }
    ImGui::SeparatorText("Database status");
    if (!state.status_error.empty()) {
        view::wrapped(state.status_error, Color::warning);
    }
    const auto& status = state.database_status;
    if (!status.is_object()) {
        return;
    }
    if (begin_fields("Database health", narrow)) {
        const auto metric = [](std::string_view label, std::string_view value) {
            ImGui::TableNextColumn();
            view::wrapped(label, Color::secondary_text);
            view::wrapped(value.empty() ? "Unavailable" : value);
        };
        const auto yes_no = [](const Json& value) -> std::string_view {
            return value.is_boolean() ? value == true ? "Yes" : "No" : "Unavailable";
        };
        metric("Database loaded", yes_no(view::field(status, "database_loaded")));
        metric("Unsaved changes", yes_no(view::field(status, "database_dirty")));
        metric("Primary checkpoint valid", yes_no(view::field(status, "primary_checkpoint_valid")));
        metric("Accounts", decimal(view::field(status, "user_count")));
        metric("Retained producer sessions", decimal(view::field(status, "session_record_count")));
        const auto& save = view::field(status, "primary_save");
        const auto save_result = view::string_field(save, "last_result");
        metric("Last primary save result", save_result == "not_attempted" ? "Not attempted" : save_result);
        if (const auto age = decimal(view::field(save, "last_success_age_ms")); !age.empty()) {
            metric("Last successful primary save", age + " ms before this check");
        }
        if (const auto age = decimal(view::field(save, "last_attempt_age_ms")); !age.empty()) {
            metric("Last primary save attempt", age + " ms before this check");
        }
        const auto& audit = view::field(status, "audit");
        if (audit.is_object()) {
            const auto enabled = view::field(audit, "enabled");
            metric("Audit logging", yes_no(enabled));
            if (enabled == true) {
                metric("Audit healthy", yes_no(view::field(audit, "healthy")));
                metric("Audit records queued", decimal(view::field(audit, "queued_records")));
                metric("Audit records written", decimal(view::field(audit, "written_records")));
                metric("Audit records dropped", decimal(view::field(audit, "dropped_records")));
                metric("Audit write failures", decimal(view::field(audit, "write_failures")));
                if (const auto error = view::string_field(audit, "last_error"); !error.empty()) {
                    metric("Last audit error", error);
                }
            }
        }
        ImGui::EndTable();
    }
}

void lookup_exact(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node)
{
    const auto id = exact_id(state.lookup_input);
    if (id.empty() || state.detail_request || !session.ready() || !can_lookup(state)) {
        return;
    }
    state.selection.clear();
    state.selection_from_roster = false;
    state.options_id.clear();
    begin_detail(state, session, config_id, node, id, Detail::lookup);
}

std::string access_description(const ConfigUsersState& state)
{
    if (!state.binding_error.empty()) {
        return state.binding_error;
    }
    if (!state.availability.empty()) {
        return state.availability;
    }
    if (state.source != Source::live) {
        return {};
    }
    std::string options;
    for (const auto& [operation, label] :
         {std::pair{"list-users", "list accounts"}, {"get-user", "look up accounts by ID"},
          {"status", "check database status"}, {"create-user", "create accounts"},
          {"update-user", "edit accounts"}, {"change-user-password", "change passwords"}}) {
        const bool detail_needed = std::string_view(operation) == "update-user" ||
                                   std::string_view(operation) == "change-user-password";
        if (allowed(state, operation) && (!detail_needed || allowed(state, "get-user"))) {
            if (!options.empty()) {
                options += ", ";
            }
            options += label;
        }
    }
    if (options.empty()) {
        return "The current API grants no account tools. Grant an operation for this node and restart.";
    }
    if (allowed(state, "list-users") && allowed(state, "create-user") && can_edit(state) &&
        can_change_password(state) && allowed(state, "status")) {
        return {};
    }
    return "Available live access · " + options + '.';
}

std::string list_notice(const ConfigUsersState& state, const PreviewSession& session, const UsersRoster* roster)
{
    if (can_list(state) && session.ready() && !session.supports_users_roster()) {
        if (session.http_fallback()) {
            return state.source == Source::live && can_lookup(state)
                       ? "The account list requires a WebSocket connection. "
                         "You can still look up an account by its exact ID."
                       : "The account list requires a WebSocket connection.";
        }
        return "This Wave backend does not support the account roster. Update the backend to browse and search names.";
    }
    if (!state.list_error.empty()) {
        return state.list_error;
    }
    return roster && roster->message != state.binding_error ? roster->message : "";
}

bool loading_roster(const UsersRoster* roster)
{
    // A routine delta leaves the committed list on screen until the changed rows arrive.
    return roster && roster->state == UsersRoster::State::loading &&
           !(roster->fresh && roster->transfer && roster->transfer->delta);
}

std::string roster_summary(const ConfigUsersState& state, const UsersRoster* roster)
{
    if (!roster || !state.binding_error.empty()) {
        return {};
    }
    if (loading_roster(roster)) {
        if (!roster->sequence && roster->transfer) {
            return std::format("Loading {} of {}", roster->transfer->rows.size(), roster->transfer->total);
        }
        return roster->sequence ? "Refreshing accounts" : "Loading users";
    }
    if (roster->state == UsersRoster::State::lost) {
        return {};
    }
    if (!roster->sequence) {
        return roster->state == UsersRoster::State::error ? "Account list unavailable" : "";
    }
    const char* label = list_stale(roster) ? "Stale" : state.source == Source::offline ? "Saved" : "Live";
    if (!has_search(state)) {
        return std::format("{} · {} users", label, roster->baseline.size());
    }
    return std::format("{} · {} matches of {} users", label, state.ordered_ids.size(), roster->baseline.size());
}

std::string empty_description(const ConfigUsersState& state, const UsersRoster* roster)
{
    if (roster && roster->sequence && state.ordered_ids.empty()) {
        return has_search(state) ? "No matches." : "No users.";
    }
    return {};
}

void render_roster_status(ConfigUsersState& state, PreviewSession& session, const UsersRoster* roster,
                          std::string_view progress)
{
    const bool refreshing = roster && roster->sequence && loading_roster(roster);
    const bool busy = state.detail_request || state.mutation_request || state.status_request || refreshing;
    const bool cancel = state.detail_request && (state.detail == Detail::open || state.detail == Detail::open_saved_password ||
                                                 state.detail == Detail::password || state.detail == Detail::lookup);
    const bool dismiss = state.mutation_uncertain && !state.mutation_request;
    const char* action = cancel ? "Cancel lookup" : dismiss ? "Dismiss outcome" : nullptr;

    auto label = state.mutation_message;
    if (!state.detail_error.empty()) {
        if (!label.empty()) {
            label += ' ';
        }
        label += state.detail_error;
    }
    const bool warning = !label.empty();
    if (label.empty()) {
        label = progress;
    }
    if (label.empty() && !state.selection.empty() && !find_row(roster, state.selection)) {
        label = std::format("Selected account {}", state.selection);
    }
    if (label.empty()) {
        label = roster_summary(state, roster);
        if (const auto empty = empty_description(state, roster); !empty.empty()) {
            label += " · " + empty;
        } else if (!label.empty() && state.source == Source::live) {
            label += " · Avg speeds in charged B/s";
        }
    }
    if (label.empty() && !action && !busy) {
        return;
    }

    const auto& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x;
    const float action_space = action ? ImGui::CalcTextSize(action).x + 2 * style.FramePadding.x + gap : 0;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - action_space -
                                       (busy ? busy_spinner_width + gap : 0));
    const float height = ImGui::GetTextLineHeight();
    const auto position = ImGui::GetCursorScreenPos();
    ImGui::Dummy({width, height});
    if (!label.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::packed(warning ? Color::warning : Color::primary_text));
        draw_clipped_label(position, width, label);
        ImGui::PopStyleColor();
    }
    if (action) {
        ImGui::SameLine();
        if (theme::small_button(action)) {
            if (cancel) {
                session.abandon(state.detail_request);
            } else {
                state.mutation_uncertain = false;
                state.reconcile_needed = false;
                state.mutation_message.clear();
                state.detail_error.clear();
            }
        }
    }
    if (busy) {
        ImGui::SameLine();
        draw_busy_spinner(height);
    }
}

void render_toolbar(ConfigUsersState& state, PreviewSession& session, int config_id, std::string_view node,
                    const UsersRoster* roster, bool narrow)
{
    const bool pending = state.mutation_request || state.detail_request;
    const bool create = can_create(state);
    const bool status = allowed(state, "status");
    const bool offline = state.source == Source::offline;
    const bool list = can_list(state) && session.supports_users_roster();
    const bool lookup = (!list || list_stale(roster) || !state.list_error.empty()) &&
                        can_lookup(state) && state.source == Source::live;
    const auto draw_search = [&](float width) {
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputTextWithHint("##Search users", "Search ID or displayed name", &state.search_input)) {
            const auto query = trim_query(state.search_input);
            state.search_is_id = is_decimal(query);
            state.search_key = state.search_is_id ? exact_id(query) : unicode_casefold(query);
            state.order_dirty = true;
        }
    };
    bool previous = false;
    const auto next_control = [&] {
        if (previous && ImGui::GetContentRegionAvail().x > 0) {
            ImGui::SameLine();
        }
        previous = true;
    };
    if (create) {
        next_control();
        ImGui::BeginDisabled(!session.ready() || pending || state.mutation_uncertain);
        if (theme::button("Create User")) {
            select_user(state, Editor::add);
        }
        ImGui::EndDisabled();
    }
    if (list && !narrow) {
        next_control();
        draw_search(std::min(270.0f, ImGui::GetContentRegionAvail().x));
    }
    if (status) {
        next_control();
        ImGui::BeginDisabled(state.status_request.has_value() || !session.ready());
        if (theme::button("Database status")) {
            state.status_open = true;
            state.status_error.clear();
            state.status_request = session.submit(request_for(state, "get_live_config_user_status", config_id, node),
                                                  5000);
            if (!state.status_request) {
                state.status_error = "The database status connection is unavailable.";
            }
        }
        ImGui::EndDisabled();
        if (state.status_open) {
            ImGui::SameLine();
            if (theme::button("Close status")) {
                session.abandon(state.status_request);
                state.status_open = false;
            }
        }
    }
    const bool retry = !session.ready() || list_stale(roster) || !state.list_error.empty() || !state.binding_error.empty();
    if (retry || (offline && list)) {
        if (!narrow || !previous || (!create || !status)) {
            next_control();
        }
        ImGui::BeginDisabled(!retry && (!roster || roster->state == UsersRoster::State::loading));
        if (theme::button(retry ? "Retry" : "Refresh from disk")) {
            retry_roster(state, session);
        }
        ImGui::EndDisabled();
    }
    if (list && narrow) {
        draw_search(-1);
    }
    if (lookup) {
        if (previous && !narrow && !list) {
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(narrow ? std::max(1.0f, ImGui::GetContentRegionAvail().x - 113) : 215);
        const bool submitted = ImGui::InputTextWithHint("##Lookup account", "Exact account ID", &state.lookup_input,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(!session.ready() || state.detail_request.has_value() || exact_id(state.lookup_input).empty());
        if (theme::button("Find account") || submitted) {
            lookup_exact(state, session, config_id, node);
        }
        ImGui::EndDisabled();
        if (!allowed(state, "list-users")) {
            view::wrapped("Browsing and name search require listing access.", Color::secondary_text);
        }
    }
}

} // namespace

std::optional<LiveCreateReceipt> unconfirmed_create(ConfigUsersState& state, PreviewSession& session)
{
    if (state.mutation != Mutation::create || state.mutation_source != Source::live) {
        return std::nullopt;
    }
    // The roster can report identity loss just before disconnect, before another frame renders the form.
    consume_identity_loss(state, session);
    // A reply can already be buffered when the identity socket disconnects or the database changes.
    if (state.mutation_request) {
        auto reply = session.take_reply(state.mutation_request);
        session.abandon(state.mutation_request);
        apply_mutation(state, session, reply.value_or(PreviewReply{PreviewReply::Status::disconnected, {}}));
    }
    if (!state.mutation_request && !state.mutation_uncertain) {
        return std::nullopt;
    }
    return LiveCreateReceipt{state.mutation_id, state.mutation_run, state.mutation_database};
}

void restore_create_receipt(ConfigUsersState& state, const LiveCreateReceipt& receipt)
{
    state.mutation = Mutation::create;
    state.mutation_source = Source::live;
    state.mutation_run = receipt.run;
    state.mutation_database = receipt.database;
    state.mutation_id = receipt.id;
    state.mutation_uncertain = true;
    state.reconcile_needed = true;
    state.mutation_message = std::format("Outcome unknown for account {}. Check the original database "
                                         "before taking another action.", receipt.id);
}

void sync_config_users(ConfigUsersState& state, PreviewSession& session, const Json& access,
                       std::string_view core_state, bool active, bool legacy_live)
{
    consume_identity_loss(state, session);
    Source source = Source::blocked;
    const auto known = view::string_field(access, "source");
    const auto run = view::string_field(access, "run_id");
    std::string reason(view::string_field(access, "reason"));
    Json grants = view::field(access, "operations");
    if (core_state == "starting" || core_state == "stopping" || known == "transition") {
        source = Source::transition;
        if (reason.empty()) {
            reason = core_state == "starting" ? "WaterWall is starting. User access is paused."
                                             : "WaterWall is stopping. User access is paused.";
        }
    } else if (core_state != "running" && !active && known == "offline" && !run.empty()) {
        source = Source::offline;
    } else if (known == "live" || (known.empty() && active && legacy_live && core_state == "running")) {
        source = Source::live;
        if (known.empty()) {
            grants = Json::array({"list-users"});
        }
    } else if (reason.empty()) {
        if (core_state != "running" && !active) {
            reason = "Reload the workspace to get the saved database scope.";
        } else {
            reason = active ? "This running config needs a local WaterWall API and a grant for this node."
                            : "The selected config is inactive. Stop WaterWall to access its database.";
        }
    }

    const auto generation = view::string_field(access, "database_generation");
    const bool database_changed = !generation.empty() && !state.database_generation.empty() &&
                                  state.database_generation != generation;
    const auto receipt = database_changed ? unconfirmed_create(state, session) : std::nullopt;
    const bool previously_lookup = can_lookup(state);
    const bool scope_changed = state.source != source || state.run_id != run || database_changed;
    const bool connection_lost = state.connected && !session.ready();
    const auto binding_error = source == Source::offline ? view::string_field(access, "binding_error")
                                                         : std::string_view{};
    const bool binding_became_unverified = state.binding_error.empty() && !binding_error.empty();
    state.connected = session.ready();
    state.source = source;
    state.run_id = run;
    state.availability = std::move(reason);
    state.binding_error = binding_error;
    state.grants = std::move(grants);
    if (!generation.empty()) {
        state.database_generation = generation;
    }
    const bool list_permitted = can_list(state);
    const bool list_revoked = state.list_permitted && !list_permitted;
    state.list_permitted = list_permitted;
    const bool live_lookup_revoked = source == Source::live && previously_lookup && !can_lookup(state);
    if (scope_changed || connection_lost || live_lookup_revoked) {
        session.unsubscribe_users_roster(state.subscription);
        state.editable = false;
        state.order_dirty = true;
        state.roster_generation = 0;
        state.roster_commits = 0;
        state.selection.clear();
        state.selection_from_roster = false;
        state.options_id.clear();
        state.list_error.clear();
        if (state.mutation_request && scope_changed) {
            state.mutation_context_changed = true;
        }
        session.abandon(state.detail_request);
        session.abandon(state.status_request);
        clear_secrets(state);
        if (scope_changed) {
            state.recent_live_id = 0;
            state.edit_token.clear();
            state.review_user = nullptr;
            state.review_token.clear();
            state.detail_error.clear();
            state.status_error.clear();
            state.database_status = nullptr;
            state.missing_reconciled_create = false;
            state.status_open = false;
            state.draft_from_roster = false;
            if (database_changed) {
                session.abandon(state.mutation_request);
                select_user(state, Editor::none);
                state.detail_id.clear();
                state.mutation_id.clear();
                state.search_key.clear();
                state.search_input.clear();
                state.search_is_id = false;
                state.lookup_input.clear();
                state.reconcile_needed = false;
                state.mutation_uncertain = false;
                state.mutation_context_changed = false;
                state.mutation_message = "The node's database changed. Open the account again.";
                if (receipt) {
                    restore_create_receipt(state, *receipt);
                }
            } else if (state.editor == Editor::password || state.editor == Editor::remove ||
                       state.editor == Editor::reset_traffic) {
                select_user(state, Editor::none);
            } else if (state.editor != Editor::none) {
                state.review_required = true;
            }
        }
    }
    if (list_revoked && !scope_changed && !connection_lost) {
        session.unsubscribe_users_roster(state.subscription);
        invalidate_roster_access(state, session, UsersRoster::LossCause::permission);
    }
    if (binding_became_unverified) {
        invalidate_roster_access(state, session, UsersRoster::LossCause::config_unverified);
    }
}

void hide_config_users(ConfigUsersState& state, PreviewSession& session)
{
    if (state.subscription) {
        session.unsubscribe_users_roster(state.subscription);
        state.options_id.clear();
        state.editable = false;
        state.order_dirty = true;
    }
}

void render_config_users(ConfigUsersState& state, PreviewSession& session, int config_id,
                         std::string_view node, bool narrow)
{
    state.config_id = config_id;
    state.node = node;
    state.scroll_bounds = {};
    ensure_roster(state, session, config_id, node);
    sync_roster(state, session);
    updates(state, session, config_id, node);
    auto* roster = session.users_roster(state.subscription);

    const auto access = access_description(state);
    if (!access.empty()) {
        view::wrapped(access, state.source == Source::live ? Color::secondary_text : Color::warning);
    }
    const auto notice = list_notice(state, session, roster);
    const bool progress = state.editor == Editor::none && roster && !list_stale(roster) &&
                          state.binding_error.empty() && state.list_error.empty();
    if (!session.ready()) {
        view::wrapped("The panel connection is unavailable.", Color::warning);
    } else if (!progress && !notice.empty()) {
        view::wrapped(notice, Color::warning);
    }
    if (state.editor != Editor::none) {
        if ((!session.ready() || list_stale(roster) || !state.binding_error.empty() || !state.list_error.empty()) &&
            theme::button("Retry")) {
            retry_roster(state, session);
        }
        render_editor(state, session, config_id, node, narrow);
        return;
    }

    const bool accessible = state.source == Source::offline || state.source == Source::live;
    if (accessible) {
        render_toolbar(state, session, config_id, node, roster, narrow);
        roster = session.users_roster(state.subscription);
        rebuild_order(state, state.binding_error.empty() ? roster : nullptr, emscripten_get_now());
    } else if (!session.ready() && theme::button("Retry")) {
        session.start(true);
    }
    const auto* rows = state.binding_error.empty() ? roster : nullptr;
    render_roster_status(state, session, rows, progress ? std::string_view(notice) : std::string_view{});
    if (state.status_open) {
        if (ImGui::BeginChild("Database status panel", {0, 0}, ImGuiChildFlags_Borders)) {
            capture_scroll_bounds(state);
            render_database_status(state, narrow);
        }
        ImGui::EndChild();
        return;
    }
    if (rows && (rows->sequence || rows->transfer)) {
        render_rows(state, session, config_id, node, rows, narrow);
    }
}

std::string describe_config_users(const ConfigUsersState& state, const PreviewSession& session)
{
    const auto* roster = session.users_roster(state.subscription);
    std::string description(view::string_field(user_ui(), "offline_description", "AuthenticationServer users."));
    const auto append = [&](std::string_view text) {
        if (!text.empty()) {
            description += ' ';
            description += text;
        }
    };
    append(access_description(state));
    append(list_notice(state, session, roster));
    if (state.editor != Editor::none) {
        append(std::string(editor_title(state)) + '.');
        if (state.editor == Editor::reset_traffic) {
            append(reset_traffic_prompt(state.user_id));
        }
        if (state.editor == Editor::add || state.editor == Editor::edit) {
            append(std::format("{} section.", section_name(state.section)));
        }
        if (state.editor == Editor::add && state.numbers.at("/id").empty()) {
            if (const auto suggestion = suggest_create_id(state, session); suggestion.id) {
                append(std::format("Next ID {}.", *suggestion.id));
            }
        }
        if (state.editor == Editor::edit && state.source == Source::live) {
            append(live_account_summary(state, session));
        }
        append(state.validation);
        if (state.draft_removed) {
            append("This account was removed. Its draft can no longer be saved.");
        } else if (state.draft_scope_lost) {
            append("The account source could not be verified. Reopen the account before saving.");
        } else if (state.review_required) {
            append("Review current account details before saving.");
            if (state.binding_error.empty() && !can_review_draft(state) &&
                (state.editor == Editor::edit || (state.editor == Editor::password && !state.mutation_uncertain))) {
                append("Access to save this account is unavailable.");
            }
            for (const auto& field : state.changed_fields) {
                append(field_label(field) + " changed.");
            }
        }
    }
    if (state.detail_request) {
        append(state.detail == Detail::saved_password ? "Loading saved password." : "Loading account details.");
    }
    if (state.mutation_request) {
        append(busy_description);
    }
    if (state.status_request) {
        append("Loading database status.");
    } else if (state.status_open && state.database_status.is_object()) {
        append("Database status.");
    }
    append(state.status_error);
    append(state.detail_error);
    append(state.mutation_message);
    if (const auto summary = roster_summary(state, roster); !summary.empty()) {
        append(summary + '.');
    }
    if (roster && roster->sequence && state.binding_error.empty()) {
        append(std::format("Sorted by {} {}.", column_label(state.sort_column),
                           state.sort_descending ? "descending" : "ascending"));
    }
    const auto* rows = state.binding_error.empty() ? roster : nullptr;
    if (const auto* selected = find_row(rows, state.selection)) {
        append("Selected account " + account_summary(*selected, rows->clock, state.source,
                                                      state.source == Source::live && rows->fresh ? selected : nullptr));
    } else if (!state.selection.empty()) {
        append(std::format("Selected account {}.", state.selection));
    }
    append(empty_description(state, rows));
    if (list_stale(roster) && roster->sequence && state.binding_error.empty()) {
        append("Showing stale accounts.");
    }
    if (state.source == Source::live) {
        if (roster && roster->sequence) {
            append("Speeds use average charged bytes per second between observed traffic updates.");
        }
        append("Running core.");
        if (can_lookup(state) && !allowed(state, "list-users")) {
            append("Exact-ID lookup. Browsing and name search require listing access.");
        }
    } else if (state.source == Source::offline) {
        append("Saved database.");
    }
    return description;
}

void abandon_config_users(ConfigUsersState& state, PreviewSession& session)
{
    unconfirmed_create(state, session);
    session.unsubscribe_users_roster(state.subscription);
    session.abandon(state.detail_request);
    session.abandon(state.mutation_request);
    session.abandon(state.status_request);
    clear_secrets(state);
    state.edit_token.clear();
    state.review_token.clear();
}

} // namespace wave
