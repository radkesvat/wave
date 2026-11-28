#include "panel_controls_view.h"

#include "browser_ui.h"
#include "core_config_json.h"
#include "dialog.h"
#include "preview_backup.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <imgui_internal.h>

#include <format>
#include <utility>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using ControlsDialog = PanelControlsState::Dialog;

constexpr char popup_id[] = "###ServerPanelControlsDialog";
constexpr std::string_view core_state_prefix = "WaterWall core ";
constexpr char target_label[] = "Target version";
constexpr char check_updates_label[] = "Check for updates###check-updates";
constexpr char checking_updates_label[] = "Checking for updates###check-updates";
constexpr char change_core_version[] = "Change core version";
constexpr char stop_before_update[] = "Stop WaterWall before changing its version.";
constexpr char panel_update_title[] = "Panel update available";
constexpr char build_guide_label[] = "Open build guide";
constexpr char build_guide_url[] =
    "https://github.com/radkesvat/wave/blob/main/doc/building.md#native-backend";
constexpr char update_notice[] =
    "WaterWall will stay stopped. Open configuration forms will reload from their saved settings.";
constexpr char unsaved_config_warning[] = "Unsaved node changes will be lost.";
constexpr char managed_starter_notice[] = "Grants Wave all live user-management permissions for this config.";
constexpr char restore_message[] =
    "Choose a backup JSON file to replace the sample TLS settings and inactive config list.";
constexpr char backup_selected_message[] = "Wave preview backup selected.";
constexpr auto request_id_bytes = sizeof(",\"request_id\":18446744073709551615") - 1;

const Json& server_snapshot(const PreviewSession& session)
{
    return field(session.data(), "server");
}

std::optional<std::uint64_t> panel_check_generation(const Json& panel)
{
    const auto& value = field(panel, "update_check_generation");
    return value.is_number_unsigned() ? std::optional{value.get<std::uint64_t>()} : std::nullopt;
}

bool update_check_pending(const Json& server, const PanelUpdateState& state)
{
    return state.check_generation || bool_field(server, "checking_updates") ||
           bool_field(field(server, "panel"), "checking_updates");
}

std::string panel_update_message(const PanelUpdateState& state)
{
    return std::format("Wave {} is available. Use the build and deployment guide to install this version.",
                       state.available_version);
}

bool supports(const Json& core, std::string_view capability)
{
    return bool_field(field(core, "capabilities"), capability);
}

bool can_edit_configs(const Json& core)
{
    return supports(core, "configs") && field(core, "configs").is_array();
}

bool can_use_backup(const PreviewSession& session)
{
    const auto& settings = field(session.data(), "settings");
    return session.ready() && can_edit_configs(field(server_snapshot(session), "core")) &&
           std::ranges::all_of(preview_backup_tls_fields,
                               [&](const char* key) { return field(settings, key).is_string(); });
}

bool can_stop(const Json& core)
{
    return supports(core, "lifecycle") && string_field(core, "state") == "running";
}

bool has_core_version(const Json& core, std::string_view version)
{
    const auto& versions = array_field(core, "available_versions");
    return !version.empty() && std::ranges::find(versions, version, [](const Json& item) {
                                   return string_value(item);
                               }) != versions.end();
}

bool core_update_pending(const Json& core)
{
    return !string_field(core, "update_target").empty();
}

std::string_view core_update_unavailable(const Json& core)
{
    const auto current = string_field(core, "state");
    if (current == "running") {
        return stop_before_update;
    }
    if (core_update_pending(core) || (current != "stopped" && current != "missing")) {
        return "Wait for the current core operation to finish.";
    }
    if (bool_field(core, "checking_updates")) {
        return "Wait for the update check to finish.";
    }
    return {};
}

const Json* find_config(const Json& core, int id)
{
    const auto& configs = array_field(core, "configs");
    const auto config =
        std::ranges::find_if(configs, [id](const Json& item) { return field(item, "id") == id; });
    return config == configs.end() ? nullptr : &*config;
}

std::string core_state_label(const Json& core)
{
    const auto state = string_field(core, "state", "unavailable");
    return std::format("{}{}.", core_state_prefix, state);
}

