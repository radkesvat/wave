#pragma once

#include "theme.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

inline constexpr std::size_t status_history_capacity = 19;

struct StatusSeries {
    const nlohmann::json& samples;
    const char* name;
    theme::Color color;
    std::string legend;
};

struct StatusScale {
    const char* unit;
    double ceiling;
    double multiplier = 1;
};

using StatusTicks = std::array<std::string, 5>;

std::optional<double> status_metric(const nlohmann::json& value);
std::string format_status_reading(double value, std::string_view unit);
StatusTicks status_ticks(double ceiling);
float status_axis_gutter(const StatusTicks& ticks);
void render_status_plot(const StatusTicks& ticks, float left_gutter, StatusScale scale, float height,
                        std::initializer_list<StatusSeries> series);

} // namespace wave
