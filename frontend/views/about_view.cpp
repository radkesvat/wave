#include "about_view.h"

#include "dialog.h"
#include "donation_face.h"
#include "preview_session.h"
#include "theme.h"
#include "view_helpers.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>

namespace wave {
namespace {

using Answer = AboutViewState::Answer;

constexpr char question[] = "did you donate anything to the project this month?";
constexpr char donate_label[] = "Donate";
constexpr char donation_url[] = "https://radkesvat-support.vercel.app/#support";
constexpr char change_answer[] = "Change answer";
constexpr char chooser_title[] = "Change answer###About donation";
constexpr char response_title[] = "Donation response###About donation";
constexpr char no_response[] = "the project is free, no problem";
constexpr char thanks[] = "Thank you for supporting the project!";
constexpr char reminder[] = "The sidebar badge will remind you next month (UTC).";
constexpr float donation_dialog_width = 420;
constexpr float face_gap = 7;

struct Choice {
    Answer answer;
    const char* key;
    const char* label;
    DonationFace face;
    float face_radius = 9;
};

constexpr std::array choices{
    Choice{Answer::no, "no", "no", DonationFace::sad},
    Choice{Answer::below_5, "below_5", "yes, below 5$", DonationFace::happy},
    Choice{Answer::more_than_5, "more_than_5", "yes, more than 5$", DonationFace::happiest, 12},
};

struct SavedDonation {
    const Choice* choice;
    std::string_view answered_at;
    bool reminder_due;