std::string controls_notice(const Json& core, const PreviewSession& session)
{
    if (!session.ready()) {
        return session.status() == PreviewStatus::unavailable
                   ? "Preview disconnected. Showing last reported values."
                   : "";
    }
    const bool lifecycle = supports(core, "lifecycle");
    const bool versions = field(core, "available_versions").is_array();
    const bool configs = can_edit_configs(core);
    if (!lifecycle && !versions && !configs) {
        return "Panel controls are unavailable in this preview.";
    }
    std::string notice;
    if (!lifecycle) {
        notice = "Lifecycle controls are unavailable. ";
    }
    if (!versions) {
        notice += "Core updates are unavailable. ";
    }
    if (!configs) {
        notice += "Config changes are unavailable.";
    }
    return notice;
}

void open_dialog(PanelControlsState& state, ControlsDialog dialog)
{
    state.dialog = dialog;
    state.open_dialog = true;
    state.action_config_id = 0;
    state.request_accepted.reset();
}

void close_dialog(PanelControlsState& state)
{
    ImGui::CloseCurrentPopup();
    state.dialog = ControlsDialog::none;
}

void submit(PanelControlsState& state, PreviewSession& session, Json request)
{
    if (state.request_id) {
        return;
    }
    state.action_config_id = 0;
    state.request_accepted.reset();
    state.request_id = session.submit(std::move(request), 5000);
    if (!state.request_id) {
        state.request_accepted = false;
    }
}

bool core_transition_pending(const PreviewSession& session)
{
    const auto& core = field(server_snapshot(session), "core");
    const auto core_state = string_field(core, "state");
    return session.ready() && (core_update_pending(core) || core_state == "installing" ||
                               core_state == "starting" || core_state == "stopping");
}

bool dialog_pending(const PanelControlsState& state, const PreviewSession& session)
{
    const bool lifecycle = state.dialog == ControlsDialog::stop || state.dialog == ControlsDialog::restart;
    const bool updating = state.dialog == ControlsDialog::update && session.ready() &&
                          core_update_pending(field(server_snapshot(session), "core"));
    return state.request_id || (state.request_accepted.value_or(false) &&
                                (updating || (lifecycle && core_transition_pending(session))));
}

float control_height(float minimum = 40.0f)
{
    return std::max(minimum, ImGui::GetFrameHeight());
}

struct ControlsLayout {
    float width;
    float frame_height;
    bool has_core;
    bool has_catalog;
    bool reconnect;
    std::string_view core_state;
    std::string state_label;
    std::string active_label;
    std::string notice;

    ControlsLayout(const PreviewSession& session, const Json& core, float width, bool compact)
        : width(width), frame_height(control_height(compact ? 32.0f : 40.0f)), has_core(core.is_object()),
          has_catalog(field(core, "available_versions").is_array()),
          reconnect(session.status() == PreviewStatus::unavailable), core_state(string_field(core, "state")),
          state_label(core_state_label(core)), notice(controls_notice(core, session))
    {
        if (const auto version = string_field(core, "version"); !version.empty()) {
            active_label = std::format("Core version {}", version);
        }
    }

    bool show_install() const
    {
        return core_state == "missing" || core_state == "installing";
    }

    float height() const
    {
        float height = 0;
        const float gap = ImGui::GetStyle().ItemSpacing.y;
        const auto line = [&](std::string_view text) {
            if (!text.empty()) {
                height += ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, width).y + gap;
            }
        };
        if (has_core) {
            line(state_label);
            const int button_rows = show_install() ? 2 : 3;
            height += button_rows * (frame_height + gap) + 2 * gap;
        }
        line(active_label);
        if (has_catalog) {
            line(target_label);
            height += 3 * (frame_height + gap);
        }
        line(notice);
        if (reconnect) {
            height += frame_height + gap;
        }
        return std::max(0.0f, height - gap);
    }

    bool button(const char* label, bool disabled = false, float button_width = 0) const
    {
        const bool compact_label = button_width > 0 && view::button_width(label) > button_width;
        if (compact_label) {
            const auto padding = ImGui::GetStyle().FramePadding;
            const auto text_width = ImGui::CalcTextSize(label, nullptr, true).x;
            const float scale = std::min(1.0f, std::max(1.0f, button_width - 4) / text_width);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2, padding.y});
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale);
        }
        ImGui::BeginDisabled(disabled);
        const bool pressed = theme::button(label, {button_width > 0 ? button_width : width, frame_height});
        ImGui::EndDisabled();
        if (compact_label) {
            ImGui::PopFont();
            ImGui::PopStyleVar();
        }
        return pressed;
    }
};

