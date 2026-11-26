#include "theme.h"

#include "theme_swatches.h"

#include <array>
#include <cstddef>

namespace wave::theme {
namespace {

constexpr std::size_t index(Color color)
{
    return static_cast<std::size_t>(color);
}

constexpr ImU32 rgb(unsigned value, unsigned alpha = 255)
{
    return IM_COL32(value >> 16 & 0xFF, value >> 8 & 0xFF, value & 0xFF, alpha);
}

constexpr unsigned active_orange = 0xF3814A;
using Palette = std::array<ImU32, index(Color::count)>;

constexpr auto standard = [] {
    Palette colors{};
    colors[index(Color::canvas)] = rgb(startup_canvas);
    colors[index(Color::header)] = rgb(0x080709);
    colors[index(Color::inset)] = rgb(0x0D0B0E);
    colors[index(Color::pane)] = rgb(0x1F1B22);
    colors[index(Color::toolbar)] = rgb(0x1A171C);
    colors[index(Color::raised)] = rgb(0x2B2730);
    colors[index(Color::console)] = rgb(0x050506);
    colors[index(Color::divider)] = rgb(0x3D3A43);
    colors[index(Color::plot_grid)] = colors[index(Color::divider)];
    colors[index(Color::control_border)] = rgb(0x8A8495);
    colors[index(Color::primary_text)] = rgb(startup_text);
    colors[index(Color::secondary_text)] = rgb(0xB5B4C5);
    colors[index(Color::disabled_text)] = rgb(0xABA7BA);
    colors[index(Color::accent)] = rgb(active_orange);
    colors[index(Color::focus)] = colors[index(Color::accent)];
    colors[index(Color::success)] = rgb(0xA3D353);
    colors[index(Color::info)] = rgb(0x51A5F6);
    colors[index(Color::error)] = rgb(0xF17F6F);
    colors[index(Color::warning)] = rgb(0xFFC36A);
    colors[index(Color::hover)] = rgb(0x37313B);
    colors[index(Color::pressed)] = rgb(0x41313B);
    colors[index(Color::selected)] = colors[index(Color::raised)];
    colors[index(Color::chart_fill)] = rgb(active_orange, 38);
    colors[index(Color::text_selection)] = rgb(0x643B2E);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    colors[index(Color::dim)] = rgb(0x050506, 170);
    colors[index(Color::transparent)] = 0;
    return colors;
}();

constexpr auto stronger = [] {
    auto colors = standard;
    colors[index(Color::divider)] = rgb(0x7C7789);
    colors[index(Color::plot_grid)] = rgb(0x53505C);
    colors[index(Color::control_border)] = rgb(0xB5B0C4);
    colors[index(Color::secondary_text)] = rgb(0xD3D2E5);
    colors[index(Color::disabled_text)] = rgb(0xC4C1D5);
    colors[index(Color::focus)] = rgb(0xFFB48F);
    colors[index(Color::hover)] = rgb(0x423B49);
    colors[index(Color::pressed)] = rgb(0x533E48);
    colors[index(Color::selected)] = rgb(0x3B3442);
    colors[index(Color::text_selection)] = rgb(0x754430);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}();

const Palette* current = &standard;

} // namespace

ImU32 packed(Color color)
{
    return (*current)[index(color)];
}

ImVec4 rgba(Color color)
{
    return ImGui::ColorConvertU32ToFloat4(packed(color));
}

void apply(bool high_contrast)
{
    current = high_contrast ? &stronger : &standard;
    auto& style = ImGui::GetStyle();
    style.WindowPadding = window_padding;
    style.FramePadding = {8, 5};
    style.ItemSpacing = {7, 7};
    style.CellPadding = {7, 5};
    style.WindowRounding = square_radius;
    style.ChildRounding = square_radius;
    style.FrameRounding = control_radius;
    style.GrabRounding = control_radius;
    style.ScrollbarRounding = scrollbar_radius;
    style.MenuItemRounding = control_radius;
    style.SelectableRounding = square_radius;
    style.PopupRounding = popup_radius;
    style.TabRounding = tab_radius;
    style.WindowBorderSize = border_width;
    style.ChildBorderSize = border_width;
    style.PopupBorderSize = border_width;
    style.FrameBorderSize = border_width;
    style.TabBorderSize = border_width;
    style.TabBarBorderSize = border_width;
    style.TabBarOverlineSize = selection_width;
    style.SeparatorSize = border_width;
    style.SeparatorTextBorderSize = border_width;
    style.ScrollbarSize = scrollbar_size;
    style.DisabledAlpha = high_contrast ? 0.90f : 0.82f;

    const auto set = [&style](ImGuiCol slot, Color color) { style.Colors[slot] = rgba(color); };
    set(ImGuiCol_Text, Color::primary_text);
    set(ImGuiCol_TextDisabled, Color::disabled_text);
    set(ImGuiCol_WindowBg, Color::canvas);
    set(ImGuiCol_ChildBg, Color::canvas);
    set(ImGuiCol_PopupBg, Color::raised);
    set(ImGuiCol_Border, Color::control_border);
    set(ImGuiCol_BorderShadow, Color::transparent);
    set(ImGuiCol_FrameBg, Color::inset);
    set(ImGuiCol_FrameBgHovered, Color::hover);
    set(ImGuiCol_FrameBgActive, Color::pressed);
    set(ImGuiCol_TitleBg, Color::header);
    set(ImGuiCol_TitleBgActive, Color::toolbar);
    set(ImGuiCol_TitleBgCollapsed, Color::header);
    set(ImGuiCol_MenuBarBg, Color::toolbar);
    set(ImGuiCol_ScrollbarBg, Color::inset);
    set(ImGuiCol_ScrollbarGrab, Color::scrollbar);
    set(ImGuiCol_ScrollbarGrabHovered, Color::secondary_text);
    set(ImGuiCol_ScrollbarGrabActive, Color::accent);
    set(ImGuiCol_CheckMark, Color::success);
    set(ImGuiCol_CheckboxSelectedBg, Color::selected);
    set(ImGuiCol_SliderGrab, Color::scrollbar);
    set(ImGuiCol_SliderGrabActive, Color::accent);
    set(ImGuiCol_Button, Color::raised);
    set(ImGuiCol_ButtonHovered, Color::hover);
    set(ImGuiCol_ButtonActive, Color::pressed);
    set(ImGuiCol_Header, Color::selected);
    set(ImGuiCol_HeaderHovered, Color::hover);
    set(ImGuiCol_HeaderActive, Color::pressed);
    set(ImGuiCol_Separator, Color::divider);
    set(ImGuiCol_SeparatorHovered, Color::control_border);
    set(ImGuiCol_SeparatorActive, Color::accent);
    set(ImGuiCol_ResizeGrip, Color::control_border);
    set(ImGuiCol_ResizeGripHovered, Color::secondary_text);
    set(ImGuiCol_ResizeGripActive, Color::accent);
    set(ImGuiCol_InputTextCursor, Color::focus);
    set(ImGuiCol_Tab, Color::toolbar);
    set(ImGuiCol_TabHovered, Color::hover);
    set(ImGuiCol_TabSelected, Color::selected);
    set(ImGuiCol_TabSelectedOverline, Color::accent);
    set(ImGuiCol_TabDimmed, Color::toolbar);
    set(ImGuiCol_TabDimmedSelected, Color::selected);
    set(ImGuiCol_TabDimmedSelectedOverline, Color::accent);
    set(ImGuiCol_PlotLines, Color::info);
    set(ImGuiCol_PlotLinesHovered, Color::accent);
    set(ImGuiCol_PlotHistogram, Color::accent);
    set(ImGuiCol_PlotHistogramHovered, Color::warning);
    set(ImGuiCol_TableHeaderBg, Color::toolbar);
    set(ImGuiCol_TableBorderStrong, Color::divider);
    set(ImGuiCol_TableBorderLight, Color::divider);
    set(ImGuiCol_TableRowBg, Color::canvas);
    set(ImGuiCol_TableRowBgAlt, Color::pane);
    set(ImGuiCol_TextLink, Color::info);
    set(ImGuiCol_TextSelectedBg, Color::text_selection);
    set(ImGuiCol_TreeLines, Color::divider);
    set(ImGuiCol_DragDropTarget, Color::focus);
    set(ImGuiCol_DragDropTargetBg, Color::chart_fill);
    set(ImGuiCol_UnsavedMarker, Color::accent);
    set(ImGuiCol_NavCursor, Color::focus);
    set(ImGuiCol_NavWindowingHighlight, Color::focus);
    set(ImGuiCol_NavWindowingDimBg, Color::dim);
    set(ImGuiCol_ModalWindowDimBg, Color::dim);
}

void apply_text_scale(int viewport_width)
{
    const bool compact = viewport_width < workspace_font_breakpoint;
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = compact ? secondary_font_size : workspace_font_size;
    style.ItemSpacing.y = compact ? 5.0f : 7.0f;
}

} // namespace wave::theme