    bool show_summary() const
    {
        return choice && choice->answer != Answer::no && !reminder_due;
    }
};

SavedDonation saved_donation(const PreviewSession& session)
{
    const auto& record = view::field(view::field(session.data(), "settings"), "donation");
    const auto answer = view::string_field(record, "answer");
    const auto choice =
        std::ranges::find_if(choices, [answer](const Choice& item) { return answer == item.key; });
    return {choice == choices.end() ? nullptr : &*choice, view::string_field(record, "answered_at"),
            view::bool_field(record, "reminder_due", true)};
}

std::string saved_answer(const SavedDonation& donation)
{
    return std::format("{} {}.", donation.reminder_due ? "Last answer was" : "Saved answer is",
                       donation.choice->label);
}

std::string next_reminder(std::string_view timestamp)
{
    if (timestamp.size() != 20 || timestamp[4] != '-' || timestamp[7] != '-') {
        return {};
    }
    const auto* year_end = timestamp.data() + 4;
    const auto* month_end = timestamp.data() + 7;
    int year = 0;
    unsigned month = 0;
    const auto parsed_year = std::from_chars(timestamp.data(), year_end, year);
    const auto parsed_month = std::from_chars(year_end + 1, month_end, month);
    const auto answered = std::chrono::year{year} / std::chrono::month{month};
    if (parsed_year.ec != std::errc{} || parsed_year.ptr != year_end || parsed_month.ec != std::errc{} ||
        parsed_month.ptr != month_end || year <= 0 || !answered.ok()) {
        return {};
    }
    const auto next = answered + std::chrono::months{1};
    return std::format("Next reminder {:02}/{:02}/01 (UTC).", int(next.year()) % 100, unsigned(next.month()));
}

void render_no_donation_response()
{
    const auto text_size = ImGui::CalcTextSize(no_response);
    const float face_size = text_size.y;
    const bool fits_on_line = text_size.x + face_gap + face_size <= ImGui::GetContentRegionAvail().x;
    view::wrapped(no_response);
    if (fits_on_line) {
        ImGui::SameLine(0, face_gap);
    }
    const auto pos = ImGui::GetCursorScreenPos();
    const float radius = face_size / 2;
    ImGui::Dummy({face_size, face_size});
    draw_donation_face(*ImGui::GetWindowDrawList(), {pos.x + radius, pos.y + radius}, radius,
                       DonationFace::happy);
}

void render_donation_dialog(AboutViewState& state)
{
    const auto choice = std::ranges::find(choices, state.submitted, &Choice::answer);
    if (choice == choices.end()) {
        return;
    }
    const bool pending = state.donation_request.has_value();
    Dialog dialog(response_title, donation_dialog_width, {{"Close", pending}}, {.busy = pending});
    if (!dialog) {
        return;
    }

    if (choice->answer == Answer::no) {
        render_no_donation_response();
        ImGui::TextLinkOpenURL(donate_label, donation_url);
    } else {
        constexpr float dialog_face_radius = 12;
        const bool top_donation = choice->answer == Answer::more_than_5;
        const ImVec2 image_size =
            top_donation ? ImVec2{64, 48} : ImVec2{2 * dialog_face_radius, 2 * dialog_face_radius};
        const bool image_beside_text =
            !top_donation || ImGui::GetContentRegionAvail().x >= image_size.x + 120;
        const auto pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(image_size);
        auto& draw = *ImGui::GetWindowDrawList();
        if (top_donation) {
            draw_donation_response_sticker(draw, pos, image_size);
        } else {
            draw_donation_face(draw, {pos.x + dialog_face_radius, pos.y + dialog_face_radius},
                               dialog_face_radius, choice->face);
        }
        if (image_beside_text) {
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        view::wrapped(thanks);
        if (state.donation_saved) {
            view::wrapped(reminder, theme::Color::secondary_text);
        } else if (pending) {
            const auto available = ImGui::GetContentRegionAvail().x;
            ImGui::Dummy({0, ImGui::CalcTextSize(reminder, nullptr, false, available).y});
        }
        ImGui::EndGroup();
    }
    if (dialog.footer() == 0) {
        ImGui::CloseCurrentPopup();
        state.submitted = Answer::none;
    }
}

bool donation_button(const Choice& choice, ImVec2 text_size, float width, float height)
{
    ImGui::PushID(choice.key);
    const bool pressed = theme::button("##donation answer", {width, height});
    if (ImGui::IsItemVisible()) {
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const float center_y = (min.y + max.y) / 2;
        const float content_width = 2 * choice.face_radius + face_gap + text_size.x;
        const float left = std::floor(min.x + (width - content_width) / 2);
        auto& draw = *ImGui::GetWindowDrawList();
        draw_donation_face(draw, {left + choice.face_radius, center_y}, choice.face_radius, choice.face);
        draw.AddText({left + 2 * choice.face_radius + face_gap, std::floor(center_y - text_size.y / 2)},
                     ImGui::GetColorU32(ImGuiCol_Text), choice.label);
    }
    ImGui::PopID();
    return pressed;
}

const Choice* render_donation_choices(bool enabled)
{
    view::wrapped(question);

    const auto& style = ImGui::GetStyle();
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = std::max(40.0f, ImGui::GetFrameHeight() + 6);
    std::array<ImVec2, choices.size()> label_sizes{};
    std::array<float, choices.size()> widths{};
    float total_width = style.ItemSpacing.x * (choices.size() - 1);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto& choice = choices[i];
        label_sizes[i] = ImGui::CalcTextSize(choice.label);
        widths[i] = 2 * (style.FramePadding.x + choice.face_radius) + face_gap + label_sizes[i].x;
        total_width += widths[i];
    }

    const bool fits_one_row = total_width <= available;
    const Choice* selected = nullptr;
    ImGui::BeginDisabled(!enabled);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i != 0 && fits_one_row) {
            ImGui::SameLine();
        }
        const auto& choice = choices[i];
        if (donation_button(choice, label_sizes[i], fits_one_row ? widths[i] : available, height)) {
            selected = &choice;
        }
    }
    ImGui::EndDisabled();
    return selected;
}

void submit_donation(AboutViewState& state, PreviewSession& session, const Choice& choice)
{
    state.choosing_answer = false;
    state.submitted = choice.answer;
    state.donation_saved = false;
    state.donation_request = session.submit({{"action", "apply_donation"}, {"answer", choice.key}}, 5000);
}

void render_donation_popup(AboutViewState& state, PreviewSession& session, bool enabled)
{
    if (!ImGui::IsPopupOpen(response_title)) {
        state.choosing_answer = false;
        state.submitted = Answer::none;
        return;
    }
    if (!state.choosing_answer) {
        render_donation_dialog(state);
        return;
    }

    Dialog chooser(chooser_title, donation_dialog_width, {{"Cancel"}});
    if (!chooser) {
        return;
    }
    const auto* selected = render_donation_choices(enabled);
    if (chooser.footer() == 0) {
        ImGui::CloseCurrentPopup();
        state.choosing_answer = false;
    } else if (selected) {
        // Keep the modal open at the same level while switching to the response.
        submit_donation(state, session, *selected);
    }
}