void draw_core_versions(PanelControlsState& state, PreviewSession& session, const Json& core,
                        const ControlsLayout& layout, bool disabled)
{
    if (!layout.active_label.empty()) {
        wrapped(layout.active_label);
    }
    if (!layout.has_catalog) {
        return;
    }
    wrapped(target_label, Color::secondary_text);
    help_marker("Checking for updates refreshes the available WaterWall releases.");
    const auto& versions = array_field(core, "available_versions");
    const auto active = string_field(core, "version");
    const auto selected = state.target_version.empty() ? active : std::string_view(state.target_version);
    const bool updating = core_update_pending(core);
    ImGui::BeginDisabled(disabled || updating || versions.empty());
    ImGui::SetNextItemWidth(layout.width);
    const auto padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {padding.x, (layout.frame_height - ImGui::GetFontSize()) * 0.5f});
    if (ImGui::BeginCombo("##target-core-version", selected.empty() ? "Choose version" : selected.data())) {
        std::string_view picked;
        for (const auto& item : versions) {
            const auto version = string_value(item);
            if (version.empty()) {
                continue;
            }
            if (ImGui::Selectable(version.data(), version == selected, 0, {0, 32})) {
                picked = version;
            }
            if (version == selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
        if (!picked.empty()) {
            state.target_version = picked == active ? std::string_view{} : picked;
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    const auto& server = server_snapshot(session);
    const auto generation = panel_check_generation(field(server, "panel"));
    const bool checking = session.ready() && update_check_pending(server, state.updates);
    const bool check_disabled = disabled || checking || !generation;
    if (layout.button(checking ? checking_updates_label : check_updates_label, check_disabled,
                      layout.width)) {
        state.updates.request_id = session.submit({{"action", "check_updates"}}, 5000);
        if (state.updates.request_id) {
            state.updates.check_generation = generation;
        }
    }
    const auto unavailable = core_update_unavailable(core);
    const auto update_label =
        updating ? std::format("Updating core to {}###update-core", string_field(core, "update_target"))
                 : "Change core version###update-core";
    if (layout.button(update_label.c_str(),
                      disabled || !unavailable.empty() || state.target_version == active ||
                          !has_core_version(core, state.target_version),
                      layout.width)) {
        open_dialog(state, ControlsDialog::update);
    }
    if (!unavailable.empty() &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", unavailable.data());
    }
}

bool config_enabled_switch(ImVec2 hit_size, ImVec2 track_size, bool enabled, bool active)
{
    const bool pressed = toggle_switch("##config-enabled", hit_size, track_size, enabled);
    if (ImGui::IsItemVisible() &&
        (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) ||
         (ImGui::GetIO().NavVisible && ImGui::IsItemFocused()))) {
        ImGui::SetTooltip("%s%s", enabled ? "Config enabled" : "Config disabled",
                          active ? "\nStop WaterWall before changing this config." : "");
    }
    return pressed;
}

std::string_view starter_label()
{
    const auto& schematic = waterwall_schematic();
    return users_starter_management_slot(schematic).empty()
               ? std::string_view{} : string_field(field(field(schematic, "ui"), "users"), "starter_label");
}

void render_create(PanelControlsState& state, PreviewSession& session, const Json& core)
{
    const bool pending = state.request_id.has_value();
    const bool unavailable = !session.ready() || !can_edit_configs(core);
    Dialog frame(
        "Create new config###ServerPanelControlsDialog", 380,
        {{"Cancel", pending},
         {"Create config", pending || unavailable || core_config_name(state.config_name.data()).empty()}},
        {.busy = pending});
    if (!frame) {
        return;
    }
    ImGui::TextUnformatted("Config name");
    help_marker("Wave uses this name for the config folder. Use up to 64 UTF-8 bytes and leave out slashes.");
    ImGui::BeginDisabled(pending || unavailable);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##core-config-name", "Enter a name", state.config_name.data(),
                             state.config_name.size());
    const auto& users = field(field(waterwall_schematic(), "ui"), "users");
    const auto label = starter_label();
    if (!label.empty()) {
        const float checkbox_width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                     ImGui::CalcTextSize(label.data()).x;
        const auto display = checkbox_width <= ImGui::GetContentRegionAvail().x
                                 ? label
                                 : string_field(users, "starter_compact_label", label);
        ImGui::Checkbox((std::string(display) + "###Add auth nodes").c_str(), &state.add_auth_nodes);
    }
    ImGui::EndDisabled();
    if (!label.empty()) {
        help_marker(string_field(users, "starter_help"));
        if (state.add_auth_nodes) {
            wrapped(managed_starter_notice, Color::secondary_text);
        }
    }
    const auto name = core_config_name(state.config_name.data());
    if (!name.empty() || state.config_name.front() == '\0') {
        wrapped("New configs start disabled.", Color::secondary_text);
    } else {
        wrapped("Enter a valid filename of up to 64 bytes.", Color::error);
    }
    if (unavailable && !pending) {
        wrapped(session.ready() ? "Config creation is unavailable."
                                : "The preview connection is unavailable.",
                Color::warning);
    }
    const int action = frame.footer();
    if (action == 0 || state.request_accepted.has_value()) {
        close_dialog(state);
    } else if (action == 1 && !name.empty()) {
        Json request{{"action", "create_core_config"}, {"name", state.config_name.data()}};
        if (!label.empty() && state.add_auth_nodes) {
            request["add_auth_nodes"] = true;
            // Older backends require this opt-in when their starter advertises the operation slot.
            request["allow_live_user_management"] = true;
        }
        submit(state, session, std::move(request));
    }
}

void render_confirmation(PanelControlsState& state, PreviewSession& session, const Json& core)
{
    const bool remove = state.dialog == ControlsDialog::remove;
    const char* title;
    const char* label;
    std::string question;
    std::string_view unavailable;
    Json request;
    if (remove) {
        title = "Delete config###ServerPanelControlsDialog";
        label = "Delete config";
        question = std::format("Delete {} and its config.json file?", state.remove_name);
        const auto* config = find_config(core, state.remove_id);
        if (!can_edit_configs(core) || !config) {
            unavailable = "That config is unavailable.";
        } else if (bool_field(*config, "active")) {
            unavailable = "Stop WaterWall before deleting an active config.";
        }
        request = {{"action", "remove_core_config"}, {"id", state.remove_id}};
    } else if (state.dialog == ControlsDialog::update) {
        title = "Change core version###ServerPanelControlsDialog";
        label = change_core_version;
        question = std::format("Switch WaterWall to version {}?", state.target_version);
        unavailable = core_update_unavailable(core);
        if (unavailable.empty()) {
            if (!has_core_version(core, state.target_version)) {
                unavailable = "That version is unavailable.";
            } else if (state.target_version == string_field(core, "version")) {
                unavailable = "That version is already active.";
            }
        }
        request = {{"action", "update_core"}, {"version", state.target_version}};
    } else {
        const bool restart = state.dialog == ControlsDialog::restart;
        title =
            restart ? "Restart core###ServerPanelControlsDialog" : "Stop core###ServerPanelControlsDialog";
        label = restart ? "Restart core" : "Stop core";
        question = restart ? "Restart WaterWall with the saved settings and enabled configs?"
                           : "Stop the WaterWall core process?";
        if (!can_stop(core)) {
            unavailable = restart ? "Restart is unavailable in the current core state."
                                  : "Stop is unavailable in the current core state.";
        }
        request = {{"action", restart ? "restart_core" : "stop_core"}};
    }

    const bool pending = dialog_pending(state, session);
    Dialog frame(title, 380,
                 {{"Cancel", pending}, {label, pending || !session.ready() || !unavailable.empty()}},
                 {.busy = pending});
    if (!frame) {
        return;
    }
    wrapped(question);
    if (remove && state.remove_has_unsaved_changes) {
        wrapped(unsaved_config_warning, Color::warning);
    }
    if (state.dialog == ControlsDialog::update) {
        wrapped(update_notice, Color::secondary_text);
    }
    if (!state.request_accepted && !pending) {
        if (!session.ready()) {
            wrapped("The preview connection is unavailable.", Color::warning);
        } else if (!unavailable.empty()) {
            wrapped(unavailable, Color::warning);
        }
    }
    const int action = frame.footer();
    if (action == 0 || (state.request_accepted.has_value() && !pending)) {
        close_dialog(state);
    } else if (action == 1) {
        submit(state, session, std::move(request));
    }
}

void download_backup(const PreviewSession& session)
{
    auto settings = Json::object();
    for (const auto* key : preview_backup_tls_fields) {
        settings[key] = field(field(session.data(), "settings"), key);
    }
    auto configs = Json::array();
    for (const auto& config : array_field(field(server_snapshot(session), "core"), "configs")) {
        if (!bool_field(config, "active")) {
            configs.push_back({{"name", field(config, "name")}, {"enabled", field(config, "enabled")}});
        }
    }
    const Json backup = {{"format", preview_backup_format},
                         {"version", preview_backup_version},
                         {"settings", std::move(settings)},
                         {"configs", std::move(configs)}};
    download_preview_file("wave-preview-backup.json", backup.dump(2) + '\n', "application/json");
}

void render_restore(PanelControlsState& state, PreviewSession& session)
{
    if (const auto content = take_preview_json_file()) {
        constexpr char invalid[] = "Choose a valid Wave preview backup.";
        std::string error;
        auto backup = parse_plain_core_config_json(*content, invalid, error);
        Json request = {{"action", "restore_preview_backup"}, {"backup", nullptr}};
        if (error.empty() && field(backup, "format") == preview_backup_format &&
            field(backup, "version").is_number_integer() &&
            field(backup, "version") == preview_backup_version) {
            request["backup"] = std::move(backup);
        }
        if (request["backup"].is_null() ||
            request.dump().size() + request_id_bytes > max_preview_restore_request_bytes) {
            state.restore_error = invalid;
        } else {
            state.restore_request = std::move(request);
        }
    }

    const bool pending = state.request_id.has_value();
    const bool unavailable = !can_use_backup(session);
    const bool selected = state.restore_request.is_object();
    Dialog frame("Restore backup###ServerPanelControlsDialog", 380,
                 {{"Cancel", pending}, {"Restore", pending || unavailable || !selected}}, {.busy = pending});
    if (!frame) {
        return;
    }
    wrapped(restore_message);
    ImGui::BeginDisabled(pending || unavailable);
    if (theme::button("Choose file", {-1, control_height()})) {
        state.restore_request = nullptr;
        state.restore_error.clear();
        choose_preview_json_file(max_preview_backup_file_bytes);
    }
    ImGui::EndDisabled();
    if (selected) {
        wrapped(backup_selected_message, Color::secondary_text);
    }
    if (unavailable && !pending) {
        wrapped(session.ready() ? "Backup restore is unavailable." : "The preview connection is unavailable.",
                Color::warning);
    }
    if (!state.restore_error.empty()) {
        wrapped(state.restore_error, Color::error);
    }
    const int action = frame.footer();
    if (action == 0 || state.request_accepted.has_value()) {
        clear_preview_json_file();
        state.restore_request = nullptr;
        close_dialog(state);
    } else if (action == 1) {
        submit(state, session, state.restore_request);
    }
}

} // namespace

