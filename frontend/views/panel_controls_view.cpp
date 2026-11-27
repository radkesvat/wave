#include "panel_controls_view.h"

#include "browser_ui.h"
#include "core_config_json.h"
#include "dialog.h"
#include "preview_backup.h"
#include "view_helpers.h"
#include "wave_version.h"

#include <imgui_internal.h>

#include <format>
#include <utility>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using ControlsDialog = PanelControlsState::Dialog;

constexpr char popup_id[] = "###ServerPanelControlsDialog";
constexpr char target_label[] = "Target panel version";
constexpr char unsaved_config_warning[] = "Unsaved node changes will be lost.";
constexpr char restore_message[] =
    "Choose a backup JSON file to replace the sample TLS settings and inactive config list.";
constexpr char backup_selected_message[] = "Wave preview backup selected.";
constexpr auto request_id_bytes = sizeof(",\"request_id\":18446744073709551615") - 1;

const Json& server_snapshot(const PreviewSession& session)
{
    return field(session.data(), "server");
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

bool has_panel_version(const Json& server, std::string_view version)
{
    const auto& versions = array_field(server, "available_panel_versions");
    return !version.empty() && std::ranges::find(versions, version, [](const Json& item) {
                                   return string_value(item);
                               }) != versions.end();
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
    return std::format("WaterWall core {}.", state);
}

std::string controls_notice(const Json& server, const PreviewSession& session)
{
    if (!session.ready()) {
        return session.status() == PreviewStatus::unavailable
                   ? "Preview disconnected. Showing last reported values."
                   : "";
    }
    const auto& core = field(server, "core");
    const bool lifecycle = supports(core, "lifecycle");
    const bool versions = !array_field(server, "available_panel_versions").empty();
    const bool configs = can_edit_configs(core);
    if (!lifecycle && !versions && !configs) {
        return "Panel controls are unavailable in this preview.";
    }
    std::string notice;
    if (!lifecycle) {
        notice = "Lifecycle controls are unavailable. ";
    }
    if (!versions) {
        notice += "Panel version updates are unavailable. ";
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
    const auto core_state = string_field(field(server_snapshot(session), "core"), "state");
    return session.ready() &&
           (core_state == "installing" || core_state == "starting" || core_state == "stopping");
}

bool dialog_pending(const PanelControlsState& state, const PreviewSession& session)
{
    const bool lifecycle = state.dialog == ControlsDialog::stop || state.dialog == ControlsDialog::restart;
    return state.request_id ||
           (lifecycle && state.request_accepted.value_or(false) && core_transition_pending(session));
}

float control_height(float minimum = 40.0f)
{
    return std::max(minimum, ImGui::GetFrameHeight());
}

struct ControlsLayout {
    float width;
    float frame_height;
    bool has_core;
    bool reconnect;
    std::string_view core_state;
    std::string state_label;
    std::string active_label;
    std::string notice;

    ControlsLayout(const PreviewSession& session, const Json& server, float width, bool compact)
        : width(width), frame_height(control_height(compact ? 32.0f : 40.0f)),
          has_core(field(server, "core").is_object()),
          reconnect(session.status() == PreviewStatus::unavailable),
          core_state(string_field(field(server, "core"), "state")),
          state_label(core_state_label(field(server, "core"))),
          active_label(std::format("Panel version {}", string_field(server, "version", wave::version))),
          notice(controls_notice(server, session))
    {
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
        line(target_label);
        height += 2 * (frame_height + gap);
        line(notice);
        if (reconnect) {
            height += frame_height + gap;
        }
        return height - gap;
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

void draw_panel_versions(PanelControlsState& state, const Json& server, const ControlsLayout& layout,
                         bool disabled)
{
    wrapped(layout.active_label);
    wrapped(target_label, Color::secondary_text);
    help_marker("This selects a release of the Wave panel. WaterWall runs as a separate core process.");
    const auto& versions = array_field(server, "available_panel_versions");
    const auto active = string_field(server, "version", wave::version);
    const auto selected = state.target_version.empty() ? active : std::string_view(state.target_version);
    disabled |= versions.empty();
    ImGui::BeginDisabled(disabled);
    ImGui::SetNextItemWidth(layout.width);
    const auto padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {padding.x, (layout.frame_height - ImGui::GetFontSize()) * 0.5f});
    if (ImGui::BeginCombo("##target-panel-version", selected.empty() ? "Choose version" : selected.data())) {
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
    if (layout.button("Update panel",
                      disabled || state.target_version.empty() || state.target_version == active)) {
        open_dialog(state, ControlsDialog::update);
    }
}

bool config_enabled_switch(ImVec2 hit_size, ImVec2 track_size, bool enabled, bool available, bool active)
{
    const bool pressed = ImGui::InvisibleButton("##config-enabled", hit_size, ImGuiButtonFlags_EnableNav);
    if (ImGui::IsItemVisible()) {
        const auto origin = ImGui::GetItemRectMin();
        const ImVec2 min{origin.x + (hit_size.x - track_size.x) / 2,
                         origin.y + (hit_size.y - track_size.y) / 2};
        const ImVec2 max{min.x + track_size.x, min.y + track_size.y};
        const float radius = track_size.y / 2;
        const bool hovered = available && ImGui::IsItemHovered();
        const auto track = enabled ? Color::accent : hovered ? Color::hover : Color::raised;
        const auto thumb = enabled ? Color::canvas : Color::primary_text;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(theme::packed(track)), radius);
        if (!enabled) {
            draw->AddRect(min, max, ImGui::GetColorU32(theme::packed(Color::control_border)), radius,
                          theme::border_width);
        }
        draw->AddCircleFilled({enabled ? max.x - radius : min.x + radius, min.y + radius}, radius - 3,
                              ImGui::GetColorU32(theme::packed(thumb)));
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) ||
            (ImGui::GetIO().NavVisible && ImGui::IsItemFocused())) {
            ImGui::SetTooltip("%s%s", enabled ? "Config enabled" : "Config disabled",
                              active ? "\nStop WaterWall before changing this config." : "");
        }
    }
    return pressed;
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
    ImGui::EndDisabled();
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
        submit(state, session, {{"action", "create_core_config"}, {"name", state.config_name.data()}});
    }
}

