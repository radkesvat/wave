#include "server_status_plot.h"

#include "view_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string_view>

namespace wave {
namespace {

using theme::Color;
using theme::packed;

constexpr float edge_padding = 10;
constexpr float tick_gap = 6;

double positive_ceiling(double ceiling)
{
    return std::isfinite(ceiling) && ceiling > 0 ? ceiling : 1;
}

std::string tick_label(double value)
{
    if (value != 0 && (value < 0.001 || value >= 100000)) {
        return std::format("{:.3g}", value);
    }
    auto label = std::format("{:.4f}", value);
    label.erase(label.find_last_not_of('0') + 1);
    if (label.back() == '.') {
        label.pop_back();
    }
    return label;
}

std::size_t sample_count(const StatusSeries& line)
{
    return line.samples.is_array() ? std::min(line.samples.size(), status_history_capacity) : 0;
}

} // namespace

StatusTicks status_ticks(double ceiling)
{
    StatusTicks ticks;
    ceiling = positive_ceiling(ceiling);
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        const double fraction = static_cast<double>(index) / (ticks.size() - 1);
        ticks[index] = tick_label(ceiling * fraction);
    }
    return ticks;
}

float status_axis_gutter(const StatusTicks& ticks)
{
    ImGui::PushFont(nullptr, theme::annotation_font_size);
    float width = 0;
    for (const auto& label : ticks) {
        width = std::max(width, ImGui::CalcTextSize(label.c_str()).x);
    }
    ImGui::PopFont();
    return edge_padding + width + tick_gap;
}

void render_status_plot(const StatusTicks& ticks, float left_gutter, const char* unit, double ceiling,
                        float height, std::initializer_list<StatusSeries> series)
{
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 corner{std::round(position.x), std::round(position.y)};
    const ImVec2 end{std::round(position.x + width), std::round(position.y + height)};
    ImGui::Dummy({width, height});
    if (!ImGui::IsItemVisible()) {
        return;
    }

    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, end, packed(Color::inset));
    draw.AddRect(corner, end, packed(Color::divider), theme::square_radius, theme::border_width);

    const float left = corner.x + left_gutter;
    const float right = end.x - edge_padding;
    ImGui::PushFont(nullptr, theme::annotation_font_size);
    const float font_size = ImGui::GetFontSize();
    const float top = corner.y + std::ceil(font_size * 0.5f) + 4;
    const float bottom = end.y - font_size - 12;
    if (right <= left || bottom <= top) {
        ImGui::PopFont();
        return;
    }

    const auto grid = packed(Color::plot_grid);
    const auto label_color = packed(Color::secondary_text);
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        const float fraction = static_cast<float>(index) / (ticks.size() - 1);
        const float y = std::round(bottom - (bottom - top) * fraction);
        const float x = std::round(left + (right - left) * fraction);
        draw.AddLineH(left, right, y, grid, theme::border_width);
        draw.AddLineV(x, top, bottom, grid, theme::border_width);
        const auto* label = ticks[index].c_str();
        draw.AddText({left - ImGui::CalcTextSize(label).x - tick_gap, std::floor(y - font_size * 0.5f)},
                     label_color, label);
    }
    const float now_x = right - ImGui::CalcTextSize("Now").x;
    constexpr char recent[] = "Recent samples";
    const float recent_width = ImGui::CalcTextSize(recent).x;
    const float recent_x = std::max(corner.x + edge_padding, std::min(left, now_x - tick_gap - recent_width));
    if (recent_x + recent_width + tick_gap <= now_x) {
        draw.AddText({recent_x, bottom + 6}, label_color, recent);
        draw.AddText({now_x, bottom + 6}, label_color, "Now");
    }
    ImGui::PopFont();

    std::size_t count = 0;
    for (const auto& line : series) {
        count = std::max(count, sample_count(line));
    }
    if (count == 0) {
        return;
    }
    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered =
        ImGui::IsItemHovered() && mouse.x >= left && mouse.x <= right && mouse.y >= top && mouse.y <= bottom;
    // Index from the newest sample, so channels with fewer samples still share the same time axis.
    const float fraction = hovered ? (right - mouse.x) / (right - left) : 0;
    const auto age = static_cast<std::size_t>(std::lround(fraction * (count - 1)));
    const float step = (right - left) / std::max<std::size_t>(1, count - 1);
    if (hovered) {
        draw.AddLineV(std::round(right - step * age), top, bottom, label_color, theme::border_width);
    }

    ceiling = positive_ceiling(ceiling);
    for (const auto& line : series) {
        const auto size = sample_count(line);
        std::array<ImVec2, status_history_capacity> points;
        for (std::size_t index = 0; index < size; ++index) {
            const auto raw = view::number(line.samples[line.samples.size() - size + index]);
            const auto ratio = static_cast<float>(std::clamp(raw / ceiling, 0.0, 1.0));
            points[index] = {right - step * (size - 1 - index), bottom - (bottom - top) * ratio};
        }
        const auto color = packed(line.color);
        if (size > 1) {
            draw.AddPolyline(points.data(), static_cast<int>(size), color, 2);
        }
        if (age < size) {
            draw.AddCircleFilled(points[size - 1 - age], hovered ? 4 : 3, color);
        }
    }
    if (hovered && ImGui::BeginTooltip()) {
        const char* spacing = std::string_view(unit) == "%" ? "" : " ";
        for (const auto& line : series) {
            if (age < sample_count(line)) {
                const auto sample = view::number(line.samples[line.samples.size() - 1 - age]);
                ImGui::Text("%s %.1f%s%s", line.name, sample, spacing, unit);
            }
        }
        ImGui::EndTooltip();
    }
}

} // namespace wave
