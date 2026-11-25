#pragma once

#include "preview_session.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace wave::view {

using Json = nlohmann::json;

inline constexpr ImVec4 mint{0.40f, 0.90f, 0.77f, 1.0f};
inline constexpr ImVec4 warm{0.98f, 0.74f, 0.47f, 1.0f};
inline constexpr ImU32 plot_surface = IM_COL32(14, 25, 36, 255);
inline constexpr ImU32 plot_grid = IM_COL32(38, 57, 72, 255);
inline constexpr ImU32 plot_muted = IM_COL32(161, 184, 197, 255);
inline constexpr ImU32 plot_mint = IM_COL32(104, 229, 195, 255);
inline constexpr ImU32 plot_blue = IM_COL32(121, 175, 244, 255);
inline constexpr ImU32 plot_amber = IM_COL32(246, 192, 119, 255);

inline const Json& field(const Json& object, std::string_view key)
{
    static const Json empty;
    const auto it = object.find(key);
    return it == object.end() ? empty : *it;
}

inline std::string_view string_value(const Json& value, std::string_view fallback = {})
{
    return value.is_string() ? value.get_ref<const std::string&>() : fallback;
}

inline std::string_view string_field(const Json& object, std::string_view key, std::string_view fallback = {})
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

template <std::size_t N>
inline void fill(std::array<char, N>& buffer, std::string_view value)
{
    const auto length = std::min(value.size(), buffer.size() - 1);
    std::copy_n(value.begin(), length, buffer.begin());
    buffer[length] = '\0';
}

template <std::size_t N>
inline bool edit(const char* label, std::array<char, N>& buffer, const char* hint = "",
          ImGuiInputTextFlags flags = 0)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
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

inline void feedback(const PreviewSession& session, std::initializer_list<std::string_view> actions)
{
    const PreviewFeedback* latest = nullptr;
    for (const auto action : actions) {
        const auto* result = session.feedback_for(action);
        if (result && (!latest || result->revision > latest->revision)) {
            latest = result;
        }
    }
    if (latest && !latest->message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, latest->ok ? mint : warm);
        wrapped(latest->message);
        ImGui::PopStyleColor();
    }
}

inline void feedback(const PreviewReply& reply)
{
    if (!reply.message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, reply.ok() ? mint : warm);
        wrapped(reply.message);
        ImGui::PopStyleColor();
    }
}

} // namespace wave::view