void update_panel_controls(PanelControlsState& state, PreviewSession& session)
{
    if (auto reply = session.take_reply(state.request_id)) {
        state.request_accepted = reply->ok();
    }
    if (state.add_auth_nodes && starter_label().empty()) {
        state.add_auth_nodes = false;
    }
    const auto& core = field(server_snapshot(session), "core");
    if (state.dialog != ControlsDialog::update && (state.target_version == string_field(core, "version") ||
                                                   !has_core_version(core, state.target_version))) {
        state.target_version.clear();
    }
}

void render_create_config_menu(PanelControlsState& state, const PreviewSession& session)
{
    const auto& core = field(server_snapshot(session), "core");
    const bool available = session.ready() && !state.request_id;
    if (ImGui::MenuItem("Create config", nullptr, false, available && can_edit_configs(core))) {
        state.config_name.fill('\0');
        state.add_auth_nodes = false;
        open_dialog(state, ControlsDialog::create);
    }
}

void render_config_header(PanelControlsState& state, PreviewSession& session, const Json& config,
                          int config_id, bool dirty, bool narrow)
{
    constexpr char delete_label[] = "Delete";
    const auto& style = ImGui::GetStyle();
    const float height = narrow ? 44.0f : ImGui::GetTextLineHeight();
    const ImVec2 switch_hit{narrow ? 44.0f : 50.0f, height};
    const ImVec2 switch_track = narrow ? ImVec2{38, 22} : ImVec2{44, std::min(height - 2, 22.0f)};
    const float delete_width = std::max(view::button_width(delete_label), narrow ? 44.0f : 0.0f);
    const auto name = string_field(config, "name");
    const auto start = ImGui::GetCursorScreenPos();
    const bool pending = config_action_pending(state, config_id);
    const float indicator = pending ? busy_spinner_width + style.ItemSpacing.x : 0;
    const float offset =
        ImGui::GetContentRegionAvail().x - switch_hit.x - style.ItemSpacing.x - delete_width - indicator;
    const float title_width = offset - style.ItemSpacing.x;
    if (title_width > 0) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + title_width);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            {style.FramePadding.x, (height - ImGui::GetFontSize()) / 2});
        ImGui::AlignTextToFramePadding();
        text(name);
        ImGui::PopStyleVar();
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({start.x + offset, start.y});
    } else {
        wrapped(name);
    }
    if (pending) {
        draw_busy_spinner(height);
        ImGui::SameLine();
    }

    const auto& core = field(server_snapshot(session), "core");
    const bool active = bool_field(config, "active");
    const bool available = session.ready() && can_edit_configs(core) && !active && !state.request_id;
    const bool enabled = bool_field(config, "enabled");
    ImGui::BeginDisabled(!available);
    if (config_enabled_switch(switch_hit, switch_track, enabled, active)) {
        submit(state, session,
               {{"action", "set_core_config_enabled"}, {"id", config_id}, {"enabled", !enabled}});
        state.action_config_id = config_id;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(Color::error));
    const bool pressed_delete =
        narrow ? theme::button(delete_label, {delete_width, height}) : theme::small_button(delete_label);
    ImGui::PopStyleColor();
    if (pressed_delete) {
        state.remove_id = config_id;
        state.remove_name = name;
        state.remove_has_unsaved_changes = dirty;
        open_dialog(state, ControlsDialog::remove);
    }
    if (active && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Stop WaterWall before deleting this config.");
    }
    ImGui::EndDisabled();
}

