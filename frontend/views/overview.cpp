#include "overview.h"

#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <optional>
#include <string_view>

namespace wave {
namespace {

using theme::Color;
using theme::packed;

constexpr std::string_view status = "No WaterWall instance connected";
constexpr std::string_view sample_label = "Sample data";
constexpr std::string_view sample_loading = "Loading sample data";
constexpr std::string_view sample_unavailable = "Sample data unavailable";

constexpr int metric_count = 3;
constexpr float gap = 16.0f;
constexpr float empty_card_height = 88.0f;

struct OverviewLayout {
    float left;
    float width;
    int columns;
    bool compact;
    bool wrap_status;
    float intro_y;
    float metrics_y;
    float card_height;
    float chart_y;
    float chart_height;

    OverviewLayout(float content_width, float x = 0.0f, float y = 0.0f)
    {
        const float margin = std::min(32.0f, content_width * 0.06f);
        width = std::min(1080.0f, content_width - 2.0f * margin);
        left = x + (content_width - width) / 2.0f;
        constexpr float minimum_card_width = 204.0f;
        columns = std::clamp(static_cast<int>((width + gap) / (minimum_card_width + gap)), 1, metric_count);
        compact = columns < metric_count;
        wrap_status = width < 250.0f;
        intro_y = y + (compact ? 30.0f : 46.0f);
        metrics_y = intro_y + (compact ? 146.0f : 124.0f) + (wrap_status ? 18.0f : 0.0f);
        card_height = compact ? 106.0f : 132.0f;
        const int rows = (metric_count + columns - 1) / columns;
        chart_y = metrics_y + rows * (card_height + gap) + 4.0f;
        chart_height = compact ? 290.0f : 322.0f;
    }
};

void text(ImDrawList& draw, ImVec2 position, float size, ImU32 color, std::string_view label,
          float wrap_width = 0.0f)
{
    draw.AddText(ImGui::GetFont(), size, position, color, label.data(), label.data() + label.size(),
                 wrap_width);
}

float text_width(float size, std::string_view label)
{
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, label.data(), label.data() + label.size()).x;
}

void panel(ImDrawList& draw, ImVec2 min, ImVec2 max, float rounding = theme::pane_radius)
{
    min = {std::round(min.x), std::round(min.y)};
    max = {std::round(max.x), std::round(max.y)};
    draw.AddRectFilled(min, max, packed(Color::toolbar), rounding);
    draw.AddRect(min, max, packed(Color::divider), rounding, theme::border_width);
}

void draw_intro(ImDrawList& draw, const OverviewLayout& layout)
{
    const float x = layout.left;
    const float y = layout.intro_y;
    text(draw, {x, y}, theme::large_title_font_size, packed(Color::primary_text), "Overview");

    const float status_y = y + theme::large_title_font_size + 14;
    draw.AddCircleFilled({x + 5, status_y + 10}, 4.0f, packed(Color::warning));
    text(draw, {x + 17, status_y}, theme::secondary_font_size, packed(Color::secondary_text), status,
         std::max(1.0f, layout.width - 17.0f));

    const float label_width = std::ceil(text_width(theme::secondary_font_size, sample_label)) + 38.0f;
    const float label_x = std::round(layout.compact ? x : x + layout.width - label_width);
    const float label_y = std::round(layout.compact ? layout.metrics_y - 47.0f : status_y - 6.0f);
    panel(draw, {label_x, label_y}, {label_x + label_width, label_y + 31}, theme::control_radius);
    draw.AddCircleFilled({label_x + 13, label_y + 15.5f}, 3.0f, packed(Color::accent));
    text(draw, {label_x + 24, label_y + 6}, theme::secondary_font_size, packed(Color::primary_text),
         sample_label);
}

enum class Metric {
    cpu,
    memory,
    uptime
};

void draw_metric_icon(ImDrawList& draw, Metric metric, float x, float y)
{
    const ImU32 accent = packed(Color::accent);
    draw.AddRectFilled({x - 17, y - 17}, {x + 17, y + 17}, packed(Color::raised), theme::control_radius);
    switch (metric) {
    case Metric::cpu:
        draw.AddRect({x - 6, y - 6}, {x + 6, y + 6}, accent);
        draw.AddRectFilled({x - 2, y - 2}, {x + 2, y + 2}, accent);
        for (const float offset : {-3.0f, 3.0f}) {
            draw.AddLine({x + offset, y - 9}, {x + offset, y - 6}, accent);
            draw.AddLine({x + offset, y + 6}, {x + offset, y + 9}, accent);
            draw.AddLine({x - 9, y + offset}, {x - 6, y + offset}, accent);
            draw.AddLine({x + 6, y + offset}, {x + 9, y + offset}, accent);
        }
        break;
    case Metric::memory:
        draw.AddRect({x - 9, y - 5}, {x + 9, y + 5}, accent);
        for (const float offset : {-5.0f, 0.0f, 5.0f}) {
            draw.AddRectFilled({x + offset - 1, y - 2}, {x + offset + 1, y + 2}, accent);
            draw.AddLine({x + offset, y + 5}, {x + offset, y + 8}, accent);
        }
        break;
    case Metric::uptime:
        draw.AddCircle({x, y}, 8.0f, accent);
        draw.AddLine({x, y - 5}, {x, y}, accent, 1.5f);
        draw.AddLine({x, y}, {x + 4, y + 2}, accent, 1.5f);
        break;
    }
}

void draw_metric(ImDrawList& draw, const OverviewLayout& layout, Metric metric, std::string_view label,
                 std::string_view value, std::optional<float> fraction = std::nullopt)
{
    const int index = static_cast<int>(metric);
    const int row = index / layout.columns;
    const int column = index % layout.columns;
    const int columns = std::min(layout.columns, metric_count - row * layout.columns);
    const float width = (layout.width - gap * (columns - 1)) / columns;
    const float x = layout.left + column * (width + gap);
    const float y = layout.metrics_y + row * (layout.card_height + gap);

    panel(draw, {x, y}, {x + width, y + layout.card_height});
    draw_metric_icon(draw, metric, std::round(x + width - 35), std::round(y + 33));
    text(draw, {x + 20, y + 22}, theme::secondary_font_size, packed(Color::secondary_text), label);
    const float value_size = text_width(theme::large_title_font_size, value) > width - 40.0f
                                 ? theme::title_font_size
                                 : theme::large_title_font_size;
    text(draw, {x + 20, y + 48}, value_size, packed(Color::primary_text), value);

    if (fraction) {
        const float bar_y = std::round(y + layout.card_height - 17.0f);
        const float bar_left = std::round(x + 20.0f);
        const float bar_right = std::round(x + width - 20.0f);
        const float filled = std::round((bar_right - bar_left) * std::clamp(*fraction, 0.0f, 1.0f));
        draw.AddRectFilled({bar_left, bar_y}, {bar_right, bar_y + 4}, packed(Color::plot_grid));
        draw.AddRectFilled({bar_left, bar_y}, {bar_left + filled, bar_y + 4}, packed(Color::accent));
    }
}

void draw_traffic(ImDrawList& draw, const OverviewLayout& layout, const OverviewData& data)
{
    const auto& samples = data.download_mib_per_second;
    const float x = layout.left;
    const float y = layout.chart_y;
    const float right_edge = x + layout.width;
    const float bottom_edge = y + layout.chart_height;
    const ImU32 accent = packed(Color::accent);
    const ImU32 label_color = packed(Color::secondary_text);

    panel(draw, {x, y}, {right_edge, bottom_edge});
    text(draw, {x + 20, y + 19}, theme::title_font_size, packed(Color::primary_text), "Download traffic");
    text(draw, {x + 20, y + 50}, theme::secondary_font_size, label_color, "Recent samples");
    const auto latest = std::format("{:.1f} MiB/s", samples.back());
    const float latest_width = text_width(theme::secondary_font_size, latest);
    const float latest_y =
        y +
        (text_width(theme::title_font_size, "Download traffic") + latest_width + 50 > layout.width ? 72 : 23);
    text(draw, {right_edge - 20 - latest_width, latest_y}, theme::secondary_font_size, accent, latest);

    const float plot_left = std::round(x + 50.0f);
    const float plot_right = std::round(right_edge - 24.0f);
    const float plot_top = std::round(y + 97.0f);
    const float plot_bottom = std::round(bottom_edge - 46.0f);
    constexpr int intervals = 4;
    const double maximum = *std::max_element(samples.begin(), samples.end());
    const double step = std::max(1.0, std::ceil(maximum / intervals));
    const double peak = intervals * step;

    draw.AddRectFilled({plot_left, plot_top}, {plot_right, plot_bottom}, packed(Color::canvas));
    std::array<ImVec2, std::tuple_size_v<decltype(data.download_mib_per_second)> + 2> points;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float fraction = static_cast<float>(samples[i] / peak);
        points[i] = {plot_left + (plot_right - plot_left) * i / (samples.size() - 1),
                     plot_bottom - (plot_bottom - plot_top) * fraction};
    }
    points[samples.size()] = {plot_right, plot_bottom};
    points[samples.size() + 1] = {plot_left, plot_bottom};
    if (maximum > 0.0) {
        draw.AddConcavePolyFilled(points.data(), static_cast<int>(points.size()), packed(Color::chart_fill));
    }
    text(draw, {x + 20, y + 73}, theme::annotation_font_size, label_color, "MiB/s");
    for (int i = 0; i <= intervals; ++i) {
        const float line_y = std::round(plot_bottom - (plot_bottom - plot_top) * i / intervals);
        const double tick = i * step;
        const auto label = tick < 10000.0 ? std::format("{:.0f}", tick) : std::format("{:.0e}", tick);
        text(draw, {plot_left - 10 - text_width(theme::annotation_font_size, label), line_y - 8},
             theme::annotation_font_size, label_color, label);
        draw.AddLineH(plot_left, plot_right, line_y, packed(Color::plot_grid), theme::border_width);
    }
    draw.AddPolyline(points.data(), static_cast<int>(samples.size()), accent, 2.3f);
    draw.AddCircleFilled(points[samples.size() - 1], 4.0f, accent);