void render_donation_summary(const SavedDonation& donation)
{
    const auto& choice = *donation.choice;
    const float radius = choice.face_radius;
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({2 * radius, 2 * radius});
    draw_donation_face(*ImGui::GetWindowDrawList(), {pos.x + radius, pos.y + radius}, radius, choice.face);
    ImGui::SameLine();
    ImGui::BeginGroup();
    view::wrapped(saved_answer(donation));
    if (const auto date = next_reminder(donation.answered_at); !date.empty()) {
        view::wrapped(date, theme::Color::secondary_text);
    }
    ImGui::EndGroup();
}

void render_donations(AboutViewState& state, PreviewSession& session)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Donations");
    const auto donation = saved_donation(session);
    const bool enabled = session.ready() && !state.donation_request;
    if (donation.show_summary()) {
        render_donation_summary(donation);
        ImGui::BeginDisabled(!enabled);
        if (theme::button(change_answer, {0, std::max(40.0f, ImGui::GetFrameHeight() + 6)})) {
            state.choosing_answer = true;
            ImGui::OpenPopup(chooser_title);
        }
        ImGui::EndDisabled();
    } else {
        if (const auto* selected = render_donation_choices(enabled)) {
            submit_donation(state, session, *selected);
            ImGui::OpenPopup(response_title);
        }
        if (donation.choice) {
            view::wrapped(saved_answer(donation), theme::Color::secondary_text);
        }
    }

    ImGui::TextLinkOpenURL(donate_label, donation_url);
    render_donation_popup(state, session, enabled);
    ImGui::Spacing();
}

} // namespace

void update_about(AboutViewState& state, PreviewSession& session)
{
    if (const auto reply = session.take_reply(state.donation_request)) {
        state.donation_saved = reply->ok();
    }
}

void render_about(AboutViewState& state, PreviewSession& session, std::string_view version)
{
    ImGui::TextColored(theme::rgba(theme::Color::accent), "Wave %.*s", static_cast<int>(version.size()),
                       version.data());
    ImGui::TextWrapped("Wave is a browser panel for the WaterWall core.");
    render_donations(state, session);
    ImGui::SeparatorText("Projects");
    ImGui::TextLinkOpenURL("Wave source", "https://github.com/radkesvat/wave");
    ImGui::TextLinkOpenURL("WaterWall core", "https://github.com/radkesvat/WaterWall");
    ImGui::SeparatorText("Notices");
    ImGui::TextLinkOpenURL("Wave (Mozilla Public License 2.0)", "/licenses/wave.txt");
    ImGui::TextLinkOpenURL("Dear ImGui (MIT)", "/licenses/dear-imgui.txt");
    ImGui::TextLinkOpenURL("Poppins Medium (SIL OFL 1.1)", "/licenses/poppins.txt");
    ImGui::TextLinkOpenURL("nlohmann/json (MIT)", "/licenses/nlohmann-json.txt");
    ImGui::TextLinkOpenURL("nlohmann/json third-party notices", "/licenses/nlohmann-json-third-party.txt");
    ImGui::TextLinkOpenURL("ImPlot (MIT)", "/licenses/implot.txt");
    ImGui::TextLinkOpenURL("xterm.js (MIT)", "/licenses/xterm.txt");
    ImGui::TextLinkOpenURL("xterm.js Fit addon (MIT)", "/licenses/xterm-addon-fit.txt");
}

std::string describe_about(const AboutViewState& state, const PreviewSession& session)
{
    const auto donation = saved_donation(session);
    std::string description = "Donations.";
    if (donation.show_summary()) {
        description += ' ' + saved_answer(donation);
        if (const auto date = next_reminder(donation.answered_at); !date.empty()) {
            description += ' ' + date;
        }
        description += std::format(" {}.", change_answer);
    }
    if (!donation.show_summary() || state.choosing_answer) {
        description += std::format(" {}", question);
        for (const auto& choice : choices) {
            description += std::format(" {}.", choice.label);
        }
        if (!donation.show_summary() && donation.choice) {
            description += ' ' + saved_answer(donation);
        }
        if (state.choosing_answer) {
            description += " Cancel.";
        }
    }
    if (state.submitted != Answer::none) {
        description +=
            std::format(" Donation response. {}", state.submitted == Answer::no ? no_response : thanks);
        if (state.submitted != Answer::no && state.donation_saved) {
            description += std::format(" {}", reminder);
        }
        if (state.donation_request) {
            description += std::string(" ") + busy_description;
        }
    }
    if (!state.choosing_answer && (state.submitted == Answer::none || state.submitted == Answer::no)) {
        description += std::format(" {} link.", donate_label);
    }
    return description;
}

} // namespace wave
