#include "server_status_view.h"

#include "panel_controls_view.h"
#include "server_status_plot.h"
#include "view_helpers.h"

#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using theme::packed;

constexpr float gutter = 12;
constexpr ImVec2 card_padding{10, 8};
constexpr float minimum_plot_height = 134;

using InfoRows = std::array<std::pair<const char*, std::string>, 10>;

double current_sample(const Json& server, std::string_view key, const Json& history)
{
    return numeric_field(server, key, history.empty() ? 0 : number(history.back()));
}

struct StatusSamples {
    const Json& cpu;
    const Json& memory;
    const Json& upload;
    const Json& download;
    double cpu_now;
    double memory_now;
    double memory_capacity;
    double upload_now;
    double download_now;

    explicit StatusSamples(const Json& server)
        : cpu(array_field(server, "cpu_history")), memory(array_field(server, "memory_history")),
          upload(array_field(server, "upload_history")), download(array_field(server, "download_history")),
          cpu_now(current_sample(server, "cpu_percent", cpu)),
          memory_now(current_sample(server, "memory_used_gib", memory)),
          memory_capacity(numeric_field(server, "memory_total_gib")),
          upload_now(current_sample(server, "upload_rate", upload)),
          download_now(current_sample(server, "download_rate", download))
    {
    }

    std::string cpu_value() const
    {
        return std::format("{:.1f}%", cpu_now);
    }
    std::string memory_value() const
    {
        return memory_capacity > 0 ? std::format("{:.1f} / {:.1f} GiB", memory_now, memory_capacity)
                                   : std::format("{:.1f} GiB", memory_now);
    }
    std::string upload_value() const
    {
        return std::format("Upload {:.1f} MiB/s", upload_now);
    }
    std::string download_value() const
    {
        return std::format("Download {:.1f} MiB/s", download_now);
    }
};

double largest_sample(const Json& samples, double peak)
{
    const auto first = samples.size() - std::min(samples.size(), status_history_capacity);
    for (std::size_t i = first; i < samples.size(); ++i) {
        peak = std::max(peak, number(samples[i]));
    }
    return peak;
}

double traffic_ceiling(const StatusSamples& samples)
{
    const double current_peak = std::max(samples.upload_now, samples.download_now);
    const double peak = largest_sample(samples.download, largest_sample(samples.upload, current_peak));
    return std::max(4.0, 4.0 * std::ceil(peak / 4.0));
}

InfoRows server_information(const Json& server, const StatusSamples& samples)
{
    const int uptime = integer_field(server, "uptime_minutes");
    return {
        {{"Name", std::string(string_field(server, "name", "Wave sample"))},
         {"Region", std::string(string_field(server, "region", "Preview"))},
         {"Platform", std::string(string_field(server, "platform", "Demo"))},
         {"Panel version", std::string(string_field(server, "version", "Unavailable"))},
         {"Uptime", std::format("{}d {}h {}m", uptime / 1440, uptime / 60 % 24, uptime % 60)},
         {"CPU cores", std::to_string(integer_field(server, "cpu_cores", 4))},
         {"Memory", samples.memory_value()},
         {"Users", std::format("{} total, {} enabled, {} online", integer_field(server, "users_total"),
                               integer_field(server, "users_enabled"),
                               integer_field(server, "users_online", integer_field(server, "users_active")))},
         {"Total upload", std::format("{:.1f} GiB", numeric_field(server, "upload_total"))},
         {"Total download", std::format("{:.1f} GiB", numeric_field(server, "download_total"))}}};
}

float text_width(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

float wrapped_height(std::string_view text, float width)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, width).y;
}

void draw_text(ImDrawList& draw, ImVec2 pos, Color color, std::string_view text, float wrap_width = 0)
{
    draw.AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, packed(color), text.data(),
                 text.data() + text.size(), wrap_width);
}

struct InformationLayout {
    float label_width;
    float value_x;
    float value_width;
    std::array<float, std::tuple_size_v<InfoRows>> rows;
    float height = 0;

    InformationLayout(const InfoRows& info, float width)
    {
        float labels = 0;
        for (const auto& [label, value] : info) {
            labels = std::max(labels, text_width(label));
        }
        label_width = std::max(1.0f, std::min(labels + 1, std::floor(width * 0.46f)));
        value_x = label_width + gutter;
        value_width = std::max(1.0f, width - value_x);
        for (std::size_t i = 0; i < info.size(); ++i) {
            const auto& [label, value] = info[i];
            rows[i] =
                std::ceil(std::max(wrapped_height(label, label_width), wrapped_height(value, value_width))) +
                6;
            height += rows[i];
        }
    }
};

void draw_information(const InfoRows& info, const InformationLayout& layout)
{
    auto& draw = *ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy({layout.value_x + layout.value_width, layout.height});
    float y = start.y;
    for (std::size_t i = 0; i < info.size(); ++i) {
        const auto& [label, value] = info[i];
        if (i != 0) {
            draw.AddLineH(start.x, start.x + layout.value_x + layout.value_width, y, packed(Color::divider),
                          theme::border_width);
        }
        draw_text(draw, {start.x, y + 3}, Color::secondary_text, label, layout.label_width);
        draw_text(draw, {start.x + layout.value_x, y + 3}, Color::primary_text, value, layout.value_width);
        y += layout.rows[i];
    }
}

float title_band_height()
{
    return ImGui::GetFontSize() + 10;
}

