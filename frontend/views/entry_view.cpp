#include "entry_view.h"

#include "preview_session.h"

#include <imgui.h>

#include <cstdio>
#include <string_view>

namespace wave {
namespace {

bool blank(std::string_view value)
{
    return value.find_first_not_of(" \r\t\n") == value.npos;
}

void field(const char* label, char* data, std::size_t size, ImGuiInputTextFlags flags = 0)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##value", data, size, flags);
    ImGui::PopID();
}

void submit(EntryViewState& state, PreviewSession& session)
{
    if (blank(state.username) || blank(state.password)) {
        state.message = "Enter a demo username and password.";
        return;
    }
    const bool setup = state.page == EntryPage::setup;
    nlohmann::json request{{"action", setup ? "setup" : "sign_in"},
                           {"username", state.username}, {"password", state.password}};
    if (setup) {
        request["name"] = state.panel_name;
        request["domain"] = state.domain;
    }
    state.request_id = session.submit(std::move(request));
    state.message = state.request_id ? (setup ? "Applying demo setup…" : "Checking demo credentials…")
                                     : "The preview connection is unavailable.";
}

void receive_result(EntryViewState& state, PreviewSession& session)
{
    const auto reply = session.take_reply(state.request_id);
    if (!reply) {
        return;
    }
    if (reply->status == PreviewReply::Status::disconnected) {
        state.message = "The preview connection closed. Try again after reconnecting.";
    } else {
        state.complete = reply->ok();
        state.message = reply->message;
    }
}

void credentials(EntryViewState& state, bool setup)
{
    field(setup ? "Demo administrator" : "Username", state.username, sizeof(state.username));
    field("Demo password", state.password, sizeof(state.password), ImGuiInputTextFlags_Password);
    if (ImGui::Button("Use example")) {
        std::snprintf(state.username, sizeof(state.username), "demo");
        std::snprintf(state.password, sizeof(state.password), "wave-demo");
    }
}

void render_setup(EntryViewState& state, PreviewSession& session)
{
    ImGui::TextDisabled("Step %d of 3", state.step + 1);
    ImGui::ProgressBar((state.step + 1.0f) / 3.0f, ImVec2(-1, 4), "");
    ImGui::Spacing();
    switch (state.step) {
    case 0:
        ImGui::TextUnformatted("Welcome to Wave");
        ImGui::TextWrapped("Walk through first-run setup with sample values. The preview stores changes "
                           "in memory for this Wave process.");
        if (ImGui::Button("Continue")) {
            ++state.step;
            state.message.clear();
        }
        break;
    case 1: {
        field("Panel name", state.panel_name, sizeof(state.panel_name));
        field("Example host", state.domain, sizeof(state.domain));
        if (ImGui::Button("Back")) {
            --state.step;
            state.message.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Continue")) {
            const std::string_view domain = state.domain;
            if (blank(state.panel_name) || domain.find('.') == domain.npos ||
                domain.find_first_of(" /:\t\n") != domain.npos) {
                state.message = "Enter a panel name and a host such as wave.example.";
            } else {
                ++state.step;
                state.message.clear();
            }
        }
        break;
    }
    case 2:
        credentials(state, true);
        if (ImGui::Button("Back")) {
            --state.step;
            state.message.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Finish setup")) {
            submit(state, session);
        }
        break;
    }
}

} // namespace

void open_entry(EntryViewState& state, EntryPage page, PreviewSession& session)
{
    session.abandon(state.request_id);
    state = {};
    state.page = page;
}

void render_entry(EntryViewState& state, PreviewSession& session)
{
    receive_result(state, session);
    ImGui::PushFont(nullptr, 28.0f);
    ImGui::TextUnformatted(state.page == EntryPage::setup ? "First-run setup" : "Demo sign-in");
    ImGui::PopFont();
    ImGui::Separator();

    if (state.complete) {
        ImGui::TextUnformatted(state.page == EntryPage::setup ? "Setup complete" : "Sign-in complete");
        ImGui::TextWrapped("%s", state.message.c_str());
        if (ImGui::Button("Open workspace")) {
            open_entry(state, EntryPage::workspace, session);
        }
        return;
    }

    ImGui::BeginDisabled(state.request_id.has_value());
    if (state.page == EntryPage::setup) {
        render_setup(state, session);
    } else {
        ImGui::TextWrapped("Default example: demo / wave-demo. Use your saved demo credentials after a change.");
        credentials(state, false);
        if (ImGui::Button("Sign in")) {
            submit(state, session);
        }
    }
    ImGui::EndDisabled();

    if (!state.message.empty()) {
        ImGui::TextWrapped("%s", state.message.c_str());
    }
    if (session.status() == PreviewStatus::unavailable && ImGui::Button("Reconnect")) {
        session.start();
    }
    ImGui::Spacing();
    if (ImGui::Button("Back to workspace")) {
        open_entry(state, EntryPage::workspace, session);
    }
}

} // namespace wave
