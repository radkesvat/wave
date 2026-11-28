#include "settings_view.h"

#include "auth_session.h"
#include "dialog.h"
#include "panel_base_path.h"
#include "session_settings.h"
#include "view_helpers.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace wave {

namespace {

using namespace view;
using theme::Color;

constexpr char tls_dialog_title[] = "Apply TLS settings";
constexpr char tls_restart_notice[] = "Restart Wave to apply the saved TLS settings.";
constexpr char tls_unavailable[] = "TLS settings are unavailable on this backend.";

void sync_tls_settings(TlsSettingsDraft& draft, const Json& settings)
{
    draft.restart_required = bool_field(settings, "restart_required");
    if (draft.dirty || draft.request || settings == draft.source || !settings.is_object()) {
        return;
    }
    draft.enabled = bool_field(settings, "enabled");
    draft.domain = string_field(settings, "domain");
    draft.certificate_chain = string_field(settings, "certificate_chain");
    draft.private_key = string_field(settings, "private_key");
    draft.source = settings;
}

bool tls_settings_changed(const TlsSettingsDraft& draft)
{
    return draft.enabled != bool_field(draft.source, "enabled") ||
           (draft.enabled && (draft.domain != string_field(draft.source, "domain") ||
                              draft.certificate_chain != string_field(draft.source, "certificate_chain") ||
                              draft.private_key != string_field(draft.source, "private_key")));
}

bool edit_tls_field(const char* label, std::string& value, const char* hint, std::string_view help)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    help_marker(help);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputTextWithHint("##value", hint, &value);
    ImGui::PopID();
    return changed;
}

void submit_tls_settings(TlsSettingsDraft& draft, PreviewSession& session)
{
    Json settings = {{"enabled", draft.enabled}};
    if (draft.enabled) {
        settings["domain"] = draft.domain;
        settings["certificate_chain"] = draft.certificate_chain;
        settings["private_key"] = draft.private_key;
    }
    draft.request =
        session.submit({{"action", "apply_tls_settings"}, {"settings", std::move(settings)}}, 5000);
}

void render_tls_settings(TlsSettingsDraft& draft, const Json& settings, bool narrow)
{
    if (!bool_field(settings, "supported")) {
        wrapped(tls_unavailable, Color::warning);
        return;
    }
    ImGui::BeginDisabled(draft.request.has_value());
    if (toggle_switch("Enable TLS", {48, std::max(26.0f, ImGui::GetFrameHeight())}, {42, 22},
                      draft.enabled)) {
        draft.enabled = !draft.enabled;
        draft.dirty = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Enable TLS");
    help_marker("Wave switches between HTTPS and HTTP when the backend restarts.");

    ImGui::BeginDisabled(!draft.enabled);
    draft.dirty |= edit_tls_field("Domain", draft.domain, "panel.example.test",
                                  "Enter the lowercase DNS hostname that the certificate covers.");
    draft.dirty |= edit_tls_field("Certificate chain path", draft.certificate_chain, "",
                                  "Enter an absolute PEM path on this server. Put the server certificate "
                                  "first, followed by its intermediates.");
    draft.dirty |= edit_tls_field("Private key path", draft.private_key, "",
                                  "Enter the absolute path to the matching unencrypted PEM private key "
                                  "on this server.");
    ImGui::EndDisabled();

    const bool complete = !draft.enabled || (!draft.domain.empty() && !draft.certificate_chain.empty() &&
                                             !draft.private_key.empty());
    ImGui::BeginDisabled(!tls_settings_changed(draft) || !complete);
    if (theme::button("Apply changes")) {
        ImGui::OpenPopup(tls_dialog_title);
    }
    ImGui::EndDisabled();
    if (draft.request && !ImGui::IsPopupOpen(tls_dialog_title)) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (draft.dirty) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (theme::button("Revert edits")) {
            draft.dirty = false;
            draft.source = nullptr;
            sync_tls_settings(draft, settings);
        }
    }
    ImGui::EndDisabled();
    if (draft.restart_required) {
        wrapped(tls_restart_notice, Color::secondary_text);
    }
}

