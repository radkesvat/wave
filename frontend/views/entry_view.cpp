#include "entry_view.h"

#include "auth_session.h"
#include "dialog.h"
#include "view_helpers.h"

#include <string_view>

namespace wave {
namespace {

using theme::Color;

constexpr char entry_popup[] = "###Entry dialog";
constexpr char checking_message[] = "Checking the session…";

const char* title(EntryPage page)
{
    return page == EntryPage::setup ? "Set up administrator" : "Sign in to Wave";
}

} // namespace

void render_entry(EntryViewState& state, AuthSession& session, float entry_top)
{
    if (state.page == EntryPage::checking) {
        ImGui::SetCursorPos({12, entry_top + 12});
        draw_busy_spinner(ImGui::GetFrameHeight());
        return;
    }

    if (!ImGui::IsPopupOpen(entry_popup)) {
        ImGui::OpenPopup(entry_popup);
    }
    const bool setup = state.page == EntryPage::setup;
    const std::string caption = std::string(title(state.page)) + entry_popup;
    const bool pending = session.pending();
    const bool ready = state.username.front() && state.password.front();
    const std::string_view feedback = pending ? std::string_view{} : session.error();
    const bool new_error = !feedback.empty() && feedback != state.message;
    Dialog dialog(caption.c_str(), 500, {{setup ? "Create administrator" : "Sign in", pending || !ready}},
                  {.entry_top = entry_top,
                   .busy = pending,
                   .footer_error = setup ? std::string_view{} : feedback,
                   .reveal_footer_error = !setup && new_error});
    if (!dialog) {
        return;
    }

    if (setup) {
        view::wrapped("Use up to 64 letters, digits, dots, underscores, or hyphens for the username.");
        view::wrapped(new_password_requirements);
    }
    constexpr auto enter_submits = ImGuiInputTextFlags_EnterReturnsTrue;
    ImGui::BeginDisabled(pending);
    const bool username_enter =
        view::edit("Username", state.username, "", enter_submits,
                   setup ? "Use this username to sign in to Wave after setup. Usernames are case-sensitive."
                         : "Enter the current administrator username. Usernames are case-sensitive.");
    const bool password_enter =
        view::edit("Password", state.password, "", enter_submits | ImGuiInputTextFlags_Password,
                   setup ? "Wave checks for common passwords and predictable patterns. A longer passphrase "
                           "can help you choose a less predictable password."
                         : "Use the password for your Wave administrator account. WaterWall user credentials "
                           "are managed separately.");
    ImGui::EndDisabled();

    if (setup && !feedback.empty()) {
        view::wrapped(feedback, Color::error);
        if (new_error) {
            ImGui::SetScrollHereY(1);
        }
    }
    const int action = dialog.footer();
    if (!pending && state.username.front() && state.password.front() &&
        (action == 0 || username_enter || password_enter)) {
        session.submit(setup ? AuthSession::Action::setup : AuthSession::Action::sign_in,
                       {{"username", state.username.data()}, {"password", state.password.data()}});
        state.password.fill('\0');
    }
    state.message = session.pending() ? busy_description : feedback;
}

std::string describe_entry(const EntryViewState& state)
{
    if (state.page == EntryPage::checking) {
        return checking_message;
    }
    std::string result = title(state.page);
    result += '.';
    if (!state.message.empty()) {
        result += ' ' + state.message;
    }
    return result;
}

} // namespace wave
