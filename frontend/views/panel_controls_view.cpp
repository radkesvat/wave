#include "panel_controls_view.h"

#include "browser_ui.h"
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
constexpr char pending_message[] = "Applying change…";
constexpr char disconnected_message[] = "The preview connection closed before confirming the change.";
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
        return session.status() == PreviewStatus::loading
                   ? "Reconnecting to the preview…"
                   : "Preview disconnected. Showing last reported values.";
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
    state.reply.reset();
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
    const auto action = string_field(request, "action");
    state.lifecycle_request = action == "install_core" || action == "start_core" || action == "stop_core";
    state.reply.reset();
    state.request_id = session.submit(std::move(request), 5000);
    if (!state.request_id) {
        state.reply =
            PreviewReply{PreviewReply::Status::disconnected, "The preview connection is unavailable."};
    }
}

void show_result(const PanelControlsState& state, const PreviewSession& session)
{
    if (state.request_id) {
        wrapped(pending_message, Color::info);
    } else if (state.reply) {
        feedback(*state.reply);
    }
    if (!session.ready() && (!state.reply || state.reply->status != PreviewReply::Status::disconnected)) {
        wrapped("The preview connection is unavailable.", Color::warning);
    }
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
    std::string_view core_error;
    std::string state_label;
    std::string active_label;
    std::string notice;
    std::string_view result;

    ControlsLayout(const PanelControlsState& state, const PreviewSession& session, const Json& server,
                   float width, bool compact)
        : width(width), frame_height(control_height(compact ? 32.0f : 40.0f)),
          has_core(field(server, "core").is_object()),
          reconnect(session.status() == PreviewStatus::unavailable),
          core_state(string_field(field(server, "core"), "state")),
          core_error(string_field(field(server, "core"), "error")),
          state_label(core_state_label(field(server, "core"))),
          active_label(std::format("Panel version {}", string_field(server, "version", wave::version))),
          notice(controls_notice(server, session)),
          result(state.request_id ? pending_message
                 : state.reply    ? std::string_view(state.reply->message)
                                  : "")
    {
        if (core_error == result) {
            core_error = {};
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
            line(core_error);
            const int button_count = show_install() ? 3 : 4;
            height += button_count * (frame_height + gap) + 2 * gap;
        }
        line(active_label);
        line(target_label);
        height += 2 * (frame_height + gap);
        line(notice);
        line(result);
        if (reconnect) {
            height += frame_height + gap;
        }
        return height - gap;
    }

    bool button(const char* label, bool disabled = false) const
    {
        ImGui::BeginDisabled(disabled);
        const bool pressed = theme::button(label, {width, frame_height});
        ImGui::EndDisabled();
        return pressed;
    }
};

