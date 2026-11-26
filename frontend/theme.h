#pragma once

#include <imgui.h>

namespace wave::theme {

inline constexpr float annotation_font_size = 15;
inline constexpr float secondary_font_size = 18;
inline constexpr float workspace_font_size = 23;
inline constexpr float title_font_size = 26;
inline constexpr int workspace_font_breakpoint = 1600;
inline constexpr ImVec2 window_padding{11, 9};
inline constexpr float scrollbar_size = 13;

enum class Color {
    canvas,
    header,
    inset,
    pane,
    toolbar,
    raised,
    console,
    divider,
    plot_grid,
    control_border,
    primary_text,
    secondary_text,
    disabled_text,
    accent,
    focus,
    success,
    info,
    error,
    warning,
    hover,
    pressed,
    button_face,
    button_hover,
    button_pressed,
    button_shadow,
    selected,
    chart_fill,
    text_selection,
    scrollbar,
    dim,
    transparent,
    count
};

inline constexpr float square_radius = 0;
inline constexpr float control_radius = 5;
inline constexpr float pane_radius = 9;
inline constexpr float tab_radius = 7;
inline constexpr float popup_radius = 8;
inline constexpr float scrollbar_radius = 9;
inline constexpr float border_width = 1;
inline constexpr float selection_width = 2;
inline constexpr float focus_width = 2;

ImU32 packed(Color color);
ImVec4 rgba(Color color);
void apply(bool high_contrast);
void apply_text_scale(int viewport_width);
bool button(const char* label, const ImVec2& size = {});
bool small_button(const char* label);

} // namespace wave::theme
