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
    colors[index(Color::terminal)] = rgb(0x050506);
    colors[index(Color::raised)] = rgb(0x2B2730);
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
    colors[index(Color::button_face)] = rgb(0x45404F);
    colors[index(Color::button_hover)] = rgb(0x595067);
    colors[index(Color::button_pressed)] = rgb(0x513938);
    colors[index(Color::button_shadow)] = rgb(0x050405, 200);
    colors[index(Color::selected)] = colors[index(Color::raised)];
    colors[index(Color::chart_fill)] = rgb(active_orange, 38);
    colors[index(Color::text_selection)] = rgb(0x643B2E);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    colors[index(Color::dim)] = rgb(0x050506, 170);
    colors[index(Color::transparent)] = 0;
    return colors;
}();

constexpr auto standard_high_contrast = [] {
    auto colors = standard;
    colors[index(Color::divider)] = rgb(0x7C7789);
    colors[index(Color::plot_grid)] = rgb(0x53505C);
    colors[index(Color::control_border)] = rgb(0xB5B0C4);
    colors[index(Color::secondary_text)] = rgb(0xD3D2E5);
    colors[index(Color::disabled_text)] = rgb(0xC4C1D5);
    colors[index(Color::focus)] = rgb(0xFFB48F);
    colors[index(Color::hover)] = rgb(0x423B49);
    colors[index(Color::pressed)] = rgb(0x533E48);
    colors[index(Color::button_face)] = rgb(0x554C60);
    colors[index(Color::button_hover)] = rgb(0x665A74);
    colors[index(Color::button_pressed)] = rgb(0x633F3C);
    colors[index(Color::selected)] = rgb(0x3B3442);
    colors[index(Color::text_selection)] = rgb(0x754430);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}();

constexpr auto light = [] {
    auto colors = standard;
    colors[index(Color::canvas)] = rgb(0xF0F2F7);
    colors[index(Color::header)] = rgb(0xE4E8F0);
    colors[index(Color::inset)] = rgb(0xFFFFFF);
    colors[index(Color::pane)] = rgb(0xF9FAFD);
    colors[index(Color::toolbar)] = rgb(0xE7EAF2);
    colors[index(Color::terminal)] = colors[index(Color::pane)];
    colors[index(Color::raised)] = rgb(0xE1E5EE);
    colors[index(Color::divider)] = rgb(0xA4ABBD);
    colors[index(Color::plot_grid)] = rgb(0xCED4E2);
    colors[index(Color::control_border)] = rgb(0x687189);
    colors[index(Color::primary_text)] = rgb(0x232834);
    colors[index(Color::secondary_text)] = rgb(0x465269);
    colors[index(Color::disabled_text)] = rgb(0x626D80);
    colors[index(Color::accent)] = rgb(0x9E420B);
    colors[index(Color::focus)] = colors[index(Color::accent)];
    colors[index(Color::success)] = rgb(0x2D6716);
    colors[index(Color::info)] = rgb(0x145C9E);
    colors[index(Color::error)] = rgb(0xA52F33);
    colors[index(Color::warning)] = rgb(0x875000);
    colors[index(Color::hover)] = rgb(0xD8DEEB);
    colors[index(Color::pressed)] = rgb(0xCED4E2);
    colors[index(Color::button_face)] = rgb(0xE6E9F2);
    colors[index(Color::button_hover)] = rgb(0xD1D9EB);
    colors[index(Color::button_pressed)] = rgb(0xC3CDE2);
    colors[index(Color::button_shadow)] = rgb(0x263750, 36);
    colors[index(Color::selected)] = rgb(0xD8E0F0);
    colors[index(Color::chart_fill)] = rgb(0x9E420B, 38);
    colors[index(Color::text_selection)] = rgb(0xBDCEE9);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    colors[index(Color::dim)] = rgb(0x232834, 120);
    return colors;
}();

constexpr auto classic = [] {
    auto colors = standard;
    colors[index(Color::canvas)] = rgb(0x1B2529);
    colors[index(Color::header)] = rgb(0x333740);
    colors[index(Color::inset)] = rgb(0x141A22);
    colors[index(Color::pane)] = rgb(0x232C36);
    colors[index(Color::toolbar)] = rgb(0x181B25);
    colors[index(Color::terminal)] = rgb(0x121B21);
    colors[index(Color::raised)] = rgb(0x303C48);
    colors[index(Color::divider)] = rgb(0x4B5662);
    colors[index(Color::plot_grid)] = rgb(0x36414B);
    colors[index(Color::control_border)] = rgb(0x778999);
    colors[index(Color::primary_text)] = rgb(0xF1F4FA);
    colors[index(Color::secondary_text)] = rgb(0xBFC8D5);
    colors[index(Color::disabled_text)] = rgb(0xADB8CA);
    colors[index(Color::accent)] = rgb(0x65A7F5);
    colors[index(Color::focus)] = colors[index(Color::accent)];
    colors[index(Color::success)] = rgb(0x86CC88);
    colors[index(Color::info)] = rgb(0x74D5D1);
    colors[index(Color::error)] = rgb(0xFF8A80);
    colors[index(Color::warning)] = rgb(0xF9C973);
    colors[index(Color::hover)] = rgb(0x394956);
    colors[index(Color::pressed)] = rgb(0x324A61);
    colors[index(Color::button_face)] = rgb(0x364653);
    colors[index(Color::button_hover)] = rgb(0x495C6D);
    colors[index(Color::button_pressed)] = rgb(0x354D65);
    colors[index(Color::button_shadow)] = rgb(0x0C121A, 165);
    colors[index(Color::selected)] = rgb(0x223B53);
    colors[index(Color::chart_fill)] = rgb(0x65A7F5, 38);
    colors[index(Color::text_selection)] = rgb(0x30567A);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}();

