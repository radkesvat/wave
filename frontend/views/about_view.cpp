#include "about_view.h"

#include "dialog.h"
#include "donation_face.h"
#include "preview_session.h"
#include "theme.h"
#include "view_helpers.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace wave {
namespace {

constexpr char question[] = "Did you donate to the project in the past 30 days?";
constexpr char donate_label[] = "Donate";
constexpr char donation_url[] = "https://radkesvat-support.vercel.app/#support";
constexpr char change_answer[] = "Change answer";
constexpr char chooser_title[] = "Change answer###About donation";
constexpr char response_title[] = "Donation response###About donation";
constexpr char project_is_free[] = "The project is free.";
constexpr char no_problem[] = "No problem";
constexpr char thanks[] = "Thank you for supporting the project!";
constexpr char reminder[] = "The sidebar badge will remind you in 30 days.";
constexpr float response_dialog_width = 420;
constexpr float chooser_dialog_width = 737;

struct SavedDonation {
    const DonationChoice* choice;
    std::string_view next_reminder_at;
    bool reminder_due;

    bool show_summary() const
    {
        return choice && !reminder_due;
    }
};

SavedDonation saved_donation(const PreviewSession& session)
{
    const auto& record = view::field(view::field(session.data(), "settings"), "donation");
    const auto answer = view::string_field(record, "answer");
    return {find_donation_choice(answer), view::string_field(record, "next_reminder_at"),
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
    return std::format("Next reminder {}/{}/{} (UTC).", timestamp.substr(2, 2), timestamp.substr(5, 2),
                       timestamp.substr(8, 2));
}

void render_declined_response()
{
    const float diameter = ImGui::GetTextLineHeight();
    const float sticker_gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float space = ImGui::CalcTextSize(" ").x;
    const float width = ImGui::CalcTextSize(project_is_free).x + space + ImGui::CalcTextSize(no_problem).x +
        sticker_gap + diameter;
    const bool fits_one_line = width <= ImGui::GetContentRegionAvail().x;
    view::wrapped(project_is_free);
    if (fits_one_line) {
        ImGui::SameLine(0, space);
    }
    view::text(no_problem);
    ImGui::SameLine(0, sticker_gap);
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({diameter, diameter});

    const ImVec2 center{pos.x + diameter / 2, pos.y + diameter / 2};
    const float radius = diameter * 0.44f;
    const auto ink = theme::packed(theme::Color::inset);
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddCircleFilled(center, radius, theme::packed(theme::Color::warning));
    for (const float side : {-1.0f, 1.0f}) {
        draw.AddCircleFilled({center.x + side * radius * 0.34f, center.y - radius * 0.28f},
                             std::max(1.0f, radius / 9), ink);
    }
    const float mouth_y = center.y + radius * 0.12f;
    draw.AddBezierQuadratic({center.x - radius * 0.4f, mouth_y}, {center.x, center.y + radius * 0.74f},
                            {center.x + radius * 0.4f, mouth_y}, ink, std::max(1.2f, radius * 0.13f));

    ImGui::TextLinkOpenURL(donate_label, donation_url);
}

void render_donation_dialog(AboutViewState& state)
{
    if (!state.submitted) {
        return;
    }
    const bool pending = state.donation_request.has_value();
    Dialog dialog(response_title, response_dialog_width, {{"Close", pending}}, {.busy = pending});
    if (!dialog) {
        return;
    }

    const auto face = state.submitted->face;
    const ImVec2 image_size = face == DonationFace::gigachad ? ImVec2{64, 48} : ImVec2{56, 56};
    const bool image_beside_text = ImGui::GetContentRegionAvail().x >= image_size.x + 140;
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(image_size);
    draw_donation_response_sticker(*ImGui::GetWindowDrawList(), pos, image_size, face);
    if (image_beside_text) {
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    if (!state.submitted->is_donation()) {
        render_declined_response();
    } else {
        view::wrapped(thanks);
        if (state.donation_saved) {
            view::wrapped(reminder, theme::Color::secondary_text);
        } else if (pending) {
            const auto available = ImGui::GetContentRegionAvail().x;
            ImGui::Dummy({0, ImGui::CalcTextSize(reminder, nullptr, false, available).y});
        }
    }
    ImGui::EndGroup();
    if (dialog.footer() == 0) {
        ImGui::CloseCurrentPopup();
        state.submitted = nullptr;
    }
}

constexpr float choice_image_gap = 5;
constexpr float choice_padding = 7;

bool donation_button(const DonationChoice& choice, float width, float height, float image_size)
{
    ImGui::PushID(choice.key);
    const bool pressed = theme::button("##donation answer", {width, height});
    if (ImGui::IsItemVisible()) {
        const auto min = ImGui::GetItemRectMin();
        const auto label = ImGui::CalcTextSize(choice.label);
        auto& draw = *ImGui::GetWindowDrawList();
        draw_donation_face(draw, {std::floor(min.x + (width - image_size) / 2), min.y + choice_padding},
                           {image_size, image_size}, choice.face);
        draw.AddText({std::floor(min.x + (width - label.x) / 2),
                      min.y + choice_padding + image_size + choice_image_gap},
                     ImGui::GetColorU32(ImGuiCol_Text), choice.label);
    }
    ImGui::PopID();
    return pressed;
}

const DonationChoice* render_donation_choices(bool enabled, float max_image_size, float max_width)
{
    view::wrapped(question);

    ImGui::PushFont(nullptr, theme::annotation_font_size);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float min_width = std::ceil(view::button_width(donation_choices.front().label));
    const auto columns =
        static_cast<std::size_t>(std::clamp((available + spacing) / (min_width + spacing), 1.0f, 4.0f));
    const float width = std::floor(std::min(max_width, (available - (columns - 1) * spacing) / columns));
    const float image_size = std::clamp(width - 2 * choice_padding, 1.0f, max_image_size);
    const float height = 2 * choice_padding + image_size + choice_image_gap + ImGui::GetTextLineHeight();
    const DonationChoice* selected = nullptr;
    ImGui::BeginDisabled(!enabled);
    for (std::size_t i = 0; i < donation_choices.size(); ++i) {
        if (i % columns != 0) {
            ImGui::SameLine();
        }
        const auto& choice = donation_choices[i];
        if (donation_button(choice, width, height, image_size)) {
            selected = &choice;
        }
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
    return selected;
}

void submit_donation(AboutViewState& state, PreviewSession& session, const DonationChoice& choice)
{
    state.choosing_answer = false;
    state.submitted = &choice;
    state.donation_saved = false;
    state.donation_request = session.submit({{"action", "apply_donation"}, {"answer", choice.key}}, 5000);
}

void render_donation_popup(AboutViewState& state, PreviewSession& session, bool enabled)
{
    if (!ImGui::IsPopupOpen(response_title)) {
        state.choosing_answer = false;
        state.submitted = nullptr;
        return;
    }
    if (!state.choosing_answer) {
        render_donation_dialog(state);
        return;
    }

    Dialog chooser(chooser_title, chooser_dialog_width, {{"Cancel"}});
    if (!chooser) {
        return;
    }
    const auto* selected = render_donation_choices(enabled, 78, 218);
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
    if (donation.choice->face != DonationFace::none) {
        constexpr ImVec2 size{40, 40};
        const auto pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        draw_donation_face(*ImGui::GetWindowDrawList(), pos, size, donation.choice->face);
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    view::wrapped(saved_answer(donation));
    if (const auto date = next_reminder(donation.next_reminder_at); !date.empty()) {
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
        if (const auto* selected = render_donation_choices(enabled, 44, 124)) {
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
    ImGui::TextLinkOpenURL("utf8proc (MIT and Unicode data notices)", "/licenses/utf8proc.txt");
    ImGui::TextLinkOpenURL("ImPlot (MIT)", "/licenses/implot.txt");
    ImGui::TextLinkOpenURL("xterm.js (MIT)", "/licenses/xterm.txt");
    ImGui::TextLinkOpenURL("xterm.js Fit addon (MIT)", "/licenses/xterm-addon-fit.txt");
    ImGui::TextLinkOpenURL("vanilla-jsoneditor (ISC)", "/licenses/vanilla-jsoneditor.txt");
    ImGui::TextLinkOpenURL("vanilla-jsoneditor third-party notices",
                           "/licenses/vanilla-jsoneditor-third-party.txt");
    ImGui::TextLinkOpenURL("lossless-json (MIT)", "/licenses/lossless-json.txt");
}

std::string describe_about(const AboutViewState& state, const PreviewSession& session)
{
    const auto donation = saved_donation(session);
    std::string description = "Donations.";
    if (donation.show_summary()) {
        description += ' ' + saved_answer(donation);
        if (const auto date = next_reminder(donation.next_reminder_at); !date.empty()) {
            description += ' ' + date;
        }
        description += std::format(" {}.", change_answer);
    }
    if (!donation.show_summary() || state.choosing_answer) {
        description += std::format(" {}", question);
        for (const auto& choice : donation_choices) {
            description += std::format(" {}.", choice.label);
        }
        if (!donation.show_summary() && donation.choice) {
            description += ' ' + saved_answer(donation);
        }
        if (state.choosing_answer) {
            description += " Cancel.";
        }
    }
    const bool declined = state.submitted && !state.submitted->is_donation();
    if (state.submitted) {
        description += " Donation response.";
        if (declined) {
            description += std::format(" {} {}.", project_is_free, no_problem);
        } else {
            description += std::format(" {}", thanks);
        }
        if (!declined && state.donation_saved) {
            description += std::format(" {}", reminder);
        }
        if (state.donation_request) {
            description += std::string(" ") + busy_description;
        }
    }
    if (!state.choosing_answer && (!state.submitted || declined)) {
        description += std::format(" {} link.", donate_label);
    }
    return description;
}

} // namespace wave
