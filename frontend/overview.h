#pragma once

#include <array>
#include <chrono>
#include <string>

namespace wave {

struct OverviewData {
    float cpu_percent;
    float memory_used_gib;
    float memory_total_gib;
    std::chrono::minutes uptime;
    // One sample every five minutes, oldest first.
    std::array<float, 13> download_mib_per_second;
};

float overview_height(float viewport_width);
void render_overview(const OverviewData& data);
std::string describe_overview(const OverviewData& data);

} // namespace wave