void render_tls_dialog(TlsSettingsDraft& draft, PreviewSession& session)
{
    const auto reply = session.take_reply(draft.request);
    if (reply && reply->ok()) {
        draft.dirty = false;
        draft.source = nullptr;
    }
    const bool pending = draft.request.has_value();
    if (Dialog frame{
            tls_dialog_title,
            370,
            {{pending ? "Close" : "Cancel", pending}, {"Confirm apply", pending || !session.ready()}},
            {.busy = pending}};
        frame) {
        draft.confirming = true;
        if (draft.enabled) {
            wrapped("Restart Wave after saving, then reconnect over HTTPS using the configured hostname.");
        } else {
            wrapped("Restart Wave after saving, then reconnect over HTTP. Credentials and sessions will "
                    "travel without TLS.");
        }
        if (!session.ready() && !pending) {
            wrapped("The settings connection is unavailable.", Color::warning);
        }
        const int action = frame.footer();
        if (action == 0 || reply) {
            ImGui::CloseCurrentPopup();
            draft.confirming = false;
        } else if (action == 1) {
            submit_tls_settings(draft, session);
            if (!draft.request) {
                ImGui::CloseCurrentPopup();
                draft.confirming = false;
            }
        }
    }
}

void render_administrator_settings(SettingsViewState& state, AuthSession& auth)
{
    if (state.administrator != auth.username()) {
        state.administrator = auth.username();
        fill(state.admin_username, state.administrator);
        state.current_password.fill('\0');
        state.new_password.fill('\0');
    }

    ImGui::BeginDisabled(auth.pending());
    edit("Username", state.admin_username, "", 0,
         "Use up to 64 ASCII letters, digits, dots, underscores, or hyphens. Sign in with the updated "
         "username after saving.");
    edit("Current password", state.current_password, "", ImGuiInputTextFlags_Password,
         "Enter the password you currently use to sign in, even when changing only the username.");
    edit("New password", state.new_password, "Optional", ImGuiInputTextFlags_Password,
         "Leave this blank to keep your current password. Wave checks new passwords for common words and "
         "predictable patterns.");
    wrapped(new_password_requirements, Color::secondary_text);

    const bool changed = state.admin_username.data() != state.administrator || state.new_password.front();
    ImGui::BeginDisabled(!state.admin_username.front() || !state.current_password.front() || !changed);
    if (theme::button("Save administrator")) {
        auth.submit(AuthSession::Action::update_administrator,
                    {{"username", state.admin_username.data()},
                     {"current_password", state.current_password.data()},
                     {"password", state.new_password.data()}});
        state.current_password.fill('\0');
        state.new_password.fill('\0');
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (auth.pending() && auth.action() == AuthSession::Action::update_administrator) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    wrapped("Saving changes signs you out.", Color::secondary_text);
    if (auth.action() == AuthSession::Action::update_administrator && !auth.error().empty()) {
        wrapped(auth.error(), Color::error);
    }
}

std::optional<int> entered_expiry(const SessionSettingsDraft& draft)
{
    const auto& input = draft.expiry_days;
    const auto end = input.data() + input.size();
    int days = 0;
    const auto parsed = std::from_chars(input.data(), end, days);
    if (parsed.ec != std::errc{} || parsed.ptr != end || days < 1 || days > max_session_expiry_days) {
        return std::nullopt;
    }
    return days;
}

std::string expiry_error()
{
    return std::format("Enter a whole number from 1 to {} for session expiry.", max_session_expiry_days);
}

void update_session_settings(SessionSettingsDraft& draft, PreviewSession& session)
{
    bool accepted = false;
    if (auto reply = session.take_reply(draft.request)) {
        accepted = reply->ok();
    }
    if (!accepted && (draft.request || !session.ready() ||
                      (draft.saved_days && entered_expiry(draft) != draft.saved_days))) {
        return;
    }
    const auto& stored = field(field(session.data(), "settings"), "session_expiry_days");
    const int days = stored.is_number_integer() && stored >= 1 && stored <= max_session_expiry_days
                         ? stored.get<int>()
                         : default_session_expiry_days;
    if (accepted || draft.saved_days != days) {
        draft.saved_days = days;
        draft.expiry_days = std::to_string(days);
    }
}

void render_session_settings(SessionSettingsDraft& draft, PreviewSession& session)
{
    ImGui::BeginDisabled(!session.ready() || draft.request.has_value());
    ImGui::PushID("Session expiry (days)");
    ImGui::TextUnformatted("Session expiry (days)");
    static const auto help =
        std::format("Enter a whole number from 1 to {}. The default is {} days. Sessions that are already "
                    "active keep their original expiry.",
                    max_session_expiry_days, default_session_expiry_days);
    help_marker(help);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##value", &draft.expiry_days);
    ImGui::PopID();
    const auto days = entered_expiry(draft);
    ImGui::BeginDisabled(!days || days == draft.saved_days);
    if (theme::button("Save session settings")) {
        draft.request =
            session.submit({{"action", "apply_session_settings"}, {"session_expiry_days", *days}}, 5000);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (draft.request) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    wrapped("Applies to new sign-ins.", Color::secondary_text);
    if (!days) {
        wrapped(expiry_error(), Color::error);
    }
}

void show_settings_connection(PreviewSession& session)
{
    if (session.status() == PreviewStatus::unavailable) {
        wrapped("Settings connection unavailable.", Color::warning);
        if (theme::button("Reconnect")) {
            session.start(true);
        }
    }
}

void render_base_path_settings(SettingsViewState& state, AuthSession& auth)
{
    if (!state.base_path) {
        state.base_path = auth.base_path();
    }
    auto& path = *state.base_path;
    ImGui::BeginDisabled(auth.pending());
    ImGui::TextUnformatted("Base path");
    help_marker("Start with a slash. Use letters, digits, hyphens, or underscores in each segment. "
                "Leave out the trailing slash.");
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputText("##base path", &path, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited() && auth.action() == AuthSession::Action::base_path) {
        auth.clear_error();
    }
    const bool valid = valid_panel_base_path(path);
    const bool can_save = !auth.pending() && valid && path != auth.base_path();
    ImGui::BeginDisabled(!can_save);
    const bool save = theme::button("Save base path");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (can_save && (save || enter)) {
        auth.submit(AuthSession::Action::base_path, {{panel_base_path_key, path}});
    }
    if (auth.pending() && auth.action() == AuthSession::Action::base_path) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    wrapped("Connected Wave tabs will open the new address. Save any open edits first.",
            Color::secondary_text);
    if (!valid) {
        wrapped(base_path_format_error, Color::error);
    } else if (auth.action() == AuthSession::Action::base_path && !auth.error().empty()) {
        wrapped(auth.error(), Color::error);
    }
}

void render_panel_settings(SettingsViewState& state, AuthSession& auth, PreviewSession& session, bool narrow)
{
    constexpr float min_column_width = 300;
    constexpr float max_card_width = 348;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::clamp(static_cast<int>((available + gap) / (min_column_width + gap)), 1, 3);
    const float width = std::min(max_card_width, (available - (columns - 1) * gap) / columns);
    int card_index = 0;
    const auto begin_card = [&](const char* title) {
        if (card_index++ % columns != 0) {
            ImGui::SameLine();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {9, 8});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::control_radius);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgba(Color::pane));
        ImGui::PushStyleColor(ImGuiCol_Border, theme::rgba(Color::divider));
        // Recompute card heights and keep processing the TLS dialog while its card is scrolled out of view.
        const bool visible =
            ImGui::BeginChild(title, {width, 0},
                              ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened |
                                  ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        if (visible) {
            ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, {0, 3});
            ImGui::SeparatorText(title);
            ImGui::PopStyleVar();
        }
        return visible;
    };

    if (session.status() != PreviewStatus::loading || state.tls.request) {
        if (begin_card("TLS")) {
            if (session.ready()) {
                render_tls_settings(state.tls, field(field(session.data(), "settings"), "tls"), narrow);
            } else {
                show_settings_connection(session);
            }
            render_tls_dialog(state.tls, session);
        }
        ImGui::EndChild();
    }
    if (begin_card("Administrator")) {
        render_administrator_settings(state, auth);
    }
    ImGui::EndChild();
    if (state.sessions.saved_days) {
        if (begin_card("Sessions")) {
            render_session_settings(state.sessions, session);
        }
        ImGui::EndChild();
    }
    if (!auth.base_path().empty()) {
        if (begin_card("Panel base path")) {
            render_base_path_settings(state, auth);
        }
        ImGui::EndChild();
    }
}

} // namespace

