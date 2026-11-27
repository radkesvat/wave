#include "dialog.h"

#include "browser_ui.h"
#include "theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace wave {
namespace {

constexpr float margin = 12;
constexpr ImVec2 padding{12, 9};
constexpr float title_padding = 4;

} // namespace

void draw_busy_spinner(float row_height)
{
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({busy_spinner_width, row_height});
    if (!ImGui::IsItemVisible()) {
        return;
    }

    constexpr int segments = 30;
    const float radius = std::min(9.0f, row_height * 0.35f);
    const float time = static_cast<float>(ImGui::GetTime());
    const float sweep = (0.9f - 0.75f * std::abs(std::sin(time * 1.8f))) * 2 * IM_PI;
    const ImVec2 center{pos.x + busy_spinner_width * 0.5f, pos.y + row_height * 0.5f};
    auto* draw = ImGui::GetWindowDrawList();
    draw->PathArcTo(center, radius, time * 8.0f - sweep, time * 8.0f, segments);
    draw->PathStroke(ImGui::GetColorU32(theme::packed(theme::Color::info)), 0, 3.0f);
}

Dialog::Dialog(const char* title, float width, std::initializer_list<DialogAction> actions,
               DialogOptions options)
    : Dialog(title, width, std::span(actions), options)
{
}

