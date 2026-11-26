#include "core_controls_view.h"

#include "browser_ui.h"
#include "dialog.h"
#include "view_helpers.h"

#include <imgui_internal.h>

#include <format>
#include <utility>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using CoreDialog = CoreControlsState::Dialog;

constexpr char popup_id[] = "###ServerCoreDialog";
constexpr char heading[] = "Core Controls";
constexpr char target_label[] = "Target version";
constexpr char pending_message[] = "Applying to preview…";
constexpr char disconnected_message[] = "The preview connection closed before confirming the change.";

const Json& core_snapshot(const PreviewSession& session)
{
    return field(field(session.data(), "server"), "core");
}

bool supports(const Json& core, std::string_view capability)
{
    return bool_field(field(core, "capabilities"), capability);
}

bool can_edit_configs(const Json& core)
{
    return supports(core, "configs") && field(core, "configs").is_array();
}

bool can_stop(const Json& core)
{
    return supports(core, "lifecycle") && string_field(core, "state") == "running";
}

bool has_version(const Json& core, std::string_view version)
{
    const auto& versions = array_field(core, "available_versions");
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
    return std::format("Preview core {}.", state);
}

std::string core_notice(const Json& core, const PreviewSession& session)
{
    if (!session.ready()) {
        return session.status() == PreviewStatus::loading
                   ? "Reconnecting to the preview…"
                   : "Preview disconnected. Showing last reported values.";
    }
    const bool lifecycle = supports(core, "lifecycle");
    const bool versions = supports(core, "versions") && !array_field(core, "available_versions").empty();
    const bool configs = can_edit_configs(core);
    if (!lifecycle && !versions && !configs) {
        return "Core controls are unavailable in this preview.";
    }
    std::string notice;
    if (!lifecycle) {
        notice = "Lifecycle controls are unavailable. ";
    }
    if (!versions) {
        notice += "Version updates are unavailable. ";
    }
    if (!configs) {
        notice += "Config changes are unavailable.";
    }
    return notice;
}

void open_dialog(CoreControlsState& state, CoreDialog dialog)
{
    state.dialog = dialog;
    state.open_dialog = true;
    state.reply.reset();
}

void close_dialog(CoreControlsState& state)
{
    ImGui::CloseCurrentPopup();
    state.dialog = CoreDialog::none;
}

void submit(CoreControlsState& state, PreviewSession& session, Json request)
{
    if (state.request_id) {
        return;
    }
    state.reply.reset();
    state.request_id = session.submit(std::move(request), 5000);
    if (!state.request_id) {
        state.reply =
            PreviewReply{PreviewReply::Status::disconnected, "The preview connection is unavailable."};
    }
}

void show_result(const CoreControlsState& state, const PreviewSession& session)
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

float control_height()
{
    return std::max(40.0f, ImGui::GetFrameHeight());
}

bool fits(float width, std::initializer_list<const char*> labels)
{
    float needed = (labels.size() - 1) * ImGui::GetStyle().ItemSpacing.x;
    for (const char* label : labels) {
        needed += button_width(label);
    }
    return width >= needed;
}

float version_combo_width(const Json& core, float frame_height)
{
    float label_width = ImGui::CalcTextSize("Choose version").x;
    for (const auto& item : array_field(core, "available_versions")) {
        const auto label = string_value(item);
        if (!label.empty()) {
            label_width =
                std::max(label_width, ImGui::CalcTextSize(label.data(), label.data() + label.size()).x);
        }
    }
    return label_width + frame_height + 2 * ImGui::GetStyle().FramePadding.x;
}

struct ControlsLayout {
    float width;
    float frame_height = control_height();
    bool has_core;
    bool reconnect;
    bool stop_inline;
    bool manage_inline;
    float combo_width;
    bool update_inline;
    std::string state_label;
    std::string active_label;
    std::string notice;
    std::string_view result;

