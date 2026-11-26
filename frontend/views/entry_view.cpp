#include "entry_view.h"

#include "auth_session.h"
#include "dialog.h"
#include "view_helpers.h"

#include <array>
#include <cstdio>
#include <format>
#include <string_view>

namespace wave {
namespace {

using theme::Color;

constexpr char entry_popup[] = "###Entry dialog";
constexpr std::array setup_titles{"Welcome to Wave", "Preview notes", "Panel details"};
constexpr double entry_timeout_ms = 5000;

const char* title(const EntryViewState& state, bool authenticated = false)
{
    if (state.page == EntryPage::setup) {
        if (authenticated && state.step == 1) {
            return "Project acknowledgments";
        }
        return state.complete ? "Setup complete" : setup_titles[state.step];
    }
    return state.complete ? "Sign-in complete" : authenticated ? "Sign in to Wave" : "Demo sign-in";
}

std::string step_progress(const EntryViewState& state)
{
    return std::format("Step {} of {}", state.step + 1, setup_titles.size());
}

bool blank(std::string_view value)
{
    return value.find_first_not_of(" \r\t\n") == value.npos;
}

void field(const char* label, char* data, std::size_t size, float label_width, ImGuiInputTextFlags flags = 0)
{
    const bool inline_label = label_width > 0;
    const float field_x = ImGui::GetCursorPosX() + label_width + ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushID(label);
    if (inline_label) {
        ImGui::AlignTextToFramePadding();
    }
    ImGui::TextUnformatted(label);
    if (inline_label) {
        ImGui::SameLine(field_x);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##value", data, size, flags);
    ImGui::PopID();
}

void submit(EntryViewState& state, PreviewSession& session)
{
    state.reveal_feedback = true;
    const bool setup = state.page == EntryPage::setup;
    if (setup && blank(state.panel_name)) {
        state.message = "Enter a panel name.";
        return;
    }
    if (blank(state.username) || blank(state.password)) {
        state.message = "Enter a demo username and password.";
        return;
    }
    nlohmann::json request{
        {"action", setup ? "setup" : "sign_in"}, {"username", state.username}, {"password", state.password}};
    if (setup) {
        request["name"] = state.panel_name;
    }
    state.request_id = session.submit(std::move(request), entry_timeout_ms);
    state.message = state.request_id ? (setup ? "Applying demo setup…" : "Checking demo credentials…")
                                     : "The preview connection is unavailable.";
}

void receive_result(EntryViewState& state, PreviewSession& session)
{
    const auto reply = session.take_reply(state.request_id);
    if (!reply) {
        return;
    }
    state.reveal_feedback = true;
    switch (reply->status) {
    case PreviewReply::Status::accepted:
        state.complete = true;
        [[fallthrough]];
    case PreviewReply::Status::rejected:
        state.message = reply->message;
        break;
    case PreviewReply::Status::disconnected:
        state.message = "The preview connection closed. Try again after reconnecting.";
        break;
    case PreviewReply::Status::timed_out:
        state.message = "The preview server did not reply. Try again.";
        break;
    }
}

void leave_entry(EntryViewState& state, PreviewSession& session)
{
    ImGui::CloseCurrentPopup();
    open_entry(state, EntryPage::workspace, session);
}

void render_form(EntryViewState& state)
{
    const bool setup = state.page == EntryPage::setup;
    const char* username = setup ? "Demo administrator" : "Username";
    constexpr char password[] = "Demo password";
    float label_width = std::max(ImGui::CalcTextSize(username).x, ImGui::CalcTextSize(password).x);
    if (setup) {
        label_width = std::max(label_width, ImGui::CalcTextSize("Panel name").x);
    }
    if (ImGui::GetContentRegionAvail().x <
        label_width + ImGui::GetStyle().ItemSpacing.x + 10 * ImGui::GetFontSize()) {
        label_width = 0;
    }

    ImGui::BeginDisabled(state.request_id.has_value());
    if (setup) {
        field("Panel name", state.panel_name, sizeof(state.panel_name), label_width);
    } else {
        ImGui::TextWrapped("Use your saved demo credentials or the default example.");
    }
    field(username, state.username, sizeof(state.username), label_width);
    field(password, state.password, 97, label_width, ImGuiInputTextFlags_Password);
    if (ImGui::Button("Use example")) {
        std::snprintf(state.username, sizeof(state.username), "demo");
        std::snprintf(state.password, sizeof(state.password), "wave-demo");
    }
    ImGui::EndDisabled();
}

void render_feedback(EntryViewState& state, PreviewSession& session)
{
    if (!state.message.empty()) {
        view::wrapped(state.message, state.request_id ? Color::info : Color::error);
    }
    if (session.status() == PreviewStatus::unavailable && ImGui::Button("Reconnect")) {
        session.start();
    }
    if (state.reveal_feedback) {
        ImGui::SetScrollHereY(1);
        state.reveal_feedback = false;
    }
}

} // namespace

void open_entry(EntryViewState& state, EntryPage page, PreviewSession& session)
{
    session.abandon(state.request_id);
    state = {};
    state.page = page;
}

void render_entry(EntryViewState& state, PreviewSession& session, float entry_top)
{
    const bool setup = state.page == EntryPage::setup;
    receive_result(state, session);
    if (!ImGui::IsPopupOpen(entry_popup)) {
        ImGui::OpenPopup(entry_popup);
    }

    std::string caption = title(state);
    if (setup && !state.complete) {
        caption += " · " + step_progress(state);
    }
    caption += entry_popup;

    if (state.complete) {
        Dialog dialog(caption.c_str(), 390, {{"Open workspace"}}, nullptr, entry_top);
        if (dialog) {
            view::wrapped(state.message, Color::success);
            if (dialog.footer() == 0) {
                leave_entry(state, session);
            }
        }
    } else if (setup && state.step == 0) {
        Dialog dialog(caption.c_str(), 590, {{"Back to workspace"}, {"Continue"}}, nullptr, entry_top);
        if (dialog) {
            ImGui::TextWrapped("Set a panel name and demo administrator credentials for this Wave preview.");
            switch (dialog.footer()) {
            case 0:
                leave_entry(state, session);
                break;
            case 1:
                ++state.step;
                break;
            }
        }
    } else if (setup && state.step == 1) {
        Dialog dialog(caption.c_str(), 690, {{"Back to workspace"}, {"Back"}, {"Continue"}}, nullptr,
                      entry_top);
        if (dialog) {
            ImGui::TextWrapped("Wave uses sample data in this preview.");
            ImGui::TextWrapped("Setup changes last for the current Wave backend process.");
            ImGui::TextWrapped("Enter demo credentials for this preview.");
            switch (dialog.footer()) {
            case 0:
                leave_entry(state, session);
                break;
            case 1:
                --state.step;
                break;
            case 2:
                ++state.step;
                break;
            }
        }
    } else if (setup) {
        const bool pending = state.request_id.has_value();
        Dialog dialog(caption.c_str(), 500,
                      {{"Back to workspace"}, {"Back", pending}, {"Finish setup", pending}}, nullptr,
                      entry_top);
        if (dialog) {
            render_form(state);
            render_feedback(state, session);
            switch (dialog.footer()) {
            case 0:
                leave_entry(state, session);
                break;
            case 1:
                --state.step;
                state.message.clear();
                break;
            case 2:
                submit(state, session);
                break;
            }
        }
    } else {
        Dialog dialog(caption.c_str(), 500,
                      {{"Back to workspace"}, {"Sign in", state.request_id.has_value()}}, nullptr, entry_top);
        if (dialog) {
            render_form(state);
            render_feedback(state, session);
            switch (dialog.footer()) {
            case 0:
                leave_entry(state, session);
                break;
            case 1:
                submit(state, session);
                break;
            }
        }
    }
}

void render_authenticated_entry(EntryViewState& state, AuthSession& session, float entry_top)
{
    if (!ImGui::IsPopupOpen(entry_popup)) {
        ImGui::OpenPopup(entry_popup);
    }
    const bool setup = state.page == EntryPage::setup;
    std::string caption = title(state, true);
    if (setup) {
        caption += " · " + step_progress(state);
    }
    caption += entry_popup;
    if (setup && state.step == 0) {
        Dialog dialog(caption.c_str(), 590, {{"Continue"}}, nullptr, entry_top);
        if (dialog) {
            ImGui::TextWrapped("Create the administrator account for this Wave installation.");
            ImGui::TextWrapped("You’ll need the one-time bootstrap token from the server administrator.");
            if (dialog.footer() == 0) {
                ++state.step;
            }
        }
        return;
    }
    if (setup && state.step == 1) {
        Dialog dialog(caption.c_str(), 690, {{"Back"}, {"Continue", !state.acknowledged}}, nullptr,
                      entry_top);
        if (dialog) {
            ImGui::TextWrapped("Wave is a web panel for the WaterWall core. The WaterWall project and its "
                               "contributors maintain the core.");
            ImGui::TextWrapped("Wave uses Dear ImGui, Emscripten, and other open source projects.");
            ImGui::Checkbox("I acknowledge these projects", &state.acknowledged);
            const int action = dialog.footer();
            if (action >= 0) {
                state.step += action == 0 ? -1 : 1;
            }
        }
        return;
    }

    const bool pending = session.pending();
    const std::array actions{DialogAction{"Back", pending},
                             DialogAction{setup ? "Finish setup" : "Sign in", pending}};
    Dialog dialog(caption.c_str(), 500, std::span(actions).subspan(setup ? 0 : 1), nullptr, entry_top);
    if (!dialog) {
        return;
    }
    ImGui::BeginDisabled(pending);
    if (setup) {
        ImGui::TextWrapped("Use up to 64 letters, digits, dots, underscores, or hyphens for the username. "
                           "Choose a password of at least 15 characters.");
    }
    field("Username", state.username, sizeof(state.username), 0);
    field("Password", state.password, sizeof(state.password), 0, ImGuiInputTextFlags_Password);
    if (setup) {
        field("Bootstrap token", state.bootstrap, sizeof(state.bootstrap), 0, ImGuiInputTextFlags_Password);
    }
    ImGui::EndDisabled();
    const std::string_view feedback =
        pending ? (setup ? "Creating the account…" : "Signing in…") : std::string_view(session.error());
    if (!feedback.empty()) {
        view::wrapped(feedback, pending ? Color::info : Color::error);
        if (feedback != state.message) {
            ImGui::SetScrollHereY(1);
        }
    }
    state.message = feedback;
    const int action = dialog.footer();
    if (setup && action == 0) {
        session.clear_error();
        state.message.clear();
        --state.step;
    } else if (action >= 0) {
        nlohmann::json input{{"username", state.username}, {"password", state.password}};
        if (setup) {
            input["bootstrap"] = state.bootstrap;
            input["acknowledged"] = state.acknowledged;
        }
        session.submit(setup ? AuthSession::Action::setup : AuthSession::Action::sign_in, input);
        std::fill(std::begin(state.password), std::end(state.password), '\0');
        std::fill(std::begin(state.bootstrap), std::end(state.bootstrap), '\0');
    }
}

std::string describe_entry(const EntryViewState& state, bool authenticated)
{
    std::string result = title(state, authenticated);
    result += '.';
    if (state.page == EntryPage::setup && !state.complete) {
        result += ' ' + step_progress(state) + '.';
    }
    if (!state.message.empty()) {
        result += ' ' + state.message;
    }
    return result;
}

} // namespace wave