Dialog::Dialog(const char* title, float width, std::span<const DialogAction> actions, DialogOptions options)
{
    if (!ImGui::IsPopupOpen(title)) {
        return;
    }
    const auto viewport = visible_canvas();
    const auto [open, entry_top, busy, footer_error, reveal_footer_error] = options;
    busy_ = busy.value_or(false);
    footer_error_ = footer_error;
    reveal_footer_error_ = reveal_footer_error;
    const auto& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float scale = font / theme::secondary_font_size;
    const float window_width =
        std::max(1.0f, std::min(std::floor(width * scale), viewport.width - 2 * margin));
    const float content_width = std::max(1.0f, window_width - 2 * padding.x);
    const float row_gap = style.ItemSpacing.y;
    button_height_ = std::max(viewport.width < 500 ? 40.0f : 32.0f, font + 12);

    buttons_.reserve(actions.size());
    const auto layout_footer = [&](float available_width) {
        buttons_.clear();
        last_row_ = 0;
        action_offset_ = 0;
        float right = available_width;
        // Leave room beside the primary action in both the idle and busy states.
        const float left =
            busy ? std::min(busy_spinner_width + style.ItemSpacing.x, available_width * 0.5f) : 0;
        // Fit rows from the right so the primary action stays at the bottom right.
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            const float text_width = ImGui::CalcTextSize(it->label, nullptr, true).x;
            const float preferred =
                std::min(available_width, std::max(94.0f * scale, std::ceil(text_width) + 2 * padding.x));
            if (!buttons_.empty() && right - preferred < (last_row_ == 0 ? left : 0)) {
                ++last_row_;
                right = available_width;
            }
            const float button_width = std::min(preferred, right - (last_row_ == 0 ? left : 0));
            buttons_.push_back({*it, right - button_width, button_width, last_row_});
            right -= button_width + style.ItemSpacing.x;
        }
        if (!footer_error_.empty()) {
            const auto start = footer_error_.data();
            const auto end = start + footer_error_.size();
            const auto text_size = ImGui::CalcTextSize(start, end);
            if (busy_ || last_row_ > 0 || text_size.x > right || text_size.y > button_height_) {
                action_offset_ = ImGui::CalcTextSize(start, end, false, available_width).y + row_gap;
            }
        }
    };
    layout_footer(content_width);

    float title_font = font;
    const float close_width = open ? font + 2 * style.ItemInnerSpacing.x : 0;
    if (ImGui::CalcTextSize(title, nullptr, true).x + close_width > content_width) {
        title_font = theme::annotation_font_size;
    }
    const float chrome_height = title_font + 2 * (title_padding + padding.y + margin);
    const float minimum_body_height = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight();
    const float bottom = viewport.top + viewport.height;
    float top = std::max(viewport.top, entry_top);
    if (bottom - top < chrome_height + minimum_body_height) {
        top = viewport.top;
    }
    const float height = std::max(1.0f, bottom - top);
    const float content_height = height - chrome_height;
    const float footer_height =
        action_offset_ + (last_row_ + 1) * (button_height_ + row_gap) + theme::border_width + row_gap;
    // Keep a label and its field visible together when deciding whether to pin the actions.
    scroll_footer_ = content_height - footer_height < minimum_body_height;
    const float body_height = std::max(1.0f, content_height - (scroll_footer_ ? 0.0f : footer_height));

    ImGui::SetNextWindowPos({viewport.left + viewport.width * 0.5f, top + height * 0.5f}, ImGuiCond_Always,
                            {0.5f, 0.5f});
    ImGui::SetNextWindowSize({window_width, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::square_radius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, {0, 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {padding.x - theme::border_width, title_padding});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::rgba(theme::Color::pane));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::rgba(theme::Color::header));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::rgba(theme::Color::header));
    if (entry_top > 0) {
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, theme::rgba(theme::Color::transparent));
    }
    ImGui::PushFont(nullptr, title_font);
    visible_ = ImGui::BeginPopupModal(title, open,
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopFont();
    ImGui::PopStyleColor(entry_top > 0 ? 4 : 3);
    ImGui::PopStyleVar(4);
    if (!visible_) {
        return;
    }

    // A walkthrough can reuse the popup ID while changing the title and content.
    const auto title_hash = static_cast<int>(ImHashStr(title, ImGui::FindRenderedTextEnd(title) - title));
    const ImGuiID title_slot = ImGui::GetID("Dialog body title");
    auto* storage = ImGui::GetStateStorage();
    if (ImGui::IsWindowAppearing() || storage->GetInt(title_slot) != title_hash) {
        ImGui::SetNextWindowScroll({0, 0});
        storage->SetInt(title_slot, title_hash);
    }
    ImGui::SetNextWindowSizeConstraints({0, 0}, {content_width, body_height});
    ImGui::BeginChild("Dialog body", {},
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize |
                          ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    body_open_ = true;
    if (scroll_footer_) {
        layout_footer(std::max(1.0f, ImGui::GetContentRegionAvail().x));
    }
}

Dialog::~Dialog()
{
    if (body_open_) {
        ImGui::EndChild();
    }
    if (visible_) {
        ImGui::EndPopup();
    }
}

int Dialog::footer()
{
    if (!scroll_footer_) {
        ImGui::EndChild();
        body_open_ = false;
    }
    ImGui::Separator();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (!footer_error_.empty()) {
        if (action_offset_ == 0) {
            const float height =
                ImGui::CalcTextSize(footer_error_.data(), footer_error_.data() + footer_error_.size()).y;
            ImGui::SetCursorScreenPos({start.x, start.y + std::floor((button_height_ - height) * 0.5f)});
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(theme::Color::error));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(footer_error_.data(), footer_error_.data() + footer_error_.size());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (scroll_footer_ && reveal_footer_error_) {
            ImGui::SetScrollHereY(0.65f);
        }
    }
    const float step = button_height_ + ImGui::GetStyle().ItemSpacing.y;
    const float actions_top = start.y + action_offset_;
    int activated = -1;
    int index = 0;
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it, ++index) {
        const auto& button = *it;
        ImGui::SetCursorScreenPos(
            {start.x + button.x, actions_top + (last_row_ - button.row_from_bottom) * step});
        ImGui::BeginDisabled(button.action.disabled);
        if (ImGui::Button(button.action.label, {button.width, button_height_})) {
            activated = index;
        }
        ImGui::EndDisabled();
    }
    if (busy_) {
        ImGui::SetCursorScreenPos({start.x, actions_top + last_row_ * step});
        draw_busy_spinner(button_height_);
    }
    if (scroll_footer_) {
        ImGui::EndChild();
        body_open_ = false;
    }
    return activated;
}

bool dialog_open()
{
    return ImGui::GetTopMostPopupModal() != nullptr;
}

bool dialog_at(float x, float y)
{
    // The active ImGui modal also owns gestures when a combo opens over its body.
    const auto* window = ImGui::GetTopMostPopupModal();
    return window && window->Rect().Contains({x, y});
}

} // namespace wave
