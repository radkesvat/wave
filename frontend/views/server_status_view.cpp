#include "server_status_view.h"

#include "view_helpers.h"

#include <format>
#include <vector>

namespace wave {

namespace {

using namespace view;

struct Series {
    const Json& samples;
    const char* name;
    ImU32 color;
};

void chart(const char* title, const char* unit, double ceiling, float height,
           std::initializer_list<Series> series)
{
    ImGui::SeparatorText(title);
    for (const auto& line : series) {
        if (!line.samples.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(line.color), "%s  %.1f %s",
                               line.name, number(line.samples.back()), unit);
        }
    }

    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy({width, height});
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled(corner, {corner.x + width, corner.y + height}, plot_surface, 6);
    if (width < 92) {
        return;
    }
    const float left = corner.x + 37;
    const float right = corner.x + width - 10;
    const float top = corner.y + 13;
    const float bottom = corner.y + height - 25;
    for (int tick = 0; tick <= 4; ++tick) {
        const float y = bottom - (bottom - top) * tick / 4;
        draw.AddLineH(left, right, y, plot_grid);
        const auto label = std::format("{:.0f}", ceiling * tick / 4);
        draw.AddText({left - ImGui::CalcTextSize(label.c_str()).x - 5, y - 8},
                     plot_muted, label.c_str());
    }
    draw.AddText({left, bottom + 5}, plot_muted, "Past hour");
    draw.AddText({right - ImGui::CalcTextSize("Now").x, bottom + 5}, plot_muted, "Now");

    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered() && mouse.x >= left && mouse.x <= right &&
        mouse.y >= top && mouse.y <= bottom;
    const float fraction = hovered ? (mouse.x - left) / (right - left) : 1;
    if (hovered) {
        draw.AddLineV(mouse.x, top, bottom, plot_muted);
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
        draw.AddPolyline(points.data(), static_cast<int>(count), line.color, 2);
        const auto selected = static_cast<std::size_t>(std::lround(fraction * (count - 1)));
        draw.AddCircleFilled(points[selected], hovered ? 4 : 3, line.color);
    }
    if (hovered && ImGui::BeginTooltip()) {
        for (const auto& line : series) {
            if (!line.samples.empty()) {
                const auto index = static_cast<std::size_t>(std::lround(fraction * (line.samples.size() - 1)));
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
            ImGui::TextDisabled("%s", label);
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
        row("Users", std::format("{} total, {} enabled, {} online",
                                 integer_field(server, "users_total"),
                                 integer_field(server, "users_enabled"),
                                 integer_field(server, "users_active")));
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
    chart("CPU history", "%", 100, 145, {{array_field(server, "cpu_history"), "CPU", plot_mint}});
    if (columns) {
        ImGui::TableNextColumn();
    }
    chart("Memory history", "GiB", std::max(1.0, numeric_field(server, "memory_total_gib")), 145,
          {{array_field(server, "memory_history"), "Memory", plot_blue}});
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
          {{upload, "Upload", plot_mint}, {download, "Download", plot_amber}});
    if (traffic_columns) {
        ImGui::EndTable();
    }
}

} // namespace wave