constexpr auto dark = [] {
    auto colors = standard;
    colors[index(Color::canvas)] = rgb(0x171A20);
    colors[index(Color::header)] = rgb(0x101217);
    colors[index(Color::inset)] = rgb(0x111318);
    colors[index(Color::pane)] = rgb(0x22262E);
    colors[index(Color::toolbar)] = rgb(0x1D2027);
    colors[index(Color::terminal)] = colors[index(Color::header)];
    colors[index(Color::raised)] = rgb(0x2B303B);
    colors[index(Color::divider)] = rgb(0x48505F);
    colors[index(Color::plot_grid)] = rgb(0x3C4350);
    colors[index(Color::control_border)] = rgb(0x808BA1);
    colors[index(Color::primary_text)] = rgb(0xE9EDF5);
    colors[index(Color::secondary_text)] = rgb(0xB7C3D4);
    colors[index(Color::disabled_text)] = rgb(0xA7B1C5);
    colors[index(Color::accent)] = rgb(0xC1A4FF);
    colors[index(Color::focus)] = colors[index(Color::accent)];
    colors[index(Color::info)] = rgb(0x74BBF4);
    colors[index(Color::hover)] = rgb(0x343C49);
    colors[index(Color::pressed)] = rgb(0x3C3550);
    colors[index(Color::button_face)] = rgb(0x393F4D);
    colors[index(Color::button_hover)] = rgb(0x4A5264);
    colors[index(Color::button_pressed)] = rgb(0x4C3D65);
    colors[index(Color::selected)] = rgb(0x373146);
    colors[index(Color::chart_fill)] = rgb(0xC1A4FF, 38);
    colors[index(Color::text_selection)] = rgb(0x4C3D65);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}();

constexpr auto darker = [] {
    auto colors = standard;
    colors[index(Color::canvas)] = rgb(0x08090B);
    colors[index(Color::header)] = rgb(0x0D0E10);
    colors[index(Color::inset)] = rgb(0x050608);
    colors[index(Color::pane)] = rgb(0x141517);
    colors[index(Color::toolbar)] = rgb(0x111214);
    colors[index(Color::terminal)] = rgb(0x030405);
    colors[index(Color::raised)] = rgb(0x202124);
    colors[index(Color::divider)] = rgb(0x36383D);
    colors[index(Color::plot_grid)] = colors[index(Color::divider)];
    colors[index(Color::control_border)] = rgb(0x787B84);
    colors[index(Color::primary_text)] = rgb(0xF2F3F5);
    colors[index(Color::secondary_text)] = rgb(0xB8BBC3);
    colors[index(Color::disabled_text)] = rgb(0xA5A9B3);
    colors[index(Color::accent)] = rgb(0xFA8B7F);
    colors[index(Color::focus)] = colors[index(Color::accent)];
    colors[index(Color::error)] = rgb(0xFD747E);
    colors[index(Color::hover)] = rgb(0x303238);
    colors[index(Color::pressed)] = rgb(0x40292D);
    colors[index(Color::button_face)] = rgb(0x34363D);
    colors[index(Color::button_hover)] = rgb(0x474B54);
    colors[index(Color::button_pressed)] = rgb(0x503337);
    colors[index(Color::selected)] = rgb(0x322528);
    colors[index(Color::chart_fill)] = rgb(0xFA8B7F, 38);
    colors[index(Color::text_selection)] = rgb(0x532E34);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}();

constexpr ImU32 mix(ImU32 from, ImU32 to, unsigned percent)
{
    const auto channel = [=](unsigned shift) {
        const auto start = (from >> shift) & 0xFF;
        const auto end = (to >> shift) & 0xFF;
        return (start * (100 - percent) + end * percent + 50) / 100;
    };
    return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT),
                    channel(IM_COL32_A_SHIFT));
}

constexpr Palette with_high_contrast(Palette colors)
{
    const auto text = colors[index(Color::primary_text)];
    const auto strengthen = [&](Color color, unsigned percent) {
        auto& value = colors[index(color)];
        value = mix(value, text, percent);
    };
    strengthen(Color::divider, 44);
    strengthen(Color::plot_grid, 24);
    strengthen(Color::control_border, 45);
    strengthen(Color::secondary_text, 50);
    strengthen(Color::disabled_text, 35);
    strengthen(Color::focus, 32);
    strengthen(Color::hover, 12);
    strengthen(Color::pressed, 15);
    strengthen(Color::selected, 12);
    colors[index(Color::scrollbar)] = colors[index(Color::control_border)];
    return colors;
}

constexpr std::array presets{standard, light, classic, dark, darker};
constexpr std::array high_contrast_presets{standard_high_contrast, with_high_contrast(light),
                                           with_high_contrast(classic), with_high_contrast(dark),
                                           with_high_contrast(darker)};

const Palette* current = &standard;

void push_button_colors()
{
    ImGui::PushStyleColor(ImGuiCol_Button, rgba(Color::button_face));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(Color::button_hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(Color::button_pressed));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, rgba(Color::button_shadow));
}

} // namespace

ImU32 packed(Color color)
{
    return (*current)[index(color)];
}

ImVec4 rgba(Color color)
{
    return ImGui::ColorConvertU32ToFloat4(packed(color));
}

void apply(Appearance appearance)
{
    const auto& palettes = appearance.high_contrast ? high_contrast_presets : presets;
    current = &palettes[static_cast<std::size_t>(appearance.preset)];
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
    style.DisabledAlpha = appearance.high_contrast ? 0.90f : 0.82f;

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

bool button(const char* label, const ImVec2& size)
{
    push_button_colors();
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool small_button(const char* label)
{
    push_button_colors();
    const bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

} // namespace wave::theme