    constexpr std::string_view end = "Now";
    text(draw, {plot_left, plot_bottom + 12}, theme::annotation_font_size, label_color, "Earlier");
    text(draw, {plot_right - text_width(theme::annotation_font_size, end), plot_bottom + 12},
         theme::annotation_font_size, label_color, end);
}

float overview_height(float content_width, bool has_data)
{
    const OverviewLayout layout(content_width);
    const float bottom =
        has_data ? layout.chart_y + layout.chart_height : layout.metrics_y + empty_card_height;
    return bottom + 32.0f;
}

void draw_overview(ImDrawList& draw, float x, float y, float width, const OverviewData* data, bool loading)
{
    const OverviewLayout layout(width, x, y);
    draw_intro(draw, layout);
    if (!data) {
        panel(draw, {layout.left, layout.metrics_y},
              {layout.left + layout.width, layout.metrics_y + empty_card_height});
        text(draw, {layout.left + 20, layout.metrics_y + 30}, theme::secondary_font_size,
             packed(loading ? Color::primary_text : Color::warning),
             loading ? sample_loading : sample_unavailable, std::max(1.0f, layout.width - 40.0f));
        return;
    }

    draw_metric(draw, layout, Metric::cpu, "CPU utilization", std::format("{:.0f}%", data->cpu_percent),
                data->cpu_percent / 100.0f);
    draw_metric(draw, layout, Metric::memory, "Memory",
                std::format("{:.1f} / {:.1f} GiB", data->memory_used_gib, data->memory_total_gib),
                data->memory_total_gib > 0 ? data->memory_used_gib / data->memory_total_gib : 0.0f);
    const auto minutes = data->uptime.count();
    draw_metric(draw, layout, Metric::uptime, "Uptime",
                std::format("{}d {}h {}m", minutes / 1440, minutes / 60 % 24, minutes % 60));
    draw_traffic(draw, layout, *data);
}

} // namespace

