#include "settings_view.h"

#include "dialog.h"
#include "view_helpers.h"

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
        const bool saved = reply->ok();
        if (saved) {
            state.dirty = false;
            state.source = nullptr;
        } else if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "Preview did not confirm the update. Check the connection and try again.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Try again after reconnecting.";
        }
        state.apply_reply = std::move(reply);
        return saved;
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
    if (state.apply_request) {
        state.apply_reply.reset();
    } else {
        state.apply_reply =
            PreviewReply{PreviewReply::Status::disconnected, "The preview connection is unavailable."};
    }
}

void show_settings_save(const SettingsViewState& state)
{
    if (state.apply_request) {
        wrapped("Applying sample TLS settings…", Color::info);
    } else if (state.apply_reply) {
        feedback(*state.apply_reply);
    }
}

} // namespace

void render_settings(SettingsViewState& state, PreviewSession& session, bool narrow)
{
    const auto& settings = field(session.data(), "settings");
    sync_settings(state, settings);
    ImGui::BeginDisabled(state.apply_request.has_value());
    state.dirty |= edit("Domain", state.domain, "panel.example.test");
    state.dirty |= edit("Certificate chain path", state.certificate_path);
    state.dirty |= edit("Private key path", state.private_key_path);

    const bool valid = state.domain[0] != '\0';
    if (state.dirty && !valid) {
        wrapped("Enter a domain.", Color::error);
    }
    ImGui::BeginDisabled(!state.dirty || !valid);
    if (theme::button("Apply changes")) {
        state.apply_reply.reset();
        ImGui::OpenPopup("Apply preview TLS settings");
    }
    ImGui::EndDisabled();
    if (state.dirty) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (theme::button("Revert edits")) {
            state.dirty = false;
            state.source = nullptr;
            state.apply_reply.reset();
            sync_settings(state, settings);
        }
    }
    ImGui::EndDisabled();
    if (!ImGui::IsPopupOpen("Apply preview TLS settings")) {
        show_settings_save(state);
    }
    render_settings_dialogs(state, session);
}

void render_settings_dialogs(SettingsViewState& state, PreviewSession& session)
{
    const bool applied = finish_settings_save(state, session);
    const bool pending = state.apply_request.has_value();
    if (Dialog frame{
            "Apply preview TLS settings", 370, {{pending ? "Close" : "Cancel"}, {"Confirm apply", pending}}};
        frame) {
        ImGui::TextWrapped("Save these TLS settings for the current preview process?");
        show_settings_save(state);
        const int action = frame.footer();
        if (action == 0 || applied) {
            ImGui::CloseCurrentPopup();
        } else if (action == 1) {
            submit_settings(state, session);
        }
    }
}

} // namespace wave