bool config_action_pending(const PanelControlsState& state, int config_id)
{
    return state.request_id && state.action_config_id == config_id;
}

float panel_controls_height(const PreviewSession& session, float width, bool compact)
{
    return ControlsLayout(session, field(server_snapshot(session), "core"), width, compact).height();
}

void render_panel_controls(PanelControlsState& state, PreviewSession& session, float width, bool compact)
{
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
    const ControlsLayout layout(session, core, width, compact);
    const bool lifecycle_busy = core_transition_pending(session);
    const bool disabled = !session.ready() || state.request_id.has_value();
    const bool checking_core = session.ready() && bool_field(core, "checking_updates");
    const bool checking = session.ready() && update_check_pending(server, state.updates);
    const bool busy = state.dialog == ControlsDialog::none &&
                      (checking_core || checking || lifecycle_busy ||
                       (state.request_id && state.action_config_id == 0));
    const float button_space = std::max(2.0f, width - ImGui::GetStyle().ItemSpacing.x);
    const float primary_width = std::floor(button_space * 2.0f / 3.0f);
    const float secondary_width = button_space - primary_width;
    ImGui::BeginGroup();
    if (layout.has_core) {
        if (busy) {
            draw_busy_spinner(ImGui::GetTextLineHeight());
            ImGui::SameLine();
        }
        const auto state_color = core_status_color(layout.core_state, Color::primary_text);
        if (state_color != Color::primary_text &&
            ImGui::CalcTextSize(layout.state_label.c_str()).x <= ImGui::GetContentRegionAvail().x) {
            text(core_state_prefix);
            ImGui::SameLine(0, 0);
            wrapped(std::string_view(layout.state_label).substr(core_state_prefix.size()), state_color);
        } else {
            wrapped(layout.state_label, state_color);
        }
        const bool lifecycle_disabled = disabled || lifecycle_busy || !supports(core, "lifecycle");
        if (layout.show_install()) {
            const bool installing = layout.core_state == "installing";
            if (layout.button("Install core", lifecycle_disabled || installing || checking_core)) {
                submit(state, session, {{"action", "install_core"}});
            }
        } else {
            if (layout.button("Start core", lifecycle_disabled || layout.core_state != "stopped",
                              primary_width)) {
                submit(state, session, {{"action", "start_core"}});
            }
            ImGui::SameLine();
            const bool stop_disabled = disabled || !can_stop(core);
            const char* stop_label = ImGui::CalcTextSize("Stop core").x + 4 <= secondary_width
                                         ? "Stop core###stop-core"
                                         : "Stop###stop-core";
            if (layout.button(stop_label, stop_disabled, secondary_width)) {
                open_dialog(state, ControlsDialog::stop);
            }
            if (layout.button("Restart core", stop_disabled)) {
                open_dialog(state, ControlsDialog::restart);
            }
        }
        ImGui::Spacing();
    }
    draw_core_versions(state, session, core, layout, disabled);
    if (layout.has_core) {
        ImGui::Spacing();
        const bool backup_disabled = disabled || !can_use_backup(session);
        if (layout.button("Backup", backup_disabled, primary_width)) {
            download_backup(session);
            session.add_local_log("info", "Requested download of the Wave preview backup.");
        }
        ImGui::SameLine();
        if (layout.button("Restore", backup_disabled, secondary_width)) {
            state.restore_error.clear();
            open_dialog(state, ControlsDialog::restore);
        }
    }
    if (!layout.notice.empty()) {
        wrapped(layout.notice, Color::secondary_text);
    }
    if (layout.reconnect && layout.button("Reconnect")) {
        session.start(true);
    }
    ImGui::EndGroup();

    // On a stacked page, ImGui lays out the full canvas and the browser scrolls it.
    const auto& ui = *ImGui::GetCurrentContext();
    const auto* window = ImGui::GetCurrentWindowRead();
    const auto focus = ImGui::GetIO().NavVisible && ui.NavWindow == window ? ui.NavId : 0;
    if (focus && focus != state.keyboard_focus) {
        const auto& rect = window->NavRectRel[ImGuiNavLayer_Main];
        reveal_canvas_item(window->Pos.y + rect.Min.y, window->Pos.y + rect.Max.y);
    }
    state.keyboard_focus = focus;
}

