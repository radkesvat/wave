#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace wave {

inline constexpr std::size_t log_history_per_source = 100;

struct WaterWallLog {
    std::string_view source;
    std::string timestamp;
    std::string_view level;
    std::string message;
};

class WaterWallLogs {
  public:
    explicit WaterWallLogs(const std::filesystem::path& directory);

    void start(const nlohmann::json& settings);
    std::vector<WaterWallLog> read();
    void stop();

  private:
    struct File {
        std::filesystem::path path;
        off_t offset = 0;
        dev_t device = 0;
        ino_t inode = 0;
        std::string partial;
        bool skip_line = false;
        bool truncated = false;
    };

    struct Logger {
        std::filesystem::path base;
        std::array<File, 2> files;
    };

    std::filesystem::path directory_;
    std::array<Logger, 4> loggers_;
    std::array<std::string, 2> dates_;
    bool active_ = false;

    void update_files(const std::tm& date);
    static std::string read_file(File& file, bool skip_existing = false);
};

} // namespace wave
