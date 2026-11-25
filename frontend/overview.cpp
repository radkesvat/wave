#include "overview.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <optional>
#include <string_view>

namespace wave {
namespace {

constexpr ImU32 surface = IM_COL32(17, 32, 44, 255);
constexpr ImU32 border = IM_COL32(38, 57, 72, 255);
constexpr ImU32 foreground = IM_COL32(234, 243, 247, 255);
constexpr ImU32 muted = IM_COL32(158, 181, 197, 255);
constexpr ImU32 accent = IM_COL32(104, 229, 195, 255);
constexpr ImU32 accent_surface = IM_COL32(23, 53, 56, 255);
constexpr ImU32 amber = IM_COL32(246, 192, 119, 255);
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
        columns = std::clamp(static_cast<int>((width + gap) / (minimum_card_width + gap)),
                             1, metric_count);
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
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, label.data(),
                                          label.data() + label.size()).x;
}

void panel(ImDrawList& draw, ImVec2 min, ImVec2 max)
{
    draw.AddRectFilled(min, max, surface, 14.0f);
    draw.AddRect(min, max, border, 14.0f);
}

void draw_intro(ImDrawList& draw, const OverviewLayout& layout)
{
    const float x = layout.left;
    const float y = layout.intro_y;
    text(draw, {x, y}, layout.compact ? 45.0f : 54.0f, foreground, "Overview");

    const float status_y = y + (layout.compact ? 58.0f : 68.0f);
    draw.AddCircleFilled({x + 5, status_y + 10}, 4.0f, amber);
    text(draw, {x + 17, status_y}, layout.width < 280.0f ? 15.0f : 17.0f, muted, status,
         std::max(1.0f, layout.width - 17.0f));

    const float pill_width = text_width(15.0f, sample_label) + 38.0f;
    const float pill_x = layout.compact ? x : x + layout.width - pill_width;
    const float pill_y = layout.compact ? layout.metrics_y - 47.0f : status_y - 6.0f;
    draw.AddRectFilled({pill_x, pill_y}, {pill_x + pill_width, pill_y + 31}, accent_surface, 15.0f);
    draw.AddCircleFilled({pill_x + 13, pill_y + 15.5f}, 3.0f, accent);
    text(draw, {pill_x + 24, pill_y + 6}, 15.0f, accent, sample_label);
}

enum class Metric { cpu, memory, uptime };