void render_panel_control_dialog(PanelControlsState& state, PreviewSession& session)
{
    if (state.open_dialog) {
        ImGui::OpenPopup(popup_id);
        state.open_dialog = false;
    }
    const auto& core = field(server_snapshot(session), "core");
    switch (state.dialog) {
    case ControlsDialog::none:
        break;
    case ControlsDialog::create:
        render_create(state, session, core);
        break;
    case ControlsDialog::restore:
        render_restore(state, session);
        break;
    case ControlsDialog::stop:
    case ControlsDialog::restart:
    case ControlsDialog::update:
    case ControlsDialog::remove:
        render_confirmation(state, session, core);
        break;
    }
}

void render_panel_update_dialog(PanelUpdateState& state, PreviewSession& session)
{
    if (const auto reply = session.take_reply(state.request_id);
        !session.ready() || (reply && !reply->ok())) {
        state.check_generation.reset();
    }
    const auto& server = server_snapshot(session);
    const auto& panel = field(server, "panel");
    const auto generation = panel_check_generation(panel);
    if (state.check_generation && !state.request_id && generation && generation != state.check_generation &&
        !bool_field(server, "checking_updates") && !bool_field(panel, "checking_updates") &&
        !bool_field(field(server, "core"), "checking_updates")) {
        state.check_generation.reset();
        if (bool_field(panel, "update_available") && string_field(panel, "update_check_error").empty()) {
            state.available_version = string_field(panel, "latest_version");
            state.open_dialog = !state.available_version.empty();
        }
    }

    if (state.open_dialog) {
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            return;
        }
        ImGui::OpenPopup(panel_update_title);
        state.open_dialog = false;
    }
    if (state.available_version.empty()) {
        return;
    }
    Dialog frame(panel_update_title, 380, {{"Close"}, {build_guide_label}});
    if (!frame) {
        state.available_version.clear();
        return;
    }
    wrapped(panel_update_message(state));
    const int action = frame.footer();
    if (action == 0) {
        state.available_version.clear();
        ImGui::CloseCurrentPopup();
    } else if (action == 1) {
        if (const auto open_link = ImGui::GetPlatformIO().Platform_OpenInShellFn) {
            open_link(ImGui::GetCurrentContext(), build_guide_url);
        }
    }
}

