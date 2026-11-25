#include "settings_view.h"

#include "browser_ui.h"
#include "view_helpers.h"

#include <utility>

namespace wave {

namespace {

using namespace view;

void sync_settings(SettingsViewState& state, const Json& settings)
{
    if (state.dirty || settings == state.source || !settings.is_object()) {
        return;
    }
    fill(state.name, string_field(settings, "name"));
    fill(state.username, string_field(settings, "admin_username"));
    fill(state.domain, string_field(settings, "domain"));
    fill(state.websocket_path, string_field(settings, "websocket_path"));
    fill(state.certificate_path, string_field(settings, "certificate_path"));
    fill(state.private_key_path, string_field(settings, "private_key_path"));
    fill(state.site_template, string_field(settings, "site_template"));
    state.port = integer_field(settings, "port", 2030);
    state.multiplex = bool_field(settings, "multiplex");
    state.source = settings;
}

} // namespace

void render_settings(SettingsViewState& state, PreviewSession& session, bool narrow)
{
    const auto& settings = field(session.data(), "settings");
    if (auto reply = session.take_reply(state.apply_request)) {
        if (reply->ok()) {
            state.dirty = false;
            state.password.fill('\0');
        } else if (reply->status == PreviewReply::Status::timed_out) {
            reply->message = "Preview did not confirm the update. Check the connection and try again.";
        } else if (reply->status == PreviewReply::Status::disconnected) {
            reply->message = "The preview connection closed. Try again after reconnecting.";
        }
        state.apply_reply = std::move(reply);
    }
    sync_settings(state, settings);
    ImGui::BeginDisabled(state.apply_request.has_value());
    const bool columns = !narrow && ImGui::GetContentRegionAvail().x >= 680 &&
                         ImGui::BeginTable("settings-columns", 2, ImGuiTableFlags_SizingStretchSame);
    if (columns) {
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Panel identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        state.dirty |= edit("Panel name", state.name);
        state.dirty |= edit("Demo admin name", state.username);
        state.dirty |= edit("New demo password", state.password, "Keep current demo password",
                            ImGuiInputTextFlags_Password);
    }
    if (columns) {
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Routing preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        state.dirty |= edit("Domain", state.domain, "preview.example");
        state.dirty |= edit_number("Sample port", state.port);
        state.dirty |= edit("Sample WebSocket path", state.websocket_path, "/demo");
        state.dirty |= edit("Landing page template", state.site_template, "Wave welcome");
        state.dirty |= ImGui::Checkbox("Multiplex routes", &state.multiplex);
    }
    if (columns) {
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Example file paths")) {
        state.dirty |= edit("Certificate path", state.certificate_path);
        state.dirty |= edit("Private key path", state.private_key_path);
    }

    const bool valid = state.name[0] && state.username[0] && state.domain[0] && state.site_template[0] &&
                       state.websocket_path[0] == '/' && state.port >= 1 && state.port <= 65535;
    if (state.dirty && !valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, warm);
        ImGui::TextWrapped("Enter a panel name, admin name, domain, template, /path and port 1-65535.");
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!state.dirty || !valid);
    if (ImGui::Button("Apply changes")) {
        ImGui::OpenPopup("Apply preview settings");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Backup preview")) {
        state.backup = Json{{"format", "wave-preview"},
                            {"settings", settings},
                            {"users", array_field(session.data(), "users")}}
                           .dump(2);
        session.send({{"action", "backup_settings"}});
        ImGui::OpenPopup("Sample backup");
    }
    if (state.dirty) {
        if (!narrow) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Revert edits")) {
            state.dirty = false;
            state.source = nullptr;
            state.password.fill('\0');
            state.apply_reply.reset();
            sync_settings(state, settings);
        }
    }
    ImGui::EndDisabled();
    if (state.apply_request) {
        ImGui::TextDisabled("Applying sample panel settings...");
    } else if (state.apply_reply) {
        feedback(*state.apply_reply);
    }

    fit_preview_dialog(430);
    if (ImGui::BeginPopupModal("Apply preview settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Save these values to this Wave process for the preview?");
        if (ImGui::Button("Confirm apply")) {
            Json draft = {{"name", state.name.data()},
                          {"admin_username", state.username.data()},
                          {"domain", state.domain.data()},
                          {"port", state.port},
                          {"websocket_path", state.websocket_path.data()},
                          {"site_template", state.site_template.data()},
                          {"certificate_path", state.certificate_path.data()},
                          {"private_key_path", state.private_key_path.data()},
                          {"multiplex", state.multiplex}};
            if (state.password[0]) {
                draft["password"] = state.password.data();
            }
            state.apply_request =
                session.submit({{"action", "apply_settings"}, {"settings", std::move(draft)}}, 5000);
            if (state.apply_request) {
                state.apply_reply.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    fit_preview_dialog(540);
    if (ImGui::BeginPopupModal("Sample backup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Applied preview settings and sample user records");
        ImGui::InputTextMultiline("##sample-backup", state.backup.data(), state.backup.size() + 1, {-1, 170},
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy backup")) {
            ImGui::SetClipboardText(state.backup.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        feedback(session, {"backup_settings"});
        ImGui::EndPopup();
    }
}

} // namespace wave
