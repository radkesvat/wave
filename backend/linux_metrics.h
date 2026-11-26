#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace wave {

class LinuxMetrics {
  public:
    void sample(nlohmann::json& server);

  private:
    using CpuTimes = std::array<std::uint64_t, 8>;

    struct NetworkCounters {
        std::uint64_t received;
        std::uint64_t sent;
    };

    using Interfaces = std::map<std::string, NetworkCounters>;

    static std::optional<Interfaces> read_interfaces();
    void sample_cpu(nlohmann::json& server);
    void sample_network(nlohmann::json& server);

    std::optional<CpuTimes> previous_cpu_;
    std::optional<Interfaces> previous_interfaces_;
    std::chrono::steady_clock::time_point previous_network_time_;
};

} // namespace wave