bool begin_card(const char* title, ImVec2 pos, ImVec2 size)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, card_padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::pane_radius);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgba(Color::pane));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgba(Color::divider));
    const bool visible =
        ImGui::BeginChild(title, size, ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (visible) {
        auto& draw = *ImGui::GetWindowDrawList();
        const float band = title_band_height();
        const auto child = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowWidth();
        draw.AddRectFilled({child.x + 1, child.y + 1}, {child.x + width - 1, child.y + band},
                           packed(Color::raised), theme::pane_radius - 1, ImDrawFlags_RoundCornersTop);
        draw.AddLineH(child.x + 1, child.x + width - 1, child.y + band, packed(Color::divider),
                      theme::border_width);
        draw_text(draw, {child.x + std::floor((width - text_width(title)) * 0.5f), child.y + 5},
                  Color::primary_text, title);
        ImGui::SetCursorScreenPos({child.x + card_padding.x, child.y + band + card_padding.y});
    }
    return visible;
}

void chart_card(const char* title, ImVec2 pos, ImVec2 size, const StatusTicks& ticks, float left_gutter,
                const char* unit, double ceiling, std::initializer_list<StatusSeries> series)
{
    if (begin_card(title, pos, size)) {
        render_status_plot(ticks, left_gutter, unit, ceiling,
                           std::max(minimum_plot_height, ImGui::GetContentRegionAvail().y), series);
    }
    ImGui::EndChild();
}

} // namespace

bool server_status_has_columns(float content_width, float font_size)
{
    return content_width >= 2 * 320 * font_size / theme::secondary_font_size + gutter;
}

float render_server_status(const Json& server, PanelControlsState& controls, PreviewSession& session,
                           bool stacked)
{
    const StatusSamples samples(server);
    const double memory_ceiling = samples.memory_capacity > 0
                                      ? samples.memory_capacity
                                      : largest_sample(samples.memory, std::max(1.0, samples.memory_now));
    const double traffic_maximum = traffic_ceiling(samples);
    const auto cpu_ticks = status_ticks(100);
    const auto memory_ticks = status_ticks(memory_ceiling);
    const auto traffic_ticks = status_ticks(traffic_maximum);
    const float axis_gutter = std::max(
        {status_axis_gutter(cpu_ticks), status_axis_gutter(memory_ticks), status_axis_gutter(traffic_ticks)});
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float left_width = stacked ? available.x : std::floor((available.x - gutter) * 0.5f);
    const float right_width = stacked ? left_width : available.x - left_width - gutter;
    const float information_width = stacked ? left_width : std::floor((left_width - gutter) * 0.5f);
    const float controls_width = stacked ? left_width : left_width - information_width - gutter;
    const float information_body = std::max(1.0f, information_width - 2 * card_padding.x);
    const float controls_body = std::max(1.0f, controls_width - 2 * card_padding.x);
    const auto info = server_information(server, samples);
    const InformationLayout information(info, information_body);
    const float card_frame_height = title_band_height() + 2 * card_padding.y;
    const float chart_minimum_height = card_frame_height + minimum_plot_height;
    float cpu_height = chart_minimum_height;
    float memory_height = chart_minimum_height;
    float traffic_height = chart_minimum_height;
    float info_height = card_frame_height + information.height;
    float controls_height =
        card_frame_height + panel_controls_height(controls, session, controls_body, !stacked);
    if (!stacked) {
        const float top_minimum = chart_minimum_height;
        const float bottom_minimum = std::max({info_height, controls_height, traffic_height});
        const float rows_height = std::max(available.y - gutter, top_minimum + bottom_minimum);
        cpu_height = memory_height =
            std::clamp(std::floor(rows_height * 0.5f), top_minimum, rows_height - bottom_minimum);
        info_height = controls_height = traffic_height = rows_height - cpu_height;
    }

    const float right_x = stacked ? start.x : start.x + left_width + gutter;
    const float memory_y = stacked ? start.y + cpu_height + gutter : start.y;
    const float info_y = memory_y + memory_height + gutter;
    const float controls_x = stacked ? start.x : start.x + information_width + gutter;
    const float controls_y = stacked ? info_y + info_height + gutter : info_y;
    const float traffic_y = stacked ? controls_y + controls_height + gutter : info_y;
    chart_card("CPU", start, {left_width, cpu_height}, cpu_ticks, axis_gutter, "%", 100,
               {{samples.cpu, "CPU", Color::accent, samples.cpu_value()}});
    chart_card("Memory", {right_x, memory_y}, {right_width, memory_height}, memory_ticks, axis_gutter, "GiB",
               memory_ceiling, {{samples.memory, "Memory", Color::info, samples.memory_value()}});
    if (begin_card("Server Information", {start.x, info_y}, {information_width, info_height})) {
        draw_information(info, information);
    }
    ImGui::EndChild();
    if (begin_card("Panel Controls", {controls_x, controls_y}, {controls_width, controls_height})) {
        render_panel_controls(controls, session, controls_body, !stacked);
    }
    ImGui::EndChild();
    chart_card("Traffic", {right_x, traffic_y}, {right_width, traffic_height}, traffic_ticks, axis_gutter,
               "MiB/s", traffic_maximum,
               {{samples.upload, "Upload", Color::info, samples.upload_value()},
                {samples.download, "Download", Color::accent, samples.download_value()}});
    return traffic_y + traffic_height - start.y;
}

std::string describe_server_status(const Json& server)
{
    const StatusSamples samples(server);
    return std::format("CPU {}. Memory {}. {}. {}.", samples.cpu_value(), samples.memory_value(),
                       samples.upload_value(), samples.download_value());
}

} // namespace wave
