#pragma once

#include "theme.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace wave {

inline constexpr std::size_t status_history_capacity = 19;

struct StatusSeries {
    const nlohmann::json& samples;
    const char* name;
    theme::Color color;
};

using StatusTicks = std::array<std::string, 5>;

StatusTicks status_ticks(double ceiling);
float status_axis_gutter(const StatusTicks& ticks);
void render_status_plot(const StatusTicks& ticks, float left_gutter, const char* unit, double ceiling,
                        float height, std::initializer_list<StatusSeries> series);

} // namespace wave