void draw_metric_icon(ImDrawList& draw, Metric metric, float x, float y)
{
    draw.AddRectFilled({x - 17, y - 17}, {x + 17, y + 17}, accent_surface, 9.0f);
    switch (metric) {
    case Metric::cpu:
        draw.AddRect({x - 6, y - 6}, {x + 6, y + 6}, accent, 2.0f);
        draw.AddRectFilled({x - 2, y - 2}, {x + 2, y + 2}, accent, 1.0f);
        for (const float offset : {-3.0f, 3.0f}) {
            draw.AddLine({x + offset, y - 9}, {x + offset, y - 6}, accent);
            draw.AddLine({x + offset, y + 6}, {x + offset, y + 9}, accent);
            draw.AddLine({x - 9, y + offset}, {x - 6, y + offset}, accent);
            draw.AddLine({x + 6, y + offset}, {x + 9, y + offset}, accent);
        }
        break;
    case Metric::memory:
        draw.AddRect({x - 9, y - 5}, {x + 9, y + 5}, accent, 2.0f);
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

void draw_metric(ImDrawList& draw, const OverviewLayout& layout, Metric metric,
                 std::string_view label, std::string_view value,
                 std::optional<float> fraction = std::nullopt)
{
    const int index = static_cast<int>(metric);
    const int row = index / layout.columns;
    const int column = index % layout.columns;
    const int columns = std::min(layout.columns, metric_count - row * layout.columns);
    const float width = (layout.width - gap * (columns - 1)) / columns;
    const float x = layout.left + column * (width + gap);
    const float y = layout.metrics_y + row * (layout.card_height + gap);

    panel(draw, {x, y}, {x + width, y + layout.card_height});
    draw_metric_icon(draw, metric, x + width - 35, y + 33);
    text(draw, {x + 20, y + 22}, 17.0f, muted, label);
    text(draw, {x + 20, y + 48}, layout.compact ? 31.0f : 33.0f, foreground, value);

    if (fraction) {
        const float bar_y = y + layout.card_height - 17.0f;
        const float bar_width = width - 40.0f;
        const float filled = bar_width * std::clamp(*fraction, 0.0f, 1.0f);
        draw.AddRectFilled({x + 20, bar_y}, {x + 20 + bar_width, bar_y + 4}, border, 2.0f);
        draw.AddRectFilled({x + 20, bar_y}, {x + 20 + filled, bar_y + 4}, accent, 2.0f);
    }
}

void draw_traffic(ImDrawList& draw, const OverviewLayout& layout, const OverviewData& data)
{
    const auto& samples = data.download_mib_per_second;
    const float x = layout.left;
    const float y = layout.chart_y;
    const float right_edge = x + layout.width;
    const float bottom_edge = y + layout.chart_height;

    panel(draw, {x, y}, {right_edge, bottom_edge});
    text(draw, {x + 20, y + 19}, layout.compact ? 21.0f : 24.0f, foreground, "Download traffic");
    text(draw, {x + 20, y + 50}, 15.0f, muted, "Recent samples");
    const auto latest = std::format("{:.1f} MiB/s", samples.back());
    const float latest_y = y + (layout.width < 260.0f ? 48.0f : 23.0f);
    text(draw, {right_edge - 20 - text_width(19.0f, latest), latest_y}, 19.0f, accent, latest);

    const float plot_left = x + 50.0f;
    const float plot_right = right_edge - 24.0f;
    const float plot_top = y + 97.0f;
    const float plot_bottom = bottom_edge - 46.0f;
    constexpr int intervals = 4;
    const double maximum = *std::max_element(samples.begin(), samples.end());
    const double step = std::max(1.0, std::ceil(maximum / intervals));
    const double peak = intervals * step;

    text(draw, {x + 20, y + 73}, 13.0f, muted, "MiB/s");
    for (int i = 0; i <= intervals; ++i) {
        const float line_y = plot_bottom - (plot_bottom - plot_top) * i / intervals;
        const double tick = i * step;
        const auto label = tick < 10000.0 ? std::format("{:.0f}", tick) : std::format("{:.0e}", tick);
        text(draw, {plot_left - 10 - text_width(14.0f, label), line_y - 8}, 14.0f, muted, label);
        draw.AddLine({plot_left, line_y}, {plot_right, line_y}, border);
    }

    std::array<ImVec2, std::tuple_size_v<decltype(data.download_mib_per_second)> + 2> points;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float fraction = static_cast<float>(samples[i] / peak);
        points[i] = {plot_left + (plot_right - plot_left) * i / (samples.size() - 1),
                     plot_bottom - (plot_bottom - plot_top) * fraction};
    }
    points[samples.size()] = {plot_right, plot_bottom};
    points[samples.size() + 1] = {plot_left, plot_bottom};
    if (maximum > 0.0) {
        draw.AddConcavePolyFilled(points.data(), static_cast<int>(points.size()), IM_COL32(104, 229, 195, 31));
    }
    draw.AddPolyline(points.data(), static_cast<int>(samples.size()), accent, 2.3f);
    draw.AddCircleFilled(points[samples.size() - 1], 4.0f, accent);

    constexpr std::string_view end = "Now";
    text(draw, {plot_left, plot_bottom + 12}, 14.0f, muted, "Earlier");
    text(draw, {plot_right - text_width(14.0f, end), plot_bottom + 12}, 14.0f, muted, end);
}

} // namespace

float overview_height(float content_width, bool has_data)
{
    const OverviewLayout layout(content_width);
    const float bottom = has_data ? layout.chart_y + layout.chart_height
                                  : layout.metrics_y + empty_card_height;
    return bottom + 32.0f;
}

void render_overview(ImDrawList& draw, float x, float y, float width,
                     const OverviewData* data, bool loading)
{
    const OverviewLayout layout(width, x, y);
    draw_intro(draw, layout);
    if (!data) {
        panel(draw, {layout.left, layout.metrics_y},
              {layout.left + layout.width, layout.metrics_y + empty_card_height});
        text(draw, {layout.left + 20, layout.metrics_y + 30}, 19.0f, foreground,
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
    const auto [low, high] = std::minmax_element(data->download_mib_per_second.begin(),
                                                data->download_mib_per_second.end());
    return std::format("Wave Overview. {}. {}. CPU utilization {:.0f} percent. Memory {:.1f} of {:.1f} "
                       "GiB. Uptime {} day{}, {} hour{}, {} minute{}. Recent download traffic ranged "
                       "from {:.1f} to {:.1f} MiB per second. Latest {:.1f} MiB per second.",
                       status, sample_label, data->cpu_percent, data->memory_used_gib,
                       data->memory_total_gib, days, days == 1 ? "" : "s", hours,
                       hours == 1 ? "" : "s", minutes, minutes == 1 ? "" : "s",
                       *low, *high, data->download_mib_per_second.back());
}

} // namespace wave
