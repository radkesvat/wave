#include "entry_view.h"

#include "auth_session.h"
#include "dialog.h"
#include "panel_base_path.h"
#include "view_helpers.h"

#include <misc/cpp/imgui_stdlib.h>

#include <string_view>

namespace wave {
namespace {

using theme::Color;

constexpr char entry_popup[] = "###Entry dialog";
constexpr char checking_message[] = "Checking the session…";

const char* title(EntryPage page)
{
    switch (page) {
    case EntryPage::base_path:
        return "Panel base path";
    case EntryPage::setup:
        return "Set up administrator";
    default:
        return "Sign in to Wave";
    }
}

} // namespace

void render_entry(EntryViewState& state, AuthSession& session, float entry_top)
{
    if (state.page == EntryPage::checking) {
        return;
    }

    if (!ImGui::IsPopupOpen(entry_popup)) {
        ImGui::OpenPopup(entry_popup);
    }
    const bool setup = state.page == EntryPage::setup;
    const bool choosing_path = state.page == EntryPage::base_path;
    const std::string caption = std::string(title(state.page)) + entry_popup;
    const bool pending = session.pending();
    const auto ready = [&] {
        return choosing_path ? !state.base_path.empty() : state.username.front() && state.password.front();
    };
    const std::string_view feedback = pending ? std::string_view{} : session.error();
    const bool new_error = !feedback.empty() && feedback != state.message;
    const char* action_label = choosing_path ? "Save base path" : setup ? "Create administrator" : "Sign in";
    Dialog dialog(caption.c_str(), 500, {{action_label, pending || !ready()}},
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
    bool enter = false;
    if (choosing_path) {
        ImGui::TextUnformatted("Base path");
        view::help_marker("Use a path such as /panel or /admin/wave. Segments can contain letters, digits, "
                          "hyphens, and underscores.");
        ImGui::SetNextItemWidth(-1);
        enter = ImGui::InputText("##base path", &state.base_path, enter_submits);
    } else {
        const bool username_enter = view::edit(
            "Username", state.username, "", enter_submits,
            setup ? "Use this username to sign in to Wave after setup. Usernames are case-sensitive."
                  : "Enter the current administrator username. Usernames are case-sensitive.");
        const bool password_enter = view::edit(
            "Password", state.password, "", enter_submits | ImGuiInputTextFlags_Password,
            setup ? "Wave checks for common passwords and predictable patterns. A longer passphrase "
                    "can help you choose a less predictable password."
                  : "Use the password for your Wave administrator account. WaterWall user credentials "
                    "are managed separately.");
        enter = username_enter || password_enter;
    }
    ImGui::EndDisabled();

    if (setup && !feedback.empty()) {
        view::wrapped(feedback, Color::error);
        if (new_error) {
            ImGui::SetScrollHereY(1);
        }
    }
    const int action = dialog.footer();
    if (!pending && ready() && (action == 0 || enter)) {
        if (choosing_path) {
            session.submit(AuthSession::Action::base_path, {{panel_base_path_key, state.base_path}});
        } else {
            session.submit(setup ? AuthSession::Action::setup : AuthSession::Action::sign_in,
                           {{"username", state.username.data()}, {"password", state.password.data()}});
            state.password.fill('\0');
        }
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