    ControlsLayout(const CoreControlsState& state, const PreviewSession& session, float width)
        : width(width), has_core(core_snapshot(session).is_object()),
          reconnect(session.status() == PreviewStatus::unavailable),
          stop_inline(fits(width, {"Start", "Stop"})),
          manage_inline(fits(width, {"Create config", "Manage configs"})),
          combo_width(version_combo_width(core_snapshot(session), frame_height)),
          update_inline(width >= combo_width + ImGui::GetStyle().ItemSpacing.x + button_width("Update core")),
          state_label(core_state_label(core_snapshot(session))),
          active_label(std::format("Active version {}",
                                   string_field(core_snapshot(session), "active_version", "unavailable"))),
          notice(core_notice(core_snapshot(session), session)),
          result(state.request_id ? pending_message
                 : state.reply    ? std::string_view(state.reply->message)
                                  : "")
    {
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
        line(heading);
        if (has_core) {
            line(state_label);
            line(active_label);
            line(target_label);
            const int rows = 3 + !stop_inline + !update_inline + !manage_inline;
            height += rows * (frame_height + gap);
        }
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
        const bool pressed = theme::button(label, {std::min(width, button_width(label)), frame_height});
        ImGui::EndDisabled();
        return pressed;
    }
};

void draw_versions(CoreControlsState& state, const Json& core, const ControlsLayout& layout, bool disabled)
{
    wrapped(layout.active_label);
    wrapped(target_label, Color::secondary_text);
    const auto& versions = array_field(core, "available_versions");
    const auto active = string_field(core, "active_version");
    const auto selected = state.target_version.empty() ? active : std::string_view(state.target_version);
    disabled |= !supports(core, "versions") || versions.empty();
    ImGui::BeginDisabled(disabled);
    ImGui::SetNextItemWidth(layout.update_inline ? layout.combo_width : layout.width);
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
    if (layout.update_inline) {
        ImGui::SameLine();
    }
    if (layout.button("Update core",
                      disabled || state.target_version.empty() || state.target_version == active)) {
        open_dialog(state, CoreDialog::update);
    }
}

