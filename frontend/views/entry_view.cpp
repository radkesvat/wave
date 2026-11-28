#include "entry_view.h"

#include "auth_session.h"
#include "browser_ui.h"
#include "dialog.h"
#include "panel_base_path.h"
#include "view_helpers.h"

#include <emscripten.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace wave {
namespace {

using theme::Color;

constexpr char entry_popup[] = "###Entry dialog";
constexpr char checking_message[] = "Checking the session…";

EM_JS(bool, prefers_reduced_motion, (), {
    if (!Module['entryMotionPreference']) {
        Module['entryMotionPreference'] = matchMedia('(prefers-reduced-motion: reduce)');
    }
    return Module['entryMotionPreference'].matches;
});

struct WaveBand {
    float center;
    float amplitude;
    float frequency;
    float slope;
    float phase;
    float speed;
    ImU32 left_color;
    ImU32 right_color;
};

void draw_sign_in_background(float top)
{
    const auto viewport = ImGui::GetIO().DisplaySize;
    const float height = viewport.y - top;
    if (viewport.x < 1 || height < 1) {
        return;
    }

    constexpr std::array bands{
        WaveBand{0.35f, 0.115f, 0.91f, 0.16f, 0.55f, 0.29f, IM_COL32(105, 83, 220, 255),
                 IM_COL32(176, 128, 232, 255)},
        WaveBand{0.56f, 0.16f, 1.02f, -0.19f, 2.30f, -0.23f, IM_COL32(86, 133, 248, 255),
                 IM_COL32(86, 216, 224, 255)},
        WaveBand{0.73f, 0.10f, 0.83f, -0.09f, 4.10f, 0.19f, IM_COL32(107, 80, 192, 255),
                 IM_COL32(238, 140, 126, 255)},
    };
    const float time = prefers_reduced_motion() ? 0.0f : static_cast<float>(ImGui::GetTime());
    const float scale = std::clamp(viewport.x / 700.0f, 0.55f, 1.0f);
    const float spacing = std::clamp(height * 0.023f, 6.0f, 18.0f);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({0, top}, viewport, true);
    draw->AddRectFilledMultiColor({0, top}, viewport, IM_COL32(33, 20, 63, 32), IM_COL32(21, 34, 69, 44),
                                  IM_COL32(44, 28, 58, 72), IM_COL32(18, 32, 66, 76));

    std::array<ImVec2, 113> points;
    for (const auto& band : bands) {
        const float phase = band.phase + time * band.speed;
        const auto stroke = [&](float width, int opacity) {
            const int first = draw->VtxBuffer.Size;
            draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                              IM_COL32(255, 255, 255, opacity), width);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, {0, top},
                                                          {viewport.x, top}, band.left_color,
                                                          band.right_color);
        };
        for (int strand = -2; strand <= 2; ++strand) {
            for (std::size_t i = 0; i < points.size(); ++i) {
                const float x = static_cast<float>(i) / (points.size() - 1);
                const float angle = x * 2 * IM_PI * band.frequency;
                const float wave = std::sin(angle + phase + strand * 0.12f) +
                                   0.22f * std::sin(angle * 1.65f - phase * 0.66f + strand * 0.15f);
                const float y = band.center + band.slope * (x - 0.5f) + band.amplitude * scale * wave;
                points[i] = {x * viewport.x, top + height * y + spacing * strand};
            }
            if (strand == 0) {
                stroke(24.0f * scale, 7);
                stroke(9.0f * scale, 15);
                stroke(1.8f, 120);
            } else {
                stroke(1.0f, strand == -1 || strand == 1 ? 58 : 34);
            }
        }
    }
    draw->PopClipRect();
}

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
        const auto viewport = visible_canvas();
        const auto size = ImGui::CalcTextSize(checking_message);
        const float top = std::max(viewport.top, entry_top);
        ImGui::GetWindowDrawList()->AddText(
            {viewport.left + std::max(0.0f, (viewport.width - size.x) * 0.5f),
             top + std::max(0.0f, (viewport.top + viewport.height - top - size.y) * 0.5f)},
            theme::packed(Color::secondary_text), checking_message);
        return;
    }
    if (state.page == EntryPage::sign_in) {
        draw_sign_in_background(entry_top);
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
            setup ? "Use this username to sign in to Wave after setup. Usernames are case-sensitive." : "");
        const bool password_enter = view::edit(
            "Password", state.password, "", enter_submits | ImGuiInputTextFlags_Password,
            setup ? "Wave checks for common passwords and predictable patterns. A longer passphrase "
                    "can help you choose a less predictable password."
                  : "");
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
