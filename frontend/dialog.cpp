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

Dialog::Dialog(const char* title, float width, std::initializer_list<DialogAction> actions, bool* open,
               float entry_top)
    : Dialog(title, width, std::span(actions), open, entry_top)
{
}

Dialog::Dialog(const char* title, float width, std::span<const DialogAction> actions, bool* open,
               float entry_top)
{
    if (!ImGui::IsPopupOpen(title)) {
        return;
    }
    const auto viewport = visible_canvas();
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
        float right = available_width;
        // Fit rows from the right so the primary action stays at the bottom right.
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            const float text_width = ImGui::CalcTextSize(it->label, nullptr, true).x;
            const float button_width =
                std::min(available_width, std::max(94.0f * scale, std::ceil(text_width) + 2 * padding.x));
            if (right < button_width) {
                ++last_row_;
                right = available_width;
            }
            buttons_.push_back({*it, right - button_width, button_width, last_row_});
            right -= button_width + style.ItemSpacing.x;
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
    const float footer_height = (last_row_ + 1) * (button_height_ + row_gap) + theme::border_width + row_gap;
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
    const float step = button_height_ + ImGui::GetStyle().ItemSpacing.y;
    int activated = -1;
    int index = 0;
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it, ++index) {
        const auto& button = *it;
        ImGui::SetCursorScreenPos(
            {start.x + button.x, start.y + (last_row_ - button.row_from_bottom) * step});
        ImGui::BeginDisabled(button.action.disabled);
        if (ImGui::Button(button.action.label, {button.width, button_height_})) {
            activated = index;
        }
        ImGui::EndDisabled();
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