void render_overview(const OverviewData* data, bool loading)
{
    const auto pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    draw_overview(*ImGui::GetWindowDrawList(), pos.x, pos.y, width, data, loading);
    ImGui::Dummy({width, overview_height(width, data != nullptr)});
}

std::string describe_overview(const OverviewData* data, bool loading)
{
    if (!data) {
        return std::format("Wave Overview. {}. {}. {}.", status, sample_label,
                           loading ? sample_loading : sample_unavailable);
    }

    const auto total_minutes = data->uptime.count();
    const auto days = total_minutes / 1440;
    const auto hours = total_minutes / 60 % 24;
    const auto minutes = total_minutes % 60;
    const auto [low, high] =
        std::minmax_element(data->download_mib_per_second.begin(), data->download_mib_per_second.end());
    return std::format("Wave Overview. {}. {}. CPU utilization {:.0f} percent. Memory {:.1f} of {:.1f} "
                       "GiB. Uptime {} day{}, {} hour{}, {} minute{}. Recent download traffic ranged "
                       "from {:.1f} to {:.1f} MiB per second. Latest {:.1f} MiB per second.",
                       status, sample_label, data->cpu_percent, data->memory_used_gib, data->memory_total_gib,
                       days, days == 1 ? "" : "s", hours, hours == 1 ? "" : "s", minutes,
                       minutes == 1 ? "" : "s", *low, *high, data->download_mib_per_second.back());
}

} // namespace wave