std::string describe_panel_update_dialog(const PanelUpdateState& state)
{
    if (state.available_version.empty() || state.open_dialog) {
        return {};
    }
    return std::format("{}. {} Close. {}.", panel_update_title, panel_update_message(state),
                       build_guide_label);
}

std::string describe_panel_controls(const PanelControlsState& state, const PreviewSession& session,
                                    bool include_status)
{
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
    const bool checking_updates = session.ready() && update_check_pending(server, state.updates);
    std::string description;
    if (include_status) {
        description = "Panel Controls. ";
        if (core.is_object()) {
            description += core_state_label(core) + ' ';
        }
        if (const auto active = string_field(core, "version"); !active.empty()) {
            const auto target =
                state.target_version.empty() ? active : std::string_view(state.target_version);
            description += std::format("Core version {}. Target version {}. ", active, target);
        }
        if (field(core, "available_versions").is_array()) {
            description += checking_updates ? "Checking for updates. " : "Check for updates. ";
            description += "Change core version. ";
        }
        if (const auto updating = string_field(core, "update_target"); session.ready() && !updating.empty()) {
            description += std::format("Updating core to {}. ", updating);
        } else if (!state.target_version.empty() && string_field(core, "state") == "running") {
            description += stop_before_update;
            description += ' ';
        }
        const auto notice = controls_notice(core, session);
        if (!notice.empty()) {
            description += notice + ' ';
        }
    }
    switch (state.dialog) {
    case ControlsDialog::none:
        break;
    case ControlsDialog::stop:
        description += "Confirm stop. ";
        break;
    case ControlsDialog::restart:
        description += "Confirm restart. ";
        break;
    case ControlsDialog::update:
        description +=
            std::format("Confirm core version change to {}. {} ", state.target_version, update_notice);
        break;
    case ControlsDialog::create:
        description += "Create new config. ";
        if (const auto label = starter_label(); !label.empty()) {
            description += label;
            description += state.add_auth_nodes ? " checked. " : " unchecked. ";
            if (state.add_auth_nodes) {
                description += managed_starter_notice;
                description += ' ';
            }
        }
        break;
    case ControlsDialog::remove:
        description += std::format("Confirm deletion of {} and its config.json file. ", state.remove_name);
        if (state.remove_has_unsaved_changes) {
            description += std::string(unsaved_config_warning) + ' ';
        }
        break;
    case ControlsDialog::restore:
        description += "Restore backup. ";
        if (state.restore_request.is_object()) {
            description += std::string(backup_selected_message) + ' ';
        }
        if (!state.restore_error.empty()) {
            description += state.restore_error + ' ';
        }
        break;
    }
    const bool core_busy =
        include_status && state.dialog == ControlsDialog::none &&
        (checking_updates || core_transition_pending(session) ||
         (session.ready() && bool_field(core, "checking_updates")));
    if (core_busy || (state.action_config_id == 0 && dialog_pending(state, session))) {
        description += busy_description;
    }
    return description;
}

} // namespace wave