void render_create(CoreControlsState& state, PreviewSession& session, const Json& core)
{
    const bool pending = state.request_id.has_value();
    const bool unavailable = !session.ready() || !can_edit_configs(core);
    Dialog frame(
        "Create new config###ServerCoreDialog", 380,
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
        wrapped("New preview configs start disabled.", Color::secondary_text);
    } else {
        wrapped("Enter a nonblank name of up to 64 bytes without control characters.", Color::error);
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

void render_configs(CoreControlsState& state, PreviewSession& session, const Json& core)
{
    const bool pending = state.request_id.has_value();
    Dialog frame("Manage configs###ServerCoreDialog", 380, {{"Close", pending}});
    if (!frame) {
        return;
    }
    const bool supported = can_edit_configs(core);
    const auto& configs = array_field(core, "configs");
    if (configs.empty()) {
        wrapped("No preview configs.", Color::secondary_text);
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
            state.dialog = CoreDialog::remove;
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

void render_confirmation(CoreControlsState& state, PreviewSession& session, const Json& core)
{
    const bool remove = state.dialog == CoreDialog::remove;
    const char* title;
    const char* label;
    std::string question;
    std::string_view unavailable;
    Json request;
    if (remove) {
        title = "Remove config###ServerCoreDialog";
        label = "Remove";
        question = std::format("Remove {} from the preview config list?", state.remove_name);
        const auto* config = find_config(core, state.remove_id);
        if (!can_edit_configs(core) || !config) {
            unavailable = "That preview config is unavailable.";
        } else if (bool_field(*config, "active")) {
            unavailable = "The active preview config is protected.";
        }
        request = {{"action", "remove_core_config"}, {"id", state.remove_id}};
    } else if (state.dialog == CoreDialog::update) {
        title = "Update core###ServerCoreDialog";
        label = "Update core";
        question = std::format(
            "Apply {} as the preview core version? The preview changes the reported version in memory.",
            state.target_version);
        if (!supports(core, "versions") || !has_version(core, state.target_version)) {
            unavailable = "That preview version is unavailable.";
        } else if (state.target_version == string_field(core, "active_version")) {
            unavailable = "That preview version is already active.";
        }
        request = {{"action", "update_core"}, {"version", state.target_version}};
    } else {
        title = "Stop core###ServerCoreDialog";
        label = "Stop core";
        question = "Stop the preview core? This changes its simulated lifecycle state.";
        if (!can_stop(core)) {
            unavailable = "Stop is unavailable in the current preview core state.";
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
            state.dialog = CoreDialog::manage;
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

} // namespace

void update_core_controls(CoreControlsState& state, PreviewSession& session)
{
    if (auto reply = session.take_reply(state.request_id)) {
        if (reply->status == PreviewReply::Status::timed_out) {
            reply->message =
                "The preview did not confirm the change. Check the current state before retrying.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = disconnected_message;
        }
        state.reply = std::move(reply);
    }
    const auto& core = core_snapshot(session);
    if (state.dialog != CoreDialog::update && (state.target_version == string_field(core, "active_version") ||
                                               !has_version(core, state.target_version))) {
        state.target_version.clear();
    }
}

float core_controls_height(const CoreControlsState& state, const PreviewSession& session, float width)
{
    return ControlsLayout(state, session, width).height();
}

void render_core_controls(CoreControlsState& state, PreviewSession& session, float width)
{
    const auto& core = core_snapshot(session);
    const ControlsLayout layout(state, session, width);
    ImGui::BeginGroup();
    wrapped(heading);
    if (layout.has_core) {
        wrapped(layout.state_label);
        const bool disabled = !session.ready() || state.request_id.has_value();
        const bool start_disabled =
            disabled || !supports(core, "lifecycle") || field(core, "state") != "stopped";
        if (layout.button("Start", start_disabled)) {
            submit(state, session, {{"action", "start_core"}});
        }
        if (layout.stop_inline) {
            ImGui::SameLine();
        }
        if (layout.button("Stop", disabled || !can_stop(core))) {
            open_dialog(state, CoreDialog::stop);
        }
        draw_versions(state, core, layout, disabled);
        if (layout.button("Create config", disabled || !can_edit_configs(core))) {
            state.config_name.fill('\0');
            open_dialog(state, CoreDialog::create);
        }
        if (layout.manage_inline) {
            ImGui::SameLine();
        }
        if (layout.button("Manage configs", disabled || !field(core, "configs").is_array())) {
            open_dialog(state, CoreDialog::manage);
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

void render_core_control_dialog(CoreControlsState& state, PreviewSession& session)
{
    if (state.open_dialog) {
        ImGui::OpenPopup(popup_id);
        state.open_dialog = false;
    }
    const auto& core = core_snapshot(session);
    switch (state.dialog) {
    case CoreDialog::none:
        break;
    case CoreDialog::create:
        render_create(state, session, core);
        break;
    case CoreDialog::manage:
        render_configs(state, session, core);
        break;
    case CoreDialog::stop:
    case CoreDialog::update:
    case CoreDialog::remove:
        render_confirmation(state, session, core);
        break;
    }
}

std::string describe_core_controls(const CoreControlsState& state, const PreviewSession& session)
{
    const auto& core = core_snapshot(session);
    std::string description = "Core Controls. ";
    if (core.is_object()) {
        const auto active = string_field(core, "active_version", "unavailable");
        const auto target = state.target_version.empty() ? active : std::string_view(state.target_version);
        description +=
            std::format("{} Active version {}. Target version {}. ", core_state_label(core), active, target);
    }
    const auto notice = core_notice(core, session);
    if (!notice.empty()) {
        description += notice + ' ';
    }
    switch (state.dialog) {
    case CoreDialog::none:
        break;
    case CoreDialog::stop:
        description += "Confirm stop. ";
        break;
    case CoreDialog::update:
        description += std::format("Confirm update to {}. ", state.target_version);
        break;
    case CoreDialog::create:
        description += "Create new config. ";
        break;
    case CoreDialog::manage: {
        description += "Manage configs. ";
        const auto& configs = array_field(core, "configs");
        if (configs.empty()) {
            description += "No preview configs. ";
        }
        for (const auto& config : configs) {
            description += std::format("{} {}{}. ", string_field(config, "name"),
                                       bool_field(config, "enabled") ? "enabled" : "disabled",
                                       bool_field(config, "active") ? ", active and protected" : "");
        }
        break;
    }
    case CoreDialog::remove:
        description += std::format("Confirm removal of {}. ", state.remove_name);
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