void draw_panel_versions(PanelControlsState& state, const Json& server, const ControlsLayout& layout,
                         bool disabled)
{
    wrapped(layout.active_label);
    wrapped(target_label, Color::secondary_text);
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

void render_create(PanelControlsState& state, PreviewSession& session, const Json& core)
{
    const bool pending = state.request_id.has_value();
    const bool unavailable = !session.ready() || !can_edit_configs(core);
    Dialog frame(
        "Create new config###ServerPanelControlsDialog", 380,
        {{"Cancel", pending},
         {"Create config", pending || unavailable || core_config_name(state.config_name.data()).empty()}});
    if (!frame) {
        return;
    }
    ImGui::TextUnformatted("Config name");
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
    if (session.ready() && unavailable) {
        wrapped("Config creation is unavailable.", Color::warning);
    }
    show_result(state, session);
    const int action = frame.footer();
    if (action == 0 || (state.reply && state.reply->ok())) {
        close_dialog(state);
    } else if (action == 1 && !name.empty()) {
        submit(state, session, {{"action", "create_core_config"}, {"name", state.config_name.data()}});
    }
}

void render_configs(PanelControlsState& state, PreviewSession& session, const Json& core)
{
    const bool pending = state.request_id.has_value();
    Dialog frame("Manage configs###ServerPanelControlsDialog", 380, {{"Close", pending}});
    if (!frame) {
        return;
    }
    const bool supported = can_edit_configs(core);
    const auto& configs = array_field(core, "configs");
    if (configs.empty()) {
        wrapped("No configs.", Color::secondary_text);
    }
    if (session.ready() && !supported) {
        wrapped("Config changes are unavailable.", Color::warning);
    }
    for (const auto& config : configs) {
        const int id = integer_field(config, "id");
        const auto name = string_field(config, "name");
        const bool active = bool_field(config, "active");
        bool enabled = bool_field(config, "enabled");
        ImGui::PushID(id);
        wrapped(active ? std::format("{} (active and protected)", name) : std::string(name));
        ImGui::BeginDisabled(id <= 0 || active || !session.ready() || !supported ||
                             state.request_id.has_value());
        const auto padding = ImGui::GetStyle().FramePadding;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            {padding.x, (control_height() - ImGui::GetFontSize()) * 0.5f});
        if (ImGui::Checkbox("Enabled", &enabled)) {
            submit(state, session, {{"action", "set_core_config_enabled"}, {"id", id}, {"enabled", enabled}});
        }
        ImGui::PopStyleVar();
        ImGui::SameLine();
        if (theme::button("Remove", {0, control_height()})) {
            state.remove_id = id;
            state.remove_name = name;
            state.dialog = ControlsDialog::remove;
            state.reply.reset();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::Separator();
    }
    show_result(state, session);
    if (frame.footer() == 0) {
        close_dialog(state);
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
        title = "Remove config###ServerPanelControlsDialog";
        label = "Remove";
        question = std::format("Remove {} and its config.json file?", state.remove_name);
        const auto* config = find_config(core, state.remove_id);
        if (!can_edit_configs(core) || !config) {
            unavailable = "That config is unavailable.";
        } else if (bool_field(*config, "active")) {
            unavailable = "Stop WaterWall before removing an active config.";
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
        title = "Stop core###ServerPanelControlsDialog";
        label = "Stop core";
        question = "Stop the WaterWall core process?";
        if (!can_stop(core)) {
            unavailable = "Stop is unavailable in the current core state.";
        }
        request = {{"action", "stop_core"}};
    }

    const bool pending = state.request_id.has_value();
    Dialog frame(title, 380,
                 {{"Cancel", pending}, {label, pending || !session.ready() || !unavailable.empty()}});
    if (!frame) {
        return;
    }
    wrapped(question);
    if (session.ready() && !unavailable.empty() && !state.reply && !pending) {
        wrapped(unavailable, Color::warning);
    }
    show_result(state, session);
    const int action = frame.footer();
    if (action == 0 || (state.reply && state.reply->ok())) {
        if (remove) {
            state.dialog = ControlsDialog::manage;
            if (action == 0) {
                state.reply.reset();
            }
        } else {
            close_dialog(state);
        }
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
        auto backup = Json::parse(*content, nullptr, false);
        Json request = {{"action", "restore_preview_backup"}, {"backup", nullptr}};
        if (field(backup, "format") == preview_backup_format &&
            field(backup, "version").is_number_integer() &&
            field(backup, "version") == preview_backup_version) {
            request["backup"] = std::move(backup);
        }
        if (request["backup"].is_null() ||
            request.dump().size() + request_id_bytes > max_preview_restore_request_bytes) {
            state.reply = PreviewReply{PreviewReply::Status::rejected, "Choose a valid Wave preview backup."};
        } else {
            state.restore_request = std::move(request);
        }
    }

    const bool pending = state.request_id.has_value();
    const bool unavailable = !can_use_backup(session);
    const bool selected = state.restore_request.is_object();
    Dialog frame("Restore backup###ServerPanelControlsDialog", 380,
                 {{"Cancel", pending}, {"Restore", pending || unavailable || !selected}});
    if (!frame) {
        return;
    }
    wrapped(restore_message);
    ImGui::BeginDisabled(pending || unavailable);
    if (theme::button("Choose file", {-1, control_height()})) {
        state.restore_request = nullptr;
        state.reply.reset();
        choose_preview_json_file(max_preview_backup_file_bytes);
    }
    ImGui::EndDisabled();
    if (selected) {
        wrapped(backup_selected_message, Color::secondary_text);
    }
    show_result(state, session);
    const int action = frame.footer();
    if (action == 0 || (state.reply && state.reply->ok())) {
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
        if (reply->status == PreviewReply::Status::timed_out) {
            reply->message =
                "The preview did not confirm the change. Check the current state before retrying.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = disconnected_message;
        } else if (state.lifecycle_request && reply->ok()) {
            // The current process state reports progress after a lifecycle request is accepted.
            reply->message.clear();
        }
        state.reply = std::move(reply);
    }
    const auto& server = server_snapshot(session);
    if (state.dialog != ControlsDialog::update && (state.target_version == string_field(server, "version") ||
                                                   !has_panel_version(server, state.target_version))) {
        state.target_version.clear();
    }
}

void render_panel_config_menu(PanelControlsState& state, const PreviewSession& session)
{
    const auto& core = field(server_snapshot(session), "core");
    const bool available = session.ready() && !state.request_id;
    if (ImGui::MenuItem("Create config", nullptr, false, available && can_edit_configs(core))) {
        state.config_name.fill('\0');
        open_dialog(state, ControlsDialog::create);
    }
    if (ImGui::MenuItem("Manage configs", nullptr, false, available && field(core, "configs").is_array())) {
        open_dialog(state, ControlsDialog::manage);
    }
}

float panel_controls_height(const PanelControlsState& state, const PreviewSession& session, float width,
                            bool compact)
{
    return ControlsLayout(state, session, server_snapshot(session), width, compact).height();
}

void render_panel_controls(PanelControlsState& state, PreviewSession& session, float width, bool compact)
{
    const auto& server = server_snapshot(session);
    const auto& core = field(server, "core");
    const ControlsLayout layout(state, session, server, width, compact);
    const bool disabled = !session.ready() || state.request_id.has_value();
    ImGui::BeginGroup();
    if (layout.has_core) {
        wrapped(layout.state_label);
        const bool lifecycle_disabled = disabled || !supports(core, "lifecycle");
        if (layout.show_install()) {
            const bool installing = layout.core_state == "installing";
            if (layout.button(installing ? "Installing core…" : "Install core",
                              lifecycle_disabled || installing)) {
                submit(state, session, {{"action", "install_core"}});
            }
        } else {
            if (layout.button("Start core", lifecycle_disabled || layout.core_state != "stopped")) {
                submit(state, session, {{"action", "start_core"}});
            }
            if (layout.button("Stop core", disabled || !can_stop(core))) {
                open_dialog(state, ControlsDialog::stop);
            }
        }
        if (!layout.core_error.empty()) {
            wrapped(layout.core_error, Color::error);
        }
        ImGui::Spacing();
    }
    draw_panel_versions(state, server, layout, disabled);
    if (layout.has_core) {
        ImGui::Spacing();
        const bool backup_disabled = disabled || !can_use_backup(session);
        if (layout.button("Backup", backup_disabled)) {
            download_backup(session);
        }
        if (layout.button("Restore", backup_disabled)) {
            open_dialog(state, ControlsDialog::restore);
        }
    }
    if (!layout.notice.empty()) {
        wrapped(layout.notice, Color::secondary_text);
    }
    if (state.request_id) {
        wrapped(pending_message, Color::info);
    } else if (state.reply) {
        feedback(*state.reply);
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
    case ControlsDialog::manage:
        render_configs(state, session, core);
        break;
    case ControlsDialog::restore:
        render_restore(state, session);
        break;
    case ControlsDialog::stop:
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
            const auto error = string_field(core, "error");
            if (!error.empty() && (state.request_id || !state.reply || state.reply->message != error)) {
                description += std::format("Core error. {} ", error);
            }
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
    case ControlsDialog::update:
        description += std::format("Confirm panel update to {}. ", state.target_version);
        break;
    case ControlsDialog::create:
        description += "Create new config. ";
        break;
    case ControlsDialog::manage: {
        description += "Manage configs. ";
        const auto& configs = array_field(core, "configs");
        if (configs.empty()) {
            description += "No configs. ";
        }
        for (const auto& config : configs) {
            description += std::format("{} {}{}. ", string_field(config, "name"),
                                       bool_field(config, "enabled") ? "enabled" : "disabled",
                                       bool_field(config, "active") ? ", active and protected" : "");
        }
        break;
    }
    case ControlsDialog::remove:
        description += std::format("Confirm removal of {}. ", state.remove_name);
        break;
    case ControlsDialog::restore:
        description += "Restore backup. ";
        if (state.restore_request.is_object()) {
            description += std::string(backup_selected_message) + ' ';
        }
        break;
    }
    if (state.request_id) {
        description += pending_message;
    } else if (state.reply) {
        description += state.reply->message;
    }
    return description;
}

} // namespace wave
