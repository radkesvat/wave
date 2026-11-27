#include "server_status_view.h"

#include "panel_controls_view.h"
#include "server_status_plot.h"
#include "view_helpers.h"
#include "wave_version.h"

#include <array>
#include <format>
#include <optional>
#include <string_view>

namespace wave {
namespace {

using namespace view;
using theme::Color;
using theme::packed;

constexpr float gutter = 12;
constexpr ImVec2 card_padding{10, 8};
constexpr float minimum_plot_height = 134;
constexpr char unavailable[] = "Unavailable";
constexpr float update_dot_radius = 4.0f;

struct VersionStatus {
    std::string_view current;
    std::string_view latest;
    bool update_available;

    VersionStatus(std::string_view current, const Json& component)
        : current(current), latest(string_field(component, "latest_version")),
          update_available(!latest.empty() && bool_field(component, "update_available"))
    {
        if (latest.empty()) {
            latest = unavailable;
        }
    }
};

struct InfoRow {
    const char* label;
    std::string value;
    std::optional<bool> update_available = std::nullopt;
};

using InfoRows = std::array<InfoRow, 11>;

struct StatusSamples {
    const Json& cpu;
    const Json& memory;
    const Json& upload;
    const Json& download;
    std::optional<double> cpu_now;
    std::optional<double> memory_now;
    std::optional<double> memory_capacity;
    std::optional<double> upload_now;
    std::optional<double> download_now;

    explicit StatusSamples(const Json& server)
        : cpu(array_field(server, "cpu_history")), memory(array_field(server, "memory_history")),
          upload(array_field(server, "upload_history")), download(array_field(server, "download_history")),
          cpu_now(status_metric(field(server, "cpu_percent"))),
          memory_now(status_metric(field(server, "memory_used_gib"))),
          memory_capacity(status_metric(field(server, "memory_total_gib"))),
          upload_now(status_metric(field(server, "upload_rate"))),
          download_now(status_metric(field(server, "download_rate")))
    {
    }

    std::string cpu_value() const
    {
        return cpu_now ? std::format("{:.1f}%", *cpu_now) : unavailable;
    }
    std::string memory_value() const
    {
        if (!memory_now) {
            return unavailable;
        }
        return memory_capacity ? std::format("{:.1f} / {:.1f} GiB", *memory_now, *memory_capacity)
                               : std::format("{:.1f} GiB", *memory_now);
    }
};

double largest_sample(const Json& samples, double peak)
{
    const auto first = samples.size() - std::min(samples.size(), status_history_capacity);
    for (std::size_t i = first; i < samples.size(); ++i) {
        if (const auto sample = status_metric(samples[i])) {
            peak = std::max(peak, *sample);
        }
    }
    return peak;
}

StatusScale traffic_scale(const StatusSamples& samples)
{
    const double current_peak = std::max(samples.upload_now.value_or(0), samples.download_now.value_or(0));
    const double peak = largest_sample(samples.download, largest_sample(samples.upload, current_peak));
    if (peak == 0) {
        return {"MiB/s", 0.25};
    }
    const bool use_kib = peak < 0.1;
    const double multiplier = use_kib ? 1024 : 1;
    const double rate = peak * multiplier;
    const double increment = rate >= 4 ? 4 : std::pow(10.0, std::floor(std::log10(rate)));
    return {use_kib ? "KiB/s" : "MiB/s", increment * std::ceil(rate / increment), multiplier};
}

std::string traffic_reading(std::string_view name, std::optional<double> value, StatusScale scale)
{
    const auto reading = value ? format_status_reading(*value * scale.multiplier, scale.unit) : "unavailable";
    return std::format("{} {}", name, reading);
}

InfoRows server_information(const Json& server, const StatusSamples& samples)
{
    const auto& core = field(server, "core");
    const VersionStatus panel(string_field(server, "version", wave::version), field(server, "panel"));
    const VersionStatus waterwall(string_field(core, "version", unavailable), core);
    const int uptime = integer_field(server, "uptime_minutes");
    const auto sent = status_metric(field(server, "upload_total"));
    const auto received = status_metric(field(server, "download_total"));
    return {{{"Host name", std::string(string_field(server, "name", unavailable))},
             {"Platform", std::string(string_field(server, "platform", unavailable))},
             {"Panel version", std::string(panel.current)},
             {"Latest panel", std::string(panel.latest), panel.update_available},
             {"Core version", std::string(waterwall.current)},
             {"Latest core", std::string(waterwall.latest), waterwall.update_available},
             {"Uptime", status_metric(field(server, "uptime_minutes"))
                            ? std::format("{}d {}h {}m", uptime / 1440, uptime / 60 % 24, uptime % 60)
                            : unavailable},
             {"Logical CPUs", status_metric(field(server, "cpu_cores"))
                                  ? std::to_string(integer_field(server, "cpu_cores"))
                                  : unavailable},
             {"Memory", samples.memory_value()},
             {"Host sent", sent ? std::format("{:.1f} GiB", *sent) : unavailable},
             {"Host received", received ? std::format("{:.1f} GiB", *received) : unavailable}}};
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
        for (const auto& row : info) {
            labels = std::max(labels, text_width(row.label));
        }
        label_width = std::max(1.0f, std::min(labels + 1, std::floor(width * 0.46f)));
        value_x = label_width + gutter;
        value_width = std::max(1.0f, width - value_x);
        for (std::size_t i = 0; i < info.size(); ++i) {
            const auto& row = info[i];
            rows[i] = std::ceil(std::max(wrapped_height(row.label, label_width),
                                         wrapped_height(row.value, text_value_width(row)))) +
                      6;
            height += rows[i];
        }
    }

