#pragma once

#include <array>
#include <chrono>

namespace wave {

struct OverviewData {
    float cpu_percent;
    float memory_used_gib;
    float memory_total_gib;
    std::chrono::minutes uptime;
    // Latest readings, oldest first.
    std::array<float, 13> download_mib_per_second;
};

} // namespace wave