void render_confirmation(PanelControlsState& state, PreviewSession& session, const Json& server)
{
    const auto& core = field(server, "core");
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
        title = "Update panel###ServerPanelControlsDialog";
        label = "Update panel";
        question = std::format("Update Wave to version {}?", state.target_version);
        if (!has_panel_version(server, state.target_version)) {
            unavailable = "That panel version is unavailable.";
        } else if (state.target_version == string_field(server, "version")) {
            unavailable = "That panel version is already active.";
        }
        request = {{"action", "update_panel"}, {"version", state.target_version}};
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
    const auto& server = server_snapshot(session);
    if (state.dialog != ControlsDialog::update && (state.target_version == string_field(server, "version") ||
                                                   !has_panel_version(server, state.target_version))) {
        state.target_version.clear();
    }
}

void render_create_config_menu(PanelControlsState& state, const PreviewSession& session)
{
    const auto& core = field(server_snapshot(session), "core");
    const bool available = session.ready() && !state.request_id;
    if (ImGui::MenuItem("Create config", nullptr, false, available && can_edit_configs(core))) {
        state.config_name.fill('\0');
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
    if (config_enabled_switch(switch_hit, switch_track, enabled, available, active)) {
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
    return ControlsLayout(session, server_snapshot(session), width, compact).height();
}

void render_panel_controls(PanelControlsState& state, PreviewSession& session, float width, bool compact)
{
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
    const ControlsLayout layout(session, server, width, compact);
    const bool lifecycle_busy = core_transition_pending(session);
    const bool disabled = !session.ready() || state.request_id.has_value();
    const bool busy = state.dialog == ControlsDialog::none &&
                      (lifecycle_busy || (state.request_id && state.action_config_id == 0) ||
                       session.status() == PreviewStatus::loading);
    const float button_space = std::max(2.0f, width - ImGui::GetStyle().ItemSpacing.x);
    const float primary_width = std::floor(button_space * 2.0f / 3.0f);
    const float secondary_width = button_space - primary_width;
    ImGui::BeginGroup();
    if (layout.has_core) {
        if (busy) {
            draw_busy_spinner(ImGui::GetTextLineHeight());
            ImGui::SameLine();
        }
        wrapped(layout.state_label);
        const bool lifecycle_disabled = disabled || lifecycle_busy || !supports(core, "lifecycle");
        if (layout.show_install()) {
            const bool installing = layout.core_state == "installing";
            if (layout.button("Install core", lifecycle_disabled || installing)) {
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
    draw_panel_versions(state, server, layout, disabled);
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
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
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
        render_confirmation(state, session, server);
        break;
    }
}

std::string describe_panel_controls(const PanelControlsState& state, const PreviewSession& session,
                                    bool include_status)
{
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
    std::string description;
    if (include_status) {
        description = "Panel Controls. ";
        if (core.is_object()) {
            description += core_state_label(core) + ' ';
        }
        const auto active = string_field(server, "version", wave::version);
        const auto target = state.target_version.empty() ? active : std::string_view(state.target_version);
        description += std::format("Panel version {}. Target panel version {}. ", active, target);
        const auto notice = controls_notice(server, session);
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
        description += std::format("Confirm panel update to {}. ", state.target_version);
        break;
    case ControlsDialog::create:
        description += "Create new config. ";
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
    const bool lifecycle_busy =
        include_status && state.dialog == ControlsDialog::none && core_transition_pending(session);
    if (lifecycle_busy || (state.action_config_id == 0 && dialog_pending(state, session))) {
        description += busy_description;
    }
    return description;
}

} // namespace wave
