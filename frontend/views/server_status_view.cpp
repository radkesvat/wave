#include "server_status_view.h"

#include "view_helpers.h"

#include <format>
#include <vector>

namespace wave {

namespace {

using namespace view;
using theme::Color;
using theme::packed;

struct Series {
    const Json& samples;
    const char* name;
    Color color;
};

void chart(const char* title, const char* unit, double ceiling, float height,
           std::initializer_list<Series> series)
{
    ImGui::SeparatorText(title);
    for (const auto& line : series) {
        if (!line.samples.empty()) {
            ImGui::TextColored(theme::rgba(line.color), "%s  %.1f %s", line.name, number(line.samples.back()),
                               unit);
        }
    }

    const auto pos = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy({width, height});
    auto& draw = *ImGui::GetWindowDrawList();
    const ImVec2 corner{std::round(pos.x), std::round(pos.y)};
    const ImVec2 end{std::round(pos.x + width), std::round(pos.y + height)};
    const ImU32 grid = packed(Color::plot_grid);
    const ImU32 label_color = packed(Color::secondary_text);
    draw.AddRectFilled(corner, end, packed(Color::canvas));
    draw.AddRect(corner, end, packed(Color::divider), theme::square_radius, theme::border_width);
    if (width < 92) {
        return;
    }
    const float left = corner.x + 37;
    const float right = end.x - 10;
    const float top = corner.y + 13;
    const float bottom = end.y - 25;
    ImGui::PushFont(nullptr, theme::secondary_font_size);
    for (int tick = 0; tick <= 4; ++tick) {
        const float y = std::round(bottom - (bottom - top) * tick / 4);
        draw.AddLineH(left, right, y, grid, theme::border_width);
        const auto label = std::format("{:.0f}", ceiling * tick / 4);
        draw.AddText({left - ImGui::CalcTextSize(label.c_str()).x - 5, y - theme::secondary_font_size / 2},
                     label_color, label.c_str());
    }
    draw.AddText({left, bottom + 5}, label_color, "Recent samples");
    draw.AddText({right - ImGui::CalcTextSize("Now").x, bottom + 5}, label_color, "Now");
    ImGui::PopFont();

    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered =
        ImGui::IsItemHovered() && mouse.x >= left && mouse.x <= right && mouse.y >= top && mouse.y <= bottom;
    const float fraction = hovered ? (mouse.x - left) / (right - left) : 1;
    if (hovered) {
        draw.AddLineV(std::round(mouse.x), top, bottom, label_color, theme::border_width);
    }

    constexpr std::size_t max_points = 120;
    for (const auto& line : series) {
        const auto count = std::min(line.samples.size(), max_points);
        if (count < 2) {
            continue;
        }
        std::array<ImVec2, max_points> points;
        const auto first = line.samples.size() - count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto rate = std::clamp(number(line.samples[first + i]) / ceiling, 0.0, 1.0);
            points[i] = {left + (right - left) * i / (count - 1),
                         bottom - (bottom - top) * static_cast<float>(rate)};
        }
        const ImU32 color = packed(line.color);
        draw.AddPolyline(points.data(), static_cast<int>(count), color, 2);
        const auto selected = static_cast<std::size_t>(std::lround(fraction * (count - 1)));
        draw.AddCircleFilled(points[selected], hovered ? 4 : 3, color);
    }
    if (hovered && ImGui::BeginTooltip()) {
        for (const auto& line : series) {
            if (!line.samples.empty()) {
                const auto index =
                    static_cast<std::size_t>(std::lround(fraction * (line.samples.size() - 1)));
                ImGui::Text("%s %.1f %s", line.name, number(line.samples[index]), unit);
            }
        }
        ImGui::EndTooltip();
    }
}

double traffic_ceiling(const Json& upload, const Json& download)
{
    double peak = 0;
    for (const auto* samples : {&upload, &download}) {
        for (const auto& sample : *samples) {
            peak = std::max(peak, number(sample));
        }
    }
    return std::max(4.0, 4.0 * std::ceil(peak / 4.0));
}

void server_info(const Json& server)
{
    ImGui::SeparatorText("Server information");
    if (ImGui::BeginTable("server-details", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Current value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        auto row = [](const char* label, std::string_view value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::rgba(Color::secondary_text), "%s", label);
            ImGui::TableNextColumn();
            wrapped(value);
        };
        const int uptime = integer_field(server, "uptime_minutes");
        row("Name", string_field(server, "name", "Wave sample"));
        row("Region", string_field(server, "region", "Preview"));
        row("Platform", string_field(server, "platform", "Demo"));
        row("Version", string_field(server, "version", "0.1.0"));
        row("Uptime", std::format("{}d {}h {}m", uptime / 1440, uptime / 60 % 24, uptime % 60));
        row("CPU", std::format("{} cores at {:.1f}%", integer_field(server, "cpu_cores", 4),
                               numeric_field(server, "cpu_percent")));
        row("Memory", std::format("{:.1f} / {:.1f} GiB", numeric_field(server, "memory_used_gib"),
                                  numeric_field(server, "memory_total_gib")));
        row("Users",
            std::format("{} total, {} enabled, {} online", integer_field(server, "users_total"),
                        integer_field(server, "users_enabled"), integer_field(server, "users_active")));
        row("Upload total", std::format("{:.1f} GiB", numeric_field(server, "upload_total")));
        row("Download total", std::format("{:.1f} GiB", numeric_field(server, "download_total")));
        ImGui::EndTable();
    }
}

} // namespace

void render_server_status(const nlohmann::json& server, bool narrow)
{
    const bool columns = !narrow && ImGui::GetContentRegionAvail().x >= 580 &&
                         ImGui::BeginTable("server-plots", 2, ImGuiTableFlags_SizingStretchSame);
    if (columns) {
        ImGui::TableNextColumn();
    }
    chart("CPU history", "%", 100, 145, {{array_field(server, "cpu_history"), "CPU", Color::accent}});
    if (columns) {
        ImGui::TableNextColumn();
    }
    chart("Memory history", "GiB", std::max(1.0, numeric_field(server, "memory_total_gib")), 145,
          {{array_field(server, "memory_history"), "Memory", Color::info}});
    if (columns) {
        ImGui::EndTable();
    }

    const bool traffic_columns = !narrow && ImGui::GetContentRegionAvail().x >= 720 &&
                                 ImGui::BeginTable("server-summary", 2, ImGuiTableFlags_SizingStretchSame);
    if (traffic_columns) {
        ImGui::TableNextColumn();
    }
    server_info(server);
    if (traffic_columns) {
        ImGui::TableNextColumn();
    }
    const auto& upload = array_field(server, "upload_history");
    const auto& download = array_field(server, "download_history");
    chart("Traffic history", "MiB/s", traffic_ceiling(upload, download), 168,
          {{upload, "Upload", Color::info}, {download, "Download", Color::accent}});
    if (traffic_columns) {
        ImGui::EndTable();
    }
}

} // namespace wave
