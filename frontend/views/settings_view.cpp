#include "settings_view.h"

#include "auth_session.h"
#include "dialog.h"
#include "session_settings.h"
#include "view_helpers.h"

#include <misc/cpp/imgui_stdlib.h>

#include <charconv>
#include <format>
#include <utility>

namespace wave {

namespace {

using namespace view;
using theme::Color;

void sync_settings(SettingsViewState& state, const Json& settings)
{
    if (state.dirty || settings == state.source || !settings.is_object()) {
        return;
    }
    fill(state.domain, string_field(settings, "domain"));
    fill(state.certificate_path, string_field(settings, "certificate_path"));
    fill(state.private_key_path, string_field(settings, "private_key_path"));
    state.source = settings;
}

bool finish_settings_save(SettingsViewState& state, PreviewSession& session)
{
    if (auto reply = session.take_reply(state.apply_request)) {
        if (reply->ok()) {
            state.dirty = false;
            state.source = nullptr;
        }
        return true;
    }
    return false;
}

void submit_settings(SettingsViewState& state, PreviewSession& session)
{
    Json draft = {{"domain", state.domain.data()},
                  {"certificate_path", state.certificate_path.data()},
                  {"private_key_path", state.private_key_path.data()}};
    state.apply_request =
        session.submit({{"action", "apply_settings"}, {"settings", std::move(draft)}}, 5000);
}

void render_tls_settings(SettingsViewState& state, PreviewSession& session, bool narrow)
{
    const auto& settings = field(session.data(), "settings");
    sync_settings(state, settings);
    ImGui::BeginDisabled(state.apply_request.has_value());
    state.dirty |=
        edit("Domain", state.domain, "panel.example.test", 0,
             "Use a hostname such as panel.example.test. These sample values apply to the preview. "
             "Set the live HTTPS hostname in wave-db.json.");
    state.dirty |= edit("Certificate chain path", state.certificate_path, "", 0,
                        "Sample path to a PEM file. The chain starts with the server certificate and then "
                        "lists its intermediate certificates. Set the live path in wave-db.json.");
    state.dirty |= edit("Private key path", state.private_key_path, "", 0,
                        "Sample path to the PEM private key that matches the server certificate. "
                        "Configure an unencrypted key for live HTTPS in wave-db.json.");

    const bool valid = state.domain[0] != '\0';
    if (state.dirty && !valid) {
        wrapped("Enter a domain.", Color::error);
    }
    ImGui::BeginDisabled(!state.dirty || !valid);
    if (theme::button("Apply changes")) {
        ImGui::OpenPopup("Apply preview TLS settings");
    }
    ImGui::EndDisabled();
    if (state.apply_request && !ImGui::IsPopupOpen("Apply preview TLS settings")) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (state.dirty) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (theme::button("Revert edits")) {
            state.dirty = false;
            state.source = nullptr;
            sync_settings(state, settings);
        }
    }
    ImGui::EndDisabled();
}

void render_tls_dialog(SettingsViewState& state, PreviewSession& session)
{
    const bool finished = finish_settings_save(state, session);
    const bool pending = state.apply_request.has_value();
    if (Dialog frame{"Apply preview TLS settings",
                     370,
                     {{pending ? "Close" : "Cancel"}, {"Confirm apply", pending || !session.ready()}},
                     {.busy = pending}};
        frame) {
        ImGui::TextWrapped("Save these TLS settings for the current preview process?");
        if (!session.ready() && !pending) {
            wrapped("The preview connection is unavailable.", Color::warning);
        }
        const int action = frame.footer();
        if (action == 0 || finished) {
            ImGui::CloseCurrentPopup();
        } else if (action == 1) {
            submit_settings(state, session);
            if (!state.apply_request) {
                ImGui::CloseCurrentPopup();
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

    ImGui::Spacing();
    ImGui::SeparatorText("Administrator");
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
    if (!draft.saved_days) {
        return;
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Sessions");
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
    if (session.status() == PreviewStatus::loading) {
        draw_busy_spinner(ImGui::GetFrameHeight());
    } else {
        wrapped("Settings connection unavailable.", Color::warning);
        if (theme::button("Reconnect")) {
            session.start(true);
        }
    }
}

} // namespace

void render_settings(SettingsViewState& state, AuthSession& auth, PreviewSession& session, bool narrow)
{
    update_core_settings(state.core, session);
    update_session_settings(state.sessions, session);
    if (!ImGui::BeginTabBar("Settings tabs")) {
        return;
    }
    if (ImGui::BeginTabItem("Panel")) {
        state.tab = SettingsViewState::Tab::panel;
        if (session.ready()) {
            render_tls_settings(state, session, narrow);
        } else {
            show_settings_connection(session);
        }
        render_tls_dialog(state, session);
        render_administrator_settings(state, auth);
        render_session_settings(state.sessions, session);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Core")) {
        state.tab = SettingsViewState::Tab::core;
        render_core_settings(state.core, session);
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
    if (state.apply_request || sessions.request ||
        (auth.pending() && auth.action() == AuthSession::Action::update_administrator)) {
        description += std::string(" ") + busy_description;
    }
    return description;
}

} // namespace wave
