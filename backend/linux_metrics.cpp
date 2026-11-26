#include "linux_metrics.h"

#include <nlohmann/json.hpp>
#include <sys/utsname.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;
constexpr double kib_per_gib = 1024.0 * 1024.0;
constexpr double bytes_per_mib = 1024.0 * 1024.0;
constexpr double bytes_per_gib = bytes_per_mib * 1024.0;
constexpr std::size_t history_capacity = 19;

template <typename T> Json value_or_null(std::optional<T> reading)
{
    return reading ? Json(*reading) : Json();
}

template <std::size_t N> bool read_counters(std::istream& fields, std::array<std::uint64_t, N>& counters)
{
    for (auto& counter : counters) {
        if (!(fields >> counter)) {
            return false;
        }
    }
    return true;
}

void record(Json& server, const char* value_key, const char* history_key, std::optional<double> reading)
{
    auto value = value_or_null(reading);
    server[value_key] = value;
    auto& history = server[history_key];
    if (history.is_null()) {
        history = Json::array();
    }
    // Start with measured values. Once a series has data, preserve gaps in time.
    if (reading || !history.empty()) {
        if (history.size() == history_capacity) {
            history.erase(history.begin());
        }
        history.push_back(std::move(value));
    }
}

void sample_memory(Json& server)
{
    std::ifstream input("/proc/meminfo");
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    for (std::string line; !(total && available) && std::getline(input, line);) {
        if (!line.starts_with("MemTotal:") && !line.starts_with("MemAvailable:")) {
            continue;
        }
        std::string key;
        std::uint64_t kib = 0;
        std::string unit;
        std::istringstream fields(line);
        if (fields >> key >> kib >> unit && unit == "kB") {
            (key == "MemTotal:" ? total : available) = kib;
        }
    }
    std::optional<double> capacity;
    std::optional<double> used;
    if (total && *total > 0) {
        capacity = *total / kib_per_gib;
        if (available && *available <= *total) {
            used = (*total - *available) / kib_per_gib;
        }
    }
    server["memory_total_gib"] = value_or_null(capacity);
    record(server, "memory_used_gib", "memory_history", used);
}

std::optional<std::uint64_t> uptime_minutes()
{
    std::ifstream input("/proc/uptime");
    std::uint64_t seconds = 0;
    // Whole seconds are sufficient for the displayed minute count.
    if (!(input >> seconds)) {
        return std::nullopt;
    }
    return seconds / 60;
}

Json host_label(std::string value)
{
    if (value.empty() ||
        !std::ranges::all_of(value, [](unsigned char byte) { return byte >= 32 && byte < 127; })) {
        return nullptr;
    }
    return value;
}

} // namespace

void LinuxMetrics::sample_cpu(Json& server)
{
    std::ifstream input("/proc/stat");
    std::optional<CpuTimes> current;
    if (std::string line; std::getline(input, line)) {
        std::istringstream fields(line);
        std::string label;
        CpuTimes times{};
        // The guest columns are already included in user and nice.
        if (fields >> label && label == "cpu" && read_counters(fields, times)) {
            current = times;
        }
    }

    unsigned cores = 0;
    for (std::string line; std::getline(input, line) && line.starts_with("cpu");) {
        if (line.size() > 3 && line[3] >= '0' && line[3] <= '9') {
            ++cores;
        }
    }
    server["cpu_cores"] = cores ? Json(cores) : Json();

    std::optional<double> percent;
    if (current && previous_cpu_) {
        double total = 0;
        double idle = 0;
        bool valid = true;
        for (std::size_t i = 0; i < current->size(); ++i) {
            if ((*current)[i] < (*previous_cpu_)[i]) {
                valid = false;
                break;
            }
            const auto ticks = (*current)[i] - (*previous_cpu_)[i];
            total += ticks;
            if (i == 3 || i == 4) { // Idle and I/O wait.
                idle += ticks;
            }
        }
        if (valid && total > 0) {
            percent = 100 * (total - idle) / total;
        }
    }
    previous_cpu_ = current;
    record(server, "cpu_percent", "cpu_history", percent);
}

std::optional<LinuxMetrics::Interfaces> LinuxMetrics::read_interfaces()
{
    std::ifstream input("/proc/net/dev");
    std::string line;
    if (!std::getline(input, line) || !std::getline(input, line)) {
        return std::nullopt;
    }
    Interfaces interfaces;
    while (std::getline(input, line)) {
        const auto separator = line.rfind(':');
        if (separator == line.npos) {
            return std::nullopt;
        }
        std::istringstream name_field(line.substr(0, separator));
        std::string name;
        if (!(name_field >> name)) {
            return std::nullopt;
        }
        if (name == "lo") {
            continue;
        }
        std::istringstream fields(line.substr(separator + 1));
        std::array<std::uint64_t, 9> counters{};
        if (!read_counters(fields, counters)) {
            return std::nullopt;
        }
        // Receive bytes are the first field, transmit bytes the ninth.
        interfaces.emplace(std::move(name), NetworkCounters{counters.front(), counters.back()});
    }
    if (input.bad()) {
        return std::nullopt;
    }
    return interfaces;
}

void LinuxMetrics::sample_network(Json& server)
{
    auto interfaces = read_interfaces();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - previous_network_time_).count();

    double received_total = 0;
    double sent_total = 0;
    double received_delta = 0;
    double sent_delta = 0;
    bool have_delta = false;
    if (interfaces) {
        have_delta = interfaces->empty() && previous_interfaces_ && previous_interfaces_->empty();
        for (const auto& [name, current] : *interfaces) {
            received_total += current.received;
            sent_total += current.sent;
            if (!previous_interfaces_) {
                continue;
            }
            const auto previous = previous_interfaces_->find(name);
            if (previous == previous_interfaces_->end() || current.received < previous->second.received ||
                current.sent < previous->second.sent) {
                continue;
            }
            received_delta += current.received - previous->second.received;
            sent_delta += current.sent - previous->second.sent;
            have_delta = true;
        }
    }
    server["upload_total"] = interfaces ? Json(sent_total / bytes_per_gib) : Json();
    server["download_total"] = interfaces ? Json(received_total / bytes_per_gib) : Json();
    const double denominator = elapsed * bytes_per_mib;
    const bool have_rate = have_delta && denominator > 0;
    record(server, "upload_rate", "upload_history",
           have_rate ? std::optional(sent_delta / denominator) : std::nullopt);
    record(server, "download_rate", "download_history",
           have_rate ? std::optional(received_delta / denominator) : std::nullopt);
    previous_interfaces_ = std::move(interfaces);
    previous_network_time_ = now;
}

void LinuxMetrics::sample(Json& server)
{
    utsname system{};
    const bool identified = uname(&system) == 0;
    server["name"] = identified ? host_label(system.nodename) : Json();
    server["platform"] = identified ? host_label(std::string(system.sysname) + ' ' + system.machine) : Json();
    server["uptime_minutes"] = value_or_null(uptime_minutes());
    sample_cpu(server);
    sample_memory(server);
    sample_network(server);
}

} // namespace wave