    float text_value_width(const InfoRow& row) const
    {
        return row.update_available.has_value() ? std::max(1.0f, value_width - 2 * update_dot_radius - 5)
                                                : value_width;
    }
};

void draw_information(const InfoRows& info, const InformationLayout& layout)
{
    auto& draw = *ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy({layout.value_x + layout.value_width, layout.height});
    float y = start.y;
    for (std::size_t i = 0; i < info.size(); ++i) {
        const auto& row = info[i];
        if (i != 0) {
            draw.AddLineH(start.x, start.x + layout.value_x + layout.value_width, y, packed(Color::divider),
                          theme::border_width);
        }
        draw_text(draw, {start.x, y + 3}, Color::secondary_text, row.label, layout.label_width);
        draw_text(draw, {start.x + layout.value_x, y + 3}, Color::primary_text, row.value,
                  layout.text_value_width(row));
        if (row.update_available.value_or(false)) {
            draw.AddCircleFilled({start.x + layout.value_x + layout.value_width - update_dot_radius,
                                  y + 3 + ImGui::GetFontSize() * 0.5f},
                                 update_dot_radius, packed(Color::warning));
        }
        y += layout.rows[i];
    }
}

float title_band_height()
{
    return ImGui::GetFontSize() + 10;
}

bool begin_card(const char* title, ImVec2 pos, ImVec2 size, std::string_view help = {})
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
        const float label_width = text_width(title);
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const float marker_width = help.empty() ? 0 : gap + text_width(view::help_marker_label);
        const float label_x = child.x + std::floor((width - label_width - marker_width) * 0.5f);
        draw_text(draw, {label_x, child.y + 5}, Color::primary_text, title);
        if (!help.empty()) {
            ImGui::SetCursorScreenPos({label_x + label_width + gap, child.y + 5});
            view::help_marker(help, false);
        }
        ImGui::SetCursorScreenPos({child.x + card_padding.x, child.y + band + card_padding.y});
    }
    return visible;
}

void chart_card(const char* title, ImVec2 pos, ImVec2 size, const StatusTicks& ticks, float left_gutter,
                StatusScale scale, std::initializer_list<StatusSeries> series, std::string_view help = {})
{
    if (begin_card(title, pos, size, help)) {
        render_status_plot(ticks, left_gutter, scale,
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
    constexpr StatusScale cpu_scale{"%", 100};
    const double capacity = samples.memory_capacity.value_or(0);
    const StatusScale memory_scale{
        "GiB", capacity > 0 ? capacity
                            : largest_sample(samples.memory, std::max(1.0, samples.memory_now.value_or(0)))};
    const auto network_scale = traffic_scale(samples);
    const auto cpu_ticks = status_ticks(cpu_scale.ceiling);
    const auto memory_ticks = status_ticks(memory_scale.ceiling);
    const auto traffic_ticks = status_ticks(network_scale.ceiling);
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
    float controls_height = card_frame_height + panel_controls_height(session, controls_body, !stacked);
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
    chart_card("CPU", start, {left_width, cpu_height}, cpu_ticks, axis_gutter, cpu_scale,
               {{samples.cpu, "CPU", Color::accent, samples.cpu_value()}});
    chart_card("Memory", {right_x, memory_y}, {right_width, memory_height}, memory_ticks, axis_gutter,
               memory_scale, {{samples.memory, "Memory", Color::info, samples.memory_value()}});
    if (begin_card("Server Information", {start.x, info_y}, {information_width, info_height})) {
        draw_information(info, information);
    }
    ImGui::EndChild();
    if (begin_card("Panel Controls", {controls_x, controls_y}, {controls_width, controls_height})) {
        render_panel_controls(controls, session, controls_body, !stacked);
    }
    ImGui::EndChild();
    chart_card("Host Network", {right_x, traffic_y}, {right_width, traffic_height}, traffic_ticks,
               axis_gutter, network_scale,
               {{samples.upload, "Upload", Color::info,
                 traffic_reading("Upload", samples.upload_now, network_scale)},
                {samples.download, "Download", Color::accent,
                 traffic_reading("Download", samples.download_now, network_scale)}},
               "Upload and download combine traffic across the host interfaces visible to Wave, excluding "
               "loopback. The chart shows transfer rates in KiB/s or MiB/s.");
    return traffic_y + traffic_height - start.y;
}

std::string describe_server_status(const Json& server)
{
    const StatusSamples samples(server);
    const auto network_scale = traffic_scale(samples);
    const auto& core = field(server, "core");
    const VersionStatus panel(string_field(server, "version", wave::version), field(server, "panel"));
    const VersionStatus waterwall(string_field(core, "version", unavailable), core);
    return std::format("CPU {}. Memory {}. Host network. {}. {}. Server Information. "
                       "Panel version {}. Latest panel {}.{} Core version {}. Latest core {}.{}",
                       samples.cpu_value(), samples.memory_value(),
                       traffic_reading("Upload", samples.upload_now, network_scale),
                       traffic_reading("Download", samples.download_now, network_scale), panel.current,
                       panel.latest, panel.update_available ? " Panel update available." : "",
                       waterwall.current, waterwall.latest,
                       waterwall.update_available ? " Core update available." : "");
}

} // namespace wave
