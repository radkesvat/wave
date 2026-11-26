#pragma once

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <sys/types.h>

namespace wave {

// The workspace serializes calls. The sampler calls update from one long-lived thread.
class WaterWall {
  public:
    WaterWall(const std::filesystem::path& directory, bool first_install);
    ~WaterWall();

    bool installed() const;
    std::string_view state() const;
    pid_t running_pid() const;
    const std::string& error() const;
    std::string install();
    std::string start(std::string core_json);
    std::string stop();
    void update();

  private:
    std::filesystem::path working_dir_;
    std::filesystem::path bin_dir_;
    std::filesystem::path executable_;
    std::future<std::string> installation_;
    std::jthread installer_;
    std::string error_;
    pid_t pid_ = -1;
    std::optional<std::string> core_json_;
    std::optional<std::chrono::steady_clock::time_point> stop_deadline_;
};

} // namespace wave
