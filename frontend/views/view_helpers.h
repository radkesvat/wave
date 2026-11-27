#pragma once

#include "theme.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace wave::view {

using Json = nlohmann::json;

inline const Json& field(const Json& object, std::string_view key)
{
    static const Json empty;
    const auto it = object.find(key);
    return it == object.end() ? empty : *it;
}

inline std::string_view string_value(const Json& value, std::string_view fallback = "")
{
    return value.is_string() ? value.get_ref<const std::string&>() : fallback;
}

inline std::string_view string_field(const Json& object, std::string_view key, std::string_view fallback = "")
{
    return string_value(field(object, key), fallback);
}

inline double number(const Json& value, double fallback = 0)
{
    if (value.is_number()) {
        const double result = value.get<double>();
        if (std::isfinite(result)) {
            return result;
        }
    }
    return fallback;
}

inline double numeric_field(const Json& object, std::string_view key, double fallback = 0)
{
    return number(field(object, key), fallback);
}

inline int integer_field(const Json& object, std::string_view key, int fallback = 0)
{
    return static_cast<int>(std::clamp(numeric_field(object, key, fallback), -1.0e9, 1.0e9));
}

inline bool bool_field(const Json& object, std::string_view key, bool fallback = false)
{
    const auto& value = field(object, key);
    return value.is_boolean() ? value.get<bool>() : fallback;
}

inline const Json& array_field(const Json& object, std::string_view key)
{
    static const Json empty = Json::array();
    const auto& value = field(object, key);
    return value.is_array() ? value : empty;
}

inline float button_width(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x;
}

inline void text(std::string_view value)
{
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

inline void wrapped(std::string_view value)
{
    ImGui::PushTextWrapPos();
    text(value);
    ImGui::PopTextWrapPos();
}

inline void wrapped(std::string_view value, theme::Color color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(color));
    wrapped(value);
    ImGui::PopStyleColor();
}

namespace detail {

inline int help_frame = -1;
inline std::string help_description;

} // namespace detail

inline std::string_view visible_help_description()
{
    return detail::help_frame == ImGui::GetFrameCount() ? std::string_view(detail::help_description)
                                                        : std::string_view{};
}

inline constexpr char help_marker_label[] = "(?)";

inline void help_marker(std::string_view description, bool same_line = true)
{
    if (description.empty()) {
        return;
    }

    constexpr char popup[] = "##help-details";
    const auto& style = ImGui::GetStyle();
    const auto size = ImGui::CalcTextSize(help_marker_label);
    const auto right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (same_line && ImGui::GetItemRectMax().x + style.ItemInnerSpacing.x + size.x <= right) {
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
    }
    ImGui::PushID(description.data(), description.data() + description.size());
    const bool pressed = ImGui::InvisibleButton("##help", size, ImGuiButtonFlags_EnableNav);
    const bool visible = ImGui::IsItemVisible();
    const bool focused = visible && ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
    const bool hovered = visible && ImGui::IsItemHovered();
    const bool show_tooltip = visible && (focused || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip));
    if (visible) {
        const auto color = hovered || focused ? theme::Color::primary_text : theme::Color::secondary_text;
        ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), ImGui::GetColorU32(theme::packed(color)),
                                            help_marker_label);
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    const auto draw_help = [&] {
        const float width = std::clamp(ImGui::GetIO().DisplaySize.x - 40, 1.0f, 360.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        text(description);
        ImGui::PopTextWrapPos();
        detail::help_description = description;
        detail::help_frame = ImGui::GetFrameCount();
    };
    if (pressed) {
        ImGui::OpenPopup(popup);
    }
    if (ImGui::BeginPopup(popup)) {
        draw_help();
        ImGui::EndPopup();
    } else if (show_tooltip && ImGui::BeginTooltip()) {
        draw_help();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

template <std::size_t N> inline void fill(std::array<char, N>& buffer, std::string_view value)
{
    const auto length = std::min(value.size(), buffer.size() - 1);
    std::copy_n(value.begin(), length, buffer.begin());
    buffer[length] = '\0';
}

template <std::size_t N>
inline bool edit(const char* label, std::array<char, N>& buffer, const char* hint = "",
                 ImGuiInputTextFlags flags = 0, std::string_view help = {})
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    help_marker(help);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputTextWithHint("##value", hint, buffer.data(), buffer.size(), flags);
    ImGui::PopID();
    return changed;
}

inline bool edit_number(const char* label, int& value)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputInt("##value", &value, 0);
    ImGui::PopID();
    return changed;
}

} // namespace wave::view
