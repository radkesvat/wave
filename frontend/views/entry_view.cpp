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
        view::wrapped(checking_message);
        return;
    }

    if (!ImGui::IsPopupOpen(entry_popup)) {
        ImGui::OpenPopup(entry_popup);
    }
    const bool setup = state.page == EntryPage::setup;
    const std::string caption = std::string(title(state.page)) + entry_popup;
    const bool pending = session.pending();
    const bool ready = state.username.front() && state.password.front();
    Dialog dialog(caption.c_str(), 500, {{setup ? "Create administrator" : "Sign in", pending || !ready}},
                  nullptr, entry_top);
    if (!dialog) {
        return;
    }

    if (setup) {
        view::wrapped("Use up to 64 letters, digits, dots, underscores, or hyphens for the username. "
                      "Choose a password of at least 15 characters.");
    }
    constexpr auto enter_submits = ImGuiInputTextFlags_EnterReturnsTrue;
    ImGui::BeginDisabled(pending);
    const bool username_enter = view::edit("Username", state.username, "", enter_submits);
    const bool password_enter =
        view::edit("Password", state.password, "", enter_submits | ImGuiInputTextFlags_Password);
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
    if (!pending && state.username.front() && state.password.front() &&
        (action == 0 || username_enter || password_enter)) {
        session.submit(setup ? AuthSession::Action::setup : AuthSession::Action::sign_in,
                       {{"username", state.username.data()}, {"password", state.password.data()}});
        state.password.fill('\0');
    }
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