void render_settings(SettingsViewState& state, AuthSession& auth, PreviewSession& session, bool narrow)
{
    update_core_settings(state.core, session);
    update_session_settings(state.sessions, session);
    sync_tls_settings(state.tls, field(field(session.data(), "settings"), "tls"));
    state.tls.confirming = false;
    if (!ImGui::BeginTabBar("Settings tabs")) {
        return;
    }
    using Tab = SettingsViewState::Tab;
    for (const auto tab : {Tab::panel, Tab::core}) {
        if (!ImGui::BeginTabItem(tab == Tab::panel ? "Panel" : "Core")) {
            continue;
        }
        state.tab = tab;
        // Attach the top edge of the content frame to the tab bar.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
        const bool visible =
            ImGui::BeginChild("Settings content", {}, ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleVar();
        if (visible) {
            if (ImGui::GetScrollMaxY() > 0) {
                const auto position = ImGui::GetWindowPos();
                const auto size = ImGui::GetWindowSize();
                state.scroll_bounds = {position.x, position.y, size.x, size.y};
            }
            if (tab == Tab::panel) {
                render_panel_settings(state, auth, session, narrow);
            } else {
                render_core_settings(state.core, session);
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

std::string describe_settings(const SettingsViewState& state, const AuthSession& auth)
{
    if (state.tab == SettingsViewState::Tab::core) {
        return "Core tab. " + describe_core_settings(state.core);
    }
    std::string description = "Panel tab. Administrator settings.";
    const auto& tls = state.tls;
    if (bool_field(tls.source, "supported")) {
        description += tls.enabled ? " TLS settings. Enable TLS on."
                                   : " TLS settings. Enable TLS off. Certificate fields locked.";
        if (tls.restart_required) {
            description += std::string(" ") + tls_restart_notice;
        }
    }
    if (tls.confirming) {
        description += std::string(" ") + tls_dialog_title + '.';
    }
    if (auth.action() == AuthSession::Action::update_administrator) {
        if (!auth.error().empty()) {
            description += ' ' + auth.error();
        }
    }
    const auto& sessions = state.sessions;
    if (sessions.saved_days) {
        description += std::format(" Session settings. Session expiry {} {}.", *sessions.saved_days,
                                   *sessions.saved_days == 1 ? "day" : "days");
        if (!entered_expiry(sessions)) {
            description += ' ' + expiry_error();
        }
    }
    if (!auth.base_path().empty()) {
        description += std::format(" Panel base path. Current path {}.", auth.base_path());
        if (state.base_path && !valid_panel_base_path(*state.base_path)) {
            description += std::string(" ") + base_path_format_error;
        } else if (auth.action() == AuthSession::Action::base_path && !auth.error().empty()) {
            description += ' ' + auth.error();
        }
    }
    if (tls.request || sessions.request ||
        (auth.pending() && (auth.action() == AuthSession::Action::update_administrator ||
                            auth.action() == AuthSession::Action::base_path))) {
        description += std::string(" ") + busy_description;
    }
    return description;
}

} // namespace wave
